Texture2D    tex  : register(t0, space2);
SamplerState samp : register(s0, space2);

struct PSInput {
    float4 position : SV_Position;
    float2 outUV    : TEXCOORD0;
    float4 outColor : TEXCOORD1;
    uint texIndex   : TEXCOORD2;
};

float4 main(PSInput input) : SV_Target
{
    return tex.Sample(samp, input.outUV) * input.outColor;
}
