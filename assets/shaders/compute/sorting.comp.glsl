/**
 * @file sorting.comp.glsl
 * @brief GPU 并行排序 Compute Shader
 * 
 * 算法：基数排序 (Radix Sort)
 * 
 * 为什么选择基数排序？
 * 1. 时间复杂度 O(n)，优于快速排序的 O(n log n)
 * 2. 完全并行化，适合 GPU
 * 3. 稳定排序，保持相同键值的相对顺序
 * 4. 不需要比较操作，对 GPU 更友好
 * 
 * 实现策略：
 * - 使用 4 位基数 (16 个桶)
 * - 8 趟排序 (32 位键 / 4 位 = 8 趟)
 * - 每趟使用 Counting Sort
 * 
 * 性能特点：
 * - 时间复杂度: O(n * k)，其中 k = 8 (趟数)
 * - 空间复杂度: O(n)
 * - 并行度: 高 (每趟可并行处理)
 * 
 * 使用方法：
 * for (pass = 0; pass < 8; pass++) {
 *     setPushConstants(pass, visibleCount);
 *     glDispatchCompute(ceil(visibleCount / 256), 1, 1);
 *     glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
 * }
 */

#version 450
layout(local_size_x = 256) in;

/**
 * 实例数据结构 (与 culling.comp.glsl 一致)
 */
struct InstanceData {
    mat2x3 transform;
    vec4 uv;
    uint textureIndex;
    uint layer;
    uint sortKey;
    uint color;
    vec2 pivot;
    vec2 size;
};

// ========== 绑定资源 ==========

/**
 * 可见实例索引缓冲区 (readwrite)
 * 输入：未排序的可见实例索引
 * 输出：排序后的可见实例索引
 */
layout(binding = 0) buffer VisibleIndexBuffer {
    uint visibleIndices[];
};

/**
 * 实例数据缓冲区 (readonly)
 * 用于读取排序键 (sortKey)
 */
