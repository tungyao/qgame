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


#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>

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
out vec4 fragColor;
void main() {
    fragColor = texture(uTexture, vUV) * vColor;
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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

void GLRenderDevice::submitImGuiDrawData(const ImDrawData* drawData) {
    if (!drawData) return;
    ImGui_ImplOpenGL3_RenderDrawData(const_cast<ImDrawData*>(drawData));
}

void GLRenderDevice::present() {
    SDL_GL_SwapWindow(window_);
}

void GLRenderDevice::initImGui() {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));

    ImGui_ImplSDL3_InitForOpenGL(window_, glContext_);
    ImGui_ImplOpenGL3_Init("#version 330 core");
}

void GLRenderDevice::shutdownImGui() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
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
    // ImGui on OpenGL expects the texture ID as a GLuint cast to ImTextureID
    return reinterpret_cast<void*>(static_cast<uintptr_t>(e.glTex));
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

    // 按命令流顺序构建 batch：texture 变化或 batch 满时 flush。
    // 不再按 cmd 类型分组——sprite/tile 完全按 CommandBuffer 提交顺序绘制。
    TextureHandle currentTex{};
    bool          hasCurrent = false;
    uint32_t      batchIdxStart  = 0;
    int32_t       batchVertStart = 0;

    auto flush = [&]() {
        if (static_cast<uint32_t>(batchIdx_.size()) > batchIdxStart) {
            batches.push_back({ currentTex, batchIdxStart,
                                static_cast<uint32_t>(batchIdx_.size()) - batchIdxStart,
                                batchVertStart });
            batchIdxStart  = static_cast<uint32_t>(batchIdx_.size());
            batchVertStart = static_cast<int32_t>(batchVerts_.size());
        }
    };

    auto maybeFlush = [&](TextureHandle tex) {
        const bool batchFull =
            (batchVerts_.size() - static_cast<size_t>(batchVertStart) >= MAX_SPRITES_PER_BATCH * 4);
        if (!hasCurrent || tex != currentTex || batchFull) {
            flush();
            currentTex = tex;
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
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            mvp[i * 4 + j] = 0.f;
            for (int k = 0; k < 4; ++k) {
                mvp[i * 4 + j] += proj[i * 4 + k] * view[k * 4 + j];
            }
        }
    }

    glUseProgram(shaderProgram_);
    glUniformMatrix4fv(uProjLoc_, 1, GL_FALSE, mvp);
    glUniform1i(uTexLoc_, 0);
    glActiveTexture(GL_TEXTURE0);

    auto drawBatches = [&](const std::vector<BatchSegment>& batches) {
        for (const BatchSegment& seg : batches) {
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
    };

    drawBatches(batches);

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
    const float halfW = 0.5f / zoom;
    const float halfH = 0.5f / zoom;
    const float cosR  = cosf(rotation);
    const float sinR  = sinf(rotation);

    memset(out, 0, 16 * sizeof(float));
    out[0]  =  cosR * halfW;
    out[1]  =  sinR * halfW;
    out[4]  = -sinR * halfH;
    out[5]  =  cosR * halfH;
    out[10] = 1.f;
    out[12] = -camX * cosR * halfW + camY * sinR * halfW + halfW;
    out[13] = -camX * sinR * halfH - camY * cosR * halfH + halfH;
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
                out[i * 4 + j] += proj[i * 4 + k] * view[k * 4 + j];
            }
        }
    }
}

} // namespace backend
