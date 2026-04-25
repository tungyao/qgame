#include "TextureAtlas.h"
#include <algorithm>
#include <cstring>

namespace backend {

TextureAtlas::TextureAtlas()
    : TextureAtlas(Config{})
{
}

TextureAtlas::TextureAtlas(const Config& cfg)
    : width_(cfg.width)
    , height_(cfg.height)
    , padding_(cfg.padding)
{
    pixels_.resize(width_ * height_ * 4, 0);
    root_ = new Node(0, 0, width_, height_);
}

void TextureAtlas::clear() {
    std::fill(pixels_.begin(), pixels_.end(), 0);
    regions_.clear();
    nameToId_.clear();
    delete root_;
    root_ = new Node(0, 0, width_, height_);
    full_ = false;
}

int TextureAtlas::addRegion(int w, int h, const std::string& name) {
    if (full_ || w <= 0 || h <= 0) return -1;
    
    int paddedW = w + padding_ * 2;
    int paddedH = h + padding_ * 2;
    
    Node* node = root_->insert(paddedW, paddedH, padding_);
    if (!node) {
        full_ = true;
        return -1;
    }
    
    AtlasRegion region;
    region.pixelRect.x = node->x + padding_;
    region.pixelRect.y = node->y + padding_;
    region.pixelRect.w = w;
    region.pixelRect.h = h;
    region.uvRect.x = static_cast<float>(region.pixelRect.x) / width_;
    region.uvRect.y = static_cast<float>(region.pixelRect.y) / height_;
    region.uvRect.w = static_cast<float>(w) / width_;
    region.uvRect.h = static_cast<float>(h) / height_;
    region.padding = padding_;
    
    int id = static_cast<int>(regions_.size());
    regions_.push_back(region);
    
    if (!name.empty()) {
        nameToId_[name] = id;
    }
    
    return id;
}

const AtlasRegion* TextureAtlas::getRegion(const std::string& name) const {
    auto it = nameToId_.find(name);
    if (it == nameToId_.end()) return nullptr;
    return getRegion(it->second);
}

const AtlasRegion* TextureAtlas::getRegion(int id) const {
    if (id < 0 || id >= static_cast<int>(regions_.size())) return nullptr;
    return &regions_[id];
}

float TextureAtlas::utilization() const {
    if (regions_.empty()) return 0.f;
    
    int usedArea = 0;
    for (const auto& r : regions_) {
        int pw = r.pixelRect.w + r.padding * 2;
        int ph = r.pixelRect.h + r.padding * 2;
        usedArea += pw * ph;
    }
    return static_cast<float>(usedArea) / (width_ * height_);
}

TextureAtlas::Node* TextureAtlas::Node::insert(int w, int h, int padding) {
    if (left || right) {
        Node* result = left ? left->insert(w, h, padding) : nullptr;
        if (result) return result;
        return right ? right->insert(w, h, padding) : nullptr;
    }
    
    if (used) return nullptr;
    
    if (w > width || h > height) return nullptr;
    
    if (w == width && h == height) {
        used = true;
        return this;
    }
    
    int dw = width - w;
    int dh = height - h;
    
    if (dw > dh) {
        left = new Node(x, y, w, height);
        right = new Node(x + w, y, width - w, height);
    } else {
        left = new Node(x, y, width, h);
        right = new Node(x, y + h, width, height - h);
    }
    
    return left->insert(w, h, padding);
}

} // namespace backend
