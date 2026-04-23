#pragma once
#include <entt/entt.hpp>
#include "ISystem.h"

namespace engine {

class PhysicsSystem : public ISystem {
public:
    PhysicsSystem(entt::registry& world, entt::dispatcher& dispatcher);

    void update(float dt) override;
    bool isManuallyScheduled() const override { return true; }

    void setGravity(float x, float y) { gravityX_ = x; gravityY_ = y; }
    float gravityX() const { return gravityX_; }
    float gravityY() const { return gravityY_; }

private:
    entt::registry&   world_;
    entt::dispatcher& dispatcher_;
    float gravityX_ = 0.f;
    float gravityY_ = 0.f;

    void integrateVelocities(float dt);
    void resolveCollisions();
};

} // namespace engine
