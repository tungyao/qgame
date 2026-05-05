struct GPUParticle {
    float4 posLife;      // x, y, age, lifetime
    float4 velSize;      // vx, vy, sizeStart, sizeEnd
    float4 color0;
    float4 color1;
    float4 uv;
    uint   textureIndex;
    uint   layer;
    int    sortKey;
    uint   flags;
};

RWStructuredBuffer<GPUParticle> Particles    : register(u0, space1);
RWStructuredBuffer<uint>        AliveIndices : register(u1, space1);
RWStructuredBuffer<uint>        DrawArgs     : register(u2, space1);

cbuffer ParticleUpdateParams : register(b0, space2)
{
    float dt;
    uint  firstParticle;
    uint  particleCount;
    uint  _pad0;
};

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    if (gid.x >= particleCount) {
        return;
    }

    // DrawArgs matches SDL_GPUIndexedIndirectDrawCommand:
    // [0] num_indices, [1] num_instances, [2] first_index,
    // [3] vertex_offset, [4] first_instance.
    // The CPU clears it before this dispatch; compute owns the live count.
    if (gid.x == 0) {
        DrawArgs[0] = 6;
        DrawArgs[2] = 0;
        DrawArgs[3] = 0;
        DrawArgs[4] = 0;
    }

    uint idx = firstParticle + gid.x;
    GPUParticle p = Particles[idx];

    // age < 0 marks an unused slot. The CPU writes freshly emitted particles
    // with age = 0, so inactive slots can be skipped without touching memory.
    if (p.posLife.z < 0.0) {
        return;
    }

    p.posLife.z += dt;
    if (p.posLife.z >= p.posLife.w) {
        p.posLife.z = -1.0;
        Particles[idx] = p;
        return;
    }

    p.posLife.xy += p.velSize.xy * dt;
    Particles[idx] = p;

    uint aliveSlot;
    InterlockedAdd(DrawArgs[1], 1u, aliveSlot);
    AliveIndices[aliveSlot] = idx;
}
