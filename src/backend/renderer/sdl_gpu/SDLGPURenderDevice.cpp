/**
 * @file SDLGPURenderDevice.cpp
 * @brief SDL3 GPU API 渲染后端实现 — 仅保留 GPU 渲染方式
 *
 * 支持后端: Vulkan (Linux/Windows), Metal (macOS/iOS), D3D12 (Windows)
 * 着色器格式: SPIRV (跨平台), DXIL (Windows only, 按需编译)
 *
 * 架构概览:
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  提交入口                                                           ║
 * ║  ┌─ submitCommandBuffer(CommandBuffer)                              ║
 * ║  │   ↓ 提取 ClearCmd/SetCameraCmd, 转为 RenderCmd* 数组              ║
 * ║  │   ↓ renderCommandBufferToTarget()                                ║
 * ║  ├─ submitPass(PassSubmitInfo, vector<RenderCmd*>)                   ║
 * ║  │   ↓ renderCmdsToTarget()  ← 核心渲染函数                          ║
 * ║  ├─ submitGPUDrivenPass(PassSubmitInfo, GPURenderParams)             ║
 * ║  │   ↓ GPU 直接从 storage buffer 读取 sprite 数据并单 pass 绘制       ║
 * ║  └─ renderToTexture / renderToTextureOffscreen                      ║
 * ║      ↓ 渲染到纹理离屏目标 (editor 预览 / GPU fence 同步)              ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 *
 * Pipeline 类型:
 *  (1) pipeline_            — Sprite/Tile 渲染 (swapchain 格式)
 *  (2) offscreenPipeline_   — Sprite/Tile 渲染 (R8G8B8A8_UNORM, 离屏)
 *  (3) msdfPipeline_        — MSDF 文字渲染 (swapchain)
 *  (4) msdfOffscreenPipeline_ — MSDF 文字渲染 (离屏)
 *  (5) gpuDrivenPipeline_   — GPU-driven sprite (直接读 storage buffer)
 *
 * 渲染管线层次 (每帧):
 *  beginFrame() → [submit* calls] → present()
 *       ↓
 *  SDL_AcquireGPUCommandBuffer → SDL_WaitAndAcquireGPUSwapchainTexture
 *       ↓
 *  每个 submit 调用录制 GPU 命令到同一个 gpuCmdBuf_
 *       ↓
 *  present() → SDL_SubmitGPUCommandBuffer → 提交到 GPU 队列
 */

#include "SDLGPURenderDevice.h"

#include <algorithm>
#include <cstring>
#include <variant>

#include <SDL3/SDL.h>

#include "../CommandBuffer.h"
#include "../../../core/Assert.h"
#include "../../../core/Logger.h"

// 预编译 SPIRV 着色器二进制 (CMake 通过 glslc 编译 .glsl → .spv)
#include "sprite_vert_spv.h"          // 标准 sprite 顶点着色器: pos+uv+color → clip space
#include "sprite_frag_spv.h"          // 标准 sprite 片段着色器: 纹理采样 × 顶点颜色
#include "msdf_frag_spv.h"            // MSDF 字体片段着色器: median() 抗锯齿
#include "sprite_gpu_vert_spv.h"      // GPU-driven 顶点着色器: 从 storage buffer 读 sprite 数据
#include "sprite_gpu_frag_spv.h"      // GPU-driven 片段着色器
#ifdef QGAME_HAS_DXIL_SHADERS
#include "sprite_vert_dxil.h"         // DXIL 版本的着色器 (Windows D3D12 后端)
#include "sprite_frag_dxil.h"
#include "msdf_frag_dxil.h"
#endif

namespace backend {

// ═══════════════════════════════════════════════════════════════════════════════
// 构造 / 析构
// ═══════════════════════════════════════════════════════════════════════════════

SDLGPURenderDevice::SDLGPURenderDevice(SDL_Window* window,bool debug)
    : window_(window), debug_(debug) {
    // 预分配 CPU 侧 batch 缓冲区 (最大 batch 大小的 sprite 数量)
    // 每个 sprite: 4 顶点 + 6 索引 (两个三角形组成一个矩形)
    batchVerts_.reserve(MAX_SPRITES_PER_BATCH * 4);
    batchIdx_.reserve(MAX_SPRITES_PER_BATCH * 6);
}

SDLGPURenderDevice::~SDLGPURenderDevice() {
    shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════════
// 初始化 — 创建设备、缓冲区、Pipeline
// ═══════════════════════════════════════════════════════════════════════════════

void SDLGPURenderDevice::init() {
    // 1. 创建 GPU 设备 — 请求 Vulkan 后端，支持 SPIRV (+ DXIL)
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef QGAME_HAS_DXIL_SHADERS
    formats |= SDL_GPU_SHADERFORMAT_DXIL;
#endif

    // 第三个参数 "vulkan" 是 hint: 优先 Vulkan，不可用时自动 fallback
    device_ = SDL_CreateGPUDevice(formats,debug_ , "vulkan");
    if (!device_) {
        core::logError("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return;
    }

    // 2. 将 SDL 窗口关联到 GPU 设备 (建立 swapchain)
    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        core::logError("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        return;
    }

    // 3. 查询实际使用的着色器格式 (SPIRV 或 DXIL)
    const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(device_);
    const char* backend = SDL_GetGPUDeviceDriver(device_);
    core::logInfo("GPU backend: %s  shader formats: 0x%x", backend, static_cast<int>(supported));

    if (supported & SDL_GPU_SHADERFORMAT_SPIRV) {
        shaderFormat_ = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef QGAME_HAS_DXIL_SHADERS
    } else if (supported & SDL_GPU_SHADERFORMAT_DXIL) {
        shaderFormat_ = SDL_GPU_SHADERFORMAT_DXIL;
#endif
    } else {
        core::logError("GPU backend '%s' requires unavailable shader formats", backend);
        SDL_ReleaseWindowFromGPUDevice(device_, window_);
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        return;
    }

    // 4. 分配 GPU 资源
    // 顶点缓冲区: 每帧动态上传 batch 的 sprite 顶点数据
    SDL_GPUBufferCreateInfo vbInfo{};
    vbInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vbInfo.size = MAX_SPRITES_PER_BATCH * 4 * sizeof(SpriteVertex);
    vertexBuf_ = SDL_CreateGPUBuffer(device_, &vbInfo);

    // 索引缓冲区: 每帧动态上传 batch 的索引数据
    SDL_GPUBufferCreateInfo ibInfo{};
    ibInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ibInfo.size = MAX_SPRITES_PER_BATCH * 6 * sizeof(uint16_t);
    indexBuf_ = SDL_CreateGPUBuffer(device_, &ibInfo);

    // 传输缓冲区: CPU→GPU 中转 (vertex + index 共用，顺序排列)
    // 大小 = vbInfo.size + ibInfo.size (因为 vertex 和 index 放在同一块里)
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size = vbInfo.size + ibInfo.size;
    transferBuf_ = SDL_CreateGPUTransferBuffer(device_, &tbInfo);

    if (!vertexBuf_ || !indexBuf_ || !transferBuf_) {
        core::logError("GPU buffer allocation failed: %s", SDL_GetError());
        return;
    }

    // 5. 创建所有渲染管线 (sprite, MSDF, GPU-driven)
    createPipeline();
    core::logInfo("SDLGPURenderDevice initialized");
}

// ═══════════════════════════════════════════════════════════════════════════════
// 帧生命周期
// ═══════════════════════════════════════════════════════════════════════════════

void SDLGPURenderDevice::beginFrame() {
    if (!device_) {
        return;
    }

    // 从 GPU 设备获取一个新的 command buffer (本帧所有 GPU 命令都录制到这里)
    gpuCmdBuf_ = SDL_AcquireGPUCommandBuffer(device_);
    ASSERT_MSG(gpuCmdBuf_, "SDL_AcquireGPUCommandBuffer failed");

    // 获取 swapchain 的下一帧纹理 (等待上一帧渲染完成)
    SDL_GPUTexture* tex = nullptr;
    const bool ok = SDL_WaitAndAcquireGPUSwapchainTexture(gpuCmdBuf_, window_, &tex, &swapW_, &swapH_);
    if (!ok || !tex) {
        // swapchain 不可用 (窗口最小化、resize 中等)，取消本帧
        SDL_CancelGPUCommandBuffer(gpuCmdBuf_);
        gpuCmdBuf_ = nullptr;
        swapchainTex_ = nullptr;
        return;
    }

    swapchainTex_ = tex;
}

void SDLGPURenderDevice::endFrame() {
    // 当前实现中 rendering 在每个 submit 调用里即时录制到 gpuCmdBuf_
    // endFrame 仅在 present() 真正提交到 GPU 后才需要清理
}

void SDLGPURenderDevice::shutdown() {
    if (!device_) {
        return;
    }

    // 等待所有 GPU 操作完成，避免释放正在使用的资源
    SDL_WaitForGPUIdle(device_);

    // 释放离屏渲染目标 (editor + offscreen)
    if (textures_.valid(editorRenderTarget_)) {
        destroyTexture(editorRenderTarget_);
        editorRenderTarget_ = {};
    }

    if (textures_.valid(offscreenRenderTarget_)) {
        destroyTexture(offscreenRenderTarget_);
        offscreenRenderTarget_ = {};
    }

    // 释放 graphics pipelines
    if (pipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_); pipeline_ = nullptr; }
    if (offscreenPipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, offscreenPipeline_); offscreenPipeline_ = nullptr; }
    if (msdfPipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, msdfPipeline_); msdfPipeline_ = nullptr; }
    if (msdfOffscreenPipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, msdfOffscreenPipeline_); msdfOffscreenPipeline_ = nullptr; }
    if (gpuDrivenPipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, gpuDrivenPipeline_); gpuDrivenPipeline_ = nullptr; }
    if (gpuDrivenQuadIndexBuf_) { SDL_ReleaseGPUBuffer(device_, gpuDrivenQuadIndexBuf_); gpuDrivenQuadIndexBuf_ = nullptr; }

    // 释放批处理缓冲 (vertex + index + transfer)
    if (vertexBuf_) { SDL_ReleaseGPUBuffer(device_, vertexBuf_); vertexBuf_ = nullptr; }
    if (indexBuf_) { SDL_ReleaseGPUBuffer(device_, indexBuf_); indexBuf_ = nullptr; }
    if (transferBuf_) { SDL_ReleaseGPUTransferBuffer(device_, transferBuf_); transferBuf_ = nullptr; }

    // 清理所有 compute pipeline (通过 HandleMap 遍历)
    while (computePipelines_.valid(ComputePipelineHandle{1, 1})) {
        ComputePipelineHandle h{1, 1};
        if (computePipelines_.tryGet(h)) {
            destroyComputePipeline(h);
        } else {
            break;
        }
    }

    // 清理所有自定义 buffer
    while (buffers_.valid(BufferHandle{1, 1})) {
        BufferHandle h{1, 1};
        if (buffers_.tryGet(h)) {
            destroyBuffer(h);
        } else {
            break;
        }
    }

    // 解绑窗口并销毁 GPU 设备
    SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;
    core::logInfo("SDLGPURenderDevice shutdown");
}

