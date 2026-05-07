#include "RenderSystem.h"
#include "UISystem.h"
#include "../runtime/EngineContext.h"
#include "../components/RenderComponents.h"
#include "../components/TextComponent.h"
#include "../components/ParticleComponent.h"
#include "../components/AnimatorComponent.h"
#include "../components/LightComponents.h"
#include "../../backend/renderer/CommandBuffer.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../core/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
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
    if (auto* p = std::get_if<backend::PushScissorCmd>(&cmd)) return p->pass;
    if (auto* q = std::get_if<backend::PopScissorCmd>(&cmd))  return q->pass;
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

backend::IRenderDevice::Lighting2DParams buildLighting2DParams(EngineContext& ctx,
                                                               const backend::CameraData& camera,
                                                               uint32_t viewportW,
                                                               uint32_t viewportH) {
    backend::IRenderDevice::Lighting2DParams out{};
    out.camera = camera;
    out.viewportW = viewportW;
    out.viewportH = viewportH;

    // L3 uploads every visible point light. Culling by screen tile is now a GPU
    // responsibility; the CPU side only filters obviously invalid lights and
    // copies stable ECS data into the backend-facing POD layout.
    auto lightView = ctx.world.view<Transform, Light2D>();
    for (auto [ent, tf, light] : lightView.each()) {
        (void)ent;
        if (!light.visible || light.radius <= 0.f) continue;
        if (light.type != Light2DType::Point) continue;
        if ((light.layerMask & renderPassBit(RenderPass::World)) == 0u) continue;
        backend::IRenderDevice::Light2DPoint gpu{};
        gpu.x = tf.x;
        gpu.y = tf.y;
        gpu.radius = light.radius;
        gpu.intensity = light.intensity;
        gpu.colorR = light.color.r / 255.f;
        gpu.colorG = light.color.g / 255.f;
        gpu.colorB = light.color.b / 255.f;
        gpu.colorA = light.color.a / 255.f;
        gpu.softness = light.softness;
        gpu.layerMask = light.layerMask;
        gpu.castsShadow = light.castsShadow ? 1u : 0u;
        out.lights.push_back(gpu);
    }

    auto pushSegment = [&](float ax, float ay, float bx, float by, float opacity) {
        backend::IRenderDevice::Light2DSegment seg{};
        seg.ax = ax;
        seg.ay = ay;
        seg.bx = bx;
        seg.by = by;
        seg.opacity = opacity;
        out.segments.push_back(seg);
    };

    auto occView = ctx.world.view<Transform, LightOccluder2D>();
    for (auto [ent, tf, occ] : occView.each()) {
        (void)ent;
        if (!occ.castsShadow || occ.opacity <= 0.f) continue;

        if (occ.shape == LightOccluder2D::Shape::Segment) {
            pushSegment(tf.x + occ.ax, tf.y + occ.ay,
                        tf.x + occ.bx, tf.y + occ.by,
                        occ.opacity);
        } else {
            // AABB is treated as centered on Transform. That matches demo3's
            // debug rectangles and keeps the first GPU data path unambiguous.
            const float hw = occ.width * 0.5f;
            const float hh = occ.height * 0.5f;
            const float x0 = tf.x - hw;
            const float y0 = tf.y - hh;
            const float x1 = tf.x + hw;
            const float y1 = tf.y + hh;
            pushSegment(x0, y0, x1, y0, occ.opacity);
            pushSegment(x1, y0, x1, y1, occ.opacity);
            pushSegment(x1, y1, x0, y1, occ.opacity);
            pushSegment(x0, y1, x0, y0, occ.opacity);
        }
    }

    // L5 reflection handoff. Reflector2D is uploaded as explicit geometry
    // instead of inferred from sprite color, so wet streets/water are opt-in
    // and UI/Text cannot accidentally become reflective. The backend shader
    // treats AABB as a rectangular wet patch and Segment as a thin water edge
    // or mirror line; both are in world pixels like lights and occluders.
    auto reflView = ctx.world.view<Transform, Reflector2D>();
    for (auto [ent, tf, refl] : reflView.each()) {
        (void)ent;
        if (!refl.visible || refl.reflectivity <= 0.f) continue;

        backend::IRenderDevice::Reflector2DRegion gpu{};
        gpu.ax = tf.x + refl.ax;
        gpu.ay = tf.y + refl.ay;
        gpu.bx = tf.x + refl.bx;
        gpu.by = tf.y + refl.by;
        gpu.width = refl.width;
        gpu.height = refl.height;
        gpu.reflectivity = refl.reflectivity;
        gpu.roughness = refl.roughness;
        gpu.tintR = refl.tint.r / 255.f;
        gpu.tintG = refl.tint.g / 255.f;
        gpu.tintB = refl.tint.b / 255.f;
        gpu.tintA = refl.tint.a / 255.f;
        gpu.shape = (refl.shape == Reflector2D::Shape::AABB) ? 1u : 0u;
        gpu.visible = refl.visible ? 1u : 0u;
        out.reflectors.push_back(gpu);
    }

    out.enabled = !out.lights.empty();
    // L4 environment handoff: RenderSystem still does not decide how lighting
    // should look, but it does select the active scene Environment2D and copy
    // its scalar values into the backend POD. The SDL_GPU composite can then
    // tint the low-light areas, apply exposure, and keep the night scene from
    // collapsing to pure black without reaching back into ECS.
    auto envView = ctx.world.view<Environment2D>();
    for (auto [ent, env] : envView.each()) {
        (void)ent;
        if (!env.enabled) continue;
        out.ambientR = env.ambientColor.r / 255.f;
        out.ambientG = env.ambientColor.g / 255.f;
        out.ambientB = env.ambientColor.b / 255.f;
        out.ambientA = env.ambientColor.a / 255.f;
        out.ambientIntensity = env.ambientIntensity;
        out.exposure = env.exposure;
        out.wetness = env.wetness;
        break;
    }
    return out;
}

}

