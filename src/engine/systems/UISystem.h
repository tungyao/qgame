#pragma once

#include "ISystem.h"
#include "../components/UIComponents.h"
#include <entt/entt.hpp>

namespace engine {

class InputState;
class EngineContext;

// ═══════════════════════════════════════════════════════════════════════════════
// UISystem - UI 交互处理系统
// ───────────────────────────────────────────────────────────────────────────────
// 负责：
// 1. 计算 UI 元素布局 (锚点、偏移)
// 2. 处理交互事件 (点击、拖拽)
// 3. 更新交互状态 (hovered、pressed)
// 4. 触发回调函数
// ═══════════════════════════════════════════════════════════════════════════════

class UISystem : public ISystem {
public:
    explicit UISystem(EngineContext& ctx);
    ~UISystem() override = default;
    
    // ── ISystem 接口 ────────────────────────────────────────────────────────────
    void update(float dt) override;
    
    // ── 公共接口 ────────────────────────────────────────────────────────────────
    
    // 获取当前悬停的 UI 元素
    entt::entity getHoveredElement() const { return hoveredEntity_; }
    
    // 获取当前按下的 UI 元素
    entt::entity getPressedElement() const { return pressedEntity_; }
    
    // 获取当前焦点的 UI 元素 (键盘导航用)
    entt::entity getFocusedElement() const { return focusedEntity_; }
    void setFocusedElement(entt::entity e) { focusedEntity_ = e; }
    
    // 强制刷新布局 (修改锚点后调用)
    void refreshLayout();
    
    // 检测点是否在元素内
    bool isPointInElement(float px, float py, entt::entity e) const;
    
private:
    // ── 内部方法 ────────────────────────────────────────────────────────────────
    
    // 计算单个元素的布局
    void computeElementLayout(entt::entity e, entt::registry& world);
    
    // 计算元素的世界坐标 (考虑父节点)
    void computeWorldPosition(entt::entity e, entt::registry& world,
                              float parentX, float parentY,
                              float parentW, float parentH);
    
    // 更新 Canvas 缩放因子
    void updateCanvasScale(entt::entity canvasEntity, Canvas& canvas);
    
    // 处理指针事件
    void processPointerEvents(entt::registry& world, InputState& input);
    
    // 处理按钮点击
    void handleButton(entt::entity e, Button& btn, UIElement& elem);
    
    // 处理开关切换
    void handleToggle(entt::entity e, Toggle& toggle, UIElement& elem);
    
    // 处理滑动条
    void handleSlider(entt::entity e, Slider& slider, UIElement& elem, InputState& input);
    
    // 处理拖拽
    void handleDrag(entt::entity e, DragHandler& drag, UIElement& elem, InputState& input);
    
    // 按 sortOrder 排序 UI 元素
    void sortUIElements(entt::registry& world);
    
    // ── 成员变量 ────────────────────────────────────────────────────────────────
    EngineContext& ctx_;
    
    entt::entity hoveredEntity_  = entt::null;  // 当前悬停元素
    entt::entity pressedEntity_  = entt::null;  // 当前按下元素
    entt::entity focusedEntity_  = entt::null;  // 当前焦点元素
    entt::entity draggingEntity_ = entt::null;  // 当前拖拽元素
    
    float screenW_ = 1920.f;  // 当前屏幕宽度
    float screenH_ = 1080.f;  // 当前屏幕高度
    
    bool prevPointerDown_ = false;  // 上一帧指针状态 (用于检测 justPressed)
};

} // namespace engine
