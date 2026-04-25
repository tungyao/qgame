#pragma once

#include "../shared/ResourceHandle.h"
#include "../../core/math/Rect.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>

namespace backend {

struct AtlasRegion {
    core::Rect uvRect;      // UV 坐标 (0-1)
    core::Rect pixelRect;   // 像素坐标
    int textureIndex = 0;   // 纹理数组索引（如果使用纹理数组）
    int padding = 0;
};

class TextureAtlas {
public:
    static constexpr int DEFAULT_ATLAS_SIZE = 2048;
    static constexpr int MIN_REGION_SIZE = 4;
    
    struct Config {
        int width = DEFAULT_ATLAS_SIZE;
        int height = DEFAULT_ATLAS_SIZE;
        int padding = 2;
        bool powerOfTwo = true;
    };
    
    TextureAtlas();
    explicit TextureAtlas(const Config& cfg);
    ~TextureAtlas() = default;
    
    // 添加区域，返回分配的 ID，失败返回 -1
    int addRegion(int width, int height, const std::string& name = "");
    
    // 按名称查询区域
    const AtlasRegion* getRegion(const std::string& name) const;
    
    // 按 ID 查询区域
    const AtlasRegion* getRegion(int id) const;
    
    // 获取图集尺寸
    int width() const { return width_; }
    int height() const { return height_; }
    
    // 获取区域数量
    size_t regionCount() const { return regions_.size(); }
    
    // 获取像素数据（用于上传到 GPU）
    const std::vector<uint8_t>& pixels() const { return pixels_; }
    std::vector<uint8_t>& pixels() { return pixels_; }
    
    // 检查是否已满
    bool isFull() const { return full_; }
    
    // 清空图集
    void clear();
    
    // 计算利用率
    float utilization() const;
    
private:
    struct Node {
        int x, y, width, height;
        bool used = false;
        Node* left = nullptr;
        Node* right = nullptr;
        
        Node(int x, int y, int w, int h) : x(x), y(y), width(w), height(h) {}
        ~Node() { delete left; delete right; }
        
        Node* insert(int w, int h, int padding);
    };
    
    int width_;
    int height_;
    int padding_;
    std::vector<uint8_t> pixels_;
    std::vector<AtlasRegion> regions_;
    std::unordered_map<std::string, int> nameToId_;
    Node* root_ = nullptr;
    bool full_ = false;
};

} // namespace backend