// ═══════════════════════════════════════════════════════════════════════════════
// 纹理管理
// ═══════════════════════════════════════════════════════════════════════════════

TextureHandle SDLGPURenderDevice::createTexture(const TextureDesc& desc) {
    ASSERT(desc.data && desc.width > 0 && desc.height > 0);
    if (!device_) {
        core::logError("createTexture: GPU device not initialized");
        return {};
    }

    // 1. 创建 GPU 纹理对象 — 2D, R8G8B8A8_UNORM, 仅采样使用
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<uint32_t>(desc.width);
    info.height = static_cast<uint32_t>(desc.height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;                    // 无 mipmap
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* gpuTex = SDL_CreateGPUTexture(device_, &info);
    if (!gpuTex) {
        core::logError("SDL_CreateGPUTexture failed (%dx%d): %s", desc.width, desc.height, SDL_GetError());
        return {};
    }

    // 2. 通过 transfer buffer 将像素数据上传到 GPU 纹理
    //    流程: CPU data → transfer buffer (map/memcpy/unmap)
    //          → copy pass (UploadToGPUTexture) → submit
    const size_t dataSize = static_cast<size_t>(desc.width) * static_cast<size_t>(desc.height) * 4u;
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size = static_cast<uint32_t>(dataSize);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tbInfo);

    void* mapped = SDL_MapGPUTransferBuffer(device_, tb, false);  // false = 不循环
    memcpy(mapped, desc.data, dataSize);
    SDL_UnmapGPUTransferBuffer(device_, tb);

    // 独立 command buffer 执行上传 (不占用主渲染 command buffer)
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;

    SDL_GPUTextureRegion dst{};
    dst.texture = gpuTex;
    dst.w = info.width;
    dst.h = info.height;
    dst.d = 1;

    SDL_UploadToGPUTexture(copyPass, &src, &dst, false);  // false = 不循环
    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, tb);

    // 3. 创建采样器 — 与纹理绑定到同一 TextureEntry 中
    const SDL_GPUFilter filter = (desc.filter == TextureFilter::Linear)
        ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = filter;
    samplerInfo.mag_filter = filter;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device_, &samplerInfo);

    return textures_.insert(TextureEntry{ gpuTex, sampler, desc.width, desc.height });
}

TextureHandle SDLGPURenderDevice::createRenderTargetTexture(int width, int height) {
    // 与 createTexture 类似，但 usage 增加 COLOR_TARGET 标志
    // 既可作为渲染目标 (COLOR_TARGET) 也可作为纹理采样 (SAMPLER)
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<uint32_t>(width);
    info.height = static_cast<uint32_t>(height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* gpuTex = SDL_CreateGPUTexture(device_, &info);
    if (!gpuTex) {
        core::logError("SDL_CreateGPUTexture render target failed (%dx%d): %s", width, height, SDL_GetError());
        return {};
    }

    // 离屏纹理通常用于 editor 预览，使用线性过滤
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device_, &samplerInfo);

    return textures_.insert(TextureEntry{ gpuTex, sampler, width, height });
}

void SDLGPURenderDevice::destroyTexture(TextureHandle h) {
    if (!textures_.valid(h)) {
        return;
    }

    TextureEntry& entry = textures_.get(h);
    // 等待 GPU 完成所有操作后再释放 (纹理可能正在被使用)
    SDL_WaitForGPUIdle(device_);
    if (entry.sampler) SDL_ReleaseGPUSampler(device_, entry.sampler);
    if (entry.gpuTex) SDL_ReleaseGPUTexture(device_, entry.gpuTex);
    textures_.remove(h);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Shader 管理 (stub — SDL GPU 后端不使用外部 ShaderHandle)
// ═══════════════════════════════════════════════════════════════════════════════

ShaderHandle SDLGPURenderDevice::createShader(const ShaderDesc&) {
    return {};
}

void SDLGPURenderDevice::destroyShader(ShaderHandle) {
}

// ═══════════════════════════════════════════════════════════════════════════════
// 字体管理 — FontData 只存储元数据和 glyph 映射，纹理由 createTexture 管理
// ═══════════════════════════════════════════════════════════════════════════════

engine::FontHandle SDLGPURenderDevice::createFont(const engine::FontData& fontData) {
    engine::FontData data = fontData;
    return fonts_.insert(std::move(data));
}

void SDLGPURenderDevice::destroyFont(engine::FontHandle h) {
    if (fonts_.valid(h)) {
        fonts_.remove(h);
    }
}

const engine::FontData* SDLGPURenderDevice::getFont(engine::FontHandle h) const {
    return fonts_.valid(h) ? &fonts_.get(h) : nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Buffer 管理 — GPU 原生 buffer 的创建/销毁/读写
//
// 每个 SDL 端 Buffer 由两部分组成:
//  (1) gpuBuffer   — SDL_GPUBuffer (存储数据，在 GPU 显存中)
//  (2) transfer    — SDL_GPUTransferBuffer (CPU↔GPU 中转，用于 upload/download)
//
// 上传流程:  CPU data → transfer (map/memcpy/unmap)
//            → copy pass (UploadToGPUBuffer) → gpuBuffer
// 下载流程:  gpuBuffer → copy pass (DownloadFromGPUBuffer) → transfer
//            → map/memcpy → CPU data
// ═══════════════════════════════════════════════════════════════════════════════

// 将引擎 BufferUsage 标志映射到 SDL GPU buffer usage 标志
BufferHandle SDLGPURenderDevice::createBuffer(const BufferDesc& desc) {
    if (!device_ || desc.size == 0) {
        core::logError("createBuffer: invalid params device=%p size=%zu", device_, desc.size);
        return {};
    }

    // 1. 映射用法标志
    SDL_GPUBufferUsageFlags gpuUsage = 0;
    if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(BufferUsage::Vertex)) {
        gpuUsage |= SDL_GPU_BUFFERUSAGE_VERTEX;
    }
    if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(BufferUsage::Index)) {
        gpuUsage |= SDL_GPU_BUFFERUSAGE_INDEX;
    }
    if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(BufferUsage::Storage)) {
        // Storage buffer 需要 compute 写入 + graphics 读取
        // (用于 GPU-driven 渲染: compute culling/sorting → graphics indirect draw)
        gpuUsage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE
                  | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ
                  | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    }
    if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(BufferUsage::Indirect)) {
        gpuUsage |= SDL_GPU_BUFFERUSAGE_INDIRECT;
    }

    SDL_GPUBufferCreateInfo bufInfo{};
    bufInfo.usage = gpuUsage;
    bufInfo.size = static_cast<uint32_t>(desc.size);

    core::logInfo("createBuffer: creating GPU buffer size=%zu usage=0x%x", desc.size, gpuUsage);

    // 2. 创建 GPU 原生 buffer
    SDL_GPUBuffer* gpuBuf = SDL_CreateGPUBuffer(device_, &bufInfo);
    if (!gpuBuf) {
        core::logError("createBuffer: SDL_CreateGPUBuffer failed: %s", SDL_GetError());
        return {};
    }

    // 3. 创建配套的 transfer buffer (用于 CPU↔GPU 数据传输)
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<uint32_t>(desc.size);

    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (!transfer) {
        SDL_ReleaseGPUBuffer(device_, gpuBuf);
        core::logError("createBuffer: SDL_CreateGPUTransferBuffer failed: %s", SDL_GetError());
        return {};
    }

    core::logInfo("createBuffer: inserting into HandleMap");
    BufferHandle handle = buffers_.insert(BufferEntry{ gpuBuf, transfer, desc.size, desc.usage });

    // 4. 如果有初始数据，立即上传
    if (desc.initialData) {
        uploadToBuffer(handle, desc.initialData, desc.size, 0);
    }

    return handle;
}

