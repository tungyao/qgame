#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <unordered_map>
#include <vector>

#include "ISystem.h"
#include "PhysicsWorld2D.h"
#include "../components/PhysicsComponents.h"
#include "../runtime/TransformInterpolation.h"

namespace engine {

// ── Tile 碰撞缓存 ──────────────────────────────────────────────────────
// 预计算每个 tile 的碰撞 AABB（本地坐标系），跳过每帧 collisionAt() 的 gid→tileset 查找链。
// 在 TileMap 组件 attach/update 时重建，不支持动态 TileCollisionShape 变化。
struct TileCollisionCacheEntry {
    float localMinX, localMinY, localMaxX, localMaxY;
    uint8_t shape;
    bool    isTrigger;
};

struct TileCollisionCache {
    std::vector<std::vector<TileCollisionCacheEntry>> grid;
    int    width = 0;
    int    height = 0;
    float  tileSize = 0.f;
    bool   valid = false;
};

/**
 * 物理系统 — ECS 适配层，委托 PhysicsWorld2D 执行模拟。
 *
 * 职责：
 * 1. 同步 ECS (Transform + RigidBody + Collider) ↔ PhysicsWorld2D (Body + Shape)
 * 2. 管理 TransformInterpolation 快照
 * 3. TileMap 碰撞解析（独立于 PhysicsWorld2D 的后处理）
 * 4. PhysicsWorld2D 接触回调 → entt::dispatcher CollisionInfo 事件
 * 5. 空间查询（委托 PhysicsWorld2D，BodyId → entt::entity 映射）
 */
class PhysicsSystem : public ISystem {
public:
    PhysicsSystem(entt::registry& world, entt::dispatcher& dispatcher);

    void init() override;
    void shutdown() override;

    UpdatePhaseMask phaseMask() const override {
        return updatePhaseBit(UpdatePhase::Physics);
    }

    void update(float dt) override;

    // ── 重力设置 ─────────────────────────────────────────────────────────

    void setGravity(float x, float y);
    float gravityX() const;
    float gravityY() const;

    // ── 固定时间步 ───────────────────────────────────────────────────────

    void setFixedTimestep(float step);
    float fixedTimestep() const;
    float accumulatorSeconds() const;

    void setVariableTimestep(bool enable);
    bool variableTimestep() const;

    float interpolationAlpha() const;

    // ── 查询功能 ─────────────────────────────────────────────────────────

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

    // ── PhysicsWorld2D 访问 ──────────────────────────────────────────────

    PhysicsWorld2D& physicsWorld() { return physicsWorld_; }
    const PhysicsWorld2D& physicsWorld() const { return physicsWorld_; }

private:
    void onPhysicsPhase(float dt) override { update(dt); }

    // ── ECS ↔ PhysicsWorld2D 同步 ────────────────────────────────────────

    void syncBodiesToWorld();
    void syncResultsToEntities();
    void syncEntityToBody(entt::entity e, Transform& tf, RigidBody* rb, Collider& col);
    void ensureBodyForEntity(entt::entity e, Transform& tf, RigidBody* rb, Collider& col);
    void removeBodyForEntity(entt::entity e);

    // ── 接触回调桥接 ─────────────────────────────────────────────────────

    void onPhysicsContact(const PhysicsWorld2D::CollisionPair& pair);

    // ── 插值快照 ─────────────────────────────────────────────────────────

    void snapshotInterpolatedBodiesForStep();

    // ── ECS 事件钩子 ─────────────────────────────────────────────────────

    void onTransformUpdated(entt::registry& reg, entt::entity e);
    void onColliderAdded(entt::registry& reg, entt::entity e);
    void onColliderRemoved(entt::registry& reg, entt::entity e);
    void onRigidBodyAdded(entt::registry& reg, entt::entity e);
    void onRigidBodyRemoved(entt::registry& reg, entt::entity e);
    void onTileMapAdded(entt::registry& reg, entt::entity e);
    void onTileMapUpdated(entt::registry& reg, entt::entity e);
    void onTileMapRemoved(entt::registry& reg, entt::entity e);

    // ── Tile 碰撞 ────────────────────────────────────────────────────────

    void resolveTileCollisions();
    void rebuildTileCollisionCache(entt::entity mapEntity, const TileMap& tmap);

    // ── 辅助 ─────────────────────────────────────────────────────────────

    PhysicsWorld2D::AABB makeEntityAABB(entt::entity e) const;
    PhysicsWorld2D::AABB makeColliderAABB(const Transform& tf, const Collider& col) const;
    static PhysicsWorld2D::ShapeDef colliderToShapeDef(const Collider& col);
    static PhysicsWorld2D::BodyDef rigidBodyToBodyDef(const Transform& tf, const RigidBody& rb);

    // ── 成员 ─────────────────────────────────────────────────────────────

    entt::registry&   world_;
    entt::dispatcher& dispatcher_;
    PhysicsWorld2D    physicsWorld_;

    // ECS entity ↔ PhysicsWorld2D BodyId 映射
    struct BodyMapping {
        PhysicsWorld2D::BodyId bodyId = PhysicsWorld2D::INVALID_BODY;
    };
    std::unordered_map<entt::entity, BodyMapping> entityToBody_;
    std::vector<entt::entity> bodyToEntity_;  // indexed by BodyId

    // 固定时间步
    float fixedTimestep_ = 1.f / 60.f;
    float accumulator_ = 0.f;
    bool  steppingPhysics_ = false;
    bool  variableTimestep_ = false;

    entt::connection transformUpdateConnection_;

    // Tile 碰撞缓存
    std::unordered_map<entt::entity, TileCollisionCache> tileCollisionCaches_;
};

} // namespace engine
