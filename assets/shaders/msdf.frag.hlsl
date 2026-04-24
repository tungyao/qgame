// SDL3 GPU D3D12: PS samplers at register(t0/s0, space2)

// SDL3 GPU D3D12: PS samplers at space2, PS UBOs at space3
Texture2D    tex  : register(t0, space2);
SamplerState samp : register(s0, space2);

cbuffer MSDFParams : register(b0, space3)
{
    float pxRange;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 inUV     : TEXCOORD0;
    float4 inColor  : TEXCOORD1;
};

float median(float r, float g, float b)
{
    return max(min(r, g), min(max(r, g), b));
}

float4 main(PSInput input) : SV_Target
{
    float3 sampleVal = tex.Sample(samp, input.inUV).rgb;
    float sigDist = median(sampleVal.r, sampleVal.g, sampleVal.b) - 0.5;
    float opacity = clamp(sigDist * pxRange + 0.5, 0.0, 1.0);
    return float4(input.inColor.rgb, input.inColor.a * opacity);
}
