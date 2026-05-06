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
    SoundHandle loadSoundFromMemory(const void* data, size_t size, const char* debugName) override;
    void        unloadSound(SoundHandle h)                      override;
    void        playSound(SoundHandle h, float vol = 1.f)       override;
    void        stopSound(SoundHandle h)                        override;
    void        stopAllSounds()                                 override;
    void        playMusic(SoundHandle h, bool loop)             override;
    void        playStream(const char* path, bool loop)         override;
    void        stopStream()                                    override;
    void        pauseMusic()                                    override;
    void        resumeMusic()                                   override;
    void        seekMusic(float seconds)                        override;
    void        setMasterVolume(float volume)                   override;
    void        setSoundVolume(float volume)                    override;
    void        setMusicVolume(float volume)                    override;
    float       masterVolume() const                            override { return masterVolume_; }
    float       soundVolume() const                             override { return soundVolume_; }
    float       musicVolume() const                             override { return musicVolume_; }
    float       musicPositionSeconds() const                    override;
    float       musicDurationSeconds() const                    override;
    bool        musicPlaying() const                            override { return musicTrack_ != nullptr; }
    bool        musicPaused() const                             override;
    void        setSpatialPos(SoundHandle h, float x, float y)  override;
    void        setListener(float x, float y)                   override;

    // 每帧 GC：回收已自然播完的 track
    void        update()                                        override;

private:
    struct SoundEntry {
        MIX_Audio*              audio = nullptr;
        struct TrackInstance {
            MIX_Track* track = nullptr;
            float      baseVolume = 1.f;
        };
        std::vector<TrackInstance> tracks;
    };

    void gcTracks();
    void applySoundGains();
    void applyMusicGain();
    float effectiveSoundGain(float baseVolume) const;
    float effectiveMusicGain() const;

    MIX_Mixer* mixer_      = nullptr;
    MIX_Track* musicTrack_ = nullptr;
    MIX_Audio* musicAudio_ = nullptr;
    SoundHandle musicSound_{};
    bool musicUsesLoadedSound_ = false;

    std::unordered_map<uint32_t, SoundEntry> soundEntries_;
    uint32_t nextHandleIdx_ = 1;

// public 仅供文件级回调 trackStoppedCallback 访问，外部不应直接使用
public:
    std::mutex              gcMutex_;
    std::vector<MIX_Track*> finishedTracks_;
private:

    float listenerX_ = 0.f;
    float listenerY_ = 0.f;
    float masterVolume_ = 1.f;
    float soundVolume_ = 1.f;
    float musicVolume_ = 1.f;

    bool initialized_ = false;
};

} // namespace backend
