#include "GLRenderDevice.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <variant>

// SDL_opengl.h  → GL 1.1 types (GLuint, GLenum…) + GL 1.1 functions
// SDL_opengl_glext.h (no GL_GLEXT_PROTOTYPES) → GL 3.x constants + PFNGL*PROC typedefs
//   Windows opengl32.lib only exports GL 1.1; everything above must be loaded
//   at runtime via SDL_GL_GetProcAddress (wraps wglGetProcAddress on Windows).
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>

// ── GL 3.x function pointer variables ────────────────────────────────────────
// GL 1.3
static PFNGLACTIVETEXTUREPROC               s_glActiveTexture           = nullptr;
// GL 1.5
static PFNGLGENBUFFERSPROC                  s_glGenBuffers              = nullptr;
static PFNGLBINDBUFFERPROC                  s_glBindBuffer              = nullptr;
static PFNGLBUFFERDATAPROC                  s_glBufferData              = nullptr;
static PFNGLDELETEBUFFERSPROC               s_glDeleteBuffers           = nullptr;
// GL 2.0
static PFNGLCREATESHADERPROC                s_glCreateShader            = nullptr;
static PFNGLSHADERSOURCEPROC                s_glShaderSource            = nullptr;
static PFNGLCOMPILESHADERPROC               s_glCompileShader           = nullptr;
static PFNGLGETSHADERIVPROC                 s_glGetShaderiv             = nullptr;
static PFNGLGETSHADERINFOLOGPROC            s_glGetShaderInfoLog        = nullptr;
static PFNGLCREATEPROGRAMPROC               s_glCreateProgram           = nullptr;
static PFNGLATTACHSHADERPROC                s_glAttachShader            = nullptr;
static PFNGLLINKPROGRAMPROC                 s_glLinkProgram             = nullptr;
static PFNGLGETPROGRAMIVPROC                s_glGetProgramiv            = nullptr;
static PFNGLGETPROGRAMINFOLOGPROC           s_glGetProgramInfoLog       = nullptr;
static PFNGLUSEPROGRAMPROC                  s_glUseProgram              = nullptr;
static PFNGLDELETESHADERPROC                s_glDeleteShader            = nullptr;
static PFNGLDELETEPROGRAMPROC               s_glDeleteProgram           = nullptr;
static PFNGLGETUNIFORMLOCATIONPROC          s_glGetUniformLocation      = nullptr;
static PFNGLUNIFORMMATRIX4FVPROC            s_glUniformMatrix4fv        = nullptr;
static PFNGLUNIFORM1IPROC                   s_glUniform1i               = nullptr;
static PFNGLUNIFORM1FPROC                   s_glUniform1f               = nullptr;
static PFNGLUNIFORM4FVPROC                  s_glUniform4fv              = nullptr;
static PFNGLENABLEVERTEXATTRIBARRAYPROC     s_glEnableVertexAttribArray = nullptr;
static PFNGLVERTEXATTRIBPOINTERPROC         s_glVertexAttribPointer     = nullptr;
// GL 3.0
static PFNGLGENVERTEXARRAYSPROC             s_glGenVertexArrays         = nullptr;
static PFNGLBINDVERTEXARRAYPROC             s_glBindVertexArray         = nullptr;
static PFNGLDELETEVERTEXARRAYSPROC          s_glDeleteVertexArrays      = nullptr;
static PFNGLGENFRAMEBUFFERSPROC             s_glGenFramebuffers         = nullptr;
static PFNGLBINDFRAMEBUFFERPROC             s_glBindFramebuffer         = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DPROC        s_glFramebufferTexture2D    = nullptr;
static PFNGLDELETEFRAMEBUFFERSPROC          s_glDeleteFramebuffers      = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC      s_glCheckFramebufferStatus  = nullptr;
// GL 3.2  (glDrawElementsBaseVertex / sync objects)
using PFN_DrawElementsBaseVertex = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLint);
static PFN_DrawElementsBaseVertex           s_glDrawElementsBaseVertex  = nullptr;
static PFNGLFENCESYNCPROC                   s_glFenceSync               = nullptr;
static PFNGLCLIENTWAITSYNCPROC              s_glClientWaitSync          = nullptr;
static PFNGLDELETESYNCPROC                  s_glDeleteSync              = nullptr;

// ── Macro redirection ─────────────────────────────────────────────────────────
// Redefine each GL name to its function pointer — valid only inside this TU.
#define glActiveTexture           s_glActiveTexture
#define glGenBuffers              s_glGenBuffers
#define glBindBuffer              s_glBindBuffer
#define glBufferData              s_glBufferData
#define glDeleteBuffers           s_glDeleteBuffers
#define glCreateShader            s_glCreateShader
#define glShaderSource            s_glShaderSource
#define glCompileShader           s_glCompileShader
#define glGetShaderiv             s_glGetShaderiv
#define glGetShaderInfoLog        s_glGetShaderInfoLog
#define glCreateProgram           s_glCreateProgram
#define glAttachShader            s_glAttachShader
#define glLinkProgram             s_glLinkProgram
#define glGetProgramiv            s_glGetProgramiv
#define glGetProgramInfoLog       s_glGetProgramInfoLog
#define glUseProgram              s_glUseProgram
#define glDeleteShader            s_glDeleteShader
#define glDeleteProgram           s_glDeleteProgram
#define glGetUniformLocation      s_glGetUniformLocation
#define glUniformMatrix4fv        s_glUniformMatrix4fv
#define glUniform1i               s_glUniform1i
#define glUniform1f               s_glUniform1f
#define glUniform4fv              s_glUniform4fv
#define glEnableVertexAttribArray s_glEnableVertexAttribArray
#define glVertexAttribPointer     s_glVertexAttribPointer
#define glGenVertexArrays         s_glGenVertexArrays
#define glBindVertexArray         s_glBindVertexArray
#define glDeleteVertexArrays      s_glDeleteVertexArrays
#define glGenFramebuffers         s_glGenFramebuffers
#define glBindFramebuffer         s_glBindFramebuffer
#define glFramebufferTexture2D    s_glFramebufferTexture2D
#define glDeleteFramebuffers      s_glDeleteFramebuffers
#define glCheckFramebufferStatus  s_glCheckFramebufferStatus
#define glDrawElementsBaseVertex  s_glDrawElementsBaseVertex
#define glFenceSync               s_glFenceSync
#define glClientWaitSync          s_glClientWaitSync
#define glDeleteSync              s_glDeleteSync


#include "../CommandBuffer.h"
#include "../../../core/Logger.h"
#include "../../../core/Assert.h"

