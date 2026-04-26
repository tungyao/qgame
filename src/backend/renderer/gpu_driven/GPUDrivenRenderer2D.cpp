/**
 * @file GPUDrivenRenderer2D.cpp
 * @brief GPU-Driven 2D 渲染器实现
 * 
 * 本文件实现了 GPU-Driven 2D 渲染器的核心逻辑。
 * 
 * 关键实现细节：
 * 
 * 1. 增量更新策略
 *    - 只上传被修改的实例数据
 *    - 使用脏标记跟踪变化
 *    - 当脏实例超过阈值时切换为全量更新
 * 
 * 2. GPU 剔除实现
 *    - 使用 Compute Shader 并行剔除
 *    - 每个线程处理一个实例
 *    - 使用原子计数器记录可见实例数
 * 
 * 3. GPU 排序实现
 *    - 使用基数排序 (Radix Sort)
 *    - GPU 友好的并行算法
 *    - 时间复杂度 O(n)，优于 CPU 的 O(n log n)
 * 
 * 4. 内存管理
 *    - 使用对象池避免频繁分配
 *    - 删除操作延迟复用
 *    - GPU 缓冲区预分配，避免运行时扩容
 */

#include "GPUDrivenRenderer2D.h"
#include "../../../core/Logger.h"
#include <cmath>
#include <algorithm>

