#pragma once

#include "ISystem.h"
#include "../components/UIComponents.h"
#include "../../backend/shared/ResourceHandle.h"
#include "../../backend/renderer/CommandBuffer.h"
#include <entt/entt.hpp>
#include <vector>

namespace engine {

class EngineContext;
class InputState;

// ═══════════════════════════════════════════════════════════════════════════════
// UISystem v2
// ───────────────────────────────────────────────────────────────────────────────
// 每帧 update() 做：
//   1. 更新 Canvas 缩放
//   2. 自顶向下计算每个 UINode 的屏幕矩形（含世界锚点投影）
//   3. 处理指针事件、拖拽、滑动条、开关、按钮回调
//   4. 准备本帧的 UI 绘制命令缓存（DrawSpriteCmd / DrawTextCmd）
//
// RenderSystem 负责消费这些命令——既支持 CPU 路径（直接附加到 cb）也支持 GPU-driven
// 路径（作为非精灵 drawable 注入）。
// ═══════════════════════════════════════════════════════════════════════════════

class UISystem final : public ISystem {
public:
    explicit UISystem(EngineContext& ctx);
    ~UISystem() override;

    void init() override;
    void update(float dt) override;
    void shutdown() override;

    // 把本帧的 UI 命令直接 append 到 CommandBuffer 末尾（CPU 路径）。
    void emitDrawCommands(backend::CommandBuffer& cb) const;

    // GPU-driven 路径用：把本帧 UI 命令以指针形式追加到外部容器。
    void appendDrawCommandPtrs(std::vector<const backend::RenderCmd*>& out) const;

    // 状态查询
    entt::entity hovered() const { return hovered_; }
    entt::entity pressed() const { return pressed_; }

    // 把屏幕像素坐标转成 Screen 相机的 world 坐标（左上角对齐）。
    // 暴露给外部以便测试/调试。
    void screenToCamera(float sx, float sy, float& cx, float& cy) const;

    // 1×1 白色纹理（纯色矩形用）。
    TextureHandle whiteTexture() const { return whiteTexture_; }

private:
    void ensureWhiteTexture();
    void updateCanvases();
    void runLayout();
    void layoutNode(entt::entity e, float parentX, float parentY,
                    float parentW, float parentH,
                    float scrollOffX, float scrollOffY);
    void measureScrollContent(entt::entity sv);
    // 按 UILayoutGroup 设定为 e 的直接子节点逐个计算屏幕矩形并递归。
    // scrollOffX/Y 是 e 自身因父级 ScrollView 产生的滚动偏移（透传给孙节点）。
    void applyLayoutGroup(entt::entity e, float scrollOffX, float scrollOffY);
    void runInteraction(InputState& input);

    // tooltip：每帧累计悬停时间；超过 delay 时由 buildCommands 末尾绘制
    void updateTooltip(float dt);
    void emitTooltipIfAny();

    // 命令缓存构建
    void buildCommands();
    void buildNodeCommands(entt::entity e, int& seq, int parentBaseSort);
    void emitNodeVisuals(entt::entity e, int baseSort);
    void emitRect(float x, float y, float w, float h,
                  core::Color tint, TextureHandle tex,
                  core::Rect src, int sortKey);
    void emitText(const UILabel& label, const UINode& node, int sortKey);
    // 把一个九宫格切成 9 个 emitRect 调用（节点矩形 < 边框总和时自动夹紧避免负宽）
    void emitNineSlice(const UINineSlice& ns, const UINode& node, int sortKey);

    // 命中过滤：检查 (px,py) 是否在 e 的所有祖先 ScrollView 视口内
    bool insideScissorAncestors(entt::entity e, float px, float py) const;

    // 世界相机参数（取第一个含 World layer 的相机）
    void getWorldCamera(float& camX, float& camY, float& zoom) const;

    // ── TextInput 字形度量（用 FontData::Glyph::advance 替代 fontSize*0.55 估算）─
    // 获取 TextInput 的 FontData，nullptr 表示不可用（调用方回退估算）
    const engine::FontData* getTextInputFontData(const UITextInput& ti) const;
    // 查询单个 glyph 的像素 advance；fd==nullptr 时按 fontSize*0.55f 回退
    float textInputGlyphAdvance(const engine::FontData* fd,
                                const UITextInput& ti, uint32_t codepoint) const;
    // 一次性计算文本总像素宽度 & 光标像素 X 位置（O(n)，n=文本长度）
    void textInputMetrics(const UITextInput& ti,
                          float& outTextPixelW, float& outCursorPixelX) const;
    // 由像素 X 反查光标字符索引（鼠标点击定位用）
    size_t textInputIndexAtX(const UITextInput& ti, float pixelX) const;

    EngineContext& ctx_;

    TextureHandle whiteTexture_{};

    entt::entity hovered_ = entt::null;
    entt::entity pressed_ = entt::null;
    bool prevPointerDown_ = false;

    // tooltip：跟踪当前悬停的 tooltip 触发器和累计停留时间
    entt::entity tooltipHovered_ = entt::null;
    float        tooltipHoverT_  = 0.f;

    // drag-and-drop：当前拖拽元素正在悬停的 DropTarget（用来跨帧产生
    // onDragEnter/Leave，并在抬起时找到最终落点）
    entt::entity currentDropTarget_ = entt::null;
    float        pointerSx_ = 0.f, pointerSy_ = 0.f;   // 上一帧鼠标屏幕像素

    float screenW_ = 1920.f, screenH_ = 1080.f;

    // 本帧命令缓存
    mutable std::vector<backend::RenderCmd> uiCommands_;
};

} // namespace engine
