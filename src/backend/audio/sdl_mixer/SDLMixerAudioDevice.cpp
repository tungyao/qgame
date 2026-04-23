#include "SDLMixerAudioDevice.h"
#include "../../../core/Logger.h"
#include <SDL3_mixer/SDL_mixer.h>
#include <algorithm>
#include <cmath>

namespace backend {

SDLMixerAudioDevice::~SDLMixerAudioDevice() {
    if (initialized_) shutdown();
}

void SDLMixerAudioDevice::init() {
    if (!MIX_Init()) {
        core::logError("MIX_Init failed: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec{};
    spec.format   = SDL_AUDIO_S16LE;
    spec.channels = 2;
    spec.freq     = 44100;
    mixer_ = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!mixer_) {
        core::logError("MIX_CreateMixerDevice failed: %s", SDL_GetError());
        MIX_Quit();
        return;
    }
    initialized_ = true;
    core::logInfo("SDLMixerAudioDevice initialized (SDL3_mixer 3.x API)");
}

void SDLMixerAudioDevice::shutdown() {
    if (!initialized_) return;

    MIX_StopAllTracks(mixer_, 0);

    for (auto& [idx, entry] : soundEntries_) {
        for (MIX_Track* t : entry.tracks) MIX_DestroyTrack(t);
        MIX_DestroyAudio(entry.audio);
    }
    soundEntries_.clear();

    if (musicTrack_) { MIX_DestroyTrack(musicTrack_); musicTrack_ = nullptr; }
    if (musicAudio_) { MIX_DestroyAudio(musicAudio_); musicAudio_ = nullptr; }

    // 清理回调还没来得及 GC 的 track（shutdown 时音频线程已停，无需加锁）
    for (MIX_Track* t : finishedTracks_) MIX_DestroyTrack(t);
    finishedTracks_.clear();

    MIX_DestroyMixer(mixer_);
    mixer_ = nullptr;
    MIX_Quit();
    initialized_ = false;
}

SoundHandle SDLMixerAudioDevice::loadSound(const char* path) {
    if (!initialized_) return {};
    MIX_Audio* audio = MIX_LoadAudio(mixer_, path, /*predecode=*/true);
    if (!audio) {
        core::logError("MIX_LoadAudio(%s) failed: %s", path, SDL_GetError());
        return {};
    }
    SoundHandle h{nextHandleIdx_++, 1};
    soundEntries_[h.index] = SoundEntry{audio, {}};
    core::logInfo("Loaded sound %s → handle %u", path, h.index);
    return h;
}

void SDLMixerAudioDevice::unloadSound(SoundHandle h) {
    if (!initialized_) return;
    auto it = soundEntries_.find(h.index);
    if (it == soundEntries_.end()) return;
    for (MIX_Track* t : it->second.tracks) MIX_DestroyTrack(t);
    MIX_DestroyAudio(it->second.audio);
    soundEntries_.erase(it);
}

// 音频线程回调（文件级 static，带 SDLCALL 以匹配 MIX_TrackStoppedCallback 类型）
// 仅做最小操作，主线程 gcTracks() 负责释放
// MIX_TrackStoppedCallback 签名：void(void* userdata, MIX_Track* track)
static void SDLCALL trackStoppedCallback(void* userdata, MIX_Track* track) {
    auto* self = static_cast<SDLMixerAudioDevice*>(userdata);
    std::lock_guard lock(self->gcMutex_);
    self->finishedTracks_.push_back(track);
}


void SDLMixerAudioDevice::gcTracks() {
    std::vector<MIX_Track*> done;
    {
        std::lock_guard lock(gcMutex_);
        done.swap(finishedTracks_);
    }
    if (done.empty()) return;

    for (MIX_Track* t : done) {
        // 从所有 soundEntry 的 tracks 向量中移除
        for (auto& [idx, entry] : soundEntries_) {
            auto& tv = entry.tracks;
            auto it = std::find(tv.begin(), tv.end(), t);
            if (it != tv.end()) {
                tv.erase(it);
                break;
            }
        }
        MIX_DestroyTrack(t);
    }
}

void SDLMixerAudioDevice::playSound(SoundHandle h, float vol) {
    if (!initialized_) return;
    auto it = soundEntries_.find(h.index);
    if (it == soundEntries_.end()) return;

    MIX_Track* track = MIX_CreateTrack(mixer_);
    if (!track) return;
    MIX_SetTrackAudio(track, it->second.audio);
    MIX_SetTrackGain(track, vol);
    MIX_SetTrackStoppedCallback(track, trackStoppedCallback, this);
    MIX_PlayTrack(track, 0);
    it->second.tracks.push_back(track);
}

void SDLMixerAudioDevice::stopSound(SoundHandle h) {
    if (!initialized_) return;
    auto it = soundEntries_.find(h.index);
    if (it == soundEntries_.end()) return;
    for (MIX_Track* t : it->second.tracks) {
        MIX_StopTrack(t, 0);
        // stop 触发 onTrackStopped → gcTracks() 里销毁；不在此处 DestroyTrack
    }
    // tracks 将在下一帧 gcTracks() 里清空
}

void SDLMixerAudioDevice::playStream(const char* path, bool loop) {
    if (!initialized_) return;
    stopStream();

    musicAudio_ = MIX_LoadAudio(mixer_, path, /*predecode=*/false);
    if (!musicAudio_) {
        core::logError("MIX_LoadAudio(%s) failed: %s", path, SDL_GetError());
        return;
    }
    musicTrack_ = MIX_CreateTrack(mixer_);
    if (!musicTrack_) { MIX_DestroyAudio(musicAudio_); musicAudio_ = nullptr; return; }

    MIX_SetTrackAudio(musicTrack_, musicAudio_);
    MIX_SetTrackLoops(musicTrack_, loop ? -1 : 0);
    MIX_PlayTrack(musicTrack_, 0);
}

void SDLMixerAudioDevice::stopStream() {
    if (!initialized_) return;
    if (musicTrack_) { MIX_StopTrack(musicTrack_, 0); MIX_DestroyTrack(musicTrack_); musicTrack_ = nullptr; }
    if (musicAudio_) { MIX_DestroyAudio(musicAudio_); musicAudio_ = nullptr; }
}

void SDLMixerAudioDevice::setSpatialPos(SoundHandle h, float x, float y) {
    if (!initialized_) return;
    auto it = soundEntries_.find(h.index);
    if (it == soundEntries_.end() || it->second.tracks.empty()) return;

    // Listener 固定 (0,0,0)，转为相对坐标
    MIX_Point3D pos{x - listenerX_, y - listenerY_, 0.f};
    for (MIX_Track* t : it->second.tracks) {
        MIX_SetTrack3DPosition(t, &pos);
    }
}

void SDLMixerAudioDevice::setListener(float x, float y) {
    listenerX_ = x;
    listenerY_ = y;
}

void SDLMixerAudioDevice::update() {
    gcTracks();
}

} // namespace backend
