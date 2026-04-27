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
                    float parentW, float parentH);
    void runInteraction(InputState& input);

    // 命令缓存构建
    void buildCommands();
    void emitRect(float x, float y, float w, float h,
                  core::Color tint, TextureHandle tex,
                  core::Rect src, int sortKey);
    void emitText(const UILabel& label, const UINode& node, int sortKey);

    // 世界相机参数（取第一个含 World layer 的相机）
    void getWorldCamera(float& camX, float& camY, float& zoom) const;

    EngineContext& ctx_;

    TextureHandle whiteTexture_{};

    entt::entity hovered_ = entt::null;
    entt::entity pressed_ = entt::null;
    bool prevPointerDown_ = false;

    float screenW_ = 1920.f, screenH_ = 1080.f;

    // 本帧命令缓存
    mutable std::vector<backend::RenderCmd> uiCommands_;
};

} // namespace engine