namespace backend {

// ── GL 3.x function loader ────────────────────────────────────────────────────
static bool loadGLFunctions() {
    // SDL_GL_GetProcAddress wraps wglGetProcAddress on Windows — must be called
    // after a GL context is current.
    #define QGAME_GL_LOAD(name) \
        s_gl##name = reinterpret_cast<decltype(s_gl##name)>( \
            SDL_GL_GetProcAddress("gl" #name)); \
        if (!s_gl##name) { core::logError("GL: missing gl" #name); ok = false; }

    #define QGAME_GL_LOAD_OPTIONAL(name) \
        s_gl##name = reinterpret_cast<decltype(s_gl##name)>( \
            SDL_GL_GetProcAddress("gl" #name));

    bool ok = true;
    QGAME_GL_LOAD(ActiveTexture)
    QGAME_GL_LOAD(GenBuffers)              QGAME_GL_LOAD(BindBuffer)
    QGAME_GL_LOAD(BufferData)              QGAME_GL_LOAD(DeleteBuffers)
    QGAME_GL_LOAD(CreateShader)            QGAME_GL_LOAD(ShaderSource)
    QGAME_GL_LOAD(CompileShader)           QGAME_GL_LOAD(GetShaderiv)
    QGAME_GL_LOAD(GetShaderInfoLog)        QGAME_GL_LOAD(CreateProgram)
    QGAME_GL_LOAD(AttachShader)            QGAME_GL_LOAD(LinkProgram)
    QGAME_GL_LOAD(GetProgramiv)            QGAME_GL_LOAD(GetProgramInfoLog)
    QGAME_GL_LOAD(UseProgram)              QGAME_GL_LOAD(DeleteShader)
    QGAME_GL_LOAD(DeleteProgram)           QGAME_GL_LOAD(GetUniformLocation)
    QGAME_GL_LOAD(UniformMatrix4fv)        QGAME_GL_LOAD(Uniform1i)
    QGAME_GL_LOAD(Uniform1f)
    QGAME_GL_LOAD(Uniform4fv)
    QGAME_GL_LOAD(EnableVertexAttribArray) QGAME_GL_LOAD(VertexAttribPointer)
    QGAME_GL_LOAD(GenVertexArrays)         QGAME_GL_LOAD(BindVertexArray)
    QGAME_GL_LOAD(DeleteVertexArrays)
    QGAME_GL_LOAD(GenFramebuffers)         QGAME_GL_LOAD(BindFramebuffer)
    QGAME_GL_LOAD(FramebufferTexture2D)    QGAME_GL_LOAD(DeleteFramebuffers)
    QGAME_GL_LOAD(CheckFramebufferStatus)
    QGAME_GL_LOAD(DrawElementsBaseVertex)
    QGAME_GL_LOAD(FenceSync)               QGAME_GL_LOAD(ClientWaitSync)
    QGAME_GL_LOAD(DeleteSync)
    #undef QGAME_GL_LOAD

    return ok;
}

// ── GLSL shaders (embedded, no compile step) ─────────────────────────────────

static const char* k_vertSrc = R"(
#version 330 core
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
uniform mat4 uProj;
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = uProj * vec4(inPos, 0.0, 1.0);
    vUV    = inUV;
    vColor = inColor;
}
)";

static const char* k_fragSrc = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTexture;
uniform sampler2D uRegionTex;        // R8 region ID 图（无则绑 dummy）
uniform vec4      uRegionTints[16];  // 每个 region ID 对应的颜色（multiply）
uniform int       uHasRegion;        // 0 = 不染色（passthrough）
out vec4 fragColor;
void main() {
    vec4 base = texture(uTexture, vUV) * vColor;
    if (uHasRegion != 0) {
        float idF = texture(uRegionTex, vUV).r;     // 0..1
        int id = int(idF * 255.0 + 0.5);
        if (id > 0 && id < 16) {
            base *= uRegionTints[id];
        }
    }
    fragColor = base;
}
)";

static const char* k_msdfFragSrc = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTexture;
uniform float uPxRange = 4.0;
out vec4 fragColor;

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec3 sampleVal = texture(uTexture, vUV).rgb;
    float sigDist = median(sampleVal.r, sampleVal.g, sampleVal.b) - 0.5;
    float opacity = clamp(sigDist * uPxRange + 0.5, 0.0, 1.0);
    fragColor = vec4(vColor.rgb, vColor.a * opacity);
}
)";

// ── ctor/dtor ─────────────────────────────────────────────────────────────────

GLRenderDevice::GLRenderDevice(SDL_Window* window , bool debug)
    : window_(window), debug_(debug) {
    batchVerts_.reserve(MAX_SPRITES_PER_BATCH * 4);
    batchIdx_.reserve(MAX_SPRITES_PER_BATCH * 6);
    debug_ = debug;
}

GLRenderDevice::~GLRenderDevice() {
    shutdown();
}

// ── IBackendSystem ────────────────────────────────────────────────────────────

void GLRenderDevice::init() {
    // GL context
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);   // 2D game, no depth buffer

    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_) {
        core::logError("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return;
    }
    SDL_GL_MakeCurrent(window_, glContext_);
    SDL_GL_SetSwapInterval(1);

    // Must load after context is current — wglGetProcAddress requires an active context
    if (!loadGLFunctions()) {
        core::logError("GLRenderDevice: one or more GL 3.x functions unavailable");
        return;
    }

    core::logInfo("OpenGL version: %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    core::logInfo("GLSL version:   %s", reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    createShaderProgram();
    createBuffers();

    // OpenGL is a correctness/reference backend for this roadmap. Keep these
    // capabilities intentionally conservative so advanced GPU features only
    // activate through the SDL_GPU path until explicitly ported and tested.
    capabilities_.supportsCompute = false;
    capabilities_.supportsStorageBuffer = false;
    capabilities_.supportsStorageTexture = false;
    capabilities_.supportsGPUDrivenSprite = false;
    capabilities_.supportsIndirectDraw = false;
    capabilities_.supportsTextureArray = false;
    capabilities_.supportsTimestampQuery = false;
    capabilities_.supportsWorldOffscreenColor = true;
    capabilities_.supportsSampledRenderTarget = true;
    // OpenGL does not support the compute lighting path, but it now provides a
    // visible L5 reflection fallback in submitLighting2DPass(). Keep compute
    // capabilities false; this flag only tells demos/tools that the high-level
    // 2D lighting hook produces an effect on this backend.
    capabilities_.supportsLighting2D = true;
    capabilities_.backendName = "OpenGL";

    // 1×1 R8 dummy region 纹理（id=0），无 region 时绑定它
    {
        TextureDesc dd{};
        const uint8_t zero = 0;
        dd.data = &zero;
        dd.width = 1;
        dd.height = 1;
        dd.channels = 1;
        dd.format = TextureFormat::R8;
        dd.filter = TextureFilter::Nearest;
        dummyRegionTex_ = createTexture(dd);
    }
    {
        // OpenGL fallback reflections are ordinary alpha-blended quads. Keeping
        // a dedicated white texture avoids depending on demo assets and lets the
        // lighting fallback run in any scene that has Light2D/Reflector2D data.
        TextureDesc dd{};
        const uint8_t white[4] = {255, 255, 255, 255};
        dd.data = white;
        dd.width = 1;
        dd.height = 1;
        dd.channels = 4;
        dd.format = TextureFormat::RGBA8;
        dd.filter = TextureFilter::Linear;
        lightingFallbackWhiteTex_ = createTexture(dd);
    }
    {
        constexpr int kSize = 128;
        std::vector<uint8_t> pixels(static_cast<size_t>(kSize * kSize * 4), 0);
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(kSize) * 2.f - 1.f;
                const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(kSize) * 2.f - 1.f;
                const float d = std::sqrt(nx * nx + ny * ny);
                const float falloff = std::max(0.f, 1.f - d);
                const float alpha = falloff * falloff;
                const size_t idx = static_cast<size_t>((y * kSize + x) * 4);
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 255;
                pixels[idx + 2] = 255;
                pixels[idx + 3] = static_cast<uint8_t>(std::min(255.f, alpha * 255.f + 0.5f));
            }
        }

        TextureDesc dd{};
        dd.data = pixels.data();
        dd.width = kSize;
        dd.height = kSize;
        dd.channels = 4;
        dd.format = TextureFormat::RGBA8;
        dd.filter = TextureFilter::Linear;
        lightingFallbackRadialTex_ = createTexture(dd);
    }

    core::logInfo("GLRenderDevice initialized");
}

void GLRenderDevice::beginFrame() {
    // Nothing to do — GL has no explicit frame acquire
    resetFrameStats();
}

void GLRenderDevice::endFrame() {
}

void GLRenderDevice::shutdown() {
    if (!glContext_) return;

    SDL_GL_MakeCurrent(window_, glContext_);

    if (screenFbo_.fbo)    destroyFbo(screenFbo_,    screenTarget_);
    if (offscreenFbo_.fbo) destroyFbo(offscreenFbo_, offscreenTarget_);

    if (dummyRegionTex_.valid()) { destroyTexture(dummyRegionTex_); dummyRegionTex_ = {}; }
    if (lightingFallbackWhiteTex_.valid()) {
        destroyTexture(lightingFallbackWhiteTex_);
        lightingFallbackWhiteTex_ = {};
    }
    if (lightingFallbackRadialTex_.valid()) {
        destroyTexture(lightingFallbackRadialTex_);
        lightingFallbackRadialTex_ = {};
    }

    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (ibo_) { glDeleteBuffers(1, &ibo_); ibo_ = 0; }
    if (shaderProgram_) { glDeleteProgram(shaderProgram_); shaderProgram_ = 0; }

    SDL_GL_DestroyContext(glContext_);
    glContext_ = nullptr;
    core::logInfo("GLRenderDevice shutdown");
}