void SDLGPURenderDevice::destroyBuffer(BufferHandle h) {
    if (!buffers_.valid(h)) return;

    BufferEntry& entry = buffers_.get(h);
    SDL_WaitForGPUIdle(device_);
    if (entry.gpuBuffer) SDL_ReleaseGPUBuffer(device_, entry.gpuBuffer);
    if (entry.transfer) SDL_ReleaseGPUTransferBuffer(device_, entry.transfer);
    buffers_.remove(h);
}

// mapBuffer: 映射 transfer buffer 供 CPU 写入 (false = 写入模式)
void* SDLGPURenderDevice::mapBuffer(BufferHandle h) {
    if (!buffers_.valid(h)) return nullptr;
    BufferEntry& entry = buffers_.get(h);
    return SDL_MapGPUTransferBuffer(device_, entry.transfer, false);
}

void SDLGPURenderDevice::unmapBuffer(BufferHandle h) {
    if (!buffers_.valid(h)) return;
    BufferEntry& entry = buffers_.get(h);
    SDL_UnmapGPUTransferBuffer(device_, entry.transfer);
}

// uploadToBuffer: CPU → transfer (map/memcpy/unmap) → GPU (copy pass)
void SDLGPURenderDevice::uploadToBuffer(BufferHandle h, const void* data, size_t size, size_t offset) {
    if (!buffers_.valid(h) || !data || size == 0) return;
    BufferEntry& entry = buffers_.get(h);
    if (offset + size > entry.size) {
        core::logError("uploadToBuffer: out of bounds (offset=%zu, size=%zu, bufferSize=%zu)", offset, size, entry.size);
        return;
    }

    // Step 1: 映射 transfer buffer，CPU 写入数据
    void* mapped = SDL_MapGPUTransferBuffer(device_, entry.transfer, false);
    if (!mapped) {
        core::logError("uploadToBuffer: map failed: %s", SDL_GetError());
        return;
    }
    memcpy(static_cast<uint8_t*>(mapped) + offset, data, size);
    SDL_UnmapGPUTransferBuffer(device_, entry.transfer);

    // Step 2: 独立的 copy pass 将 transfer buffer 数据拷贝到 GPU buffer
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTransferBufferLocation src{};
    src.transfer_buffer = entry.transfer;
    src.offset = static_cast<uint32_t>(offset);

    SDL_GPUBufferRegion dst{};
    dst.buffer = entry.gpuBuffer;
    dst.offset = static_cast<uint32_t>(offset);
    dst.size = static_cast<uint32_t>(size);

    SDL_UploadToGPUBuffer(copyPass, &src, &dst, false);
    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(cmd);
}

// downloadFromBuffer: GPU buffer → transfer (copy pass + download) → CPU (map/memcpy)
void SDLGPURenderDevice::downloadFromBuffer(BufferHandle h, void* data, size_t size, size_t offset) {
    if (!buffers_.valid(h) || !data || size == 0) return;
    BufferEntry& entry = buffers_.get(h);
    if (offset + size > entry.size) {
        core::logError("downloadFromBuffer: out of bounds");
        return;
    }

    // Step 1: 创建专门用于下载的 transfer buffer
    SDL_GPUTransferBufferCreateInfo downloadInfo{};
    downloadInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    downloadInfo.size = static_cast<uint32_t>(size);
    SDL_GPUTransferBuffer* downloadBuf = SDL_CreateGPUTransferBuffer(device_, &downloadInfo);
    if (!downloadBuf) {
        core::logError("downloadFromBuffer: create transfer buffer failed: %s", SDL_GetError());
        return;
    }

    // Step 2: copy pass 将 GPU buffer 数据下载到 transfer buffer
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUBufferRegion src{};
    src.buffer = entry.gpuBuffer;
    src.offset = static_cast<uint32_t>(offset);
    src.size = static_cast<uint32_t>(size);

    SDL_GPUTransferBufferLocation dst{};
    dst.transfer_buffer = downloadBuf;
    dst.offset = 0;

    SDL_DownloadFromGPUBuffer(copyPass, &src, &dst);
    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(cmd);

    // Step 3: 映射 transfer buffer，读取数据到 CPU 侧 (true = 读取模式)
    void* mapped = SDL_MapGPUTransferBuffer(device_, downloadBuf, true);
    if (mapped) {
        memcpy(data, mapped, size);
        SDL_UnmapGPUTransferBuffer(device_, downloadBuf);
    }

    SDL_ReleaseGPUTransferBuffer(device_, downloadBuf);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Compute Pipeline 管理 — 从 ComputePipelineDesc 创建原生 GPU compute pipeline
//
// 着色器代码来源优先级:
//  (1) desc.spirvCode/spirvSize — SPIRV 格式 (Vulkan/Metal 后端)
//  (2) desc.dxilCode/dxilSize   — DXIL 格式 (D3D12 后端)
//  (3) desc.code/desc.codeSize   — 通用备选 (OpenGL 用 GLSL, SDL GPU 不推荐)
// ═══════════════════════════════════════════════════════════════════════════════

ComputePipelineHandle SDLGPURenderDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    if (!device_) return {};

    // 根据当前设备的着色器格式选择合适的二进制代码
    const void* blob = nullptr;
    size_t blobSize = 0;
    if (shaderFormat_ == SDL_GPU_SHADERFORMAT_SPIRV && desc.spirvCode && desc.spirvSize) {
        blob = desc.spirvCode;
        blobSize = desc.spirvSize;
    }
    else if (shaderFormat_ == SDL_GPU_SHADERFORMAT_DXIL && desc.dxilCode && desc.dxilSize) {
        blob = desc.dxilCode;
        blobSize = desc.dxilSize;
    }
    else if (desc.code && desc.codeSize) {
        // 通用备选 (GLSL 等，不推荐在 SDL GPU 后端使用)
        blob = desc.code;
        blobSize = desc.codeSize;
    }
    else {
        core::logError("createComputePipeline: no shader blob matches device format 0x%x", shaderFormat_);
        return {};
    }
    core::logInfo("createComputePipeline: format=0x%x blobSize=%zu (spirv=%zu dxil=%zu)",
     shaderFormat_, blobSize, desc.spirvSize, desc.dxilSize);

    // 填充 compute pipeline 创建信息
    SDL_GPUComputePipelineCreateInfo info{};
    info.code_size = blobSize;
    info.code = static_cast<const Uint8*>(blob);
    info.entrypoint = desc.entryPoint ? desc.entryPoint : "main";
    info.format = shaderFormat_;
    info.num_samplers = desc.numSamplers;
    info.num_readonly_storage_textures = desc.numReadonlyStorageTextures;
    info.num_readonly_storage_buffers = desc.numReadonlyStorageBuffers;
    info.num_readwrite_storage_textures = desc.numReadwriteStorageTextures;
    info.num_readwrite_storage_buffers = desc.numReadwriteStorageBuffers;
    info.num_uniform_buffers = desc.numUniformBuffers;
    info.threadcount_x = desc.threadCountX > 0 ? desc.threadCountX : 64;
    info.threadcount_y = desc.threadCountY > 0 ? desc.threadCountY : 1;
    info.threadcount_z = desc.threadCountZ > 0 ? desc.threadCountZ : 1;
    info.props = 0;

    SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(device_, &info);
    if (!pipeline) {
        core::logError("createComputePipeline: SDL_CreateGPUComputePipeline failed: %s", SDL_GetError());
        return {};
    }

    return computePipelines_.insert(ComputePipelineEntry{ pipeline });
}

