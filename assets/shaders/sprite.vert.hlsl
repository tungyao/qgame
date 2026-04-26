// SDL3 GPU D3D12: VS UBO at register(b0, space1)
// Vertex semantics: TEXCOORD{location} (SDL default)

cbuffer UBO : register(b0, space1)
{
    float4x4 proj;
};

struct VSInput
{
    float2 inPos   : TEXCOORD0;
    float2 inUV    : TEXCOORD1;
    float4 inColor : TEXCOORD2;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 outUV    : TEXCOORD0;
    float4 outColor : TEXCOORD1;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(proj, float4(input.inPos, 0.0f, 1.0f));
    output.outUV    = input.inUV;
    output.outColor = input.inColor;
    return output;
}
