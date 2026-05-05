#pragma once

#include <string>
#include <vector>

namespace engine {

// GameManifest 对应项目根目录的 game.json。
//
// 这里保存的是“游戏启动配置”，不是运行时状态：引擎启动器读取它后，再决定
// 加载哪个 asset manifest、注册哪个 startupScene、是否加载 Native game 模块。
// 当前阶段只解析字段，不执行动态库加载，避免把 S5 的 Native Mod 设计提前塞进 S1。
struct GameManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string startupScene;
    std::string assetManifest;
    std::string nativeLibrary;
    std::vector<std::string> mods;

    bool valid() const {
        return !id.empty() && !startupScene.empty();
    }
};

// GameManifestLoader 是一个无状态解析器。
//
// 选择静态函数而不是可实例化服务，是因为第一阶段没有缓存、热加载、编辑器监听
// 等需求；后续如果需要项目数据库，再把它包进更高层对象。
class GameManifestLoader {
public:
    static bool loadFromFile(const std::string& path, GameManifest& out);
};

} // namespace engine