void SDLGPURenderDevice::destroyComputePipeline(ComputePipelineHandle h) {
    if (!computePipelines_.valid(h)) return;
    ComputePipelineEntry& entry = computePipelines_.get(h);
    SDL_WaitForGPUIdle(device_);
    if (entry.pipeline) SDL_ReleaseGPUComputePipeline(device_, entry.pipeline);
    computePipelines_.remove(h);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 帧提交接口 — 各上层入口最终汇聚到 renderCmdsToTarget
// ═══════════════════════════════════════════════════════════════════════════════

// submitCommandBuffer: 从 CommandBuffer 提取命令并渲染到 swapchain
void SDLGPURenderDevice::submitCommandBuffer(const CommandBuffer& cb) {
    if (!gpuCmdBuf_ || !swapchainTex_) {
        return;
    }
    renderCommandBufferToTarget(gpuCmdBuf_, pipeline_, cb, swapchainTex_, swapW_, swapH_, true);
}

// submitPass: pipeline-driven 路径 — 直接接受 RenderCmd* 数组
void SDLGPURenderDevice::submitPass(const PassSubmitInfo& info,
                                     const std::vector<const RenderCmd*>& cmds) {
    if (!gpuCmdBuf_ || !swapchainTex_) return;
    CameraData cam = info.camera;
    if (cam.viewportW == 0) cam.viewportW = static_cast<int>(swapW_);
    if (cam.viewportH == 0) cam.viewportH = static_cast<int>(swapH_);
    renderCmdsToTarget(gpuCmdBuf_, pipeline_, cmds, cam,
                       info.clearEnabled, info.clearColor,
                       swapchainTex_, swapW_, swapH_);
}

// 获取底层 SDL_GPUTexture 指针 (供外部需要原生纹理句柄的场景使用)
SDL_GPUTexture* SDLGPURenderDevice::getSDLTexture(TextureHandle handle) const {
    const TextureEntry* entry = textures_.valid(handle) ? &textures_.get(handle) : nullptr;
    return entry ? entry->gpuTex : nullptr;
}

void* SDLGPURenderDevice::getRawTexture(TextureHandle handle) const {
    return getSDLTexture(handle);
}

bool SDLGPURenderDevice::getTextureDimensions(TextureHandle handle, int& outW, int& outH) const {
    if (!textures_.valid(handle)) return false;
    const TextureEntry& e = textures_.get(handle);
    outW = e.width;
    outH = e.height;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// renderCommandBufferToTarget — 从 CommandBuffer 提取命令并渲染到指定目标
//
// 这是 submitCommandBuffer 和离屏渲染的共同底层。
// 流程:
//   1. 遍历 CommandBuffer 中的所有 cmd，区分 ClearCmd/SetCameraCmd/其他渲染命令
//   2. 提取 clearColor 和 camera 并转换 camera 坐标
//   3. 将剩余的渲染命令转为 RenderCmd* 指针数组
//   4. 调用 renderCmdsToTarget 执行实际渲染
// ═══════════════════════════════════════════════════════════════════════════════

void SDLGPURenderDevice::renderCommandBufferToTarget(SDL_GPUCommandBuffer* cmdBuf, SDL_GPUGraphicsPipeline* pipeline, const CommandBuffer& cb, SDL_GPUTexture* target, uint32_t targetWidth, uint32_t targetHeight, bool clearTarget) {
    if (!cmdBuf || !target || !pipeline) {
        core::logError("renderCommandBufferToTarget: invalid params cmdBuf=%p target=%p pipeline=%p", cmdBuf, target, pipeline);
        return;
    }
    // 兼容 editor 路径：从命令流中提取 ClearCmd/SetCameraCmd，转交给指针版本
    std::vector<const RenderCmd*> cmdPtrs;
    cmdPtrs.reserve(cb.commands().size());
    core::Color clearColor = core::Color::Black;
    CameraData camera{};
    camera.viewportW = static_cast<int>(targetWidth);
    camera.viewportH = static_cast<int>(targetHeight);
    for (const auto& cmd : cb.commands()) {
        if (std::holds_alternative<ClearCmd>(cmd)) {
            clearColor = std::get<ClearCmd>(cmd).color;
        } else if (std::holds_alternative<SetCameraCmd>(cmd)) {
            camera = std::get<SetCameraCmd>(cmd).camera;
        } else {
            cmdPtrs.push_back(&cmd);
        }
    }
    renderCmdsToTarget(cmdBuf, pipeline, cmdPtrs, camera, clearTarget, clearColor,
                       target, targetWidth, targetHeight);
}

// renderCmdsToTarget — 核心渲染函数
//
// 此函数是 SDL GPU 后端的渲染中枢，处理四类命令:
//
//   [Phase 1] DispatchCmd  — GPU compute 派发 (culling/sorting/generation)
//      ↓ SDL_BeginGPUComputePass → SDL_BindGPUComputePipeline
//      ↓ bind storage buffers/textures → SDL_DispatchGPUCompute
//      ↓ (BarrierCmd 在 SDL GPU 后端被忽略 — 驱动自动插入 barrier)
//
//   [Phase 2] 几何构建 (CPU 侧)
//      ↓ 遍历 DrawSpriteCmd/DrawTileCmd/DrawTextCmd
//      ↓ 在 CPU 侧构建顶点/索引数据 (batchVerts_ / batchIdx_)
//      ↓ 自动按纹理/font/pxRange 分 batch
//
//   [Phase 3] 数据上传 (CPU → GPU)
//      ↓ 将 batchVerts_/batchIdx_ 通过 transferBuf_ 上传到 vertexBuf_/indexBuf_
//
//   [Phase 4] GPU 绘制
//      ↓ SDL_BeginGPURenderPass → bind pipeline → bind textures
//      ↓ push mvp uniform → SDL_DrawGPUIndexedPrimitives
//
// Batch 切换规则:
//   - 纹理变化 (tex != currentTex): 必须 flush
//   - 字体 vs 精灵 (isFont != currentIsFont): 必须 flush (不同 pipeline)
//   - 字体 pxRange 变化: 必须 flush (需要 push 不同的 fragment uniform)
//   - batch 顶点数达到上限 (MAX_SPRITES_PER_BATCH * 4): 必须 flush
// ═══════════════════════════════════════════════════════════════════════════════
void SDLGPURenderDevice::renderCmdsToTarget(SDL_GPUCommandBuffer* cmdBuf,
                                             SDL_GPUGraphicsPipeline* pipeline,
                                             const std::vector<const RenderCmd*>& cmds,
                                             const CameraData& cameraIn,
                                             bool clearEnabled,
                                             core::Color clearColor,
                                             SDL_GPUTexture* target,
                                             uint32_t targetWidth, uint32_t targetHeight) {
    if (!cmdBuf || !target || !pipeline) {
        core::logError("renderCmdsToTarget: invalid params cmdBuf=%p target=%p pipeline=%p", cmdBuf, target, pipeline);
        return;
    }

    CameraData camera = cameraIn;
    if (camera.viewportW == 0) camera.viewportW = static_cast<int>(targetWidth);
    if (camera.viewportH == 0) camera.viewportH = static_cast<int>(targetHeight);

    // ── Phase 1: Compute 命令派发 ─────────────────────────────────────────
    for (const RenderCmd* cmd : cmds) {
        if (auto* d = std::get_if<DispatchCmd>(cmd)) {
            if (!computePipelines_.valid(d->pipeline)) continue;
            ComputePipelineEntry& pe = computePipelines_.get(d->pipeline);

            // Prepare readwrite storage buffer bindings for SDL_BeginGPUComputePass
            SDL_GPUStorageBufferReadWriteBinding rwBufferBindings[8];
            uint32_t rwBufferCount = 0;
            for (uint32_t i = 0; i < d->bindings.readwriteStorageBufferCount && i < 8; ++i) {
                if (buffers_.valid(d->bindings.readwriteStorageBuffers[i])) {
                    rwBufferBindings[rwBufferCount].buffer = buffers_.get(d->bindings.readwriteStorageBuffers[i]).gpuBuffer;
                    rwBufferBindings[rwBufferCount].cycle = false;
                    ++rwBufferCount;
                }
            }

            // Prepare readwrite storage texture bindings
            SDL_GPUStorageTextureReadWriteBinding rwTextureBindings[8];
            uint32_t rwTextureCount = 0;
            for (uint32_t i = 0; i < d->bindings.readwriteStorageTextureCount && i < 8; ++i) {
                if (textures_.valid(d->bindings.readwriteStorageTextures[i])) {
                    TextureEntry& te = textures_.get(d->bindings.readwriteStorageTextures[i]);
                    rwTextureBindings[rwTextureCount].texture = te.gpuTex;
                    rwTextureBindings[rwTextureCount].mip_level = 0;
                    rwTextureBindings[rwTextureCount].layer = 0;
                    rwTextureBindings[rwTextureCount].cycle = false;
                    ++rwTextureCount;
                }
            }

            SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(
                cmdBuf, rwTextureBindings, rwTextureCount, rwBufferBindings, rwBufferCount);
            
            SDL_BindGPUComputePipeline(computePass, pe.pipeline);

            // Bind readonly storage buffers
            if (d->bindings.readonlyStorageBufferCount > 0) {
                SDL_GPUBuffer* readonlyBuffers[8];
                for (uint32_t i = 0; i < d->bindings.readonlyStorageBufferCount && i < 8; ++i) {
                    if (buffers_.valid(d->bindings.readonlyStorageBuffers[i])) {
                        readonlyBuffers[i] = buffers_.get(d->bindings.readonlyStorageBuffers[i]).gpuBuffer;
                    } else {
                        readonlyBuffers[i] = nullptr;
                    }
                }
                SDL_BindGPUComputeStorageBuffers(computePass, 0, readonlyBuffers, d->bindings.readonlyStorageBufferCount);
            }

            // Bind sampled textures (with samplers)
            if (d->bindings.sampledTextureCount > 0) {
                SDL_GPUTextureSamplerBinding samplerBindings[8];
                for (uint32_t i = 0; i < d->bindings.sampledTextureCount && i < 8; ++i) {
                    if (textures_.valid(d->bindings.sampledTextures[i])) {
                        TextureEntry& te = textures_.get(d->bindings.sampledTextures[i]);
                        samplerBindings[i].texture = te.gpuTex;
                        samplerBindings[i].sampler = te.sampler;
                    } else {
                        samplerBindings[i].texture = nullptr;
                        samplerBindings[i].sampler = nullptr;
                    }
                }
                SDL_BindGPUComputeSamplers(computePass, 0, samplerBindings, d->bindings.sampledTextureCount);
            }

            // Bind readonly storage textures
            if (d->bindings.readonlyStorageTextureCount > 0) {
                SDL_GPUTexture* readonlyTextures[8];
                for (uint32_t i = 0; i < d->bindings.readonlyStorageTextureCount && i < 8; ++i) {
                    if (textures_.valid(d->bindings.readonlyStorageTextures[i])) {
                        readonlyTextures[i] = textures_.get(d->bindings.readonlyStorageTextures[i]).gpuTex;
                    } else {
                        readonlyTextures[i] = nullptr;
                    }
                }
                SDL_BindGPUComputeStorageTextures(computePass, 0, readonlyTextures, d->bindings.readonlyStorageTextureCount);
            }

            SDL_DispatchGPUCompute(computePass, d->groupCountX, d->groupCountY, d->groupCountZ);
            SDL_EndGPUComputePass(computePass);
        }
        else if (auto* b = std::get_if<BarrierCmd>(cmd)) {
            // SDL GPU 后端由驱动自动插入 barrier，忽略显式 BarrierCmd
            (void)b;
        }
    }

    // ── Phase 2: CPU 侧几何构建 + Batch 分组 ─────────────────────────────
    batchVerts_.clear();
    batchIdx_.clear();
    std::vector<BatchSegment> batches;

    TextureHandle currentTex{};
    bool          hasCurrent = false;
    bool          currentIsFont = false;
    float         currentPxRange = 4.0f;
    uint32_t      batchIdxStart  = 0;
    int32_t       batchVertStart = 0;

    auto flush = [&]() {
        if (static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
            batches.push_back({ currentTex, batchIdxStart,
                                static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart,
                                batchVertStart, currentIsFont, currentPxRange });
            batchIdxStart  = static_cast<uint32_t>(batchIdx_.size());
            batchVertStart = static_cast<int32_t>(batchVerts_.size());
        }
    };
    auto maybeFlush = [&](TextureHandle tex, bool isFont = false, float pxRange = 4.0f) {
        const bool batchFull =
            (batchVerts_.size() - static_cast<size_t>(batchVertStart) >= MAX_SPRITES_PER_BATCH * 4);
        if (!hasCurrent || tex != currentTex || batchFull ||
            currentIsFont != isFont || (isFont && currentPxRange != pxRange)) {
            flush();
            currentTex = tex;
            currentIsFont = isFont;
            currentPxRange = pxRange;
            hasCurrent = true;
        }
    };
    auto pushQuad = [&](float x0, float y0, float x1, float y1,
                        float x2, float y2, float x3, float y3,
                        float u0, float v0, float u1, float v1,
                        const core::Color& tint)
    {
        const auto base = static_cast<uint16_t>(batchVerts_.size() - static_cast<size_t>(batchVertStart));
        batchVerts_.push_back({ x0, y0, u0, v0, tint.r, tint.g, tint.b, tint.a });
        batchVerts_.push_back({ x1, y1, u1, v0, tint.r, tint.g, tint.b, tint.a });
        batchVerts_.push_back({ x2, y2, u1, v1, tint.r, tint.g, tint.b, tint.a });
        batchVerts_.push_back({ x3, y3, u0, v1, tint.r, tint.g, tint.b, tint.a });
        batchIdx_.insert(batchIdx_.end(), {
            base,
            static_cast<uint16_t>(base + 1),
            static_cast<uint16_t>(base + 2),
            base,
            static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 3)
        });
    };

    for (const RenderCmd* cmd : cmds) {
        if (auto* s = std::get_if<DrawSpriteCmd>(cmd)) {
            maybeFlush(s->texture);
            const float hw = s->srcRect.w * s->scaleX * 0.5f;
            const float hh = s->srcRect.h * s->scaleY * 0.5f;
            const float cosR = cosf(s->rotation);
            const float sinR = sinf(s->rotation);
            const float lx[4] = { -hw,  hw,  hw, -hw };
            const float ly[4] = { -hh, -hh,  hh,  hh };
            const TextureEntry* entry = textures_.tryGet(s->texture);
            const float tw = entry ? static_cast<float>(entry->width)  : 1.f;
            const float th = entry ? static_cast<float>(entry->height) : 1.f;
            const float u0 =  s->srcRect.x              / tw;
            const float v0 =  s->srcRect.y              / th;
            const float u1 = (s->srcRect.x + s->srcRect.w) / tw;
            const float v1 = (s->srcRect.y + s->srcRect.h) / th;
            float px[4], py[4];
            for (int i = 0; i < 4; ++i) {
                px[i] = s->x + lx[i] * cosR - ly[i] * sinR;
                py[i] = s->y + lx[i] * sinR + ly[i] * cosR;
            }
            pushQuad(px[0],py[0], px[1],py[1], px[2],py[2], px[3],py[3],
                     u0,v0, u1,v1, s->tint);
        }
        else if (auto* t = std::get_if<DrawTileCmd>(cmd)) {
            maybeFlush(t->tileset);
            const TextureEntry* entry = textures_.tryGet(t->tileset);
            const float tw = entry ? static_cast<float>(entry->width)  : 1.f;
            const float th = entry ? static_cast<float>(entry->height) : 1.f;
            const int   ts = t->tileSize > 0 ? t->tileSize : 16;
            int tilesetCols = static_cast<int>(tw) / ts;
            if (tilesetCols < 1) tilesetCols = 1;
            const int col = t->tileId % tilesetCols;
            const int row = t->tileId / tilesetCols;
            const float u0 = (col * ts) / tw;
            const float v0 = (row * ts) / th;
            const float u1 = u0 + ts / tw;
            const float v1 = v0 + ts / th;
            const float px  = static_cast<float>(t->gridX * ts);
            const float py  = static_cast<float>(t->gridY * ts);
            const float px1 = px + ts;
            const float py1 = py + ts;
            pushQuad(px,py, px1,py, px1,py1, px,py1, u0,v0, u1,v1,
                     core::Color{255,255,255,255});
        }
        else if (auto* text = std::get_if<DrawTextCmd>(cmd)) {
            const engine::FontData* font = getFont(text->font);
            if (!font || !textures_.valid(font->texture)) continue;

            const float scale = text->fontSize / font->fontSize;
            // screenPxRange = atlasPxRange * (screenPxPerEm / atlasPxPerEm) = pxRange * scale * cameraZoom。
            const float camZoom = (camera.zoom > 0.f) ? camera.zoom : 1.f;
            const float screenPxRange = font->pxRange * scale * camZoom;
            maybeFlush(font->texture, true, screenPxRange);

            float cursorX = text->x;
            float cursorY = text->y;
            const std::string& s = text->text;

            for (size_t i = 0; i < s.size();) {
                uint32_t cp = 0;
                unsigned char c0 = static_cast<unsigned char>(s[i]);
                size_t adv = 1;
                if (c0 < 0x80) { cp = c0; adv = 1; }
                else if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
                    cp = (c0 & 0x1F) << 6 | (static_cast<unsigned char>(s[i+1]) & 0x3F);
                    adv = 2;
                } else if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
                    cp = (c0 & 0x0F) << 12
                       | (static_cast<unsigned char>(s[i+1]) & 0x3F) << 6
                       | (static_cast<unsigned char>(s[i+2]) & 0x3F);
                    adv = 3;
                } else if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
                    cp = (c0 & 0x07) << 18
                       | (static_cast<unsigned char>(s[i+1]) & 0x3F) << 12
                       | (static_cast<unsigned char>(s[i+2]) & 0x3F) << 6
                       | (static_cast<unsigned char>(s[i+3]) & 0x3F);
                    adv = 4;
                } else {
                    cp = 0xFFFD; adv = 1;
                }
                i += adv;

                const engine::Glyph* glyph = font->getGlyph(cp);
                if (!glyph) {
                    cursorX += font->fontSize * 0.5f * scale;
                    continue;
                }

                // 屏幕为 y-down：字形顶端在屏幕上更靠上（y 更小）= baseline - bearingY。
                const float x0 = cursorX + glyph->bearingX * scale;
                const float y0 = cursorY - glyph->bearingY * scale;
                const float x1 = x0 + glyph->width * scale;
                const float y1 = y0 + glyph->height * scale;

                pushQuad(x0, y0, x1, y0, x1, y1, x0, y1,
                         glyph->u0, glyph->v0, glyph->u1, glyph->v1,
                         text->color);

                cursorX += glyph->advance * scale;
            }
        }
    }
    flush();  // flush 最后一个 batch

    // ── Phase 3: CPU → GPU 数据传输 ──────────────────────────────────────
    if (!batchVerts_.empty()) {
        const size_t vSize = batchVerts_.size() * sizeof(SpriteVertex);
        const size_t iSize = batchIdx_.size() * sizeof(uint16_t);

        uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, transferBuf_, true));
        memcpy(mapped, batchVerts_.data(), vSize);
        memcpy(mapped + vSize, batchIdx_.data(), iSize);
        SDL_UnmapGPUTransferBuffer(device_, transferBuf_);

        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdBuf);
        SDL_GPUTransferBufferLocation vSrc{ transferBuf_, 0 };
        SDL_GPUBufferRegion vDst{ vertexBuf_, 0, static_cast<uint32_t>(vSize) };
        SDL_UploadToGPUBuffer(copyPass, &vSrc, &vDst, true);

        SDL_GPUTransferBufferLocation iSrc{ transferBuf_, static_cast<uint32_t>(vSize) };
        SDL_GPUBufferRegion iDst{ indexBuf_, 0, static_cast<uint32_t>(iSize) };
        SDL_UploadToGPUBuffer(copyPass, &iSrc, &iDst, true);
        SDL_EndGPUCopyPass(copyPass);
    }

    // ── Phase 4: GPU 绘制 ────────────────────────────────────────────────
    // 构建 MVP 矩阵 (列主序: mvp = proj * view)
    float proj[16];
    float view[16];
    const float zoom = (camera.zoom > 0.f) ? camera.zoom : 1.f;
    buildOrthoProjectionMatrix(static_cast<float>(targetWidth), static_cast<float>(targetHeight), proj);
    buildViewMatrix(camera.x, camera.y, zoom, camera.rotation, view);

    float mvp[16];
    // 列主序：mvp = proj * view（先 view 把世界变到相机空间，再 proj 投影到 NDC）
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            mvp[i * 4 + j] = 0.f;
            for (int k = 0; k < 4; ++k) {
                mvp[i * 4 + j] += view[i * 4 + k] * proj[k * 4 + j];
            }
        }
    }

    SDL_GPUColorTargetInfo colorTarget{};
    colorTarget.texture = target;
    colorTarget.load_op = clearEnabled ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    colorTarget.store_op = SDL_GPU_STOREOP_STORE;
    colorTarget.clear_color = {
        clearColor.r / 255.0f,
        clearColor.g / 255.0f,
        clearColor.b / 255.0f,
        clearColor.a / 255.0f
    };

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmdBuf, &colorTarget, 1, nullptr);
    SDL_PushGPUVertexUniformData(cmdBuf, 0, mvp, sizeof(mvp));

    if (!batchVerts_.empty()) {
        SDL_GPUBufferBinding vertexBinding{ vertexBuf_, 0 };
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
        SDL_GPUBufferBinding indexBinding{ indexBuf_, 0 };
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

        for (const BatchSegment& segment : batches) {
            SDL_GPUGraphicsPipeline* segPipeline = segment.isFont ? 
                (pipeline == offscreenPipeline_ ? msdfOffscreenPipeline_ : msdfPipeline_) :
                pipeline;
            SDL_BindGPUGraphicsPipeline(pass, segPipeline);
            
            if (textures_.valid(segment.tex)) {
                TextureEntry& entry = textures_.get(segment.tex);
                SDL_GPUTextureSamplerBinding binding{ entry.gpuTex, entry.sampler };
                SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            }
            
            if (segment.isFont) {
                float pxRange = segment.pxRange;
                SDL_PushGPUFragmentUniformData(cmdBuf, 0, &pxRange, sizeof(pxRange));
            }
            
            SDL_DrawGPUIndexedPrimitives(pass, segment.idxCount, 1, segment.idxOffset, segment.vertOffset, 0);
        }
    }

    SDL_EndGPURenderPass(pass);
}

