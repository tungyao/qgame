#pragma once
#include <entt/entt.hpp>

namespace editor {

class InspectorPanel {
public:
    void draw(entt::entity selected, entt::registry& world);

private:
    void drawAddComponentMenu(entt::entity selected, entt::registry& world);
    void addComponentByName(entt::entity entity, entt::registry& world, const char* componentName);
};

} // namespace editor