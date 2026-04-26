#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "../../backend/shared/ResourceHandle.h"
#include "../../core/math/Rect.h"

namespace engine {

struct AnimationFrame {
    core::Rect srcRect;     // 在 spritesheet 内的像素区域
    float      duration;    // 本帧持续时长 (秒)
};

// Phase 2: 帧事件 (Animation Notify)
struct AnimEvent {
    float       time = 0.f;     // 在 clip 内的时间 (秒)
    std::string name;           // 约定: hitbox_on / hitbox_off / footstep / sfx / vfx ...
    int         intParam    = 0;
    float       floatParam  = 0.f;
    std::string stringParam;
};

struct AnimationClip {
    std::string                 name;        // tag 名 (或文件名)
    TextureHandle               texture;     // 关联的 spritesheet
    std::vector<AnimationFrame> frames;
    float                       duration = 0.f; // 所有帧累计时长
    bool                        loop = true;
    std::vector<AnimEvent>      events;      // Phase 2: 帧事件 (按 time 升序)
};

// Phase 2: 当帧派发的事件实例 (per-entity 队列)
struct AnimEventInstance {
    std::string     name;
    int             intParam;
    float           floatParam;
    std::string     stringParam;
    AnimationHandle clip;       // 派发时所属 clip
    float           time;       // 事件在 clip 内的时间
};

struct AnimEventQueue {
    // 每帧由 AnimatorSystem 追加，消费者 system 处理后须 clear()。
    // AnimatorSystem 在每次 update 开始时清空 (作为单帧事件队列)。
    std::vector<AnimEventInstance> events;
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

// ── Phase 3: 参数 / 状态机 (FSM) ──────────────────────────────────────────
enum class ParamType : uint8_t { Float, Int, Bool, Trigger };

struct AnimParam {
    ParamType type = ParamType::Float;
    float     f    = 0.f;       // float 直接存; int 存为 float; bool / trigger: 0/非0
};

enum class ConditionOp : uint8_t {
    Greater, GreaterEq, Less, LessEq, Equal, NotEqual,
    IsTrue, IsFalse,
    Trigger,    // 仅参数为 Trigger 类型: 已 set
};

struct TransitionCondition {
    std::string parameter;
    ConditionOp op = ConditionOp::Greater;
    float       threshold = 0.f;    // 数值参数比较的阈值
};

constexpr int kAnyState = -1;

struct AnimState {
    std::string     name;
    AnimationHandle clip;
    float           speed = 1.f;
    PlayMode        mode  = PlayMode::ClipDefault;  // 状态级播放模式
};

struct AnimTransition {
    int  from = kAnyState;
    int  to   = 0;
    std::vector<TransitionCondition> conditions;
    bool  hasExitTime  = false;
    float exitTime     = 1.f;       // 归一化 [0,1]，相对 from 状态 clip duration
    float duration     = 0.f;       // crossfade (秒)
    bool  interruptible = true;     // crossfade 期间是否允许被新 transition 打断
};

// ── Phase 4: 动画分层 (Layers) ────────────────────────────────────────────
// 通道掩码：标记某层会写回哪些目标通道
namespace LayerChannel {
    enum : uint32_t {
        SrcRect  = 1u << 0,
        Texture  = 1u << 1,
        Tint     = 1u << 2,
        Offset   = 1u << 3,   // Transform 平移偏移 (procedural)
        Rotation = 1u << 4,   // Transform 旋转偏移 (procedural)
        Scale    = 1u << 5,   // Transform 缩放偏移 (procedural)
    };
}

enum class LayerBlendMode : uint8_t { Override, Additive };

struct AnimatorLayer {
    std::string                       name;
    float                             weight    = 1.f;       // 0..1
    LayerBlendMode                    blendMode = LayerBlendMode::Override;
    uint32_t                          mask      = LayerChannel::SrcRect | LayerChannel::Texture;
    std::vector<AnimState>            states;
    std::vector<AnimTransition>       transitions;
    int                               defaultState = 0;
};

struct AnimatorController {
    // Base 层：保留扁平字段 (向后兼容)；mask 默认 SrcRect|Texture，blendMode=Override，weight=1
    std::vector<AnimState>      states;
    std::vector<AnimTransition> transitions;
    int                         defaultState = 0;

    // 额外层 (索引 0 → AnimatorComponent.extraLayers[0])
    std::vector<AnimatorLayer>  layers;
};

// 单个额外层的运行时状态 (基础层运行时复用 AnimatorComponent 既有字段)
struct AnimatorLayerRuntime {
    AnimationHandle currentAnim;
    float           time     = 0.f;
    float           speed    = 1.f;
    bool            playing  = false;
    bool            finished = false;
    PlayMode        currentMode = PlayMode::ClipDefault;

    int             currentState = -1;
    int             fromState    = -1;
    float           transitionT  = 0.f;
    float           transitionDuration = 0.f;
    bool            stateFinishedFired = false;
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

    // ── Phase 3: 参数 / 状态机 ────────────────────────────────────────────
    std::shared_ptr<AnimatorController> controller;
    std::unordered_map<std::string, AnimParam> parameters;

    int   currentState = -1;
    int   fromState    = -1;        // crossfade 中的源状态; -1 表示无 transition
    float transitionT  = 0.f;       // 已经过的 transition 时间
    float transitionDuration = 0.f; // 总 transition 时长

    // 上一帧此状态是否已经触发过 state_finished (避免每帧重复发)
    bool  stateFinishedFired = false;

    // ── Phase 4: 额外层运行时 (索引对齐 controller->layers) ───────────────
    std::vector<AnimatorLayerRuntime> extraLayers;

    // 参数 API
    void setFloat  (const std::string& n, float v) { auto& p = parameters[n]; p.type = ParamType::Float;   p.f = v; }
    void setInt    (const std::string& n, int v)   { auto& p = parameters[n]; p.type = ParamType::Int;     p.f = static_cast<float>(v); }
    void setBool   (const std::string& n, bool v)  { auto& p = parameters[n]; p.type = ParamType::Bool;    p.f = v ? 1.f : 0.f; }
    void setTrigger(const std::string& n)          { auto& p = parameters[n]; p.type = ParamType::Trigger; p.f = 1.f; }
    void resetTrigger(const std::string& n) {
        auto it = parameters.find(n);
        if (it != parameters.end() && it->second.type == ParamType::Trigger) it->second.f = 0.f;
    }
    float getFloat(const std::string& n) const {
        auto it = parameters.find(n);
        return it != parameters.end() ? it->second.f : 0.f;
    }

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