// ═══════════════════════════════════════════════════════════════════════════════
// present — 提交帧到 GPU 队列并呈现
// ═══════════════════════════════════════════════════════════════════════════════

void SDLGPURenderDevice::present() {
    if (!gpuCmdBuf_) {
        return;
    }
    // 提交本帧录制的所有 GPU 命令，触发异步执行
    SDL_SubmitGPUCommandBuffer(gpuCmdBuf_);
    gpuCmdBuf_ = nullptr;
    swapchainTex_ = nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 离屏渲染 — 渲染到纹理 (editor 预览 / offscreen compositing)
//
// renderToTexture: 渲染到 editor 预览纹理 (复用 swapchain 的 cmd buffer)
//   — 画布尺寸变化时重新创建纹理
//   — 每帧直接向当前 gpuCmdBuf_ 录制命令
//
// renderToTextureOffscreen: 独立离屏渲染
//   — 创建独立的 command buffer + fence 等待
//   — 确保读取前渲染已完成 (同步点)
// ═══════════════════════════════════════════════════════════════════════════════

TextureHandle SDLGPURenderDevice::renderToTexture(const CommandBuffer& cb, int width, int height) {
    if (!gpuCmdBuf_ || width <= 0 || height <= 0) {
        return {};
    }

    if (!textures_.valid(editorRenderTarget_) ||
        editorRenderTargetWidth_ != width ||
        editorRenderTargetHeight_ != height) {
        if (textures_.valid(editorRenderTarget_)) {
            destroyTexture(editorRenderTarget_);
        }
        editorRenderTarget_ = createRenderTargetTexture(width, height);
        editorRenderTargetWidth_ = width;
        editorRenderTargetHeight_ = height;
    }

    TextureEntry* entry = textures_.tryGet(editorRenderTarget_);
    if (!entry) {
        return {};
    }

    renderCommandBufferToTarget(gpuCmdBuf_, pipeline_, cb, entry->gpuTex, static_cast<uint32_t>(width), static_cast<uint32_t>(height), true);
    return editorRenderTarget_;
}

TextureHandle SDLGPURenderDevice::renderToTextureOffscreen(const CommandBuffer& cb, int width, int height) {
    if (!device_ || width <= 0 || height <= 0) {
        core::logError("renderToTextureOffscreen: invalid params device=%p w=%d h=%d", device_, width, height);
        return {};
    }

    if (!offscreenPipeline_) {
        core::logError("renderToTextureOffscreen: offscreenPipeline_ is null");
        return {};
    }

    if (!textures_.valid(offscreenRenderTarget_) ||
        offscreenRenderTargetWidth_ != width ||
        offscreenRenderTargetHeight_ != height) {
        if (textures_.valid(offscreenRenderTarget_)) {
            destroyTexture(offscreenRenderTarget_);
        }
        offscreenRenderTarget_ = createRenderTargetTexture(width, height);
        offscreenRenderTargetWidth_ = width;
        offscreenRenderTargetHeight_ = height;
        core::logInfo("renderToTextureOffscreen: resized render target %dx%d", width, height);
    }

    TextureEntry* entry = textures_.tryGet(offscreenRenderTarget_);
    if (!entry || !entry->gpuTex) {
        core::logError("renderToTextureOffscreen: failed to get texture entry");
        return {};
    }

    SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmdBuf) {
        core::logError("renderToTextureOffscreen: SDL_AcquireGPUCommandBuffer failed");
        return {};
    }

    renderCommandBufferToTarget(cmdBuf, offscreenPipeline_, cb, entry->gpuTex, static_cast<uint32_t>(width), static_cast<uint32_t>(height), true);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdBuf);
    if (fence) {
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
    }

    return offscreenRenderTarget_;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Shader 加载工具 — 从预编译字节码创建 SDL_GPUShader
