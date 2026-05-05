struct GPUParticle {
    float4 posLife;
    float4 velSize;
    float4 color0;
    float4 color1;
    float4 uv;
    uint   textureIndex;
    uint   layer;
    int    sortKey;
    uint   flags;
};

StructuredBuffer<GPUParticle> Particles : register(t0, space0);
StructuredBuffer<uint>        DrawArgs  : register(t1, space0);
RWStructuredBuffer<uint>      AliveIndices : register(u0, space1);

cbuffer SortParams : register(b0, space2)
{
    uint phase;
    uint maxParticleCount;
    uint _pad0;
    uint _pad1;
};

bool Less(uint leftIdx, uint rightIdx)
{
    GPUParticle a = Particles[leftIdx];
    GPUParticle b = Particles[rightIdx];

    if (a.layer != b.layer) {
        return a.layer < b.layer;
    }

    // bit 8 mirrors ParticleComponent::ySort. When enabled, lower y draws
    // earlier, matching the CPU sprite sort convention in RenderSystem.
    bool aySort = (a.flags & (1u << 8)) != 0u;
    bool bySort = (b.flags & (1u << 8)) != 0u;
    if (aySort != bySort) {
        return !aySort;
    }
    if (aySort && a.posLife.y != b.posLife.y) {
        return a.posLife.y < b.posLife.y;
    }

    if (a.sortKey != b.sortKey) {
        return a.sortKey < b.sortKey;
    }
    return leftIdx < rightIdx;
}

[numthreads(128, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    // Odd-even transposition sort:
    // even phase compares (0,1), (2,3)...
    // odd phase compares (1,2), (3,4)...
    // Running maxParticleCount phases fully sorts the compact alive list for
    // any alive count, without CPU readback of DrawArgs[1].
    uint i = gid.x * 2u + (phase & 1u);
    uint count = DrawArgs[1];
    if (i + 1u >= count) {
        return;
    }

    uint aIdx = AliveIndices[i];
    uint bIdx = AliveIndices[i + 1u];
    bool outOfOrder = !Less(aIdx, bIdx);

    if (outOfOrder) {
        AliveIndices[i] = bIdx;
        AliveIndices[i + 1u] = aIdx;
    }
}
