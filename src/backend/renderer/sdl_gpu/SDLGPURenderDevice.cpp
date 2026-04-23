#include "SDLGPURenderDevice.h"
#include "../CommandBuffer.h"
#include "../../../core/Logger.h"
#include "../../../core/Assert.h"
#include <SDL3/SDL.h>
#include <cstring>
#include <algorithm>
#include <variant>

// 编译期生成的 SPIRV 头文件（由 CMake shader 编译步骤生成）
#include "sprite_vert_spv.h"
#include "sprite_frag_spv.h"

namespace backend {

// ── 构造 / 析构 ───────────────────────────────────────────────────────────────

SDLGPURenderDevice::SDLGPURenderDevice(SDL_Window* window)
    : window_(window) {
    batchVerts_.reserve(MAX_SPRITES_PER_BATCH * 4);
    batchIdx_.reserve(MAX_SPRITES_PER_BATCH * 6);
}

SDLGPURenderDevice::~SDLGPURenderDevice() {
    shutdown();
}

// ── IBackendSystem ────────────────────────────────────────────────────────────

void SDLGPURenderDevice::init() {
    // 按平台偏好顺序请求 shader format
    SDL_GPUShaderFormat fmts =
        SDL_GPU_SHADERFORMAT_SPIRV |
        SDL_GPU_SHADERFORMAT_DXIL  |
        SDL_GPU_SHADERFORMAT_MSL;

    device_ = SDL_CreateGPUDevice(fmts, /*debug=*/false, nullptr);
    if (!device_) {
        core::logError("SDL_CreateGPUDevice failed: %s — rendering disabled", SDL_GetError());
        return;
    }

    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        core::logError("SDL_ClaimWindowForGPUDevice failed: %s — rendering disabled", SDL_GetError());
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        return;
    }

    SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(device_);
    const char* backend = SDL_GetGPUDeviceDriver(device_);
    core::logInfo("GPU backend: %s  shader formats: 0x%x", backend, (int)supported);
    if (!(supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
        core::logError("GPU device does not support SPIRV. On Windows ensure Vulkan drivers are installed.");
        SDL_ReleaseWindowFromGPUDevice(device_, window_);
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        return;
    }

    // 分配顶点/索引缓冲（静态大小，每帧映射写入）
    SDL_GPUBufferCreateInfo vbInfo{};
    vbInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vbInfo.size  = MAX_SPRITES_PER_BATCH * 4 * sizeof(SpriteVertex);
    vertexBuf_ = SDL_CreateGPUBuffer(device_, &vbInfo);

    SDL_GPUBufferCreateInfo ibInfo{};
    ibInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ibInfo.size  = MAX_SPRITES_PER_BATCH * 6 * sizeof(uint16_t);
    indexBuf_ = SDL_CreateGPUBuffer(device_, &ibInfo);

    // Transfer buffer（CPU→GPU 上传通道，复用）
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = vbInfo.size + ibInfo.size;
    transferBuf_ = SDL_CreateGPUTransferBuffer(device_, &tbInfo);

    ASSERT(vertexBuf_ && indexBuf_ && transferBuf_);

    createPipeline();
    core::logInfo("SDLGPURenderDevice initialized");
}

void SDLGPURenderDevice::beginFrame() {
    gpuCmdBuf_ = SDL_AcquireGPUCommandBuffer(device_);
    ASSERT_MSG(gpuCmdBuf_, "SDL_AcquireGPUCommandBuffer failed");

    // 获取本帧 swapchain 纹理
    SDL_GPUTexture* tex = nullptr;
    bool ok = SDL_WaitAndAcquireGPUSwapchainTexture(
        gpuCmdBuf_, window_, &tex, &swapW_, &swapH_);
    if (!ok || !tex) {
        // 窗口最小化 / 未就绪，跳过本帧
        SDL_CancelGPUCommandBuffer(gpuCmdBuf_);
        gpuCmdBuf_    = nullptr;
        swapchainTex_ = nullptr;
        return;
    }
    swapchainTex_ = tex;
}

void SDLGPURenderDevice::endFrame() {
    // submitCommandBuffer 里已经处理，这里不需要额外操作
}

void SDLGPURenderDevice::shutdown() {
    if (!device_) return;
    SDL_WaitForGPUIdle(device_);

    if (pipeline_)    { SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_); pipeline_ = nullptr; }
    if (vertexBuf_)   { SDL_ReleaseGPUBuffer(device_, vertexBuf_);  vertexBuf_ = nullptr; }
    if (indexBuf_)    { SDL_ReleaseGPUBuffer(device_, indexBuf_);   indexBuf_ = nullptr; }
    if (transferBuf_) { SDL_ReleaseGPUTransferBuffer(device_, transferBuf_); transferBuf_ = nullptr; }

