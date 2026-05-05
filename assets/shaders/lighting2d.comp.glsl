#version 450

// Reference GLSL source for the L3 2D hard-shadow prototype.
// The build currently compiles the HLSL twin (lighting2d.comp.hlsl) through DXC
// so SDL_GPU can share the same shader toolchain as sprite/particle compute.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct Light2DPoint {
    vec2 position;
    float radius;
    float intensity;
    vec4 color;
    uint layerMask;
    uint castsShadow;
    uint pad0;
    uint pad1;
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

layout(set = 0, binding = 2) readonly buffer TileRangeBuffer {
    uvec2 tileRanges[];
};

layout(set = 0, binding = 3) readonly buffer TileLightIndexBuffer {
    uint tileLightIndices[];
};

layout(set = 1, binding = 0, rgba8) uniform writeonly image2D shadowOverlay;

layout(push_constant) uniform LightingParams {
    vec2 cameraPos;
    vec2 viewportSize;
    vec2 lightingSize;
    uint lightCount;
    uint segmentCount;
    uint tileCols;
    uint tileRows;
    float zoom;
    float shadowStrength;
    uint tileSize;
    uint maxLightsPerTile;
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

    vec3 lightAccum = vec3(0.0);
    float glowAlpha = 0.0;
    float shadowAlpha = 0.0;
    uvec2 tileCoord = min(uvec2(floor(screen / max(float(params.tileSize), 1.0))),
                          uvec2(max(params.tileCols, 1u) - 1u,
                                max(params.tileRows, 1u) - 1u));
    uint tileId = tileCoord.y * params.tileCols + tileCoord.x;
    uvec2 range = tileRanges[tileId];
    uint count = min(range.y, params.maxLightsPerTile);

    for (uint listIndex = 0; listIndex < count; ++listIndex) {
        uint lightIndex = tileLightIndices[range.x + listIndex];
        if (lightIndex >= params.lightCount) {
            continue;
        }

        Light2DPoint light = lights[lightIndex];
        if ((light.layerMask & 1u) == 0u || light.radius <= 0.0 || light.intensity <= 0.0) {
            continue;
        }

        float dist = distance(world, light.position);
        if (dist < light.radius) {
            float blocked = 0.0;
            for (uint i = 0; i < params.segmentCount; ++i) {
                Light2DSegment seg = segments[i];
                if (light.castsShadow != 0u &&
                    rayIntersectsSegment(world, light.position, seg.a, seg.b)) {
                    blocked = max(blocked, clamp(seg.opacity, 0.0, 1.0));
                }
            }
            float attenuation = clamp(1.0 - dist / light.radius, 0.0, 1.0);
            float lightWeight = attenuation * attenuation * max(light.intensity, 0.0);
            float visibility = 1.0 - clamp(blocked, 0.0, 1.0);
            float visibleWeight = lightWeight * visibility;

            lightAccum += light.color.rgb * visibleWeight;
            glowAlpha = max(glowAlpha, visibleWeight * light.color.a * 0.36);
            shadowAlpha = max(shadowAlpha, blocked * attenuation * params.shadowStrength);
        }
    }

    float luminance = dot(lightAccum, vec3(0.2126, 0.7152, 0.0722));
    float finalGlowAlpha = clamp(min(max(glowAlpha, luminance * 0.24), 0.62), 0.0, 1.0);
    float finalShadowAlpha = clamp(min(shadowAlpha, 0.82), 0.0, 1.0);

    if (finalGlowAlpha > finalShadowAlpha * 0.65) {
        vec3 glowColor = clamp(lightAccum / max(luminance, 1.0), vec3(0.0), vec3(1.0));
        imageStore(shadowOverlay, pix, vec4(glowColor, finalGlowAlpha));
    } else {
        imageStore(shadowOverlay, pix, vec4(0.0, 0.0, 0.0, finalShadowAlpha));
    }
}
