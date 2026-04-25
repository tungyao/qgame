#include "TilemapCache.h"

namespace backend {

CachedTileChunk* TilemapCache::getOrCreate(uint32_t entityId, int layer) {
    ChunkKey key{entityId, layer};
    auto it = chunks_.find(key);
    if (it != chunks_.end()) {
        return &it->second;
    }
    
    if (chunks_.size() >= MAX_CACHE_ENTRIES) {
        chunks_.clear();
    }
    
    CachedTileChunk chunk;
    chunk.layer = layer;
    chunk.valid = false;
    
    auto [insertIt, _] = chunks_.emplace(key, std::move(chunk));
    return &insertIt->second;
}

void TilemapCache::invalidate(uint32_t entityId) {
    for (int layer = 0; layer < 16; ++layer) {
        ChunkKey key{entityId, layer};
        chunks_.erase(key);
    }
}

void TilemapCache::clear() {
    chunks_.clear();
}

size_t TilemapCache::totalCachedTiles() const {
    size_t total = 0;
    for (const auto& [_, chunk] : chunks_) {
        total += chunk.tiles.size();
    }
    return total;
}

} // namespace backend