//
// 参数:
//   code/codeSize: 预编译着色器二进制 (SPIRV 或 DXIL)
//   stage:         着色器阶段 (VERTEX / FRAGMENT)
//   numSamplers:   片段着色器需要的纹理采样器数量
//   numUBOs:       需要的 uniform buffer 数量
//   fmt:           着色器格式 (SPIRV / DXIL)
// ═══════════════════════════════════════════════════════════════════════════════
SDL_GPUShader* SDLGPURenderDevice::loadShader(const uint8_t* code, size_t size, SDL_GPUShaderStage stage, int numSamplers, int numUBOs, SDL_GPUShaderFormat fmt) {
    SDL_GPUShaderCreateInfo info{};
    info.code = code;
    info.code_size = size;
    info.entrypoint = "main";
    info.format = fmt;
    info.stage = stage;
    info.num_samplers = static_cast<uint32_t>(numSamplers);
    info.num_uniform_buffers = static_cast<uint32_t>(numUBOs);
    return SDL_CreateGPUShader(device_, &info);
}

// ═══════════════════════════════════════════════════════════════════════════════
// createPipeline — 创建所有渲染管线
//
// 共创建 5 条管线:
//  (1) pipeline_             — 标准 sprite/tile 管线 (swapchain 格式)
//  (2) offscreenPipeline_    — 标准 sprite/tile 管线 (R8G8B8A8_UNORM, 离屏)
//  (3) msdfPipeline_         — MSDF 文字管线 (swapchain)
//  (4) msdfOffscreenPipeline_ — MSDF 文字管线 (离屏)
//  (5) gpuDrivenPipeline_    — GPU-driven sprite 管线 (仅 SPIRV 后端)
//
// swapchain 格式是运行时查询的 (不同 GPU/平台可能不同), offscreen 固定为 R8G8B8A8
// ═══════════════════════════════════════════════════════════════════════════════
void SDLGPURenderDevice::createPipeline() {
    if (!device_) {
        return;
    }

    SDL_GPUTextureFormat swapchainFormat = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    pipeline_ = createPipelineForFormat(swapchainFormat);
    ASSERT_MSG(pipeline_, "Failed to create swapchain pipeline");

    offscreenPipeline_ = createPipelineForFormat(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    ASSERT_MSG(offscreenPipeline_, "Failed to create offscreen pipeline");

    msdfPipeline_ = createMSDFPipelineForFormat(swapchainFormat);
    ASSERT_MSG(msdfPipeline_, "Failed to create MSDF swapchain pipeline");

    msdfOffscreenPipeline_ = createMSDFPipelineForFormat(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    ASSERT_MSG(msdfOffscreenPipeline_, "Failed to create MSDF offscreen pipeline");

    // GPU-driven 通路目前只在 SPIRV 后端有预编译 shader（DXIL 暂未提供）
    if (shaderFormat_ == SDL_GPU_SHADERFORMAT_SPIRV) {
        gpuDrivenPipeline_ = createGPUDrivenPipelineForFormat(swapchainFormat);
        if (!gpuDrivenPipeline_) {
            core::logError("createPipeline: failed to create GPU-driven pipeline");
        } else {
            createGPUDrivenIndexBuffer();
        }
    }

    core::logInfo("Pipelines created (swapchain: 0x%x, offscreen: R8G8B8A8, gpuDriven: %s)",
                  static_cast<int>(swapchainFormat),
                  gpuDrivenPipeline_ ? "yes" : "no");
}

// 创建标准 sprite/tile 渲染管线 (给定颜色目标格式)
// 顶点布局: pos(2f) | uv(2f) | color(4ub normalised)
// 片元: 纹理采样 × 顶点颜色, alpha blend
SDL_GPUGraphicsPipeline* SDLGPURenderDevice::createPipelineForFormat(SDL_GPUTextureFormat format) {
    SDL_GPUShader* vs = nullptr;
    SDL_GPUShader* fs = nullptr;
    if (shaderFormat_ == SDL_GPU_SHADERFORMAT_SPIRV) {
        vs = loadShader(sprite_vert_spv, sprite_vert_spv_size, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, SDL_GPU_SHADERFORMAT_SPIRV);
        fs = loadShader(sprite_frag_spv, sprite_frag_spv_size, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, SDL_GPU_SHADERFORMAT_SPIRV);
#ifdef QGAME_HAS_DXIL_SHADERS
    } else if (shaderFormat_ == SDL_GPU_SHADERFORMAT_DXIL) {
        vs = loadShader(sprite_vert_dxil, sprite_vert_dxil_size, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, SDL_GPU_SHADERFORMAT_DXIL);
        fs = loadShader(sprite_frag_dxil, sprite_frag_dxil_size, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, SDL_GPU_SHADERFORMAT_DXIL);
#endif
    }
    if (!vs || !fs) {
        if (vs) SDL_ReleaseGPUShader(device_, vs);
        if (fs) SDL_ReleaseGPUShader(device_, fs);
        return nullptr;
    }

    SDL_GPUVertexBufferDescription vbDesc{};
    vbDesc.slot = 0;
    vbDesc.pitch = sizeof(SpriteVertex);
    vbDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbDesc.instance_step_rate = 0;

    SDL_GPUVertexAttribute attrs[3]{};
    attrs[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SpriteVertex, x) };
    attrs[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SpriteVertex, u) };
    attrs[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(SpriteVertex, r) };

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = format;
    colorTarget.blend_state.enable_blend = true;
    colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.vertex_shader = vs;
    pipeInfo.fragment_shader = fs;
    pipeInfo.vertex_input_state.vertex_buffer_descriptions = &vbDesc;
    pipeInfo.vertex_input_state.num_vertex_buffers = 1;
    pipeInfo.vertex_input_state.vertex_attributes = attrs;
    pipeInfo.vertex_input_state.num_vertex_attributes = 3;
    pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipeInfo.target_info.color_target_descriptions = &colorTarget;
    pipeInfo.target_info.num_color_targets = 1;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &pipeInfo);

    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    return pipeline;
}

