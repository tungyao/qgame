#include "LightingPass.h"
#include "../../engine/components/RenderComponents.h"

namespace backend {

void LightingPass::execute(FrameContext& ctx, CommandBuffer& cb, const engine::light::LightFrameData& lights) {
    // 开始光照Pass
    cb.beginLightPass(ctx.viewportW, ctx.viewportH);
    
    // 提交所有点光源（additive blend累加到light RT）
    for (const auto& light : lights.pointLights) {
        engine::light::PointLightGPU gpuLight = light;
        cb.drawLight({
            {gpuLight.x, gpuLight.y},
            gpuLight.radius,
            {gpuLight.r, gpuLight.g, gpuLight.b, gpuLight.a},
            gpuLight.intensity,
            engine::RenderPass::World
        });
    }
}

void CompositePass::execute(FrameContext& ctx, CommandBuffer& cb) {
    // 合成Pass：GBuffer颜色 × LightRT → swapchain
    cb.beginCompositePass();
    // TODO: 提交全屏quad，采样scene纹理和light纹理
}

} // namespace backend
