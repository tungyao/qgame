# 碰撞检测系统优化路线图

> 基于当前 `PhysicsSystem` (O(n²) 暴力遍历) 的分析，以下列出 10 个优化方向，按收益/复杂度排序。

---

## 当前现状

```cpp
// PhysicsSystem::resolveCollisions() — 当前实现
for (int i = 0; i < entries.size(); ++i) {
    for (int j = i + 1; j < entries.size(); ++j) {
        // O(n²) 双重循环
        // 每帧对所有 Transform+Collider 实体全量检测
    }
}
```

- **Broad Phase**: 无，直接全量 pairwise 检测
- **Narrow Phase**: 仅 AABB vs AABB，返回单一 MTV 向量
- **查询接口**: `raycast()`, `overlapBox()`, `overlapCircle()` 均遍历全量实体
- **TileMap 碰撞**: 每帧对每个动态刚体遍历 AABB 覆盖的 tile 范围

---

## 1. Broad Phase — 空间分区（最高收益）

### 问题
实体数过百时 O(n²) 急剧恶化。500 个实体每帧 12.5 万次检测，其中 99% 是空间上相距甚远的无效检测。

### 方案对比

| 算法 | 时间复杂度 | 空间复杂度 | 动态物体支持 | 适用场景 |
|------|-----------|-----------|-------------|---------|
| **Spatial Hash** | 插入/删除/查询 O(1) 均摊 | O(动态实体数) | 优秀（每帧重建或增量更新） | 2D 游戏首选，实现简单 |
| **Sweep and Prune (SAP)** | 排序 O(n log n)，检测 O(n+k) | O(n) | 中（需每帧重排序） | 物体分布沿某一轴聚集 |
| **Uniform Grid (数组版)** | O(1) | O(世界大小/cellSize²) | 优秀 | 密集世界、固定边界 |
| **四叉树** | 插入/查询 O(log n) | O(n) | 中（树 rebalance 开销） | 物体大小差异极大 |

### 推荐实现：Spatial Hash

复用渲染系统的 `SpatialHashGrid<uint32_t>` 思路，为碰撞系统独立维护一份：

```cpp
struct CollisionSpatialHash {
    float cellSize = 64.0f;  // 根据平均碰撞体尺寸调整
    float invCellSize = 1.0f / 64.0f;
    // key: 打包的 cell 坐标, value: 该 cell 内的 Collider 索引
    std::unordered_map<int64_t, std::vector<uint32_t>> cells;
};
```

**关键设计**:
- **cellSize 选择**: 设为最大碰撞体尺寸的 1.5~2 倍，避免单个对象跨过多 cell
- **跨 cell 插入**: AABB 覆盖的所有 cell 都插入该实体索引（参考 RenderSystem 的 `SpriteCullProxy` 做法）
- **持久化 vs 每帧重建**: 
  - 静态物体（地形、TileMap）→ 持久化索引，只更新一次
  - 动态物体 → 每帧 `clear()` + 重建，或增量更新（先删旧 cell 再插新 cell）
- **去重机制**: 大对象跨多 cell，查询时用 `queryStamp` 递增标记去重（同 RenderSystem）

### 预期收益
- 1000 实体均匀分布 → 检测对数从 ~50万 降至 ~5000（假设每 cell 平均 5 个）
- 与总对象数脱钩，只和**局部密度**相关

---

## 2. 碰撞形状扩展

当前 `Collider` 只有 width/height，所有碰撞退化为 AABB。

### 新增形状

```cpp
enum class ColliderShape {
    Box,      // 当前已有
    Circle,   // 新增：子弹、炸弹、角色
    Capsule,  // 新增：平台角色（比 AABB 更适合斜面）
    Polygon,  // 新增：凸多边形，地形斜坡
};

struct Collider {
    ColliderShape shape = ColliderShape::Box;
    float width = 0.f, height = 0.f;  // Box/Capsule 用
    float radius = 0.f;               // Circle/Capsule 用
    std::vector<Vec2> vertices;       // Polygon 用（凸包，局部坐标）
    // ... 其他字段不变
};
```

### 窄相位检测矩阵

