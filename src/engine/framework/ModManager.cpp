#include "ModManager.h"

#include "../assets/AssetManager.h"
#include "../../core/Logger.h"

#include <algorithm>
#include <filesystem>
#include <queue>
#include <set>
#include <unordered_set>

namespace engine {

namespace {

std::string normalizePath(const std::filesystem::path& path) {
    return path.lexically_normal().string();
}

std::filesystem::path resolveRelativeTo(const std::filesystem::path& baseDir,
                                        const std::string& path) {
    std::filesystem::path p(path);
    if (p.is_absolute()) return p.lexically_normal();
    return (baseDir / p).lexically_normal();
}

} // namespace

bool ModManager::scanMods(const std::string& modsDir) {
    discovered_.clear();
    mountedMods_.clear();
    modsDir_ = modsDir;

    const std::filesystem::path root(modsDir);
    if (!std::filesystem::exists(root)) {
        core::logInfo("[ModManager] mods directory not found, skipping: %s", modsDir.c_str());
        return true;
    }
    if (!std::filesystem::is_directory(root)) {
        core::logError("[ModManager] mods path is not a directory: %s", modsDir.c_str());
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) continue;

        const std::filesystem::path manifestPath = entry.path() / "mod.json";
        if (!std::filesystem::exists(manifestPath)) continue;

        ModManifest manifest;
        if (!ModManifestLoader::loadFromFile(normalizePath(manifestPath), manifest)) {
            return false;
        }
        if (discovered_.find(manifest.id) != discovered_.end()) {
            core::logError("[ModManager] duplicate mod id discovered: %s", manifest.id.c_str());
            return false;
        }

        DiscoveredMod mod{};
        mod.manifest = std::move(manifest);
        mod.rootDir = normalizePath(entry.path());
        discovered_[mod.manifest.id] = std::move(mod);
    }

    core::logInfo("[ModManager] scanned %s (%zu mods)", modsDir.c_str(), discovered_.size());
    return true;
}

bool ModManager::resolveLoadOrder(const std::vector<std::string>& enabledMods,
                                  std::vector<LoadedMod>& outOrder) const {
    outOrder.clear();
    if (enabledMods.empty()) return true;

    std::unordered_map<std::string, int> enabledOrder;
    for (size_t i = 0; i < enabledMods.size(); ++i) {
        if (enabledOrder.find(enabledMods[i]) == enabledOrder.end()) {
            enabledOrder[enabledMods[i]] = static_cast<int>(i);
        }
    }

    // Collect the enabled set plus transitive dependencies first. This lets a
    // project list only its top-level mods while still mounting required data
    // packs before the mods that depend on them.
    std::set<std::string> required;
    std::vector<std::string> stack = enabledMods;
    while (!stack.empty()) {
        const std::string id = stack.back();
        stack.pop_back();
        if (!required.insert(id).second) continue;

        auto it = discovered_.find(id);
        if (it == discovered_.end()) {
            core::logError("[ModManager] enabled mod not found: %s", id.c_str());
            return false;
        }

        for (const std::string& dep : it->second.manifest.dependencies) {
            stack.push_back(dep);
            if (enabledOrder.find(dep) == enabledOrder.end()) {
                enabledOrder[dep] = -1;
            }
        }
    }

    std::unordered_map<std::string, std::vector<std::string>> dependents;
    std::unordered_map<std::string, int> indegree;
    for (const std::string& id : required) indegree[id] = 0;

    for (const std::string& id : required) {
        const auto& mod = discovered_.at(id);
        for (const std::string& dep : mod.manifest.dependencies) {
            if (required.find(dep) == required.end()) continue;
            dependents[dep].push_back(id);
            ++indegree[id];
        }
    }

    auto lessForMount = [&](const std::string& a, const std::string& b) {
        const auto& ma = discovered_.at(a).manifest;
        const auto& mb = discovered_.at(b).manifest;
        if (ma.priority != mb.priority) return ma.priority > mb.priority;
        if (enabledOrder.at(a) != enabledOrder.at(b)) return enabledOrder.at(a) > enabledOrder.at(b);
        return a > b;
    };

    std::priority_queue<std::string, std::vector<std::string>, decltype(lessForMount)> ready(lessForMount);
    for (const auto& [id, degree] : indegree) {
        if (degree == 0) ready.push(id);
    }

    while (!ready.empty()) {
        const std::string id = ready.top();
        ready.pop();

        const auto& mod = discovered_.at(id);
        LoadedMod loaded{};
        loaded.manifest = mod.manifest;
        loaded.rootDir = mod.rootDir;
        loaded.enabledOrder = enabledOrder.at(id);
        outOrder.push_back(std::move(loaded));

        for (const std::string& child : dependents[id]) {
            --indegree[child];
            if (indegree[child] == 0) ready.push(child);
        }
    }

    if (outOrder.size() != required.size()) {
        core::logError("[ModManager] dependency cycle detected while resolving mods");
        return false;
    }
    return true;
}

bool ModManager::mountGameAssetsAndMods(AssetManager& assets,
                                        const std::string& gameManifestPath,
                                        const GameManifest& gameManifest) {
    mountedMods_.clear();

    const std::filesystem::path gameFile(gameManifestPath);
    const std::filesystem::path gameRoot = gameFile.parent_path();
    const std::filesystem::path gameAssetManifest =
        resolveRelativeTo(gameRoot, gameManifest.assetManifest);

    if (!assets.loadManifest(normalizePath(gameAssetManifest))) {
        return false;
    }

    const std::filesystem::path modsRoot = gameRoot / "mods";
    if (!scanMods(normalizePath(modsRoot))) {
        return false;
    }

    std::vector<LoadedMod> order;
    if (!resolveLoadOrder(gameManifest.mods, order)) {
        return false;
    }

    for (const LoadedMod& mod : order) {
        if (mod.manifest.assetManifest.empty()) {
            mountedMods_.push_back(mod);
            core::logInfo("[ModManager] mounted mod metadata only: %s", mod.manifest.id.c_str());
            continue;
        }

        const std::filesystem::path modAssetManifest =
            resolveRelativeTo(mod.rootDir, mod.manifest.assetManifest);
        const std::string sourceName = "mod:" + mod.manifest.id;
        if (!assets.loadManifestOverlay(normalizePath(modAssetManifest), sourceName)) {
            return false;
        }

        mountedMods_.push_back(mod);
        core::logInfo("[ModManager] mounted mod assets: %s priority=%d",
                      mod.manifest.id.c_str(), mod.manifest.priority);
    }

    return true;
}

} // namespace engine
