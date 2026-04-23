#include "PhysicsSystem.h"
#include "../components/RenderComponents.h"   // Transform
#include "../components/PhysicsComponents.h"
#include <cmath>
#include <vector>

namespace engine {

PhysicsSystem::PhysicsSystem(entt::registry& world, entt::dispatcher& dispatcher)
    : world_(world), dispatcher_(dispatcher) {}

void PhysicsSystem::update(float dt) {
    integrateVelocities(dt);
    resolveCollisions();
}

void PhysicsSystem::integrateVelocities(float dt) {
    auto view = world_.view<Transform, RigidBody>();
    for (auto [e, tf, rb] : view.each()) {
        if (!rb.isKinematic) {
            rb.velocityX += gravityX_ * rb.gravityScale * dt;
            rb.velocityY += gravityY_ * rb.gravityScale * dt;
        }
        tf.x += rb.velocityX * dt;
        tf.y += rb.velocityY * dt;
    }
}

namespace {

struct AABB { float minX, minY, maxX, maxY; };

AABB makeAABB(const Transform& tf, const Collider& col) {
    float x = tf.x + col.offsetX;
    float y = tf.y + col.offsetY;
    return {x, y, x + col.width, y + col.height};
}

bool overlaps(const AABB& a, const AABB& b) {
    return a.minX < b.maxX && a.maxX > b.minX &&
           a.minY < b.maxY && a.maxY > b.minY;
}

// 最小分离向量：返回将 a 推出 b 所需的最小位移（基于中心点方向判断推出方向）
void minSeparation(const AABB& a, const AABB& b, float& outX, float& outY) {
    float overlapX = std::min(a.maxX, b.maxX) - std::max(a.minX, b.minX);
    float overlapY = std::min(a.maxY, b.maxY) - std::max(a.minY, b.minY);

    if (overlapX < overlapY) {
        float aCx = (a.minX + a.maxX) * 0.5f;
        float bCx = (b.minX + b.maxX) * 0.5f;
        outX = (aCx < bCx) ? -overlapX : overlapX;
        outY = 0.f;
    } else {
        float aCy = (a.minY + a.maxY) * 0.5f;
        float bCy = (b.minY + b.maxY) * 0.5f;
        outX = 0.f;
        outY = (aCy < bCy) ? -overlapY : overlapY;
    }
}

} // anonymous namespace

void PhysicsSystem::resolveCollisions() {
    // 收集所有有 Transform + Collider 的 entity
    struct ColEntry {
        entt::entity e;
        AABB         aabb;
    };
    std::vector<ColEntry> entries;
    entries.reserve(64);

    auto view = world_.view<Transform, Collider>();
    for (auto [e, tf, col] : view.each()) {
        entries.push_back({e, makeAABB(tf, col)});
    }

    for (int i = 0; i < (int)entries.size(); ++i) {
        for (int j = i + 1; j < (int)entries.size(); ++j) {
            auto& ei = entries[i];
            auto& ej = entries[j];

            if (!overlaps(ei.aabb, ej.aabb)) continue;

            float sepX = 0.f, sepY = 0.f;
            minSeparation(ei.aabb, ej.aabb, sepX, sepY);

            const Collider& ci = world_.get<Collider>(ei.e);
            const Collider& cj = world_.get<Collider>(ej.e);

            // 派发碰撞事件（trigger 和实体都触发）
            dispatcher_.trigger(CollisionInfo{ei.e, ej.e,  sepX,  sepY});
            dispatcher_.trigger(CollisionInfo{ej.e, ei.e, -sepX, -sepY});

            // trigger 不做物理分离
            if (ci.isTrigger || cj.isTrigger) continue;

            bool iHasRb = world_.all_of<RigidBody>(ei.e);
            bool jHasRb = world_.all_of<RigidBody>(ej.e);
            bool iKin   = iHasRb && world_.get<RigidBody>(ei.e).isKinematic;
            bool jKin   = jHasRb && world_.get<RigidBody>(ej.e).isKinematic;

            Transform& tfi = world_.get<Transform>(ei.e);
            Transform& tfj = world_.get<Transform>(ej.e);

            if (iHasRb && !iKin && jHasRb && !jKin) {
                // 两者都可移动：各承担一半
                tfi.x += sepX * 0.5f; tfi.y += sepY * 0.5f;
                tfj.x -= sepX * 0.5f; tfj.y -= sepY * 0.5f;
            } else if (iHasRb && !iKin) {
                tfi.x += sepX; tfi.y += sepY;
            } else if (jHasRb && !jKin) {
                tfj.x -= sepX; tfj.y -= sepY;
            }

            // 更新 AABB 以免后续同帧检测使用旧位置
            ei.aabb = makeAABB(world_.get<Transform>(ei.e), ci);
            ej.aabb = makeAABB(world_.get<Transform>(ej.e), cj);
        }
    }
}

} // namespace engine