| 形状 A | 形状 B | 算法 | 复杂度 |
|--------|--------|------|--------|
| Circle | Circle | 距离 < 半径和 | O(1) |
| Circle | Box | 圆心 clamp 到 Box 最近点 | O(1) |
| Box | Box | AABB 重叠检测 | O(1) |
| Polygon | Polygon | SAT (分离轴定理) | O(v₁+v₂) |
| Capsule | Any | 退化为线段+半径的 Minkowski Sum | O(1)~O(n) |

### 实现路径
1. **先加 Circle**：最简单，收益明确（子弹、炸弹、技能范围）
2. **再加 Capsule**：平台跳跃角色首选，解决 AABB 在斜坡上的滑行问题
3. **Polygon 最后**：需要凸包分解或预计算支持轴

---

## 3. 窄相位 — 接触流形 (Contact Manifold)

当前 `minSeparation()` 只返回一个 `overlapX/Y` MTV 向量，缺陷：
- 无接触点位置 → 无法施加旋转冲量（力矩）
- 无接触法线 → 反弹方向不精确
- 一帧多对碰撞 → 分离后可能立刻与其他对象重新碰撞，抖动

### Contact 结构

```cpp
struct Contact {
    Vec2 point;        // 世界空间接触点
    Vec2 normal;       // 从 B 指向 A 的单位法线
    float penetration; // 穿透深度（正值）
    float impulse;     // 法向冲量累加（用于迭代求解）
    float tangentImpulse; // 切向冲量累加（摩擦）
    uint32_t id;       // 特征标识，用于跨帧持久化匹配
};

struct Manifold {
    entt::entity entityA;
    entt::entity entityB;
    std::array<Contact, 4> contacts; // Box-Box 最多 2 个，通常 1-2 个
    int contactCount;
};
```

### 持久化接触 (Persistent Contacts)

同一对碰撞体连续多帧碰撞时，用 contact ID 匹配旧接触点：
- ID 编码：`(featureA << 16) | featureB`，feature 可以是顶点/边索引
- 匹配上的接触点继承上帧的 `impulse` 累加值 → **Warm Starting**，大幅减少迭代次数
- 未匹配的新接触点初始化 impulse 为 0

### 各形状接触点生成

- **Circle-Circle**: 1 个接触点，法线 = 圆心连线方向
- **Circle-Box**: 1 个接触点，圆心 clamp 到 Box 的最近点
- **Box-Box**: 1-2 个接触点，用 Sutherland-Hodgman 裁剪或特征对特征
- **Polygon-Polygon**: 1-2 个接触点，SAT 找出参考边和入射边后裁剪

---

## 4. 冲量法求解 (Impulse-based Solver)

当前是**位置修正**（直接把重叠的物体推开），不物理且易抖动。

### 核心公式

**相对速度沿法线分量**:
```
v_rel = (vA + ωA × rA) - (vB + ωB × rB)
v_rel_n = dot(v_rel, normal)
```

**碰撞冲量**（考虑质量和弹性）:
```
e = restitution (弹性系数，0=完全非弹性，1=完全弹性)
j = -(1 + e) * v_rel_n / (1/mA + 1/mB + (rA×n)²/IA + (rB×n)²/IB)
```

**摩擦冲量**（Coulomb 摩擦模型）:
```
tangent = normalize(v_rel - v_rel_n * normal)
j_t = -dot(v_rel, tangent) / (同上质量项)
j_t = clamp(j_t, -μ * j, μ * j)  // μ = friction 系数
```

### 实现框架

```cpp
void PhysicsSystem::solveVelocityConstraints(const std::vector<Manifold>& manifolds) {
    for (int iter = 0; iter < velocityIterations; ++iter) {
        for (auto& manifold : manifolds) {
            for (int i = 0; i < manifold.contactCount; ++i) {
                Contact& c = manifold.contacts[i];
                // 计算相对速度
                // 应用法向冲量
                // 应用切向冲量（摩擦）
                // 累加到 contact.impulse / contact.tangentImpulse
            }
        }
    }
}
```

**Baumgarte 位置修正**:
把穿透深度也转化为速度修正项，避免完全依赖位置 snap：
```
bias = (baumgarteFactor / dt) * max(0, penetration - allowedPenetration)
// 在速度求解时加入 bias，让物体自然分开
```

### 质量属性

当前 `RigidBody` 没有质量/转动惯量字段：
```cpp
struct RigidBody {
    // ... 现有字段
    float mass = 1.0f;
    float invMass = 1.0f;      // 0 表示静态/kinematic
    float inertia = 1.0f;      // 转动惯量
    float invInertia = 1.0f;   // 0 表示不能旋转
    float angle = 0.0f;        // 旋转角度（弧度）
    float angularVelocity = 0.0f;
};
```