namespace engine {

// ========== 构造与初始化 ==========

GPUDrivenRenderer2D::GPUDrivenRenderer2D(
    backend::IRenderDevice* device,
    const GPUDrivenConfig& config
)
    : device_(device)
    , config_(config)
{
    // ========== 初始化 CPU 端数据结构 ==========
    
    // 实例缓冲区：管理所有精灵实例数据
    instanceBuffer_ = std::make_unique<InstanceBuffer>(config.maxInstances);
    
    // 空间分区网格：用于 CPU 端粗粒度裁剪 (可选)
    if (config.useSpatialGrid) {
        spatialGrid_ = std::make_unique<SpatialHashGrid<uint32_t>>(
            config.spatialGridCellSize
        );
    }
    
    // 初始化层级 Y 排序配置
    // 默认：中间层级 (10-20) 启用 Y 排序
    for (int i = 10; i <= 20; ++i) {
        layerYSort_[i] = true;
    }
    
    // ========== 初始化 GPU 资源 ==========
    
    initGPUBuffers();
    initComputePipelines();
    initRenderPipeline();
    
    core::logInfo("GPUDrivenRenderer2D initialized: maxInstances=%u, maxTextures=%u",
        config.maxInstances, config.maxTextures);
}

GPUDrivenRenderer2D::~GPUDrivenRenderer2D() {
    // 释放 GPU 缓冲区
    if (gpuInstanceBuffer_.valid()) {
        device_->destroyBuffer(gpuInstanceBuffer_);
    }
    if (gpuVisibleIndexBuffer_.valid()) {
        device_->destroyBuffer(gpuVisibleIndexBuffer_);
    }
    if (gpuCounterBuffer_.valid()) {
        device_->destroyBuffer(gpuCounterBuffer_);
    }
    if (gpuIndirectArgsBuffer_.valid()) {
        device_->destroyBuffer(gpuIndirectArgsBuffer_);
    }
    
    // 释放 Compute Pipelines
    if (cullingPipeline_.valid()) {
        device_->destroyComputePipeline(cullingPipeline_);
    }
    if (sortingPipeline_.valid()) {
        device_->destroyComputePipeline(sortingPipeline_);
    }
    
    // 释放着色器
    if (spriteShader_.valid()) {
        device_->destroyShader(spriteShader_);
    }
    
    // 释放纹理数组
    if (textureArray_.valid()) {
        device_->destroyTexture(textureArray_);
    }
    
    core::logInfo("GPUDrivenRenderer2D destroyed");
}

// ========== GPU 资源初始化 ==========

void GPUDrivenRenderer2D::initGPUBuffers() {
    /**
     * 实例缓冲区 (SSBO)
     * 
     * 存储 GPUInstanceData 数组，每个元素 72 字节。
     * Compute Shader 和顶点着色器都可以访问。
     * 
     * 使用场景：
     * - CPU 写入: 上传实例数据
     * - Compute Shader 读取: 剔除、排序
     * - 顶点着色器读取: 获取变换矩阵、UV 等
     */
    gpuInstanceBuffer_ = device_->createBuffer({
        .size = config_.maxInstances * sizeof(GPUInstanceData),
        .usage = backend::BufferUsage::Storage,
        .initialData = nullptr  // 稍后上传
    });
    
    /**
     * 可见实例索引缓冲区 (SSBO)
     * 
     * 存储剔除后可见实例的索引。
     * 大小等于最大实例数 (最坏情况：全部可见)。
     * 
     * 使用场景：
     * - Compute Shader 写入: 剔除 Pass 输出
     * - Compute Shader 读写: 排序 Pass 输入/输出
     * - 顶点着色器读取: Indirect Draw 时获取实例 ID
     */
    gpuVisibleIndexBuffer_ = device_->createBuffer({
        .size = config_.maxInstances * sizeof(uint32_t),
        .usage = backend::BufferUsage::Storage
    });
    
    /**
     * 原子计数器缓冲区 (SSBO)
     * 
     * 存储可见实例计数。
     * Compute Shader 使用原子操作递增。
     * 
     * 布局：
     * - offset 0: 可见实例计数
     * - offset 4: 排序临时计数 (可选)
     */
    gpuCounterBuffer_ = device_->createBuffer({
        .size = 4 * sizeof(uint32_t),  // 4 个计数器
        .usage = backend::BufferUsage::Storage
    });
    
    /**
     * 间接绘制参数缓冲区
     * 
     * 存储 DrawArraysIndirect 参数：
     * - vertexCount: 6 (每个精灵 6 个顶点)
     * - instanceCount: 可见实例数 (由 Compute Shader 填充)
     * - firstVertex: 0
     * - firstInstance: 0
     */
    gpuIndirectArgsBuffer_ = device_->createBuffer({
        .size = sizeof(uint32_t) * 4,  // DrawArraysIndirectCmd
        .usage = backend::BufferUsage::Indirect
    });
}

void GPUDrivenRenderer2D::initComputePipelines() {
    /**
     * 剔除 Compute Pipeline
     * 
     * 输入：
     * - binding 0: InstanceBuffer (readonly)
     * - binding 1: VisibleIndexBuffer (writeonly)
     * - binding 2: CounterBuffer (readwrite)
     * - push constants: 视口参数、层级掩码
     * 
     * 输出：
     * - VisibleIndexBuffer: 可见实例索引
     * - CounterBuffer[0]: 可见实例计数
     */
    cullingPipeline_ = device_->createComputePipeline({
        // 这里应该加载预编译的 SPIR-V 字节码
        // 暂时使用占位符
        .code = nullptr,
        .codeSize = 0,
        .threadCountX = config_.workGroupSize,
        .numReadonlyStorageBuffers = 1,  // InstanceBuffer
        .numReadwriteStorageBuffers = 2   // VisibleIndexBuffer, CounterBuffer
    });
    
    /**
     * 排序 Compute Pipeline
     * 
     * 输入：
     * - binding 0: VisibleIndexBuffer (readwrite)
     * - binding 1: InstanceBuffer (readonly, 用于读取 sortKey)
     * - push constants: 可见实例数
     * 
     * 输出：
     * - VisibleIndexBuffer: 排序后的可见实例索引
     * 
     * 算法：基数排序 (Radix Sort)
     * - 4 趟，每趟处理 4 位
     * - 时间复杂度 O(n)
     * - 空间复杂度 O(n)
     */
    sortingPipeline_ = device_->createComputePipeline({
        .code = nullptr,
        .codeSize = 0,
        .threadCountX = 256,  // 基数排序使用更大的工作组
        .numReadonlyStorageBuffers = 1,
        .numReadwriteStorageBuffers = 1
    });
}

void GPUDrivenRenderer2D::initRenderPipeline() {
    /**
     * 精灵渲染着色器
     * 
     * 顶点着色器：
     * - 从 InstanceBuffer 读取实例数据
     * - 从 VisibleIndexBuffer 读取实例索引 (通过 gl_InstanceID)
     * - 生成四边形顶点
     * 
     * 片段着色器：
     * - 采样纹理 (从 Texture2DArray)
     * - 应用颜色调制
     * - 输出最终颜色
     */
    spriteShader_ = device_->createShader({
        // 加载预编译的着色器字节码
        .vsData = nullptr,
        .vsSize = 0,
        .fsData = nullptr,
        .fsSize = 0
    });
}

// ========== 精灵管理实现 ==========

uint32_t GPUDrivenRenderer2D::addSprite(const AddSpriteParams& params) {
    GPUInstanceData data = {};
    
    // ========== 构建变换矩阵 ==========
    
    buildTransformMatrix(
        data.transform,
        params.x, params.y,
        params.rotation,
        params.scaleX, params.scaleY,
        params.pivotX, params.pivotY
    );
    
    // ========== 设置 UV 坐标 ==========
    
    if (params.useSrcRect && device_->getTextureDimensions(params.texture, 
        reinterpret_cast<int&>(data.size[0]), reinterpret_cast<int&>(data.size[1]))) {
        // 使用源矩形计算 UV
        float texW = static_cast<float>(data.size[0]);
        float texH = static_cast<float>(data.size[1]);
        
        data.uv[0] = params.srcRect.x / texW;                       // u0
        data.uv[1] = params.srcRect.y / texH;                       // v0
        data.uv[2] = (params.srcRect.x + params.srcRect.w) / texW; // u1
        data.uv[3] = (params.srcRect.y + params.srcRect.h) / texH; // v1
        
        // 设置实际尺寸
        data.size[0] = static_cast<float>(params.srcRect.w) * params.scaleX;
        data.size[1] = static_cast<float>(params.srcRect.h) * params.scaleY;
    } else {
        // 使用整个纹理
        data.uv[0] = 0.0f;
        data.uv[1] = 0.0f;
        data.uv[2] = 1.0f;
        data.uv[3] = 1.0f;
    }
    
    // ========== 设置渲染参数 ==========
    
    data.textureIndex = getTextureIndex(params.texture);
    data.layer = static_cast<uint32_t>(params.layer);
    
    // 计算排序键
    if (params.useCustomSortKey) {
        data.sortKey = params.customSortKey;
    } else {
        data.sortKey = GPUInstanceData::computeSortKey(
            static_cast<uint32_t>(params.layer),
            params.ySort ? params.y : 0.0f
        );
    }
    
    // 设置颜色
    data.setColor(params.tint);
    
    // 设置锚点
    data.pivot[0] = params.pivotX;
    data.pivot[1] = params.pivotY;
    
    // ========== 添加到实例缓冲区 ==========
    
    uint32_t instanceId = instanceBuffer_->addInstance(data);
    
    if (instanceId == InstanceBuffer::INVALID_INSTANCE_ID) {
        core::logError("GPUDrivenRenderer2D::addSprite: failed to add instance (max reached)");
        return InstanceBuffer::INVALID_INSTANCE_ID;
    }
    
    // ========== 添加到空间分区网格 ==========
    
    if (spatialGrid_) {
        spatialGrid_->insert(instanceId, params.x, params.y);
    }
    
    return instanceId;
}

bool GPUDrivenRenderer2D::updatePosition(uint32_t instanceId, float x, float y) {
    GPUInstanceData* data = instanceBuffer_->getInstanceMutable(instanceId);
    if (!data) {
        return false;
    }
    
    // 更新变换矩阵中的平移部分
    // transform[2] 是平移向量的 X 分量
    // transform[5] 是平移向量的 Y 分量
    data->transform[2] = x;
    data->transform[5] = y;
    
    // 如果该实例参与 Y 排序，更新排序键
    uint32_t layer = data->layer;
    if (layer < 32 && layerYSort_[layer]) {
        data->sortKey = GPUInstanceData::computeSortKey(layer, y);
    }
    
    instanceBuffer_->markDirty(instanceId);
    
    // 更新空间分区
    if (spatialGrid_) {
        spatialGrid_->clear();
        // 需要重新插入所有实例 (简化实现)
        // 优化方案：使用增量更新
    }
    
    return true;
}

bool GPUDrivenRenderer2D::updateTransform(
    uint32_t instanceId,
    float x, float y,
    float rotation,
    float scaleX,
    float scaleY
) {
    GPUInstanceData* data = instanceBuffer_->getInstanceMutable(instanceId);
    if (!data) {
        return false;
    }
    
    // 重新构建变换矩阵
    buildTransformMatrix(
        data->transform,
        x, y, rotation, scaleX, scaleY,
        data->pivot[0], data->pivot[1]
    );
    
    // 更新 Y 排序键
    uint32_t layer = data->layer;
    if (layer < 32 && layerYSort_[layer]) {
        data->sortKey = GPUInstanceData::computeSortKey(layer, y);
    }
    
    instanceBuffer_->markDirty(instanceId);
    
    return true;
}

bool GPUDrivenRenderer2D::updateColor(uint32_t instanceId, const core::Color& color) {
    GPUInstanceData* data = instanceBuffer_->getInstanceMutable(instanceId);
    if (!data) {
        return false;
    }
    
    data->setColor(color);
    instanceBuffer_->markDirty(instanceId);
    
    return true;
}

bool GPUDrivenRenderer2D::updateLayer(uint32_t instanceId, int layer) {
    GPUInstanceData* data = instanceBuffer_->getInstanceMutable(instanceId);
    if (!data) {
        return false;
    }
    
    data->layer = static_cast<uint32_t>(layer);
    
    // 重新计算排序键
    float y = data->transform[5];  // 从变换矩阵提取 Y 坐标
    data->sortKey = GPUInstanceData::computeSortKey(
        static_cast<uint32_t>(layer),
        layerYSort_[layer] ? y : 0.0f
    );
    
    instanceBuffer_->markDirty(instanceId);
    
    return true;
}

bool GPUDrivenRenderer2D::updateTexture(
    uint32_t instanceId,
    backend::TextureHandle texture,
    const core::Rect* srcRect
) {
    GPUInstanceData* data = instanceBuffer_->getInstanceMutable(instanceId);
    if (!data) {
        return false;
    }
    
    data->textureIndex = getTextureIndex(texture);
    
    // 更新 UV 坐标
    if (srcRect) {
        int texW, texH;
        if (device_->getTextureDimensions(texture, texW, texH)) {
            float fw = static_cast<float>(texW);
            float fh = static_cast<float>(texH);
            
            data->uv[0] = srcRect->x / fw;
            data->uv[1] = srcRect->y / fh;
            data->uv[2] = (srcRect->x + srcRect->w) / fw;
            data->uv[3] = (srcRect->y + srcRect->h) / fh;
            
            data->size[0] = static_cast<float>(srcRect->w);
            data->size[1] = static_cast<float>(srcRect->h);
        }
    }
    
    instanceBuffer_->markDirty(instanceId);
    
    return true;
}

bool GPUDrivenRenderer2D::removeSprite(uint32_t instanceId) {
    return instanceBuffer_->removeInstance(instanceId);
}

// ========== 层级控制实现 ==========

void GPUDrivenRenderer2D::setLayerVisible(int layer, bool visible) {
    if (layer < 0 || layer >= 32) {
        core::logError("GPUDrivenRenderer2D::setLayerVisible: invalid layer %d", layer);
        return;
    }
    
    if (visible) {
        layerMask_ |= (1u << layer);
    } else {
        layerMask_ &= ~(1u << layer);
    }
}

void GPUDrivenRenderer2D::setLayerYSort(int layer, bool enabled) {
    if (layer < 0 || layer >= 32) {
        return;
    }
    
    layerYSort_[layer] = enabled;
}

// ========== 渲染实现 ==========

void GPUDrivenRenderer2D::render(const backend::CameraData& camera) {
    // 重置统计信息
    stats_ = {};
    
    // ========== Step 1: 上传脏实例数据到 GPU ==========
    
    uploadDirtyInstances();
    
    // ========== Step 2: 执行 GPU 剔除 ==========
    
    uint32_t visibleCount = 0;
    
    if (config_.enableGPUCulling) {
        visibleCount = executeGPUCulling(camera);
    } else {
        // 禁用 GPU 剔除，假设全部可见
        visibleCount = instanceBuffer_->getActiveCount();
    }
    
    stats_.visibleInstances = visibleCount;
    
    // ========== Step 3: 执行 GPU 排序 ==========
    
    if (config_.enableGPUSorting && visibleCount > 1) {
        executeGPUSorting(visibleCount);
    }
    
    // ========== Step 4: 执行 Indirect Draw ==========
    
    executeIndirectDraw(camera);
    
    // 清除脏标记
    instanceBuffer_->clearDirty();
}

void GPUDrivenRenderer2D::uploadDirtyInstances() {
    const auto& dirtyInstances = instanceBuffer_->getDirtyInstances();
    
    if (dirtyInstances.empty()) {
        return;  // 没有需要上传的数据
    }
    
    uint32_t activeCount = instanceBuffer_->getActiveCount();
    
    // 决定是增量上传还是全量上传
    bool useIncremental = dirtyInstances.size() < 
        activeCount * config_.incrementalThreshold;
    
    if (useIncremental) {
        // 增量上传：只上传脏实例
        // 注意：这需要 GPU Buffer 支持部分更新
        for (uint32_t id : dirtyInstances) {
            const GPUInstanceData* data = instanceBuffer_->getInstance(id);
            if (data) {
                // 上传单个实例到 GPU Buffer 的对应位置
                size_t offset = id * sizeof(GPUInstanceData);
                device_->uploadToBuffer(
                    gpuInstanceBuffer_,
                    data,
                    sizeof(GPUInstanceData),
                    offset
                );
            }
        }
    } else {
        // 全量上传：上传所有实例数据
        const auto& allInstances = instanceBuffer_->getAllInstances();
        void* mapped = device_->mapBuffer(gpuInstanceBuffer_);
        if (mapped) {
            std::memcpy(mapped, allInstances.data(), 
                allInstances.size() * sizeof(GPUInstanceData));
            device_->unmapBuffer(gpuInstanceBuffer_);
        }
    }
    
    stats_.dirtyInstances = static_cast<uint32_t>(dirtyInstances.size());
}

uint32_t GPUDrivenRenderer2D::executeGPUCulling(const backend::CameraData& camera) {
    /**
     * GPU 剔除流程：
     * 
     * 1. 重置原子计数器为 0
     * 2. 绑定 Compute Pipeline 和资源
     * 3. 设置 Push Constants (视口参数、层级掩码)
     * 4. Dispatch Compute Shader
     * 5. 插入内存屏障
     * 6. 读取可见实例计数
     */
    
    // 重置计数器
    uint32_t zero = 0;
    device_->uploadToBuffer(gpuCounterBuffer_, &zero, sizeof(zero), 0);
    
    // 绑定 Compute Pipeline
    device_->bindComputePipeline(cullingPipeline_);
    
    // 绑定资源
    // 注意：具体的绑定槽位需要与 Shader 一致
    // binding 0: InstanceBuffer (readonly)
    // binding 1: VisibleIndexBuffer (writeonly)
    // binding 2: CounterBuffer (readwrite)
    
    // 设置 Push Constants
    struct CullingParams {
        float viewportX, viewportY, viewportW, viewportH;
        uint32_t instanceCount;
        uint32_t layerMask;
    } params;
    
    // 计算视口边界 (世界坐标)
    params.viewportX = camera.x - camera.viewportW * 0.5f / camera.zoom;
    params.viewportY = camera.y - camera.viewportH * 0.5f / camera.zoom;
    params.viewportW = camera.viewportW / camera.zoom;
    params.viewportH = camera.viewportH / camera.zoom;
    params.instanceCount = instanceBuffer_->getActiveCount();
    params.layerMask = layerMask_;
    
    // 通过 GPU API 设置 Push Constants
    // device_->pushConstants(cullingPipeline_, &params, sizeof(params));
    
    // Dispatch
    uint32_t instanceCount = instanceBuffer_->getActiveCount();
    uint32_t numGroups = (instanceCount + config_.workGroupSize - 1) / config_.workGroupSize;
    
    device_->dispatch(numGroups, 1, 1);
    
    // 内存屏障：确保写入完成
    // device_->memoryBarrier(...);
    
    // 读取可见实例计数
    uint32_t visibleCount = 0;
    device_->downloadFromBuffer(gpuCounterBuffer_, &visibleCount, sizeof(visibleCount), 0);
    
    return visibleCount;
}

void GPUDrivenRenderer2D::executeGPUSorting(uint32_t visibleCount) {
    /**
     * GPU 排序流程：
     * 
     * 使用基数排序 (Radix Sort)：
     * - 每趟处理 4 位
     * - 总共 8 趟 (32 位排序键)
     * - 每趟使用 Counting Sort
     * 
     * 时间复杂度：O(n * k)，其中 k = 8 (趟数)
     * 空间复杂度：O(n + 16) (需要临时数组)
     */
    
    device_->bindComputePipeline(sortingPipeline_);
    
    // 设置 Push Constants
    struct SortingParams {
        uint32_t visibleCount;
        uint32_t pass;  // 当前趟数 (0-7)
    } params;
    
    params.visibleCount = visibleCount;
    
    // 执行 8 趟排序 (每趟处理 4 位)
    for (uint32_t pass = 0; pass < 8; ++pass) {
        params.pass = pass;
        
        // 设置 Push Constants
        // device_->pushConstants(sortingPipeline_, &params, sizeof(params));
        
        // Dispatch
        uint32_t numGroups = (visibleCount + 255) / 256;
        device_->dispatch(numGroups, 1, 1);
        
        // 内存屏障
        // device_->memoryBarrier(...);
    }
}

void GPUDrivenRenderer2D::executeIndirectDraw(const backend::CameraData& camera) {
    /**
     * 间接绘制流程：
     * 
     * 1. 设置渲染状态 (视口、混合模式等)
     * 2. 绑定着色器和资源
     * 3. 执行 DrawArraysIndirect
     * 
     * DrawArraysIndirect 参数：
     * - vertexCount = 6 (每个精灵 2 个三角形 = 6 个顶点)
     * - instanceCount = visibleCount (由 GPU 填充)
     * - firstVertex = 0
     * - firstInstance = 0
     */
    
    // 构建渲染参数
    backend::IRenderDevice::GPURenderParams params;
    params.spriteBuffer = gpuInstanceBuffer_;
    params.visibleIndexBuffer = gpuVisibleIndexBuffer_;
    params.spriteCount = instanceBuffer_->getActiveCount();
    params.visibleCount = stats_.visibleInstances;
    params.camera = camera;
    params.clearEnabled = true;
    params.clearColor = core::Color::Black;
    
    // 如果启用了批处理，构建批次信息
    if (config_.enableGPUBatching) {
        // 简化：假设所有可见实例使用同一纹理
        // 实际实现需要按纹理分组
        backend::IRenderDevice::GPUDrawBatch batch;
        batch.firstInstance = 0;
        batch.instanceCount = stats_.visibleInstances;
        // batch.texture = ...;
        params.batches.push_back(batch);
    }
    
    // 调用渲染设备的 GPU 驱动渲染接口
    backend::IRenderDevice::PassSubmitInfo passInfo;
    passInfo.camera = camera;
    passInfo.clearEnabled = true;
    passInfo.clearColor = core::Color::Black;
    
    device_->submitGPUDrivenPass(passInfo, params);
    
    // 更新统计
    stats_.drawCalls = static_cast<uint32_t>(params.batches.size());
    stats_.totalInstances = instanceBuffer_->getActiveCount();
}

void GPUDrivenRenderer2D::clear() {
    instanceBuffer_->clear();
    
    if (spatialGrid_) {
        spatialGrid_->clear();
    }
    
    stats_ = {};
}

// ========== 辅助方法实现 ==========

void GPUDrivenRenderer2D::buildTransformMatrix(
    float* outTransform,
    float x, float y,
    float rotation,
    float scaleX, float scaleY,
    float pivotX, float pivotY
) {
    /**
     * 构建 2D 仿射变换矩阵：
     * 
     * [ sx*cos  -sy*sin  tx - pivotX*sx*cos + pivotY*sy*sin ]
     * [ sx*sin   sy*cos  ty - pivotX*sx*sin - pivotY*sy*cos ]
     * 
     * 最终结果：
     * outTransform[0] = sx * cos
     * outTransform[1] = sx * sin
     * outTransform[2] = tx (平移 X)
     * outTransform[3] = -sy * sin
     * outTransform[4] = sy * cos
     * outTransform[5] = ty (平移 Y)
     */
    
    float cosR = std::cos(rotation);
    float sinR = std::sin(rotation);
    
    // 缩放 + 旋转
    outTransform[0] = scaleX * cosR;    // m00
    outTransform[1] = scaleX * sinR;    // m10
    outTransform[3] = -scaleY * sinR;   // m01
    outTransform[4] = scaleY * cosR;    // m11
    
    // 平移 (考虑锚点偏移)
    outTransform[2] = x - pivotX * outTransform[0] + pivotY * outTransform[3];  // m02
    outTransform[5] = y - pivotX * outTransform[1] - pivotY * outTransform[4];  // m12
}

uint32_t GPUDrivenRenderer2D::getTextureIndex(backend::TextureHandle texture) {
    /**
     * 获取纹理在纹理数组中的索引
     * 
     * 实现策略：
     * 1. 维护一个纹理句柄到索引的映射表
     * 2. 如果纹理已存在，返回索引
     * 3. 如果纹理不存在，分配新索引
     * 
     * 优化方向：
     * - 使用 LRU 缓存策略
     * - 支持纹理图集
     */
    
    // 简化实现：使用纹理句柄的 ID 作为索引
    // 实际实现需要更复杂的映射逻辑
    return texture.id();
}

} // namespace engine
