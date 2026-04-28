#pragma once
#include <cstdint>
#include <functional>
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
    // 启动延迟 (秒)。延迟期间 outValue 钉在 from，且按 from 写回目标通道
    // (语义 B: 一上场就把字段拉到起点，避免"延迟里看到目标停在原值再突然
    // 跳到 from 开始动")。延迟期不推进 elapsed；delay 本帧用尽后不补偿剩
    // 余 dt，下一帧从 elapsed=0 正常推进 (省一行代码、最多差 1 帧)。
    float        delay    = 0.f;
    Easing       easing   = Easing::Linear;
    bool         loop     = false;
    bool         pingpong = false;      // pingpong 时 loop 默认开启往返
    bool         finished = false;
    bool         removeOnFinish = true; // 完成后由 system 移除
    int8_t       direction = 1;         // pingpong 内部状态：+1 / -1
    float        outValue = 0.f;        // 当前缓动后的值 (Custom 通道用)
    uint32_t     userId   = 0;          // 调用者自定义 id (用于查找/取消)

    // 每帧把当前 outValue 推给调用方。配合 channel=Custom 即可让 TweenSystem
    // 不知道目标类型的前提下 lerp 任意字段 (UI/物理/音量/着色器参数等)。
    // 也对内置通道生效 —— 业务想"通道写回 + 自己也观察"是合法用法。
    std::function<void(float v)> onUpdate;

    // 动画 finished=true 的那一帧触发一次 (在 system 删除该实例之前)。
    // 触发时机被推迟到当前帧的 ECS 遍历结束之后，因此回调里安全 add/remove
    // TweenInstance、emplace/erase TweenComponent，不会破坏迭代器。
    std::function<void()> onComplete;
};

struct TweenComponent {
    std::vector<TweenInstance> instances;

    TweenInstance& add(const TweenInstance& t) {
        instances.push_back(t);
        return instances.back();
    }
};

} // namespace engine
