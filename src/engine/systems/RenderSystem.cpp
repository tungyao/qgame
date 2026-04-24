#include "RenderSystem.h"
#include "../runtime/EngineContext.h"
#include "../components/RenderComponents.h"
#include "../components/TextComponent.h"
#include "../../backend/renderer/CommandBuffer.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../core/Logger.h"
#include <algorithm>
#include <vector>

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

namespace {
// 统一的 2D 可绘制条目：sprite 和 tile 走同一条排序流水线，保证
// 不同命令类型在同 pass/layer 下也能按 ySort / sortKey 正确交错。
enum class DrawKind { Sprite, Tile, Text };

struct Drawable {
    engine::RenderPass pass;
    int   layer;
    bool  ySort;
    float y;
    int   sortKey;
    int   seq;        // 稳定 tie-breaker

    DrawKind kind;
    backend::DrawSpriteCmd sprite;
    backend::DrawTileCmd   tile;
    backend::DrawTextCmd   text;
};

bool drawableLess(const Drawable& A, const Drawable& B) {
    if (A.pass  != B.pass)  return static_cast<int>(A.pass) < static_cast<int>(B.pass);
    if (A.layer != B.layer) return A.layer < B.layer;
    // 非 ySort 在前（底层），ySort 在后（按 y 交错）
    if (A.ySort != B.ySort) return !A.ySort;
    if (A.ySort) {
        int ay = static_cast<int>(A.y);
        int by = static_cast<int>(B.y);
        if (ay != by) return ay < by;
    }
    if (A.sortKey != B.sortKey) return A.sortKey < B.sortKey;
    return A.seq < B.seq;
}
} // namespace

void RenderSystem::buildSceneCommands(EngineContext& ctx, backend::CommandBuffer& cb, int viewportW, int viewportH) {
    // 场景命令流中不再包含 Clear/SetCamera：
    //   - swapchain 路径由 RenderPipeline 通过 PassState 统一管理
    //   - editor 离屏路径需自行在 submit 前 cb.clear()/cb.setCamera()
    cb.begin();

    static std::vector<Drawable> drawables;
    drawables.clear();
    int seq = 0;

    // tilemap
    auto tileView = ctx.world.view<Transform, TileMap>();
    for (auto [ent, tf, tmap] : tileView.each()) {
        for (int layer = 0; layer < TileMap::MAX_LAYERS; ++layer) {
            for (int y = 0; y < tmap.height; ++y) {
                for (int x = 0; x < tmap.width; ++x) {
                    int tileId = tmap.tileAt(layer, x, y);
                    if (tileId < 0) continue;
                    Drawable d{};
                    d.pass   = RenderPass::World;
                    d.layer  = layer;
                    d.ySort  = true;  // 启用 y-sorting 以支持正确的遮挡关系
                    d.y      = tf.y + static_cast<float>(y * tmap.tileSize);
                    d.sortKey = 0;
                    d.seq    = seq++;
                    d.kind   = DrawKind::Tile;
                    d.tile.tileset  = tmap.tileset;
                    d.tile.tileId   = tileId;
                    d.tile.gridX    = static_cast<int>(tf.x) + x;
                    d.tile.gridY    = static_cast<int>(tf.y) + y;
                    d.tile.tileSize = tmap.tileSize;
                    d.tile.layer    = layer;
                    d.tile.sortKey  = 0;
                    d.tile.ySort    = true;  // 启用 y-sorting
                    d.tile.pass     = RenderPass::World;
                    drawables.push_back(d);
                }
            }
        }
    }

    // sprite
    auto spriteView = ctx.world.view<Transform, Sprite>();
    for (auto [ent, tf, sprite] : spriteView.each()) {
        Drawable d{};
        d.pass    = sprite.pass;
        d.layer   = sprite.layer;
        d.ySort   = sprite.ySort;
        d.y       = tf.y;
        d.sortKey = sprite.sortOrder;
        d.seq     = seq++;
        d.kind    = DrawKind::Sprite;
        auto& s = d.sprite;
        s.texture  = sprite.texture;
        s.x        = tf.x;
        s.y        = tf.y;
        s.rotation = tf.rotation;
        s.scaleX   = tf.scaleX;
        s.scaleY   = tf.scaleY;
        s.pivotX   = sprite.pivotX;
        s.pivotY   = sprite.pivotY;
        s.srcRect  = sprite.srcRect;
        s.layer    = sprite.layer;
        s.sortKey  = sprite.sortOrder;
        s.ySort    = sprite.ySort;
        s.tint     = sprite.tint;
        s.pass     = sprite.pass;
        drawables.push_back(d);
    }

    // text
    auto textView = ctx.world.view<Transform, TextComponent>();
    for (auto [ent, tf, text] : textView.each()) {
        if (!text.visible || text.text.empty()) continue;

        Drawable d{};
        d.pass    = text.pass;
        d.layer   = text.layer;
        d.ySort   = text.ySort;
        d.y       = tf.y;
        d.sortKey = text.sortOrder;
        d.seq     = seq++;
        d.kind    = DrawKind::Text;
        auto& t = d.text;
        t.font     = text.font;
        t.text     = text.text;
        t.x        = tf.x;
        t.y        = tf.y;
        t.fontSize = text.fontSize;
        t.layer    = text.layer;
        t.sortKey  = text.sortOrder;
        t.ySort    = text.ySort;
        t.color    = text.color;
        t.pass     = text.pass;
        drawables.push_back(d);
    }

    std::sort(drawables.begin(), drawables.end(), drawableLess);

    // 按排序顺序写入 CommandBuffer。后端必须按命令流顺序绘制，不再二次排序。
    for (const Drawable& d : drawables) {
        switch (d.kind) {
            case DrawKind::Tile:   cb.drawTile(d.tile);     break;
            case DrawKind::Sprite: cb.drawSprite(d.sprite); break;
            case DrawKind::Text:   cb.drawText(d.text);     break;
        }
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
