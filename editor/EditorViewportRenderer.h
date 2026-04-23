#pragma once

#include <backend/renderer/CommandBuffer.h>
#include <backend/shared/ResourceHandle.h>
#include <backend/renderer/sdl_gpu/SDLGPURenderDevice.h>
#include <engine/runtime/EngineContext.h>
#include <engine/components/RenderComponents.h>
#include <SDL3/SDL_gpu.h>

namespace editor {

class EditorViewportRenderer {
public:
    explicit EditorViewportRenderer(engine::EngineContext& ctx);

    TextureHandle render(int width, int height);
    SDL_GPUTexture* getTexture(TextureHandle handle) const;
    bool isAvailable() const;

private:
    void buildCommandBuffer(int viewportW, int viewportH);

    engine::EngineContext& ctx_;
    backend::CommandBuffer viewportCmdBuf_;
    TextureHandle cachedTexture_;
    int cachedWidth_ = 0;
    int cachedHeight_ = 0;
};

} // namespace editor
