#include "GameAPI.h"
#include "../runtime/EngineContext.h"
#include "../scene/SceneSerializer.h"
#include "../components/RenderComponents.h"
#include "../components/UIComponents.h"
#include "../systems/UISystem.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../backend/audio/AudioCommandQueue.h"
#include "../../backend/audio/IAudioDevice.h"
#include <SDL3/SDL.h>
#include <cstring>
#include <cstdio>


namespace engine {

entt::entity GameAPI::spawnEntity() {
    entt::entity e = ctx_.world.create();
    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "entity_%08x", static_cast<uint32_t>(e));
    ctx_.world.emplace<EntityID>(e, buf);
    return e;
}

void GameAPI::destroyEntity(entt::entity e) {
    ctx_.world.destroy(e);
}

// ── Audio ────────────────────────────────────────────────────────────────────

SoundHandle GameAPI::loadSound(const char* path) {
    return ctx_.audioDevice().loadSound(path);
}

void GameAPI::playSound(SoundHandle h, float vol) {
    backend::AudioCmd cmd{};
    cmd.type   = backend::AudioCmd::Type::PLAY;
    cmd.handle = h;
    cmd.vol    = vol;
    ctx_.audioCommandQueue().push(cmd);
}

void GameAPI::stopSound(SoundHandle h) {
    backend::AudioCmd cmd{};
    cmd.type   = backend::AudioCmd::Type::STOP;
    cmd.handle = h;
    ctx_.audioCommandQueue().push(cmd);
}

void GameAPI::playMusic(const char* path, bool loop) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::PLAY_STREAM;
    cmd.loop = loop;
    std::strncpy(cmd.path, path, sizeof(cmd.path) - 1);
    ctx_.audioCommandQueue().push(cmd);
}

void GameAPI::stopMusic() {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::STOP_STREAM;
    ctx_.audioCommandQueue().push(cmd);
}

void GameAPI::setSpatialListener(float x, float y) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::SET_LISTENER;
    cmd.x    = x;
    cmd.y    = y;
    ctx_.audioCommandQueue().push(cmd);
}

// ── Input ────────────────────────────────────────────────────────────────────

bool  GameAPI::isKeyDown(int k)         const { return ctx_.inputState.isKeyDown(k); }
bool  GameAPI::isKeyJustPressed(int k)  const { return ctx_.inputState.isKeyJustPressed(k); }
bool  GameAPI::isKeyJustReleased(int k) const { return ctx_.inputState.isKeyJustReleased(k); }
bool  GameAPI::pointerDown(int id)      const { return ctx_.inputState.pointerDown(id); }
float GameAPI::pointerX(int id)         const { return ctx_.inputState.pointerX(id); }
float GameAPI::pointerY(int id)         const { return ctx_.inputState.pointerY(id); }

// ── Physics ──────────────────────────────────────────────────────────────────

void GameAPI::setGravity(float x, float y) {
    ctx_.systems.get<PhysicsSystem>().setGravity(x, y);
}

void GameAPI::setFixedTimestep(float step) {
    ctx_.systems.get<PhysicsSystem>().setFixedTimestep(step);
}

RaycastHit GameAPI::raycast(float startX, float startY, float dirX, float dirY, 
                            float maxDist, CollisionLayer layerMask) {
    return ctx_.systems.get<PhysicsSystem>().raycast(startX, startY, dirX, dirY, maxDist, layerMask);
}

std::vector<OverlapResult> GameAPI::overlapBox(float centerX, float centerY,
                                               float halfW, float halfH,
                                               CollisionLayer layerMask) {
    return ctx_.systems.get<PhysicsSystem>().overlapBox(centerX, centerY, halfW, halfH, layerMask);
}

std::vector<entt::entity> GameAPI::overlapCircle(float centerX, float centerY, float radius,
                                                  CollisionLayer layerMask) {
    return ctx_.systems.get<PhysicsSystem>().overlapCircle(centerX, centerY, radius, layerMask);
}

