#pragma once
static const char sprite_culling_glsl[] = R"GLSL(
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

layout(std430, binding = 0) readonly buffer SpriteBuffer {
    GPUSprite sprites[];
};

layout(std430, binding = 1) buffer VisibleBuffer {
    uint visibleCount;
    uint visibleIndices[];
};

layout(std140, binding = 2) uniform CameraParams {
    float camX, camY;
    float zoom;
    float rotation;
    float viewMinX, viewMinY;
    float viewMaxX, viewMaxY;
    uint spriteCount;
    uint cullEnabled;
    uint layerMask;
};

layout(std430, binding = 3) buffer CounterBuffer {
    uint atomicCounter;
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
        uint localIdx = atomicAdd(localCount, 1);
        if (localIdx < 64) {
            localIndices[localIdx] = idx;
        }
    }
    barrier();
    
    if (gl_LocalInvocationIndex == 0 && localCount > 0) {
        uint globalOffset = atomicAdd(atomicCounter, localCount);
        for (uint i = 0; i < localCount && globalOffset + i < spriteCount; ++i) {
            visibleIndices[globalOffset + i] = localIndices[i];
        }
    }
}
)GLSL";
static const size_t sprite_culling_glsl_size = sizeof(sprite_culling_glsl);
