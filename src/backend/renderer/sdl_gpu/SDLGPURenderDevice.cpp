#include "SDLGPURenderDevice.h"

#include <algorithm>
#include <cstring>
#include <variant>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>

#include "../CommandBuffer.h"
#include "../../../core/Assert.h"
#include "../../../core/Logger.h"

#include "sprite_vert_spv.h"
#include "sprite_frag_spv.h"
#ifdef QGAME_HAS_DXIL_SHADERS
#include "sprite_vert_dxil.h"
#include "sprite_frag_dxil.h"
#endif

namespace backend {

SDLGPURenderDevice::SDLGPURenderDevice(SDL_Window* window)
    : window_(window) {
    batchVerts_.reserve(MAX_SPRITES_PER_BATCH * 4);
    batchIdx_.reserve(MAX_SPRITES_PER_BATCH * 6);
}

SDLGPURenderDevice::~SDLGPURenderDevice() {
    shutdown();
}

void SDLGPURenderDevice::init() {
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef QGAME_HAS_DXIL_SHADERS
    formats |= SDL_GPU_SHADERFORMAT_DXIL;
#endif

    device_ = SDL_CreateGPUDevice(formats, false, nullptr);
    if (!device_) {
        core::logError("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return;
    }

    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        core::logError("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        return;
    }

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

    SDL_GPUBufferCreateInfo vbInfo{};
    vbInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vbInfo.size = MAX_SPRITES_PER_BATCH * 4 * sizeof(SpriteVertex);
    vertexBuf_ = SDL_CreateGPUBuffer(device_, &vbInfo);

    SDL_GPUBufferCreateInfo ibInfo{};
    ibInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ibInfo.size = MAX_SPRITES_PER_BATCH * 6 * sizeof(uint16_t);
    indexBuf_ = SDL_CreateGPUBuffer(device_, &ibInfo);

    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size = vbInfo.size + ibInfo.size;
    transferBuf_ = SDL_CreateGPUTransferBuffer(device_, &tbInfo);

    if (!vertexBuf_ || !indexBuf_ || !transferBuf_) {
        core::logError("GPU buffer allocation failed: %s", SDL_GetError());
        return;
    }

    createPipeline();
    core::logInfo("SDLGPURenderDevice initialized");
}

void SDLGPURenderDevice::beginFrame() {
    if (!device_) {
        return;
    }

    gpuCmdBuf_ = SDL_AcquireGPUCommandBuffer(device_);
    ASSERT_MSG(gpuCmdBuf_, "SDL_AcquireGPUCommandBuffer failed");

    SDL_GPUTexture* tex = nullptr;
    const bool ok = SDL_WaitAndAcquireGPUSwapchainTexture(gpuCmdBuf_, window_, &tex, &swapW_, &swapH_);
    if (!ok || !tex) {
        SDL_CancelGPUCommandBuffer(gpuCmdBuf_);
        gpuCmdBuf_ = nullptr;
        swapchainTex_ = nullptr;
        return;
    }

    swapchainTex_ = tex;
}

void SDLGPURenderDevice::endFrame() {
}

void SDLGPURenderDevice::shutdown() {
    if (!device_) {
        return;
    }

    SDL_WaitForGPUIdle(device_);

    if (textures_.valid(editorRenderTarget_)) {
        destroyTexture(editorRenderTarget_);
        editorRenderTarget_ = {};
    }

    if (textures_.valid(offscreenRenderTarget_)) {
        destroyTexture(offscreenRenderTarget_);
        offscreenRenderTarget_ = {};
    }

    if (pipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_); pipeline_ = nullptr; }
    if (offscreenPipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, offscreenPipeline_); offscreenPipeline_ = nullptr; }
    if (vertexBuf_) { SDL_ReleaseGPUBuffer(device_, vertexBuf_); vertexBuf_ = nullptr; }
    if (indexBuf_) { SDL_ReleaseGPUBuffer(device_, indexBuf_); indexBuf_ = nullptr; }
    if (transferBuf_) { SDL_ReleaseGPUTransferBuffer(device_, transferBuf_); transferBuf_ = nullptr; }

    SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;
    core::logInfo("SDLGPURenderDevice shutdown");
}

