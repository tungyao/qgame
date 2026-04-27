#pragma once
#include <entt/entt.hpp>
#include <vector>
#include "ISystem.h"
#include "../components/PhysicsComponents.h"

namespace engine {

/**
 * 物理系统 - 处理刚体运动和碰撞检测
 * 
 * 主要功能：
 * 1. 重力模拟 - 对所有非 kinematic 刚体应用重力
 * 2. 碰撞检测 - AABB 碰撞检测与分离
 * 3. 碰撞过滤 - 基于 Layer/Mask 的碰撞层系统
 * 4. 固定时间步 - 保证物理模拟的一致性
 * 5. 查询功能 - 射线检测和区域查询
 * 
 * 使用方式：
 * 1. 通过 GameAPI 设置重力：api.setGravity(0, 980);
 * 2. 给实体添加组件：Transform + RigidBody + Collider
 * 3. 监听碰撞事件：api.onCollision(listener, &Listener::onCollision);
 * 4. 查询碰撞：api.raycast(...) / api.overlapBox(...) / api.overlapCircle(...)
 * 
 * 性能特征：
 * - 碰撞检测：O(n²) 暴力遍历（后续优化为四叉树）
 * - 查询功能：O(n) 线性扫描
 * - 固定时间步：默认 60Hz，可调整
 */
class PhysicsSystem : public ISystem {
public:
    PhysicsSystem(entt::registry& world, entt::dispatcher& dispatcher);

    void update(float dt) override;
    bool isManuallyScheduled() const override { return true; }

    // ── 重力设置 ─────────────────────────────────────────────────────────────
    
    /**
     * 设置全局重力
     * @param x X 方向重力加速度（像素/秒²）
     * @param y Y 方向重力加速度（像素/秒²）
     * 
     * 典型值：
     * - 标准重力：setGravity(0, 980) - Y 轴向下为正
     * - 横版平台：setGravity(0, 1500) - 更快的下落
     * - 俯视游戏：setGravity(0, 0) - 无重力
     */
    void setGravity(float x, float y) { gravityX_ = x; gravityY_ = y; }
    float gravityX() const { return gravityX_; }
    float gravityY() const { return gravityY_; }

    // ── 固定时间步 ───────────────────────────────────────────────────────────
    
    /**
     * 设置物理更新的固定时间步
     * @param step 时间步长（秒）
     * 
     * 默认值：1/60 = 0.0167 秒（60Hz）
     * 
     * 调整建议：
     * - 精确物理：1/120（120Hz）- 更准确但更耗性能
     * - 移动端：1/30（30Hz）- 省电但可能穿透
     */
    void setFixedTimestep(float step) { fixedTimestep_ = step; }
    float fixedTimestep() const { return fixedTimestep_; }

    // ── 查询功能 ─────────────────────────────────────────────────────────────
    
    /**
     * 射线检测 - 从起点发射射线，返回最近的碰撞体
     * 
     * @param startX, startY 射线起点
     * @param dirX, dirY 射线方向（无需归一化）
     * @param maxDist 最大检测距离
     * @param layerMask 只检测指定层（默认检测所有层）
     * @return RaycastHit 结果结构体
     * 
     * 用例：
     * // 检测玩家前方是否有墙壁
     * auto hit = api.raycast(playerX, playerY, 1, 0, 100, COLLISION_LAYER_STATIC);
     * if (hit.hit) {
     *     // hit.entity 是墙壁实体
     *     // hit.distance 是距离
     * }
     */
    RaycastHit raycast(float startX, float startY, float dirX, float dirY, float maxDist,
                       CollisionLayer layerMask = COLLISION_LAYER_ALL);
    
    /**
     * 盒形区域查询 - 检测矩形区域内所有碰撞体
     * 
     * @param centerX, centerY 查询区域中心
     * @param halfW, halfH 查询区域半宽/半高
     * @param layerMask 只查询指定层
     * @return 所有重叠的碰撞体列表
     * 
     * 用例：
     * // 检测攻击范围内的敌人
     * auto hits = api.overlapBox(attackX, attackY, 50, 30, COLLISION_LAYER_ENEMY);
     * for (auto& h : hits) {
     *     applyDamage(h.entity, 10);
     * }
     */
    std::vector<OverlapResult> overlapBox(float centerX, float centerY, 
                                           float halfW, float halfH,
                                           CollisionLayer layerMask = COLLISION_LAYER_ALL);
    
    /**
     * 圆形区域查询 - 检测圆形区域内所有碰撞体
     * 
     * @param centerX, centerY 圆心
     * @param radius 半径
     * @param layerMask 只查询指定层
     * @return 所有重叠的碰撞体列表
     * 
     * 用例：
     * // 检测爆炸范围内的所有物体
     * auto hits = api.overlapCircle(explosionX, explosionY, 150, COLLISION_LAYER_ALL);
     * for (auto e : hits) {
     *     applyExplosionForce(e);
     * }
     */
    std::vector<entt::entity> overlapCircle(float centerX, float centerY, float radius,
                                            CollisionLayer layerMask = COLLISION_LAYER_ALL);

private:
    entt::registry&   world_;
    entt::dispatcher& dispatcher_;
    
    float gravityX_ = 0.f;       // X 方向重力
    float gravityY_ = 0.f;       // Y 方向重力
    float fixedTimestep_ = 1.f / 60.f;  // 固定时间步（默认 60Hz）
    float accumulator_ = 0.f;    // 时间累积器

    void integrateVelocities(float dt);  // 速度积分
    void resolveCollisions();            // 碰撞解决
    bool canCollide(const Collider& a, const Collider& b) const;  // 碰撞过滤
};

} // namespace engine
