#include "FrameScheduler.h"
#include "EngineContext.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../core/Logger.h"
#include "../systems/PhysicsSystem.h"
#include "../systems/InputSystem.h"
#include <SDL3/SDL.h>

namespace engine {

bool FrameScheduler::tick() {
    // ── 计时 ─────────────────────────────────────────────────────────────────
    uint64_t now = SDL_GetTicks();
    if (lastTick_ == 0) lastTick_ = now;
    float dt = static_cast<float>(now - lastTick_) / 1000.f;
    if (dt > 0.1f) dt = 0.1f;
    lastTick_ = now;
    lastDt_   = dt;
    ctx_.deltaTime    = dt;
    ctx_.frameCounter = frameCount_;

    // ── Step 1: 采集输入 ──────────────────────────────────────────────────────
    if (ctx_.systems.has<InputSystem>()) {
        if (!ctx_.systems.get<InputSystem>().pollInput()) return false;
    }

    // ── Step 1b: 开始渲染帧（获取 swapchain 纹理）────────────────────────────
    ctx_.renderDevice().beginFrame();

    for (auto& s : ctx_.systems.systems()) s->preUpdate();

    // ── Step 2: 物理 ─────────────────────────────────────────────────────────
    if (ctx_.systems.has<PhysicsSystem>())
        ctx_.systems.get<PhysicsSystem>().update(dt);

    // ── Step 3: 游戏逻辑 ─────────────────────────────────────────────────────
    // isManuallyScheduled() 的 system（如 PhysicsSystem）已在特定步骤显式调用
    for (auto& s : ctx_.systems.systems())
        if (!s->isManuallyScheduled()) s->update(dt);

    // ── Step 4: 音频命令提交 (Month 4) ──────────────────────────────────────

    // ── Step 8: 清除单帧状态 ─────────────────────────────────────────────────
    for (auto& s : ctx_.systems.systems()) s->postUpdate();

    ctx_.renderDevice().endFrame();

    if (frameCount_ > 0 && frameCount_ % 300 == 0 && dt > 0.f) {
        core::logInfo("frame %llu  fps %.1f", (unsigned long long)frameCount_, 1.f / dt);
    }

    ++frameCount_;
    return true;
}

} // namespace engine
