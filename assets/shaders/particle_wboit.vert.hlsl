// ─────────────────────────────────────────────────────────────────────────────
// particle_wboit.vert.hlsl
//
// GPU particle vertex shader for WBOIT rendering.  Expands each particle
// instance into a 4-vertex quad (no vertex buffer — vertices generated from
// SV_VertexID).  Outputs a normalised depth value for the WBOIT weight
// function in the fragment shader.
//
// Depth derivation:  particles on higher layers or with lower Y (when ySort
// is enabled) are considered "nearer" → depth closer to 1.
// ─────────────────────────────────────────────────────────────────────────────

struct GPUParticle
{
    float4 posLife;       // x, y, age, lifetime
    float4 velSize;       // vx, vy, sizeStart, sizeEnd
    float4 color0;
    float4 color1;
    float4 uv;
    uint   textureIndex;
    uint   layer;
    int    sortKey;
    uint   flags;         // bit 0-2 = RenderPass, bit 8-9 = sort mode
};

StructuredBuffer<GPUParticle> Particles    : register(t0, space0);
StructuredBuffer<uint>        AliveIndices : register(t1, space0);

cbuffer ViewUBO : register(b0, space1)
{
    float4x4 viewProj;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 outUV    : TEXCOORD0;
    float4 outColor : TEXCOORD1;
    float  depth    : TEXCOORD2;   // 0 = farthest, 1 = nearest
};

static const float2 quadVerts[4] = {
    float2(-0.5, -0.5),
    float2( 0.5, -0.5),
    float2( 0.5,  0.5),
    float2(-0.5,  0.5)
};

// ── Normalise layer + (optionally) y-position into a [0,1] depth ────────────
float computeDepth(uint layer, uint flags, float worldY)
{
    // Base depth from layer:  layer 0 = farthest (0), layer 255 = nearest (1)
    float depth = saturate(float(layer) / 255.0);

    // If ySort is enabled, refine by world Y so lower-Y particles (drawn
    // first in back-to-front order) are "farther" in WBOIT weight space.
    uint sortMode = (flags >> 8u) & 3u;
    if (sortMode == 1u)   // ParticleSortMode::Y
    {
        // Map world Y to a sub-layer offset:  [-100..100] → [-0.01..0.01]
        depth += saturate(-worldY / 20000.0 + 0.5) * 0.004;
    }

    return saturate(depth);
}

VSOutput main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    VSOutput output;

    GPUParticle p = Particles[AliveIndices[iid]];
    uint vertIdx = vid & 3;

    float alive = (p.posLife.z >= 0.0 && p.posLife.z < p.posLife.w) ? 1.0 : 0.0;
    float t = alive > 0.0 ? saturate(p.posLife.z / max(p.posLife.w, 0.0001)) : 0.0;
    float size = lerp(p.velSize.z, p.velSize.w, t) * alive;

    float2 worldPos = p.posLife.xy + quadVerts[vertIdx] * size;
    output.position = mul(viewProj, float4(worldPos, 0.0, 1.0));

    if (vertIdx == 0) output.outUV = p.uv.xy;
    else if (vertIdx == 1) output.outUV = float2(p.uv.z, p.uv.y);
    else if (vertIdx == 2) output.outUV = p.uv.zw;
    else output.outUV = float2(p.uv.x, p.uv.w);

    output.outColor = lerp(p.color0, p.color1, t);
    output.outColor.a *= alive;

    // Depth for WBOIT weight: combine layer and (optionally) Y position.
    output.depth = computeDepth(p.layer, p.flags, p.posLife.y);

    return output;
}
