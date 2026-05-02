#include "GameAPI.h"
#include "../runtime/EngineContext.h"
#include "../components/RenderComponents.h"
#include "../components/UIComponents.h"
#include "../systems/SceneSystem.h"
#include "../systems/UISystem.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../backend/audio/AudioCommandQueue.h"
#include "../../backend/audio/IAudioDevice.h"
#include <SDL3/SDL.h>
#include <cstring>
#include <cstdio>
#include <algorithm>


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
    return ctx_.assetManager.loadSound(path ? path : "");
}

SoundHandle GameAPI::loadSoundById(const char* assetId) {
    return ctx_.assetManager.loadSoundById(assetId ? assetId : "");
}

void GameAPI::releaseSound(SoundHandle h) {
    ctx_.assetManager.releaseSound(h);
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
    return ctx_.systems.get<SceneSystem>().loadScene(path);
}

bool GameAPI::saveScene(const char* path) {
    return ctx_.systems.get<SceneSystem>().saveScene(path);
}

void GameAPI::unloadScene() {
    ctx_.systems.get<SceneSystem>().unloadScene();
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

bool GameAPI::loadAssetManifest(const char* path) {
    return ctx_.assetManager.loadManifest(path);
}

TextureHandle GameAPI::loadTextureById(const char* assetId) {
    return ctx_.assetManager.loadTextureById(assetId);
}

FontHandle GameAPI::loadFontById(const char* assetId) {
    return ctx_.assetManager.loadFontById(assetId);
}

AnimationHandle GameAPI::loadAnimation(const char* path) {
    return ctx_.assetManager.loadAnimation(path ? path : "");
}

AnimationHandle GameAPI::loadAnimationById(const char* assetId) {
    return ctx_.assetManager.loadAnimationById(assetId ? assetId : "");
}

void GameAPI::releaseAnimation(AnimationHandle h) {
    ctx_.assetManager.releaseAnimation(h);
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
// UI System v2
// ═══════════════════════════════════════════════════════════════════════════════

namespace {
inline void renameEntity(entt::registry& w, entt::entity e, const char* prefix) {
    if (!w.all_of<EntityID>(e)) return;
    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "%s_%08x", prefix, static_cast<uint32_t>(e));
    w.get<EntityID>(e) = EntityID(buf);
}
} // namespace

// ── Canvas ───────────────────────────────────────────────────────────────────

entt::entity GameAPI::createCanvas(int referenceW, int referenceH) {
    entt::entity e = ctx_.world.create();
    auto& c = ctx_.world.emplace<UICanvas>(e);
    c.referenceWidth  = referenceW;
    c.referenceHeight = referenceH;
    c.scaleMode = UICanvas::ScaleMode::ScaleWithScreen;

    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "canvas_%08x", static_cast<uint32_t>(e));
    ctx_.world.emplace<EntityID>(e, buf);
    return e;
}

void GameAPI::setCanvasScaleMode(entt::entity canvas, bool scaleWithScreen) {
    if (auto* c = ctx_.world.try_get<UICanvas>(canvas)) {
        c->scaleMode = scaleWithScreen ? UICanvas::ScaleMode::ScaleWithScreen
                                       : UICanvas::ScaleMode::Fixed;
    }
}

void GameAPI::setCanvasSafeArea(entt::entity canvas, float left, float top,
                                float right, float bottom) {
    if (auto* c = ctx_.world.try_get<UICanvas>(canvas)) {
        c->safeAreaLeft   = left;
        c->safeAreaTop    = top;
        c->safeAreaRight  = right;
        c->safeAreaBottom = bottom;
    }
}

// ── 通用节点 ────────────────────────────────────────────────────────────────

entt::entity GameAPI::createUIElement(entt::entity parent) {
    entt::entity e = ctx_.world.create();
    auto& n = ctx_.world.emplace<UINode>(e);
    n.parent = parent;

    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "ui_node_%08x", static_cast<uint32_t>(e));
    ctx_.world.emplace<EntityID>(e, buf);
    return e;
}

