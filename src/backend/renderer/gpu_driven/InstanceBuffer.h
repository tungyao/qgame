/**
 * @file InstanceBuffer.h
 * @brief 实例缓冲区管理 - GPU-Driven 渲染的核心数据结构
 * 
 * 设计理念：
 * - 维护一个大的 GPU Buffer，存储所有精灵的实例数据
 * - 支持增量更新，避免每帧全量上传
 * - 管理实例的生命周期 (添加、更新、删除)
 * 
 * GPU 端数据布局：
 * - 每个 Sprite 对应一个 GPUInstanceData 结构
 * - Compute Shader 直接访问这个 Buffer
 * - Indirect Draw 使用这个 Buffer 中的数据
 * 
 * 内存管理策略：
 * - 使用对象池模式，避免频繁分配/释放
 * - 删除操作只是标记为"空闲"，不立即释放
 * - 空闲槽位会被后续添加操作复用
 */

#pragma once

#include <vector>
#include <cstdint>
#include <cstring>
#include <queue>
#include <optional>
#include "../../shared/ResourceHandle.h"
#include "../../../core/math/Color.h"

namespace engine {

/**
 * @brief GPU 实例数据结构
 * 
 * 这个结构体直接映射到 GPU Shader 中的布局。
 * 必须保持 16 字节对齐 (std140 / std430 布局)。
 * 
 * GLSL 对应定义：
 * struct InstanceData {
 *     mat2x3 transform;   // 6 floats = 24 bytes
 *     vec4 uv;            // 4 floats = 16 bytes
 *     uint textureIndex;  // 4 bytes
 *     uint layer;         // 4 bytes
 *     uint sortKey;       // 4 bytes
 *     uint color;         // 4 bytes
 *     vec2 pivot;         // 8 bytes (新增: 锚点)
 *     vec2 size;          // 8 bytes (新增: 尺寸)
 * };
 * 
 * 总大小: 72 bytes per instance
 */
struct GPUInstanceData {
    // ========== 变换数据 ==========
    
    /**
     * @brief 2D 仿射变换矩阵 (2x3)
     * 
     * [m00, m01, m02]   [sx*cos,  -sy*sin,  tx]
     * [m10, m11, m12] = [sx*sin,   sy*cos,  ty]
     * 
     * 其中：
     * - sx, sy: 缩放
     * - cos, sin: 旋转
     * - tx, ty: 平移
     */
    float transform[6];
    
    // ========== 纹理数据 ==========
    
    /**
     * @brief UV 坐标 (u0, v0, u1, v1)
     * 
     * u0, v0: 左上角 UV
     * u1, v1: 右下角 UV
     * 
     * 使用纹理图集时，这些值在 0-1 范围内。
     */
    float uv[4];
    
    // ========== 渲染参数 ==========
    
    /**
     * @brief 纹理数组索引
     * 
     * 当使用 Texture2DArray 时，这是纹理层索引。
     * 当使用纹理图集时，这是图集中的子纹理索引。
     */
    uint32_t textureIndex;
    
    /**
     * @brief 渲染层 (0-31)
     * 
     * 用于控制渲染顺序：
     * - 0: 最底层 (如背景)
     * - 15: 中间层 (如角色)
     * - 31: 最顶层 (如 UI)
     */
    uint32_t layer;
    
    /**
     * @brief 排序键
     * 
     * 用于 Y 排序：
     * - 高 16 位: layer (确保层级优先)
     * - 低 16 位: Y 坐标 (确保同层级内 Y 排序)
     * 
     * 这样可以在一次排序中同时完成层级排序和 Y 排序。
     */
    uint32_t sortKey;
    
    /**
     * @brief 颜色 (RGBA 打包)
     * 
     * 格式: 0xRRGGBBAA
     * - R: 红色通道 (0-255)
     * - G: 绿色通道 (0-255)
     * - B: 蓝色通道 (0-255)
     * - A: 透明度 (0-255)
     * 
     * 使用打包格式减少 GPU 内存占用。
     */
    uint32_t color;
    
    // ========== 额外数据 (可选) ==========
    
    /**
     * @brief 锚点 (pivotX, pivotY)
     * 
     * 旋转和缩放的中心点：
     * - (0, 0): 左上角
     * - (0.5, 0.5): 中心点
     * - (1, 1): 右下角
     */
    float pivot[2];
    
