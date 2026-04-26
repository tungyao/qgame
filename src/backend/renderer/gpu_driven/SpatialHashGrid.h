/**
 * @file SpatialHashGrid.h
 * @brief 空间哈希网格 - 用于高效的视口裁剪
 * 
 * 设计理念：
 * - 将世界空间划分为固定大小的格子 (Cell)
 * - 每个格子存储该区域内的对象引用
 * - 查询时只遍历视口覆盖的格子，而非所有对象
 * 
 * 时间复杂度：
 * - 插入: O(1) 平均
 * - 删除: O(1) 平均
 * - 查询: O(视口内对象数)，与总对象数无关
 * 
 * 适用场景：
 * - 大型 2D 世界 (如开放世界游戏)
 * - 大量动态对象 (如粒子系统、子弹)
 * - 需要频繁视口裁剪的场景
 */

#pragma once

#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cmath>

namespace engine {

/**
 * @brief 空间哈希网格模板类
 * @tparam T 存储的对象类型 (通常是指针或 ID)
 * 
 * 使用示例：
 * @code
 * SpatialHashGrid<Entity> grid(64.0f);  // 64x64 像素的格子
 * 
 * // 插入对象
 * grid.insert(entity, entity.position.x, entity.position.y);
 * 
 * // 查询视口内的对象
 * auto visible = grid.query(viewport.x, viewport.y, viewport.w, viewport.h);
 * 
 * // 清空 (每帧开始时)
 * grid.clear();
 * @endcode
 */
template<typename T>
class SpatialHashGrid {
public:
    /**
     * @brief 构造函数
     * @param cellSize 格子大小 (像素)，通常设置为最大对象尺寸的 2 倍
     * 
     * 格子大小的选择：
     * - 太小: 格子数量多，内存开销大
     * - 太大: 每个格子内对象多，裁剪效率低
     * - 推荐: 64-128 像素 (适合大多数 2D 游戏)
     */
    explicit SpatialHashGrid(float cellSize = 64.0f)
        : cellSize_(cellSize)
        , invCellSize_(1.0f / cellSize)
    {
    }

    /**
     * @brief 插入对象到网格中
     * @param item 要插入的对象
     * @param x 对象的世界坐标 X
     * @param y 对象的世界坐标 Y
     * 
     * 注意：
     * - 对象会被插入到其中心点所在的格子
     * - 如果对象跨越多个格子，需要多次调用 insert
     * - 建议每帧开始时先 clear()，然后重新插入所有活跃对象
     */
    void insert(const T& item, float x, float y) {
        // 计算哈希键：将世界坐标映射到格子坐标
        int64_t key = hashKey(
            static_cast<int>(x * invCellSize_),
            static_cast<int>(y * invCellSize_)
        );
        
        cells_[key].push_back(item);
    }
    
    /**
     * @brief 插入对象到网格中 (支持跨越多个格子)
     * @param item 要插入的对象
     * @param x 对象边界框左上角 X
     * @param y 对象边界框左上角 Y
     * @param width 边界框宽度
     * @param height 边界框高度
     * 
     * 这个方法会遍历边界框覆盖的所有格子，确保对象被正确索引。
     * 适用于大型对象 (如 Boss、大型建筑)。
     */
    void insert(const T& item, float x, float y, float width, float height) {
        int minCx = static_cast<int>(x * invCellSize_);
        int maxCx = static_cast<int>((x + width) * invCellSize_);
        int minCy = static_cast<int>(y * invCellSize_);
        int maxCy = static_cast<int>((y + height) * invCellSize_);
        
        // 遍历覆盖的所有格子
        for (int cx = minCx; cx <= maxCx; ++cx) {
            for (int cy = minCy; cy <= maxCy; ++cy) {
                int64_t key = hashKey(cx, cy);
                
                // 避免重复插入同一格子
                auto& cell = cells_[key];
                if (cell.empty() || cell.back() != item) {
                    cell.push_back(item);
                }
            }
        }
    }