RenderSystem::RenderSystem(EngineContext& ctx) 
    : ctx_(ctx) {
}

RenderSystem::~RenderSystem() = default;

void RenderSystem::init() {
    spriteBuffer_.init(&ctx_.renderDevice(), SpriteBuffer::INITIAL_CAPACITY);
    gpuRenderer_.init(&ctx_.renderDevice());
    ensureMixedGPUCapacity(SpriteBuffer::INITIAL_CAPACITY);
    particleRenderer_.init(&ctx_.renderDevice());

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

void RenderSystem::update(float dt) {
    if (!ctx_.renderToSwapchain) {
        return;
    }
    lastDt_ = dt;
    syncParticleEmitters(dt);
    syncEntitiesToGPU();
    spriteBuffer_.advanceFrame();
    spriteBuffer_.uploadDirty();

    backend::IRenderDevice& dev = ctx_.renderDevice();
    backend::RenderFrameStats& stats = dev.mutableFrameStats();
    stats.spriteCount = spriteBuffer_.activeCount();
    // Phase L0/L1 of the 2D lighting plan: RenderSystem deliberately only
    // observes the lighting ECS data here. The actual Vulkan compute pass will
    // consume the same components later, but these counters already prove that
    // scenes, prefabs, and demos can feed stable light/occluder/reflector data.
    stats.light2DCount = static_cast<uint32_t>(ctx_.world.view<Light2D>().size());
    stats.occluder2DCount = static_cast<uint32_t>(ctx_.world.view<LightOccluder2D>().size());
    stats.reflector2DCount = static_cast<uint32_t>(ctx_.world.view<Reflector2D>().size());
    stats.environment2DCount = static_cast<uint32_t>(ctx_.world.view<Environment2D>().size());

    // Route selection is capability driven. The CPU-batch path remains a
    // correctness fallback; all performance work should make this branch less
    // frequent rather than faster.
    const backend::RendererCapabilities& caps = dev.capabilities();
    const bool canUseGPUDriven =
        caps.supportsGPUDrivenSprite &&
        gpuRenderer_.isInitialized() &&
        gpuRenderer_.hasCullingPipeline();

    if (canUseGPUDriven) {
        stats.path = backend::RenderPath::SDLGPU_GPUDriven;
        stats.fallbackReason = nullptr;
        static bool logged = false;
        if (!logged) { core::logInfo("[GPU-driven] using GPU-driven path"); logged = true; }
        buildCommandBufferGPUDriven();
    } else {
        stats.path = caps.supportsStorageBuffer
            ? backend::RenderPath::SDLGPU_CPUBatch
            : backend::RenderPath::OpenGL_CPUBatch;
        if (!caps.supportsGPUDrivenSprite) {
            stats.fallbackReason = "gpu-driven sprite pipeline unavailable";
        } else if (!gpuRenderer_.isInitialized()) {
            stats.fallbackReason = "gpu-driven renderer not initialized";
        } else if (!gpuRenderer_.hasCullingPipeline()) {
            stats.fallbackReason = "compute culling pipeline unavailable";
        } else {
            stats.fallbackReason = "gpu-driven path disabled";
        }
        buildCommandBuffer();
    }
}

void RenderSystem::shutdown() {
    destroyConnection_.release();
    transformUpdateConnection_.release();
    particleRenderer_.shutdown();
    gpuRenderer_.shutdown();
    if (mixedGpuSpriteBuffer_.valid()) {
        ctx_.renderDevice().destroyBuffer(mixedGpuSpriteBuffer_);
        mixedGpuSpriteBuffer_ = {};
        mixedGpuSpriteCapacity_ = 0;
    }
    spriteBuffer_.shutdown();
}

void RenderSystem::syncParticleEmitters(float dt) {
    if (!particleRenderer_.isInitialized()) return;
    if (!ctx_.renderDevice().capabilities().supportsCompute) return;

    auto view = ctx_.world.view<Transform, ParticleComponent>();
    for (auto [ent, tf, pc] : view.each()) {
        (void)ent;
        particleRenderer_.emit(pc, tf, dt, ctx_.renderDevice());
    }
}

std::vector<backend::IRenderDevice::GPUParticleParams>
RenderSystem::collectParticleParams(const Transform& tf, const Camera& cam,
                                    int viewportW, int viewportH, float dt) {
    std::vector<backend::IRenderDevice::GPUParticleParams> out;
    if (!particleRenderer_.isInitialized() ||
        !particleRenderer_.hasUpdatePipeline() ||
        !particleRenderer_.hasSortPipeline()) return out;
    if (!ctx_.renderDevice().capabilities().supportsCompute) return out;

    backend::CameraData camera = toBackendCamera(tf, cam, viewportW, viewportH);

    auto view = ctx_.world.view<ParticleComponent>();
    for (auto [ent, pc] : view.each()) {
        (void)ent;
        if (!pc.visible || !pc.texture.valid()) continue;
        if (pc.gpuOffset == 0xFFFFFFFFu || pc.gpuCount == 0) continue;
        if ((cam.layerMask & renderPassBit(pc.pass)) == 0) continue;

        backend::IRenderDevice::GPUParticleParams params;
        params.updatePipeline = particleRenderer_.updatePipeline();
        params.sortPipeline = particleRenderer_.sortPipeline();
        params.particleBuffer = particleRenderer_.particleBuffer();
        params.aliveIndexBuffer = particleRenderer_.aliveIndexBuffer();
        params.indirectArgsBuffer = particleRenderer_.indirectArgsBuffer();
        params.texture = pc.texture;
        params.firstParticle = pc.gpuOffset;
        params.particleCount = pc.gpuCount;
        params.dt = std::max(0.f, dt);
        params.camera = camera;
        params.clearEnabled = false;
        params.clearColor = cam.clearColor;
        out.push_back(params);
    }
    return out;
}

void RenderSystem::submitParticlePass(const Transform& tf, const Camera& cam,
                                      int viewportW, int viewportH, float dt) {
    backend::IRenderDevice& dev = ctx_.renderDevice();
    backend::CameraData camera = toBackendCamera(tf, cam, viewportW, viewportH);
    auto particles = collectParticleParams(tf, cam, viewportW, viewportH, dt);
    for (const auto& params : particles) {
        dev.submitGPUParticlePass({ camera, false, cam.clearColor }, params);
    }
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

void RenderSystem::ensureMixedGPUCapacity(uint32_t required) {
    if (required <= mixedGpuSpriteCapacity_ && mixedGpuSpriteBuffer_.valid()) return;

    uint32_t newCapacity = mixedGpuSpriteCapacity_ > 0
        ? mixedGpuSpriteCapacity_
        : SpriteBuffer::INITIAL_CAPACITY;
    while (newCapacity < required) {
        newCapacity *= 2;
    }

    if (mixedGpuSpriteBuffer_.valid()) {
        ctx_.renderDevice().destroyBuffer(mixedGpuSpriteBuffer_);
    }

    backend::BufferDesc desc{};
    desc.size = newCapacity * sizeof(GPUSprite);
    desc.usage = backend::BufferUsage::Storage | backend::BufferUsage::Vertex;
    mixedGpuSpriteBuffer_ = ctx_.renderDevice().createBuffer(desc);
    mixedGpuSpriteCapacity_ = mixedGpuSpriteBuffer_.valid() ? newCapacity : 0;

    // Recreating the mixed buffer invalidates the static tile data that lived
    // in it. The CPU-side cache remains valid, but its GPU copy must be
    // uploaded again before the next frame graph submission.
    tileGpuCacheSignature_ = 0;
}

void RenderSystem::syncSpritesToMixedGPUBuffer() {
    if (!mixedGpuSpriteBuffer_.valid()) return;

    // The mixed GPU buffer is the single storage buffer consumed by the
    // GPU-driven shader. SpriteBuffer still owns sprite lifetime and dirty
    // bookkeeping for the legacy path, so this bridge mirrors active sprite
    // slots into the same indices in the mixed buffer before sorting references
    // those indices.
    auto view = ctx_.world.view<Sprite>();
    for (auto [ent, spr] : view.each()) {
        (void)ent;
        if (!spr.gpuHandle.valid()) continue;

        const GPUSprite* slot = spriteBuffer_.getSlot(spr.gpuHandle);
        if (!slot) continue;

        ctx_.renderDevice().uploadToBuffer(mixedGpuSpriteBuffer_,
                                           slot,
                                           sizeof(GPUSprite),
                                           spr.gpuHandle.index * sizeof(GPUSprite));
    }
}

uint64_t RenderSystem::computeTileGPUCacheSignature() const {
    // FNV-1a over every field that changes tile geometry, sorting, collision
    // independence, or texture selection. This keeps the cache deterministic:
    // a tilemap edit or transform move rebuilds the GPU tile instances, while
    // unrelated per-frame work leaves the static upload alone.
    uint64_t h = 1469598103934665603ull;
    auto mixU64 = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    auto mixI32 = [&](int v) { mixU64(static_cast<uint32_t>(v)); };
    auto mixF32 = [&](float v) {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(v), "float hash assumes 32-bit float");
        std::memcpy(&bits, &v, sizeof(bits));
        mixU64(bits);
    };

    auto tileView = ctx_.world.view<Transform, TileMap>();
    for (auto [ent, tf, tmap] : tileView.each()) {
        mixU64(static_cast<uint64_t>(entt::to_integral(ent)));
        mixF32(tf.x);
        mixF32(tf.y);
        mixF32(tf.rotation);
        mixF32(tf.scaleX);
        mixF32(tf.scaleY);
        mixI32(tmap.width);
        mixI32(tmap.height);
        mixI32(tmap.tileSize);
        mixU64(tmap.tilesets.size());
        for (const auto& ts : tmap.tilesets) {
            mixU64(ts.texture.index);
            mixU64(ts.texture.version);
            mixI32(ts.firstGid);
            mixI32(ts.count);
            mixI32(ts.columns);
        }
        mixU64(tmap.layers.size());
        for (const auto& layer : tmap.layers) {
            mixU64(layer.visible ? 1u : 0u);
            mixU64(layer.collidable ? 1u : 0u);
            mixI32(layer.renderLayer);
            mixU64(layer.tiles.size());
            for (int gid : layer.tiles) {
                mixI32(gid);
            }
        }
    }
    return h;
}

void RenderSystem::rebuildTileGPUCacheIfNeeded() {
    const uint32_t spriteCapacity = spriteBuffer_.capacity();
    const uint64_t signature = computeTileGPUCacheSignature();
    const uint32_t desiredBase = spriteCapacity;
    if (signature == tileGpuCacheSignature_ && desiredBase == tileGpuBaseIndex_) {
        return;
    }

    tileGpuBaseIndex_ = desiredBase;
    tileGpuInstances_.clear();
    tileGpuItems_.clear();

    int seq = 0;
    auto tileView = ctx_.world.view<Transform, TileMap>();
    for (auto [ent, tf, tmap] : tileView.each()) {
        (void)ent;
        if (tmap.tileSize <= 0) continue;

        for (int layerIndex = 0; layerIndex < static_cast<int>(tmap.layers.size()); ++layerIndex) {
            const auto& tileLayer = tmap.layers[static_cast<size_t>(layerIndex)];
            if (!tileLayer.visible) continue;

            for (int y = 0; y < tmap.height; ++y) {
                for (int x = 0; x < tmap.width; ++x) {
                    const int gid = tmap.tileAt(layerIndex, x, y);
                    const TileMap::Tileset* tileset = tmap.tilesetForGid(gid);
                    const int tileId = tmap.localTileId(gid);
                    if (!tileset || !tileset->texture.valid() || tileId < 0) continue;

                    int texW = 1, texH = 1;
                    ctx_.renderDevice().getTextureDimensions(tileset->texture, texW, texH);
                    const int columns = std::max(1, tileset->columns);
                    const int sx = (tileId % columns) * tmap.tileSize;
                    const int sy = (tileId / columns) * tmap.tileSize;
                    const float worldX = tf.x + static_cast<float>(x * tmap.tileSize);
                    const float worldY = tf.y + static_cast<float>(y * tmap.tileSize);
                    const float centerX = worldX + tmap.tileSize * 0.5f;
                    const float centerY = worldY + tmap.tileSize * 0.5f;

                    GPUSprite gpu{};
                    buildTransform2D(gpu.transform,
                                     centerX,
                                     centerY,
                                     0.f,
                                     1.f,
                                     1.f,
                                     0.5f,
                                     0.5f,
                                     static_cast<float>(tmap.tileSize),
                                     static_cast<float>(tmap.tileSize));
                    packColor(gpu.color, 255, 255, 255, 255);
                    packUV(gpu.uv,
                           sx / static_cast<float>(texW),
                           sy / static_cast<float>(texH),
                           (sx + tmap.tileSize) / static_cast<float>(texW),
                           (sy + tmap.tileSize) / static_cast<float>(texH));
                    gpu.textureIndex = tileset->texture.index;
                    gpu.layer = static_cast<uint32_t>(tileLayer.renderLayer);
                    gpu.sortKey = 0;
                    gpu.flags = 1u | (static_cast<uint32_t>(RenderPass::World) << 1);

                    CachedGPUTile item{};
                    item.texture = tileset->texture;
                    item.gpuIndex = tileGpuBaseIndex_ + static_cast<uint32_t>(tileGpuInstances_.size());
                    item.layer = tileLayer.renderLayer;
                    item.ySort = true;
                    item.y = worldY;
                    item.sortKey = 0;
                    item.seq = seq++;
                    item.centerX = centerX;
                    item.centerY = centerY;
                    item.halfW = tmap.tileSize * 0.5f;
                    item.halfH = tmap.tileSize * 0.5f;

                    tileGpuInstances_.push_back(gpu);
                    tileGpuItems_.push_back(item);
                }
            }
        }
    }

    ensureMixedGPUCapacity(tileGpuBaseIndex_ + static_cast<uint32_t>(tileGpuInstances_.size()));
    if (mixedGpuSpriteBuffer_.valid() && !tileGpuInstances_.empty()) {
        ctx_.renderDevice().uploadToBuffer(mixedGpuSpriteBuffer_,
                                           tileGpuInstances_.data(),
                                           tileGpuInstances_.size() * sizeof(GPUSprite),
                                           tileGpuBaseIndex_ * sizeof(GPUSprite));
    }
    tileGpuCacheSignature_ = signature;
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
        for (int layer = 0; layer < static_cast<int>(tmap.layers.size()); ++layer) {
            const auto& tileLayer = tmap.layers[static_cast<size_t>(layer)];
            if (!tileLayer.visible) continue;
            for (int y = 0; y < tmap.height; ++y) {
                for (int x = 0; x < tmap.width; ++x) {
                    int gid = tmap.tileAt(layer, x, y);
                    const TileMap::Tileset* tileset = tmap.tilesetForGid(gid);
                    int tileId = tmap.localTileId(gid);
                    if (!tileset || !tileset->texture.valid() || tileId < 0) continue;
                    Drawable d{};
                    d.pass    = RenderPass::World;
                    d.layer   = tileLayer.renderLayer;
                    d.ySort   = true;
                    d.y       = tf.y + static_cast<float>(y * tmap.tileSize);
                    d.sortKey = 0;
                    d.seq     = seq++;
                    d.kind    = DrawKind::Tile;
                    d.tile.tileset  = tileset->texture;
                    d.tile.tileId   = tileId;
                    d.tile.gridX    = static_cast<int>(tf.x) + x;
                    d.tile.gridY    = static_cast<int>(tf.y) + y;
                    d.tile.tileSize = tmap.tileSize;
                    d.tile.layer    = tileLayer.renderLayer;
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
        if (!sprite.visible) continue;
        const AnimatorOutput* aout = ctx.world.try_get<AnimatorOutput>(ent);

        Drawable d{};
        d.pass    = sprite.pass;
        d.layer   = sprite.layer;
        d.ySort   = sprite.ySort;
        d.y       = tf.y + (aout ? aout->offsetY : 0.f);
        d.sortKey = sprite.sortOrder;
        d.seq     = seq++;
        d.kind    = DrawKind::Sprite;
        auto& s = d.sprite;
        s.texture  = sprite.texture;
        s.x        = tf.x + (aout ? aout->offsetX : 0.f);
        s.y        = tf.y + (aout ? aout->offsetY : 0.f);
        s.rotation = tf.rotation + (aout ? aout->rotationOffset : 0.f);
        s.scaleX   = tf.scaleX * (aout ? aout->scaleMulX : 1.f);
        s.scaleY   = tf.scaleY * (aout ? aout->scaleMulY : 1.f);
        s.pivotX   = sprite.pivotX;
        s.pivotY   = sprite.pivotY;
        s.srcRect  = sprite.srcRect;
        s.layer    = sprite.layer;
        s.sortKey  = sprite.sortOrder;
        s.ySort    = sprite.ySort;
        s.pass     = sprite.pass;

        if (aout) {
            int r = sprite.tint.r + aout->tintMul.r;
            if (r > 255) r = 255;
            s.tint.r = static_cast<uint8_t>(r);
            s.tint.g = sprite.tint.g;
            s.tint.b = sprite.tint.b;
            s.tint.a = sprite.tint.a;
        } else {
            s.tint = sprite.tint;
        }

        // Region tint：Tinting 组件 + sibling .id.png 都存在才生效。
        if (const Tinting* tnt = ctx.world.try_get<Tinting>(ent)) {
            TextureHandle regionTex = ctx.assetManager.regionIdTexture(sprite.texture);
            if (regionTex.valid()) {
                s.hasRegion = true;
                s.regionTex = regionTex;
                for (int i = 0; i < Tinting::MAX_REGIONS; ++i) {
                    s.regionTints[i] = tnt->slots[i].enabled
                        ? tnt->slots[i].color
                        : core::Color::White;  // passthrough = 与 White 相乘
                }
            }
        }
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

    // UI 命令直接附加到 cb 末尾（pass=Screen，按 layerMask 过滤到 UI 相机）。
    if (ctx.systems.has<UISystem>()) {
        ctx.systems.get<UISystem>().emitDrawCommands(cb);
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

    backend::IRenderDevice::FrameGraphSubmitInfo frameGraph;
    frameGraph.cameraPasses.reserve(cameras.size());

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
            if (std::holds_alternative<backend::DrawSpriteCmd>(cmd)) {
                dev.mutableFrameStats().visibleSpriteCount++;
            }
            filtered.push_back(&cmd);
        }

        backend::IRenderDevice::PassSubmitInfo info;
        info.camera       = toBackendCamera(tf, cam, w, h);
        info.clearEnabled = cam.clear;
        info.clearColor   = cam.clearColor;
        const auto particles = collectParticleParams(tf, cam, w, h, (i == 0) ? lastDt_ : 0.f);
        if ((cam.layerMask & renderPassBit(RenderPass::World)) != 0) {
            auto lighting = buildLighting2DParams(ctx_, info.camera,
                                                  static_cast<uint32_t>(w),
                                                  static_cast<uint32_t>(h));
            if (ctx_.renderDevice().capabilities().supportsLighting2D &&
                !lighting.lights.empty()) {
                std::vector<const backend::RenderCmd*> worldCmdPtrs;
                std::vector<const backend::RenderCmd*> uiCmdPtrs;
                worldCmdPtrs.clear();
                uiCmdPtrs.clear();
                worldCmdPtrs.reserve(filtered.size());
                uiCmdPtrs.reserve(filtered.size());
                for (const backend::RenderCmd* cmd : filtered) {
                    if (cmdPass(*cmd) == RenderPass::World) {
                        worldCmdPtrs.push_back(cmd);
                    } else {
                        uiCmdPtrs.push_back(cmd);
                    }
                }

                backend::IRenderDevice::FrameGraphCameraPass node;
                node.kind = backend::IRenderDevice::FrameGraphPassKind::WorldLighting;
                node.debugName = "CameraWorldLighting";
                node.worldLighting.worldPass = info;
                node.worldLighting.worldCommands = std::move(worldCmdPtrs);
                node.worldLighting.particles = particles;
                node.worldLighting.uiCommands = std::move(uiCmdPtrs);
                node.worldLighting.lighting = std::move(lighting);
                frameGraph.cameraPasses.push_back(std::move(node));
            } else {
                backend::IRenderDevice::FrameGraphCameraPass node;
                node.kind = backend::IRenderDevice::FrameGraphPassKind::Raster;
                node.debugName = "CameraRaster";
                node.pass = info;
                node.commands = filtered;
                node.particles = particles;
                frameGraph.cameraPasses.push_back(std::move(node));
            }
        } else {
            backend::IRenderDevice::FrameGraphCameraPass node;
            node.kind = backend::IRenderDevice::FrameGraphPassKind::Raster;
            node.debugName = "CameraRaster";
            node.pass = info;
            node.commands = filtered;
            node.particles = particles;
            frameGraph.cameraPasses.push_back(std::move(node));
        }
    }
    dev.submitFrameGraph(frameGraph);
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

    backend::IRenderDevice::FrameGraphSubmitInfo frameGraph;
    frameGraph.cameraPasses.reserve(cameras.size());

    rebuildTileGPUCacheIfNeeded();
    syncSpritesToMixedGPUBuffer();

    uint32_t spriteCount = spriteBuffer_.activeCount();
    auto spriteView = ctx_.world.view<Transform, Sprite>();

    static std::vector<Drawable> cpuDrawables;
    cpuDrawables.clear();
    int seq = 0;

    // 收集 Text
    auto textView = ctx_.world.view<Transform, TextComponent>();
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
        cpuDrawables.push_back(d);
    }

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

        struct GPUVisible {
            TextureHandle texture;
            uint32_t gpuIndex = 0;
            RenderPass pass = RenderPass::World;
            int layer = 0;
            bool ySort = false;
            float y = 0.f;
            int sortKey = 0;
            int seq = 0;
        };

        std::vector<GPUVisible> gpuDrawables;
        gpuDrawables.reserve(spriteCount + tileGpuItems_.size());

        for (const CachedGPUTile& tile : tileGpuItems_) {
            if ((cam.layerMask & renderPassBit(RenderPass::World)) == 0) continue;
            if (cam.cullEnabled) {
                if (tile.centerX + tile.halfW < viewMinX || tile.centerX - tile.halfW > viewMaxX ||
                    tile.centerY + tile.halfH < viewMinY || tile.centerY - tile.halfH > viewMaxY) {
                    continue;
                }
            }
            gpuDrawables.push_back(GPUVisible{
                tile.texture,
                tile.gpuIndex,
                RenderPass::World,
                tile.layer,
                tile.ySort,
                tile.y,
                tile.sortKey,
                tile.seq
            });
        }

        // Sprite and TileMap entries share one mixed GPU buffer. Sprite entries
        // keep their SpriteBuffer slot index; TileMap entries use the cached
        // range starting at tileGpuBaseIndex_.
        for (auto [ent, eTf, spr] : spriteView.each()) {
            if (!spr.visible) continue;
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

            gpuDrawables.push_back(GPUVisible{
                spr.texture,
                spr.gpuHandle.index,
                spr.pass,
                spr.layer,
                spr.ySort,
                eTf.y,
                spr.sortOrder,
                seq++
            });
        }

        std::sort(gpuDrawables.begin(), gpuDrawables.end(),
                  [](const GPUVisible& A, const GPUVisible& B) {
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
                  });
        if (gpuDrawables.size() > GPUDrivenRenderer::MAX_VISIBLE_SPRITES) {
            gpuDrawables.resize(GPUDrivenRenderer::MAX_VISIBLE_SPRITES);
        }

        const uint32_t visibleCount = static_cast<uint32_t>(gpuDrawables.size());
        dev.mutableFrameStats().visibleSpriteCount += visibleCount;
        ensureMixedGPUCapacity(std::max<uint32_t>(
            tileGpuBaseIndex_ + static_cast<uint32_t>(tileGpuInstances_.size()),
            spriteBuffer_.capacity()));

        std::vector<uint32_t> visibleIndices(visibleCount);
        std::vector<backend::IRenderDevice::GPUDrawBatch> batches;
        batches.reserve(8);

        for (uint32_t k = 0; k < visibleCount; ++k) {
            visibleIndices[k] = gpuDrawables[k].gpuIndex;
            const TextureHandle tex = gpuDrawables[k].texture;
            if (batches.empty() || !(batches.back().texture == tex)) {
                batches.push_back({ tex, k, 1 });
            } else {
                batches.back().instanceCount++;
            }
        }

        backend::IRenderDevice::PassSubmitInfo info;
        info.camera       = toBackendCamera(tf, cam, w, h);
        info.clearEnabled = cam.clear;
        info.clearColor   = cam.clearColor;

        // ========== Step 1: GPU 渲染 Sprite ==========
        backend::IRenderDevice::GPURenderParams params;
        params.spriteBuffer       = mixedGpuSpriteBuffer_;
        params.visibleIndexBuffer = gpuRenderer_.getVisibleIndexBuffer();
        params.spriteCount        = visibleCount;
        params.visibleCount       = visibleCount;
        params.batches            = std::move(batches);
        params.ownedVisibleIndices = std::move(visibleIndices);
        params.camera             = info.camera;
        params.clearEnabled       = false;  // 只在第一次清除
        params.clearColor         = info.clearColor;
        
        // 第一个相机清除，后续不清除
        if (i == 0) {
            params.clearEnabled = info.clearEnabled;
        }
        
        // Submission is intentionally delayed until CPU world drawables below
        // are collected. The graph path needs GPU sprites, tile/text commands,
        // and particles as inputs to one WorldColor pass before lighting
        // composite. Non-lighting scenes still use the old direct submission.
        const auto particles = collectParticleParams(tf, cam, w, h, (i == 0) ? lastDt_ : 0.f);

        // ========== Step 2: CPU 渲染 Text / Tile / UI (叠加在 Sprite 之上) ==========
        static std::vector<backend::RenderCmd> overlayCommands;
        overlayCommands.clear();

        if (!cpuDrawables.empty()) {
            for (const Drawable& d : cpuDrawables) {
                // 检查 Pass 是否匹配
                if ((cam.layerMask & renderPassBit(d.pass)) == 0) continue;
                
                // 视口裁剪 (仅 World pass)
                if (cam.cullEnabled && d.pass == RenderPass::World) {
                    if (d.kind == DrawKind::Tile) {
                        float ts = static_cast<float>(d.tile.tileSize);
                        float cx = d.tile.gridX * ts + ts * 0.5f;
                        float cy = d.tile.gridY * ts + ts * 0.5f;
                        if (cx + ts < viewMinX || cx - ts > viewMaxX ||
                            cy + ts < viewMinY || cy - ts > viewMaxY) {
                            continue;
                        }
                    }
                    // 文字暂不裁剪
                }
                
                // 转换为 RenderCmd 并存储
                if (d.kind == DrawKind::Tile) {
                    backend::RenderCmd cmd = d.tile;  // 复制到 vector
                    overlayCommands.push_back(cmd);
                } else if (d.kind == DrawKind::Text) {
                    backend::RenderCmd cmd = d.text;  // 复制到 vector
                    overlayCommands.push_back(cmd);
                }
            }
        }

        // 注入 UI 命令（pass=Screen，按本相机 layerMask 过滤）。纯 UI 场景没有
        // cpuDrawables，也必须提交这一批命令。
        if (ctx_.systems.has<UISystem>()) {
            std::vector<const backend::RenderCmd*> uiPtrs;
            ctx_.systems.get<UISystem>().appendDrawCommandPtrs(uiPtrs);
            for (const backend::RenderCmd* p : uiPtrs) {
                const RenderPass pp = cmdPass(*p);
                if ((cam.layerMask & renderPassBit(pp)) == 0) continue;
                overlayCommands.push_back(*p);
            }
        }

        bool usesWorldLightingGraph = false;
        if ((cam.layerMask & renderPassBit(RenderPass::World)) != 0) {
            auto lighting = buildLighting2DParams(ctx_, info.camera,
                                                  static_cast<uint32_t>(w),
                                                  static_cast<uint32_t>(h));
            if (dev.capabilities().supportsLighting2D && !lighting.lights.empty()) {
                backend::IRenderDevice::FrameGraphCameraPass node;
                node.kind = backend::IRenderDevice::FrameGraphPassKind::WorldLighting;
                node.debugName = "CameraGPUWorldLighting";
                node.ownedCommands = std::move(overlayCommands);
                node.worldLighting.worldPass = info;
                node.worldLighting.hasGPUWorld = true;
                node.worldLighting.gpuWorld = std::move(params);
                node.worldLighting.particles = particles;
                node.worldLighting.lighting = std::move(lighting);
                node.worldLighting.worldCommands.reserve(node.ownedCommands.size());
                node.worldLighting.uiCommands.reserve(node.ownedCommands.size());
                for (const auto& cmd : node.ownedCommands) {
                    if (cmdPass(cmd) == RenderPass::World) {
                        node.worldLighting.worldCommands.push_back(&cmd);
                    } else {
                        node.worldLighting.uiCommands.push_back(&cmd);
                    }
                }

                frameGraph.cameraPasses.push_back(std::move(node));
                usesWorldLightingGraph = true;
            }
        }

        if (!usesWorldLightingGraph) {
            backend::IRenderDevice::FrameGraphCameraPass node;
            node.kind = backend::IRenderDevice::FrameGraphPassKind::GPUDriven;
            node.debugName = "CameraGPUDriven";
            node.pass = info;
            node.gpu = std::move(params);
            node.particles = particles;
            node.ownedCommands = std::move(overlayCommands);
            node.commands.reserve(node.ownedCommands.size());
            for (const auto& cmd : node.ownedCommands) {
                node.commands.push_back(&cmd);
            }
            frameGraph.cameraPasses.push_back(std::move(node));
        }
    }
    dev.submitFrameGraph(frameGraph);
}

}
