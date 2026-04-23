#pragma once
#include "../IAudioDevice.h"
#include <unordered_map>
#include <vector>
#include <mutex>

struct MIX_Mixer;
struct MIX_Audio;
struct MIX_Track;

namespace backend {

class SDLMixerAudioDevice final : public IAudioDevice {
public:
    SDLMixerAudioDevice() = default;
    ~SDLMixerAudioDevice() override;

    void init()     override;
    void shutdown() override;

    SoundHandle loadSound(const char* path)                     override;
    void        unloadSound(SoundHandle h)                      override;
    void        playSound(SoundHandle h, float vol = 1.f)       override;
    void        stopSound(SoundHandle h)                        override;
    void        playStream(const char* path, bool loop)         override;
    void        stopStream()                                    override;
    void        setSpatialPos(SoundHandle h, float x, float y)  override;
    void        setListener(float x, float y)                   override;

    // 每帧 GC：回收已自然播完的 track
    void        update()                                        override;

private:
    struct SoundEntry {
        MIX_Audio*              audio = nullptr;
        std::vector<MIX_Track*> tracks;
    };

    void gcTracks();

    MIX_Mixer* mixer_      = nullptr;
    MIX_Track* musicTrack_ = nullptr;
    MIX_Audio* musicAudio_ = nullptr;

    std::unordered_map<uint32_t, SoundEntry> soundEntries_;
    uint32_t nextHandleIdx_ = 1;

// public 仅供文件级回调 trackStoppedCallback 访问，外部不应直接使用
public:
    std::mutex              gcMutex_;
    std::vector<MIX_Track*> finishedTracks_;
private:

    float listenerX_ = 0.f;
    float listenerY_ = 0.f;

    bool initialized_ = false;
};

} // namespace backend
