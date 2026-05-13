# 碰撞检测系统统一优化方案

## Context

当前 PhysicsSystem 使用 SAP (Sweep and Prune) 做 broad phase，但最坏情况仍是 O(n²)（当所有物体在 X 轴重叠时 early break 失效）；查询 API (raycast/overlapBox/overlapCircle) 都是 O(n) 全量遍历。代码库已有 SpatialHashGrid<T> 模板（SpatialHashGrid.h）但仅被 GPU-driven renderer 使用。

出发点是统一方案：两个游戏类型（星露谷：大量静态物体 + 少数动态体；横版射击：大量高速动态体）底层需求一致——都需要高效空间查询，差异仅在于配置参数。核心优化（空间哈希 + 静动分离）同时受益，只需少量可选扩展（CCD）服务特殊场景。

## 设计

核心思想：单 SpatialHashGrid 服务所有物理

PhysicsSystem 持有 SpatialHashGrid<entt::entity>，同时服务于：
- Broad phase 碰撞对生成（取代 SAP）
- Query API (raycast/overlapBox/overlapCircle)

两层策略可独立开关，接口统一。

### 架构概览

```
PhysicsSystem
  ├── entityGrid_ / staticGrid_ / dynamicGrid_ (SpatialHashGrid<entt::entity>)
  │     - 每帧对动态实体做 clear() + multi-cell insert (用 AABB)
  │     - 静态实体 cache 在 staticGrid_ 中，不每帧重建
  │     - 休眠实体仍插入 dynamicGrid_（只跳过 integration，不跳过碰撞检测）
  │
  ├── resolveCollisions()
  │     1. 每帧重建 dynamicGrid_（动态体 + 休眠体）
  │     2. 对每个动态实体：
  │        a. 从 dynamicGrid_ 查邻居 → 处理动态-动态对
  │        b. 从 staticGrid_ 查邻居 → 处理动态-静态对
  │     3. 跳过静态-静态对
  │     4. 碰撞事件 + 分离逻辑不变
  │
  ├── raycast() / overlapBox() / overlapCircle()
  │     1. 计算查询覆盖的 grid cell 范围
  │     2. 只查这些 cell 内的实体（entityGrid_ + staticGrid_）
  │     3. 去重（一个实体跨多 cell 会被重复查到）
  │     4. 层过滤 + 精确测试不变
  │
  ├── resolveTileCollisions()
  │     - 保留独立路径（tile 碰撞有自己的 semantic——需要知道 tile 坐标、gid、触发事件）
  │     - 但添加 TileCollisionCache：预计算 tile→AABB 映射，避免每帧重建
  │
  └── sleepSystem_ (可选扩展)
        - SleepState 组件 + 速度阈值
        - 休眠实体跳过 integration，但不跳过 grid 插入和碰撞检测
        - 碰撞/外部速度变化时唤醒
```

### 实现步骤

### Step 1: SpatialHashGrid 接入 broad phase

**文件**: PhysicsSystem.h, PhysicsSystem.cpp

PhysicsSystem 新增成员：
```cpp
// 默认 64px cell size；实际值应根据场景平均 Collider 尺寸调整
// tile 尺寸 32px → cell 64px (2x2 tile)
// tile 尺寸 64px → cell 128px (2x2 tile)
SpatialHashGrid<entt::entity> entityGrid_{64.f};
```

`resolveCollisions()` 改为：
1. `entityGrid_.clear()`
2. 对所有 `Transform + Collider` 实体做 multi-cell insert（用 AABB）
3. 对每个动态实体，query 覆盖的 cell 获得候选集
4. 对候选集做 layer 过滤 + AABB 重叠 + 分离
5. 使用 visited 标记避免重复处理同一对

**复杂度说明**：
- 当前 SAP：排序 O(n log n)，最坏 O(n²)（宽物体全重叠时），平均 O(n log n + n·k)（k 为 X 轴平均重叠数）
- 优化后：插入 O(n·cells_per_entity)，查询 O(cells_queried × candidates_per_cell)
- 对窄物体/均匀分布的场景提升显著；对全部密集重叠的场景两方案接近

**去重方案**：
- 推荐：`std::vector<uint64_t> processedPairs_`，每对 entity pair 编码为 `(min(e1,e2) << 32) | max(e1,e2)`，每帧 `clear()` + 插入 + 最终 `sort + unique` 去重
- 备选：每动态实体用 `entt::sparse_set visited` 标记已配对实体（O(1) 查插，但需要每帧 clear 或 frame counter 技巧）
- 不建议：裸 `std::vector<pair>` + 线性查找（退化为 O(n²)）

### Step 2: 查询 API 接入 grid

**文件**: PhysicsSystem.cpp

**overlapBox()** — 从 O(n) 到 O(cells_covered_by_box)：
- 用 box AABB 算覆盖的 cell 范围
- 只查这些 cell 内的实体
- 去重：使用 `entt::sparse_set seen{};` 标记已访问实体（O(1)，复用性好）

**overlapCircle()** — 同上，但按圆覆盖的 AABB 查 cell。去重方式同上。

