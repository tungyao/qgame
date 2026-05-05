#include "AssetManager.h"
#include "FontLoader.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../backend/audio/IAudioDevice.h"
#include "../../core/Logger.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <iterator>

namespace engine {

const std::string AssetManager::kEmpty_;
uint32_t AssetManager::nextAnimIndex_ = 1;

uint32_t AssetManager::makeHandleKey(uint32_t index, uint32_t version) {
    return (index << 12) | (version & 0xFFFu);
}

bool AssetManager::isPackPath(const std::string& path) {
    return path.rfind("pak://", 0) == 0;
}

bool AssetManager::splitPackPath(const std::string& path, std::string& outPackId, std::string& outFile) {
    if (!isPackPath(path)) return false;
    const size_t start = 6;
    const size_t slash = path.find('/', start);
    if (slash == std::string::npos || slash == start || slash + 1 >= path.size()) return false;
    outPackId = path.substr(start, slash - start);
    outFile = path.substr(slash + 1);
    std::replace(outFile.begin(), outFile.end(), '\\', '/');
    return true;
}

std::string AssetManager::normalizeAssetPath(const std::string& path) {
    if (isPackPath(path)) {
        const size_t hash = path.find('#');
        const std::string pathOnly = hash == std::string::npos ? path : path.substr(0, hash);
        const std::string suffix = hash == std::string::npos ? std::string{} : path.substr(hash);
        std::string packId, file;
        if (!splitPackPath(pathOnly, packId, file)) return path;
        return "pak://" + packId + "/" + file + suffix;
    }
    return std::filesystem::path(path).lexically_normal().string();
}

std::string AssetManager::siblingRegionIdPath(const std::string& path) {
    if (isPackPath(path)) {
        std::string packId, file;
        if (!splitPackPath(path, packId, file)) return {};
        const size_t slash = file.find_last_of('/');
        const size_t dot = file.find_last_of('.');
        const bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
        const std::string base = hasExt ? file.substr(0, dot) : file;
        return "pak://" + packId + "/" + base + ".id.png";
    }
    std::filesystem::path sib = std::filesystem::path(path);
    sib.replace_extension();
    sib += ".id.png";
    return sib.lexically_normal().string();
}

std::string AssetManager::parentAssetPath(const std::string& path) {
    if (isPackPath(path)) {
        std::string packId, file;
        if (!splitPackPath(path, packId, file)) return {};
        const size_t slash = file.find_last_of('/');
        if (slash == std::string::npos) return "pak://" + packId + "/";
        return "pak://" + packId + "/" + file.substr(0, slash + 1);
    }
    return std::filesystem::path(path).parent_path().string();
}

std::string AssetManager::joinAssetPath(const std::string& baseDir, const std::string& child) {
    if (child.empty()) return baseDir;
    if (isPackPath(child) || std::filesystem::path(child).is_absolute()) {
        return normalizeAssetPath(child);
    }
    if (isPackPath(baseDir)) {
        std::string base = baseDir;
        if (!base.empty() && base.back() != '/') base += '/';
        return normalizeAssetPath(base + child);
    }
    return (std::filesystem::path(baseDir) / child).lexically_normal().string();
}

void AssetManager::init(backend::IRenderDevice* render, backend::IAudioDevice* audio) {
    render_ = render;
    audio_  = audio;
}

void AssetManager::shutdown() {
    // 强制释放所有残留资源（不依赖 release 引用计数）
    if (render_) {
        for (auto& [path, e] : fontByPath_) {
            render_->destroyFont(e.handle);
            if (e.atlas.valid()) render_->destroyTexture(e.atlas);
        }
        for (auto& [path, e] : texByPath_) {
            if (e.regionId.valid()) render_->destroyTexture(e.regionId);
            render_->destroyTexture(e.handle);
        }
    }
    if (audio_) {
        for (auto& [path, e] : sndByPath_)
            audio_->unloadSound(e.handle);
    }
    texByPath_.clear();
    sndByPath_.clear();
    fontByPath_.clear();
    texPathById_.clear();
    sndPathById_.clear();
    fontPathById_.clear();
    animByPath_.clear();
    animPathById_.clear();
    clearManifestIndex();
    textureAssetIdByHandle_.clear();
    soundAssetIdByHandle_.clear();
    animationAssetIdByHandle_.clear();
    fontAssetIdByHandle_.clear();
}

bool AssetManager::mountPack(const std::string& id, const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        core::logError("[AssetManager] failed to open pack %s: %s", id.c_str(), path.c_str());
        return false;
    }

