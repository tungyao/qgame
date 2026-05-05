# QGame Mod 开发与 Native API 扩展指南

本文对应当前实现阶段 S3-S6：资源包、数据 Mod、Native Mod 加载，以及 Native 注册点扩展。

## 1. Mod 目录结构

项目目录约定如下：

```text
game/
├── game.json
└── mods/
    └── my_mod/
        ├── mod.json
        ├── assets/
        ├── prefabs/
        ├── scenes/
        ├── configs/
        └── native/
```

`game.json` 通过 `mods` 数组启用 Mod：

```json
{
  "id": "sample_game",
  "name": "Sample Game",
  "version": "0.1.0",
  "startupScene": "scene.game.main",
  "assetManifest": "assets/manifest.json",
  "mods": [ "my_mod" ]
}
```

启用顺序只在依赖和 priority 无法决定时参与排序。最终挂载规则是：

1. 依赖先挂载，依赖方后挂载。
2. `priority` 小的先挂载，大的后挂载。
3. priority 相同按 `game.json` 的 `mods` 顺序。
4. 仍相同按 Mod ID 字典序，保证确定性。

后挂载的同 ID 内容会覆盖先挂载内容。

## 2. 数据 Mod

`mod.json` 支持这些字段：

```json
{
  "id": "my_mod",
  "name": "My Mod",
  "version": "0.1.0",
  "engineVersion": "0.1.x",
  "type": "data",
  "priority": 10,
  "assetManifest": "assets/manifest.json",
  "prefabManifest": "prefabs/manifest.json",
  "sceneManifest": "scenes/manifest.json",
  "configManifest": "configs/manifest.json",
  "dependencies": []
}
```

单数和复数形式都支持，例如 `prefabManifest` 或 `prefabManifests`。

### 2.1 资源覆盖

`assets/manifest.json` 使用稳定 asset ID。覆盖游戏资源时复用原 ID：

```json
{
  "assets": [
    {
      "id": "texture.demo.character",
      "type": "texture",
      "source": "textures/character_hd.png"
    }
  ]
}
```

新增资源建议使用 `mod.<mod_id>` 命名空间：

```text
texture.mod.my_mod.fire_sword
font.mod.my_mod.ui
```

### 2.2 Prefab

`prefabs/manifest.json`：

```json
{
  "prefabs": [
    {
      "id": "prefab.mod.my_mod.fire_sword",
      "components": {
        "Name": { "s": "FireSword" },
        "Transform": { "x": 0, "y": 0, "rot": 0, "sx": 1, "sy": 1 },
        "Sprite": {
          "assetId": "texture.mod.my_mod.fire_sword",
          "srcX": 0, "srcY": 0, "srcW": 32, "srcH": 32
        }
      }
    }
  ]
}
```

同 ID prefab 会被后挂载 Mod 覆盖。

### 2.3 Scene

`scenes/manifest.json`：

```json
{
  "scenes": [
    { "id": "scene.mod.my_mod.preview", "path": "preview.scene.json" }
  ]
}
```

`path` 相对 scene manifest 所在目录解析。

### 2.4 Config

`configs/manifest.json`：

```json
{
  "configs": [
    {
      "id": "config.game.balance",
      "value": {
        "playerDamage": 42
      }
    }
  ]
}
```

Config 同样按稳定 ID 覆盖。游戏侧通过 `ConfigRegistry::findConfig(id)` 读取。

## 3. Native Mod

Native Mod 的 `mod.json`：

```json
{
  "id": "native_sample",
  "name": "Native Sample",
  "version": "0.1.0",
  "type": "native",
  "priority": 20,
  "library": "native/native_sample.dll",
  "dependencies": []
}
```

平台库后缀：

```text
Windows: .dll
Linux:   .so
macOS:   .dylib
```

入口函数固定为 C ABI：

