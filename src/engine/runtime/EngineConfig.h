#pragma once
#include <string>

namespace engine {

enum class RenderBackend {
    SDL_GPU,  // Vulkan / Metal / D3D12（默认，现代 GPU）
    OpenGL,   // OpenGL 3.3 Core（兼容老显卡）
};

struct EngineConfig {
    std::string   windowTitle   = "StarEngine";
    int           windowWidth   = 1280;
    int           windowHeight  = 720;
    bool          vsync         = true;
    bool          resizable     = true;
    bool            debug = false;
    float         targetFps     = 60.f;
    RenderBackend renderBackend = RenderBackend::SDL_GPU;
};

} // namespace engine