    // 释放所有纹理（遍历 HandleMap 不直接支持，用 vector 追踪）
    // TODO: 添加纹理追踪列表（Month 7 资源管理时统一处理）

    SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;
    core::logInfo("SDLGPURenderDevice shutdown");
}

// ── IRenderDevice: 资源管理 ───────────────────────────────────────────────────

TextureHandle SDLGPURenderDevice::createTexture(const TextureDesc& desc) {
    ASSERT(desc.data && desc.width > 0 && desc.height > 0);

    // 创建 GPU 纹理
    SDL_GPUTextureCreateInfo info{};
    info.type             = SDL_GPU_TEXTURETYPE_2D;
    info.format           = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage            = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width            = static_cast<uint32_t>(desc.width);
    info.height           = static_cast<uint32_t>(desc.height);
    info.layer_count_or_depth = 1;
    info.num_levels       = 1;
    SDL_GPUTexture* gpuTex = SDL_CreateGPUTexture(device_, &info);
    ASSERT_MSG(gpuTex, "SDL_CreateGPUTexture failed");

    // 上传像素数据
    size_t dataSize = (size_t)desc.width * desc.height * 4;
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size  = static_cast<uint32_t>(dataSize);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tbInfo);

    void* mapped = SDL_MapGPUTransferBuffer(device_, tb, false);
    memcpy(mapped, desc.data, dataSize);
    SDL_UnmapGPUTransferBuffer(device_, tb);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    src.offset          = 0;

    SDL_GPUTextureRegion dst{};
    dst.texture = gpuTex;
    dst.w       = info.width;
    dst.h       = info.height;
    dst.d       = 1;

    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, tb);

    // 创建 sampler
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter    = SDL_GPU_FILTER_NEAREST;
    si.mag_filter    = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode   = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device_, &si);

    TextureEntry entry{ gpuTex, sampler, desc.width, desc.height };
    return textures_.insert(entry);
}

void SDLGPURenderDevice::destroyTexture(TextureHandle h) {
    if (!textures_.valid(h)) return;
    TextureEntry& e = textures_.get(h);
    SDL_WaitForGPUIdle(device_);
    if (e.sampler) SDL_ReleaseGPUSampler(device_, e.sampler);
    if (e.gpuTex)  SDL_ReleaseGPUTexture(device_, e.gpuTex);
    textures_.remove(h);
}

ShaderHandle SDLGPURenderDevice::createShader(const ShaderDesc&) {
    // Month 3 暂不暴露自定义 shader，使用内建 sprite pipeline
    return {};
}
void SDLGPURenderDevice::destroyShader(ShaderHandle) {}

// ── IRenderDevice: 渲染 ───────────────────────────────────────────────────────

