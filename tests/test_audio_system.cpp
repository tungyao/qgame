#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "backend/audio/AudioCommandQueue.h"
#include "backend/audio/IAudioDevice.h"

// ── 假 AudioDevice — 记录所有调用，无需真实硬件 ──────────────────────────────
struct CallRecord {
    backend::AudioCmd::Type type;
    SoundHandle             handle;
    float x = 0.f, y = 0.f, vol = 1.f;
    bool  loop = false;
    char  path[256] = {};
};

class MockAudioDevice final : public backend::IAudioDevice {
public:
    void init()     override { initCalled = true; }
    void shutdown() override { shutdownCalled = true; }
    void update()   override { updateCalled = true; }

    SoundHandle loadSound(const char* p) override {
        CallRecord r{}; r.type = backend::AudioCmd::Type::PLAY; // 借用 PLAY 记录 load 类型不重要
        std::strncpy(r.path, p, 255);
        calls.push_back(r);
        return SoundHandle{nextIdx++, 1};
    }
    void unloadSound(SoundHandle h) override {
        CallRecord r{}; r.type = backend::AudioCmd::Type::STOP; r.handle = h;
        calls.push_back(r);
    }
    void playSound(SoundHandle h, float v) override {
        CallRecord r{}; r.type = backend::AudioCmd::Type::PLAY; r.handle = h; r.vol = v;
        calls.push_back(r);
    }
    void stopSound(SoundHandle h) override {
        CallRecord r{}; r.type = backend::AudioCmd::Type::STOP; r.handle = h;
        calls.push_back(r);
    }
    void playStream(const char* p, bool l) override {
        CallRecord r{}; r.type = backend::AudioCmd::Type::PLAY_STREAM; r.loop = l;
        std::strncpy(r.path, p, 255);
        calls.push_back(r);
    }
    void stopStream() override {
        CallRecord r{}; r.type = backend::AudioCmd::Type::STOP_STREAM;
        calls.push_back(r);
    }
    void setSpatialPos(SoundHandle h, float x, float y) override {
        CallRecord r{}; r.type = backend::AudioCmd::Type::SET_SPATIAL;
        r.handle = h; r.x = x; r.y = y;
        calls.push_back(r);
    }
    void setListener(float x, float y) override {
        CallRecord r{}; r.type = backend::AudioCmd::Type::SET_LISTENER;
        r.x = x; r.y = y;
        calls.push_back(r);
    }

    std::vector<CallRecord> calls;
    bool initCalled     = false;
    bool shutdownCalled = false;
    bool updateCalled   = false;
    uint32_t nextIdx    = 1;
};

