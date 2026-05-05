# Game Framework 与 Native Mod 工程化计划

> 目标：QGame 引擎本体提供完整的基础游戏开发能力；Mod 系统不替代引擎开发能力，而是作为工程化扩展层，用于资源覆盖、内容追加、规则修改和 Native Gameplay 扩展。
> 当前决策：不规划脚本系统。Mod 侧以数据、资源包和 Native C++ 工程为主。

---

## 1. 定位

QGame 后续按三层演进：

```text
QGame Engine
├── Engine Core
│   ├── ECS
│   ├── Render / Audio / Input
│   ├── AssetManager / QPAK
│   ├── Time / Event
│   └── Platform
│
├── Game Framework
│   ├── GameInstance
│   ├── Scene / Level
│   ├── Entity / Component
│   ├── Prefab / Archetype
│   ├── Camera
│   ├── UI 基础
│   ├── SaveData 基础
│   └── Gameplay System 注册
│
└── Native Mod Layer
    ├── 新增资源
    ├── 覆盖资源
    ├── 修改配置
    ├── 注册组件
    ├── 注册系统
    ├── 注册 Prefab / Scene
    └── 扩展游戏内容
```

引擎本体应该能直接开发一个完整小型游戏。Mod 层用于扩展和修改这个游戏，而不是承担引擎应有的生命周期、场景、资源、输入、渲染等基础职责。

---

## 2. 核心原则

### 2.1 引擎提供基础开发能力

以下能力属于引擎或 Game Framework，不放到 Mod 内自行实现：

- 游戏生命周期：初始化、主循环、暂停、关闭。
- 场景系统：加载、切换、卸载、运行。
- Prefab / Archetype：实体模板、组件组合、资源引用。
- 资源系统：稳定 asset ID、manifest、QPAK、资源缓存、资源热加载入口。
- 输入、事件、时间、基础 UI、基础存档。
- Gameplay System 注册和调度框架。

### 2.2 Mod 只走公开扩展点

Mod 不直接访问底层实现对象，例如：

- `RenderDevice`
- `CommandBuffer`
- `RenderPipeline`
- GPU buffer / texture backend object
- ECS 内部存储结构

Mod 应通过稳定 API 注册内容和系统：

- `registerComponent`
- `registerSystem`
- `registerPrefab`
- `registerScene`
- `registerAssetManifest`
- `registerAssetLoader`
- `registerEventHandler`

### 2.3 Native Mod 使用 C ABI 边界

Mod 内部可以使用现代 C++，但引擎和 Mod 的动态库边界应尽量保持 C ABI：

```cpp
extern "C" QGAME_API bool qgame_mod_init(QGameModContext* ctx);
extern "C" QGAME_API void qgame_mod_shutdown(QGameModContext* ctx);
```

原因：

- 降低 C++ ABI、编译器、标准库版本差异风险。
- 便于跨平台加载 `.dll` / `.so` / `.dylib`。
- 便于后续做版本检查和 API 表扩展。

### 2.4 数据和资源优先，Native 逻辑后置

常见 Mod 需求应优先数据化：

- 新贴图、字体、音频、动画。
- 覆盖 UI、配置、Prefab。
- 新物品、新敌人、新关卡。
- 平衡性数值修改。

Native Mod 主要用于：

- 新 Gameplay System。
- 新 Component 类型。
- 新资源加载器。
- 复杂规则修改。
- 原生性能敏感逻辑。

---

## 3. 目录结构建议

项目级结构：

```text
qgame_project/
├── engine/
├── game/
│   ├── game.json
│   ├── assets/
│   │   └── manifest.json
│   └── native/
│       └── libmain_game.so
└── mods/
    ├── hd_textures/
    │   ├── mod.json
    │   └── assets/
    ├── balance_patch/
    │   ├── mod.json
    │   └── assets/
    └── fire_weapons/
        ├── mod.json
        ├── assets/
        └── native/
            └── libfire_weapons.so
```

引擎仓库内的示例可先采用：

