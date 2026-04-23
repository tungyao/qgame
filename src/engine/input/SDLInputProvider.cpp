#include "SDLInputProvider.h"
#include "InputState.h"
#include "../../platform/Window.h"
#include "../../platform/InputRawEvent.h"

namespace engine {

SDLInputProvider::SDLInputProvider(platform::Window& window)
    : window_(window) {}

bool SDLInputProvider::poll(InputState& out) {
    out.beginFrame();

    bool quit = false;
    window_.pollEvents([&](const platform::InputRawEvent& e) {
        if (e.type == platform::InputRawEvent::Type::QUIT) quit = true;
        out.feedEvent(e);
    });
    return !quit;
}

} // namespace engine
