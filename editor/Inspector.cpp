#include "Inspector.h"
#include "ComponentReflect.h"
#include "FieldEditors.h"
#include <imgui.h>
#include <SDL3/SDL.h>
#include <cstring>
#include <functional>
#include <engine/components/RenderComponents.h>
#include <engine/components/PhysicsComponents.h>

namespace editor {

namespace {

char entityLabelBuffer[64];

const char* entityLabel(entt::entity entity) {
    SDL_snprintf(entityLabelBuffer, sizeof(entityLabelBuffer), "Entity %u", static_cast<unsigned>(entt::to_integral(entity)));
    return entityLabelBuffer;
}

void drawTreeNode(const char* label, const std::function<void()>& contentFn) {
    if (ImGui::TreeNode(label)) {
        contentFn();
        ImGui::TreePop();
    }
}

} // namespace

void InspectorPanel::draw(entt::entity selected, entt::registry& world) {
    ImGui::Begin("Inspector");

    if (selected == entt::null || !world.valid(selected)) {
        ImGui::TextUnformatted("Select an entity to inspect.");
        ImGui::End();
        return;
    }

    ImGui::Text("%s", entityLabel(selected));
    ImGui::Separator();

    if (ImGui::TreeNode("Components")) {
        bool firstComponent = true;

        auto drawIfPresent = [&](const char* name, auto&& getFn, auto&& removeFn) {
            if (getFn()) {
                firstComponent = false;
                ImGui::PushID(name);

                ImGui::BeginGroup();
                bool open = ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen);

                ImGui::SameLine(ImGui::GetContentRegionMax().x - 60);
                if (ImGui::SmallButton("X")) {
                    removeFn();
                }
                ImGui::EndGroup();

                if (open) {
                    ImGui::Indent();
                    getFn()();
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
        };

        drawIfPresent("Transform",
            [&]() -> std::function<void()> {
                if (world.all_of<engine::Transform>(selected)) {
                    return [&]() {
                        auto& c = world.get<engine::Transform>(selected);
                        drawTreeNode("Position", [&]() {
                            editFloat(&c.x, "X");
                            editFloat(&c.y, "Y");
                        });
                        drawTreeNode("Rotation", [&]() {
                            editFloat(&c.rotation, "Radians", 0.01f);
                        });
                        drawTreeNode("Scale", [&]() {
                            editFloat(&c.scaleX, "Scale X", 0.01f, 0.1f, 10.0f);
                            editFloat(&c.scaleY, "Scale Y", 0.01f, 0.1f, 10.0f);
                        });
                    };
                }
                return nullptr;
            },
            [&]() { world.remove<engine::Transform>(selected); }
        );

        drawIfPresent("Sprite",
            [&]() -> std::function<void()> {
                if (world.all_of<engine::Sprite>(selected)) {
                    return [&]() {
                        auto& c = world.get<engine::Sprite>(selected);
                        ImGui::Text("Texture: %u", c.texture.index);
                        drawTreeNode("Source Rect", [&]() {
                            editFloat(&c.srcRect.x, "X");
                            editFloat(&c.srcRect.y, "Y");
                            editFloat(&c.srcRect.w, "W");
                            editFloat(&c.srcRect.h, "H");
                        });
                        drawTreeNode("Properties", [&]() {
                            editInt(&c.layer, "Layer");
                            editColor(&c.tint, "Tint");
                        });
                        drawTreeNode("Pivot", [&]() {
                            editFloat(&c.pivotX, "X", 0.01f, 0.0f, 1.0f);
                            editFloat(&c.pivotY, "Y", 0.01f, 0.0f, 1.0f);
                        });
                    };
                }
                return nullptr;
            },
            [&]() { world.remove<engine::Sprite>(selected); }
        );

        drawIfPresent("TileMap",
            [&]() -> std::function<void()> {
                if (world.all_of<engine::TileMap>(selected)) {
                    return [&]() {
                        auto& c = world.get<engine::TileMap>(selected);
                        drawTreeNode("Dimensions", [&]() {
                            editInt(&c.width, "Width");
                            editInt(&c.height, "Height");
                            editInt(&c.tileSize, "Tile Size");
                        });
                        drawTreeNode("Tileset", [&]() {
                            ImGui::Text("Handle: %u", c.tileset.index);
                            editInt(&c.tilesetCols, "Columns");
                        });
                        ImGui::Text("Layers: %d", engine::TileMap::MAX_LAYERS);
                    };
                }
                return nullptr;
            },
            [&]() { world.remove<engine::TileMap>(selected); }
        );

        drawIfPresent("Camera",
            [&]() -> std::function<void()> {
                if (world.all_of<engine::Camera>(selected)) {
                    return [&]() {
                        auto& c = world.get<engine::Camera>(selected);
                        editBool(&c.primary, "Primary");
                        editFloat(&c.zoom, "Zoom", 0.01f, 0.1f, 8.0f);
                    };
                }
                return nullptr;
            },
            [&]() { world.remove<engine::Camera>(selected); }
        );

        drawIfPresent("RigidBody",
            [&]() -> std::function<void()> {
                if (world.all_of<engine::RigidBody>(selected)) {
                    return [&]() {
                        auto& c = world.get<engine::RigidBody>(selected);
                        drawTreeNode("Velocity", [&]() {
                            editFloat(&c.velocityX, "X");
                            editFloat(&c.velocityY, "Y");
                        });
                        drawTreeNode("Physics", [&]() {
                            editFloat(&c.gravityScale, "Gravity Scale");
                            editBool(&c.isKinematic, "Is Kinematic");
                        });
                    };
                }
                return nullptr;
            },
            [&]() { world.remove<engine::RigidBody>(selected); }
        );

        drawIfPresent("Collider",
            [&]() -> std::function<void()> {
                if (world.all_of<engine::Collider>(selected)) {
                    return [&]() {
                        auto& c = world.get<engine::Collider>(selected);
                        drawTreeNode("Size", [&]() {
                            editFloat(&c.width, "Width");
                            editFloat(&c.height, "Height");
                        });
                        drawTreeNode("Offset", [&]() {
                            editFloat(&c.offsetX, "X");
                            editFloat(&c.offsetY, "Y");
                        });
                        editBool(&c.isTrigger, "Is Trigger");
                    };
                }
                return nullptr;
            },
            [&]() { world.remove<engine::Collider>(selected); }
        );

        ImGui::TreePop();
    }

    ImGui::Separator();
    drawAddComponentMenu(selected, world);

    ImGui::End();
}

void InspectorPanel::drawAddComponentMenu(entt::entity selected, entt::registry& world) {
    if (ImGui::Button("Add Component")) {
        ImGui::OpenPopup("AddComponentPopup");
    }

    if (ImGui::BeginPopup("AddComponentPopup")) {
        auto available = ComponentRegistry::getAvailableComponents(selected, world);

        if (available.empty()) {
            ImGui::TextUnformatted("No components available.");
        } else {
            ImGui::TextUnformatted("Select a component to add:");
            ImGui::Separator();

            for (const auto* meta : available) {
                if (ImGui::Selectable(meta->name)) {
                    addComponentByName(selected, world, meta->name);
                }
            }
        }

        ImGui::EndPopup();
    }
}

void InspectorPanel::addComponentByName(entt::entity entity, entt::registry& world, const char* componentName) {
    if (strcmp(componentName, "Transform") == 0) {
        world.emplace<engine::Transform>(entity);
    } else if (strcmp(componentName, "Sprite") == 0) {
        world.emplace<engine::Sprite>(entity);
    } else if (strcmp(componentName, "TileMap") == 0) {
        world.emplace<engine::TileMap>(entity);
    } else if (strcmp(componentName, "Camera") == 0) {
        world.emplace<engine::Camera>(entity);
    } else if (strcmp(componentName, "RigidBody") == 0) {
        world.emplace<engine::RigidBody>(entity);
    } else if (strcmp(componentName, "Collider") == 0) {
        world.emplace<engine::Collider>(entity);
    }
}

} // namespace editor