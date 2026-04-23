#pragma once
#include <entt/entt.hpp>

namespace engine {

struct RigidBody {
    float velocityX    = 0.f;
    float velocityY    = 0.f;
    float gravityScale = 0.f;  // 0 = 不受全局重力
    bool  isKinematic  = false; // true = 只做位移，不被碰撞推开
};

struct Collider {
    float width   = 0.f;
    float height  = 0.f;
    float offsetX = 0.f;  // 碰撞盒相对 Transform.x 的偏移
    float offsetY = 0.f;
    bool  isTrigger = false; // true = 只触发事件，不做物理分离
};

// EnTT dispatcher 发射的碰撞事件
struct CollisionInfo {
    entt::entity self;
    entt::entity other;
    float overlapX = 0.f;  // 最小分离向量 X（正表示 self 需向右移）
    float overlapY = 0.f;
};

} // namespace engine
