// ─────────────────────────────────────────────────────────────────────────────
// particle_emit.comp.hlsl
//
// GPU-side particle emission + simulation, a full replacement for the old
// CPU-driven particle_update.comp.hlsl.
//
// Architecture:
//   - Emitter descriptors live in a structured buffer (Emitters[]).  The CPU
//     uploads only the position / rotation each frame; all emission logic
//     (accumulator, random seeds, spawn scheduling) is GPU-resident.
//   - A lock-free free list (FreeListHead + FreeList[]) recycles dead
//     particle slots so the pool never leaks.
//   - Alive particles are compacted into AliveIndices[] via atomic counter.
//     DrawArgs[1] (instance count) is written by GPU, consumed by indirect draw.
//
// Dispatch:  (emitterCount + 63) / 64  workgroups of 64 threads.
//   Thread gid processes emitter gid if gid < emitterCount, then processes
//   one particle slot within that emitter's range.
//
// Data flow per frame:
//   1. For each emitter:  spawnCount = floor(accumulator + dt * rate).
//      For each spawn:  pop free-list → init particle → store in ring buffer.
//   2. For each emitter slot:  advance age, velocity, position.
//      If dead → push to free list.  If alive → compact into AliveIndices.
//   3. Thread 0 of each emitter writes DrawArgs for that emitter's draw call.
// ─────────────────────────────────────────────────────────────────────────────

// ── Global constants ────────────────────────────────────────────────────────
#define SENTINEL     0xFFFFFFFFu
#define MAX_PARTICLES 65536u
#define THREADS       64u

// ── Particle struct (must match engine::GPUParticle, 96 bytes) ──────────────
struct GPUParticle
{
    float4 posLife;       // x, y, age, lifetime
    float4 velSize;       // vx, vy, sizeStart, sizeEnd
    float4 color0;
    float4 color1;
    float4 uv;
    uint   textureIndex;
    uint   layer;
    int    sortKey;
    uint   flags;         // bit 0-2 = RenderPass, bit 8-9 = sort mode
};

// ── Emitter descriptor (must match engine::GPUEmitter, 128 bytes) ───────────
struct GPUEmitter
{
    float4 pos_rot;       // x, y, rotation, as_uint(firstParticle)
    float4 config0;       // as_uint(particleCount), emissionRate, lifetime, speedMin
    float4 config1;       // speedMax, sizeStart, sizeEnd, spread
    float4 colorStart;    // rgba
    float4 colorEnd;      // rgba
    float4 uvRect;        // u0, v0, u1, v1
    float4 misc;          // as_uint(textureIndex), as_uint(layer), as_int(sortKey), as_uint(flags)
    float4 state;         // accumulator, as_uint(seed), as_uint(writeCursor), pad
};

// ── Binding points ──────────────────────────────────────────────────────────
RWStructuredBuffer<GPUParticle> Particles    : register(u0, space1);
RWStructuredBuffer<uint>        AliveIndices : register(u1, space1);
RWStructuredBuffer<uint>        FreeList     : register(u2, space1);
RWStructuredBuffer<uint>        DrawArgs     : register(u3, space1);   // per-emitter indirect args
RWStructuredBuffer<GPUEmitter>  Emitters     : register(u4, space1);

cbuffer EmitParams : register(b0, space2)
{
    float dt;
    uint  emitterCount;
    uint  _pad0;
    uint  _pad1;
};

// ═════════════════════════════════════════════════════════════════════════════
// Small LCG random — deterministic, thread-safe, fast.
// ═════════════════════════════════════════════════════════════════════════════
float rand01(inout uint state)
{
    state = state * 1664525u + 1013904223u;
    return float((state >> 8) & 0x00FFFFFFu) / 16777215.0f;
}

// ═════════════════════════════════════════════════════════════════════════════
// lock-free free list:  push dead slot → pop for new particles.
// FreeList[slot] stores the index of the next free slot (or SENTINEL).
// FreeListHead is aliased to FreeList[MAX_PARTICLES] — one extra slot.
// ═════════════════════════════════════════════════════════════════════════════
uint freeListPop()
{
    uint slot;
    for (;;)
    {
        slot = FreeList[MAX_PARTICLES];   // head
        if (slot == SENTINEL) return SENTINEL;
        uint next = FreeList[slot];
        uint original;
        InterlockedCompareExchange(FreeList[MAX_PARTICLES], next, slot, original);
        if (original == slot) return slot;
    }
}

