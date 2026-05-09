#pragma once
#include "../components/LightComponents.h"
#include <vector>
#include <entt/entt.hpp>

namespace engine::light {

struct PointLightGPU {
    float x, y;
    float radius;
    float intensity;
    float r, g, b, a;
};

struct LightFrameData {
    std::vector<PointLightGPU> pointLights;
    // 将来可以加 directionalLights, ambientLight
};

// 纯函数：从ECS采集光源数据，无状态、无tick
void collect(entt::registry& registry, LightFrameData& out, int viewportW, int viewportH);

} // namespace engine::light