// ── 资源管理 ──────────────────────────────────────────────────────────────────

TextureHandle GLRenderDevice::createTexture(const TextureDesc& desc) {
    ASSERT(desc.data && desc.width > 0 && desc.height > 0);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    const GLint filter = (desc.filter == TextureFilter::Linear) ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (desc.format == TextureFormat::R8) {
        // R8 单通道：用于 region ID 图。pack alignment 改为 1 防止行对齐截断。
        GLint prevAlign = 4;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
                     desc.width, desc.height, 0,
                     GL_RED, GL_UNSIGNED_BYTE, desc.data);
        glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     desc.width, desc.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, desc.data);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    return textures_.insert(TextureEntry{ tex, desc.width, desc.height });
}

void GLRenderDevice::destroyTexture(TextureHandle h) {
    if (!textures_.valid(h)) return;
    TextureEntry& e = textures_.get(h);
    if (e.glTex) glDeleteTextures(1, &e.glTex);
    textures_.remove(h);
}

ShaderHandle GLRenderDevice::createShader(const ShaderDesc&) { return {}; }
void         GLRenderDevice::destroyShader(ShaderHandle)     {}

engine::FontHandle GLRenderDevice::createFont(const engine::FontData& fontData) {
    engine::FontData data = fontData;
    return fonts_.insert(std::move(data));
}

void GLRenderDevice::destroyFont(engine::FontHandle h) {
    if (fonts_.valid(h)) {
        fonts_.remove(h);
    }
}

const engine::FontData* GLRenderDevice::getFont(engine::FontHandle h) const {
    return fonts_.valid(h) ? &fonts_.get(h) : nullptr;
}

// ── Buffer 管理 ────────────────────────────────────────────────────────────────

BufferHandle GLRenderDevice::createBuffer(const BufferDesc& desc) {
    if (desc.size == 0) return {};

    GLenum target = GL_ARRAY_BUFFER;
    if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(BufferUsage::Index)) {
        target = GL_ELEMENT_ARRAY_BUFFER;
    } else if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(BufferUsage::Indirect)) {
        target = 0x8F3F; // GL_DRAW_INDIRECT_BUFFER
    }

    unsigned int glBuf = 0;
    glGenBuffers(1, &glBuf);
    glBindBuffer(target, glBuf);

    GLenum glUsage = GL_DYNAMIC_DRAW;
    if (desc.initialData) {
        glBufferData(target, static_cast<GLsizeiptr>(desc.size), desc.initialData, glUsage);
    } else {
        glBufferData(target, static_cast<GLsizeiptr>(desc.size), nullptr, glUsage);
    }

    glBindBuffer(target, 0);

    return buffers_.insert(BufferEntry{ glBuf, desc.size, desc.usage });
}

void GLRenderDevice::destroyBuffer(BufferHandle h) {
    if (!buffers_.valid(h)) return;
    BufferEntry& entry = buffers_.get(h);
    if (entry.glBuffer) {
        glDeleteBuffers(1, &entry.glBuffer);
    }
    buffers_.remove(h);
}

void* GLRenderDevice::mapBuffer(BufferHandle) {
    return nullptr;
}

void GLRenderDevice::unmapBuffer(BufferHandle) {
}

void GLRenderDevice::uploadToBuffer(BufferHandle, const void*, size_t, size_t) {
}

void GLRenderDevice::downloadFromBuffer(BufferHandle, void*, size_t, size_t) {
}

ComputePipelineHandle GLRenderDevice::createComputePipeline(const ComputePipelineDesc&) {
    return {};
}

void GLRenderDevice::destroyComputePipeline(ComputePipelineHandle) {
}

// ── 帧控制 ────────────────────────────────────────────────────────────────────

void GLRenderDevice::submitCommandBuffer(const CommandBuffer& cb) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    renderCommandBufferToTarget(cb, 0 /*default framebuffer*/, w, h);
}

void GLRenderDevice::submitPass(const PassSubmitInfo& info,
                                const std::vector<const RenderCmd*>& cmds) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    CameraData cam = info.camera;
    if (cam.viewportW == 0) cam.viewportW = w;
    if (cam.viewportH == 0) cam.viewportH = h;
    renderCmdsToTarget(cmds, cam, info.clearEnabled, info.clearColor, 0, w, h);
}

void GLRenderDevice::present() {
    SDL_GL_SwapWindow(window_);
}

// ── 离屏渲染 ──────────────────────────────────────────────────────────────────

TextureHandle GLRenderDevice::renderToTexture(const CommandBuffer& cb, int w, int h) {
    if (w <= 0 || h <= 0) return {};
    if (!ensureFbo(screenFbo_, screenTarget_, w, h)) return {};
    renderCommandBufferToTarget(cb, screenFbo_.fbo, w, h);
    return screenTarget_;
}

TextureHandle GLRenderDevice::renderToTextureOffscreen(const CommandBuffer& cb, int w, int h) {
    if (w <= 0 || h <= 0) return {};
    if (!ensureFbo(offscreenFbo_, offscreenTarget_, w, h)) return {};

    renderCommandBufferToTarget(cb, offscreenFbo_.fbo, w, h);

    // 等待本次渲染完成（对应 SDL_GPU backend 的 fence 语义）
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ULL);
    glDeleteSync(fence);

    return offscreenTarget_;
}

void* GLRenderDevice::getRawTexture(TextureHandle handle) const {
    if (!textures_.valid(handle)) return nullptr;
    const TextureEntry& e = textures_.get(handle);
    // OpenGL backend expects the texture ID as a GLuint
    return reinterpret_cast<void*>(static_cast<uintptr_t>(e.glTex));
}

bool GLRenderDevice::getTextureDimensions(TextureHandle handle, int& outW, int& outH) const {
    if (!textures_.valid(handle)) return false;
    const TextureEntry& e = textures_.get(handle);
    outW = e.width;
    outH = e.height;
    return true;
}

// ── 内部：初始化辅助 ──────────────────────────────────────────────────────────

void GLRenderDevice::createShaderProgram() {
    auto compileShader = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            core::logError("Shader compile error: %s", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };

    GLuint vs = compileShader(GL_VERTEX_SHADER,   k_vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, k_fragSrc);
    ASSERT_MSG(vs && fs, "GL shader compile failed");

    shaderProgram_ = glCreateProgram();
    glAttachShader(shaderProgram_, vs);
    glAttachShader(shaderProgram_, fs);
    glLinkProgram(shaderProgram_);

    GLint ok = 0;
    glGetProgramiv(shaderProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(shaderProgram_, sizeof(log), nullptr, log);
        core::logError("Shader link error: %s", log);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    uProjLoc_        = glGetUniformLocation(shaderProgram_, "uProj");
    uTexLoc_         = glGetUniformLocation(shaderProgram_, "uTexture");
    uRegionTexLoc_   = glGetUniformLocation(shaderProgram_, "uRegionTex");
    uRegionTintsLoc_ = glGetUniformLocation(shaderProgram_, "uRegionTints");
    uHasRegionLoc_   = glGetUniformLocation(shaderProgram_, "uHasRegion");
    ASSERT_MSG(uProjLoc_ >= 0 && uTexLoc_ >= 0, "GL uniform location not found");
    
    GLuint msdfFs = compileShader(GL_FRAGMENT_SHADER, k_msdfFragSrc);
    ASSERT_MSG(msdfFs, "MSDF shader compile failed");
    
    msdfShaderProgram_ = glCreateProgram();
    glAttachShader(msdfShaderProgram_, vs);
    glAttachShader(msdfShaderProgram_, msdfFs);
    glLinkProgram(msdfShaderProgram_);
    
    glGetProgramiv(msdfShaderProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(msdfShaderProgram_, sizeof(log), nullptr, log);
        core::logError("MSDF shader link error: %s", log);
    }
    
    glDeleteShader(msdfFs);
    
    msdfProjLoc_    = glGetUniformLocation(msdfShaderProgram_, "uProj");
    msdfTexLoc_     = glGetUniformLocation(msdfShaderProgram_, "uTexture");
    msdfPxRangeLoc_ = glGetUniformLocation(msdfShaderProgram_, "uPxRange");
    ASSERT_MSG(msdfProjLoc_ >= 0 && msdfTexLoc_ >= 0 && msdfPxRangeLoc_ >= 0, 
               "MSDF shader uniform location not found");
}

void GLRenderDevice::createBuffers() {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ibo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);

    // layout: pos(2f) | uv(2f) | color(4ub)
    const GLsizei stride = sizeof(SpriteVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT,         GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT,         GL_FALSE, stride, reinterpret_cast<void*>(8));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,  stride, reinterpret_cast<void*>(16));

    glBindVertexArray(0);
}

