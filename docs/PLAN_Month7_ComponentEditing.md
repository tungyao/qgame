# Month 7: 组件编辑工作流实现计划

> 当前状态：Month 6 已完成编辑器框架基础建设（ImGui 接入、视口离屏渲染、基础面板）
> 本阶段目标：实现 Inspector 组件化系统，支持所有现有组件的可编辑

---

## 一、当前状态

### 已完成
- EditorAPI 基础接口（离屏渲染、编辑器相机、临时实体、ImGui 提交）
- ImGui 已接入 SDL GPU 渲染后端
- 视口离屏预览链路已跑通
- EditorApplication 有 5 个面板：Editor / Hierarchy / Inspector / Viewport / Stats
- Inspector 已支持编辑 Transform、Camera、Sprite 三个组件（硬编码）

### 未完成
- Inspector 只能编辑已知组件，不是通用的组件系统
- 没有组件属性编辑器反射机制
- Hierarchy 仅有列表显示和选中，无创建/删除/重命名/拖拽
- 没有 Entity 命名系统
- 没有组件动态添加/删除功能

---

## 二、整体方案

采用 **自定义轻量反射系统**（而非 EnTT meta），理由：
1. **可控性强**：不依赖 EnTT 版本特性
2. **Editor 专用**：仅在 editor 层使用，不影响 engine/backend
3. **可扩展**：game 层可注册自定义组件的编辑器
4. **成本可接受**：现有 6 个组件手动注册元信息工作量不大

---

## 三、现有组件清单

| 组件名 | 文件 | 字段 |
|--------|------|------|
| Transform | RenderComponents.h | x, y, rotation, scaleX, scaleY |
| Sprite | RenderComponents.h | texture, srcRect, layer, tint, pivotX, pivotY |
| TileMap | RenderComponents.h | width, height, tileSize, tilesetCols, tileset, layers[3] |
| Camera | RenderComponents.h | zoom, primary |
| RigidBody | PhysicsComponents.h | velocityX, velocityY, gravityScale, isKinematic |
| Collider | PhysicsComponents.h | width, height, offsetX, offsetY, isTrigger |

---

## 四、实施计划

### Phase 1: Inspector 组件化系统

#### 1.1 组件元信息注册表 (`editor/ComponentReflect.h`)

```cpp
// 字段编辑器回调
using FieldEditor = std::function<void(void* component, const char* fieldName)>;

// 组件元信息
struct ComponentMeta {
    const char* name;                          // "Transform", "Sprite"
    size_t typeHash;                           // typeid(T).hash_code()
    std::vector<const char*> fieldNames;       // 字段名列表
    std::unordered_map<std::string, FieldEditor> editors;  // 字段名 → 编辑器
    std::function<void(entt::entity, entt::registry&)> drawFn;  // 完整组件编辑器
};

// 全局注册表
class ComponentRegistry {
public:
    template<typename T>
    static void registerComponent(const char* name, std::function<void(entt::entity, entt::registry&)> drawFn);
    
    static const ComponentMeta* get(size_t typeHash);
    static const ComponentMeta* getForComponent(entt::entity e, entt::registry& world);
    static std::vector<const ComponentMeta*> getAllRegistered();
};
```

#### 1.2 字段编辑器模板 (`editor/FieldEditors.h`)

为常见字段类型提供模板函数：

| 函数 | ImGui 控件 |
|------|-----------|
| `editFloat(void* ptr, const char* label, float step = 1.0f, float min = 0, float max = 0)` | `DragFloat` |
| `editInt(void* ptr, const char* label, int step = 1)` | `DragInt` |
| `editBool(void* ptr, const char* label)` | `Checkbox` |
| `editVec2(void* ptr, const char* label)` | `DragFloat2` |
| `editColor(void* ptr, const char* label)` | `ColorEdit4` |
| `editRect(void* ptr, const char* label)` | `DragFloat4` |

#### 1.3 组件编辑器注册 (`editor/ComponentEditors.cpp`)

