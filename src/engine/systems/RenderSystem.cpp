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

RenderPass cmdPass(const backend::RenderCmd& cmd) {
    if (auto* s = std::get_if<backend::DrawSpriteCmd>(&cmd)) return s->pass;
    if (auto* t = std::get_if<backend::DrawTileCmd>(&cmd))   return t->pass;
    if (auto* x = std::get_if<backend::DrawTextCmd>(&cmd))   return x->pass;
    return RenderPass::World;
}

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
    return false;
}

}

RenderSystem::RenderSystem(EngineContext& ctx) 
    : ctx_(ctx) {
}

RenderSystem::~RenderSystem() = default;

void RenderSystem::init() {
    spriteBuffer_.init(&ctx_.renderDevice(), SpriteBuffer::INITIAL_CAPACITY);
    gpuRenderer_.init(&ctx_.renderDevice());

    destroyConnection_ = ctx_.world.on_destroy<Sprite>().connect<&RenderSystem::freeGPUSlot>(this);
    transformUpdateConnection_ = ctx_.world.on_update<Transform>().connect<&RenderSystem::onTransformUpdate>(this);

    auto view = ctx_.world.view<Sprite>();
    for (auto [e, spr] : view.each()) {
        if (!spr.gpuHandle.valid()) {
            allocateGPUSlot(e, spr);
            spr.gpuDirty = true;
        }
    }

    core::logInfo("RenderSystem initialized (S3: Persistent GPU Sprite Buffer + M1/M2 GPU-Driven)");
}

void RenderSystem::update(float /*dt*/) {
    if (!ctx_.renderToSwapchain) {
        return;
    }
    syncEntitiesToGPU();
    spriteBuffer_.advanceFrame();
    spriteBuffer_.uploadDirty();
    
    if (gpuDrivenEnabled_ && gpuRenderer_.isInitialized() && gpuRenderer_.hasCullingPipeline()) {
        static bool logged = false;
        if (!logged) { core::logInfo("[GPU-driven] using GPU-driven path"); logged = true; }
        buildCommandBufferGPUDriven();
    } else {
        if (gpuDrivenEnabled_) {
            static bool warned = false;
            if (!warned) {
                core::logError("[GPU-driven] enabled but falling back: initialized=%d hasCulling=%d",
                               gpuRenderer_.isInitialized(), gpuRenderer_.hasCullingPipeline());
                warned = true;
            }
        }
        buildCommandBuffer();
    }
}

void RenderSystem::shutdown() {
    destroyConnection_.release();
    transformUpdateConnection_.release();
    gpuRenderer_.shutdown();
    spriteBuffer_.shutdown();
}

void RenderSystem::onTransformUpdate(entt::registry& reg, entt::entity e) {
    if (reg.all_of<Sprite>(e)) {
        auto& spr = reg.get<Sprite>(e);
        spr.gpuDirty = true;
    }
}

void RenderSystem::syncEntitiesToGPU() {
    auto view = ctx_.world.view<Transform, Sprite>();
    for (auto [e, tf, spr] : view.each()) {
        if (!spr.gpuHandle.valid()) {
            allocateGPUSlot(e, spr);
            spr.gpuDirty = true;
        }
        // Phase 5.3: 程序化输出每帧重新合成
        const AnimatorOutput* aout = ctx_.world.try_get<AnimatorOutput>(e);
        if (aout) spr.gpuDirty = true;
        if (spr.gpuDirty) {
            updateGPUSlot(tf, spr, aout);
            spr.gpuDirty = false;
        }
    }
}

void RenderSystem::allocateGPUSlot(entt::entity, Sprite& spr) {
    spr.gpuHandle = spriteBuffer_.allocate();
}

void RenderSystem::freeGPUSlot(entt::registry& reg, entt::entity e) {
    if (reg.all_of<Sprite>(e)) {
        auto& spr = reg.get<Sprite>(e);
        if (spr.gpuHandle.valid()) {
            spriteBuffer_.free(spr.gpuHandle);
            spr.gpuHandle = GPUHandle::invalid();
        }
    }
}