```text
game/
├── game.json
├── assets/
└── mods/
    └── sample_mod/
        ├── mod.json
        ├── assets/
        └── native/
```

---

## 4. 配置格式

### 4.1 game.json

```json
{
  "id": "sample_game",
  "name": "Sample Game",
  "version": "0.1.0",
  "startupScene": "scene.sample.main",
  "assetManifest": "assets/manifest.json",
  "nativeLibrary": "native/libmain_game.so",
  "mods": [
    "hd_textures",
    "fire_weapons"
  ]
}
```

说明：

- `startupScene` 使用稳定 scene ID，不直接绑定磁盘路径。
- `assetManifest` 接入现有 Asset Pipeline。
- `nativeLibrary` 是主游戏 Native 模块，可选。
- `mods` 表示默认启用的 Mod 列表，后续可由启动器或编辑器管理。

### 4.2 mod.json

```json
{
  "id": "fire_weapons",
  "name": "Fire Weapons",
  "version": "0.1.0",
  "engineVersion": "0.1.x",
  "type": "native",
  "priority": 0,
  "assetManifest": "assets/manifest.json",
  "library": "native/libfire_weapons.so",
  "dependencies": []
}
```

字段说明：

| 字段 | 说明 |
|---|---|
| `id` | 全局唯一 Mod ID。 |
| `version` | Mod 自身版本。 |
| `engineVersion` | 兼容的引擎版本范围。 |
| `type` | `data` / `native`。 |
| `priority` | 可选，默认 `0`。相同 ID 覆盖冲突时，高 priority 后挂载并获胜。 |
| `assetManifest` | 可选，指向该 Mod 的资源 manifest。 |
| `library` | `native` 类型使用，指向动态库。 |
| `dependencies` | Mod 依赖列表，ModManager 按依赖排序加载。 |

---

## 5. 资源与覆盖规则

现有 Asset Pipeline 已支持 manifest、稳定 asset ID、QPAK。Mod 计划应建立在这些能力之上。

### 5.1 推荐引用方式

游戏代码、场景、Prefab、Mod 配置都应引用稳定 asset ID：

```cpp
TextureHandle tex = api.loadTextureById("texture.player.idle");
FontHandle font = api.loadFontById("font.ui.main");
```

不要在 Gameplay 逻辑里直接写文件路径。

### 5.2 挂载顺序

运行时资源查找顺序：

```text
enabled mods, last override wins
game project
engine assets
```

ModManager 必须把 `last override wins` 解析成稳定规则，不能依赖文件系统扫描顺序：

1. 先按 `dependencies` 做拓扑排序，被依赖的 Mod 先挂载，依赖方后挂载。
2. 依赖关系无法决定时，按 `priority` 升序挂载；数值越高越晚挂载，也就是越容易覆盖别人。
3. `priority` 相同且无依赖关系时，按 `game.json` / 启动器中的启用列表顺序挂载，列表越靠后越晚挂载。
4. 仍无法决定时，按 Mod ID 字典序作为最终 tie-breaker，保证不同机器结果一致。

相同 asset ID 被多个 Mod 覆盖时，引擎应明确记录完整覆盖链和最终 winner。例如：

```text
asset override: texture.player.idle
  game -> hd_textures(priority=10) -> balance_patch(priority=10)
  winner: balance_patch
```

同一套排序和 winner 规则也适用于 Prefab、Scene、配置等所有按稳定 ID 覆盖的资源类型。

Mod 覆盖资源时，优先覆盖 asset ID，而不是依赖相同文件路径。例如：

```json
{
  "id": "texture.player.idle",
  "type": "texture",
  "source": "textures/player_idle_hd.png"
}
```

### 5.3 命名空间约定

建议 asset ID 按来源命名：

```text
texture.engine.white
font.engine.default
texture.game.player.idle
scene.game.main
prefab.game.player
texture.mod.fire_weapons.fire_sword
prefab.mod.fire_weapons.fire_sword
```

