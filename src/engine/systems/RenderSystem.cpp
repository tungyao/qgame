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

// 世界空间视锥矩形（轴对齐）。enabled=false → 不裁剪。
struct ViewRect {
    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    bool  enabled = false;

    bool intersectsAABB(float x0, float y0, float x1, float y1) const {
        if (!enabled) return true;
        return !(x1 < minX || x0 > maxX || y1 < minY || y0 > maxY);
    }
};

ViewRect computeCameraViewRect(const Transform& tf, const Camera& cam,
                               int viewportW, int viewportH) {
    ViewRect vr{};
    if (!cam.cullEnabled || cam.zoom <= 0.f) return vr;

    const float halfW = (viewportW * 0.5f) / cam.zoom;
    const float halfH = (viewportH * 0.5f) / cam.zoom;
    float rx = halfW, ry = halfH;
    if (cam.rotation != 0.f) {
        const float c = std::abs(std::cos(cam.rotation));
        const float s = std::abs(std::sin(cam.rotation));
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

backend::CameraData toBackendCamera(const Transform& tf, const Camera& cam,
                                    int viewportW, int viewportH) {
    backend::CameraData out{};
    out.x         = tf.x;
    out.y         = tf.y;
    out.zoom      = (cam.zoom > 0.f) ? cam.zoom : 1.f;
    out.rotation  = cam.rotation;
    out.viewportW = viewportW;
    out.viewportH = viewportH;
    return out;
}

// 提取 drawable 的 pass 字段，用于 layerMask 过滤。
RenderPass cmdPass(const backend::RenderCmd& cmd) {
    if (auto* s = std::get_if<backend::DrawSpriteCmd>(&cmd)) return s->pass;
    if (auto* t = std::get_if<backend::DrawTileCmd>(&cmd))   return t->pass;
    if (auto* x = std::get_if<backend::DrawTextCmd>(&cmd))   return x->pass;
    return RenderPass::World;
}

// 取 drawable 的中心 + AABB 半边长，用于剔除（UI/Screen 不会走到这里）。
bool cmdAABB(const backend::RenderCmd& cmd,
             float& cx, float& cy, float& halfW, float& halfH) {
    if (auto* s = std::get_if<backend::DrawSpriteCmd>(&cmd)) {
        const float w = s->srcRect.w * std::abs(s->scaleX);
        const float h = s->srcRect.h * std::abs(s->scaleY);
        cx = s->x + (0.5f - s->pivotX) * w;
        cy = s->y + (0.5f - s->pivotY) * h;
        halfW = w * 0.5f;
        halfH = h * 0.5f;
        if (s->rotation != 0.f) {
            const float c = std::abs(std::cos(s->rotation));
            const float ss = std::abs(std::sin(s->rotation));
            const float hw = halfW, hh = halfH;
            halfW = hw * c + hh * ss;
            halfH = hw * ss + hh * c;
        }
        return true;
    }
    if (auto* t = std::get_if<backend::DrawTileCmd>(&cmd)) {
        const float ts = static_cast<float>(t->tileSize);
        cx = t->gridX * ts + ts * 0.5f;
        cy = t->gridY * ts + ts * 0.5f;
        halfW = halfH = ts * 0.5f;
        return true;
    }
    // 文本/未知：保守不剔除
    return false;
}

} // namespace

void RenderSystem::init() {
    core::logInfo("RenderSystem initialized (camera-driven)");
}

void RenderSystem::update(float /*dt*/) {
    if (!ctx_.renderToSwapchain) {
        return;
    }
    buildCommandBuffer();
}

void RenderSystem::shutdown() {}

namespace {

enum class DrawKind { Sprite, Tile, Text };

struct Drawable {
    RenderPass pass;
    int   layer;
    bool  ySort;
    float y;
    int   sortKey;
    int   seq;

    DrawKind kind;
    backend::DrawSpriteCmd sprite;
    backend::DrawTileCmd   tile;
    backend::DrawTextCmd   text;
};

bool drawableLess(const Drawable& A, const Drawable& B) {
    if (A.pass  != B.pass)  return static_cast<int>(A.pass) < static_cast<int>(B.pass);
    if (A.layer != B.layer) return A.layer < B.layer;
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

void RenderSystem::buildSceneCommands(EngineContext& ctx, backend::CommandBuffer& cb,
                                      int /*viewportW*/, int /*viewportH*/) {
    // 不再做相机相关裁剪：相机是渲染单元，剔除由 buildCommandBuffer 按相机各自处理。
    // 这里只把所有 ECS 实体录制为统一排好序的命令流。编辑器离屏路径也用这一份。
    cb.begin();

    static std::vector<Drawable> drawables;
    drawables.clear();
    int seq = 0;

    // tilemap
    auto tileView = ctx.world.view<Transform, TileMap>();
    for (auto [ent, tf, tmap] : tileView.each()) {
        if (tmap.tileSize <= 0) continue;
        for (int layer = 0; layer < TileMap::MAX_LAYERS; ++layer) {
            for (int y = 0; y < tmap.height; ++y) {
                for (int x = 0; x < tmap.width; ++x) {
                    int tileId = tmap.tileAt(layer, x, y);
                    if (tileId < 0) continue;
                    Drawable d{};
                    d.pass    = RenderPass::World;
                    d.layer   = layer;
                    d.ySort   = true;
                    d.y       = tf.y + static_cast<float>(y * tmap.tileSize);
                    d.sortKey = 0;
                    d.seq     = seq++;
                    d.kind    = DrawKind::Tile;
                    d.tile.tileset  = tmap.tileset;
                    d.tile.tileId   = tileId;
                    d.tile.gridX    = static_cast<int>(tf.x) + x;
                    d.tile.gridY    = static_cast<int>(tf.y) + y;
                    d.tile.tileSize = tmap.tileSize;
                    d.tile.layer    = layer;
                    d.tile.sortKey  = 0;
                    d.tile.ySort    = true;
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

    for (const Drawable& d : drawables) {
        switch (d.kind) {
            case DrawKind::Tile:   cb.drawTile(d.tile);     break;
            case DrawKind::Sprite: cb.drawSprite(d.sprite); break;
            case DrawKind::Text:   cb.drawText(d.text);     break;
        }
    }

    cb.end();
}

void RenderSystem::buildCommandBuffer() {
    const int w = ctx_.window->width();
    const int h = ctx_.window->height();

    backend::CommandBuffer& cb = ctx_.renderCommandBuffer();
    buildSceneCommands(ctx_, cb, w, h);

    // 收集 active 相机，按 depth 升序
    struct CamEntry {
        const Transform* tf;
        const Camera*    cam;
    };
    std::vector<CamEntry> cameras;
    auto camView = ctx_.world.view<Transform, Camera>();
    for (auto [ent, tf, cam] : camView.each()) {
        if (!cam.primary) continue;
        cameras.push_back({ &tf, &cam });
    }
    std::stable_sort(cameras.begin(), cameras.end(),
                     [](const CamEntry& a, const CamEntry& b) {
                         return a.cam->depth < b.cam->depth;
                     });

    backend::IRenderDevice& dev = ctx_.renderDevice();

    // 没有任何相机：清屏并返回（避免出现未定义画面）
    if (cameras.empty()) {
        backend::IRenderDevice::PassSubmitInfo info;
        info.camera.viewportW = w;
        info.camera.viewportH = h;
        info.clearEnabled = true;
        info.clearColor   = core::Color::Black;
        dev.submitPass(info, {});
        return;
    }

    // 按相机分发命令
    static std::vector<const backend::RenderCmd*> filtered;
    for (size_t i = 0; i < cameras.size(); ++i) {
        const Transform& tf = *cameras[i].tf;
        const Camera&    cam = *cameras[i].cam;

        const ViewRect vr = computeCameraViewRect(tf, cam, w, h);

        filtered.clear();
        for (const backend::RenderCmd& cmd : cb.commands()) {
            // layerMask 过滤
            const RenderPass p = cmdPass(cmd);
            if ((cam.layerMask & renderPassBit(p)) == 0) continue;

            // 视锥剔除（仅 World pass + cullEnabled 相机）
            if (vr.enabled && p == RenderPass::World) {
                float cx, cy, hw, hh;
                if (cmdAABB(cmd, cx, cy, hw, hh)) {
                    if (!vr.intersectsAABB(cx - hw, cy - hh, cx + hw, cy + hh)) {
                        continue;
                    }
                }
            }
            filtered.push_back(&cmd);
        }

        backend::IRenderDevice::PassSubmitInfo info;
        info.camera       = toBackendCamera(tf, cam, w, h);
        info.clearEnabled = cam.clear;
        info.clearColor   = cam.clearColor;
        dev.submitPass(info, filtered);
    }
}

} // namespace engine