void RenderSystem::updateGPUSlot(const Transform& tf, const Sprite& spr, const AnimatorOutput* aout) {
    GPUSprite* slot = spriteBuffer_.getSlot(spr.gpuHandle);
    if (!slot) return;

    float w = spr.srcRect.w;
    float h = spr.srcRect.h;

    // Phase 5.3: 程序化层位移/旋转/缩放叠加
    float px = tf.x, py = tf.y, prot = tf.rotation;
    float psx = tf.scaleX, psy = tf.scaleY;
    if (aout) {
        px   += aout->offsetX;
        py   += aout->offsetY;
        prot += aout->rotationOffset;
        psx  *= aout->scaleMulX;
        psy  *= aout->scaleMulY;
    }
    buildTransform2D(slot->transform, px, py, prot,
                     psx, psy, spr.pivotX, spr.pivotY, w, h);

    // Phase 5.3: 程序化层 tint 加色 (HurtFlash 等)
    int rr = spr.tint.r, gg = spr.tint.g, bb = spr.tint.b, aa = spr.tint.a;
    if (aout) {
        rr += aout->tintMul.r;  // tintMul.r 实为加色偏移 (HurtFlash 写入)
        // tintMul.g/b 此处不参与；HurtFlash 通过 r 通道表达红闪强度
    }
    if (rr > 255) rr = 255;
    slot->color[0] = rr / 255.0f;
    slot->color[1] = gg / 255.0f;
    slot->color[2] = bb / 255.0f;
    slot->color[3] = aa / 255.0f;

    int tw = 1, th = 1;
    ctx_.renderDevice().getTextureDimensions(spr.texture, tw, th);
    slot->uv[0] = spr.srcRect.x / static_cast<float>(tw);
    slot->uv[1] = spr.srcRect.y / static_cast<float>(th);
    slot->uv[2] = (spr.srcRect.x + spr.srcRect.w) / static_cast<float>(tw);
    slot->uv[3] = (spr.srcRect.y + spr.srcRect.h) / static_cast<float>(th);

    slot->textureIndex = spr.texture.index;
    slot->layer        = static_cast<uint32_t>(spr.layer);
    slot->sortKey      = spr.sortOrder;
    slot->flags        = (spr.ySort ? 1u : 0u) | (static_cast<uint32_t>(spr.pass) << 1);

    spriteBuffer_.markDirty(spr.gpuHandle);
}

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

}

