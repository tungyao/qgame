#include "AnimatorSystem.h"
#include "../components/AnimatorComponent.h"
#include "../components/RenderComponents.h"
#include "../runtime/EngineContext.h"
#include "../assets/AssetManager.h"
#include <cmath>

namespace engine {

void AnimatorSystem::init() {}

namespace {
    // 在 clip 上扫描 [a, b) 区间的事件 (forward)；inclEnd=true 时为 [a, b]
    void scanForward(const AnimationClip& clip, float a, float b, bool inclEnd,
                     AnimationHandle handle, AnimEventQueue& q) {
        for (const auto& e : clip.events) {
            const bool inEnd = inclEnd ? (e.time <= b) : (e.time < b);
            if (e.time >= a && inEnd) {
                q.events.push_back({ e.name, e.intParam, e.floatParam, e.stringParam, handle, e.time });
            }
        }
    }
    // 反向扫描 (b, a]; inclEnd=true 时为 [b, a]
    void scanBackward(const AnimationClip& clip, float a, float b, bool inclEnd,
                      AnimationHandle handle, AnimEventQueue& q) {
        // a > b。反向遍历事件维持时序
        for (auto it = clip.events.rbegin(); it != clip.events.rend(); ++it) {
            const bool inEnd = inclEnd ? (it->time >= b) : (it->time > b);
            if (it->time <= a && inEnd) {
                q.events.push_back({ it->name, it->intParam, it->floatParam, it->stringParam,
                                     handle, it->time });
            }
        }
    }
} // namespace

void AnimatorSystem::update(float dt) {
    auto view = ctx_.world.view<AnimatorComponent>();
    for (auto [ent, anim] : view.each()) {
        // Phase 2: 每帧开始时清空事件队列 (单帧生命周期)
        if (auto* q = ctx_.world.try_get<AnimEventQueue>(ent)) {
            q->events.clear();
        }

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
                    // newTime 为负，落到 [0, duration)
                    newTime = std::fmod(newTime, clip->duration);
                    if (newTime < 0.f) newTime += clip->duration;
                    wrapped = true;
                } else {
                    newTime = 0.f;
                    clamped = true;
                }
            }
        }

        // ── Phase 2: 事件扫描 ────────────────────────────────────────────────
        if (anim.playing && !clip->events.empty() && (newTime != prevTime || clamped)) {
            AnimEventQueue* queue = ctx_.world.try_get<AnimEventQueue>(ent);
            if (!queue) queue = &ctx_.world.emplace<AnimEventQueue>(ent);

            if (anim.speed >= 0.f) {
                if (wrapped) {
                    scanForward(*clip, prevTime, clip->duration, false, scanHandle, *queue);
                    scanForward(*clip, 0.f,      newTime,        false, scanHandle, *queue);
                } else {
                    scanForward(*clip, prevTime, newTime, clamped, scanHandle, *queue);
                }
            } else {
                if (wrapped) {
                    scanBackward(*clip, prevTime, 0.f,            false, scanHandle, *queue);
                    scanBackward(*clip, clip->duration, newTime,  false, scanHandle, *queue);
                } else {
                    scanBackward(*clip, prevTime, newTime, clamped, scanHandle, *queue);
                }
            }
        }

        // ── 状态推进 / one-shot 完成处理 ─────────────────────────────────────
        anim.time = newTime;
        if (clamped) {
            anim.playing = false;
            anim.finished = true;
            anim.interruptible = true;
            // Phase 1: 完成后消费 queued
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

        // 写入 Sprite (若存在)
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
