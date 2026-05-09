#pragma once
#include "ISystem.h"
#include <string>

namespace engine {
class EngineContext;

class SceneSystem final : public ISystem {
public:
    explicit SceneSystem(EngineContext& ctx) : ctx_(ctx) {}

    UpdatePhaseMask phaseMask() const override {
        return updatePhaseBit(UpdatePhase::GameplayPrePhysics);
    }

    void preUpdate() override;

    bool loadScene(const char* path);
    bool saveScene(const char* path);
    void unloadScene();

    void requestLoadScene(const char* path);
    const std::string& currentScenePath() const { return currentScenePath_; }

private:
    EngineContext& ctx_;
    std::string currentScenePath_;
    std::string pendingLoadPath_;
};

} // namespace engine
