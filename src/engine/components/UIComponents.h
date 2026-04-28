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

// ── 九宫格背景 ──────────────────────────────────────────────────────────────
// 把一张纹理切成 3×3 共 9 块：四角保持原始像素尺寸不缩放，四条边只在自己的主轴
// 上拉伸，中心同时在两个方向拉伸。常用于对话框、面板、按钮底图——任意尺寸都
// 不变形，只有中间填充区被拉大。
//
// 切分方式：纹理上由 (border.l/t/r/b) 四个像素值确定切线，整张图变成 9 个 srcRect。
// 渲染时把节点的 screenW/H 减去四角后剩余部分给四边和中心。
//
//        srcRect 全图 (w,h)                      节点矩形 (W,H)
//   ┌───┬─────────────┬───┐               ┌───┬─────────────┬───┐
//   │TL │     T       │TR │               │TL │      T      │TR │  四角:固定
//   ├───┼─────────────┼───┤               ├───┼─────────────┼───┤  上下边:横向拉伸
//   │ L │   Center    │ R │  =拉伸=>      │ L │   Center    │ R │  左右边:纵向拉伸
//   ├───┼─────────────┼───┤               ├───┼─────────────┼───┤  中心:双向拉伸
//   │BL │     B       │BR │               │BL │      B      │BR │
//   └───┴─────────────┴───┘               └───┴─────────────┴───┘
//
// fillCenter=false 可以做成"边框模式"——只画 8 块外框，中心透出底层。
struct UINineSlice {
    TextureHandle texture;
    core::Rect    srcRect;            // (0,0,0,0) = 整张纹理
    // 四向边界（源纹理像素，从 srcRect 内侧切开）
    float         borderLeft   = 8.f;
    float         borderTop    = 8.f;
    float         borderRight  = 8.f;
    float         borderBottom = 8.f;
    core::Color   tint         = core::Color::White;
    bool          fillCenter   = true;
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

// ── 滚动容器 ────────────────────────────────────────────────────────────────
// 视口节点本身的 UINode.screenW/H 即可见区域。其直接子节点构成"内容"，会按
// scrollX/Y 整体平移，并被裁剪到视口矩形内。
//   - contentW/H 若 <=0，UISystem 每帧自动测量（取子节点 offset+size 的最大值）。
//   - direction 决定哪些方向参与滚动 / 滚轮响应。
//   - dragToScroll：在视口空白处按下拖动也能滚（不影响子节点的 Button/Slider）。
struct UIScrollView {
    enum class Direction { Vertical, Horizontal, Both };
    Direction direction = Direction::Vertical;

    float contentW = 0.f;
    float contentH = 0.f;

    float scrollX = 0.f;
    float scrollY = 0.f;

    float wheelSpeed   = 40.f;
    bool  clamp        = true;
    bool  dragToScroll = true;

    // 运行时
    bool  dragging = false;
    float lastPx   = 0.f;
    float lastPy   = 0.f;

    std::function<void(float, float)> onScroll;   // (scrollX, scrollY)
};

// ── 工具提示 ────────────────────────────────────────────────────────────────
// 给一个 UINode 挂上 UITooltip 后，鼠标在其上停留 delay 秒就会弹出一块带文字
// 的小面板（紧贴鼠标右下，超出屏幕时自动反向）。鼠标移开/抬起即消失。
//
// 实现要点：
//   - UISystem 维护一个"当前悬停的 tooltip 触发器 + 累计停留时间"，dt>=delay 时
//     在 buildCommands 末尾追加 tooltip 的矩形+文本命令（最大 sortKey，不进任何
//     ScrollView 的 scissor 子树，因此不会被裁剪）。
//   - 文本宽度用 fontSize*0.55 估算（与 emitText 保持一致）。
//   - 想让没有交互组件(UIButton/Toggle/...)的纯标签也能触发，UISystem 在命中过滤
//     里把 UITooltip 也算作可命中目标。
struct UITooltip {
    std::string text;
    FontHandle  font;
    float       fontSize  = 14.f;
    float       delay     = 0.4f;     // 悬停多久后弹出
    float       paddingX  = 8.f;
    float       paddingY  = 6.f;
    float       offsetX   = 14.f;     // 相对鼠标的偏移（默认右下）
    float       offsetY   = 18.f;
    core::Color bgColor   = { 20, 20, 25, 230 };
    core::Color textColor = core::Color::White;
};

// ── 自动布局组 ──────────────────────────────────────────────────────────────
// 给一个 UINode 加上 UILayoutGroup 后，UISystem 每帧会主动计算它的"直接子节点"
// 的位置/大小，覆盖子节点原本的 anchor / pivot / offset (Stretch 模式还会覆盖
// 子节点交叉轴的 width/height)。
//
// 三种排布：
//   Horizontal  从左到右(reverse=true 则从右到左)依次摆放，子节点宽度由各自 width
//               决定；交叉轴(垂直方向)按 crossAlign 对齐，Stretch 时撑满父高。
//   Vertical    上下方向，规则同上。
//   Grid        以"首个子节点"的 width/height 作为单元格大小，按 gridColumns 列
//               左到右、上到下排布；spacingX/spacingY 分别为列/行间距。
//
// 排序：子节点按 UINode.sortOrder 升序入队，再按 reverse 翻转主轴。和渲染层一致。
//
// 注意：被布局管理的子节点不应再手工 setUIAnchor / setUIOffset —— 那些值会被本
// 系统每帧重写。如果需要嵌套，一个布局组的子节点本身可以再挂 UILayoutGroup。
struct UILayoutGroup {
    enum class Type       { Horizontal, Vertical, Grid };
    enum class CrossAlign { Start, Center, End, Stretch };

    Type       type        = Type::Vertical;

    // 内边距：实际排布区域 = 父节点屏幕矩形 - padding
    float      paddingL    = 0.f;
    float      paddingT    = 0.f;
    float      paddingR    = 0.f;
    float      paddingB    = 0.f;

    // 子节点间距。Horizontal 用 spacingX；Vertical 用 spacingY；Grid 两者都用。
    float      spacingX    = 0.f;
    float      spacingY    = 0.f;

    // 仅 Grid：每行列数（>=1）
    int        gridColumns = 4;

    // 交叉轴对齐 (Horizontal:y 方向 / Vertical:x 方向；Grid 忽略)
    CrossAlign crossAlign  = CrossAlign::Start;

    // 主轴反向：Horizontal 时从右到左，Vertical 时从下到上
    bool       reverse     = false;
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
