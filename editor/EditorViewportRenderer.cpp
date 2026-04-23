#include "EditorViewportRenderer.h"
#include <backend/renderer/sdl_gpu/SDLGPURenderDevice.h>
#include <backend/shared/ResourceHandle.h>
#include <core/Logger.h>

namespace editor {

EditorViewportRenderer::EditorViewportRenderer(engine::EngineContext& ctx)
    : ctx_(ctx) {
}

bool EditorViewportRenderer::isAvailable() const {
    auto* renderDevice = dynamic_cast<backend::SDLGPURenderDevice*>(&ctx_.renderDevice());
    return renderDevice != nullptr;
}

TextureHandle EditorViewportRenderer::render(int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }

    auto* renderDevice = dynamic_cast<backend::SDLGPURenderDevice*>(&ctx_.renderDevice());
    if (!renderDevice) {
        core::logError("EditorViewportRenderer: renderDevice is null");
        return {};
    }

    buildCommandBuffer(width, height);
    core::logInfo("EditorViewportRenderer: rendering %dx%d, commands=%zu", width, height, viewportCmdBuf_.commands().size());

    TextureHandle texture = ctx_.renderDevice().renderToTextureOffscreen(viewportCmdBuf_, width, height);
    if (texture.valid()) {
        core::logInfo("EditorViewportRenderer: got texture handle %u", texture.index);
        cachedTexture_ = texture;
        cachedWidth_ = width;
        cachedHeight_ = height;
    } else {
        core::logError("EditorViewportRenderer: renderToTextureOffscreen returned invalid handle");
    }

    return texture;
}

SDL_GPUTexture* EditorViewportRenderer::getTexture(TextureHandle handle) const {
    auto* renderDevice = dynamic_cast<backend::SDLGPURenderDevice*>(&ctx_.renderDevice());
    if (!renderDevice || !handle.valid()) {
        core::logError("EditorViewportRenderer::getTexture: invalid params renderDevice=%p handle.valid=%d", 
                       renderDevice, handle.valid());
        return nullptr;
    }
    SDL_GPUTexture* tex = renderDevice->getSDLTexture(handle);
    core::logInfo("EditorViewportRenderer::getTexture: handle=%u -> tex=%p", handle.index, tex);
    return tex;
}

void EditorViewportRenderer::buildCommandBuffer(int viewportW, int viewportH) {
    viewportCmdBuf_.begin();
    viewportCmdBuf_.clear(core::Color::Black);

    backend::CameraData cam{};
    cam.viewportW = viewportW;
    cam.viewportH = viewportH;

    auto camView = ctx_.world.view<engine::Transform, engine::Camera>();
    for (auto [ent, tf, camera] : camView.each()) {
        if (!camera.primary) continue;
        cam.x = tf.x;
        cam.y = tf.y;
        cam.zoom = camera.zoom;
        break;
    }
    viewportCmdBuf_.setCamera(cam);

    auto tileView = ctx_.world.view<engine::Transform, engine::TileMap>();
    for (auto [ent, tf, tmap] : tileView.each()) {
        for (int layer = 0; layer < engine::TileMap::MAX_LAYERS; ++layer) {
            for (int y = 0; y < tmap.height; ++y) {
                for (int x = 0; x < tmap.width; ++x) {
                    int tileId = tmap.tileAt(layer, x, y);
                    if (tileId < 0) continue;
                    backend::DrawTileCmd cmd{};
                    cmd.tileset = tmap.tileset;
                    cmd.tileId = tileId;
                    cmd.gridX = static_cast<int>(tf.x) + x;
                    cmd.gridY = static_cast<int>(tf.y) + y;
                    cmd.tileSize = tmap.tileSize;
                    cmd.layer = layer;
                    viewportCmdBuf_.drawTile(cmd);
                }
            }
        }
    }

    auto spriteView = ctx_.world.view<engine::Transform, engine::Sprite>();
    for (auto [ent, tf, sprite] : spriteView.each()) {
        backend::DrawSpriteCmd cmd{};
        cmd.texture = sprite.texture;
        cmd.x = tf.x;
        cmd.y = tf.y;
        cmd.rotation = tf.rotation;
        cmd.scaleX = tf.scaleX;
        cmd.scaleY = tf.scaleY;
        cmd.srcRect = sprite.srcRect;
        cmd.layer = sprite.layer;
        cmd.tint = sprite.tint;
        viewportCmdBuf_.drawSprite(cmd);
    }

    viewportCmdBuf_.end();
}

} // namespace editor
