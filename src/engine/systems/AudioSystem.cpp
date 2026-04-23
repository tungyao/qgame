#include "AudioSystem.h"
#include "../runtime/EngineContext.h"
#include "../../backend/audio/IAudioDevice.h"
#include "../../backend/audio/AudioCommandQueue.h"
#include "../../core/Logger.h"

namespace engine {

void AudioSystem::init() {
    ctx_.audioDevice().init();
    core::logInfo("AudioSystem initialized");
}

void AudioSystem::update(float /*dt*/) {
    backend::IAudioDevice& dev = ctx_.audioDevice();
    backend::AudioCmd cmd{};

    while (ctx_.audioCommandQueue().pop(cmd)) {
        switch (cmd.type) {
        case backend::AudioCmd::Type::PLAY:
            dev.playSound(cmd.handle, cmd.vol);          break;
        case backend::AudioCmd::Type::STOP:
            dev.stopSound(cmd.handle);                   break;
        case backend::AudioCmd::Type::SET_SPATIAL:
            dev.setSpatialPos(cmd.handle, cmd.x, cmd.y); break;
        case backend::AudioCmd::Type::SET_LISTENER:
            dev.setListener(cmd.x, cmd.y);               break;
        case backend::AudioCmd::Type::PLAY_STREAM:
            dev.playStream(cmd.path, cmd.loop);          break;
        case backend::AudioCmd::Type::STOP_STREAM:
            dev.stopStream();                            break;
        }
    }

    dev.update();  // GC finished tracks
}

void AudioSystem::shutdown() {
    ctx_.audioDevice().shutdown();
}

} // namespace engine
