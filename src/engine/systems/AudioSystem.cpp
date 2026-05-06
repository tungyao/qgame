#include "AudioSystem.h"
#include "../runtime/EngineContext.h"
#include "../../backend/audio/IAudioDevice.h"
#include "../../backend/audio/AudioCommandQueue.h"
#include "../../core/Logger.h"
#include <algorithm>
#include <cstring>

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
        case backend::AudioCmd::Type::STOP_ALL_SOUNDS:
            dev.stopAllSounds();                         break;
        case backend::AudioCmd::Type::SET_SPATIAL:
            dev.setSpatialPos(cmd.handle, cmd.x, cmd.y); break;
        case backend::AudioCmd::Type::SET_LISTENER:
            dev.setListener(cmd.x, cmd.y);               break;
        case backend::AudioCmd::Type::PLAY_MUSIC:
            dev.playMusic(cmd.handle, cmd.loop);         break;
        case backend::AudioCmd::Type::PLAY_STREAM:
            dev.playStream(cmd.path, cmd.loop);          break;
        case backend::AudioCmd::Type::STOP_STREAM:
            dev.stopStream();                            break;
        case backend::AudioCmd::Type::PAUSE_MUSIC:
            dev.pauseMusic();                            break;
        case backend::AudioCmd::Type::RESUME_MUSIC:
            dev.resumeMusic();                           break;
        case backend::AudioCmd::Type::SEEK_MUSIC:
            dev.seekMusic(cmd.x);                         break;
        case backend::AudioCmd::Type::SET_MASTER_VOLUME:
            dev.setMasterVolume(cmd.vol);                break;
        case backend::AudioCmd::Type::SET_SOUND_VOLUME:
            dev.setSoundVolume(cmd.vol);                 break;
        case backend::AudioCmd::Type::SET_MUSIC_VOLUME:
            dev.setMusicVolume(cmd.vol);                 break;
        }
    }

    dev.update();  // GC finished tracks
}

void AudioSystem::shutdown() {
    ctx_.audioDevice().shutdown();
}

float AudioSystem::clamp01(float value) {
    return std::clamp(value, 0.f, 1.f);
}

void AudioSystem::enqueue(const backend::AudioCmd& cmd) {
    if (!ctx_.audioCommandQueue().push(cmd)) {
        core::logWarn("Audio command queue full; dropping command");
    }
}

SoundHandle AudioSystem::loadSound(const char* assetPath) {
    return ctx_.assetManager.loadSound(assetPath ? assetPath : "");
}

SoundHandle AudioSystem::loadSoundById(const char* assetId) {
    return ctx_.assetManager.loadSoundById(assetId ? assetId : "");
}

void AudioSystem::releaseSound(SoundHandle h) {
    ctx_.assetManager.releaseSound(h);
}

void AudioSystem::playSound(SoundHandle h, float volume) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::PLAY;
    cmd.handle = h;
    cmd.vol = clamp01(volume);
    enqueue(cmd);
}

void AudioSystem::stopSound(SoundHandle h) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::STOP;
    cmd.handle = h;
    enqueue(cmd);
}

void AudioSystem::stopAllSounds() {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::STOP_ALL_SOUNDS;
    enqueue(cmd);
}

void AudioSystem::playMusic(SoundHandle h, bool loop) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::PLAY_MUSIC;
    cmd.handle = h;
    cmd.loop = loop;
    enqueue(cmd);
}

void AudioSystem::playMusic(const char* assetPath, bool loop) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::PLAY_STREAM;
    cmd.loop = loop;
    std::strncpy(cmd.path, assetPath ? assetPath : "", sizeof(cmd.path) - 1);
    enqueue(cmd);
}

void AudioSystem::playMusicById(const char* assetId, bool loop) {
    SoundHandle h = loadSoundById(assetId);
    if (h.valid()) playMusic(h, loop);
}

void AudioSystem::pauseMusic() {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::PAUSE_MUSIC;
    enqueue(cmd);
}

void AudioSystem::resumeMusic() {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::RESUME_MUSIC;
    enqueue(cmd);
}

void AudioSystem::seekMusic(float seconds) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::SEEK_MUSIC;
    cmd.x = std::max(0.f, seconds);
    enqueue(cmd);
}

void AudioSystem::stopMusic() {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::STOP_STREAM;
    enqueue(cmd);
}

void AudioSystem::setMasterVolume(float volume) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::SET_MASTER_VOLUME;
    cmd.vol = clamp01(volume);
    enqueue(cmd);
}

void AudioSystem::setSoundVolume(float volume) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::SET_SOUND_VOLUME;
    cmd.vol = clamp01(volume);
    enqueue(cmd);
}

void AudioSystem::setMusicVolume(float volume) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::SET_MUSIC_VOLUME;
    cmd.vol = clamp01(volume);
    enqueue(cmd);
}

float AudioSystem::masterVolume() const {
    return ctx_.audioDevice().masterVolume();
}

float AudioSystem::soundVolume() const {
    return ctx_.audioDevice().soundVolume();
}

float AudioSystem::musicVolume() const {
    return ctx_.audioDevice().musicVolume();
}

float AudioSystem::musicPositionSeconds() const {
    return ctx_.audioDevice().musicPositionSeconds();
}

float AudioSystem::musicDurationSeconds() const {
    return ctx_.audioDevice().musicDurationSeconds();
}

float AudioSystem::musicProgress() const {
    const float duration = musicDurationSeconds();
    return duration > 0.f ? clamp01(musicPositionSeconds() / duration) : 0.f;
}

bool AudioSystem::musicPlaying() const {
    return ctx_.audioDevice().musicPlaying();
}

bool AudioSystem::musicPaused() const {
    return ctx_.audioDevice().musicPaused();
}

void AudioSystem::setSpatialListener(float x, float y) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::SET_LISTENER;
    cmd.x = x;
    cmd.y = y;
    enqueue(cmd);
}

} // namespace engine
