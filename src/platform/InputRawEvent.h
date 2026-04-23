#pragma once

namespace platform {

// SDL 原始事件 → 平台无关结构
// 触摸和鼠标统一为 POINTER_* 事件
struct InputRawEvent {
    enum class Type {
        POINTER_DOWN,
        POINTER_MOVE,
        POINTER_UP,
        KEY_DOWN,
        KEY_UP,
        GAMEPAD_BUTTON,
        GAMEPAD_AXIS,
        QUIT,
    };

    Type  type;
    int   pointerId = 0;   // 多点触摸 ID，鼠标固定为 0
    float x = 0.f, y = 0.f;  // 归一化坐标 [0,1]
    int   keyCode = 0;
    int   gamepadButton = 0;
    float gamepadAxis = 0.f;
};

} // namespace platform
