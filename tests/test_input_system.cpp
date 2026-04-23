#include <cassert>
#include <cstdio>

#include "engine/input/InputState.h"
#include "platform/InputRawEvent.h"

using engine::InputState;
using platform::InputRawEvent;

static InputRawEvent makeKey(InputRawEvent::Type t, int code) {
    InputRawEvent e{};
    e.type    = t;
    e.keyCode = code;
    return e;
}

static InputRawEvent makePointer(InputRawEvent::Type t, int id, float x, float y) {
    InputRawEvent e{};
    e.type      = t;
    e.pointerId = id;
    e.x = x; e.y = y;
    return e;
}

// ── 测试：按键状态跳变 ─────────────────────────────────────────────────────────

void testKeyJustPressed() {
    InputState s;

    // 帧 1: 按下 A
    s.beginFrame();
    s.feedEvent(makeKey(InputRawEvent::Type::KEY_DOWN, 65));
    assert(s.isKeyDown(65));
    assert(s.isKeyJustPressed(65));
    assert(!s.isKeyJustReleased(65));

    // 帧 2: 仍然按着 A
    s.beginFrame();
    s.feedEvent(makeKey(InputRawEvent::Type::KEY_DOWN, 65)); // SDL 重复 KEY_DOWN
    assert(s.isKeyDown(65));
    assert(!s.isKeyJustPressed(65));   // 已不是"刚按"

    // 帧 3: 松开 A
    s.beginFrame();
    s.feedEvent(makeKey(InputRawEvent::Type::KEY_UP, 65));
    assert(!s.isKeyDown(65));
    assert(!s.isKeyJustPressed(65));
    assert(s.isKeyJustReleased(65));

    // 帧 4: 无事件
    s.beginFrame();
    assert(!s.isKeyDown(65));
    assert(!s.isKeyJustReleased(65));

    ::printf("  key just-pressed/released OK\n");
}

// ── 测试：多键同帧 ─────────────────────────────────────────────────────────────

void testMultiKey() {
    InputState s;
    s.beginFrame();
    s.feedEvent(makeKey(InputRawEvent::Type::KEY_DOWN, 10));
    s.feedEvent(makeKey(InputRawEvent::Type::KEY_DOWN, 20));
    assert(s.isKeyDown(10) && s.isKeyDown(20));
    assert(!s.isKeyDown(30));

    s.beginFrame();
    s.feedEvent(makeKey(InputRawEvent::Type::KEY_UP, 10));
    assert(!s.isKeyDown(10));
    assert(s.isKeyDown(20));
    assert(s.isKeyJustReleased(10));
    assert(!s.isKeyJustReleased(20));

    ::printf("  multi-key OK\n");
}

// ── 测试：指针状态 ────────────────────────────────────────────────────────────

void testPointer() {
    InputState s;
    s.beginFrame();
    s.feedEvent(makePointer(InputRawEvent::Type::POINTER_DOWN, 0, 0.3f, 0.7f));
    assert(s.pointerDown(0));
    assert(s.pointerX(0) == 0.3f);
    assert(s.pointerY(0) == 0.7f);
    assert(!s.pointerDown(1));  // 其他 id 未按下

    s.feedEvent(makePointer(InputRawEvent::Type::POINTER_MOVE, 0, 0.5f, 0.5f));
    assert(s.pointerX(0) == 0.5f && s.pointerY(0) == 0.5f);

    s.beginFrame();
    s.feedEvent(makePointer(InputRawEvent::Type::POINTER_UP, 0, 0.5f, 0.5f));
    assert(!s.pointerDown(0));

    ::printf("  pointer state OK\n");
}

// ── 测试：多点触摸 ────────────────────────────────────────────────────────────

void testMultiTouch() {
    InputState s;
    s.beginFrame();
    s.feedEvent(makePointer(InputRawEvent::Type::POINTER_DOWN, 0, 0.1f, 0.2f));
    s.feedEvent(makePointer(InputRawEvent::Type::POINTER_DOWN, 1, 0.8f, 0.9f));
    assert(s.pointerDown(0) && s.pointerDown(1));
    assert(s.pointerX(0) == 0.1f && s.pointerX(1) == 0.8f);

    ::printf("  multi-touch OK\n");
}

int main() {
    ::printf("=== InputState Tests ===\n");
    testKeyJustPressed();
    testMultiKey();
    testPointer();
    testMultiTouch();
    ::printf("All InputState tests passed.\n");
    return 0;
}
