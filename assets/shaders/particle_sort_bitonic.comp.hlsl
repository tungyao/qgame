// ─────────────────────────────────────────────────────────────────────────────
// particle_sort_bitonic.comp.hlsl
//
// Bitonic merge-sort for GPU particle alive-index compaction.
//
// Replaces the multi-pass odd-even transposition sort when all data fits in a
// single workgroup (maxParticleCount ≤ 256). Sorting happens entirely within
// groupshared memory with GroupMemoryBarrierWithGroupSync barriers, avoiding
// the massive SDL_BeginGPUComputePass / SDL_EndGPUComputePass overhead that
// the odd‑even path pays on every phase.
//
// Algorithm overview:
//   1. Load alive particle indices from AliveIndices[] into g_Keys[].idx
//      Fill unused slots with SENTINEL (0xFFFFFFFF) so they sort to the end.
//   2. Compute a packed sort‑key for each alive element by reading its
//      GPUParticle struct once. The key is cached in groupshared memory so
//      subsequent comparisons never re‑read device memory.
//   3. Run a classical parallel bitonic sort over n (power‑of‑two) elements:
//        for block_size = 2, 4, 8, …, n:
//            for stride = block_size/2, stride/2, …, 1:
//                compare‑swap(tid, tid ^ stride)
//                GroupMemoryBarrierWithGroupSync()
//   4. Write the sorted .idx values back to AliveIndices[].
//
// Performance (n = 256, the default emitter size):
//   - 8 block‑size iterations × avg 4.5 stride iterations = 36 barriers
//     (vs 256 barriers + 256 CPU passes in the odd‑even path).
//   - Each GPUParticle is read exactly once (pre‑load), not once per
//     comparison (which would be O(n log² n) device reads).
//   - Single compute dispatch from the CPU.
//
// Correctness constraints:
//   - The shader is compiled with [numthreads(256, 1, 1)].  The CPU must
//     dispatch exactly 1 workgroup for this pipeline.  The odd‑even fallback
//     path is used when maxParticleCount > 256.
//   - All threads in the workgroup must participate in every barrier.
//     Threads with tid ≥ aliveCount hold sentinel keys and still execute
//     the compare‑swap loop to satisfy barrier uniformity requirements.
//   - The comparison function LessSortKey() MUST produce identical ordering
//     to the Less() function in particle_sort.comp.hlsl; otherwise particles
//     from the same emitter could be drawn out of order relative to sprites.
// ─────────────────────────────────────────────────────────────────────────────

// ---- GPU-side particle layout (must match engine::GPUParticle) ----
struct GPUParticle
{
    float4 posLife;       // x, y, age, lifetime
    float4 velSize;       // vx, vy, sizeStart, sizeEnd
    float4 color0;        // start colour
    float4 color1;        // end colour
    float4 uv;            // u0, v0, u1, v1
    uint   textureIndex;
    uint   layer;
    int    sortKey;       // user‑defined priority (ParticleComponent::sortOrder)
    uint   flags;         // bit 0‑2 = RenderPass, bit 8 = ySort
};

// ---- GPU buffer bindings ----
StructuredBuffer<GPUParticle> Particles    : register(t0, space0);
StructuredBuffer<uint>        DrawArgs     : register(t1, space0);
RWStructuredBuffer<uint>      AliveIndices : register(u0, space1);

// ---- CPU‑pushed uniform ----
cbuffer BitonicSortParams : register(b0, space2)
{
    uint maxParticleCount;   // emitter capacity (≤ 256 in single‑pass path)
    uint pad0;
    uint pad1;
    uint pad2;
};

// ---- Sentinel value for unused slots ----
// The global particle pool's maximum index is MAX_PARTICLES-1 = 65535, so
// 0xFFFFFFFF is always out‑of‑range and acts as a “greater than everything”
// sentinel during sorting.
static const uint SENTINEL = 0xFFFFFFFFu;

// ═════════════════════════════════════════════════════════════════════════════
// SortKey — cached per‑element comparison metadata, stored in groupshared
// memory so that the bitonic compare‑swap loop never re‑reads the (96‑byte)
// GPUParticle struct from device memory.
// ═════════════════════════════════════════════════════════════════════════════
struct SortKey
{
    uint  idx;       // particle index in Particles[]; SENTINEL for padding
    uint  layer;     // draw layer (ascending)
    uint  flags;     // bit 8 = ySort (mirrors GPUParticle.flags)
    int   sortKey;   // user sort order (ascending)
    float y;         // world‑space y position (used when ySort == true)
};

