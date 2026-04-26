#pragma once
#include <cstdint>
#include <vector>
#include "../anim/Easing.h"

namespace engine {

// Phase 5.1: Tween 通道 — 系统据此把 lerp 结果写到目标字段
enum class TweenChannel : uint8_t {
    PositionX,   PositionY,
    Rotation,
    ScaleX,      ScaleY,
    SpriteTintR, SpriteTintG, SpriteTintB, SpriteTintA,
    CameraZoom,
    Custom,      // 不写回任何字段；调用者通过 outValue 自行读取
};

struct TweenInstance {
    TweenChannel channel  = TweenChannel::Custom;
    float        from     = 0.f;
    float        to       = 1.f;
    float        duration = 1.f;        // 秒
    float        elapsed  = 0.f;
    Easing       easing   = Easing::Linear;
    bool         loop     = false;
    bool         pingpong = false;      // pingpong 时 loop 默认开启往返
    bool         finished = false;
    bool         removeOnFinish = true; // 完成后由 system 移除
    int8_t       direction = 1;         // pingpong 内部状态：+1 / -1
    float        outValue = 0.f;        // 当前缓动后的值 (Custom 通道用)
    uint32_t     userId   = 0;          // 调用者自定义 id (用于查找/取消)
};

struct TweenComponent {
    std::vector<TweenInstance> instances;

    TweenInstance& add(const TweenInstance& t) {
        instances.push_back(t);
        return instances.back();
    }
};

} // namespace engine
