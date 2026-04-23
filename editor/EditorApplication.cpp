#include "EditorApplication.h"

#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <engine/components/RenderComponents.h>

namespace editor {

namespace {

std::vector<uint8_t> makeCheckerTexture(int width, int height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 255u);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool light = ((x / 8) + (y / 8)) % 2 == 0;
            const uint8_t value = light ? 230u : 90u;
            const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
            pixels[index + 0] = value;
            pixels[index + 1] = static_cast<uint8_t>(light ? 180u : 110u);
            pixels[index + 2] = static_cast<uint8_t>(light ? 80u : 170u);
            pixels[index + 3] = 255u;
        }
    }
    return pixels;
}

const char* entityLabel(entt::entity entity) {
    static char buffer[32];
    SDL_snprintf(buffer, sizeof(buffer), "Entity %u", static_cast<unsigned>(entt::to_integral(entity)));
    return buffer;
}

} // namespace

EditorApplication::~EditorApplication() {
    shutdownImGui();
    ctx_.shutdown();
}

void EditorApplication::run() {
    engine::EngineConfig config;
    config.windowTitle = "StarEngine Editor";
    config.windowWidth = 1600;
    config.windowHeight = 900;
    config.resizable = true;

    ctx_.init(config);
    setupDemoScene();
    setupImGui();

    while (ctx_.scheduler.tick()) {
        beginImGuiFrame();
        drawDockspace();
        drawHierarchy();
        drawInspector();
        drawViewport();
        drawStats();
        endImGuiFrame();

        if (ctx_.inputState.isKeyJustPressed(SDLK_ESCAPE)) {
            game_.quit();
        }
    }
}

void EditorApplication::setupDemoScene() {
    const std::vector<uint8_t> texturePixels = makeCheckerTexture(64, 64);
    const TextureHandle texture = game_.createTextureFromMemory(texturePixels.data(), 64, 64);

    const auto camera = game_.spawnEntity();
    game_.addComponent(camera, engine::Transform{ 0.0f, 0.0f, 0.0f, 1.0f, 1.0f });
    game_.addComponent(camera, engine::Camera{ 1.0f, true });

    const auto sprite = game_.spawnEntity();
    game_.addComponent(sprite, engine::Transform{ 320.0f, 180.0f, 0.0f, 2.0f, 2.0f });
    game_.addComponent(sprite, engine::Sprite{ texture, { 0.0f, 0.0f, 64.0f, 64.0f }, 1, core::Color::White, 0.5f, 0.5f });

    const auto tilemap = game_.spawnEntity();
    game_.addComponent(tilemap, engine::Transform{ 2.0f, 8.0f, 0.0f, 1.0f, 1.0f });
    engine::TileMap map;
    map.width = 16;
    map.height = 8;
    map.tileSize = 32;
    map.tileset = texture;
    map.tilesetCols = 2;
    map.layers[0].assign(static_cast<size_t>(map.width * map.height), 0);
    game_.addComponent(tilemap, map);

    selectedEntity_ = sprite;
    editor_.setEditorCamera(editorCameraX_, editorCameraY_, editorCameraZoom_);
}

void EditorApplication::setupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.DisplaySize = ImVec2(static_cast<float>(ctx_.window->width()), static_cast<float>(ctx_.window->height()));

    imguiReady_ = true;
}

void EditorApplication::shutdownImGui() {
    if (!imguiReady_) {
        return;
    }
    ImGui::DestroyContext();
    imguiReady_ = false;
}

void EditorApplication::beginImGuiFrame() {
    if (!imguiReady_) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ctx_.window->width()), static_cast<float>(ctx_.window->height()));
    io.DeltaTime = ctx_.deltaTime > 0.0f ? ctx_.deltaTime : (1.0f / 60.0f);
    ImGui::NewFrame();
}

void EditorApplication::drawDockspace() {
    ImGui::Begin("Editor");

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit")) {
                game_.quit();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::TextUnformatted("ImGui editor shell");
    ImGui::TextUnformatted("Scene, hierarchy, inspector and stats panels are active.");
    ImGui::End();
}

