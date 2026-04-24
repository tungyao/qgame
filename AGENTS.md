# QGame Engine - Agent Memory

## 项目架构

### 渲染后端
- **OpenGL 3.3 Core**: `src/backend/renderer/opengl/GLRenderDevice.cpp`
- **SDL GPU (Vulkan/Metal/D3D12)**: `src/backend/renderer/sdl_gpu/SDLGPURenderDevice.cpp`

### 枲心系统
- **RenderSystem**: `src/engine/systems/RenderSystem.cpp` - 场景命令构建
- **RenderPipeline**: `src/backend/renderer/RenderPipeline.cpp` - Pass管理
- **CommandBuffer**: `src/backend/renderer/CommandBuffer.cpp` - 渲染命令录制

## MSDF 字体渲染系统

### 数据结构

#### FontData (`src/engine/components/FontData.h`)
```cpp
struct Glyph {
    uint32_t codepoint;     // Unicode码点
    float u0, v0, u1, v1;   // UV坐标
    float width, height;     // 字形尺寸
    float bearingX, bearingY; // 基线偏移
    float advance;           // 光标前进距离
};

struct FontData {
    TextureHandle texture;              // MSDF图集纹理
    std::unordered_map<uint32_t, Glyph> glyphs;
    float lineHeight;  // 行高（倍数）
    float baseline;    // 基线位置
    float fontSize;    // 原始字体大小
};
```

#### TextComponent (`src/engine/components/TextComponent.h`)
```cpp
struct TextComponent {
    std::string text;
    FontHandle  font;        // 字体句柄
    float       fontSize;    // 渲染大小
    core::Color color;       // 文字颜色
    int         layer;       // 渲染层
    bool        visible;
};
```

### 渲染流程

1. **RenderSystem::buildSceneCommands()**
   - 遍历所有 `TextComponent`
   - 创建 `DrawTextCmd` 并添加到命令缓冲

2. **CommandBuffer**
   - `DrawTextCmd` 包含字体句柄、文本内容、位置、颜色等

3. **RenderDevice::renderCmdsToTarget()**
   - 解析 `DrawTextCmd`
   - 为每个字符构建四边形顶点
   - 使用MSDF pipeline渲染

### MSDF着色器

#### OpenGL (`src/backend/renderer/opengl/GLRenderDevice.cpp`)
```glsl
// 片段着色器
float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec3 sampleVal = texture(uTexture, vUV).rgb;
    float sigDist = median(sampleVal.r, sampleVal.g, sampleVal.b) - 0.5;
    float opacity = clamp(sigDist * uPxRange + 0.5, 0.0, 1.0);
    fragColor = vec4(vColor.rgb, vColor.a * opacity);
}
```

#### SDL GPU (`assets/shaders/msdf.frag.glsl`)
- SPIRV: 通过push constant传递 `pxRange`
- DXIL: 通过cbuffer传递 `pxRange`

### 使用方法

#### 1. 生成MSDF字体图集
使用 `msdfgen` 工具：
```bash
msdfgen -font font.ttf -o font.png -json font.json
```

#### 2. 加载字体数据
```cpp
engine::FontData fontData;
fontData.texture = renderDevice.createTexture({...});
// 从JSON加载glyphs数据
FontHandle font = renderDevice.createFont(fontData);
```

#### 3. 创建文字实体
```cpp
auto entity = api.createEntity();
auto& text = api.addComponent<TextComponent>(entity);
text.font = font;
text.text = "Hello World";
text.fontSize = 24.0f;
text.color = core::Color::White;
```

### 关键实现细节

#### 字符渲染
- 每个字符渲染为纹理四边形
- 光标位置 = `cursorX + advance * scale`
- 字形位置 = `(cursorX + bearingX, cursorY + bearingY - height)`
- 缩放比例 = `fontSize / font.fontSize`

#### 批处理优化
- `BatchSegment.isFont` 标志区分字体/精灵批次
- 字体批次使用MSDF pipeline
- 精灵批次使用普通pipeline

#### 像素范围 (pxRange)
- 默认值: 4.0f
- 影响抗锯齿边缘锐度
- 可根据MSDF生成参数调整

## 编译命令

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4
```

### 着色器编译
- 自动编译: CMake通过 `glslc` 编译 `.glsl` → `.spv`
- DXIL支持: Windows上自动编译 `.hlsl` → `.dxil`
- Fallback: 预编译头文件在 `src/backend/renderer/sdl_gpu/shaders_precompiled/`

## 文件结构

```
src/
├── engine/
│   ├── components/
│   │   ├── FontData.h          # 字体数据结构
│   │   └── TextComponent.h     # 文字组件
│   └── systems/
│       └── RenderSystem.cpp    # 场景渲染逻辑
├── backend/
│   ├── renderer/
│   │   ├── CommandBuffer.h     # DrawTextCmd定义
│   │   ├── opengl/
│   │   │   └── GLRenderDevice.cpp  # MSDF着色器实现
│   │   └── sdl_gpu/
│   │       └── SDLGPURenderDevice.cpp # SDL GPU实现
│   └── shared/
│       └── ResourceHandle.h    # FontHandle定义
└── assets/shaders/
    ├── msdf.frag.glsl          # SPIRV着色器
    └── msdf.frag.hlsl          # DXIL着色器
```

## 待完成功能

- [ ] 字体资源加载器（集成AssetManager）
- [ ] MSDF字体图集生成工具集成
- [ ] 多行文本支持
- [ ] 文字对齐（左/中/右）
- [ ] 文字效果（描边/阴影/发光）