    ifs.seekg(0, std::ios::end);
    const std::streamoff fileSize = ifs.tellg();
    if (fileSize < 12) {
        core::logError("[AssetManager] pack too small: %s", path.c_str());
        return false;
    }

    char magic[4] = {};
    ifs.seekg(fileSize - 4);
    ifs.read(magic, 4);
    if (std::memcmp(magic, "QPAK", 4) != 0) {
        core::logError("[AssetManager] bad pack magic: %s", path.c_str());
        return false;
    }

    uint64_t indexSize = 0;
    ifs.seekg(fileSize - 12);
    ifs.read(reinterpret_cast<char*>(&indexSize), sizeof(indexSize));
    if (indexSize == 0 || indexSize > static_cast<uint64_t>(fileSize - 12)) {
        core::logError("[AssetManager] invalid pack index size: %s", path.c_str());
        return false;
    }

    std::string indexJson(indexSize, '\0');
    ifs.seekg(fileSize - 12 - static_cast<std::streamoff>(indexSize));
    ifs.read(indexJson.data(), static_cast<std::streamsize>(indexJson.size()));

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(indexJson);
    } catch (const std::exception& e) {
        core::logError("[AssetManager] pack index parse error in %s: %s", path.c_str(), e.what());
        return false;
    }

    MountedPack pack{};
    pack.path = path;
    for (const auto& item : j.value("files", nlohmann::json::array())) {
        PackEntry e{};
        std::string rel = item.value("path", std::string{});
        e.offset = item.value("offset", uint64_t{0});
        e.size = item.value("size", uint64_t{0});
        e.sha256 = item.value("sha256", std::string{});
        if (!rel.empty()) {
            std::replace(rel.begin(), rel.end(), '\\', '/');
            pack.files[rel] = std::move(e);
        }
    }

    core::logInfo("[AssetManager] mounted pack %s: %s (%zu files)",
                  id.c_str(), path.c_str(), pack.files.size());
    packsById_[id] = std::move(pack);
    return true;
}