void GameAPI::setUIParent(entt::entity e, entt::entity parent) {
    if (auto* n = ctx_.world.try_get<UINode>(e)) n->parent = parent;
}

void GameAPI::setUISize(entt::entity e, float width, float height) {
    if (auto* n = ctx_.world.try_get<UINode>(e)) {
        n->width  = width;
        n->height = height;
    }
}

void GameAPI::setUIAnchor(entt::entity e, float minX, float minY,
                          float maxX, float maxY) {
    if (auto* n = ctx_.world.try_get<UINode>(e)) {
        n->anchor = {minX, minY, maxX, maxY};
    }
}

void GameAPI::setUIOffset(entt::entity e, float x, float y) {
    if (auto* n = ctx_.world.try_get<UINode>(e)) {
        n->offsetX = x;
        n->offsetY = y;
    }
}

void GameAPI::setUIPivot(entt::entity e, float x, float y) {
    if (auto* n = ctx_.world.try_get<UINode>(e)) {
        n->pivotX = x;
        n->pivotY = y;
    }
}

void GameAPI::setUIInteractable(entt::entity e, bool interactable) {
    if (auto* n = ctx_.world.try_get<UINode>(e)) n->interactable = interactable;
}

void GameAPI::setUIVisible(entt::entity e, bool visible) {
    if (auto* n = ctx_.world.try_get<UINode>(e)) n->visible = visible;
}

void GameAPI::setUISortOrder(entt::entity e, int order) {
    if (auto* n = ctx_.world.try_get<UINode>(e)) n->sortOrder = order;
}

void GameAPI::setUILayer(entt::entity e, int layer) {
    if (auto* n = ctx_.world.try_get<UINode>(e)) n->layer = layer;
}

void GameAPI::attachToWorld(entt::entity e, entt::entity target,
                            float offsetX, float offsetY) {
    if (!ctx_.world.all_of<UINode>(e)) return;
    auto& w = ctx_.world.get_or_emplace<UIWorldAnchor>(e);
    w.target  = target;
    w.offsetX = offsetX;
    w.offsetY = offsetY;
}

void GameAPI::detachFromWorld(entt::entity e) {
    if (ctx_.world.all_of<UIWorldAnchor>(e)) {
        ctx_.world.remove<UIWorldAnchor>(e);
    }
}

void GameAPI::setUIBackground(entt::entity e, const core::Color& color,
                              TextureHandle texture) {
    if (!ctx_.world.all_of<UINode>(e)) return;
    auto& bg = ctx_.world.get_or_emplace<UIBackground>(e);
    bg.color   = color;
    bg.texture = texture;
}

// ── Button ──────────────────────────────────────────────────────────────────

entt::entity GameAPI::createButton(float width, float height,
                                   std::function<void()> onClick) {
    entt::entity e = createUIElement();
    setUISize(e, width, height);
    auto& btn = ctx_.world.emplace<UIButton>(e);
    btn.onClick = std::move(onClick);
    
    renameEntity(ctx_.world, e, "button");
    return e;
}

void GameAPI::setButtonCallback(entt::entity e, std::function<void()> onClick) {
    if (auto* b = ctx_.world.try_get<UIButton>(e)) b->onClick = std::move(onClick);
}

void GameAPI::setButtonOnClick(entt::entity e, std::function<void()> onClick) {
    setButtonCallback(e, std::move(onClick));
}

void GameAPI::setButtonColors(entt::entity e,
                              const core::Color& normal,
                              const core::Color& hover,
                              const core::Color& pressed) {
    if (auto* b = ctx_.world.try_get<UIButton>(e)) {
        b->normal  = normal;
        b->hover   = hover;
        b->pressed = pressed;
    }
}

void GameAPI::setButtonEnabled(entt::entity e, bool enabled) {
    if (auto* b = ctx_.world.try_get<UIButton>(e)) b->isDisabled = !enabled;
}

