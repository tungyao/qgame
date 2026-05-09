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
    uint singlePass;   // 1 = internal loop with barriers, 0 = multi-pass (legacy)
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
    uint count = DrawArgs[1];

    if (singlePass != 0u) {
        // All sort threads are in a single workgroup. Run the entire
        // odd-even transposition sort inside this dispatch, using group
        // memory barriers between phases. This avoids the massive per-pass
        // overhead of issuing particleCount separate SDL_BeginGPUComputePass
        // calls from the CPU.
        for (uint p = 0u; p < maxParticleCount; ++p) {
            uint i = gid.x * 2u + (p & 1u);
            if (i + 1u < count) {
                uint aIdx = AliveIndices[i];
                uint bIdx = AliveIndices[i + 1u];
                if (!Less(aIdx, bIdx)) {
                    AliveIndices[i] = bIdx;
                    AliveIndices[i + 1u] = aIdx;
                }
            }
            DeviceMemoryBarrierWithGroupSync();
        }
    } else {
        // Multi-pass path for particle pools that span multiple workgroups
        // (maxParticles > ~256). The CPU dispatches one pass per phase and
        // the per-pass begin/end provides the global device barrier.
        uint i = gid.x * 2u + (phase & 1u);
        if (i + 1u >= count) {
            return;
        }

        uint aIdx = AliveIndices[i];
        uint bIdx = AliveIndices[i + 1u];
        if (!Less(aIdx, bIdx)) {
            AliveIndices[i] = bIdx;
            AliveIndices[i + 1u] = aIdx;
        }
    }
}
