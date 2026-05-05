// L4 separable blur for the 2D lighting overlay.
//
// This pass is intentionally tiny and deterministic. The lighting pass already
// does area-light sampling, so this blur is not trying to invent shadows; it
// removes the low-resolution/jitter grain and makes light edges less stair-
// stepped. The renderer runs it twice:
//
//   horizontal: LightingTexture -> BlurTexture
//   vertical:   BlurTexture     -> LightingTexture
//
// Keeping it separable costs two cheap passes instead of a large 2D kernel, and
// keeping all data in storage textures avoids CPU readback or render-target
// gymnastics until the later RenderGraph/composite work lands.

Texture2D<float4> SourceLighting : register(t0, space0);
RWTexture2D<float4> OutputLighting : register(u0, space1);

cbuffer LightingBlurParams : register(b0, space2)
{
    uint2 lightingSize;
    int2  direction;
    float ambientIntensity;
    float exposure;
    uint2 pad0;
};

float4 sampleClamped(int2 p)
{
    int2 maxPixel = int2((int)lightingSize.x - 1, (int)lightingSize.y - 1);
    int2 clamped = clamp(p, int2(0, 0), maxPixel);
    return SourceLighting.Load(int3(clamped, 0));
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    if (gid.x >= lightingSize.x || gid.y >= lightingSize.y) {
        return;
    }

    int2 p = int2(gid.xy);

    // Five-tap Gaussian-like kernel. The weights sum to 1, so pure ambient
    // areas stay stable and do not brighten/darken as blur direction changes.
    float4 c0 = sampleClamped(p - direction * 2) * 0.0625;
    float4 c1 = sampleClamped(p - direction)     * 0.25;
    float4 c2 = sampleClamped(p)                 * 0.375;
    float4 c3 = sampleClamped(p + direction)     * 0.25;
    float4 c4 = sampleClamped(p + direction * 2) * 0.0625;
    float4 blurred = c0 + c1 + c2 + c3 + c4;

    // Preserve alpha bounds after filtering. The small exposure term prevents
    // very low ambient night presets from losing their faint colored floor.
    blurred.a = saturate(max(blurred.a, ambientIntensity * exposure * 0.025));
    OutputLighting[gid.xy] = saturate(blurred);
}
