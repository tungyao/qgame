#pragma once

#include "../../backend/shared/ResourceHandle.h"
#include "../../core/math/Color.h"
#include "../../core/math/Rect.h"
#include "RenderComponents.h"
#include <cstdint>

namespace engine {

// GPU 粒子发射器组件。
//
// CPU 职责（每帧）：
//   - syncEmitterPosition: 仅上传位置 / 旋转（2 floats × 3 = 12 bytes）。
//   - 配置参数（寿命、速度、颜色等）仅在组件创建或修改时上传一次。
//
// GPU 职责（particle_emit.comp.hlsl）：
//   - 从 emitter buffer 读取参数，自主发射新粒子。
//   - 使用 lock-free free list 回收死亡粒子的槽位。
//   - 推进生命周期、速度、位置。
//   - 把活粒子压缩到 AliveIndices 并写入间接绘制参数。
//
// OpenGL 后端：不支持 compute，粒子不渲染。

enum class ParticleSortMode : uint32_t {
    None      = 0,   // 不排序（火/烟/叠加混合）
    Y         = 1,   // 按 Y 轴深度排序
    Depth     = 2,   // 按相机距离排序（预留）
    CustomKey = 3,   // 按 sortKey 排序
};

inline uint32_t sortModeBits(ParticleSortMode m) { return static_cast<uint32_t>(m) << 8; }

struct ParticleComponent {
    TextureHandle texture;
    core::Rect    srcRect;

    uint32_t      maxParticles = 256;
    float         emissionRate = 64.f;     // particles / second
    float         lifetime     = 1.f;
    float         speedMin     = 20.f;
    float         speedMax     = 80.f;
    float         sizeStart    = 8.f;
    float         sizeEnd      = 0.f;
    float         spread       = 6.28318530718f; // radians

    core::Color   colorStart = core::Color::White;
    core::Color   colorEnd   = core::Color{255, 255, 255, 0};

    int           layer     = 0;
    int           sortOrder = 0;
    ParticleSortMode sortMode = ParticleSortMode::None;
    bool          visible   = true;
    bool          playing   = true;
    RenderPass    pass      = RenderPass::World;

    // ── GPU 侧管理字段（场景序列化跳过）─────────────────────────────────────
    uint32_t      gpuOffset       = 0xFFFFFFFFu;  // particle pool base
    uint32_t      gpuCount        = 0;            // allocated slot count
    uint32_t      gpuEmitterIndex = 0xFFFFFFFFu;  // emitter buffer slot
    uint32_t      seed            = 1u;           // LCG 初始种子（上传一次）
    bool          configDirty     = true;          // 配置变化标志
};

} // namespace engine
