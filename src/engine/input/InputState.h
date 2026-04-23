#pragma once
#include <unordered_set>
#include <vector>
#include "../../platform/InputRawEvent.h"

namespace engine {

// 高层输入状态 — 按帧管理 just* 状态，供 InputSystem / GameAPI 查询
class InputState {
public:
    static constexpr int MAX_POINTERS = 10;

    // IInputProvider::poll() 开头调用：清除上帧的 just* 状态和事件列表
    void beginFrame() {
        justPressed_.clear();
        justReleased_.clear();
        frameEvents_.clear();
    }

    // IInputProvider::poll() 的 pollEvents 回调里调用
    void feedEvent(const platform::InputRawEvent& e) {
        frameEvents_.push_back(e);
        using T = platform::InputRawEvent::Type;
        switch (e.type) {
        case T::KEY_DOWN:
            if (!keysDown_.count(e.keyCode)) {
                justPressed_.insert(e.keyCode);
                keysDown_.insert(e.keyCode);
            }
            break;
        case T::KEY_UP:
            keysDown_.erase(e.keyCode);
            justReleased_.insert(e.keyCode);
            break;
        case T::POINTER_DOWN:
            if (e.pointerId >= 0 && e.pointerId < MAX_POINTERS) {
                pointers_[e.pointerId] = {true, e.x, e.y};
            }
            break;
        case T::POINTER_UP:
            if (e.pointerId >= 0 && e.pointerId < MAX_POINTERS) {
                pointers_[e.pointerId].down = false;
            }
            break;
        case T::POINTER_MOVE:
            if (e.pointerId >= 0 && e.pointerId < MAX_POINTERS) {
                pointers_[e.pointerId].x = e.x;
                pointers_[e.pointerId].y = e.y;
            }
            break;
        default: break;
        }
    }

    // 本帧收到的全部原始事件（调试/编辑器用）
    const std::vector<platform::InputRawEvent>& frameEvents() const { return frameEvents_; }

    bool isKeyDown(int keyCode)        const { return keysDown_.count(keyCode) > 0; }
    bool isKeyJustPressed(int keyCode) const { return justPressed_.count(keyCode) > 0; }
    bool isKeyJustReleased(int keyCode)const { return justReleased_.count(keyCode) > 0; }

    bool  pointerDown(int id = 0) const {
        return (id >= 0 && id < MAX_POINTERS) && pointers_[id].down;
    }
    float pointerX(int id = 0) const {
        return (id >= 0 && id < MAX_POINTERS) ? pointers_[id].x : 0.f;
    }
    float pointerY(int id = 0) const {
        return (id >= 0 && id < MAX_POINTERS) ? pointers_[id].y : 0.f;
    }

private:
    std::unordered_set<int>              keysDown_;
    std::unordered_set<int>              justPressed_;
    std::unordered_set<int>              justReleased_;
    std::vector<platform::InputRawEvent> frameEvents_;

    struct PointerState { bool down = false; float x = 0.f, y = 0.f; };
    PointerState pointers_[MAX_POINTERS] = {};
};

} // namespace engine
