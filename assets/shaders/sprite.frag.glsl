#version 450

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;

// SDL3 GPU: fragment sampler 从 set=2 binding=0 开始
layout(set = 2, binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(tex, inUV) * inColor;
}