// ── 内部：FBO 管理 ────────────────────────────────────────────────────────────

bool GLRenderDevice::ensureFbo(FboEntry& fbo, TextureHandle& colorHandle, int w, int h) {
    if (fbo.fbo && fbo.width == w && fbo.height == h) return true;
    if (fbo.fbo) destroyFbo(fbo, colorHandle);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    colorHandle = textures_.insert(TextureEntry{ tex, w, h });

    GLuint fboId = 0;
    glGenFramebuffers(1, &fboId);
    glBindFramebuffer(GL_FRAMEBUFFER, fboId);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        core::logError("GLRenderDevice: FBO incomplete (%dx%d)", w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fboId);
        destroyTexture(colorHandle);
        colorHandle = {};
        fbo = {};
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    core::logInfo("GLRenderDevice: FBO created %dx%d", w, h);
    fbo = { fboId, colorHandle, w, h };
    return true;
}

void GLRenderDevice::destroyFbo(FboEntry& fbo, TextureHandle& colorHandle) {
    if (fbo.fbo) { glDeleteFramebuffers(1, &fbo.fbo); fbo.fbo = 0; }
    if (colorHandle.valid()) { destroyTexture(colorHandle); colorHandle = {}; }
    fbo.width = fbo.height = 0;
}

// ── 内部：渲染核心 ────────────────────────────────────────────────────────────

void GLRenderDevice::renderCommandBufferToTarget(const CommandBuffer& cb,
                                                  unsigned int fbo,
                                                  int width, int height) {
    // 兼容 editor 路径：从命令流中提取 ClearCmd/SetCameraCmd，转交给指针版本
    std::vector<const RenderCmd*> cmdPtrs;
    cmdPtrs.reserve(cb.commands().size());
    core::Color clearColor = core::Color::Black;
    CameraData camera{};
    camera.viewportW = width;
    camera.viewportH = height;

    for (const auto& cmd : cb.commands()) {
        if (std::holds_alternative<ClearCmd>(cmd)) {
            clearColor = std::get<ClearCmd>(cmd).color;
        } else if (std::holds_alternative<SetCameraCmd>(cmd)) {
            camera = std::get<SetCameraCmd>(cmd).camera;
        } else {
            cmdPtrs.push_back(&cmd);
        }
    }
    renderCmdsToTarget(cmdPtrs, camera, true, clearColor, fbo, width, height);
}

