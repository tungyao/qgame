#pragma once
#include "../shared/ResourceHandle.h"

namespace backend {

class IAudioDevice {
public:
    virtual ~IAudioDevice() = default;

    virtual void init()     = 0;
    virtual void shutdown() = 0;

    virtual SoundHandle loadSound(const char* path)               = 0;
    virtual void        unloadSound(SoundHandle)                   = 0;
    virtual void        playSound(SoundHandle, float vol = 1.f)   = 0;
    virtual void        stopSound(SoundHandle)                     = 0;
    virtual void        playStream(const char* path, bool loop)   = 0;
    virtual void        stopStream()                               = 0;
    virtual void        setSpatialPos(SoundHandle, float x, float y) = 0;
    virtual void        setListener(float x, float y)             = 0;

    // 每帧由 AudioSystem 调用：GC 已停止的 track 等周期性工作
    virtual void        update()                                  = 0;
};

} // namespace backend