// ── Scene ────────────────────────────────────────────────────────────────────

bool GameAPI::loadScene(const char* path) {
    return SceneSerializer::loadScene(ctx_.world, ctx_.assetManager, path);
}

bool GameAPI::saveScene(const char* path) {
    return SceneSerializer::saveScene(ctx_.world, ctx_.assetManager, path);
}

void GameAPI::unloadScene() {
    ctx_.world.clear();
}

// ── Asset ─────────────────────────────────────────────────────────────────────

TextureHandle GameAPI::loadTexture(const char* path) {
    return ctx_.assetManager.loadTexture(path);
}

void GameAPI::releaseTexture(TextureHandle h) {
    ctx_.assetManager.releaseTexture(h);
}

FontHandle GameAPI::loadFont(const char* path) {
    return ctx_.assetManager.loadFont(path);
}

void GameAPI::releaseFont(FontHandle h) {
    ctx_.assetManager.releaseFont(h);
}

AssetManager& GameAPI::assetManager() {
    return ctx_.assetManager;
}

TextureHandle GameAPI::createTextureFromMemory(const void* rgbaPixels, int w, int h) {
    backend::TextureDesc desc{};
    desc.data   = rgbaPixels;
    desc.width  = w;
    desc.height = h;
    return ctx_.renderDevice().createTexture(desc);
}

AnimationHandle GameAPI::createAnimation(const char* name, const engine::AnimationClip& clip) {
    return ctx_.assetManager.registerAnimation(name, clip);
}

void GameAPI::quit() {
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Time API
// ═══════════════════════════════════════════════════════════════════════════════

float GameAPI::getDeltaTime() const {
    return ctx_.deltaTime;
}

float GameAPI::getTimeScale() const {
    return ctx_.timeScale;
}

void GameAPI::setTimeScale(float scale) {
    ctx_.timeScale = scale;
}

// ═══════════════════════════════════════════════════════════════════════════════
// UI System - Canvas
// ═══════════════════════════════════════════════════════════════════════════════

entt::entity GameAPI::createCanvas(int referenceW, int referenceH) {
    entt::entity e = ctx_.world.create();
    
    // 添加 Canvas 组件
    Canvas& canvas = ctx_.world.emplace<Canvas>(e);
    canvas.referenceWidth  = referenceW;
    canvas.referenceHeight = referenceH;
    canvas.scaleMode = Canvas::ScaleMode::ScaleWithScreenSize;
    
    // 添加 EntityID
    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "canvas_%08x", static_cast<uint32_t>(e));
    ctx_.world.emplace<EntityID>(e, buf);
    
    return e;
}

void GameAPI::setCanvasScaleMode(entt::entity canvas, bool scaleWithScreen) {
    if (!ctx_.world.all_of<Canvas>(canvas)) return;
    
    auto& c = ctx_.world.get<Canvas>(canvas);
    c.scaleMode = scaleWithScreen ? Canvas::ScaleMode::ScaleWithScreenSize 
                                  : Canvas::ScaleMode::ConstantPixelSize;
}

void GameAPI::setCanvasSafeArea(entt::entity canvas, float left, float top, 
                                float right, float bottom) {
    if (!ctx_.world.all_of<Canvas>(canvas)) return;
    
    auto& c = ctx_.world.get<Canvas>(canvas);
    c.safeAreaLeft   = left;
    c.safeAreaTop    = top;
    c.safeAreaRight  = right;
    c.safeAreaBottom = bottom;
}

// ═══════════════════════════════════════════════════════════════════════════════
// UI System - Element
// ═══════════════════════════════════════════════════════════════════════════════

