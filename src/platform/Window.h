#pragma once
#include <cstdint>
#include <string>
#include "InputRawEvent.h"
#include <functional>

namespace platform {

struct WindowConfig {
    std::string title      = "StarEngine";
    int         width      = 1280;
    int         height     = 720;
    bool        vsync      = true;
    bool        resizable  = true;
    bool        openglMode = false;  // true → 设置 SDL_WINDOW_OPENGL，供 GL backend 使用
    bool        debug   = false;
};

class Window {
public:
    using EventCallback = std::function<void(const InputRawEvent&)>;

    explicit Window(const WindowConfig& cfg);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void pollEvents(const EventCallback& cb);

    // SDL_Window* 供 SDL3 GPU API 使用
    void* sdlWindow() const;

    // 平台原生句柄（供未来 bgfx 等底层后端使用）
    void* nativeWindowHandle() const;
    void* nativeDisplayHandle() const;  // Linux/X11

    int  width()  const;
    int  height() const;
    bool shouldClose() const;

    // 通知窗口大小变化（由 FrameScheduler 驱动）
    void notifyResize(int w, int h);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace platform
