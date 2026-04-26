/**
 * @file GPUDrivenRenderer2D.h
 * @brief GPU-Driven 2D 渲染器 - 将渲染工作从 CPU 转移到 GPU
 * 
 * 核心理念：
 * 
 * 传统 2D 渲染：
 *   CPU: 遍历精灵 → 视口裁剪 → 排序 → 批处理 → 提交 Draw Call
 *   问题: CPU 时间随精灵数量线性增长，Draw Call 多
 * 
 * GPU-Driven 2D 渲染：
 *   CPU: 更新实例数据 → 提交 Compute Dispatch → 提交 Indirect Draw
 *   GPU: 剔除 → 排序 → 批处理 → 绘制
 *   优势: CPU 时间恒定，Draw Call 极少，充分利用 GPU 并行能力
 * 
 * 架构流程：
 * 
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                    每帧渲染流程                              │
 *   ├─────────────────────────────────────────────────────────────┤
 *   │                                                              │
 *   │   Step 1: 更新实例数据 (CPU)                                 │
 *   │   ─────────────────────────                                  │
 *   │   - 增量更新变换矩阵、位置、颜色等                           │
 *   │   - 上传到 GPU Instance Buffer                               │
 *   │                                                              │
 *   │   Step 2: GPU 剔除 (Compute Shader)                          │
 *   │   ───────────────────────────                                │
 *   │   - 遍历所有实例                                             │
 *   │   - 检查是否在视口内                                         │
 *   │   - 检查是否在指定层级                                       │
 *   │   - 输出可见实例索引到 VisibleIndexBuffer                    │
 *   │                                                              │
 *   │   Step 3: GPU 排序 (Compute Shader)                          │
 *   │   ───────────────────────────                                │
 *   │   - 对可见实例按 sortKey 排序                                │
 *   │   - 使用基数排序 (Radix Sort)，GPU 友好                      │
 *   │   - 输出排序后的索引                                         │
 *   │                                                              │
 *   │   Step 4: GPU 批处理 (Compute Shader, 可选)                  │
 *   │   ──────────────────────────────────                         │
 *   │   - 按纹理分组                                               │
 *   │   - 生成 Indirect Draw 参数                                  │
 *   │                                                              │
 *   │   Step 5: 间接绘制 (GPU)                                     │
 *   │   ─────────────────────                                      │
 *   │   - 单次 DrawIndirect 调用                                   │
 *   │   - GPU 读取排序后的索引，从 InstanceBuffer 获取数据          │
 *   │   - 顶点着色器生成四边形                                     │
 *   │                                                              │
 *   └─────────────────────────────────────────────────────────────┘
 * 
 * 性能对比：
 * 
 *   场景: 10,000 精灵
 *   传统:  CPU 5ms, 100+ Draw Calls
 *   本架构: CPU 0.1ms, 1-10 Draw Calls
 * 
 * 使用示例：
 * 
 * @code
 *   // 初始化
 *   GPUDrivenRenderer2D renderer(device);
 *   
 *   // 添加精灵
 *   uint32_t id1 = renderer.addSprite({
 *       .texture = textureAtlas,
 *       .x = 100, .y = 200,
 *       .layer = 5
 *   });
 *   
 *   // 更新位置 (每帧)
 *   renderer.updatePosition(id1, newX, newY);
 *   
 *   // 渲染
 *   renderer.render(camera);
 * @endcode
 * 
 * 兼容性：
 * - 支持 OpenGL 4.3+ (Compute Shader, SSBO)
 * - 支持 Vulkan 1.0+ (Compute Pipeline, Storage Buffer)
 * - 支持 Direct3D 11/12 (Compute Shader, Structured Buffer)
 */

#pragma once

#include <vector>
#include <memory>
#include "../IRenderDevice.h"
#include "InstanceBuffer.h"
#include "SpatialHashGrid.h"

namespace engine {

/**
 * @brief 添加精灵时的参数
 */
struct AddSpriteParams {
    // ========== 必需参数 ==========
    
    backend::TextureHandle texture;   // 纹理句柄
    
    float x = 0.0f;                   // 世界坐标 X
    float y = 0.0f;                   // 世界坐标 Y
    
    // ========== 可选参数 (有默认值) ==========
    
    float rotation = 0.0f;            // 旋转角度 (弧度)
    float scaleX = 1.0f;              // X 缩放
    float scaleY = 1.0f;              // Y 缩放
    
    float pivotX = 0.5f;              // 锚点 X (0-1)
    float pivotY = 0.5f;              // 锚点 Y (0-1)
    
    int layer = 0;                    // 渲染层 (0-31)
    bool ySort = false;               // 是否参与 Y 排序
    
    core::Color tint = core::Color::White;  // 颜色调制
    
    // ========== 纹理坐标 ==========
    
    /**
     * @brief 源矩形 (纹理像素坐标)
     * 
     * 如果不设置，默认使用整个纹理。
     * 使用纹理图集时必须设置。
     */
    core::Rect srcRect;
    bool useSrcRect = false;          // 是否使用源矩形
    