// 创建 MSDF 字体渲染管线
// 与标准管线使用相同的顶点着色器，但片段着色器是 MSDF special:
//   - 采样 MSDF 图集纹理 → median(r,g,b) 计算有符号距离
//   - sigDist * pxRange → 抗锯齿 alpha
//   - 额外的 fragment uniform: float pxRange (通过 push constant 传入)
SDL_GPUGraphicsPipeline* SDLGPURenderDevice::createMSDFPipelineForFormat(SDL_GPUTextureFormat format) {
    SDL_GPUShader* vs = nullptr;
    SDL_GPUShader* fs = nullptr;
    if (shaderFormat_ == SDL_GPU_SHADERFORMAT_SPIRV) {
        vs = loadShader(sprite_vert_spv, sprite_vert_spv_size, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, SDL_GPU_SHADERFORMAT_SPIRV);
        fs = loadShader(msdf_frag_spv, msdf_frag_spv_size, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, SDL_GPU_SHADERFORMAT_SPIRV);
#ifdef QGAME_HAS_DXIL_SHADERS
    } else if (shaderFormat_ == SDL_GPU_SHADERFORMAT_DXIL) {
        vs = loadShader(sprite_vert_dxil, sprite_vert_dxil_size, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, SDL_GPU_SHADERFORMAT_DXIL);
        fs = loadShader(msdf_frag_dxil, msdf_frag_dxil_size, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, SDL_GPU_SHADERFORMAT_DXIL);
#endif
    }
    if (!vs || !fs) {
        if (vs) SDL_ReleaseGPUShader(device_, vs);
        if (fs) SDL_ReleaseGPUShader(device_, fs);
        return nullptr;
    }

    SDL_GPUVertexBufferDescription vbDesc{};
    vbDesc.slot = 0;
    vbDesc.pitch = sizeof(SpriteVertex);
    vbDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbDesc.instance_step_rate = 0;

    SDL_GPUVertexAttribute attrs[3]{};
    attrs[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SpriteVertex, x) };
    attrs[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SpriteVertex, u) };
    attrs[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(SpriteVertex, r) };

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = format;
    colorTarget.blend_state.enable_blend = true;
    colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.vertex_shader = vs;
    pipeInfo.fragment_shader = fs;
    pipeInfo.vertex_input_state.vertex_buffer_descriptions = &vbDesc;
    pipeInfo.vertex_input_state.num_vertex_buffers = 1;
    pipeInfo.vertex_input_state.vertex_attributes = attrs;
    pipeInfo.vertex_input_state.num_vertex_attributes = 3;
    pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipeInfo.target_info.color_target_descriptions = &colorTarget;
    pipeInfo.target_info.num_color_targets = 1;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &pipeInfo);

    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    return pipeline;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 矩阵构建工具
// ═══════════════════════════════════════════════════════════════════════════════

void SDLGPURenderDevice::buildOrthoMatrix(float w, float h, float out[16]) {
    buildOrthoProjectionMatrix(w, h, out);
}

void SDLGPURenderDevice::buildOrthoProjectionMatrix(float w, float h, float out[16]) {
    const float left   = -w * 0.5f;
    const float right  =  w * 0.5f;
    const float top    =  h * 0.5f;
    const float bottom = -h * 0.5f;

    memset(out, 0, 16 * sizeof(float));
    out[0]  =  2.f / (right - left);
    out[5]  = -2.f / (top - bottom);
    out[10] =  1.f;
    out[12] = -(right + left)  / (right - left);
    out[13] = -(top + bottom)  / (top - bottom);
    out[15] =  1.f;
}

void SDLGPURenderDevice::buildViewMatrix(float camX, float camY, float zoom, float rotation, float out[16]) {
    // 标准 2D view：先平移到相机原点，再绕原点旋转，再按 zoom 缩放。
    // 列主序，世界点 (x,y) → eye = R * zoom * (world - cam)
    const float c = cosf(rotation);
    const float s = sinf(rotation);

    memset(out, 0, 16 * sizeof(float));
    out[0]  =  c * zoom;
    out[1]  =  s * zoom;
    out[4]  = -s * zoom;
    out[5]  =  c * zoom;
    out[10] = 1.f;
    out[12] = -( c * camX - s * camY) * zoom;
    out[13] = -( s * camX + c * camY) * zoom;
    out[15] = 1.f;
}

