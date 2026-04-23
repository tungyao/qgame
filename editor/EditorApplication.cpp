#include "EditorApplication.h"

#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>

#include <backend/renderer/sdl_gpu/SDLGPURenderDevice.h>
#include <engine/components/RenderComponents.h>
#include <core/Logger.h>

#include "ComponentEditors.h"

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

ImGuiKey mapSDLKey(int keyCode) {
    switch (keyCode) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_INSERT: return ImGuiKey_Insert;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_APOSTROPHE: return ImGuiKey_Apostrophe;
    case SDLK_COMMA: return ImGuiKey_Comma;
    case SDLK_MINUS: return ImGuiKey_Minus;
    case SDLK_PERIOD: return ImGuiKey_Period;
    case SDLK_SLASH: return ImGuiKey_Slash;
    case SDLK_SEMICOLON: return ImGuiKey_Semicolon;
    case SDLK_EQUALS: return ImGuiKey_Equal;
    case SDLK_LEFTBRACKET: return ImGuiKey_LeftBracket;
    case SDLK_BACKSLASH: return ImGuiKey_Backslash;
    case SDLK_RIGHTBRACKET: return ImGuiKey_RightBracket;
    case SDLK_GRAVE: return ImGuiKey_GraveAccent;
    case SDLK_CAPSLOCK: return ImGuiKey_CapsLock;
    case SDLK_SCROLLLOCK: return ImGuiKey_ScrollLock;
    case SDLK_NUMLOCKCLEAR: return ImGuiKey_NumLock;
    case SDLK_PRINTSCREEN: return ImGuiKey_PrintScreen;
    case SDLK_PAUSE: return ImGuiKey_Pause;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_LGUI: return ImGuiKey_LeftSuper;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    case SDLK_RGUI: return ImGuiKey_RightSuper;
    case SDLK_0: return ImGuiKey_0;
    case SDLK_1: return ImGuiKey_1;
    case SDLK_2: return ImGuiKey_2;
    case SDLK_3: return ImGuiKey_3;
    case SDLK_4: return ImGuiKey_4;
    case SDLK_5: return ImGuiKey_5;
    case SDLK_6: return ImGuiKey_6;
    case SDLK_7: return ImGuiKey_7;
    case SDLK_8: return ImGuiKey_8;
    case SDLK_9: return ImGuiKey_9;
    case SDLK_A: return ImGuiKey_A;
    case SDLK_B: return ImGuiKey_B;
    case SDLK_C: return ImGuiKey_C;
    case SDLK_D: return ImGuiKey_D;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_F: return ImGuiKey_F;
    case SDLK_G: return ImGuiKey_G;
    case SDLK_H: return ImGuiKey_H;
    case SDLK_I: return ImGuiKey_I;
    case SDLK_J: return ImGuiKey_J;
    case SDLK_K: return ImGuiKey_K;
    case SDLK_L: return ImGuiKey_L;
    case SDLK_M: return ImGuiKey_M;
    case SDLK_N: return ImGuiKey_N;
    case SDLK_O: return ImGuiKey_O;
    case SDLK_P: return ImGuiKey_P;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_R: return ImGuiKey_R;
    case SDLK_S: return ImGuiKey_S;
    case SDLK_T: return ImGuiKey_T;
    case SDLK_U: return ImGuiKey_U;
    case SDLK_V: return ImGuiKey_V;
    case SDLK_W: return ImGuiKey_W;
    case SDLK_X: return ImGuiKey_X;
    case SDLK_Y: return ImGuiKey_Y;
    case SDLK_Z: return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
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
    ctx_.renderToSwapchain = false;  // Editor 使用离屏渲染，不直接渲染到 swapchain
    registerComponentEditors();
    ctx_.beforePresentCallback = [this]() { editor_.submitImGuiDrawData(); };
    setupDemoScene();
    setupImGui();

    while (ctx_.scheduler.tick()) {
        beginImGuiFrame();

        const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        const float leftWidth = 200.0f;
        const float rightWidth = 250.0f;
        const float menuBarHeight = 24.0f;
        const float statsHeight = 60.0f;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, menuBarHeight));
        ImGui::Begin("##menubar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Exit")) {
                    game_.quit();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(0, menuBarHeight));
        ImGui::SetNextWindowSize(ImVec2(leftWidth, displaySize.y - menuBarHeight));
        ImGui::Begin("Hierarchy", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        drawHierarchy();
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(displaySize.x - rightWidth, menuBarHeight));
        ImGui::SetNextWindowSize(ImVec2(rightWidth, displaySize.y - menuBarHeight - statsHeight));
        ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        inspector_.draw(selectedEntity_, ctx_.world);
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(displaySize.x - rightWidth, displaySize.y - statsHeight));
        ImGui::SetNextWindowSize(ImVec2(rightWidth, statsHeight));
        ImGui::Begin("Stats", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        drawStats();
        ImGui::End();

        const float viewportWidth = displaySize.x - leftWidth - rightWidth;
        const float viewportHeight = displaySize.y - menuBarHeight;
        ImGui::SetNextWindowPos(ImVec2(leftWidth, menuBarHeight));
        ImGui::SetNextWindowSize(ImVec2(viewportWidth, viewportHeight));
        ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground);
        drawViewport();
        ImGui::End();

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

    auto* renderDevice = dynamic_cast<backend::SDLGPURenderDevice*>(&ctx_.renderDevice());
    IM_ASSERT(renderDevice != nullptr);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.DisplaySize = ImVec2(static_cast<float>(ctx_.window->width()), static_cast<float>(ctx_.window->height()));

    ImGui_ImplSDLGPU3_InitInfo initInfo{};
    initInfo.Device = renderDevice->gpuDevice();
    initInfo.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(
        renderDevice->gpuDevice(),
        static_cast<SDL_Window*>(ctx_.window->sdlWindow())
    );
    ImGui_ImplSDLGPU3_Init(&initInfo);

    imguiReady_ = true;
}