// ── Toggle ──────────────────────────────────────────────────────────────────

entt::entity GameAPI::createToggle(float width, float height,
                                   std::function<void(bool)> onChanged) {
    entt::entity e = createUIElement();
    setUISize(e, width, height);
    auto& t = ctx_.world.emplace<UIToggle>(e);
    t.onChanged = std::move(onChanged);
    renameEntity(ctx_.world, e, "toggle");
    return e;
}

void GameAPI::setToggleValue(entt::entity e, bool isOn) {
    auto* t = ctx_.world.try_get<UIToggle>(e);
    if (!t) return;
    if (t->isOn != isOn) {
        t->isOn = isOn;
        if (t->onChanged) t->onChanged(isOn);
    }
}

bool GameAPI::getToggleValue(entt::entity e) const {
    auto* t = ctx_.world.try_get<UIToggle>(e);
    return t ? t->isOn : false;
}

void GameAPI::setToggleCallback(entt::entity e, std::function<void(bool)> onChanged) {
    if (auto* t = ctx_.world.try_get<UIToggle>(e)) t->onChanged = std::move(onChanged);
}

// ── Slider ──────────────────────────────────────────────────────────────────

entt::entity GameAPI::createSlider(float width, float height,
                                   float min, float max,
                                   std::function<void(float)> onChanged) {
    entt::entity e = createUIElement();
    setUISize(e, width, height);
    auto& s = ctx_.world.emplace<UISlider>(e);
    s.min = min;
    s.max = max;
    s.value = min;
    s.onChanged = std::move(onChanged);
    renameEntity(ctx_.world, e, "slider");
    return e;
}

void GameAPI::setSliderValue(entt::entity e, float value) {
    auto* s = ctx_.world.try_get<UISlider>(e);
    if (!s) return;
    if (value < s->min) value = s->min;
    if (value > s->max) value = s->max;
    if (s->value != value) {
        s->value = value;
        if (s->onChanged) s->onChanged(value);
    }
}

float GameAPI::getSliderValue(entt::entity e) const {
    auto* s = ctx_.world.try_get<UISlider>(e);
    return s ? s->value : 0.f;
}

void GameAPI::setSliderRange(entt::entity e, float min, float max) {
    auto* s = ctx_.world.try_get<UISlider>(e);
    if (!s) return;
    s->min = min;
    s->max = max;
    if (s->value < min) s->value = min;
    if (s->value > max) s->value = max;
}

void GameAPI::setSliderCallback(entt::entity e, std::function<void(float)> onChanged) {
    if (auto* s = ctx_.world.try_get<UISlider>(e)) s->onChanged = std::move(onChanged);
}

// ── Progress Bar ────────────────────────────────────────────────────────────

entt::entity GameAPI::createProgressBar(float width, float height) {
    entt::entity e = createUIElement();
    setUISize(e, width, height);
    ctx_.world.emplace<UIProgressBar>(e);
    if (auto* n = ctx_.world.try_get<UINode>(e)) n->interactable = false;
    renameEntity(ctx_.world, e, "progressbar");
    return e;
}

void GameAPI::setProgressValue(entt::entity e, float value) {
    if (auto* pb = ctx_.world.try_get<UIProgressBar>(e)) {
        if (value < 0.f) value = 0.f;
        if (value > 1.f) value = 1.f;
        pb->value = value;
    }
}

void GameAPI::setProgressColors(entt::entity e,
                                const core::Color& background,
                                const core::Color& fill) {
    if (auto* pb = ctx_.world.try_get<UIProgressBar>(e)) {
        pb->bgColor   = background;
        pb->fillColor = fill;
    }
}

// ── Image ───────────────────────────────────────────────────────────────────

entt::entity GameAPI::createUIImage(float width, float height, TextureHandle texture) {
    entt::entity e = createUIElement();
    setUISize(e, width, height);
    auto& img = ctx_.world.emplace<UIImage>(e);
    img.texture = texture;
    renameEntity(ctx_.world, e, "uiimage");
    return e;
}