void SDLGPURenderDevice::buildOrthoMatrixCamera(float w, float h,
                                                 float camX, float camY, float zoom,
                                                 float rotation,
                                                 float out[16]) {
    float proj[16];
    float view[16];
    buildOrthoProjectionMatrix(w, h, proj);
    buildViewMatrix(camX, camY, zoom, rotation, view);

    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i * 4 + j] = 0.f;
            for (int k = 0; k < 4; ++k) {
                out[i * 4 + j] += view[i * 4 + k] * proj[k * 4 + j];
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// GPU-driven 渲染 — 顶点着色器直接从 storage buffer 读取 sprite 数据
//
// 与标准管线不同，GPU-driven 顶点着色器:
//   - 没有 vertex buffer 输入
//   - 从 2 个 storage buffer 读取数据:
//       set=0, b=0 → spriteBuffer (GPUSprite 数组，含 transform/uv/color/textureIndex)
//       set=0, b=1 → visibleIndices (经过 compute culling/sorting 后的可见索引)
//   - set=1, b=0 → viewProj uniform
//   - gl_VertexIndex: 6 个 index/instance → 4 个 quad 顶点
//   - gl_InstanceIndex: 当前 sprite 在 visibleIndices 中的序号
//
// 每个 sprite 作为 1 个 instance 绘制 (instanceCount = visibleCount)
// 每批绑定不同纹理 (按 GPUDrawBatch 分组)
// ═══════════════════════════════════════════════════════════════════════════════
SDL_GPUGraphicsPipeline* SDLGPURenderDevice::createGPUDrivenPipelineForFormat(SDL_GPUTextureFormat format) {
    // GPU-driven 顶点着色器通过 storage buffer 读取 sprite/index 数据，
    // 因此既不需要 vertex buffer，也不需要 vertex attribute。
    SDL_GPUShaderCreateInfo vsInfo{};
    vsInfo.code      = sprite_gpu_vert_spv;
    vsInfo.code_size = sprite_gpu_vert_spv_size;
    vsInfo.entrypoint = "main";
    vsInfo.format    = SDL_GPU_SHADERFORMAT_SPIRV;
    vsInfo.stage     = SDL_GPU_SHADERSTAGE_VERTEX;
    vsInfo.num_samplers         = 0;
    vsInfo.num_storage_buffers  = 2;   // set=0,b=0: spriteBuffer; set=0,b=1: visibleIndices
    vsInfo.num_storage_textures = 0;
    vsInfo.num_uniform_buffers  = 1;   // set=1,b=0: viewProj
    SDL_GPUShader* vs = SDL_CreateGPUShader(device_, &vsInfo);

    SDL_GPUShaderCreateInfo fsInfo{};
    fsInfo.code      = sprite_gpu_frag_spv;
    fsInfo.code_size = sprite_gpu_frag_spv_size;
    fsInfo.entrypoint = "main";
    fsInfo.format    = SDL_GPU_SHADERFORMAT_SPIRV;
    fsInfo.stage     = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsInfo.num_samplers        = 1;
    fsInfo.num_storage_buffers = 0;
    fsInfo.num_uniform_buffers = 0;
    SDL_GPUShader* fs = SDL_CreateGPUShader(device_, &fsInfo);

    if (!vs || !fs) {
        core::logError("createGPUDrivenPipelineForFormat: shader compile failed: %s", SDL_GetError());
        if (vs) SDL_ReleaseGPUShader(device_, vs);
        if (fs) SDL_ReleaseGPUShader(device_, fs);
        return nullptr;
    }

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = format;
    colorTarget.blend_state.enable_blend          = true;
    colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.vertex_shader   = vs;
    pipeInfo.fragment_shader = fs;
    pipeInfo.vertex_input_state.num_vertex_buffers    = 0;
    pipeInfo.vertex_input_state.num_vertex_attributes = 0;
    pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipeInfo.target_info.color_target_descriptions = &colorTarget;
    pipeInfo.target_info.num_color_targets         = 1;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &pipeInfo);
    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    return pipeline;
}

void SDLGPURenderDevice::createGPUDrivenIndexBuffer() {
    // 6 个索引、复用 4 个 quad 顶点 (vertIdx = gl_VertexIndex & 3)
    static const uint16_t quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_GPUBufferCreateInfo bufInfo{};
    bufInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    bufInfo.size  = sizeof(quadIdx);
    gpuDrivenQuadIndexBuf_ = SDL_CreateGPUBuffer(device_, &bufInfo);
    if (!gpuDrivenQuadIndexBuf_) {
        core::logError("createGPUDrivenIndexBuffer: SDL_CreateGPUBuffer failed: %s", SDL_GetError());
        return;
    }

    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = sizeof(quadIdx);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tbInfo);
    void* mapped = SDL_MapGPUTransferBuffer(device_, tb, false);
    memcpy(mapped, quadIdx, sizeof(quadIdx));
    SDL_UnmapGPUTransferBuffer(device_, tb);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{ tb, 0 };
    SDL_GPUBufferRegion           dst{ gpuDrivenQuadIndexBuf_, 0, sizeof(quadIdx) };
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
}

// GPU-driven 方式提交一帧 sprite 渲染
// 与 renderCmdsToTarget 不同: 此函数不构建 CPU 侧几何体，而是将 sprite 数据
// 和可见索引直接绑定为 storage buffer，由 GPU 顶点着色器自行读取。
// 按 params.batches 分组，每组绑定不同的纹理并 instanced draw。
void SDLGPURenderDevice::submitGPUDrivenPass(const PassSubmitInfo& info,
                                             const GPURenderParams& params) {
    if (!gpuCmdBuf_ || !swapchainTex_) return;

    CameraData cam = info.camera;
    if (cam.viewportW == 0) cam.viewportW = static_cast<int>(swapW_);
    if (cam.viewportH == 0) cam.viewportH = static_cast<int>(swapH_);

    auto beginAndEndEmpty = [&]() {
        if (!info.clearEnabled) return;
        SDL_GPUColorTargetInfo colorTarget{};
        colorTarget.texture     = swapchainTex_;
        colorTarget.load_op     = SDL_GPU_LOADOP_CLEAR;
        colorTarget.store_op    = SDL_GPU_STOREOP_STORE;
        colorTarget.clear_color = SDL_FColor{
            info.clearColor.r / 255.f, info.clearColor.g / 255.f,
            info.clearColor.b / 255.f, info.clearColor.a / 255.f
        };
        SDL_GPURenderPass* p = SDL_BeginGPURenderPass(gpuCmdBuf_, &colorTarget, 1, nullptr);
        SDL_EndGPURenderPass(p);
    };

    // 没有可见 sprite 时只处理清屏，其他 pass 会接力补画。
    if (params.visibleCount == 0) { beginAndEndEmpty(); return; }

    if (!gpuDrivenPipeline_ || !gpuDrivenQuadIndexBuf_) {
        core::logError("submitGPUDrivenPass: GPU-driven pipeline not ready");
        beginAndEndEmpty();
        return;
    }

    if (!buffers_.valid(params.spriteBuffer) || !buffers_.valid(params.visibleIndexBuffer)) {
        beginAndEndEmpty();
        return;
    }
    if (params.batches.empty()) { beginAndEndEmpty(); return; }

    BufferEntry& spriteBuf  = buffers_.get(params.spriteBuffer);
    BufferEntry& visibleBuf = buffers_.get(params.visibleIndexBuffer);

    // viewProj，列主序 = view * proj（与现有 renderCmdsToTarget 同步）
    float proj[16], view[16], viewProj[16];
    const float zoom = (cam.zoom > 0.f) ? cam.zoom : 1.f;
    buildOrthoProjectionMatrix(static_cast<float>(cam.viewportW),
                               static_cast<float>(cam.viewportH), proj);
    buildViewMatrix(cam.x, cam.y, zoom, cam.rotation, view);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            viewProj[i * 4 + j] = 0.f;
            for (int k = 0; k < 4; ++k)
                viewProj[i * 4 + j] += view[i * 4 + k] * proj[k * 4 + j];
        }

    SDL_GPUColorTargetInfo colorTarget{};
    colorTarget.texture     = swapchainTex_;
    colorTarget.load_op     = info.clearEnabled ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    colorTarget.store_op    = SDL_GPU_STOREOP_STORE;
    colorTarget.clear_color = SDL_FColor{
        info.clearColor.r / 255.f, info.clearColor.g / 255.f,
        info.clearColor.b / 255.f, info.clearColor.a / 255.f
    };

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(gpuCmdBuf_, &colorTarget, 1, nullptr);
    if (!pass) {
        core::logError("submitGPUDrivenPass: SDL_BeginGPURenderPass failed: %s", SDL_GetError());
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass, gpuDrivenPipeline_);

    SDL_PushGPUVertexUniformData(gpuCmdBuf_, 0, viewProj, sizeof(viewProj));

    SDL_GPUBuffer* vsStorage[2] = { spriteBuf.gpuBuffer, visibleBuf.gpuBuffer };
    SDL_BindGPUVertexStorageBuffers(pass, 0, vsStorage, 2);

    SDL_GPUBufferBinding idxBinding{ gpuDrivenQuadIndexBuf_, 0 };
    SDL_BindGPUIndexBuffer(pass, &idxBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

    TextureHandle currentTex{};
    for (const GPUDrawBatch& batch : params.batches) {
        if (batch.instanceCount == 0) continue;
        if (!textures_.valid(batch.texture)) continue;

        if (!(batch.texture == currentTex)) {
            const TextureEntry& te = textures_.get(batch.texture);
            SDL_GPUTextureSamplerBinding tb{ te.gpuTex, te.sampler };
            SDL_BindGPUFragmentSamplers(pass, 0, &tb, 1);
            currentTex = batch.texture;
        }

        SDL_DrawGPUIndexedPrimitives(pass, 6, batch.instanceCount, 0, 0, batch.firstInstance);
    }

    SDL_EndGPURenderPass(pass);
}

} // namespace backend