void SDLGPURenderDevice::submitCommandBuffer(const CommandBuffer& cb) {
    if (!gpuCmdBuf_ || !swapchainTex_) return;

    // 收集并按 layer 排序 DrawSpriteCmd
    std::vector<DrawSpriteCmd> sprites;
    std::vector<DrawTileCmd>   tiles;
    core::Color clearColor = core::Color::Black;

    for (const auto& cmd : cb.commands()) {
        std::visit([&](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, DrawSpriteCmd>) sprites.push_back(c);
            else if constexpr (std::is_same_v<T, DrawTileCmd>)   tiles.push_back(c);
            else if constexpr (std::is_same_v<T, ClearCmd>)      clearColor = c.color;
        }, cmd);
    }

    std::stable_sort(sprites.begin(), sprites.end(),
        [](const DrawSpriteCmd& a, const DrawSpriteCmd& b){ return a.layer < b.layer; });

    // 构建正交投影矩阵
    float proj[16];
    buildOrthoMatrix(static_cast<float>(swapW_), static_cast<float>(swapH_), proj);

    // 开始 render pass
    SDL_GPUColorTargetInfo ct{};
    ct.texture            = swapchainTex_;
    ct.load_op            = SDL_GPU_LOADOP_CLEAR;
    ct.store_op           = SDL_GPU_STOREOP_STORE;
    ct.clear_color        = { clearColor.r / 255.f, clearColor.g / 255.f,
                               clearColor.b / 255.f, clearColor.a / 255.f };

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(gpuCmdBuf_, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);

    // 上传 projection matrix（uniform buffer slot 0, vertex stage）
    SDL_PushGPUVertexUniformData(gpuCmdBuf_, 0, proj, sizeof(proj));

    drawSpriteBatch(pass, sprites);
    drawTileBatch(pass, tiles);

    SDL_EndGPURenderPass(pass);
}

void SDLGPURenderDevice::present() {
    if (!gpuCmdBuf_) return;
    SDL_SubmitGPUCommandBuffer(gpuCmdBuf_);
    gpuCmdBuf_    = nullptr;
    swapchainTex_ = nullptr;
}

TextureHandle SDLGPURenderDevice::renderToTexture(const CommandBuffer&, int, int) {
    // Month 6 EditorAPI 实现
    return {};
}

// ── 内部实现 ──────────────────────────────────────────────────────────────────

SDL_GPUShader* SDLGPURenderDevice::loadSPIRV(const uint8_t* code, size_t size,
                                              SDL_GPUShaderStage stage,
                                              int numSamplers, int numUBOs) {
    SDL_GPUShaderCreateInfo info{};
    info.code                  = code;
    info.code_size             = size;
    info.entrypoint            = "main";
    info.format                = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage                 = stage;
    info.num_samplers          = static_cast<uint32_t>(numSamplers);
    info.num_uniform_buffers   = static_cast<uint32_t>(numUBOs);
    return SDL_CreateGPUShader(device_, &info);
}