void GameAPI::setUIImageTexture(entt::entity e, TextureHandle texture) {
    if (auto* img = ctx_.world.try_get<UIImage>(e)) img->texture = texture;
}

void GameAPI::setUIImageColor(entt::entity e, const core::Color& color) {
    if (auto* img = ctx_.world.try_get<UIImage>(e)) img->tint = color;
}

// ── Label / Text ────────────────────────────────────────────────────────────

entt::entity GameAPI::createUIText(float width, float height, const char* text) {
    entt::entity e = createUIElement();
    setUISize(e, width, height);
    auto& lbl = ctx_.world.emplace<UILabel>(e);
    if (text) lbl.text = text;
    if (auto* n = ctx_.world.try_get<UINode>(e)) n->interactable = false;
    renameEntity(ctx_.world, e, "uitext");
    return e;
}

void GameAPI::setUIText(entt::entity e, const char* text) {
    if (auto* lbl = ctx_.world.try_get<UILabel>(e)) lbl->text = text ? text : "";
}

void GameAPI::setUITextFont(entt::entity e, FontHandle font, float fontSize) {
    if (auto* lbl = ctx_.world.try_get<UILabel>(e)) {
        lbl->font     = font;
        lbl->fontSize = fontSize;
    }
}

void GameAPI::setUITextColor(entt::entity e, const core::Color& color) {
    if (auto* lbl = ctx_.world.try_get<UILabel>(e)) lbl->color = color;
}

void GameAPI::setUITextAlignment(entt::entity e, int halign) {
    if (auto* lbl = ctx_.world.try_get<UILabel>(e)) {
        switch (halign) {
            case 0:  lbl->halign = UILabel::HAlign::Left;   break;
            case 2:  lbl->halign = UILabel::HAlign::Right;  break;
            default: lbl->halign = UILabel::HAlign::Center; break;
        }
    }
}

// ── ScrollView ──────────────────────────────────────────────────────────────

entt::entity GameAPI::createScrollView(float width, float height, int dir) {
    entt::entity e = createUIElement();
    setUISize(e, width, height);
    auto& sv = ctx_.world.emplace<UIScrollView>(e);
    sv.direction = (dir == 1) ? UIScrollView::Direction::Horizontal
                  : (dir == 2) ? UIScrollView::Direction::Both
                               : UIScrollView::Direction::Vertical;
    renameEntity(ctx_.world, e, "scrollview");
    return e;
}

void GameAPI::setScrollContentSize(entt::entity e, float w, float h) {
    if (auto* sv = ctx_.world.try_get<UIScrollView>(e)) {
        sv->contentW = w;
        sv->contentH = h;
    }
}

void GameAPI::setScrollOffset(entt::entity e, float x, float y) {
    if (auto* sv = ctx_.world.try_get<UIScrollView>(e)) {
        sv->scrollX = x;
        sv->scrollY = y;
    }
}

void GameAPI::setScrollWheelSpeed(entt::entity e, float speed) {
    if (auto* sv = ctx_.world.try_get<UIScrollView>(e)) sv->wheelSpeed = speed;
}

void GameAPI::setScrollOnChanged(entt::entity e, std::function<void(float, float)> cb) {
    if (auto* sv = ctx_.world.try_get<UIScrollView>(e)) sv->onScroll = std::move(cb);
}

void GameAPI::getScrollOffset(entt::entity e, float* outX, float* outY) const {
    auto* sv = ctx_.world.try_get<UIScrollView>(e);
    if (outX) *outX = sv ? sv->scrollX : 0.f;
    if (outY) *outY = sv ? sv->scrollY : 0.f;
}

// ── ScrollBar ───────────────────────────────────────────────────────────────

