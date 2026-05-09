#pragma once
#include <memory>
#include "ISystem.h"
#include "../input/IInputProvider.h"

namespace engine {

class InputState;

// InputSystem — 拥有 IInputProvider，由 FrameScheduler Step 1 显式驱动
// 替换 provider 即可切换平台输入源（桌面 / 移动 / 自动化测试）
class InputSystem : public ISystem {
public:
    InputSystem(InputState& state, std::unique_ptr<IInputProvider> provider);

    UpdatePhaseMask phaseMask() const override {
        return updatePhaseBit(UpdatePhase::Input);
    }

    // Input phase 调用：泵事件 → 填充 InputState
    // 返回 false 表示收到退出信号
    bool pollInput();

    // 运行时替换 provider（如切换为移动端 provider）
    void setProvider(std::unique_ptr<IInputProvider> provider);

private:
    bool onInputPhase(float dt) override;

    InputState&                     state_;
    std::unique_ptr<IInputProvider> provider_;
};

} // namespace engine
