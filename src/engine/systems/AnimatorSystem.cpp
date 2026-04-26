#include "AnimatorSystem.h"
#include "../components/AnimatorComponent.h"
#include "../components/RenderComponents.h"
#include "../runtime/EngineContext.h"
#include "../assets/AssetManager.h"
#include <cmath>

namespace engine {

void AnimatorSystem::init() {}

namespace {
    void scanForward(const AnimationClip& clip, float a, float b, bool inclEnd,
                     AnimationHandle handle, AnimEventQueue& q) {
        for (const auto& e : clip.events) {
            const bool inEnd = inclEnd ? (e.time <= b) : (e.time < b);
            if (e.time >= a && inEnd) {
                q.events.push_back({ e.name, e.intParam, e.floatParam, e.stringParam, handle, e.time });
            }
        }
    }
    void scanBackward(const AnimationClip& clip, float a, float b, bool inclEnd,
                      AnimationHandle handle, AnimEventQueue& q) {
        for (auto it = clip.events.rbegin(); it != clip.events.rend(); ++it) {
            const bool inEnd = inclEnd ? (it->time >= b) : (it->time > b);
            if (it->time <= a && inEnd) {
                q.events.push_back({ it->name, it->intParam, it->floatParam, it->stringParam,
                                     handle, it->time });
            }
        }
    }

    void pushStateEvent(AnimEventQueue& q, const std::string& name, AnimationHandle clip = {}) {
        q.events.push_back({ name, 0, 0.f, "", clip, 0.f });
    }

    // 计算单个条件是否满足
    bool evalCondition(const AnimatorComponent& anim, const TransitionCondition& c) {
        auto it = anim.parameters.find(c.parameter);
        if (it == anim.parameters.end()) return false;
        const AnimParam& p = it->second;
        switch (c.op) {
            case ConditionOp::Greater:   return p.f >  c.threshold;
            case ConditionOp::GreaterEq: return p.f >= c.threshold;
            case ConditionOp::Less:      return p.f <  c.threshold;
            case ConditionOp::LessEq:    return p.f <= c.threshold;
            case ConditionOp::Equal:     return std::abs(p.f - c.threshold) < 1e-5f;
            case ConditionOp::NotEqual:  return std::abs(p.f - c.threshold) >= 1e-5f;
            case ConditionOp::IsTrue:    return p.f != 0.f;
            case ConditionOp::IsFalse:   return p.f == 0.f;
            case ConditionOp::Trigger:   return p.type == ParamType::Trigger && p.f != 0.f;
        }
        return false;
    }