entt::entity GameAPI::createUIElement(entt::entity parent) {
    entt::entity e = ctx_.world.create();
    
    // 添加基础 UIElement 组件
    ctx_.world.emplace<UIElement>(e);
    
    // 添加 Transform (用于渲染系统)
    ctx_.world.emplace<Transform>(e);
    
    // 添加 EntityID
    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "ui_element_%08x", static_cast<uint32_t>(e));
    ctx_.world.emplace<EntityID>(e, buf);
    
    // 如果有父节点，建立层级关系
    if (parent != entt::null) {
        ctx_.world.emplace<UIParent>(e, parent);
        
        // 更新父节点的 children 列表
        if (!ctx_.world.all_of<UIChildren>(parent)) {
            ctx_.world.emplace<UIChildren>(parent);
        }
        ctx_.world.get<UIChildren>(parent).children.push_back(e);
    }
    
    return e;
}

void GameAPI::setUISize(entt::entity e, float width, float height) {
    if (!ctx_.world.all_of<UIElement>(e)) return;
    
    auto& elem = ctx_.world.get<UIElement>(e);
    elem.width  = width;
    elem.height = height;
}

void GameAPI::setUIAnchor(entt::entity e, float minX, float minY, float maxX, float maxY) {
    if (!ctx_.world.all_of<UIElement>(e)) return;
    
    auto& elem = ctx_.world.get<UIElement>(e);
    elem.anchor.minX = minX;
    elem.anchor.minY = minY;
    elem.anchor.maxX = maxX;
    elem.anchor.maxY = maxY;
}

void GameAPI::setUIOffset(entt::entity e, float left, float top, float right, float bottom) {
    if (!ctx_.world.all_of<UIElement>(e)) return;
    
    auto& elem = ctx_.world.get<UIElement>(e);
    elem.offsetLeft   = left;
    elem.offsetTop    = top;
    elem.offsetRight  = right;
    elem.offsetBottom = bottom;
}

void GameAPI::setUIPivot(entt::entity e, float x, float y) {
    if (!ctx_.world.all_of<UIElement>(e)) return;
    
    auto& elem = ctx_.world.get<UIElement>(e);
    elem.pivotX = x;
    elem.pivotY = y;
}

void GameAPI::setUIInteractable(entt::entity e, bool interactable) {
    if (!ctx_.world.all_of<UIElement>(e)) return;
    
    ctx_.world.get<UIElement>(e).interactable = interactable;
}

void GameAPI::setUISortOrder(entt::entity e, int order) {
    if (!ctx_.world.all_of<UIElement>(e)) return;
    
    ctx_.world.get<UIElement>(e).sortOrder = order;
}

// ═══════════════════════════════════════════════════════════════════════════════
// UI System - Button
// ═══════════════════════════════════════════════════════════════════════════════

entt::entity GameAPI::createButton(float width, float height, 
                                   std::function<void()> onClick) {
    entt::entity e = createUIElement();
    
    setUISize(e, width, height);
    
    // 添加 Button 组件
    Button& btn = ctx_.world.emplace<Button>(e);
    btn.onClick = std::move(onClick);
    
    // 添加 Sprite 用于渲染
    Sprite& spr = ctx_.world.emplace<Sprite>(e);
    spr.tint = btn.normalColor;
    spr.pass = RenderPass::Screen;  // UI 使用 Screen 空间
    
    // 更新 EntityID 名称
    auto& id = ctx_.world.get<EntityID>(e);
    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "button_%08x", static_cast<uint32_t>(e));
    id = EntityID(buf);
    
    return e;
}

void GameAPI::setButtonCallback(entt::entity e, std::function<void()> onClick) {
    if (!ctx_.world.all_of<Button>(e)) return;
    
    ctx_.world.get<Button>(e).onClick = std::move(onClick);
}

void GameAPI::setButtonColors(entt::entity e, 
                              const core::Color& normal,
                              const core::Color& hover,
                              const core::Color& pressed) {
    if (!ctx_.world.all_of<Button>(e)) return;
    
    auto& btn = ctx_.world.get<Button>(e);
    btn.normalColor  = normal;
    btn.hoverColor   = hover;
    btn.pressedColor = pressed;
}