bool AssetManager::readAssetBytes(const std::string& path, std::vector<uint8_t>& out) const {
    out.clear();
    if (isPackPath(path)) {
        std::string packId, file;
        if (!splitPackPath(path, packId, file)) return false;
        auto pit = packsById_.find(packId);
        if (pit == packsById_.end()) return false;
        auto fit = pit->second.files.find(file);
        if (fit == pit->second.files.end()) return false;

        std::ifstream ifs(pit->second.path, std::ios::binary);
        if (!ifs.is_open()) return false;
        out.resize(static_cast<size_t>(fit->second.size));
        ifs.seekg(static_cast<std::streamoff>(fit->second.offset));
        ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return static_cast<size_t>(ifs.gcount()) == out.size();
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    return true;
}

bool AssetManager::assetPathExists(const std::string& path) const {
    if (isPackPath(path)) {
        std::string packId, file;
        if (!splitPackPath(path, packId, file)) return false;
        auto pit = packsById_.find(packId);
        return pit != packsById_.end() && pit->second.files.find(file) != pit->second.files.end();
    }
    return std::filesystem::exists(path);
}

AssetManager::AssetType AssetManager::parseAssetType(const std::string& type) {
    if (type == "texture") return AssetType::Texture;
    if (type == "sound" || type == "audio") return AssetType::Sound;
    if (type == "animation") return AssetType::Animation;
    if (type == "font") return AssetType::Font;
    return AssetType::Unknown;
}

std::string AssetManager::resolveManifestPath(const AssetRecord& rec) const {
    const std::string& raw = !rec.baked.empty() ? rec.baked : rec.source;
    if (raw.empty()) return {};
    if (isPackPath(raw)) return normalizeAssetPath(raw);

    std::filesystem::path p(raw);
    const std::string& baseDir = !rec.baseDir.empty() ? rec.baseDir : manifestDir_;
    if (p.is_absolute() || baseDir.empty()) return p.lexically_normal().string();

    const std::filesystem::path fromManifest = std::filesystem::path(baseDir) / p;
    if (std::filesystem::exists(fromManifest)) {
        return fromManifest.lexically_normal().string();
    }

    return p.lexically_normal().string();
}

void AssetManager::clearManifestIndex() {
    // Manifest 数据是资源 ID 到磁盘/QPAK 路径的索引。清掉索引不会销毁
    // 已加载 GPU/音频资源；资源生命周期仍由各自的引用计数和 shutdown 管理。
    assetsById_.clear();
    assetOverrideChains_.clear();
    packsById_.clear();
    manifestDir_.clear();
    textureIdByPath_.clear();
    soundIdByPath_.clear();
    animationIdByPath_.clear();
    fontIdByPath_.clear();
}

void AssetManager::indexManifestRecord(const AssetRecord& rec) {
    const std::string resolved = resolveManifestPath(rec);
    if (resolved.empty()) return;

    // Runtime lookups are indexed by the resolved baked path. Later pack/VFS
    // support can map the same stable ID to a virtual path without changing
    // game-facing APIs or scene files.
    switch (rec.type) {
        case AssetType::Texture:   textureIdByPath_[resolved] = rec.id; break;
        case AssetType::Sound:     soundIdByPath_[resolved] = rec.id; break;
        case AssetType::Animation: animationIdByPath_[resolved] = rec.id; break;
        case AssetType::Font:      fontIdByPath_[resolved] = rec.id; break;
        default: break;
    }
}

void AssetManager::removeManifestRecordIndex(const AssetRecord& rec) {
    const std::string resolved = resolveManifestPath(rec);
    if (resolved.empty()) return;

    // 覆盖同一个 asset ID 时，旧路径不能继续反查到这个 ID；否则旧场景
    // 直接按路径加载后，保存时会误写成当前已经被 Mod 覆盖的 stable ID。
    switch (rec.type) {
        case AssetType::Texture:   textureIdByPath_.erase(resolved); break;
        case AssetType::Sound:     soundIdByPath_.erase(resolved); break;
        case AssetType::Animation: animationIdByPath_.erase(resolved); break;
        case AssetType::Font:      fontIdByPath_.erase(resolved); break;
        default: break;
    }
}

const std::string& AssetManager::assetIdForPath(AssetType type, const std::string& path) const {
    const std::string normalized = normalizeAssetPath(path);
    switch (type) {
        case AssetType::Texture: {
            auto it = textureIdByPath_.find(normalized);
            return (it != textureIdByPath_.end()) ? it->second : kEmpty_;
        }
        case AssetType::Sound: {
            auto it = soundIdByPath_.find(normalized);
            return (it != soundIdByPath_.end()) ? it->second : kEmpty_;
        }
        case AssetType::Animation: {
            auto it = animationIdByPath_.find(normalized);
            return (it != animationIdByPath_.end()) ? it->second : kEmpty_;
        }
        case AssetType::Font: {
            auto it = fontIdByPath_.find(normalized);
            return (it != fontIdByPath_.end()) ? it->second : kEmpty_;
        }
        default:
            return kEmpty_;
    }
}

bool AssetManager::loadManifest(const std::string& path) {
    clearManifestIndex();
    return loadManifestOverlay(path, "game");
}

bool AssetManager::loadManifestOverlay(const std::string& path, const std::string& sourceName) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        core::logError("[AssetManager] failed to open manifest: %s", path.c_str());
        return false;
    }

    nlohmann::json j;
    try {
        ifs >> j;
    } catch (const std::exception& e) {
        core::logError("[AssetManager] manifest parse error in %s: %s", path.c_str(), e.what());
        return false;
    }

    if (!j.contains("assets") || !j["assets"].is_array()) {
        core::logError("[AssetManager] manifest missing assets array: %s", path.c_str());
        return false;
    }

    manifestDir_ = std::filesystem::path(path).parent_path().string();

    if (j.contains("packs") && j["packs"].is_array()) {
        for (const auto& item : j["packs"]) {
            const std::string id = item.value("id", std::string{});
            const std::string rawPath = item.value("path", std::string{});
            if (id.empty() || rawPath.empty()) continue;
            std::filesystem::path packPath(rawPath);
            if (!packPath.is_absolute() && !manifestDir_.empty()) {
                packPath = std::filesystem::path(manifestDir_) / packPath;
            }
            mountPack(id, packPath.lexically_normal().string());
        }
    }

    for (const auto& item : j["assets"]) {
        AssetRecord rec{};
        rec.id     = item.value("id", std::string{});
        rec.type   = parseAssetType(item.value("type", std::string{}));
        rec.source = item.value("source", std::string{});
        rec.baked  = item.value("baked", std::string{});
        rec.baseDir = manifestDir_;

        if (rec.id.empty()) {
            core::logWarn("[AssetManager] manifest asset without id ignored: %s", path.c_str());
            continue;
        }
        if (rec.type == AssetType::Unknown) {
            core::logWarn("[AssetManager] manifest asset %s has unknown type", rec.id.c_str());
            continue;
        }
        if (rec.source.empty() && rec.baked.empty()) {
            core::logWarn("[AssetManager] manifest asset %s has no source/baked path", rec.id.c_str());
            continue;
        }
        auto& chain = assetOverrideChains_[rec.id];
        if (!chain.empty()) {
            core::logWarn("[AssetManager] asset override: %s", rec.id.c_str());
            for (const auto& layer : chain) {
                core::logWarn("  previous: %s (%s)",
                              layer.sourceName.c_str(),
                              layer.manifestPath.c_str());
            }
            core::logWarn("  winner: %s (%s)", sourceName.c_str(), path.c_str());
            auto old = assetsById_.find(rec.id);
            if (old != assetsById_.end()) {
                removeManifestRecordIndex(old->second);
            }
        }
        chain.push_back(AssetOverrideEntry{sourceName, path});

        indexManifestRecord(rec);
        assetsById_[rec.id] = std::move(rec);
    }

    core::logInfo("[AssetManager] loaded manifest layer %s from %s (%zu total assets)",
                  sourceName.c_str(), path.c_str(), assetsById_.size());
    return true;
}

