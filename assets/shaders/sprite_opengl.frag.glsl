#version 330 core

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;

uniform sampler2D uTexture;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTexture, inUV) * inColor;
}