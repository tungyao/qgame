/**
 * TileMapObjectConverter: 将 ObjectLayer 中的对象转换为 Sprite 实体
 *
 * 支持在运行时将对象"升级"为 Sprite（例如：砍倒树、破坏建筑、取得物品）
 */

#pragma once

#include "../components/RenderComponents.h"
#include "../components/PhysicsComponents.h"
#include "../runtime/EngineContext.h"
#include <functional>
#include <map>

namespace engine {

/**
 * ObjectConversionConfig: 转换配置
 *
 * 定义如何将 TileMap::ObjectInstance 转换为 Sprite 实体
 */
struct ObjectConversionConfig {
    // 基础配置
    int tileSizePixels = 16;
    
    // Sprite 配置
    bool createVisibleSprite = true;
    bool createRigidBody = false;
    float rigidBodyDensity = 1.0f;
    
    // 动画配置
    bool playDestructionAnimation = false;
    const char* destructionAnimationName = nullptr;  // "falling", "explode" 等
    
    // 事件回调
    std::function<void(entt::entity)> onObjectConverted = nullptr;
};

/**
 * TileMapObjectConverter: 对象转换器
 */
class TileMapObjectConverter {
public:
    /**
     * 将指定的 ObjectInstance 转换为 Sprite 实体
     * 
     * @param ctx 引擎上下文
     * @param tilemap TileMap 组件
     * @param tmTransform TileMap 的 Transform
     * @param layerIndex 层索引
     * @param cellIdx 对象在层中的 cellIdx
     * @param config 转换配置
     * @return 创建的 Sprite 实体，如果转换失败返回 entt::null
     */
    static entt::entity convertObjectToSprite(
        EngineContext& ctx,
        const TileMap& tilemap,
        const Transform& tmTransform,
        int layerIndex,
        int cellIdx,
        const ObjectConversionConfig& config = {}
    ) {
        // 查找对象
        if (layerIndex < 0 || layerIndex >= static_cast<int>(tilemap.layers.size())) {
            return entt::null;
        }

        const auto& layer = tilemap.layers[layerIndex];
        if (layer.type != TileMap::LayerType::Object) {
            return entt::null;
        }

        auto objIt = layer.objects.find(cellIdx);
        if (objIt == layer.objects.end()) {
            return entt::null;
        }

        const auto& obj = objIt->second;

        // 计算格子坐标
        int cellX = cellIdx % tilemap.width;
        int cellY = cellIdx / tilemap.width;

        // 计算世界坐标（对象左上角）
        float worldX = tmTransform.x + cellX * tilemap.tileSize;
        float worldY = tmTransform.y + cellY * tilemap.tileSize;

        // 查询 baseGid 的纹理信息
        const auto* tileset = tilemap.tilesetForGid(obj.baseGid);
        if (!tileset || !tileset->texture.valid()) {
            return entt::null;
        }

        int localId = obj.baseGid - tileset->firstGid;
        if (localId < 0 || localId >= tileset->count) {
            return entt::null;
        }

        // 计算源矩形（从 tileset 中查询）
        int atlasX = localId % tileset->columns;
        int atlasY = localId / tileset->columns;
        core::Rect srcRect{
            static_cast<float>(atlasX * tilemap.tileSize),
            static_cast<float>(atlasY * tilemap.tileSize),
            static_cast<float>(tilemap.tileSize * obj.width),
            static_cast<float>(tilemap.tileSize * obj.height)
        };

        // 创建 Sprite 实体
        auto entity = ctx.world.create();

        // 添加 Transform（对象中心）
        auto& tf = ctx.world.emplace<Transform>(entity);
        tf.x = worldX + obj.width * tilemap.tileSize * 0.5f;
        tf.y = worldY + obj.height * tilemap.tileSize * 0.5f;
        tf.rotation = 0.f;
        tf.scaleX = 1.f;
        tf.scaleY = 1.f;

        // 添加 Sprite
        auto& sprite = ctx.world.emplace<Sprite>(entity);
        sprite.texture = tileset->texture;
        sprite.srcRect = srcRect;
        sprite.layer = layer.renderLayer;
        sprite.ySort = true;
        sprite.sortOrder = 0;
        sprite.visible = true;
        sprite.pass = RenderPass::World;
        sprite.pivotX = 0.5f;
        sprite.pivotY = 0.5f;
        sprite.tint = core::Color::White;

        // 可选：添加 RigidBody2D（用于物理交互）
        if (config.createRigidBody) {
            auto& rb = ctx.world.emplace<RigidBody2D>(entity);
            rb.mass = 1.0f;
            rb.gravityScale = 1.0f;
            rb.useGravity = true;
            rb.isKinematic = false;
            
            // 添加碰撞体（使用对象占用的范围）
            auto& collider = ctx.world.emplace<BoxCollider2D>(entity);
            collider.offsetX = 0.f;
            collider.offsetY = 0.f;
            collider.width = obj.width * tilemap.tileSize;
            collider.height = obj.height * tilemap.tileSize;
        }

        // 执行回调
        if (config.onObjectConverted) {
            config.onObjectConverted(entity);
        }

        return entity;
    }

    /**
     * 将 ObjectInstance 转换为配置字典（用于脚本或编辑器）
     */
    static std::map<std::string, float> objectToConfigMap(
        const TileMap::ObjectInstance& obj
    ) {
        std::map<std::string, float> config;
        config["width"] = obj.width;
        config["height"] = obj.height;
        config["baseGid"] = obj.baseGid;
        config["collisionGid"] = obj.collisionGid;
        config["flags"] = static_cast<float>(obj.flags);
        
        // 添加自定义属性
        for (size_t i = 0; i < obj.customAttrs.size(); ++i) {
            config["attr_" + std::to_string(i)] = obj.customAttrs[i];
        }
        
        return config;
    }
};

/**
 * ObjectDestructionEvent: 对象销毁事件
 * 
 * 事件类型，可用于系统监听对象销毁
 */
struct ObjectDestructionEvent {
    entt::entity spriteEntity;
    std::string objectId;
    int cellIdx;
    int layerIndex;
};

/**
 * ObjectInteractionEvent: 对象交互事件
 * 
 * 事件类型，可用于系统处理玩家与对象的交互
 */
struct ObjectInteractionEvent {
    entt::entity interactorEntity;  // 交互者（如玩家）
    entt::entity spriteEntity;       // 对象的 Sprite 实体
    std::string objectId;
    std::string interactionType;     // "pickup", "cut", "open" 等
};

}  // namespace engine