为现有 6 个组件注册编辑器：

```cpp
// Transform: x, y, rotation, scaleX, scaleY
ComponentRegistry::registerComponent<Transform>("Transform", [](auto entity, auto& world) {
    auto& c = world.get<Transform>(entity);
    ImGui::DragFloat2("Position", &c.x);
    ImGui::DragFloat("Rotation", &c.rotation, 0.01f);
    ImGui::DragFloat2("Scale", &c.scaleX, 0.01f, 0.1f, 10.0f);
});

// Sprite: texture, srcRect, layer, tint, pivotX, pivotY
ComponentRegistry::registerComponent<Sprite>("Sprite", [](auto entity, auto& world) {
    auto& c = world.get<Sprite>(entity);
    ImGui::Text("Texture: %u", c.texture.index);
    ImGui::DragInt("Layer", &c.layer);
    // ColorEdit4, DragFloat2 for pivot...
});

// TileMap: width, height, tileSize, tileset, layers
ComponentRegistry::registerComponent<TileMap>("TileMap", [](auto entity, auto& world) {
    auto& c = world.get<TileMap>(entity);
    ImGui::DragInt("Width", &c.width);
    ImGui::DragInt("Height", &c.height);
    ImGui::DragInt("Tile Size", &c.tileSize);
    // tile editing (future)
});

// Camera: zoom, primary
ComponentRegistry::registerComponent<Camera>("Camera", [](auto entity, auto& world) {
    auto& c = world.get<Camera>(entity);
    ImGui::Checkbox("Primary", &c.primary);
    ImGui::DragFloat("Zoom", &c.zoom, 0.01f, 0.1f, 8.0f);
});

// RigidBody: velocityX, velocityY, gravityScale, isKinematic
ComponentRegistry::registerComponent<RigidBody>("RigidBody", [](auto entity, auto& world) {
    auto& c = world.get<RigidBody>(entity);
    ImGui::DragFloat2("Velocity", &c.velocityX);
    ImGui::DragFloat("Gravity Scale", &c.gravityScale);
    ImGui::Checkbox("Is Kinematic", &c.isKinematic);
});

// Collider: width, height, offsetX, offsetY, isTrigger
ComponentRegistry::registerComponent<Collider>("Collider", [](auto entity, auto& world) {
    auto& c = world.get<Collider>(entity);
    ImGui::DragFloat2("Size", &c.width);
    ImGui::DragFloat2("Offset", &c.offsetX);
    ImGui::Checkbox("Is Trigger", &c.isTrigger);
});
```

#### 1.4 Inspector 面板重构 (`editor/Inspector.cpp`)

```cpp
class InspectorPanel {
public:
    void draw(entt::entity selected, entt::registry& world);

private:
    void drawAddComponentMenu(entt::entity selected, entt::registry& world);
};
```

核心逻辑：
1. 检查 entity 是否有效
2. 遍历 entity 所有组件（`world.visit()`）
3. 从注册表获取组件元信息
4. 调用注册的 `drawFn` 渲染编辑器
5. 底部 "Add Component" 按钮：下拉菜单显示所有未挂载的组件类型

---

### Phase 2: game 层扩展点

#### 2.1 注册自定义组件编辑器接口

```cpp
// game/GameComponentEditors.h
#pragma once
#ifdef BUILD_EDITOR
void registerCustomComponentEditors();
#endif
```

#### 2.2 在 EditorApplication 初始化时调用

```cpp
// EditorApplication.cpp
#include "game/GameComponentEditors.h"

void EditorApplication::run() {
    ctx_.init(config);
    
    #ifdef BUILD_EDITOR
    registerCustomComponentEditors();  // ← 新增：注册 game 层自定义组件编辑器
    #endif
    
    // ... rest of init
}
```

#### 2.3 示例：game 层注册自定义组件（future）

