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
#include <vector>

#include <SDL3/SDL.h>

#include "../CommandBuffer.h"
#include "../RenderGraph.h"
#include "../../../core/Assert.h"
#include "../../../core/Logger.h"

// 预编译 SPIRV 着色器二进制 (CMake 通过 glslc 编译 .glsl → .spv)
#include "sprite_vert_spv.h"          // 标准 sprite 顶点着色器: pos+uv+color → clip space
#include "sprite_frag_spv.h"          // 标准 sprite 片段着色器: 纹理采样 × 顶点颜色
#include "msdf_frag_spv.h"            // MSDF 字体片段着色器: median() 抗锯齿
#include "sprite_gpu_vert_spv.h"      // GPU-driven 顶点着色器: 从 storage buffer 读 sprite 数据
#include "sprite_gpu_frag_spv.h"      // GPU-driven 片段着色器
#include "particle_gpu_vert_spv.h"    // GPU 粒子顶点着色器: 从 storage buffer 展开 quad
#include "particle_gpu_frag_spv.h"    // GPU 粒子片段着色器
#include "lighting2d_spv.h"           // L3 2D lighting compute shader
#include "lighting2d_cull_spv.h"      // L3 screen-tile light-list builder
#include "lighting2d_blur_spv.h"      // L4 separable blur for soft lighting
#include "lighting2d_composite_frag_spv.h" // WorldColor * Lighting composite
#ifdef QGAME_HAS_DXIL_SHADERS
#include "sprite_vert_dxil.h"         // DXIL 版本的着色器 (Windows D3D12 后端)
#include "sprite_frag_dxil.h"
#include "msdf_frag_dxil.h"
#include "particle_gpu_vert_dxil.h"
#include "particle_gpu_frag_dxil.h"
#include "lighting2d_dxil.h"
#include "lighting2d_cull_dxil.h"
#include "lighting2d_blur_dxil.h"
#include "lighting2d_composite_frag_dxil.h"
#endif

namespace backend {

namespace {

// L3 tiled lighting uses screen-space tiles, not lighting-texture texels, so a
// tile stays stable if the lighting overlay changes resolution later. 32 px is
// deliberately conservative for 2D: it keeps tile counts small while still
// rejecting most small-radius lights before the expensive segment ray tests.
constexpr uint32_t kLighting2DTileSize = 32;

// The fixed per-tile list keeps the first L3 implementation simple and avoids
// global atomics/prefix sums. Overflow is handled by clamping; tiny dynamic
// lights still behave well, while pathological cases degrade by ignoring the
// least recently scanned lights in an overcrowded tile.
constexpr uint32_t kLighting2DMaxLightsPerTile = 64;

} // namespace

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

    // Capability policy for the SDL_GPU backend:
    // - Storage buffers and compute are part of the intended mainline path.
    // - GPU-driven sprite rendering is enabled only when its pipeline and
    //   static quad index buffer were actually created.
    // - Storage texture support is now probed by actually asking SDL/Vulkan
    //   whether RGBA8 can be used as a compute-write + sampler texture and by
    //   creating one tiny texture. This is still a capability probe, not the
    //   complete 2D lighting pass.
    // - Texture array and timestamp query stay conservative until each feature
    //   has a real conformance test.
    capabilities_.supportsCompute = true;
    capabilities_.supportsStorageBuffer = true;
    capabilities_.supportsStorageTexture = probeStorageTextureSupport();
    capabilities_.supportsGPUDrivenSprite = (gpuDrivenPipeline_ != nullptr &&
                                             gpuDrivenQuadIndexBuf_ != nullptr);
    capabilities_.supportsIndirectDraw = true;
    capabilities_.supportsTextureArray = false;
    capabilities_.supportsTimestampQuery = false;
    capabilities_.supportsWorldOffscreenColor = true;
    capabilities_.supportsSampledRenderTarget = true;
    capabilities_.supportsLighting2D = capabilities_.supportsCompute &&
        capabilities_.supportsStorageBuffer &&
        capabilities_.supportsStorageTexture;
    capabilities_.backendName = "SDL_GPU(Vulkan-first)";

    // 6. 1×1 R8 dummy region 纹理（无 region 时绑到 sampler 槽 1）
    {
        TextureDesc dd{};
        const uint8_t zero = 0;
        dd.data = &zero;
        dd.width = 1;
        dd.height = 1;
        dd.channels = 1;
        dd.format = TextureFormat::R8;
        dd.filter = TextureFilter::Nearest;
        dummyRegionTex_ = createTexture(dd);
    }
    {
        TextureDesc dd{};
        const uint8_t white[4] = {255, 255, 255, 255};
        dd.data = white;
        dd.width = 1;
        dd.height = 1;
        dd.channels = 4;
        dd.format = TextureFormat::RGBA8;
        dd.filter = TextureFilter::Linear;
        lighting2DWhiteTexture_ = createTexture(dd);
    }

    // Graphics-side Light2D composite helper. The compute lighting texture is
    // still generated below, but this radial sprite path makes Light2D visibly
    // affect the scene while the final render-graph multiply/composite path is
    // being hardened.
    {
        constexpr int kSize = 128;
        std::vector<uint8_t> pixels(static_cast<size_t>(kSize * kSize * 4), 0);
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(kSize) * 2.f - 1.f;
                const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(kSize) * 2.f - 1.f;
                const float d = std::sqrt(nx * nx + ny * ny);
                const float falloff = std::max(0.f, 1.f - d);
                const float alpha = falloff * falloff;
                const size_t idx = static_cast<size_t>((y * kSize + x) * 4);
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 255;
                pixels[idx + 2] = 255;
                pixels[idx + 3] = static_cast<uint8_t>(std::min(255.f, alpha * 255.f + 0.5f));
            }
        }

        TextureDesc lightDesc{};
        lightDesc.width = kSize;
        lightDesc.height = kSize;
        lightDesc.channels = 4;
        lightDesc.data = pixels.data();
        lightDesc.filter = TextureFilter::Linear;
        lighting2DRadialTexture_ = createTexture(lightDesc);
    }

    core::logInfo("SDLGPURenderDevice initialized");
}

// ═══════════════════════════════════════════════════════════════════════════════
// 帧生命周期
// ═══════════════════════════════════════════════════════════════════════════════

void SDLGPURenderDevice::beginFrame() {
    if (!device_) {
        return;
    }
    resetFrameStats();

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

    // 释放 dummy region 纹理
    if (textures_.valid(dummyRegionTex_)) {
        destroyTexture(dummyRegionTex_);
        dummyRegionTex_ = {};
    }

    // 释放离屏渲染目标 (editor + offscreen)
    if (textures_.valid(editorRenderTarget_)) {
        destroyTexture(editorRenderTarget_);
        editorRenderTarget_ = {};
    }

    if (textures_.valid(offscreenRenderTarget_)) {
        destroyTexture(offscreenRenderTarget_);
        offscreenRenderTarget_ = {};
    }
    if (textures_.valid(worldColorTarget_)) {
        destroyTexture(worldColorTarget_);
        worldColorTarget_ = {};
    }

    // 释放 graphics pipelines
    if (pipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_); pipeline_ = nullptr; }
    if (offscreenPipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, offscreenPipeline_); offscreenPipeline_ = nullptr; }
    if (msdfPipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, msdfPipeline_); msdfPipeline_ = nullptr; }
    if (msdfOffscreenPipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, msdfOffscreenPipeline_); msdfOffscreenPipeline_ = nullptr; }
    if (gpuDrivenPipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, gpuDrivenPipeline_); gpuDrivenPipeline_ = nullptr; }
    if (gpuDrivenOffscreenPipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, gpuDrivenOffscreenPipeline_); gpuDrivenOffscreenPipeline_ = nullptr; }
    if (particlePipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, particlePipeline_); particlePipeline_ = nullptr; }
    if (particleOffscreenPipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, particleOffscreenPipeline_); particleOffscreenPipeline_ = nullptr; }
    if (lightingCompositePipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, lightingCompositePipeline_); lightingCompositePipeline_ = nullptr; }
    if (gpuDrivenQuadIndexBuf_) { SDL_ReleaseGPUBuffer(device_, gpuDrivenQuadIndexBuf_); gpuDrivenQuadIndexBuf_ = nullptr; }

    if (lighting2DComputePipeline_.valid()) {
        destroyComputePipeline(lighting2DComputePipeline_);
        lighting2DComputePipeline_ = {};
    }
    if (lighting2DCullPipeline_.valid()) {
        destroyComputePipeline(lighting2DCullPipeline_);
        lighting2DCullPipeline_ = {};
    }
    if (lighting2DBlurPipeline_.valid()) {
        destroyComputePipeline(lighting2DBlurPipeline_);
        lighting2DBlurPipeline_ = {};
    }
    if (textures_.valid(lighting2DTexture_)) {
        destroyTexture(lighting2DTexture_);
        lighting2DTexture_ = {};
    }
    if (textures_.valid(lighting2DBlurTexture_)) {
        destroyTexture(lighting2DBlurTexture_);
        lighting2DBlurTexture_ = {};
    }
    if (textures_.valid(lighting2DWhiteTexture_)) {
        destroyTexture(lighting2DWhiteTexture_);
        lighting2DWhiteTexture_ = {};
    }
    if (textures_.valid(lighting2DRadialTexture_)) {
        destroyTexture(lighting2DRadialTexture_);
        lighting2DRadialTexture_ = {};
    }
    if (buffers_.valid(lighting2DLightBuffer_)) {
        destroyBuffer(lighting2DLightBuffer_);
        lighting2DLightBuffer_ = {};
    }
    if (buffers_.valid(lighting2DSegmentBuffer_)) {
        destroyBuffer(lighting2DSegmentBuffer_);
        lighting2DSegmentBuffer_ = {};
    }
    if (buffers_.valid(lighting2DReflectorBuffer_)) {
        destroyBuffer(lighting2DReflectorBuffer_);
        lighting2DReflectorBuffer_ = {};
    }
    if (buffers_.valid(lighting2DTileRangeBuffer_)) {
        destroyBuffer(lighting2DTileRangeBuffer_);
        lighting2DTileRangeBuffer_ = {};
    }
    if (buffers_.valid(lighting2DTileIndexBuffer_)) {
        destroyBuffer(lighting2DTileIndexBuffer_);
        lighting2DTileIndexBuffer_ = {};
    }

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

    const bool isR8 = (desc.format == TextureFormat::R8);
    const uint32_t bytesPerPixel = isR8 ? 1u : 4u;

    // 1. 创建 GPU 纹理对象 — 2D, RGBA8 或 R8, 仅采样使用
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = isR8 ? SDL_GPU_TEXTUREFORMAT_R8_UNORM
                       : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
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
    const size_t dataSize = static_cast<size_t>(desc.width) * static_cast<size_t>(desc.height) * bytesPerPixel;
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

