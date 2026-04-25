#pragma once

#include "../../backend/renderer/TextureAtlas.h"
#include "../../backend/shared/ResourceHandle.h"
#include "../../core/math/Rect.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>

namespace backend {
class IRenderDevice;
}

namespace engine {

struct AtlasSpriteInfo {
    TextureHandle atlasTexture;  // 图集纹理句柄
    core::Rect srcRect;          // 在图集中的像素区域（整个原始纹理）
    core::Rect uvRect;           // 在图集中的 UV 区域（预计算）
    int atlasIndex;              // 图集索引
    int originalWidth = 0;       // 原始纹理宽度
    int originalHeight = 0;      // 原始纹理高度
    bool valid = false;
};

class TextureAtlasManager {
public:
    static constexpr int MAX_ATLASES = 4;
    
    void init(backend::IRenderDevice* device);
    void shutdown();
    
    // 添加纹理到图集，返回图集内的区域信息
    AtlasSpriteInfo addTexture(
        const std::string& name,
        int width, int height, int channels,
        const void* pixels
    );
    
    // 按名称查询精灵信息
    const AtlasSpriteInfo* getSprite(const std::string& name) const;
    
    // 获取图集纹理
    TextureHandle getAtlasTexture(int index) const;
    
    // 获取图集数量
    int atlasCount() const { return static_cast<int>(atlases_.size()); }
    
    // 统计信息
    size_t totalSprites() const { return sprites_.size(); }
    float averageUtilization() const;
    
private:
    backend::IRenderDevice* device_ = nullptr;
    std::vector<std::unique_ptr<backend::TextureAtlas>> atlases_;
    std::vector<TextureHandle> atlasTextures_;
    std::unordered_map<std::string, AtlasSpriteInfo> sprites_;
};

} // namespace engine