    // ========== 高级参数 ==========
    
    /**
     * @brief 自定义排序键
     * 
     * 如果设置，会覆盖自动计算的排序键。
     * 用于自定义渲染顺序。
     */
    uint32_t customSortKey = 0;
    bool useCustomSortKey = false;
};

/**
 * @brief GPU-Driven 2D 渲染器配置
 */
struct GPUDrivenConfig {
    // ========== 实例限制 ==========
    
    uint32_t maxInstances = 100000;   // 最大精灵实例数
    uint32_t maxTextures = 256;       // 纹理数组最大层数
    
    // ========== 空间分区 ==========
    
    float spatialGridCellSize = 64.0f;  // 空间哈希格子大小
    bool useSpatialGrid = true;         // 是否使用空间分区
    
    // ========== 渲染策略 ==========
    
    bool enableGPUCulling = true;     // 启用 GPU 剔除
    bool enableGPUSorting = true;      // 启用 GPU 排序
    bool enableGPUBatching = true;     // 启用 GPU 批处理
    
    // ========== 性能调优 ==========
    
    /**
     * @brief 工作组大小 (Compute Shader)
     * 
     * - 64: 适合大多数 GPU
     * - 128: 适合高端 GPU
     * - 256: 适合超大规模场景
     */
    uint32_t workGroupSize = 64;
    
    /**
     * @brief 增量更新阈值
     * 
     * 当脏实例数超过总实例数的这个比例时，
     * 切换为全量更新 (更高效)。
     */
    float incrementalThreshold = 0.5f;
};

/**
 * @brief GPU-Driven 2D 渲染器
 * 
 * 主要职责：
 * 1. 管理精灵实例的生命周期
 * 2. 管理 GPU 缓冲区 (Instance Buffer, Index Buffer 等)
 * 3. 执行 GPU Compute Pass (剔除、排序)
 * 4. 执行 Indirect Draw
 */
class GPUDrivenRenderer2D {
public:
    // ========== 构造与初始化 ==========
    
    /**
     * @brief 构造函数
     * @param device 渲染设备接口
     * @param config 配置参数
     */
    GPUDrivenRenderer2D(
        backend::IRenderDevice* device,
        const GPUDrivenConfig& config = GPUDrivenConfig{}
    );
    
    /**
     * @brief 析构函数
     * 
     * 释放所有 GPU 资源。
     */
    ~GPUDrivenRenderer2D();
    
    // 不允许拷贝
    GPUDrivenRenderer2D(const GPUDrivenRenderer2D&) = delete;
    GPUDrivenRenderer2D& operator=(const GPUDrivenRenderer2D&) = delete;
    
    // ========== 精灵管理 API ==========
    
    /**
     * @brief 添加一个精灵实例
     * @param params 精灵参数
     * @return 实例 ID (用于后续更新/删除)
     * 
     * 添加操作会：
     * 1. 在 CPU 端创建实例数据
     * 2. 标记为"脏"，等待上传到 GPU
     * 3. 如果启用空间分区，添加到 SpatialHashGrid
     */
    uint32_t addSprite(const AddSpriteParams& params);
    
    /**
     * @brief 更新精灵位置
     * @param instanceId 实例 ID
     * @param x 新的 X 坐标
     * @param y 新的 Y 坐标
     * @return 是否成功
     * 
     * 这是最常用的更新操作，用于移动精灵。
     * 如果精灵参与 Y 排序，会自动更新排序键。
     */
    bool updatePosition(uint32_t instanceId, float x, float y);
    
    /**
     * @brief 更新精灵变换
     * @param instanceId 实例 ID
     * @param x 新的 X 坐标
     * @param y 新的 Y 坐标
     * @param rotation 旋转角度 (弧度)
     * @param scaleX X 缩放
     * @param scaleY Y 缩放
     * @return 是否成功
     */
    bool updateTransform(
        uint32_t instanceId,
        float x, float y,
        float rotation = 0.0f,
        float scaleX = 1.0f,
        float scaleY = 1.0f
    );
    
    /**
     * @brief 更新精灵颜色
     * @param instanceId 实例 ID
     * @param color 新的颜色
     * @return 是否成功
     */
    bool updateColor(uint32_t instanceId, const core::Color& color);
    
    /**
     * @brief 更新精灵层级
     * @param instanceId 实例 ID
     * @param layer 新的层级
     * @return 是否成功
     */
    bool updateLayer(uint32_t instanceId, int layer);
    
    /**
     * @brief 更新精灵纹理
     * @param instanceId 实例 ID
     * @param texture 新的纹理句柄
     * @param srcRect 源矩形 (可选)
     * @return 是否成功
     */
    bool updateTexture(
        uint32_t instanceId,
        backend::TextureHandle texture,
        const core::Rect* srcRect = nullptr
    );
    
    /**
     * @brief 删除精灵实例
     * @param instanceId 实例 ID
     * @return 是否成功
     * 
     * 删除操作不会立即释放 GPU 内存，
     * 而是将槽位标记为空闲，等待复用。
     */
    bool removeSprite(uint32_t instanceId);
    
