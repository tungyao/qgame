#pragma once

#include <entt/entt.hpp>

#include <engine/api/EditorAPI.h>
#include <engine/api/GameAPI.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>

namespace editor {

class EditorApplication {
public:
    EditorApplication() = default;
    ~EditorApplication();

    void run();

private:
    void setupDemoScene();
    void setupImGui();
    void shutdownImGui();
    void beginImGuiFrame();
    void syncInputToImGui();
    void drawDockspace();
    void drawHierarchy();
    void drawInspector();
    void drawViewport();
    void drawStats();
    void endImGuiFrame();

    engine::EngineContext ctx_;
    engine::GameAPI game_{ ctx_ };
    engine::EditorAPI editor_{ ctx_ };
    entt::entity selectedEntity_ = entt::null;
    float editorCameraX_ = 0.0f;
    float editorCameraY_ = 0.0f;
    float editorCameraZoom_ = 1.0f;
    bool imguiReady_ = false;
};

} // namespace editor