void GameAPI::setButtonEnabled(entt::entity e, bool enabled) {
    if (!ctx_.world.all_of<Button>(e)) return;
    
    ctx_.world.get<Button>(e).disabled = !enabled;
}

// ═══════════════════════════════════════════════════════════════════════════════
// UI System - Toggle
// ═══════════════════════════════════════════════════════════════════════════════

entt::entity GameAPI::createToggle(float width, float height,
                                   std::function<void(bool)> onValueChanged) {
    entt::entity e = createUIElement();
    
    setUISize(e, width, height);
    
    // 添加 Toggle 组件
    Toggle& toggle = ctx_.world.emplace<Toggle>(e);
    toggle.onValueChanged = std::move(onValueChanged);
    
    // 添加 Sprite 用于渲染
    ctx_.world.emplace<Sprite>(e).pass = RenderPass::Screen;
    
    // 更新 EntityID 名称
    auto& id = ctx_.world.get<EntityID>(e);
    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "toggle_%08x", static_cast<uint32_t>(e));
    id = EntityID(buf);
    
    return e;
}

void GameAPI::setToggleValue(entt::entity e, bool isOn) {
    if (!ctx_.world.all_of<Toggle>(e)) return;
    
    auto& toggle = ctx_.world.get<Toggle>(e);
    if (toggle.isOn != isOn) {
        toggle.isOn = isOn;
        if (toggle.onValueChanged) {
            toggle.onValueChanged(isOn);
        }
    }
}

bool GameAPI::getToggleValue(entt::entity e) const {
    if (!ctx_.world.all_of<Toggle>(e)) return false;
    return ctx_.world.get<Toggle>(e).isOn;
}

void GameAPI::setToggleCallback(entt::entity e, std::function<void(bool)> onValueChanged) {
    if (!ctx_.world.all_of<Toggle>(e)) return;
    
    ctx_.world.get<Toggle>(e).onValueChanged = std::move(onValueChanged);
}

// ═══════════════════════════════════════════════════════════════════════════════
// UI System - Slider
// ═══════════════════════════════════════════════════════════════════════════════

entt::entity GameAPI::createSlider(float width, float height,
                                   float min, float max,
                                   std::function<void(float)> onValueChanged) {
    entt::entity e = createUIElement();
    
    setUISize(e, width, height);
    
    // 添加 Slider 组件
    Slider& slider = ctx_.world.emplace<Slider>(e);
    slider.min = min;
    slider.max = max;
    slider.value = min;
    slider.onValueChanged = std::move(onValueChanged);
    
    // 更新 EntityID 名称
    auto& id = ctx_.world.get<EntityID>(e);
    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "slider_%08x", static_cast<uint32_t>(e));
    id = EntityID(buf);
    
    return e;
}

void GameAPI::setSliderValue(entt::entity e, float value) {
    if (!ctx_.world.all_of<Slider>(e)) return;
    
    auto& slider = ctx_.world.get<Slider>(e);
    value = std::clamp(value, slider.min, slider.max);
    
    if (slider.value != value) {
        slider.value = value;
        if (slider.onValueChanged) {
            slider.onValueChanged(value);
        }
    }
}

float GameAPI::getSliderValue(entt::entity e) const {
    if (!ctx_.world.all_of<Slider>(e)) return 0.f;
    return ctx_.world.get<Slider>(e).value;
}

void GameAPI::setSliderRange(entt::entity e, float min, float max) {
    if (!ctx_.world.all_of<Slider>(e)) return;
    
    auto& slider = ctx_.world.get<Slider>(e);
    slider.min = min;
    slider.max = max;
    slider.value = std::clamp(slider.value, min, max);
}

void GameAPI::setSliderCallback(entt::entity e, std::function<void(float)> onValueChanged) {
    if (!ctx_.world.all_of<Slider>(e)) return;
    
    ctx_.world.get<Slider>(e).onValueChanged = std::move(onValueChanged);
}

