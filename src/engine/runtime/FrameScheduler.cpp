#include "FrameScheduler.h"

#include <SDL3/SDL.h>

#include "EngineContext.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../core/Logger.h"

namespace engine {

bool FrameScheduler::executePhase(UpdatePhase phase, float dt) {
    // 调度器按注册顺序扫描系统，但是否在当前 phase 执行由 system.phaseMask()
    // 决定。这样“阶段”决定粗粒度时序，“注册顺序”只负责同阶段内的稳定次序。
    const UpdatePhaseMask phaseBit = updatePhaseBit(phase);
    for (const auto& system : ctx_.systems.systems()) {
        if ((system->phaseMask() & phaseBit) == 0u) continue;
        if (!system->runPhase(phase, dt)) {
            return false;
        }
    }
    return true;
}

bool FrameScheduler::tick() {
    const uint64_t now = SDL_GetTicks();
    if (lastTick_ == 0) {
        lastTick_ = now;
    }

    float dt = static_cast<float>(now - lastTick_) / 1000.0f;
    if (dt > 0.1f) {
        dt = 0.1f;
    }

    lastTick_ = now;
    lastDt_ = dt;
    ctx_.deltaTime = dt;
    ctx_.frameCounter = frameCount_;

    ctx_.renderDevice().beginFrame();

    // 显式阶段顺序。这个数组就是运行时调度契约：
    // - GameplayPrePhysics 在 Physics 前
    // - Camera 在 UI/Render 前
    // - PostFrame 在 present 之后
    constexpr UpdatePhase kPhaseOrder[] = {
        UpdatePhase::Input,
        UpdatePhase::GameplayPrePhysics,
        UpdatePhase::Physics,
        UpdatePhase::GameplayPostPhysics,
        UpdatePhase::Animation,
        UpdatePhase::Camera,
        UpdatePhase::UI,
        UpdatePhase::Render
    };

    for (UpdatePhase phase : kPhaseOrder) {
        if (!executePhase(phase, dt)) {
            ctx_.renderDevice().endFrame();
            return false;
        }

        // Input phase 可能在本帧消费了 SDL 的 resize 事件。这里立刻把窗口尺寸
        // 同步到 EngineContext 的公开缓存里，这样后续 Gameplay/Camera/UI phase
        // 读取到的就是“本帧已经更新后的尺寸”，不会再晚一帧。
        if (phase == UpdatePhase::Input && ctx_.window) {
            ctx_.windowWidth = ctx_.window->width();
            ctx_.windowHeight = ctx_.window->height();
        }
    }

    // beforePresentCallback 仍保留为“最后一个 CPU 钩子”：
    // 所有显式 phase（包括 Render）都已结束，但 swapchain 还没 present。
    // 这适合 editor/debug overlay 读取本帧最终结果做轻量收尾。
    if (ctx_.beforePresentCallback) {
        ctx_.beforePresentCallback();
    }

    ctx_.renderDevice().present();

    if (!executePhase(UpdatePhase::PostFrame, dt)) {
        ctx_.renderDevice().endFrame();
        return false;
    }

    ctx_.renderDevice().endFrame();

    if (frameCount_ > 0 && frameCount_ % 300 == 0 && dt > 0.0f) {
        const backend::RenderFrameStats& rs = ctx_.renderDevice().frameStats();
        core::logInfo(
            "frame %llu  fps %.1f  render=%s sprites=%u visible=%u draws=%u gpuBatches=%u uploads=%u/%lluB compute=%u%s%s",
            static_cast<unsigned long long>(frameCount_),
            1.0f / dt,
            backend::renderPathName(rs.path),
            rs.spriteCount,
            rs.visibleSpriteCount,
            rs.drawCallCount,
            rs.gpuDrawBatchCount,
            rs.uploadCallCount,
            static_cast<unsigned long long>(rs.uploadBytes),
            rs.computeDispatchCount,
            rs.fallbackReason ? " fallback=" : "",
            rs.fallbackReason ? rs.fallbackReason : "");
    }

    ++frameCount_;
    return true;
}

} // namespace engine
