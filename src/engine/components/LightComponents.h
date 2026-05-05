#pragma once

#include <cstdint>

#include "../../core/math/Color.h"

namespace engine {

// 2D 光源类型保持非常小的枚举集。第一阶段只把数据稳定下来；
// 后续 Vulkan compute shader 会根据 type 选择点光/聚光/面光的衰减公式。
enum class Light2DType : uint8_t {
    Point = 0,
    Spot  = 1,
    Area  = 2,
};

// Light2D 是“参数光源”，不依赖 normal map、roughness map 或 emission map。
// 它的 Transform 组件提供世界坐标；这里仅保存光照参数。这样同一个光源
// 可以像普通实体一样被动画、脚本或编辑器移动。
struct Light2D {
    Light2DType type = Light2DType::Point;

    // 光源颜色使用现有 core::Color，最终 GPU 上传时会转成 0..1 float4。
    core::Color color = core::Color::White;

    // radius 是世界像素半径。2D 光照系统统一以 world pixel 作为单位，
    // 避免 tile、sprite、physics 和 lighting 各自有一套坐标解释。
    float radius = 256.f;

    // intensity 是线性强度倍率。夜晚 demo 可以把环境光压低、局部灯强度提高，
    // 但不需要改 sprite 贴图本身。
    float intensity = 1.f;

    // softness 表示软阴影采样半径或 area light 半径。第一版硬阴影可以忽略它；
    // 第二版 compute shader 会用它在光源圆盘上采样多个点。
    float softness = 16.f;

    // Spot light 使用 coneRotation/coneAngle。Point/Area 会忽略这两个字段。
    float coneRotation = 0.f;
    float coneAngle = 6.28318530718f;

    // layerMask 与 Camera::layerMask 的思路一致：光源只影响指定 RenderPass/层。
    // 初版主要用于 World pass，保留字段是为了后续多层世界和编辑器预览。
    uint32_t layerMask = 0xFFFFFFFFu;

    bool castsShadow = true;
    bool visible = true;
};

// LightOccluder2D 是“遮挡几何”，不是纹理 mask。它让墙、门、角色、tile 边
// 都能用几何数据参与 2D ray casting。第一阶段只要求 AABB/Segment 稳定；
// Polygon 预留给后续编辑器多边形工具。
struct LightOccluder2D {
    enum class Shape : uint8_t {
        AABB    = 0,
        Segment = 1,
        Polygon = 2,
    };

    Shape shape = Shape::AABB;

    // AABB 模式：Transform 给中心点或左上定位由调用方约定；width/height 给尺寸。
    // RenderSystem 上传前会把 AABB 展开成 4 条 segment，compute shader 只处理线段。
    float width = 0.f;
    float height = 0.f;

    // Segment 模式：a/b 是相对 Transform 的局部端点。这样移动实体时不用改遮挡线。
    float ax = 0.f;
    float ay = 0.f;
    float bx = 0.f;
    float by = 0.f;

    // opacity 允许半透明遮挡：窗帘、烟雾、树叶可以只削弱光，而不是完全挡住。
    float opacity = 1.f;

    // height 是 2.5D 预留：后续可以让矮物体只投短阴影，墙投长阴影。
    float heightZ = 1.f;

    bool castsShadow = true;
};

// Reflector2D 只让显式区域产生反射。这样夜晚湿地、水面、镜面墙可控，
// 普通地砖不会因为算法“过度聪明”把整张画面复制一遍。
struct Reflector2D {
    enum class Shape : uint8_t {
        Segment = 0,
        AABB    = 1,
    };

    Shape shape = Shape::Segment;

    // Segment 模式：通常用作水面线或镜面边界。
    float ax = 0.f;
    float ay = 0.f;
    float bx = 0.f;
    float by = 0.f;

    // AABB 模式：通常用作湿地区域或一块浅水区域。
    float width = 0.f;
    float height = 0.f;

    float reflectivity = 0.5f;
    float roughness = 0.35f;
    core::Color tint = core::Color::White;
    bool visible = true;
};

// Environment2D 是每个场景的“夜晚/天气/曝光”参数。它不直接绑定到贴图，
// 也不要求全局单例；挂在任意实体上即可，RenderSystem 后续会取第一个 enabled
// 的环境组件作为当前 World lighting 参数。
struct Environment2D {
    core::Color ambientColor = core::Color{32, 40, 56, 255};
    float ambientIntensity = 0.25f;
    float exposure = 1.f;
    float bloomThreshold = 1.1f;
    float wetness = 0.f;
    bool enabled = true;
};

} // namespace engine
