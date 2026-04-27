#pragma once
#include <entt/entt.hpp>
#include <cstdint>

namespace engine {

/**
 * 碰撞层类型定义
 * 
 * 使用位掩码实现，支持 32 个不同的碰撞层
 * 物体只能属于一个层（单 bit），但可以与多个层碰撞（多 bit mask）
 * 
 * 预定义层：
 * - DEFAULT: 默认层，所有未指定层的物体
 * - STATIC:  静态物体（地形、墙壁），不移动
 * - PLAYER:  玩家
 * - ENEMY:   敌人
 * - ALL:     所有层（用于 mask，表示与所有层碰撞）
 * 
 * 自定义层示例：
 * constexpr CollisionLayer COLLISION_LAYER_BULLET  = 16;  // 第5层
 * constexpr CollisionLayer COLLISION_LAYER_PICKUP  = 32;  // 第6层
 */
using CollisionLayer = uint32_t;

// 预定义碰撞层（位掩码）
constexpr CollisionLayer COLLISION_LAYER_DEFAULT = 1;      // 0001 - 第1层
constexpr CollisionLayer COLLISION_LAYER_STATIC  = 2;      // 0010 - 第2层
constexpr CollisionLayer COLLISION_LAYER_PLAYER  = 4;      // 0100 - 第3层
constexpr CollisionLayer COLLISION_LAYER_ENEMY   = 8;      // 1000 - 第4层
constexpr CollisionLayer COLLISION_LAYER_ALL     = 0xFFFFFFFF;  // 所有层

/**
 * 刚体组件 - 控制物理运动
 * 
 * 必须与 Transform 组件一起使用
 * 表示实体参与物理模拟，具有速度和重力响应
 */
struct RigidBody {
    float velocityX    = 0.f;  // X 方向速度（像素/秒）
    float velocityY    = 0.f;  // Y 方向速度（像素/秒）
    float gravityScale = 0.f;  // 重力缩放：0=不受重力, 1=正常, 2=双倍
    bool  isKinematic  = false; // 是否为运动学刚体
    // kinematic=true: 受速度影响但不受碰撞影响（如移动平台）
    // kinematic=false: 完全参与物理模拟（受重力和碰撞影响）
};

/**
 * 碰撞体组件 - 定义碰撞区域
 * 
 * 必须与 Transform 组件一起使用
 * 定义实体的碰撞盒大小和碰撞行为
 */
struct Collider {
    float width   = 0.f;    // 碰撞盒宽度（像素）
    float height  = 0.f;    // 碰撞盒高度（像素）
    float offsetX = 0.f;    // 碰撞盒相对于 Transform 的 X 偏移
    float offsetY = 0.f;    // 碰撞盒相对于 Transform 的 Y 偏移
    bool  isTrigger = false; // 是否为触发器
    // isTrigger=true: 只触发碰撞事件，不做物理分离（如传送门、拾取物）
    // isTrigger=false: 正常物理碰撞，会产生分离
    CollisionLayer layer = COLLISION_LAYER_DEFAULT;  // 所属碰撞层
    CollisionLayer mask  = COLLISION_LAYER_ALL;      // 可碰撞的层（位掩码）
};

/**
 * 碰撞事件 - 由 PhysicsSystem 通过 dispatcher 触发
 * 
 * 当两个碰撞体发生重叠时触发
 * 包含碰撞双方实体和分离向量
 * 
 * 监听示例：
 * class MyListener {
 * public:
 *     void onCollision(const CollisionInfo& info) {
 *         // info.self 是监听者关联的实体
 *         // info.other 是碰撞的对方
 *         // info.overlapX/Y 是分离向量
 *     }
 * };
 * 
 * 注册监听：
 * api.onCollision(listener, &MyListener::onCollision);
 */
struct CollisionInfo {
    entt::entity self;      // 碰撞的一方
    entt::entity other;     // 碰撞的另一方
    float overlapX = 0.f;   // 最小分离向量 X（正数表示 self 需向右移动）
    float overlapY = 0.f;   // 最小分离向量 Y（正数表示 self 需向下移动）
};

/**
 * 射线检测结果 - 由 PhysicsSystem::raycast 返回
 * 
 * 包含射线命中信息：
 * - 是否命中
 * - 命中的实体
 * - 命中点坐标
 * - 命中点法线
 * - 射线起点到命中点的距离
 */
struct RaycastHit {
    entt::entity entity;     // 命中的实体（未命中时无效）
    float hitX, hitY;        // 命中点坐标
    float normalX, normalY;  // 命中点法线（单位向量，指向碰撞体中心）
    float distance;          // 起点到命中点的距离
    bool  hit;               // 是否命中
};

/**
 * 区域重叠查询结果 - 由 PhysicsSystem::overlapBox 返回
 * 
 * 包含重叠的实体和重叠区域信息
 */
struct OverlapResult {
    entt::entity entity;      // 重叠的实体
    float overlapX, overlapY; // 重叠区域尺寸（像素）
};

} // namespace engine
