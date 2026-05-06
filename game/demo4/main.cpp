#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <engine/api/GameAPI.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/UIComponents.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/runtime/EngineContext.h>

#ifndef QGAME_BAKED_MANIFEST
#define QGAME_BAKED_MANIFEST "assets/manifest.baked.json"
#endif

namespace {

bool hasArg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

std::string formatTime(float seconds) {
    const int total = std::max(0, static_cast<int>(seconds + 0.5f));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", total / 60, total % 60);
    return buf;
}

entt::entity addText(engine::GameAPI& api,
                     entt::entity parent,
                     engine::FontHandle font,
                     const char* text,
                     float x,
                     float y,
                     float w,
                     float h,
                     float size,
                     core::Color color,
                     int align = 0) {
    auto e = api.createUIText(w, h, text);
    api.setUIParent(e, parent);
    api.setUIAnchor(e, 0.f, 0.f, 0.f, 0.f);
    api.setUIPivot(e, 0.f, 0.f);
    api.setUIOffset(e, x, y);
    api.setUITextFont(e, font, size);
    api.setUITextColor(e, color);
    api.setUITextAlignment(e, align);
    return e;
}

entt::entity addButton(engine::GameAPI& api,
                       entt::entity parent,
                       engine::FontHandle font,
                       const char* text,
                       float x,
                       float y,
                       float w,
                       float h,
                       std::function<void()> onClick) {
    auto e = api.createButton(w, h, std::move(onClick));
    api.setUIParent(e, parent);
    api.setUIAnchor(e, 0.f, 0.f, 0.f, 0.f);
    api.setUIPivot(e, 0.f, 0.f);
    api.setUIOffset(e, x, y);
    api.setButtonColors(e,
                        core::Color{52, 85, 120, 255},
                        core::Color{68, 108, 150, 255},
                        core::Color{32, 58, 86, 255});

    auto label = api.createUIText(w, h, text);
    api.setUIParent(label, e);
    api.setUIAnchor(label, 0.f, 0.f, 1.f, 1.f);
    api.setUIPivot(label, 0.5f, 0.5f);
    api.setUIOffset(label, 0.f, 0.f);
    api.setUITextFont(label, font, 17.f);
    api.setUITextColor(label, core::Color{238, 245, 252, 255});
    api.setUITextAlignment(label, 1);
    return e;
}

entt::entity addSlider(engine::GameAPI& api,
                       entt::entity parent,
                       float x,
                       float y,
                       float w,
                       float value,
                       std::function<void(float)> onChanged) {
    auto e = api.createSlider(w, 28.f, 0.f, 1.f, std::move(onChanged));
    api.setUIParent(e, parent);
    api.setUIAnchor(e, 0.f, 0.f, 0.f, 0.f);
    api.setUIPivot(e, 0.f, 0.f);
    api.setUIOffset(e, x, y);
    api.setSliderValue(e, value);
    return e;
}

} // namespace