```cpp
#include <engine/framework/NativeModAPI.h>

static void update(void* userData, float dt) {
    (void)userData;
    (void)dt;
}

QGAME_MOD_EXPORT bool qgame_mod_init(engine::QGameModContext* ctx) {
    if (!ctx || ctx->apiVersion != engine::QGAME_MOD_API_VERSION) {
        return false;
    }

    ctx->log->info("native_sample init");

    engine::QGameNativeSystemDesc sys{};
    sys.id = "native_sample.system";
    sys.update = update;
    return ctx->systems->register_system(ctx, &sys);
}

QGAME_MOD_EXPORT void qgame_mod_shutdown(engine::QGameModContext* ctx) {
    if (ctx && ctx->log) {
        ctx->log->info("native_sample shutdown");
    }
}
```

Native Mod 可用 API：

```text
ctx->assets->load_manifest(ctx, path)
ctx->scenes->register_scene(ctx, id, path)
ctx->scenes->register_manifest(ctx, path)
ctx->prefabs->register_manifest(ctx, path)
ctx->configs->register_manifest(ctx, path)
ctx->systems->register_system(ctx, desc)
ctx->events->subscribe(ctx, eventName, userData, handler)
ctx->events->emit(ctx, eventName, payload)
ctx->assetLoaders->register_loader(ctx, type, userData, load, unload)
ctx->log->info/warn/error(message)
```

注意：

- `qgame_mod_init` 只做注册，不直接创建场景实体。
- 引擎会在 unload 前注销 Native 注册的系统、事件和 asset loader。
- `components` 目前保留但不开放。组件注册要等反射、序列化、编辑器字段和存档兼容设计完成。
- Mod 不要保存 `QGameModContext*` 长期跨生命周期使用。

## 4. 宿主侧接入顺序

游戏或 demo 初始化时按这个顺序：

```cpp
engine::GameContext gameCtx{engineCtx};
engine::AssetLoaderRegistry assetLoaders{gameCtx};
engine::ConfigRegistry configs{gameCtx};
engine::PrefabRegistry prefabs{gameCtx};
engine::SceneManager scenes{gameCtx};

engine::GameManifest manifest;
engine::GameManifestLoader::loadFromFile(gameJsonPath, manifest);

engine::ModManager mods;
mods.mountGameAndMods(gameCtx, gameJsonPath, manifest);
mods.loadNativeMods(gameCtx);

// shutdown:
mods.shutdownNativeMods();
```

`mountGameAndMods` 会加载游戏 asset manifest、扫描 `mods/`、按规则挂载资源 manifest，并注册 scene/prefab/config manifest。

## 5. 如何扩展 Native API

给未来的引擎开发者，也就是你自己，扩展 API 时按下面步骤走：

1. 在 `src/engine/framework/NativeModAPI.h` 增加新的 C ABI 函数表或字段。
2. 只使用 C ABI 友好类型：整数、浮点、`const char*`、纯 POD struct、函数指针、`void* userData`。
3. 增加 `QGAME_MOD_API_VERSION`。破坏兼容时必须加版本号。
4. 在 `ModManager.cpp` 中实现桥接函数，把 C ABI 调用转成引擎内部 C++ API。
5. 在 `loadNativeMod` 填充新的函数表指针。
6. 在 `tests/framework_smoke/sample_native_mod.cpp` 增加一条真实调用。
7. 在 `tests/framework_smoke/main.cpp` 验证调用产生了可观察结果，并验证 shutdown 后注销干净。
8. 更新本文档，写清楚 Mod 作者该怎么用。

不要把这些东西直接暴露给 Native Mod：

```text
RenderDevice
CommandBuffer
RenderPipeline
GPU buffer / texture backend object
entt::registry 内部存储
C++ STL 对象所有权
```

如果某个能力必须访问这些对象，先在引擎侧做一个稳定门面，再通过 C ABI 暴露小而明确的函数。

## 6. 当前限制

- Native Component 注册尚未开放。
- AssetLoaderRegistry 目前是自定义类型加载器注册点，尚未自动接入 `AssetManager::loadTextureById` 等内建类型。
- EventBus 当前是 Native Mod 字符串事件总线，不等同于 EnTT typed dispatcher。
- Native 热重载未实现。
