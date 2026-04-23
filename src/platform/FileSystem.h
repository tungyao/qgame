#pragma once
#include <string>
#include <vector>
#include <functional>

namespace platform {

// 跨平台文件读写，异步读文件
class FileSystem {
public:
    using ReadCallback = std::function<void(std::vector<uint8_t> data, bool ok)>;

    // 同步读（小文件/工具链）
    static bool readFile(const std::string& path, std::vector<uint8_t>& out);
    static bool writeFile(const std::string& path, const void* data, size_t size);

    // 异步读（游戏运行时，回调在主线程执行）
    static void readFileAsync(const std::string& path, ReadCallback cb);

    // 路径工具
    static std::string assetsDir();  // 返回 assets/ 目录绝对路径
    static std::string join(const std::string& base, const std::string& rel);
    static bool        exists(const std::string& path);
};

} // namespace platform
