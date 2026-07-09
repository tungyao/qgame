#pragma once
#include <entt/entt.hpp>
#include <cstdint>
#include "box2d/box2d.h"

namespace engine {

using CollisionLayer = uint64_t;

constexpr CollisionLayer COLLISION_LAYER_DEFAULT = 1;
constexpr CollisionLayer COLLISION_LAYER_STATIC  = 2;
constexpr CollisionLayer COLLISION_LAYER_PLAYER  = 4;
constexpr CollisionLayer COLLISION_LAYER_ENEMY   = 8;
constexpr CollisionLayer COLLISION_LAYER_ALL     = 0xFFFFFFFFFFFFFFFF;

enum class BodyType : uint8_t {
    Static    = 0,
    Kinematic = 1,
    Dynamic   = 2
};

enum class ShapeType : uint8_t {
    Box     = 0,
    Circle  = 1,
    Capsule = 2
};

enum class ContactState : uint8_t {
    Begin   = 0,
    Persist = 1,
    End     = 2
};

struct RigidBody {
    BodyType type         = BodyType::Dynamic;
    float    gravityScale = 1.0f;
    bool     enabled      = true;
    bool     freezeRotation = false;
    b2BodyId bodyId       = b2_nullBodyId;
};

struct Collider {
    ShapeType shapeType = ShapeType::Box;
    float width  = 1.0f;
    float height = 1.0f;
    float radius = 0.5f;
    float density     = 1.0f;
    float friction    = 0.3f;
    float restitution = 0.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    bool  isTrigger = false;
    CollisionLayer layer = COLLISION_LAYER_DEFAULT;
    CollisionLayer mask  = COLLISION_LAYER_ALL;
    b2ShapeId shapeId = b2_nullShapeId;
};

struct TileMapCollider {
    b2ChainId     chainId = b2_nullChainId;
    float         friction = 0.3f;
    CollisionLayer layer = COLLISION_LAYER_STATIC;
    CollisionLayer mask  = COLLISION_LAYER_ALL;
};

struct CollisionInfo {
    entt::entity self;
    entt::entity other;
    float overlapX    = 0.0f;
    float overlapY    = 0.0f;
    float normalX     = 0.0f;
    float normalY     = 0.0f;
    float contactX    = 0.0f;
    float contactY    = 0.0f;
    float approachSpeed = 0.0f;
    ContactState state = ContactState::Begin;
};

struct RaycastHit {
    entt::entity entity;
    float hitX, hitY;
    float normalX, normalY;
    float distance;
    bool  hit;
};

struct OverlapResult {
    entt::entity entity;
};

} // namespace engine
