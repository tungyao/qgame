// L4 2D lighting prototype with tiled culling, soft shadows, and night ambience.
//
// This compute shader writes a half-resolution dynamic-light overlay texture.
// The renderer then draws that texture over the world color using the existing
// sprite pipeline. Unlike L2, it does not scan every uploaded light per pixel:
// a preceding cull pass builds a compact light list for each screen-space tile.
//
// The overlay intentionally carries both signals for the current prototype:
//   - visible light writes a colored translucent glow;
//   - occluded light writes a black translucent shadow.
// A later full composite pass should replace this alpha-blended approximation
// with sceneColor * lighting + shadow, but keeping both signals here makes demo3
// exercise dynamic lights and shadows together without adding a render graph yet.

struct Light2DPoint {
    float2 position;
    float  radius;
    float  intensity;
    float4 color;
    float  softness;
    uint   layerMask;
    uint   castsShadow;
    uint   pad0;
};

struct Light2DSegment {
    float2 a;
    float2 b;
    float  opacity;
    float  pad0;
    float  pad1;
    float  pad2;
};

// SDL_GPU HLSL convention used elsewhere in the engine:
//   t#,space0: readonly storage buffers
//   u#,space1: readwrite storage buffers/textures
//   b#,space2: push uniform data
StructuredBuffer<Light2DPoint>   Lights   : register(t0, space0);
StructuredBuffer<Light2DSegment> Segments : register(t1, space0);
StructuredBuffer<uint2>          TileRanges : register(t2, space0);
StructuredBuffer<uint>           TileLightIndices : register(t3, space0);
RWTexture2D<float4>              ShadowOverlay : register(u0, space1);

cbuffer Lighting2DParams : register(b0, space2)
{
    float2 cameraPos;
    float2 viewportSize;
    float2 lightingSize;
    uint   lightCount;
    uint   segmentCount;
    uint   tileCols;
    uint   tileRows;
    uint   tileSize;
    uint   maxLightsPerTile;
    float4 ambientColor;
    float  zoom;
    float  shadowStrength;
    float  ambientIntensity;
    float  exposure;
    uint   frameIndex;
    float  time;
    float2 pad0;
};

float cross2(float2 a, float2 b)
{
    return a.x * b.y - a.y * b.x;
}

bool rayIntersectsSegment(float2 p, float2 q, float2 a, float2 b)
{
    float2 r = q - p;
    float2 s = b - a;
    float denom = cross2(r, s);
    if (abs(denom) < 0.00001) {
        return false;
    }

    float t = cross2(a - p, s) / denom;
    float u = cross2(a - p, r) / denom;
    return t > 0.001 && t < 0.999 && u >= 0.0 && u <= 1.0;
}