    /**
     * @brief 精灵尺寸 (width, height)
     * 
     * 用于：
     * - GPU 剔除时计算包围盒
     * - 顶点着色器生成四边形
     */
    float size[2];
    
    // ========== 辅助方法 ==========
    
    /**
     * @brief 设置颜色
     */
    void setColor(float r, float g, float b, float a) {
        color = (static_cast<uint32_t>(r * 255) << 24) |
                (static_cast<uint32_t>(g * 255) << 16) |
                (static_cast<uint32_t>(b * 255) << 8)  |
                (static_cast<uint32_t>(a * 255));
    }
    
    /**
     * @brief 设置颜色 (从 Color 结构体)
     */
    void setColor(const core::Color& c) {
        setColor(c.r, c.g, c.b, c.a);
    }
    
    /**
     * @brief 计算排序键
     * @param layer 渲染层
     * @param y Y 坐标 (世界坐标)
     * 
     * 排序键编码：
     * - 高 16 位: layer (确保层级优先)
     * - 低 16 位: Y 坐标 (转换为正整数)
     */
    static uint32_t computeSortKey(uint32_t layer, float y) {
        // 将 Y 坐标映射到 0-65535 范围
        // 假设世界坐标范围是 [-32768, 32767]
        uint32_t yKey = static_cast<uint32_t>(y + 32768) & 0xFFFF;
        return (layer << 16) | yKey;
    }
};

/**
 * @brief 实例缓冲区管理器
 * 
 * 职责：
 * 1. 管理 CPU 端的实例数据数组
 * 2. 跟踪哪些实例被修改 (需要上传到 GPU)
 * 3. 管理实例的生命周期 (添加、更新、删除)
 * 4. 提供增量更新接口
 */
class InstanceBuffer {
public:
    /**
     * @brief 构造函数
     * @param maxInstances 最大实例数量
     */
    explicit InstanceBuffer(uint32_t maxInstances = 65536)
        : maxInstances_(maxInstances)
        , instanceCount_(0)
    {
        instances_.reserve(maxInstances);
    }
    
    // ========== 实例管理 API ==========
    
    /**
     * @brief 添加一个新实例
     * @param data 实例数据
     * @return 实例 ID (用于后续更新/删除)
     * 
     * 添加策略：
     * 1. 优先复用空闲槽位 (对象池模式)
     * 2. 如果没有空闲槽位，追加到末尾
     * 3. 标记该实例为"脏" (需要上传到 GPU)
     */
    uint32_t addInstance(const GPUInstanceData& data) {
        uint32_t id;
        
        if (!freeSlots_.empty()) {
            // 复用空闲槽位
            id = freeSlots_.front();
            freeSlots_.pop();
            instances_[id] = data;
        } else {
            // 追加新实例
            id = static_cast<uint32_t>(instances_.size());
            if (id >= maxInstances_) {
                // 达到最大实例数，返回无效 ID
                return INVALID_INSTANCE_ID;
            }
            instances_.push_back(data);
            instanceCount_ = instances_.size();
        }
        
        // 标记为脏
        dirtyInstances_.push_back(id);
        
        return id;
    }
    
    /**
     * @brief 更新实例数据
     * @param id 实例 ID
     * @param data 新的实例数据
     * @return 是否成功
     * 
     * 注意：会标记实例为"脏"，下次 render() 时会上传到 GPU。
     */
    bool updateInstance(uint32_t id, const GPUInstanceData& data) {
        if (id >= instances_.size() || isSlotFree(id)) {
            return false;
        }
        
        instances_[id] = data;
        dirtyInstances_.push_back(id);
        return true;
    }
    
    /**
     * @brief 更新实例的变换矩阵
     * @param id 实例 ID
     * @param transform 6 元素的变换矩阵
     * @return 是否成功
     * 
     * 这是一个高频操作，用于动画和物理更新。
     */
    bool updateTransform(uint32_t id, const float transform[6]) {
        if (id >= instances_.size() || isSlotFree(id)) {
            return false;
        }
        
        std::memcpy(instances_[id].transform, transform, 6 * sizeof(float));
        dirtyInstances_.push_back(id);
        return true;
    
    }
    
