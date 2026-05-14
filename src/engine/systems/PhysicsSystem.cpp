#include "PhysicsSystem.h"
#include "../components/RenderComponents.h"
#include "../components/PhysicsComponents.h"
#include <cmath>
#include <vector>
#include <algorithm>

namespace engine {

PhysicsSystem::PhysicsSystem(entt::registry& world, entt::dispatcher& dispatcher)
    : world_(world), dispatcher_(dispatcher) {
}

void PhysicsSystem::init() {
    transformUpdateConnection_ =
        world_.on_update<Transform>().connect<&PhysicsSystem::onTransformUpdated>(this);

    world_.on_construct<Collider>().connect<&PhysicsSystem::onColliderAdded>(this);
    world_.on_destroy<Collider>().connect<&PhysicsSystem::onColliderRemoved>(this);
    world_.on_construct<RigidBody>().connect<&PhysicsSystem::onRigidBodyAdded>(this);
    world_.on_destroy<RigidBody>().connect<&PhysicsSystem::onRigidBodyRemoved>(this);

    world_.on_construct<TileMap>().connect<&PhysicsSystem::onTileMapAdded>(this);
    world_.on_update<TileMap>().connect<&PhysicsSystem::onTileMapUpdated>(this);
    world_.on_destroy<TileMap>().connect<&PhysicsSystem::onTileMapRemoved>(this);

    // 注册接触回调桥接
    physicsWorld_.setContactCallback(
        [this](const PhysicsWorld2D::CollisionPair& pair) {
            onPhysicsContact(pair);
        });
}

void PhysicsSystem::shutdown() {
    transformUpdateConnection_.release();
    physicsWorld_.setContactCallback(nullptr);
}

// ── 公共 API ────────────────────────────────────────────────────────────────

void PhysicsSystem::setGravity(float x, float y)  { physicsWorld_.setGravity(x, y); }
float PhysicsSystem::gravityX() const             { return physicsWorld_.gravityX(); }
float PhysicsSystem::gravityY() const             { return physicsWorld_.gravityY(); }

void PhysicsSystem::setFixedTimestep(float step)  { fixedTimestep_ = step; }
float PhysicsSystem::fixedTimestep() const        { return fixedTimestep_; }
float PhysicsSystem::accumulatorSeconds() const   { return accumulator_; }

void PhysicsSystem::setVariableTimestep(bool e)   { variableTimestep_ = e; }
bool PhysicsSystem::variableTimestep() const      { return variableTimestep_; }

float PhysicsSystem::interpolationAlpha() const {
    if (fixedTimestep_ <= 0.f) return 0.f;
    return std::clamp(accumulator_ / fixedTimestep_, 0.f, 1.f);
}

// ── 主更新 ──────────────────────────────────────────────────────────────────

void PhysicsSystem::update(float dt) {
    if (variableTimestep_) {
        snapshotInterpolatedBodiesForStep();
        steppingPhysics_ = true;
        syncBodiesToWorld();
        physicsWorld_.integrateVelocities(dt);
        physicsWorld_.resolveCollisions();
        syncResultsToEntities();
        resolveTileCollisions();
        steppingPhysics_ = false;
        // Snap previous to current for direct display
        auto snapView = world_.view<Transform, RigidBody>();
        for (auto [e, tf, rb] : snapView.each()) {
            (void)rb;
            if (auto* interpolation = world_.try_get<TransformInterpolation>(e)) {
                interpolation->previous = tf;
            }
        }
        accumulator_ = 0.f;
        return;
    }

    accumulator_ += dt;
    while (accumulator_ >= fixedTimestep_) {
        snapshotInterpolatedBodiesForStep();
        steppingPhysics_ = true;
        syncBodiesToWorld();
        physicsWorld_.step(fixedTimestep_);
        syncResultsToEntities();
        steppingPhysics_ = false;
        resolveTileCollisions();
        accumulator_ -= fixedTimestep_;
    }
}

// ── ECS ↔ PhysicsWorld2D 同步 ───────────────────────────────────────────────

PhysicsWorld2D::ShapeDef PhysicsSystem::colliderToShapeDef(const Collider& col) {
    PhysicsWorld2D::ShapeDef def;
    def.shapeType = col.shapeType;
    def.width     = col.width;
    def.height    = col.height;
    def.radius    = col.radius;
    def.capsuleLength = col.capsuleLength;
    def.offsetX   = col.offsetX;
    def.offsetY   = col.offsetY;
    def.isTrigger = col.isTrigger;
    def.layer     = col.layer;
    def.mask      = col.mask;
    return def;
}

PhysicsWorld2D::BodyDef PhysicsSystem::rigidBodyToBodyDef(const Transform& tf, const RigidBody& rb) {
    PhysicsWorld2D::BodyDef def;
    def.type         = rb.isKinematic ? BodyType::Kinematic : rb.type;
    def.x            = tf.x;
    def.y            = tf.y;
    def.velocityX    = rb.velocityX;
    def.velocityY    = rb.velocityY;
    def.gravityScale = rb.gravityScale;
    def.mass         = rb.mass;
    def.bounciness   = rb.bounciness;
    def.friction     = rb.friction;
    def.ccdEnabled   = rb.ccdEnabled;
    def.contactMargin = rb.contactMargin;
    return def;
}

void PhysicsSystem::ensureBodyForEntity(entt::entity e, Transform& tf, RigidBody* rb, Collider& col) {
    auto& mapping = entityToBody_[e];
    if (mapping.bodyId != PhysicsWorld2D::INVALID_BODY) {
        // Already exists — update
        physicsWorld_.setBodyTransform(mapping.bodyId, tf.x, tf.y);
        if (rb) {
            physicsWorld_.setBodyVelocity(mapping.bodyId, rb->velocityX, rb->velocityY);
            physicsWorld_.setBodyType(mapping.bodyId,
                rb->isKinematic ? BodyType::Kinematic : rb->type);
            physicsWorld_.setBodyGravityScale(mapping.bodyId, rb->gravityScale);
            physicsWorld_.setBodyMass(mapping.bodyId, rb->mass);
        }
        physicsWorld_.clearShapes(mapping.bodyId);
        physicsWorld_.addShape(mapping.bodyId, colliderToShapeDef(col));
        return;
    }

    // Create new body
    PhysicsWorld2D::BodyDef bodyDef;
    if (rb) {
        bodyDef = rigidBodyToBodyDef(tf, *rb);
    } else {
        bodyDef.type = BodyType::Static;
        bodyDef.x = tf.x;
        bodyDef.y = tf.y;
    }

    PhysicsWorld2D::BodyId id = physicsWorld_.createBody(bodyDef);
    physicsWorld_.addShape(id, colliderToShapeDef(col));

    mapping.bodyId = id;
    if (static_cast<size_t>(id) >= bodyToEntity_.size()) {
        bodyToEntity_.resize(id + 1, entt::null);
    }
    bodyToEntity_[id] = e;
}

void PhysicsSystem::removeBodyForEntity(entt::entity e) {
    auto it = entityToBody_.find(e);
    if (it != entityToBody_.end()) {
        physicsWorld_.destroyBody(it->second.bodyId);
        entityToBody_.erase(it);
    }
}

void PhysicsSystem::syncBodiesToWorld() {
    // 收集所有当前有 Collider 的实体
    entt::sparse_set current;
    auto view = world_.view<Transform, Collider>();
    for (auto e : view) {
        current.push(e);
    }

    // 移除不再有 Collider 的实体
    for (auto it = entityToBody_.begin(); it != entityToBody_.end(); ) {
        if (!current.contains(it->first)) {
            physicsWorld_.destroyBody(it->second.bodyId);
            it = entityToBody_.erase(it);
        } else {
            ++it;
        }
    }

    // 确保所有当前实体有对应的 body
    for (auto e : view) {
        auto& tf = view.get<Transform>(e);
        auto& col = view.get<Collider>(e);
        auto* rb = world_.try_get<RigidBody>(e);
        ensureBodyForEntity(e, tf, rb, col);
    }
}

void PhysicsSystem::syncResultsToEntities() {
    for (auto& [e, mapping] : entityToBody_) {
        if (mapping.bodyId == PhysicsWorld2D::INVALID_BODY) continue;

        float x, y, vx, vy;
        physicsWorld_.getBodyState(mapping.bodyId, x, y, vx, vy);

        auto* tf = world_.try_get<Transform>(e);
        if (tf) {
            tf->x = x;
            tf->y = y;
            world_.patch<Transform>(e);
        }

        auto* rb = world_.try_get<RigidBody>(e);
        if (rb) {
            rb->velocityX = vx;
            rb->velocityY = vy;
        }
    }
}

// ── 接触回调桥接 ────────────────────────────────────────────────────────────

void PhysicsSystem::onPhysicsContact(const PhysicsWorld2D::CollisionPair& pair) {
    entt::entity self = entt::null;
    entt::entity other = entt::null;

    if (pair.bodyA > 0 && static_cast<size_t>(pair.bodyA) < bodyToEntity_.size())
        self = bodyToEntity_[pair.bodyA];
    if (pair.bodyB > 0 && static_cast<size_t>(pair.bodyB) < bodyToEntity_.size())
        other = bodyToEntity_[pair.bodyB];

    if (self == entt::null || other == entt::null) {
        std::printf("[Physics] contact IGNORED: bodyA=%d→valid=%d bodyB=%d→valid=%d trigger=%d\n",
                    pair.bodyA, self != entt::null,
                    pair.bodyB, other != entt::null,
                    pair.isTriggerPair);
        return;
    }

    std::printf("[Physics] contact: e%d↔e%d trigger=%d overlap=%.1f\n",
                static_cast<int>(self), static_cast<int>(other),
                pair.isTriggerPair, pair.overlap);

    CollisionInfo info;
    info.self     = self;
    info.other    = other;
    info.normalX  = pair.normalX;
    info.normalY  = pair.normalY;
    info.overlapX = pair.normalX * pair.overlap;
    info.overlapY = pair.normalY * pair.overlap;
    info.state    = pair.state;

    dispatcher_.trigger(info);
}

// ── 插值快照 ────────────────────────────────────────────────────────────────

void PhysicsSystem::snapshotInterpolatedBodiesForStep() {
    auto view = world_.view<Transform, RigidBody>();
    for (auto [e, tf, rb] : view.each()) {
        auto& interpolation = world_.get_or_emplace<TransformInterpolation>(e);
        interpolation.previous = tf;
        interpolation.initialized = true;
        interpolation.disabled = !rb.interpolate;
    }
}

// ── ECS 事件钩子 ────────────────────────────────────────────────────────────

void PhysicsSystem::onTransformUpdated(entt::registry& reg, entt::entity e) {
    if (steppingPhysics_) {
        // 物理 step 期间 Transform 被 patch 了（如碰撞回调中移动 entity）
        // 立即同步到 PhysicsWorld2D，否则后续 syncResultsToEntities 会用旧位置覆盖
        auto it = entityToBody_.find(e);
        if (it != entityToBody_.end() && it->second.bodyId != PhysicsWorld2D::INVALID_BODY) {
            auto* tf = reg.try_get<Transform>(e);
            if (tf) physicsWorld_.setBodyTransform(it->second.bodyId, tf->x, tf->y);
        }
        return;
    }
    if (!reg.all_of<RigidBody, Transform>(e)) return;

    auto& interpolation = reg.get_or_emplace<TransformInterpolation>(e);
    interpolation.previous = reg.get<Transform>(e);
    interpolation.initialized = true;
}

void PhysicsSystem::onColliderAdded(entt::registry& reg, entt::entity e) {
    // Body creation deferred to next syncBodiesToWorld()
    (void)reg; (void)e;
}

void PhysicsSystem::onColliderRemoved(entt::registry& reg, entt::entity e) {
    (void)reg;
    removeBodyForEntity(e);
}

void PhysicsSystem::onRigidBodyAdded(entt::registry& reg, entt::entity e) {
    // Body type change deferred to next syncBodiesToWorld()
    (void)reg; (void)e;
}

void PhysicsSystem::onRigidBodyRemoved(entt::registry& reg, entt::entity e) {
    // If entity still has Collider, it becomes Static — deferred to sync
    (void)reg; (void)e;
}

// ── Tile 碰撞缓存 ───────────────────────────────────────────────────────────

void PhysicsSystem::rebuildTileCollisionCache(entt::entity mapEntity, const TileMap& tmap) {
    TileCollisionCache cache;
    cache.width = tmap.width;
    cache.height = tmap.height;
    cache.tileSize = static_cast<float>(tmap.tileSize);
    cache.valid = true;

    cache.grid.resize(tmap.height);
    for (int ty = 0; ty < tmap.height; ++ty) {
        cache.grid[ty].resize(tmap.width);
        for (int tx = 0; tx < tmap.width; ++tx) {
            auto& entry = cache.grid[ty][tx];

            TileMap::TileCollision collision;
            collision.shape = TileMap::TileCollisionShape::None;
            for (int layer = 0; layer < static_cast<int>(tmap.layers.size()); ++layer) {
                collision = tmap.collisionAt(layer, tx, ty);
                if (collision.shape != TileMap::TileCollisionShape::None) break;
            }

            entry.shape = static_cast<uint8_t>(collision.shape);
            entry.isTrigger = (collision.shape == TileMap::TileCollisionShape::Trigger);

            if (collision.shape == TileMap::TileCollisionShape::None) {
                entry.localMinX = entry.localMinY = entry.localMaxX = entry.localMaxY = 0.f;
                continue;
            }

            const float ts = cache.tileSize;
            const float tx_f = static_cast<float>(tx);
            const float ty_f = static_cast<float>(ty);

            if (collision.shape == TileMap::TileCollisionShape::Rect &&
                collision.points.size() >= 4) {
                entry.localMinX = tx_f * ts + collision.points[0];
                entry.localMinY = ty_f * ts + collision.points[1];
                entry.localMaxX = entry.localMinX + collision.points[2];
                entry.localMaxY = entry.localMinY + collision.points[3];
            }
            else if ((collision.shape == TileMap::TileCollisionShape::Polygon ||
                      collision.shape == TileMap::TileCollisionShape::OneWay) &&
                      collision.points.size() >= 4) {
                float minX = tx_f * ts + collision.points[0];
                float maxX = minX;
                float minY = ty_f * ts + collision.points[1];
                float maxY = minY;
                for (size_t pi = 2; pi + 1 < collision.points.size(); pi += 2) {
                    minX = std::min(minX, tx_f * ts + collision.points[pi]);
                    maxX = std::max(maxX, tx_f * ts + collision.points[pi]);
                    minY = std::min(minY, ty_f * ts + collision.points[pi + 1]);
                    maxY = std::max(maxY, ty_f * ts + collision.points[pi + 1]);
                }
                entry.localMinX = minX;
                entry.localMinY = minY;
                entry.localMaxX = maxX;
                entry.localMaxY = maxY;
            }
            else {
                entry.localMinX = tx_f * ts;
                entry.localMinY = ty_f * ts;
                entry.localMaxX = entry.localMinX + ts;
                entry.localMaxY = entry.localMinY + ts;
            }
        }
    }

    tileCollisionCaches_[mapEntity] = std::move(cache);
}

void PhysicsSystem::onTileMapAdded(entt::registry& reg, entt::entity e) {
    if (!reg.all_of<TileMap>(e)) return;
    rebuildTileCollisionCache(e, reg.get<TileMap>(e));
}

void PhysicsSystem::onTileMapUpdated(entt::registry& reg, entt::entity e) {
    if (!reg.all_of<TileMap>(e)) return;
    rebuildTileCollisionCache(e, reg.get<TileMap>(e));
}

void PhysicsSystem::onTileMapRemoved(entt::registry& reg, entt::entity e) {
    (void)reg;
    tileCollisionCaches_.erase(e);
}

// ── AABB 辅助 ───────────────────────────────────────────────────────────────

PhysicsWorld2D::AABB PhysicsSystem::makeColliderAABB(const Transform& tf, const Collider& col) const {
    // Simple AABB without sprite — used for pure physics entities
    auto def = colliderToShapeDef(col);
    return PhysicsWorld2D::computeAABB(tf.x, tf.y, def);
}

PhysicsWorld2D::AABB PhysicsSystem::makeEntityAABB(entt::entity e) const {
    const Transform& tf = world_.get<Transform>(e);
    const Collider& col = world_.get<Collider>(e);

    auto def = colliderToShapeDef(col);

    if (const Sprite* sprite = world_.try_get<Sprite>(e)) {
        const float spriteW = sprite->srcRect.w * std::abs(tf.scaleX);
        const float spriteH = sprite->srcRect.h * std::abs(tf.scaleY);
        return PhysicsWorld2D::computeAABB(
            tf.x, tf.y, def,
            &spriteW, &spriteH,
            tf.scaleX, tf.scaleY,
            sprite->pivotX, sprite->pivotY);
    }

    return makeColliderAABB(tf, col);
}

// ── Tile 碰撞解析 ───────────────────────────────────────────────────────────

void PhysicsSystem::resolveTileCollisions() {
    if (tileCollisionCaches_.empty()) return;

    auto actors = world_.view<Transform, Collider, RigidBody>();
    for (auto [actor, tf, col, rb] : actors.each()) {
        if (col.isTrigger || rb.isKinematic) continue;

        // 简化的碰撞检测：tileCollider 与 actor 的碰撞
        // tile 始终是 STATIC 层
        if ((col.layer & COLLISION_LAYER_STATIC) == 0 &&
            (COLLISION_LAYER_STATIC & col.mask) == 0) continue;

        PhysicsWorld2D::AABB actorBox = makeEntityAABB(actor);

        for (auto& [mapEntity, cache] : tileCollisionCaches_) {
            if (!cache.valid) continue;
            const Transform* mapTf = world_.try_get<Transform>(mapEntity);
            if (!mapTf) continue;

            const float ts = cache.tileSize;
            if (ts <= 0.f) continue;

            int minTileX = static_cast<int>(std::floor((actorBox.minX - mapTf->x) / ts));
            int maxTileX = static_cast<int>(std::floor((actorBox.maxX - mapTf->x) / ts));
            int minTileY = static_cast<int>(std::floor((actorBox.minY - mapTf->y) / ts));
            int maxTileY = static_cast<int>(std::floor((actorBox.maxY - mapTf->y) / ts));

            minTileX = std::max(0, minTileX);
            minTileY = std::max(0, minTileY);
            maxTileX = std::min(cache.width - 1, maxTileX);
            maxTileY = std::min(cache.height - 1, maxTileY);
            if (minTileX > maxTileX || minTileY > maxTileY) continue;

            for (int ty = minTileY; ty <= maxTileY; ++ty) {
                for (int tx = minTileX; tx <= maxTileX; ++tx) {
                    const auto& entry = cache.grid[ty][tx];
                    if (entry.shape == static_cast<uint8_t>(TileMap::TileCollisionShape::None))
                        continue;

                    PhysicsWorld2D::AABB tileBox{
                        mapTf->x + entry.localMinX,
                        mapTf->y + entry.localMinY,
                        mapTf->x + entry.localMaxX,
                        mapTf->y + entry.localMaxY
                    };

                    if (!PhysicsWorld2D::overlaps(actorBox, tileBox)) continue;

                    float sepX = 0.f, sepY = 0.f;
                    PhysicsWorld2D::minSeparation(actorBox, tileBox, sepX, sepY);

                    dispatcher_.trigger(CollisionInfo{actor, mapEntity, sepX, sepY});
                    dispatcher_.trigger(CollisionInfo{mapEntity, actor, -sepX, -sepY});

                    if (entry.isTrigger) continue;

                    tf.x += sepX;
                    tf.y += sepY;
                    world_.patch<Transform>(actor);

                    if (sepX != 0.f && rb.velocityX * sepX < 0.f) rb.velocityX = 0.f;
                    if (sepY != 0.f && rb.velocityY * sepY < 0.f) rb.velocityY = 0.f;

                    actorBox.minX += sepX; actorBox.maxX += sepX;
                    actorBox.minY += sepY; actorBox.maxY += sepY;

                    // 同步回 PhysicsWorld2D 以供下帧 CCD
                    auto mit = entityToBody_.find(actor);
                    if (mit != entityToBody_.end()) {
                        physicsWorld_.setBodyTransform(mit->second.bodyId, tf.x, tf.y);
                    }
                }
            }
        }
    }
}

// ── 空间查询 ────────────────────────────────────────────────────────────────

RaycastHit PhysicsSystem::raycast(float startX, float startY, float dirX, float dirY,
                                   float maxDist, CollisionLayer layerMask,
                                   CollisionLayer ignoreLayer, entt::entity ignoreEntity) {
    PhysicsWorld2D::BodyId ignoreBody = PhysicsWorld2D::INVALID_BODY;
    if (ignoreEntity != entt::null) {
        auto it = entityToBody_.find(ignoreEntity);
        if (it != entityToBody_.end()) ignoreBody = it->second.bodyId;
    }

    auto result = physicsWorld_.raycast(startX, startY, dirX, dirY, maxDist,
                                         layerMask, ignoreLayer, ignoreBody);

    RaycastHit hit;
    hit.hit      = result.hit;
    hit.distance = result.distance;
    hit.hitX     = result.hitX;
    hit.hitY     = result.hitY;
    hit.normalX  = result.normalX;
    hit.normalY  = result.normalY;
    if (result.hit && result.bodyId > 0 &&
        static_cast<size_t>(result.bodyId) < bodyToEntity_.size()) {
        hit.entity = bodyToEntity_[result.bodyId];
    } else {
        hit.entity = entt::null;
    }
    return hit;
}

std::vector<OverlapResult> PhysicsSystem::overlapBox(float centerX, float centerY,
                                                      float halfW, float halfH,
                                                      CollisionLayer layerMask) {
    auto worldResults = physicsWorld_.overlapRect(centerX, centerY, halfW, halfH, layerMask);

    std::vector<OverlapResult> results;
    results.reserve(worldResults.size());
    for (auto& wr : worldResults) {
        OverlapResult r;
        if (wr.bodyId > 0 && static_cast<size_t>(wr.bodyId) < bodyToEntity_.size()) {
            r.entity = bodyToEntity_[wr.bodyId];
        } else {
            r.entity = entt::null;
        }
        r.overlapX = wr.overlapX;
        r.overlapY = wr.overlapY;
        results.push_back(r);
    }
    return results;
}

std::vector<entt::entity> PhysicsSystem::overlapCircle(float centerX, float centerY,
                                                        float radius,
                                                        CollisionLayer layerMask) {
    auto worldResults = physicsWorld_.overlapCircle(centerX, centerY, radius, layerMask);

    std::vector<entt::entity> results;
    results.reserve(worldResults.size());
    for (auto id : worldResults) {
        if (id > 0 && static_cast<size_t>(id) < bodyToEntity_.size()) {
            results.push_back(bodyToEntity_[id]);
        }
    }
    return results;
}

} // namespace engine
