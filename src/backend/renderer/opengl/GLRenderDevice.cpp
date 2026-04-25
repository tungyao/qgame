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

// GL 4.3+ Compute shader
static PFNGLDISPATCHCOMPUTEPROC             s_glDispatchCompute         = nullptr;
static PFNGLMEMORYBARRIERPROC               s_glMemoryBarrier           = nullptr;
static PFNGLBINDBUFFERBASEPROC              s_glBindBufferBase          = nullptr;
static PFNGLBINDBUFFERRANGEPROC             s_glBindBufferRange         = nullptr;
static PFNGLMAPBUFFERRANGEPROC              s_glMapBufferRange          = nullptr;
static PFNGLUNMAPBUFFERPROC                 s_glUnmapBuffer             = nullptr;
static PFNGLBUFFERSUBDATAPROC               s_glBufferSubData           = nullptr;
static void (*s_glGetIntegerv)(GLenum, GLint*) = nullptr;

// GL compute capability flag
static bool s_hasCompute = false;

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
#define glDispatchCompute         s_glDispatchCompute
#define glMemoryBarrier           s_glMemoryBarrier
#define glBindBufferBase          s_glBindBufferBase
#define glBindBufferRange         s_glBindBufferRange
#define glMapBufferRange          s_glMapBufferRange
#define glUnmapBuffer             s_glUnmapBuffer
#define glBufferSubData           s_glBufferSubData
#define glGetIntegerv             s_glGetIntegerv


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

    // GL 4.3+ compute shader functions (optional)
    QGAME_GL_LOAD_OPTIONAL(DispatchCompute)
    QGAME_GL_LOAD_OPTIONAL(MemoryBarrier)
    QGAME_GL_LOAD_OPTIONAL(BindBufferBase)
    QGAME_GL_LOAD_OPTIONAL(BindBufferRange)
    QGAME_GL_LOAD_OPTIONAL(MapBufferRange)
    QGAME_GL_LOAD_OPTIONAL(UnmapBuffer)
    QGAME_GL_LOAD_OPTIONAL(BufferSubData)
    QGAME_GL_LOAD_OPTIONAL(GetIntegerv)
    #undef QGAME_GL_LOAD_OPTIONAL

    // Check for GL 4.3+ compute support
    s_hasCompute = s_glDispatchCompute && s_glMemoryBarrier && s_glBindBufferBase;
    if (s_hasCompute) {
        int major = 0, minor = 0;
        s_glGetIntegerv(0x821B /*GL_MAJOR_VERSION*/, &major);
        s_glGetIntegerv(0x821C /*GL_MINOR_VERSION*/, &minor);
        core::logInfo("GL compute shader support: GL %d.%d", major, minor);
    } else {
        core::logInfo("GL compute shader not available (requires GL 4.3+)");
    }

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
out vec4 fragColor;
void main() {
    fragColor = texture(uTexture, vUV) * vColor;
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

GLRenderDevice::GLRenderDevice(SDL_Window* window)
    : window_(window) {
    batchVerts_.reserve(MAX_SPRITES_PER_BATCH * 4);
    batchIdx_.reserve(MAX_SPRITES_PER_BATCH * 6);
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

    core::logInfo("GLRenderDevice initialized");
}

void GLRenderDevice::beginFrame() {
    // Nothing to do — GL has no explicit frame acquire
}

void GLRenderDevice::endFrame() {
}

void GLRenderDevice::shutdown() {
    if (!glContext_) return;

    SDL_GL_MakeCurrent(window_, glContext_);

    if (screenFbo_.fbo)    destroyFbo(screenFbo_,    screenTarget_);
    if (offscreenFbo_.fbo) destroyFbo(offscreenFbo_, offscreenTarget_);

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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 desc.width, desc.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, desc.data);
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
    } else if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(BufferUsage::Storage)) {
        target = 0x90F2; // GL_SHADER_STORAGE_BUFFER
    } else if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(BufferUsage::Indirect)) {
        target = 0x8F3F; // GL_DRAW_INDIRECT_BUFFER
    } else if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(BufferUsage::Uniform)) {
        target = 0x8A11; // GL_UNIFORM_BUFFER
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

void* GLRenderDevice::mapBuffer(BufferHandle h) {
    if (!buffers_.valid(h) || !s_glMapBufferRange) return nullptr;
    BufferEntry& entry = buffers_.get(h);

    GLenum target = GL_ARRAY_BUFFER;
    if (static_cast<uint32_t>(entry.usage) & static_cast<uint32_t>(BufferUsage::Storage)) {
        target = 0x90F2; // GL_SHADER_STORAGE_BUFFER
    }

    glBindBuffer(target, entry.glBuffer);
    void* ptr = glMapBufferRange(target, 0, static_cast<GLsizeiptr>(entry.size),
                                  GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    return ptr;
}

void GLRenderDevice::unmapBuffer(BufferHandle h) {
    if (!buffers_.valid(h) || !s_glUnmapBuffer) return;
    BufferEntry& entry = buffers_.get(h);

    GLenum target = GL_ARRAY_BUFFER;
    if (static_cast<uint32_t>(entry.usage) & static_cast<uint32_t>(BufferUsage::Storage)) {
        target = 0x90F2;
    }

    glBindBuffer(target, entry.glBuffer);
    glUnmapBuffer(target);
    glBindBuffer(target, 0);
}

void GLRenderDevice::uploadToBuffer(BufferHandle h, const void* data, size_t size, size_t offset) {
    if (!buffers_.valid(h) || !data || size == 0) return;
    BufferEntry& entry = buffers_.get(h);
    if (offset + size > entry.size) return;

    GLenum target = GL_ARRAY_BUFFER;
    if (static_cast<uint32_t>(entry.usage) & static_cast<uint32_t>(BufferUsage::Storage)) {
        target = 0x90F2;
    }

    glBindBuffer(target, entry.glBuffer);
    if (s_glMapBufferRange) {
        void* ptr = glMapBufferRange(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size),
                                      GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
        if (ptr) {
            memcpy(ptr, data, size);
            glUnmapBuffer(target);
        }
    } else {
        // Fallback: orphan and re-upload
        glBufferData(target, static_cast<GLsizeiptr>(entry.size), nullptr, GL_DYNAMIC_DRAW);
        glBufferSubData(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
    }
    glBindBuffer(target, 0);
}

void GLRenderDevice::downloadFromBuffer(BufferHandle h, void* data, size_t size, size_t offset) {
    if (!buffers_.valid(h) || !data || size == 0) return;
    BufferEntry& entry = buffers_.get(h);
    if (offset + size > entry.size) return;

    GLenum target = GL_ARRAY_BUFFER;
    if (static_cast<uint32_t>(entry.usage) & static_cast<uint32_t>(BufferUsage::Storage)) {
        target = 0x90F2;
    }

    glBindBuffer(target, entry.glBuffer);
    if (s_glMapBufferRange) {
        void* ptr = glMapBufferRange(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size),
                                      GL_MAP_READ_BIT);
        if (ptr) {
            memcpy(data, ptr, size);
            glUnmapBuffer(target);
        }
    }
    glBindBuffer(target, 0);
}

// ── Compute Pipeline 管理 ──────────────────────────────────────────────────────

ComputePipelineHandle GLRenderDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    if (!s_hasCompute || !desc.code || desc.codeSize == 0) {
        core::logError("createComputePipeline: compute not supported or invalid desc");
        return {};
    }

    const char* src = static_cast<const char*>(desc.code);
    unsigned int cs = glCreateShader(0x91B9); // GL_COMPUTE_SHADER
    glShaderSource(cs, 1, &src, nullptr);
    glCompileShader(cs);

    int compiled = 0;
    glGetShaderiv(cs, 0x8B81 /*GL_COMPILE_STATUS*/, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(cs, sizeof(log), nullptr, log);
        core::logError("Compute shader compile error: %s", log);
        glDeleteShader(cs);
        return {};
    }

    unsigned int program = glCreateProgram();
    glAttachShader(program, cs);
    glLinkProgram(program);

    int linked = 0;
    glGetProgramiv(program, 0x8B82 /*GL_LINK_STATUS*/, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        core::logError("Compute program link error: %s", log);
        glDeleteShader(cs);
        glDeleteProgram(program);
        return {};
    }

    glDeleteShader(cs);
    return computePipelines_.insert(ComputePipelineEntry{ program });
}

void GLRenderDevice::destroyComputePipeline(ComputePipelineHandle h) {
    if (!computePipelines_.valid(h)) return;
    ComputePipelineEntry& entry = computePipelines_.get(h);
    if (entry.program) {
        glDeleteProgram(entry.program);
    }
    computePipelines_.remove(h);
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

    uProjLoc_ = glGetUniformLocation(shaderProgram_, "uProj");
    uTexLoc_  = glGetUniformLocation(shaderProgram_, "uTexture");
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

    // Process compute commands first
    for (const RenderCmd* cmd : cmds) {
        if (auto* d = std::get_if<DispatchCmd>(cmd)) {
            if (!s_hasCompute || !computePipelines_.valid(d->pipeline)) continue;
            ComputePipelineEntry& pe = computePipelines_.get(d->pipeline);

            glUseProgram(pe.program);

            uint32_t bindingIndex = 0;

            // Bind readonly storage buffers
            for (uint32_t i = 0; i < d->bindings.readonlyStorageBufferCount && i < 8; ++i) {
                if (buffers_.valid(d->bindings.readonlyStorageBuffers[i])) {
                    glBindBufferBase(0x90F2, bindingIndex,
                                     buffers_.get(d->bindings.readonlyStorageBuffers[i]).glBuffer);
                    ++bindingIndex;
                }
            }

            // Bind readwrite storage buffers
            for (uint32_t i = 0; i < d->bindings.readwriteStorageBufferCount && i < 8; ++i) {
                if (buffers_.valid(d->bindings.readwriteStorageBuffers[i])) {
                    glBindBufferBase(0x90F2, bindingIndex,
                                     buffers_.get(d->bindings.readwriteStorageBuffers[i]).glBuffer);
                    ++bindingIndex;
                }
            }

            // Bind sampled textures
            for (uint32_t i = 0; i < d->bindings.sampledTextureCount && i < 8; ++i) {
                if (textures_.valid(d->bindings.sampledTextures[i])) {
                    glActiveTexture(GL_TEXTURE0 + i);
                    glBindTexture(GL_TEXTURE_2D, textures_.get(d->bindings.sampledTextures[i]).glTex);
                }
            }

            glDispatchCompute(d->groupCountX, d->groupCountY, d->groupCountZ);
        }
        else if (auto* b = std::get_if<BarrierCmd>(cmd)) {
            if (!s_hasCompute) continue;
            GLbitfield barriers = 0;
            switch (b->type) {
                case BarrierCmd::Type::Memory:
                    barriers = GL_ALL_BARRIER_BITS;
                    break;
                case BarrierCmd::Type::StorageBuffer:
                    barriers = 0x2000; // GL_SHADER_STORAGE_BARRIER_BIT
                    break;
                case BarrierCmd::Type::Texture:
                    barriers = 0x0800; // GL_TEXTURE_UPDATE_BARRIER_BIT
                    break;
            }
            glMemoryBarrier(barriers);
        }
    }

    batchVerts_.clear();
    batchIdx_.clear();
    std::vector<BatchSegment> batches;

    TextureHandle currentTex{};
    bool          hasCurrent = false;
    bool          currentIsFont = false;
    float         currentPxRange = 4.0f;
    uint32_t      batchIdxStart  = 0;
    int32_t       batchVertStart = 0;

    auto flush = [&]() {
        if (static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
            batches.push_back({ currentTex, batchIdxStart,
                                static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart,
                                batchVertStart, currentIsFont, currentPxRange });
            batchIdxStart  = static_cast<uint32_t>(batchIdx_.size());
            batchVertStart = static_cast<int32_t>(batchVerts_.size());
        }
    };

    auto maybeFlush = [&](TextureHandle tex, bool isFont = false, float pxRange = 4.0f) {
        const bool batchFull =
            (batchVerts_.size() - static_cast<size_t>(batchVertStart) >= MAX_SPRITES_PER_BATCH * 4);
        if (!hasCurrent || tex != currentTex || batchFull ||
            currentIsFont != isFont || (isFont && currentPxRange != pxRange)) {
            flush();
            currentTex = tex;
            currentIsFont = isFont;
            currentPxRange = pxRange;
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
            maybeFlush(s->texture);
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
    glActiveTexture(GL_TEXTURE0);

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
        }
        
        if (textures_.valid(seg.tex)) {
            glBindTexture(GL_TEXTURE_2D, textures_.get(seg.tex).glTex);
        }
        glDrawElementsBaseVertex(
            GL_TRIANGLES,
            static_cast<GLsizei>(seg.idxCount),
            GL_UNSIGNED_SHORT,
            reinterpret_cast<const void*>(seg.idxOffset * sizeof(uint16_t)),
            seg.vertOffset
        );
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ── 内部：几何构建（与 SDLGPURenderDevice 逻辑完全相同）───────────────────────

void GLRenderDevice::buildSpriteGeometry(const std::vector<DrawSpriteCmd>& cmds,
                                          std::vector<BatchSegment>& batches) {
    if (cmds.empty()) return;

    TextureHandle currentTex{};
    uint32_t batchIdxStart  = 0;
    int32_t  batchVertStart = 0;

    for (const DrawSpriteCmd& cmd : cmds) {
        const bool needFlush =
            (cmd.texture != currentTex) ||
            (batchVerts_.size() - static_cast<size_t>(batchVertStart) >= MAX_SPRITES_PER_BATCH * 4);

        if (needFlush && !batchVerts_.empty() &&
            static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
            batches.push_back({ currentTex, batchIdxStart,
                                static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart,
                                batchVertStart });
            batchIdxStart  = static_cast<uint32_t>(batchIdx_.size());
            batchVertStart = static_cast<int32_t>(batchVerts_.size());
        }
        currentTex = cmd.texture;

        const float hw = cmd.srcRect.w * cmd.scaleX * 0.5f;
        const float hh = cmd.srcRect.h * cmd.scaleY * 0.5f;
        const float cosR = cosf(cmd.rotation);
        const float sinR = sinf(cmd.rotation);
        const float lx[4] = { -hw,  hw,  hw, -hw };
        const float ly[4] = { -hh, -hh,  hh,  hh };

        const TextureEntry* entry = textures_.tryGet(currentTex);
        const float tw = entry ? static_cast<float>(entry->width)  : 1.f;
        const float th = entry ? static_cast<float>(entry->height) : 1.f;
        const float u0 =  cmd.srcRect.x              / tw;
        const float v0 =  cmd.srcRect.y              / th;
        const float u1 = (cmd.srcRect.x + cmd.srcRect.w) / tw;
        const float v1 = (cmd.srcRect.y + cmd.srcRect.h) / th;
        const float us[4] = { u0, u1, u1, u0 };
        const float vs[4] = { v0, v0, v1, v1 };

        const auto base = static_cast<uint16_t>(batchVerts_.size() - static_cast<size_t>(batchVertStart));
        for (int i = 0; i < 4; ++i) {
            batchVerts_.push_back({
                cmd.x + lx[i] * cosR - ly[i] * sinR,
                cmd.y + lx[i] * sinR + ly[i] * cosR,
                us[i], vs[i],
                cmd.tint.r, cmd.tint.g, cmd.tint.b, cmd.tint.a
            });
        }
        batchIdx_.insert(batchIdx_.end(), {
            base,
            static_cast<uint16_t>(base + 1),
            static_cast<uint16_t>(base + 2),
            base,
            static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 3)
        });
    }

    if (static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
        batches.push_back({ currentTex, batchIdxStart,
                            static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart,
                            batchVertStart });
    }
}

void GLRenderDevice::buildTileGeometry(const std::vector<DrawTileCmd>& cmds,
                                        std::vector<BatchSegment>& batches) {
    if (cmds.empty()) return;

    TextureHandle currentTex{};
    uint32_t batchIdxStart  = 0;
    int32_t  batchVertStart = 0;

    for (const DrawTileCmd& cmd : cmds) {
        const bool needFlush =
            (cmd.tileset != currentTex) ||
            (batchVerts_.size() - static_cast<size_t>(batchVertStart) >= MAX_SPRITES_PER_BATCH * 4);

        if (needFlush && !batchVerts_.empty() &&
            static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
            batches.push_back({ currentTex, batchIdxStart,
                                static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart,
                                batchVertStart });
            batchIdxStart  = static_cast<uint32_t>(batchIdx_.size());
            batchVertStart = static_cast<int32_t>(batchVerts_.size());
        }
        currentTex = cmd.tileset;

        const TextureEntry* entry = textures_.tryGet(currentTex);
        const float tw = entry ? static_cast<float>(entry->width)  : 1.f;
        const float th = entry ? static_cast<float>(entry->height) : 1.f;
        const int   ts = cmd.tileSize > 0 ? cmd.tileSize : 16;

        int tilesetCols = static_cast<int>(tw) / ts;
        if (tilesetCols < 1) tilesetCols = 1;
        const int col = cmd.tileId % tilesetCols;
        const int row = cmd.tileId / tilesetCols;

        const float u0 = (col * ts) / tw;
        const float v0 = (row * ts) / th;
        const float u1 = u0 + ts / tw;
        const float v1 = v0 + ts / th;

        const float px  = static_cast<float>(cmd.gridX * ts);
        const float py  = static_cast<float>(cmd.gridY * ts);
        const float px1 = px + ts;
        const float py1 = py + ts;

        const auto base = static_cast<uint16_t>(batchVerts_.size() - static_cast<size_t>(batchVertStart));
        batchVerts_.push_back({ px,  py,  u0, v0, 255, 255, 255, 255 });
        batchVerts_.push_back({ px1, py,  u1, v0, 255, 255, 255, 255 });
        batchVerts_.push_back({ px1, py1, u1, v1, 255, 255, 255, 255 });
        batchVerts_.push_back({ px,  py1, u0, v1, 255, 255, 255, 255 });
        batchIdx_.insert(batchIdx_.end(), {
            base,
            static_cast<uint16_t>(base + 1),
            static_cast<uint16_t>(base + 2),
            base,
            static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 3)
        });
    }

    if (static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
        batches.push_back({ currentTex, batchIdxStart,
                            static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart,
                            batchVertStart });
    }
}

void GLRenderDevice::buildOrthoMatrix(float w, float h, float out[16]) {
    buildOrthoProjectionMatrix(w, h, out);
}

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
    // 标准 2D view：eye = R * zoom * (world - cam)，列主序
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

void GLRenderDevice::buildOrthoMatrixCamera(float w, float h,
                                             float camX, float camY, float zoom,
                                             float rotation,
                                             float out[16]) {
    float proj[16];
    float view[16];
    buildOrthoProjectionMatrix(w, h, proj);
    buildViewMatrix(camX, camY, zoom, rotation, view);

    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i * 4 + j] = 0.f;
            for (int k = 0; k < 4; ++k) {
                out[i * 4 + j] += view[i * 4 + k] * proj[k * 4 + j];
            }
        }
    }
}

