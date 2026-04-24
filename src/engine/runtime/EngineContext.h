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

    uint64_t frameCounter = 0;
    float deltaTime = 0.0f;
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
};

} // namespace engine
