# Box2D 物理系统集成设计

## 概述

将 QGame 引擎的自定义 2D 物理系统替换为 Box2D v3.1.1（MIT 许可），以获得更好的性能、稳定性和功能完备性。

## 动机

- **性能与稳定性**：Box2D 经过多年生产验证，Soft Step 求解器更稳定
- **减少维护成本**：移除 ~1500 行自定义物理代码，利用成熟开源项目
- **功能需求**：Box2D 内置关节、马达、传感器、CCD 等高级功能

## 架构设计

### 整体结构

```
GameAPI (像素单位)
    ↓
PhysicsSystem (ECS 适配层, UpdatePhase::Physics)
    ↓  ← 内部转换 像素↔米 (32 PPM)
Box2D C API (米单位)
```

- 移除 `PhysicsWorld2D`（自定义物理引擎核心）
- 移除 `SpatialHashGrid`（Box2D 内置 broadphase）
- `PhysicsSystem` 直接调用 Box2D C API
- `GameAPI` 适配为新组件接口

## 组件设计

### RigidBody

```cpp
struct RigidBody {
    BodyType type = BodyType::Dynamic;    // Static / Kinematic / Dynamic
    float gravityScale = 1.0f;
    bool enabled = true;
    bool freezeRotation = false;
    b2BodyId bodyId = b2_nullBodyId;
};
```

变更说明：
- 移除 velocityX/Y（由 Box2D 管理）
- 移除 mass（Box2D 根据 density 自动计算）
- 移除 bounciness/friction（移到 Collider）
- 移除 ccdEnabled（Box2D 世界级配置）
- 移除 isKinematic（用 BodyType 替代）
- 新增 freezeRotation
- bodyId 在 PhysicsSystem 创建 Box2D body 时填充

### Collider

```cpp
struct Collider {
    ShapeType shapeType = ShapeType::Box;  // Box / Circle / Capsule
    float width = 1.0f;      // 像素单位
    float height = 1.0f;
    float radius = 0.5f;
    float density = 1.0f;
    float friction = 0.3f;
    float restitution = 0.0f;
    bool isTrigger = false;
    CollisionLayer layer = COLLISION_LAYER_DEFAULT;  // uint64_t
    CollisionLayer mask = COLLISION_LAYER_ALL;
    float offsetX = 0.0f;   // 相对 transform 偏移
    float offsetY = 0.0f;
    b2ShapeId shapeId = b2_nullShapeId;
};
```

变更说明：
- 新增 density/friction/restitution（Box2D 原生字段）
- `CollisionLayer` 从 `uint32_t` 改为 `uint64_t`（Box2D v3 使用 64 位 filter）
- 移除 contactMargin（Box2D 内置 CCD）
- shapeId 在 PhysicsSystem 创建 Box2D shape 时填充

### TileMapCollider（新增）

```cpp
struct TileMapCollider {
    b2ChainId chainId = b2_nullChainId;
    float friction = 0.3f;
    CollisionLayer layer = COLLISION_LAYER_STATIC;
    CollisionLayer mask = COLLISION_LAYER_ALL;
};
```

- 使用 Box2D `b2CreateChain` 创建静态链形状
- 替代现有的 TileMap 碰撞独立代码路径

### CollisionInfo（保留并适配）

```cpp
struct CollisionInfo {
    entt::entity self;
    entt::entity other;
    float normalX, normalY;
    float contactX, contactY;
    float approachSpeed;
    ContactState state = ContactState::Begin;
};
```

- 从 Box2D 的 `b2ContactBeginTouchEvent` / `b2ContactEndTouchEvent` / `b2ContactHitEvent` 转换而来
- `approachSpeed` 从 `b2ContactHitEvent` 获取

### 碰撞层（64 位）

```cpp
using CollisionLayer = uint64_t;
constexpr CollisionLayer COLLISION_LAYER_DEFAULT = 1;
constexpr CollisionLayer COLLISION_LAYER_STATIC  = 2;
constexpr CollisionLayer COLLISION_LAYER_PLAYER  = 4;
constexpr CollisionLayer COLLISION_LAYER_ENEMY   = 8;
constexpr CollisionLayer COLLISION_LAYER_ALL     = 0xFFFFFFFFFFFFFFFF;
```

## 单位转换

```cpp
constexpr float PIXELS_PER_METER = 32.0f;

inline float toMeters(float pixels)  { return pixels / PIXELS_PER_METER; }
inline float toPixels(float meters)  { return meters * PIXELS_PER_METER; }
inline b2Vec2 toMeters(float x, float y) {
    return {x / PIXELS_PER_METER, y / PIXELS_PER_METER};
}
```

- 所有 GameAPI 接口保持像素单位（对游戏代码透明）
- PhysicsSystem 内部在同步 Transform 时做像素↔米转换
- Box2D 内部始终使用米单位（推荐物体 0.1~10 米，对应像素 3.2~320）

## PhysicsSystem 更新流程

```cpp
constexpr int SUB_STEP_COUNT = 4;

void PhysicsSystem::update(float dt) {
    // 1. ECS → Box2D 同步 (on_update<Transform> 钩子)
    //    Transform (像素) → b2Body_SetTransform (米)
    syncTransformsToBox2D();

    // 2. Box2D 步进
    b2World_Step(worldId_, dt, SUB_STEP_COUNT);

    // 3. 事件轮询 + ECS 回写
    pollBodyEvents();       // b2BodyMoveEvent → Transform (米→像素)
    pollContactEvents();    // b2ContactEvents → CollisionInfo → dispatcher
    pollSensorEvents();     // b2SensorEvents → 触发器事件
}
```