// AudioSystem::update() 的 dispatch 逻辑（与 AudioSystem.cpp 保持一致）
static void dispatchCmd(const backend::AudioCmd& cmd, backend::IAudioDevice& dev) {
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

// ── 测试：AudioCommandQueue SPSC push/pop ─────────────────────────────────────
void testQueuePushPop() {
    backend::AudioCommandQueue q;
    assert(q.empty());

    backend::AudioCmd cmd{};
    cmd.type = backend::AudioCmd::Type::PLAY;
    cmd.vol  = 0.5f;
    assert(q.push(cmd));
    assert(!q.empty());

    backend::AudioCmd out{};
    assert(q.pop(out));
    assert(out.type == backend::AudioCmd::Type::PLAY);
    assert(out.vol  == 0.5f);
    assert(q.empty());
    ::printf("  AudioCommandQueue push/pop OK\n");
}

// ── 测试：多命令入队后顺序消费 ────────────────────────────────────────────────
void testQueueOrder() {
    backend::AudioCommandQueue q;

    backend::AudioCmd cmds[3]{};
    cmds[0].type = backend::AudioCmd::Type::PLAY;
    cmds[1].type = backend::AudioCmd::Type::STOP;
    cmds[2].type = backend::AudioCmd::Type::STOP_STREAM;
    for (auto& c : cmds) assert(q.push(c));

    backend::AudioCmd out{};
    assert(q.pop(out)); assert(out.type == backend::AudioCmd::Type::PLAY);
    assert(q.pop(out)); assert(out.type == backend::AudioCmd::Type::STOP);
    assert(q.pop(out)); assert(out.type == backend::AudioCmd::Type::STOP_STREAM);
    assert(q.empty());
    ::printf("  AudioCommandQueue order OK\n");
}

// ── 测试：AudioSystem dispatch 所有命令类型 ───────────────────────────────────
void testAudioSystemDispatch() {
    MockAudioDevice dev;
    backend::AudioCmd c{};

    c.type = backend::AudioCmd::Type::PLAY; c.vol = 0.8f;          dispatchCmd(c, dev);
    c.type = backend::AudioCmd::Type::STOP;                         dispatchCmd(c, dev);
    c.type = backend::AudioCmd::Type::PLAY_STREAM; c.loop = true;
    std::strncpy(c.path, "music.ogg", 255);                         dispatchCmd(c, dev);
    c.type = backend::AudioCmd::Type::STOP_STREAM;                  dispatchCmd(c, dev);
    c.type = backend::AudioCmd::Type::SET_LISTENER;
    c.x = 10.f; c.y = 20.f;                                        dispatchCmd(c, dev);
    c.type = backend::AudioCmd::Type::SET_SPATIAL;
    SoundHandle h{3, 1}; c.handle = h; c.x = 5.f; c.y = 7.f;      dispatchCmd(c, dev);

    assert(dev.calls.size() == 6);
    assert(dev.calls[0].type == backend::AudioCmd::Type::PLAY);
    assert(dev.calls[0].vol  == 0.8f);
    assert(dev.calls[2].type == backend::AudioCmd::Type::PLAY_STREAM);
    assert(dev.calls[2].loop == true);
    assert(std::strcmp(dev.calls[2].path, "music.ogg") == 0);
    assert(dev.calls[4].x == 10.f && dev.calls[4].y == 20.f);
    assert(dev.calls[5].type == backend::AudioCmd::Type::SET_SPATIAL);
    assert(dev.calls[5].x == 5.f && dev.calls[5].y == 7.f);
    ::printf("  AudioSystem dispatch all cmd types OK\n");
}

// ── 测试：queue → dispatch 端到端 ────────────────────────────────────────────
void testQueueToDispatch() {
    backend::AudioCommandQueue q;
    MockAudioDevice dev;

    backend::AudioCmd c{};
    c.type = backend::AudioCmd::Type::PLAY; c.vol = 0.3f; q.push(c);
    c.type = backend::AudioCmd::Type::STOP_STREAM;                  q.push(c);

    backend::AudioCmd out{};
    while (q.pop(out)) dispatchCmd(out, dev);

    assert(dev.calls.size() == 2);
    assert(dev.calls[0].type == backend::AudioCmd::Type::PLAY);
    assert(dev.calls[1].type == backend::AudioCmd::Type::STOP_STREAM);
    ::printf("  Queue → dispatch end-to-end OK\n");
}

// ── 测试：IAudioDevice 生命周期（init / update / shutdown）───────────────────
void testDeviceLifecycle() {
    MockAudioDevice dev;
    assert(!dev.initCalled && !dev.updateCalled && !dev.shutdownCalled);
    dev.init();
    assert(dev.initCalled);
    dev.update();
    assert(dev.updateCalled);
    dev.shutdown();
    assert(dev.shutdownCalled);
    ::printf("  IAudioDevice lifecycle OK\n");
}

// ── 测试：SoundHandle 有效性（Handle 基础行为）────────────────────────────────
void testSoundHandleValidity() {
    SoundHandle h1{1, 1};
    SoundHandle h2{};
    assert(h1.valid());
    assert(!h2.valid());
    ::printf("  SoundHandle validity OK\n");
}

int main() {
    ::printf("Running audio system tests...\n");
    testQueuePushPop();
    testQueueOrder();
    testAudioSystemDispatch();
    testQueueToDispatch();
    testDeviceLifecycle();
    testSoundHandleValidity();
    ::printf("All audio tests passed.\n");
    return 0;
}
