#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
#include "box2d/box2d.h"

#include "ISystem.h"
#include "../components/PhysicsComponents.h"
#include "../runtime/TransformInterpolation.h"

namespace engine {

constexpr float PIXELS_PER_METER = 32.0f;
constexpr int   SUB_STEP_COUNT   = 4;

inline float    toMeters(float px)       { return px / PIXELS_PER_METER; }
inline float    toPixels(float m)        { return m * PIXELS_PER_METER; }
inline b2Vec2   toMetersVec2(float x, float y) { return {x / PIXELS_PER_METER, y / PIXELS_PER_METER}; }
inline float    toRadians(float deg)     { return deg * 3.141592653589793f / 180.0f; }
inline float    toDegrees(float rad)     { return rad * 180.0f / 3.141592653589793f; }

inline entt::entity userDataToEntity(void* ud) {
    return static_cast<entt::entity>(static_cast<uintptr_t>(reinterpret_cast<intptr_t>(ud)));
}
inline void* entityToUserData(entt::entity e) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(static_cast<uint32_t>(e)));
}

class PhysicsSystem : public ISystem {
public:
    PhysicsSystem(entt::registry& world, entt::dispatcher& dispatcher);

    void init() override;
    void shutdown() override;

    UpdatePhaseMask phaseMask() const override {
        return updatePhaseBit(UpdatePhase::Physics);
    }

    void update(float dt) override;

    void setGravity(float x, float y);
    float gravityX() const;
    float gravityY() const;

    void setFixedTimestep(float step);
    float fixedTimestep() const;
    float accumulatorSeconds() const;
    void setVariableTimestep(bool enable);
    bool variableTimestep() const;
    float interpolationAlpha() const;

    RaycastHit raycast(float startX, float startY, float dirX, float dirY,
                       float maxDist,
                       CollisionLayer layerMask = COLLISION_LAYER_ALL,
                       CollisionLayer ignoreLayer = 0,
                       entt::entity ignoreEntity = entt::null);

    std::vector<OverlapResult> overlapBox(float centerX, float centerY,
                                           float halfW, float halfH,
                                           CollisionLayer layerMask = COLLISION_LAYER_ALL);

    std::vector<entt::entity> overlapCircle(float centerX, float centerY,
                                              float radius,
                                              CollisionLayer layerMask = COLLISION_LAYER_ALL);

    b2WorldId worldId() const { return worldId_; }

private:
    void onPhysicsPhase(float dt) override { update(dt); }

    void createBox2DBody(entt::entity e);
    void destroyBox2DBody(entt::entity e);
    void createBox2DShape(entt::entity e);
    void buildTileMapChain(entt::entity e);
    void destroyTileMapChain(entt::entity e);

    void pollBodyEvents();
    void pollContactEvents();
    void pollSensorEvents();

    void onTransformUpdated(entt::registry& reg, entt::entity e);
    void onColliderAdded(entt::registry& reg, entt::entity e);
    void onColliderRemoved(entt::registry& reg, entt::entity e);
    void onRigidBodyAdded(entt::registry& reg, entt::entity e);
    void onRigidBodyRemoved(entt::registry& reg, entt::entity e);
    void onTileMapAdded(entt::registry& reg, entt::entity e);
    void onTileMapUpdated(entt::registry& reg, entt::entity e);
    void onTileMapRemoved(entt::registry& reg, entt::entity e);

    entt::registry&   world_;
    entt::dispatcher& dispatcher_;
    b2WorldId         worldId_ = b2_nullWorldId;

    float gravityX_ = 0.0f;
    float gravityY_ = 0.0f;
    float fixedTimestep_ = 1.0f / 60.0f;
    float accumulator_ = 0.0f;
    bool  steppingPhysics_ = false;
    bool  variableTimestep_ = false;

    entt::connection transformUpdateConnection_;
};

} // namespace engine
