#pragma once

#include <algorithm>
#include <cmath>

#include <entt/entt.hpp>

#include "../components/RenderComponents.h"

namespace engine {

// TransformInterpolation 是运行时表现层组件，不参与 authoring/序列化。
//
// 它只保存“上一物理步结束时的 Transform”，当前物理真值仍然继续放在
// Transform 组件本体里。渲染/UI/相机在需要平滑 fixed-timestep 结果时，使用：
//
//   present = lerp(previousPhysicsTransform, currentTransform, alpha)
//
// 其中 alpha = PhysicsSystem.accumulator / fixedTimestep。
//
// 这样 gameplay/physics 继续读取权威 current Transform，不会被表现层插值污染；
// 而所有面向屏幕的系统又能共享同一时刻的平滑姿态。
struct TransformInterpolation {
    Transform previous;
    bool initialized = false;
};

inline float normalizeRadians(float angle) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    while (angle > 3.14159265358979323846f) angle -= kTwoPi;
    while (angle < -3.14159265358979323846f) angle += kTwoPi;
    return angle;
}

inline float lerpAngleRadians(float from, float to, float alpha) {
    const float delta = normalizeRadians(to - from);
    return normalizeRadians(from + delta * std::clamp(alpha, 0.f, 1.f));
}

inline Transform interpolateTransform(const Transform& previous,
                                      const Transform& current,
                                      float alpha) {
    const float t = std::clamp(alpha, 0.f, 1.f);
    Transform out{};
    out.x = previous.x + (current.x - previous.x) * t;
    out.y = previous.y + (current.y - previous.y) * t;
    out.rotation = lerpAngleRadians(previous.rotation, current.rotation, t);
    out.scaleX = previous.scaleX + (current.scaleX - previous.scaleX) * t;
    out.scaleY = previous.scaleY + (current.scaleY - previous.scaleY) * t;
    return out;
}

inline Transform sampleInterpolatedTransform(const Transform& current,
                                             const TransformInterpolation* interpolation,
                                             float alpha) {
    if (!interpolation || !interpolation->initialized) {
        return current;
    }
    return interpolateTransform(interpolation->previous, current, alpha);
}

inline Transform sampleInterpolatedTransform(const entt::registry& world,
                                             entt::entity entity,
                                             float alpha) {
    const Transform* current = world.try_get<Transform>(entity);
    if (!current) return {};
    const auto* interpolation = world.try_get<TransformInterpolation>(entity);
    return sampleInterpolatedTransform(*current, interpolation, alpha);
}

} // namespace engine
