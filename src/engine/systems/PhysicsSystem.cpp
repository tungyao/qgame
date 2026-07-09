#include "PhysicsSystem.h"
#include "../components/RenderComponents.h"
#include <algorithm>
#include <cmath>

namespace engine {

static entt::entity getEntityFromBody(b2BodyId bodyId) {
    void* ud = b2Body_GetUserData(bodyId);
    return userDataToEntity(ud);
}

struct RaycastCtx {
    entt::entity ignoreEntity;
    CollisionLayer ignoreLayer;
    float origDirX, origDirY;
    RaycastHit* out;
    bool found;
};

static float raycastFilter(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal,
                            float fraction, void* context) {
    auto* ctx = static_cast<RaycastCtx*>(context);
    b2BodyId bodyId = b2Shape_GetBody(shapeId);
    entt::entity e = getEntityFromBody(bodyId);
    if (e == entt::null || e == ctx->ignoreEntity) return -1.0f;

    b2Filter filter = b2Shape_GetFilter(shapeId);
    if (filter.categoryBits & ctx->ignoreLayer) return -1.0f;

    ctx->found = true;
    ctx->out->entity = e;
    ctx->out->hitX = toPixels(point.x);
    ctx->out->hitY = toPixels(point.y);
    ctx->out->normalX = normal.x;
    ctx->out->normalY = normal.y;
    ctx->out->distance = fraction * std::sqrt(ctx->origDirX * ctx->origDirX + ctx->origDirY * ctx->origDirY);
    ctx->out->hit = true;
    return fraction;
}

struct OverlapCtx {
    std::vector<entt::entity>* results;
    CollisionLayer ignoreLayer;
    entt::entity ignoreEntity;
};

static bool overlapFilter(b2ShapeId shapeId, void* context) {
    auto* ctx = static_cast<OverlapCtx*>(context);
    b2BodyId bodyId = b2Shape_GetBody(shapeId);
    entt::entity e = getEntityFromBody(bodyId);
    if (e == entt::null || e == ctx->ignoreEntity) return true;

    b2Filter filter = b2Shape_GetFilter(shapeId);
    if (filter.categoryBits & ctx->ignoreLayer) return true;

    ctx->results->push_back(e);
    return true;
}

PhysicsSystem::PhysicsSystem(entt::registry& world, entt::dispatcher& dispatcher)
    : world_(world), dispatcher_(dispatcher) {
}

void PhysicsSystem::init() {
    b2WorldDef worldDef = b2DefaultWorldDef();
    worldDef.gravity = {0.0f, 0.0f};
    worldDef.enableSleep = true;
    worldDef.enableContinuous = true;
    worldId_ = b2CreateWorld(&worldDef);

    transformUpdateConnection_ =
        world_.on_update<Transform>().connect<&PhysicsSystem::onTransformUpdated>(this);

    world_.on_construct<Collider>().connect<&PhysicsSystem::onColliderAdded>(this);
    world_.on_destroy<Collider>().connect<&PhysicsSystem::onColliderRemoved>(this);
    world_.on_construct<RigidBody>().connect<&PhysicsSystem::onRigidBodyAdded>(this);
    world_.on_destroy<RigidBody>().connect<&PhysicsSystem::onRigidBodyRemoved>(this);

    world_.on_construct<TileMap>().connect<&PhysicsSystem::onTileMapAdded>(this);
    world_.on_update<TileMap>().connect<&PhysicsSystem::onTileMapUpdated>(this);
    world_.on_destroy<TileMap>().connect<&PhysicsSystem::onTileMapRemoved>(this);
}

void PhysicsSystem::shutdown() {
    transformUpdateConnection_.release();
    if (B2_IS_NON_NULL(worldId_)) {
        b2DestroyWorld(worldId_);
        worldId_ = b2_nullWorldId;
    }
}

void PhysicsSystem::setGravity(float x, float y) {
    gravityX_ = x;
    gravityY_ = y;
    b2Vec2 g = toMetersVec2(x, y);
    b2World_SetGravity(worldId_, g);
}

float PhysicsSystem::gravityX() const { return gravityX_; }
float PhysicsSystem::gravityY() const { return gravityY_; }

void PhysicsSystem::setFixedTimestep(float step)  { fixedTimestep_ = step; }
float PhysicsSystem::fixedTimestep() const        { return fixedTimestep_; }
float PhysicsSystem::accumulatorSeconds() const   { return accumulator_; }

void PhysicsSystem::setVariableTimestep(bool e)   { variableTimestep_ = e; }
bool PhysicsSystem::variableTimestep() const      { return variableTimestep_; }

float PhysicsSystem::interpolationAlpha() const {
    if (fixedTimestep_ <= 0.0f) return 0.0f;
    return std::clamp(accumulator_ / fixedTimestep_, 0.0f, 1.0f);
}

void PhysicsSystem::update(float dt) {
    if (!B2_IS_NON_NULL(worldId_)) return;

    if (variableTimestep_) {
        b2World_Step(worldId_, dt, SUB_STEP_COUNT);
        pollBodyEvents();
        pollContactEvents();
        pollSensorEvents();
        auto view = world_.view<Transform, RigidBody>();
        for (auto [e, tf, rb] : view.each()) {
            (void)rb;
            if (auto* interp = world_.try_get<TransformInterpolation>(e))
                interp->previous = tf;
        }
        accumulator_ = 0.0f;
        return;
    }

    accumulator_ += dt;
    while (accumulator_ >= fixedTimestep_) {
        auto interpView = world_.view<Transform, RigidBody>();
        for (auto [e, tf, rb] : interpView.each()) {
            (void)rb;
            auto& interp = world_.get_or_emplace<TransformInterpolation>(e);
            interp.previous = tf;
            interp.initialized = true;
            interp.disabled = false;
        }

        b2World_Step(worldId_, fixedTimestep_, SUB_STEP_COUNT);

        pollBodyEvents();
        pollContactEvents();
        pollSensorEvents();

        accumulator_ -= fixedTimestep_;
    }
}

void PhysicsSystem::createBox2DBody(entt::entity e) {
    auto& rb = world_.get<RigidBody>(e);
    const auto& tf = world_.get<Transform>(e);

    if (B2_IS_NON_NULL(rb.bodyId)) return;

    b2BodyDef bodyDef = b2DefaultBodyDef();
    switch (rb.type) {
        case BodyType::Static:    bodyDef.type = b2_staticBody; break;
        case BodyType::Kinematic: bodyDef.type = b2_kinematicBody; break;
        case BodyType::Dynamic:   bodyDef.type = b2_dynamicBody; break;
    }
    bodyDef.position = toMetersVec2(tf.x, tf.y);
    bodyDef.gravityScale = rb.gravityScale;
    bodyDef.fixedRotation = rb.freezeRotation;
    bodyDef.isEnabled = rb.enabled;
    bodyDef.userData = entityToUserData(e);

    steppingPhysics_ = true;
    rb.bodyId = b2CreateBody(worldId_, &bodyDef);
    steppingPhysics_ = false;

    if (world_.all_of<Collider>(e))
        createBox2DShape(e);
}

void PhysicsSystem::destroyBox2DBody(entt::entity e) {
    auto& rb = world_.get<RigidBody>(e);
    if (B2_IS_NON_NULL(rb.bodyId)) {
        auto* col = world_.try_get<Collider>(e);
        if (col)
            col->shapeId = b2_nullShapeId;
        steppingPhysics_ = true;
        b2DestroyBody(rb.bodyId);
        steppingPhysics_ = false;
        rb.bodyId = b2_nullBodyId;
    }
}

void PhysicsSystem::createBox2DShape(entt::entity e) {
    auto& rb = world_.get<RigidBody>(e);
    auto& col = world_.get<Collider>(e);

    if (!B2_IS_NON_NULL(rb.bodyId)) return;
    if (B2_IS_NON_NULL(col.shapeId)) return;

    b2ShapeDef shapeDef = b2DefaultShapeDef();
    shapeDef.density = col.density;
    shapeDef.material.friction = col.friction;
    shapeDef.material.restitution = col.restitution;
    shapeDef.isSensor = col.isTrigger;
    shapeDef.filter.categoryBits = col.layer;
    shapeDef.filter.maskBits = col.mask;
    shapeDef.enableContactEvents = true;

    b2Vec2 offset = toMetersVec2(col.offsetX, col.offsetY);

    steppingPhysics_ = true;
    switch (col.shapeType) {
        case ShapeType::Box: {
            float hw = toMeters(col.width * 0.5f);
            float hh = toMeters(col.height * 0.5f);
            b2Polygon poly = b2MakeOffsetBox(hw, hh, offset, b2Rot_identity);
            col.shapeId = b2CreatePolygonShape(rb.bodyId, &shapeDef, &poly);
            break;
        }
        case ShapeType::Circle: {
            b2Circle circle;
            circle.center = offset;
            circle.radius = toMeters(col.radius);
            col.shapeId = b2CreateCircleShape(rb.bodyId, &shapeDef, &circle);
            break;
        }
        case ShapeType::Capsule: {
            float r = toMeters(col.radius);
            b2Capsule capsule;
            capsule.center1 = {offset.x - r, offset.y};
            capsule.center2 = {offset.x + r, offset.y};
            capsule.radius = r;
            col.shapeId = b2CreateCapsuleShape(rb.bodyId, &shapeDef, &capsule);
            break;
        }
    }
    steppingPhysics_ = false;
}

void PhysicsSystem::buildTileMapChain(entt::entity e) {
    auto& tmap = world_.get<TileMap>(e);
    auto& tmc = world_.get_or_emplace<TileMapCollider>(e);

    if (B2_IS_NON_NULL(tmc.chainId)) {
        b2DestroyChain(tmc.chainId);
        tmc.chainId = b2_nullChainId;
    }

    const float ts = static_cast<float>(tmap.tileSize);
    const float halfTs = ts * 0.5f;

    std::vector<b2Vec2> points;
    points.reserve(static_cast<size_t>(tmap.width) * static_cast<size_t>(tmap.height));

    for (int ty = 0; ty < tmap.height; ++ty) {
        for (int tx = 0; tx < tmap.width; ++tx) {
            bool solid = false;
            for (int layer = 0; layer < static_cast<int>(tmap.layers.size()); ++layer) {
                auto col = tmap.collisionAt(layer, tx, ty);
                if (col.shape != TileMap::TileCollisionShape::None &&
                    col.shape != TileMap::TileCollisionShape::Trigger) {
                    solid = true;
                    break;
                }
            }
            if (solid) {
                float cx = static_cast<float>(tx) * ts + halfTs;
                float cy = static_cast<float>(ty) * ts + halfTs;
                points.push_back(toMetersVec2(cx, cy));
            }
        }
    }

    if (points.size() < 4) return;

    b2BodyDef bodyDef = b2DefaultBodyDef();
    bodyDef.type = b2_staticBody;
    bodyDef.userData = entityToUserData(e);
    b2BodyId bodyId = b2CreateBody(worldId_, &bodyDef);

    b2SurfaceMaterial material = b2DefaultSurfaceMaterial();
    material.friction = tmc.friction;

    b2ChainDef chainDef = b2DefaultChainDef();
    chainDef.points = points.data();
    chainDef.count = static_cast<int>(points.size());
    chainDef.materials = &material;
    chainDef.materialCount = 1;
    chainDef.isLoop = false;
    chainDef.filter.categoryBits = tmc.layer;
    chainDef.filter.maskBits = tmc.mask;

    tmc.chainId = b2CreateChain(bodyId, &chainDef);
}

void PhysicsSystem::destroyTileMapChain(entt::entity e) {
    auto* tmc = world_.try_get<TileMapCollider>(e);
    if (!tmc) return;
    if (B2_IS_NON_NULL(tmc->chainId)) {
        b2DestroyChain(tmc->chainId);
        tmc->chainId = b2_nullChainId;
    }
}

void PhysicsSystem::pollBodyEvents() {
    b2BodyEvents events = b2World_GetBodyEvents(worldId_);
    for (int i = 0; i < events.moveCount; ++i) {
        const auto& ev = events.moveEvents[i];
        entt::entity e = getEntityFromBody(ev.bodyId);
        if (e == entt::null) continue;

        auto* tf = world_.try_get<Transform>(e);
        if (!tf) continue;

        steppingPhysics_ = true;
        tf->x = toPixels(ev.transform.p.x);
        tf->y = toPixels(ev.transform.p.y);
        tf->rotation = toDegrees(b2Rot_GetAngle(ev.transform.q));
        world_.patch<Transform>(e);
        steppingPhysics_ = false;

        auto* rb = world_.try_get<RigidBody>(e);
        if (rb) {
            b2Vec2 v = b2Body_GetLinearVelocity(rb->bodyId);
            (void)v;
        }
    }
}

void PhysicsSystem::pollContactEvents() {
    b2ContactEvents events = b2World_GetContactEvents(worldId_);

    for (int i = 0; i < events.beginCount; ++i) {
        const auto& ev = events.beginEvents[i];
        entt::entity eA = userDataToEntity(b2Body_GetUserData(b2Shape_GetBody(ev.shapeIdA)));
        entt::entity eB = userDataToEntity(b2Body_GetUserData(b2Shape_GetBody(ev.shapeIdB)));
        if (eA == entt::null || eB == entt::null) continue;

        CollisionInfo info;
        info.self = eA;
        info.other = eB;
        info.state = ContactState::Begin;
        dispatcher_.trigger(info);
    }

    for (int i = 0; i < events.endCount; ++i) {
        const auto& ev = events.endEvents[i];
        if (!b2Shape_IsValid(ev.shapeIdA) || !b2Shape_IsValid(ev.shapeIdB))
            continue;
        entt::entity eA = userDataToEntity(b2Body_GetUserData(b2Shape_GetBody(ev.shapeIdA)));
        entt::entity eB = userDataToEntity(b2Body_GetUserData(b2Shape_GetBody(ev.shapeIdB)));
        if (eA == entt::null || eB == entt::null) continue;

        CollisionInfo info;
        info.self = eA;
        info.other = eB;
        info.state = ContactState::End;
        dispatcher_.trigger(info);
    }

    for (int i = 0; i < events.hitCount; ++i) {
        const auto& ev = events.hitEvents[i];
        entt::entity eA = userDataToEntity(b2Body_GetUserData(b2Shape_GetBody(ev.shapeIdA)));
        entt::entity eB = userDataToEntity(b2Body_GetUserData(b2Shape_GetBody(ev.shapeIdB)));
        if (eA == entt::null || eB == entt::null) continue;

        CollisionInfo info;
        info.self = eA;
        info.other = eB;
        info.normalX = ev.normal.x;
        info.normalY = ev.normal.y;
        info.contactX = toPixels(ev.point.x);
        info.contactY = toPixels(ev.point.y);
        info.approachSpeed = ev.approachSpeed;
        info.state = ContactState::Persist;
        dispatcher_.trigger(info);
    }
}

void PhysicsSystem::pollSensorEvents() {
    b2SensorEvents events = b2World_GetSensorEvents(worldId_);

    for (int i = 0; i < events.beginCount; ++i) {
        const auto& ev = events.beginEvents[i];
        entt::entity eSensor = userDataToEntity(b2Body_GetUserData(b2Shape_GetBody(ev.sensorShapeId)));
        entt::entity eVisitor = userDataToEntity(b2Body_GetUserData(b2Shape_GetBody(ev.visitorShapeId)));
        if (eSensor == entt::null || eVisitor == entt::null) continue;

        CollisionInfo info;
        info.self = eSensor;
        info.other = eVisitor;
        info.state = ContactState::Begin;
        dispatcher_.trigger(info);
    }

    for (int i = 0; i < events.endCount; ++i) {
        const auto& ev = events.endEvents[i];
        if (!b2Shape_IsValid(ev.visitorShapeId)) continue;
        entt::entity eSensor = userDataToEntity(b2Body_GetUserData(b2Shape_GetBody(ev.sensorShapeId)));
        entt::entity eVisitor = userDataToEntity(b2Body_GetUserData(b2Shape_GetBody(ev.visitorShapeId)));
        if (eSensor == entt::null || eVisitor == entt::null) continue;

        CollisionInfo info;
        info.self = eSensor;
        info.other = eVisitor;
        info.state = ContactState::End;
        dispatcher_.trigger(info);
    }
}

void PhysicsSystem::onTransformUpdated(entt::registry& reg, entt::entity e) {
    if (steppingPhysics_) return;
    auto* rb = reg.try_get<RigidBody>(e);
    auto* tf = reg.try_get<Transform>(e);
    if (!rb || !tf) return;
    if (!B2_IS_NON_NULL(rb->bodyId)) return;

    steppingPhysics_ = true;
    b2Vec2 pos = toMetersVec2(tf->x, tf->y);
    b2Rot rot = b2MakeRot(toRadians(tf->rotation));
    b2Body_SetTransform(rb->bodyId, pos, rot);
    steppingPhysics_ = false;
}

void PhysicsSystem::onColliderAdded(entt::registry& reg, entt::entity e) {
    if (!reg.all_of<RigidBody>(e)) return;
    createBox2DShape(e);
}

void PhysicsSystem::onColliderRemoved(entt::registry& reg, entt::entity e) {
    (void)reg;
    auto* col = world_.try_get<Collider>(e);
    if (!col) return;
    if (B2_IS_NON_NULL(col->shapeId)) {
        b2DestroyShape(col->shapeId, true);
        col->shapeId = b2_nullShapeId;
    }
}

void PhysicsSystem::onRigidBodyAdded(entt::registry& reg, entt::entity e) {
    if (!reg.all_of<Collider>(e)) return;
    createBox2DBody(e);
}

void PhysicsSystem::onRigidBodyRemoved(entt::registry& reg, entt::entity e) {
    auto* rb = reg.try_get<RigidBody>(e);
    if (!rb) return;
    if (B2_IS_NON_NULL(rb->bodyId)) {
        auto* col = reg.try_get<Collider>(e);
        if (col)
            col->shapeId = b2_nullShapeId;
        b2DestroyBody(rb->bodyId);
        rb->bodyId = b2_nullBodyId;
    }
}

void PhysicsSystem::onTileMapAdded(entt::registry& reg, entt::entity e) {
    if (!reg.all_of<TileMap>(e)) return;
    buildTileMapChain(e);
}

void PhysicsSystem::onTileMapUpdated(entt::registry& reg, entt::entity e) {
    if (!reg.all_of<TileMap>(e)) return;
    buildTileMapChain(e);
}

void PhysicsSystem::onTileMapRemoved(entt::registry& reg, entt::entity e) {
    (void)reg;
    destroyTileMapChain(e);
}

RaycastHit PhysicsSystem::raycast(float startX, float startY,
                                   float dirX, float dirY,
                                   float maxDist,
                                   CollisionLayer layerMask,
                                   CollisionLayer ignoreLayer,
                                   entt::entity ignoreEntity) {
    RaycastHit hit{};
    hit.hit = false;

    if (!B2_IS_NON_NULL(worldId_) || maxDist <= 0.0f) return hit;

    float len = std::sqrt(dirX * dirX + dirY * dirY);
    if (len <= 0.0f) return hit;

    b2Vec2 origin = toMetersVec2(startX, startY);
    b2Vec2 translation = toMetersVec2(dirX / len * maxDist, dirY / len * maxDist);

    b2QueryFilter filter;
    filter.categoryBits = layerMask;
    filter.maskBits = COLLISION_LAYER_ALL;

    RaycastCtx ctx;
    ctx.ignoreEntity = ignoreEntity;
    ctx.ignoreLayer = ignoreLayer;
    ctx.origDirX = dirX;
    ctx.origDirY = dirY;
    ctx.out = &hit;
    ctx.found = false;

    b2World_CastRay(worldId_, origin, translation, filter, raycastFilter, &ctx);

    return hit;
}

std::vector<OverlapResult> PhysicsSystem::overlapBox(float centerX, float centerY,
                                                      float halfW, float halfH,
                                                      CollisionLayer layerMask) {
    std::vector<OverlapResult> results;
    if (!B2_IS_NON_NULL(worldId_)) return results;

    b2AABB aabb;
    b2Vec2 c = toMetersVec2(centerX, centerY);
    b2Vec2 h = toMetersVec2(halfW, halfH);
    aabb.lowerBound = {c.x - h.x, c.y - h.y};
    aabb.upperBound = {c.x + h.x, c.y + h.y};

    b2QueryFilter filter;
    filter.categoryBits = layerMask;
    filter.maskBits = COLLISION_LAYER_ALL;

    std::vector<entt::entity> entities;

    OverlapCtx ctx;
    ctx.results = &entities;
    ctx.ignoreLayer = 0;
    ctx.ignoreEntity = entt::null;

    b2World_OverlapAABB(worldId_, aabb, filter, overlapFilter, &ctx);

    results.reserve(entities.size());
    for (auto e : entities) {
        OverlapResult r;
        r.entity = e;
        results.push_back(r);
    }

    return results;
}

std::vector<entt::entity> PhysicsSystem::overlapCircle(float centerX, float centerY,
                                                        float radius,
                                                        CollisionLayer layerMask) {
    auto boxResults = overlapBox(centerX, centerY, radius, radius, layerMask);
    std::vector<entt::entity> results;
    results.reserve(boxResults.size());
    for (auto& r : boxResults) {
        results.push_back(r.entity);
    }
    return results;
}

} // namespace engine
