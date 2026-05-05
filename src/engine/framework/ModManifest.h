#pragma once

#include <string>
#include <vector>

namespace engine {

// ModType 保持和 mod.json 的 type 字段一一对应。
//
// data 表示只挂载资源/配置；native 表示除了数据外还可能加载动态库。
// 第一阶段只解析类型，不执行 native library 加载。
enum class ModType {
    Data,
    Native
};

// ModManifest 对应单个 Mod 根目录下的 mod.json。
//
// priority、dependencies 和启用顺序共同决定挂载顺序；这里保留原始字段，
// ModManager 后续会在这些字段之上实现拓扑排序和 last override wins。
struct ModManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string engineVersion;
    ModType type = ModType::Data;
    int priority = 0;
    std::string assetManifest;
    std::vector<std::string> sceneManifests;
    std::vector<std::string> prefabManifests;
    std::vector<std::string> configManifests;
    std::string library;
    std::vector<std::string> dependencies;

    bool valid() const {
        return !id.empty() && !version.empty();
    }
};

class ModManifestLoader {
public:
    static bool loadFromFile(const std::string& path, ModManifest& out);
};

} // namespace engine
