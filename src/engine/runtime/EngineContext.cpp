#include "EngineContext.h"
#include "FrameScheduler.h"

// backend headers — 只有这个 .cpp 能 include，不会传播给外部
#include "../../backend/renderer/IRenderDevice.h"
#include "../../backend/renderer/CommandBuffer.h"
#include "../../backend/renderer/sdl_gpu/SDLGPURenderDevice.h"
#include "../../backend/renderer/opengl/GLRenderDevice.h"
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
#include "../systems/AnimatorSystem.h"
#include "../input/SDLInputProvider.h"
#include "../../backend/audio/sdl_mixer/SDLMixerAudioDevice.h"

namespace engine {

EngineContext::EngineContext()  = default;
EngineContext::~EngineContext() = default;

void EngineContext::init(const EngineConfig& cfg) {
    core::logInfo("EngineContext::init");

    // 窗口
    platform::WindowConfig wcfg;
    wcfg.title      = cfg.windowTitle;
    wcfg.width      = cfg.windowWidth;
    wcfg.height     = cfg.windowHeight;
    wcfg.vsync      = cfg.vsync;
    wcfg.resizable  = cfg.resizable;
    wcfg.debug      = cfg.debug;
    wcfg.openglMode = (cfg.renderBackend == RenderBackend::OpenGL);
    window = std::make_unique<platform::Window>(wcfg);

    // Command buffer
    renderCmdBuf_ = std::make_unique<backend::CommandBuffer>();

    // 渲染后端选择
    SDL_Window* sdlWin = static_cast<SDL_Window*>(window->sdlWindow());
    if (cfg.renderBackend == RenderBackend::OpenGL) {
        core::logInfo("Render backend: OpenGL 3.3");
        renderDevice_ = std::make_unique<backend::GLRenderDevice>(sdlWin,wcfg.debug);
    } else {
        core::logInfo("Render backend: SDL_GPU (Vulkan/Metal/D3D12)");
        renderDevice_ = std::make_unique<backend::SDLGPURenderDevice>(sdlWin, wcfg.debug);
    }
    renderDevice_->init();
    
    // Fallback to OpenGL if SDL_GPU failed
    if (cfg.renderBackend != RenderBackend::OpenGL) {
        auto* gpuDevice = static_cast<backend::SDLGPURenderDevice*>(renderDevice_.get());
        if (!gpuDevice->gpuDevice()) {
            core::logWarn("SDL_GPU initialization failed, falling back to OpenGL");
            renderDevice_->shutdown();
            renderDevice_.reset();
            
            // Recreate window in OpenGL mode
            window.reset();
            wcfg.openglMode = true;
            window = std::make_unique<platform::Window>(wcfg);
            sdlWin = static_cast<SDL_Window*>(window->sdlWindow());
            
            renderDevice_ = std::make_unique<backend::GLRenderDevice>(sdlWin);
            renderDevice_->init();
        }
    }
    
    assetManager.init(renderDevice_.get(), nullptr); // audio device 还未创建，稍后在 audio init 后更新

    // Audio device + command queue
    audioCmdQueue_ = std::make_unique<backend::AudioCommandQueue>();
    audioDevice_ = std::make_unique<backend::SDLMixerAudioDevice>();
    assetManager.init(renderDevice_.get(), audioDevice_.get()); // 重新初始化，补上 audio ptr

    // 注册 Systems
    systems.registerSystem<InputSystem>(
        inputState,
        std::make_unique<SDLInputProvider>(*window)
    );
    systems.registerSystem<RenderSystem>(*this);
    systems.registerSystem<AudioSystem>(*this);
    systems.registerSystem<PhysicsSystem>(world, dispatcher);
    systems.registerSystem<AnimatorSystem>(*this);

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
    assetManager.shutdown();
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