// ═════════════════════════════════════════════════════════════════════════════
// LessSortKey — total order matching particle_sort.comp.hlsl :: Less()
//
// Ordering precedence (ascending ≡ smaller values render first / back‑to‑front):
//   1. layer        — lower layer renders first
//   2. ySort flag   — non‑ySort elements render before ySort elements
//   3. y position   — when ySort == true, lower y renders first
//   4. sortKey      — lower value renders first
//   5. particleIdx  — stable tie‑breaker
//
// Returns true if 'a' should appear before 'b' in the sorted output.
// ═════════════════════════════════════════════════════════════════════════════
bool LessSortKey(SortKey a, SortKey b)
{
    // Sentinel always sorts last.
    if (a.idx == SENTINEL) return false;  // a is sentinel → a > everything
    if (b.idx == SENTINEL) return true;   // b is sentinel → b > a

    // 1. layer
    if (a.layer != b.layer)
        return a.layer < b.layer;

    // 2. ySort flag
    bool aySort = (a.flags & (1u << 8)) != 0u;
    bool bySort = (b.flags & (1u << 8)) != 0u;
    if (aySort != bySort)
        return !aySort;          // non‑ySort first

    // 3. y position (only meaningful when ySort is true)
    if (aySort && a.y != b.y)
        return a.y < b.y;

    // 4. user sort key
    if (a.sortKey != b.sortKey)
        return a.sortKey < b.sortKey;

    // 5. stable tie‑break by particle index
    return a.idx < b.idx;
}

// ═════════════════════════════════════════════════════════════════════════════
// Shared memory: one SortKey per thread in the workgroup.
// Max size = 256 × 20 bytes = 5120 bytes (well within common 32 KB limit).
// ═════════════════════════════════════════════════════════════════════════════
groupshared SortKey g_Keys[256];

// ═════════════════════════════════════════════════════════════════════════════
// main — parallel bitonic merge sort, single dispatch
//
// numthreads(256, 1, 1): one thread per potential particle slot.
// Threads whose tid ≥ aliveCount operate on sentinel keys and still execute
// the full barrier loop so barrier uniformity is not violated.
// ═════════════════════════════════════════════════════════════════════════════
[numthreads(256, 1, 1)]
void main(uint tid : SV_GroupThreadID)
{
    // ── Phase A: load alive indices into shared memory ──────────────────────
    uint aliveCount = DrawArgs[1];          // written by update compute

    SortKey key;
    if (tid < aliveCount && tid < maxParticleCount)
    {
        uint idx = AliveIndices[tid];

        // Read the GPUParticle once to cache comparison metadata.
        GPUParticle p = Particles[idx];

        key.idx     = idx;
        key.layer   = p.layer;
        key.flags   = p.flags;
        key.sortKey = p.sortKey;
        key.y       = p.posLife.y;
    }
    else
    {
        // Pad with sentinel so unused slots naturally settle at the tail.
        key.idx     = SENTINEL;
        key.layer   = 0u;
        key.flags   = 0u;
        key.sortKey = 0;
        key.y       = 0.0f;
    }

    g_Keys[tid] = key;
    GroupMemoryBarrierWithGroupSync();

    // ── Phase B: parallel bitonic sort (n = 256) ────────────────────────────
    //
    // Classic bitonic merge network — each thread compares and conditionally
    // swaps its element with the element at index (tid ^ stride).
    //
    // Outer loop k  = size of the bitonic block being merged (2, 4, 8, … 256).
    // Inner loop j  = comparison stride (k/2, k/4, …, 1).
    //
    // (tid & k) determines the sort direction for this thread’s position:
    //   (tid & k) == 0  →  ascending  (smaller values stay at lower indices)
    //   (tid & k) != 0  →  descending (larger values stay at lower indices)
    // This alternating‑direction property is what gives bitonic sort its
    // O(log² n) depth.

    uint n = 256u;    // hard‑wired workgroup size
    for (uint k = 2u; k <= n; k <<= 1u)
    {
        for (uint j = k >> 1u; j > 0u; j >>= 1u)
        {
            uint partner = tid ^ j;

            // Only the thread with the lower index executes the swap so that
            // each pair is processed exactly once (no data race).
            if (partner > tid)
            {
                // Read both keys from shared memory.
                SortKey mine  = g_Keys[tid];
                SortKey other = g_Keys[partner];

                // Sort direction: ascending when (tid & k) == 0.
                bool ascending = ((tid & k) == 0u);

                bool shouldSwap;
                if (ascending)
                {
                    // Ascending block → smaller element should be at index tid.
                    // Swap if other < mine (i.e. mine should not be first).
                    shouldSwap = LessSortKey(other, mine);
                }
                else
                {
                    // Descending block → larger element should be at index tid.
                    // Swap if mine < other (i.e. mine should not be first).
                    shouldSwap = LessSortKey(mine, other);
                }

                if (shouldSwap)
                {
                    g_Keys[tid]    = other;
                    g_Keys[partner] = mine;
                }
            }

            // All threads must reach this barrier before the next stride.
            // This guarantees that writes from the previous phase are visible
            // to all threads in the workgroup.
            GroupMemoryBarrierWithGroupSync();
        }
    }

    // ── Phase C: write sorted indices back ──────────────────────────────────
    if (tid < aliveCount && tid < maxParticleCount)
    {
        AliveIndices[tid] = g_Keys[tid].idx;
    }
}
