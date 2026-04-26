#pragma once
#include "ISystem.h"
#include "../components/RenderComponents.h"
#include "../components/AnimatorComponent.h"
#include "../resources/SpriteBuffer.h"
#include "../resources/GPUDrivenRenderer.h"
#include <entt/entt.hpp>

namespace backend { class CommandBuffer; }

namespace engine {
class EngineContext;

class RenderSystem final : public ISystem {
public:
    explicit RenderSystem(EngineContext& ctx);
    ~RenderSystem();

    void init()           override;
    void update(float dt) override;
    void shutdown()       override;

    static void buildSceneCommands(EngineContext& ctx, backend::CommandBuffer& cb,
                                   int viewportW, int viewportH);

    SpriteBuffer& spriteBuffer() { return spriteBuffer_; }
    GPUDrivenRenderer& gpuRenderer() { return gpuRenderer_; }
    
    void setGPUDrivenEnabled(bool enabled) { gpuDrivenEnabled_ = enabled; }
    bool isGPUDrivenEnabled() const { return gpuDrivenEnabled_; }

private:
    void buildCommandBuffer();
    void buildCommandBufferGPUDriven();
    void syncEntitiesToGPU();
    void allocateGPUSlot(entt::entity e, Sprite& spr);
    void freeGPUSlot(entt::registry& reg, entt::entity e);
    void updateGPUSlot(const Transform& tf, const Sprite& spr, const AnimatorOutput* aout = nullptr);
    void onTransformUpdate(entt::registry& reg, entt::entity e);

    EngineContext& ctx_;
    SpriteBuffer spriteBuffer_;
    GPUDrivenRenderer gpuRenderer_;
    bool gpuDrivenEnabled_ = false;
    entt::connection destroyConnection_;
    entt::connection transformUpdateConnection_;
};

} // namespace engine
