#pragma once
#include <cmath>

namespace engine {

// Phase 5.2: 临界/欠/过阻尼弹簧（半隐式欧拉）
// 用途：相机跟随、UI 弹入、采集物吸附、命中位移回弹
struct SpringValue {
    float value     = 0.f;
    float velocity  = 0.f;
    float target    = 0.f;
    float stiffness = 120.f;   // 越大越快收敛
    float damping   = 14.f;    // 越大越没"震荡"，~2*sqrt(stiffness) ≈ 临界

    // 推进一帧；返回当前 value
    float update(float dt) {
        // F = -k*(x - target) - c*v
        const float a = -stiffness * (value - target) - damping * velocity;
        velocity += a * dt;
        value    += velocity * dt;
        return value;
    }

    // 直接 snap 到目标 (无残余速度)
    void snap(float v) {
        value = target = v;
        velocity = 0.f;
    }

    // 是否已经"足够静止"
    bool atRest(float epsValue = 1e-3f, float epsVel = 1e-3f) const {
        return std::abs(value - target) < epsValue && std::abs(velocity) < epsVel;
    }

    // 临界阻尼系数（给定 stiffness 推荐 damping）
    static float criticalDamping(float k) { return 2.f * std::sqrt(k); }
};

} // namespace engine
