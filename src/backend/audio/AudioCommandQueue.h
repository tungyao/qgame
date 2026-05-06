#pragma once
#include "../../core/containers/RingBuffer.h"
#include "../shared/ResourceHandle.h"
#include <cstring>

namespace backend {

struct AudioCmd {
    enum class Type {
        PLAY,
        STOP,
        STOP_ALL_SOUNDS,
        SET_SPATIAL,
        SET_LISTENER,
        PLAY_MUSIC,
        PLAY_STREAM,
        STOP_STREAM,
        PAUSE_MUSIC,
        RESUME_MUSIC,
        SEEK_MUSIC,
        SET_MASTER_VOLUME,
        SET_SOUND_VOLUME,
        SET_MUSIC_VOLUME
    };
    // LOAD/UNLOAD 无返回通道，走 GameAPI 同步路径，不经过命令队列

    Type        type;
    SoundHandle handle;
    float       x   = 0.f;
    float       y   = 0.f;
    float       vol = 1.f;
    bool        loop = false;
    char        path[256] = {};
};

// SPSC 无锁队列：主线程 push，AudioThread pop
class AudioCommandQueue {
    core::RingBuffer<AudioCmd, 1024> ring_;
public:
    // 主线程调用（非阻塞）
    bool push(const AudioCmd& cmd) { return ring_.push(cmd); }

    // AudioThread 调用
    bool pop(AudioCmd& out) { return ring_.pop(out); }

    bool empty() const { return ring_.empty(); }
};

} // namespace backend
