#include "ComponentReflect.h"
#include <engine/components/RenderComponents.h>
#include <engine/components/PhysicsComponents.h>

namespace editor {

std::array<ComponentRegistry::Entry, ComponentRegistry::MAX_COMPONENTS>& ComponentRegistry::getEntries() {
    static std::array<Entry, MAX_COMPONENTS> entries;
    return entries;
}

size_t& ComponentRegistry::getEntryCount() {
    static size_t count = 0;
    return count;
}

std::vector<const ComponentMeta*> ComponentRegistry::getAllRegistered() {
    std::vector<const ComponentMeta*> result;
    auto& entries = getEntries();
    for (size_t i = 0; i < getEntryCount(); ++i) {
        result.push_back(reinterpret_cast<const ComponentMeta*>(&entries[i]));
    }
    return result;
}

std::vector<const ComponentMeta*> ComponentRegistry::getAvailableComponents(entt::entity entity, entt::registry& world) {
    std::vector<const ComponentMeta*> result;
    auto& entries = getEntries();

    for (size_t i = 0; i < getEntryCount(); ++i) {
        const auto& entry = entries[i];
        const size_t hash = entry.typeHash;
        bool hasComponent = false;

        if (hash == entt::type_hash<engine::Name>::value() && world.all_of<engine::Name>(entity)) hasComponent = true;
        else if (hash == entt::type_hash<engine::Transform>::value() && world.all_of<engine::Transform>(entity)) hasComponent = true;
        else if (hash == entt::type_hash<engine::Sprite>::value() && world.all_of<engine::Sprite>(entity)) hasComponent = true;
        else if (hash == entt::type_hash<engine::TileMap>::value() && world.all_of<engine::TileMap>(entity)) hasComponent = true;
        else if (hash == entt::type_hash<engine::Camera>::value() && world.all_of<engine::Camera>(entity)) hasComponent = true;
        else if (hash == entt::type_hash<engine::RigidBody>::value() && world.all_of<engine::RigidBody>(entity)) hasComponent = true;
        else if (hash == entt::type_hash<engine::Collider>::value() && world.all_of<engine::Collider>(entity)) hasComponent = true;

        if (!hasComponent) {
            result.push_back(reinterpret_cast<const ComponentMeta*>(&entry));
        }
    }
    return result;
}

} // namespace editor