#version 450

struct GPUSprite {
    float transform[12];
    float color[4];
    float uv[4];
    uint textureIndex;
    uint layer;
    int sortKey;
    uint flags;
};

layout(set = 0, binding = 0, std430) readonly buffer SpriteBuffer {
    GPUSprite sprites[];
};

layout(set = 0, binding = 1, std430) readonly buffer IndexBuffer {
    uint visibleIndices[];
};

layout(set = 1, binding = 0) uniform ViewUBO {
    mat4 viewProj;
} ubo;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;
layout(location = 2) flat out uint outTexIndex;

const vec2 quadVerts[4] = vec2[](
    vec2(-0.5, -0.5),
    vec2( 0.5, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5,  0.5)
);

void main() {
    uint spriteIdx = visibleIndices[gl_InstanceIndex];
    GPUSprite s = sprites[spriteIdx];
    
    int vertIdx = gl_VertexIndex & 3;
    vec2 qv = quadVerts[vertIdx];
    
    float tx = s.transform[3];
    float ty = s.transform[7];
    
    float m00 = s.transform[0];
    float m01 = s.transform[1];
    float m10 = s.transform[4];
    float m11 = s.transform[5];
    
    vec2 worldPos = vec2(
        tx + m00 * qv.x + m01 * qv.y,
        ty + m10 * qv.x + m11 * qv.y
    );
    
    gl_Position = ubo.viewProj * vec4(worldPos, 0.0, 1.0);
    
    float u0 = s.uv[0];
    float v0 = s.uv[1];
    float u1 = s.uv[2];
    float v1 = s.uv[3];
    
    if (vertIdx == 0) outUV = vec2(u0, v0);
    else if (vertIdx == 1) outUV = vec2(u1, v0);
    else if (vertIdx == 2) outUV = vec2(u1, v1);
    else outUV = vec2(u0, v1);
    
    outColor = vec4(s.color[0], s.color[1], s.color[2], s.color[3]);
    outTexIndex = s.textureIndex;
}
