#include "UISystem.h"

#include "../runtime/EngineContext.h"
#include "../input/InputState.h"
#include "../components/RenderComponents.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../core/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace engine {

UISystem::UISystem(EngineContext& ctx) : ctx_(ctx) {}
UISystem::~UISystem() = default;

void UISystem::init() {
    // 注：renderDevice 在 EngineContext::init 之后才可用，此处不主动建白纹理。
    // 第一次 update() 里再 ensureWhiteTexture()。
}

void UISystem::shutdown() {
    if (whiteTexture_.valid()) {
        ctx_.renderDevice().destroyTexture(whiteTexture_);
        whiteTexture_ = TextureHandle{};
    }
}

void UISystem::ensureWhiteTexture() {
    if (whiteTexture_.valid()) return;
    backend::TextureDesc desc{};
    desc.width  = 1;
    desc.height = 1;
    desc.channels = 4;
    static const uint8_t kWhite[4] = {255, 255, 255, 255};
    desc.data = kWhite;
    whiteTexture_ = ctx_.renderDevice().createTexture(desc);
}

// ───────────────────────────────────────────────────────────────────────────────

void UISystem::update(float /*dt*/) {
    ensureWhiteTexture();

    if (ctx_.window) {
        screenW_ = static_cast<float>(ctx_.window->width());
        screenH_ = static_cast<float>(ctx_.window->height());
    }

    updateCanvases();
    runLayout();
    runInteraction(ctx_.inputState);
    buildCommands();
}

void UISystem::updateCanvases() {
    auto view = ctx_.world.view<UICanvas>();
    for (auto [e, c] : view.each()) {
        if (c.scaleMode == UICanvas::ScaleMode::ScaleWithScreen && c.referenceWidth > 0 && c.referenceHeight > 0) {
            const float sx = screenW_ / static_cast<float>(c.referenceWidth);
            const float sy = screenH_ / static_cast<float>(c.referenceHeight);
            c.scaleFactor = std::min(sx, sy);
        } else {
            c.scaleFactor = 1.f;
        }
    }
}

// ───────────────────────────────────────────────────────────────────────────────

void UISystem::getWorldCamera(float& camX, float& camY, float& zoom) const {
    camX = 0.f; camY = 0.f; zoom = 1.f;
    auto view = ctx_.world.view<Transform, Camera>();
    for (auto [e, tf, cam] : view.each()) {
        if (!cam.primary) continue;
        if ((cam.layerMask & renderPassBit(RenderPass::World)) == 0) continue;
        camX = tf.x; camY = tf.y;
        zoom = (cam.zoom > 0.f) ? cam.zoom : 1.f;
        return;
    }
}

void UISystem::screenToCamera(float sx, float sy, float& cx, float& cy) const {
    // UI/Screen 相机假设位于 (0,0)、zoom=1。屏幕像素 (sx,sy)（y-down，左上角原点）
    // 在该相机的 world 空间中即 (sx - vpW/2, sy - vpH/2)。
    cx = sx - screenW_ * 0.5f;
    cy = sy - screenH_ * 0.5f;
}

// ───────────────────────────────────────────────────────────────────────────────

void UISystem::runLayout() {
    auto& world = ctx_.world;

    auto canvasView = world.view<UICanvas>();
    for (auto [canvasE, c] : canvasView.each()) {
        const float cx = c.safeAreaLeft;
        const float cy = c.safeAreaTop;
        const float cw = std::max(0.f, screenW_ - c.safeAreaLeft - c.safeAreaRight);
        const float ch = std::max(0.f, screenH_ - c.safeAreaTop - c.safeAreaBottom);

        auto nv = world.view<UINode>();
        for (auto [e, n] : nv.each()) {
            if (n.parent != canvasE) continue;
            layoutNode(e, cx, cy, cw, ch);
        }
    }
}