    /**
     * @brief 更新实例的 Y 坐标 (用于动态 Y 排序)
     * @param id 实例 ID
     * @param y 新的 Y 坐标
     * @return 是否成功
     */
    bool updateY(uint32_t id, float y) {
        if (id >= instances_.size() || isSlotFree(id)) {
            return false;
        }
        
        instances_[id].sortKey = GPUInstanceData::computeSortKey(
            instances_[id].layer, y
        );
        dirtyInstances_.push_back(id);
        return true;
    }
    
    /**
     * @brief 删除实例
     * @param id 实例 ID
     * @return 是否成功
     * 
     * 删除策略：
     * - 不立即从数组中移除 (避免大量元素移动)
     * - 将槽位加入空闲队列，等待复用
     * - 标记为"脏"，以便 GPU 端更新
     */
    bool removeInstance(uint32_t id) {
        if (id >= instances_.size() || isSlotFree(id)) {
            return false;
        }
        
        freeSlots_.push(id);
        dirtyInstances_.push_back(id);
        
        // 更新活跃实例数
        if (id == instances_.size() - 1) {
            // 如果删除的是最后一个，缩小数组
            instances_.pop_back();
            instanceCount_ = instances_.size();
        }
        
        return true;
    }
    
    /**
     * @brief 获取实例数据
     * @param id 实例 ID
     * @return 实例数据指针 (只读)，失败返回 nullptr
     */
    const GPUInstanceData* getInstance(uint32_t id) const {
        if (id >= instances_.size() || isSlotFree(id)) {
            return nullptr;
        }
        return &instances_[id];
    }
    
    /**
     * @brief 获取实例数据 (可修改)
     * @param id 实例 ID
     * @return 实例数据指针，失败返回 nullptr
     * 
     * 注意：修改后需要手动调用 markDirty()
     */
    GPUInstanceData* getInstanceMutable(uint32_t id) {
        if (id >= instances_.size() || isSlotFree(id)) {
            return nullptr;
        }
        return &instances_[id];
    }
    
    /**
     * @brief 标记实例为脏
     */
    void markDirty(uint32_t id) {
        if (id < instances_.size()) {
            dirtyInstances_.push_back(id);
        }
    }

    // ========== 数据访问 API ==========
    
    /**
     * @brief 获取所有实例数据 (用于全量上传)
     */
    const std::vector<GPUInstanceData>& getAllInstances() const {
        return instances_;
    }
    
    /**
     * @brief 获取脏实例列表 (用于增量上传)
     * @return 脏实例 ID 列表
     */
    const std::vector<uint32_t>& getDirtyInstances() const {
        return dirtyInstances_;
    }
    
    /**
     * @brief 清除脏标记 (上传到 GPU 后调用)
     */
    void clearDirty() {
        dirtyInstances_.clear();
    }
    
    /**
     * @brief 获取活跃实例数
     */
    uint32_t getActiveCount() const {
        return instanceCount_ - static_cast<uint32_t>(freeSlots_.size());
    }
    
    /**
     * @brief 获取总容量
     */
    uint32_t getCapacity() const {
        return maxInstances_;
    }
    
    /**
     * @brief 清空所有实例
     */
    void clear() {
        instances_.clear();
        dirtyInstances_.clear();
        while (!freeSlots_.empty()) {
            freeSlots_.pop();
        }
        instanceCount_ = 0;
    }

    // ========== 常量 ==========
    
    static constexpr uint32_t INVALID_INSTANCE_ID = 0xFFFFFFFF;

private:
    /**
     * @brief 检查槽位是否空闲
     */
    bool isSlotFree(uint32_t id) const {
        // 遍历空闲队列检查
        std::queue<uint32_t> temp = freeSlots_;
        while (!temp.empty()) {
            if (temp.front() == id) return true;
            temp.pop();
        }
        return false;
    }
    
    uint32_t maxInstances_;         // 最大实例数
    uint32_t instanceCount_;        // 当前实例数 (包括空闲槽位)
    
    std::vector<GPUInstanceData> instances_;     // 实例数据数组
    std::vector<uint32_t> dirtyInstances_;       // 脏实例列表
    std::queue<uint32_t> freeSlots_;             // 空闲槽位队列
};

} // namespace engine
