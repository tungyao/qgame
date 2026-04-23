# Editor Viewport 离屏渲染实现

## 概述

为编辑器实现了 Viewport 离屏场景预览功能，允许在编辑器窗口中独立渲染场景，不影响主游戏渲染流程。

## 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                      EditorApplication                       │
│  ┌─────────────────────────────────────────────────────┐   │
│  │              EditorViewportRenderer                  │   │
│  │  - 独立 CommandBuffer                                │   │
│  │  - 构建渲染命令                                      │   │
│  │  - 调用 renderToTextureOffscreen()                   │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                     IRenderDevice (接口)                     │
│  - renderToTexture()        : 与 swapchain 共享 cmdBuf      │
│  - renderToTextureOffscreen(): 独立 cmdBuf，不依赖 swapchain │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   SDLGPURenderDevice                         │
│  - pipeline_           : swapchain 格式 pipeline            │
│  - offscreenPipeline_  : R8G8B8A8_UNORM 格式 pipeline       │
│  - editorRenderTarget_      : 与 swapchain 共享的渲染目标   │
│  - offscreenRenderTarget_   : 独立离屏渲染目标              │
└─────────────────────────────────────────────────────────────┘
```

## 修改的文件

### Backend 层 (src/backend/)

1. **IRenderDevice.h**
   - 新增 `renderToTextureOffscreen()` 虚函数接口

2. **SDLGPURenderDevice.h**
   - 新增 `offscreenPipeline_` 成员
   - 新增 `offscreenRenderTarget_` 及尺寸成员
   - 新增 `createPipelineForFormat()` 方法
   - 修改 `renderCommandBufferToTarget()` 签名，接受 cmdBuf 和 pipeline 参数

3. **SDLGPURenderDevice.cpp**
   - `createPipeline()`: 同时创建 swapchain 和 offscreen 两个 pipeline
   - `createPipelineForFormat()`: 根据 texture format 创建 pipeline
   - `renderToTextureOffscreen()`: 独立创建 command buffer，使用 offscreen pipeline 渲染
   - `renderCommandBufferToTarget()`: 重构为接受外部 cmdBuf 和 pipeline
   - `shutdown()`: 清理 offscreenPipeline_ 和 offscreenRenderTarget_

### Engine 层 (src/engine/)

4. **EngineContext.h**
   - 新增 `renderToSwapchain` 标志，控制是否自动渲染到 swapchain

5. **RenderSystem.cpp**
   - 在 `update()` 中检查 `renderToSwapchain` 标志

### Editor 层 (editor/)

6. **EditorViewportRenderer.h** (新建)
   - 兼容层，独立构建渲染命令
   - 封装离屏渲染逻辑

7. **EditorViewportRenderer.cpp** (新建)
   - `render()`: 调用 `renderToTextureOffscreen()`
   - `getTexture()`: 获取 SDL_GPUTexture 指针供 ImGui 使用
   - `buildCommandBuffer()`: 独立构建渲染命令（复制自 RenderSystem）

8. **EditorApplication.h**
   - 新增 `viewportRenderer_` 成员

9. **EditorApplication.cpp**
   - 设置 `ctx_.renderToSwapchain = false`
   - `drawViewport()` 使用 `viewportRenderer_` 渲染

10. **CMakeLists.txt**
    - 添加 `EditorViewportRenderer.cpp` 到构建

### Core 层 (src/core/)

11. **Logger.h**
    - Windows 平台自动分配控制台 (`AllocConsole`)

## 关键问题解决

### 问题 1: gpuCmdBuf_ 为 NULL

**原因**: `renderToTexture()` 依赖 `gpuCmdBuf_`，它与 swapchain 绑定。当窗口最小化/不可见时，swapchain 不可用，`gpuCmdBuf_` 被置为 NULL。

**解决**: 新增 `renderToTextureOffscreen()` 独立创建 command buffer，不依赖 swapchain 状态。

### 问题 2: Pipeline 格式不匹配

**原因**: Swapchain 使用 B8G8R8A8 格式，离屏渲染目标使用 R8G8B8A8_UNORM 格式。Pipeline 必须匹配 render target 格式。

**解决**: 创建两个 pipeline：
- `pipeline_`: swapchain 格式
- `offscreenPipeline_`: R8G8B8A8_UNORM 格式

### 问题 3: 重复渲染

**原因**: `RenderSystem` 自动渲染到 swapchain，同时 `EditorViewportRenderer` 渲染到离屏纹理。

**解决**: 新增 `EngineContext::renderToSwapchain` 标志，Editor 模式下禁用自动渲染。

### 问题 4: Windows 无控制台日志

**原因**: Windows GUI 程序没有控制台窗口。

**解决**: 在 Logger.h 中添加 `AllocConsole()` 自动分配控制台。

## 待清理的调试日志

以下文件包含调试日志，生产版本应移除或降级：

1. **SDLGPURenderDevice.cpp**
   - `renderToTextureOffscreen()` 中的 logInfo/logError
   - `renderCommandBufferToTarget()` 中的 logInfo

2. **EditorViewportRenderer.cpp**
   - `render()` 中的 logInfo
   - `getTexture()` 中的 logInfo/logError

3. **EditorApplication.cpp**
   - `drawViewport()` 中的 logInfo

## 测试状态

- [x] 编译通过 (Linux + Windows)
- [x] 离屏纹理创建成功
- [x] 渲染命令正确执行
- [x] Viewport 正确显示场景
- [x] 禁用重复渲染
- [ ] 性能测试
- [ ] 多视口支持（未来）

## 使用方式

```cpp
// Editor 模式下禁用 swapchain 渲染
ctx_.renderToSwapchain = false;

// 使用 ViewportRenderer 渲染
TextureHandle texture = viewportRenderer_.render(width, height);
SDL_GPUTexture* gpuTex = viewportRenderer_.getTexture(texture);
ImGui::Image(reinterpret_cast<ImTextureID>(gpuTex), ImVec2(width, height));
```

## 注意事项

1. `renderToTextureOffscreen()` 内部调用 `SDL_WaitForGPUIdle()` 确保渲染完成，可能影响性能
2. 每帧都会重新创建 command buffer，未来可考虑复用
3. `buildCommandBuffer()` 逻辑与 `RenderSystem` 重复，可考虑抽取为共享函数
