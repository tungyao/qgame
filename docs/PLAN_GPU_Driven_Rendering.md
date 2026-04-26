# GPU-Driven 2D Rendering 演进路径

## 概述

本文档描述 QGame 引擎从传统 CPU 驱动渲染到现代 GPU 驱动渲染的演进路线。

## 当前架构问题

### 传统 2D 渲染管线的瓶颈

```
┌─────────────────────────────────────────────────────────────┐
│                    传统 CPU 驱动渲染                         │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  每帧流程:                                                   │
│  1. 遍历所有精灵 → O(n)                                      │
│  2. 视口裁剪 (CPU 端)                                        │
│  3. 按 Y/Z 排序 → O(n log n)                                │
│  4. 按纹理分组批处理                                         │
│  5. 提交 Draw Call (每个批次一次)                            │
│                                                              │
│  问题:                                                       │
│  - CPU 时间随精灵数量线性增长                                │
│  - 排序操作 CPU 密集                                         │
│  - 大量 Draw Call (纹理切换导致批次中断)                     │
│  - 无法充分利用 GPU 并行能力                                 │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### 性能测试数据

| 精灵数量 | CPU 时间 | Draw Calls | GPU 时间 |
|---------|---------|-----------|---------|
| 1,000   | 0.5ms   | 10-20     | 0.1ms   |
| 10,000  | 5ms     | 100-200   | 0.5ms   |
| 50,000  | 25ms    | 500+      | 2ms     |

## 演进阶段

### Phase 1: GPU-Driven 2D Rendering (核心架构)

**目标**: 将 CPU 工作转移到 GPU

```
┌──────────────────────────────────────────────────────────────┐
│              GPU-Driven 2D Rendering Architecture            │
├──────────────────────────────────────────────────────────────┤
│                                                               │
│   CPU (每帧):                    GPU (每帧):                  │
│   ──────────                     ──────────                  │
│   1. 更新变换矩阵 Buffer         1. Culling Compute Shader    │
│      (增量更新)                     - 视口裁剪               │
│                                    - 层级过滤                 │
│   2. 提交 DrawIndirect           2. Sorting Compute Shader   │
│      (单次调用)                     - Y 排序                 │
│                                    - Z 排序                  │
│                                 3. Batching Compute Shader   │
│                                    - 合并相同纹理             │
│                                 4. Indirect Draw             │
│                                    - 单次提交                 │
│                                                               │
│   CPU Time: <0.1ms              GPU Time: 0.5ms              │
│   Draw Calls: 1 (indirect)                                    │
│                                                               │
└──────────────────────────────────────────────────────────────┘
```

**关键文件**:
- `src/backend/renderer/gpu_driven/GPUDrivenRenderer2D.h` - 主渲染器
- `src/backend/renderer/gpu_driven/SpatialHashGrid.h` - 空间分区
- `src/backend/renderer/gpu_driven/InstanceBuffer.h` - 实例数据管理
- `assets/shaders/compute/culling.comp.glsl` - GPU 剔除
- `assets/shaders/compute/sorting.comp.glsl` - GPU 排序

**预期收益**:
- CPU 时间降低 10x
- Draw Calls 降低 100x
- 支持更大规模的精灵数量

---

### Phase 2: Reactive Render Graph (响应式渲染图)

**目标**: 状态变化驱动渲染，消除冗余绘制

```
┌──────────────────────────────────────────────────────────────┐
│              Reactive Render Graph                            │
├──────────────────────────────────────────────────────────────┤
│                                                               │
│   传统方式:                                                   │
│   ┌─────────┐                                                 │
│   │ 每帧重绘 │ → 即使没有变化也重绘                           │
│   └─────────┘                                                 │
│                                                               │
│   响应式:                                                     │
│   ┌─────────────┐     ┌─────────────┐     ┌─────────────┐   │
│   │ 状态变化    │ --> │ 脏标记传播  │ --> │ 仅重绘脏节点 │   │
│   └─────────────┘     └─────────────┘     └─────────────┘   │
│                                                               │
│   示例:                                                       │
│   playerPos->set(vec2(100, 100));                            │
│   // 自动触发: Player节点, HealthBar节点, CameraFollow节点   │
│   // 不触发: Background节点, 其他玩家节点                     │
│                                                               │
└──────────────────────────────────────────────────────────────┘
```

**关键文件**:
- `src/backend/renderer/reactive/Signal.h` - 响应式信号
- `src/backend/renderer/reactive/ReactiveNode.h` - 响应式节点
- `src/backend/renderer/reactive/ReactiveGraph.h` - 图管理

**预期收益**:
- 静态场景零开销
- UI/动画系统性能提升
- 更清晰的数据流

---

### Phase 3: Streaming Tile World (流式瓦片世界)

**目标**: 支持无限大小地图，按需加载

```
┌────────────────────────────────────────────────────────────┐
│                 Streaming Tile World                         │
├────────────────────────────────────────────────────────────┤
│                                                             │
│    ┌──────────────────────────────────────────────┐        │
│    │              玩家视野 (Viewport)              │        │
│    │    ┌────────┐  ┌────────┐  ┌────────┐       │        │
│    │    │Chunk   │  │Chunk   │  │Chunk   │       │        │
│    │    │ -1,-1  │  │ 0,-1   │  │ 1,-1   │       │        │
│    │    └────────┘  └────────┘  └────────┘       │        │
│    │    ┌────────┐  ┌────────┐  ┌────────┐       │        │
│    │    │Chunk   │  │Player  │  │Chunk   │       │        │
│    │    │ -1,0   │  │ 0,0    │  │ 1,0    │       │        │
│    │    └────────┘  └────────┘  └────────┘       │        │
│    └──────────────────────────────────────────────┘        │
│                                                             │
│    流式加载策略:                                             │
│    1. 检测玩家位置 → 计算需要的 Chunk                        │
│    2. 异步加载/生成新 Chunk                                 │
│    3. 卸载远离的 Chunk                                      │
│    4. GPU Ring Buffer 复用内存                              │
│                                                             │
└────────────────────────────────────────────────────────────┘
```

**关键文件**:
- `src/engine/world/StreamingWorld.h` - 流式世界管理
- `src/engine/world/ChunkManager.h` - Chunk 生命周期
- `src/engine/world/ChunkLoader.h` - 异步加载器

**预期收益**:
- 支持无限地图大小
- 内存占用恒定
- 流畅的无缝世界体验

---

## 实现优先级

```
Phase 1 (GPU-Driven)     ████████████████████  最高优先级
Phase 2 (Reactive)       ████████████          中等优先级
Phase 3 (Streaming)      ████████              低优先级
```

## 兼容性策略

### 渐进式迁移

```cpp
// 兼容层: 支持传统 API 和 GPU-Driven API
class Renderer2D {
public:
    // 传统 API (向后兼容)
    void drawSprite(const SpriteComponent& sprite);
    void drawText(const TextComponent& text);
    
    // GPU-Driven API (新架构)
    uint32_t registerSprite(Entity e);
    void updateTransform(uint32_t instanceId, const mat3& transform);
    void renderGPUDriven(const Camera2D& camera);
};
```

### 分支策略

```
main
  └── feature/gpu-driven-rendering
        └── 测试验证通过后合并到 main
```

## 性能基准测试

### 测试场景

1. **小型场景**: 1000 精灵，10 层
2. **中型场景**: 10000 精灵，20 层
3. **大型场景**: 50000 精灵，30 层
4. **极端场景**: 100000 精灵，50 层

### 测试指标

- CPU Frame Time
- GPU Frame Time
- Draw Call Count
- Memory Usage
- Frame Rate Stability

## 下一步

1. 实现 `GPUDrivenRenderer2D` 核心类
2. 编写 Compute Shaders (culling, sorting)
3. 集成到现有 `RenderSystem`
4. 性能测试和优化
5. 编写迁移文档

## 参考资料

- [GPU-Driven Rendering Pipelines](https://advances.realtimerendering.com/s2015/)
- [AMD GPUOpen - GPU Driven Rendering](https://gpuopen.com/)
- [Our Machinery - Render Graph](https://ourmachinery.com/)