TextureHandle SDLGPURenderDevice::createTexture(const TextureDesc& desc) {
    ASSERT(desc.data && desc.width > 0 && desc.height > 0);
    if (!device_) {
        core::logError("createTexture: GPU device not initialized");
        return {};
    }

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<uint32_t>(desc.width);
    info.height = static_cast<uint32_t>(desc.height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* gpuTex = SDL_CreateGPUTexture(device_, &info);
    if (!gpuTex) {
        core::logError("SDL_CreateGPUTexture failed (%dx%d): %s", desc.width, desc.height, SDL_GetError());
        return {};
    }

    const size_t dataSize = static_cast<size_t>(desc.width) * static_cast<size_t>(desc.height) * 4u;
    SDL_GPUTransferBufferCreateInfo tbInfo{};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size = static_cast<uint32_t>(dataSize);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tbInfo);

    void* mapped = SDL_MapGPUTransferBuffer(device_, tb, false);
    memcpy(mapped, desc.data, dataSize);
    SDL_UnmapGPUTransferBuffer(device_, tb);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;

    SDL_GPUTextureRegion dst{};
    dst.texture = gpuTex;
    dst.w = info.width;
    dst.h = info.height;
    dst.d = 1;

    SDL_UploadToGPUTexture(copyPass, &src, &dst, false);
    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, tb);

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device_, &samplerInfo);

    return textures_.insert(TextureEntry{ gpuTex, sampler, desc.width, desc.height });
}

TextureHandle SDLGPURenderDevice::createRenderTargetTexture(int width, int height) {
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
    SDL_WaitForGPUIdle(device_);
    if (entry.sampler) SDL_ReleaseGPUSampler(device_, entry.sampler);
    if (entry.gpuTex) SDL_ReleaseGPUTexture(device_, entry.gpuTex);
    textures_.remove(h);
}

ShaderHandle SDLGPURenderDevice::createShader(const ShaderDesc&) {
    return {};
}

void SDLGPURenderDevice::destroyShader(ShaderHandle) {
}

void SDLGPURenderDevice::submitCommandBuffer(const CommandBuffer& cb) {
    if (!gpuCmdBuf_ || !swapchainTex_) {
        return;
    }
    renderCommandBufferToTarget(gpuCmdBuf_, pipeline_, cb, swapchainTex_, swapW_, swapH_, true);
}

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

SDL_GPUTexture* SDLGPURenderDevice::getSDLTexture(TextureHandle handle) const {
    const TextureEntry* entry = textures_.valid(handle) ? &textures_.get(handle) : nullptr;
    return entry ? entry->gpuTex : nullptr;
}

