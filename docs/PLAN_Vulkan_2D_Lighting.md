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

### 5.1 Render Graph 管理升级计划

当前实现的问题是 pass 顺序靠 `RenderSystem` 手工调用后端接口维持，资源也由后端临时管理：

- World sprite/tile、particle、lighting overlay、UI/Text 直接或间接写 swapchain。
- lighting fallback 目前通过 alpha-blended overlay 近似暗化和径向加亮。
- SDL GPU compute lighting texture 已能生成，但还没有严格的 `WorldColor -> Lighting -> Composite` 数据流。
- 多 camera / World+UI 混合时，后续 pass 覆盖前面 pass 的风险较高，缺少统一依赖描述和调试输出。

升级目标是引入一个轻量 Render Graph，把“要渲染什么”和“写到哪里、读什么资源”显式化。第一版不追求复杂泛型框架，只解决 2D lighting 需要的资源生命周期、pass 顺序和调试可见性。

#### 核心抽象

```cpp
enum class RGResourceType {
    Texture,
    Buffer,
    Swapchain,
};

enum class RGAccess {
    ReadSampled,
    ReadStorage,
    WriteColor,
    WriteStorage,
    Present,
};

struct RGTextureDesc {
    uint32_t width;
    uint32_t height;
    TextureFormat format;
    TextureUsage usage;
    const char* debugName;
};

struct RGPass {
    const char* name;
    std::vector<RGResourceUse> reads;
    std::vector<RGResourceUse> writes;
    ExecuteFn execute;
};
```

第一阶段 Render Graph 可以是 immediate builder：

1. `RenderSystem` 每个 camera 构建一组 pass 描述。
2. 后端根据 pass 描述顺序执行，不做复杂重排。
3. SDL GPU 后端根据 resource use 插入正确的 render/compute/copy pass 边界。
4. debug overlay 显示 graph pass 名称、resource 尺寸、lighting 是否 composite 到 swapchain。

#### 资源模型

World lighting 至少需要以下 transient resources：

```text
WorldColor      RGBA8, full resolution, COLOR_TARGET | SAMPLER
LightingTexture RGBA8/RGBA16F, half/full resolution, STORAGE_WRITE | STORAGE_READ | SAMPLER
LightingBlur    RGBA8/RGBA16F, half/full resolution, STORAGE_WRITE | STORAGE_READ
CompositeColor  optional, full resolution, COLOR_TARGET | SAMPLER
```

短期可以不引入 `CompositeColor`，`LightingCompositePass` 直接写 swapchain；后续接 Bloom/Tonemap 时再把 composite 输出改成中间纹理。

#### Pass 顺序

```text
AcquireSwapchain

WorldColorPass
    writes: WorldColor
    reads: sprite textures, tile textures, particle buffers

LightUploadPass
    writes: LightBuffer, SegmentBuffer, ReflectorBuffer

LightCullingCompute
    reads: LightBuffer
    writes: TileRanges, TileLightIndices

LightVisibilityCompute
    reads: LightBuffer, SegmentBuffer, ReflectorBuffer, TileRanges, TileLightIndices
    writes: LightingTexture

LightingBlurCompute
    reads/writes: LightingTexture, LightingBlur

LightingCompositePass
    reads: WorldColor, LightingTexture
    writes: Swapchain or CompositeColor

UIPass
    reads: font/sprite textures
    writes: Swapchain or CompositeColor

Present
```

#### RenderSystem 侧改造

- World camera 不再直接 `submitPass(..., swapchain)`。
- RenderSystem 把 World draw commands 交给 `submitWorldGraph()` 或 `submitFrameGraph()`。
- UI/Text camera 保持独立 pass，默认在 lighting composite 后执行。
- `RenderFrameStats` 增加：
  - `renderGraphPassCount`
  - `worldColorPassCount`
  - `lightingCompositeCount`
  - `lighting2DSubmitCount`
  - `lightingDebugMode`

#### 后端边界

短期接口建议：

```cpp
struct WorldLightingSubmitInfo {
    CameraData camera;
    std::vector<const RenderCmd*> worldCommands;
    std::vector<const RenderCmd*> uiCommands;
    Lighting2DParams lighting;
    bool clearEnabled;
    core::Color clearColor;
};

virtual void submitWorldLightingGraph(const WorldLightingSubmitInfo& info) = 0;
```

这样 SDL GPU 可以在内部拥有 `WorldColor/Lighting/Composite` 资源，不把 SDL 原生 texture 暴露给 engine 层。OpenGL 后端可以先用同样接口走 fallback：WorldColor FBO + fragment/fullscreen composite 或继续 alpha fallback，但必须保持 pass 顺序一致。

### 5.2 SDL GPU Offscreen WorldColor + Lighting Composite 计划

当前 SDL GPU lighting 的最大缺口是 composite 仍是 swapchain overlay。正确目标是：

