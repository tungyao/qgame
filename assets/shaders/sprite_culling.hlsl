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

StructuredBuffer<GPUSprite> SpriteBuffer : register(t0);
RWStructuredBuffer<uint> VisibleBuffer : register(u0);
RWStructuredBuffer<uint> CounterBuffer : register(u1);

cbuffer CameraParams : register(b0) {
    float camX;
    float camY;
    float zoom;
    float rotation;
    float viewMinX;
    float viewMinY;
    float viewMaxX;
    float viewMaxY;
    uint spriteCount;
    uint cullEnabled;
    uint layerMask;
    uint pad;
};

groupshared uint localCount;
groupshared uint localIndices[64];

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID, uint localIdx : SV_GroupIndex) {
    if (localIdx == 0) {
        localCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    
    bool visible = false;
    
    if (gid.x < spriteCount) {
        GPUSprite s = SpriteBuffer[gid.x];
        
        uint passBits = (s.flags >> 1) & 0x7;
        if ((layerMask & (1u << passBits)) != 0u) {
            if (cullEnabled == 0u) {
                visible = true;
            } else {
                float tx = s.transform0.w;
                float ty = s.transform1.w;
                
                float sx = abs(s.transform0.x) + abs(s.transform0.y);
                float sy = abs(s.transform1.x) + abs(s.transform1.y);
                float halfW = sx * 0.5;
                float halfH = sy * 0.5;
                
                if (tx + halfW >= viewMinX && tx - halfW <= viewMaxX &&
                    ty + halfH >= viewMinY && ty - halfH <= viewMaxY) {
                    visible = true;
                }
            }
        }
    }
    
    if (visible) {
        uint lidx;
        InterlockedAdd(localCount, 1, lidx);
        if (lidx < 64) {
            localIndices[lidx] = gid.x;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    
    if (localIdx == 0 && localCount > 0) {
        uint globalOffset;
        InterlockedAdd(CounterBuffer[0], localCount, globalOffset);
        for (uint i = 0; i < localCount && globalOffset + i < spriteCount; ++i) {
            VisibleBuffer[globalOffset + i + 1] = localIndices[i];
        }
    }
}
