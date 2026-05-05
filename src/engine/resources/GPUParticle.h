#pragma once

#include <cstdint>

namespace engine {

// CPU 与 HLSL 共享的粒子实例布局。
//
// 注意：
// - 每个 float4 / uint4 对齐到 16 字节，匹配 StructuredBuffer 的常见布局。
// - age < 0 表示空槽；age >= lifetime 表示死亡，compute 会把它折回空槽。
// - textureIndex 目前只是保留字段，第一版按 emitter 绑定单张纹理绘制。
struct alignas(16) GPUParticle {
    float posLife[4];   // x, y, age, lifetime
    float velSize[4];   // vx, vy, sizeStart, sizeEnd
    float color0[4];    // start rgba, normalized
    float color1[4];    // end rgba, normalized
    float uv[4];        // u0, v0, u1, v1
    uint32_t textureIndex;
    uint32_t layer;
    int32_t  sortKey;
    uint32_t flags;     // bit 0..2 = RenderPass, bit 8 = ySort
};

static_assert(sizeof(GPUParticle) == 96, "GPUParticle must stay shader-compatible");
static_assert(sizeof(GPUParticle) % 16 == 0, "GPUParticle must be 16-byte aligned");

inline void packParticleColor(float* out, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

} // namespace engine
