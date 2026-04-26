# GPU-Driven 2D 渲染器集成指南

## 概述

本文档说明如何将 GPU-Driven 2D 渲染架构集成到 QGame 引擎的现有系统中。

## 架构对比

### 现有架构 (CPU-Driven)

```
RenderSystem
    ↓
CommandBuffer (录制绘制命令)
    ↓
RenderPipeline (Pass 管理)
    ↓
IRenderDevice (提交命令)
    ↓
GPU (执行渲染)
```

### 新架构 (GPU-Driven)

```
RenderSystem
    ↓
GPUDrivenRenderer2D (管理实例数据)
    ↓
Compute Shaders (GPU 剔除、排序)
    ↓
Indirect Draw (GPU 渲染)
    ↓
GPU (执行渲染)
```

## 集成步骤

### Step 1: 修改 RenderSystem

```cpp
// src/engine/systems/RenderSystem.h

#include "backend/renderer/gpu_driven/GPUDrivenRenderer2D.h"

class RenderSystem : public System {
public:
    RenderSystem() {
        // 初始化 GPU-Driven 渲染器
        gpuRenderer_ = std::make_unique<engine::GPUDrivenRenderer2D>(
            renderDevice_,
            engine::GPUDrivenConfig{
                .maxInstances = 100000,
                .enableGPUCulling = true,
                .enableGPUSorting = true
            }
        );
    }
    
    void update(float dt) override {
        // 方式 1: 使用传统 CPU-Driven 路径
        // renderCPUDriven();
        
        // 方式 2: 使用 GPU-Driven 路径
        renderGPUDriven();
    }
    
private:
    std::unique_ptr<engine::GPUDrivenRenderer2D> gpuRenderer_;
    
    void renderGPUDriven();
};
```

### Step 2: 实现实例注册

```cpp
// src/engine/systems/RenderSystem.cpp

void RenderSystem::renderGPUDriven() {
    // ========== Step 1: 注册精灵实例 ==========
    
    // 清除上一帧的实例
    gpuRenderer_->clear();
    
    // 遍历所有精灵实体
    auto view = world_.view<SpriteComponent, TransformComponent>();
    
    for (auto entity : view) {
        auto& sprite = view.get<SpriteComponent>(entity);
        auto& transform = view.get<TransformComponent>(entity);
        
        // 添加精灵实例
        uint32_t instanceId = gpuRenderer_->addSprite({
            .texture = sprite.texture,
            .x = transform.x,
            .y = transform.y,
            .rotation = transform.rotation,
            .scaleX = transform.scaleX,
            .scaleY = transform.scaleY,
            .layer = sprite.layer,
            .ySort = sprite.ySort,
            .tint = sprite.tint
        });
        
        // 存储 instanceId，用于后续更新
        entityToInstanceId_[entity] = instanceId;
    }
    
    // ========== Step 2: 执行渲染 ==========
    
    gpuRenderer_->render(camera_);
}
```

### Step 3: 实现实例更新 (可选优化)

```cpp
// 如果精灵位置频繁变化，使用增量更新

void RenderSystem::onTransformChanged(Entity entity) {
    auto it = entityToInstanceId_.find(entity);
    if (it != entityToInstanceId_.end()) {
        auto& transform = world_.get<TransformComponent>(entity);
        gpuRenderer_->updatePosition(it->second, transform.x, transform.y);
    }
}
```

### Step 4: 配置层级

```cpp
// 设置层级可见性
gpuRenderer_->setLayerVisible(0, true);   // 背景层
gpuRenderer_->setLayerVisible(10, true);  // 游戏对象层
gpuRenderer_->setLayerVisible(20, false); // UI 层 (暂时隐藏)

// 设置层级 Y 排序
gpuRenderer_->setLayerYSort(10, true);    // 游戏对象层启用 Y 排序
```

## 兼容性处理

### 渐进式迁移

```cpp
class RenderSystem {
    bool useGPUDriven_ = true;  // 运行时切换
    
    void update(float dt) override {
        if (useGPUDriven_) {
            renderGPUDriven();
        } else {
            renderCPUDriven();  // 传统路径作为后备
        }
    }
    
    // 运行时切换
    void setRenderMode(bool gpuDriven) {
        useGPUDriven_ = gpuDriven;
    }
};
```

### 混合渲染

```cpp
// 部分 Pass 使用 GPU-Driven，部分使用传统方式

void RenderSystem::update(float dt) {
    // 背景：使用传统方式 (无需排序)
    renderBackgroundTraditional();
    
    // 游戏对象：使用 GPU-Driven (大量精灵)
    gpuRenderer_->render(camera_);
    
    // UI：使用传统方式 (少量元素)
    renderUITraditional();
}
```

## 性能监控

```cpp
// 获取渲染统计信息

void RenderSystem::printStats() {
    const auto& stats = gpuRenderer_->getStats();
    
    core::logInfo("GPU-Driven Rendering Stats:");
    core::logInfo("  Total Instances: %u", stats.totalInstances);
    core::logInfo("  Visible Instances: %u", stats.visibleInstances);
    core::logInfo("  Draw Calls: %u", stats.drawCalls);
    core::logInfo("  CPU Time: %.2f ms", stats.cpuTimeMs);
    core::logInfo("  GPU Time: %.2f ms", stats.gpuTimeMs);
}
```

## 调试技巧

### 1. 验证 GPU 剔除结果

```glsl
// 在 culling.comp.glsl 中添加调试输出

// 将被剔除的实例标记为红色
if (!isInViewport(inst)) {
    debugBuffer[idx] = vec4(1.0, 0.0, 0.0, 1.0);  // 红色
    return;
}
```

### 2. 可视化排序结果

```glsl
// 在 sprite.frag.glsl 中添加调试输出

// 根据 sortKey 显示颜色
fragColor = vec4(
    float(sortKey % 256) / 255.0,
    float((sortKey / 256) % 256) / 255.0,
    float((sortKey / 65536) % 256) / 255.0,
    1.0
);
```

### 3. 性能分析

```cpp
// 使用 Timer Query 测量 GPU 时间

gpuRenderer_->setDebugMode(true);
// ... 渲染 ...
const auto& stats = gpuRenderer_->getStats();
```

## 已知限制

### 1. 纹理数量限制

- **问题**: Texture2DArray 的层数有限 (通常 256-2048)
- **解决**: 使用纹理图集 或虚拟纹理

### 2. 实例数量限制

- **问题**: GPU Buffer 大小有限
- **解决**: 增大 `maxInstances` 或分帧渲染

### 3. Compute Shader 兼容性

- **问题**: 某些旧 GPU 不支持 Compute Shader
- **解决**: 使用传统 CPU-Driven 路径作为后备

## 下一步

1. **Phase 2**: 实现 Reactive Render Graph
2. **Phase 3**: 实现 Streaming Tile World
3. **优化**: 添加 GPU Timer Query
4. **扩展**: 支持粒子系统、骨骼动画

## 参考文档

- [PLAN_GPU_Driven_Rendering.md](./PLAN_GPU_Driven_Rendering.md) - 演进路径总览
- [MSDF 字体渲染系统](../AGENTS.md) - 字体渲染实现
