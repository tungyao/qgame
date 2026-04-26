#pragma once
#include <cstdint>
#include <cmath>

namespace engine {

struct GPUHandle {
    uint32_t index = 0xFFFFFFFF;
    uint32_t generation = 0;

    bool valid() const { return index != 0xFFFFFFFF; }
    static GPUHandle invalid() { return { 0xFFFFFFFF, 0 }; }
};

struct alignas(16) GPUSprite {
    float transform[12];
    float color[4];
    float uv[4];
    uint32_t textureIndex;
    uint32_t layer;
    int32_t sortKey;
    uint32_t flags;
};

static_assert(sizeof(GPUSprite) % 16 == 0, "GPUSprite must be 16-byte aligned");
static_assert(sizeof(GPUSprite) == 96, "GPUSprite size check");

inline void buildTransform2D(float* out,
                              float x, float y,
                              float rotation,
                              float scaleX, float scaleY,
                              float pivotX, float pivotY,
                              float width, float height) {
    const float c = cosf(rotation);
    const float s = sinf(rotation);

    const float sx = scaleX * width;
    const float sy = scaleY * height;

    out[0] = c * sx;   out[1] = -s * sy;  out[2] = 0.f;  out[3] = x - (pivotX - 0.5f) * c * sx + (pivotY - 0.5f) * s * sy;
    out[4] = s * sx;   out[5] =  c * sy;  out[6] = 0.f;  out[7] = y - (pivotX - 0.5f) * s * sx - (pivotY - 0.5f) * c * sy;
    out[8] = 0.f;      out[9] =  0.f;     out[10] = 1.f; out[11] = 0.f;
}

inline void packColor(float* out, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

inline void packUV(float* out, float u0, float v0, float u1, float v1) {
    out[0] = u0;
    out[1] = v0;
    out[2] = u1;
    out[3] = v1;
}

}