TextureHandle SDLGPURenderDevice::createStorageTexture(int width, int height) {
    if (!device_ || width <= 0 || height <= 0) return {};

    // Lighting L4 writes this texture from compute, reads it during the blur
    // passes, then samples it through the regular sprite pipeline. No
    // color-target usage is needed for the prototype; compute owns the pixels.
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
                 SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
                 SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<uint32_t>(width);
    info.height = static_cast<uint32_t>(height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* gpuTex = SDL_CreateGPUTexture(device_, &info);
    if (!gpuTex) {
        core::logError("createStorageTexture failed (%dx%d): %s", width, height, SDL_GetError());
        return {};
    }

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
    frameStats_.uploadBytes += static_cast<uint64_t>(size);
    frameStats_.uploadCallCount++;
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
            frameStats_.computeDispatchCount++;
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

    bool                              currentHasRegion = false;
    TextureHandle                     currentRegionTex{};
    std::array<core::Color, 16>       currentRegionTints{};

    // Scissor 栈 (屏幕像素 / framebuffer 坐标，整数化)。每次 push 取与栈顶交集；
    // pop 回退到上一层。栈空时 hasScissor=false，绘制时不调用 SetGPUScissor。
    struct ScissorRect { int x, y, w, h; };
    std::vector<ScissorRect> scissorStack;
    bool currentHasScissor = false;
    ScissorRect currentScissor{};

    auto flush = [&]() {
        if (static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
            BatchSegment seg{};
            seg.tex        = currentTex;
            seg.idxOffset  = batchIdxStart;
            seg.idxCount   = static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart;
            seg.vertOffset = batchVertStart;
            seg.isFont     = currentIsFont;
            seg.pxRange    = currentPxRange;
            seg.hasScissor = currentHasScissor;
            if (currentHasScissor) {
                seg.scissorX = currentScissor.x;
                seg.scissorY = currentScissor.y;
                seg.scissorW = currentScissor.w;
                seg.scissorH = currentScissor.h;
            }
            seg.hasRegion   = currentHasRegion;
            seg.regionTex   = currentRegionTex;
            seg.regionTints = currentRegionTints;
            batches.push_back(seg);
            batchIdxStart  = static_cast<uint32_t>(batchIdx_.size());
            batchVertStart = static_cast<int32_t>(batchVerts_.size());
        }
    };
    auto regionStateDiffers = [&](bool hasRegion, TextureHandle regionTex,
                                  const std::array<core::Color, 16>* tints) {
        if (currentHasRegion != hasRegion) return true;
        if (!hasRegion) return false;
        if (currentRegionTex != regionTex) return true;
        return std::memcmp(currentRegionTints.data(), tints->data(),
                           sizeof(core::Color) * 16) != 0;
    };
    auto maybeFlush = [&](TextureHandle tex, bool isFont = false, float pxRange = 4.0f,
                          bool hasRegion = false, TextureHandle regionTex = {},
                          const std::array<core::Color, 16>* regionTints = nullptr) {
        const bool batchFull =
            (batchVerts_.size() - static_cast<size_t>(batchVertStart) >= MAX_SPRITES_PER_BATCH * 4);
        const bool regionDiff = regionStateDiffers(hasRegion, regionTex, regionTints);
        if (!hasCurrent || tex != currentTex || batchFull ||
            currentIsFont != isFont || (isFont && currentPxRange != pxRange) ||
            regionDiff) {
            flush();
            currentTex = tex;
            currentIsFont = isFont;
            currentPxRange = pxRange;
            currentHasRegion = hasRegion;
            currentRegionTex = regionTex;
            if (hasRegion && regionTints) {
                currentRegionTints = *regionTints;
            } else {
                currentRegionTints = {};
            }
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
            maybeFlush(s->texture, false, 4.0f,
                       s->hasRegion, s->regionTex, &s->regionTints);
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
        else if (auto* ps = std::get_if<PushScissorCmd>(cmd)) {
            ScissorRect r{
                static_cast<int>(ps->rect.x),
                static_cast<int>(ps->rect.y),
                static_cast<int>(ps->rect.w),
                static_cast<int>(ps->rect.h)
            };
            if (!scissorStack.empty()) {
                const ScissorRect& top = scissorStack.back();
                const int x0 = std::max(r.x, top.x);
                const int y0 = std::max(r.y, top.y);
                const int x1 = std::min(r.x + r.w, top.x + top.w);
                const int y1 = std::min(r.y + r.h, top.y + top.h);
                r.x = x0; r.y = y0;
                r.w = std::max(0, x1 - x0);
                r.h = std::max(0, y1 - y0);
            }
            scissorStack.push_back(r);
            flush();
            currentHasScissor = true;
            currentScissor    = r;
            continue;
        }
        else if (auto* /*pp*/ pp = std::get_if<PopScissorCmd>(cmd)) {
            (void)pp;
            if (!scissorStack.empty()) scissorStack.pop_back();
            flush();
            if (scissorStack.empty()) {
                currentHasScissor = false;
            } else {
                currentHasScissor = true;
                currentScissor    = scissorStack.back();
            }
            continue;
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
        frameStats_.uploadBytes += static_cast<uint64_t>(vSize + iSize);
        frameStats_.uploadCallCount++;

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
                if (segment.isFont) {
                    SDL_GPUTextureSamplerBinding binding{ entry.gpuTex, entry.sampler };
                    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
                } else {
                    // Sprite pipeline: 2 个 sampler 槽 (base + region)
                    TextureHandle rtex = (segment.hasRegion && textures_.valid(segment.regionTex))
                        ? segment.regionTex : dummyRegionTex_;
                    TextureEntry* rEntry = textures_.valid(rtex) ? &textures_.get(rtex) : nullptr;
                    SDL_GPUTextureSamplerBinding bindings[2]{};
                    bindings[0] = { entry.gpuTex, entry.sampler };
                    bindings[1] = rEntry ? SDL_GPUTextureSamplerBinding{ rEntry->gpuTex, rEntry->sampler }
                                         : bindings[0];
                    SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
                    frameStats_.textureBindCount += 2;
                }
                if (segment.isFont) {
                    frameStats_.textureBindCount++;
                }
            }

            if (segment.isFont) {
                float pxRange = segment.pxRange;
                SDL_PushGPUFragmentUniformData(cmdBuf, 0, &pxRange, sizeof(pxRange));
            } else {
                // Sprite UBO: regionTints[16] (float4×16) + hasRegion (int) + 12B pad → 272B
                struct alignas(16) TintUBO {
                    float regionTints[16][4];
                    int   hasRegion;
                    int   _pad0, _pad1, _pad2;
                } ubo{};
                for (int i = 0; i < 16; ++i) {
                    ubo.regionTints[i][0] = segment.regionTints[i].r / 255.f;
                    ubo.regionTints[i][1] = segment.regionTints[i].g / 255.f;
                    ubo.regionTints[i][2] = segment.regionTints[i].b / 255.f;
                    ubo.regionTints[i][3] = segment.regionTints[i].a / 255.f;
                }
                ubo.hasRegion = segment.hasRegion ? 1 : 0;
                SDL_PushGPUFragmentUniformData(cmdBuf, 0, &ubo, sizeof(ubo));
            }

            if (segment.hasScissor) {
                // 与 framebuffer 取交集，避免越界 (SDL 要求 scissor 在 target 内)。
                const int fbW = static_cast<int>(targetWidth);
                const int fbH = static_cast<int>(targetHeight);
                const int x0 = std::max(0, segment.scissorX);
                const int y0 = std::max(0, segment.scissorY);
                const int x1 = std::min(fbW, segment.scissorX + segment.scissorW);
                const int y1 = std::min(fbH, segment.scissorY + segment.scissorH);
                SDL_Rect scRect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
                SDL_SetGPUScissor(pass, &scRect);
            } else {
                SDL_Rect full{ 0, 0, static_cast<int>(targetWidth), static_cast<int>(targetHeight) };
                SDL_SetGPUScissor(pass, &full);
            }

            SDL_DrawGPUIndexedPrimitives(pass, segment.idxCount, 1, segment.idxOffset, segment.vertOffset, 0);
            frameStats_.drawCallCount++;
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
        gpuDrivenOffscreenPipeline_ = createGPUDrivenPipelineForFormat(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        if (!gpuDrivenPipeline_) {
            core::logError("createPipeline: failed to create GPU-driven pipeline");
        } else {
            createGPUDrivenIndexBuffer();
        }
    }

    // 粒子渲染同样使用 HLSL 自动生成的 SPIRV/DXIL 产物。compute shader
    // 由 engine 层 GPUParticleRenderer 创建；这里仅创建 instanced quad pipeline。
    particlePipeline_ = createParticlePipelineForFormat(swapchainFormat);
    particleOffscreenPipeline_ = createParticlePipelineForFormat(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    if (!particlePipeline_) {
        core::logError("createPipeline: failed to create particle pipeline");
    } else if (!gpuDrivenQuadIndexBuf_) {
        createGPUDrivenIndexBuffer();
    }

    lightingCompositePipeline_ = createLightingCompositePipelineForFormat(swapchainFormat);
    if (!lightingCompositePipeline_) {
        core::logError("createPipeline: failed to create lighting composite pipeline");
    }

    core::logInfo("Pipelines created (swapchain: 0x%x, offscreen: R8G8B8A8, gpuDriven: %s, particles: %s, lightingComposite: %s)",
                  static_cast<int>(swapchainFormat),
                  gpuDrivenPipeline_ ? "yes" : "no",
                  particlePipeline_ ? "yes" : "no",
                  lightingCompositePipeline_ ? "yes" : "no");
}

bool SDLGPURenderDevice::probeStorageTextureSupport() {
    if (!device_) return false;

    // L2 lighting needs a texture that compute can write and a later graphics
    // pass can sample. RGBA8 is the first target format because it maps cleanly
    // to the existing sprite/composite color path and is broadly supported for
    // storage use in SDL GPU's documented format table.
    constexpr SDL_GPUTextureUsageFlags kLightingUsage =
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
        SDL_GPU_TEXTUREUSAGE_SAMPLER;
    constexpr SDL_GPUTextureFormat kLightingFormat =
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    if (!SDL_GPUTextureSupportsFormat(device_, kLightingFormat,
                                      SDL_GPU_TEXTURETYPE_2D,
                                      kLightingUsage)) {
        core::logWarn("2D lighting storage texture probe: RGBA8 read/write+sample unsupported");
        return false;
    }

    // Querying support is useful, but actually creating the resource catches
    // driver/backend problems earlier and gives demo3 a trustworthy status
    // line. The texture is immediately released; real lighting targets will be
    // created per viewport size by the future Light2D renderer.
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = kLightingFormat;
    info.usage = kLightingUsage;
    info.width = 4;
    info.height = 4;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* probe = SDL_CreateGPUTexture(device_, &info);
    if (!probe) {
        core::logWarn("2D lighting storage texture probe create failed: %s", SDL_GetError());
        return false;
    }

    SDL_ReleaseGPUTexture(device_, probe);
    core::logInfo("2D lighting storage texture probe: RGBA8 compute read/write + sampler supported");
    return true;
}

bool SDLGPURenderDevice::ensureLighting2DResources(uint32_t viewportW, uint32_t viewportH,
                                                   uint32_t lightCount, uint32_t segmentCount,
                                                   uint32_t reflectorCount) {
    if (!capabilities_.supportsStorageTexture || viewportW == 0 || viewportH == 0) {
        return false;
    }

    if (!lighting2DComputePipeline_.valid()) {
        ComputePipelineDesc desc{};
        desc.spirvCode = lighting2d_spv;
        desc.spirvSize = lighting2d_spv_size;
#ifdef QGAME_HAS_DXIL_SHADERS
        desc.dxilCode = lighting2d_dxil;
        desc.dxilSize = lighting2d_dxil_size;
#endif
        desc.entryPoint = "main";
        desc.threadCountX = 8;
        desc.threadCountY = 8;
        desc.threadCountZ = 1;
        // Lighting reads scene lights, occluder segments, reflector regions,
        // tile ranges, and the per-tile light index list produced by the cull
        // pass directly before it. SDL_GPU inserts the required pass ordering
        // for this command buffer.
        desc.numReadonlyStorageBuffers = 5;
        desc.numReadwriteStorageTextures = 1;
        desc.numUniformBuffers = 1;
        lighting2DComputePipeline_ = createComputePipeline(desc);
        if (!lighting2DComputePipeline_.valid()) {
            core::logError("ensureLighting2DResources: failed to create lighting2d compute pipeline");
            return false;
        }
    }

    if (!lighting2DCullPipeline_.valid()) {
        ComputePipelineDesc desc{};
        desc.spirvCode = lighting2d_cull_spv;
        desc.spirvSize = lighting2d_cull_spv_size;
#ifdef QGAME_HAS_DXIL_SHADERS
        desc.dxilCode = lighting2d_cull_dxil;
        desc.dxilSize = lighting2d_cull_dxil_size;
#endif
        desc.entryPoint = "main";
        desc.threadCountX = 64;
        desc.threadCountY = 1;
        desc.threadCountZ = 1;
        // Culling reads only the light array and writes two compact buffers:
        // a uint2 range per tile and a fixed-size uint index list per tile.
        desc.numReadonlyStorageBuffers = 1;
        desc.numReadwriteStorageBuffers = 2;
        desc.numUniformBuffers = 1;
        lighting2DCullPipeline_ = createComputePipeline(desc);
        if (!lighting2DCullPipeline_.valid()) {
            core::logError("ensureLighting2DResources: failed to create lighting2d cull pipeline");
            return false;
        }
    }

    if (!lighting2DBlurPipeline_.valid()) {
        ComputePipelineDesc desc{};
        desc.spirvCode = lighting2d_blur_spv;
        desc.spirvSize = lighting2d_blur_spv_size;
#ifdef QGAME_HAS_DXIL_SHADERS
        desc.dxilCode = lighting2d_blur_dxil;
        desc.dxilSize = lighting2d_blur_dxil_size;
#endif
        desc.entryPoint = "main";
        desc.threadCountX = 8;
        desc.threadCountY = 8;
        desc.threadCountZ = 1;
        // Blur reads one storage texture and writes one storage texture. The
        // same pipeline is dispatched twice with different source/destination
        // resources and a direction uniform: horizontal, then vertical.
        desc.numReadonlyStorageTextures = 1;
        desc.numReadwriteStorageTextures = 1;
        desc.numUniformBuffers = 1;
        lighting2DBlurPipeline_ = createComputePipeline(desc);
        if (!lighting2DBlurPipeline_.valid()) {
            core::logError("ensureLighting2DResources: failed to create lighting2d blur pipeline");
            return false;
        }
    }

    const int desiredW = static_cast<int>(std::max(1u, (viewportW + 1u) / 2u));
    const int desiredH = static_cast<int>(std::max(1u, (viewportH + 1u) / 2u));
    if (!textures_.valid(lighting2DTexture_) ||
        lighting2DTextureWidth_ != desiredW ||
        lighting2DTextureHeight_ != desiredH) {
        if (textures_.valid(lighting2DTexture_)) {
            destroyTexture(lighting2DTexture_);
        }
        lighting2DTexture_ = createStorageTexture(desiredW, desiredH);
        lighting2DTextureWidth_ = desiredW;
        lighting2DTextureHeight_ = desiredH;
        if (!lighting2DTexture_.valid()) return false;
    }
    if (!textures_.valid(lighting2DBlurTexture_) ||
        textures_.get(lighting2DBlurTexture_).width != desiredW ||
        textures_.get(lighting2DBlurTexture_).height != desiredH) {
        if (textures_.valid(lighting2DBlurTexture_)) {
            destroyTexture(lighting2DBlurTexture_);
        }
        lighting2DBlurTexture_ = createStorageTexture(desiredW, desiredH);
        if (!lighting2DBlurTexture_.valid()) return false;
    }

    const uint32_t lightCapacity = std::max(1u, lightCount);
    if (!buffers_.valid(lighting2DLightBuffer_) || lighting2DLightCapacity_ < lightCapacity) {
        if (buffers_.valid(lighting2DLightBuffer_)) {
            destroyBuffer(lighting2DLightBuffer_);
        }
        BufferDesc bd{};
        bd.size = sizeof(Light2DPoint) * lightCapacity;
        bd.usage = BufferUsage::Storage;
        lighting2DLightBuffer_ = createBuffer(bd);
        lighting2DLightCapacity_ = lightCapacity;
        if (!lighting2DLightBuffer_.valid()) return false;
    }

    const uint32_t segmentCapacity = std::max(1u, segmentCount);
    if (!buffers_.valid(lighting2DSegmentBuffer_) || lighting2DSegmentCapacity_ < segmentCapacity) {
        if (buffers_.valid(lighting2DSegmentBuffer_)) {
            destroyBuffer(lighting2DSegmentBuffer_);
        }
        BufferDesc bd{};
        bd.size = sizeof(Light2DSegment) * segmentCapacity;
        bd.usage = BufferUsage::Storage;
        lighting2DSegmentBuffer_ = createBuffer(bd);
        lighting2DSegmentCapacity_ = segmentCapacity;
        if (!lighting2DSegmentBuffer_.valid()) return false;
    }

    const uint32_t reflectorCapacity = std::max(1u, reflectorCount);
    if (!buffers_.valid(lighting2DReflectorBuffer_) ||
        lighting2DReflectorCapacity_ < reflectorCapacity) {
        if (buffers_.valid(lighting2DReflectorBuffer_)) {
            destroyBuffer(lighting2DReflectorBuffer_);
        }
        BufferDesc bd{};
        bd.size = sizeof(IRenderDevice::Reflector2DRegion) * reflectorCapacity;
        bd.usage = BufferUsage::Storage;
        lighting2DReflectorBuffer_ = createBuffer(bd);
        lighting2DReflectorCapacity_ = reflectorCapacity;
        if (!lighting2DReflectorBuffer_.valid()) return false;
    }

    const uint32_t tileCols = (viewportW + kLighting2DTileSize - 1u) / kLighting2DTileSize;
    const uint32_t tileRows = (viewportH + kLighting2DTileSize - 1u) / kLighting2DTileSize;
    const uint32_t tileCapacity = std::max(1u, tileCols * tileRows);
    if (!buffers_.valid(lighting2DTileRangeBuffer_) || lighting2DTileCapacity_ < tileCapacity) {
        if (buffers_.valid(lighting2DTileRangeBuffer_)) {
            destroyBuffer(lighting2DTileRangeBuffer_);
        }
        BufferDesc bd{};
        bd.size = sizeof(uint32_t) * 2u * tileCapacity;
        bd.usage = BufferUsage::Storage;
        lighting2DTileRangeBuffer_ = createBuffer(bd);
        lighting2DTileCapacity_ = tileCapacity;
        if (!lighting2DTileRangeBuffer_.valid()) return false;
    }

    const uint32_t tileIndexCapacity = std::max(1u, tileCapacity * kLighting2DMaxLightsPerTile);
    if (!buffers_.valid(lighting2DTileIndexBuffer_) ||
        lighting2DTileIndexCapacity_ < tileIndexCapacity) {
        if (buffers_.valid(lighting2DTileIndexBuffer_)) {
            destroyBuffer(lighting2DTileIndexBuffer_);
        }
        BufferDesc bd{};
        bd.size = sizeof(uint32_t) * tileIndexCapacity;
        bd.usage = BufferUsage::Storage;
        lighting2DTileIndexBuffer_ = createBuffer(bd);
        lighting2DTileIndexCapacity_ = tileIndexCapacity;
        if (!lighting2DTileIndexBuffer_.valid()) return false;
    }

    return true;
}

// 创建标准 sprite/tile 渲染管线 (给定颜色目标格式)
// 顶点布局: pos(2f) | uv(2f) | color(4ub normalised)
// 片元: 纹理采样 × 顶点颜色, alpha blend
SDL_GPUGraphicsPipeline* SDLGPURenderDevice::createPipelineForFormat(SDL_GPUTextureFormat format) {
    SDL_GPUShader* vs = nullptr;
    SDL_GPUShader* fs = nullptr;
    if (shaderFormat_ == SDL_GPU_SHADERFORMAT_SPIRV) {
        vs = loadShader(sprite_vert_spv, sprite_vert_spv_size, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, SDL_GPU_SHADERFORMAT_SPIRV);
        fs = loadShader(sprite_frag_spv, sprite_frag_spv_size, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1, SDL_GPU_SHADERFORMAT_SPIRV);
#ifdef QGAME_HAS_DXIL_SHADERS
    } else if (shaderFormat_ == SDL_GPU_SHADERFORMAT_DXIL) {
        vs = loadShader(sprite_vert_dxil, sprite_vert_dxil_size, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, SDL_GPU_SHADERFORMAT_DXIL);
        fs = loadShader(sprite_frag_dxil, sprite_frag_dxil_size, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1, SDL_GPU_SHADERFORMAT_DXIL);
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

SDL_GPUGraphicsPipeline* SDLGPURenderDevice::createParticlePipelineForFormat(SDL_GPUTextureFormat format) {
    // 粒子顶点没有传统 vertex buffer；每个 instance 直接从 storage buffer
    // 读取 GPUParticle，然后在 shader 中展开成 4 个 quad 顶点。
    SDL_GPUShaderCreateInfo vsInfo{};
    if (shaderFormat_ == SDL_GPU_SHADERFORMAT_SPIRV) {
        vsInfo.code      = particle_gpu_vert_spv;
        vsInfo.code_size = particle_gpu_vert_spv_size;
#ifdef QGAME_HAS_DXIL_SHADERS
    } else if (shaderFormat_ == SDL_GPU_SHADERFORMAT_DXIL) {
        vsInfo.code      = particle_gpu_vert_dxil;
        vsInfo.code_size = particle_gpu_vert_dxil_size;
#endif
    }
    vsInfo.entrypoint = "main";
    vsInfo.format    = shaderFormat_;
    vsInfo.stage     = SDL_GPU_SHADERSTAGE_VERTEX;
    vsInfo.num_samplers         = 0;
    vsInfo.num_storage_buffers  = 2; // set=0,b=0: particles, b=1: alive indices
    vsInfo.num_storage_textures = 0;
    vsInfo.num_uniform_buffers  = 1; // set=1,b=0: viewProj
    SDL_GPUShader* vs = SDL_CreateGPUShader(device_, &vsInfo);

    SDL_GPUShaderCreateInfo fsInfo{};
    if (shaderFormat_ == SDL_GPU_SHADERFORMAT_SPIRV) {
        fsInfo.code      = particle_gpu_frag_spv;
        fsInfo.code_size = particle_gpu_frag_spv_size;
#ifdef QGAME_HAS_DXIL_SHADERS
    } else if (shaderFormat_ == SDL_GPU_SHADERFORMAT_DXIL) {
        fsInfo.code      = particle_gpu_frag_dxil;
        fsInfo.code_size = particle_gpu_frag_dxil_size;
#endif
    }
    fsInfo.entrypoint = "main";
    fsInfo.format    = shaderFormat_;
    fsInfo.stage     = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsInfo.num_samplers        = 1;
    fsInfo.num_storage_buffers = 0;
    fsInfo.num_uniform_buffers = 0;
    SDL_GPUShader* fs = SDL_CreateGPUShader(device_, &fsInfo);

    if (!vs || !fs) {
        core::logError("createParticlePipelineForFormat: shader compile failed: %s", SDL_GetError());
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

SDL_GPUGraphicsPipeline* SDLGPURenderDevice::createLightingCompositePipelineForFormat(SDL_GPUTextureFormat format) {
    // The composite pass reuses the normal sprite vertex format so we can draw
    // one full-screen/world-aligned quad with the same CPU vertex upload path.
    // The fragment shader differs: it samples WorldColor and LightingTexture
    // and produces final scene color, replacing the old swapchain overlay.
    SDL_GPUShader* vs = nullptr;
    SDL_GPUShader* fs = nullptr;
    if (shaderFormat_ == SDL_GPU_SHADERFORMAT_SPIRV) {
        vs = loadShader(sprite_vert_spv, sprite_vert_spv_size,
                        SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, SDL_GPU_SHADERFORMAT_SPIRV);
        fs = loadShader(lighting2d_composite_frag_spv, lighting2d_composite_frag_spv_size,
                        SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1, SDL_GPU_SHADERFORMAT_SPIRV);
#ifdef QGAME_HAS_DXIL_SHADERS
    } else if (shaderFormat_ == SDL_GPU_SHADERFORMAT_DXIL) {
        vs = loadShader(sprite_vert_dxil, sprite_vert_dxil_size,
                        SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, SDL_GPU_SHADERFORMAT_DXIL);
        fs = loadShader(lighting2d_composite_frag_dxil, lighting2d_composite_frag_dxil_size,
                        SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1, SDL_GPU_SHADERFORMAT_DXIL);
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
    colorTarget.blend_state.enable_blend = false;

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
    submitGPUDrivenPassToTarget(info, params, swapchainTex_, swapW_, swapH_, gpuDrivenPipeline_);
}

void SDLGPURenderDevice::submitGPUDrivenPassToTarget(const PassSubmitInfo& info,
                                                     const GPURenderParams& params,
                                                     SDL_GPUTexture* target,
                                                     uint32_t targetWidth,
                                                     uint32_t targetHeight,
                                                     SDL_GPUGraphicsPipeline* pipeline) {
    if (!gpuCmdBuf_ || !target) return;

    CameraData cam = info.camera;
    if (cam.viewportW == 0) cam.viewportW = static_cast<int>(targetWidth);
    if (cam.viewportH == 0) cam.viewportH = static_cast<int>(targetHeight);

    auto beginAndEndEmpty = [&]() {
        if (!info.clearEnabled) return;
        SDL_GPUColorTargetInfo colorTarget{};
        colorTarget.texture     = target;
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

    if (!pipeline || !gpuDrivenQuadIndexBuf_) {
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
    colorTarget.texture     = target;
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
    SDL_BindGPUGraphicsPipeline(pass, pipeline);

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
            frameStats_.textureBindCount++;
        }

        SDL_DrawGPUIndexedPrimitives(pass, 6, batch.instanceCount, 0, 0, batch.firstInstance);
        frameStats_.gpuDrawBatchCount++;
        frameStats_.drawCallCount++;
    }

    SDL_EndGPURenderPass(pass);
}

bool SDLGPURenderDevice::submitRadialLightingFallback(const CameraData& cam,
                                                      const Lighting2DParams& params) {
    if (!gpuCmdBuf_ || !swapchainTex_) return false;
    if (!textures_.valid(lighting2DRadialTexture_)) return false;

    const TextureEntry& radial = textures_.get(lighting2DRadialTexture_);
    static std::vector<RenderCmd> lightCmds;
    static std::vector<const RenderCmd*> lightPtrs;
    lightCmds.clear();
    lightPtrs.clear();
    lightCmds.reserve(params.lights.size() + 1u);
    lightPtrs.reserve(params.lights.size() + 1u);

    if (textures_.valid(lighting2DWhiteTexture_)) {
        const float zoom = (cam.zoom > 0.f) ? cam.zoom : 1.f;
        const float visibleWorldW = static_cast<float>(cam.viewportW) / zoom;
        const float visibleWorldH = static_cast<float>(cam.viewportH) / zoom;
        const float darkness = std::min(0.58f,
                                        std::max(0.18f, 0.54f - params.ambientIntensity * 0.35f));

        DrawSpriteCmd ambient{};
        ambient.texture = lighting2DWhiteTexture_;
        ambient.x = cam.x;
        ambient.y = cam.y;
        ambient.scaleX = visibleWorldW;
        ambient.scaleY = visibleWorldH;
        ambient.pivotX = 0.5f;
        ambient.pivotY = 0.5f;
        ambient.srcRect = core::Rect{0.f, 0.f, 1.f, 1.f};
        ambient.layer = 9998;
        ambient.pass = engine::RenderPass::World;
        ambient.tint = core::Color{0, 0, 0,
            static_cast<uint8_t>(std::min(255.f, darkness * 255.f + 0.5f))};
        lightCmds.push_back(ambient);
    }

    for (const Light2DPoint& light : params.lights) {
        if (light.radius <= 0.f || light.intensity <= 0.f) continue;
        if ((light.layerMask & engine::renderPassBit(engine::RenderPass::World)) == 0u) continue;

        const float alpha01 = std::min(0.82f,
                                       std::max(0.06f, 0.10f + light.intensity * 0.18f)) *
                              std::max(0.f, std::min(1.f, light.colorA));
        if (alpha01 <= 0.001f) continue;

        DrawSpriteCmd cmd{};
        cmd.texture = lighting2DRadialTexture_;
        cmd.x = light.x;
        cmd.y = light.y;
        cmd.scaleX = (light.radius * 2.f) / static_cast<float>(radial.width);
        cmd.scaleY = (light.radius * 2.f) / static_cast<float>(radial.height);
        cmd.pivotX = 0.5f;
        cmd.pivotY = 0.5f;
        cmd.srcRect = core::Rect{0.f, 0.f,
                                 static_cast<float>(radial.width),
                                 static_cast<float>(radial.height)};
        cmd.layer = 9999;
        cmd.pass = engine::RenderPass::World;
        cmd.tint = core::Color{
            static_cast<uint8_t>(std::min(255.f, std::max(0.f, light.colorR * 255.f + 0.5f))),
            static_cast<uint8_t>(std::min(255.f, std::max(0.f, light.colorG * 255.f + 0.5f))),
            static_cast<uint8_t>(std::min(255.f, std::max(0.f, light.colorB * 255.f + 0.5f))),
            static_cast<uint8_t>(std::min(255.f, std::max(0.f, alpha01 * 255.f + 0.5f)))
        };

        lightCmds.push_back(cmd);
    }

    if (lightCmds.empty()) return false;
    for (const RenderCmd& cmd : lightCmds) {
        lightPtrs.push_back(&cmd);
    }
    renderCmdsToTarget(gpuCmdBuf_, pipeline_, lightPtrs, cam, false, core::Color::Black,
                       swapchainTex_, swapW_, swapH_);
    return true;
}

bool SDLGPURenderDevice::runLighting2DComputePass(const CameraData& cam,
                                                  const Lighting2DParams& params) {
    if (!gpuCmdBuf_) return false;
    if (!params.enabled || params.lights.empty()) return false;
    if (cam.viewportW <= 0 || cam.viewportH <= 0) return false;
    if (!capabilities_.supportsStorageTexture) return false;

    // L3 uploads all lights and all expanded occluder segments. The expensive
    // per-pixel loop no longer scans every light: a short compute pass first
    // writes a compact fixed-size light list for each screen-space tile.
    const uint32_t lightCount = static_cast<uint32_t>(params.lights.size());
    const uint32_t segmentCount = static_cast<uint32_t>(params.segments.size());
    const uint32_t reflectorCount = static_cast<uint32_t>(params.reflectors.size());
    if (!ensureLighting2DResources(static_cast<uint32_t>(cam.viewportW),
                                   static_cast<uint32_t>(cam.viewportH),
                                   lightCount,
                                   std::max(1u, segmentCount),
                                   std::max(1u, reflectorCount))) {
        return false;
    }

    uploadToBuffer(lighting2DLightBuffer_, params.lights.data(),
                   sizeof(Light2DPoint) * lightCount, 0);
    if (segmentCount > 0) {
        uploadToBuffer(lighting2DSegmentBuffer_, params.segments.data(),
                       sizeof(Light2DSegment) * segmentCount, 0);
    } else {
        Light2DSegment dummy{};
        uploadToBuffer(lighting2DSegmentBuffer_, &dummy, sizeof(dummy), 0);
    }
    if (reflectorCount > 0) {
        uploadToBuffer(lighting2DReflectorBuffer_, params.reflectors.data(),
                       sizeof(IRenderDevice::Reflector2DRegion) * reflectorCount, 0);
    } else {
        IRenderDevice::Reflector2DRegion dummy{};
        uploadToBuffer(lighting2DReflectorBuffer_, &dummy, sizeof(dummy), 0);
    }

    if (!computePipelines_.valid(lighting2DComputePipeline_)) return false;
    if (!computePipelines_.valid(lighting2DCullPipeline_)) return false;
    if (!computePipelines_.valid(lighting2DBlurPipeline_)) return false;
    if (!textures_.valid(lighting2DTexture_)) return false;
    if (!textures_.valid(lighting2DBlurTexture_)) return false;
    if (!buffers_.valid(lighting2DLightBuffer_)) return false;
    if (!buffers_.valid(lighting2DSegmentBuffer_)) return false;
    if (!buffers_.valid(lighting2DReflectorBuffer_)) return false;
    if (!buffers_.valid(lighting2DTileRangeBuffer_)) return false;
    if (!buffers_.valid(lighting2DTileIndexBuffer_)) return false;

    ComputePipelineEntry& compute = computePipelines_.get(lighting2DComputePipeline_);
    ComputePipelineEntry& cullCompute = computePipelines_.get(lighting2DCullPipeline_);
    ComputePipelineEntry& blurCompute = computePipelines_.get(lighting2DBlurPipeline_);
    TextureEntry& lightingTex = textures_.get(lighting2DTexture_);
    TextureEntry& blurTex = textures_.get(lighting2DBlurTexture_);
    BufferEntry& lightBuffer = buffers_.get(lighting2DLightBuffer_);
    BufferEntry& segmentBuffer = buffers_.get(lighting2DSegmentBuffer_);
    BufferEntry& reflectorBuffer = buffers_.get(lighting2DReflectorBuffer_);
    BufferEntry& tileRangeBuffer = buffers_.get(lighting2DTileRangeBuffer_);
    BufferEntry& tileIndexBuffer = buffers_.get(lighting2DTileIndexBuffer_);

    const uint32_t tileCols =
        (static_cast<uint32_t>(cam.viewportW) + kLighting2DTileSize - 1u) / kLighting2DTileSize;
    const uint32_t tileRows =
        (static_cast<uint32_t>(cam.viewportH) + kLighting2DTileSize - 1u) / kLighting2DTileSize;
    const uint32_t tileCount = std::max(1u, tileCols * tileRows);

    struct LightingUniforms {
        float cameraPos[2];
        float viewportSize[2];
        float lightingSize[2];
        uint32_t lightCount;
        uint32_t segmentCount;
        uint32_t reflectorCount;
        uint32_t padCounts;
        uint32_t tileCols;
        uint32_t tileRows;
        uint32_t tileSize;
        uint32_t maxLightsPerTile;
        uint32_t padTile[2];
        float ambientColor[4];
        float zoom;
        float shadowStrength;
        float ambientIntensity;
        float exposure;
        float wetness;
        uint32_t frameIndex;
        float time;
        float pad0;
    } uniforms{
        { cam.x, cam.y },
        { static_cast<float>(cam.viewportW), static_cast<float>(cam.viewportH) },
        { static_cast<float>(lighting2DTextureWidth_), static_cast<float>(lighting2DTextureHeight_) },
        lightCount,
        segmentCount,
        reflectorCount,
        0u,
        tileCols,
        tileRows,
        kLighting2DTileSize,
        kLighting2DMaxLightsPerTile,
        { 0u, 0u },
        { params.ambientR, params.ambientG, params.ambientB, params.ambientA },
        (cam.zoom > 0.f) ? cam.zoom : 1.f,
        0.72f,
        params.ambientIntensity,
        params.exposure,
        params.wetness,
        lighting2DFrameIndex_,
        params.time > 0.f ? params.time : static_cast<float>(SDL_GetTicks()) / 1000.f,
        0.f
    };

    struct LightingCullUniforms {
        float cameraPos[2];
        float viewportSize[2];
        uint32_t lightCount;
        uint32_t tileCols;
        uint32_t tileRows;
        uint32_t tileSize;
        float zoom;
        uint32_t maxLightsPerTile;
        uint32_t pad0[2];
    } cullUniforms{
        { cam.x, cam.y },
        { static_cast<float>(cam.viewportW), static_cast<float>(cam.viewportH) },
        lightCount,
        tileCols,
        tileRows,
        kLighting2DTileSize,
        (cam.zoom > 0.f) ? cam.zoom : 1.f,
        kLighting2DMaxLightsPerTile,
        { 0u, 0u }
    };

    SDL_GPUStorageBufferReadWriteBinding cullRwBuffers[2]{};
    cullRwBuffers[0].buffer = tileRangeBuffer.gpuBuffer;
    cullRwBuffers[0].cycle = false;
    cullRwBuffers[1].buffer = tileIndexBuffer.gpuBuffer;
    cullRwBuffers[1].cycle = false;

    SDL_GPUComputePass* cullPass = SDL_BeginGPUComputePass(gpuCmdBuf_, nullptr, 0,
                                                           cullRwBuffers, 2);
    if (!cullPass) {
        core::logError("runLighting2DComputePass: SDL_BeginGPUComputePass(cull) failed: %s", SDL_GetError());
        return false;
    }

    // One shader invocation owns one tile, so the list for that tile is written
    // sequentially without atomics. This is intentionally easier to debug than
    // a global append buffer and is enough for the 32-light L3 acceptance scene.
    SDL_BindGPUComputePipeline(cullPass, cullCompute.pipeline);
    SDL_GPUBuffer* cullReadonlyBuffers[1] = { lightBuffer.gpuBuffer };
    SDL_BindGPUComputeStorageBuffers(cullPass, 0, cullReadonlyBuffers, 1);
    SDL_PushGPUComputeUniformData(gpuCmdBuf_, 0, &cullUniforms, sizeof(cullUniforms));
    SDL_DispatchGPUCompute(cullPass, (tileCount + 63u) / 64u, 1, 1);
    SDL_EndGPUComputePass(cullPass);
    frameStats_.computeDispatchCount++;

    SDL_GPUStorageTextureReadWriteBinding rwTexture{};
    rwTexture.texture = lightingTex.gpuTex;
    rwTexture.mip_level = 0;
    rwTexture.layer = 0;
    rwTexture.cycle = false;

    SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(gpuCmdBuf_, &rwTexture, 1, nullptr, 0);
    if (!computePass) {
        core::logError("runLighting2DComputePass: SDL_BeginGPUComputePass failed: %s", SDL_GetError());
        return false;
    }

    SDL_BindGPUComputePipeline(computePass, compute.pipeline);
    SDL_GPUBuffer* readonlyBuffers[5] = {
        lightBuffer.gpuBuffer,
        segmentBuffer.gpuBuffer,
        reflectorBuffer.gpuBuffer,
        tileRangeBuffer.gpuBuffer,
        tileIndexBuffer.gpuBuffer
    };
    SDL_BindGPUComputeStorageBuffers(computePass, 0, readonlyBuffers, 5);
    SDL_PushGPUComputeUniformData(gpuCmdBuf_, 0, &uniforms, sizeof(uniforms));

    const uint32_t groupsX = static_cast<uint32_t>((lighting2DTextureWidth_ + 7) / 8);
    const uint32_t groupsY = static_cast<uint32_t>((lighting2DTextureHeight_ + 7) / 8);
    SDL_DispatchGPUCompute(computePass, groupsX, groupsY, 1);
    SDL_EndGPUComputePass(computePass);
    frameStats_.computeDispatchCount++;

    struct LightingBlurUniforms {
        uint32_t lightingSize[2];
        int32_t direction[2];
        float ambientIntensity;
        float exposure;
        uint32_t pad0[2];
    };

    auto dispatchBlur = [&](SDL_GPUTexture* source,
                            SDL_GPUTexture* target,
                            int32_t dirX,
                            int32_t dirY) {
        LightingBlurUniforms blurUniforms{
            { static_cast<uint32_t>(lighting2DTextureWidth_),
              static_cast<uint32_t>(lighting2DTextureHeight_) },
            { dirX, dirY },
            params.ambientIntensity,
            params.exposure,
            { 0u, 0u }
        };

        SDL_GPUStorageTextureReadWriteBinding blurWrite{};
        blurWrite.texture = target;
        blurWrite.mip_level = 0;
        blurWrite.layer = 0;
        blurWrite.cycle = false;

        SDL_GPUComputePass* blurPass =
            SDL_BeginGPUComputePass(gpuCmdBuf_, &blurWrite, 1, nullptr, 0);
        if (!blurPass) {
            core::logError("runLighting2DComputePass: SDL_BeginGPUComputePass(blur) failed: %s",
                           SDL_GetError());
            return false;
        }

        // Blur reads one storage texture and writes a different storage texture.
        // The two dispatches below ping-pong between lightingTex and blurTex,
        // which keeps read/write hazards clear without needing simultaneous
        // storage read-write texture support.
        SDL_BindGPUComputePipeline(blurPass, blurCompute.pipeline);
        SDL_GPUTexture* readonlyTextures[1] = { source };
        SDL_BindGPUComputeStorageTextures(blurPass, 0, readonlyTextures, 1);
        SDL_PushGPUComputeUniformData(gpuCmdBuf_, 0, &blurUniforms, sizeof(blurUniforms));
        SDL_DispatchGPUCompute(blurPass, groupsX, groupsY, 1);
        SDL_EndGPUComputePass(blurPass);
        frameStats_.computeDispatchCount++;
        return true;
    };

    if (!dispatchBlur(lightingTex.gpuTex, blurTex.gpuTex, 1, 0)) return false;
    if (!dispatchBlur(blurTex.gpuTex, lightingTex.gpuTex, 0, 1)) return false;
    lighting2DFrameIndex_++;
    return true;
}

void SDLGPURenderDevice::submitLighting2DPass(const PassSubmitInfo& info,
                                              const Lighting2DParams& params) {
    if (!gpuCmdBuf_ || !swapchainTex_) return;
    if (!params.enabled || params.lights.empty()) return;

    CameraData cam = params.camera;
    if (cam.viewportW == 0) cam.viewportW = static_cast<int>(params.viewportW ? params.viewportW : swapW_);
    if (cam.viewportH == 0) cam.viewportH = static_cast<int>(params.viewportH ? params.viewportH : swapH_);
    if (cam.viewportW <= 0 || cam.viewportH <= 0) return;
    frameStats_.lighting2DSubmitCount++;

    const bool computed = runLighting2DComputePass(cam, params);
    if (!computed) {
        frameStats_.fallbackReason = "lighting compute unavailable";
        submitRadialLightingFallback(cam, params);
        return;
    }

    // Composite: draw the compute-produced dynamic-light/shadow overlay over
    // the world. This is not the final lighting model, but it proves the chain:
    // ECS data -> storage buffers -> compute storage texture -> sampled overlay.
    DrawSpriteCmd overlay{};
    overlay.texture = lighting2DTexture_;
    overlay.x = cam.x;
    overlay.y = cam.y;
    const float zoom = (cam.zoom > 0.f) ? cam.zoom : 1.f;
    const float visibleWorldW = static_cast<float>(cam.viewportW) / zoom;
    const float visibleWorldH = static_cast<float>(cam.viewportH) / zoom;
    overlay.scaleX = visibleWorldW / static_cast<float>(lighting2DTextureWidth_);
    overlay.scaleY = visibleWorldH / static_cast<float>(lighting2DTextureHeight_);
    overlay.pivotX = 0.5f;
    overlay.pivotY = 0.5f;
    overlay.srcRect = core::Rect{
        0.f, 0.f,
        static_cast<float>(lighting2DTextureWidth_),
        static_cast<float>(lighting2DTextureHeight_)
    };
    overlay.layer = 10000;
    overlay.pass = engine::RenderPass::World;
    overlay.tint = core::Color::White;

    RenderCmd overlayCmd = overlay;
    const RenderCmd* overlayPtr = &overlayCmd;
    std::vector<const RenderCmd*> cmds{ overlayPtr };

    renderCmdsToTarget(gpuCmdBuf_, pipeline_, cmds, cam, false, core::Color::Black,
                       swapchainTex_, swapW_, swapH_);
    // Legacy compatibility: the old entry point still overlays the lighting
    // texture on the swapchain, then adds the simple radial fallback. The graph
    // path below deliberately does not call this function, so WorldColor
    // composite is the only swapchain writer there.
    submitRadialLightingFallback(cam, params);
}

void SDLGPURenderDevice::submitWorldLightingGraph(const WorldLightingSubmitInfo& info) {
    if (!gpuCmdBuf_ || !swapchainTex_) return;

    CameraData cam = info.worldPass.camera;
    if (cam.viewportW == 0) cam.viewportW = static_cast<int>(swapW_);
    if (cam.viewportH == 0) cam.viewportH = static_cast<int>(swapH_);
    if (cam.viewportW <= 0 || cam.viewportH <= 0) return;

    // The graph object is currently a declaration/debug record. Execution is
    // still explicit below so the first migration stays easy to review: every
    // pass in the graph maps to one obvious block of backend work.
    RenderGraph graph;
    const uint32_t swapchain = graph.addResource({
        RenderGraphResourceType::Swapchain, "Swapchain", swapW_, swapH_
    });
    const uint32_t worldColor = graph.addResource({
        RenderGraphResourceType::Texture, "WorldColor",
        static_cast<uint32_t>(cam.viewportW), static_cast<uint32_t>(cam.viewportH)
    });
    const uint32_t lighting = graph.addResource({
        RenderGraphResourceType::Texture, "LightingTexture",
        static_cast<uint32_t>((cam.viewportW + 1) / 2),
        static_cast<uint32_t>((cam.viewportH + 1) / 2)
    });
    graph.addPass({"WorldColorPass", {}, {{worldColor, RenderGraphAccess::WriteColor}}});
    graph.addPass({"LightVisibilityCompute",
                   {{worldColor, RenderGraphAccess::ReadSampled}},
                   {{lighting, RenderGraphAccess::WriteStorage}}});
    graph.addPass({"LightingCompositePass",
                   {{worldColor, RenderGraphAccess::ReadSampled},
                    {lighting, RenderGraphAccess::ReadSampled}},
                   {{swapchain, RenderGraphAccess::WriteColor}}});
    if (!info.uiCommands.empty()) {
        graph.addPass({"UIPass",
                       {{swapchain, RenderGraphAccess::ReadSampled}},
                       {{swapchain, RenderGraphAccess::WriteColor}}});
    }
    frameStats_.renderGraphPassCount += static_cast<uint32_t>(graph.passes().size());

    // Pass 1: render the world into an offscreen color target. This is the key
    // behavioral change from the old overlay path: World no longer writes the
    // swapchain before lighting has had a chance to composite. The pass accepts
    // both CPU-batch commands and GPU-driven sprite batches so RenderSystem does
    // not need to abandon the GPU sprite path just because lighting is enabled.
    if (!textures_.valid(worldColorTarget_) ||
        worldColorTargetWidth_ != cam.viewportW ||
        worldColorTargetHeight_ != cam.viewportH) {
        if (textures_.valid(worldColorTarget_)) {
            destroyTexture(worldColorTarget_);
        }
        worldColorTarget_ = createRenderTargetTexture(cam.viewportW, cam.viewportH);
        worldColorTargetWidth_ = cam.viewportW;
        worldColorTargetHeight_ = cam.viewportH;
    }
    TextureEntry* worldEntry = textures_.tryGet(worldColorTarget_);
    if (!worldEntry || !worldEntry->gpuTex) {
        // If the offscreen target cannot be created, fall back to the old direct
        // path instead of dropping the frame. The stats make this visible.
        frameStats_.fallbackReason = "world color target unavailable";
        submitPass(info.worldPass, info.worldCommands);
        submitRadialLightingFallback(cam, info.lighting);
        return;
    }

    bool worldWasCleared = false;
    if (info.hasGPUWorld) {
        GPURenderParams gpuWorld = info.gpuWorld;
        gpuWorld.camera = cam;
        gpuWorld.clearEnabled = info.worldPass.clearEnabled;
        gpuWorld.clearColor = info.worldPass.clearColor;
        submitGPUDrivenPassToTarget(info.worldPass, gpuWorld,
                                    worldEntry->gpuTex,
                                    static_cast<uint32_t>(cam.viewportW),
                                    static_cast<uint32_t>(cam.viewportH),
                                    gpuDrivenOffscreenPipeline_);
        worldWasCleared = true;
    }

    renderCmdsToTarget(gpuCmdBuf_, offscreenPipeline_, info.worldCommands, cam,
                       !worldWasCleared && info.worldPass.clearEnabled,
                       info.worldPass.clearColor,
                       worldEntry->gpuTex,
                       static_cast<uint32_t>(cam.viewportW),
                       static_cast<uint32_t>(cam.viewportH));

    for (GPUParticleParams particle : info.particles) {
        particle.camera = cam;
        particle.clearEnabled = false;
        submitGPUParticlePassToTarget(info.worldPass, particle,
                                      worldEntry->gpuTex,
                                      static_cast<uint32_t>(cam.viewportW),
                                      static_cast<uint32_t>(cam.viewportH),
                                      particleOffscreenPipeline_);
    }
    frameStats_.worldColorPassCount++;

    // Pass 2: produce LightingTexture only. This is the important split from
    // the legacy submitLighting2DPass() path: graph execution must not touch the
    // swapchain until LightingCompositePass, otherwise camera/frame graph work
    // would inherit the old overlay side effect.
    frameStats_.lighting2DSubmitCount++;
    if (!runLighting2DComputePass(cam, info.lighting)) {
        frameStats_.fallbackReason = "lighting compute unavailable";
        if (info.hasGPUWorld) {
            GPURenderParams gpuWorld = info.gpuWorld;
            gpuWorld.camera = cam;
            submitGPUDrivenPass(info.worldPass, gpuWorld);
        } else {
            submitPass(info.worldPass, info.worldCommands);
        }
        if (info.hasGPUWorld && !info.worldCommands.empty()) {
            renderCmdsToTarget(gpuCmdBuf_, pipeline_, info.worldCommands, cam,
                               false, core::Color::Black,
                               swapchainTex_, swapW_, swapH_);
        }
        for (const GPUParticleParams& particleParams : info.particles) {
            GPUParticleParams particle = particleParams;
            particle.camera = cam;
            particle.clearEnabled = false;
            submitGPUParticlePass(info.worldPass, particle);
        }
        submitRadialLightingFallback(cam, info.lighting);
        if (!info.uiCommands.empty()) {
            renderCmdsToTarget(gpuCmdBuf_, pipeline_, info.uiCommands, cam,
                               false, core::Color::Black,
                               swapchainTex_, swapW_, swapH_);
            frameStats_.uiPassCount++;
        }
        return;
    }
    if (!lightingCompositePipeline_ ||
        !textures_.valid(lighting2DTexture_) ||
        !textures_.valid(worldColorTarget_)) {
        // Composite is the new main path. If it is unavailable, keep a visible
        // frame using the old copy/overlay behavior and expose the reason.
        frameStats_.fallbackReason = "lighting composite unavailable";
        return;
    }

    TextureEntry& lightingEntry = textures_.get(lighting2DTexture_);
    worldEntry = textures_.tryGet(worldColorTarget_);
    if (!worldEntry) return;

    // Pass 3: draw one world-aligned quad to the swapchain. The vertex shader
    // uses the same camera matrix as sprites, so the quad exactly covers the
    // current camera's visible world rectangle.
    const float zoom = (cam.zoom > 0.f) ? cam.zoom : 1.f;
    const float visibleW = static_cast<float>(cam.viewportW) / zoom;
    const float visibleH = static_cast<float>(cam.viewportH) / zoom;
    const float hw = visibleW * 0.5f;
    const float hh = visibleH * 0.5f;

    SpriteVertex verts[4]{
        {cam.x - hw, cam.y - hh, 0.f, 0.f, 255, 255, 255, 255},
        {cam.x + hw, cam.y - hh, 1.f, 0.f, 255, 255, 255, 255},
        {cam.x + hw, cam.y + hh, 1.f, 1.f, 255, 255, 255, 255},
        {cam.x - hw, cam.y + hh, 0.f, 1.f, 255, 255, 255, 255},
    };
    uint16_t indices[6]{0, 1, 2, 0, 2, 3};

    uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, transferBuf_, true));
    std::memcpy(mapped, verts, sizeof(verts));
    std::memcpy(mapped + sizeof(verts), indices, sizeof(indices));
    SDL_UnmapGPUTransferBuffer(device_, transferBuf_);
    frameStats_.uploadBytes += sizeof(verts) + sizeof(indices);
    frameStats_.uploadCallCount++;

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(gpuCmdBuf_);
    SDL_GPUTransferBufferLocation vSrc{ transferBuf_, 0 };
    SDL_GPUBufferRegion vDst{ vertexBuf_, 0, sizeof(verts) };
    SDL_UploadToGPUBuffer(copyPass, &vSrc, &vDst, true);
    SDL_GPUTransferBufferLocation iSrc{ transferBuf_, static_cast<uint32_t>(sizeof(verts)) };
    SDL_GPUBufferRegion iDst{ indexBuf_, 0, sizeof(indices) };
    SDL_UploadToGPUBuffer(copyPass, &iSrc, &iDst, true);
    SDL_EndGPUCopyPass(copyPass);

    float proj[16], view[16], mvp[16];
    buildOrthoProjectionMatrix(static_cast<float>(swapW_), static_cast<float>(swapH_), proj);
    buildViewMatrix(cam.x, cam.y, zoom, cam.rotation, view);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            mvp[i * 4 + j] = 0.f;
            for (int k = 0; k < 4; ++k) {
                mvp[i * 4 + j] += view[i * 4 + k] * proj[k * 4 + j];
            }
        }
    }

    SDL_GPUColorTargetInfo colorTarget{};
    colorTarget.texture = swapchainTex_;
    colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTarget.store_op = SDL_GPU_STOREOP_STORE;
    colorTarget.clear_color = SDL_FColor{0.f, 0.f, 0.f, 1.f};

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(gpuCmdBuf_, &colorTarget, 1, nullptr);
    if (!pass) {
        core::logError("submitWorldLightingGraph: composite render pass failed: %s", SDL_GetError());
        return;
    }

    SDL_BindGPUGraphicsPipeline(pass, lightingCompositePipeline_);
    SDL_PushGPUVertexUniformData(gpuCmdBuf_, 0, mvp, sizeof(mvp));

    SDL_GPUTextureSamplerBinding samplers[2]{
        {worldEntry->gpuTex, worldEntry->sampler},
        {lightingEntry.gpuTex, lightingEntry.sampler},
    };
    SDL_BindGPUFragmentSamplers(pass, 0, samplers, 2);
    frameStats_.textureBindCount += 2;

    struct CompositeUniforms {
        float ambientColor[4];
        float ambientIntensity;
        float exposure;
        uint32_t debugMode;
        float pad0;
    } uniforms{
        {info.lighting.ambientR, info.lighting.ambientG,
         info.lighting.ambientB, info.lighting.ambientA},
        info.lighting.ambientIntensity,
        info.lighting.exposure,
        0u,
        0.f
    };
    SDL_PushGPUFragmentUniformData(gpuCmdBuf_, 0, &uniforms, sizeof(uniforms));

    SDL_GPUBufferBinding vertexBinding{ vertexBuf_, 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
    SDL_GPUBufferBinding indexBinding{ indexBuf_, 0 };
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
    SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, 0, 0);
    SDL_EndGPURenderPass(pass);

    frameStats_.drawCallCount++;
    frameStats_.lightingCompositeCount++;

    // Pass 4: UI/Text/Screen commands are explicitly part of the graph now.
    // They still reuse the normal CPU batch renderer because UI correctness is
    // more important than throughput here, but their ordering is no longer an
    // implicit RenderSystem side-effect: they are declared as UIPass and always
    // run after world lighting composite.
    if (!info.uiCommands.empty()) {
        renderCmdsToTarget(gpuCmdBuf_, pipeline_, info.uiCommands, cam,
                           false, core::Color::Black,
                           swapchainTex_, swapW_, swapH_);
        frameStats_.uiPassCount++;
    }
}

void SDLGPURenderDevice::submitGPUParticlePass(const PassSubmitInfo& info,
                                               const GPUParticleParams& params) {
    submitGPUParticlePassToTarget(info, params, swapchainTex_, swapW_, swapH_, particlePipeline_);
}

void SDLGPURenderDevice::submitGPUParticlePassToTarget(const PassSubmitInfo& info,
                                                       const GPUParticleParams& params,
                                                       SDL_GPUTexture* target,
                                                       uint32_t targetWidth,
                                                       uint32_t targetHeight,
                                                       SDL_GPUGraphicsPipeline* pipeline) {
    if (!gpuCmdBuf_ || !target) return;
    if (params.particleCount == 0) return;
    if (!pipeline || !gpuDrivenQuadIndexBuf_) return;
    if (!buffers_.valid(params.particleBuffer)) return;
    if (!buffers_.valid(params.aliveIndexBuffer)) return;
    if (!buffers_.valid(params.indirectArgsBuffer)) return;
    if (!textures_.valid(params.texture)) return;
    if (!computePipelines_.valid(params.updatePipeline)) return;
    if (!computePipelines_.valid(params.sortPipeline)) return;

    BufferEntry& particleBuf = buffers_.get(params.particleBuffer);
    BufferEntry& aliveBuf = buffers_.get(params.aliveIndexBuffer);
    BufferEntry& indirectBuf = buffers_.get(params.indirectArgsBuffer);
    ComputePipelineEntry& update = computePipelines_.get(params.updatePipeline);
    ComputePipelineEntry& sort = computePipelines_.get(params.sortPipeline);

    // Reset the indirect draw command before GPU compaction. The update
    // compute owns num_instances after this point; CPU never reads it back.
    // Layout = SDL_GPUIndexedIndirectDrawCommand.
    const uint32_t indirectInit[5] = { 6u, 0u, 0u, 0u, 0u };
    uploadToBuffer(params.indirectArgsBuffer, indirectInit, sizeof(indirectInit), 0);

    // Phase 1: GPU simulation. The compute shader only receives dt/range,
    // then updates age and position in-place inside the particle storage buffer.
    struct UpdateUniforms {
        float dt;
        uint32_t firstParticle;
        uint32_t particleCount;
        uint32_t pad0;
    } updateUniforms{
        params.dt,
        params.firstParticle,
        params.particleCount,
        0u
    };

    SDL_GPUStorageBufferReadWriteBinding rwBindings[3]{};
    rwBindings[0].buffer = particleBuf.gpuBuffer;
    rwBindings[0].cycle = false;
    rwBindings[1].buffer = aliveBuf.gpuBuffer;
    rwBindings[1].cycle = false;
    rwBindings[2].buffer = indirectBuf.gpuBuffer;
    rwBindings[2].cycle = false;
    SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(gpuCmdBuf_, nullptr, 0, rwBindings, 3);
    if (computePass) {
        SDL_BindGPUComputePipeline(computePass, update.pipeline);
        SDL_PushGPUComputeUniformData(gpuCmdBuf_, 0, &updateUniforms, sizeof(updateUniforms));
        const uint32_t groups = (params.particleCount + 63u) / 64u;
        SDL_DispatchGPUCompute(computePass, groups, 1, 1);
        SDL_EndGPUComputePass(computePass);
        frameStats_.computeDispatchCount++;
    }

    // Phase 1.5: GPU sort. The alive list was compacted by update compute,
    // and DrawArgs[1] carries the current alive count. Odd-even sort needs
    // particleCount phases to be correct for any alive count without readback.
    const uint32_t sortGroups = ((params.particleCount + 1u) / 2u + 127u) / 128u;
    for (uint32_t phase = 0; phase < params.particleCount; ++phase) {
        struct SortUniforms {
            uint32_t phase;
            uint32_t maxParticleCount;
            uint32_t pad0;
            uint32_t pad1;
        } sortUniforms{ phase, params.particleCount, 0u, 0u };

        SDL_GPUStorageBufferReadWriteBinding sortRw{};
        sortRw.buffer = aliveBuf.gpuBuffer;
        sortRw.cycle = false;
        SDL_GPUComputePass* sortPass = SDL_BeginGPUComputePass(gpuCmdBuf_, nullptr, 0, &sortRw, 1);
        if (!sortPass) break;

        SDL_BindGPUComputePipeline(sortPass, sort.pipeline);
        SDL_GPUBuffer* readonlyBuffers[2] = { particleBuf.gpuBuffer, indirectBuf.gpuBuffer };
        SDL_BindGPUComputeStorageBuffers(sortPass, 0, readonlyBuffers, 2);
        SDL_PushGPUComputeUniformData(gpuCmdBuf_, 0, &sortUniforms, sizeof(sortUniforms));
        SDL_DispatchGPUCompute(sortPass, sortGroups, 1, 1);
        SDL_EndGPUComputePass(sortPass);
        frameStats_.computeDispatchCount++;
    }

    CameraData cam = params.camera;
    if (cam.viewportW == 0) cam = info.camera;
    if (cam.viewportW == 0) cam.viewportW = static_cast<int>(targetWidth);
    if (cam.viewportH == 0) cam.viewportH = static_cast<int>(targetHeight);

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

    // Phase 2: indirect draw. The vertex shader indexes through AliveIndices,
    // and the indirect command's instance count is produced by compute.
    SDL_GPUColorTargetInfo colorTarget{};
    colorTarget.texture     = target;
    colorTarget.load_op     = params.clearEnabled ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    colorTarget.store_op    = SDL_GPU_STOREOP_STORE;
    colorTarget.clear_color = SDL_FColor{
        params.clearColor.r / 255.f, params.clearColor.g / 255.f,
        params.clearColor.b / 255.f, params.clearColor.a / 255.f
    };

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(gpuCmdBuf_, &colorTarget, 1, nullptr);
    if (!pass) {
        core::logError("submitGPUParticlePass: SDL_BeginGPURenderPass failed: %s", SDL_GetError());
        return;
    }

    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_PushGPUVertexUniformData(gpuCmdBuf_, 0, viewProj, sizeof(viewProj));

    SDL_GPUBuffer* vsStorage[2] = { particleBuf.gpuBuffer, aliveBuf.gpuBuffer };
    SDL_BindGPUVertexStorageBuffers(pass, 0, vsStorage, 2);

    SDL_GPUBufferBinding idxBinding{ gpuDrivenQuadIndexBuf_, 0 };
    SDL_BindGPUIndexBuffer(pass, &idxBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

    const TextureEntry& tex = textures_.get(params.texture);
    SDL_GPUTextureSamplerBinding tb{ tex.gpuTex, tex.sampler };
    SDL_BindGPUFragmentSamplers(pass, 0, &tb, 1);
    frameStats_.textureBindCount++;

    SDL_DrawGPUIndexedPrimitivesIndirect(pass, indirectBuf.gpuBuffer, 0, 1);
    frameStats_.gpuDrawBatchCount++;
    frameStats_.drawCallCount++;

    SDL_EndGPURenderPass(pass);
}

} // namespace backend
