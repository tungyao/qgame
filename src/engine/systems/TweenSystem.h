#pragma once
#include "ISystem.h"

namespace engine {
class EngineContext;

// Phase 5.1: Tween 系统 — 推进所有 entity 的 TweenComponent，将值写回目标通道
class TweenSystem final : public ISystem {
public:
    explicit TweenSystem(EngineContext& ctx) : ctx_(ctx) {}

    void update(float dt) override;

private:
    EngineContext& ctx_;
};

} // namespace engine
