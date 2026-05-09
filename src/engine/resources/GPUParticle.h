#pragma once

#include <cstdint>

namespace engine {

// ═════════════════════════════════════════════════════════════════════════════
// GPUParticle — 单个粒子实例，CPU 与 HLSL compute/vertex shader 共享布局。
//
// 96 bytes, 16-byte aligned。包含位置、速度、颜色、UV 和排序元数据。
// age < 0  → 空槽（GPU compute 在 compaction 时跳过）
// age >= lifetime → 死亡（下一帧被 free list 回收）
// ═════════════════════════════════════════════════════════════════════════════
struct alignas(16) GPUParticle {
    float     posLife[4];      // x, y, age, lifetime
    float     velSize[4];      // vx, vy, sizeStart, sizeEnd
    float     color0[4];       // start colour, normalized
    float     color1[4];       // end colour, normalized
    float     uv[4];           // u0, v0, u1, v1
    uint32_t  textureIndex;
    uint32_t  layer;
    int32_t   sortKey;         // user priority (ParticleComponent::sortOrder)
    uint32_t  flags;           // bit 0..2 = RenderPass, bit 8-9 = ParticleSortMode
};

static_assert(sizeof(GPUParticle) == 96, "GPUParticle must be 96 bytes");
static_assert(sizeof(GPUParticle) % 16 == 0, "GPUParticle must be 16-byte aligned");

// ═════════════════════════════════════════════════════════════════════════════
// GPUEmitter — 发射器描述符，存储在 GPU structured buffer 中。
//
// CPU 职责（每帧或变化时上传）：
//   pos_rot[0..2]  = 世界坐标 + 旋转（从 Transform 同步）
//   config0[0]     = particleCount（从 ParticleComponent 同步）
//   其余字段       = 发射器配置（仅在组件变化时上传）
//
// GPU 职责（compute shader 读写）：
//   state[0]  = accumulator   — 分数粒子累积
//   state[1]  = seed          — LCG 随机种子
//   state[2]  = writeCursor   — ring buffer 写入游标
//
// 布局策略：全部用 float4 槽位，避免 HLSL/C++ 跨编译器对齐差异。
// ═════════════════════════════════════════════════════════════════════════════
struct alignas(16) GPUEmitter {
    float pos_rot[4];    // [0]=x, [1]=y, [2]=rotation, [3]=as_uint(firstParticle)
    float config0[4];    // [0]=as_uint(particleCount), [1]=emissionRate, [2]=lifetime, [3]=speedMin
    float config1[4];    // [0]=speedMax, [1]=sizeStart, [2]=sizeEnd, [3]=spread
    float colorStart[4]; // rgba start
    float colorEnd[4];   // rgba end
    float uvRect[4];     // u0, v0, u1, v1
    float misc[4];       // [0]=as_uint(textureIndex), [1]=as_uint(layer), [2]=as_int(sortKey), [3]=as_uint(flags)
    float state[4];      // [0]=accumulator, [1]=as_uint(seed), [2]=as_uint(writeCursor), [3]=pad
};

static_assert(sizeof(GPUEmitter) == 128, "GPUEmitter must be 128 bytes");
static_assert(sizeof(GPUEmitter) % 16 == 0, "GPUEmitter must be 16-byte aligned");

// ── 辅助读写器 — 避免到处 reinterpret_cast ────────────────────────────────────
inline uint32_t as_uint(float v) { uint32_t u; __builtin_memcpy(&u, &v, 4); return u; }
inline int32_t  as_int(float v)  { int32_t i;  __builtin_memcpy(&i, &v, 4); return i;  }
inline float    as_float(uint32_t u) { float f; __builtin_memcpy(&f, &u, 4); return f; }
inline float    as_float(int32_t i)  { float f; __builtin_memcpy(&f, &i, 4); return f;  }

inline float    em_posX(const GPUEmitter& e)       { return e.pos_rot[0]; }
inline float    em_posY(const GPUEmitter& e)       { return e.pos_rot[1]; }
inline float    em_rotation(const GPUEmitter& e)   { return e.pos_rot[2]; }
inline uint32_t em_firstParticle(const GPUEmitter& e) { return as_uint(e.pos_rot[3]); }
inline uint32_t em_particleCount(const GPUEmitter& e) { return as_uint(e.config0[0]); }
inline float    em_emissionRate(const GPUEmitter& e)  { return e.config0[1]; }
inline float    em_lifetime(const GPUEmitter& e)      { return e.config0[2]; }
inline float    em_speedMin(const GPUEmitter& e)      { return e.config0[3]; }
inline float    em_speedMax(const GPUEmitter& e)      { return e.config1[0]; }
inline float    em_sizeStart(const GPUEmitter& e)     { return e.config1[1]; }
inline float    em_sizeEnd(const GPUEmitter& e)       { return e.config1[2]; }
inline float    em_spread(const GPUEmitter& e)        { return e.config1[3]; }
inline uint32_t em_textureIndex(const GPUEmitter& e)  { return as_uint(e.misc[0]); }
inline uint32_t em_layer(const GPUEmitter& e)         { return as_uint(e.misc[1]); }
inline int32_t  em_sortKey(const GPUEmitter& e)       { return as_int(e.misc[2]); }
inline uint32_t em_flags(const GPUEmitter& e)         { return as_uint(e.misc[3]); }
inline float    em_accumulator(const GPUEmitter& e)   { return e.state[0]; }
inline uint32_t em_seed(const GPUEmitter& e)          { return as_uint(e.state[1]); }
inline uint32_t em_writeCursor(const GPUEmitter& e)   { return as_uint(e.state[2]); }

inline void em_setPosX(GPUEmitter& e, float v)          { e.pos_rot[0] = v; }
inline void em_setPosY(GPUEmitter& e, float v)          { e.pos_rot[1] = v; }
inline void em_setRotation(GPUEmitter& e, float v)       { e.pos_rot[2] = v; }
inline void em_setFirstParticle(GPUEmitter& e, uint32_t v) { e.pos_rot[3] = as_float(v); }
inline void em_setParticleCount(GPUEmitter& e, uint32_t v) { e.config0[0] = as_float(v); }
inline void em_setEmissionRate(GPUEmitter& e, float v)    { e.config0[1] = v; }
inline void em_setLifetime(GPUEmitter& e, float v)        { e.config0[2] = v; }
inline void em_setSpeedMin(GPUEmitter& e, float v)        { e.config0[3] = v; }
inline void em_setSpeedMax(GPUEmitter& e, float v)        { e.config1[0] = v; }
inline void em_setSizeStart(GPUEmitter& e, float v)       { e.config1[1] = v; }
inline void em_setSizeEnd(GPUEmitter& e, float v)         { e.config1[2] = v; }
inline void em_setSpread(GPUEmitter& e, float v)          { e.config1[3] = v; }
inline void em_setTextureIndex(GPUEmitter& e, uint32_t v) { e.misc[0] = as_float(v); }
inline void em_setLayer(GPUEmitter& e, uint32_t v)        { e.misc[1] = as_float(v); }
inline void em_setSortKey(GPUEmitter& e, int32_t v)       { e.misc[2] = as_float(v); }
inline void em_setFlags(GPUEmitter& e, uint32_t v)        { e.misc[3] = as_float(v); }
inline void em_setAccumulator(GPUEmitter& e, float v)      { e.state[0] = v; }
inline void em_setSeed(GPUEmitter& e, uint32_t v)          { e.state[1] = as_float(v); }
inline void em_setWriteCursor(GPUEmitter& e, uint32_t v)   { e.state[2] = as_float(v); }

inline void packParticleColor(float* out, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

} // namespace engine
