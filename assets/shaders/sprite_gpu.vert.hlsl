struct GPUSprite {
    float4 transform0;
    float4 transform1;
    float4 transform2;
    float4 color;
    float4 uv;
    uint textureIndex;
    uint layer;
    int sortKey;
    uint flags;
};

StructuredBuffer<GPUSprite> SpriteBuffer : register(t0, space0);
StructuredBuffer<uint> IndexBuffer : register(t1, space0);

cbuffer ViewUBO : register(b0, space1) {
    float4x4 viewProj;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 outUV    : TEXCOORD0;
    float4 outColor : TEXCOORD1;
    uint texIndex   : TEXCOORD2;
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
    
    uint spriteIdx = IndexBuffer[iid];
    GPUSprite s = SpriteBuffer[spriteIdx];
    
    int vertIdx = vid & 3;
    float2 qv = quadVerts[vertIdx];
    
    float tx = s.transform0.w;
    float ty = s.transform1.w;
    
    float m00 = s.transform0.x;
    float m01 = s.transform0.y;
    float m10 = s.transform1.x;
    float m11 = s.transform1.y;
    
    float2 worldPos = float2(
        tx + m00 * qv.x + m01 * qv.y,
        ty + m10 * qv.x + m11 * qv.y
    );
    
    output.position = mul(viewProj, float4(worldPos, 0.0, 1.0));
    
    float u0 = s.uv.x;
    float v0 = s.uv.y;
    float u1 = s.uv.z;
    float v1 = s.uv.w;
    
    if (vertIdx == 0) output.outUV = float2(u0, v0);
    else if (vertIdx == 1) output.outUV = float2(u1, v0);
    else if (vertIdx == 2) output.outUV = float2(u1, v1);
    else output.outUV = float2(u0, v1);
    
    output.outColor = s.color;
    output.texIndex = s.textureIndex;
    
    return output;
}
