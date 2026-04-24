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
    if (!ctx_.renderToSwapchain) {
        return;
    }
    buildCommandBuffer();
}

void RenderSystem::shutdown() {}

static constexpr int MAX_SPRITES = 4096;
static backend::DrawSpriteCmd spriteBuffer[MAX_SPRITES];
static int spriteCount = 0;

static void collectSprites(backend::DrawSpriteCmd*& out, Transform& tf, Sprite& spr) {
    out->texture  = spr.texture;
    out->x        = tf.x;
    out->y        = tf.y;
    out->rotation = tf.rotation;
    out->scaleX   = tf.scaleX;
    out->scaleY   = tf.scaleY;
    out->pivotX   = spr.pivotX;
    out->pivotY   = spr.pivotY;
    out->srcRect  = spr.srcRect;
    out->layer    = spr.layer;
    out->sortKey  = spr.sortOrder;
    out->ySort    = spr.ySort;
    out->tint     = spr.tint;
    out->pass     = spr.pass;
    ++out;
}

static int compareSprite(const void* a, const void* b) {
    const auto& A = *(const backend::DrawSpriteCmd*)a;
    const auto& B = *(const backend::DrawSpriteCmd*)b;
    if (A.pass != B.pass) return static_cast<int>(A.pass) - static_cast<int>(B.pass);
    if (A.layer != B.layer) return A.layer - B.layer;
    if (A.ySort && B.ySort) {
        int ay = static_cast<int>(A.y);
        int by = static_cast<int>(B.y);
        if (ay != by) return ay - by;
    }
    if (A.ySort != B.ySort) return A.ySort ? 1 : -1;
    return A.sortKey - B.sortKey;
}

void RenderSystem::buildSceneCommands(EngineContext& ctx, backend::CommandBuffer& cb, int viewportW, int viewportH) {
    cb.begin();
    cb.clear(core::Color::Black);

    backend::CameraData worldCam{};
    worldCam.viewportW = viewportW;
    worldCam.viewportH = viewportH;

    auto camView = ctx.world.view<Transform, Camera>();
    for (auto [ent, tf, camera] : camView.each()) {
        if (camera.type == CameraType::World && camera.primary) {
            worldCam.x        = tf.x;
            worldCam.y        = tf.y;
            worldCam.zoom     = camera.zoom;
            worldCam.rotation = camera.rotation;
        }
    }

    spriteCount = 0;
    auto* spritePtr = spriteBuffer;

    auto tileView = ctx.world.view<Transform, TileMap>();
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
                    cmd.pass     = RenderPass::World;
                    cb.drawTile(cmd);
                }
            }
        }
    }

    auto spriteView = ctx.world.view<Transform, Sprite>();
    for (auto [ent, tf, sprite] : spriteView.each()) {
        collectSprites(spritePtr, tf, sprite);
        ++spriteCount;
    }

    if (spriteCount > 1) {
        qsort(spriteBuffer, spriteCount, sizeof(backend::DrawSpriteCmd), compareSprite);
    }

    RenderPass currentPass = RenderPass::World;
    cb.beginPass(currentPass);
    cb.setCamera(worldCam);

    for (int i = 0; i < spriteCount; ++i) {
        const auto& cmd = spriteBuffer[i];
        if (cmd.pass != currentPass) {
            cb.endPass(currentPass);
            currentPass = cmd.pass;
            cb.beginPass(currentPass);
            if (currentPass == RenderPass::World) {
                cb.setCamera(worldCam);
            } else {
                backend::CameraData screenCam{};
                screenCam.viewportW = viewportW;
                screenCam.viewportH = viewportH;
                cb.setCamera(screenCam);
            }
        }
        cb.drawSprite(cmd);
    }
    cb.endPass(currentPass);

    cb.end();
}

void RenderSystem::buildCommandBuffer() {
    backend::CommandBuffer& cb = ctx_.renderCommandBuffer();
    buildSceneCommands(ctx_, cb, ctx_.window->width(), ctx_.window->height());
    ctx_.renderDevice().submitCommandBuffer(cb);
}

} // namespace engine