entt::entity GameAPI::createScrollBar(entt::entity target, int orientation,
                                       float thickness, float length) {
    entt::entity e = createUIElement();
    auto& bar = ctx_.world.emplace<UIScrollBar>(e);
    bar.orientation = (orientation == 1) ? UIScrollBar::Orientation::Horizontal
                                         : UIScrollBar::Orientation::Vertical;
    bar.target = target;
    // 主轴 = length，交叉轴 = thickness
    if (bar.orientation == UIScrollBar::Orientation::Vertical) {
        setUISize(e, thickness, length);
    } else {
        setUISize(e, length, thickness);
    }
    renameEntity(ctx_.world, e, "scrollbar");
    return e;
}

void GameAPI::setScrollBarColors(entt::entity e,
                                  const core::Color& track,
                                  const core::Color& thumb,
                                  const core::Color& thumbHover,
                                  const core::Color& thumbPressed) {
    if (auto* bar = ctx_.world.try_get<UIScrollBar>(e)) {
        bar->trackColor   = track;
        bar->thumbColor   = thumb;
        bar->thumbHover   = thumbHover;
        bar->thumbPressed = thumbPressed;
    }
}

void GameAPI::setScrollBarMinThumbSize(entt::entity e, float size) {
    if (auto* bar = ctx_.world.try_get<UIScrollBar>(e)) bar->minThumbSize = size;
}

void GameAPI::setScrollBarTrackPadding(entt::entity e, float pad) {
    if (auto* bar = ctx_.world.try_get<UIScrollBar>(e)) bar->trackPadding = pad;
}

void GameAPI::setScrollBarAutoHide(entt::entity e, bool enabled) {
    if (auto* bar = ctx_.world.try_get<UIScrollBar>(e)) bar->autoHide = enabled;
}

// ── Mask / Clip ─────────────────────────────────────────────────────────────

void GameAPI::setUIMask(entt::entity e, bool enabled) {
    if (!ctx_.world.all_of<UINode>(e)) return;
    ctx_.world.get_or_emplace<UIMask>(e).enabled = enabled;
}

// ── Modal ───────────────────────────────────────────────────────────────────

void GameAPI::setUIModal(entt::entity e, bool enabled) {
    if (!ctx_.world.all_of<UINode>(e)) return;
    ctx_.world.get_or_emplace<UIModal>(e).enabled = enabled;
}

void GameAPI::setUIModalOverlay(entt::entity e, bool drawOverlay,
                                const core::Color& overlayColor) {
    if (!ctx_.world.all_of<UINode>(e)) return;
    auto& m = ctx_.world.get_or_emplace<UIModal>(e);
    m.drawOverlay  = drawOverlay;
    m.overlayColor = overlayColor;
}

void GameAPI::setUIModalOnClickOutside(entt::entity e, std::function<void()> cb) {
    if (!ctx_.world.all_of<UINode>(e)) return;
    ctx_.world.get_or_emplace<UIModal>(e).onClickOutside = std::move(cb);
}

// ── Tooltip ─────────────────────────────────────────────────────────────────

void GameAPI::setUITooltip(entt::entity e, const char* text,
                           FontHandle font, float fontSize, float delay) {
    if (!ctx_.world.all_of<UINode>(e)) return;
    auto& t = ctx_.world.get_or_emplace<UITooltip>(e);
    t.text     = text ? text : "";
    t.font     = font;
    t.fontSize = fontSize;
    t.delay    = delay;
}

void GameAPI::setUITooltipColors(entt::entity e,
                                 const core::Color& bg, const core::Color& text) {
    if (auto* t = ctx_.world.try_get<UITooltip>(e)) {
        t->bgColor   = bg;
        t->textColor = text;
    }
}

void GameAPI::clearUITooltip(entt::entity e) {
    if (ctx_.world.all_of<UITooltip>(e)) ctx_.world.remove<UITooltip>(e);
}

// ── TextInput ───────────────────────────────────────────────────────────────

