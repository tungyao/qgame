struct GPUParticle {
    float4 posLife;      // x, y, age, lifetime
    float4 velSize;      // vx, vy, sizeStart, sizeEnd
    float4 color0;
    float4 color1;
    float4 uv;
    uint   textureIndex;
    uint   layer;
    int    sortKey;
    uint   flags;
};

StructuredBuffer<GPUParticle> Particles    : register(t0, space0);
StructuredBuffer<uint>        AliveIndices : register(t1, space0);

cbuffer ViewUBO : register(b0, space1)
{
    float4x4 viewProj;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 outUV    : TEXCOORD0;
    float4 outColor : TEXCOORD1;
};

static const float2 quadVerts[4] = {
    float2(-0.5, -0.5),
    float2( 0.5, -0.5),
    float2( 0.5,  0.5),
    float2(-0.5,  0.5)
};

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
    return output;
}