    // 进入指定状态：切 currentAnim、time 归零、发 state_enter
    void enterState(AnimatorComponent& anim, int newState, AnimEventQueue& queue) {
        const AnimatorController& ctrl = *anim.controller;
        if (newState < 0 || newState >= (int)ctrl.states.size()) return;
        const AnimState& s = ctrl.states[newState];

        anim.currentState = newState;
        anim.currentAnim  = s.clip;
        anim.time         = 0.f;
        anim.speed        = s.speed;
        anim.currentMode  = s.mode;
        anim.playing      = s.clip.valid();
        anim.finished     = false;
        anim.stateFinishedFired = false;
        anim.interruptible = true;

        pushStateEvent(queue, "state_enter:" + s.name, s.clip);
    }
} // namespace

void AnimatorSystem::update(float dt) {
    auto view = ctx_.world.view<AnimatorComponent>();
    for (auto [ent, anim] : view.each()) {
        // 每帧开始清空事件队列 (单帧生命周期)
        AnimEventQueue* queuePtr = ctx_.world.try_get<AnimEventQueue>(ent);
        if (queuePtr) queuePtr->events.clear();

        // ── Phase 3: FSM 评估 ────────────────────────────────────────────
        if (anim.controller) {
            AnimatorController& ctrl = *anim.controller;

            // 首次进入默认状态
            if (anim.currentState < 0 && !ctrl.states.empty()) {
                if (!queuePtr) queuePtr = &ctx_.world.emplace<AnimEventQueue>(ent);
                enterState(anim, ctrl.defaultState, *queuePtr);
            }

            // 评估转移：当 transition 进行中且不可打断 → 跳过新转移评估
            const bool inTransition = (anim.fromState >= 0);
            const bool transBlocking = inTransition && anim.transitionDuration > 0.f &&
                                       anim.transitionT < anim.transitionDuration;
            // 当前 transition 是否禁止被打断：用源 transition 的 interruptible (此处简化为 true 总允许；按计划严格实现可记录)
            // 这里的 transitionInterruptible 默认 true，符合"过渡期间允许新 transition"语义。

            if (anim.currentState >= 0) {
                // 计算当前状态归一化进度 (用于 hasExitTime)
                float normalized = 0.f;
                if (anim.currentAnim.valid()) {
                    if (const AnimationClip* curClip = ctx_.assetManager.getAnimationClip(anim.currentAnim)) {
                        if (curClip->duration > 0.f) {
                            normalized = anim.time / curClip->duration;
                            if (curClip->loop || anim.currentMode == PlayMode::Loop) {
                                // loop 状态下 normalized 用累计圈数，hasExitTime 取整数倍达成
                                // 简化: 检查是否大于等于 exitTime (含跨圈)
                            }
                        }
                    }
                }
                if (anim.finished) normalized = std::max(normalized, 1.f);

                int chosen = -1;
                for (size_t i = 0; i < ctrl.transitions.size(); ++i) {
                    const auto& t = ctrl.transitions[i];
                    if (t.from != kAnyState && t.from != anim.currentState) continue;
                    // 不允许从自己转到自己 (除非显式列出)
                    if (t.from == kAnyState && t.to == anim.currentState) continue;
                    if (t.hasExitTime && normalized < t.exitTime) continue;
                    bool ok = true;
                    for (const auto& c : t.conditions) {
                        if (!evalCondition(anim, c)) { ok = false; break; }
                    }
                    if (!ok) continue;
                    chosen = (int)i;
                    break;
                }

                if (chosen >= 0) {
                    const auto& t = ctrl.transitions[chosen];
                    if (!queuePtr) queuePtr = &ctx_.world.emplace<AnimEventQueue>(ent);

                    // 触发 state_exit
                    if (anim.currentState >= 0 && anim.currentState < (int)ctrl.states.size()) {
                        pushStateEvent(*queuePtr, "state_exit:" + ctrl.states[anim.currentState].name);
                    }

                    // 启动 crossfade (sprite 模式简化为硬切，但记录 transition 状态)
                    anim.fromState         = anim.currentState;
                    anim.transitionT       = 0.f;
                    anim.transitionDuration = t.duration;

                    // 进入目标状态
                    enterState(anim, t.to, *queuePtr);

                    // 消费用到的 trigger 条件
                    for (const auto& c : t.conditions) {
                        if (c.op == ConditionOp::Trigger) {
                            anim.resetTrigger(c.parameter);
                        }
                    }

                    // 刷新当前 clip 指针并继续后续推进
                }
            }

            // crossfade 计时
            if (anim.fromState >= 0) {
                anim.transitionT += dt;
                if (anim.transitionT >= anim.transitionDuration) {
                    anim.fromState = -1;
                    anim.transitionT = 0.f;
                    anim.transitionDuration = 0.f;
                }
            }
            (void)transBlocking; // 保留以便后续严格化
        }

        // ── 时间推进 / 帧事件扫描 / sprite 写回 ─────────────────────────────
        if (!anim.currentAnim.valid()) continue;

        const AnimationClip* clip = ctx_.assetManager.getAnimationClip(anim.currentAnim);
        if (!clip || clip->frames.empty() || clip->duration <= 0.f) continue;

        const float prevTime = anim.time;
        AnimationHandle scanHandle = anim.currentAnim;
        float newTime = prevTime;
        bool wrapped = false;
        bool clamped = false;

        if (anim.playing && anim.speed != 0.f) {
            newTime = prevTime + dt * anim.speed;

            const bool loop = (anim.currentMode == PlayMode::Loop)
                           || (anim.currentMode == PlayMode::PingPong)
                           || (anim.currentMode == PlayMode::ClipDefault && clip->loop);

            if (anim.speed > 0.f && newTime >= clip->duration) {
                if (loop) {
                    newTime = std::fmod(newTime, clip->duration);
                    wrapped = true;
                } else {
                    newTime = clip->duration;
                    clamped = true;
                }
            } else if (anim.speed < 0.f && newTime < 0.f) {
                if (loop) {
                    newTime = std::fmod(newTime, clip->duration);
                    if (newTime < 0.f) newTime += clip->duration;
                    wrapped = true;
                } else {
                    newTime = 0.f;
                    clamped = true;
                }
            }
        }

        // Phase 2: 帧事件扫描
        if (anim.playing && !clip->events.empty() && (newTime != prevTime || clamped)) {
            if (!queuePtr) queuePtr = &ctx_.world.emplace<AnimEventQueue>(ent);

            if (anim.speed >= 0.f) {
                if (wrapped) {
                    scanForward(*clip, prevTime, clip->duration, false, scanHandle, *queuePtr);
                    scanForward(*clip, 0.f,      newTime,        false, scanHandle, *queuePtr);
                } else {
                    scanForward(*clip, prevTime, newTime, clamped, scanHandle, *queuePtr);
                }
            } else {
                if (wrapped) {
                    scanBackward(*clip, prevTime, 0.f,            false, scanHandle, *queuePtr);
                    scanBackward(*clip, clip->duration, newTime,  false, scanHandle, *queuePtr);
                } else {
                    scanBackward(*clip, prevTime, newTime, clamped, scanHandle, *queuePtr);
                }
            }
        }

        anim.time = newTime;
        if (clamped) {
            anim.playing = false;
            anim.finished = true;
            anim.interruptible = true;

            // Phase 3: 一次性发 state_finished (FSM 模式)
            if (anim.controller && !anim.stateFinishedFired) {
                if (!queuePtr) queuePtr = &ctx_.world.emplace<AnimEventQueue>(ent);
                pushStateEvent(*queuePtr, "state_finished", scanHandle);
                anim.stateFinishedFired = true;
            }

            // Phase 1: 完成后消费 queued (非 FSM 路径仍可用)
            if (anim.hasQueued && anim.queuedAnim.valid()) {
                AnimationHandle next = anim.queuedAnim;
                PlayOptions     opts = anim.queuedOpts;
                anim.hasQueued = false;
                anim.play(next, opts);
                clip = ctx_.assetManager.getAnimationClip(anim.currentAnim);
                if (!clip || clip->frames.empty() || clip->duration <= 0.f) continue;
            }
        }

        // 按 per-frame duration 累加定位当前帧
        float t = anim.time;
        size_t idx = clip->frames.size() - 1;
        for (size_t i = 0; i < clip->frames.size(); ++i) {
            if (t < clip->frames[i].duration) { idx = i; break; }
            t -= clip->frames[i].duration;
        }

        if (auto* spr = ctx_.world.try_get<Sprite>(ent)) {
            spr->srcRect = clip->frames[idx].srcRect;
            if (anim.applyTexture && clip->texture.valid()) {
                spr->texture = clip->texture;
            }
            spr->gpuDirty = true;
        }
    }
}

void AnimatorSystem::shutdown() {}

} // namespace engine