    // ========== 层级控制 API ==========
    
    /**
     * @brief 设置层级可见性
     * @param layer 渲染层 (0-31)
     * @param visible 是否可见
     */
    void setLayerVisible(int layer, bool visible);
    
    /**
     * @brief 设置层级 Y 排序
     * @param layer 渲染层 (0-31)
     * @param enabled 是否启用 Y 排序
     */
    void setLayerYSort(int layer, bool enabled);
    
    /**
     * @brief 获取层级掩码
     * @return 32 位掩码，每位对应一个层级
     */
    uint32_t getLayerMask() const { return layerMask_; }
    
    // ========== 渲染 API ==========
    
    /**
     * @brief 执行渲染
     * @param camera 相机参数
     * 
     * 渲染流程：
     * 1. 上传脏实例数据到 GPU
     * 2. 执行 GPU 剔除
     * 3. 执行 GPU 排序
     * 4. 执行 Indirect Draw
     */
    void render(const backend::CameraData& camera);
    
    /**
     * @brief 清除所有实例
     */
    void clear();
    
    // ========== 调试 API ==========
    
    /**
     * @brief 获取统计信息
     */
    struct Stats {
        uint32_t totalInstances;      // 总实例数
        uint32_t visibleInstances;    // 可见实例数
        uint32_t drawCalls;           // Draw Call 数
        uint32_t dirtyInstances;      // 脏实例数
        
        float cpuTimeMs;              // CPU 时间 (毫秒)
        float gpuTimeMs;              // GPU 时间 (毫秒)
    };
    
    const Stats& getStats() const { return stats_; }
    
    /**
     * @brief 启用/禁用调试模式
     * 
     * 调试模式下会：
     * - 输出详细的性能日志
     * - 验证所有输入参数
     * - 检查 GPU 错误
     */
    void setDebugMode(bool enabled) { debugMode_ = enabled; }

private:
    // ========== 内部初始化方法 ==========
    
    /**
     * @brief 初始化 GPU 缓冲区
     */
    void initGPUBuffers();
    
    /**
     * @brief 初始化 Compute Pipelines
     */
    void initComputePipelines();
    
    /**
     * @brief 初始化渲染管线
     */
    void initRenderPipeline();
    
    // ========== 内部渲染方法 ==========
    
    /**
     * @brief 上传脏实例数据到 GPU
     */
    void uploadDirtyInstances();
    
    /**
     * @brief 执行 GPU 剔除
     * @param camera 相机参数
     * @return 可见实例数
     */
    uint32_t executeGPUCulling(const backend::CameraData& camera);
    
    /**
     * @brief 执行 GPU 排序
     * @param visibleCount 可见实例数
     */
    void executeGPUSorting(uint32_t visibleCount);
    
    /**
     * @brief 执行 GPU 批处理 (可选)
     */
    void executeGPUBatching();
    
    /**
     * @brief 执行间接绘制
     */
    void executeIndirectDraw(const backend::CameraData& camera);
    
    // ========== 辅助方法 ==========
    
    /**
     * @brief 构建 2D 仿射变换矩阵
     */
    void buildTransformMatrix(
        float* outTransform,
        float x, float y,
        float rotation,
        float scaleX, float scaleY,
        float pivotX, float pivotY
    );
    
    /**
     * @brief 获取纹理索引
     * @param texture 纹理句柄
     * @return 纹理数组中的索引
     */
    uint32_t getTextureIndex(backend::TextureHandle texture);
    
    // ========== 成员变量 ==========
    
    backend::IRenderDevice* device_;   // 渲染设备
    GPUDrivenConfig config_;           // 配置
    
    // CPU 端数据
    std::unique_ptr<InstanceBuffer> instanceBuffer_;    // 实例缓冲区
    std::unique_ptr<SpatialHashGrid<uint32_t>> spatialGrid_;  // 空间分区
    
    // GPU 端缓冲区
    backend::BufferHandle gpuInstanceBuffer_;      // SSBO: 实例数据
    backend::BufferHandle gpuVisibleIndexBuffer_;  // SSBO: 可见实例索引
    backend::BufferHandle gpuCounterBuffer_;       // SSBO: 原子计数器
    backend::BufferHandle gpuIndirectArgsBuffer_;  // Indirect Draw 参数
    
    // Compute Pipelines
    backend::ComputePipelineHandle cullingPipeline_;   // 剔除管线
    backend::ComputePipelineHandle sortingPipeline_;   // 排序管线
    
    // 渲染管线
    backend::ShaderHandle spriteShader_;          // 精灵着色器
    backend::TextureHandle textureArray_;         // 纹理数组
    
    // 层级控制
    uint32_t layerMask_ = 0xFFFFFFFF;             // 层级可见性掩码
    bool layerYSort_[32] = {};                    // 层级 Y 排序开关
    
    // 统计信息
    Stats stats_;
    
    // 调试模式
    bool debugMode_ = false;
};

} // namespace engine
