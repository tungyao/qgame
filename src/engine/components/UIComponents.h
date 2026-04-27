#pragma once

#include <functional>
#include <cstdint>
#include <string>
#include <vector>
#include <entt/entt.hpp>
#include "../../core/math/Color.h"
#include "../../core/math/Rect.h"
#include "../../backend/shared/ResourceHandle.h"
#include "FontData.h"

namespace engine {

// ═══════════════════════════════════════════════════════════════════════════════
// UI 锚点系统
// ───────────────────────────────────────────────────────────────────────────────
// 锚点定义 UI 元素相对于父容器的位置关系
// min = 左上角锚点, max = 右下角锚点 (0-1 范围)
// 例: anchorMin(0,0) + anchorMax(1,1) = 拉伸填满父容器
// 例: anchorMin(0.5,0.5) + anchorMax(0.5,0.5) = 居中定位
// ═══════════════════════════════════════════════════════════════════════════════

struct UIAnchor {
    float minX = 0.f;   // 左锚点 (0=左边缘, 1=右边缘)
    float minY = 0.f;   // 上锚点 (0=上边缘, 1=下边缘)
    float maxX = 0.f;   // 右锚点
    float maxY = 0.f;   // 下锚点
    
    // 常用预设
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

// ═══════════════════════════════════════════════════════════════════════════════
// UIElement - 所有 UI 组件的基类
// ───────────────────────────────────────────────────────────────────────────────
// 包含布局、交互状态、层级等通用属性
// ═══════════════════════════════════════════════════════════════════════════════

struct UIElement {
    // ── 布局属性 ────────────────────────────────────────────────────────────────
    float width  = 100.f;    // 元素宽度 (像素)
    float height = 50.f;     // 元素高度 (像素)
    float pivotX = 0.5f;     // 中心点 X (0=左, 1=右)
    float pivotY = 0.5f;     // 中心点 Y (0=上, 1=下)
    
    // 锚点相对于父容器
    UIAnchor anchor;
    
    // 锚点偏移量 (像素)
    float offsetLeft   = 0.f;
    float offsetTop    = 0.f;
    float offsetRight  = 0.f;
    float offsetBottom = 0.f;
    
    // ── 交互状态 ────────────────────────────────────────────────────────────────
    bool interactable = true;   // 是否响应交互
    bool raycastTarget = true;  // 是否参与射线检测
    bool hovered  = false;      // 当前帧鼠标/触摸悬停
    bool pressed  = false;      // 当前帧按下
    bool selected = false;      // 当前被选中 (Tab 导航/焦点)
    
    // ── 层级控制 ────────────────────────────────────────────────────────────────
    int sortOrder = 0;          // 渲染顺序 (大数在后)
    int layer = 0;              // 渲染层
    
    // ── 运行时计算 (由 UISystem 填充) ───────────────────────────────────────────
    float computedX = 0.f;      // 计算后的世界坐标 X
    float computedY = 0.f;      // 计算后的世界坐标 Y
    float computedW = 0.f;      // 计算后的实际宽度
    float computedH = 0.f;      // 计算后的实际高度
};

// ═══════════════════════════════════════════════════════════════════════════════
// Canvas - UI 根容器
// ───────────────────────────────────────────────────────────────────────────────
// Canvas 定义 UI 的渲染空间和缩放模式
// 所有 UI 元素必须挂在某个 Canvas 下
// ═══════════════════════════════════════════════════════════════════════════════

struct Canvas {
    enum class ScaleMode {
        ConstantPixelSize,  // 固定像素大小 (不随分辨率缩放)
        ScaleWithScreenSize // 随屏幕分辨率缩放
    };
    
    int   referenceWidth  = 1920;   // 设计分辨率宽度
    int   referenceHeight = 1080;   // 设计分辨率高度
    float scaleFactor     = 1.f;    // 当前缩放因子 (由系统计算)
    ScaleMode scaleMode   = ScaleMode::ScaleWithScreenSize;
    int   sortingOrder    = 0;      // Canvas 间排序 (多个 Canvas 时)
    
    // 屏幕安全区域 (刘海屏适配)
    float safeAreaLeft   = 0.f;
    float safeAreaTop    = 0.f;
    float safeAreaRight  = 0.f;
    float safeAreaBottom = 0.f;
};

// ═══════════════════════════════════════════════════════════════════════════════
// UIParent - 层级关系组件 (可选)
// ───────────────────────────────────────────────────────────────────────────────
// 用于构建 UI 树结构，子元素坐标相对于父元素
// ═══════════════════════════════════════════════════════════════════════════════

struct UIParent {
    entt::entity parent = entt::null;  // 父节点实体
};

struct UIChildren {
    std::vector<entt::entity> children;  // 子节点列表
};

// ═══════════════════════════════════════════════════════════════════════════════
// Button - 按钮组件
// ───────────────────────────────────────────────────────────────────────────────
// 可点击的交互元素，支持颜色变化和回调
// ═══════════════════════════════════════════════════════════════════════════════

struct Button {
    // ── 视觉状态 ────────────────────────────────────────────────────────────────
    core::Color normalColor  = {200, 200, 200, 255};  // 正常状态颜色
    core::Color hoverColor   = {230, 230, 230, 255};  // 悬停状态颜色
    core::Color pressedColor = {150, 150, 150, 255};  // 按下状态颜色
    core::Color disabledColor = {100, 100, 100, 255}; // 禁用状态颜色
    
