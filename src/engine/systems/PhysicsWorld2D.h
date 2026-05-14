#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../components/PhysicsComponents.h"
#include "../../backend/renderer/gpu_driven/SpatialHashGrid.h"

namespace engine {

/**
 * 独立 2D 物理世界 — 不含 ECS 依赖，可直接单元测试。
 *
 * 架构参考 Godot 4.x PhysicsServer2D / Cocos2d-x PhysicsWorld：
 * - Body 是带有运动属性的碰撞体载体
 * - Shape 定义碰撞几何（每个 Body 可挂多个 Shape）
 * - BroadPhase (SpatialHashGrid) → NarrowPhase (shape dispatch) → Solver (mass-weighted)
 * - CCD 作为 step 后处理
 * - 接触持久化跟踪 (Begin / Persist / End)
 */
class PhysicsWorld2D {
public:
    using BodyId = int;
    static constexpr BodyId INVALID_BODY = 0;

    // ── 内部类型 ──────────────────────────────────────────────────────────

    struct AABB {
        float minX, minY, maxX, maxY;
    };

    struct BodyDef {
        BodyType type         = BodyType::Static;
        float    x            = 0.f;
        float    y            = 0.f;
        float    velocityX    = 0.f;
        float    velocityY    = 0.f;
        float    gravityScale = 1.f;
        float    mass         = 1.f;
        float    bounciness   = 0.f;
        float    friction     = 0.f;
        bool     ccdEnabled   = false;
        float    contactMargin = 1.f;
    };

    struct ShapeDef {
        ShapeType shapeType = ShapeType::Box;
        // Box
        float width  = 0.f;
        float height = 0.f;
        // Circle / Capsule
        float radius        = 0.f;
        float capsuleLength = 0.f;
        // Offset from body origin
        float offsetX = 0.f;
        float offsetY = 0.f;
        bool  isTrigger = false;
        CollisionLayer layer = COLLISION_LAYER_DEFAULT;
        CollisionLayer mask  = COLLISION_LAYER_ALL;
    };

    struct CollisionPair {
        BodyId bodyA = INVALID_BODY;
        BodyId bodyB = INVALID_BODY;
        float  normalX = 0.f;
        float  normalY = 0.f;
        float  overlap = 0.f;
        ContactState state = ContactState::Begin;
        bool   isTriggerPair = false;
    };

    struct RaycastResult {
        BodyId bodyId = INVALID_BODY;
        float  hitX = 0.f, hitY = 0.f;
        float  normalX = 0.f, normalY = 0.f;
        float  distance = 0.f;
        bool   hit = false;
    };

    struct OverlapResult {
        BodyId bodyId = INVALID_BODY;
        float  overlapX = 0.f, overlapY = 0.f;
    };

    using ContactCallback = std::function<void(const CollisionPair&)>;

    // ── 生命周期 ──────────────────────────────────────────────────────────

    PhysicsWorld2D();
    ~PhysicsWorld2D();

    // ── 世界配置 ──────────────────────────────────────────────────────────

    void setGravity(float x, float y);
    void setIterationCount(int count);
    void setContactMargin(float margin);
    void setBroadphaseCellSize(float size);

    float gravityX() const { return gravityX_; }
    float gravityY() const { return gravityY_; }
    int   iterationCount() const { return iterationCount_; }
    float contactMargin() const { return contactMargin_; }

    // ── Body 管理 ─────────────────────────────────────────────────────────

    BodyId createBody(const BodyDef& def);
    void   destroyBody(BodyId id);
    void   clearAllBodies();

    void setBodyTransform(BodyId id, float x, float y);
    void setBodyVelocity(BodyId id, float vx, float vy);
    void setBodyType(BodyId id, BodyType type);
    void setBodyGravityScale(BodyId id, float scale);
    void setBodyMass(BodyId id, float mass);

    BodyType bodyType(BodyId id) const;
    bool     isBodyValid(BodyId id) const;
    size_t   bodyCount() const { return bodies_.size(); }

    // ── Shape 管理 ────────────────────────────────────────────────────────

    void addShape(BodyId bodyId, const ShapeDef& def);
    void clearShapes(BodyId bodyId);

    // ── 模拟管线 ──────────────────────────────────────────────────────────

    void integrateVelocities(float dt);  // 仅速度积分（不含碰撞）
    void resolveCollisions();            // 碰撞检测 + 求解 + 接触回调（不含积分和CCD）
    void step(float dt);                 // 完整步进：integrate + resolve + CCD

    // ── 状态读回 ──────────────────────────────────────────────────────────

