#pragma once
#include "ISystem.h"

namespace engine {
class EngineContext;

class AudioSystem final : public ISystem {
public:
    explicit AudioSystem(EngineContext& ctx) : ctx_(ctx) {}

    void init()           override;
    void update(float dt) override;
    void shutdown()       override;

private:
    EngineContext& ctx_;
};

} // namespace engine
