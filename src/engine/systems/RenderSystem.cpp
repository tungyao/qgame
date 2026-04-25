#include "RenderSystem.h"
#include "../runtime/EngineContext.h"
#include "../components/RenderComponents.h"
#include "../components/TextComponent.h"
#include "../../backend/renderer/CommandBuffer.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../core/Logger.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace engine {

namespace {

// 世界空间视锥矩形（轴对齐）。zoom <= 0 表示禁用剔除。
struct ViewRect {
    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    bool  enabled = false;

    bool contains(float x, float y) const {
        return enabled ? (x >= minX && x <= maxX && y >= minY && y <= maxY) : true;
    }
    bool intersectsAABB(float x0, float y0, float x1, float y1) const {
        if (!enabled) return true;
        return !(x1 < minX || x0 > maxX || y1 < minY || y0 > maxY);
    }
};

ViewRect computeWorldViewRect(EngineContext& ctx, int viewportW, int viewportH) {
    ViewRect vr{};
    auto camView = ctx.world.view<Transform, Camera>();
    for (auto [ent, tf, camera] : camView.each()) {
        if (camera.type != CameraType::World || !camera.primary) continue;
        if (camera.zoom <= 0.f) return vr;

        // 旋转相机时取外接 AABB（乘 sqrt(2) 足够覆盖任意角度）
        const float halfW = (viewportW * 0.5f) / camera.zoom;
        const float halfH = (viewportH * 0.5f) / camera.zoom;
        float rx = halfW, ry = halfH;
        if (camera.rotation != 0.f) {
            const float c = std::abs(std::cos(camera.rotation));
            const float s = std::abs(std::sin(camera.rotation));
            rx = halfW * c + halfH * s;
            ry = halfW * s + halfH * c;
        }
        vr.minX = tf.x - rx;
        vr.maxX = tf.x + rx;
        vr.minY = tf.y - ry;
        vr.maxY = tf.y + ry;
        vr.enabled = true;
        return vr;
    }
    return vr;
}

} // namespace

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

    const ViewRect viewRect = computeWorldViewRect(ctx, viewportW, viewportH);

    static std::vector<Drawable> drawables;
    drawables.clear();
    int seq = 0;

    // tilemap：先把 view rect 反变换到 tile 网格坐标，按可见范围裁剪
    auto tileView = ctx.world.view<Transform, TileMap>();
    for (auto [ent, tf, tmap] : tileView.each()) {
        if (tmap.tileSize <= 0) continue;

        int xBegin = 0, xEnd = tmap.width;
        int yBegin = 0, yEnd = tmap.height;
        if (viewRect.enabled) {
            const float ts = static_cast<float>(tmap.tileSize);
            xBegin = std::max(0,           static_cast<int>(std::floor((viewRect.minX - tf.x) / ts)));
            xEnd   = std::min(tmap.width,  static_cast<int>(std::ceil ((viewRect.maxX - tf.x) / ts)) + 1);
            yBegin = std::max(0,           static_cast<int>(std::floor((viewRect.minY - tf.y) / ts)));
            yEnd   = std::min(tmap.height, static_cast<int>(std::ceil ((viewRect.maxY - tf.y) / ts)) + 1);
            if (xBegin >= xEnd || yBegin >= yEnd) continue;
        }

        for (int layer = 0; layer < TileMap::MAX_LAYERS; ++layer) {
            for (int y = yBegin; y < yEnd; ++y) {
                for (int x = xBegin; x < xEnd; ++x) {
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

    // sprite：仅对 World pass 做 AABB 剔除，UI/Screen pass 不裁
    auto spriteView = ctx.world.view<Transform, Sprite>();
    for (auto [ent, tf, sprite] : spriteView.each()) {
        if (viewRect.enabled && sprite.pass == RenderPass::World) {
            const float w = sprite.srcRect.w * std::abs(tf.scaleX);
            const float h = sprite.srcRect.h * std::abs(tf.scaleY);
            // pivot 偏移：中心相对 (tf.x,tf.y) 的位移
            const float cx = tf.x + (0.5f - sprite.pivotX) * w;
            const float cy = tf.y + (0.5f - sprite.pivotY) * h;
            // 旋转外接：sqrt(2)/2 ≈ 0.7071 用于半边长
            float halfW = w * 0.5f;
            float halfH = h * 0.5f;
            if (tf.rotation != 0.f) {
                const float c = std::abs(std::cos(tf.rotation));
                const float s = std::abs(std::sin(tf.rotation));
                const float hw = halfW, hh = halfH;
                halfW = hw * c + hh * s;
                halfH = hw * s + hh * c;
            }
            if (!viewRect.intersectsAABB(cx - halfW, cy - halfH, cx + halfW, cy + halfH)) {
                continue;
            }
        }
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
