// SDL3 GPU D3D12: PS samplers at register(t0/s0, space2)
// Sprite frag with optional region tinting (Tinting 组件)。
//   - tex (t0):       base sprite texture
//   - regionTex (t1): R8 region ID 图（无则绑 dummy 1×1）
//   - UBO b0:         hasRegion + 16 个 region tint colors

Texture2D    tex       : register(t0, space2);
SamplerState samp      : register(s0, space2);
Texture2D    regionTex : register(t1, space2);
SamplerState regionSamp : register(s1, space2);

cbuffer TintParams : register(b0, space3)
{
    float4 regionTints[16];
    int    hasRegion;
    int    _pad0;
    int    _pad1;
    int    _pad2;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 inUV     : TEXCOORD0;
    float4 inColor  : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target
{
    float4 base = tex.Sample(samp, input.inUV) * input.inColor;
    if (hasRegion != 0)
    {
        float idF = regionTex.Sample(regionSamp, input.inUV).r;
        int id = (int)(idF * 255.0 + 0.5);
        if (id > 0 && id < 16)
        {
            base *= regionTints[id];
        }
    }
    return base;
}
