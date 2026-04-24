#pragma once
#include "ISystem.h"
#include "../../backend/renderer/RenderPipeline.h"

namespace backend { class CommandBuffer; }

namespace engine {
class EngineContext;

class RenderSystem final : public ISystem {
public:
    explicit RenderSystem(EngineContext& ctx) : ctx_(ctx) {}

    void init()           override;
    void update(float dt) override;
    void shutdown()       override;

    // 将当前 ECS 场景写入 cb，供 EditorAPI 复用（editor 路径直接 submit，不走 pipeline）
    static void buildSceneCommands(EngineContext& ctx, backend::CommandBuffer& cb, int viewportW, int viewportH);

private:
    void buildCommandBuffer();
    void syncCamerasToPassStates(int viewportW, int viewportH);

    EngineContext&        ctx_;
    backend::RenderPipeline pipeline_;
};

} // namespace engine