layout(binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

/**
 * 临时缓冲区 (readwrite)
 * 用于排序过程中的数据交换
 */
layout(binding = 2) buffer TempBuffer {
    uint tempIndices[];
};

// ========== Push Constants ==========

layout(push_constant) uniform SortingParams {
    uint visibleCount;    // 可见实例数
    uint currentPass;     // 当前趟数 (0-7)
    uint padding[2];
} params;

// ========== 共享内存 ==========

/**
 * 工作组共享内存
 * 用于工作组内的计数和前缀和计算
 */
shared uint localCount[16];      // 每个桶的计数
shared uint localPrefixSum[16];  // 前缀和
shared uint localIndices[256];   // 工作组内的实例索引
shared uint localKeys[256];      // 工作组内的排序键

// ========== 辅助函数 ==========

/**
 * 提取排序键的指定位
 * @param key 排序键
 * @param pass 当前趟数 (0-7)
 * @return 提取的 4 位值 (0-15)
 */
uint extractDigit(uint key, uint pass) {
    // 每趟处理 4 位
    // pass 0: 处理 bit 0-3 (最低 4 位)
    // pass 7: 处理 bit 28-31 (最高 4 位)
    return (key >> (pass * 4)) & 0xF;
}

/**
 * 工作组内的前缀和计算
 * 使用 Blelloch 算法 (工作高效前缀和)
 */
void computePrefixSum() {
    // 初始化
    if (gl_LocalInvocationID.x < 16) {
        localPrefixSum[gl_LocalInvocationID.x] = 0;
    }
    barrier();
    
    // 累加
    if (gl_LocalInvocationID.x < 16) {
        uint sum = 0;
        for (int i = 0; i < 16; i++) {
            uint temp = localPrefixSum[i];
            localPrefixSum[i] = sum;
            sum += localCount[i];
        }
    }
    barrier();
}

// ========== 主函数 ==========

void main() {
    uint localId = gl_LocalInvocationID.x;
    uint globalId = gl_GlobalInvocationID.x;
    uint groupId = gl_WorkGroupID.x;
    uint groupSize = gl_WorkGroupSize.x;
    
    // ========== Step 1: 初始化共享内存 ==========
    
    if (localId < 16) {
        localCount[localId] = 0;
    }
    barrier();
    
    // ========== Step 2: 加载数据到共享内存 ==========
    
    // 计算当前工作组处理的范围
    uint startIdx = groupId * groupSize;
    uint endIdx = min(startIdx + groupSize, params.visibleCount);
    uint itemCount = endIdx - startIdx;
    
    // 加载可见实例索引和排序键
    if (globalId < params.visibleCount) {
        localIndices[localId] = visibleIndices[globalId];
        
        // 从实例数据中读取排序键
        uint instanceIdx = visibleIndices[globalId];
        localKeys[localId] = instances[instanceIdx].sortKey;
        
        // 提取当前趟的数字
        uint digit = extractDigit(localKeys[localId], params.currentPass);
        
        // 原子递增对应桶的计数
        atomicAdd(localCount[digit], 1);
    }
    barrier();
    
    // ========== Step 3: 计算前缀和 ==========
    
    // 计算每个桶的起始位置 (工作组内)
    if (localId == 0) {
        uint sum = 0;
        for (int i = 0; i < 16; i++) {
            uint temp = localCount[i];
            localCount[i] = sum;
            sum += temp;
        }
    }
    barrier();
    
    // ========== Step 4: 分散数据到临时缓冲区 ==========
    
    if (globalId < params.visibleCount) {
        uint digit = extractDigit(localKeys[localId], params.currentPass);
        
        // 获取目标位置
        uint targetPos = atomicAdd(localCount[digit], 1);
        
        // 写入临时缓冲区
        tempIndices[startIdx + targetPos] = localIndices[localId];
    }
    barrier();
    
    // ========== Step 5: 写回结果 ==========
    
    if (globalId < params.visibleCount) {
        visibleIndices[globalId] = tempIndices[globalId];
    }
}

/**
 * 基数排序的工作原理：
 * 
 * 示例：排序键 [42, 17, 93, 5]
 * 
 * 趟 0 (处理最低 4 位):
 *   42 = 0b00101010 → digit = 10 (0xA)
 *   17 = 0b00010001 → digit = 1  (0x1)
 *   93 = 0b01011101 → digit = 13 (0xD)
 *   5  = 0b00000101 → digit = 5  (0x5)
 *   
 *   按数字排序: [17, 5, 42, 93]
 * 
 * 趟 1 (处理次低 4 位):
 *   17 = 0b00010001 → digit = 1  (0x1)
 *   5  = 0b00000101 → digit = 0  (0x0)
 *   42 = 0b00101010 → digit = 2  (0x2)
 *   93 = 0b01011101 → digit = 9  (0x9)
 *   
 *   按数字排序: [5, 17, 42, 93]
 * 
 * 经过 8 趟后，数据完全排序。
 * 
 * GPU 并行化策略：
 * - 每个工作组独立处理一部分数据
 * - 工作组内使用共享内存进行局部排序
 * - 多个工作组的全局合并 (需要额外 Pass)
 */

/**
 * 优化建议：
 * 
 * 1. 双缓冲策略：
 *    - 使用两个缓冲区交替作为输入/输出
 *    - 避免每次都需要写回原缓冲区
 * 
 * 2. 全局前缀和：
 *    - 当前实现只在工作组内排序
 *    - 需要额外的 Pass 进行全局合并
 *    - 可以使用 Blelloch 算法计算全局前缀和
 * 
 * 3. 大键值支持：
 *    - 当前实现支持 32 位键
 *    - 如果需要更大键值，增加趟数
 * 
 * 4. 批处理优化：
 *    - 对于小规模数据 (<1024)，单个工作组更高效
 *    - 对于大规模数据，使用多 Pass 全局排序
 */