---

## 5. 连续碰撞检测 (CCD)

固定步长下，高速小物体（子弹、角色快速掉落）会**穿透**薄墙。

### 方案

| 方案 | 原理 | 适用对象 | 开销 |
|------|------|---------|------|
| **Swept AABB** | 上一帧位置→当前位置的线段扩展为 AABB，检测是否与其他 AABB 时序相交 | 中高速动态物体 | 中 |
| **Ray Casting** | 速度方向发射射线，检测 ray-AABB 交点 | 子弹、激光等极高速 | 低 |
| **TOI (Time of Impact)** | 精确计算碰撞时刻 t∈[0,1]，回退到接触位置 | 需要精确物理时 | 高 |

### 推荐实现

**动态切换策略**：
```cpp
struct RigidBody {
    // ...
    bool useCCD = false;  // 高速物体开启
    float ccdThreshold = 0.0f;  // 速度超过此值时启用 CCD（如 10 * radius / dt）
};
```

1. **每帧更新前**：计算 `motion = velocity * dt`，若 `motion > body.radius * threshold` → 标记为 CCD
2. **CCD 物体**：
   - 用上一帧 AABB 和当前帧 AABB 合并为 **Swept AABB**
   - Broad Phase 查询 Swept AABB 覆盖的潜在碰撞体
   - 对每个潜在碰撞体做 **Swept AABB vs AABB** 求 TOI
   - 若有碰撞，将物体位置回退到 `previous + velocity * dt * t_hit`
3. **子弹/射线**：直接用 `raycast()`，跳过 Swept AABB

---

## 6. Sleep / Wake 机制

静止物体（地面、停住的箱子）每帧参与模拟浪费 CPU。

### 实现

```cpp
struct RigidBody {
    // ...
    bool isSleeping = false;
    float sleepTimer = 0.0f;
};

static constexpr float SLEEP_THRESHOLD = 0.1f;      // 速度平方阈值
static constexpr float SLEEP_TIME_REQUIRED = 0.5f;  // 连续静止 0.5 秒入睡
```

**入睡条件**:
```cpp
bool shouldSleep = (velocityX² + velocityY² + angularVelocity²) < SLEEP_THRESHOLD²;
if (shouldSleep) {
    sleepTimer += dt;
    if (sleepTimer >= SLEEP_TIME_REQUIRED) isSleeping = true;
} else {
    sleepTimer = 0.0f;
    isSleeping = false;
}
```

**唤醒条件**:
- 被非 sleeping 物体碰撞
- 被施加力/冲量
- 被脚本修改 velocity / position

**Broad Phase 收益**:
- Sleeping 物体不参与 Dynamic-Dynamic 碰撞检测
- 但仍在 Spatial Hash 中，可被 Dynamic 物体查询到（被动碰撞）

---

## 7. 查询接口加速

当前 `raycast()` / `overlapBox()` / `overlapCircle()` 遍历全量实体。

### 射线检测 — DDA 遍历 Spatial Hash

```cpp
// 2D DDA (Digital Differential Analyzer) 算法
// 沿射线方向逐 cell 步进，只检测射线穿过的格子

RaycastHit raycast(float startX, float startY, float dirX, float dirY, float maxDist) {
    int cx = floor(startX * invCellSize);
    int cy = floor(startY * invCellSize);
    
    int stepX = (dirX > 0) ? 1 : -1;
    int stepY = (dirY > 0) ? 1 : -1;
    
    float tMaxX = (dirX > 0) ? ((cx + 1) * cellSize - startX) / dirX
                             : (cx * cellSize - startX) / dirX;
    float tMaxY = (dirY > 0) ? ((cy + 1) * cellSize - startY) / dirY
                             : (cy * cellSize - startY) / dirY;
    
    float tDeltaX = abs(cellSize / dirX);
    float tDeltaY = abs(cellSize / dirY);
    
    float t = 0.0f;
    while (t < maxDist) {
        // 检测当前 cell 内的所有碰撞体
        auto* cell = spatialHash.queryCell(cx, cy);
        if (cell) { /* 精确射线 vs AABB/Circle/Polygon */ }
        
        // 步进到下一个 cell
        if (tMaxX < tMaxY) { t = tMaxX; tMaxX += tDeltaX; cx += stepX; }
        else               { t = tMaxY; tMaxY += tDeltaY; cy += stepY; }
    }
}
```

