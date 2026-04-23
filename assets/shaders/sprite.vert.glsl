#version 450

layout(location = 0) in vec2  inPos;
layout(location = 1) in vec2  inUV;
layout(location = 2) in vec4  inColor;  // normalized ubyte4

layout(set = 1, binding = 0) uniform UBO {
    mat4 proj;
} ubo;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

void main() {
    gl_Position = ubo.proj * vec4(inPos, 0.0, 1.0);
    outUV    = inUV;
    outColor = inColor;
}