void* SDLGPURenderDevice::getRawTexture(TextureHandle handle) const {
    return getSDLTexture(handle);
}

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

    batchVerts_.clear();
    batchIdx_.clear();
    std::vector<BatchSegment> batches;

    // 按命令流顺序构建 batch：texture 变化或 batch 满时 flush。
    // sprite/tile 完全按 CommandBuffer 提交顺序绘制，不再二次排序或分组。
    TextureHandle currentTex{};
    bool          hasCurrent = false;
    uint32_t      batchIdxStart  = 0;
    int32_t       batchVertStart = 0;

    auto flush = [&]() {
        if (static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
            batches.push_back({ currentTex, batchIdxStart,
                                static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart,
                                batchVertStart });
            batchIdxStart  = static_cast<uint32_t>(batchIdx_.size());
            batchVertStart = static_cast<int32_t>(batchVerts_.size());
        }
    };
    auto maybeFlush = [&](TextureHandle tex) {
        const bool batchFull =
            (batchVerts_.size() - static_cast<size_t>(batchVertStart) >= MAX_SPRITES_PER_BATCH * 4);
        if (!hasCurrent || tex != currentTex || batchFull) {
            flush();
            currentTex = tex;
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
    }
    flush();

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

    float proj[16];
    float view[16];
    const float zoom = (camera.zoom > 0.f) ? camera.zoom : 1.f;
    buildOrthoProjectionMatrix(static_cast<float>(targetWidth), static_cast<float>(targetHeight), proj);
    buildViewMatrix(camera.x, camera.y, zoom, camera.rotation, view);

    float mvp[16];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            mvp[i * 4 + j] = 0.f;
            for (int k = 0; k < 4; ++k) {
                mvp[i * 4 + j] += proj[i * 4 + k] * view[k * 4 + j];
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
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_PushGPUVertexUniformData(cmdBuf, 0, mvp, sizeof(mvp));

    if (!batchVerts_.empty()) {
        SDL_GPUBufferBinding vertexBinding{ vertexBuf_, 0 };
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
        SDL_GPUBufferBinding indexBinding{ indexBuf_, 0 };
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

        for (const BatchSegment& segment : batches) {
            if (textures_.valid(segment.tex)) {
                TextureEntry& entry = textures_.get(segment.tex);
                SDL_GPUTextureSamplerBinding binding{ entry.gpuTex, entry.sampler };
                SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            }
            SDL_DrawGPUIndexedPrimitives(pass, segment.idxCount, 1, segment.idxOffset, segment.vertOffset, 0);
        }
    }

    SDL_EndGPURenderPass(pass);
}

void SDLGPURenderDevice::submitImGuiDrawData(const ImDrawData* drawData) {
    if (!gpuCmdBuf_ || !swapchainTex_ || drawData == nullptr) {
        return;
    }

    ImDrawData* mutableDrawData = const_cast<ImDrawData*>(drawData);
    if (mutableDrawData->TotalVtxCount <= 0) {
        return;
    }

    ImGui_ImplSDLGPU3_PrepareDrawData(mutableDrawData, gpuCmdBuf_);

    SDL_GPUColorTargetInfo colorTarget{};
    colorTarget.texture = swapchainTex_;
    colorTarget.load_op = SDL_GPU_LOADOP_LOAD;
    colorTarget.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(gpuCmdBuf_, &colorTarget, 1, nullptr);
    ImGui_ImplSDLGPU3_RenderDrawData(mutableDrawData, gpuCmdBuf_, pass);
    SDL_EndGPURenderPass(pass);
}

void SDLGPURenderDevice::present() {
    if (!gpuCmdBuf_) {
        return;
    }
    SDL_SubmitGPUCommandBuffer(gpuCmdBuf_);
    gpuCmdBuf_ = nullptr;
    swapchainTex_ = nullptr;
}

void SDLGPURenderDevice::initImGui() {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));

    ImGui_ImplSDLGPU3_InitInfo initInfo{};
    initInfo.Device = device_;
    initInfo.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    ImGui_ImplSDLGPU3_Init(&initInfo);
}

void SDLGPURenderDevice::shutdownImGui() {
    ImGui_ImplSDLGPU3_Shutdown();
}

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

void SDLGPURenderDevice::createPipeline() {
    if (!device_) {
        return;
    }

    SDL_GPUTextureFormat swapchainFormat = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    pipeline_ = createPipelineForFormat(swapchainFormat);
    ASSERT_MSG(pipeline_, "Failed to create swapchain pipeline");

    offscreenPipeline_ = createPipelineForFormat(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    ASSERT_MSG(offscreenPipeline_, "Failed to create offscreen pipeline");

    core::logInfo("Pipelines created (swapchain: 0x%x, offscreen: R8G8B8A8)", static_cast<int>(swapchainFormat));
}

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

void SDLGPURenderDevice::buildSpriteGeometry(const std::vector<DrawSpriteCmd>& cmds, std::vector<BatchSegment>& batches) {
    if (cmds.empty()) {
        return;
    }

    TextureHandle currentTexture{};
    uint32_t batchIdxStart = 0;
    int32_t batchVertStart = 0;

    for (const DrawSpriteCmd& cmd : cmds) {
        const bool needFlush = (cmd.texture != currentTexture) ||
            (batchVerts_.size() - static_cast<size_t>(batchVertStart) >= MAX_SPRITES_PER_BATCH * 4);
        if (needFlush && !batchVerts_.empty() && static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
            batches.push_back({ currentTexture, batchIdxStart, static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart, batchVertStart });
            batchIdxStart = static_cast<uint32_t>(batchIdx_.size());
            batchVertStart = static_cast<int32_t>(batchVerts_.size());
        }
        currentTexture = cmd.texture;

        const float hw = cmd.srcRect.w * cmd.scaleX * 0.5f;
        const float hh = cmd.srcRect.h * cmd.scaleY * 0.5f;
        const float cosR = cosf(cmd.rotation);
        const float sinR = sinf(cmd.rotation);

        const float lx[4] = { -hw, hw, hw, -hw };
        const float ly[4] = { -hh, -hh, hh, hh };

        TextureEntry* textureEntry = textures_.tryGet(currentTexture);
        const float tw = textureEntry ? static_cast<float>(textureEntry->width) : 1.0f;
        const float th = textureEntry ? static_cast<float>(textureEntry->height) : 1.0f;
        const float u0 = cmd.srcRect.x / tw;
        const float v0 = cmd.srcRect.y / th;
        const float u1 = (cmd.srcRect.x + cmd.srcRect.w) / tw;
        const float v1 = (cmd.srcRect.y + cmd.srcRect.h) / th;
        const float us[4] = { u0, u1, u1, u0 };
        const float vs[4] = { v0, v0, v1, v1 };

        const uint16_t base = static_cast<uint16_t>(batchVerts_.size() - static_cast<size_t>(batchVertStart));
        for (int i = 0; i < 4; ++i) {
            batchVerts_.push_back({
                cmd.x + lx[i] * cosR - ly[i] * sinR,
                cmd.y + lx[i] * sinR + ly[i] * cosR,
                us[i], vs[i],
                cmd.tint.r, cmd.tint.g, cmd.tint.b, cmd.tint.a
            });
        }
        batchIdx_.insert(batchIdx_.end(), { base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2), base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3) });
    }

    if (static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
        batches.push_back({ currentTexture, batchIdxStart, static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart, batchVertStart });
    }
}

