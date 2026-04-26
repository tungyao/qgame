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

    // 一层的运行时字段视图 (引用绑定)：基础层指向 AnimatorComponent 既有字段，额外层指向 AnimatorLayerRuntime
    struct LayerRT {
        AnimationHandle& currentAnim;
        float&           time;
        float&           speed;
        bool&            playing;
        bool&            finished;
        PlayMode&        currentMode;
        int&             currentState;
        int&             fromState;
        float&           transitionT;
        float&           transitionDuration;
        bool&            stateFinishedFired;
    };

    void enterStateRT(AnimatorComponent& anim, LayerRT& lr,
                      const std::vector<AnimState>& states, int newState,
                      AnimEventQueue& queue) {
        if (newState < 0 || newState >= (int)states.size()) return;
        const AnimState& s = states[newState];
        lr.currentState        = newState;
        lr.currentAnim         = s.clip;
        lr.time                = 0.f;
        lr.speed               = s.speed;
        lr.currentMode         = s.mode;
        lr.playing             = s.clip.valid();
        lr.finished            = false;
        lr.stateFinishedFired  = false;
        anim.interruptible     = true;  // 仅基础层语义；额外层无优先级 (无害)
        pushStateEvent(queue, "state_enter:" + s.name, s.clip);
    }

    // FSM 评估 + 转移启动 + transition 计时推进
    void tickFSM(AnimatorComponent& anim, LayerRT& lr,
                 const std::vector<AnimState>& states,
                 const std::vector<AnimTransition>& transitions,
                 int defaultState,
                 const AssetManager& am,
                 AnimEventQueue& queue, float dt) {
        // 首次进入默认状态
        if (lr.currentState < 0 && !states.empty()) {
            enterStateRT(anim, lr, states, defaultState, queue);
        }

        if (lr.currentState >= 0) {
            // 归一化进度 (用于 hasExitTime)
            float normalized = 0.f;
            if (lr.currentAnim.valid()) {
                if (const AnimationClip* curClip = am.getAnimationClip(lr.currentAnim)) {
                    if (curClip->duration > 0.f) {
                        normalized = lr.time / curClip->duration;
                    }
                }
            }
            if (lr.finished) normalized = std::max(normalized, 1.f);

            int chosen = -1;
            for (size_t i = 0; i < transitions.size(); ++i) {
                const auto& t = transitions[i];
                if (t.from != kAnyState && t.from != lr.currentState) continue;
                if (t.from == kAnyState && t.to == lr.currentState) continue;
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
                const auto& t = transitions[chosen];
                if (lr.currentState >= 0 && lr.currentState < (int)states.size()) {
                    pushStateEvent(queue, "state_exit:" + states[lr.currentState].name);
                }
                lr.fromState           = lr.currentState;
                lr.transitionT         = 0.f;
                lr.transitionDuration  = t.duration;

                enterStateRT(anim, lr, states, t.to, queue);

                for (const auto& c : t.conditions) {
                    if (c.op == ConditionOp::Trigger) anim.resetTrigger(c.parameter);
                }
            }
        }

        // crossfade 计时
        if (lr.fromState >= 0) {
            lr.transitionT += dt;
            if (lr.transitionT >= lr.transitionDuration) {
                lr.fromState           = -1;
                lr.transitionT         = 0.f;
                lr.transitionDuration  = 0.f;
            }
        }
    }

    // 时间推进 + 帧事件扫描；返回当前 clip 指针 (供调用方做 sprite 写回)
    const AnimationClip* advanceTime(LayerRT& lr, const AssetManager& am,
                                     AnimEventQueue*& queuePtr,
                                     entt::registry* worldForLazyQ, entt::entity ent,
                                     float dt) {
        if (!lr.currentAnim.valid()) return nullptr;
        const AnimationClip* clip = am.getAnimationClip(lr.currentAnim);
        if (!clip || clip->frames.empty() || clip->duration <= 0.f) return clip;

        const float prevTime = lr.time;
        AnimationHandle scanHandle = lr.currentAnim;
        float newTime = prevTime;
        bool wrapped = false;
        bool clamped = false;

        if (lr.playing && lr.speed != 0.f) {
            newTime = prevTime + dt * lr.speed;
            const bool loop = (lr.currentMode == PlayMode::Loop)
                           || (lr.currentMode == PlayMode::PingPong)
                           || (lr.currentMode == PlayMode::ClipDefault && clip->loop);

            if (lr.speed > 0.f && newTime >= clip->duration) {
                if (loop) { newTime = std::fmod(newTime, clip->duration); wrapped = true; }
                else      { newTime = clip->duration; clamped = true; }
            } else if (lr.speed < 0.f && newTime < 0.f) {
                if (loop) {
                    newTime = std::fmod(newTime, clip->duration);
                    if (newTime < 0.f) newTime += clip->duration;
                    wrapped = true;
                } else { newTime = 0.f; clamped = true; }
            }
        }

        if (lr.playing && !clip->events.empty() && (newTime != prevTime || clamped)) {
            if (!queuePtr) queuePtr = &worldForLazyQ->emplace<AnimEventQueue>(ent);
            if (lr.speed >= 0.f) {
                if (wrapped) {
                    scanForward(*clip, prevTime, clip->duration, false, scanHandle, *queuePtr);
                    scanForward(*clip, 0.f,      newTime,        false, scanHandle, *queuePtr);
                } else {
                    scanForward(*clip, prevTime, newTime, clamped, scanHandle, *queuePtr);
                }
            } else {
                if (wrapped) {
                    scanBackward(*clip, prevTime, 0.f,           false, scanHandle, *queuePtr);
                    scanBackward(*clip, clip->duration, newTime, false, scanHandle, *queuePtr);
                } else {
                    scanBackward(*clip, prevTime, newTime, clamped, scanHandle, *queuePtr);
                }
            }
        }

        lr.time = newTime;
        if (clamped) {
            lr.playing  = false;
            lr.finished = true;
        }
        return clip;
    }

    // 从 clip + time 计算当前帧索引
    size_t resolveFrameIndex(const AnimationClip& clip, float time) {
        float t = time;
        size_t idx = clip.frames.size() - 1;
        for (size_t i = 0; i < clip.frames.size(); ++i) {
            if (t < clip.frames[i].duration) { idx = i; break; }
            t -= clip.frames[i].duration;
        }
        return idx;
    }
} // namespace

