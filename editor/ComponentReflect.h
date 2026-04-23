#pragma once
#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <functional>
#include <array>

namespace editor {

using ComponentDrawFn = std::function<void(entt::entity, entt::registry&)>;

struct ComponentMeta {
    const char* name;
    ComponentDrawFn drawFn;
};

class ComponentRegistry {
public:
    template<typename T>
    static void registerComponent(const char* name, ComponentDrawFn drawFn);

    template<typename T>
    static const ComponentMeta* get();

    static std::vector<const ComponentMeta*> getAllRegistered();
    static std::vector<const ComponentMeta*> getAvailableComponents(entt::entity entity, entt::registry& world);

    template<typename T>
    static void addComponent(entt::entity e, entt::registry& world);

private:
    static constexpr size_t MAX_COMPONENTS = 32;

    struct Entry {
        const char* name = nullptr;
        ComponentDrawFn drawFn;
        size_t typeHash = 0;
    };

    static std::array<Entry, MAX_COMPONENTS>& getEntries();
    static size_t& getEntryCount();
};

template<typename T>
const ComponentMeta* ComponentRegistry::get() {
    auto& entries = getEntries();
    const size_t targetHash = entt::type_hash<T>::value();
    for (size_t i = 0; i < getEntryCount(); ++i) {
        if (entries[i].typeHash == targetHash) {
            return reinterpret_cast<const ComponentMeta*>(&entries[i]);
        }
    }
    return nullptr;
}

template<typename T>
void ComponentRegistry::registerComponent(const char* name, ComponentDrawFn drawFn) {
    auto& entries = getEntries();
    size_t& count = getEntryCount();

    if (count >= MAX_COMPONENTS) {
        return;
    }

    entries[count].name = name;
    entries[count].drawFn = std::move(drawFn);
    entries[count].typeHash = entt::type_hash<T>::value();
    ++count;
}

template<typename T>
void ComponentRegistry::addComponent(entt::entity e, entt::registry& world) {
    world.emplace<T>(e);
}

void registerComponentEditors();

} // namespace editor