void UISystem::layoutNode(entt::entity e, float parentX, float parentY,
                          float parentW, float parentH) {
    auto& world = ctx_.world;
    auto& n = world.get<UINode>(e);

    bool placed = false;

    // 世界锚点：把目标实体的世界坐标投影到屏幕像素，作为本节点 anchor 中心。
    if (auto* wa = world.try_get<UIWorldAnchor>(e)) {
        if (wa->target != entt::null && world.all_of<Transform>(wa->target)) {
            const auto& tf = world.get<Transform>(wa->target);
            float camX, camY, camZoom;
            getWorldCamera(camX, camY, camZoom);

            const float ax = (tf.x + wa->offsetX - camX) * camZoom + screenW_ * 0.5f;
            const float ay = (tf.y + wa->offsetY - camY) * camZoom + screenH_ * 0.5f;

            n.screenW = n.width;
            n.screenH = n.height;
            n.screenX = ax + n.offsetX - n.pivotX * n.screenW;
            n.screenY = ay + n.offsetY - n.pivotY * n.screenH;
            placed = true;
        }
    }

    if (!placed) {
        const float aMinX = parentX + n.anchor.minX * parentW;
        const float aMinY = parentY + n.anchor.minY * parentH;
        const float aMaxX = parentX + n.anchor.maxX * parentW;
        const float aMaxY = parentY + n.anchor.maxY * parentH;

        n.screenW = (n.anchor.minX != n.anchor.maxX) ? (aMaxX - aMinX) : n.width;
        n.screenH = (n.anchor.minY != n.anchor.maxY) ? (aMaxY - aMinY) : n.height;

        const float cx = (aMinX + aMaxX) * 0.5f;
        const float cy = (aMinY + aMaxY) * 0.5f;
        n.screenX = cx + n.offsetX - n.pivotX * n.screenW;
        n.screenY = cy + n.offsetY - n.pivotY * n.screenH;
    }

    // 递归布局子节点
    auto nv = world.view<UINode>();
    for (auto [child, cn] : nv.each()) {
        if (cn.parent == e) {
            layoutNode(child, n.screenX, n.screenY, n.screenW, n.screenH);
        }
    }
}

// ───────────────────────────────────────────────────────────────────────────────

namespace {
inline bool pointInRect(float px, float py, const UINode& n) {
    return px >= n.screenX && px < n.screenX + n.screenW &&
           py >= n.screenY && py < n.screenY + n.screenH;
}
} // namespace