int main(int argc, char** argv) {
    const bool useOpenGL = hasArg(argc, argv, "--opengl");
    const bool autoExit = hasArg(argc, argv, "--auto-exit");

    engine::EngineConfig cfg;
    cfg.windowTitle = "QGame Demo4 - Audio Controls";
    cfg.windowWidth = 1280;
    cfg.windowHeight = 720;
    cfg.vsync = true;
    if (useOpenGL) cfg.renderBackend = engine::RenderBackend::OpenGL;

    engine::EngineContext ctx;
    ctx.init(cfg);
    engine::GameAPI api{ctx};

    api.loadAssetManifest(QGAME_BAKED_MANIFEST);
    engine::FontHandle font = api.loadFontById("font.demo.main");
    SoundHandle music = api.loadSoundById("audio.demo4.music");
    SoundHandle click = api.loadSoundById("audio.demo4.click");

    auto screenCam = api.spawnEntity();
    api.addComponent(screenCam, engine::Transform{0.f, 0.f});
    engine::Camera cam{};
    cam.zoom = 1.f;
    cam.primary = true;
    cam.depth = 0;
    cam.layerMask = engine::renderPassBit(engine::RenderPass::Screen);
    cam.clear = true;
    cam.clearColor = core::Color{18, 22, 28, 255};
    cam.cullEnabled = false;
    api.addComponent(screenCam, cam);

    auto canvas = api.createCanvas(1280, 720);
    api.setCanvasScaleMode(canvas, true);

    auto panel = api.createUIElement(canvas);
    api.setUIAnchor(panel, 0.5f, 0.5f, 0.5f, 0.5f);
    api.setUIPivot(panel, 0.5f, 0.5f);
    api.setUISize(panel, 720.f, 430.f);
    api.setUIOffset(panel, 0.f, 0.f);
    api.setUIBackground(panel, core::Color{30, 38, 48, 255});

    addText(api, panel, font, "Demo4 Audio System", 34.f, 26.f, 650.f, 38.f, 28.f,
            core::Color{245, 248, 250, 255});
    auto status = addText(api, panel, font, "Ready", 34.f, 66.f, 650.f, 28.f, 16.f,
                          core::Color{172, 190, 205, 255});

    addButton(api, panel, font, "Play Music", 34.f, 116.f, 150.f, 46.f, [&] {
        api.playMusic(music, true);
    });
    addButton(api, panel, font, "Pause", 200.f, 116.f, 118.f, 46.f, [&] {
        api.pauseMusic();
    });
    addButton(api, panel, font, "Resume", 334.f, 116.f, 118.f, 46.f, [&] {
        api.resumeMusic();
    });
    addButton(api, panel, font, "Stop", 468.f, 116.f, 94.f, 46.f, [&] {
        api.stopMusic();
    });
    addButton(api, panel, font, "SFX", 578.f, 116.f, 94.f, 46.f, [&] {
        api.playSound(click, 1.f);
    });

    addText(api, panel, font, "Progress", 34.f, 190.f, 120.f, 24.f, 16.f,
            core::Color{210, 220, 230, 255});
    auto progress = api.createProgressBar(480.f, 18.f);
    api.setUIParent(progress, panel);
    api.setUIAnchor(progress, 0.f, 0.f, 0.f, 0.f);
    api.setUIPivot(progress, 0.f, 0.f);
    api.setUIOffset(progress, 158.f, 193.f);
    api.setProgressColors(progress,
                          core::Color{16, 20, 25, 255},
                          core::Color{80, 188, 140, 255});
    auto timeLabel = addText(api, panel, font, "00:00 / 00:00", 528.f, 218.f, 140.f, 24.f, 15.f,
                             core::Color{176, 192, 206, 255}, 2);

    bool seeking = false;
    auto seekSlider = addSlider(api, panel, 158.f, 224.f, 340.f, 0.f, [&](float value) {
        const float duration = api.musicDurationSeconds();
        if (duration > 0.f) {
            seeking = true;
            api.seekMusic(duration * value);
        }
    });

    addText(api, panel, font, "Master", 34.f, 276.f, 120.f, 24.f, 16.f,
            core::Color{210, 220, 230, 255});
    addSlider(api, panel, 158.f, 274.f, 340.f, 0.85f, [&](float v) {
        api.setMasterVolume(v);
    });

    addText(api, panel, font, "Music", 34.f, 320.f, 120.f, 24.f, 16.f,
            core::Color{210, 220, 230, 255});
    addSlider(api, panel, 158.f, 318.f, 340.f, 0.80f, [&](float v) {
        api.setMusicVolume(v);
    });

    addText(api, panel, font, "Effects", 34.f, 364.f, 120.f, 24.f, 16.f,
            core::Color{210, 220, 230, 255});
    addSlider(api, panel, 158.f, 362.f, 340.f, 0.90f, [&](float v) {
        api.setSoundVolume(v);
    });

    float t = 0.f;
    while (ctx.scheduler.tick()) {
        const float dt = ctx.scheduler.deltaTime();
        t += dt;

        const float pos = api.musicPositionSeconds();
        const float dur = api.musicDurationSeconds();
        const float prog = api.musicProgress();
        api.setProgressValue(progress, prog);
        if (!seeking) {
            if (auto* slider = ctx.world.try_get<engine::UISlider>(seekSlider)) {
                slider->value = prog;
            }
        }
        seeking = false;

        const std::string state = api.musicPlaying()
            ? (api.musicPaused() ? "Paused" : "Playing")
            : "Stopped";
        api.setUIText(status, (state + " | master " + std::to_string(static_cast<int>(api.masterVolume() * 100.f)) +
                               "% | music " + std::to_string(static_cast<int>(api.musicVolume() * 100.f)) +
                               "% | sfx " + std::to_string(static_cast<int>(api.soundVolume() * 100.f)) + "%").c_str());
        api.setUIText(timeLabel, (formatTime(pos) + " / " + formatTime(dur)).c_str());

        if (api.isKeyJustPressed(SDLK_SPACE)) {
            if (api.musicPaused()) api.resumeMusic();
            else api.pauseMusic();
        }
        if (api.isKeyJustPressed(SDLK_RETURN)) api.playMusic(music, true);
        if (api.isKeyJustPressed(SDLK_ESCAPE)) {
            api.quit();
            break;
        }
        if (autoExit && t >= 2.0f) {
            api.quit();
            break;
        }
    }

    ctx.shutdown();
    return (font.valid() && music.valid() && click.valid()) ? 0 : 1;
}