void EditorApplication::shutdownImGui() {
    if (!imguiReady_) {
        return;
    }
    ImGui_ImplSDLGPU3_Shutdown();
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
    ImGui_ImplSDLGPU3_NewFrame();
    syncInputToImGui();
    ImGui::NewFrame();
}

void EditorApplication::syncInputToImGui() {
    ImGuiIO& io = ImGui::GetIO();

    io.AddMousePosEvent(
        ctx_.inputState.pointerX() * static_cast<float>(ctx_.window->width()),
        ctx_.inputState.pointerY() * static_cast<float>(ctx_.window->height())
    );
    io.AddMouseButtonEvent(0, ctx_.inputState.pointerDown(0));

    for (const auto& event : ctx_.inputState.frameEvents()) {
        if (event.type == platform::InputRawEvent::Type::KEY_DOWN || event.type == platform::InputRawEvent::Type::KEY_UP) {
            const ImGuiKey key = mapSDLKey(event.keyCode);
            if (key != ImGuiKey_None) {
                io.AddKeyEvent(key, event.type == platform::InputRawEvent::Type::KEY_DOWN);
            }
        }
    }

    io.AddKeyEvent(static_cast<ImGuiKey>(ImGuiMod_Ctrl), ctx_.inputState.isKeyDown(SDLK_LCTRL) || ctx_.inputState.isKeyDown(SDLK_RCTRL));
    io.AddKeyEvent(static_cast<ImGuiKey>(ImGuiMod_Shift), ctx_.inputState.isKeyDown(SDLK_LSHIFT) || ctx_.inputState.isKeyDown(SDLK_RSHIFT));
    io.AddKeyEvent(static_cast<ImGuiKey>(ImGuiMod_Alt), ctx_.inputState.isKeyDown(SDLK_LALT) || ctx_.inputState.isKeyDown(SDLK_RALT));
    io.AddKeyEvent(static_cast<ImGuiKey>(ImGuiMod_Super), ctx_.inputState.isKeyDown(SDLK_LGUI) || ctx_.inputState.isKeyDown(SDLK_RGUI));
}

void EditorApplication::drawHierarchy() {
    auto& entityStorage = ctx_.world.storage<entt::entity>();
    for (const entt::entity entity : entityStorage) {
        const bool selected = entity == selectedEntity_;
        if (ImGui::Selectable(entityLabel(entity), selected)) {
            selectedEntity_ = entity;
        }
    }
}

void EditorApplication::drawViewport() {
    const ImVec2 viewportSize = ImGui::GetContentRegionAvail();
    if (viewportSize.x > 0.0f && viewportSize.y > 0.0f) {
        editor_.setEditorCamera(editorCameraX_, editorCameraY_, editorCameraZoom_);

        const TextureHandle preview = viewportRenderer_.render(
            static_cast<int>(viewportSize.x),
            static_cast<int>(viewportSize.y)
        );

        SDL_GPUTexture* previewTexture = viewportRenderer_.getTexture(preview);

        core::logInfo("drawViewport: previewTexture=%p", previewTexture);

        if (previewTexture != nullptr) {
            ImGui::Image(reinterpret_cast<ImTextureID>(previewTexture), viewportSize);
        } else {
            ImGui::Button("Scene preview", viewportSize);
        }
    }
}

void EditorApplication::drawStats() {
    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(ctx_.frameCounter));
    ImGui::Text("Delta: %.4f", ctx_.deltaTime);
    ImGui::Text("Entities: %u", static_cast<unsigned>(ctx_.world.storage<entt::entity>().size()));
}

void EditorApplication::endImGuiFrame() {
    if (!imguiReady_) {
        return;
    }
    ImGui::Render();
}

} // namespace editor