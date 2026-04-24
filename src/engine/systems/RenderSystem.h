#pragma once
#include "ISystem.h"

namespace backend { class CommandBuffer; }

namespace engine {
class EngineContext;

class RenderSystem final : public ISystem {
public:
    explicit RenderSystem(EngineContext& ctx) : ctx_(ctx) {}

    void init()           override;
    void update(float dt) override;
    void shutdown()       override;

    // 将当前 ECS 场景写入 cb，供 EditorAPI 复用
    static void buildSceneCommands(EngineContext& ctx, backend::CommandBuffer& cb, int viewportW, int viewportH);

private:
    void buildCommandBuffer();

    EngineContext& ctx_;
};

} // namespace engine
