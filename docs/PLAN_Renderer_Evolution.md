# 渲染器演化路线（2D → GPU-Driven → 2D 光追）

> 目标：从当前的"CPU 录制 + Pass 分桶"演进到"GPU-Driven + Compute-First"，为 2D 光追与 GPU 计算（物理/粒子/AI）打底。
> 原则：**数据导向、CPU/GPU 对称、Pass 即节点、保留软件 fallback**。

---

## 短期（1–2 个月）：把底座立起来

### S1. 去 Pass 模型，转 Camera-driven
- `Camera` 组件扩展：`depth / viewportNorm / renderTarget / layerMask / clear / clearColor`。
- `RenderSystem` 主循环改为「按 depth 排相机 → 每相机独立剔除/录制/submit」。
- `RenderPass` 枚举降级为 `DrawLayer`（仅参与排序，不再驱动渲染阶段）。
- `RenderPipeline.execute` 删除分桶逻辑，收敛为「camera renderer」。
- 视锥剔除改为「每相机一次 `computeViewRect`」。

**交付物**：`Camera`/`RenderSystem` 改造完成；现有 World+UI 用「两台相机」表达；新增一台 PIP/小地图 demo 验证。

### S2. Compute Pipeline 抽象
- `IRenderDevice` 增加：
  - `ComputePipelineHandle createComputePipeline(shader, layout)`
  - `void dispatchCompute(handle, groupX/Y/Z, bindings)`
  - `BufferHandle createBuffer(size, usage = Vertex|Index|Storage|Indirect)`
- `CommandBuffer` 增加 `cb.dispatch(...)` / `cb.barrier(...)` 命令变体。
- SDL_GPU / GL 后端各实现一份；先确保最小 demo（compute shader 写一个 RW buffer）跑通。

**交付物**：能在 compute shader 里读写 storage buffer；提供 hello-compute 测试。

### S3. Persistent GPU Buffer（Sprite/Transform）✅
- `RenderSystem` 不再每帧重建 `Drawable`。改为：
  - sprite 数据持久化在一个 GPU storage buffer（每 entity 一个 slot）；
  - ECS 通过 `entt::observer` 监听 `Transform/Sprite` 变更，仅 upload dirty slot；
  - 销毁的 entity slot 加入 free list，复用。
- 文字 (MSDF) 暂保持 CPU 录制，后续随 GPU-driven 一起改造。

**交付物**：sprite 数据上 GPU 后稳定显示；CPU profile 显示 RenderSystem 工作量显著下降。

**实现状态** (2025-04):
- ✅ `GPUSprite` 结构体：transform/color/uv/textureIndex/layer/sortKey/flags
- ✅ `SpriteBuffer` 管理器：triple buffering + free list + generation
- ✅ `GPUHandle` 字段添加到 Sprite 组件
- ✅ Dirty tracking：`on_update<Transform>` + `on_destroy<Sprite>` 信号
- ✅ Batch update + coalesce 优化
- ⚠️ 渲染路径：当前仍用 CPU 批处理，Storage Buffer 已准备就绪供 M1/M2 使用

**新建文件**：
- `src/engine/resources/GPUSprite.h`
- `src/engine/resources/SpriteBuffer.h`

---

## 中期（3–6 个月）：让 GPU 自己干活

### M1. GPU Frustum Culling ✅
- 当前 CPU 视锥剔除（`computeWorldViewRect`）改写到 compute shader。
- 输入：persistent sprite buffer + 每相机 view rect。
- 输出：visibleIndices buffer（per camera）。
- 多相机各自 dispatch 一次。

**交付物**：CPU 端剔除代码删除；GPU profile 验证 compute pass <0.2ms / 万 sprite。

**实现状态** (2025-04):
- ✅ `sprite_culling.comp` - GPU 视锥剔除 compute shader (GLSL + HLSL)
- ✅ `GPUDrivenRenderer` - 管理 compute pipeline 和 buffer
- ✅ `RenderSystem::buildCommandBufferGPUDriven()` - GPU-driven 渲染路径
- ✅ 支持 layerMask 过滤和 cullEnabled 开关
- ✅ OpenGL 后端使用内嵌 GLSL 源码，无需 SPIRV
- ✅ 测试：game/main.cpp 添加 400 精灵测试场景，G 键切换 GPU/CPU 模式
- ✅ `--opengl` 命令行参数强制使用 OpenGL 后端

**新建文件**：
- `assets/shaders/sprite_culling.comp`
- `assets/shaders/sprite_culling.hlsl`
- `src/engine/resources/GPUDrivenRenderer.h`
- `src/engine/resources/GPUDrivenRenderer.cpp`

### M2. GPU-Driven Indirect Draw ✅
- 排序也搬到 GPU：bitonic / radix sort compute shader 输出已排好的 indirect draw arg buffer。
- 主渲染走 `drawIndirect` / `drawIndirectCount`，CPU 只下发「开始渲染」一次调用。
- Drawable 字段下移：layer / ySort / sortKey 编码进 `uint64 sortKey`，GPU 端比较即可。

**交付物**：单次 indirect draw 渲染整帧 sprite；CPU 端 `cb.drawSprite` 调用消失。

**实现状态** (2025-04):
- ✅ `sprite_sort.comp` - Bitonic sort compute shader (GLSL + HLSL)
- ✅ `IRenderDevice::submitGPUDrivenPass()` - GPU-driven 提交 API
- ✅ GL/SDL_GPU 后端 stub 实现（fallback 到 CPU 路径）
- ✅ `RenderSystem::setGPUDrivenEnabled()` - 运行时开关
- ⚠️ 间接绘制需要 bindless texture 支持（当前 fallback 到 CPU 批处理）

