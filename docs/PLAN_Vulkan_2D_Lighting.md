# Vulkan 2D 光照 / 阴影 / 反射落地计划

> 目标：为 QGame 建立一条不依赖额外材质贴图的 2D 光照主线。光照、遮挡、反射和夜晚氛围都由 ECS 组件、2D 几何、颜色参数、场景缓冲和 Vulkan compute 驱动。
>
> 立场：QGame 的 2D 光照不走 normal/roughness/specular 贴图路线，也不把完整硬件 ray tracing pipeline 作为第一目标。主线应聚焦 Vulkan compute：用 2D 几何做可见性、遮挡、软阴影和一次反射。硬件 RT 只作为未来原生 Vulkan 后端的高端加速选项。

---

## 0. 研究结论

当前代码和文档给出的事实：

- `SDLGPURenderDevice` 创建 GPU device 时优先请求 `"vulkan"`，并使用 SPIR-V 作为主 shader 格式，DXIL 是 Windows/D3D12 兼容路径。
- `IRenderDevice` 已有 `RendererCapabilities`、storage buffer、compute pipeline、`DispatchCmd`、`BarrierCmd` 和 GPU-driven sprite/particle 的初步接口。
- `supportsCompute` / `supportsStorageBuffer` 在 SDL GPU 路径中已经作为主线能力启用；`supportsStorageTexture` 目前仍是 false，说明 lighting texture / storage image 的落地需要单独推进。
- `RenderSystem` 当前 GPU-driven 路径仍在 CPU 侧收集 visible sprite、排序、上传 `visibleIndexBuffer`，再交给 `submitGPUDrivenPass()` 绘制。光照系统不能假设渲染已经完全 GPU-driven。
- Tile/Text/UI 仍走 CPU command overlay。2D lighting 第一阶段应只影响 World pass，UI/Text 默认在 composite 后叠加，避免文字被夜晚光照错误压暗。
- 现有文档 `PLAN_GPU_Performance_Roadmap.md` 已把 “Compute Lighting / 2D Ray Shadow” 放在阶段 7，但缺少具体数据结构、pass 顺序、Vulkan 能力边界和反射策略。

因此最合适的落地方式是：

```text
短期：SDL GPU / Vulkan compute lighting prototype
中期：RenderGraph + storage texture + tiled light list
长期：原生 VulkanRenderDevice，接入 ray query 或 ray tracing pipeline
```

---

## 1. 设计目标与非目标

### 目标

- 2D 动态光源：点光、聚光、面光、全局环境光。
- 几何遮挡：墙、门、树、角色、tile 边界都能投影。
- 夜晚场景：冷色 ambient、局部暖光、发光区域、bloom、湿地/水面反射。
- 无额外材质贴图：不要求 normal map、roughness map、reflection map。
- Vulkan 优先：shader、buffer layout 和 pass 设计以 SPIR-V/Vulkan compute 为第一验证目标。
- 可回退：OpenGL/CPU-batch 不追求同等效果，但要能关闭 lighting 保持场景可显示。

### 非目标

- 不做完整 2D PBR 材质系统。
- 不要求所有 sprite 作者额外提供光照贴图。
- 不把 OpenGL 3.3 作为动态光照优化目标。
- 第一阶段不实现 `VK_KHR_ray_tracing_pipeline`、BLAS/TLAS、SBT。
- 第一阶段不追求真实多 bounce 全局光照；夜晚氛围用艺术参数和一次反射解决。

---

## 2. 方案对比

| 方案 | 适配 QGame | 视觉能力 | 工程成本 | 结论 |
|---|---:|---:|---:|---|
| CPU 生成 shadow mesh | 高 | 中 | 低 | 可做 debug/fallback，但不是主线 |
| Fragment shader per-light | 中 | 中 | 低 | 多光源成本高，遮挡差 |
| Vulkan compute 2D ray casting | 很高 | 高 | 中 | 主线方案 |
| SDF / distance field ray marching | 中 | 高 | 中-高 | 可作为二阶段增强，但不要要求额外贴图 |
| Vulkan ray query | 中 | 高 | 高 | 原生 Vulkan 后端后评估 |
| Vulkan RT pipeline | 低-中 | 很高 | 很高 | 长期高端实验 |

