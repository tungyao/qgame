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

    // 录制完整场景命令到 cb（不含 SetCamera/Clear，调用方按相机分发）。
    // 编辑器离屏路径直接消费这个 cb。
    static void buildSceneCommands(EngineContext& ctx, backend::CommandBuffer& cb,
                                   int viewportW, int viewportH);

private:
    void buildCommandBuffer();

    EngineContext& ctx_;
};

} // namespace engine
