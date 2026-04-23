#pragma once
#include <string>

namespace engine {

struct EngineConfig {
    std::string windowTitle  = "StarEngine";
    int         windowWidth  = 1280;
    int         windowHeight = 720;
    bool        vsync        = true;
    bool        resizable    = true;
    float       targetFps    = 60.f;   // 0 = unlocked
};

} // namespace engine
