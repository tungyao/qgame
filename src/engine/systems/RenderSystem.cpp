#include "RenderSystem.h"
#include "../runtime/EngineContext.h"
#include "../components/RenderComponents.h"
#include "../../backend/renderer/CommandBuffer.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../core/Logger.h"

namespace engine {

void RenderSystem::init() {
    core::logInfo("RenderSystem initialized");
}

void RenderSystem::update(float /*dt*/) {
    buildCommandBuffer();
}

void RenderSystem::shutdown() {}

void RenderSystem::buildCommandBuffer() {
    backend::CommandBuffer& cb = ctx_.renderCommandBuffer();
    cb.begin();

    // Clear
    cb.clear(core::Color::Black);

    // ── 摄像机 ───────────────────────────────────────────────────────────────
    backend::CameraData cam{};
    cam.viewportW = ctx_.window->width();
    cam.viewportH = ctx_.window->height();

    auto camView = ctx_.world.view<Transform, Camera>();
    for (auto [ent, tf, camera] : camView.each()) {
        if (!camera.primary) continue;
        cam.x    = tf.x;
        cam.y    = tf.y;
        cam.zoom = camera.zoom;
        break;
    }
    cb.setCamera(cam);

    // ── TileMap（先画，在 Sprite 下方）────────────────────────────────────────
    auto tileView = ctx_.world.view<Transform, TileMap>();
    for (auto [ent, tf, tmap] : tileView.each()) {
        for (int layer = 0; layer < TileMap::MAX_LAYERS; ++layer) {
            for (int y = 0; y < tmap.height; ++y) {
                for (int x = 0; x < tmap.width; ++x) {
                    int tileId = tmap.tileAt(layer, x, y);
                    if (tileId < 0) continue;
                    backend::DrawTileCmd cmd{};
                    cmd.tileset  = tmap.tileset;
                    cmd.tileId   = tileId;
                    cmd.gridX    = static_cast<int>(tf.x) + x;
                    cmd.gridY    = static_cast<int>(tf.y) + y;
                    cmd.tileSize = tmap.tileSize;
                    cmd.layer    = layer;
                    cb.drawTile(cmd);
                }
            }
        }
    }

    // ── Sprite ───────────────────────────────────────────────────────────────
    auto spriteView = ctx_.world.view<Transform, Sprite>();
    for (auto [ent, tf, sprite] : spriteView.each()) {
        backend::DrawSpriteCmd cmd{};
        cmd.texture  = sprite.texture;
        cmd.x        = tf.x;
        cmd.y        = tf.y;
        cmd.rotation = tf.rotation;
        cmd.scaleX   = tf.scaleX;
        cmd.scaleY   = tf.scaleY;
        cmd.srcRect  = sprite.srcRect;
        cmd.layer    = sprite.layer;
        cmd.tint     = sprite.tint;
        cb.drawSprite(cmd);
    }

    cb.end();

    // 提交到 GPU
    ctx_.renderDevice().submitCommandBuffer(cb);
    ctx_.renderDevice().present();
}

} // namespace engine
