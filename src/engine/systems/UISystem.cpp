#include "UISystem.h"
#include "../runtime/EngineContext.h"
#include "../input/InputState.h"
#include "../components/RenderComponents.h"
#include <algorithm>

namespace engine {

UISystem::UISystem(EngineContext& ctx)
    : ctx_(ctx)
{
}

void UISystem::update(float dt) {
    auto& world = ctx_.world;
    auto& input = ctx_.inputState;
    
    // ── Step 1: 更新屏幕尺寸 ────────────────────────────────────────────────────
    if (ctx_.window) {
        screenW_ = static_cast<float>(ctx_.window->width());
        screenH_ = static_cast<float>(ctx_.window->height());
    }
    
    // ── Step 2: 更新 Canvas 缩放 ────────────────────────────────────────────────
    auto canvasView = world.view<Canvas>();
    for (auto [e, canvas] : canvasView.each()) {
        updateCanvasScale(e, canvas);
    }
    
    // ── Step 3: 计算布局 ────────────────────────────────────────────────────────
    refreshLayout();
    
    // ── Step 4: 处理指针事件 ────────────────────────────────────────────────────
    processPointerEvents(world, input);
    
    // ── Step 5: 更新按钮视觉状态 ────────────────────────────────────────────────
    auto buttonView = world.view<Button, UIElement>();
    for (auto [e, btn, elem] : buttonView.each()) {
        handleButton(e, btn, elem);
    }
    
    // ── Step 6: 处理拖拽 ────────────────────────────────────────────────────────
    auto dragView = world.view<DragHandler, UIElement>();
    for (auto [e, drag, elem] : dragView.each()) {
        handleDrag(e, drag, elem, input);
    }
}

void UISystem::refreshLayout() {
    auto& world = ctx_.world;
    
    // 获取所有 Canvas (根节点)
    auto canvasView = world.view<Canvas>();
    
    for (auto [canvasEntity, canvas] : canvasView.each()) {
        // Canvas 的世界坐标从屏幕左上角 (0, 0) 开始
        // 考虑 safeArea 偏移
        float canvasX = canvas.safeAreaLeft;
        float canvasY = canvas.safeAreaTop;
        float canvasW = screenW_ - canvas.safeAreaLeft - canvas.safeAreaRight;
        float canvasH = screenH_ - canvas.safeAreaTop - canvas.safeAreaBottom;
        
        // 遍历 Canvas 下的所有 UI 元素
        auto elemView = world.view<UIElement>();
        for (auto [e, elem] : elemView.each()) {
            // 检查是否属于该 Canvas (通过 UIParent 或直接判断)
            if (world.all_of<UIParent>(e)) {
                continue;  // 有父节点，由递归处理
            }
            computeWorldPosition(e, world, canvasX, canvasY, canvasW, canvasH);
        }
    }
}

void UISystem::updateCanvasScale(entt::entity canvasEntity, Canvas& canvas) {
    if (canvas.scaleMode == Canvas::ScaleMode::ScaleWithScreenSize) {
        // 计算保持宽高比的缩放因子
        float scaleX = screenW_ / static_cast<float>(canvas.referenceWidth);
        float scaleY = screenH_ / static_cast<float>(canvas.referenceHeight);
        canvas.scaleFactor = std::min(scaleX, scaleY);
    } else {
        canvas.scaleFactor = 1.f;
    }
}

void UISystem::computeWorldPosition(entt::entity e, entt::registry& world,
                                    float parentX, float parentY,
                                    float parentW, float parentH) {
    if (!world.all_of<UIElement>(e)) return;
    
    auto& elem = world.get<UIElement>(e);
    
    // ── 计算锚点位置 ────────────────────────────────────────────────────────────
    // 锚点定义了元素相对于父容器的参考点
    float anchorX = parentX + elem.anchor.minX * parentW;
    float anchorY = parentY + elem.anchor.minY * parentH;
    float anchorMaxX = parentX + elem.anchor.maxX * parentW;
    float anchorMaxY = parentY + elem.anchor.maxY * parentH;
    
    // ── 计算元素尺寸 ────────────────────────────────────────────────────────────
    if (elem.anchor.minX != elem.anchor.maxX) {
        // 水平拉伸模式：宽度由锚点决定
        elem.computedW = (anchorMaxX - anchorX) - elem.offsetLeft - elem.offsetRight;
    } else {
        // 固定宽度模式
        elem.computedW = elem.width;
    }
    
    if (elem.anchor.minY != elem.anchor.maxY) {
        // 垂直拉伸模式：高度由锚点决定
        elem.computedH = (anchorMaxY - anchorY) - elem.offsetTop - elem.offsetBottom;
    } else {
        // 固定高度模式
        elem.computedH = elem.height;
    }
    
    // ── 计算元素位置 ────────────────────────────────────────────────────────────
    // 位置 = 锚点 + 偏移 - 中心点偏移
    elem.computedX = anchorX + elem.offsetLeft - elem.pivotX * elem.computedW;
    elem.computedY = anchorY + elem.offsetTop - elem.pivotY * elem.computedH;
    
    // 同步到 Transform (如果有)
    if (world.all_of<Transform>(e)) {
        auto& tf = world.get<Transform>(e);
        tf.x = elem.computedX;
        tf.y = elem.computedY;
    }
    
    // ── 递归处理子节点 ──────────────────────────────────────────────────────────
    if (world.all_of<UIChildren>(e)) {
        auto& children = world.get<UIChildren>(e);
        for (auto child : children.children) {
            computeWorldPosition(child, world,
                                 elem.computedX, elem.computedY,
                                 elem.computedW, elem.computedH);
        }
    }
}

void UISystem::processPointerEvents(entt::registry& world, InputState& input) {
    // 获取指针位置 (屏幕坐标)
    float px = input.pointerX(0);
    float py = input.pointerY(0);
    bool pointerDown = input.pointerDown(0);
    bool pointerJustPressed = pointerDown && !prevPointerDown_;  // 本帧刚按下
    prevPointerDown_ = pointerDown;  // 记录当前状态
    
    // ── 清除上一帧状态 ──────────────────────────────────────────────────────────
    if (!pointerDown) {
        hoveredEntity_ = entt::null;
    }
    
    // ── 遍历所有可交互元素 ──────────────────────────────────────────────────────
    auto elemView = world.view<UIElement>();
    
    // 按 sortOrder 降序排序 (优先处理上层元素)
    std::vector<entt::entity> sortedEntities;
    for (auto [e, elem] : elemView.each()) {
        if (elem.interactable && elem.raycastTarget) {
            sortedEntities.push_back(e);
        }
    }
    
    std::sort(sortedEntities.begin(), sortedEntities.end(),
              [&world](entt::entity a, entt::entity b) {
                  return world.get<UIElement>(a).sortOrder > 
                         world.get<UIElement>(b).sortOrder;
              });
    
    // ── 检测悬停 ────────────────────────────────────────────────────────────────
    hoveredEntity_ = entt::null;
    for (auto e : sortedEntities) {
        auto& elem = world.get<UIElement>(e);
        if (isPointInElement(px, py, e)) {
            hoveredEntity_ = e;
            elem.hovered = true;
            break;  // 只处理最上层
        } else {
            elem.hovered = false;
        }
    }
    
    // ── 处理按下 ────────────────────────────────────────────────────────────────
    if (pointerJustPressed && hoveredEntity_ != entt::null) {
        pressedEntity_ = hoveredEntity_;
        auto& elem = world.get<UIElement>(pressedEntity_);
        elem.pressed = true;
        
        // 触发 Button.onDown
        if (world.all_of<Button>(pressedEntity_)) {
            auto& btn = world.get<Button>(pressedEntity_);
            if (!btn.disabled && btn.onDown) {
                btn.onDown();
            }
        }
    }
    
    // ── 处理松开 ────────────────────────────────────────────────────────────────
    if (!pointerDown && pressedEntity_ != entt::null) {
        auto& elem = world.get<UIElement>(pressedEntity_);
        elem.pressed = false;
        
        // 触发 Button.onUp 和 onClick
        if (world.all_of<Button>(pressedEntity_)) {
            auto& btn = world.get<Button>(pressedEntity_);
            if (!btn.disabled) {
                if (btn.onUp) btn.onUp();
                // 只有在元素上松开才算点击
                if (hoveredEntity_ == pressedEntity_ && btn.onClick) {
                    btn.onClick();
                }
            }
        }
        
        // 触发 Toggle
        if (world.all_of<Toggle>(pressedEntity_) && hoveredEntity_ == pressedEntity_) {
            auto& toggle = world.get<Toggle>(pressedEntity_);
            toggle.isOn = !toggle.isOn;
            if (toggle.onValueChanged) {
                toggle.onValueChanged(toggle.isOn);
            }
        }
        
        pressedEntity_ = entt::null;
    }
    
    // ── 重置非悬停元素的 hovered 状态 ────────────────────────────────────────────
    for (auto [e, elem] : elemView.each()) {
        if (e != hoveredEntity_) {
            elem.hovered = false;
        }
    }
    
    // ── 处理 Slider ─────────────────────────────────────────────────────────────
    auto sliderView = world.view<Slider, UIElement>();
    for (auto [e, slider, elem] : sliderView.each()) {
        handleSlider(e, slider, elem, input);
    }
}

bool UISystem::isPointInElement(float px, float py, entt::entity e) const {
    auto& world = ctx_.world;
    if (!world.all_of<UIElement>(e)) return false;
    
    const auto& elem = world.get<UIElement>(e);
    
    // 简单的 AABB 检测
    return px >= elem.computedX &&
           px <= elem.computedX + elem.computedW &&
           py >= elem.computedY &&
           py <= elem.computedY + elem.computedH;
}

void UISystem::handleButton(entt::entity e, Button& btn, UIElement& elem) {
    // 更新 Sprite 颜色 (如果有)
    if (ctx_.world.all_of<Sprite>(e)) {
        auto& sprite = ctx_.world.get<Sprite>(e);
        
        if (btn.disabled) {
            sprite.tint = btn.disabledColor;
        } else if (elem.pressed) {
            sprite.tint = btn.pressedColor;
        } else if (elem.hovered) {
            sprite.tint = btn.hoverColor;
        } else {
            sprite.tint = btn.normalColor;
        }
    }
}

void UISystem::handleSlider(entt::entity e, Slider& slider, UIElement& elem,
                            InputState& input) {
    if (!elem.interactable) return;
    
    float px = input.pointerX(0);
    bool pointerDown = input.pointerDown(0);
    
    // ── 开始拖拽 ────────────────────────────────────────────────────────────────
    if (pointerDown && !slider.isDragging) {
        if (isPointInElement(px, input.pointerY(0), e)) {
            slider.isDragging = true;
        }
    }
    
    // ── 拖拽中 ──────────────────────────────────────────────────────────────────
    if (slider.isDragging && pointerDown) {
        // 计算滑动条内的相对位置
        float relX = px - elem.computedX;
        float normalizedPos = relX / elem.computedW;
        normalizedPos = std::clamp(normalizedPos, 0.f, 1.f);
        
        // 应用 step
        if (slider.step > 0.f) {
            float stepCount = std::round(normalizedPos / slider.step);
            normalizedPos = stepCount * slider.step;
        }
        
        // 计算实际值
        float newValue = slider.min + normalizedPos * (slider.max - slider.min);
        
        if (newValue != slider.value) {
            slider.value = newValue;
            if (slider.onValueChanged) {
                slider.onValueChanged(slider.value);
            }
        }
    }
    
    // ── 结束拖拽 ────────────────────────────────────────────────────────────────
    if (!pointerDown && slider.isDragging) {
        slider.isDragging = false;
        if (slider.onDragEnd) {
            slider.onDragEnd(slider.value);
        }
    }
}

void UISystem::handleDrag(entt::entity e, DragHandler& drag, UIElement& elem,
                          InputState& input) {
    if (!drag.draggable || !elem.interactable) return;
    
    // ── 开始拖拽 ────────────────────────────────────────────────────────────────
    if (input.pointerDown(0) && !drag.isDragging) {
        if (elem.hovered) {
            drag.isDragging = true;
            draggingEntity_ = e;
            if (drag.onDragStart) {
                drag.onDragStart();
            }
        }
    }
    
    // ── 拖拽中 ──────────────────────────────────────────────────────────────────
    if (drag.isDragging && input.pointerDown(0)) {
        float newX = input.pointerX(0) - elem.computedW * elem.pivotX;
        float newY = input.pointerY(0) - elem.computedH * elem.pivotY;
        
        // 应用范围限制
        if (drag.minX != drag.maxX) {
            newX = std::clamp(newX, drag.minX, drag.maxX - elem.computedW);
        }
        if (drag.minY != drag.maxY) {
            newY = std::clamp(newY, drag.minY, drag.maxY - elem.computedH);
        }
        
        // 限制在父容器内
        if (drag.constrainToParent) {
            // TODO: 获取父容器边界
        }
        
        // 更新位置
        elem.computedX = newX;
        elem.computedY = newY;
        elem.offsetLeft = newX;
        elem.offsetTop = newY;
        
        // 同步到 Transform
        if (ctx_.world.all_of<Transform>(e)) {
            auto& tf = ctx_.world.get<Transform>(e);
            tf.x = newX;
            tf.y = newY;
        }
        
        if (drag.onDrag) {
            drag.onDrag(newX, newY);
        }
    }
    
    // ── 结束拖拽 ────────────────────────────────────────────────────────────────
    if (!input.pointerDown(0) && drag.isDragging) {
        drag.isDragging = false;
        if (draggingEntity_ == e) {
            draggingEntity_ = entt::null;
        }
        if (drag.onDragEnd) {
            drag.onDragEnd();
        }
    }
}

} // namespace engine
