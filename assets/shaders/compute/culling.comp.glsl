/**
 * @file culling.comp.glsl
 * @brief GPU 视口剔除 Compute Shader
 * 
 * 功能：
 * - 并行遍历所有精灵实例
 * - 检查每个实例是否在视口内
 * - 检查每个实例是否在可见层级
 * - 输出可见实例的索引
 * 
 * 性能：
 * - 每个线程处理一个实例
 * - 时间复杂度 O(n / threadCount)
 * - 完全并行，无同步开销
 * 
 * 使用方法：
 * glDispatchCompute(ceil(instanceCount / 64), 1, 1)
 * 
 * 注意事项：
 * - 需要绑定 InstanceBuffer (readonly)
 * - 需要绑定 VisibleIndexBuffer (writeonly)
 * - 需要绑定 CounterBuffer (readwrite)
 * - 需要设置 Push Constants
 */

#version 450
layout(local_size_x = 64) in;

/**
 * 实例数据结构
 * 必须与 CPU 端 GPUInstanceData 保持一致
 */
struct InstanceData {
    mat2x3 transform;     // 2D 仿射变换矩阵
    vec4 uv;              // UV 坐标
    uint textureIndex;    // 纹理索引
    uint layer;           // 渲染层
    uint sortKey;         // 排序键
    uint color;           // 颜色 (RGBA 打包)
    vec2 pivot;           // 锚点
    vec2 size;            // 精灵尺寸
};

// ========== 绑定资源 ==========

/**
 * 实例数据缓冲区 (readonly)
 * 包含所有精灵的实例数据
 */
layout(binding = 0) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

/**
 * 可见实例索引缓冲区 (writeonly)
 * 存储剔除后可见实例的索引
 */
layout(binding = 1) writeonly buffer VisibleIndexBuffer {
    uint visibleIndices[];
};

/**
 * 原子计数器缓冲区 (readwrite)
 * 用于统计可见实例数量
 */
layout(binding = 2) buffer CounterBuffer {
    uint visibleCount;
};

// ========== Push Constants ==========

/**
 * 剔除参数
 * 由 CPU 设置，包含视口信息和层级掩码
 */
layout(push_constant) uniform CullingParams {
    vec4 viewport;        // (x, y, width, height) 世界坐标
    uint instanceCount;   // 总实例数
    uint layerMask;       // 层级可见性掩码 (每位对应一个层级)
    uint padding[2];      // 对齐填充
} params;

// ========== 辅助函数 ==========

/**
 * 检查实例是否在视口内
 * 
 * 简化算法：
 * - 提取实例的世界坐标 (transform[2][0], transform[2][1])
 * - 检查是否在视口边界内
 * - 考虑精灵尺寸 (使用 size 字段)
 * 
 * 更精确的算法：
 * - 计算旋转后的包围盒
 * - 使用分离轴定理 (SAT) 检测相交
 */
bool isInViewport(InstanceData inst) {
    // 提取世界坐标 (变换矩阵的平移部分)
    float x = inst.transform[2][0];
    float y = inst.transform[2][1];
    
    // 使用精灵尺寸进行粗略检测
    float halfW = inst.size.x * 0.5;
    float halfH = inst.size.y * 0.5;
    
    // AABB 相交检测
    // 视口边界
    float vpLeft   = params.viewport.x;
    float vpTop    = params.viewport.y;
    float vpRight  = params.viewport.x + params.viewport.z;
    float vpBottom = params.viewport.y + params.viewport.w;
    
    // 精灵 AABB
    float spriteLeft   = x - halfW;
    float spriteTop    = y - halfH;
    float spriteRight  = x + halfW;
    float spriteBottom = y + halfH;
    
    // 检查是否相交
    return spriteRight >= vpLeft && spriteLeft <= vpRight &&
           spriteBottom >= vpTop && spriteTop <= vpBottom;
}

/**
 * 检查实例是否在可见层级
 */
bool isInVisibleLayer(uint layer) {
    // 使用位运算检查层级是否被启用
    return (params.layerMask & (1u << layer)) != 0u;
}

// ========== 主函数 ==========

void main() {
    // 获取当前线程处理的实例索引
    uint idx = gl_GlobalInvocationID.x;
    
    // 边界检查：确保索引在有效范围内
    if (idx >= params.instanceCount) {
        return;
    }
    
    // 读取实例数据
    InstanceData inst = instances[idx];
    
    // ========== 执行剔除测试 ==========
    
    // 1. 层级测试
    if (!isInVisibleLayer(inst.layer)) {
        return;  // 该层级被禁用，剔除
    }
    
    // 2. 视口测试
    if (!isInViewport(inst)) {
        return;  // 不在视口内，剔除
    }
    
    // ========== 通过测试，输出可见实例 ==========
    
    // 使用原子操作递增计数器，并获取当前槽位
    // atomicAdd 返回递增前的值，正好是我们需要的槽位索引
    uint slot = atomicAdd(visibleCount, 1);
    
    // 将可见实例的索引写入缓冲区
    visibleIndices[slot] = idx;
}

/**
 * 优化建议：
 * 
 * 1. Early-Z 剔除：
 *    - 如果启用了深度测试，可以在剔除阶段进行
 *    - 需要额外的深度缓冲区访问
 * 
 * 2. 遮挡剔除：
 *    - 使用 Hi-Z (Hierarchical Z) 进行遮挡剔除
 *    - 适合复杂场景，减少过度绘制
 * 
 * 3. 簇剔除：
 *    - 将实例按空间位置分组 (簇)
 *    - 先对簇进行粗略剔除，再对簇内实例精确剔除
 *    - 减少全局原子操作次数
 * 
 * 4. 异步计算：
 *    - 使用多个 Compute Queue 并行剔除
 *    - 适合超大规模场景 (100K+ 实例)
 */