void SDLGPURenderDevice::buildTileGeometry(const std::vector<DrawTileCmd>& cmds, std::vector<BatchSegment>& batches) {
    if (cmds.empty()) {
        return;
    }

    TextureHandle currentTexture{};
    uint32_t batchIdxStart = 0;
    int32_t batchVertStart = 0;

    for (const DrawTileCmd& cmd : cmds) {
        const bool needFlush = (cmd.tileset != currentTexture) ||
            (batchVerts_.size() - static_cast<size_t>(batchVertStart) >= MAX_SPRITES_PER_BATCH * 4);
        if (needFlush && !batchVerts_.empty() && static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
            batches.push_back({ currentTexture, batchIdxStart, static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart, batchVertStart });
            batchIdxStart = static_cast<uint32_t>(batchIdx_.size());
            batchVertStart = static_cast<int32_t>(batchVerts_.size());
        }
        currentTexture = cmd.tileset;

        TextureEntry* textureEntry = textures_.tryGet(currentTexture);
        const float tw = textureEntry ? static_cast<float>(textureEntry->width) : 1.0f;
        const float th = textureEntry ? static_cast<float>(textureEntry->height) : 1.0f;
        const int tileSize = cmd.tileSize > 0 ? cmd.tileSize : 16;
        int tilesetCols = static_cast<int>(tw) / tileSize;
        if (tilesetCols < 1) tilesetCols = 1;

        const int col = cmd.tileId % tilesetCols;
        const int row = cmd.tileId / tilesetCols;
        const float u0 = (col * tileSize) / tw;
        const float v0 = (row * tileSize) / th;
        const float u1 = u0 + tileSize / tw;
        const float v1 = v0 + tileSize / th;

        const float px = static_cast<float>(cmd.gridX * tileSize);
        const float py = static_cast<float>(cmd.gridY * tileSize);
        const float px1 = px + tileSize;
        const float py1 = py + tileSize;

        const uint16_t base = static_cast<uint16_t>(batchVerts_.size() - static_cast<size_t>(batchVertStart));
        batchVerts_.push_back({ px, py, u0, v0, 255, 255, 255, 255 });
        batchVerts_.push_back({ px1, py, u1, v0, 255, 255, 255, 255 });
        batchVerts_.push_back({ px1, py1, u1, v1, 255, 255, 255, 255 });
        batchVerts_.push_back({ px, py1, u0, v1, 255, 255, 255, 255 });
        batchIdx_.insert(batchIdx_.end(), { base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2), base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3) });
    }

    if (static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
        batches.push_back({ currentTexture, batchIdxStart, static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart, batchVertStart });
    }
}

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
    const float halfW = 0.5f / zoom;
    const float halfH = 0.5f / zoom;
    const float cosR  = cosf(rotation);
    const float sinR  = sinf(rotation);

    memset(out, 0, 16 * sizeof(float));
    out[0]  =  cosR * halfW;
    out[1]  =  sinR * halfW;
    out[4]  = -sinR * halfH;
    out[5]  =  cosR * halfH;
    out[10] = 1.f;
    out[12] = -camX * cosR * halfW + camY * sinR * halfW + halfW;
    out[13] = -camX * sinR * halfH - camY * cosR * halfH + halfH;
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
                out[i * 4 + j] += proj[i * 4 + k] * view[k * 4 + j];
            }
        }
    }
}

} // namespace backend
