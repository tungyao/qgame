#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

struct GameContext;

// SceneDesc 记录稳定 scene ID 到磁盘 JSON 的绑定。
//
// 第一阶段仍复用现有 SceneSerializer 读取 JSON 文件；后续 asset manifest /
// QPAK / Mod 覆盖接入后，path 可以从“磁盘路径”升级成“资源解析结果”，调用方
// 仍只引用 id。
struct SceneDesc {
    std::string id;
    std::string path;
};

// SceneManager 是 Game Framework 的场景门面。
//
// 它负责把“按路径加载场景”的旧接口收敛到“按稳定 scene ID 切换场景”的新接口，
// 但当前实现不引入新序列化格式，避免 S1 一次性跨到 Prefab/Mod 设计。
class SceneManager {
public:
    explicit SceneManager(GameContext& ctx);

    // 注册一个 scene ID。重复注册同一 ID 会覆盖旧路径，便于后续 Mod 按确定性
    // 规则覆盖 scene 定义。
    bool registerScene(const std::string& id, const std::string& path);

    // 按稳定 ID 加载场景。成功后 currentSceneId/currentScenePath 同步更新。
    bool loadScene(const std::string& id);

    // 直接按路径加载，保留给调试工具和旧 demo 迁移期使用。因为没有稳定 ID，
    // 成功后 currentSceneId 会被清空。
    bool loadScenePath(const std::string& path);

    // switchScene 当前等价于 loadScene；单独保留命名，给后续异步切换、过渡、
    // unload/load 分阶段处理留出公开语义。
    bool switchScene(const std::string& id) { return loadScene(id); }

    // 卸载当前场景实体和待切换状态。资产缓存不在这里清空，因为它属于
    // AssetManager 的跨场景缓存职责。
    void unloadScene();

    bool hasScene(const std::string& id) const;
    const SceneDesc* findScene(const std::string& id) const;
    std::vector<std::string> sceneIds() const;

    const std::string& currentSceneId() const { return currentSceneId_; }
    const std::string& currentScenePath() const { return currentScenePath_; }

private:
    GameContext& ctx_;
    std::unordered_map<std::string, SceneDesc> scenes_;
    std::string currentSceneId_;
    std::string currentScenePath_;
};

} // namespace engine