void UISystem::runInteraction(InputState& input) {
    auto& world = ctx_.world;

    const float px = input.pointerX(0) * screenW_;
    const float py = input.pointerY(0) * screenH_;
    const bool  down         = input.pointerDown(0);
    const bool  justPressed  =  down && !prevPointerDown_;
    const bool  justReleased = !down &&  prevPointerDown_;
    prevPointerDown_ = down;
    // ── 1. 命中测试（取最上层可交互节点：sortOrder 最大，相等时按 buildCommands
    //     的稳定迭代顺序——后绘制者在上）。tiebreaker 必须与渲染一致，否则
    //     重叠的同序节点 hover 会和视觉对不上。
    entt::entity bestHit = entt::null;
    int          bestOrder = std::numeric_limits<int>::min();
    uint32_t     bestSeq   = 0;
    {
        auto nv = world.view<UINode>();
        uint32_t seq = 0;
        for (auto [e, n] : nv.each()) {
            n.hovered = false;
            const uint32_t mySeq = seq++;
            if (!n.visible || !n.interactable) continue;
            if (!pointInRect(px, py, n)) continue;
            // 只把真正的交互组件视作命中目标。否则贴在按钮上的 UILabel
                      // (interactable 默认 true) 会盖住父按钮、屏幕角落的提示文字会
                          // 偷走拖拽块附近的点击 —— 表现为"按钮命中错位"和"拖几次后无法
                         // 再拖"。如果业务确实想让背景/标签接受 raycast，自行加上
                         // UIButton 或调用 setUIInteractable(false) 由用户显式控制。
               const bool hasInteractor = world.any_of<UIButton, UIToggle,UISlider, UIDraggable>(e);
            if (!hasInteractor) continue;

            if (bestHit == entt::null ||n.sortOrder > bestOrder || (n.sortOrder == bestOrder && mySeq >= bestSeq)) {
                bestOrder = n.sortOrder;
                bestSeq   = mySeq;
                bestHit   = e;
                break;
            }
        }
    }
    hovered_ = bestHit;
    if (hovered_ != entt::null) world.get<UINode>(hovered_).hovered = true;

    // ── 2. 按下：抓取 pressed_，触发 onDown，开始拖拽/滑条 ───────────────────
    if (justPressed && hovered_ != entt::null) {
        pressed_ = hovered_;
        auto& n = world.get<UINode>(pressed_);
        n.pressed = true;

        if (auto* btn = world.try_get<UIButton>(pressed_)) {
            if (!btn->isDisabled && btn->onDown) btn->onDown();
        }
        if (auto* d = world.try_get<UIDraggable>(pressed_)) {
            d->dragging = true;
            d->grabOffsetX = px - n.screenX;
            d->grabOffsetY = py - n.screenY;
            if (d->onDragStart) d->onDragStart();
        }
        if (auto* s = world.try_get<UISlider>(pressed_)) {
            s->dragging = true;
        }
    }

    // ── 3. 滑条拖动跟随 ────────────────────────────────────────────────────
    if (down) {
        auto sv = world.view<UINode, UISlider>();
        for (auto [e, n, s] : sv.each()) {
            if (!s.dragging) continue;
            const float w = std::max(1.f, n.screenW);
            float t = (px - n.screenX) / w;
            t = std::clamp(t, 0.f, 1.f);
            if (s.step > 0.f) {
                t = std::round(t / s.step) * s.step;
                t = std::clamp(t, 0.f, 1.f);
            }
            const float v = s.min + t * (s.max - s.min);
            if (v != s.value) {
                s.value = v;
                if (s.onChanged) s.onChanged(v);
            }
        }
    }

    // ── 4. 拖拽元素跟随：直接改写 offsetX/Y（按工厂约定，此时 anchor=topLeft, pivot=0,0）
    //     offset 必须相对父节点原点存储，否则下一帧 layoutNode 会再叠一次
    //     parent 的 safeArea/screen 偏移，导致命中区相对视觉漂移。
    if (down) {
        auto dv = world.view<UINode, UIDraggable>();
        for (auto [e, n, d] : dv.each()) {
            if (!d.dragging) continue;
            float nx = px - d.grabOffsetX;
            float ny = py - d.grabOffsetY;
            if (d.clamp) {
                nx = std::clamp(nx, d.minX, std::max(d.minX, d.maxX - n.screenW));
                ny = std::clamp(ny, d.minY, std::max(d.minY, d.maxY - n.screenH));
            }

            float parentX = 0.f, parentY = 0.f;
            if (n.parent != entt::null) {
                if (auto* pc = world.try_get<UICanvas>(n.parent)) {
                    parentX = pc->safeAreaLeft;
                    parentY = pc->safeAreaTop;
                } else if (auto* pn = world.try_get<UINode>(n.parent)) {
                    parentX = pn->screenX;
                    parentY = pn->screenY;
                }
            }
            n.offsetX = nx - parentX;
            n.offsetY = ny - parentY;
            n.screenX = nx;
            n.screenY = ny;
            if (d.onDrag) d.onDrag(nx, ny);
        }
    }

    // ── 5. 抬起：触发 onUp/onClick/onChanged，结束拖拽/滑条 ─────────────────
    if (justReleased && pressed_ != entt::null) {
        auto& n = world.get<UINode>(pressed_);
        n.pressed = false;

        if (auto* btn = world.try_get<UIButton>(pressed_)) {
            if (!btn->isDisabled) {
                if (btn->onUp) btn->onUp();
                if (hovered_ == pressed_ && btn->onClick) btn->onClick();
            }
        }
        if (auto* tog = world.try_get<UIToggle>(pressed_)) {
            if (hovered_ == pressed_) {
                tog->isOn = !tog->isOn;
                if (tog->onChanged) tog->onChanged(tog->isOn);
            }
        }
        if (auto* d = world.try_get<UIDraggable>(pressed_)) {
            if (d->dragging) {
                d->dragging = false;
                if (d->onDragEnd) d->onDragEnd();
            }
        }
        if (auto* s = world.try_get<UISlider>(pressed_)) {
            if (s->dragging) {
                s->dragging = false;
                if (s->onReleased) s->onReleased(s->value);
            }
        }
        pressed_ = entt::null;
    }
}

