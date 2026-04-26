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
//   set=0 readonly storage   set=1 readwrite storage   set=2 uniforms
layout(set = 0, binding = 0, std430) readonly buffer SpriteBuffer {
    GPUSprite sprites[];
};

layout(set = 0, binding = 1, std430) readonly buffer IndexBuffer {
    uint visibleIndices[];
};

layout(set = 1, binding = 0, std430) buffer DrawArgsBuffer {
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
};

layout(set = 1, binding = 1, std430) buffer CounterBuffer {
    uint visibleCount;
};

layout(set = 2, binding = 0) uniform CullParams {
    uint spriteCount;
    uint cullEnabled;
    uint pad1;
    uint pad2;
};

void main() {
    uint idx = gl_GlobalInvocationID.x;
    
    if (idx == 0) {
        instanceCount = visibleCount;
    }
}