bool AssetManager::hasAsset(const std::string& id) const {
    return assetsById_.find(id) != assetsById_.end();
}

std::vector<std::string> AssetManager::assetIds() const {
    std::vector<std::string> ids;
    ids.reserve(assetsById_.size());
    for (const auto& [id, _] : assetsById_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<AssetManager::AssetOverrideEntry>
AssetManager::assetOverrideChain(const std::string& id) const {
    auto it = assetOverrideChains_.find(id);
    if (it == assetOverrideChains_.end()) return {};
    return it->second;
}

TextureHandle AssetManager::loadTextureById(const std::string& id) {
    auto it = assetsById_.find(id);
    if (it == assetsById_.end() || it->second.type != AssetType::Texture) {
        core::logError("[AssetManager] texture asset id not found: %s", id.c_str());
        return {};
    }
    TextureHandle h = loadTexture(resolveManifestPath(it->second));
    if (h.valid()) textureAssetIdByHandle_[makeHandleKey(h.index, h.version)] = id;
    return h;
}

SoundHandle AssetManager::loadSoundById(const std::string& id) {
    auto it = assetsById_.find(id);
    if (it == assetsById_.end() || it->second.type != AssetType::Sound) {
        core::logError("[AssetManager] sound asset id not found: %s", id.c_str());
        return {};
    }
    SoundHandle h = loadSound(resolveManifestPath(it->second));
    if (h.valid()) soundAssetIdByHandle_[makeHandleKey(h.index, h.version)] = id;
    return h;
}

AnimationHandle AssetManager::loadAnimationById(const std::string& id) {
    auto it = assetsById_.find(id);
    if (it == assetsById_.end() || it->second.type != AssetType::Animation) {
        core::logError("[AssetManager] animation asset id not found: %s", id.c_str());
        return {};
    }
    AnimationHandle h = loadAnimation(resolveManifestPath(it->second));
    if (h.valid()) animationAssetIdByHandle_[makeHandleKey(h.index, h.version)] = id;
    return h;
}

FontHandle AssetManager::loadFontById(const std::string& id) {
    auto it = assetsById_.find(id);
    if (it == assetsById_.end() || it->second.type != AssetType::Font) {
        core::logError("[AssetManager] font asset id not found: %s", id.c_str());
        return {};
    }
    FontHandle h = loadFont(resolveManifestPath(it->second));
    if (h.valid()) fontAssetIdByHandle_[makeHandleKey(h.index, h.version)] = id;
    return h;
}

TextureHandle AssetManager::loadTexture(const std::string& path) {
    const std::string cachePath = normalizeAssetPath(path);
    auto it = texByPath_.find(cachePath);
    if (it != texByPath_.end()) {
        it->second.refCount++;
        if (const std::string& assetId = assetIdForPath(AssetType::Texture, cachePath); !assetId.empty()) {
            textureAssetIdByHandle_[makeHandleKey(it->second.handle.index, it->second.handle.version)] = assetId;
        }
        return it->second.handle;
    }

    if (!render_) return {};

    int w, h, ch;
    std::vector<uint8_t> fileBytes;
    if (!readAssetBytes(cachePath, fileBytes)) {
        core::logError("[AssetManager] failed to read texture: %s", cachePath.c_str());
        return {};
    }
    unsigned char* pixels = stbi_load_from_memory(fileBytes.data(),
                                                  static_cast<int>(fileBytes.size()),
                                                  &w, &h, &ch, 4);
    if (!pixels) {
        core::logError("[AssetManager] failed to load texture: %s", cachePath.c_str());
        return {};
    }

    backend::TextureDesc desc{};
    desc.data    = pixels;
    desc.width   = w;
    desc.height  = h;
    desc.channels = 4;
    TextureHandle h_tex = render_->createTexture(desc);
    stbi_image_free(pixels);

    if (!h_tex.valid()) return {};

    // sibling region ID 图：<path>.id.png（约定单通道 R8，不存在则忽略）
    TextureHandle h_region{};
    {
        const std::string sib = siblingRegionIdPath(cachePath);
        if (assetPathExists(sib)) {
            int rw = 0, rh = 0, rch = 0;
            std::vector<uint8_t> regionBytes;
            unsigned char* rpix = nullptr;
            if (readAssetBytes(sib, regionBytes)) {
                // 强制读 R 通道（stbi 会按需转换）
                rpix = stbi_load_from_memory(regionBytes.data(),
                                             static_cast<int>(regionBytes.size()),
                                             &rw, &rh, &rch, 1);
            }
            if (!rpix) {
                core::logWarn("[AssetManager] region id sibling exists but failed to load: %s",
                              sib.c_str());
            } else if (rw != w || rh != h) {
                core::logWarn("[AssetManager] region id %s size %dx%d != base %dx%d, ignored",
                              sib.c_str(), rw, rh, w, h);
                stbi_image_free(rpix);
            } else {
                backend::TextureDesc rdesc{};
                rdesc.data     = rpix;
                rdesc.width    = rw;
                rdesc.height   = rh;
                rdesc.channels = 1;
                rdesc.format   = backend::TextureFormat::R8;
                rdesc.filter   = backend::TextureFilter::Nearest;  // ID 必须 nearest
                h_region = render_->createTexture(rdesc);
                stbi_image_free(rpix);
                if (!h_region.valid()) {
                    core::logWarn("[AssetManager] failed to create region id GPU texture: %s",
                                  sib.c_str());
                }
            }
        }
    }

    uint32_t id = makeHandleKey(h_tex.index, h_tex.version);
    texByPath_[cachePath]  = {h_tex, 1, h_region};
    texPathById_[id]  = cachePath;
    if (const std::string& assetId = assetIdForPath(AssetType::Texture, cachePath); !assetId.empty()) {
        textureAssetIdByHandle_[id] = assetId;
    }
    return h_tex;
}

TextureHandle AssetManager::regionIdTexture(TextureHandle base) const {
    if (!base.valid()) return {};
    uint32_t id = makeHandleKey(base.index, base.version);
    auto pit = texPathById_.find(id);
    if (pit == texPathById_.end()) return {};
    auto it = texByPath_.find(pit->second);
    if (it == texByPath_.end()) return {};
    return it->second.regionId;
}

SoundHandle AssetManager::loadSound(const std::string& path) {
    const std::string cachePath = normalizeAssetPath(path);
    auto it = sndByPath_.find(cachePath);
    if (it != sndByPath_.end()) {
        it->second.refCount++;
        if (const std::string& assetId = assetIdForPath(AssetType::Sound, cachePath); !assetId.empty()) {
            soundAssetIdByHandle_[makeHandleKey(it->second.handle.index, it->second.handle.version)] = assetId;
        }
        return it->second.handle;
    }

    if (!audio_) return {};
    if (isPackPath(cachePath)) {
        std::vector<uint8_t> soundBytes;
        if (!readAssetBytes(cachePath, soundBytes)) {
            core::logError("[AssetManager] failed to read sound: %s", cachePath.c_str());
            return {};
        }
        SoundHandle h_snd = audio_->loadSoundFromMemory(soundBytes.data(), soundBytes.size(), cachePath.c_str());
        if (!h_snd.valid()) {
            core::logError("[AssetManager] failed to load sound: %s", cachePath.c_str());
            return {};
        }
        uint32_t id = makeHandleKey(h_snd.index, h_snd.version);
        sndByPath_[cachePath] = {h_snd, 1};
        sndPathById_[id] = cachePath;
        if (const std::string& assetId = assetIdForPath(AssetType::Sound, cachePath); !assetId.empty()) {
            soundAssetIdByHandle_[id] = assetId;
        }
        return h_snd;
    }

    SoundHandle h_snd = audio_->loadSound(cachePath.c_str());
    if (!h_snd.valid()) {
        core::logError("[AssetManager] failed to load sound: %s", cachePath.c_str());
        return {};
    }

    uint32_t id = makeHandleKey(h_snd.index, h_snd.version);
    sndByPath_[cachePath]  = {h_snd, 1};
    sndPathById_[id]  = cachePath;
    if (const std::string& assetId = assetIdForPath(AssetType::Sound, cachePath); !assetId.empty()) {
        soundAssetIdByHandle_[id] = assetId;
    }
    return h_snd;
}

void AssetManager::releaseTexture(TextureHandle h) {
    if (!h.valid() || !render_) return;
    uint32_t id = makeHandleKey(h.index, h.version);
    auto pit = texPathById_.find(id);
    if (pit == texPathById_.end()) return;

    auto& entry = texByPath_[pit->second];
    if (--entry.refCount <= 0) {
        if (entry.regionId.valid()) render_->destroyTexture(entry.regionId);
        render_->destroyTexture(h);
        texByPath_.erase(pit->second);
        texPathById_.erase(id);
        textureAssetIdByHandle_.erase(id);
    }
}

void AssetManager::releaseSound(SoundHandle h) {
    if (!h.valid() || !audio_) return;
    uint32_t id = makeHandleKey(h.index, h.version);
    auto pit = sndPathById_.find(id);
    if (pit == sndPathById_.end()) return;

    auto& entry = sndByPath_[pit->second];
    if (--entry.refCount <= 0) {
        audio_->unloadSound(h);
        sndByPath_.erase(pit->second);
        sndPathById_.erase(id);
        soundAssetIdByHandle_.erase(id);
    }
}

// 解析 Aseprite JSON (Array 格式)。path 形如 "anim/player.json" 或 "anim/player.json#walk"
AnimationHandle AssetManager::loadAnimation(const std::string& path) {
    const std::string cachePath = normalizeAssetPath(path);
    auto it = animByPath_.find(cachePath);
    if (it != animByPath_.end()) {
        it->second.refCount++;
        if (const std::string& assetId = assetIdForPath(AssetType::Animation, cachePath); !assetId.empty()) {
            animationAssetIdByHandle_[makeHandleKey(it->second.handle.index, it->second.handle.version)] = assetId;
        }
        return it->second.handle;
    }

    // 拆分 path 和 tag
    std::string filePath = cachePath;
    std::string tagName;
    if (auto hash = cachePath.find('#'); hash != std::string::npos) {
        filePath = cachePath.substr(0, hash);
        tagName  = cachePath.substr(hash + 1);
    }

    AnimationClip clip;
    clip.name = tagName.empty() ? filePath : tagName;
    clip.loop = true;

    std::vector<uint8_t> jsonBytes;
    if (!readAssetBytes(filePath, jsonBytes)) {
        core::logError("[AssetManager] failed to read animation file: %s", filePath.c_str());
        return {};
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonBytes.begin(), jsonBytes.end());
    } catch (const std::exception& e) {
        core::logError("[AssetManager] json parse error: %s", e.what());
        return {};
    }

    // 1) 收集所有 frame (支持 Array 和 Hash 格式)
    struct RawFrame { core::Rect rect; float duration; int frameNum; };
    std::vector<RawFrame> rawFrames;

    if (j.contains("frames")) {
        if (j["frames"].is_array()) {
            // Array 格式: [{"filename":"...", "frame":{"x":..,"y":..,"w":..,"h":..}, "duration":100}, ...]
            for (size_t idx = 0; idx < j["frames"].size(); ++idx) {
                auto& f = j["frames"][idx];
                RawFrame rf;
                auto& fr = f["frame"];
                rf.rect = { fr.value("x", 0.f), fr.value("y", 0.f),
                            fr.value("w", 0.f), fr.value("h", 0.f) };
                rf.duration = f.value("duration", 100) / 1000.f; // ms → s
                rf.frameNum = f.value("frameNum", static_cast<int>(idx));
                rawFrames.push_back(rf);
            }
        } else if (j["frames"].is_object()) {
            // Hash 格式: {"frame1.png": {"frame":{"x":..,"y":..,...}, "duration":100}, ...}
            // 需要按 frameNum 排序
            std::vector<std::pair<int, RawFrame>> sorted;
            for (auto& [key, f] : j["frames"].items()) {
                RawFrame rf;
                auto& fr = f["frame"];
                rf.rect = { fr.value("x", 0.f), fr.value("y", 0.f),
                            fr.value("w", 0.f), fr.value("h", 0.f) };
                rf.duration = f.value("duration", 100) / 1000.f;
                rf.frameNum = f.value("frameNum", 0);
                sorted.emplace_back(rf.frameNum, rf);
            }
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            for (auto& [_, rf] : sorted) rawFrames.push_back(rf);
        }
    }

    if (rawFrames.empty()) {
        core::logError("[AssetManager] no frames found in: %s", filePath.c_str());
        return {};
    }

    // 2) 选择 tag 范围 (Aseprite frameTags)
    int from = 0, to = static_cast<int>(rawFrames.size()) - 1;
    bool loop = true;
    if (!tagName.empty() && j.contains("meta") && j["meta"].contains("frameTags")) {
        bool found = false;
        for (auto& t : j["meta"]["frameTags"]) {
            if (t.value("name", "") == tagName) {
                from = t.value("from", 0);
                to   = t.value("to", from);
                std::string dir = t.value("direction", "forward");
                // Aseprite direction: "forward", "reverse", "pingpong"
                loop = (dir != "none");
                found = true;
                break;
            }
        }
        if (!found) {
            core::logWarn("[AssetManager] tag not found: %s in %s", tagName.c_str(), filePath.c_str());
        }
    }

    // 3) 填充 clip.frames
    from = std::max(0, from);
    to   = std::min(to, static_cast<int>(rawFrames.size()) - 1);
    clip.frames.clear();
    clip.duration = 0.f;
    for (int i = from; i <= to; ++i) {
        AnimationFrame af;
        af.srcRect = rawFrames[i].rect;
        af.duration = rawFrames[i].duration;
        clip.frames.push_back(af);
        clip.duration += af.duration;
    }
    clip.loop = loop;

    // Phase 2: 解析 events 数组
    // 形式: "events": [ { "time": 0.10, "name": "hitbox_on", "int": 0, "float": 0.0, "string": "" }, ... ]
    if (j.contains("events") && j["events"].is_array()) {
        for (auto& e : j["events"]) {
            AnimEvent ev;
            ev.time        = e.value("time", 0.f);
            ev.name        = e.value("name", std::string{});
            ev.intParam    = e.value("int", 0);
            ev.floatParam  = e.value("float", 0.f);
            ev.stringParam = e.value("string", std::string{});
            clip.events.push_back(std::move(ev));
        }
        std::sort(clip.events.begin(), clip.events.end(),
                  [](const AnimEvent& a, const AnimEvent& b) { return a.time < b.time; });
    }

    // 4) 加载 spritesheet 贴图 (meta.image 相对 JSON 文件目录)
    if (j.contains("meta") && j["meta"].contains("image")) {
        std::string dir = parentAssetPath(filePath);
        std::string img = joinAssetPath(dir, j["meta"]["image"].get<std::string>());
        clip.texture = loadTexture(img);
        if (!clip.texture.valid()) {
            core::logWarn("[AssetManager] failed to load spritesheet: %s", img.c_str());
        }
    }

    AnimationHandle h;
    h.index = nextAnimIndex_++;
    h.version = 1;
    uint32_t id = makeHandleKey(h.index, h.version);
    animByPath_[cachePath] = {h, 1, std::move(clip)};
    animPathById_[id] = cachePath;
    if (const std::string& assetId = assetIdForPath(AssetType::Animation, cachePath); !assetId.empty()) {
        animationAssetIdByHandle_[id] = assetId;
    }
    return h;
}

void AssetManager::releaseAnimation(AnimationHandle h) {
    if (!h.valid()) return;
    uint32_t id = makeHandleKey(h.index, h.version);
    auto pit = animPathById_.find(id);
    if (pit == animPathById_.end()) return;

    auto& entry = animByPath_[pit->second];
    if (--entry.refCount <= 0) {
        if (entry.clip.texture.valid()) releaseTexture(entry.clip.texture);
        animByPath_.erase(pit->second);
        animPathById_.erase(id);
        animationAssetIdByHandle_.erase(id);
    }
}

const std::string& AssetManager::animationPath(AnimationHandle h) const {
    if (!h.valid()) return kEmpty_;
    uint32_t id = makeHandleKey(h.index, h.version);
    auto it = animPathById_.find(id);
    return (it != animPathById_.end()) ? it->second : kEmpty_;
}

const AnimationClip* AssetManager::getAnimationClip(AnimationHandle h) const {
    if (!h.valid()) return nullptr;
    uint32_t id = makeHandleKey(h.index, h.version);
    auto pit = animPathById_.find(id);
    if (pit == animPathById_.end()) return nullptr;
    auto it = animByPath_.find(pit->second);
    if (it == animByPath_.end()) return nullptr;
    return &it->second.clip;
}

AnimationHandle AssetManager::registerAnimation(const std::string& name, const AnimationClip& clip) {
    AnimationHandle h;
    h.index = nextAnimIndex_++;
    h.version = 1;
    
    uint32_t id = makeHandleKey(h.index, h.version);
    animByPath_[name] = {h, 1, clip};
    animPathById_[id] = name;
    return h;
}

const std::string& AssetManager::texturePath(TextureHandle h) const {
    if (!h.valid()) return kEmpty_;
    uint32_t id = makeHandleKey(h.index, h.version);
    auto it = texPathById_.find(id);
    return (it != texPathById_.end()) ? it->second : kEmpty_;
}

const std::string& AssetManager::soundPath(SoundHandle h) const {
    if (!h.valid()) return kEmpty_;
    uint32_t id = makeHandleKey(h.index, h.version);
    auto it = sndPathById_.find(id);
    return (it != sndPathById_.end()) ? it->second : kEmpty_;
}

const std::string& AssetManager::textureAssetId(TextureHandle h) const {
    if (!h.valid()) return kEmpty_;
    auto it = textureAssetIdByHandle_.find(makeHandleKey(h.index, h.version));
    return (it != textureAssetIdByHandle_.end()) ? it->second : kEmpty_;
}

const std::string& AssetManager::soundAssetId(SoundHandle h) const {
    if (!h.valid()) return kEmpty_;
    auto it = soundAssetIdByHandle_.find(makeHandleKey(h.index, h.version));
    return (it != soundAssetIdByHandle_.end()) ? it->second : kEmpty_;
}

const std::string& AssetManager::animationAssetId(AnimationHandle h) const {
    if (!h.valid()) return kEmpty_;
    auto it = animationAssetIdByHandle_.find(makeHandleKey(h.index, h.version));
    return (it != animationAssetIdByHandle_.end()) ? it->second : kEmpty_;
}

const std::string& AssetManager::fontAssetId(FontHandle h) const {
    if (!h.valid()) return kEmpty_;
    auto it = fontAssetIdByHandle_.find(makeHandleKey(h.index, h.version));
    return (it != fontAssetIdByHandle_.end()) ? it->second : kEmpty_;
}

FontHandle AssetManager::loadFont(const std::string& path) {
    const std::string cachePath = normalizeAssetPath(path);
    auto it = fontByPath_.find(cachePath);
    if (it != fontByPath_.end()) {
        it->second.refCount++;
        if (const std::string& assetId = assetIdForPath(AssetType::Font, cachePath); !assetId.empty()) {
            fontAssetIdByHandle_[makeHandleKey(it->second.handle.index, it->second.handle.version)] = assetId;
        }
        return it->second.handle;
    }
    if (!render_) return {};

    const std::string binPath = cachePath + ".font";
    FontData data{};
    std::vector<uint8_t> atlas;
    std::vector<uint8_t> fontBytes;
    if (!readAssetBytes(binPath, fontBytes)
        || !loadFontBytes(fontBytes.data(), fontBytes.size(), binPath, data, atlas)) {
        core::logError("[AssetManager] failed to load font: %s (expected baked file %s)",
                       cachePath.c_str(), binPath.c_str());
        return {};
    }

    backend::TextureDesc desc{};
    desc.data     = atlas.data();
    desc.width    = static_cast<int>(data.atlasWidth);
    desc.height   = static_cast<int>(data.atlasHeight);
    desc.channels = 4;
    desc.filter   = backend::TextureFilter::Linear;  // MSDF 必须线性
    TextureHandle atlasTex = render_->createTexture(desc);
    if (!atlasTex.valid()) {
        core::logError("[AssetManager] failed to create font atlas texture: %s", cachePath.c_str());
        return {};
    }
    data.texture = atlasTex;

    FontHandle fh = render_->createFont(data);
    if (!fh.valid()) {
        render_->destroyTexture(atlasTex);
        return {};
    }

    uint32_t id = makeHandleKey(fh.index, fh.version);
    fontByPath_[cachePath]  = {fh, atlasTex, 1};
    fontPathById_[id]  = cachePath;
    if (const std::string& assetId = assetIdForPath(AssetType::Font, cachePath); !assetId.empty()) {
        fontAssetIdByHandle_[id] = assetId;
    }
    return fh;
}

void AssetManager::releaseFont(FontHandle h) {
    if (!h.valid() || !render_) return;
    uint32_t id = makeHandleKey(h.index, h.version);
    auto pit = fontPathById_.find(id);
    if (pit == fontPathById_.end()) return;

    auto& entry = fontByPath_[pit->second];
    if (--entry.refCount <= 0) {
        render_->destroyFont(h);
        if (entry.atlas.valid()) render_->destroyTexture(entry.atlas);
        fontByPath_.erase(pit->second);
        fontPathById_.erase(id);
        fontAssetIdByHandle_.erase(id);
    }
}

const std::string& AssetManager::fontPath(FontHandle h) const {
    if (!h.valid()) return kEmpty_;
    uint32_t id = makeHandleKey(h.index, h.version);
    auto it = fontPathById_.find(id);
    return (it != fontPathById_.end()) ? it->second : kEmpty_;
}

} // namespace engine
