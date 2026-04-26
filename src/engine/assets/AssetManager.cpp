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

namespace engine {

const std::string AssetManager::kEmpty_;
uint32_t AssetManager::nextAnimIndex_ = 1;

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
        for (auto& [path, e] : texByPath_)
            render_->destroyTexture(e.handle);
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
}

TextureHandle AssetManager::loadTexture(const std::string& path) {
    auto it = texByPath_.find(path);
    if (it != texByPath_.end()) {
        it->second.refCount++;
        return it->second.handle;
    }

    if (!render_) return {};

    int w, h, ch;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pixels) {
        core::logError("[AssetManager] failed to load texture: %s", path.c_str());
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

    uint32_t id = (uint32_t(h_tex.index) << 12) | h_tex.version;
    texByPath_[path]  = {h_tex, 1};
    texPathById_[id]  = path;
    return h_tex;
}

SoundHandle AssetManager::loadSound(const std::string& path) {
    auto it = sndByPath_.find(path);
    if (it != sndByPath_.end()) {
        it->second.refCount++;
        return it->second.handle;
    }

    if (!audio_) return {};

    SoundHandle h_snd = audio_->loadSound(path.c_str());
    if (!h_snd.valid()) {
        core::logError("[AssetManager] failed to load sound: %s", path.c_str());
        return {};
    }

    uint32_t id = (uint32_t(h_snd.index) << 12) | h_snd.version;
    sndByPath_[path]  = {h_snd, 1};
    sndPathById_[id]  = path;
    return h_snd;
}

void AssetManager::releaseTexture(TextureHandle h) {
    if (!h.valid() || !render_) return;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto pit = texPathById_.find(id);
    if (pit == texPathById_.end()) return;

    auto& entry = texByPath_[pit->second];
    if (--entry.refCount <= 0) {
        render_->destroyTexture(h);
        texByPath_.erase(pit->second);
        texPathById_.erase(id);
    }
}

void AssetManager::releaseSound(SoundHandle h) {
    if (!h.valid() || !audio_) return;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto pit = sndPathById_.find(id);
    if (pit == sndPathById_.end()) return;

    auto& entry = sndByPath_[pit->second];
    if (--entry.refCount <= 0) {
        audio_->unloadSound(h);
        sndByPath_.erase(pit->second);
        sndPathById_.erase(id);
    }
}

// 解析 Aseprite JSON (Array 格式)。path 形如 "anim/player.json" 或 "anim/player.json#walk"
AnimationHandle AssetManager::loadAnimation(const std::string& path) {
    auto it = animByPath_.find(path);
    if (it != animByPath_.end()) {
        it->second.refCount++;
        return it->second.handle;
    }

    // 拆分 path 和 tag
    std::string filePath = path;
    std::string tagName;
    if (auto hash = path.find('#'); hash != std::string::npos) {
        filePath = path.substr(0, hash);
        tagName  = path.substr(hash + 1);
    }

    AnimationClip clip;
    clip.name = tagName.empty() ? filePath : tagName;
    clip.loop = true;

    std::ifstream ifs(filePath);
    if (!ifs.is_open()) {
        core::logError("[AssetManager] failed to open animation file: %s", filePath.c_str());
        return {};
    }

    nlohmann::json j;
    try {
        ifs >> j;
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
        std::filesystem::path dir = std::filesystem::path(filePath).parent_path();
        std::filesystem::path img = dir / j["meta"]["image"].get<std::string>();
        clip.texture = loadTexture(img.string());
        if (!clip.texture.valid()) {
            core::logWarn("[AssetManager] failed to load spritesheet: %s", img.string().c_str());
        }
    }

    AnimationHandle h;
    h.index = nextAnimIndex_++;
    h.version = 1;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    animByPath_[path] = {h, 1, std::move(clip)};
    animPathById_[id] = path;
    return h;
}

void AssetManager::releaseAnimation(AnimationHandle h) {
    if (!h.valid()) return;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto pit = animPathById_.find(id);
    if (pit == animPathById_.end()) return;

    auto& entry = animByPath_[pit->second];
    if (--entry.refCount <= 0) {
        if (entry.clip.texture.valid()) releaseTexture(entry.clip.texture);
        animByPath_.erase(pit->second);
        animPathById_.erase(id);
    }
}

const std::string& AssetManager::animationPath(AnimationHandle h) const {
    if (!h.valid()) return kEmpty_;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto it = animPathById_.find(id);
    return (it != animPathById_.end()) ? it->second : kEmpty_;
}

const AnimationClip* AssetManager::getAnimationClip(AnimationHandle h) const {
    if (!h.valid()) return nullptr;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
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
    
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    animByPath_[name] = {h, 1, clip};
    animPathById_[id] = name;
    return h;
}

const std::string& AssetManager::texturePath(TextureHandle h) const {
    if (!h.valid()) return kEmpty_;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto it = texPathById_.find(id);
    return (it != texPathById_.end()) ? it->second : kEmpty_;
}

const std::string& AssetManager::soundPath(SoundHandle h) const {
    if (!h.valid()) return kEmpty_;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto it = sndPathById_.find(id);
    return (it != sndPathById_.end()) ? it->second : kEmpty_;
}

FontHandle AssetManager::loadFont(const std::string& path) {
    auto it = fontByPath_.find(path);
    if (it != fontByPath_.end()) {
        it->second.refCount++;
        return it->second.handle;
    }
    if (!render_) return {};

    const std::string binPath = path + ".font";
    FontData data{};
    std::vector<uint8_t> atlas;
    if (!loadFontFile(binPath, data, atlas)) {
        core::logError("[AssetManager] failed to load font: %s (expected baked file %s)",
                       path.c_str(), binPath.c_str());
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
        core::logError("[AssetManager] failed to create font atlas texture: %s", path.c_str());
        return {};
    }
    data.texture = atlasTex;

    FontHandle fh = render_->createFont(data);
    if (!fh.valid()) {
        render_->destroyTexture(atlasTex);
        return {};
    }

    uint32_t id = (uint32_t(fh.index) << 12) | fh.version;
    fontByPath_[path]  = {fh, atlasTex, 1};
    fontPathById_[id]  = path;
    return fh;
}

void AssetManager::releaseFont(FontHandle h) {
    if (!h.valid() || !render_) return;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto pit = fontPathById_.find(id);
    if (pit == fontPathById_.end()) return;

    auto& entry = fontByPath_[pit->second];
    if (--entry.refCount <= 0) {
        render_->destroyFont(h);
        if (entry.atlas.valid()) render_->destroyTexture(entry.atlas);
        fontByPath_.erase(pit->second);
        fontPathById_.erase(id);
    }
}

const std::string& AssetManager::fontPath(FontHandle h) const {
    if (!h.valid()) return kEmpty_;
    uint32_t id = (uint32_t(h.index) << 12) | h.version;
    auto it = fontPathById_.find(id);
    return (it != fontPathById_.end()) ? it->second : kEmpty_;
}

} // namespace engine