float hash12(float2 p)
{
    // Tiny deterministic hash for temporal jitter. The input mixes pixel
    // position and frame index, so neighboring pixels do not all sample the
    // same point on the area light and subsequent blur can smooth the pattern.
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float2 unitDiskSample(uint sampleIndex, float2 pixel, uint frame)
{
    // Four low-discrepancy directions, rotated per frame. This is deliberately
    // stable and cheap: it gives soft penumbra without the obvious shimmer of
    // fully random samples, while the frame-dependent phase prevents fixed
    // banding on perfectly vertical/horizontal occluders.
    const float PI = 3.14159265359;
    float base = (float)sampleIndex + 0.5;
    float jitter = hash12(pixel + float2(frame * 17u, frame * 29u));
    float angle = (base * 1.57079632679) + jitter * 0.78539816339;
    float radius = sqrt((base + jitter * 0.35) / 4.35);
    return float2(cos(angle), sin(angle)) * radius;
}

float sampleVisibility(float2 world, Light2DPoint light, float2 pixel)
{
    if (light.castsShadow == 0u) {
        return 1.0;
    }

    float visibility = 0.0;
    float sampleRadius = max(light.softness, 0.0);

    // A softness of 0 keeps the light as a hard point light. For area lights we
    // sample four nearby points on the light disk and average whether each path
    // reaches the pixel. Segment opacity attenuates visibility rather than
    // forcing a binary answer, which keeps half-transparent occluders useful.
    [unroll]
    for (uint sampleIndex = 0; sampleIndex < 4u; ++sampleIndex) {
        float2 sampleLight = light.position;
        if (sampleRadius > 0.0) {
            sampleLight += unitDiskSample(sampleIndex, pixel, frameIndex) * sampleRadius;
        }

        float blocked = 0.0;
        [loop]
        for (uint i = 0; i < segmentCount; ++i) {
            Light2DSegment seg = Segments[i];
            if (rayIntersectsSegment(world, sampleLight, seg.a, seg.b)) {
                blocked = max(blocked, saturate(seg.opacity));
            }
        }
        visibility += 1.0 - saturate(blocked);
    }

    return visibility * 0.25;
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    if (gid.x >= (uint)lightingSize.x || gid.y >= (uint)lightingSize.y) {
        return;
    }

    // Convert half-resolution lighting pixel -> screen pixel -> world pixel.
    // This mirrors CameraData::screenToWorld and keeps the shader independent
    // from the current raster path.
    float2 screen = (float2(gid.xy) + 0.5) * (viewportSize / lightingSize);
    float2 world = (screen - viewportSize * 0.5) / max(zoom, 0.0001) + cameraPos;

    float3 lightAccum = float3(0.0, 0.0, 0.0);
    float  glowAlpha = 0.0;
    float  shadowAlpha = 0.0;
    float  strongestVisibilityLoss = 0.0;
    uint2 tileCoord = min((uint2)floor(screen / max((float)tileSize, 1.0)),
                          uint2(max(tileCols, 1u) - 1u, max(tileRows, 1u) - 1u));
    uint tileId = tileCoord.y * tileCols + tileCoord.x;
    uint2 range = TileRanges[tileId];
    uint count = min(range.y, maxLightsPerTile);

    [loop]
    for (uint listIndex = 0; listIndex < count; ++listIndex) {
        uint lightIndex = TileLightIndices[range.x + listIndex];
        if (lightIndex >= lightCount) {
            continue;
        }

        Light2DPoint light = Lights[lightIndex];
        if ((light.layerMask & 1u) == 0u || light.radius <= 0.0 || light.intensity <= 0.0) {
            continue;
        }

        float dist = distance(world, light.position);

        if (dist < light.radius) {
            // A squared falloff gives the glow a stronger hot center while
            // still fading softly at the light radius. The visibility term is
            // exactly what ties the old dynamic light visualization to the new
            // shadow path: an occluder can remove the colored contribution and
            // replace it with a dark overlay in the same compute pass.
            float attenuation = saturate(1.0 - dist / light.radius);
            float lightWeight = attenuation * attenuation * max(light.intensity, 0.0);
            float visibility = sampleVisibility(world, light, (float2)gid.xy);
            float visibleWeight = lightWeight * visibility;
            float visibilityLoss = 1.0 - visibility;

            lightAccum += light.color.rgb * visibleWeight;
            glowAlpha = max(glowAlpha, visibleWeight * light.color.a * 0.36);
            shadowAlpha = max(shadowAlpha, visibilityLoss * attenuation * shadowStrength);
            strongestVisibilityLoss = max(strongestVisibilityLoss, visibilityLoss * attenuation);
        }
    }

    // Environment2D enters the composite here. The alpha-blended prototype
    // cannot multiply the already-rendered world color yet, so we preserve night
    // detail by always carrying a low-alpha ambient tint in the lighting texture.
    // Exposure scales both direct light and ambient, while shadowAlpha can still
    // win and draw a dark soft-edged shape when a light is occluded.
    float3 ambient = ambientColor.rgb * saturate(ambientIntensity) * max(exposure, 0.0);
    lightAccum = lightAccum * max(exposure, 0.0) + ambient;

    float luminance = dot(lightAccum, float3(0.2126, 0.7152, 0.0722));
    float finalGlowAlpha = saturate(min(max(glowAlpha, luminance * 0.24), 0.62));
    float finalShadowAlpha = saturate(min(shadowAlpha, 0.82));
    float ambientAlpha = saturate(ambientIntensity * 0.32 + ambientColor.a * 0.04);

    if (finalGlowAlpha + ambientAlpha > finalShadowAlpha * 0.65) {
        float3 glowColor = saturate(lightAccum / max(luminance, 1.0));
        ShadowOverlay[gid.xy] = float4(glowColor, max(finalGlowAlpha, ambientAlpha));
    } else {
        // Preserve a hint of the ambient color even inside shadowed areas. This
        // keeps the night scene readable and satisfies the L4 goal of avoiding
        // crushed black, while the alpha still darkens enough to show occlusion.
        float3 shadowTint = ambient * (0.15 + 0.35 * (1.0 - strongestVisibilityLoss));
        ShadowOverlay[gid.xy] = float4(saturate(shadowTint), finalShadowAlpha);
    }
}
