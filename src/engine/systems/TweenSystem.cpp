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

    // 收集本帧需要触发的 onComplete，待 ECS 遍历结束后再统一调用。
    // 原因：回调里业务可能 emplace/erase TweenComponent 或 push_back 新
    // TweenInstance，正在 view 遍历或 vector 遍历中执行会破坏迭代器/触发
    // realloc。延后到所有遍历结束、erase 收缩完成之后再调，安全且不影响
    // 用户感知 (callback 仍发生在"动画完成的这一帧"内)。
    std::vector<std::function<void()>> pendingComplete;

    auto view = w.view<TweenComponent>();
    for (auto [ent, tc] : view.each()) {
        for (auto& t : tc.instances) {
            if (t.finished || t.duration <= 0.f) continue;

            // ── 延迟期 (语义 B) ─────────────────────────────────────────────
            // 把 outValue 钉在 from、按 from 写回目标，但不推进 elapsed。
            // 这样 fade-in 类动画在 delay 期间字段就是 0，时间到才开始上升。
            if (t.delay > 0.f) {
                t.delay -= dt;
                t.outValue = t.from;
                applyToTarget(w, ent, t.channel, t.outValue);
                if (t.onUpdate) t.onUpdate(t.outValue);
                if (t.delay > 0.f) continue;
                t.delay = 0.f;
                continue;   // 本帧只把字段钉到 from，下一帧再开始推进
            }

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
            if (t.onUpdate) t.onUpdate(t.outValue);

            // 刚刚 finish：把 onComplete 移走 (清空原字段防御重复)，留到
            // 遍历结束后再调。即便 removeOnFinish=false，下一帧由顶部
            // `if (t.finished) continue` 拦掉，回调也不会被再触发。
            if (t.finished && t.onComplete) {
                pendingComplete.emplace_back(std::move(t.onComplete));
            }
        }

        // 清理已完成的（保留 finished 标记一帧后再删，或直接删）
        tc.instances.erase(
            std::remove_if(tc.instances.begin(), tc.instances.end(),
                [](const TweenInstance& t) { return t.finished && t.removeOnFinish; }),
            tc.instances.end());
    }

    // 安全期：所有 view/vector 遍历都已结束，回调里随意改 ECS / 新加 tween。
    for (auto& cb : pendingComplete) cb();
}

} // namespace engine