void freeListPush(uint slot)
{
    uint oldHead;
    for (;;)
    {
        oldHead = FreeList[MAX_PARTICLES];
        FreeList[slot] = oldHead;
        uint original;
        InterlockedCompareExchange(FreeList[MAX_PARTICLES], slot, oldHead, original);
        if (original == oldHead) break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// initParticle — 用发射器参数填充一个 GPUParticle。
// ═════════════════════════════════════════════════════════════════════════════
void initParticle(inout GPUParticle p, GPUEmitter e, inout uint seed)
{
    uint first = asuint(e.pos_rot.w);
    uint count = asuint(e.config0.x);

    float spread   = e.config1.w;
    float speedMin = e.config0.w;
    float speedMax = e.config1.x;
    float angle    = e.pos_rot.z + (rand01(seed) - 0.5f) * max(0.0f, spread);
    float speed    = speedMin + (speedMax - speedMin) * rand01(seed);

    p.posLife   = e.pos_rot;                          // x, y, 0, lifetime
    p.posLife.z = 0.0f;                               // age = 0
    p.posLife.w = max(e.config0.z, 0.001f);           // lifetime
    p.velSize   = float4(cos(angle) * speed, sin(angle) * speed,
                         e.config1.y, e.config1.z);   // sizeStart, sizeEnd
    p.color0    = e.colorStart;
    p.color1    = e.colorEnd;
    p.uv        = e.uvRect;
    p.textureIndex = asuint(e.misc.x);
    p.layer     = asuint(e.misc.y);
    p.sortKey   = asint(e.misc.z);
    p.flags     = asuint(e.misc.w);
}

// ═════════════════════════════════════════════════════════════════════════════
// main
// ═════════════════════════════════════════════════════════════════════════════
[numthreads(THREADS, 1, 1)]
void main(uint gid : SV_GroupID, uint tid : SV_GroupThreadID, uint gtid : SV_DispatchThreadID)
{
    // ── Per-emitter init (one emitter per workgroup) ────────────────────────
    if (gid >= emitterCount) return;

    GPUEmitter e = Emitters[gid];
    uint first = asuint(e.pos_rot.w);
    uint count = asuint(e.config0.x);
    if (count == 0) return;

    // Thread 0: reset this emitter's indirect draw args slot.
    // Each emitter owns 5 consecutive uints in DrawArgs.
    uint argsBase = gid * 5u;
    if (tid == 0) {
        DrawArgs[argsBase + 0] = 6;   // index count per instance
        DrawArgs[argsBase + 1] = 0;   // instance count (computed below)
        DrawArgs[argsBase + 2] = 0;   // first index
        DrawArgs[argsBase + 3] = 0;   // vertex offset
        DrawArgs[argsBase + 4] = 0;   // first instance
    }
    GroupMemoryBarrierWithGroupSync();

    // ── Phase 1: emission ──────────────────────────────────────────────────
    // Only one thread per workgroup handles emission (accumulator is shared).
    if (tid == 0)
    {
        float acc = e.state.x + dt * max(0.0f, e.config0.y);
        uint spawnCount = uint(acc);
        e.state.x = acc - float(spawnCount);

        uint seed = asuint(e.state.y);
        uint cursor = asuint(e.state.z);

        for (uint s = 0u; s < spawnCount && s < count; ++s)
        {
            // Try free list first, then ring buffer cursor.
            uint slot = freeListPop();
            if (slot == SENTINEL) {
                slot = first + (cursor % count);
            }

            GPUParticle p = Particles[slot];  // read existing (preserve unrelated fields)
            initParticle(p, e, seed);
            Particles[slot] = p;

            cursor = (cursor + 1u) % count;
        }

        e.state.y = asuint(seed);
        e.state.z = asuint(cursor);
        Emitters[gid] = e;
    }
    GroupMemoryBarrierWithGroupSync();

    // Re-read emitter (state may have been updated by thread 0)
    e = Emitters[gid];

    // ── Phase 2: update + compact ──────────────────────────────────────────
    // Each thread handles one particle slot within the emitter's range.
    uint localIdx = tid;
    if (localIdx >= count) return;

    uint globalIdx = first + localIdx;
    GPUParticle p = Particles[globalIdx];

    // Skip empty slots
    if (p.posLife.z < 0.0f) return;

    // Advance age
    p.posLife.z += dt;

    if (p.posLife.z >= p.posLife.w)
    {
        // Particle died — return slot to free list.
        p.posLife.z = -1.0f;
        Particles[globalIdx] = p;
        freeListPush(globalIdx);
        return;
    }

    // Move particle
    p.posLife.xy += p.velSize.xy * dt;
    Particles[globalIdx] = p;

    // Compact alive particle into the instance array (atomic per emitter).
    uint aliveSlot;
    InterlockedAdd(DrawArgs[argsBase + 1], 1u, aliveSlot);
    AliveIndices[aliveSlot] = globalIdx;
}