最终选择：

```text
主线：Vulkan compute 2D ray lighting
增强：低分辨率 lighting texture + temporal accumulation + bilateral blur
长期：原生 Vulkan ray query / RT pipeline
```

---

## 3. ECS 组件设计

这些组件应放在 `src/engine/components/RenderComponents.h` 或拆成 `LightComponents.h`。为了序列化和编辑器，建议独立文件，避免 `RenderComponents.h` 继续膨胀。

```cpp
enum class Light2DType : uint8_t {
    Point = 0,
    Spot = 1,
    Area = 2
};

struct Light2D {
    Light2DType type = Light2DType::Point;
    core::Color color = core::Color::White;
    float radius = 256.f;
    float intensity = 1.f;
    float softness = 16.f;
    float coneAngle = 6.2831853f;
    float coneRotation = 0.f;
    int layerMask = 0xFFFFFFFF;
    bool castsShadow = true;
    bool visible = true;
};

struct LightOccluder2D {
    // 第一阶段只支持 AABB / segment；polygon 后续扩展。
    enum class Shape : uint8_t { AABB = 0, Segment = 1, Polygon = 2 };
    Shape shape = Shape::AABB;
    float width = 0.f;
    float height = 0.f;
    float ax = 0.f, ay = 0.f;
    float bx = 0.f, by = 0.f;
    float opacity = 1.f;
    bool castsShadow = true;
};

struct Reflector2D {
    // 反射区域可以是地面线段、水面线段或矩形湿地区域。
    enum class Shape : uint8_t { Segment = 0, AABB = 1 };
    Shape shape = Shape::Segment;
    float ax = 0.f, ay = 0.f;
    float bx = 0.f, by = 0.f;
    float width = 0.f;
    float height = 0.f;
    float reflectivity = 0.5f;
    float roughness = 0.35f;
    core::Color tint = core::Color::White;
    bool visible = true;
};

struct Environment2D {
    core::Color ambientColor = core::Color{32, 40, 56, 255};
    float ambientIntensity = 0.25f;
    float exposure = 1.f;
    float bloomThreshold = 1.1f;
    float wetness = 0.f;
    bool enabled = true;
};
```

### 序列化

`ComponentJson.cpp` 需要为以下组件增加 JSON：

- `Light2D`
- `LightOccluder2D`
- `Reflector2D`
- `Environment2D`

第一阶段字段应保持标量化，不保存动态数组，方便手写 scene/prefab。Polygon 后续再设计稳定 JSON 形状。

---

## 4. GPU 数据布局

GPU 端使用 storage buffer，不要求额外贴图。坐标统一为 world pixel。

```cpp
struct GPULight2D {
    float pos[2];
    float radius;
    float intensity;
    float color[4];
    float coneDir[2];
    float coneCos;
    float softness;
    uint32_t type;
    uint32_t layerMask;
    uint32_t castsShadow;
    uint32_t _pad0;
};

struct GPUOccluderSegment2D {
    float a[2];
    float b[2];
    float opacity;
    float height;
    uint32_t flags;
    uint32_t _pad0;
};

struct GPUReflector2D {
    float a[2];
    float b[2];
    float reflectivity;
    float roughness;
    float tint[4];
    uint32_t shape;
    uint32_t flags;
};

struct GPULightingUniforms {
    float cameraPos[2];
    float invZoom;
    float time;
    uint32_t viewportW;
    uint32_t viewportH;
    uint32_t lightCount;
    uint32_t occluderCount;
    uint32_t reflectorCount;
    float ambientColor[4];
    float ambientIntensity;
    float exposure;
    float wetness;
    float _pad0;
};
```

### 遮挡提取规则

- `LightOccluder2D::AABB` 在上传前展开为 4 条 segment。
- `LightOccluder2D::Segment` 直接上传。
- TileMap 第一阶段不自动为所有 tile 生成遮挡，先允许 game/editor 给关键墙体挂 `LightOccluder2D`。
- 第二阶段可从 TileMap collision layer 或 region id 生成 occluder segment，并按 chunk 合并相邻边。

