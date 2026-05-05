#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

namespace engine {

class AssetManager;
struct GameContext;

// PrefabDesc 是一个稳定 prefab ID 对应的一组组件数据。
//
// components 使用和 SceneSerializer 相同的组件 JSON object，例如：
// {
//   "Transform": { "x": 0, "y": 0 },
//   "Sprite": { "assetId": "texture.game.player" }
// }
// S2 阶段只保存已知组件；未知组件等 ComponentRegistry/反射系统成熟后再接入。
struct PrefabDesc {
    std::string id;
    nlohmann::json components = nlohmann::json::object();
    std::string source;
};

// PrefabRegistry 保存可实例化的实体模板。
//
// 设计重点是稳定 ID：游戏场景和后续 Mod 都引用 prefab ID，而不是复制整份组件。
// 同 ID 再注册会覆盖旧定义，这是后续 Mod 覆盖 prefab 的基础规则。
class PrefabRegistry {
public:
    PrefabRegistry() = default;
    explicit PrefabRegistry(GameContext& ctx);

    bool registerPrefab(const std::string& id,
                        const nlohmann::json& components,
                        const std::string& source = {});
    bool registerPrefabJson(const nlohmann::json& prefabJson,
                            const std::string& source = {});
    bool registerManifest(const std::string& path);

    bool hasPrefab(const std::string& id) const;
    const PrefabDesc* findPrefab(const std::string& id) const;
    std::vector<std::string> prefabIds() const;

    // 创建一个 prefab 实例，并把 overrides 作为局部组件覆盖应用到实例上。
    // overrides 的格式同 components；调用方可以只覆盖 Transform/Name 等少数项。
    entt::entity instantiate(const std::string& id,
                             entt::registry& world,
                             AssetManager& assets,
                             const nlohmann::json& overrides = nlohmann::json::object()) const;

private:
    std::unordered_map<std::string, PrefabDesc> prefabs_;
};

} // namespace engine