**raycast()** — DDA 遍历 cell：
- 从起点到终点，沿射线遍历穿过的 cell
- 只检测这些 cell 内的实体做 slab 相交测试
- DDA (Digital Differential Analyzer) 算法 —— 参考 "A Fast Voxel Traversal Algorithm for Ray Tracing" (Amanatides & Woo, 1987)
- 注意处理方向分量为 0 的边沿情况（避免除零）
- START 参数初始化：
  - `tMaxX/Y` = 从射线起点到下一个 cell 边界的距离
  - `tDeltaX/Y` = 沿射线穿过一个 cell 所需的距离
- 去重同 overlapBox

### Step 3: 静态/动态分离

**文件**: PhysicsSystem.h, PhysicsSystem.cpp

**定义**：
- **静态体** = 有 Collider 且无 RigidBody（完全不动的地形、装饰物）
- **isKinematic** = 有 Collider + RigidBody.isKinematic（脚本驱动移动，放入 dynamicGrid_ 但不积分）

- 新增 `staticGrid_` (SpatialHashGrid) 只存静态体
- 静态体插入后不 clear，只在实体 add/remove 时更新
- 每帧只重建 `dynamicGrid_`（动态体 + isKinematic）
- `resolveCollisions()` 循环：
  - 动态 vs 动态：dynamicGrid_ 内查
  - 动态 vs 静态：dynamicGrid_ 的每个实体查 staticGrid_
  - 静态 vs 静态：跳过

**收益**：星露谷场景（400 静态 + 10 动态）碰撞对从 C(410,2)=83,945 降到 C(10,2)+10×400=4,045，降 95%。

**新增 hook（PhysicsSystem.cpp）**：
```cpp
// init() 中注册——四个方向都要覆盖
world_.on_construct<Collider>().connect<&PhysicsSystem::onColliderAdded>(this);
world_.on_destroy<Collider>().connect<&PhysicsSystem::onColliderRemoved>(this);
world_.on_construct<RigidBody>().connect<&PhysicsSystem::onRigidBodyAdded>(this);
world_.on_destroy<RigidBody>().connect<&PhysicsSystem::onRigidBodyRemoved>(this);
```

`onRigidBodyAdded`：如果实体已在 staticGrid_ 中，迁移到 dynamicGrid_
`onRigidBodyRemoved`：如果实体在 dynamicGrid_ 中，迁移到 staticGrid_

**isKinematic 归属**：
- 插入 **dynamicGrid_** 而非 staticGrid_
- `integrateVelocities()` 中跳过（速度不由物理驱动）
- 碰撞检测正常参与（可以被其他物体推开，触发射击判定等）

### Step 4: Tile 碰撞缓存

**文件**: PhysicsSystem.h, PhysicsSystem.cpp, RenderComponents.h (可选)

当前 `resolveTileCollisions()` 对每个 actor 的每个重叠 tile 调用 `collisionAt()`，后者做 gid→tileset→collision profile 的线性查找。

优化：TileMap 缓存预计算的碰撞 tile AABB：

```cpp
// PhysicsSystem 新增——复用全局 AABB 结构
struct CachedTileCollision {
    AABB aabb;                     // 复用 PhysicsSystem.cpp 内部 AABB
    TileMap::TileCollisionShape shape;
    bool isTrigger;
};

struct TileCollisionCache {
    std::vector<std::vector<CachedTileCollision>> grid;  // [y][x] 行优先
    int width = 0, height = 0;
    bool valid = false;
    // 标记哪些 tile 具有动画碰撞（这类 tile 不做缓存，每帧回退到 collisionAt()）
    std::vector<bool> animatedCollisionTiles;
};

std::unordered_map<entt::entity, TileCollisionCache> tileCollisionCaches_;
```

- 在 TileMap 组件 attach/update 时重建 cache（通过 `on_construct<TileMap>` + `on_update<TileMap>` hook）
- **动画 tile 处理**：检查 tileset 是否有动画且动画帧的 collision 不同 → 标记 `animatedCollisionTiles[pos] = true`，此类 tile 每帧退回到 `collisionAt()` 查询
- `resolveTileCollisions()` 直接用 cache 里的 AABB，跳过 collisionAt() 查找链
- 碰撞逻辑本身不变

**内存估算**：200×200 tile 地图 → 40,000 条目 → 每条目 ~24 字节 → ~960KB，可接受。

### Step 5: 休眠系统

**文件**: PhysicsComponents.h, PhysicsSystem.h, PhysicsSystem.cpp

新增组件：
```cpp
struct SleepState {
    float timer = 0.f;             // 已静止时间
    bool asleep = false;           // 是否已休眠
    // 可配置阈值
    float velocityThreshold = 10.f;       // 低于此速度认为静止
    float sleepTimeThreshold = 0.5f;      // 静止超过此时间进入休眠
};
```

**改动**：
- `integrateVelocities()`：跳过 asleep 实体
- **重要**：休眠实体**仍然插入 dynamicGrid_**。只跳过积分，不跳过碰撞检测。这样可以确保：
  - 其他动态实体能碰撞到休眠实体
  - 碰撞发生时可以唤醒休眠实体