void GLRenderDevice::submitGPUDrivenPass(const PassSubmitInfo& info,
                                         const GPURenderParams& params) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    
    if (info.clearEnabled) {
        glClearColor(info.clearColor.r / 255.f, info.clearColor.g / 255.f,
                     info.clearColor.b / 255.f, info.clearColor.a / 255.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    
    if (params.visibleCount == 0) {
        return;
    }
    
    if (!buffers_.valid(params.spriteBuffer) || !buffers_.valid(params.visibleIndexBuffer)) {
        return;
    }
    
    BufferEntry& spriteBuf = buffers_.get(params.spriteBuffer);
    BufferEntry& indexBuf = buffers_.get(params.visibleIndexBuffer);
    
    std::vector<uint32_t> visibleIndices(params.visibleCount);
    glBindBuffer(GL_COPY_READ_BUFFER, indexBuf.glBuffer);
    void* mapped = glMapBufferRange(GL_COPY_READ_BUFFER, 0, params.visibleCount * sizeof(uint32_t),
                                     GL_MAP_READ_BIT);
    if (mapped) {
        memcpy(visibleIndices.data(), mapped, params.visibleCount * sizeof(uint32_t));
        glUnmapBuffer(GL_COPY_READ_BUFFER);
    }
    
    glBindBuffer(GL_COPY_READ_BUFFER, spriteBuf.glBuffer);
    std::vector<uint8_t> spriteData(spriteBuf.size);
    mapped = glMapBufferRange(GL_COPY_READ_BUFFER, 0, spriteBuf.size, GL_MAP_READ_BIT);
    if (mapped) {
        memcpy(spriteData.data(), mapped, spriteBuf.size);
        glUnmapBuffer(GL_COPY_READ_BUFFER);
    }
    
    engine::GPUSprite* sprites = reinterpret_cast<engine::GPUSprite*>(spriteData.data());
    
    batchVerts_.clear();
    batchIdx_.clear();
    std::vector<BatchSegment> batches;
    TextureHandle currentTex{};
    bool hasCurrent = false;
    uint32_t batchIdxStart = 0;
    int32_t batchVertStart = 0;
    
    auto flush = [&]() {
        if (static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
            batches.push_back({ currentTex, batchIdxStart,
                                static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart,
                                batchVertStart, false, 4.0f });
            batchIdxStart = static_cast<uint32_t>(batchIdx_.size());
            batchVertStart = static_cast<int32_t>(batchVerts_.size());
        }
    };
    
    for (uint32_t i = 0; i < params.visibleCount; ++i) {
        uint32_t spriteIdx = visibleIndices[i];
        if (spriteIdx >= params.spriteCount) continue;
        
        engine::GPUSprite& s = sprites[spriteIdx];
        
        TextureHandle texHandle;
        texHandle.index = s.textureIndex;
        
        if (!textures_.valid(texHandle)) continue;
        
        if (!hasCurrent || texHandle != currentTex) {
            flush();
            currentTex = texHandle;
            hasCurrent = true;
        }
        
        if (static_cast<size_t>(batchVertStart) + 4 >= MAX_SPRITES_PER_BATCH * 4) {
            flush();
        }
        
        float tx = s.transform[3];
        float ty = s.transform[7];
        float m00 = s.transform[0];
        float m01 = s.transform[1];
        float m10 = s.transform[4];
        float m11 = s.transform[5];
        
        float hw = std::abs(m00) * 0.5f;
        float hh = std::abs(m11) * 0.5f;
        
        float x0 = tx - hw, y0 = ty - hh;
        float x1 = tx + hw, y1 = ty + hh;
        
        float u0 = s.uv[0], v0 = s.uv[1];
        float u1 = s.uv[2], v1 = s.uv[3];
        
        uint8_t r = static_cast<uint8_t>(s.color[0] * 255);
        uint8_t g = static_cast<uint8_t>(s.color[1] * 255);
        uint8_t b = static_cast<uint8_t>(s.color[2] * 255);
        uint8_t a = static_cast<uint8_t>(s.color[3] * 255);
        
        const auto base = static_cast<uint16_t>(batchVerts_.size() - static_cast<size_t>(batchVertStart));
        batchVerts_.push_back({ x0, y0, u0, v0, r, g, b, a });
        batchVerts_.push_back({ x1, y0, u1, v0, r, g, b, a });
        batchVerts_.push_back({ x1, y1, u1, v1, r, g, b, a });
        batchVerts_.push_back({ x0, y1, u0, v1, r, g, b, a });
        batchIdx_.insert(batchIdx_.end(), {
            base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
            base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)
        });
    }
    flush();
    
    if (batchVerts_.empty()) return;
    
    glUseProgram(shaderProgram_);
    
    float proj[16];
    float view[16];
    float mvp[16];
    const float zoom = (info.camera.zoom > 0.f) ? info.camera.zoom : 1.f;
    buildOrthoProjectionMatrix(static_cast<float>(w), static_cast<float>(h), proj);
    buildViewMatrix(info.camera.x, info.camera.y, zoom, info.camera.rotation, view);
    
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            mvp[i * 4 + j] = 0.f;
            for (int k = 0; k < 4; ++k) {
                mvp[i * 4 + j] += view[i * 4 + k] * proj[k * 4 + j];
            }
        }
    }
    glUniformMatrix4fv(uProjLoc_, 1, GL_FALSE, mvp);
    
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, batchVerts_.size() * sizeof(SpriteVertex),
                 batchVerts_.data(), GL_STREAM_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, batchIdx_.size() * sizeof(uint16_t),
                 batchIdx_.data(), GL_STREAM_DRAW);
    
    for (const auto& batch : batches) {
        if (!textures_.valid(batch.tex)) continue;
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures_.get(batch.tex).glTex);
        
        glDrawElementsBaseVertex(GL_TRIANGLES, batch.idxCount, GL_UNSIGNED_SHORT,
                                 reinterpret_cast<void*>(batch.idxOffset * sizeof(uint16_t)),
                                 batch.vertOffset);
    }
    
    glBindVertexArray(0);
}

} // namespace backend
