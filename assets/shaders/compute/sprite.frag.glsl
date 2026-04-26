/**
 * @file sprite.frag.glsl
 * @brief GPU-Driven 精灵片段着色器
 * 
 * 功能：
 * - 采样纹理 (从 Texture2DArray)
 * - 应用颜色调制
 * - 输出最终颜色
 * 
 * 支持特性：
 * - Alpha 混合
 * - 颜色调制
 * - 纹理数组采样
 */

#version 450

// ========== 输入 ==========

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in flat uint vTextureIndex;

// ========== 绑定资源 ==========

/**
 * 纹理数组
 * 包含所有精灵纹理
 */
layout(binding = 0) uniform sampler2DArray textureArray;

// ========== 输出 ==========

layout(location = 0) out vec4 fragColor;

// ========== 主函数 ==========

void main() {
    // ========== Step 1: 采样纹理 ==========
    
    // 使用 textureIndex 作为纹理数组的 Z 坐标
    vec4 texColor = texture(textureArray, vec3(vUV, float(vTextureIndex)));
    
    // ========== Step 2: 应用颜色调制 ==========
    
    fragColor = texColor * vColor;
    
    // ========== Step 3: Alpha 测试 (可选) ==========
    
    // 如果 Alpha 值过低，丢弃片段
    if (fragColor.a < 0.01) {
        discard;
    }
}

/**
 * 扩展功能：
 * 
 * 1. MSDF 字体渲染：
 *    - 使用 MSDF 技术
 *    - 支持高质量文字
 *    - 需要额外的 Shader 逻辑
 * 
 * 2. 特效支持：
 *    - 发光效果 (Glow)
 *    - 描边效果 (Outline)
 *    - 阴影效果 (Shadow)
 * 
 * 3. 自定义着色器：
 *    - 通过 uniform 传递特效参数
 *    - 支持运行时切换着色器
 */
