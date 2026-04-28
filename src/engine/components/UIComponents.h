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
    // 全局层级：跨越父子树覆盖 sortOrder 与 DFS 顺序，大数永远在上。用来手动
    // 解决"scrollbar 与 button 重叠时点击穿透"这类跨子树覆盖问题——把要置顶
    // 的控件 layer 设大一些即可。0 = 默认（按树形顺序）。绘制和命中共用同一
    // 比较：先比 layer，再比同一棵 DFS 中的访问次序。
    int  layer        = 0;

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

// ── 裁剪遮罩 ────────────────────────────────────────────────────────────────
// 给一个 UINode 加上 UIMask 后，其整棵子树的渲染和命中测试都会被裁剪到该节点
// 自身的屏幕矩形（与 UIScrollView 共用同一套 scissor 机制）。常用于：
//   - 头像圆形裁剪槽、技能图标方框遮罩
//   - 容器面板内的"溢出隐藏"
//   - 关卡选择条这种横向条带，限定子项只在条带可视区内显示
// 与 UIScrollView 区别：UIMask 不参与滚动 / 输入拖拽，纯粹只做裁剪。两者可以
// 同时挂在同一节点上，scissor 仍然只 push 一次（节点自身矩形）。
struct UIMask {
    bool enabled = true;
};

// ── 滚动条 ──────────────────────────────────────────────────────────────────
// 把一个 UINode 标记成 ScrollView 的"滚动条"：节点自身的屏幕矩形即滑槽 (track)，
// 内部按 target 的 contentW/H、screenW/H、scrollX/Y 自动绘制一个滑块 (thumb)。
//
// 主轴定义：
//   Orientation::Vertical   主轴 = Y，节点高 = 滑槽长度，节点宽 = 滑槽厚度
//   Orientation::Horizontal 主轴 = X，节点宽 = 滑槽长度，节点高 = 滑槽厚度
//
// 滑块尺寸 / 位置（设主轴长度为 L，target 的内容主轴长度为 C，视口主轴长度为 V，
// 当前滚动量为 S；S 已被 ScrollView::clamp 夹到 [0, C-V]）：
//   thumbLen   = clamp(L * V / C, minThumbSize, L)        // 比例反映可见占比
//   thumbStart = (L - thumbLen) * S / (C - V)             // C<=V 时不画 (autoHide)
//
// 交互：
//   - 按住 thumb 拖动：记录按下时的 (pointerMain, scroll)，每帧把鼠标主轴位移
//     按 (C - V) / (L - thumbLen) 的比率转换成内容滚动量，写回 target.scrollX/Y
//     并触发其 onScroll 回调（与滚轮/dragToScroll 完全一致的写入路径）。
//   - 按 track 空白区：往鼠标方向翻一页 (一屏 = V)，鼠标在 thumb 上方=减、下方=加。
//
// 视觉：track + thumb 都是纯色矩形，颜色随 hover / pressed 变化。autoHide=true 时
// 当 C <= V (内容能完全装下) 直接不渲染，但节点的 hovered/pressed 等状态仍然按
// 普通 UINode 维护——这是为了让"出现/消失"完全跟着内容尺寸跑，不需要业务代码
// 手动 setUIVisible。
//
// 该组件不主动做 layout —— 通常做法是让 ScrollBar 与 ScrollView 同级，靠 anchor
// 把它贴到视口右侧 (Vertical) 或底部 (Horizontal)；也可以塞进同一父 LayoutGroup
// 做"列表 + 滚动条"的整体排版。
struct UIScrollBar {
    enum class Orientation { Vertical, Horizontal };
    Orientation  orientation = Orientation::Vertical;

    // 关联的 UIScrollView 实体；非法/缺失时滚动条静默不画 (也不响应交互)。
    entt::entity target = entt::null;

    // 滑块最小像素尺寸：内容远大于视口时避免滑块退化成几像素难以拖动。
    float minThumbSize = 16.f;

    // 滑槽内边距：左右(或上下)留白，让滑块不顶到边。
    float trackPadding = 0.f;

    core::Color trackColor    = {  30,  30,  35, 200 };
    core::Color thumbColor    = { 120, 120, 130, 220 };
    core::Color thumbHover    = { 160, 160, 170, 240 };
    core::Color thumbPressed  = { 200, 200, 210, 255 };

    // 内容能完全装下视口 (contentLen <= viewportLen) 时是否不绘制
    bool autoHide = true;

    // ── 运行时 (由 UISystem 维护，业务勿手改) ──
    bool  dragging         = false;
    float dragStartPointer = 0.f;   // 按下时鼠标主轴像素坐标
    float dragStartScroll  = 0.f;   // 按下时 target 的 scrollX 或 scrollY
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

    // 可选标签：UIDropTarget 用 acceptedPayload 做粗粒度过滤（例如背包格子只
    // 接受 "item"，技能槽只接受 "skill"）。空字符串 = 不参与匹配。
    std::string payload;

    // 若拖拽以"成功投放到 DropTarget"结束，UISystem 会在 onDragEnd 之前把
    // dropAccepted 置为 true，回调里可据此决定是否回弹到原位。
    bool  dropAccepted = false;       // 运行时
    // 拖拽开始前的 offsetX/Y，UISystem 记录用以"未命中 DropTarget 自动回弹"。
    float originX = 0.f;
    float originY = 0.f;
    bool  snapBackOnMiss = false;     // 未命中任意 DropTarget 时是否回原位

