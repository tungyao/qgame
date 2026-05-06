#pragma once
#include "../shared/ResourceHandle.h"
#include <cstddef>

namespace backend {

class IAudioDevice {
public:
    virtual ~IAudioDevice() = default;

    virtual void init()     = 0;
    virtual void shutdown() = 0;

    virtual SoundHandle loadSound(const char* path)               = 0;
    virtual SoundHandle loadSoundFromMemory(const void* data, size_t size, const char* debugName) = 0;
    virtual void        unloadSound(SoundHandle)                   = 0;
    virtual void        playSound(SoundHandle, float vol = 1.f)   = 0;
    virtual void        stopSound(SoundHandle)                     = 0;
    virtual void        stopAllSounds()                            = 0;
    virtual void        playMusic(SoundHandle, bool loop)          = 0;
    virtual void        playStream(const char* path, bool loop)   = 0;
    virtual void        stopStream()                               = 0;
    virtual void        pauseMusic()                               = 0;
    virtual void        resumeMusic()                              = 0;
    virtual void        seekMusic(float seconds)                   = 0;
    virtual void        setMasterVolume(float volume)              = 0;
    virtual void        setSoundVolume(float volume)               = 0;
    virtual void        setMusicVolume(float volume)               = 0;
    virtual float       masterVolume() const                       = 0;
    virtual float       soundVolume() const                        = 0;
    virtual float       musicVolume() const                        = 0;
    virtual float       musicPositionSeconds() const               = 0;
    virtual float       musicDurationSeconds() const               = 0;
    virtual bool        musicPlaying() const                       = 0;
    virtual bool        musicPaused() const                        = 0;
    virtual void        setSpatialPos(SoundHandle, float x, float y) = 0;
    virtual void        setListener(float x, float y)             = 0;

    // 每帧由 AudioSystem 调用：GC 已停止的 track 等周期性工作
    virtual void        update()                                  = 0;
};

} // namespace backend