```cpp
// game/GameComponentEditors.cpp
#ifdef BUILD_EDITOR
#include "editor/ComponentReflect.h"
#include "game/components/GameComponents.h"

void registerCustomComponentEditors() {
    // Harvestable 组件编辑器
    ComponentRegistry::registerComponent<Harvestable>("Harvestable", [](auto entity, auto& world) {
        auto& c = world.get<Harvestable>(entity);
        ImGui::InputText("Item ID", &c.itemId);
        ImGui::Checkbox("Ready", &c.ready);
    });
}
#endif
```

---

### Phase 3: EditorAPI 扩展（可选）

如需支持 Hierarchy 的实体创建/删除，在 EditorAPI 中添加：

```cpp
// EditorAPI.h
entt::entity createEntity(const char* name = nullptr);
void destroyEntity(entt::entity e);
void setEntityName(entt::entity e, const char* name);
const char* getEntityName(entt::entity e);
```

---

## 五、文件变更清单

| 操作 | 文件路径 | 描述 |
|------|----------|------|
| 新建 | `editor/ComponentReflect.h` | 反射系统核心：ComponentRegistry 类 |
| 新建 | `editor/ComponentReflect.cpp` | 注册表实现：全局注册表实例 |
| 新建 | `editor/FieldEditors.h` | 字段编辑器模板函数 |
| 新建 | `editor/ComponentEditors.cpp` | 现有 6 个组件的编辑器注册 |
| 新建 | `editor/Inspector.cpp` | 重构后的 Inspector 面板 |
| 新建 | `editor/Inspector.h` | InspectorPanel 类声明 |
| 修改 | `editor/EditorApplication.cpp` | 移除内联 drawInspector，调用 InspectorPanel |
| 修改 | `editor/EditorApplication.h` | 添加 InspectorPanel 成员 |
| 修改 | `editor/CMakeLists.txt` | 新增源文件 |
| 新建(可选) | `game/GameComponentEditors.h` | game 层扩展接口 |
| 新建(可选) | `game/GameComponentEditors.cpp` | 示例：自定义组件编辑器注册 |

---

## 六、关键约束

1. **不污染 backend**：所有代码在 `editor/` 目录（game 层扩展点除外）
2. **不污染 engine**：EditorAPI 保持 minimal，仅在需要时扩展
3. **EnTT 兼容性**：使用 `ctx_.world.visit()` 遍历组件，不直接依赖 EnTT meta 系统
4. **不添加外部依赖**：仅使用标准库 + ImGui + EnTT 现有能力

---

## 七、验收标准

### 功能验收
- [ ] Inspector 面板能显示任意实体已挂载的所有组件
- [ ] 每个组件的字段都能通过 ImGui 控件编辑
- [ ] 编辑器响应实时：修改数值后立即反映到 Viewport 预览
- [ ] "Add Component" 下拉菜单能添加新组件到选中实体
- [ ] "Remove Component" 功能（每个组件编辑器顶部有删除按钮）

### 代码质量验收
- [ ] 所有组件编辑器注册集中在一处（ComponentEditors.cpp）
- [ ] Inspector 面板代码 < 100 行，逻辑清晰
- [ ] FieldEditors 提供足够覆盖所有基础类型的编辑器模板
- [ ] game 层可通过简单调用注册自定义组件编辑器

### 扩展性验收
- [ ] 新增组件只需在 ComponentEditors.cpp 添加注册代码
- [ ] Inspector 无需修改即可自动支持新组件
- [ ] game 层的自定义组件可正常注册和编辑

---

## 八、开发顺序

1. **FieldEditors.h** — 提供基础字段编辑器模板
2. **ComponentReflect.h/cpp** — 注册表核心实现
3. **ComponentEditors.cpp** — 注册现有 6 个组件的编辑器
4. **Inspector.h/cpp** — 重构 Inspector 面板
5. **EditorApplication.cpp** — 集成 InspectorPanel
6. **测试与调试** — 验证所有组件可编辑
7. **(Optional) game 扩展点** — 如有需要

---

*Generated for Month 7: Component Editing Workflow*