#include "InputSystem.h"
#include "../input/InputState.h"

namespace engine {

InputSystem::InputSystem(InputState& state, std::unique_ptr<IInputProvider> provider)
    : state_(state), provider_(std::move(provider)) {}

bool InputSystem::pollInput() {
    if (!provider_) return true;
    return provider_->poll(state_);
}

void InputSystem::setProvider(std::unique_ptr<IInputProvider> provider) {
    provider_ = std::move(provider);
}

} // namespace engine
