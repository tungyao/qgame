#pragma once
#include <entt/entt.hpp>
#include <cstdint>

namespace engine {

struct Interactable {
    float radius = 0.5f;
    uint32_t layer = 0;
};

enum class InteractType : uint8_t {
    Hover,
    Click,
    Release,
};

struct InteractEvent {
    entt::entity entity;
    InteractType type;
    float worldX;
    float worldY;
    float screenX;
    float screenY;
};

} // namespace engine
