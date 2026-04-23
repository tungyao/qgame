#pragma once
#include "ISystem.h"

namespace engine {
class EngineContext;

class RenderSystem final : public ISystem {
public:
    explicit RenderSystem(EngineContext& ctx) : ctx_(ctx) {}

    void init()           override;
    void update(float dt) override;
    void shutdown()       override;

private:
    void buildCommandBuffer();

    EngineContext& ctx_;
};

} // namespace engine
