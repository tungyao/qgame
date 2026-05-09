#pragma once

#include <functional>
#include <memory>

#include <entt/entt.hpp>

#include "EngineConfig.h"
#include "SystemRegistry.h"
#include "FrameScheduler.h"
#include "../../platform/Window.h"
#include "../../core/Handle.h"
#include "../input/InputState.h"
#include "../assets/AssetManager.h"

namespace backend {
class IRenderDevice;
class IAudioDevice;
class AudioThread;
class CommandBuffer;
class AudioCommandQueue;
}

namespace engine {

class EngineContext {
public:
    EngineContext();
    ~EngineContext();

    entt::registry world;
    entt::dispatcher dispatcher;

    SystemRegistry systems;
    FrameScheduler scheduler{*this};

    InputState inputState;
    AssetManager assetManager;
    std::unique_ptr<platform::Window> window;
    // 公开给 gameplay/demo/mod 使用的当前窗口尺寸缓存。
    //
    // 设计上故意把这个值放在 EngineContext，而不是要求上层直接调用
    // platform::Window::width()/height()：
    // 1. gameplay/demo 代码不应该为了读分辨率而依赖 platform 静态库符号；
    // 2. 这样可执行文件只需要链接 engine，就能安全读取当前视口大小；
    // 3. 后续若接入真正的 drawable/swapchain 像素尺寸，这里也可以成为统一出口。
    int windowWidth = 0;
    int windowHeight = 0;

    uint64_t frameCounter = 0;
    float deltaTime = 0.0f;
    float timeScale = 1.0f;   // Phase 5.4: 全局时间缩放 (hit-stop / 慢动作)
    // 所有显式 phase（包括 Render）结束后、present() 之前的最后一个 CPU 钩子。
    // editor/debug 工具可在这里读取本帧最终状态做轻量收尾。
    std::function<void()> beforePresentCallback;
    bool renderToSwapchain = true;  // 设为 false 可禁用自动渲染到 swapchain（editor 模式）

    void init(const EngineConfig& cfg);
    void run();
    void shutdown();
    backend::IRenderDevice& renderDevice();
    backend::IAudioDevice& audioDevice();
    backend::CommandBuffer& renderCommandBuffer();
    backend::AudioCommandQueue& audioCommandQueue();

private:
    std::unique_ptr<backend::IRenderDevice> renderDevice_;
    std::unique_ptr<backend::IAudioDevice> audioDevice_;
    std::unique_ptr<backend::AudioThread> audioThread_;
    std::unique_ptr<backend::CommandBuffer> renderCmdBuf_;
    std::unique_ptr<backend::AudioCommandQueue> audioCmdQueue_;

    bool running_ = false;
    bool debug = false;
};

} // namespace engine
