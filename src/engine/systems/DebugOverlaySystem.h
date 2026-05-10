#pragma once
#include "ISystem.h"
#include "../runtime/EngineContext.h"

namespace engine {


struct DebugOverlayComponent {

    float updateTimer = 0.0f;
    float fpsAccumulator = 0.0f;
    int fpsFrames = 0;
};

class DebugOverlaySystem final : public ISystem {
public:
    explicit DebugOverlaySystem(EngineContext& ctx);
    UpdatePhaseMask phaseMask() const override;
    void init() override;
    bool runPhase(UpdatePhase phase, float dt) override;
private:
    EngineContext& ctx_;
};

} // namespace engine