void EditorApplication::drawHierarchy() {
    ImGui::Begin("Hierarchy");
    auto& entityStorage = ctx_.world.storage<entt::entity>();
    for (const entt::entity entity : entityStorage) {
        const bool selected = entity == selectedEntity_;
        if (ImGui::Selectable(entityLabel(entity), selected)) {
            selectedEntity_ = entity;
        }
    }
    ImGui::End();
}

void EditorApplication::drawInspector() {
    ImGui::Begin("Inspector");
    if (selectedEntity_ == entt::null || !ctx_.world.valid(selectedEntity_)) {
        ImGui::TextUnformatted("Select an entity to inspect.");
        ImGui::End();
        return;
    }

    ImGui::Text("%s", entityLabel(selectedEntity_));

    if (ctx_.world.all_of<engine::Transform>(selectedEntity_)) {
        auto& transform = ctx_.world.get<engine::Transform>(selectedEntity_);
        ImGui::SeparatorText("Transform");
        ImGui::DragFloat2("Position", &transform.x, 1.0f);
        ImGui::DragFloat("Rotation", &transform.rotation, 0.01f);
        ImGui::DragFloat2("Scale", &transform.scaleX, 0.01f, 0.1f, 10.0f);
    }

    if (ctx_.world.all_of<engine::Camera>(selectedEntity_)) {
        auto& camera = ctx_.world.get<engine::Camera>(selectedEntity_);
        ImGui::SeparatorText("Camera");
        ImGui::Checkbox("Primary", &camera.primary);
        if (ImGui::DragFloat("Zoom", &camera.zoom, 0.01f, 0.1f, 8.0f)) {
            editorCameraZoom_ = camera.zoom;
        }
    }

    if (ctx_.world.all_of<engine::Sprite>(selectedEntity_)) {
        auto& sprite = ctx_.world.get<engine::Sprite>(selectedEntity_);
        ImGui::SeparatorText("Sprite");
        ImGui::Text("Texture Handle: %u", sprite.texture.index);
        ImGui::DragInt("Layer", &sprite.layer, 1.0f);
        float tint[4] = {
            sprite.tint.r / 255.0f,
            sprite.tint.g / 255.0f,
            sprite.tint.b / 255.0f,
            sprite.tint.a / 255.0f
        };
        if (ImGui::ColorEdit4("Tint", tint)) {
            sprite.tint.r = static_cast<uint8_t>(tint[0] * 255.0f);
            sprite.tint.g = static_cast<uint8_t>(tint[1] * 255.0f);
            sprite.tint.b = static_cast<uint8_t>(tint[2] * 255.0f);
            sprite.tint.a = static_cast<uint8_t>(tint[3] * 255.0f);
        }
    }

    ImGui::End();
}

void EditorApplication::drawViewport() {
    ImGui::Begin("Viewport");
    const ImVec2 viewportSize = ImGui::GetContentRegionAvail();
    if (viewportSize.x > 0.0f && viewportSize.y > 0.0f) {
        editor_.setEditorCamera(editorCameraX_, editorCameraY_, editorCameraZoom_);
        const TextureHandle preview = editor_.renderSceneToTexture(
            static_cast<int>(viewportSize.x),
            static_cast<int>(viewportSize.y)
        );

        if (preview.valid()) {
            ImGui::Text("Preview handle: %u", preview.index);
        } else {
            ImGui::Button("Scene preview backend pending", viewportSize);
        }
    }
    ImGui::End();
}

void EditorApplication::drawStats() {
    ImGui::Begin("Stats");
    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(ctx_.frameCounter));
    ImGui::Text("Delta: %.4f", ctx_.deltaTime);
    ImGui::Text("Entities: %u", static_cast<unsigned>(ctx_.world.storage<entt::entity>().size()));
    ImGui::Text("Transient: %u", static_cast<unsigned>(editor_.transientEntities().size()));
    ImGui::TextUnformatted("Month 6 editor shell is ready for scene, hierarchy, inspector and viewport workflows.");
    ImGui::End();
}

void EditorApplication::endImGuiFrame() {
    if (!imguiReady_) {
        return;
    }
    ImGui::Render();
}

} // namespace editor
