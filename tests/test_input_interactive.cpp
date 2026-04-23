// 交互式输入测试 — 打开 SDL 窗口，实时打印所有输入事件
// 操作：
//   任意键盘按键  → 显示键名 + just-pressed / just-released
//   WASD/方向键   → [held] 行显示持续按压
//   鼠标点击/移动 → 显示归一化坐标
//   ESC 或关窗    → 退出

#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <memory>

#include "engine/input/InputState.h"
#include "engine/input/IInputProvider.h"
#include "engine/input/SDLInputProvider.h"
#include "platform/InputRawEvent.h"
#include "platform/Window.h"

static const char* keyName(int code) {
    return SDL_GetKeyName(static_cast<SDL_Keycode>(code));
}

int main(int /*argc*/, char* /*argv*/[]) {
    platform::WindowConfig wcfg;
    wcfg.title     = "StarEngine — Interactive Input Test";
    wcfg.width     = 800;
    wcfg.height    = 480;
    wcfg.resizable = false;

    platform::Window window(wcfg);

    engine::InputState state;
    std::unique_ptr<engine::IInputProvider> provider =
        std::make_unique<engine::SDLInputProvider>(window);

    ::printf("========================================\n");
    ::printf("  StarEngine Interactive Input Test\n");
    ::printf("  [provider: SDLInputProvider]\n");
    ::printf("  Press any key / click / move mouse\n");
    ::printf("  ESC or close window to quit\n");
    ::printf("========================================\n\n");

    const int kWatchKeys[] = {
        SDLK_W, SDLK_A, SDLK_S, SDLK_D,
        SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
        SDLK_SPACE, SDLK_RETURN, SDLK_LSHIFT,
    };

    float lastPtrX = -1.f, lastPtrY = -1.f;

    while (provider->poll(state)) {
        using T = platform::InputRawEvent::Type;

        // ── 遍历本帧所有原始事件 ──────────────────────────────────────────────
        for (const auto& e : state.frameEvents()) {
            switch (e.type) {
            case T::KEY_DOWN:
                // 过滤 SDL key-repeat：只在 justPressed 时打印
                if (state.isKeyJustPressed(e.keyCode))
                    ::printf("[KEY  DOWN ]  %-20s  code=%-6d\n",
                             keyName(e.keyCode), e.keyCode);
                break;

            case T::KEY_UP:
                ::printf("[KEY  UP   ]  %-20s  code=%-6d\n",
                         keyName(e.keyCode), e.keyCode);
                break;

            case T::POINTER_DOWN:
                ::printf("[PTR  DOWN ]  id=%-2d  x=%.3f  y=%.3f\n",
                         e.pointerId, e.x, e.y);
                lastPtrX = e.x; lastPtrY = e.y;
                break;

            case T::POINTER_UP:
                ::printf("[PTR  UP   ]  id=%-2d  x=%.3f  y=%.3f\n",
                         e.pointerId, e.x, e.y);
                lastPtrX = -1.f; lastPtrY = -1.f;
                break;

            case T::POINTER_MOVE: {
                float dx = e.x - lastPtrX, dy = e.y - lastPtrY;
                if (lastPtrX < 0.f || dx*dx + dy*dy > 0.0001f) {
                    ::printf("[PTR  MOVE ]  id=%-2d  x=%.3f  y=%.3f\n",
                             e.pointerId, e.x, e.y);
                    lastPtrX = e.x; lastPtrY = e.y;
                }
                break;
            }

            default: break;
            }
        }

        // ── 持续按键汇总（每帧一行，有才打印）────────────────────────────────
        char held[256] = {}; int heldLen = 0;
        for (int k : kWatchKeys) {
            if (state.isKeyDown(k))
                heldLen += ::snprintf(held + heldLen, sizeof(held) - heldLen,
                                      "%s ", keyName(k));
        }
        if (heldLen > 0)
            ::printf("[HELD      ]  %s\n", held);

        if (state.isKeyJustReleased(SDLK_ESCAPE)) break;

        SDL_Delay(16);
    }

    ::printf("\nBye.\n");
    return 0;
}
