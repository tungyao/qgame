#version 450

// Reference GLSL source for L3 2D tiled light culling.
// The build compiles the HLSL twin so SDL_GPU can share the same DXC path as
// the rest of the Vulkan-first compute shaders.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

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

layout(set = 0, binding = 0) readonly buffer LightBuffer {
    Light2DPoint lights[];
};

layout(set = 1, binding = 0) writeonly buffer TileRangeBuffer {
    uvec2 tileRanges[];
};

layout(set = 1, binding = 1) writeonly buffer TileLightIndexBuffer {
    uint tileLightIndices[];
};

layout(push_constant) uniform LightingCullParams {
    vec2 cameraPos;
    vec2 viewportSize;
    uint lightCount;
    uint tileCols;
    uint tileRows;
    uint tileSize;
    float zoom;
    uint maxLightsPerTile;
} params;

bool lightTouchesTile(vec2 lightScreen, float radiusScreen, vec2 tileMin, vec2 tileMax) {
    vec2 closest = clamp(lightScreen, tileMin, tileMax);
    vec2 delta = lightScreen - closest;
    return dot(delta, delta) <= radiusScreen * radiusScreen;
}

void main() {
    uint tileId = gl_GlobalInvocationID.x;
    uint tileCount = params.tileCols * params.tileRows;
    if (tileId >= tileCount) {
        return;
    }

    uint tileX = tileId % params.tileCols;
    uint tileY = tileId / params.tileCols;
    vec2 tileMin = vec2(tileX * params.tileSize, tileY * params.tileSize);
    vec2 tileMax = min(tileMin + vec2(params.tileSize), params.viewportSize);

    uint writeBase = tileId * params.maxLightsPerTile;
    uint writeCount = 0;
    for (uint i = 0; i < params.lightCount; ++i) {
        Light2DPoint light = lights[i];
        if ((light.layerMask & 1u) == 0u || light.radius <= 0.0 || light.intensity <= 0.0) {
            continue;
        }

        float safeZoom = max(params.zoom, 0.0001);
        vec2 lightScreen = (light.position - params.cameraPos) * safeZoom + params.viewportSize * 0.5;
        float radiusScreen = light.radius * safeZoom;
        if (lightTouchesTile(lightScreen, radiusScreen, tileMin, tileMax)) {
            if (writeCount < params.maxLightsPerTile) {
                tileLightIndices[writeBase + writeCount] = i;
                writeCount++;
            }
        }
    }

    tileRanges[tileId] = uvec2(writeBase, writeCount);
}
