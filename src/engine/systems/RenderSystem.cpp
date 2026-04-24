#include "RenderSystem.h"
#include "../runtime/EngineContext.h"
#include "../components/RenderComponents.h"
#include "../../backend/renderer/CommandBuffer.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../core/Logger.h"

namespace engine {

void RenderSystem::init() {
    // 默认两个 pass：World 先、UI 后；World 清屏，UI 叠加
    pipeline_.addPass(RenderPass::World);
    pipeline_.addPass(RenderPass::UI);
    pipeline_.setPassClear(RenderPass::World, true, core::Color::Black);
    pipeline_.setPassClear(RenderPass::UI,    false);
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
    // 场景命令流中不再包含 Clear/SetCamera：
    //   - swapchain 路径由 RenderPipeline 通过 PassState 统一管理
    //   - editor 离屏路径需自行在 submit 前 cb.clear()/cb.setCamera()
    cb.begin();

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

    for (int i = 0; i < spriteCount; ++i) {
        const auto& cmd = spriteBuffer[i];
        cb.drawSprite(cmd);
    }

    cb.end();
}

void RenderSystem::syncCamerasToPassStates(int viewportW, int viewportH) {
    backend::CameraData worldCam{};
    worldCam.viewportW = viewportW;
    worldCam.viewportH = viewportH;

    auto camView = ctx_.world.view<Transform, Camera>();
    for (auto [ent, tf, camera] : camView.each()) {
        if (camera.type == CameraType::World && camera.primary) {
            worldCam.x        = tf.x;
            worldCam.y        = tf.y;
            worldCam.zoom     = camera.zoom;
            worldCam.rotation = camera.rotation;
        }
    }

    backend::CameraData screenCam{};
    screenCam.viewportW = viewportW;
    screenCam.viewportH = viewportH;

    pipeline_.setPassCamera(RenderPass::World, worldCam);
    pipeline_.setPassCamera(RenderPass::UI,    screenCam);
}

void RenderSystem::buildCommandBuffer() {
    const int w = ctx_.window->width();
    const int h = ctx_.window->height();

    backend::CommandBuffer& cb = ctx_.renderCommandBuffer();
    buildSceneCommands(ctx_, cb, w, h);

    syncCamerasToPassStates(w, h);
    pipeline_.execute(cb, ctx_.renderDevice());
}

} // namespace engine
