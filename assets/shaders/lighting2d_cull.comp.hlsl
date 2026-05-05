// L3 2D tiled light culling.
//
// One compute invocation owns one screen-space tile. It scans the uploaded
// Light2DPoint array, tests each light's screen-space bounding circle against
// the tile AABB, and writes a compact fixed-capacity list:
//
//   TileRanges[tile] = uint2(startIndex, lightCountInTile)
//   TileLightIndices[startIndex + n] = light index
//
// This avoids global atomics and prefix sums in the first L3 milestone. The
// fixed list is predictable in captures and easy to validate in demo3 with 32
// moving lights. If too many lights overlap one tile, the list clamps; that is
// a controlled debug-time degradation rather than a memory hazard.

struct Light2DPoint {
    float2 position;
    float  radius;
    float  intensity;
    float4 color;
    uint   layerMask;
    uint   castsShadow;
    uint   pad0;
    uint   pad1;
};

StructuredBuffer<Light2DPoint> Lights : register(t0, space0);
RWStructuredBuffer<uint2>      TileRanges : register(u0, space1);
RWStructuredBuffer<uint>       TileLightIndices : register(u1, space1);

cbuffer LightingCullParams : register(b0, space2)
{
    float2 cameraPos;
    float2 viewportSize;
    uint   lightCount;
    uint   tileCols;
    uint   tileRows;
    uint   tileSize;
    float  zoom;
    uint   maxLightsPerTile;
    uint2  pad0;
};

bool lightTouchesTile(float2 lightScreen, float radiusScreen, float2 tileMin, float2 tileMax)
{
    // Closest-point circle-vs-AABB test. It is cheap, branch-light, and avoids
    // admitting lights that merely touch the broad viewport but not this tile.
    float2 closest = clamp(lightScreen, tileMin, tileMax);
    float2 delta = lightScreen - closest;
    return dot(delta, delta) <= radiusScreen * radiusScreen;
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    uint tileId = gid.x;
    uint tileCount = tileCols * tileRows;
    if (tileId >= tileCount) {
        return;
    }

    uint tileX = tileId % tileCols;
    uint tileY = tileId / tileCols;
    float2 tileMin = float2(tileX * tileSize, tileY * tileSize);
    float2 tileMax = min(tileMin + float2(tileSize, tileSize), viewportSize);

    uint writeBase = tileId * maxLightsPerTile;
    uint writeCount = 0;

    [loop]
    for (uint i = 0; i < lightCount; ++i) {
        Light2DPoint light = Lights[i];

        // RenderSystem already filters World-pass lights on CPU. Keeping this
        // guard here makes bad hand-authored buffers harmless and documents the
        // layer-mask contract on the GPU side too.
        if ((light.layerMask & 1u) == 0u || light.radius <= 0.0 || light.intensity <= 0.0) {
            continue;
        }

        float safeZoom = max(zoom, 0.0001);
        float2 lightScreen = (light.position - cameraPos) * safeZoom + viewportSize * 0.5;
        float radiusScreen = light.radius * safeZoom;

        if (lightTouchesTile(lightScreen, radiusScreen, tileMin, tileMax)) {
            if (writeCount < maxLightsPerTile) {
                TileLightIndices[writeBase + writeCount] = i;
                writeCount++;
            }
        }
    }

    TileRanges[tileId] = uint2(writeBase, writeCount);
}
