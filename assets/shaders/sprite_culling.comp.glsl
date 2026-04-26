#version 450
layout(local_size_x = 64) in;

struct GPUSprite {
    float transform[12];
    float color[4];
    float uv[4];
    uint textureIndex;
    uint layer;
    int sortKey;
    uint flags;
};

// SDL_GPU compute binding convention:
//   set=0 readonly storage (textures then buffers)
//   set=1 readwrite storage (textures then buffers)
//   set=2 uniform buffers
layout(set = 0, binding = 0, std430) readonly buffer SpriteBuffer {
    GPUSprite sprites[];
};

layout(set = 1, binding = 0, std430) buffer VisibleBuffer {
    uint visibleIndices[];
};

layout(set = 1, binding = 1, std430) buffer CounterBuffer {
    uint atomicCounter;
};

layout(set = 2, binding = 0) uniform CameraParams {
    float camX, camY;
    float zoom;
    float rotation;
    float viewMinX, viewMinY;
    float viewMaxX, viewMaxY;
    uint spriteCount;
    uint cullEnabled;
    uint layerMask;
    uint maxVisible;   // VisibleBuffer 容量上限
};

shared uint localCount;
shared uint localIndices[64];

void main() {
    uint idx = gl_GlobalInvocationID.x;

    if (gl_LocalInvocationIndex == 0) {
        localCount = 0;
    }
    barrier();

    bool visible = false;

    if (idx < spriteCount) {
        GPUSprite s = sprites[idx];

        uint passBits = (s.flags >> 1) & 0x7u;
        if ((layerMask & (1u << passBits)) != 0u) {
            if (cullEnabled == 0u) {
                visible = true;
            } else {
                float tx = s.transform[3];
                float ty = s.transform[7];

                float sx = abs(s.transform[0]) + abs(s.transform[1]);
                float sy = abs(s.transform[4]) + abs(s.transform[5]);
                float halfW = sx * 0.5;
                float halfH = sy * 0.5;

                if (tx + halfW >= viewMinX && tx - halfW <= viewMaxX &&
                    ty + halfH >= viewMinY && ty - halfH <= viewMaxY) {
                    visible = true;
                }
            }
        }
    }

    if (visible) {
        uint lidx = atomicAdd(localCount, 1u);
        if (lidx < 64u) {
            localIndices[lidx] = idx;
        }
    }
    barrier();

    if (gl_LocalInvocationIndex == 0 && localCount > 0u) {
        uint writeCount = min(localCount, 64u);   // 截断保护
        uint globalOffset = atomicAdd(atomicCounter, writeCount);

        for (uint i = 0u; i < writeCount; ++i) {
            uint dst = globalOffset + i + 1u;     // [0] 预留给总数
            if (dst < maxVisible) {               // 越界保护
                visibleIndices[dst] = localIndices[i];
            }
        }
    }
}
