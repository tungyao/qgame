#pragma once

#include "../../backend/shared/ResourceHandle.h"
#include "../../core/math/Color.h"
#include "../../core/math/Rect.h"
#include "RenderComponents.h"
#include <cstdint>

namespace engine {

// GPU 粒子发射器组件。
//
// 第一版故意保持窄接口：
// - CPU 只根据 emissionRate 生成新粒子的初始状态。
// - GPU compute 每帧推进生命周期、速度和位置。
// - SDL_GPU 后端直接从粒子 storage buffer 绘制 instanced quad。
// - OpenGL 后端不做优化，也不会渲染 GPU 粒子。
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
    float         spread       = 6.28318530718f; // radians, centered on emitter.rotation

    core::Color   colorStart = core::Color::White;
    core::Color   colorEnd   = core::Color{255, 255, 255, 0};

    int           layer     = 0;
    int           sortOrder = 0;
    bool          ySort     = false;
    bool          visible   = true;
    bool          playing   = true;
    RenderPass    pass      = RenderPass::World;

    // RenderSystem / GPUParticleRenderer 内部状态。组件保留这些字段，
    // 避免额外 side table；场景序列化以后可以选择跳过它们。
    uint32_t      gpuOffset   = 0xFFFFFFFFu;
    uint32_t      gpuCount    = 0;
    float         accumulator = 0.f;
    uint32_t      seed        = 1u;
};

} // namespace engine
