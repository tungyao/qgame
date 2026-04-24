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
};

} // namespace engine