    std::function<void(float, float)> onDrag;
    std::function<void()>             onDragStart;
    std::function<void()>             onDragEnd;
};

// ── 投放目标 ────────────────────────────────────────────────────────────────
// 给一个 UINode 加上 UIDropTarget 后，它就成为可接收 UIDraggable 投放的区域。
// 拖拽过程中 UISystem 每帧做命中测试（鼠标坐标 vs DropTarget 屏幕矩形），找到
// 最上层(sortOrder 最大)且接受当前 payload 的目标，触发 onDragEnter/Leave。
// 鼠标抬起时若仍命中目标 → 触发 onDrop，并把被拖元素的 UIDraggable.dropAccepted
// 置为 true。
//
// 过滤规则（依次判断，前者通过才轮到下一个）：
//   1) acceptedPayload 非空时，要求 UIDraggable.payload == acceptedPayload；
//   2) canAccept 回调存在时，调用并要求返回 true。
// 两者都为空/未提供 → 默认接受任意拖拽。
//
// 视觉反馈：highlightOnHover=true 时，UISystem 在节点之上叠一层 highlightColor
// 矩形；不需要时关掉自行用 onDragEnter/Leave 改 UIBackground。
struct UIDropTarget {
    std::string acceptedPayload;     // 空 = 不按 payload 过滤
    std::function<bool(entt::entity)> canAccept;       // (draggable) -> 是否接受
    std::function<void(entt::entity)> onDragEnter;     // (draggable)
    std::function<void(entt::entity)> onDragLeave;     // (draggable)
    std::function<void(entt::entity)> onDrop;          // (draggable)

    bool        highlightOnHover = true;
    core::Color highlightColor   = {255, 255, 255, 60};

    // 运行时：当前是否正有可接受的拖拽元素悬停在我之上
    bool         hovering        = false;
    entt::entity hoveringEntity  = entt::null;
};

// ── 文本输入框 ───────────────────────────────────────────────────────────────
// 单行文本输入框，支持点击聚焦、光标导航、字符插入/删除、选中高亮、占位文字、
// 密码模式、最大长度限制、水平滚动（长文本超出输入框宽度时自动滚动到光标所在处）。
//
// 键盘交互：
//   Printable ASCII     → 插入字符（选中时替换选中文本）
//   Backspace           → 删除光标前一个字符（选中时删除选中文本）
//   Delete              → 删除光标后一个字符（选中时删除选中文本）
//   ← / →               → 光标左/右移动一位（Shift+方向键扩展选中范围）
//   Home / End          → 光标跳到开头/末尾（Shift+Home/End 扩展选中范围）
//   Enter               → 触发 onSubmitted 回调
//   Escape              → 失焦（不触发 onBlur 回调？按 Escape 应当主动 blur）
//   Ctrl+V              → 从系统剪贴板粘贴文本
//   Ctrl+A              → 全选
//
// 渲染层（由 UISystem::emitNodeVisuals 处理）：
//   1. 背景矩形（聚焦/失焦两种颜色）
//   2. 边框（聚焦/失焦两种颜色）
//   3. 文本内容（裁剪到 padding 后的内区域，水平滚动时 scrollOffsetX 偏移）
//   4. 选中矩形（从 selectionStart 到 cursorPos）
//   5. 光标竖线（聚焦时闪烁，间隔 0.5s）
//   6. 占位文字（文本为空且未聚焦时显示，样式用 placeholderColor）
struct UITextInput {
    // ── 数据 ──
    std::string text;                 // 当前文本（UTF-8 编码）
    std::string placeholder;          // 空时显示的占位文字
    size_t      cursorPos       = 0;  // 光标位置（byte 偏移，始终指向 UTF-8 字符边界）
    size_t      selectionStart  = std::string::npos; // 选中锚点 byte 偏移，npos=无选中

    // ── 视觉 ──
    FontHandle  font;
    float       fontSize            = 16.f;
    core::Color textColor           = { 220, 220, 220, 255 };
    core::Color placeholderColor    = { 120, 120, 120, 255 };
    core::Color caretColor          = { 255, 255, 255, 255 };
    core::Color selectionColor      = { 60, 100, 180, 140 };
    core::Color bgColor             = { 25, 25, 30, 255 };
    core::Color focusedBgColor      = { 30, 35, 45, 255 };
    core::Color borderColor         = { 70, 70, 80, 255 };
    core::Color focusedBorderColor  = { 110, 140, 210, 255 };
    float       borderWidth         = 1.5f;
    float       paddingX            = 6.f;
    float       paddingY            = 3.f;

    // ── 行为限制 ──
    uint32_t    maxLength           = UINT32_MAX;
    bool        passwordMode        = false;
    char        passwordChar        = '*';
    bool        readOnly            = false;

    // ── 回调 ──
    std::function<void(const std::string&)> onChanged;    // 文本变化时触发
    std::function<void(const std::string&)> onSubmitted;  // 按 Enter 时触发
    std::function<void()>                   onFocus;      // 获得焦点时
    std::function<void()>                   onBlur;       // 失去焦点时

    // ── 运行时（由 UISystem 维护） ──
    bool        focused         = false;
    float       caretTimer      = 0.f;   // 累计时间（秒），用于光标闪烁
    float       scrollOffsetX   = 0.f;   // 文本水平滚动偏移（像素）
};

} // namespace engine
