// SDL3 GPU D3D12: PS samplers at register(t0/s0, space2)

Texture2D    tex  : register(t0, space2);
SamplerState samp : register(s0, space2);

struct PSInput
{
    float4 position : SV_Position;
    float2 inUV     : TEXCOORD0;
    float4 inColor  : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target
{
    return tex.Sample(samp, input.inUV) * input.inColor;
}