- 唤醒条件：
  - 速度被外部修改（通过 `on_update<RigidBody>` listener）
  - 在碰撞循环中，如果动态实体 A 与休眠实体 B 发生碰撞 → 唤醒 B
  - 游戏逻辑通过 API 显式唤醒

**级联唤醒（堆叠场景）**：
- 当一堆方块堆叠休眠后，从上方掉落新方块时，被碰撞的方块应逐级唤醒
- 实现：碰撞循环中使用一个 `wakeQueue_`，本帧唤醒的实体标记为 `recheck = true`，在当前帧循环中重新处理被唤醒实体的碰撞（不超过 N 次迭代防死循环）

### Step 6: CCD 连续碰撞检测（横版射击专属）

> ⚠️ **注意**：此步为后续工作，不在初始实施范围内。Step 1-5 已覆盖 90%+ 性能提升。

文件: PhysicsComponents.h, PhysicsSystem.h, PhysicsSystem.cpp

新加配置标记（轻量，不增加结构体大小）：
```cpp
// RigidBody 新增字段
bool ccdEnabled = false;      // 启用连续碰撞检测
```

在 `resolveCollisions()` 中，对 ccdEnabled 的实体：
1. 计算本帧位移向量 `vx*dt, vy*dt`
2. 如果位移超过最小碰撞体尺寸的一定比例（如 50%），触发 CCD
3. 沿位移做 swept AABB vs AABB 测试（需要 SAT 的时间版本，而非简单 raycast）
4. 在首次碰撞位置停止
5. 支持反弹/滑动（至少 1 次反弹迭代）

**实现复杂度**：
- 窄相位测试需要 Swept AABB vs AABB 算法（基于分离轴定理找出最早碰撞时间 t）
- 两个 CCD 物体相向而行时需要对称处理
- 网格查询提供 broad phase 候选集，但 narrow phase 不复用网格逻辑

### 性能预期

| 场景                | 当前（含 SAP early exit） | 优化后            | 关键优化     |
|---------------------|--------------------------|--------------------|--------------|
| 星露谷 (400静态+10动态) | ~4,500 碰撞对 (SAP平均) | ~4,045 碰撞对      | 静态分离     |
| 横版射击 (50动态+200静态) | ~3,750 碰撞对 (SAP平均) | ~1,225 碰撞对 | grid broad phase |
| overlapBox 每帧10次  | 10 × n 全量遍历          | 10 × cell_count    | grid query   |
| raycast 每帧20次     | 20 × n 全量遍历          | 20 × traversed_cells | DDA traversal |

> 注：SAP 当前实际复杂度是 O(n log n + n·k)（k 为 X 轴平均重叠实体数），在窄物体场景下远好于 O(n²)。表格中"当前"列反映的是平均而非最坏情况。

### 关键文件列表

| 文件                             | 改动                                                              |
|----------------------------------|-------------------------------------------------------------------|
| src/engine/components/PhysicsComponents.h | 新增 SleepState 组件；RigidBody 加 ccdEnabled           |
| src/engine/systems/PhysicsSystem.h        | 新增 entityGrid_, staticGrid_, dynamicGrid_, tileCollisionCaches_, 新方法 |
| src/engine/systems/PhysicsSystem.cpp      | 重写 resolveCollisions(), 查询 API, tile collision cache, sleep 逻辑 |
| src/engine/systems/PhysicsSystem.cpp      | 新增 DDA raycast 辅助函数                                        |

不新增 .h 文件。SpatialHashGrid.h 和 SpatialHashGrid.cpp 不修改（模板头文件已完整）。

### 增量实施路线

分 3 个 Phase 滚动落地，每个 Phase 有独立验证：

**Phase A（核心）**：
- Step 1: Grid broad phase（与现有 SAP 用 `#if` 或运行时 flag 切换，benchmark 对比验证）
- Step 3: 静动分离（此时 Phase A 即获得最大收益）

**Phase B（查询 + Tile）**：
- Step 2: 查询 API 接入 grid
- Step 4: Tile 碰撞缓存

**Phase C（休眠）**：
- Step 5: 休眠系统
- Step 6: CCD（可选，后续工作）

### 验证方法

1. 构建验证：`make` 或 `cmake --build` 确认编译通过
2. **Phase A 性能对比**：
   - 在 PhysicsSystem 中添加 `perfCollisionPairs_`、`perfGridBuildTimeUs_`、`perfBroadphaseTimeUs_` 帧计数器
   - 在 SAP 和 grid 两种模式下分别运行典型场景，对比帧耗时
3. demo6 (snake) 运行：
   - Snake 吃食物增长，自碰撞检测正常
   - 碰撞事件触发正确（snake head vs food → Trigger 事件）
4. demo7 (stress test) 内存压力测试 — 不需要 physics，确认未引入回归
5. 手动验证：
   - overlapBox / overlapCircle / raycast 返回与 grid 集成的结果一致
   - 静态体缓存生效（断点验证 staticGrid_ 内容）
   - 休眠系统可触发/唤醒