---

## 5. Pass 结构

目标 pass 顺序：

```text
WorldColorPass
    - 现有 sprite/tile/particle 渲染

OccluderUpload / LightUpload
    - CPU 收集 Light2D / LightOccluder2D / Reflector2D
    - 上传到 storage buffer

LightCullingCompute
    - 可选，按 16x16 或 32x32 screen tile 建 light list

LightVisibilityCompute
    - 低分辨率或全分辨率 lighting texture
    - 每像素/每 tile sample 对光源发射 2D ray

ReflectionCompute
    - 对 Reflector2D 覆盖区域做一次反射采样
    - 输出 reflection texture

LightingCompositePass
    - sceneColor * lighting + reflection + emission-like bright color
    - exposure / night tint

Bloom/Tonemap
    - 后续接入

Text/UI Pass
    - 默认不受 World lighting 影响
```

### 与当前架构的衔接

当前 `IRenderDevice::submitPass()` 直接把 command list 渲染到 swapchain。为了 lighting，需要引入至少一个 world offscreen color target。

过渡方案：

1. 新增 `IRenderDevice::submitWorldLightingPass(...)`，由 SDL GPU 后端内部完成 offscreen color、lighting compute 和 composite。
2. 或先在 `RenderSystem` 中为 World camera 调用 `renderToTextureOffscreen()`，再把结果交给 lighting/composite。

推荐短期用第 1 种，避免在上层暴露太多 SDL GPU 纹理细节。中期 RenderGraph 完成后再把这些 pass 拆成显式节点。

---

## 6. 2D Ray Casting 算法

### 硬阴影基础版

对每个 lighting pixel 和每个影响范围内的 light：

```text
P = pixel world position
L = light position
if distance(P, L) > light.radius: skip
visibility = 1
for each occluder segment:
    if segment intersects ray(P -> L):
        visibility *= 1 - opacity
lightContribution = attenuation * visibility * color * intensity
```

线段相交使用 2D cross product。第一阶段直接遍历 occluder list，配合小场景和低分辨率 lighting texture 验证正确性。

### 软阴影

第二阶段做 area sampling：

```text
sampleCount = 4 或 8
for each sample point around light disk:
    cast ray(P -> sampleL)
visibility += unblocked ? 1 : 0
visibility /= sampleCount
```

优化：

- lighting texture 以 1/2 或 1/4 分辨率生成；
- blue-noise/jitter 随时间改变；
- temporal accumulation；
- bilateral blur，避免阴影穿过高对比边界。

### Tiled light culling

多光源时不能每像素遍历所有 light。建议屏幕 tile 为 16x16：

- `LightCullingCompute` 输出 `tileLightIndices` 和 `tileLightRanges`。
- 每个 lighting compute thread group 只遍历当前 tile 的 light list。
- 第一阶段可以先用全局 light list，超过 32 光源再启用 tile culling。

---

## 7. 几何反射方案

反射不依赖 reflection texture，也不依赖 normal map。只对显式 `Reflector2D` 区域启用：

```text
if pixel inside reflector region:
    N = reflector normal
    V = view direction approximation
    R = reflect(V, N)
    hit = ray cast from P along R against scene receiver proxy
    reflectedColor = sample WorldColorPass around hit screen position
    output = reflectedColor * reflectivity * tint
```

2D 游戏里 view direction 可简化为固定方向或由 camera 与 pixel 的相对位置估算。实际视觉更重要的是稳定和可控：

- 水面：沿水面法线做垂直翻转采样，再用波动函数扰动 UV。
- 湿地：采样上方/附近高亮颜色，乘 `Environment2D::wetness` 和 reflector roughness。
- 镜面：沿 segment normal 做更清晰的一次采样。
- 夜晚：提高暖色光源和高亮 sprite 对反射的权重，配合 bloom。

第一阶段推荐只做水面/湿地矩形反射：

