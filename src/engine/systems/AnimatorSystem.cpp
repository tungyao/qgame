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
            if (anim.time >= clip->duration) {
                if (clip->loop) {
                    anim.time = std::fmod(anim.time, clip->duration);
                } else {
                    anim.time = clip->duration;
                    anim.playing = false;
                }
            }
        }

        // 按 per-frame duration 累加定位当前帧
        float t = anim.time;
        size_t idx = 0;
        for (size_t i = 0; i < clip->frames.size(); ++i) {
            if (t < clip->frames[i].duration) { idx = i; break; }
            t -= clip->frames[i].duration;
            idx = i;
        }

        // 写入 Sprite (若存在)
        if (auto* spr = ctx_.world.try_get<Sprite>(ent)) {
            spr->srcRect = clip->frames[idx].srcRect;
            if (anim.applyTexture && clip->texture.valid()) {
                spr->texture = clip->texture;
            }
        }
    }
}

void AnimatorSystem::shutdown() {}

} // namespace engine
