#pragma once
#include "IInputProvider.h"

namespace platform { class Window; }

namespace engine {

// 桌面端 provider：从 SDL Window 泵事件，统一映射为 InputRawEvent
class SDLInputProvider : public IInputProvider {
public:
    explicit SDLInputProvider(platform::Window& window);
    bool poll(InputState& out) override;

private:
    platform::Window& window_;
};

} // namespace engine