entt::entity GameAPI::createTextInput(float width, float height) {
    entt::entity e = createUIElement();
    setUISize(e, width, height);
    ctx_.world.emplace<UITextInput>(e);
    // 不设置默认字体——调用方自行 setTextInputFont
    if (auto* n = ctx_.world.try_get<UINode>(e)) n->interactable = true;
    renameEntity(ctx_.world, e, "textinput");
    return e;
}

void GameAPI::setTextInputText(entt::entity e, const char* text) {
    if (auto* ti = ctx_.world.try_get<UITextInput>(e)) {
        if (!ti->readOnly) {
            ti->text = text ? text : "";
            ti->cursorPos = std::min(ti->cursorPos, ti->text.size());
            ti->selectionStart = std::string::npos;
        }
    }
}

const char* GameAPI::getTextInputText(entt::entity e) const {
    auto* ti = ctx_.world.try_get<UITextInput>(e);
    return ti ? ti->text.c_str() : "";
}

void GameAPI::setTextInputFont(entt::entity e, FontHandle font, float fontSize) {
    if (auto* ti = ctx_.world.try_get<UITextInput>(e)) {
        ti->font     = font;
        ti->fontSize = fontSize;
    }
}

void GameAPI::setTextInputPlaceholder(entt::entity e, const char* text) {
    if (auto* ti = ctx_.world.try_get<UITextInput>(e)) {
        ti->placeholder = text ? text : "";
    }
}

void GameAPI::setTextInputMaxLength(entt::entity e, uint32_t maxLen) {
    if (auto* ti = ctx_.world.try_get<UITextInput>(e)) {
        ti->maxLength = maxLen;
        if (ti->text.size() > maxLen) {
            ti->text.resize(maxLen);
            ti->cursorPos = std::min(ti->cursorPos, ti->text.size());
        }
    }
}

void GameAPI::setTextInputPasswordMode(entt::entity e, bool enabled) {
    if (auto* ti = ctx_.world.try_get<UITextInput>(e)) {
        ti->passwordMode = enabled;
    }
}

void GameAPI::setTextInputReadOnly(entt::entity e, bool readOnly) {
    if (auto* ti = ctx_.world.try_get<UITextInput>(e)) {
        ti->readOnly = readOnly;
    }
}

void GameAPI::setTextInputCallback(entt::entity e, std::function<void(const std::string&)> onChanged) {
    if (auto* ti = ctx_.world.try_get<UITextInput>(e)) {
        ti->onChanged = std::move(onChanged);
    }
}

void GameAPI::setTextInputSubmitCallback(entt::entity e, std::function<void(const std::string&)> onSubmitted) {
    if (auto* ti = ctx_.world.try_get<UITextInput>(e)) {
        ti->onSubmitted = std::move(onSubmitted);
    }
}

void GameAPI::setTextInputFocusCallbacks(entt::entity e,
                                          std::function<void()> onFocus,
                                          std::function<void()> onBlur) {
    if (auto* ti = ctx_.world.try_get<UITextInput>(e)) {
        ti->onFocus = std::move(onFocus);
        ti->onBlur  = std::move(onBlur);
    }
}

void GameAPI::focusTextInput(entt::entity e) {
    if (auto* ti = ctx_.world.try_get<UITextInput>(e)) {
        if (!ti->focused) {
            // 先让其他输入框失焦
            auto tiv = ctx_.world.view<UITextInput>();
            for (auto [other, oti] : tiv.each()) {
                if (&oti != ti && oti.focused) {
                    oti.focused = false;
                    oti.caretTimer = 0.f;
                    oti.selectionStart = std::string::npos;
                    if (oti.onBlur) oti.onBlur();
                }
            }
            ti->focused = true;
            ti->caretTimer = 0.f;
            ti->selectionStart = std::string::npos;
            if (ti->onFocus) ti->onFocus();
        }
    }
}

void GameAPI::blurTextInput(entt::entity e) {
    if (auto* ti = ctx_.world.try_get<UITextInput>(e)) {
        if (ti->focused) {
            ti->focused = false;
            ti->caretTimer = 0.f;
            ti->selectionStart = std::string::npos;
            if (ti->onBlur) ti->onBlur();
        }
    }
}