```text
finalColor = WorldColor * Lighting + Reflection + Emissive
```

#### 阶段 A：WorldColor offscreen

交付：

- 在 `SDLGPURenderDevice` 中新增/复用 full-res `worldColorTexture_`。
- `WorldColorPass` 将 World sprite/tile/particle 渲染到 `worldColorTexture_`，不写 swapchain。
- UI/Text 暂时仍写 swapchain，但必须在 composite 后执行。

验收：

- 关闭 lighting 时，`WorldColor -> Composite(copy) -> Swapchain` 与原直接渲染视觉一致。
- demo3 状态栏显示 `worldColor=1 composite=1`。

#### 阶段 B：Fragment composite shader

新增 shader：

```text
assets/shaders/lighting2d_composite.frag.hlsl
assets/shaders/lighting2d_composite.frag.glsl
```

输入：

- `WorldColor` sampled texture。
- `LightingTexture` sampled texture。
- uniform：ambient/exposure/debugMode。

输出：

```hlsl
float3 world = WorldColor.Sample(...).rgb;
float4 light = LightingTexture.Sample(...);
float3 lit = world * max(light.rgb, ambient.rgb) * exposure;
float3 reflection = light.a/reflection channel policy 或单独 ReflectionTexture;
return float4(saturate(lit + reflection), worldAlpha);
```

第一版如果 `LightingTexture` 仍是 RGBA8 overlay 语义，可以临时约定：

- `rgb` = light color multiplier / tint。
- `a` = light strength。
- composite 中转成 `lighting = ambient + rgb * a`。

验收：

- 删除 `Light2D` 时 World 只剩环境暗度。
- `patrolLight.intensity = 100.f` 必须明显改变 `WorldColor * Lighting` 结果。
- UI/Text 不被暗化。

#### 阶段 C：替换 overlay fallback

删除或降级当前路径：

- 不再把 `lighting2DTexture_` 当普通 sprite 画到 swapchain。
- 不再用 “黑色 full-screen sprite + radial sprite” 作为 SDL GPU 主路径。
- 保留 fallback 只在 `supportsStorageTexture == false` 或 composite pipeline 创建失败时启用，并在状态栏显示 fallback reason。

验收：

- `lighting2DSubmitCount > 0` 且 `lightingCompositeCount > 0`。
- RenderDoc/日志中能看到 `WorldColorPass -> LightVisibilityCompute -> LightingCompositePass -> UIPass`。
- demo3 A/B：按键关闭全部 Light2D 后，局部光照消失但 UI 保持可读。

#### 阶段 D：Debug views

增加 lighting debug mode：

```text
0 final composite
1 WorldColor only
2 LightingTexture only
3 Tile light count
4 Shadow/visibility
5 Reflection only
```

demo3 按键建议：

- `L` toggle Light2D visible。
- `G` cycle lighting debug mode。
- `O` toggle occluders。
- `R` toggle reflectors。

验收：

- 用户能直接区分 ECS 数据未提交、compute 输出为空、composite 没执行、UI 覆盖等问题。

#### 阶段 E：Render Graph 固化

当 SDL GPU offscreen composite 稳定后，把内部 hardcoded 顺序迁移为 Render Graph builder：

- 资源按 viewport size 自动 resize。
- pass 名称和 resource use 可导出到日志。
- 多 camera 时每个 World camera 建独立 graph scope，UI camera 在最后统一提交。
- 后续 Bloom/Tonemap/ColorGrading 只作为 graph pass 添加。

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

2026-05 阶段 2 已开始：

- SDL GPU 后端已加入 storage texture 能力探针：初始化时验证 RGBA8 纹理是否支持 `COMPUTE_STORAGE_WRITE | SAMPLER` 并实际创建 4x4 probe texture。
- `supportsStorageTexture` 现在来自真实后端探针。
- `assets/shaders/lighting2d.comp.glsl` 已加入；构建链当前使用同语义的 `lighting2d.comp.hlsl` 生成 SPIR-V/DXIL。
- SDL GPU 后端已接入单光源硬阴影 compute：Light2D/segment storage buffer -> 1/2 resolution storage texture -> sprite overlay composite。
- `supportsLighting2D` 在 compute/storage texture 能力通过时启用；demo3 状态行会显示 lighting2D 与 compute dispatch 计数。
- 2026-05 最新诊断：`Light2D` 数据链路已能到达后端，demo3 可显示 `lights=32 submit=1`。但当前主路径仍是 swapchain overlay/fallback，不是最终的 `WorldColor * Lighting` composite。下一步必须迁移到 Render Graph 管理的 offscreen WorldColor + LightingCompositePass。

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
- 临时 overlay composite 可用于 bring-up，但不能作为最终验收。
- SDL GPU 新增 WorldColor offscreen，并通过 fragment composite 输出到 swapchain。

验收：

