#include <cassert>
#include <cstdio>
#include <cmath>

#include <entt/entt.hpp>
#include "engine/components/RenderComponents.h"
#include "engine/components/PhysicsComponents.h"
#include "engine/systems/PhysicsSystem.h"

using namespace engine;

static constexpr float kEps = 0.001f;
static bool near(float a, float b) { return std::fabs(a - b) < kEps; }

// ── 测试：速度积分 ────────────────────────────────────────────────────────────

void testVelocityIntegration() {
    entt::registry   world;
    entt::dispatcher disp;
    PhysicsSystem    phys(world, disp);

    auto e = world.create();
    world.emplace<Transform>(e, 0.f, 0.f);
    world.emplace<RigidBody>(e, 100.f /*vx*/, 0.f /*vy*/);

    phys.update(0.1f);  // dt = 0.1s

    auto& tf = world.get<Transform>(e);
    assert(near(tf.x, 10.f));  // 100 * 0.1
    assert(near(tf.y, 0.f));

    ::printf("  velocity integration OK\n");
}

// ── 测试：重力 ───────────────────────────────────────────────────────────────

void testGravity() {
    entt::registry   world;
    entt::dispatcher disp;
    PhysicsSystem    phys(world, disp);
    phys.setGravity(0.f, 100.f);  // 向下 100 px/s²

    auto e = world.create();
    world.emplace<Transform>(e, 0.f, 0.f);
    world.emplace<RigidBody>(e, 0.f, 0.f, /*gravityScale=*/1.f);

    phys.update(1.f);  // dt = 1s

    auto& rb = world.get<RigidBody>(e);
    auto& tf = world.get<Transform>(e);
    assert(near(rb.velocityY, 100.f));
    assert(near(tf.y, 100.f));

    ::printf("  gravity OK\n");
}

// ── 测试：kinematic 不受重力 ──────────────────────────────────────────────────

void testKinematic() {
    entt::registry   world;
    entt::dispatcher disp;
    PhysicsSystem    phys(world, disp);
    phys.setGravity(0.f, 100.f);

    auto e = world.create();
    world.emplace<Transform>(e, 0.f, 0.f);
    RigidBody rb{};
    rb.gravityScale = 1.f;
    rb.isKinematic  = true;
    rb.velocityX    = 50.f;
    world.emplace<RigidBody>(e, rb);

    phys.update(1.f);

    auto& rb2 = world.get<RigidBody>(e);
    auto& tf  = world.get<Transform>(e);
    // velocity 未因重力改变
    assert(near(rb2.velocityY, 0.f));
    // 但位置因 velocityX 移动
    assert(near(tf.x, 50.f));

    ::printf("  kinematic OK\n");
}

// ── 测试：AABB 碰撞分离 ───────────────────────────────────────────────────────

void testAABBSeparation() {
    entt::registry   world;
    entt::dispatcher disp;
    PhysicsSystem    phys(world, disp);

    // 两个 20×20 的 box 重叠
    // box A: 位于 (0,0)，box B: 位于 (10,0)  → X 轴重叠 10px
    auto a = world.create();
    world.emplace<Transform>(a, 0.f, 0.f);
    world.emplace<Collider>(a, 20.f, 20.f, 0.f, 0.f, false);
    world.emplace<RigidBody>(a); // 可移动

    auto b = world.create();
    world.emplace<Transform>(b, 10.f, 0.f);
    world.emplace<Collider>(b, 20.f, 20.f, 0.f, 0.f, false);
    world.emplace<RigidBody>(b); // 可移动

    phys.update(0.f);  // dt=0 只做碰撞分辨

    auto& tfa = world.get<Transform>(a);
    auto& tfb = world.get<Transform>(b);
    // 两者各分一半，总分离 10px
    assert(tfa.x < 0.f);
    assert(tfb.x > 10.f);
    // 总间距 ≥ 20（不再重叠）
    float dist = tfb.x - tfa.x;
    assert(dist >= 20.f - kEps);

    ::printf("  AABB separation OK\n");
}

// ── 测试：trigger 不分离，但派发事件 ─────────────────────────────────────────

void testTriggerDispatch() {
    entt::registry   world;
    entt::dispatcher disp;
    PhysicsSystem    phys(world, disp);

    auto a = world.create();
    world.emplace<Transform>(a, 0.f, 0.f);
    world.emplace<Collider>(a, 20.f, 20.f, 0.f, 0.f, /*isTrigger=*/true);
    world.emplace<RigidBody>(a);

    auto b = world.create();
    world.emplace<Transform>(b, 5.f, 0.f);
    world.emplace<Collider>(b, 20.f, 20.f, 0.f, 0.f, false);
    world.emplace<RigidBody>(b);

    int hitCount = 0;
    disp.sink<CollisionInfo>().connect<[](int& cnt, const CollisionInfo&){ ++cnt; }>(hitCount);

    float origAx = world.get<Transform>(a).x;
    float origBx = world.get<Transform>(b).x;

    phys.update(0.f);

    // trigger：位置不变
    assert(near(world.get<Transform>(a).x, origAx));
    // b 也不变（a 是 trigger，两者都不分离）
    assert(near(world.get<Transform>(b).x, origBx));
    // 事件：a→b 和 b→a 各一次
    assert(hitCount == 2);

    ::printf("  trigger dispatch OK\n");
}

// ── 测试：static（无 RigidBody）碰到有 RigidBody 的 entity ───────────────────

void testStaticVsDynamic() {
    entt::registry   world;
    entt::dispatcher disp;
    PhysicsSystem    phys(world, disp);

    // 静态 box（无 RigidBody）
    auto wall = world.create();
    world.emplace<Transform>(wall, 0.f, 0.f);
    world.emplace<Collider>(wall, 20.f, 20.f);

    // 动态 box 嵌入 wall
    auto dyn = world.create();
    world.emplace<Transform>(dyn, 10.f, 0.f);
    world.emplace<Collider>(dyn, 20.f, 20.f);
    world.emplace<RigidBody>(dyn);

    phys.update(0.f);

    auto& wallTf = world.get<Transform>(wall);
    auto& dynTf  = world.get<Transform>(dyn);

    // wall 不动
    assert(near(wallTf.x, 0.f));
    // dyn 被推出
    assert(dynTf.x > 10.f);

    ::printf("  static vs dynamic OK\n");
}

int main() {
    ::printf("=== PhysicsSystem Tests ===\n");
    testVelocityIntegration();
    testGravity();
    testKinematic();
    testAABBSeparation();
    testTriggerDispatch();
    testStaticVsDynamic();
    ::printf("All PhysicsSystem tests passed.\n");
    return 0;
}
