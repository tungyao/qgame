#pragma once
#include "ISystem.h"
#include "../components/InteractionComponents.h"
#include <entt/entt.hpp>

namespace engine {

class EngineContext;

class InteractionSystem final : public ISystem {
public:
    explicit InteractionSystem(EngineContext& ctx);

    UpdatePhaseMask phaseMask() const override {
        return updatePhaseBit(UpdatePhase::GameplayPostPhysics);
    }

    void init() override;
    void update(float dt) override;
    void shutdown() override;

    entt::entity hovered() const { return hovered_; }
    entt::entity pressed() const { return pressed_; }

private:
    void onGameplayPostPhysicsPhase(float dt) override { update(dt); }

    EngineContext& ctx_;

    entt::entity hovered_ = entt::null;
    entt::entity pressed_ = entt::null;
    bool prevPointerDown_ = false;
};

} // namespace engine