- 一个点光被 AABB 遮挡后产生稳定阴影。
- 1080p 下 1/2 分辨率、1 light、64 segments 预算 < 1ms 作为初始目标。
- 关闭 lighting 时 `WorldColor -> Composite(copy)` 与原直接渲染一致。
- 打开 lighting 时局部光照来自 `Light2D`，删除/隐藏 `Light2D` 会明显改变大范围明暗。
- UI/Text 在 composite 之后绘制，不被 World lighting 暗化。

### L3：多光源和 tiled culling

交付：

- `tileLightList` buffer。
- 多光源混合、半径裁剪、layerMask。
- debug view：light tiles、shadow mask、lighting texture。
- `lighting2d_composite.frag.hlsl/glsl`。
- RenderFrameStats 记录 `lighting2DSubmitCount`、`lightingCompositeCount`、`worldColorPassCount`。

验收：

- 32 个小半径动态光源稳定运行。
- CPU 不再为每个像素/光源做任何工作。
- demo3 中 `lights > 0`、`submit > 0`、`composite > 0` 同时成立。
- debug mode 可切换 WorldColor only / LightingTexture only / Final composite。

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
src/backend/renderer/RenderGraph.h
src/backend/renderer/RenderGraph.cpp
assets/shaders/lighting2d.comp.glsl
assets/shaders/lighting2d_composite.frag.glsl
assets/shaders/lighting2d_composite.frag.hlsl
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
| swapchain overlay 被后续 camera/pass 覆盖 | World pass 不再直接写 swapchain；Render Graph 固定 WorldColor -> Composite -> UI |
| `Light2D` 已提交但画面无变化 | demo3 增加 submit/composite/debug view；增加 LightingTexture only 和 WorldColor only 诊断 |
| offscreen composite 与原渲染有色差 | 增加 lighting disabled copy-path 验收，先保证 WorldColor 直通一致 |

---

## 13. 当前实现进度

- `IRenderDevice::submitFrameGraph()` 已作为 frame-level camera graph 入口落地。
- `RenderSystem` 在 CPU batch 和 GPU-driven 两条路径中都先收集本帧 active camera，按 `Camera::depth` 构建 `FrameGraphCameraPass`，最后一次性提交 frame graph。
- frame graph 节点类型覆盖：
  - `Raster`：普通 CPU batch camera pass。
  - `GPUDriven`：GPU sprite pass + particle pass + CPU overlay/text/UI pass。
  - `WorldLighting`：WorldColor + LightingTexture + LightingComposite + 可选 UI pass。
- GPU-driven 路径的 per-camera visible index list 已放入 `GPURenderParams::ownedVisibleIndices`，由 `submitFrameGraph()` 在每个 camera 节点执行前上传，避免多个 camera 共用 visible index buffer 时互相覆盖。
- SDL GPU graph 路径已拆开 lighting compute 与旧 swapchain overlay：`submitWorldLightingGraph()` 正常路径只运行 `runLighting2DComputePass()` 生成 `LightingTexture`，swapchain 写入集中在 `LightingCompositePass`。
- OpenGL 仍是兼容实现：通过同一 frame graph 接口执行，但 `submitWorldLightingGraph()` 仍走现有 fallback 顺序。

尚未完成：

- RenderGraph 仍是 pass/resource 声明和统计层，尚未负责 barrier、资源生命周期、pass 重排。
- UI camera 现在由 frame-level camera graph 按 depth 表达，但单个 `WorldLighting` 节点内部的 `uiCommands` 仍只适合同一 camera 内的 UI/Screen 命令。
- 粒子 update/render 还未拆成 “每帧一次 update + 每 camera render”，多 camera 场景仍需后续收敛。

---

## 14. 决策记录

- **不做额外贴图**：QGame 的 2D 光照主线基于几何和参数，不要求资产作者维护 normal/roughness/emission map。
- **Vulkan 优先**：SPIR-V shader 和 Vulkan compute 是第一验证对象；其他 SDL GPU 后端是兼容目标，不是设计约束源头。
- **Compute 先于硬件 RT**：2D shadow/reflection 的核心是 2D 可见性查询，compute ray casting 更简单、更可控。
- **硬件 RT 后置**：只有在原生 Vulkan 后端成熟后，才评估 `VK_KHR_ray_query` 或 RT pipeline 是否带来足够收益。
- **World 与 UI 分离**：动态光照只作用于 WorldColorPass；Text/UI 默认保持可读性。
- **Overlay 只是过渡**：swapchain sprite overlay/fallback 只用于 bring-up 和能力不足时的降级；SDL GPU 主路径必须升级为 offscreen WorldColor + LightingCompositePass。
- **Render Graph 先轻量后泛化**：第一版 graph 只表达 2D lighting 所需 pass 和 transient texture，稳定后再承载 Bloom/Tonemap 等后处理。
