#include "SceneSystem.h"
#include "../runtime/EngineContext.h"
#include "../scene/SceneSerializer.h"

namespace engine {

void SceneSystem::preUpdate() {
    if (pendingLoadPath_.empty()) return;

    std::string path = pendingLoadPath_;
    pendingLoadPath_.clear();
    loadScene(path.c_str());
}

bool SceneSystem::loadScene(const char* path) {
    if (!path || !path[0]) return false;

    if (!SceneSerializer::loadScene(ctx_.world, ctx_.assetManager, path)) {
        return false;
    }

    currentScenePath_ = path;
    return true;
}

bool SceneSystem::saveScene(const char* path) {
    if (!path || !path[0]) return false;
    return SceneSerializer::saveScene(ctx_.world, ctx_.assetManager, path);
}

void SceneSystem::unloadScene() {
    ctx_.world.clear();
    currentScenePath_.clear();
    pendingLoadPath_.clear();
}

void SceneSystem::requestLoadScene(const char* path) {
    pendingLoadPath_ = path ? path : "";
}

} // namespace engine