// ═══════════════════════════════════════════════════════════════════════════════
// UI System - ProgressBar
// ═══════════════════════════════════════════════════════════════════════════════

entt::entity GameAPI::createProgressBar(float width, float height) {
    entt::entity e = createUIElement();
    
    setUISize(e, width, height);
    
    // 添加 ProgressBar 组件
    ctx_.world.emplace<ProgressBar>(e);
    
    // 更新 EntityID 名称
    auto& id = ctx_.world.get<EntityID>(e);
    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "progressbar_%08x", static_cast<uint32_t>(e));
    id = EntityID(buf);
    
    return e;
}

void GameAPI::setProgressValue(entt::entity e, float value) {
    if (!ctx_.world.all_of<ProgressBar>(e)) return;
    
    ctx_.world.get<ProgressBar>(e).value = std::clamp(value, 0.f, 1.f);
}

void GameAPI::setProgressColors(entt::entity e, 
                                const core::Color& background,
                                const core::Color& fill) {
    if (!ctx_.world.all_of<ProgressBar>(e)) return;
    
    auto& pb = ctx_.world.get<ProgressBar>(e);
    pb.backgroundColor = background;
    pb.fillColor = fill;
}

// ═══════════════════════════════════════════════════════════════════════════════
// UI System - Image
// ═══════════════════════════════════════════════════════════════════════════════

entt::entity GameAPI::createUIImage(float width, float height, TextureHandle texture) {
    entt::entity e = createUIElement();
    
    setUISize(e, width, height);
    
    // 添加 UIImage 组件
    UIImage& img = ctx_.world.emplace<UIImage>(e);
    img.texture = texture;
    
    // 添加 Sprite 用于渲染
    Sprite& spr = ctx_.world.emplace<Sprite>(e);
    spr.texture = texture;
    spr.pass = RenderPass::Screen;
    
    // 更新 EntityID 名称
    auto& id = ctx_.world.get<EntityID>(e);
    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "uiimage_%08x", static_cast<uint32_t>(e));
    id = EntityID(buf);
    
    return e;
}

void GameAPI::setUIImageTexture(entt::entity e, TextureHandle texture) {
    if (!ctx_.world.all_of<UIImage>(e)) return;
    
    ctx_.world.get<UIImage>(e).texture = texture;
    
    // 同步到 Sprite
    if (ctx_.world.all_of<Sprite>(e)) {
        ctx_.world.get<Sprite>(e).texture = texture;
    }
}

