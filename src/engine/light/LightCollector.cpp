#include "LightCollector.h"
#include "../components/RenderComponents.h"

namespace engine::light {

void collect(entt::registry& registry, LightFrameData& out, int /*viewportW*/, int /*viewportH*/) {
    out.pointLights.clear();
    
    auto view = registry.view<PointLight, Transform>();
    for (auto entity : view) {
        auto& light = view.get<PointLight>(entity);
        auto& transform = view.get<Transform>(entity);
        
        if (!light.enabled) continue;
        
        // TODO: 视锥剔除（相机rect ∩ light.radius圆）
        
        PointLightGPU gpu{};
        gpu.x = transform.x;
        gpu.y = transform.y;
        gpu.radius = light.radius;
        gpu.intensity = light.intensity;
        gpu.r = light.color.r;
        gpu.g = light.color.g;
        gpu.b = light.color.b;
        gpu.a = light.color.a;
        
        out.pointLights.push_back(gpu);
    }
}

} // namespace engine::light