### 区域查询

- `overlapBox()` / `overlapCircle()`：先用 Spatial Hash 粗筛（query 覆盖的 cell），再对候选集做精确形状测试
- **缓存友好**：查询结果存入固定大小的栈数组，避免 `std::vector` 频繁分配

---

## 8. TileMap 碰撞优化

当前每帧对每个动态刚体遍历 AABB 覆盖的 tile 范围。

### 方案 1: 预烘焙 TileMap 碰撞网格

```cpp
class TileMapCollisionGrid {
public:
    void bake(const TileMap& tmap);  // 只调用一次
    
    // 查询 actor AABB 覆盖的 tile 碰撞体
    void query(float minX, float minY, float maxX, float maxY, 
               std::vector<TileCollider>& out);
    
private:
    SpatialHashGrid<TileCollider> grid_;  // 只包含 collidable tile
};
```

**bake 流程**:
1. 遍历 TileMap 所有 layer
2. 只收集 `collidable=true` 且 shape != None 的 tile
3. 把每个 tile 的 AABB 插入 `SpatialHashGrid`
4. 后续查询和动态物体共用一套 Spatial Hash 接口

### 方案 2: TileMap 直接作为静态 Spatial Hash

把 TileMap 的 tile 碰撞数据直接存入物理系统的 Spatial Hash，key 与普通实体统一。

### 方案 3: 分层查询

- 先查 TileMap 的粗网格（知道哪些区域有 tile）
- 只在有 tile 的区域做精细的 per-tile AABB 检测
- 空区域直接跳过

---

## 9. 数据布局与缓存优化

当前 ECS 访问模式存在缓存不友好问题：
- `world_.get<Collider>(e)` 每对碰撞 2 次 hash 查找
- `makeEntityAABB()` 每帧对同一实体多次计算
- `std::vector` 在 tight loop 中频繁 push/pop

### 优化措施

#### A. 扁平 SOA 数组

```cpp
struct CollisionWorld {
    // 连续内存，缓存友好
    std::vector<AABB> aabbs;           // 世界空间 AABB
    std::vector<uint32_t> layers;      // layer mask
    std::vector<uint32_t> masks;       // collision mask
    std::vector<entt::entity> entities;
    std::vector<uint8_t> isStatic;     // 静态标记，快速跳过
    
    // 只包含活跃碰撞体的紧凑索引
    std::vector<uint32_t> dynamicIndices;
    std::vector<uint32_t> staticIndices;
};
```

**同步策略**:
- Transform/Collider 变化时 → 通过 ECS observer 更新 CollisionWorld 对应条目
- 每帧物理步开始前 → 批量更新所有 dynamic 物体的 AABB

#### B. AABB 缓存

```cpp
struct CachedAABB {
    AABB box;
    uint32_t frameId;  // 计算时的帧号
    bool dirty;        // Transform 是否变化
};
```

- Transform 变化时标记 dirty
- 碰撞检测时若 dirty 则重新计算，否则复用

#### C. 避免 std::vector 分配

- 预分配 `std::vector<Manifold> manifolds; manifolds.reserve(256);`
- 查询结果用固定大小数组 + 计数器，超限时再回退到 vector

---

## 10. 约束与关节（高级）

如需实现物理效果：弹簧、绳子、旋转门、滑轨平台。

### 约束类型

| 约束 | 描述 | 数学形式 |
|------|------|---------|
| **Distance** | 两点保持距离 | `C = |pA - pB| - L = 0` |
| **Hinge** | 旋转关节，限制相对角度 | `C = angleA - angleB - θ = 0` |
| **Prismatic** | 滑轨，限制沿某轴平移 | `C = dot(pA - pB, axis) = 0` |
| **Weld** | 完全固定相对位置和角度 | 上述组合 |

### 求解方法

**Sequential Impulse (Erin Catto / Box2D 方法)**:
1. 把所有约束和接触统一表示为 `J * v = -C / dt`（速度层面）
2. 逐约束迭代求解，每次更新速度并累加 impulse
3. 通常 4-10 次迭代即可收敛

### 数据结构

