#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include "../../backend/shared/ResourceHandle.h"
#include "../components/AnimatorComponent.h"
#include "../components/FontData.h"

namespace backend {
class IRenderDevice;
class IAudioDevice;
}

namespace engine {

// 按路径缓存 Texture / Sound / Animation / Font，引用计数，统一生命周期管理。
//
// 资源管线约定：
// - Phase 1: manifest 提供稳定 asset ID，运行时可按 ID 加载。
// - Phase 2: 资源编译器生成 baked manifest，运行时仍只看 manifest。
// - Phase 3: 场景/游戏逻辑优先保存 asset ID，路径仅作为旧数据兼容回退。
// - Phase 5+: pack/VFS 接入时只需要让 resolveManifestPath 返回虚拟路径或挂载路径。
class AssetManager {
public:
    void init(backend::IRenderDevice* render, backend::IAudioDevice* audio);
    void shutdown();

    // 返回已缓存的 handle，不存在则加载；引用计数 +1
    TextureHandle loadTexture(const std::string& path);
    SoundHandle   loadSound(const std::string& path);
    AnimationHandle loadAnimation(const std::string& path);
    // path 是 ttf 路径；实际加载 sibling "<path>.font"（编译阶段预烘焙，本地阶段手工生成）。
    FontHandle    loadFont(const std::string& path);

    // Phase 1/3 resource pipeline: manifest + stable asset IDs.
    bool loadManifest(const std::string& path);
    bool hasAsset(const std::string& id) const;
    TextureHandle loadTextureById(const std::string& id);
    SoundHandle   loadSoundById(const std::string& id);
    AnimationHandle loadAnimationById(const std::string& id);
    FontHandle    loadFontById(const std::string& id);
    std::vector<std::string> assetIds() const;

    // 引用计数 -1，归零时销毁 GPU 资源
    void releaseTexture(TextureHandle h);
    void releaseSound(SoundHandle h);
    void releaseAnimation(AnimationHandle h);
    void releaseFont(FontHandle h);

    // 返回与 base 关联的 region ID 图（sibling "<path>.id.png"）。无则返回空 handle。
    TextureHandle regionIdTexture(TextureHandle base) const;

    // 通过 handle 反查路径（SceneSerializer 序列化时用）
    const std::string& texturePath(TextureHandle h) const;
    const std::string& soundPath(SoundHandle h)    const;
    const std::string& animationPath(AnimationHandle h) const;
    const std::string& fontPath(FontHandle h) const;

    // 通过 handle 反查稳定 asset ID。为空表示该资源不是从当前 manifest 加载，
    // 或者是程序化创建的临时资源。SceneSerializer 会优先保存这些 ID。
    const std::string& textureAssetId(TextureHandle h) const;
    const std::string& soundAssetId(SoundHandle h) const;
    const std::string& animationAssetId(AnimationHandle h) const;
    const std::string& fontAssetId(FontHandle h) const;

    // 获取动画剪辑数据
    const AnimationClip* getAnimationClip(AnimationHandle h) const;

    // 直接注册动画剪辑（用于程序化创建，不依赖文件）
    AnimationHandle registerAnimation(const std::string& name, const AnimationClip& clip);

private:
    enum class AssetType {
        Texture,
        Sound,
        Animation,
        Font,
        Unknown
    };

    struct AssetRecord {
        std::string id;
        AssetType   type = AssetType::Unknown;
        std::string source;
        std::string baked;
    };

    struct PackEntry {
        uint64_t offset = 0;
        uint64_t size = 0;
        std::string sha256;
    };

    struct MountedPack {
        std::string path;
        std::unordered_map<std::string, PackEntry> files;
    };

    backend::IRenderDevice* render_ = nullptr;
    backend::IAudioDevice*  audio_  = nullptr;

    struct TexEntry { TextureHandle handle; int refCount = 0; TextureHandle regionId; };
    struct SndEntry { SoundHandle   handle; int refCount = 0; };
    struct AnimEntry { AnimationHandle handle; int refCount = 0; AnimationClip clip; };
    struct FontEntry { FontHandle handle; TextureHandle atlas; int refCount = 0; };

    std::unordered_map<std::string, TexEntry> texByPath_;
    std::unordered_map<std::string, SndEntry> sndByPath_;
    std::unordered_map<std::string, AnimEntry> animByPath_;
    std::unordered_map<std::string, FontEntry> fontByPath_;
    std::unordered_map<std::string, AssetRecord> assetsById_;
    std::unordered_map<std::string, MountedPack> packsById_;
    std::string manifestDir_;

    // handle -> path 反查表。key 使用 Handle 的 20-bit index + 12-bit version，
    // 与 core::Handle 的布局保持一致，后续若 Handle 变宽只需要替换 makeHandleKey。
    std::unordered_map<uint32_t, std::string> texPathById_;
    std::unordered_map<uint32_t, std::string> sndPathById_;
    std::unordered_map<uint32_t, std::string> animPathById_;
    std::unordered_map<uint32_t, std::string> fontPathById_;

    // resolved path -> manifest id。直接按路径加载时也能自动关联 ID，
    // 这是 Phase 3 场景渐进迁移的关键：旧代码 loadTexture("x.png") 后保存场景，
    // 如果 x.png 已在 manifest 中声明，输出会自动变成 assetId。
    std::unordered_map<std::string, std::string> textureIdByPath_;
    std::unordered_map<std::string, std::string> soundIdByPath_;
    std::unordered_map<std::string, std::string> animationIdByPath_;
    std::unordered_map<std::string, std::string> fontIdByPath_;

    // handle -> manifest id。资源释放时同步清理，避免 handle 复用后串 ID。
    std::unordered_map<uint32_t, std::string> textureAssetIdByHandle_;
    std::unordered_map<uint32_t, std::string> soundAssetIdByHandle_;
    std::unordered_map<uint32_t, std::string> animationAssetIdByHandle_;
    std::unordered_map<uint32_t, std::string> fontAssetIdByHandle_;

    static const std::string kEmpty_;
    static uint32_t nextAnimIndex_;

    static AssetType parseAssetType(const std::string& type);
    static uint32_t makeHandleKey(uint32_t index, uint32_t version);
    static bool isPackPath(const std::string& path);
    static bool splitPackPath(const std::string& path, std::string& outPackId, std::string& outFile);
    static std::string normalizeAssetPath(const std::string& path);
    static std::string siblingRegionIdPath(const std::string& path);
    static std::string parentAssetPath(const std::string& path);
    static std::string joinAssetPath(const std::string& baseDir, const std::string& child);
    std::string resolveManifestPath(const AssetRecord& rec) const;
    void indexManifestRecord(const AssetRecord& rec);
    const std::string& assetIdForPath(AssetType type, const std::string& path) const;
    bool mountPack(const std::string& id, const std::string& path);
    bool readAssetBytes(const std::string& path, std::vector<uint8_t>& out) const;
    bool assetPathExists(const std::string& path) const;
};

} // namespace engine
