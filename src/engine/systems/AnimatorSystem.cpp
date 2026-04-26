#include "AnimatorSystem.h"
#include "../components/AnimatorComponent.h"
#include "../components/RenderComponents.h"
#include "../runtime/EngineContext.h"
#include "../assets/AssetManager.h"
#include <cmath>

namespace engine {

void AnimatorSystem::init() {}

void AnimatorSystem::update(float dt) {
    auto view = ctx_.world.view<AnimatorComponent>();
    for (auto [ent, anim] : view.each()) {
        if (!anim.currentAnim.valid()) continue;

        const AnimationClip* clip = ctx_.assetManager.getAnimationClip(anim.currentAnim);
        if (!clip || clip->frames.empty() || clip->duration <= 0.f) continue;

        if (anim.playing) {
            anim.time += dt * anim.speed;

            const bool loop = (anim.currentMode == PlayMode::Loop)
                           || (anim.currentMode == PlayMode::PingPong)
                           || (anim.currentMode == PlayMode::ClipDefault && clip->loop)
                           || (anim.currentMode == PlayMode::Once ? false : false);

            if (anim.time >= clip->duration) {
                if (loop) {
                    anim.time = std::fmod(anim.time, clip->duration);
                } else {
                    anim.time = clip->duration;
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
            }
        }

        // 按 per-frame duration 累加定位当前帧
        float t = anim.time;
        size_t idx = 0;
        // 保护：如果 frames 为空，跳过
        if (clip->frames.empty()) continue;
        idx = clip->frames.size() - 1; // 默认最后一帧
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
