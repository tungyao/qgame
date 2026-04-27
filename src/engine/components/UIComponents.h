#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <entt/entt.hpp>

#include "../../core/math/Color.h"
#include "../../core/math/Rect.h"
#include "../../backend/shared/ResourceHandle.h"
#include "FontData.h"

namespace engine {

// ═══════════════════════════════════════════════════════════════════════════════
// UI System v2
// ───────────────────────────────────────────────────────────────────────────────
// 设计要点
//   1. 单一布局节点 UINode：包含布局参数 + 计算结果 + 交互状态。所有 UI 实体都挂这个。
//   2. 视觉/功能组件按需附加（UIBackground/UIButton/UISlider/...）。
//   3. 渲染走独立路径：UISystem::emitDrawCommands 直接输出 DrawSpriteCmd/DrawTextCmd。
//      不再借用通用 Sprite 组件，从而避开 srcRect/pivot/RenderPass 的耦合。
//   4. 世界空间锚点 UIWorldAnchor：UI 跟随某个世界实体投影到屏幕，仍以屏幕像素尺寸渲染。
//   5. 纯色矩形通过 UISystem 持有的 1×1 白色纹理 + tint 实现，未来可平滑升级到纹理/九宫格。
// ═══════════════════════════════════════════════════════════════════════════════

// ── 锚点（相对父级矩形）─────────────────────────────────────────────────────
struct UIAnchor {
    float minX = 0.f, minY = 0.f;
    float maxX = 0.f, maxY = 0.f;

    static UIAnchor topLeft()      { return {0.f, 0.f, 0.f, 0.f}; }
    static UIAnchor topCenter()    { return {0.5f, 0.f, 0.5f, 0.f}; }
    static UIAnchor topRight()     { return {1.f, 0.f, 1.f, 0.f}; }
    static UIAnchor centerLeft()   { return {0.f, 0.5f, 0.f, 0.5f}; }
    static UIAnchor center()       { return {0.5f, 0.5f, 0.5f, 0.5f}; }
    static UIAnchor centerRight()  { return {1.f, 0.5f, 1.f, 0.5f}; }
    static UIAnchor bottomLeft()   { return {0.f, 1.f, 0.f, 1.f}; }
    static UIAnchor bottomCenter() { return {0.5f, 1.f, 0.5f, 1.f}; }
    static UIAnchor bottomRight()  { return {1.f, 1.f, 1.f, 1.f}; }
    static UIAnchor stretch()      { return {0.f, 0.f, 1.f, 1.f}; }
};

// ── Canvas：UI 根容器 ───────────────────────────────────────────────────────
struct UICanvas {
    enum class ScaleMode { Fixed, ScaleWithScreen };
    int       referenceWidth  = 1920;
    int       referenceHeight = 1080;
    ScaleMode scaleMode       = ScaleMode::ScaleWithScreen;
    float     scaleFactor     = 1.f;     // 由 UISystem 计算
    int       sortingOrder    = 0;
    float     safeAreaLeft   = 0.f, safeAreaTop    = 0.f;
    float     safeAreaRight  = 0.f, safeAreaBottom = 0.f;
};

// ── 世界空间锚定：UI 跟随某个世界实体（如角色头顶血条）────────────────────
struct UIWorldAnchor {
    entt::entity target  = entt::null;   // 必须有 Transform 组件
    float        offsetX = 0.f;          // 世界空间偏移（像素）
    float        offsetY = 0.f;
};

// ── UINode：所有 UI 元素的核心节点（布局 + 状态）───────────────────────────
struct UINode {
    // 父子层级。parent 既可指向另一个 UINode 实体，也可指向 UICanvas 实体。
    entt::entity parent = entt::null;

    // 布局参数
    float    width  = 100.f;
    float    height = 40.f;
    float    pivotX = 0.5f;
    float    pivotY = 0.5f;
    UIAnchor anchor = UIAnchor::topLeft();
    float    offsetX = 0.f;       // 锚点中心相对偏移
    float    offsetY = 0.f;

