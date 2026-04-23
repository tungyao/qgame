#pragma once
#include "AudioCommandQueue.h"
#include "IAudioDevice.h"
#include "../../platform/Thread.h"
#include <atomic>

namespace backend {

// 独立音频线程 — 主线程非阻塞 push 命令，AudioThread 异步消费
class AudioThread {
public:
    AudioThread(AudioCommandQueue& queue, IAudioDevice& device)
        : queue_(queue), device_(device) {}

    ~AudioThread() { stop(); }

    void start() {
        running_.store(true);
        thread_.start([this] { loop(); }, "AudioThread");
    }

    void stop() {
        running_.store(false);
        thread_.join();
    }

private:
    void loop() {
        while (running_.load(std::memory_order_relaxed)) {
            AudioCmd cmd{};
            while (queue_.pop(cmd)) {
                dispatch(cmd);
            }
            // 没有命令时短暂休眠，避免空转
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void dispatch(const AudioCmd& cmd) {
        switch (cmd.type) {
        case AudioCmd::Type::PLAY:
            device_.playSound(cmd.handle, cmd.vol);
            break;
        case AudioCmd::Type::STOP:
            device_.stopSound(cmd.handle);
            break;
        case AudioCmd::Type::SET_SPATIAL:
            device_.setSpatialPos(cmd.handle, cmd.x, cmd.y);
            break;
        case AudioCmd::Type::SET_LISTENER:
            device_.setListener(cmd.x, cmd.y);
            break;
        case AudioCmd::Type::PLAY_STREAM:
            device_.playStream(cmd.path, cmd.loop);
            break;
        case AudioCmd::Type::STOP_STREAM:
            device_.stopStream();
            break;
        }
    }

    AudioCommandQueue& queue_;
    IAudioDevice&      device_;
    platform::Thread   thread_;
    std::atomic<bool>  running_{false};
};

} // namespace backend