void RenderSystem::buildSceneCommands(EngineContext& ctx, backend::CommandBuffer& cb,
                                      int /*viewportW*/, int /*viewportH*/) {
    cb.begin();

    static std::vector<Drawable> drawables;
    drawables.clear();
    int seq = 0;

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

    if (cameras.empty()) {
        backend::IRenderDevice::PassSubmitInfo info;
        info.camera.viewportW = w;
        info.camera.viewportH = h;
        info.clearEnabled = true;
        info.clearColor   = core::Color::Black;
        dev.submitPass(info, {});
        return;
    }

    static std::vector<const backend::RenderCmd*> filtered;
    for (size_t i = 0; i < cameras.size(); ++i) {
        const Transform& tf = *cameras[i].tf;
        const Camera&    cam = *cameras[i].cam;

        const ViewRect vr = computeCameraViewRect(tf, cam, w, h);

        filtered.clear();
        for (const backend::RenderCmd& cmd : cb.commands()) {
            const RenderPass p = cmdPass(cmd);
            if ((cam.layerMask & renderPassBit(p)) == 0) continue;

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

void RenderSystem::buildCommandBufferGPUDriven() {
    const int w = ctx_.window->width();
    const int h = ctx_.window->height();
    
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
    
    if (cameras.empty()) {
        backend::IRenderDevice::PassSubmitInfo info;
        info.camera.viewportW = w;
        info.camera.viewportH = h;
        info.clearEnabled = true;
        info.clearColor   = core::Color::Black;
        dev.submitPass(info, {});
        return;
    }

    uint32_t spriteCount = spriteBuffer_.activeCount();
    auto spriteView = ctx_.world.view<Transform, Sprite>();

    struct Visible {
        uint32_t      gpuIndex;
        TextureHandle texture;
        int           layer;
        bool          ySort;
        float         y;
        int           sortKey;
        int           seq;
    };

    for (size_t i = 0; i < cameras.size(); ++i) {
        const Transform& tf = *cameras[i].tf;
        const Camera&    cam = *cameras[i].cam;

        const float zoom  = (cam.zoom > 0.f) ? cam.zoom : 1.f;
        const float halfW = (w * 0.5f) / zoom;
        const float halfH = (h * 0.5f) / zoom;
        float rx = halfW, ry = halfH;
        if (cam.rotation != 0.f) {
            const float c = std::abs(std::cos(cam.rotation));
            const float s = std::abs(std::sin(cam.rotation));
            rx = halfW * c + halfH * s;
            ry = halfW * s + halfH * c;
        }
        const float viewMinX = tf.x - rx;
        const float viewMinY = tf.y - ry;
        const float viewMaxX = tf.x + rx;
        const float viewMaxY = tf.y + ry;

        std::vector<Visible> visibles;
        visibles.reserve(spriteCount);
        int seq = 0;

        for (auto [ent, eTf, spr] : spriteView.each()) {
            if (!spr.gpuHandle.valid()) continue;
            const GPUSprite* slot = spriteBuffer_.getSlot(spr.gpuHandle);
            if (!slot) continue;

            const uint32_t passBits = (slot->flags >> 1) & 0x7;
            if ((cam.layerMask & (1u << passBits)) == 0) continue;

            if (cam.cullEnabled) {
                const float tx = slot->transform[3];
                const float ty = slot->transform[7];
                const float hw = fabsf(slot->transform[0]) * 0.5f;
                const float hh = fabsf(slot->transform[5]) * 0.5f;
                if (tx + hw < viewMinX || tx - hw > viewMaxX ||
                    ty + hh < viewMinY || ty - hh > viewMaxY) {
                    continue;
                }
            }

            visibles.push_back(Visible{
                spr.gpuHandle.index, spr.texture,
                spr.layer, spr.ySort, eTf.y, spr.sortOrder, seq++
            });
        }

        // 与 CPU 路径 drawableLess 保持一致：layer → ySort → y → sortKey → texture(同 key 内分组减少切换) → seq。
        std::sort(visibles.begin(), visibles.end(),
                  [](const Visible& A, const Visible& B) {
                      if (A.layer != B.layer) return A.layer < B.layer;
                      if (A.ySort != B.ySort) return !A.ySort;
                      if (A.ySort) {
                          int ay = static_cast<int>(A.y);
                          int by = static_cast<int>(B.y);
                          if (ay != by) return ay < by;
                      }
                      if (A.sortKey != B.sortKey) return A.sortKey < B.sortKey;
                      if (A.texture.index != B.texture.index)
                          return A.texture.index < B.texture.index;
                      return A.seq < B.seq;
                  });

        const uint32_t visibleCount = static_cast<uint32_t>(visibles.size());
        std::vector<uint32_t> visibleIndices(visibleCount);
        std::vector<backend::IRenderDevice::GPUDrawBatch> batches;
        batches.reserve(8);

        for (uint32_t k = 0; k < visibleCount; ++k) {
            visibleIndices[k] = visibles[k].gpuIndex;
            const TextureHandle tex = visibles[k].texture;
            if (batches.empty() || !(batches.back().texture == tex)) {
                batches.push_back({ tex, k, 1 });
            } else {
                batches.back().instanceCount++;
            }
        }

        if (visibleCount > 0) {
            BufferHandle visibleBuf = gpuRenderer_.getVisibleIndexBuffer();
            if (visibleBuf.valid()) {
                dev.uploadToBuffer(visibleBuf, visibleIndices.data(),
                                   visibleCount * sizeof(uint32_t), 0);
            }
        }

        backend::IRenderDevice::PassSubmitInfo info;
        info.camera       = toBackendCamera(tf, cam, w, h);
        info.clearEnabled = cam.clear;
        info.clearColor   = cam.clearColor;

        backend::IRenderDevice::GPURenderParams params;
        params.spriteBuffer       = spriteBuffer_.currentBuffer();
        params.visibleIndexBuffer = gpuRenderer_.getVisibleIndexBuffer();
        params.spriteCount        = spriteCount;
        params.visibleCount       = visibleCount;
        params.batches            = std::move(batches);
        dev.submitGPUDrivenPass(info, params);
    }
}

}
