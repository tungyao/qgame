// ─────────────────────────────────────────────────────────────────────────────
// particle_wboit.frag.hlsl
//
// Weighted Blended Order-Independent Transparency fragment shader for GPU
// particles.  Writes to two render targets simultaneously:
//
//   target 0 — accum  (RGBA16F):  Σ(color * alpha * weight, alpha * weight)
//   target 1 — reveal (R16F):     multiplicative (1 - alpha * weight)
//
// Blend state required:
//   accum:  src = ONE,  dst = ONE,            op = ADD
//   reveal: src = ZERO, dst = ONE_MINUS_SRC_COLOR, op = ADD
//
// Weight function (McGuire & Bavoil 2013, adapted for 2D):
//   w(depth, alpha) = alpha * clamp(0.03 / (1e-5 + z²), 1e-2, 1e3)
//   where z = max(1e-5, 1.0 - depth) maps near→0, far→1
//
// The vertex shader passes a normalised depth value derived from the particle's
// draw layer and (optionally) world-space Y.  Particles on higher layers or
// closer to the camera receive higher weights and occlude farther particles.
//
// Composite formula (fullscreen pass):
//   C_final = accum.rgb / max(accum.a, epsilon) * (1 - reveal.r) + bg * reveal.r
// ─────────────────────────────────────────────────────────────────────────────

struct PSInput
{
    float4 position : SV_Position;
    float2 outUV    : TEXCOORD0;
    float4 outColor : TEXCOORD1;
    float  depth    : TEXCOORD2;   // 0 = farthest, 1 = nearest
};

struct PSOutput
{
    float4 accum  : SV_Target0;    // RGBA16F
    float  reveal : SV_Target1;    // R16F
};

// ═════════════════════════════════════════════════════════════════════════════
// wboit_weight — penalise particles that are farther from the camera.
// Near particles (depth ≈ 1, z ≈ 0) get full weight.
// Far particles  (depth ≈ 0, z ≈ 1) get weight ≈ 1 / z².
// ═════════════════════════════════════════════════════════════════════════════
float wboit_weight(float depth, float alpha)
{
    float z = max(1e-5, 1.0 - depth);          // far → larger z
    float w = alpha * clamp(0.03 / (1e-5 + z * z), 1e-2, 1e3);
    return w;
}

Texture2D    tex  : register(t0, space2);
SamplerState samp : register(s0, space2);

PSOutput main(PSInput input)
{
    float4 baseColor = tex.Sample(samp, input.outUV) * input.outColor;
    float  alpha     = baseColor.a;
    float  weight    = wboit_weight(input.depth, alpha);

    PSOutput o;
    o.accum  = float4(baseColor.rgb * weight, alpha * weight);
    o.reveal = alpha * weight;                   // single channel → R16F
    return o;
}
