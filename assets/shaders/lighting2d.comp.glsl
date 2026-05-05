#version 450

// Reference GLSL source for the L2 2D hard-shadow prototype.
// The build currently compiles the HLSL twin (lighting2d.comp.hlsl) through DXC
// so SDL_GPU can share the same shader toolchain as sprite/particle compute.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct Light2DPoint {
    vec2 position;
    float radius;
    float intensity;
    vec4 color;
};

struct Light2DSegment {
    vec2 a;
    vec2 b;
    float opacity;
    float pad0;
    float pad1;
    float pad2;
};

layout(set = 0, binding = 0) readonly buffer LightBuffer {
    Light2DPoint lights[];
};

layout(set = 0, binding = 1) readonly buffer SegmentBuffer {
    Light2DSegment segments[];
};

layout(set = 1, binding = 0, rgba8) uniform writeonly image2D shadowOverlay;

layout(push_constant) uniform LightingParams {
    vec2 cameraPos;
    vec2 viewportSize;
    vec2 lightingSize;
    uint lightCount;
    uint segmentCount;
    float zoom;
    float shadowStrength;
} params;

float cross2(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

bool rayIntersectsSegment(vec2 p, vec2 q, vec2 a, vec2 b) {
    vec2 r = q - p;
    vec2 s = b - a;
    float denom = cross2(r, s);
    if (abs(denom) < 0.00001) return false;
    float t = cross2(a - p, s) / denom;
    float u = cross2(a - p, r) / denom;
    return t > 0.001 && t < 0.999 && u >= 0.0 && u <= 1.0;
}

void main() {
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    if (pix.x >= int(params.lightingSize.x) || pix.y >= int(params.lightingSize.y)) {
        return;
    }

    vec2 screen = (vec2(pix) + vec2(0.5)) * (params.viewportSize / params.lightingSize);
    vec2 world = (screen - params.viewportSize * 0.5) / max(params.zoom, 0.0001) + params.cameraPos;

    float alpha = 0.0;
    if (params.lightCount > 0) {
        Light2DPoint light = lights[0];
        float dist = distance(world, light.position);
        if (dist < light.radius) {
            float blocked = 0.0;
            uint count = min(params.segmentCount, 64u);
            for (uint i = 0; i < count; ++i) {
                Light2DSegment seg = segments[i];
                if (rayIntersectsSegment(world, light.position, seg.a, seg.b)) {
                    blocked = max(blocked, clamp(seg.opacity, 0.0, 1.0));
                }
            }
            float attenuation = 1.0 - dist / light.radius;
            alpha = blocked * attenuation * params.shadowStrength;
        }
    }

    imageStore(shadowOverlay, pix, vec4(0.0, 0.0, 0.0, clamp(alpha, 0.0, 0.85)));
}
