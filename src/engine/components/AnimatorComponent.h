#pragma once
#include <string>
#include <vector>
#include "../../backend/shared/ResourceHandle.h"
#include "../../core/math/Rect.h"

namespace engine {

struct AnimationFrame {
    core::Rect srcRect;     // 在 spritesheet 内的像素区域
    float      duration;    // 本帧持续时长 (秒)
};

struct AnimationClip {
    std::string                 name;        // tag 名 (或文件名)
    TextureHandle               texture;     // 关联的 spritesheet
    std::vector<AnimationFrame> frames;
    float                       duration = 0.f; // 所有帧累计时长
    bool                        loop = true;
};

// Phase 1: 播放选项
enum class PlayMode : uint8_t {
    Once,
    Loop,        // 强制循环（覆盖 clip.loop）
    PingPong,    // 预留，Phase 5 实现真正反向；当前等同 Loop
    ClipDefault, // 跟随 clip.loop
};

struct PlayOptions {
    int      priority     = 0;       // 数值越大越高
    bool     forceRestart = false;   // 同 clip 也重置 time
    float    startTime    = 0.f;     // 起始时间
    float    speed        = 1.f;
    PlayMode mode         = PlayMode::ClipDefault;
};

struct AnimatorComponent {
    // ── 当前播放 ──────────────────────────────────────────────────────────
    AnimationHandle currentAnim;
    float           time    = 0.f;
    float           speed   = 1.f;
    bool            playing = false;
    bool            applyTexture = true;
    bool            finished = false;

    // ── Phase 1: 优先级 / 打断 / 队列 ─────────────────────────────────────
    int             currentPriority = 0;
    bool            interruptible   = true;     // 当前播放是否允许被同/低优先级打断
    PlayMode        currentMode     = PlayMode::ClipDefault;

    // 待播放请求 (高优先级 / 已锁定时延后)
    AnimationHandle queuedAnim;
    PlayOptions     queuedOpts{};
    bool            hasQueued = false;

    // ── 兼容旧接口 ────────────────────────────────────────────────────────
    void play(AnimationHandle anim) {
        PlayOptions o{};
        play(anim, o);
    }

    // 主入口：按优先级决策立刻播 / 入队 / 丢弃
    void play(AnimationHandle anim, const PlayOptions& opts) {
        if (!anim.valid()) return;

        const bool same = (currentAnim == anim);

        // 已有不可打断的高优先级 clip 在播：低优先级请求入队
        if (playing && !interruptible && opts.priority < currentPriority) {
            queuedAnim = anim;
            queuedOpts = opts;
            hasQueued  = true;
            return;
        }

        // 同 clip 不重启 (除非强制)
        if (same && playing && !opts.forceRestart) {
            speed           = opts.speed;
            currentPriority = opts.priority > currentPriority ? opts.priority : currentPriority;
            return;
        }

        currentAnim     = anim;
        time            = opts.startTime;
        speed           = opts.speed;
        playing         = true;
        finished        = false;
        currentPriority = opts.priority;
        currentMode     = opts.mode;
        interruptible   = true; // 新请求默认可打断；调用方需要锁定可在 play 后置 false
        hasQueued       = false;
    }

    // 排队：当前播完后再播
    void queue(AnimationHandle anim, const PlayOptions& opts = {}) {
        queuedAnim = anim;
        queuedOpts = opts;
        hasQueued  = true;
    }

    // 标记当前 clip 不可被打断 (锁定 startup/active 帧)
    void lock()   { interruptible = false; }
    void unlock() { interruptible = true; }

    void stop() {
        playing = false;
        time = 0.f;
        finished = false;
        hasQueued = false;
        currentPriority = 0;
        interruptible = true;
    }
};

} // namespace engine
