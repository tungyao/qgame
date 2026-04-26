#include "TweenSystem.h"
#include "../components/TweenComponent.h"
#include "../components/RenderComponents.h"
#include "../runtime/EngineContext.h"
#include <algorithm>

namespace engine {

namespace {
    inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

    inline uint8_t toByte(float v) {
        if (v < 0.f)   v = 0.f;
        if (v > 255.f) v = 255.f;
        return static_cast<uint8_t>(v + 0.5f);
    }

    void applyToTarget(entt::registry& w, entt::entity ent,
                       TweenChannel ch, float v) {
        switch (ch) {
            case TweenChannel::PositionX:
                { if (auto* tf = w.try_get<Transform>(ent)) tf->x = v; } break;
            case TweenChannel::PositionY:
                { if (auto* tf = w.try_get<Transform>(ent)) tf->y = v; } break;
            case TweenChannel::Rotation:
                { if (auto* tf = w.try_get<Transform>(ent)) tf->rotation = v; } break;
            case TweenChannel::ScaleX:
                { if (auto* tf = w.try_get<Transform>(ent)) tf->scaleX = v; } break;
            case TweenChannel::ScaleY:
                { if (auto* tf = w.try_get<Transform>(ent)) tf->scaleY = v; } break;
            case TweenChannel::SpriteTintR:
                { if (auto* sp = w.try_get<Sprite>(ent)) { sp->tint.r = toByte(v); sp->gpuDirty = true; } } break;
            case TweenChannel::SpriteTintG:
                { if (auto* sp = w.try_get<Sprite>(ent)) { sp->tint.g = toByte(v); sp->gpuDirty = true; } } break;
            case TweenChannel::SpriteTintB:
                { if (auto* sp = w.try_get<Sprite>(ent)) { sp->tint.b = toByte(v); sp->gpuDirty = true; } } break;
            case TweenChannel::SpriteTintA:
                { if (auto* sp = w.try_get<Sprite>(ent)) { sp->tint.a = toByte(v); sp->gpuDirty = true; } } break;
            case TweenChannel::CameraZoom:
                { if (auto* cam = w.try_get<Camera>(ent)) cam->zoom = v; } break;
            case TweenChannel::Custom:
                break;
        }
    }
} // namespace

void TweenSystem::update(float rawDt) {
    const float dt = rawDt * ctx_.timeScale;
    auto& w = ctx_.world;

    auto view = w.view<TweenComponent>();
    for (auto [ent, tc] : view.each()) {
        for (auto& t : tc.instances) {
            if (t.finished || t.duration <= 0.f) continue;

            t.elapsed += dt * (t.pingpong ? static_cast<float>(t.direction) : 1.f);

            // 边界处理
            if (t.elapsed >= t.duration) {
                if (t.pingpong) {
                    t.elapsed = t.duration - (t.elapsed - t.duration);
                    t.direction = -1;
                } else if (t.loop) {
                    t.elapsed = std::fmod(t.elapsed, t.duration);
                } else {
                    t.elapsed = t.duration;
                    t.finished = true;
                }
            } else if (t.elapsed < 0.f) {
                if (t.pingpong) {
                    t.elapsed = -t.elapsed;
                    t.direction = +1;
                    if (!t.loop) t.finished = true;  // pingpong 一回合即完成
                } else {
                    t.elapsed = 0.f;
                }
            }

            const float u = t.duration > 0.f ? (t.elapsed / t.duration) : 1.f;
            const float k = applyEasing(t.easing, u);
            t.outValue = lerp(t.from, t.to, k);
            applyToTarget(w, ent, t.channel, t.outValue);
        }

        // 清理已完成的（保留 finished 标记一帧后再删，或直接删）
        tc.instances.erase(
            std::remove_if(tc.instances.begin(), tc.instances.end(),
                [](const TweenInstance& t) { return t.finished && t.removeOnFinish; }),
            tc.instances.end());
    }
}

} // namespace engine
