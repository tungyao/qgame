#pragma once
#include "ISystem.h"
#include "../../backend/audio/AudioCommandQueue.h"
#include "../../backend/shared/ResourceHandle.h"
#include <string>

namespace engine {
class EngineContext;

class AudioSystem final : public ISystem {
public:
    explicit AudioSystem(EngineContext& ctx) : ctx_(ctx) {}

    void init()           override;
    void update(float dt) override;
    void shutdown()       override;

    SoundHandle loadSound(const char* assetPath);
    SoundHandle loadSoundById(const char* assetId);
    void releaseSound(SoundHandle h);

    void playSound(SoundHandle h, float volume = 1.f);
    void stopSound(SoundHandle h);
    void stopAllSounds();

    void playMusic(SoundHandle h, bool loop = true);
    void playMusic(const char* assetPath, bool loop = true);
    void playMusicById(const char* assetId, bool loop = true);
    void pauseMusic();
    void resumeMusic();
    void seekMusic(float seconds);
    void stopMusic();

    void setMasterVolume(float volume);
    void setSoundVolume(float volume);
    void setMusicVolume(float volume);
    float masterVolume() const;
    float soundVolume() const;
    float musicVolume() const;
    float musicPositionSeconds() const;
    float musicDurationSeconds() const;
    float musicProgress() const;
    bool musicPlaying() const;
    bool musicPaused() const;

    void setSpatialListener(float x, float y);

private:
    void enqueue(const backend::AudioCmd& cmd);
    static float clamp01(float value);

    EngineContext& ctx_;
};

} // namespace engine
