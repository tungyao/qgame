#include "EngineContext.h"
#include "FrameScheduler.h"

// backend headers — 只有这个 .cpp 能 include，不会传播给外部
#include "../../backend/renderer/IRenderDevice.h"
#include "../../backend/renderer/CommandBuffer.h"
#include "../../backend/renderer/sdl_gpu/SDLGPURenderDevice.h"
#include "../../backend/audio/IAudioDevice.h"
#include "../../backend/audio/AudioCommandQueue.h"
#include "../../backend/audio/AudioThread.h"
#include "../../core/Logger.h"
#include "../../core/Assert.h"
#include <SDL3/SDL.h>

// engine systems
#include "../systems/RenderSystem.h"
#include "../systems/AudioSystem.h"
#include "../systems/PhysicsSystem.h"
#include "../systems/InputSystem.h"
#include "../input/SDLInputProvider.h"
#include "../../backend/audio/sdl_mixer/SDLMixerAudioDevice.h"

namespace engine {

EngineContext::EngineContext()  = default;
EngineContext::~EngineContext() = default;

void EngineContext::init(const EngineConfig& cfg) {
    core::logInfo("EngineContext::init");

    // 窗口
    platform::WindowConfig wcfg;
    wcfg.title     = cfg.windowTitle;
    wcfg.width     = cfg.windowWidth;
    wcfg.height    = cfg.windowHeight;
    wcfg.vsync     = cfg.vsync;
    wcfg.resizable = cfg.resizable;
    window = std::make_unique<platform::Window>(wcfg);

    // Command buffer
    renderCmdBuf_ = std::make_unique<backend::CommandBuffer>();

    // SDL3 GPU 渲染设备（需要 SDL_Window* 句柄）
    auto sdlGpu = std::make_unique<backend::SDLGPURenderDevice>(
        static_cast<SDL_Window*>(window->sdlWindow())
    );
    renderDevice_ = std::move(sdlGpu);
    renderDevice_->init();

    // Audio device + command queue
    audioCmdQueue_ = std::make_unique<backend::AudioCommandQueue>();
    audioDevice_ = std::make_unique<backend::SDLMixerAudioDevice>();

    // 注册 Systems
    systems.registerSystem<InputSystem>(
        inputState,
        std::make_unique<SDLInputProvider>(*window)
    );
    systems.registerSystem<RenderSystem>(*this);
    systems.registerSystem<AudioSystem>(*this);
    systems.registerSystem<PhysicsSystem>(world, dispatcher);

    systems.initAll();

    running_ = true;
    core::logInfo("EngineContext ready");
}

void EngineContext::run() {
    ASSERT_MSG(window, "Call init() before run()");
    core::logInfo("Entering main loop");
    while (running_) {
        if (!scheduler.tick()) running_ = false;
    }
    core::logInfo("Exiting main loop");
}

void EngineContext::shutdown() {
    core::logInfo("EngineContext::shutdown");
    systems.shutdownAll();
    audioThread_.reset();
    audioDevice_.reset();
    renderDevice_->shutdown();
    renderDevice_.reset();
    window.reset();
}

// ── Backend accessors ─────────────────────────────────────────────────────────

backend::IRenderDevice& EngineContext::renderDevice() {
    ASSERT_MSG(renderDevice_, "RenderDevice not initialized");
    return *renderDevice_;
}

backend::IAudioDevice& EngineContext::audioDevice() {
    ASSERT_MSG(audioDevice_, "AudioDevice not initialized");
    return *audioDevice_;
}

backend::CommandBuffer& EngineContext::renderCommandBuffer() {
    ASSERT(renderCmdBuf_);
    return *renderCmdBuf_;
}

backend::AudioCommandQueue& EngineContext::audioCommandQueue() {
    ASSERT(audioCmdQueue_);
    return *audioCmdQueue_;
}

} // namespace engine
