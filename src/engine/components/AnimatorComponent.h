#pragma once
#include <string>
#include <vector>
#include "../../backend/shared/ResourceHandle.h"
#include "../../core/math/Rect.h"

namespace engine {

struct AnimationFrame {
    core::Rect srcRect;     // 在 spritesheet 内的像素区域
    float      duration;    // 本帧持续时长 (秒)
};

struct AnimationClip {
    std::string                 name;        // tag 名 (或文件名)
    TextureHandle               texture;     // 关联的 spritesheet
    std::vector<AnimationFrame> frames;
    float                       duration = 0.f; // 所有帧累计时长
    bool                        loop = true;
};

struct AnimatorComponent {
    AnimationHandle currentAnim;
    float           time    = 0.f;
    float           speed   = 1.f;
    bool            playing = false;
    bool            applyTexture = true; // 播放时是否同时覆盖 Sprite.texture
    bool            finished = false;    // 非循环动画播放完成标志

    // 播放指定动画（会重置时间）
    void play(AnimationHandle anim) {
        if (currentAnim != anim) {
            currentAnim = anim;
            time = 0.f;
            finished = false;
        }
        playing = true;
    }

    // 停止播放并重置
    void stop() {
        playing = false;
        time = 0.f;
        finished = false;
    }
};

} // namespace engine