    // 状态
    bool visible      = true;
    bool interactable = true;
    int  sortOrder    = 0;        // 同层级内排序：大数后画

    // 运行时计算（屏幕像素，矩形左上角，y-down）
    float screenX = 0.f, screenY = 0.f;
    float screenW = 0.f, screenH = 0.f;

    // 当前帧交互状态
    bool hovered = false;
    bool pressed = false;
};

// ── 视觉：纯色或图片背景 ───────────────────────────────────────────────────
struct UIBackground {
    core::Color   color = {200, 200, 200, 255};
    TextureHandle texture;          // 空 = 纯色矩形（走白色 1×1）
    core::Rect    srcRect;          // (0,0,0,0) = 整张
};

// ── 按钮 ────────────────────────────────────────────────────────────────────
struct UIButton {
    core::Color normal   = {200, 200, 200, 255};
    core::Color hover    = {230, 230, 230, 255};
    core::Color pressed  = {150, 150, 150, 255};
    core::Color disabled = {100, 100, 100, 255};

    std::function<void()> onClick;
    std::function<void()> onDown;
    std::function<void()> onUp;

    bool isDisabled = false;
};

// ── 开关 ────────────────────────────────────────────────────────────────────
struct UIToggle {
    bool        isOn      = false;
    core::Color onColor   = {100, 200, 100, 255};
    core::Color offColor  = { 80,  80,  80, 255};
    core::Color knobColor = {235, 235, 235, 255};

    std::function<void(bool)> onChanged;
};

// ── 滑动条 ──────────────────────────────────────────────────────────────────
struct UISlider {
    float value = 0.f;
    float min   = 0.f;
    float max   = 1.f;
    float step  = 0.f;            // 0 = 连续

    float       handleW    = 20.f;
    core::Color trackColor = { 50,  50,  50, 255};
    core::Color fillColor  = {100, 150, 200, 255};
    core::Color handleColor= {220, 220, 220, 255};

    std::function<void(float)> onChanged;
    std::function<void(float)> onReleased;

    bool dragging = false;        // 运行时
};

// ── 进度条 ──────────────────────────────────────────────────────────────────
struct UIProgressBar {
    enum class Direction { LeftToRight, RightToLeft, BottomToTop, TopToBottom };

    float       value     = 0.f;       // 0..1
    core::Color bgColor   = { 50,  50,  50, 255};
    core::Color fillColor = {100, 200, 100, 255};
    Direction   direction = Direction::LeftToRight;
};

// ── 图像 ────────────────────────────────────────────────────────────────────
struct UIImage {
    TextureHandle texture;
    core::Rect    srcRect;            // (0,0,0,0) = 整张
    core::Color   tint = core::Color::White;
};

// ── 文本 ────────────────────────────────────────────────────────────────────
struct UILabel {
    enum class HAlign { Left, Center, Right };
    enum class VAlign { Top,  Middle, Bottom };

    std::string text;
    FontHandle  font;
    float       fontSize = 16.f;
    core::Color color    = core::Color::White;
    HAlign      halign   = HAlign::Center;
    VAlign      valign   = VAlign::Middle;
};

// ── 拖拽 ────────────────────────────────────────────────────────────────────
// 约束：拖拽元素假设 anchor=topLeft, pivot=(0,0)，offsetX/Y 直接当作屏幕坐标。
//      工厂函数 makeDraggable 会强制设好这些参数。
struct UIDraggable {
    bool  dragging      = false;      // 运行时
    float grabOffsetX   = 0.f;        // 运行时（按下点相对 node 左上角偏移）
    float grabOffsetY   = 0.f;

    bool  clamp         = false;
    float minX = 0.f, minY = 0.f;
    float maxX = 0.f, maxY = 0.f;     // 仅当 clamp=true 时生效

    std::function<void(float, float)> onDrag;
    std::function<void()>             onDragStart;
    std::function<void()>             onDragEnd;
};

} // namespace engine
