#include "SceneManager.h"

#include "GameContext.h"
#include "../scene/SceneSerializer.h"
#include "../../core/Logger.h"

namespace engine {

SceneManager::SceneManager(GameContext& ctx)
    : ctx_(ctx) {
    // 回填到 GameContext，让 GameInstance / 后续 Native API 都能从同一个上下文
    // 找到场景门面。SceneManager 不由 GameContext 拥有，宿主仍负责生命周期。
    ctx_.scenes = this;
}

bool SceneManager::registerScene(const std::string& id, const std::string& path) {
    if (id.empty() || path.empty()) {
        core::logWarn("[SceneManager] ignored scene with empty id/path");
        return false;
    }

    scenes_[id] = SceneDesc{id, path};
    core::logInfo("[SceneManager] registered scene %s -> %s", id.c_str(), path.c_str());
    return true;
}

bool SceneManager::loadScene(const std::string& id) {
    const SceneDesc* desc = findScene(id);
    if (!desc) {
        core::logError("[SceneManager] scene id not registered: %s", id.c_str());
        return false;
    }

    if (!SceneSerializer::loadScene(ctx_.world, ctx_.assets, desc->path)) {
        core::logError("[SceneManager] failed to load scene %s from %s",
                       id.c_str(), desc->path.c_str());
        return false;
    }

    currentSceneId_ = desc->id;
    currentScenePath_ = desc->path;
    core::logInfo("[SceneManager] loaded scene %s", currentSceneId_.c_str());
    return true;
}

bool SceneManager::loadScenePath(const std::string& path) {
    if (path.empty()) {
        core::logWarn("[SceneManager] ignored empty scene path");
        return false;
    }

    if (!SceneSerializer::loadScene(ctx_.world, ctx_.assets, path)) {
        core::logError("[SceneManager] failed to load scene path %s", path.c_str());
        return false;
    }

    currentSceneId_.clear();
    currentScenePath_ = path;
    core::logInfo("[SceneManager] loaded scene path %s", currentScenePath_.c_str());
    return true;
}

void SceneManager::unloadScene() {
    // Scene entities live in the shared world registry. Clearing the registry is
    // intentionally explicit here so callers can see that entity handles from the
    // previous scene become invalid after unload.
    ctx_.world.clear();
    currentSceneId_.clear();
    currentScenePath_.clear();
}

bool SceneManager::hasScene(const std::string& id) const {
    return scenes_.find(id) != scenes_.end();
}

const SceneDesc* SceneManager::findScene(const std::string& id) const {
    auto it = scenes_.find(id);
    return it != scenes_.end() ? &it->second : nullptr;
}

std::vector<std::string> SceneManager::sceneIds() const {
    std::vector<std::string> ids;
    ids.reserve(scenes_.size());
    for (const auto& kv : scenes_) {
        ids.push_back(kv.first);
    }
    return ids;
}

} // namespace engine