void SDLGPURenderDevice::createPipeline() {
    if (!device_) return;

    SDL_GPUShader* vs = loadSPIRV(sprite_vert_spv, sprite_vert_spv_size,
                                   SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader* fs = loadSPIRV(sprite_frag_spv, sprite_frag_spv_size,
                                   SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    ASSERT(vs && fs);

    // 顶点布局：pos(float2) + uv(float2) + color(ubyte4)
    SDL_GPUVertexBufferDescription vbDesc{};
    vbDesc.slot             = 0;
    vbDesc.pitch            = sizeof(SpriteVertex);
    vbDesc.input_rate       = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbDesc.instance_step_rate = 0;

    SDL_GPUVertexAttribute attrs[3]{};
    attrs[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,  offsetof(SpriteVertex, x) };
    attrs[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,  offsetof(SpriteVertex, u) };
    attrs[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(SpriteVertex, r) };

    // Alpha blending
    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
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
    pipeInfo.vertex_input_state.vertex_buffer_descriptions = &vbDesc;
    pipeInfo.vertex_input_state.num_vertex_buffers         = 1;
    pipeInfo.vertex_input_state.vertex_attributes          = attrs;
    pipeInfo.vertex_input_state.num_vertex_attributes      = 3;
    pipeInfo.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipeInfo.target_info.color_target_descriptions = &colorTarget;
    pipeInfo.target_info.num_color_targets         = 1;

    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeInfo);
    ASSERT_MSG(pipeline_, "SDL_CreateGPUGraphicsPipeline failed");

    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    core::logInfo("Sprite pipeline created");
}

void SDLGPURenderDevice::drawSpriteBatch(SDL_GPURenderPass* pass,
                                          const std::vector<DrawSpriteCmd>& cmds) {
    if (cmds.empty()) return;

    batchVerts_.clear();
    batchIdx_.clear();

    auto flush = [&]() {
        if (batchVerts_.empty()) return;
        flushBatch(pass);
        batchVerts_.clear();
        batchIdx_.clear();
    };

    TextureHandle curTex{};

    for (const auto& cmd : cmds) {
        if (cmd.texture != curTex || batchVerts_.size() >= MAX_SPRITES_PER_BATCH * 4) {
            flush();
            curTex = cmd.texture;

            // 绑定纹理
            if (textures_.valid(curTex)) {
                TextureEntry& e = textures_.get(curTex);
                SDL_GPUTextureSamplerBinding tsb{ e.gpuTex, e.sampler };
                SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
            }
        }

        // 计算 sprite 四顶点（支持旋转/缩放）
        float hw = cmd.srcRect.w * cmd.scaleX * 0.5f;
        float hh = cmd.srcRect.h * cmd.scaleY * 0.5f;
        float cos_r = cosf(cmd.rotation);
        float sin_r = sinf(cmd.rotation);

        // 本地坐标（以锚点为原点）
        float lx[4] = {-hw,  hw,  hw, -hw};
        float ly[4] = {-hh, -hh,  hh,  hh};

        // UV（归一化）
        TextureEntry* te = textures_.tryGet(curTex);
        float tw = te ? te->width  : 1.f;
        float th = te ? te->height : 1.f;
        float u0 = cmd.srcRect.x / tw;
        float v0 = cmd.srcRect.y / th;
        float u1 = (cmd.srcRect.x + cmd.srcRect.w) / tw;
        float v1 = (cmd.srcRect.y + cmd.srcRect.h) / th;
        float us[4] = {u0, u1, u1, u0};
        float vs_[4]= {v0, v0, v1, v1};

        uint16_t base = static_cast<uint16_t>(batchVerts_.size());
        for (int i = 0; i < 4; ++i) {
            batchVerts_.push_back({
                cmd.x + lx[i] * cos_r - ly[i] * sin_r,
                cmd.y + lx[i] * sin_r + ly[i] * cos_r,
                us[i], vs_[i],
                cmd.tint.r, cmd.tint.g, cmd.tint.b, cmd.tint.a
            });
        }
        // 两个三角形：0-1-2, 0-2-3
        batchIdx_.insert(batchIdx_.end(),
            {base, (uint16_t)(base+1), (uint16_t)(base+2),
             base, (uint16_t)(base+2), (uint16_t)(base+3)});
    }
    flush();
}

void SDLGPURenderDevice::drawTileBatch(SDL_GPURenderPass* pass,
                                        const std::vector<DrawTileCmd>& cmds) {
    if (cmds.empty()) return;

    batchVerts_.clear();
    batchIdx_.clear();

    TextureHandle curTex{};

    auto flush = [&]() {
        if (batchVerts_.empty()) return;
        flushBatch(pass);
        batchVerts_.clear();
        batchIdx_.clear();
    };

    for (const auto& cmd : cmds) {
        if (cmd.tileset != curTex || batchVerts_.size() >= MAX_SPRITES_PER_BATCH * 4) {
            flush();
            curTex = cmd.tileset;
            if (textures_.valid(curTex)) {
                TextureEntry& e = textures_.get(curTex);
                SDL_GPUTextureSamplerBinding tsb{ e.gpuTex, e.sampler };
                SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
            }
        }

        TextureEntry* te = textures_.tryGet(curTex);
        float tw = te ? te->width  : 1.f;
        float th = te ? te->height : 1.f;
        int tileSize = cmd.tileSize > 0 ? cmd.tileSize : 16;
        // 假设 tileset 是等宽格子，每行 tilesetCols 列（DrawTileCmd 暂无此字段，用 tw/tileSize 推算）
        int tsetCols = static_cast<int>(tw) / tileSize;
        if (tsetCols < 1) tsetCols = 1;

        int col = cmd.tileId % tsetCols;
        int row = cmd.tileId / tsetCols;
        float u0 = (col * tileSize) / tw;
        float v0 = (row * tileSize) / th;
        float u1 = u0 + tileSize / tw;
        float v1 = v0 + tileSize / th;

        float px = static_cast<float>(cmd.gridX * tileSize);
        float py = static_cast<float>(cmd.gridY * tileSize);
        float px1 = px + tileSize;
        float py1 = py + tileSize;

        uint16_t base = static_cast<uint16_t>(batchVerts_.size());
        batchVerts_.push_back({ px,  py,  u0, v0, 255,255,255,255 });
        batchVerts_.push_back({ px1, py,  u1, v0, 255,255,255,255 });
        batchVerts_.push_back({ px1, py1, u1, v1, 255,255,255,255 });
        batchVerts_.push_back({ px,  py1, u0, v1, 255,255,255,255 });
        batchIdx_.insert(batchIdx_.end(),
            {base, (uint16_t)(base+1), (uint16_t)(base+2),
             base, (uint16_t)(base+2), (uint16_t)(base+3)});
    }
    flush();
}

void SDLGPURenderDevice::flushBatch(SDL_GPURenderPass* pass) {
    if (batchVerts_.empty()) return;

    size_t vSize = batchVerts_.size() * sizeof(SpriteVertex);
    size_t iSize = batchIdx_.size()   * sizeof(uint16_t);

    // 写入 transfer buffer
    uint8_t* mapped = static_cast<uint8_t*>(
        SDL_MapGPUTransferBuffer(device_, transferBuf_, true));
    memcpy(mapped,         batchVerts_.data(), vSize);
    memcpy(mapped + vSize, batchIdx_.data(),   iSize);
    SDL_UnmapGPUTransferBuffer(device_, transferBuf_);

    // Copy pass: transfer → vertex/index buffer
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(gpuCmdBuf_);

    SDL_GPUTransferBufferLocation vSrc{ transferBuf_, 0 };
    SDL_GPUBufferRegion vDst{ vertexBuf_, 0, static_cast<uint32_t>(vSize) };
    SDL_UploadToGPUBuffer(cp, &vSrc, &vDst, true);

    SDL_GPUTransferBufferLocation iSrc{ transferBuf_, static_cast<uint32_t>(vSize) };
    SDL_GPUBufferRegion iDst{ indexBuf_, 0, static_cast<uint32_t>(iSize) };
    SDL_UploadToGPUBuffer(cp, &iSrc, &iDst, true);

    SDL_EndGPUCopyPass(cp);

    // Draw
    SDL_GPUBufferBinding vBind{ vertexBuf_, 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &vBind, 1);
    SDL_GPUBufferBinding iBind{ indexBuf_, 0 };
    SDL_BindGPUIndexBuffer(pass, &iBind, SDL_GPU_INDEXELEMENTSIZE_16BIT);

    SDL_DrawGPUIndexedPrimitives(pass,
        static_cast<uint32_t>(batchIdx_.size()), 1, 0, 0, 0);
}

void SDLGPURenderDevice::buildOrthoMatrix(float w, float h, float out[16]) {
    // 列主序正交矩阵：(0,0) 左上，(w,h) 右下，NDC Z [0,1]
    memset(out, 0, 16 * sizeof(float));
    out[0]  =  2.f / w;
    out[5]  = -2.f / h;
    out[10] =  1.f;
    out[12] = -1.f;
    out[13] =  1.f;
    out[15] =  1.f;
}

} // namespace backend
