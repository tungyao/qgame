#include "ComponentEditors.h"
#include "FieldEditors.h"
#include <engine/components/RenderComponents.h>
#include <engine/components/PhysicsComponents.h>
#include <imgui.h>

namespace editor {

void registerComponentEditors() {
    ComponentRegistry::registerComponent<engine::Name>("Name", [](entt::entity entity, entt::registry& world) {
        auto& c = world.get<engine::Name>(entity);
        ImGui::InputText("Name", c.buf.data(), engine::Name::MAX_LEN);
    });

    ComponentRegistry::registerComponent<engine::Transform>("Transform", [](entt::entity entity, entt::registry& world) {
        auto& c = world.get<engine::Transform>(entity);
        editFloat(&c.x, "Position X");
        editFloat(&c.y, "Position Y");
        editFloat(&c.rotation, "Rotation", 0.01f);
        editFloat(&c.scaleX, "Scale X", 0.01f, 0.1f, 10.0f);
        editFloat(&c.scaleY, "Scale Y", 0.01f, 0.1f, 10.0f);
    });

    ComponentRegistry::registerComponent<engine::Sprite>("Sprite", [](entt::entity entity, entt::registry& world) {
        auto& c = world.get<engine::Sprite>(entity);
        ImGui::Text("Texture: %u", c.texture.index);
        editRect(&c.srcRect, "Source Rect");
        editInt(&c.layer, "Layer");
        editColor(&c.tint, "Tint");
        editFloat(&c.pivotX, "Pivot X", 0.01f, 0.0f, 1.0f);
        editFloat(&c.pivotY, "Pivot Y", 0.01f, 0.0f, 1.0f);
    });

    ComponentRegistry::registerComponent<engine::TileMap>("TileMap", [](entt::entity entity, entt::registry& world) {
        auto& c = world.get<engine::TileMap>(entity);
        editInt(&c.width, "Width");
        editInt(&c.height, "Height");
        editInt(&c.tileSize, "Tile Size");
        editInt(&c.tilesetCols, "Tileset Columns");
        ImGui::Text("Tileset: %u", c.tileset.index);
        ImGui::Text("Layers: %d", engine::TileMap::MAX_LAYERS);
    });

    ComponentRegistry::registerComponent<engine::Camera>("Camera", [](entt::entity entity, entt::registry& world) {
        auto& c = world.get<engine::Camera>(entity);
        editBool(&c.primary, "Primary");
        editFloat(&c.zoom, "Zoom", 0.01f, 0.1f, 8.0f);
    });

    ComponentRegistry::registerComponent<engine::RigidBody>("RigidBody", [](entt::entity entity, entt::registry& world) {
        auto& c = world.get<engine::RigidBody>(entity);
        editFloat(&c.velocityX, "Velocity X");
        editFloat(&c.velocityY, "Velocity Y");
        editFloat(&c.gravityScale, "Gravity Scale");
        editBool(&c.isKinematic, "Is Kinematic");
    });

    ComponentRegistry::registerComponent<engine::Collider>("Collider", [](entt::entity entity, entt::registry& world) {
        auto& c = world.get<engine::Collider>(entity);
        editFloat(&c.width, "Width");
        editFloat(&c.height, "Height");
        editFloat(&c.offsetX, "Offset X");
        editFloat(&c.offsetY, "Offset Y");
        editBool(&c.isTrigger, "Is Trigger");
    });
}

} // namespace editor