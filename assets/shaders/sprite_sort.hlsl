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
StructuredBuffer<uint> IndexBuffer : register(t1);
RWStructuredBuffer<uint> SortedBuffer : register(u0);

cbuffer SortParams : register(b0) {
    uint itemCount;
    uint stage;
    uint pass;
    uint pad;
};

uint getSortKey(uint idx) {
    if (idx >= itemCount) return 0xFFFFFFFFu;
    uint spriteIdx = IndexBuffer[idx];
    if (spriteIdx >= itemCount) return 0xFFFFFFFFu;
    
    GPUSprite s = SpriteBuffer[spriteIdx];
    
    uint layer = s.layer;
    int sortKey = s.sortKey;
    uint passVal = (s.flags >> 1) & 0x7u;
    
    uint key = (passVal << 28) | (layer << 20) | (uint(sortKey + 0x7FFF) & 0xFFFFF);
    return key;
}

[numthreads(256, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID) {
    uint idx = gid.x;
    if (idx >= itemCount) return;
    
    uint k = 1u << stage;
    uint j = pass;
    
    uint low = idx & (k - 1);
    uint high = 2u * (idx >> (stage + 1u));
    uint i = high * k + low;
    uint i2 = i | (k >> 1u);
    
    if (i2 < itemCount) {
        uint keyA = getSortKey(i);
        uint keyB = getSortKey(i2);
        
        bool ascending = (high & 1u) == 0u;
        bool shouldSwap = ascending ? (keyA > keyB) : (keyA < keyB);
        
        if (shouldSwap) {
            SortedBuffer[idx] = IndexBuffer[i2];
        } else {
            SortedBuffer[idx] = IndexBuffer[i];
        }
    } else {
        SortedBuffer[idx] = IndexBuffer[idx];
    }
}
