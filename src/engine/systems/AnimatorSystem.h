#pragma once
#include "ISystem.h"

namespace engine {
class EngineContext;

class AnimatorSystem final : public ISystem {
public:
    explicit AnimatorSystem(EngineContext& ctx) : ctx_(ctx) {}

    UpdatePhaseMask phaseMask() const override {
        return updatePhaseBit(UpdatePhase::Animation);
    }

    void init() override;
    void update(float dt) override;
    void shutdown() override;

private:
    void onAnimationPhase(float dt) override {
        update(dt);
    }

    EngineContext& ctx_;
};

} // namespace engine