void GLRenderDevice::renderCmdsToTarget(const std::vector<const RenderCmd*>& cmds,
                                         const CameraData& cameraIn,
                                         bool clearEnabled,
                                         core::Color clearColor,
                                         unsigned int fbo,
                                         int width, int height) {
    CameraData camera = cameraIn;
    if (camera.viewportW == 0) camera.viewportW = width;
    if (camera.viewportH == 0) camera.viewportH = height;

    batchVerts_.clear();
    batchIdx_.clear();
    std::vector<BatchSegment> batches;

    TextureHandle currentTex{};
    bool          hasCurrent = false;
    bool          currentIsFont = false;
    float         currentPxRange = 4.0f;
    uint32_t      batchIdxStart  = 0;
    int32_t       batchVertStart = 0;

    // Region tint 当前批次状态
    bool                              currentHasRegion = false;
    TextureHandle                     currentRegionTex{};
    std::array<core::Color, 16>       currentRegionTints{};

    struct ScissorRect { int x, y, w, h; };
    std::vector<ScissorRect> scissorStack;
    bool currentHasScissor = false;
    ScissorRect currentScissor{};

    auto flush = [&]() {
        if (static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
            BatchSegment seg{};
            seg.tex        = currentTex;
            seg.idxOffset  = batchIdxStart;
            seg.idxCount   = static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart;
            seg.vertOffset = batchVertStart;
            seg.isFont     = currentIsFont;
            seg.pxRange    = currentPxRange;
            seg.hasScissor = currentHasScissor;
            if (currentHasScissor) {
                seg.scissorX = currentScissor.x;
                seg.scissorY = currentScissor.y;
                seg.scissorW = currentScissor.w;
                seg.scissorH = currentScissor.h;
            }
            seg.hasRegion   = currentHasRegion;
            seg.regionTex   = currentRegionTex;
            seg.regionTints = currentRegionTints;
            batches.push_back(seg);
            batchIdxStart  = static_cast<uint32_t>(batchIdx_.size());
            batchVertStart = static_cast<int32_t>(batchVerts_.size());
        }
    };

    auto regionStateDiffers = [&](bool hasRegion, TextureHandle regionTex,
                                  const std::array<core::Color, 16>* tints) {
        if (currentHasRegion != hasRegion) return true;
        if (!hasRegion) return false;
        if (currentRegionTex != regionTex) return true;
        // memcmp 比较 16 个 RGBA8 (64 字节)
        return std::memcmp(currentRegionTints.data(), tints->data(),
                           sizeof(core::Color) * 16) != 0;
    };

    auto maybeFlush = [&](TextureHandle tex, bool isFont = false, float pxRange = 4.0f,
                          bool hasRegion = false, TextureHandle regionTex = {},
                          const std::array<core::Color, 16>* regionTints = nullptr) {
        const bool batchFull =
            (batchVerts_.size() - static_cast<size_t>(batchVertStart) >= MAX_SPRITES_PER_BATCH * 4);
        const bool regionDiff = regionStateDiffers(hasRegion, regionTex, regionTints);
        if (!hasCurrent || tex != currentTex || batchFull ||
            currentIsFont != isFont || (isFont && currentPxRange != pxRange) ||
            regionDiff) {
            flush();
            currentTex = tex;
            currentIsFont = isFont;
            currentPxRange = pxRange;
            currentHasRegion = hasRegion;
            currentRegionTex = regionTex;
            if (hasRegion && regionTints) {
                currentRegionTints = *regionTints;
            } else {
                currentRegionTints = {};
            }
            hasCurrent = true;
        }
    };

    auto pushQuad = [&](float x0, float y0, float x1, float y1,
                        float x2, float y2, float x3, float y3,
                        float u0, float v0, float u1, float v1,
                        const core::Color& tint)
    {
        const auto base = static_cast<uint16_t>(batchVerts_.size() - static_cast<size_t>(batchVertStart));
        batchVerts_.push_back({ x0, y0, u0, v0, tint.r, tint.g, tint.b, tint.a });
        batchVerts_.push_back({ x1, y1, u1, v0, tint.r, tint.g, tint.b, tint.a });
        batchVerts_.push_back({ x2, y2, u1, v1, tint.r, tint.g, tint.b, tint.a });
        batchVerts_.push_back({ x3, y3, u0, v1, tint.r, tint.g, tint.b, tint.a });
        batchIdx_.insert(batchIdx_.end(), {
            base,
            static_cast<uint16_t>(base + 1),
            static_cast<uint16_t>(base + 2),
            base,
            static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 3)
        });
    };

    for (const RenderCmd* cmd : cmds) {
        if (auto* s = std::get_if<DrawSpriteCmd>(cmd)) {
            maybeFlush(s->texture, false, 4.0f,
                       s->hasRegion, s->regionTex, &s->regionTints);
            const float hw = s->srcRect.w * s->scaleX * 0.5f;
            const float hh = s->srcRect.h * s->scaleY * 0.5f;
            const float cosR = cosf(s->rotation);
            const float sinR = sinf(s->rotation);
            const float lx[4] = { -hw,  hw,  hw, -hw };
            const float ly[4] = { -hh, -hh,  hh,  hh };
            const TextureEntry* entry = textures_.tryGet(s->texture);
            const float tw = entry ? static_cast<float>(entry->width)  : 1.f;
            const float th = entry ? static_cast<float>(entry->height) : 1.f;
            const float u0 =  s->srcRect.x              / tw;
            const float v0 =  s->srcRect.y              / th;
            const float u1 = (s->srcRect.x + s->srcRect.w) / tw;
            const float v1 = (s->srcRect.y + s->srcRect.h) / th;
            float px[4], py[4];
            for (int i = 0; i < 4; ++i) {
                px[i] = s->x + lx[i] * cosR - ly[i] * sinR;
                py[i] = s->y + lx[i] * sinR + ly[i] * cosR;
            }
            pushQuad(px[0],py[0], px[1],py[1], px[2],py[2], px[3],py[3],
                     u0,v0, u1,v1, s->tint);
        }
        else if (auto* t = std::get_if<DrawTileCmd>(cmd)) {
            maybeFlush(t->tileset);
            const TextureEntry* entry = textures_.tryGet(t->tileset);
            const float tw = entry ? static_cast<float>(entry->width)  : 1.f;
            const float th = entry ? static_cast<float>(entry->height) : 1.f;
            const int   ts = t->tileSize > 0 ? t->tileSize : 16;
            int tilesetCols = static_cast<int>(tw) / ts;
            if (tilesetCols < 1) tilesetCols = 1;
            const int col = t->tileId % tilesetCols;
            const int row = t->tileId / tilesetCols;
            const float u0 = (col * ts) / tw;
            const float v0 = (row * ts) / th;
            const float u1 = u0 + ts / tw;
            const float v1 = v0 + ts / th;
            const float px  = static_cast<float>(t->gridX * ts);
            const float py  = static_cast<float>(t->gridY * ts);
            const float px1 = px + ts;
            const float py1 = py + ts;
            pushQuad(px,py, px1,py, px1,py1, px,py1, u0,v0, u1,v1,
                     core::Color{255,255,255,255});
        }
        else if (auto* ps = std::get_if<PushScissorCmd>(cmd)) {
            ScissorRect r{
                static_cast<int>(ps->rect.x),
                static_cast<int>(ps->rect.y),
                static_cast<int>(ps->rect.w),
                static_cast<int>(ps->rect.h)
            };
            if (!scissorStack.empty()) {
                const ScissorRect& top = scissorStack.back();
                const int x0 = std::max(r.x, top.x);
                const int y0 = std::max(r.y, top.y);
                const int x1 = std::min(r.x + r.w, top.x + top.w);
                const int y1 = std::min(r.y + r.h, top.y + top.h);
                r.x = x0; r.y = y0;
                r.w = std::max(0, x1 - x0);
                r.h = std::max(0, y1 - y0);
            }
            scissorStack.push_back(r);
            flush();
            currentHasScissor = true;
            currentScissor    = r;
            continue;
        }
        else if (auto* /*pp*/ pp = std::get_if<PopScissorCmd>(cmd)) {
            (void)pp;
            if (!scissorStack.empty()) scissorStack.pop_back();
            flush();
            if (scissorStack.empty()) {
                currentHasScissor = false;
            } else {
                currentHasScissor = true;
                currentScissor    = scissorStack.back();
            }
            continue;
        }
        else if (auto* text = std::get_if<DrawTextCmd>(cmd)) {
            const engine::FontData* font = getFont(text->font);
            if (!font || !textures_.valid(font->texture)) continue;

            const float scale = text->fontSize / font->fontSize;
            const float camZoom = (camera.zoom > 0.f) ? camera.zoom : 1.f;
            const float screenPxRange = font->pxRange * scale * camZoom;
            maybeFlush(font->texture, true, screenPxRange);

            float cursorX = text->x;
            float cursorY = text->y;
            const std::string& s = text->text;

            for (size_t i = 0; i < s.size();) {
                uint32_t cp = 0;
                unsigned char c0 = static_cast<unsigned char>(s[i]);
                size_t adv = 1;
                if (c0 < 0x80) { cp = c0; adv = 1; }
                else if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
                    cp = (c0 & 0x1F) << 6 | (static_cast<unsigned char>(s[i+1]) & 0x3F);
                    adv = 2;
                } else if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
                    cp = (c0 & 0x0F) << 12
                       | (static_cast<unsigned char>(s[i+1]) & 0x3F) << 6
                       | (static_cast<unsigned char>(s[i+2]) & 0x3F);
                    adv = 3;
                } else if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
                    cp = (c0 & 0x07) << 18
                       | (static_cast<unsigned char>(s[i+1]) & 0x3F) << 12
                       | (static_cast<unsigned char>(s[i+2]) & 0x3F) << 6
                       | (static_cast<unsigned char>(s[i+3]) & 0x3F);
                    adv = 4;
                } else {
                    cp = 0xFFFD; adv = 1;
                }
                i += adv;

                const engine::Glyph* glyph = font->getGlyph(cp);
                if (!glyph) {
                    cursorX += font->fontSize * 0.5f * scale;
                    continue;
                }

                const float x0 = cursorX + glyph->bearingX * scale;
                const float y0 = cursorY - glyph->bearingY * scale;
                const float x1 = x0 + glyph->width * scale;
                const float y1 = y0 + glyph->height * scale;

                pushQuad(x0, y0, x1, y0, x1, y1, x0, y1,
                         glyph->u0, glyph->v0, glyph->u1, glyph->v1,
                         text->color);

                cursorX += glyph->advance * scale;
            }
        }
    }
    flush();

    // --- GL state setup ---
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    if (clearEnabled) {
        glClearColor(clearColor.r / 255.f, clearColor.g / 255.f,
                     clearColor.b / 255.f, clearColor.a / 255.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    if (batchVerts_.empty()) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // Upload vertex + index data (buffer orphaning for streaming)
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(batchVerts_.size() * sizeof(SpriteVertex)),
                 batchVerts_.data(), GL_STREAM_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(batchIdx_.size() * sizeof(uint16_t)),
                 batchIdx_.data(), GL_STREAM_DRAW);
    frameStats_.uploadBytes +=
        static_cast<uint64_t>(batchVerts_.size() * sizeof(SpriteVertex)) +
        static_cast<uint64_t>(batchIdx_.size() * sizeof(uint16_t));
    frameStats_.uploadCallCount++;

    float proj[16];
    float view[16];
    const float zoom = (camera.zoom > 0.f) ? camera.zoom : 1.f;
    buildOrthoProjectionMatrix(static_cast<float>(width), static_cast<float>(height), proj);
    buildViewMatrix(camera.x, camera.y, zoom, camera.rotation, view);

    float mvp[16];
    // 列主序：mvp = proj * view
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            mvp[i * 4 + j] = 0.f;
            for (int k = 0; k < 4; ++k) {
                mvp[i * 4 + j] += view[i * 4 + k] * proj[k * 4 + j];
            }
        }
    }

    glUseProgram(shaderProgram_);
    glUniformMatrix4fv(uProjLoc_, 1, GL_FALSE, mvp);
    glUniform1i(uTexLoc_, 0);
    if (uRegionTexLoc_ >= 0) glUniform1i(uRegionTexLoc_, 1);
    if (uHasRegionLoc_ >= 0) glUniform1i(uHasRegionLoc_, 0);
    glActiveTexture(GL_TEXTURE0);

    bool scissorEnabled = false;
    for (const BatchSegment& seg : batches) {
        if (seg.isFont) {
            glUseProgram(msdfShaderProgram_);
            glUniformMatrix4fv(msdfProjLoc_, 1, GL_FALSE, mvp);
            glUniform1i(msdfTexLoc_, 0);
            glUniform1f(msdfPxRangeLoc_, seg.pxRange);
        } else {
            glUseProgram(shaderProgram_);
            glUniformMatrix4fv(uProjLoc_, 1, GL_FALSE, mvp);
            glUniform1i(uTexLoc_, 0);
            if (uRegionTexLoc_ >= 0) glUniform1i(uRegionTexLoc_, 1);
            if (uHasRegionLoc_ >= 0) glUniform1i(uHasRegionLoc_, seg.hasRegion ? 1 : 0);
            if (uRegionTintsLoc_ >= 0) {
                // 把 16 个 RGBA8 转成 16 个 vec4
                float tintsF[16 * 4];
                for (int i = 0; i < 16; ++i) {
                    tintsF[i*4 + 0] = seg.regionTints[i].r / 255.f;
                    tintsF[i*4 + 1] = seg.regionTints[i].g / 255.f;
                    tintsF[i*4 + 2] = seg.regionTints[i].b / 255.f;
                    tintsF[i*4 + 3] = seg.regionTints[i].a / 255.f;
                }
                glUniform4fv(uRegionTintsLoc_, 16, tintsF);
            }
            // 绑 region 纹理到 TEXTURE1（无则用 dummy，避免 sampler 未绑定 UB）
            glActiveTexture(GL_TEXTURE1);
            TextureHandle rtex = (seg.hasRegion && textures_.valid(seg.regionTex))
                ? seg.regionTex : dummyRegionTex_;
            if (textures_.valid(rtex)) {
                glBindTexture(GL_TEXTURE_2D, textures_.get(rtex).glTex);
                frameStats_.textureBindCount++;
            }
            glActiveTexture(GL_TEXTURE0);
        }

        if (seg.hasScissor) {
            // GL scissor 原点在左下角，引擎屏幕坐标 y 向下，所以要翻转 y。
            const int x0 = std::max(0, seg.scissorX);
            const int y0 = std::max(0, seg.scissorY);
            const int x1 = std::min(width,  seg.scissorX + seg.scissorW);
            const int y1 = std::min(height, seg.scissorY + seg.scissorH);
            const int sw = std::max(0, x1 - x0);
            const int sh = std::max(0, y1 - y0);
            const int glY = height - y1;
            if (!scissorEnabled) { glEnable(GL_SCISSOR_TEST); scissorEnabled = true; }
            glScissor(x0, glY, sw, sh);
        } else if (scissorEnabled) {
            glDisable(GL_SCISSOR_TEST);
            scissorEnabled = false;
        }

        if (textures_.valid(seg.tex)) {
            glBindTexture(GL_TEXTURE_2D, textures_.get(seg.tex).glTex);
            frameStats_.textureBindCount++;
        }
        glDrawElementsBaseVertex(
            GL_TRIANGLES,
            static_cast<GLsizei>(seg.idxCount),
            GL_UNSIGNED_SHORT,
            reinterpret_cast<const void*>(seg.idxOffset * sizeof(uint16_t)),
            seg.vertOffset
        );
        frameStats_.drawCallCount++;
    }
    if (scissorEnabled) glDisable(GL_SCISSOR_TEST);

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ── 内部：矩阵运算 ──────────────────────────────────────────────────────────