```cpp
struct Constraint {
    entt::entity entityA;
    entt::entity entityB;
    ConstraintType type;
    
    // Jacobian 相关（每约束预计算）
    float jacobian[4];  // J = [n, rA×n, -n, -rB×n] 的扁平存储
    float bias;         // 位置修正项
    float impulse;      // 累加冲量（Warm Starting）
    
    // 参数
    float minImpulse, maxImpulse;  // 冲量范围（如 distance constraint 允许推和拉）
};
```

### 建议
- 优先级低，除非明确需求（如平台需要滑轨、角色需要抓钩）
- 实现前确保冲量求解器和接触流形已经稳定

---

## 优先级矩阵

| 优先级 | 优化项 | 工作量 | 性能收益 | 前提依赖 |
|--------|--------|--------|---------|---------|
| **P0 — 立刻做** | Spatial Hash Broad Phase | 中 | ★★★★★ | 无 |
| **P0 — 立刻做** | 查询接口加速 (raycast/overlap) | 低 | ★★★★☆ | Spatial Hash |
| **P1 — 近期** | Circle/Capsule 形状 | 中 | ★★★★☆ | 无 |
| **P1 — 近期** | 冲量法 + 接触流形 | 中高 | ★★★★★ | 形状扩展 |
| **P1 — 近期** | Sleep/Wake 机制 | 低 | ★★★☆☆ | Broad Phase |
| **P2 — 中期** | TileMap 碰撞网格烘焙 | 中 | ★★★★☆ | Spatial Hash |
| **P2 — 中期** | 数据布局优化 (SOA) | 低 | ★★★☆☆ | Broad Phase |
| **P2 — 中期** | 连续碰撞检测 CCD | 中 | ★★★☆☆ | 冲量求解器 |
| **P2 — 中期** | 摩擦/弹性/质量属性 | 低 | ★★★☆☆ | 冲量求解器 |
| **P3 — 远期** | 约束与关节 | 高 | ★★☆☆☆ | 完整求解器 |

---

## 推荐实现顺序

```
Phase 1 (1-2 天):
  ├── 实现 CollisionSpatialHash
  ├── 将 PhysicsSystem::resolveCollisions() 改为 hash-based broad phase
  └── raycast/overlapBox/overlapCircle 接入 Spatial Hash

Phase 2 (2-3 天):
  ├── 扩展 ColliderShape: Circle, Capsule
  ├── 实现对应 Narrow Phase 检测函数
  └── 更新 makeEntityAABB 支持新形状（或改为 makeShapeBounds）

Phase 3 (3-5 天):
  ├── 添加 Contact/Manifold 结构
  ├── 实现各形状的 Contact Generation
  ├── 为 RigidBody 添加 mass/inertia/angle/angularVelocity
  ├── 实现 Impulse-based Velocity Solver
  └── 加入 Baumgarte Position Bias

Phase 4 (1-2 天):
  ├── Sleep/Wake 机制
  ├── TileMap 碰撞网格预烘焙
  └── 数据布局 SOA 优化

Phase 5 (可选):
  └── CCD + 约束关节
```

---

## 参考资源

- **Box2D Lite** (Erin Catto): 最简洁的冲量求解器实现，约 1000 行 C++
- **Game Physics Engine Development** (Ian Millington): 完整 2D/3D 物理引擎教材
- **Realtime Collision Detection** (Christer Ericson): 碰撞检测算法圣经
- **chipmunk-physics** (C 语言 2D 物理引擎): 开源，结构清晰，适合参考

---

## 附录：当前 PhysicsSystem 调用链

```
PhysicsSystem::update(dt)
  ├── integrateVelocities(dt)          // 速度积分，更新 Transform
  ├── resolveCollisions()
  │     ├── 收集所有 Transform+Collider → entries[]
  │     ├── O(n²) pairwise AABB 检测
  │     ├── 层过滤 (canCollide)
  │     ├── minSeparation() → MTV 向量
  │     ├── 触发 CollisionInfo 事件（双向）
  │     ├── 非 Trigger → 位置分离（直接修改 Transform.x/y）
  │     └── 更新 AABB（避免同帧重复碰撞）
  ├── resolveTileCollisions()
  │     ├── 遍历所有动态 RigidBody
  │     ├── 计算 AABB 覆盖的 tile 范围
  │     ├── 逐 tile 检测碰撞
  │     └── 触发事件 + 位置分离 + 零速
  └── (variable timestep: 插值快照追齐)
```
