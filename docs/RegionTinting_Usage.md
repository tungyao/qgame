# Region Tinting

按 region ID 给 sprite 分区染色。一张主纹理 + 一张 R8 单通道 region ID 图，
ECS 端挂 `Tinting` 组件指定每个 region 的颜色。

适合：换装配色、NPC 外观多样化、状态效果（中毒/湿身）、装备覆盖色。

---

## 资产约定

每张可染色 sprite **必须**有同名 sibling region ID 图：

```
assets/sprites/char.png      ← 主纹理（RGBA）
assets/sprites/char.id.png   ← sibling region ID（R8 单通道，引擎自动加载）
```

- 像素必须**完全对齐**且**同尺寸**，否则忽略并 logWarn
- 每像素值即 region ID（0..255），引擎当前用 ID 0..15
- **ID 0 = 背景 / 不染色**（passthrough）
- ID 1..15 = 可染色部位（约定：1=皮肤 2=头发 3=上衣 4=裤子 5=鞋 ...）
- ID >= 16 在 shader 里被忽略（`Tinting::MAX_REGIONS = 16`）

主纹理画法：可染色区域**用纯白填充**，这样最终颜色 = Tinting 设置色（multiply
对 white = identity）；不染色细节（眼睛、轮廓）画原色即可。

### 生成 ID 图

**方式 A — 离线 bake 脚本** `tools/bake_region_id.py`：

美术用饱和原色画"标记图"（比如上衣纯红、裤子纯绿），写一份颜色→ID 的 JSON：

```json
{
  "FF0000": 1,
  "00FF00": 3,
  "0000FF": 4
}
```

```bash
python tools/bake_region_id.py marker.png mapping.json out.id.png
```

**方式 B — 美术工具直接画**：在 Aseprite/Photoshop 里把每个部位填成
对应灰度值（R=1, R=2, R=3...），导出 PNG。人眼分辨不出 R=1 和 R=4，但程序读得对。

---

## ECS 用法

```cpp
#include <engine/components/RenderComponents.h>

auto e = api.spawnEntity();
api.addComponent(e, engine::Transform{ x, y });

engine::Sprite sp{};
sp.texture = api.assetManager().loadTexture("assets/sprites/char.png");
sp.srcRect = { 0, 0, 32, 48 };
api.addComponent(e, sp);

// 每个 region 单独设置染色；enabled=false 的 slot 保持 passthrough
engine::Tinting tnt{};
tnt.slots[1] = { true, core::Color{255, 210, 170, 255} };  // 皮肤
tnt.slots[2] = { true, core::Color{ 80,  50,  30, 255} };  // 头发
tnt.slots[3] = { true, core::Color{ 60,  80, 160, 255} };  // 上衣
tnt.slots[4] = { true, core::Color{ 40,  40,  40, 255} };  // 裤子
tnt.slots[5] = { true, core::Color{ 30,  20,  10, 255} };  // 鞋
api.addComponent(e, tnt);
```

**没有 sibling `.id.png`** → `regionIdTexture()` 返回空 → `hasRegion=false`，
Tinting 组件被忽略，sprite 正常无染色渲染。

**没挂 Tinting 组件** → 同上，无染色。

---

## 运行时切换颜色

直接改组件成员，下一帧生效：

```cpp
auto& tnt = api.getComponent<engine::Tinting>(e);
tnt.slots[3].color = core::Color{255, 0, 0, 255};   // 上衣立刻变红
tnt.slots[3].enabled = false;                       // 暂时关闭染色
```

平滑过渡：每帧自己 lerp（参考 `game/main.cpp` 的 Region Tint Demo 段）。
引擎不内置 tween，因为颜色叠加规则属于 game 层逻辑。

---

## 多源叠加（基础 + 装备 + 状态 buff）

引擎**不**支持同 entity 多个 `Tinting` 组件叠加。在 game 层自己合并：

```cpp
struct CharacterColors {
    TintPalette base;       // 基础肤色发色
    TintPalette equipment;  // 装备覆盖色（可选）
    bool        poisoned;   // 状态 buff
};

void rebuildTinting(entt::entity e, const CharacterColors& cc) {
    auto& tnt = registry.get_or_emplace<engine::Tinting>(e);
    // 优先级：装备 > 基础
    tnt.slots[3] = { true, cc.equipment.shirt.has_value()
                            ? *cc.equipment.shirt : cc.base.shirt };
    // 状态 buff：所有可见部位偏绿
    if (cc.poisoned) {
        for (auto& s : tnt.slots) {
            if (s.enabled) {
                s.color.r = static_cast<uint8_t>(s.color.r * 0.6f);
                s.color.b = static_cast<uint8_t>(s.color.b * 0.6f);
            }
        }
    }
}
```

只有装备/状态变化时调用 `rebuildTinting`，不需要每帧重算。

---

## 性能与限制

- **Batch 拆分**：相同 base texture 但不同 LUT 的 sprite 必须独立 draw call。
  10 个 NPC 各自配色 = 10 次 draw call。星露谷场景规模可接受，性能瓶颈前不优化。
- **MAX_REGIONS = 16**：超出需要换 LUT 纹理方案（动 shader）。
- **per-cmd 内存开销**：DrawSpriteCmd 多 ~280 字节（16×Color + handle + bool）。
  1000 sprite/帧 = 280KB，可忽略。
- **dummy R8 纹理**：无 region 的 sprite 也会绑定 1×1 R8 dummy 到 sampler 槽 1，
  避免 sampler 未绑定的 UB。开销 = 一次额外 bind，可忽略。

---

## 后端实现要点

- HLSL shader：`assets/shaders/sprite.frag.hlsl`，`PS sampler` 升到 2 个
  (t0=base, t1=region)，PS UBO 1 个 (b0 = `regionTints[16]` + `hasRegion`)。
- GLSL shader（OpenGL 内嵌于 `GLRenderDevice.cpp`）：等价 uniform `uRegionTex`
  + `uRegionTints[16]` + `uHasRegion`。
- SDL_GPU `loadShader`：sprite frag 改为 `numSamplers=2, numUBOs=1`。
- 双后端的 batch key 都加了 `(regionTex, regionTints memcmp)`，状态变化即切批。

---

## 文件清单

| 文件 | 作用 |
|---|---|
| `src/engine/components/RenderComponents.h` | `Tinting` 组件 |
| `src/engine/assets/AssetManager.{h,cpp}` | sibling `.id.png` 自动加载 + `regionIdTexture(handle)` |
| `src/engine/systems/RenderSystem.cpp` | 写入 `Tinting` 到 `DrawSpriteCmd` |
| `src/backend/renderer/CommandBuffer.h` | `DrawSpriteCmd.hasRegion / regionTex / regionTints` |
| `src/backend/renderer/IRenderDevice.h` | `TextureFormat::R8` |
| `src/backend/renderer/sdl_gpu/SDLGPURenderDevice.cpp` | SDL_GPU 实现 |
| `src/backend/renderer/opengl/GLRenderDevice.cpp` | OpenGL 实现 |
| `assets/shaders/sprite.frag.hlsl` | HLSL 片段 shader |
| `tools/bake_region_id.py` | 离线烤 ID 图 |
| `assets/sprites/char.png` + `char.id.png` | demo 资产（main.cpp 用） |
