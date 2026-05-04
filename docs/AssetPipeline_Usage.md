# Asset Pipeline 使用文档

> 覆盖已落地 Phase 1-5：manifest + 稳定 asset ID、资源编译/烘焙、场景 asset ID 序列化、增量缓存、QPAK 打包/挂载。
> 源码：`src/engine/assets/AssetManager.*`、`tools/resource_compiler.py`、`src/engine/scene/SceneSerializer.cpp`。

---

## 目录

1. [核心目标](#1-核心目标)
2. [manifest 写法](#2-manifest-写法)
3. [资源编译](#3-资源编译)
4. [运行时加载](#4-运行时加载)
5. [场景序列化](#5-场景序列化)
6. [字体资源](#6-字体资源)
7. [纹理 region ID](#7-纹理-region-id)
8. [动画资源](#8-动画资源)
9. [增量构建](#9-增量构建)
10. [QPAK 打包](#10-qpak-打包)
11. [限制与约定](#11-限制与约定)
12. [后续 Phase 预留接口](#12-后续-phase-预留接口)

---

## 1. 核心目标

资源管线把游戏代码从“文件路径”迁移到“稳定 asset ID”：

```cpp
TextureHandle tex = api.loadTextureById("texture.demo.character");
FontHandle font = api.loadFontById("font.demo.main");
AnimationHandle clip = api.loadAnimationById("animation.demo.test");
```

这样资源可以移动、烘焙、打包，只要 ID 不变，场景和游戏逻辑就不需要改。

当前阶段的职责：

- Phase 1：`AssetManager` 读取 manifest，按 ID 加载资源
- Phase 2：`tools/resource_compiler.py` 生成 baked manifest 和 baked asset root
- Phase 3：场景保存优先写 asset ID，路径字段只做兼容回退
- Phase 4：资源编译器用依赖 hash 做增量跳过
- Phase 5：资源编译器生成 `.qpak`，运行时按 `pak://` 虚拟路径读取

---

## 2. manifest 写法

源 manifest 放在：

```text
assets/manifest.json
```

示例：

```json
{
  "version": 1,
  "assets": [
    {
      "id": "texture.demo.character",
      "type": "texture",
      "source": "sprites/char.png"
    },
    {
      "id": "font.demo.main",
      "type": "font",
      "source": "fonts/DejaVuSans.ttf"
    },
    {
      "id": "animation.demo.test",
      "type": "animation",
      "source": "test_anim.json"
    }
  ]
}
```

字段说明：

- `id`：稳定资源 ID。场景、游戏代码、编辑器都应该引用它。
- `type`：当前支持 `texture`、`font`、`animation`、`sound` / `audio`。
- `source`：源资源路径，相对 manifest 所在目录。
- `baked`：运行时路径。源 manifest 可不写，由资源编译器生成 baked manifest 时补齐。

命名建议：

```text
texture.player.idle
texture.ui.button_primary
font.ui.main
animation.player.walk
sound.weapon.hit_light
```

ID 不要包含文件扩展名，也不要直接编码目录结构。目录可以变，ID 应该尽量稳定。

---

## 3. 资源编译

手动运行：

```bash
python3 tools/resource_compiler.py \
  --manifest assets/manifest.json \
  --out-dir build/assets \
  --cache build/assets/.assetcache.json \
  --pack build/assets/main.qpak
```

输出：

```text
build/assets/
├── manifest.baked.json
├── main.qpak
├── .assetcache.json
├── fonts/DejaVuSans.ttf
├── fonts/DejaVuSans.ttf.font
├── sprites/char.png
├── sprites/char.id.png
└── test_anim.json
```

`manifest.baked.json` 是运行时推荐加载的 manifest：

```json
{
  "version": 1,
  "pipelineVersion": 2,
  "packs": [
    {
      "id": "main",
      "path": "main.qpak"
    }
  ],
  "assets": [
    {
      "id": "font.demo.main",
      "type": "font",
      "source": "fonts/DejaVuSans.ttf",
      "baked": "pak://main/fonts/DejaVuSans.ttf"
    }
  ]
}
```

运行时 `AssetManager` 会优先使用 `baked`，没有 `baked` 时回退到 `source`。

---

## 4. 运行时加载

### 加载 manifest

```cpp
engine::EngineContext ctx;
ctx.init(cfg);

engine::GameAPI api{ctx};
api.loadAssetManifest("build/assets/manifest.baked.json");
```

CMake 已定义：

```cpp
QGAME_BAKED_MANIFEST
```

游戏目标可直接使用：

```cpp
api.loadAssetManifest(QGAME_BAKED_MANIFEST);
```

`asset_manifest_demo` 也会通过 `QGAME_DEMO_MANIFEST` 读取 baked manifest。

### 加载资源

```cpp
TextureHandle tex = api.loadTextureById("texture.demo.character");
FontHandle font = api.loadFontById("font.demo.main");
AnimationHandle anim = api.loadAnimationById("animation.demo.test");
SoundHandle snd = api.loadSoundById("sound.demo.click");
```

释放：

```cpp
api.releaseTexture(tex);
api.releaseFont(font);
api.releaseAnimation(anim);
api.releaseSound(snd);
```

旧路径 API 仍保留：

```cpp
TextureHandle tex = api.loadTexture("assets/sprites/char.png");
```

但新代码应优先使用 ID。路径 API 主要用于临时资源、程序化测试、旧场景兼容。

---

## 5. 场景序列化

`SceneSerializer` 保存 `Sprite` / `TileMap` / `TextComponent` 时会优先写 asset ID：

```json
{
  "Sprite": {
    "assetId": "texture.demo.character",
    "tex": "build/assets/sprites/char.png",
    "srcX": 0,
    "srcY": 0,
    "srcW": 32,
    "srcH": 48
  }
}
```

读取规则：

1. 如果存在 `assetId` / `fontId`，优先 `loadTextureById()` / `loadFontById()`
2. 如果 ID 不存在或加载失败，回退到旧路径字段 `tex` / `font`

这让旧场景可以继续读，新保存的场景会自然迁移到 asset ID。

注意：保存前必须先加载 manifest，否则 `AssetManager` 无法把 handle 反查为 asset ID。

```cpp
api.loadAssetManifest(QGAME_BAKED_MANIFEST);
api.saveScene("save/demo.scene.json");
```

---

## 6. 字体资源

运行时约定：

```text
fonts/DejaVuSans.ttf
fonts/DejaVuSans.ttf.font
```

`AssetManager::loadFont("fonts/DejaVuSans.ttf")` 会实际读取 sibling：

```text
fonts/DejaVuSans.ttf.font
```

资源编译器处理顺序：

1. 如果源目录已经有 `DejaVuSans.ttf.font`，直接复制
2. 否则如果有 `DejaVuSans.png` + `DejaVuSans.json`，调用 `tools/bake_font.py` 生成 `.font`
3. 否则如果传入 `--msdf-atlas-gen`，从 TTF 自动生成 atlas/json，再生成 `.font`

完整自动烘焙示例：

```bash
python3 tools/resource_compiler.py \
  --manifest assets/manifest.json \
  --out-dir build/assets \
  --cache build/assets/.assetcache.json \
  --msdf-atlas-gen /path/to/msdf-atlas-gen \
  --font-size 32 \
  --font-pxrange 4
```

`font-size` 和 `font-pxrange` 会进入增量 hash。改这些参数会触发字体重烘焙。

---

## 7. 纹理 region ID

纹理支持 sibling region ID 图：

```text
assets/sprites/char.png
assets/sprites/char.id.png
```

资源编译器会同时复制：

```text
build/assets/sprites/char.png
build/assets/sprites/char.id.png
```

运行时 `AssetManager::loadTexture()` 会自动查找同名 `.id.png` 并创建 R8 nearest 纹理。`Tinting` 组件可直接使用，不需要在 manifest 中额外声明 region ID 图。

详细用法见 `docs/RegionTinting_Usage.md`。

---

## 8. 动画资源

`animation` 当前读取 Aseprite JSON。manifest：

```json
{
  "id": "animation.demo.test",
  "type": "animation",
  "source": "test_anim.json"
}
```

资源编译器会：

- 复制 JSON
- 如果 `meta.image` 存在，复制对应 spritesheet
- 如果 spritesheet 有 sibling `.id.png`，一起复制

运行时：

```cpp
AnimationHandle anim = api.loadAnimationById("animation.demo.test");
```

如果需要加载某个 tag，当前路径 API 支持：

```cpp
AnimationHandle walk = api.loadAnimation("assets/player.json#walk");
```

后续建议把 tag 拆成独立 manifest ID：

```json
{
  "id": "animation.player.walk",
  "type": "animation",
  "source": "player.json#walk"
}
```

---

## 9. 增量构建

资源编译器写入：

```text
build/assets/.assetcache.json
```

缓存记录：

- asset ID
- asset type
- 源文件 hash
- 依赖文件 hash
- bake 参数
- 输出文件列表

只要 hash 和输出文件都没变，下次编译会跳过实际复制/烘焙。

CMake 集成：

```bash
cmake --build build --target qgame_assets
cmake --build build --target asset_manifest_demo
```

`qgame_assets` 是 `ALL` target，正常构建 `game` / `asset_manifest_demo` 时会自动执行。

---

## 10. QPAK 打包

CMake 默认会让 `qgame_assets` 同时生成：

```text
build/assets/manifest.baked.json
build/assets/main.qpak
```

打包后的 manifest 会包含 `packs` 列表，并把每个资源的 `baked` 字段写成：

```json
{
  "id": "texture.demo.character",
  "type": "texture",
  "baked": "pak://main/sprites/char.png"
}
```

运行时只需要照常加载 baked manifest：

```cpp
api.loadAssetManifest(QGAME_BAKED_MANIFEST);
TextureHandle tex = api.loadTextureById("texture.demo.character");
```

`AssetManager` 会在读取 manifest 时自动 mount `main.qpak`。当前支持从 QPAK 读取：

- `texture`，包括 sibling `*.id.png`
- `font`，读取 `*.ttf.font`
- `animation`，包括 Aseprite JSON 的 `meta.image`
- `sound` / `audio`，通过 SDL_mixer 内存加载

QPAK 文件格式很小：

```text
[file blobs][json index][uint64 index_size]["QPAK"]
```

index 记录虚拟路径、offset、size 和 sha256。当前没有压缩和加密，目标是先把运行时路径从散文件切到 pack/VFS。

---

## 11. 限制与约定

- 当前没有运行时热更新，修改资源后需要重新运行 `qgame_assets` 并重启程序。
- 当前没有自动删除 stale 输出文件。删除 manifest 记录后，旧 baked 文件可能仍留在 `build/assets`。
- QPAK 当前不做压缩、加密、分块读取缓存或版本兼容迁移。
- `GameAPI::playMusic(path)` 仍是直接路径 API；音乐流如果要走 pack，后续建议接入 AssetManager 或增加 stream asset ID。
- `AssetManager` 当前以路径作为缓存 key。pack 模式下 key 是 `pak://...` 虚拟路径。
- 场景保存 asset ID 依赖 handle 反查。资源必须来自已加载 manifest，或者路径能匹配 manifest 解析后的路径。
- 字体运行时仍要求 `path + ".font"`，所以 baked manifest 的 font `baked` 指向 `.ttf` 路径，而不是 `.font` 路径。

---

## 12. 后续 Phase 预留接口

### Pack/VFS 后续增强

推荐方向：

- QPAK 压缩：按文件或按 block 压缩
- 多 pack mount：base/game/dlc/mod 分层覆盖
- pack index 校验：加载时可选择验证 sha256
- 文件系统抽象：让 `FileSystem`、Audio stream、编辑器预览共用同一套 VFS

### Phase 6：热更新 / 编辑器集成

可复用 `.assetcache.json` 的 dependency 列表：

- 文件 watcher 监听依赖
- 单资源重新编译
- `AssetManager` 增加 `reloadAssetById(id)`
- 编辑器资源面板显示 source、baked、hash、依赖、加载状态

### Phase 7：资源生命周期与诊断

可基于 `AssetManager` 的现有表扩展：

- handle -> path
- handle -> asset ID
- path -> refCount
- asset ID -> type/source/baked

建议后续增加调试 API：

```cpp
std::vector<AssetDebugInfo> AssetManager::debugAssets() const;
bool AssetManager::isAssetLoaded(const std::string& id) const;
size_t AssetManager::estimateAssetMemory(const std::string& id) const;
```

这些 API 不影响当前运行时加载路径，但能支撑编辑器和内存诊断。