bool GameAPI::isTextInputFocused(entt::entity e) const {
    auto* ti = ctx_.world.try_get<UITextInput>(e);
    return ti ? ti->focused : false;
}

// ── NineSlice ───────────────────────────────────────────────────────────────

void GameAPI::setUINineSlice(entt::entity e, TextureHandle texture,
                             float borderL, float borderT,
                             float borderR, float borderB,
                             const core::Color& tint) {
    if (!ctx_.world.all_of<UINode>(e)) return;
    auto& ns = ctx_.world.get_or_emplace<UINineSlice>(e);
    ns.texture      = texture;
    ns.borderLeft   = borderL;
    ns.borderTop    = borderT;
    ns.borderRight  = borderR;
    ns.borderBottom = borderB;
    ns.tint         = tint;
}

void GameAPI::setUINineSliceSrcRect(entt::entity e, float x, float y, float w, float h) {
    if (auto* ns = ctx_.world.try_get<UINineSlice>(e)) {
        ns->srcRect = core::Rect{x, y, w, h};
    }
}

void GameAPI::setUINineSliceFillCenter(entt::entity e, bool fillCenter) {
    if (auto* ns = ctx_.world.try_get<UINineSlice>(e)) ns->fillCenter = fillCenter;
}

// ── LayoutGroup ─────────────────────────────────────────────────────────────

entt::entity GameAPI::createLayoutGroup(float width, float height, int type) {
    entt::entity e = createUIElement();
    setUISize(e, width, height);
    auto& g = ctx_.world.emplace<UILayoutGroup>(e);
    g.type = (type == 0) ? UILayoutGroup::Type::Horizontal
           : (type == 2) ? UILayoutGroup::Type::Grid
                         : UILayoutGroup::Type::Vertical;
    renameEntity(ctx_.world, e, "layout");
    return e;
}

void GameAPI::setLayoutPadding(entt::entity e, float left, float top,
                               float right, float bottom) {
    if (auto* g = ctx_.world.try_get<UILayoutGroup>(e)) {
        g->paddingL = left;  g->paddingT = top;
        g->paddingR = right; g->paddingB = bottom;
    }
}

void GameAPI::setLayoutSpacing(entt::entity e, float spacingX, float spacingY) {
    if (auto* g = ctx_.world.try_get<UILayoutGroup>(e)) {
        g->spacingX = spacingX;
        g->spacingY = spacingY;
    }
}

void GameAPI::setLayoutGridColumns(entt::entity e, int columns) {
    if (auto* g = ctx_.world.try_get<UILayoutGroup>(e)) {
        g->gridColumns = std::max(1, columns);
    }
}

void GameAPI::setLayoutCrossAlign(entt::entity e, int align) {
    if (auto* g = ctx_.world.try_get<UILayoutGroup>(e)) {
        switch (align) {
            case 1:  g->crossAlign = UILayoutGroup::CrossAlign::Center;  break;
            case 2:  g->crossAlign = UILayoutGroup::CrossAlign::End;     break;
            case 3:  g->crossAlign = UILayoutGroup::CrossAlign::Stretch; break;
            default: g->crossAlign = UILayoutGroup::CrossAlign::Start;   break;
        }
    }
}

void GameAPI::setLayoutReverse(entt::entity e, bool reverse) {
    if (auto* g = ctx_.world.try_get<UILayoutGroup>(e)) g->reverse = reverse;
}

// ── Draggable ───────────────────────────────────────────────────────────────

void GameAPI::makeDraggable(entt::entity e,
                            std::function<void(float, float)> onDrag) {
    auto* n = ctx_.world.try_get<UINode>(e);
    if (!n) return;
    n->anchor = UIAnchor::topLeft();
    n->pivotX = 0.f;
    n->pivotY = 0.f;
    auto& d = ctx_.world.get_or_emplace<UIDraggable>(e);
    d.onDrag = std::move(onDrag);
}

