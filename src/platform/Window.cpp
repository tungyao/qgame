// 自己的头文件必须在 SDL3 之前 include，防止 <windows.h> 宏污染我们的类解析
#include "Window.h"
#include "../core/Logger.h"
#include "../core/Assert.h"

// SDL3 在 Windows 上内部 include <windows.h>，放最后隔离宏影响
#include <SDL3/SDL.h>

namespace platform {

struct Window::Impl {
    SDL_Window* sdlWindow   = nullptr;
    int         width       = 0;
    int         height      = 0;
    bool        shouldClose = false;
};

Window::Window(const WindowConfig& cfg) {
    bool ok = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    if (!ok) {
        core::logError("SDL_Init failed: %s", SDL_GetError());
        ASSERT(false);
    }

    SDL_WindowFlags flags = 0;
    if (cfg.resizable)  flags |= SDL_WINDOW_RESIZABLE;
    if (cfg.openglMode) flags |= SDL_WINDOW_OPENGL;

    impl_ = new Impl{};
    impl_->width  = cfg.width;
    impl_->height = cfg.height;
    impl_->sdlWindow = SDL_CreateWindow(cfg.title.c_str(), cfg.width, cfg.height, flags);
    ASSERT_MSG(impl_->sdlWindow, "Failed to create SDL window");
    core::logInfo("Window created: %s (%dx%d)", cfg.title.c_str(), cfg.width, cfg.height);
}

Window::~Window() {
    if (impl_) {
        if (impl_->sdlWindow) SDL_DestroyWindow(impl_->sdlWindow);
        SDL_Quit();
        delete impl_;
    }
}

void Window::pollEvents(const EventCallback& cb) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        InputRawEvent raw{};
        switch (e.type) {

        case SDL_EVENT_QUIT:
            impl_->shouldClose = true;
            raw.type = InputRawEvent::Type::QUIT;
            cb(raw);
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            raw.type      = InputRawEvent::Type::POINTER_DOWN;
            raw.pointerId = 0;
            raw.x = e.button.x / static_cast<float>(impl_->width);
            raw.y = e.button.y / static_cast<float>(impl_->height);
            cb(raw);
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            raw.type      = InputRawEvent::Type::POINTER_UP;
            raw.pointerId = 0;
            raw.x = e.button.x / static_cast<float>(impl_->width);
            raw.y = e.button.y / static_cast<float>(impl_->height);
            cb(raw);
            break;

        case SDL_EVENT_MOUSE_MOTION:
            raw.type      = InputRawEvent::Type::POINTER_MOVE;
            raw.pointerId = 0;
            raw.x = e.motion.x / static_cast<float>(impl_->width);
            raw.y = e.motion.y / static_cast<float>(impl_->height);
            cb(raw);
            break;

        case SDL_EVENT_FINGER_DOWN:
            raw.type      = InputRawEvent::Type::POINTER_DOWN;
            raw.pointerId = static_cast<int>(e.tfinger.fingerID);
            raw.x = e.tfinger.x;
            raw.y = e.tfinger.y;
            cb(raw);
            break;

        case SDL_EVENT_FINGER_UP:
            raw.type      = InputRawEvent::Type::POINTER_UP;
            raw.pointerId = static_cast<int>(e.tfinger.fingerID);
            raw.x = e.tfinger.x;
            raw.y = e.tfinger.y;
            cb(raw);
            break;

        case SDL_EVENT_FINGER_MOTION:
            raw.type      = InputRawEvent::Type::POINTER_MOVE;
            raw.pointerId = static_cast<int>(e.tfinger.fingerID);
            raw.x = e.tfinger.x;
            raw.y = e.tfinger.y;
            cb(raw);
            break;

        case SDL_EVENT_KEY_DOWN:
            raw.type    = InputRawEvent::Type::KEY_DOWN;
            raw.keyCode = static_cast<int>(e.key.key);
            cb(raw);
            break;

        case SDL_EVENT_KEY_UP:
            raw.type    = InputRawEvent::Type::KEY_UP;
            raw.keyCode = static_cast<int>(e.key.key);
            cb(raw);
            break;

        case SDL_EVENT_WINDOW_RESIZED:
            impl_->width  = e.window.data1;
            impl_->height = e.window.data2;
            break;

        default:
            break;
        }
    }
}

void* Window::sdlWindow() const {
    return impl_->sdlWindow;
}

void* Window::nativeWindowHandle() const {
    SDL_PropertiesID props = SDL_GetWindowProperties(impl_->sdlWindow);
#if defined(SDL_PLATFORM_WIN32)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_LINUX)
    void* wl = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    if (wl) return wl;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
#elif defined(SDL_PLATFORM_MACOS)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_IOS)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_ANDROID)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
#else
    return nullptr;
#endif
}

void* Window::nativeDisplayHandle() const {
    SDL_PropertiesID props = SDL_GetWindowProperties(impl_->sdlWindow);
#if defined(SDL_PLATFORM_LINUX)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
#else
    return nullptr;
#endif
}

int  Window::width()       const { return impl_->width; }
int  Window::height()      const { return impl_->height; }
bool Window::shouldClose() const { return impl_->shouldClose; }
void Window::notifyResize(int w, int h) { impl_->width = w; impl_->height = h; }

} // namespace platform