    void getBodyState(BodyId id, float& x, float& y, float& vx, float& vy) const;

    // ── 空间查询 ──────────────────────────────────────────────────────────

    RaycastResult raycast(float startX, float startY,
                          float dirX, float dirY,
                          float maxDist,
                          CollisionLayer layerMask = COLLISION_LAYER_ALL,
                          CollisionLayer ignoreLayer = 0,
                          BodyId ignoreBody = INVALID_BODY) const;

    std::vector<OverlapResult> overlapRect(float centerX, float centerY,
                                            float halfW, float halfH,
                                            CollisionLayer layerMask = COLLISION_LAYER_ALL) const;

    std::vector<BodyId> overlapCircle(float centerX, float centerY, float radius,
                                     CollisionLayer layerMask = COLLISION_LAYER_ALL) const;

    // ── 接触回调 ──────────────────────────────────────────────────────────

    void setContactCallback(ContactCallback cb) { contactCb_ = std::move(cb); }

    // ── AABB 工具（静态，供 PhysicsSystem tile 碰撞复用）──────────────────

    static AABB computeAABB(float bodyX, float bodyY,
                            const ShapeDef& shape,
                            const float* spriteSrcW = nullptr,
                            const float* spriteSrcH = nullptr,
                            float scaleX = 1.f, float scaleY = 1.f,
                            float pivotX = 0.5f, float pivotY = 0.5f);
    static bool overlaps(const AABB& a, const AABB& b);
    static void minSeparation(const AABB& a, const AABB& b, float& outX, float& outY);

private:
    struct Body {
        BodyType type;
        float x, y;
        float velocityX, velocityY;
        float gravityScale;
        float mass, invMass;
        float bounciness, friction;
        bool  ccdEnabled;
        float contactMargin;
        std::vector<ShapeDef> shapes;
    };

    struct ContactKey {
        int a, b;
        bool operator==(const ContactKey& o) const { return a == o.a && b == o.b; }
    };
    struct ContactKeyHash {
        size_t operator()(const ContactKey& k) const {
            return std::hash<int>{}(k.a) ^ (std::hash<int>{}(k.b) << 16);
        }
    };

    struct CcdEntry {
        BodyId id;
        AABB   oldBox;
        float  oldX, oldY;
    };

    // ── 内部管线 ──────────────────────────────────────────────────────────

    void rebuildBroadphase_() const;
    void broadPhase_(std::vector<std::pair<BodyId, BodyId>>& outPairs);
    bool narrowPhase_(const AABB& aabbA, const ShapeDef& shapeA,
                      const AABB& aabbB, const ShapeDef& shapeB,
                      float& outNX, float& outNY, float& outOverlap);
    void solveContacts_(const std::vector<CollisionPair>& pairs);
    void ccdPostPass_();
    void processContacts_(const std::vector<CollisionPair>& pairs);

    bool canCollide_(const ShapeDef& a, const ShapeDef& b) const;
    AABB computeBodyAABB_(const Body& body, const ShapeDef& shape) const;

    // ── 窄相位函数 ────────────────────────────────────────────────────────

    static bool boxVsBox_(const AABB& a, const AABB& b,
                          float& nx, float& ny, float& overlap);
    static bool boxVsCircle_(const AABB& box, float cx, float cy, float r,
                             float& nx, float& ny, float& overlap);
    static bool circleVsCircle_(float ax, float ay, float ar,
                                float bx, float by, float br,
                                float& nx, float& ny, float& overlap);
    // Capsule variants stubs for Phase 4
    static bool capsuleVsAny_(const AABB&, const ShapeDef&,
                              const AABB&, const ShapeDef&,
                              float&, float&, float&) { return false; }

    static bool sweptAABBvsAABB_(const AABB& start, float dx, float dy,
                                 const AABB& target, float& outTime);

    // ── 成员 ──────────────────────────────────────────────────────────────

    float gravityX_ = 0.f;
    float gravityY_ = 0.f;
    int   iterationCount_ = 3;
    float contactMargin_ = 1.f;

    BodyId nextBodyId_ = 1;
    std::unordered_map<BodyId, Body> bodies_;

    mutable SpatialHashGrid<BodyId> broadphase_{64.f};

    std::unordered_set<ContactKey, ContactKeyHash> prevFrameContacts_;
    std::unordered_map<ContactKey, CollisionPair, ContactKeyHash> lastContactNormals_;

    std::vector<CcdEntry> ccdBuffer_;

    ContactCallback contactCb_;
};

} // namespace engine
