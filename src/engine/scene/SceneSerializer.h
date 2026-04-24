#pragma once
#include <string>
#include <entt/entt.hpp>

namespace engine {

class AssetManager;

// EnTT Snapshot + JSON archive 实现场景序列化/反序列化
// 支持组件：Transform / Sprite / TileMap / Camera / RigidBody / Collider
class SceneSerializer {
public:
    // 序列化 registry 到 JSON 文件；assetMgr 用于将 TextureHandle 转路径
    static bool saveScene(entt::registry& reg,
                          AssetManager& assetMgr,
                          const std::string& path);

    // 从 JSON 文件重建 registry；加载纹理通过 assetMgr
    static bool loadScene(entt::registry& reg,
                          AssetManager& assetMgr,
                          const std::string& path);
};

} // namespace engine