// ───────────────────────────────────────────────────────────────────────────────

void UISystem::emitRect(float x, float y, float w, float h,
                        core::Color tint, TextureHandle tex,
                        core::Rect src, int sortKey) {
    if (w <= 0.f || h <= 0.f) return;

    backend::DrawSpriteCmd c{};
    if (tex.valid()) {
        c.texture = tex;
        if (src.w <= 0.f || src.h <= 0.f) {
            int tw = 1, th = 1;
            ctx_.renderDevice().getTextureDimensions(tex, tw, th);
            src = core::Rect{0.f, 0.f, static_cast<float>(tw), static_cast<float>(th)};
        }
    } else {
        c.texture = whiteTexture_;
        src = core::Rect{0.f, 0.f, 1.f, 1.f};
    }
      // CPU 后端 (SDL_GPU / GL) 把 (cmd.x, cmd.y) 当 quad 中心硬编码绘制，
           // 完全忽略 pivot；GPU-driven 后端则按 pivot 走矩阵。统一传中心坐标 +
          // pivot=0.5 即可让两条路径都把矩形左上角落在调用方指定的 (x, y)。
    float cx, cy;
    screenToCamera(x, y, cx, cy);
    c.x        = cx + w * 0.5f;
    c.y        = cy + h * 0.5f;
    c.pivotX   = 0.5f;
    c.pivotY   = 0.5f;
    c.rotation = 0.f;
    c.scaleX   = w / src.w;
    c.scaleY   = h / src.h;
    c.srcRect  = src;
    c.tint     = tint;
    c.layer    = 0;
    c.sortKey  = sortKey;
    c.ySort    = false;
    c.pass     = RenderPass::Screen;
    uiCommands_.emplace_back(c);
}

void UISystem::emitText(const UILabel& lbl, const UINode& n, int sortKey) {
    if (lbl.text.empty()) return;

    backend::DrawTextCmd t{};
    t.font     = lbl.font;
    t.text     = lbl.text;
    t.fontSize = lbl.fontSize;
    t.color    = lbl.color;
    t.layer    = 0;
    t.sortKey  = sortKey;
    t.ySort    = false;
    t.pass     = RenderPass::Screen;

    // 文字以 baseline 为锚（y 向下增长），近似估算字体度量：
    //   ascent ≈ fontSize * 0.8, total height ≈ fontSize。
    float ox = n.screenX, oy = n.screenY;
    switch (lbl.halign) {
        case UILabel::HAlign::Left:   ox = n.screenX; break;
        case UILabel::HAlign::Center: {
            const float approxW = static_cast<float>(lbl.text.size()) * lbl.fontSize * 0.55f;
            ox = n.screenX + (n.screenW - approxW) * 0.5f;
            break;
        }
        case UILabel::HAlign::Right: {
            const float approxW = static_cast<float>(lbl.text.size()) * lbl.fontSize * 0.55f;
            ox = n.screenX + n.screenW - approxW;
            break;
        }
    }
    const float ascent = lbl.fontSize * 0.8f;
    switch (lbl.valign) {
        case UILabel::VAlign::Top:    oy = n.screenY + ascent; break;
        case UILabel::VAlign::Middle: oy = n.screenY + (n.screenH + ascent) * 0.5f - lbl.fontSize * 0.1f; break;
        case UILabel::VAlign::Bottom: oy = n.screenY + n.screenH; break;
    }

    float cx, cy;
    screenToCamera(ox, oy, cx, cy);
    t.x = cx;
    t.y = cy;
    uiCommands_.emplace_back(std::move(t));
}