void GameAPI::setDragBounds(entt::entity e, float minX, float minY,
                            float maxX, float maxY) {
    auto& d = ctx_.world.get_or_emplace<UIDraggable>(e);
    d.clamp = true;
    d.minX = minX;
    d.minY = minY;
    d.maxX = maxX;
    d.maxY = maxY;
}

void GameAPI::setDragPayload(entt::entity e, std::string payload) {
    auto& d = ctx_.world.get_or_emplace<UIDraggable>(e);
    d.payload = std::move(payload);
}

void GameAPI::setDragSnapBack(entt::entity e, bool snapBack) {
    auto& d = ctx_.world.get_or_emplace<UIDraggable>(e);
    d.snapBackOnMiss = snapBack;
}

// ── DropTarget ──────────────────────────────────────────────────────────────

void GameAPI::makeDropTarget(entt::entity e,
                             std::function<void(entt::entity)> onDrop) {
    auto* n = ctx_.world.try_get<UINode>(e);
    if (!n) return;
    auto& dt = ctx_.world.get_or_emplace<UIDropTarget>(e);
    dt.onDrop = std::move(onDrop);
}

void GameAPI::setDropAcceptedPayload(entt::entity e, std::string payload) {
    auto& dt = ctx_.world.get_or_emplace<UIDropTarget>(e);
    dt.acceptedPayload = std::move(payload);
}

void GameAPI::setDropHoverHighlight(entt::entity e, bool enabled, core::Color color) {
    auto& dt = ctx_.world.get_or_emplace<UIDropTarget>(e);
    dt.highlightOnHover = enabled;
    dt.highlightColor   = color;
}

void GameAPI::setDropAcceptCallback(entt::entity e,
                                    std::function<bool(entt::entity)> canAccept) {
    auto& dt = ctx_.world.get_or_emplace<UIDropTarget>(e);
    dt.canAccept = std::move(canAccept);
}

void GameAPI::setDropEnterCallback(entt::entity e,
                                   std::function<void(entt::entity)> onEnter) {
    auto& dt = ctx_.world.get_or_emplace<UIDropTarget>(e);
    dt.onDragEnter = std::move(onEnter);
}

void GameAPI::setDropLeaveCallback(entt::entity e,
                                   std::function<void(entt::entity)> onLeave) {
    auto& dt = ctx_.world.get_or_emplace<UIDropTarget>(e);
    dt.onDragLeave = std::move(onLeave);
}

// ── 状态查询 ─────────────────────────────────────────────────────────────────

bool GameAPI::isPointerOverUI(entt::entity e) const {
    auto* n = ctx_.world.try_get<UINode>(e);
    if (!n) return false;
    const float px = ctx_.inputState.pointerX(0);
    const float py = ctx_.inputState.pointerY(0);
    return px >= n->screenX && px < n->screenX + n->screenW &&
           py >= n->screenY && py < n->screenY + n->screenH;
}

entt::entity GameAPI::getHoveredUI() const {
    if (ctx_.systems.has<UISystem>()) {
        return ctx_.systems.get<UISystem>().hovered();
    }
    return entt::null;
}

entt::entity GameAPI::getPressedUI() const {
    if (ctx_.systems.has<UISystem>()) {
        return ctx_.systems.get<UISystem>().pressed();
    }
    return entt::null;
}

void GameAPI::getUIComputedRect(entt::entity e, float* outX, float* outY,
                                float* outW, float* outH) const {
    auto* n = ctx_.world.try_get<UINode>(e);
    if (!n) {
        if (outX) *outX = 0.f;
        if (outY) *outY = 0.f;
        if (outW) *outW = 0.f;
        if (outH) *outH = 0.f;
        return;
    }
    if (outX) *outX = n->screenX;
    if (outY) *outY = n->screenY;
    if (outW) *outW = n->screenW;
    if (outH) *outH = n->screenH;
}

} // namespace engine