void GLRenderDevice::buildOrthoProjectionMatrix(float w, float h, float out[16]) {
    const float left   = -w * 0.5f;
    const float right  =  w * 0.5f;
    const float top    =  h * 0.5f;
    const float bottom = -h * 0.5f;

    memset(out, 0, 16 * sizeof(float));
    out[0]  =  2.f / (right - left);
    out[5]  = -2.f / (top - bottom);
    out[10] =  1.f;
    out[12] = -(right + left)  / (right - left);
    out[13] = -(top + bottom)  / (top - bottom);
    out[15] =  1.f;
}

void GLRenderDevice::buildViewMatrix(float camX, float camY, float zoom, float rotation, float out[16]) {
    const float c = cosf(rotation);
    const float s = sinf(rotation);

    memset(out, 0, 16 * sizeof(float));
    out[0]  =  c * zoom;
    out[1]  =  s * zoom;
    out[4]  = -s * zoom;
    out[5]  =  c * zoom;
    out[10] = 1.f;
    out[12] = -( c * camX - s * camY) * zoom;
    out[13] = -( s * camX + c * camY) * zoom;
    out[15] = 1.f;
}

void GLRenderDevice::submitGPUDrivenPass(const PassSubmitInfo&,
                                         const GPURenderParams&) {
    // GPU-driven pass not supported in CPU rendering mode
}

void GLRenderDevice::submitGPUParticlePass(const PassSubmitInfo&,
                                           const GPUParticleParams&) {
    // GPU particle pass intentionally stays unsupported on the OpenGL fallback.
}