void AnimatorSystem::update(float dt) {
    auto view = ctx_.world.view<AnimatorComponent>();
    for (auto [ent, anim] : view.each()) {
        // 单帧事件队列：每帧开始清空
        AnimEventQueue* queuePtr = ctx_.world.try_get<AnimEventQueue>(ent);
        if (queuePtr) queuePtr->events.clear();

        // 基础层运行时视图 (绑到 AnimatorComponent 既有字段)
        LayerRT base{
            anim.currentAnim, anim.time, anim.speed, anim.playing, anim.finished,
            anim.currentMode, anim.currentState, anim.fromState,
            anim.transitionT, anim.transitionDuration, anim.stateFinishedFired,
        };

        // ── Phase 3: 基础层 FSM 评估 ─────────────────────────────────────────
        if (anim.controller) {
            AnimatorController& ctrl = *anim.controller;
            if (!queuePtr) queuePtr = &ctx_.world.emplace<AnimEventQueue>(ent);
            tickFSM(anim, base, ctrl.states, ctrl.transitions, ctrl.defaultState,
                    ctx_.assetManager, *queuePtr, dt);

            // ── Phase 4: 额外层 FSM ────────────────────────────────────────
            if (anim.extraLayers.size() != ctrl.layers.size()) {
                anim.extraLayers.resize(ctrl.layers.size());
            }
            for (size_t li = 0; li < ctrl.layers.size(); ++li) {
                const AnimatorLayer& Ldef = ctrl.layers[li];
                AnimatorLayerRuntime& Lrt = anim.extraLayers[li];
                LayerRT lrv{
                    Lrt.currentAnim, Lrt.time, Lrt.speed, Lrt.playing, Lrt.finished,
                    Lrt.currentMode, Lrt.currentState, Lrt.fromState,
                    Lrt.transitionT, Lrt.transitionDuration, Lrt.stateFinishedFired,
                };
                tickFSM(anim, lrv, Ldef.states, Ldef.transitions, Ldef.defaultState,
                        ctx_.assetManager, *queuePtr, dt);
            }
        }

        // ── 基础层时间推进 + 事件扫描 ────────────────────────────────────────
        const AnimationClip* baseClip =
            advanceTime(base, ctx_.assetManager, queuePtr, &ctx_.world, ent, dt);

        // 基础层完成处理：state_finished + queued 续播
        if (anim.finished && baseClip) {
            if (anim.controller && !anim.stateFinishedFired) {
                if (!queuePtr) queuePtr = &ctx_.world.emplace<AnimEventQueue>(ent);
                pushStateEvent(*queuePtr, "state_finished", anim.currentAnim);
                anim.stateFinishedFired = true;
            }
            anim.interruptible = true;
            if (anim.hasQueued && anim.queuedAnim.valid()) {
                AnimationHandle next = anim.queuedAnim;
                PlayOptions     opts = anim.queuedOpts;
                anim.hasQueued = false;
                anim.play(next, opts);
                baseClip = ctx_.assetManager.getAnimationClip(anim.currentAnim);
            }
        }

        // ── 基础层写回 sprite (Override：srcRect + texture) ─────────────────
        Sprite* spr = ctx_.world.try_get<Sprite>(ent);
        if (spr && baseClip && !baseClip->frames.empty() && baseClip->duration > 0.f) {
            const size_t idx = resolveFrameIndex(*baseClip, anim.time);
            spr->srcRect = baseClip->frames[idx].srcRect;
            if (anim.applyTexture && baseClip->texture.valid()) {
                spr->texture = baseClip->texture;
            }
            spr->gpuDirty = true;
        }

        // ── Phase 4: 额外层时间推进 + 写回合成 ──────────────────────────────
        if (anim.controller) {
            AnimatorController& ctrl = *anim.controller;
            for (size_t li = 0; li < ctrl.layers.size() && li < anim.extraLayers.size(); ++li) {
                const AnimatorLayer& Ldef = ctrl.layers[li];
                AnimatorLayerRuntime& Lrt = anim.extraLayers[li];
                LayerRT lrv{
                    Lrt.currentAnim, Lrt.time, Lrt.speed, Lrt.playing, Lrt.finished,
                    Lrt.currentMode, Lrt.currentState, Lrt.fromState,
                    Lrt.transitionT, Lrt.transitionDuration, Lrt.stateFinishedFired,
                };
                const AnimationClip* lClip =
                    advanceTime(lrv, ctx_.assetManager, queuePtr, &ctx_.world, ent, dt);

                if (Lrt.finished && !Lrt.stateFinishedFired) {
                    if (!queuePtr) queuePtr = &ctx_.world.emplace<AnimEventQueue>(ent);
                    pushStateEvent(*queuePtr, "state_finished", Lrt.currentAnim);
                    Lrt.stateFinishedFired = true;
                }

                if (Ldef.weight <= 0.f) continue;
                if (!lClip || lClip->frames.empty() || lClip->duration <= 0.f) continue;
                if (!spr) continue;

                const bool wantSrcRect = (Ldef.mask & LayerChannel::SrcRect) != 0;
                const bool wantTexture = (Ldef.mask & LayerChannel::Texture) != 0;
                if (!wantSrcRect && !wantTexture) continue; // 其他通道无 clip 数据源 (Phase 5)

                const size_t idx = resolveFrameIndex(*lClip, Lrt.time);
                if (Ldef.blendMode == LayerBlendMode::Override) {
                    if (wantSrcRect) spr->srcRect = lClip->frames[idx].srcRect;
                    if (wantTexture && lClip->texture.valid()) spr->texture = lClip->texture;
                    spr->gpuDirty = true;
                }
                // Additive: 帧动画无每帧 srcRect 增量语义，留给 Phase 5 程序化层
            }
        }
    }
}

void AnimatorSystem::shutdown() {}

} // namespace engine