允许 Mod 覆盖 `game.*` ID，但新增内容应使用 `mod.<mod_id>.*`，降低冲突概率。

---

## 6. Game Framework 公开能力

### 6.1 GameInstance

引擎负责窗口、主循环、输入、资源、渲染提交。游戏侧只实现逻辑生命周期：

```cpp
class GameInstance {
public:
    virtual bool onInit(GameContext& ctx);
    virtual void onUpdate(GameContext& ctx, float dt);
    virtual void onShutdown(GameContext& ctx);
};
```

### 6.2 SceneManager

职责：

- 按 scene ID 加载场景。
- 切换场景。
- 管理当前 World。
- 与 AssetManager、PrefabRegistry、SystemRegistry 协作。

### 6.3 PrefabRegistry

Prefab 用于新增实体类型和内容包扩展：

```json
{
  "id": "prefab.mod.fire_weapons.fire_sword",
  "components": {
    "Transform": {
      "position": [0, 0, 0]
    },
    "Sprite": {
      "texture": "texture.mod.fire_weapons.fire_sword"
    },
    "Weapon": {
      "damage": 30,
      "element": "fire"
    }
  }
}
```

Mod 可注册新的 Prefab，也可通过覆盖同名 ID 修改游戏已有 Prefab。

### 6.4 Component / System Registry

第一阶段可先开放系统注册，组件注册后置：

```cpp
ctx.systems->registerSystem("fire_weapons.burn", createBurnSystem);
ctx.prefabs->registerManifest("prefabs/manifest.json");
```

组件注册涉及序列化、编辑器、反射和存档兼容，必须和类型信息系统一起设计。

---

## 7. Native Mod API 草案

### 7.1 上下文

```cpp
struct QGameModContext {
    uint32_t apiVersion;

    AssetManagerAPI* assets;
    SceneRegistryAPI* scenes;
    PrefabRegistryAPI* prefabs;
    ComponentRegistryAPI* components;
    SystemRegistryAPI* systems;
    EventBusAPI* events;
    LogAPI* log;
};
```

### 7.2 生命周期

```cpp
extern "C" QGAME_API bool qgame_mod_init(QGameModContext* ctx);
extern "C" QGAME_API void qgame_mod_shutdown(QGameModContext* ctx);
```

可选后续扩展：

```cpp
extern "C" QGAME_API QGameModDesc qgame_mod_desc();
extern "C" QGAME_API void qgame_mod_on_game_start(QGameModContext* ctx);
extern "C" QGAME_API void qgame_mod_on_game_stop(QGameModContext* ctx);
```

`init` 只做注册，不建议直接创建场景实体。实体创建应放在 GameInstance、Scene load hook 或系统 update 中。

---

## 8. 分阶段计划

### S1. Game Framework 底座

- 增加 `GameInstance`。
- 增加 `GameContext`，集中暴露资产、场景、世界、事件、输入、日志。
- 增加 `SceneManager`。
- 明确启动流程：Engine init -> load game.json -> load assets -> create GameInstance -> load startup scene -> main loop。
- 现有 demo/gameplay 逻辑逐步迁移到 GameInstance。

**交付物**：不用 Mod，也能通过 `game.json + GameInstance + startupScene` 跑一个完整 demo。

### S2. Scene / Prefab 数据化

- 统一 Scene JSON 格式。
- 增加 Prefab JSON 格式和 `PrefabRegistry`。
- 场景引用 Prefab ID，而不是复制所有组件字段。
- 支持 Prefab 实例覆盖局部字段。
- Asset 引用统一使用 stable asset ID。

**交付物**：新增角色、UI、文字、Sprite 可以通过 Prefab 数据完成。

### S3. Mod 资源包

- 增加 `ModManifest`。
- 增加 `ModManager`。
- 扫描 `mods/`。
- 读取 `mod.json`。
- 按依赖和启用顺序挂载资源 manifest / QPAK。
- 支持 Mod 覆盖游戏 asset ID。

**交付物**：`hd_textures` 这类纯资源 Mod 能覆盖原游戏资源。

