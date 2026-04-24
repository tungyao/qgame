# 将 Engine 动态化（SHARED 库）而不引起 EnTT 问题

## 问题背景

EnTT 使用模板和内联函数，当 `engine` 作为 SHARED 库（DLL）时：
- `entt::registry world` 在 DLL 中构造
- `GameAPI::addComponent<T>()` 模板在可执行文件中实例化
- 跨 DLL 边界访问 EnTT 内部数据结构会出错

## 解决方案：显式模板实例化

### 1. 架构说明

```
┌─────────────────────────────────────────────────────┐
│  engine.dll (SHARED)                                 │
│  ├─ GameAPI.h                 (模板声明)             │
│  ├─ GameAPI.cpp               (非模板实现)           │
│  ├─ GameAPI_ExternTemplates.h (extern template 声明) │
│  └─ GameAPI_ExplicitInstantiation.cpp               │
│       (显式实例化所有组件类型)                        │
└─────────────────────────────────────────────────────┘
         ↓ 链接
┌─────────────────────────────────────────────────────┐
│  game.exe                                            │
│  ├─ main.cpp                                         │
│  └─ 调用 api.addComponent<Transform>(...)           │
│       ↓ 查找符号                                     │
│       → 在 engine.dll 中找到显式实例化的版本          │
│       → 不会在本地实例化模板                         │
└─────────────────────────────────────────────────────┘
```

### 2. 关键文件

#### GameAPI.h
```cpp
template<typename T>
T& addComponent(entt::entity e, T component);

// 模板实现在头文件（内联）
template<typename T>
T& GameAPI::addComponent(entt::entity e, T component) {
    return ctx_.world.template emplace_or_replace<T>(e, std::move(component));
}

// 包含 extern template 声明
#include "GameAPI_ExternTemplates.h"
```

#### GameAPI_ExternTemplates.h
```cpp
// 告诉编译器：不要在本地实例化，符号在 DLL 中
extern template QGAME_ENGINE_API Transform& GameAPI::addComponent<Transform>(entt::entity, Transform);
extern template QGAME_ENGINE_API Sprite&    GameAPI::addComponent<Sprite>(entt::entity, Sprite);
// ... 其他组件类型
```

#### GameAPI_ExplicitInstantiation.cpp
```cpp
// 在 DLL 中实例化模板，导出符号
template Transform& GameAPI::addComponent<Transform>(entt::entity, Transform);
template Sprite&    GameAPI::addComponent<Sprite>(entt::entity, Sprite);
// ... 其他组件类型
```

### 3. 添加新组件类型

当需要添加新的组件类型时，必须在以下两个文件中添加：

#### 步骤1：在 GameAPI_ExternTemplates.h 中添加声明
```cpp
extern template QGAME_ENGINE_API MyComponent& GameAPI::addComponent<MyComponent>(entt::entity, MyComponent);
extern template QGAME_ENGINE_API MyComponent& GameAPI::getComponent<MyComponent>(entt::entity);
extern template QGAME_ENGINE_API bool GameAPI::hasComponent<MyComponent>(entt::entity) const;
```

#### 步骤2：在 GameAPI_ExplicitInstantiation.cpp 中添加实例化
```cpp
template MyComponent& GameAPI::addComponent<MyComponent>(entt::entity, MyComponent);
template MyComponent& GameAPI::getComponent<MyComponent>(entt::entity);
template bool GameAPI::hasComponent<MyComponent>(entt::entity) const;
```

### 4. 为什么这样可以解决问题？

1. **`extern template`**：告诉编译器"这个模板已经在其他地方实例化了，不要在本地实例化"
2. **显式实例化**：在 DLL 中强制实例化模板，生成符号并导出
3. **结果**：可执行文件调用时，链接到 DLL 中的实例化版本，而不是自己实例化

```
没有 extern template：
  game.exe 中调用 addComponent<Transform>()
  → 编译器在 game.exe 中实例化模板
  → 访问 engine.dll 中的 entt::registry
  → 跨 DLL 边界 → 出错

有 extern template：
  game.exe 中调用 addComponent<Transform>()
  → 编译器看到 extern template 声明
  → 不实例化，等待链接
  → 链接时找到 engine.dll 中的符号
  → 调用 engine.dll 中的实例化版本
  → 同一编译单元内访问 entt::registry
  → 正常工作
```

### 5. 已支持的组件类型

**渲染组件**（RenderComponents.h）：
- `EntityID`, `Name`, `Transform`, `Camera`, `Sprite`, `TileMap`

**物理组件**（PhysicsComponents.h）：
- `RigidBody`, `Collider`

**动画组件**（AnimatorComponent.h）：
- `AnimatorComponent`

**文本组件**（TextComponent.h）：
- `TextComponent`

### 6. 注意事项

1. **必须预知所有组件类型**：这是显式实例化的代价
2. **添加新组件需要重新编译 engine**：因为要添加新的实例化
3. **第三方组件**：如果游戏需要自定义组件，建议：
   - 方案A：将自定义组件也放在 engine 中
   - 方案B：使用静态库而不是动态库
   - 方案C：使用类型擦除接口（更复杂，但更灵活）
