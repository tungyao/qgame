#include "PhysicsSystem.h"
#include "../components/RenderComponents.h"
#include "../components/PhysicsComponents.h"
#include <cmath>
#include <vector>
#include <algorithm>

namespace engine {

PhysicsSystem::PhysicsSystem(entt::registry& world, entt::dispatcher& dispatcher)
    : world_(world), dispatcher_(dispatcher) {}

/**
 * 主更新函数 - 使用固定时间步进行物理模拟
 * 
 * 固定时间步的优势：
 * 1. 物理行为一致 - 无论帧率如何，物体运动轨迹相同
 * 2. 避免穿透 - 高帧率不会导致碰撞检测遗漏
 * 3. 可重现性 - 相同输入产生相同结果，便于调试和网络同步
 * 
 * 实现原理：
 * - 累积器收集帧时间
 * - 当累积时间超过固定步长时，执行一次物理更新
 * - 剩余时间保留到下一帧
 */
void PhysicsSystem::update(float dt) {
    accumulator_ += dt;
    while (accumulator_ >= fixedTimestep_) {
        integrateVelocities(fixedTimestep_);
        resolveCollisions();
        accumulator_ -= fixedTimestep_;
    }
}

/**
 * 速度积分 - 根据力和速度更新位置
 * 
 * 使用半隐式欧拉积分（Symplectic Euler）：
 * 1. 先更新速度（考虑重力）
 * 2. 再用新速度更新位置
 * 
 * 相比显式欧拉积分，半隐式欧拉更稳定，能量守恒更好
 */
void PhysicsSystem::integrateVelocities(float dt) {
    auto view = world_.view<Transform, RigidBody>();
    for (auto [e, tf, rb] : view.each()) {
        // 非运动学刚体才受重力影响
        if (!rb.isKinematic) {
            // v += g * gravityScale * dt
            // gravityScale = 0 表示不受重力（如浮空平台）
            // gravityScale = 1 正常重力
            // gravityScale = 2 双倍重力（如重物）
            rb.velocityX += gravityX_ * rb.gravityScale * dt;
            rb.velocityY += gravityY_ * rb.gravityScale * dt;
        }
        // 位置更新：x += v * dt
        tf.x += rb.velocityX * dt;
        tf.y += rb.velocityY * dt;
    }
}

namespace {

/**
 * 轴对齐包围盒 - 用于快速碰撞检测
 * minXY 是左上角，maxXY 是右下角
 */
struct AABB { float minX, minY, maxX, maxY; };

/**
 * 从 Transform 和 Collider 计算 AABB
 * 
 * Collider 的 offset 用于将碰撞盒偏移到精灵中心或特定位置
 * 例如：精灵 32x32，碰撞盒 24x24 居中，offset = (4, 4)
 */
AABB makeAABB(const Transform& tf, const Collider& col) {
    float x = tf.x + col.offsetX;
    float y = tf.y + col.offsetY;
    return {x, y, x + col.width, y + col.height};
}

/**
 * AABB 重叠检测 - 分离轴定理（SAT）的简化版
 * 
 * 两个 AABB 重叠当且仅当：
 * - X 轴投影重叠 AND Y 轴投影重叠
 * 
 * 这是 O(1) 的快速检测，用于宽相位过滤
 */
bool overlaps(const AABB& a, const AABB& b) {
    return a.minX < b.maxX && a.maxX > b.minX &&
           a.minY < b.maxY && a.maxY > b.minY;
}

/**
 * 计算最小分离向量（Minimum Translation Vector）
 * 
 * 当两个 AABB 重叠时，返回将它们分离所需的最小位移
 * 
 * 算法：
 * 1. 计算 X 和 Y 方向的重叠量
 * 2. 选择重叠量较小的轴推出（这是视觉上更自然的分离方向）
 * 3. 根据中心点位置决定推出方向
 * 
 * outX, outY 是将 A 推离 B 的位移向量
 */
void minSeparation(const AABB& a, const AABB& b, float& outX, float& outY) {
    // 计算重叠区域尺寸
    float overlapX = std::min(a.maxX, b.maxX) - std::max(a.minX, b.minX);
    float overlapY = std::min(a.maxY, b.maxY) - std::max(a.minY, b.minY);

    // 选择重叠量较小的轴分离（穿透深度较小的方向）
    if (overlapX < overlapY) {
        // X 轴分离
        float aCx = (a.minX + a.maxX) * 0.5f;
        float bCx = (b.minX + b.maxX) * 0.5f;
        // A 在 B 左边则向左推，否则向右推
        outX = (aCx < bCx) ? -overlapX : overlapX;
        outY = 0.f;
    } else {
        // Y 轴分离
        float aCy = (a.minY + a.maxY) * 0.5f;
        float bCy = (b.minY + b.maxY) * 0.5f;
        outX = 0.f;
        outY = (aCy < bCy) ? -overlapY : overlapY;
    }
}

} // anonymous namespace

/**
 * 碰撞过滤 - 判断两个碰撞体是否应该产生碰撞
 * 
 * 碰撞层系统（Layer/Mask）：
 * - layer: 物体所属的层（位掩码，一个物体只能在一个层）
 * - mask: 物体能碰撞的层（位掩码，可以碰撞多个层）
 * 
 * 示例配置：
 * - 玩家：layer = PLAYER(4), mask = ALL
 * - 敌人：layer = ENEMY(8), mask = PLAYER | STATIC
 * - 子弹：layer = BULLET(16), mask = ENEMY
 * 
 * 碰撞条件：(A.layer & B.mask) != 0 AND (B.layer & A.mask) != 0
 * 这允许非对称碰撞：子弹能碰敌人，敌人不能碰子弹
 */
bool PhysicsSystem::canCollide(const Collider& a, const Collider& b) const {
    // 两个都是 Trigger 则不产生物理碰撞
    if (a.isTrigger && b.isTrigger) return false;
    // 双向检测：A 能碰 B，且 B 能碰 A
    return (a.layer & b.mask) != 0 && (b.layer & a.mask) != 0;
}

/**
 * 碰撞解决 - 检测并处理所有碰撞对
 * 
 * 当前实现：O(n²) 暴力遍历
 * 后续优化：四叉树 / Sweep and Prune
 * 
 * 流程：
 * 1. 收集所有碰撞体到数组
 * 2. 两两检测重叠
 * 3. 触发碰撞事件（无论是否 Trigger）
 * 4. 非Trigger碰撞体进行位置分离
 * 5. 更新 AABB 避免同帧重复碰撞
 */
void PhysicsSystem::resolveCollisions() {
    // 收集所有有 Transform + Collider 的实体及其 AABB
    // 使用数组而非直接遍历 view，因为需要多次访问和更新 AABB
    struct ColEntry {
        entt::entity e;
        AABB aabb;
    };
    std::vector<ColEntry> entries;
    entries.reserve(64);

    auto view = world_.view<Transform, Collider>();
    for (auto [e, tf, col] : view.each()) {
        entries.push_back({e, makeAABB(tf, col)});
    }

    // O(n²) 碰撞检测
    for (int i = 0; i < (int)entries.size(); ++i) {
        for (int j = i + 1; j < (int)entries.size(); ++j) {
            auto& ei = entries[i];
            auto& ej = entries[j];

            const Collider& ci = world_.get<Collider>(ei.e);
            const Collider& cj = world_.get<Collider>(ej.e);

            // 层过滤
            if (!canCollide(ci, cj)) continue;
            // AABB 重叠检测
            if (!overlaps(ei.aabb, ej.aabb)) continue;

            // 计算分离向量
            float sepX = 0.f, sepY = 0.f;
            minSeparation(ei.aabb, ej.aabb, sepX, sepY);

            // 派发碰撞事件（双向，让双方都知道被撞了）
            dispatcher_.trigger(CollisionInfo{ei.e, ej.e,  sepX,  sepY});
            dispatcher_.trigger(CollisionInfo{ej.e, ei.e, -sepX, -sepY});

            // Trigger 只触发事件，不做物理分离
            if (ci.isTrigger || cj.isTrigger) continue;

            // 根据刚体类型决定分离方式
            bool iHasRb = world_.all_of<RigidBody>(ei.e);
            bool jHasRb = world_.all_of<RigidBody>(ej.e);
            bool iKin   = iHasRb && world_.get<RigidBody>(ei.e).isKinematic;
            bool jKin   = jHasRb && world_.get<RigidBody>(ej.e).isKinematic;

            Transform& tfi = world_.get<Transform>(ei.e);
            Transform& tfj = world_.get<Transform>(ej.e);

            if (iHasRb && !iKin && jHasRb && !jKin) {
                // 双方都是动态物体：各承担一半位移
                tfi.x += sepX * 0.5f; tfi.y += sepY * 0.5f;
                tfj.x -= sepX * 0.5f; tfj.y -= sepY * 0.5f;
            } else if (iHasRb && !iKin) {
                // 只有 i 是动态物体：i 完全承担位移
                tfi.x += sepX; tfi.y += sepY;
            } else if (jHasRb && !jKin) {
                // 只有 j 是动态物体：j 完全承担位移
                tfj.x -= sepX; tfj.y -= sepY;
            }
            // 如果都是静态物体或 kinematic，不做分离

            // 更新 AABB，避免同帧后续碰撞检测使用旧位置
            ei.aabb = makeAABB(world_.get<Transform>(ei.e), ci);
            ej.aabb = makeAABB(world_.get<Transform>(ej.e), cj);
        }
    }
}

/**
 * 射线检测 - 从起点沿方向发射射线，返回第一个碰撞体
 * 
 * 使用 Slab 算法（Ray-AABB 交叉检测）
 * 
 * 参数：
 * - startX, startY: 射线起点
 * - dirX, dirY: 射线方向（会自动归一化）
 * - maxDist: 最大检测距离
 * - layerMask: 只检测指定层的物体
 * 
 * 返回：
 * - RaycastHit.hit: 是否命中
 * - RaycastHit.entity: 命中的实体
 * - RaycastHit.hitX, hitY: 命中点坐标
 * - RaycastHit.normalX, normalY: 命中点法线（指向碰撞体中心）
 * - RaycastHit.distance: 起点到命中点的距离
 * 
 * 用途：
 * - 子弹/激光检测
 * - 视线检测（AI 能否看到玩家）
 * - 地面检测（角色是否站在地面上）
 */
RaycastHit PhysicsSystem::raycast(float startX, float startY, float dirX, float dirY, 
                                  float maxDist, CollisionLayer layerMask) {
    RaycastHit result{};
    result.hit = false;
    result.distance = maxDist;

    // 归一化方向向量
    float len = std::sqrt(dirX * dirX + dirY * dirY);
    if (len < 0.0001f) return result;  // 零方向，无效射线
    dirX /= len; dirY /= len;

    // 遍历所有碰撞体
    auto view = world_.view<Transform, Collider>();
    for (auto [e, tf, col] : view.each()) {
        // 层过滤
        if ((col.layer & layerMask) == 0) continue;

        AABB box = makeAABB(tf, col);
        
        // Slab 算法：计算射线与 AABB 的交点
        // tMin: 射线进入 AABB 的时间
        // tMax: 射线离开 AABB 的时间
        float tMin = 0.f;
        float tMax = maxDist;

        // 对每个轴分别计算
        for (int axis = 0; axis < 2; ++axis) {
            float p = (axis == 0) ? startX : startY;
            float d = (axis == 0) ? dirX : dirY;
            float minB = (axis == 0) ? box.minX : box.minY;
            float maxB = (axis == 0) ? box.maxX : box.maxY;

            if (std::abs(d) < 0.0001f) {
                // 射线与该轴平行，检查起点是否在 slab 内
                if (p < minB || p > maxB) { 
                    tMin = maxDist + 1.f;  // 标记为不相交
                    break; 
                }
            } else {
                // 计算射线与 slab 两个平面的交点
                float t1 = (minB - p) / d;
                float t2 = (maxB - p) / d;
                if (t1 > t2) std::swap(t1, t2);  // 确保 t1 <= t2
                
                // 更新相交区间
                tMin = std::max(tMin, t1);
                tMax = std::min(tMax, t2);
            }
        }

        // 检查是否相交且在有效范围内
        if (tMin <= tMax && tMin >= 0.f && tMin < result.distance) {
            result.hit = true;
            result.entity = e;
            result.distance = tMin;
            result.hitX = startX + dirX * tMin;
            result.hitY = startY + dirY * tMin;
            
            // 计算法线（简化：指向碰撞体中心）
            AABB box = makeAABB(tf, col);
            float cx = (box.minX + box.maxX) * 0.5f;
            float cy = (box.minY + box.maxY) * 0.5f;
            result.normalX = cx - result.hitX;
            result.normalY = cy - result.hitY;
            float nLen = std::sqrt(result.normalX * result.normalX + result.normalY * result.normalY);
            if (nLen > 0.0001f) {
                result.normalX /= nLen;
                result.normalY /= nLen;
            }
        }
    }

    return result;
}

/**
 * 盒形区域查询 - 检测指定矩形区域内所有碰撞体
 * 
 * 参数：
 * - centerX, centerY: 查询区域中心
 * - halfW, halfH: 查询区域半宽/半高
 * - layerMask: 只查询指定层的物体
 * 
 * 返回：
 * - OverlapResult.entity: 碰撞的实体
 * - OverlapResult.overlapX, overlapY: 重叠区域尺寸
 * 
 * 用途：
 * - 技能范围检测（横扫攻击）
 * - 触发区域检测
 * - AI 感知范围
 */
std::vector<OverlapResult> PhysicsSystem::overlapBox(float centerX, float centerY,
                                                     float halfW, float halfH,
                                                     CollisionLayer layerMask) {
    std::vector<OverlapResult> results;
    // 构造查询区域的 AABB
    AABB query{centerX - halfW, centerY - halfH, centerX + halfW, centerY + halfH};

    auto view = world_.view<Transform, Collider>();
    for (auto [e, tf, col] : view.each()) {
        // 层过滤
        if ((col.layer & layerMask) == 0) continue;

        AABB box = makeAABB(tf, col);
        if (overlaps(query, box)) {
            OverlapResult r;
            r.entity = e;
            // 计算重叠区域尺寸
            r.overlapX = std::min(query.maxX, box.maxX) - std::max(query.minX, box.minX);
            r.overlapY = std::min(query.maxY, box.maxY) - std::max(query.minY, box.minY);
            results.push_back(r);
        }
    }

    return results;
}

/**
 * 圆形区域查询 - 检测指定圆形区域内所有碰撞体
 * 
 * 参数：
 * - centerX, centerY: 圆心
 * - radius: 半径
 * - layerMask: 只查询指定层的物体
 * 
 * 算法：
 * 1. 先用 AABB 快速排除明显不相交的碰撞体
 * 2. 计算 AABB 上离圆心最近的点
 * 3. 检查该点到圆心的距离是否小于半径
 * 
 * 用途：
 * - 爆炸伤害范围
 * - 吸引/排斥效果范围
 * - NPC 交互范围
 */
std::vector<entt::entity> PhysicsSystem::overlapCircle(float centerX, float centerY, float radius,
                                                       CollisionLayer layerMask) {
    std::vector<entt::entity> results;
    float r2 = radius * radius;  // 使用距离平方避免开方

    auto view = world_.view<Transform, Collider>();
    for (auto [e, tf, col] : view.each()) {
        // 层过滤
        if ((col.layer & layerMask) == 0) continue;

        AABB box = makeAABB(tf, col);
        
        // 找到 AABB 上离圆心最近的点
        // 将圆心 clamp 到 AABB 内部
        float closestX = std::max(box.minX, std::min(centerX, box.maxX));
        float closestY = std::max(box.minY, std::min(centerY, box.maxY));
        
        // 计算距离平方
        float dx = closestX - centerX;
        float dy = closestY - centerY;
        
        // 如果最近点在圆内，则 AABB 与圆相交
        if (dx * dx + dy * dy <= r2) {
            results.push_back(e);
        }
    }

    return results;
}

} // namespace engine
