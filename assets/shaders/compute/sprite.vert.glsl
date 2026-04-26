/**
 * @file sprite.vert.glsl
 * @brief GPU-Driven 精灵顶点着色器
 * 
 * 功能：
 * - 从 InstanceBuffer 读取实例数据
 * - 从 VisibleIndexBuffer 读取实例索引 (通过 gl_InstanceID)
 * - 生成精灵四边形的顶点
 * - 应用变换矩阵
 * 
 * 渲染策略：
 * - 每个精灵渲染为一个四边形 (2 个三角形)
 * - 使用 Geometry Shader 或顶点着色器生成顶点
 * - 当前实现：顶点着色器 + Instancing
 * 
 * 性能优化：
 * - 减少内存访问
 * - 使用实例化渲染
 * - 避免分支
 */

#version 450

// ========== 输入 ==========

/**
 * 顶点属性
 * 每个 sprite 是一个四边形，6 个顶点
 * 
 * 顶点布局：
 *   0---1
 *   |  /|
 *   | / |
 *   |/  2
 *   3
 */
layout(location = 0) in uint vertexIndex;  // 顶点索引 (0-5)

// ========== 绑定资源 ==========

/**
 * 实例数据缓冲区
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

layout(binding = 0) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

/**
 * 可见实例索引缓冲区
 * 存储排序后的可见实例索引
 */
layout(binding = 1) readonly buffer VisibleIndexBuffer {
    uint visibleIndices[];
};

// ========== Uniform ==========

/**
 * 相机参数
 */
layout(push_constant) uniform CameraParams {
    mat3 viewProjection;  // 2D 相机的 ViewProjection 矩阵
} camera;

// ========== 输出 ==========

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out flat uint vTextureIndex;

// ========== 常量 ==========

// 四边形顶点位置 (相对于精灵中心)
const vec2 quadVertices[6] = vec2[](
    vec2(0.0, 0.0),  // 左上
    vec2(1.0, 0.0),  // 右上
    vec2(1.0, 1.0),  // 右下
    vec2(0.0, 0.0),  // 左上
    vec2(1.0, 1.0),  // 右下
    vec2(0.0, 1.0)   // 左下
);

// ========== 主函数 ==========

void main() {
    // ========== Step 1: 获取实例数据 ==========
    
    // 从可见实例索引缓冲区获取实例 ID
    uint instanceId = visibleIndices[gl_InstanceID];
    
    // 从实例缓冲区读取实例数据
    InstanceData inst = instances[instanceId];
    
    // ========== Step 2: 生成顶点位置 ==========
    
    // 获取当前顶点在四边形中的相对位置 (0-1)
    vec2 quadPos = quadVertices[vertexIndex];
    
    // 应用锚点偏移
    // quadPos 范围是 [0, 1]，锚点范围也是 [0, 1]
    // 将锚点移动到中心：(quadPos - pivot)
    vec2 localPos = (quadPos - inst.pivot) * inst.size;
    
    // ========== Step 3: 应用变换 ==========
    
    // 应用 2D 仿射变换
    // transform 是 mat2x3，格式为：
    // [m00, m01, m02]   [sx*cos,  -sy*sin,  tx]
    // [m10, m11, m12] = [sx*sin,   sy*cos,  ty]
    vec2 worldPos = vec2(
        inst.transform[0][0] * localPos.x + inst.transform[1][0] * localPos.y + inst.transform[2][0],
        inst.transform[0][1] * localPos.x + inst.transform[1][1] * localPos.y + inst.transform[2][1]
    );
    
    // 应用相机变换
    vec3 clipPos = camera.viewProjection * vec3(worldPos, 1.0);
    gl_Position = vec4(clipPos.xy, 0.0, 1.0);
    
    // ========== Step 4: 计算 UV ==========
    
    // UV 坐标范围是 [uv.xy, uv.zw]
    // quadPos.x 对应 UV 的 x 方向
    // quadPos.y 对应 UV 的 y 方向
    vUV = mix(inst.uv.xy, inst.uv.zw, quadPos);
    
    // ========== Step 5: 设置颜色 ==========
    
    // 解包颜色 (RGBA → vec4)
    vColor = vec4(
        float((inst.color >> 24) & 0xFF) / 255.0,
        float((inst.color >> 16) & 0xFF) / 255.0,
        float((inst.color >> 8) & 0xFF) / 255.0,
        float(inst.color & 0xFF) / 255.0
    );
    
    // 设置纹理索引
    vTextureIndex = inst.textureIndex;
}

/**
 * 优化建议：
 * 
 * 1. 使用 Geometry Shader：
 *    - 输入一个点 (精灵中心)
 *    - 输出四边形的 6 个顶点
 *    - 减少顶点缓冲区大小
 * 
 * 2. 使用 Mesh Shader (如果支持)：
 *    - 更灵活的顶点生成
 *    - 支持 LOD
 * 
 * 3. 纹理数组：
 *    - 使用 Texture2DArray 而非纹理图集
 *    - 避免 UV 坐标转换
 * 
 * 4. Sprite Batching：
 *    - 合并相同纹理的精灵
 *    - 减少纹理切换
 */
