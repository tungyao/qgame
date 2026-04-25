#pragma once

#include "CommandBuffer.h"
#include "../../core/math/Rect.h"
#include "../shared/ResourceHandle.h"
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace backend {

struct CachedTileChunk {
    std::vector<DrawTileCmd> tiles;
    TextureHandle texture;
    int layer = 0;
    int mapWidth = 0;
    int mapHeight = 0;
    int tileSize = 16;
    uint32_t version = 0;
    bool valid = false;
};

class TilemapCache {
public:
    static constexpr size_t MAX_CACHE_ENTRIES = 256;
    
    CachedTileChunk* getOrCreate(uint32_t entityId, int layer);
    void invalidate(uint32_t entityId);
    void clear();
    
    size_t totalCachedTiles() const;
    size_t cacheEntryCount() const { return chunks_.size(); }
    
private:
    struct ChunkKey {
        uint32_t entityId;
        int layer;
        
        bool operator==(const ChunkKey& o) const {
            return entityId == o.entityId && layer == o.layer;
        }
    };
    
    struct ChunkKeyHash {
        size_t operator()(const ChunkKey& k) const {
            return (size_t(k.entityId) << 4) | k.layer;
        }
    };
    
    std::unordered_map<ChunkKey, CachedTileChunk, ChunkKeyHash> chunks_;
};

} // namespace backend