    /**
     * @brief 查询视口内的所有对象
     * @param x 视口左上角 X (世界坐标)
     * @param y 视口左上角 Y (世界坐标)
     * @param width 视口宽度
     * @param height 视口高度
     * @return 视口内的对象列表 (可能包含重复，需要调用方去重)
     * 
     * 性能特点：
     * - 时间复杂度：O(视口内格子数 + 格子内对象数)
     * - 不受世界总对象数影响
     * - 视口越大，查询时间越长
     */
    std::vector<T> query(float x, float y, float width, float height) const {
        std::vector<T> result;
        
        // 计算视口覆盖的格子范围
        int minCx = static_cast<int>(x * invCellSize_);
        int maxCx = static_cast<int>((x + width) * invCellSize_);
        int minCy = static_cast<int>(y * invCellSize_);
        int maxCy = static_cast<int>((y + height) * invCellSize_);
        
        // 预估结果大小，避免多次分配
        result.reserve((maxCx - minCx + 1) * (maxCy - minCy + 1) * 4);
        
        // 遍历所有覆盖的格子
        for (int cx = minCx; cx <= maxCx; ++cx) {
            for (int cy = minCy; cy <= maxCy; ++cy) {
                int64_t key = hashKey(cx, cy);
                auto it = cells_.find(key);
                
                if (it != cells_.end()) {
                    // 将格子内所有对象加入结果
                    result.insert(result.end(), it->second.begin(), it->second.end());
                }
            }
        }
        
        return result;
    }
    
    /**
     * @brief 查询指定格子内的所有对象
     * @param cx 格子坐标 X
     * @param cy 格子坐标 Y
     * @return 格子内的对象列表
     */
    const std::vector<T>* queryCell(int cx, int cy) const {
        int64_t key = hashKey(cx, cy);
        auto it = cells_.find(key);
        return it != cells_.end() ? &it->second : nullptr;
    }

    /**
     * @brief 清空网格 (每帧开始时调用)
     * 
     * 注意：这会清除所有格子，但不会释放已分配的内存。
     * 如果需要释放内存，使用 shrink_to_fit()。
     */
    void clear() {
        for (auto& [key, cell] : cells_) {
            cell.clear();
        }
        // 不清除 cells_ 本身，保留已分配的内存以便复用
    }
    
    /**
     * @brief 完全重置网格，释放所有内存
     */
    void reset() {
        cells_.clear();
        cells_.rehash(0);  // 释放 hash 表内存
    }

    /**
     * @brief 获取格子大小
     */
    float cellSize() const { return cellSize_; }
    
    /**
     * @brief 获取格子数量
     */
    size_t cellCount() const { return cells_.size(); }
    
    /**
     * @brief 获取对象总数 (可能有重复)
     */
    size_t itemCount() const {
        size_t count = 0;
        for (const auto& [key, cell] : cells_) {
            count += cell.size();
        }
        return count;
    }
    
    /**
     * @brief 获取格子坐标 (从世界坐标)
     */
    void worldToCell(float wx, float wy, int& outCx, int& outCy) const {
        outCx = static_cast<int>(wx * invCellSize_);
        outCy = static_cast<int>(wy * invCellSize_);
    }
    
    /**
     * @brief 获取世界坐标 (格子中心)
     */
    void cellToWorld(int cx, int cy, float& outX, float& outY) const {
        outX = (cx + 0.5f) * cellSize_;
        outY = (cy + 0.5f) * cellSize_;
    }

private:
    /**
     * @brief 计算格子的哈希键
     * @param cx 格子坐标 X
     * @param cy 格子坐标 Y
     * @return 64 位哈希键
     * 
     * 哈希策略：
     * - 将两个 32 位整数打包成一个 64 位整数
     * - 高 32 位存储 X，低 32 位存储 Y
     * - 确保不同坐标映射到不同键值
     */
    int64_t hashKey(int cx, int cy) const {
        // 使用位运算打包，避免哈希冲突
        return (static_cast<int64_t>(cx) << 32) | 
               (static_cast<uint32_t>(cy) & 0xFFFFFFFFLL);
    }

    float cellSize_;       // 格子大小 (像素)
    float invCellSize_;    // 1 / cellSize_，用于加速计算
    
    /**
     * @brief 存储格子的哈希表
     * 
     * 数据结构选择：
     * - unordered_map: 平均 O(1) 查找，适合稀疏网格
     * - 如果网格密集，可考虑 flat_hash_map 或 vector
     */
    std::unordered_map<int64_t, std::vector<T>> cells_;
};

} // namespace engine
