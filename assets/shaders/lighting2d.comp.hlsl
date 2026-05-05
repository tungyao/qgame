// L2 2D hard-shadow prototype.
//
// This compute shader writes a half-resolution black-alpha overlay texture.
// The renderer then draws that texture over the world color using the existing
// sprite pipeline. It is intentionally simple: first light only, up to 64
// occluder segments, hard visibility test from pixel -> light.

struct Light2DPoint {
    float2 position;
    float  radius;
    float  intensity;
    float4 color;
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
RWTexture2D<float4>              ShadowOverlay : register(u0, space1);

cbuffer Lighting2DParams : register(b0, space2)
{
    float2 cameraPos;
    float2 viewportSize;
    float2 lightingSize;
    uint   lightCount;
    uint   segmentCount;
    float  zoom;
    float  shadowStrength;
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

    float alpha = 0.0;
    if (lightCount > 0) {
        Light2DPoint light = Lights[0];
        float dist = distance(world, light.position);

        if (dist < light.radius) {
            float blocked = 0.0;
            uint count = min(segmentCount, 64u);

            [loop]
            for (uint i = 0; i < count; ++i) {
                Light2DSegment seg = Segments[i];
                if (rayIntersectsSegment(world, light.position, seg.a, seg.b)) {
                    blocked = max(blocked, saturate(seg.opacity));
                }
            }

            float attenuation = 1.0 - dist / light.radius;
            alpha = blocked * attenuation * shadowStrength;
        }
    }

    ShadowOverlay[gid.xy] = float4(0.0, 0.0, 0.0, saturate(min(alpha, 0.85)));
}
