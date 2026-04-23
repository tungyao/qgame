#pragma once

namespace engine {

class InputState;

// 输入数据提供者抽象 — 封装"从哪里拿事件"的平台差异
// 桌面端：SDLInputProvider（键盘 + 鼠标）
// 移动端：MobileInputProvider（触摸 + 加速度计 + 手势）
class IInputProvider {
public:
    virtual ~IInputProvider() = default;

    // 泵一帧事件填充 out（内部先调 out.beginFrame()）
    // 返回 false 表示收到退出信号，FrameScheduler 应终止主循环
    virtual bool poll(InputState& out) = 0;
};

} // namespace engine