void UISystem::buildCommands() {
    uiCommands_.clear();
    auto& world = ctx_.world;

    // 收集所有可见 UINode，按 sortOrder 升序绘制（同序按实体 id 稳定）
    struct Entry { entt::entity e; int order; };
    std::vector<Entry> ordered;
    {
        auto nv = world.view<UINode>();
        for (auto [e, n] : nv.each()) {
            if (n.visible) ordered.push_back({e, n.sortOrder});
        }
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Entry& a, const Entry& b) { return a.order < b.order; });

    int seq = 0;
    for (const Entry& it : ordered) {
        const auto& n = world.get<UINode>(it.e);
        const int baseSort = it.order * 16 + (seq++ & 0xFFFF);

        // 背景
        if (auto* bg = world.try_get<UIBackground>(it.e)) {
            emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                     bg->color, bg->texture, bg->srcRect, baseSort);
        }

        // 按钮（自带状态色）
        if (auto* btn = world.try_get<UIButton>(it.e)) {
            core::Color col = btn->normal;
            if (btn->isDisabled) col = btn->disabled;
            else if (n.pressed)  col = btn->pressed;
            else if (n.hovered)  col = btn->hover;
            emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                     col, {}, {}, baseSort);
        }

        // 开关（轨道 + 圆钮）
        if (auto* tog = world.try_get<UIToggle>(it.e)) {
            const core::Color trackCol = tog->isOn ? tog->onColor : tog->offColor;
            emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                     trackCol, {}, {}, baseSort);
            const float pad  = 3.f;
            const float side = std::max(2.f, std::min(n.screenW, n.screenH) - pad * 2.f);
            const float ky   = n.screenY + (n.screenH - side) * 0.5f;
            const float kx   = tog->isOn ? (n.screenX + n.screenW - side - pad)
                                         : (n.screenX + pad);
            emitRect(kx, ky, side, side, tog->knobColor, {}, {}, baseSort + 1);
        }

        // 滑动条（轨道 + 填充 + 把手）
        if (auto* s = world.try_get<UISlider>(it.e)) {
            const float trackH = std::max(3.f, n.screenH * 0.35f);
            const float trackY = n.screenY + (n.screenH - trackH) * 0.5f;
            emitRect(n.screenX, trackY, n.screenW, trackH,
                     s->trackColor, {}, {}, baseSort);

            float t = (s->max > s->min) ? (s->value - s->min) / (s->max - s->min) : 0.f;
            t = std::clamp(t, 0.f, 1.f);
            emitRect(n.screenX, trackY, n.screenW * t, trackH,
                     s->fillColor, {}, {}, baseSort + 1);

            const float hw = s->handleW;
            const float hx = n.screenX + n.screenW * t - hw * 0.5f;
            emitRect(hx, n.screenY, hw, n.screenH,
                     s->handleColor, {}, {}, baseSort + 2);
        }

        // 进度条
        if (auto* pb = world.try_get<UIProgressBar>(it.e)) {
            emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                     pb->bgColor, {}, {}, baseSort);
            const float t = std::clamp(pb->value, 0.f, 1.f);
            float fx = n.screenX, fy = n.screenY, fw = n.screenW, fh = n.screenH;
            switch (pb->direction) {
                case UIProgressBar::Direction::LeftToRight:
                    fw = n.screenW * t; break;
                case UIProgressBar::Direction::RightToLeft:
                    fw = n.screenW * t; fx = n.screenX + n.screenW - fw; break;
                case UIProgressBar::Direction::TopToBottom:
                    fh = n.screenH * t; break;
                case UIProgressBar::Direction::BottomToTop:
                    fh = n.screenH * t; fy = n.screenY + n.screenH - fh; break;
            }
            emitRect(fx, fy, fw, fh, pb->fillColor, {}, {}, baseSort + 1);
        }

        // 图像
        if (auto* img = world.try_get<UIImage>(it.e)) {
            emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                     img->tint, img->texture, img->srcRect, baseSort);
        }

        // 文本（最上层）
        if (auto* lbl = world.try_get<UILabel>(it.e)) {
            emitText(*lbl, n, baseSort + 8);
        }
    }
}

void UISystem::emitDrawCommands(backend::CommandBuffer& cb) const {
    for (const auto& cmd : uiCommands_) {
        if (auto* s = std::get_if<backend::DrawSpriteCmd>(&cmd))      cb.drawSprite(*s);
        else if (auto* t = std::get_if<backend::DrawTextCmd>(&cmd))   cb.drawText(*t);
    }
}

void UISystem::appendDrawCommandPtrs(std::vector<const backend::RenderCmd*>& out) const {
    out.reserve(out.size() + uiCommands_.size());
    for (const auto& cmd : uiCommands_) out.push_back(&cmd);
}

} // namespace engine
