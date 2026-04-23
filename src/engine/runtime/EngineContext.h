#pragma once
#include <memory>
#include <entt/entt.hpp>

#include "EngineConfig.h"
#include "SystemRegistry.h"
#include "FrameScheduler.h"
#include "../../platform/Window.h"
#include "../../core/Handle.h"
#include "../input/InputState.h"

// forward-declare backend types — 头文件不 include backend，保持 PRIVATE 隔离
// 实现文件 EngineContext.cpp 才真正 include backend headers
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
    ~EngineContext();  // 定义在 .cpp，届时 backend 类型已完整
    // ECS world — public，Systems 直接操作
    entt::registry   world;
    entt::dispatcher dispatcher;  // 碰撞事件 / 游戏事件总线

    // Runtime — public
    SystemRegistry systems;
    FrameScheduler scheduler{*this};

    // Input — public（GameAPI / Systems 直接查询）
    InputState inputState;

    // Window — public（InputSystem 需要 pollEvents）
    std::unique_ptr<platform::Window> window;

    uint64_t frameCounter = 0;
    float    deltaTime    = 0.f;

    // ── 生命周期 ──────────────────────────────────────────────────────────
    void init(const EngineConfig& cfg);
    void run();       // 阻塞直到窗口关闭
    void shutdown();

    // ── Backend 访问（仅 engine 内部 Systems 可调用）─────────────────────
    backend::IRenderDevice&    renderDevice();
    backend::IAudioDevice&     audioDevice();
    backend::CommandBuffer&    renderCommandBuffer();
    backend::AudioCommandQueue& audioCommandQueue();

private:
    // Backend 指针全私有 — game/editor 通过 CMake PRIVATE 看不到这些类型
    std::unique_ptr<backend::IRenderDevice>    renderDevice_;
    std::unique_ptr<backend::IAudioDevice>     audioDevice_;
    std::unique_ptr<backend::AudioThread>      audioThread_;
    std::unique_ptr<backend::CommandBuffer>    renderCmdBuf_;
    std::unique_ptr<backend::AudioCommandQueue> audioCmdQueue_;

    bool running_ = false;
};

} // namespace engine