**新建文件**：
- `assets/shaders/sprite_sort.comp`
- `assets/shaders/sprite_sort.hlsl`
- `assets/shaders/sprite_gpu.vert.glsl`
- `assets/shaders/sprite_gpu.frag.glsl`

### M3. 2D SDF 烘焙管线
- 工具链：把 sprite/tilemap 离线烘焙成 SDF 纹理（`stb_truetype`-like 或 `msdfgen`）。
- 运行时：合成「全场景 SDF」（jump flooding 在 compute pass 里做，每帧重建受变更影响的区域）。
- SDF 同时服务于：
  - 光照/阴影（下一阶段 2D 光追的几何源）；
  - 碰撞/AI 距离查询（PhysicsSystem 替代方案）。

**交付物**：场景级 SDF 纹理；可视化 debug view；接入 PhysicsSystem 实验性距离查询。

---

## 长期（6+ 个月）：光追与 Compute-First 主渲染

### L1. Compute-First 主渲染
将渲染阶段从「光栅化为主」翻转为「compute 为主，光栅只做 composite」：

```
[CPU]  ECS 增量 → persistent buffer
   ↓
[Compute] cull + sort         → visibleList
   ↓
[Compute] light/shadow (SDF)  → lightingRT
   ↓
[Compute] particles/physics   → 同一份 buffer
   ↓
[Graphics] composite (fullscreen) → swapchain
```

- 引入 RenderGraph 抽象（节点 = pass，边 = buffer/RT 依赖，引擎自动拓扑排序 + barrier）。
- 现有 sprite 渲染下沉为 RenderGraph 中的一个 graphics 节点，与 compute 节点同级调度。

**交付物**：RenderGraph 框架；至少 3 个示例节点（cull / light / composite）跑通完整一帧。

### L2. 2D 光追 Pass
基于 L1 的 SDF 与 RenderGraph 实现 2D 光线追踪：
- **基础版**：SDF ray marching，在 compute shader 里发射射线、采样光源、累积辐射；
- **进阶版**：实现 Radiance Cascades（2024 Pavel Panchekha），分级 probe + 二次反弹；
- **硬件加速 fallback**：检测 RTX/RDNA 光追支持，对显式几何使用 `VK_KHR_ray_tracing` / `DXR`，否则走 SDF 软件实现。
- 光照结果作为输入贴回主 composite 节点。

**交付物**：动态光源在 sprite 间产生正确遮挡 + 软阴影；性能预算 <3ms / 1080p / RTX 3060 级。

### L3. Bindless + Mesh Shader（视后端能力）
- 等 SDL_GPU 支持 descriptor indexing / mesh shader 后启用：
  - **Bindless**：所有纹理进一张大 array，材质 ID 进 buffer，消除 batch by texture；
  - **Mesh Shader 2D**：在 task/mesh shader 内直接展开 sprite/tilemap/text glyph，跳过 vertex 装配。
- 对 OpenGL 后端保留 fallback 路径（已有的 batch sprite renderer）。

**交付物**：sprite 渲染不再受纹理切换分批限制；可处理 100k+ 动态 sprite。

---

## 横切关注点（每阶段都要跟进）

| 项目 | 说明 |
|---|---|
| **跨后端一致性** | SDL_GPU 与 GLRenderDevice 同步演进，差异隔离在 device 实现层。 |
| **编辑器同步** | 渲染架构每次改造，EditorAPI / EDITOR_VIEWPORT 路径同步迁移。 |
| **profiling 基础设施** | 引入 GPU timestamp query，每个 pass 有耗时 telemetry，避免改造引入隐性回归。 |
| **回归测试** | 每个里程碑增加截图对比测试（参考帧 PNG diff），保证视觉一致。 |
| **资产/序列化** | `SceneSerializer` 跟随 `Camera`/`Drawable` 字段演化更新。 |
| **文档** | 每个 S/M/L 子项在 `docs/` 留一篇设计 + 决策记录。 |

---

## 与现有代码的衔接点

- 当前已完成：CPU 视锥剔除、相机视图矩阵标准化（`buildViewMatrix` 改为 `R · zoom · (world − cam)`，`mvp = proj · view`）。
- **S1 起点**：`RenderSystem.cpp:syncCamerasToPassStates` + `RenderPipeline.execute`。
- **S2 起点**：`IRenderDevice.h` / `CommandBuffer.h`（增加 compute / buffer / dispatch 命令变体）。
- **S3 起点**：`RenderSystem.cpp:buildSceneCommands` 内部循环改造为 dirty-tracked observer。
- **M1 起点**：`computeWorldViewRect` 替换为 GPU compute kernel。
- **L1 起点**：将 `RenderPipeline` 演化为 `RenderGraph`（节点/资源/边）。

---

## 风险与回退策略

| 风险 | 缓解 |
|---|---|
| SDL_GPU 在某后端 (D3D12/Vulkan/Metal) compute 行为差异 | 先在 Vulkan 验证，其他后端 fallback 走 graphics shader 模拟 |
| GPU-driven 调试困难 | 保留 CPU 路径作为 reference 实现，可运行时切换 |
| 光追硬件不普及 | 始终以 SDF 软件实现为主路径，硬件 RT 仅作为加速选项 |
| RenderGraph 设计过度 | 先用最薄的「显式 Pass + 资源依赖声明」版本，不引入完整 DAG 框架 |
