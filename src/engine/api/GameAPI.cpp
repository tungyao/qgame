#include "GameAPI.h"
#include "../runtime/EngineContext.h"
#include "../scene/SceneSerializer.h"
#include "../components/RenderComponents.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../backend/audio/AudioCommandQueue.h"
#include "../../backend/audio/IAudioDevice.h"
#include <SDL3/SDL.h>
#include <cstring>
#include <cstdio>


namespace engine {

entt::entity GameAPI::spawnEntity() {
    entt::entity e = ctx_.world.create();
    char buf[EntityID::MAX_LEN];
    std::snprintf(buf, sizeof(buf), "entity_%08x", static_cast<uint32_t>(e));
    ctx_.world.emplace<EntityID>(e, buf);
    return e;
}

void GameAPI::destroyEntity(entt::entity e) {
    ctx_.world.destroy(e);
}

// ── Audio ────────────────────────────────────────────────────────────────────

SoundHandle GameAPI::loadSound(const char* path) {
    return ctx_.audioDevice().loadSound(path);
}

void GameAPI::playSound(SoundHandle h, float vol) {
    backend::AudioCmd cmd{};
    cmd.type   = backend::AudioCmd::Type::PLAY;
    cmd.handle = h;
    cmd.vol    = vol;
    ctx_.audioCommandQueue().push(cmd);
}

void GameAPI::stopSound(SoundHandle h) {
    backend::AudioCmd cmd{};
    cmd.type   = backend::AudioCmd::Type::STOP;
    cmd.handle = h;
    ctx_.audioCommandQueue().push(cmd);
}

void GameAPI::playMusic(const char* path, bool loop) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::PLAY_STREAM;
    cmd.loop = loop;
    std::strncpy(cmd.path, path, sizeof(cmd.path) - 1);
    ctx_.audioCommandQueue().push(cmd);
}

void GameAPI::stopMusic() {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::STOP_STREAM;
    ctx_.audioCommandQueue().push(cmd);
}

void GameAPI::setSpatialListener(float x, float y) {
    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::SET_LISTENER;
    cmd.x    = x;
    cmd.y    = y;
    ctx_.audioCommandQueue().push(cmd);
}

// ── Input ────────────────────────────────────────────────────────────────────

bool  GameAPI::isKeyDown(int k)         const { return ctx_.inputState.isKeyDown(k); }
bool  GameAPI::isKeyJustPressed(int k)  const { return ctx_.inputState.isKeyJustPressed(k); }
bool  GameAPI::isKeyJustReleased(int k) const { return ctx_.inputState.isKeyJustReleased(k); }
bool  GameAPI::pointerDown(int id)      const { return ctx_.inputState.pointerDown(id); }
float GameAPI::pointerX(int id)         const { return ctx_.inputState.pointerX(id); }
float GameAPI::pointerY(int id)         const { return ctx_.inputState.pointerY(id); }

// ── Physics ──────────────────────────────────────────────────────────────────

void GameAPI::setGravity(float x, float y) {
    ctx_.systems.get<PhysicsSystem>().setGravity(x, y);
}

void GameAPI::setFixedTimestep(float step) {
    ctx_.systems.get<PhysicsSystem>().setFixedTimestep(step);
}

RaycastHit GameAPI::raycast(float startX, float startY, float dirX, float dirY, 
                            float maxDist, CollisionLayer layerMask) {
    return ctx_.systems.get<PhysicsSystem>().raycast(startX, startY, dirX, dirY, maxDist, layerMask);
}

std::vector<OverlapResult> GameAPI::overlapBox(float centerX, float centerY,
                                               float halfW, float halfH,
                                               CollisionLayer layerMask) {
    return ctx_.systems.get<PhysicsSystem>().overlapBox(centerX, centerY, halfW, halfH, layerMask);
}

std::vector<entt::entity> GameAPI::overlapCircle(float centerX, float centerY, float radius,
                                                  CollisionLayer layerMask) {
    return ctx_.systems.get<PhysicsSystem>().overlapCircle(centerX, centerY, radius, layerMask);
}

// ── Scene ────────────────────────────────────────────────────────────────────

bool GameAPI::loadScene(const char* path) {
    return SceneSerializer::loadScene(ctx_.world, ctx_.assetManager, path);
}

bool GameAPI::saveScene(const char* path) {
    return SceneSerializer::saveScene(ctx_.world, ctx_.assetManager, path);
}

void GameAPI::unloadScene() {
    ctx_.world.clear();
}

// ── Asset ─────────────────────────────────────────────────────────────────────

TextureHandle GameAPI::loadTexture(const char* path) {
    return ctx_.assetManager.loadTexture(path);
}

void GameAPI::releaseTexture(TextureHandle h) {
    ctx_.assetManager.releaseTexture(h);
}

FontHandle GameAPI::loadFont(const char* path) {
    return ctx_.assetManager.loadFont(path);
}

void GameAPI::releaseFont(FontHandle h) {
    ctx_.assetManager.releaseFont(h);
}

AssetManager& GameAPI::assetManager() {
    return ctx_.assetManager;
}

TextureHandle GameAPI::createTextureFromMemory(const void* rgbaPixels, int w, int h) {
    backend::TextureDesc desc{};
    desc.data   = rgbaPixels;
    desc.width  = w;
    desc.height = h;
    return ctx_.renderDevice().createTexture(desc);
}

AnimationHandle GameAPI::createAnimation(const char* name, const engine::AnimationClip& clip) {
    return ctx_.assetManager.registerAnimation(name, clip);
}

void GameAPI::quit() {
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

} // namespace engine
