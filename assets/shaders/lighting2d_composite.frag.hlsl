// Final 2D lighting composite.
//
// This shader is the first step away from swapchain overlay lighting. World
// geometry is rendered into WorldColor, compute writes LightingTexture, and this
// pass combines them into the swapchain before UI/Text are drawn.

Texture2D    WorldColor      : register(t0, space2);
SamplerState WorldSampler    : register(s0, space2);
Texture2D    LightingTexture : register(t1, space2);
SamplerState LightingSampler : register(s1, space2);

cbuffer LightingCompositeParams : register(b0, space3)
{
    float4 ambientColor;
    float  ambientIntensity;
    float  exposure;
    uint   debugMode;
    float  pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 inUV     : TEXCOORD0;
    float4 inColor  : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target
{
    float4 world = WorldColor.Sample(WorldSampler, input.inUV);
    float4 light = LightingTexture.Sample(LightingSampler, input.inUV);

    if (debugMode == 1u) {
        return world;
    }
    if (debugMode == 2u) {
        return float4(light.rgb, 1.0);
    }

    // Current LightingTexture still comes from the L4 compute prototype and
    // carries "colored light in rgb + strength in alpha" rather than a pure
    // physically linear multiplier. Convert it into a practical multiplier so
    // Light2D visibly modulates WorldColor without relying on alpha overlay.
    float3 ambient = ambientColor.rgb * max(ambientIntensity, 0.0);
    float3 direct = light.rgb * light.a * 2.4;
    float3 multiplier = saturate(ambient + direct);
    multiplier = max(multiplier, ambient * 0.35);

    float3 lit = world.rgb * multiplier * max(exposure, 0.0);
    return float4(saturate(lit), world.a);
}
