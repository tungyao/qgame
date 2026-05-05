#pragma once
#include <string>
#include <entt/entt.hpp>

namespace engine {

class AssetManager;
class PrefabRegistry;

// EnTT Snapshot + JSON archive 实现场景序列化/反序列化。
// 资源引用优先写 assetId，路径字段仅作为旧场景/调试回退。
// 支持组件：Transform / Sprite / TileMap / TextComponent / Camera / RigidBody / Collider
// / Light2D / LightOccluder2D / Reflector2D / Environment2D
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

    // S2: 支持 scene entity 引用 prefab ID。
    // prefabs 可为空；为空时 loader 仍保持旧行为，只读取直接写在场景里的组件。
    static bool loadScene(entt::registry& reg,
                          AssetManager& assetMgr,
                          const std::string& path,
                          const PrefabRegistry* prefabs);
};

} // namespace engine