    // ── 点击回调 ────────────────────────────────────────────────────────────────
    std::function<void()> onClick;  // 点击时调用
    std::function<void()> onDown;   // 按下时调用
    std::function<void()> onUp;     // 松开时调用
    
    // ── 状态 ────────────────────────────────────────────────────────────────────
    bool disabled = false;  // 禁用状态
};

// ═══════════════════════════════════════════════════════════════════════════════
// Toggle - 开关组件
// ───────────────────────────────────────────────────────────────────────────────
// 二态切换控件 (复选框/开关)
// ═══════════════════════════════════════════════════════════════════════════════

struct Toggle {
    bool isOn = false;  // 当前开关状态
    std::function<void(bool)> onValueChanged;  // 状态改变回调
    
    // 视觉资源 (可选)
    TextureHandle onTexture;   // 开启状态纹理
    TextureHandle offTexture;  // 关闭状态纹理
};

// ═══════════════════════════════════════════════════════════════════════════════
// Slider - 滑动条组件
// ───────────────────────────────────────────────────────────────────────────────
// 数值选择控件，支持拖拽调节
// ═══════════════════════════════════════════════════════════════════════════════

struct Slider {
    // ── 数值范围 ────────────────────────────────────────────────────────────────
    float value = 0.f;      // 当前值 (0-1 归一化)
    float min   = 0.f;      // 最小值
    float max   = 1.f;      // 最大值
    float step  = 0.f;      // 步进值 (0=连续)
    
    // ── 视觉属性 ────────────────────────────────────────────────────────────────
    float handleWidth   = 20.f;   // 滑块手柄宽度
    core::Color backgroundColor = {50, 50, 50, 255};     // 背景色
    core::Color fillColor       = {100, 150, 200, 255};  // 填充色
    core::Color handleColor     = {200, 200, 200, 255};  // 手柄色
    
    // ── 回调 ────────────────────────────────────────────────────────────────────
    std::function<void(float)> onValueChanged;  // 值改变回调
    std::function<void(float)> onDragEnd;       // 拖拽结束回调
    
    // ── 运行时状态 ──────────────────────────────────────────────────────────────
    bool isDragging = false;   // 是否正在拖拽
};

// ═══════════════════════════════════════════════════════════════════════════════
// ProgressBar - 进度条组件
// ───────────────────────────────────────────────────────────────────────────────
// 显示进度的非交互组件 (血条/加载条)
// ═══════════════════════════════════════════════════════════════════════════════

struct ProgressBar {
    float value = 0.f;          // 当前值 (0-1)
    
    core::Color backgroundColor = {50, 50, 50, 255};     // 背景色
    core::Color fillColor       = {100, 200, 100, 255};  // 填充色
    
    // 进度条填充方向
    enum class Direction {
        LeftToRight,
        RightToLeft,
        BottomToTop,
        TopToBottom
    } direction = Direction::LeftToRight;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Image - UI 图像组件
// ───────────────────────────────────────────────────────────────────────────────
// 纯显示的 UI 图片，可配合 Button 使用
// ═══════════════════════════════════════════════════════════════════════════════

struct UIImage {
    TextureHandle texture;          // 纹理资源
    core::Rect    srcRect;          // 纹理采样区域
    core::Color   color = core::Color::White;  // 颜色调制
    bool          preserveAspect = true;       // 保持宽高比
};

// ═══════════════════════════════════════════════════════════════════════════════
// UIText - UI 专用文本组件
// ───────────────────────────────────────────────────────────────────────────────
// 用于 UI 显示的文本，支持对齐和自动换行
// ═══════════════════════════════════════════════════════════════════════════════

struct UIText {
    std::string text;           // 文本内容
    FontHandle  font;           // 字体资源
    float       fontSize = 16.f;// 字体大小
    
    enum class Alignment {
        Left,
        Center,
        Right
    } alignment = Alignment::Center;
    
    enum class VerticalAlignment {
        Top,
        Middle,
        Bottom
    } verticalAlignment = VerticalAlignment::Middle;
    
    core::Color color = core::Color::White;  // 文本颜色
    bool  wordWrap = false;   // 自动换行
    float lineSpacing = 1.2f; // 行间距倍数
    bool  visible = true;     // 是否可见
};

// ═══════════════════════════════════════════════════════════════════════════════
// DragHandler - 拖拽组件
// ───────────────────────────────────────────────────────────────────────────────
// 允许元素被拖拽移动
// ═══════════════════════════════════════════════════════════════════════════════

struct DragHandler {
    bool draggable = true;      // 是否可拖拽
    bool constrainToParent = false;  // 限制在父容器内
    
    // 拖拽范围限制 (0=不限制)
    float minX = 0.f, maxX = 0.f;
    float minY = 0.f, maxY = 0.f;
    
    std::function<void(float x, float y)> onDrag;      // 拖拽中回调
    std::function<void()> onDragStart;                 // 开始拖拽
    std::function<void()> onDragEnd;                   // 结束拖拽
    
    bool isDragging = false;   // 运行时状态
};

} // namespace engine