```text
reflection = mirroredSceneColor * reflectivity * (1 - roughnessBlur)
```

第二阶段再接入几何 ray cast 命中。

---

## 8. 夜晚与发光

不额外做 emission map，但可以用现有数据表达“发光”：

- `Light2D` 自身是发光源。
- Sprite `tint` 或未来 `SpriteLightingFlags` 标记为 emissive。
- UI/Text 默认不进 world lighting。
- 夜晚由 `Environment2D` 控制整体曝光、环境色和湿度。

夜晚推荐参数：

```json
{
  "Environment2D": {
    "ambientColor": [24, 32, 52, 255],
    "ambientIntensity": 0.18,
    "exposure": 0.9,
    "bloomThreshold": 0.85,
    "wetness": 0.65
  }
}
```

视觉规则：

- 暗部不要纯黑，保留冷色 ambient。
- 灯光半径和 falloff 在夜晚更明显。
- 反射只强化亮部，不把整张画面复制到地面。
- Bloom 只吃高亮结果，不让 UI 文本参与世界 bloom。

---

## 9. Vulkan / SDL GPU 能力边界

### 短期必须补齐

`RendererCapabilities` 增加或明确以下能力：

- `supportsLighting2D`
- `supportsWorldOffscreenColor`
- `supportsStorageTexture`
- `supportsSampledRenderTarget`
- `preferredGraphicsAPI` 或 `backendName`

当前 `supportsStorageTexture=false`，所以 lighting texture 不能直接作为默认可用能力。需要在 SDL GPU 后端验证：

- storage texture 创建；
- compute write；
- graphics sample；
- render target sample；
- pass 间 barrier；
- resize 时重建。

### SDL GPU 路线

SDL GPU 仍可作为短中期主后端，但文档和代码应把它视为 “Vulkan-first over SDL GPU”：

- shader 以 GLSL/SPIR-V 为第一产物；
- HLSL/DXIL 后续补齐；
- Metal 路径只要求关闭 lighting 后能运行；
- 如果 SDL GPU 抽象阻塞 storage texture / render graph，创建原生 `VulkanRenderDevice`。

### 原生 Vulkan 路线

当需要以下能力时，进入原生 Vulkan 后端：

- descriptor indexing / bindless；
- async compute；
- timeline semaphore；
- `VK_KHR_ray_query`；
- `VK_KHR_ray_tracing_pipeline`；
- 精确的 barrier/render graph 控制。

---

## 10. 落地阶段

### 当前实现状态

2026-05 阶段 0/1 已落地：

- `Light2D` / `LightOccluder2D` / `Reflector2D` / `Environment2D` 组件已加入。
- `SceneSerializer` / `ComponentJson` 已支持保存和加载上述组件。
- `RendererCapabilities` 已增加 2D lighting 相关能力字段。
- `RenderFrameStats` 已增加 light / occluder / reflector / environment 计数。
- `RenderSystem` 每帧收集上述计数，作为 future compute pass 的数据路径探针。
- `game/demo3` 已加入，用 debug sprite 可视化夜晚、光源、遮挡物和反射区域。

### L0：文档和能力探针

交付：

- 本文档。
- `RendererCapabilities` 扩展草案。
- SDL GPU storage texture / render target sample 小测试。
- debug overlay 显示 lighting path、light count、occluder count、lighting resolution。

验收：

- 没有光照 pass 时现有游戏显示不变。
- 能明确记录 “lighting disabled: reason”。

### L1：组件和序列化

交付：

- `LightComponents.h`。
- `ComponentJson.cpp` 支持 `Light2D`、`LightOccluder2D`、`Reflector2D`、`Environment2D`。
- `RenderSystem` 收集这些组件并生成 CPU-side frame arrays。

验收：

- scene JSON 可保存/加载一盏灯、一个遮挡物、一个反射区域。
- 编辑器后续能基于组件元数据暴露字段。

### L2：单光源硬阴影 compute prototype

交付：

- `assets/shaders/lighting2d.comp.glsl`。
- light buffer / occluder segment buffer。
- 1/2 分辨率 lighting texture。
- composite 到 world color。

