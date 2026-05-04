# GPU 渲染性能强化计划

> 目标：把 QGame 的主渲染路径推进到 SDL GPU / GPU-driven / compute-first。OpenGL CPU-batch 与 SDL GPU CPU-batch 只作为正确性 fallback 和测试对照，不作为性能优化目标。

---

## 路径优先级

1. **SDL GPU GPU-driven**：主性能路径。
2. **SDL GPU CPU-batch**：SDL GPU 的正确性 fallback。
3. **OpenGL CPU-batch**：跨后端测试、调试和最低兜底。

CPU-batch 路径只要求：
- 能稳定显示同一场景；
- 能用于截图 diff 和行为对照；
- 能在 GPU-driven 失败时回退。

不在 CPU-batch 上投入 tile/text/sort/batch 性能优化。

---

## 当前关键问题

当前 GPU-driven 还不是完整 GPU-driven：

- `RenderSystem::buildCommandBufferGPUDriven()` 仍在 CPU 侧遍历 sprite、做可见性筛选、排序并生成 `visibleIndices`。
- `uploadToBuffer()` 每次上传都会独立 acquire/submit command buffer，容易产生提交碎片和同步压力。
- 已有 compute culling/shader 与 `GPUDrivenRenderer` 基础，但当前主路径没有真正把 visible list 生成交给 GPU。
- visible count 如果通过 GPU readback 参与常规提交，会形成 CPU/GPU 同步点，应避免作为主路径。
- 硬件光追不适合塞进 SDL GPU 后端；短中期应先做 compute lighting / 2D ray marching，长期再考虑原生 Vulkan/D3D12 光追后端。

---

## 阶段 0：能力与观测

### 0.1 RendererCapabilities

在 `IRenderDevice` 层增加后端能力描述，用于选择路径和记录 fallback 原因：

- `supportsCompute`
- `supportsStorageBuffer`
- `supportsStorageTexture`
- `supportsGPUDrivenSprite`
- `supportsIndirectDraw`
- `supportsTextureArray`
- `supportsTimestampQuery`

SDL GPU 根据实际 pipeline/buffer 创建结果填写。OpenGL 保守声明 CPU-batch fallback 能力，compute/GPU-driven 只有确认稳定后再开启。

### 0.2 GPU 性能计数器

每帧记录：

- sprite 总数；
- visible sprite 数；
- GPU draw batch 数；
- draw call 数；
- compute dispatch 次数；
- buffer upload bytes；
- texture bind 次数；
- 当前渲染路径；
- fallback 原因。

优先提供 CPU 侧 counters。GPU timestamp query 作为后续能力项接入。

---

## 阶段 1：上传系统改造

### 1.1 Frame Upload Queue

当前 `uploadToBuffer()` 每次上传都单独提交 copy command。改为：

- 本帧收集所有 buffer/texture upload；
- 在当前 frame command buffer 里统一录制 copy pass；
- 支持按 offset 合并相邻 upload；
- sprite dirty 数据批量上传。

### 1.2 Staging / Ring Upload Buffer

为频繁小上传提供 ring staging：

- 每帧分配一段 staging 空间；
- 批量 memcpy；
- copy pass 统一提交；
- 避免为每个 BufferEntry 长期持有同尺寸 transfer buffer。

### 1.3 资源销毁延迟

把帧中 `WaitForGPUIdle` 式释放改为 deferred destruction：

- buffer/texture/pipeline 延迟 N 帧释放；
- shutdown 时统一等待 idle；
- fallback 路径可继续简单实现，但 SDL GPU 主路径避免帧中阻塞。

---

## 阶段 2：真正 GPU Culling

### 2.1 Compute 生成 Visible Index Buffer

输入：

- persistent sprite buffer；
- camera/view rect；
- layer mask；
- sprite count。

输出：

- visible index buffer；
- visible counter / draw args buffer。

CPU 不再逐 sprite 做可见性筛选。

