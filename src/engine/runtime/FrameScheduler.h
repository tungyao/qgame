#pragma once
#include <cstdint>

namespace engine {

class EngineContext;

// 驱动单帧，严格按架构文档的 8 步顺序执行
class FrameScheduler {
public:
    explicit FrameScheduler(EngineContext& ctx) : ctx_(ctx) {}

    // 执行一帧，返回 false 表示应退出
    bool tick();

    uint64_t frameCount() const { return frameCount_; }
    float    deltaTime()  const { return lastDt_; }

private:
    EngineContext& ctx_;
    uint64_t       frameCount_ = 0;
    uint64_t       lastTick_   = 0;   // SDL_GetTicks64() 上一帧时间戳
    float          lastDt_     = 0.f;
};

} // namespace engine