void GLRenderDevice::submitLighting2DPass(const PassSubmitInfo& info,
                                          const Lighting2DParams& params) {
    if (!lightingFallbackWhiteTex_.valid() && !lightingFallbackRadialTex_.valid()) return;
    if (!params.enabled || params.lights.empty()) return;

    // OpenGL fallback for L5 reflections.
    //
    // The SDL_GPU/Vulkan path produces lighting/reflection in a compute-written
    // overlay texture. Old OpenGL 3.3 machines do not have the storage texture
    // path, but they can still show the authored Reflector2D intent by drawing
    // a few translucent quads after the World pass and before the UI camera.
    //
    // This is intentionally not a full compute-equivalent lighting implementation:
    // - Light2D decides the reflected color and brightness.
    // - LightOccluder2D has already been expanded by RenderSystem into world
    //   segments; each segment extrudes a translucent shadow quad away from the
    //   light. This is a geometric fallback, not per-pixel ray casting, but it
    //   makes blockers visibly cast shadows on old OpenGL-only machines.
    // - Reflector2D AABB produces vertical wet-road/water streaks.
    // - Reflector2D Segment produces a thin shimmering water-edge highlight.
    // - Environment2D::wetness is the scene-wide weather multiplier.
    //
    // Because RenderSystem invokes this pass only for World cameras, and UI/Text
    // are drawn by the later UI camera/pass, the fallback keeps the L5 contract:
    // reflections do not affect UI/Text readability.
    CameraData cam = params.camera;
    if (cam.viewportW == 0) cam.viewportW = info.camera.viewportW;
    if (cam.viewportH == 0) cam.viewportH = info.camera.viewportH;
    if (cam.viewportW <= 0 || cam.viewportH <= 0) return;
    frameStats_.lighting2DSubmitCount++;

    auto clamp01 = [](float v) {
        return std::max(0.f, std::min(1.f, v));
    };
    auto toByte = [&](float v) -> uint8_t {
        return static_cast<uint8_t>(std::max(0.f, std::min(255.f, v * 255.f + 0.5f)));
    };
    auto luma = [](const Light2DPoint& l) {
        return l.colorR * 0.2126f + l.colorG * 0.7152f + l.colorB * 0.0722f;
    };

    static std::vector<RenderCmd> overlayCmds;
    static std::vector<const RenderCmd*> overlayPtrs;
    overlayCmds.clear();
    overlayPtrs.clear();

    if (lightingFallbackWhiteTex_.valid()) {
        const float zoom = (cam.zoom > 0.f) ? cam.zoom : 1.f;
        const float visibleWorldW = static_cast<float>(cam.viewportW) / zoom;
        const float visibleWorldH = static_cast<float>(cam.viewportH) / zoom;
        const float darkness = std::min(0.58f,
                                        std::max(0.18f, 0.54f - params.ambientIntensity * 0.35f));

        DrawSpriteCmd ambient{};
        ambient.texture = lightingFallbackWhiteTex_;
        ambient.x = cam.x;
        ambient.y = cam.y;
        ambient.scaleX = visibleWorldW;
        ambient.scaleY = visibleWorldH;
        ambient.pivotX = 0.5f;
        ambient.pivotY = 0.5f;
        ambient.srcRect = core::Rect{0.f, 0.f, 1.f, 1.f};
        ambient.layer = 9998;
        ambient.pass = engine::RenderPass::World;
        ambient.tint = core::Color{0, 0, 0, toByte(darkness)};
        overlayCmds.push_back(ambient);
    }

    if (lightingFallbackRadialTex_.valid()) {
        const TextureEntry& radial = textures_.get(lightingFallbackRadialTex_);
        for (const Light2DPoint& light : params.lights) {
            if (light.radius <= 0.f || light.intensity <= 0.f) continue;
            if ((light.layerMask & engine::renderPassBit(engine::RenderPass::World)) == 0u) continue;

            const float alpha01 = std::min(0.82f,
                                           std::max(0.06f, 0.10f + light.intensity * 0.18f)) *
                                  clamp01(light.colorA);
            if (alpha01 <= 0.001f) continue;

            DrawSpriteCmd cmd{};
            cmd.texture = lightingFallbackRadialTex_;
            cmd.x = light.x;
            cmd.y = light.y;
            cmd.scaleX = (light.radius * 2.f) / static_cast<float>(radial.width);
            cmd.scaleY = (light.radius * 2.f) / static_cast<float>(radial.height);
            cmd.pivotX = 0.5f;
            cmd.pivotY = 0.5f;
            cmd.srcRect = core::Rect{0.f, 0.f,
                                     static_cast<float>(radial.width),
                                     static_cast<float>(radial.height)};
            cmd.layer = 9999;
            cmd.pass = engine::RenderPass::World;
            cmd.tint = core::Color{
                toByte(light.colorR),
                toByte(light.colorG),
                toByte(light.colorB),
                toByte(alpha01)
            };
            overlayCmds.push_back(cmd);
        }
    }

    batchVerts_.clear();
    batchIdx_.clear();
    auto pushShadowVolume = [&](float ax, float ay,
                                float bx, float by,
                                float farBx, float farBy,
                                float farAx, float farAy,
                                uint8_t alpha) {
        // Shadows need real quadrilateral geometry. Using DrawSpriteCmd would
        // collapse this into a rectangular sprite and produce the blocky tiles
        // the user reported. Here we write the four vertices directly into the
        // GL streaming buffers; the existing sprite shader can still draw them
        // because it only needs position, UV, and vertex color.
        const uint16_t base = static_cast<uint16_t>(batchVerts_.size());
        batchVerts_.push_back({ ax,    ay,    0.f, 0.f, 4, 7, 13, alpha });
        batchVerts_.push_back({ bx,    by,    1.f, 0.f, 4, 7, 13, alpha });
        batchVerts_.push_back({ farBx, farBy, 1.f, 1.f, 4, 7, 13, 0 });
        batchVerts_.push_back({ farAx, farAy, 0.f, 1.f, 4, 7, 13, 0 });
        batchIdx_.insert(batchIdx_.end(), {
            base,
            static_cast<uint16_t>(base + 1),
            static_cast<uint16_t>(base + 2),
            base,
            static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 3)
        });
    };

    auto drawShadowBatch = [&]() {
        if (batchVerts_.empty()) return;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, cam.viewportW, cam.viewportH);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(batchVerts_.size() * sizeof(SpriteVertex)),
                     batchVerts_.data(), GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(batchIdx_.size() * sizeof(uint16_t)),
                     batchIdx_.data(), GL_STREAM_DRAW);
        frameStats_.uploadBytes +=
            static_cast<uint64_t>(batchVerts_.size() * sizeof(SpriteVertex)) +
            static_cast<uint64_t>(batchIdx_.size() * sizeof(uint16_t));
        frameStats_.uploadCallCount++;

        float proj[16];
        float view[16];
        float mvp[16];
        const float zoom = (cam.zoom > 0.f) ? cam.zoom : 1.f;
        buildOrthoProjectionMatrix(static_cast<float>(cam.viewportW),
                                   static_cast<float>(cam.viewportH), proj);
        buildViewMatrix(cam.x, cam.y, zoom, cam.rotation, view);
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                mvp[i * 4 + j] = 0.f;
                for (int k = 0; k < 4; ++k) {
                    mvp[i * 4 + j] += view[i * 4 + k] * proj[k * 4 + j];
                }
            }
        }

        glUseProgram(shaderProgram_);
        glUniformMatrix4fv(uProjLoc_, 1, GL_FALSE, mvp);
        glUniform1i(uTexLoc_, 0);
        if (uHasRegionLoc_ >= 0) glUniform1i(uHasRegionLoc_, 0);
        if (uRegionTexLoc_ >= 0) glUniform1i(uRegionTexLoc_, 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures_.get(lightingFallbackWhiteTex_).glTex);
        frameStats_.textureBindCount++;

        glDrawElementsBaseVertex(GL_TRIANGLES,
                                 static_cast<GLsizei>(batchIdx_.size()),
                                 GL_UNSIGNED_SHORT,
                                 reinterpret_cast<const void*>(0),
                                 0);
        frameStats_.drawCallCount++;
        glBindVertexArray(0);
        glUseProgram(0);
        batchVerts_.clear();
        batchIdx_.clear();
    };

    // Shadow fallback. We generate it before reflections so wet/water highlights
    // can still appear over darkened ground, matching the SDL_GPU overlay order.
    for (const Light2DPoint& light : params.lights) {
        if (light.radius <= 0.f || light.intensity <= 0.f || light.castsShadow == 0u) continue;
        if ((light.layerMask & engine::renderPassBit(engine::RenderPass::World)) == 0u) continue;

        for (const Light2DSegment& seg : params.segments) {
            const float sx = seg.bx - seg.ax;
            const float sy = seg.by - seg.ay;
            const float segLenSq = sx * sx + sy * sy;
            if (segLenSq < 1.f || seg.opacity <= 0.f) continue;

            const float midX = (seg.ax + seg.bx) * 0.5f;
            const float midY = (seg.ay + seg.by) * 0.5f;
            const float toMidX = midX - light.x;
            const float toMidY = midY - light.y;
            const float midDist = std::sqrt(toMidX * toMidX + toMidY * toMidY);
            if (midDist >= light.radius) continue;

            // RenderSystem expands AABB occluders in a stable clockwise order.
            // The outward normal below points away from the blocker. Casting
            // only the edge whose outward normal is away from the light avoids
            // the previous "all four sides cast a block" artifact and keeps the
            // shadow from starting on the front face of the object.
            float nx = sy;
            float ny = -sx;
            const float nLen = std::max(0.0001f, std::sqrt(nx * nx + ny * ny));
            nx /= nLen;
            ny /= nLen;
            const float lightSide = (light.x - midX) * nx + (light.y - midY) * ny;
            if (lightSide >= 0.f) continue;

            const float remaining = std::max(0.f, light.radius - midDist);
            const float shadowLen = remaining * (0.75f + 0.25f * clamp01(light.intensity));
            if (shadowLen <= 1.f) continue;

            // Extrude both endpoints away from the light. This approximates the
            // same geometric idea as ray-cast shadow volumes, but stays within
            // a single alpha quad per segment so old GL hardware can handle it.
            auto extrudePoint = [&](float x, float y, float& ox, float& oy) {
                float vx = x - light.x;
                float vy = y - light.y;
                const float vl = std::max(0.0001f, std::sqrt(vx * vx + vy * vy));
                vx /= vl;
                vy /= vl;
                ox = x + vx * shadowLen;
                oy = y + vy * shadowLen;
            };

            // Start a little behind the casting edge. Since this fallback is
            // composited after world geometry, this offset prevents the shadow
            // from visibly painting over the occluder itself.
            const float startOffset = std::min(42.f, std::max(10.f, shadowLen * 0.18f));
            float sax = 0.f, say = 0.f, sbx = 0.f, sby = 0.f;
            float ex0 = 0.f, ey0 = 0.f, ex1 = 0.f, ey1 = 0.f;
            auto offsetPoint = [&](float x, float y, float dist, float& ox, float& oy) {
                float vx = x - light.x;
                float vy = y - light.y;
                const float vl = std::max(0.0001f, std::sqrt(vx * vx + vy * vy));
                vx /= vl;
                vy /= vl;
                ox = x + vx * dist;
                oy = y + vy * dist;
            };
            offsetPoint(seg.ax, seg.ay, startOffset, sax, say);
            offsetPoint(seg.bx, seg.by, startOffset, sbx, sby);
            extrudePoint(seg.ax, seg.ay, ex0, ey0);
            extrudePoint(seg.bx, seg.by, ex1, ey1);

            const float attenuation = 1.f - clamp01(midDist / light.radius);
            const float alpha = clamp01(seg.opacity) * attenuation *
                                (0.18f + 0.12f * clamp01(light.intensity));
            if (alpha <= 0.01f) continue;

            // Draw a dark blue-black shadow, not pure black. This preserves the
            // night-scene ambient floor and avoids making old-GL fallback harsher
            // than the compute path.
            pushShadowVolume(sax, say, sbx, sby, ex1, ey1, ex0, ey0, toByte(alpha));
        }
    }

    drawShadowBatch();

    const float sceneWetness = clamp01(params.wetness);
    if (sceneWetness > 0.f) {
        for (const Reflector2DRegion& refl : params.reflectors) {
            if (!refl.visible || refl.reflectivity <= 0.f) continue;

            for (const Light2DPoint& light : params.lights) {
                if (light.radius <= 0.f || light.intensity <= 0.f) continue;
                if ((light.layerMask & engine::renderPassBit(engine::RenderPass::World)) == 0u) continue;

                const float sourceBrightness = luma(light) * light.intensity;
                const float brightGate = clamp01((sourceBrightness - 0.12f) / 0.70f);
                if (brightGate <= 0.f) continue;

                const float roughness = clamp01(refl.roughness);
                const float baseStrength =
                    clamp01(refl.reflectivity) * sceneWetness * brightGate *
                    std::max(0.f, light.intensity);
                if (baseStrength <= 0.f) continue;

                const float tintR = light.colorR * refl.tintR * refl.tintA;
                const float tintG = light.colorG * refl.tintG * refl.tintA;
                const float tintB = light.colorB * refl.tintB * refl.tintA;

                if (refl.shape == 1u) {
                    // AABB wet patch. The reflector's a point is its center,
                    // which matches RenderSystem's upload for
                    // Reflector2D::Shape::AABB. We draw several nested vertical
                    // streaks so roughness reads as blur/spread even on
                    // fixed-function OpenGL blending.
                    const float halfW = std::max(1.f, refl.width * 0.5f);
                    const float halfH = std::max(1.f, refl.height * 0.5f);
                    const float minX = refl.ax - halfW;
                    const float maxX = refl.ax + halfW;
                    const float topY = refl.ay - halfH;
                    if (light.x < minX - light.radius * 0.25f ||
                        light.x > maxX + light.radius * 0.25f) {
                        continue;
                    }

                    const float lightAbove =
                        clamp01((topY - light.y + light.radius * 0.18f) /
                                std::max(light.radius, 1.f));
                    if (lightAbove <= 0.f) continue;

                    const int taps = 5;
                    for (int i = 0; i < taps; ++i) {
                        const float t = static_cast<float>(i) / static_cast<float>(taps - 1);
                        const float spread = 1.f + t * (1.2f + roughness * 2.4f);
                        const float width = (18.f + light.radius * (0.08f + roughness * 0.28f)) * spread;
                        const float height = refl.height * (1.08f - t * 0.11f);
                        const float y = topY + height * 0.48f + t * refl.height * 0.05f;
                        const float x = std::max(minX, std::min(maxX, light.x));
                        const float fade = std::exp(-t * (1.15f + roughness));
                        const float alpha = baseStrength * lightAbove * fade *
                                            (0.34f - roughness * 0.10f);
                        if (alpha <= 0.01f) continue;

                        DrawSpriteCmd cmd{};
                        cmd.texture = lightingFallbackWhiteTex_;
                        cmd.x = x;
                        cmd.y = y;
                        cmd.scaleX = width;
                        cmd.scaleY = height;
                        cmd.pivotX = 0.5f;
                        cmd.pivotY = 0.5f;
                        cmd.srcRect = core::Rect{0.f, 0.f, 1.f, 1.f};
                        cmd.layer = 10000 + i;
                        cmd.pass = engine::RenderPass::World;
                        cmd.tint = core::Color{
                            toByte(tintR),
                            toByte(tintG),
                            toByte(tintB),
                            toByte(clamp01(alpha))
                        };
                        overlayCmds.push_back(cmd);
                    }
                } else {
                    // Segment reflector. Draw a rotated translucent strip
                    // centered on the segment, strongest near the light's
                    // projection onto the segment.
                    const float ax = refl.ax;
                    const float ay = refl.ay;
                    const float bx = refl.bx;
                    const float by = refl.by;
                    const float dx = bx - ax;
                    const float dy = by - ay;
                    const float lenSq = std::max(dx * dx + dy * dy, 1.f);
                    const float len = std::sqrt(lenSq);
                    const float t = clamp01(((light.x - ax) * dx + (light.y - ay) * dy) / lenSq);
                    const float px = ax + dx * t;
                    const float py = ay + dy * t;
                    const float alongWidth = std::min(len, 36.f + light.radius * (0.18f + roughness * 0.5f));
                    const float thickness = 5.f + roughness * 36.f;
                    const float sideDist = std::abs((light.x - px) * (-dy / len) + (light.y - py) * (dx / len));
                    const float sideWeight = clamp01(sideDist / std::max(light.radius * 0.35f, 1.f));
                    const float alpha = baseStrength * sideWeight * (0.28f - roughness * 0.08f);
                    if (alpha <= 0.01f) continue;

                    DrawSpriteCmd cmd{};
                    cmd.texture = lightingFallbackWhiteTex_;
                    cmd.x = px;
                    cmd.y = py;
                    cmd.rotation = std::atan2(dy, dx);
                    cmd.scaleX = alongWidth;
                    cmd.scaleY = thickness;
                    cmd.pivotX = 0.5f;
                    cmd.pivotY = 0.5f;
                    cmd.srcRect = core::Rect{0.f, 0.f, 1.f, 1.f};
                    cmd.layer = 10050;
                    cmd.pass = engine::RenderPass::World;
                    cmd.tint = core::Color{
                        toByte(tintR),
                        toByte(tintG),
                        toByte(tintB),
                        toByte(clamp01(alpha))
                    };
                    overlayCmds.push_back(cmd);
                }
            }
        }
    }

    if (overlayCmds.empty()) return;
    overlayPtrs.reserve(overlayCmds.size());
    for (const RenderCmd& cmd : overlayCmds) {
        overlayPtrs.push_back(&cmd);
    }

    renderCmdsToTarget(overlayPtrs, cam, false, core::Color::Black, 0,
                       cam.viewportW, cam.viewportH);
}

void GLRenderDevice::submitWorldLightingGraph(const WorldLightingSubmitInfo& info) {
    // OpenGL keeps a compatibility implementation for the new RenderGraph-facing
    // interface. It does not yet allocate a separate WorldColor FBO/composite
    // shader; instead it preserves the existing correctness fallback while
    // reporting graph-shaped stats. This keeps RenderSystem able to call one
    // interface across backends and makes the SDL GPU graph migration isolated.
    frameStats_.renderGraphPassCount += info.uiCommands.empty() ? 3u : 4u;
    submitPass(info.worldPass, info.worldCommands);
    frameStats_.worldColorPassCount++;
    submitLighting2DPass(info.worldPass, info.lighting);
    frameStats_.lightingCompositeCount++;
    if (!info.uiCommands.empty()) {
        PassSubmitInfo uiPass = info.worldPass;
        uiPass.clearEnabled = false;
        submitPass(uiPass, info.uiCommands);
        frameStats_.uiPassCount++;
    }
}

} // namespace backend
