#version 450

// SDL3 GPU SPIR-V 布局：fragment samplers set=2, fragment UBOs set=3
layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;

layout(set = 2, binding = 0) uniform sampler2D tex;

layout(set = 3, binding = 0) uniform MSDFParams {
    float pxRange;
} params;

layout(location = 0) out vec4 outColor;

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec3 sampleVal = texture(tex, inUV).rgb;
    float sigDist = median(sampleVal.r, sampleVal.g, sampleVal.b) - 0.5;
    float opacity = clamp(sigDist * params.pxRange + 0.5, 0.0, 1.0);
    outColor = vec4(inColor.rgb, inColor.a * opacity);
}
