#include "TextureAtlasManager.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../core/Logger.h"

namespace engine {

void TextureAtlasManager::init(backend::IRenderDevice* device) {
    device_ = device;
}

void TextureAtlasManager::shutdown() {
    for (auto& tex : atlasTextures_) {
        if (tex.valid() && device_) {
            device_->destroyTexture(tex);
        }
    }
    atlases_.clear();
    atlasTextures_.clear();
    sprites_.clear();
}

AtlasSpriteInfo TextureAtlasManager::addTexture(
    const std::string& name,
    int width, int height, int channels,
    const void* pixels)
{
    AtlasSpriteInfo info;
    
    if (!device_ || name.empty() || width <= 0 || height <= 0 || !pixels) {
        return info;
    }
    
    // 检查是否已存在
    auto it = sprites_.find(name);
    if (it != sprites_.end()) {
        return it->second;
    }
    
    // 尝试添加到现有图集
    int atlasIndex = -1;
    int regionId = -1;
    
    for (size_t i = 0; i < atlases_.size(); ++i) {
        regionId = atlases_[i]->addRegion(width, height, name);
        if (regionId >= 0) {
            atlasIndex = static_cast<int>(i);
            break;
        }
    }
    
    // 需要创建新图集
    if (atlasIndex < 0) {
        if (atlases_.size() >= MAX_ATLASES) {
            core::logError("[TextureAtlasManager] max atlases reached, cannot add: %s", name.c_str());
            return info;
        }
        
        auto newAtlas = std::make_unique<backend::TextureAtlas>();
        atlases_.push_back(std::move(newAtlas));
        atlasIndex = static_cast<int>(atlases_.size() - 1);
        
        // 创建 GPU 纹理
        backend::TextureDesc desc{};
        desc.width = atlases_[atlasIndex]->width();
        desc.height = atlases_[atlasIndex]->height();
        desc.channels = 4;
        desc.data = nullptr;
        desc.filter = backend::TextureFilter::Linear;
        
        TextureHandle tex = device_->createTexture(desc);
        if (!tex.valid()) {
            core::logError("[TextureAtlasManager] failed to create atlas texture");
            atlases_.pop_back();
            return info;
        }
        atlasTextures_.push_back(tex);
        
        regionId = atlases_[atlasIndex]->addRegion(width, height, name);
        if (regionId < 0) {
            core::logError("[TextureAtlasManager] failed to add region to new atlas");
            return info;
        }
    }
    
    // 复制像素数据到图集
    const backend::AtlasRegion* region = atlases_[atlasIndex]->getRegion(regionId);
    if (!region) {
        return info;
    }
    
    auto& atlasPixels = atlases_[atlasIndex]->pixels();
    int atlasWidth = atlases_[atlasIndex]->width();
    
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    uint8_t* dst = atlasPixels.data();
    
    for (int y = 0; y < height; ++y) {
        int dstY = region->pixelRect.y + y;
        int dstOffset = (dstY * atlasWidth + region->pixelRect.x) * 4;
        int srcOffset = y * width * channels;
        
        for (int x = 0; x < width; ++x) {
            int di = dstOffset + x * 4;
            int si = srcOffset + x * channels;
            
            dst[di + 0] = src[si + 0];
            dst[di + 1] = (channels > 1) ? src[si + 1] : src[si + 0];
            dst[di + 2] = (channels > 2) ? src[si + 2] : src[si + 0];
            dst[di + 3] = (channels > 3) ? src[si + 3] : 255;
        }
    }
    
    // 更新 GPU 纹理（这里需要后端支持更新部分纹理，简化处理：整张更新）
    // 实际实现需要后端支持 glTexSubImage2D 或类似的 API
    
    // 保存精灵信息
    info.atlasTexture = atlasTextures_[atlasIndex];
    info.srcRect = region->pixelRect;
    info.uvRect = region->uvRect;
    info.atlasIndex = atlasIndex;
    info.originalWidth = width;
    info.originalHeight = height;
    info.valid = true;
    
    sprites_[name] = info;
    
    return info;
}

const AtlasSpriteInfo* TextureAtlasManager::getSprite(const std::string& name) const {
    auto it = sprites_.find(name);
    if (it == sprites_.end()) return nullptr;
    return &it->second;
}

TextureHandle TextureAtlasManager::getAtlasTexture(int index) const {
    if (index < 0 || index >= static_cast<int>(atlasTextures_.size())) return {};
    return atlasTextures_[index];
}

float TextureAtlasManager::averageUtilization() const {
    if (atlases_.empty()) return 0.f;
    
    float total = 0.f;
    for (const auto& atlas : atlases_) {
        total += atlas->utilization();
    }
    return total / atlases_.size();
}

} // namespace engine
