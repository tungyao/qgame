// ─────────────────────────────────────────────────────────────────────────────
// wboit_composite.frag.hlsl
//
// Fullscreen composite pass: blends the WBOIT accum + reveal textures (from
// the particle render pass) onto the scene background colour.
//
// Composite formula (McGuire & Bavoil 2013):
//   C_final = accum.rgb / max(accum.a, epsilon) * (1 - reveal) + bg * reveal
//
// Inputs:
//   t0 (space2) = accum texture  (RGBA16F)
//   s0 (space2) = accum sampler  (point — no filtering needed for fullscreen)
//   t1 (space2) = reveal texture (R16F)
//   s1 (space2) = reveal sampler (point)
//
// Uniform:
//   b0 (space1) = backgroundColor (float4, premultiplied or straight)
// ─────────────────────────────────────────────────────────────────────────────

Texture2D    accumTex   : register(t0, space2);
SamplerState accumSamp  : register(s0, space2);
Texture2D    revealTex  : register(t1, space2);
SamplerState revealSamp : register(s1, space2);

cbuffer CompositeParams : register(b0, space1)
{
    float4 backgroundColor;   // scene clear / fog colour
    float  intensity;         // overall WBOIT intensity multiplier
    float  _pad0;
    float  _pad1;
    float  _pad2;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    float4 accum  = accumTex.Sample(accumSamp, input.uv);
    float  reveal = revealTex.Sample(revealSamp, input.uv).r;

    // Normalise accumulated colour by total weight.
    float  totalAlpha = max(accum.a, 1e-5);
    float3 particleColor = (accum.rgb / totalAlpha) * intensity;

    // Blend:  particle (1 - reveal)  +  background (reveal)
    // When reveal = 1, no particles present → 100% background.
    // When reveal ≈ 0, many particles → mostly particle colour.
    float3 result = particleColor * (1.0 - reveal) + backgroundColor.rgb * reveal;

    return float4(result, 1.0);
}
