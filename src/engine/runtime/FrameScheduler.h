#pragma once
#include <cstdint>
#include "../systems/ISystem.h"

namespace engine {

class EngineContext;

// 驱动单帧，按显式 phase 顺序执行。
//
// 调度器不再区分“手动系统 + 通用 update 循环”，而是统一执行：
//   Input -> GameplayPrePhysics -> Physics -> GameplayPostPhysics
//   -> Animation -> Camera -> UI -> Render -> present -> PostFrame
class FrameScheduler {
public:
    explicit FrameScheduler(EngineContext& ctx) : ctx_(ctx) {}

    // 执行一帧，返回 false 表示应退出
    bool tick();

    uint64_t frameCount() const { return frameCount_; }
    float    deltaTime()  const { return lastDt_; }

private:
    bool executePhase(UpdatePhase phase, float dt);

    EngineContext& ctx_;
    uint64_t       frameCount_ = 0;
    uint64_t       lastTick_   = 0;   // SDL_GetTicks() 返回的上一帧时间戳（毫秒）
    float          lastDt_     = 0.f;
};

} // namespace engine