### ECS 生命周期钩子（EnTT）

| 事件 | 操作 |
|------|------|
| `on_construct<RigidBody>` | 创建 b2Body（若已有 Collider 则同时创建 shape） |
| `on_destroy<RigidBody>` | 销毁 b2Body（清理所有 shape） |
| `on_construct<Collider>` | 若已有 RigidBody，创建 b2Shape |
| `on_destroy<Collider>` | 销毁 b2Shape |
| `on_update<Transform>` | 同步位置到 Box2D（像素→米） |
| `on_construct<TileMapCollider>` | 从 TileMap 组件创建 b2Chain |
| `on_destroy<TileMapCollider>` | 销毁 b2Chain |

## 事件处理

### 接触事件

```cpp
void PhysicsSystem::pollContactEvents() {
    b2ContactEvents events = b2World_GetContactEvents(worldId_);

    for (int i = 0; i < events.beginCount; ++i) {
        auto* ev = events.beginEvents + i;
        CollisionInfo info = makeCollisionInfo(ev, ContactState::Begin);
        dispatcher_.trigger<CollisionEvent>(info);
    }

    for (int i = 0; i < events.endCount; ++i) {
        auto* ev = events.endEvents + i;
        if (!b2Shape_IsValid(ev->shapeIdA) || !b2Shape_IsValid(ev->shapeIdB))
            continue;
        CollisionInfo info = makeCollisionInfo(ev, ContactState::End);
        dispatcher_.trigger<CollisionEvent>(info);
    }
}
```

### Body 移动事件

```cpp
void PhysicsSystem::pollBodyEvents() {
    b2BodyEvents events = b2World_GetBodyEvents(worldId_);
    for (int i = 0; i < events.moveCount; ++i) {
        auto* ev = events.moveEvents + i;
        auto entity = bodyToEntity_[ev->bodyId];
        if (entity == entt::null) continue;
        auto& transform = registry_.get<Transform>(entity);
        transform.x = toPixels(ev->transform.p.x);
        transform.y = toPixels(ev->transform.p.y);
        transform.rotation = b2Rot_GetAngle(ev->transform.q);
    }
}
```

## 文件变更清单

### 移除的文件

| 文件 | 行数 | 替代 |
|------|------|------|
| `src/engine/systems/PhysicsWorld2D.h` | ~50 | Box2D |
| `src/engine/systems/PhysicsWorld2D.cpp` | ~875 | Box2D |
| `src/backend/renderer/gpu_driven/SpatialHashGrid.h` | ~200 | Box2D 内置 |

### 修改的文件

| 文件 | 变更 |
|------|------|
| `src/engine/components/PhysicsComponents.h` | 重构组件为 Box2D 原生包装 |
| `src/engine/systems/PhysicsSystem.h` | 移除 PhysicsWorld2D 引用，使用 Box2D |
| `src/engine/systems/PhysicsSystem.cpp` | 直接调用 Box2D C API |
| `src/engine/api/GameAPI.h` | 适配新组件接口 |
| `src/engine/api/GameAPI.cpp` | 适配新组件接口 |
| `src/engine/CMakeLists.txt` | 移除 PhysicsWorld2D.cpp |
| `CMakeLists.txt` | 添加 Box2D FetchContent |

## Box2D 构建集成

```cmake
# CMakeLists.txt
FetchContent_Declare(
    box2d
    GIT_REPOSITORY https://github.com/erincatto/box2d.git
    GIT_TAG v3.1.1
    GIT_SHALLOW TRUE
)

# 关键: 禁用所有示例/测试/文档
set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
set(BOX2D_PROFILE OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(box2d)
```

编译命令（单线程）:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -j1  # 单线程编译
```

## 测试

现有 `tests/physics_smoke/main.cpp`（15 个自定义物理测试）重写为 Box2D 测试：

| 测试 | 描述 |
|------|------|
| `test_box_box_collision` | Box 与 Box 碰撞检测 |
| `test_circle_circle_collision` | Circle 与 Circle 碰撞检测 |
| `test_box_circle_collision` | Box 与 Circle 碰撞检测 |
| `test_raycast` | 射线检测 |
| `test_contact_events` | 接触事件（begin/end） |
| `test_sensor_events` | 传感器事件 |
| `test_ccd` | 连续碰撞检测 |
| `test_joint` | 关节约束 |
| `test_unit_conversion` | 像素↔米转换 |
| `test_tilemap_collision` | TileMap 链形状碰撞 |

## 工作量估算

| 阶段 | 内容 | 预估 |
|------|------|------|
| 1 | 集成 Box2D 构建系统 | 0.5 天 |
| 2 | 重构 ECS 组件 | 1 天 |
| 3 | 重写 PhysicsSystem | 2 天 |
| 4 | 重写 GameAPI | 0.5 天 |
| 5 | TileMap 碰撞迁移 | 1 天 |
| 6 | 测试重写 | 1 天 |
| 7 | 集成测试 + Bug 修复 | 1 天 |

## 注意事项

1. 所有 Box2D def 结构体必须用 `b2Default*()` 初始化（不可零初始化）
2. `b2ShapeDef` 需设 `enableContactEvents = true` 才能收到接触事件
3. 结束事件中 shape ID 可能无效（销毁后），需用 `b2Shape_IsValid` 检查
4. Box2D 射线使用 translation 向量（非终点坐标）
5. 编译需单线程 `-j1`