验收：

- 一个点光被 AABB 遮挡后产生稳定阴影。
- 1080p 下 1/2 分辨率、1 light、64 segments 预算 < 1ms 作为初始目标。

### L3：多光源和 tiled culling

交付：

- `tileLightList` buffer。
- 多光源混合、半径裁剪、layerMask。
- debug view：light tiles、shadow mask、lighting texture。

验收：

- 32 个小半径动态光源稳定运行。
- CPU 不再为每个像素/光源做任何工作。

### L4：软阴影和夜晚环境

交付：

- area light sample。
- temporal jitter。
- bilateral blur 或 separable blur。
- `Environment2D` 接入 composite。

验收：

- 火把、路灯、月光三种 preset 可在同一场景中切换。
- 夜晚暗部保留层次，光源边缘不明显闪烁。

### L5：反射

交付：

- `Reflector2D` 矩形/线段区域。
- 水面/湿地 screen-space reflection。
- roughness blur 和 wetness。

验收：

- 夜晚路灯在湿地/水面出现可控反射。
- 反射不会影响 UI/Text。

### L6：原生 Vulkan 评估

交付：

- `VulkanRenderDevice` 可行性 ADR。
- 对比 SDL GPU 能力缺口。
- ray query / RT pipeline 是否值得接入的结论。

验收：

- 只有当 SDL GPU 阻塞 storage texture、barrier 或 descriptor 控制时，才进入原生 Vulkan 实现。

---

## 11. 文件计划

预计新增：

```text
src/engine/components/LightComponents.h
src/engine/resources/Light2DBuffer.h
src/engine/resources/Light2DRenderer.h
src/engine/resources/Light2DRenderer.cpp
assets/shaders/lighting2d.comp.glsl
assets/shaders/lighting2d_composite.frag.glsl
docs/PLAN_Vulkan_2D_Lighting.md
```

预计修改：

```text
src/backend/renderer/IRenderDevice.h
src/backend/renderer/CommandBuffer.h
src/backend/renderer/sdl_gpu/SDLGPURenderDevice.h
src/backend/renderer/sdl_gpu/SDLGPURenderDevice.cpp
src/engine/scene/ComponentJson.cpp
src/engine/scene/SceneSerializer.h
src/engine/systems/RenderSystem.cpp
src/engine/CMakeLists.txt
assets/manifest.json
```

---

## 12. 风险与控制

| 风险 | 控制 |
|---|---|
| SDL GPU storage texture 能力不足 | 先做能力探针；必要时使用 offscreen render target + fragment composite 过渡 |
| 低分辨率 lighting 模糊导致像素风格脏 | 提供 nearest/upscale 和 bilateral 两种模式 |
| 多光源遍历成本过高 | L3 引入 tiled light culling |
| 遮挡物过多 | chunk 合并 TileMap 边界，动态遮挡物单独 buffer |
| 反射过度真实导致画面乱 | 只对显式 Reflector2D 区域启用，并限制亮部贡献 |
| 与 GPU-driven sprite 迁移互相阻塞 | Lighting 第一阶段允许 WorldColorPass 使用现有 CPU/GPU-driven 任一路径 |
| UI/Text 被错误光照影响 | Pass 顺序固定为 World lighting 后再叠加 Text/UI |

---

## 13. 决策记录

- **不做额外贴图**：QGame 的 2D 光照主线基于几何和参数，不要求资产作者维护 normal/roughness/emission map。
- **Vulkan 优先**：SPIR-V shader 和 Vulkan compute 是第一验证对象；其他 SDL GPU 后端是兼容目标，不是设计约束源头。
- **Compute 先于硬件 RT**：2D shadow/reflection 的核心是 2D 可见性查询，compute ray casting 更简单、更可控。
- **硬件 RT 后置**：只有在原生 Vulkan 后端成熟后，才评估 `VK_KHR_ray_query` 或 RT pipeline 是否带来足够收益。
- **World 与 UI 分离**：动态光照只作用于 WorldColorPass；Text/UI 默认保持可读性。