### S4. 数据 Mod

- Mod 可注册 scene / prefab / config manifest。
- 支持新增 Prefab。
- 支持覆盖 Prefab。
- 支持配置覆盖。
- 冲突检测：相同 ID 被多个 Mod 覆盖时，按确定性优先级规则选出 winner，并输出完整覆盖链。

**交付物**：不写 C++，也能新增道具、敌人、UI、关卡配置。

### S5. Native Mod 加载

- 增加跨平台动态库加载封装。
- 支持 Windows `.dll`、Linux `.so`、macOS `.dylib`。
- 查找 `qgame_mod_init` / `qgame_mod_shutdown`。
- 校验 `apiVersion`。
- 按依赖顺序 init，反向顺序 shutdown。
- 失败时给出可诊断日志。

**交付物**：`sample_native_mod` 能注册系统和 Prefab，并参与运行时。

### S6. Native 注册点扩展

- `SystemRegistry` 对 Mod 开放。
- `EventBus` 对 Mod 开放。
- `AssetLoaderRegistry` 对 Mod 开放。
- `ComponentRegistry` 设计反射、序列化、编辑器字段和存档兼容后再开放。

**交付物**：Native Mod 能添加 Gameplay 行为，但不触碰渲染后端和 ECS 内部存储。

### S7. 开发体验

- CMake 增加 Native Mod 模板。
- 增加 `tools/create_mod.py` 或等价脚手架。
- 编辑器显示已启用 Mod、资源覆盖和冲突。
- 桌面开发期支持 Mod reload 策略研究。

**交付物**：新建一个 Native Mod 工程不需要手写 CMake 和入口样板。

---

## 9. 与现有系统衔接点

| 系统 | 衔接方式 |
|---|---|
| Asset Pipeline | 继续使用 `AssetManager`、manifest、stable asset ID、QPAK。Mod 只新增挂载层和覆盖规则。 |
| SceneSerializer | Scene/Prefab 数据化需要复用现有组件序列化逻辑，并补充 prefab reference。 |
| RenderSystem | Mod 只通过组件、Prefab 和系统影响渲染，不直接提交 `CommandBuffer`。 |
| MSDF Text | 字体资源应通过 AssetManager 加载，Mod 可覆盖字体 asset ID 或新增字体。 |
| UI System | UI 资源和 prefab 可被 Mod 覆盖；UI runtime 仍归引擎。 |
| Editor | 后续显示 Mod 来源、资源覆盖链、Prefab override。 |

---

## 10. 风险与约束

| 风险 | 缓解 |
|---|---|
| Native ABI 不稳定 | 动态库边界使用 C ABI + API version + 函数表。 |
| Mod 直接依赖内部头文件 | 单独建立 `include/qgame_mod_api/`，示例 Mod 只包含公开 API。 |
| 组件注册影响序列化和编辑器 | 第一阶段先开放系统/Prefab/资源，组件注册等反射系统成熟后再开放。 |
| 资源覆盖冲突难排查 | ModManager 输出覆盖链，并在编辑器里展示最终来源。 |
| 动态库卸载后残留函数指针 | Mod shutdown 前注销所有系统、事件回调、资源加载器。 |
| 存档兼容 | 存档记录 Mod 列表和版本；缺失 Mod 时只允许安全降级或拒绝加载。 |

---

## 11. 暂不做

- 脚本系统。
- Mod 直接提交渲染命令。
- Mod 直接访问 OpenGL / SDL GPU 后端。
- 运行时下载可执行逻辑。
- 第一阶段开放任意组件反射注册。
- Native Mod 热重载作为首期目标。

---

## 12. 推荐近期优先级

1. `GameInstance`
2. `GameContext`
3. `SceneManager`
4. `PrefabRegistry`
5. `mod.json` / `game.json`
6. Mod 资源 manifest 挂载
7. 资源和 Prefab 覆盖日志
8. Native Mod Loader
9. `SystemRegistry` 对 Native Mod 开放
10. Native Mod CMake 模板