void GameAPI::setUIImageColor(entt::entity e, const core::Color& color) {
    if (!ctx_.world.all_of<UIImage>(e)) return;
    
    ctx_.world.get<UIImage>(e).color = color;
    
    // 同步到 Sprite
    if (ctx_.world.all_of<Sprite>(e)) {
        ctx_.world.get<Sprite>(e).tint = color;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// UI System - Text
// ═══════════════════════════════════════════════════════════════════════════════

entt::entity GameAPI::createUIText(float width, float height, const char* text) {
    entt::entity e = createUIElement();
    
    setUISize(e, width, height);
    
    // 添加 UIText 组件
    UIText& uiText = ctx_.world.emplace<UIText>(e);
    if (text) uiText.text = text;
    
    // 添加 TextComponent 用于渲染
    TextComponent& tc = ctx_.world.emplace<TextComponent>(e);
    tc.text = text ? text : "";
    tc.pass = RenderPass::Screen;
    
    // 更新 EntityID 名称
    auto& id = ctx_.world.get<EntityID>(e);
    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "uitext_%08x", static_cast<uint32_t>(e));
    id = EntityID(buf);
    
    return e;
}

void GameAPI::setUIText(entt::entity e, const char* text) {
    if (!ctx_.world.all_of<UIText>(e)) return;
    
    ctx_.world.get<UIText>(e).text = text ? text : "";
    
    // 同步到 TextComponent
    if (ctx_.world.all_of<TextComponent>(e)) {
        ctx_.world.get<TextComponent>(e).text = text ? text : "";
    }
}

void GameAPI::setUITextFont(entt::entity e, FontHandle font, float fontSize) {
    if (!ctx_.world.all_of<UIText>(e)) return;
    
    auto& uiText = ctx_.world.get<UIText>(e);
    uiText.font = font;
    uiText.fontSize = fontSize;
    
    // 同步到 TextComponent
    if (ctx_.world.all_of<TextComponent>(e)) {
        auto& tc = ctx_.world.get<TextComponent>(e);
        tc.font = font;
        tc.fontSize = fontSize;
    }
}

void GameAPI::setUITextColor(entt::entity e, const core::Color& color) {
    if (!ctx_.world.all_of<UIText>(e)) return;
    
    ctx_.world.get<UIText>(e).color = color;
    
    // 同步到 TextComponent
    if (ctx_.world.all_of<TextComponent>(e)) {
        ctx_.world.get<TextComponent>(e).color = color;
    }
}

void GameAPI::setUITextAlignment(entt::entity e, int alignment) {
    if (!ctx_.world.all_of<UIText>(e)) return;
    
    auto& uiText = ctx_.world.get<UIText>(e);
    uiText.alignment = static_cast<UIText::Alignment>(alignment);
}

// ═══════════════════════════════════════════════════════════════════════════════
// UI System - Drag
// ═══════════════════════════════════════════════════════════════════════════════

void GameAPI::makeDraggable(entt::entity e, std::function<void(float x, float y)> onDrag) {
    if (!ctx_.world.all_of<UIElement>(e)) return;
    
    // 添加 DragHandler 组件
    DragHandler& drag = ctx_.world.emplace<DragHandler>(e);
    drag.onDrag = std::move(onDrag);
}

void GameAPI::setDragBounds(entt::entity e, float minX, float minY, float maxX, float maxY) {
    if (!ctx_.world.all_of<DragHandler>(e)) return;
    
    auto& drag = ctx_.world.get<DragHandler>(e);
    drag.minX = minX;
    drag.minY = minY;
    drag.maxX = maxX;
    drag.maxY = maxY;
}

// ═══════════════════════════════════════════════════════════════════════════════
// UI System - State Query
// ═══════════════════════════════════════════════════════════════════════════════

bool GameAPI::isPointerOverUI(entt::entity e) const {
    if (!ctx_.world.all_of<UIElement>(e)) return false;
    
    const auto& elem = ctx_.world.get<UIElement>(e);
    float px = ctx_.inputState.pointerX(0);
    float py = ctx_.inputState.pointerY(0);
    
    return px >= elem.computedX && px <= elem.computedX + elem.computedW &&
           py >= elem.computedY && py <= elem.computedY + elem.computedH;
}

entt::entity GameAPI::getHoveredUI() const {
    if (ctx_.systems.has<UISystem>()) {
        return ctx_.systems.get<UISystem>().getHoveredElement();
    }
    return entt::null;
}

entt::entity GameAPI::getPressedUI() const {
    if (ctx_.systems.has<UISystem>()) {
        return ctx_.systems.get<UISystem>().getPressedElement();
    }
    return entt::null;
}

void GameAPI::getUIComputedRect(entt::entity e, float* outX, float* outY,
                                float* outW, float* outH) const {
    if (!ctx_.world.all_of<UIElement>(e)) {
        if (outX) *outX = 0.f;
        if (outY) *outY = 0.f;
        if (outW) *outW = 0.f;
        if (outH) *outH = 0.f;
        return;
    }
    
    const auto& elem = ctx_.world.get<UIElement>(e);
    if (outX) *outX = elem.computedX;
    if (outY) *outY = elem.computedY;
    if (outW) *outW = elem.computedW;
    if (outH) *outH = elem.computedH;
}

} // namespace engine