### 2.2 避免每帧 Readback

不要把 visible count 从 GPU 读回 CPU 再决定绘制数量。可选过渡方案：

- 固定上限 instance draw，shader 丢弃无效 instance；
- 使用 indirect draw args buffer；
- 按固定 bucket 范围提交多个 draw。

主线目标是 draw args / indirect draw。

---

## 阶段 3：GPU Draw Args 与 Indirect

### 3.1 Draw Args Buffer

compute pass 生成 draw 参数：

- index count；
- instance count；
- first index；
- vertex offset；
- first instance。

### 3.2 Indirect Draw

优先实现 SDL GPU 后端的 indirect draw 路径。若某后端不支持或行为不稳定：

- 自动 fallback 到 SDL GPU CPU-batch；
- OpenGL CPU-batch 继续作为参考路径；
- 日志记录 fallback 原因。

---

## 阶段 4：GPU Sorting / Bucketing

完整 GPU sort 复杂度较高，按阶段推进：

1. **GPU culling + CPU 轻量 batch 表**：过渡方案，只保留最小 CPU 参与。
2. **GPU bucket by pass/layer/texture**：先减少 texture/pipeline 切换。
3. **GPU sort visible indices**：按 layer、y-sort、sortKey 生成稳定顺序。
4. **GPU 生成 batch/draw args**：CPU 只提交渲染入口。

排序 key 建议压成固定宽度整数，避免 shader 端复杂分支。

---

## 阶段 5：纹理绑定优化

当前 GPU-driven 仍按 texture 切 batch。优化方向：

- atlas packing：短期最稳定，减少 texture 切换；
- texture array：适合同规格 sprite atlas；
- descriptor indexing / bindless-like：仅在 SDL GPU 能力允许时开启；
- material/texture id 进入 sprite buffer，shader 端索引采样资源。

此阶段应由 capabilities 控制，不能破坏 fallback。

---

## 阶段 6：GPU Tilemap

不优化 CPU tile 路径，直接规划 GPU 化：

- tile layer 数据进入 storage buffer 或 texture buffer；
- compute 根据 camera 生成 visible tile instances；
- vertex shader 根据 tile id 计算 UV；
- 可选：按 chunk 维护 dirty tile 数据，但绘制仍走 GPU-driven。

目标是让 tile 与 sprite 共用 culling/batching/draw args 思路。

---

## 阶段 7：Compute Lighting / 2D Ray Shadow

在硬件光追之前，先做 SDL GPU 能承载的 compute lighting：

- light buffer；
- occluder buffer / tile collision mask / SDF texture；
- compute pass 生成 lighting texture；
- sprite composite pass 采样 lighting texture；
- 扩展 2D ray marching shadow 和软阴影。

这条路径与 SDL GPU 的 compute 能力匹配，也能为未来原生硬件光追后端积累资源图和光照数据结构。

---

## 长期：原生硬件光追后端

硬件 RT 不作为 `SDLGPURenderDevice` 的直接扩展目标。长期如果需要 RTX/DXR/Vulkan RT：

- 新增 `VulkanRenderDevice` 或 `D3D12RenderDevice`；
- 新增 acceleration structure 抽象；
- 新增 ray tracing pipeline / shader binding table；
- SDL GPU 后端保持 raster + compute 主通用后端；
- OpenGL CPU-batch 继续作为调试 fallback。

---

## 开发顺序

1. `RendererCapabilities` 与 GPU counters。
2. Frame upload queue 与 staging/ring upload buffer。
3. Deferred destruction。
4. Compute culling 接入当前 GPU-driven 路径。
5. 去掉 visible count readback 依赖，改 draw args / indirect。
6. GPU bucketing / sorting。
7. 纹理绑定优化。
8. GPU tilemap。
9. Compute lighting / 2D ray shadow。
10. 评估原生 Vulkan/D3D12 光追后端。

