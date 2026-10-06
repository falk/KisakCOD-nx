// Host proof that the TAAU resolve program (deko9_taau_shaders.cpp) renders
// what the reference resolve below renders: both run on Mesa's software
// rasteriser (EGL surfaceless, llvmpipe, GL 4.5) over the same random scene,
// depth, history, object motion and reactive images, for every program
// variant and a set of frames (depth reprojection, object motion, reset,
// viewmodel, reactive mask, flat 2x2s). The reference is the resolve as
// first measured on hardware, kept verbatim; the production program is
// rewritten for fewer instructions and registers, so its output may differ
// only by float rounding: every output and history channel within
// kTolerance, except a bounded share of pixels where a threshold test
// (flat 2x2, history validity) lands on the other side by rounding.
// The present pass's power-curve gamma build maps every 8-bit level to the
// engine's ramp entry.

#include "src/deko9/deko9_fsr.h"
#include "src/deko9/deko9_taau.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace
{
int g_failures;

void Check(bool ok, const char *what, const char *detail = "")
{
    std::printf("%s:DEKO9_TAAU_GL_%s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok)
        ++g_failures;
}

#define GL_FUNCS(X)                                                                                                    \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                                           \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                                           \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                                         \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                                             \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                                                   \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                                         \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                                           \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                                             \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                                           \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                                                 \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                                               \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                                                         \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                                                           \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                                                             \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                                                       \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                                                                             \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                                                                               \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                                                                         \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                                                         \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                                                     \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                                                               \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                                     \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                                                           \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                                                       \
    X(PFNGLDRAWBUFFERSPROC, glDrawBuffers)                                                                             \
    X(PFNGLREADBUFFERPROC, glReadBuffer)                                                                               \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                                                               \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                                               \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                                                         \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                                               \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                                                               \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)                                                                       \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                                                     \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                                     \
    X(PFNGLVIEWPORTPROC, glViewport)                                                                                   \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                                                               \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                                                             \
    X(PFNGLGETERRORPROC, glGetError)                                                                                   \
    X(PFNGLGETSTRINGPROC, glGetString)

#define GL_DECLARE(type, name) type name;
struct Gl
{
    GL_FUNCS(GL_DECLARE)
} gl;

bool LoadGl()
{
    bool ok = true;
#define GL_LOAD(type, name)                                                                                            \
    gl.name = (type)eglGetProcAddress(#name);                                                                          \
    ok = ok && gl.name;
    GL_FUNCS(GL_LOAD)
    return ok;
}

// A GL 4.5 core context on Mesa's CPU rasteriser, no window or device.
bool CreateContext(std::string *error)
{
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    setenv("EGL_PLATFORM", "surfaceless", 1);
    auto getDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!getDisplay)
        return *error = "no eglGetPlatformDisplayEXT", false;
    EGLDisplay display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint major, minor;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor))
        return *error = "no surfaceless EGL display", false;
    if (!eglBindAPI(EGL_OPENGL_API))
        return *error = "no desktop GL API", false;
    const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION,
                              4,
                              EGL_CONTEXT_MINOR_VERSION,
                              5,
                              EGL_CONTEXT_OPENGL_PROFILE_MASK,
                              EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                              EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        return *error = "no GL 4.5 core context", false;
    if (!LoadGl())
        return *error = "GL entry points missing", false;
    return true;
}

GLuint Compile(GLenum stage, const std::string &source, std::string *error)
{
    GLuint shader = gl.glCreateShader(stage);
    const char *text = source.c_str();
    gl.glShaderSource(shader, 1, &text, nullptr);
    gl.glCompileShader(shader);
    GLint ok = 0;
    gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[4096] = {};
        gl.glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        *error = log;
        gl.glDeleteShader(shader);
        return 0;
    }
    return shader;
}

const char kFullScreenVertex[] = R"GLSL(#version 450
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// The device programs target GLSL 4.60; nothing in them needs more than 4.50.
std::string AsGl45(std::string text)
{
    const size_t at = text.find("#version 460");
    if (at != std::string::npos)
        text.replace(at, 12, "#version 450");
    return text;
}

GLuint Link(const std::string &fragment, std::string *error, const char *vertex = kFullScreenVertex)
{
    GLuint vs = Compile(GL_VERTEX_SHADER, AsGl45(vertex), error);
    GLuint fs = vs ? Compile(GL_FRAGMENT_SHADER, AsGl45(fragment), error) : 0;
    if (!fs)
        return 0;
    GLuint program = gl.glCreateProgram();
    gl.glAttachShader(program, vs);
    gl.glAttachShader(program, fs);
    gl.glLinkProgram(program);
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    GLint ok = 0;
    gl.glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[4096] = {};
        gl.glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        *error = log;
        gl.glDeleteProgram(program);
        return 0;
    }
    return program;
}

GLuint Texture(GLint internal, int w, int h, GLenum format, GLenum type, const void *data, bool linear)
{
    GLuint t;
    gl.glGenTextures(1, &t);
    gl.glBindTexture(GL_TEXTURE_2D, t);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, data);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return t;
}

// One resolve's inputs: render-size scene colour / depth / motion /
// reactive change, output-size previous history, the constant block.
struct Scene
{
    int rw, rh, ow, oh;
    int32_t rect[4]; // the output rectangle inside the ow x oh target
    std::vector<uint8_t> color;     // RGBA8
    std::vector<float> depth;       // R32F window depth
    std::vector<float> motion;      // RG, uv units or the -16 / +16 markers
    std::vector<float> reactive;    // R, luma change
    std::vector<uint32_t> history;  // RGB10A2 (A = youth level)
    std::vector<uint8_t> uniforms;  // the program's binding-1 block
    std::vector<uint8_t> referenceUniforms; // the reference program's
};

struct Outputs
{
    std::vector<float> color, history; // RGBA32F, output size
};

bool Render(GLuint program, const Scene &s, const std::vector<uint8_t> &uniforms, Outputs *out, std::string *error)
{
    GLuint tex[5];
    tex[0] = Texture(GL_RGBA8, s.rw, s.rh, GL_RGBA, GL_UNSIGNED_BYTE, s.color.data(), false);
    tex[1] = Texture(GL_R32F, s.rw, s.rh, GL_RED, GL_FLOAT, s.depth.data(), false);
    tex[2] = Texture(GL_RGB10_A2, s.ow, s.oh, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, s.history.data(), true);
    tex[3] = Texture(GL_RG32F, s.rw, s.rh, GL_RG, GL_FLOAT, s.motion.data(), false);
    tex[4] = Texture(GL_R32F, s.rw, s.rh, GL_RED, GL_FLOAT, s.reactive.data(), false);
    GLuint targets[2];
    const std::vector<float> zero((size_t)s.ow * s.oh * 4, 0.0f);
    targets[0] = Texture(GL_RGBA32F, s.ow, s.oh, GL_RGBA, GL_FLOAT, zero.data(), false);
    targets[1] = Texture(GL_RGBA32F, s.ow, s.oh, GL_RGBA, GL_FLOAT, zero.data(), false);
    GLuint fbo, ubo, vao;
    gl.glGenFramebuffers(1, &fbo);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets[0], 0);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, targets[1], 0);
    const GLenum draw[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    gl.glDrawBuffers(2, draw);
    bool ok = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl.glGenBuffers(1, &ubo);
    gl.glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    gl.glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)uniforms.size(), uniforms.data(), GL_STATIC_DRAW);
    gl.glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo);
    for (int i = 0; i < 5; ++i)
    {
        gl.glActiveTexture(GL_TEXTURE0 + i);
        gl.glBindTexture(GL_TEXTURE_2D, tex[i]);
    }
    gl.glGenVertexArrays(1, &vao);
    gl.glBindVertexArray(vao);
    gl.glUseProgram(program);
    gl.glViewport(s.rect[0], s.rect[1], s.rect[2], s.rect[3]);
    gl.glDrawArrays(GL_TRIANGLES, 0, 3);
    out->color.assign((size_t)s.ow * s.oh * 4, 0.0f);
    out->history.assign((size_t)s.ow * s.oh * 4, 0.0f);
    gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl.glReadPixels(0, 0, s.ow, s.oh, GL_RGBA, GL_FLOAT, out->color.data());
    gl.glReadBuffer(GL_COLOR_ATTACHMENT1);
    gl.glReadPixels(0, 0, s.ow, s.oh, GL_RGBA, GL_FLOAT, out->history.data());
    const GLenum glError = gl.glGetError();
    ok = ok && glError == GL_NO_ERROR;
    if (!ok)
        *error = "GL error " + std::to_string(glError);
    gl.glDeleteFramebuffers(1, &fbo);
    gl.glDeleteBuffers(1, &ubo);
    gl.glDeleteTextures(5, tex);
    gl.glDeleteTextures(2, targets);
    return ok;
}

// One full-screen pass of `program` over an RGBA8 source of w x h into a
// w x h RGBA32F target, with `uniforms` at binding 1.
bool RenderOne(GLuint program, int w, int h, const std::vector<uint8_t> &source, const std::vector<uint8_t> &uniforms,
               std::vector<float> *out, std::string *error)
{
    const GLuint tex = Texture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, source.data(), true);
    const std::vector<float> zero((size_t)w * h * 4, 0.0f);
    const GLuint target = Texture(GL_RGBA32F, w, h, GL_RGBA, GL_FLOAT, zero.data(), false);
    GLuint fbo, ubo, vao;
    gl.glGenFramebuffers(1, &fbo);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    const GLenum draw = GL_COLOR_ATTACHMENT0;
    gl.glDrawBuffers(1, &draw);
    bool ok = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl.glGenBuffers(1, &ubo);
    gl.glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    gl.glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)uniforms.size(), uniforms.data(), GL_STATIC_DRAW);
    gl.glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    gl.glGenVertexArrays(1, &vao);
    gl.glBindVertexArray(vao);
    gl.glUseProgram(program);
    gl.glViewport(0, 0, w, h);
    gl.glDrawArrays(GL_TRIANGLES, 0, 3);
    out->assign((size_t)w * h * 4, 0.0f);
    gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl.glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, out->data());
    const GLenum glError = gl.glGetError();
    ok = ok && glError == GL_NO_ERROR;
    if (!ok)
        *error = "GL error " + std::to_string(glError);
    gl.glDeleteFramebuffers(1, &fbo);
    gl.glDeleteBuffers(1, &ubo);
    gl.glDeleteTextures(1, &tex);
    gl.glDeleteTextures(1, &target);
    return ok;
}

struct ReferenceConstants;
void ReferenceSetup(ReferenceConstants *out, const int32_t srcRect[4], const int32_t dstRect[4], uint32_t colorWidth,
                    uint32_t colorHeight, uint32_t histWidth, uint32_t histHeight, const deko9::TaauFrame &frame);

struct FrameCase
{
    const char *name;
    bool motion, opaque, reset, viewmodel, half;
    float shift; // reprojection: NDC shift of the previous view (pushes a border off the history)
    bool lowContrast = false; // scene colours within 3/255 of their 2x2 block's base
};

// Random but structured inputs: a third of the 2x2 blocks flat, the rest
// random (or, low contrast, within 3/255 of a random base per block); depths
// with a near band (viewmodel when the split is on) and sky beyond the far
// plane; motion markers mixed with small motions. `gates` keeps the one-tap
// history fallbacks (position bar, range) in the production block; without
// it both are 0, so the program runs the reference's five taps everywhere.
Scene MakeScene(std::mt19937 &rng, const FrameCase &fc, uint32_t variant, int rw, int rh, int ow, int oh,
                const int32_t rect[4], bool gates = false)
{
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    Scene s;
    s.rw = rw;
    s.rh = rh;
    s.ow = ow;
    s.oh = oh;
    std::memcpy(s.rect, rect, sizeof(s.rect));
    s.color.resize((size_t)rw * rh * 4);
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x)
        {
            uint8_t *p = &s.color[((size_t)y * rw + x) * 4];
            const uint32_t block = (uint32_t)((y / 2) * 7919 + (x / 2) * 104729);
            if (block % 3 == 0)
            {
                std::mt19937 flat(block);
                for (int c = 0; c < 3; ++c)
                    p[c] = (uint8_t)(flat() & 0xff);
            }
            else if (fc.lowContrast)
            {
                std::mt19937 base(block);
                for (int c = 0; c < 3; ++c)
                    p[c] = (uint8_t)std::min<uint32_t>(252, base() & 0xff) + (uint8_t)(rng() % 4);
            }
            else
                for (int c = 0; c < 3; ++c)
                    p[c] = (uint8_t)(rng() & 0xff);
            p[3] = 255;
        }
    s.depth.resize((size_t)rw * rh);
    for (float &d : s.depth)
    {
        const float r = u01(rng);
        d = r < 0.1f ? 1.0f : r < 0.2f ? 0.02f * u01(rng) : 0.05f + 0.9f * u01(rng);
    }
    s.motion.resize((size_t)rw * rh * 2);
    for (size_t i = 0; i < s.motion.size(); i += 2)
    {
        const float r = u01(rng);
        if (!fc.motion) // the device binds the scene colour instead
        {
            s.motion[i] = u01(rng);
            s.motion[i + 1] = u01(rng);
        }
        else if (r < 0.5f)
            s.motion[i] = s.motion[i + 1] = deko9::kTaauMotionNone;
        else if (r < 0.6f)
            s.motion[i] = s.motion[i + 1] = deko9::kTaauMotionReject;
        else
        {
            s.motion[i] = (u01(rng) - 0.5f) * 0.1f;
            s.motion[i + 1] = (u01(rng) - 0.5f) * 0.1f;
        }
    }
    s.reactive.resize((size_t)rw * rh);
    for (float &r : s.reactive)
        r = u01(rng) < 0.7f ? 0.0f : (u01(rng) - 0.5f) * 0.4f;
    s.history.resize((size_t)ow * oh);
    for (uint32_t &h : s.history)
        h = (rng() & 0x3fffffffu) | (uint32_t)(rng() % 4) << 30;
    if (fc.lowContrast)
        // A history near the scene (within one 8-bit step of the texel under
        // the pixel), so the clamp does not pin both filters' results to the
        // same bound.
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x)
            {
                const int tx = std::min((int)((x - rect[0] + 0.5f) * rw / rect[2]), rw - 1);
                const int ty = std::min((int)((y - rect[1] + 0.5f) * rh / rect[3]), rh - 1);
                const uint8_t *p = &s.color[((size_t)std::max(ty, 0) * rw + std::max(tx, 0)) * 4];
                uint32_t h = (uint32_t)(rng() % 4) << 30;
                for (int c = 0; c < 3; ++c)
                    h |= (uint32_t)std::min(std::max(p[c] * 4 + (int)(rng() % 9) - 4, 0), 1023) << (10 * c);
                s.history[(size_t)y * ow + x] = h;
            }

    deko9::TaauFrame f{};
    deko9::TaauHalton(rng() % 8, &f.jitter[0], &f.jitter[1]);
    // Unjittered NDC -> previous clip: a small rotation-like shear and shift
    // with w = 1 + a depth term, so prev.z stays positive.
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            f.reproj[i][j] = i == j ? 1.0f : 0.0f;
    f.reproj[3][0] = fc.shift;
    f.reproj[3][1] = -0.5f * fc.shift;
    f.reproj[0][1] = 0.01f;
    f.reproj[1][0] = -0.01f;
    f.reproj[2][3] = 0.05f;
    f.viewmodelSplit = fc.viewmodel ? 0.02f : 0.0f;
    f.sceneMinZ = 0.0f;
    f.sceneMaxZ = 1.0f;
    f.farNdcZ = 0.99f;
    f.blend = 0.1f;
    f.reactive = 0.8f;
    f.antiFlicker = 0.5f;
    f.flat = deko9::kTaauFlatDefault / 255.0f;
    f.bilinearRange = gates ? deko9::kTaauBilinearRangeDefault / 255.0f : 0.0f;
    f.reset = fc.reset;
    f.motion = fc.motion;
    f.opaque = fc.opaque;
    f.bilinearHistory = (variant & deko9::kTaauBilinearHistory) != 0;
    f.bilinearCurrent = (variant & deko9::kTaauBilinearCurrent) != 0;
    const int32_t src[4] = {0, 0, rw, rh}, *dst = rect;
    deko9::TaauConstants c;
    deko9::TaauSetup(&c, src, dst, (uint32_t)rw, (uint32_t)rh, (uint32_t)ow, (uint32_t)oh, f);
    if (!gates)
        c.depth[1] = 0.0f;
    s.uniforms.resize(sizeof(c));
    std::memcpy(s.uniforms.data(), &c, sizeof(c));
    s.referenceUniforms.resize(256);
    ReferenceSetup(reinterpret_cast<ReferenceConstants *>(s.referenceUniforms.data()), src, dst, (uint32_t)rw,
                   (uint32_t)rh, (uint32_t)ow, (uint32_t)oh, f);
    return s;
}

const char kReferenceResolveGlsl[] = R"GLSL(#version 460
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outHistory;
layout(binding = 0) uniform sampler2D uColor;
layout(binding = 1) uniform sampler2D uDepth;
layout(binding = 2) uniform sampler2D uHistory;
layout(binding = 3) uniform sampler2D uMotion;
layout(binding = 4) uniform sampler2D uReactiveDelta;
layout(std140, binding = 1) uniform TaauBlock
{
    vec4 uReproj[4];  // rows: prev clip (x, y, w) of (u.x, u.y, z, 1), 0.5 folded
    vec4 uPos;        // output px -> jittered render px - 0.5: fr = frag * xy + zw
    vec4 uUv;         // output px -> output uv: u = frag * xy + zw
    vec4 uGather;     // 2x2 gather uv = corner * xy + zw
    vec4 uTexel;      // point-sample uv = (render px - 0.5) * xy + zw
    vec4 uDst;        // output rect w, h; x - 0.5, y - 0.5 (history px - 0.5 = u * xy + zw)
    vec4 uHist;       // 1/w, 1/h, -1/w, -1/h of the history
    vec4 uHist2;      // 2/w, 2/h, 0.5/w, 0.5/h
    vec4 uScale;      // kernel units per render px x, y; flat threshold; 0
    vec4 uDepthParm;  // viewmodel split, 0, 0, depth at infinity
    vec4 uBlend;      // blend, history valid sign, 0, motion texture
    vec4 uReactive;   // 0, threshold, scale (0: no mask), weight
    vec4 uOutput;     // anti-flicker, youth weight, youth step, 0
};

float Luma(vec3 c)
{
    return dot(c, vec3(0.25, 0.5, 0.25));
}

// The history at `fr` + 0.5 (history pixels); alpha carries the pixel's
// youth (see main). The sampler clamps at the history's edge.
vec4 History(vec2 fr)
{
#ifndef TAAU_BILINEAR_HISTORY
    // Catmull-Rom in 5 bilinear taps (the four corner taps dropped). With
    // e = 1 - f, twice the weights are W12 = 2 + f e, W0 = -f e e,
    // W3 = -f f e; the common factor cancels in the normalisation, and the
    // five tap weights sum to 2 (W12.x + W12.y) - W12.x W12.y.
    vec2 fl = floor(fr);
    vec2 f = fr - fl, e = 1.0 - f;
    vec2 fe = f * e;
    vec2 w12 = fe + 2.0, p0 = fe * e, p3 = fe * f;
    vec2 off = f * (1.0 + f * (4.0 - 3.0 * f)) * (1.0 / w12);
    vec2 base = fl * uHist.xy + uHist2.zw;
    vec2 t0 = base + uHist.zw, t3 = base + uHist2.xy, t12 = off * uHist.xy + base;
    float a = -w12.x * p0.y, b = -p0.x * w12.y, c = w12.x * w12.y, d = -p3.x * w12.y, e5 = -w12.x * p3.y;
    vec4 centre = textureLod(uHistory, t12, 0.0);
    vec3 r = centre.rgb * c + textureLod(uHistory, vec2(t12.x, t0.y), 0.0).rgb * a +
             textureLod(uHistory, vec2(t0.x, t12.y), 0.0).rgb * b + textureLod(uHistory, vec2(t3.x, t12.y), 0.0).rgb * d +
             textureLod(uHistory, vec2(t12.x, t3.y), 0.0).rgb * e5;
    return vec4(r / (2.0 * (w12.x + w12.y) - c), centre.a);
#else
    return textureLod(uHistory, fr * uHist.xy + uHist2.zw, 0.0);
#endif
}

void main()
{
    // The 2x2 scene texels around the sample point: one gather uv for the
    // colour and the depth. Gather order: x (0,1), y (1,1), z (1,0), w (0,0).
    vec2 u = gl_FragCoord.xy * uUv.xy + uUv.zw;
    vec2 fr = gl_FragCoord.xy * uPos.xy + uPos.zw;
    vec2 corner = floor(fr);
    vec2 guv = corner * uGather.xy + uGather.zw;
    vec4 r = textureGather(uColor, guv, 0), g = textureGather(uColor, guv, 1), b = textureGather(uColor, guv, 2);

    // Current frame over the 4 texels, whose RGB range bounds the history.
#ifdef TAAU_BILINEAR_CURRENT
    // Bilinear weights (a tent one render px wide; the weights sum to 1).
    vec2 f = fr - corner, e = 1.0 - f;
    vec4 w = vec4(e.x * f.y, f.x * f.y, f.x * e.y, e.x * e.y);
    float wmax = max(max(w.x, w.y), max(w.z, w.w));
    vec3 current = vec3(dot(r, w), dot(g, w), dot(b, w));
#else
    // A (1 - d^2)^2 kernel in kernel units (TaauKernelScale).
    vec2 f0 = (fr - corner) * uScale.xy, f1 = f0 - uScale.xy;
    vec2 d0 = f0 * f0, d1 = f1 * f1;
    vec4 w = clamp(1.0 - vec4(d0.x + d1.y, d1.x + d1.y, d1.x + d0.y, d0.x + d0.y), 0.0, 1.0);
    w *= w;
    float wmax = max(max(w.x, w.y), max(w.z, w.w));
    vec3 current = vec3(dot(r, w), dot(g, w), dot(b, w)) / max(dot(w, vec4(1.0)), 9.5367431640625e-7);
#endif
    vec3 lo = vec3(min(min(r.x, r.y), min(r.z, r.w)), min(min(g.x, g.y), min(g.z, g.w)), min(min(b.x, b.y), min(b.z, b.w)));
    vec3 hi = vec3(max(max(r.x, r.y), max(r.z, r.w)), max(max(g.x, g.y), max(g.z, g.w)), max(max(b.x, b.y), max(b.z, b.w)));

    // Reactive: the transparent passes changed this pixel's luma; the
    // history remembers it for one more frame, so a particle's trail clears
    // as fast as the particle.
    float delta = textureLod(uReactiveDelta, fr * uTexel.xy + uTexel.zw, 0.0).x;
    float reactive = clamp((abs(delta) - uReactive.y) * uReactive.z, 0.0, 1.0);

    // Youth (the history's 2-bit alpha): 1 where the history restarted from
    // the current frame alone, then one level less per blended frame; it
    // floors the current frame's weight at youth * uOutput.y, so a fresh
    // pixel averages its first frames evenly instead of keeping the aliased
    // first one for 1 / blend frames. Reactive pixels count as fresh.
    vec3 result = current;
    float youth = reactive;
    vec3 range = hi - lo;
    // A flat 2x2 clamps any history onto the current colour: skip it.
    if (max(max(range.r, range.g), range.b) > uScale.z)
    {
        youth = 1.0;
        // Nearest depth of the 2x2: edges take the motion of the foreground.
        vec4 dg = textureGather(uDepth, guv, 0);
        vec2 col = min(dg.xy, dg.wz);
        float dz = min(col.x, col.y);
        vec2 near = vec2(col.y < col.x, min(dg.x, dg.y) < min(dg.w, dg.z));

        // Per-object motion (uBlend.w) at the nearest texel, else depth
        // reprojection; the viewmodel (in front of the split) stays put.
        // Depth clear (sky) and anything past it reprojects as infinitely far.
        vec2 motion = textureLod(uMotion, guv + (near - 0.5) * uGather.xy, 0.0).xy;
        motion = uBlend.w != 0.0 ? motion : vec2(-16.0);
        float z = min(dz, uDepthParm.w);
        vec3 prev = u.x * uReproj[0].xyz + u.y * uReproj[1].xyz + z * uReproj[2].xyz + uReproj[3].xyz;
        vec2 reproj = prev.xy * (1.0 / prev.z);
        vec2 moved = u + motion;
        bool useMotion = motion.x > -8.0;
        bool viewmodel = dz < uDepthParm.x;
        vec2 still = viewmodel ? u : reproj;
        vec2 prevU = useMotion ? moved : still;
        // Positive when the history is usable: not a reset frame, no reject
        // motion, a point in front of the previous camera (or the
        // viewmodel), and inside the history. The bar is 2^-20, not 0: a
        // tiny prev.z makes 1 / prev.z infinite (0 * inf = NaN when prev.xy
        // is 0) and min/max drop a NaN edge term, so the reprojected uv is
        // used only where prev.z exceeds the bar and its reciprocal is finite.
        vec2 edge = abs(prevU - 0.5);
        float good = useMotion ? 8.0 - motion.x : max(uDepthParm.x - dz, prev.z);
        good = min(min(good, 0.5 - max(edge.x, edge.y)), uBlend.y);
        if (good > 9.5367431640625e-7)
        {
            vec4 h = History(prevU * uDst.xy + uDst.zw);
            vec3 hist = clamp(h.rgb, lo, hi);
            float histY = Luma(hist), currentY = Luma(current);
            float alpha = clamp(uBlend.x * wmax, 0.0, 1.0);
            // Anti-flicker: agreeing luma (relative difference near 0)
            // trusts the history more; disagreement keeps the full weight.
            float agree = 1.0 - clamp(abs(currentY - histY) / max(max(currentY, histY), 0.2), 0.0, 1.0);
            alpha *= 1.0 - uOutput.x * agree * agree;
            alpha = max(alpha, max(reactive * uReactive.w, h.a * uOutput.y));
            youth = max(reactive, h.a - uOutput.z);
            // Luma-weighted blend (1 / (1 + luma) per side) as one lerp.
            float t = alpha * (1.0 + histY);
            t /= t + (1.0 - alpha) * (1.0 + currentY);
            result = mix(hist, current, t);
        }
    }
    outColor = vec4(result, 1.0);
    outHistory = vec4(result, youth);
}
)GLSL";

// The reference program's constant block and its setup, as first measured.
struct ReferenceConstants
{
    float reproj[4][4];  // rows k of (u.x, u.y, depth, 1) -> previous clip (x + w) / 2, (w - y) / 2, w: divided, the previous uv
    float pos[4];        // output px -> jittered render px - 0.5: fr = frag * xy + zw
    float uv[4];         // output px -> output uv: u = frag * xy + zw
    float gather[4];     // 2x2 gather uv = corner * xy + zw
    float texel[4];      // point-sample uv of render px p = p * xy + zw
    float dst[4];        // output rect w, h, x - 0.5, y - 0.5
    float hist[4];       // 1 / w, 1 / h, -1 / w, -1 / h of the history
    float hist2[4];      // 2 / w, 2 / h, 0.5 / w, 0.5 / h
    float scale[4];      // kernel units per render px x, y; flat threshold; 0
    float depth[4];      // viewmodel split, 0, 0, depth at infinity (the clear lies beyond it)
    float blend[4];      // blend, history valid (1, reset -1), 0, motion texture (0/1)
    float reactive[4];   // 0, threshold, scale (0: no reactive mask), weight
    float output[4];     // anti-flicker, youth weight, youth step, 0
};

void ReferenceSetup(ReferenceConstants *out, const int32_t srcRect[4], const int32_t dstRect[4], uint32_t colorWidth,
               uint32_t colorHeight, uint32_t histWidth, uint32_t histHeight, const deko9::TaauFrame &frame)
{
    std::memset(out, 0, sizeof(*out));
    // The NDC mapping of (u.x, u.y, z, 1) folded in: ndc = (2 u.x - 1, 1 - 2 u.y)
    // in, (x + w) / 2, (w - y) / 2 out, so the shader divides by w and has
    // the previous uv; then z = (depth - min) / (max - min) folded into the
    // depth and constant rows.
    for (int c = 0; c < 4; ++c)
    {
        const float n[4] = {2.0f * frame.reproj[0][c], -2.0f * frame.reproj[1][c], frame.reproj[2][c],
                            frame.reproj[3][c] + frame.reproj[1][c] - frame.reproj[0][c]};
        for (int k = 0; k < 4; ++k)
        {
            if (c == 0)
                out->reproj[k][0] += 0.5f * n[k];
            else if (c == 1)
                out->reproj[k][1] -= 0.5f * n[k];
            else if (c == 3)
            {
                out->reproj[k][0] += 0.5f * n[k];
                out->reproj[k][1] += 0.5f * n[k];
                out->reproj[k][2] = n[k];
            }
        }
    }
    const float zScale = 1.0f / (frame.sceneMaxZ - frame.sceneMinZ);
    for (int c = 0; c < 3; ++c)
    {
        out->reproj[3][c] -= frame.sceneMinZ * zScale * out->reproj[2][c];
        out->reproj[2][c] *= zScale;
    }
    const float inv[2] = {1.0f / (float)colorWidth, 1.0f / (float)colorHeight};
    const float src[4] = {(float)srcRect[0], (float)srcRect[1], (float)srcRect[2], (float)srcRect[3]};
    const float dst[4] = {(float)dstRect[0], (float)dstRect[1], (float)dstRect[2], (float)dstRect[3]};
    for (int i = 0; i < 2; ++i)
    {
        out->pos[i] = src[2 + i] / dst[2 + i];
        out->pos[2 + i] = frame.jitter[i] - dst[i] * out->pos[i] - 0.5f;
        out->uv[i] = 1.0f / dst[2 + i];
        out->uv[2 + i] = -dst[i] / dst[2 + i];
        out->gather[i] = inv[i];
        out->gather[2 + i] = (src[i] + 1.0f) * inv[i];
        out->texel[i] = inv[i] * frame.reactiveUv[i];
        out->texel[2 + i] = (src[i] + 0.5f) * inv[i] * frame.reactiveUv[i];
        out->dst[i] = dst[2 + i];
        out->dst[2 + i] = dst[i] - 0.5f;
        const float histInv = 1.0f / (float)(i ? histHeight : histWidth);
        out->hist[i] = histInv;
        out->hist[2 + i] = -histInv;
        out->hist2[i] = 2.0f * histInv;
        out->hist2[2 + i] = 0.5f * histInv;
        out->scale[i] = deko9::TaauKernelScale(dst[2 + i] / src[2 + i]);
    }
    out->scale[2] = frame.flat;
    out->depth[0] = frame.viewmodelSplit;
    out->depth[3] = frame.sceneMinZ + frame.farNdcZ * (frame.sceneMaxZ - frame.sceneMinZ);
    out->blend[0] = frame.blend;
    out->blend[1] = frame.reset ? -1.0f : 1.0f;
    out->blend[3] = frame.motion ? 1.0f : 0.0f;
    out->reactive[1] = deko9::kTaauReactiveThreshold;
    out->reactive[2] = frame.opaque && frame.reactive > 0.0f ? deko9::kTaauReactiveScale : 0.0f;
    out->reactive[3] = frame.reactive;
    out->output[0] = frame.antiFlicker;
    out->output[1] = deko9::kTaauYouthWeight;
    out->output[2] = deko9::kTaauYouthStep;
}


// Per channel: |a - b| <= kTolerance passes. The scene is 8-bit and the
// history 10-bit; a 2^-12 bound is a quarter of the history's step.
constexpr float kTolerance = 1.0f / 4096.0f;
// Pixels allowed past it (a threshold decided the other way by rounding).
constexpr double kFlipShare = 0.002;

struct Diff
{
    float maxColor = 0.0f, maxHistory = 0.0f;
    size_t over = 0, pixels = 0;
};

void Compare(const Outputs &a, const Outputs &b, Diff *d)
{
    for (size_t p = 0; p < a.color.size() / 4; ++p)
    {
        float worst = 0.0f;
        for (int c = 0; c < 4; ++c)
        {
            const float dc = c < 3 ? std::fabs(a.color[p * 4 + c] - b.color[p * 4 + c]) : 0.0f;
            const float dh = std::fabs(a.history[p * 4 + c] - b.history[p * 4 + c]);
            worst = std::max(worst, std::max(dc, dh));
            if (dc <= kTolerance)
                d->maxColor = std::max(d->maxColor, dc);
            if (dh <= kTolerance)
                d->maxHistory = std::max(d->maxHistory, dh);
        }
        d->over += !(worst <= kTolerance);
        ++d->pixels;
    }
}

} // namespace

int main()
{
    std::string error;
    if (!CreateContext(&error))
    {
        Check(false, "CONTEXT", error.c_str());
        return 1;
    }
    Check(true, "CONTEXT", (const char *)gl.glGetString(GL_RENDERER));

    const FrameCase cases[] = {
        {"depth", false, false, false, false, false, 0.0f},
        {"depth_shift", false, true, false, true, false, 0.08f},
        {"motion", true, true, false, true, false, 0.02f},
        {"reset", true, false, true, false, false, 0.0f},
    };
    // Output / render sizes: 0.6 and 0.5 scale (the kernel's two unit rules).
    // Render w, h; target w, h; output rectangle (one off the origin).
    const int sizes[][8] = {{48, 27, 80, 45, 0, 0, 80, 45}, {40, 23, 80, 45, 0, 0, 80, 45},
                            {48, 27, 86, 50, 3, 2, 80, 45}};
    for (uint32_t variant = 0; variant < deko9::kTaauResolveVariants; ++variant)
    {
        std::string err;
        const GLuint reference = Link(deko9::TaauVariant(kReferenceResolveGlsl, variant), &err);
        const GLuint program = reference ? Link(deko9::TaauVariant(deko9::kTaauResolveGlsl, variant), &err) : 0;
        char name[64];
        std::snprintf(name, sizeof(name), "LINK_V%u", variant);
        Check(reference && program, name, err.c_str());
        if (!reference || !program)
            continue;
        Diff d;
        bool rendered = true;
        size_t historyUsed = 0, historyPixels = 0;
        std::mt19937 rng(1234 + variant);
        for (const FrameCase &fc : cases)
            for (const auto &size : sizes)
            {
                const Scene s = MakeScene(rng, fc, variant, size[0], size[1], size[2], size[3], &size[4]);
                Outputs a, b;
                rendered = rendered && Render(reference, s, s.referenceUniforms, &a, &err) &&
                           Render(program, s, s.uniforms, &b, &err);
                if (rendered)
                    Compare(a, b, &d);
                // Non-vacuous: the same scene as a reset frame differs where
                // the history was used.
                if (rendered && !fc.reset)
                {
                    std::vector<uint8_t> reset = s.referenceUniforms;
                    const float invalid = -1.0f;
                    std::memcpy(reset.data() + offsetof(ReferenceConstants, blend) + 4, &invalid, 4);
                    Outputs c;
                    rendered = Render(reference, s, reset, &c, &err);
                    for (size_t p = 0; rendered && p < a.color.size() / 4; ++p)
                        historyUsed += std::fabs(a.color[p * 4] - c.color[p * 4]) > 1.0f / 255.0f;
                    historyPixels += a.color.size() / 4;
                }
            }
        char detail[256];
        std::snprintf(detail, sizeof(detail), "pixels=%zu over=%zu (%.3f%%) max_color=%.2e max_history=%.2e history_used=%.1f%% %s",
                      d.pixels, d.over, 100.0 * (double)d.over / (double)std::max<size_t>(d.pixels, 1),
                      d.maxColor, d.maxHistory, 100.0 * (double)historyUsed / (double)std::max<size_t>(historyPixels, 1),
                      err.c_str());
        std::snprintf(name, sizeof(name), "MATCHES_REFERENCE_V%u", variant);
        Check(rendered && d.pixels && historyUsed * 5 > historyPixels && (double)d.over <= kFlipShare * (double)d.pixels, name, detail);
        gl.glDeleteProgram(reference);
        gl.glDeleteProgram(program);
    }
    // The one-tap history fallbacks (the position bar, the range) against
    // the reference's five taps: where they fire, the output moves by less
    // than the 2x2 range (the clamp box both results land in) plus the bar's
    // share of the local contrast; the low-contrast scene makes them fire on
    // a sizeable share of the pixels, the still scene (identity
    // reprojection, no shift) on nearly all of them.
    {
        const FrameCase gated[] = {
            {"low_contrast", false, true, false, true, false, 0.03f, true},
            {"low_contrast_motion", true, false, false, false, false, 0.0f, true},
            {"still", false, false, false, true, false, 0.0f, false},
        };
        for (uint32_t variant = 0; variant < deko9::kTaauResolveVariants; variant += 2)
        {
            std::string err;
            const GLuint reference = Link(deko9::TaauVariant(kReferenceResolveGlsl, variant), &err);
            const GLuint program = reference ? Link(deko9::TaauVariant(deko9::kTaauResolveGlsl, variant), &err) : 0;
            bool rendered = reference && program;
            float worstOverRange = 0.0f, worstStill = 0.0f;
            size_t pixels = 0, moved = 0, movedStill = 0, stillPixels = 0;
            std::mt19937 rng(777 + variant);
            for (const FrameCase &fc : gated)
                for (const auto &size : sizes)
                {
                    if (!rendered)
                        break;
                    const Scene s = MakeScene(rng, fc, variant, size[0], size[1], size[2], size[3], &size[4], true);
                    Outputs a, b;
                    rendered = Render(reference, s, s.referenceUniforms, &a, &err) &&
                               Render(program, s, s.uniforms, &b, &err);
                    // The 2x2 under each output pixel: the program's own
                    // sample mapping (uPos) and the colour texels.
                    for (int y = s.rect[1]; rendered && y < s.rect[1] + s.rect[3]; ++y)
                        for (int x = s.rect[0]; x < s.rect[0] + s.rect[2]; ++x)
                        {
                            const size_t p = (size_t)y * s.ow + x;
                            float range = 0.0f;
                            const float frx = (x + 0.5f) * ((float)s.rw / s.rect[2]) - s.rect[0] * ((float)s.rw / s.rect[2]);
                            const float fry = (y + 0.5f) * ((float)s.rh / s.rect[3]) - s.rect[1] * ((float)s.rh / s.rect[3]);
                            // Any jitter puts the corner at floor(fr - 0.5 + jitter): take the
                            // widest range over the three candidate corners per axis.
                            for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx)
                                    for (int c = 0; c < 3; ++c)
                                    {
                                        int lo = 255, hi = 0;
                                        for (int j = 0; j < 2; ++j)
                                            for (int i = 0; i < 2; ++i)
                                            {
                                                const int tx = std::min(std::max((int)std::floor(frx - 0.5f) + dx + i, 0), s.rw - 1);
                                                const int ty = std::min(std::max((int)std::floor(fry - 0.5f) + dy + j, 0), s.rh - 1);
                                                const int v = s.color[((size_t)ty * s.rw + tx) * 4 + c];
                                                lo = std::min(lo, v);
                                                hi = std::max(hi, v);
                                            }
                                        range = std::max(range, (hi - lo) / 255.0f);
                                    }
                            float diff = 0.0f;
                            for (int c = 0; c < 3; ++c)
                                diff = std::max(diff, std::fabs(a.color[p * 4 + c] - b.color[p * 4 + c]));
                            const bool still = fc.shift == 0.0f && !fc.motion;
                            if (diff > kTolerance)
                            {
                                ++moved;
                                movedStill += still;
                            }
                            if (still)
                            {
                                ++stillPixels;
                                worstStill = std::max(worstStill, diff);
                            }
                            else
                                worstOverRange = std::max(worstOverRange, diff - range);
                            ++pixels;
                        }
                }
            char name[64], detail[320];
            std::snprintf(name, sizeof(name), "ONE_TAP_FALLBACK_V%u", variant);
            std::snprintf(detail, sizeof(detail),
                          "pixels=%zu moved=%zu (%.1f%%) worst_over_range=%.2e still_pixels=%zu moved_still=%zu worst_still=%.2e %s",
                          pixels, moved, 100.0 * (double)moved / (double)std::max<size_t>(pixels, 1), worstOverRange,
                          stillPixels, movedStill, worstStill, err.c_str());
            // Moving frames: beyond the range only by rounding. Still frames:
            // the five taps collapse onto the centre exactly, so the one tap
            // changes nothing (identity reprojection lands on the texel).
            Check(rendered && pixels && moved * 25 > pixels - stillPixels && worstOverRange <= kTolerance &&
                      worstStill <= kTolerance,
                  name, detail);
            gl.glDeleteProgram(reference);
            gl.glDeleteProgram(program);
        }
    }

    // The present pass's display gamma: the engine's ramp is a power curve
    // (GammaFitExponent), and the 1:1 bilinear program's curve build maps
    // every 8-bit level to the ramp's entry within one 16-bit step, every
    // channel through its own value, alpha 1.
    for (const double gamma : {0.8, 2.2})
    {
        uint16_t ramp[256];
        for (int i = 0; i < 256; ++i)
            ramp[i] = (uint16_t)std::floor(std::pow(i / 255.0, 1.0 / gamma) * 65535.0 + 0.5);
        const float exponent = deko9::GammaFitExponent(ramp, ramp, ramp);
        std::string err;
        const std::string text = deko9::GammaVariant(deko9::kBilinearGlsl, true);
        const GLuint program = exponent > 0.0f && !text.empty() ? Link(text, &err, deko9::kFsrVertexGlsl) : 0;
        float worst = 1.0f, alpha = 1.0f;
        size_t changed = 0;
        bool rendered = program != 0;
        if (rendered)
        {
            // 16x16: red walks the levels, green runs backwards, blue a permutation.
            std::vector<uint8_t> source(16 * 16 * 4);
            for (int i = 0; i < 256; ++i)
            {
                source[i * 4] = (uint8_t)i;
                source[i * 4 + 1] = (uint8_t)(255 - i);
                source[i * 4 + 2] = (uint8_t)(i ^ 0x5a);
                source[i * 4 + 3] = 255;
            }
            std::vector<uint8_t> uniforms(deko9::GammaLutOffset(deko9::UPSCALE_BILINEAR) + sizeof(deko9::GammaCurveConstants));
            deko9::BilinearConstants block{{0.0f, 0.0f, 1.0f, 1.0f}, {0.5f / 16, 0.5f / 16, 15.5f / 16, 15.5f / 16}};
            deko9::GammaCurveConstants curve;
            deko9::GammaCurveSetup(&curve, exponent);
            std::memcpy(uniforms.data(), &block, sizeof(block));
            std::memcpy(uniforms.data() + deko9::GammaLutOffset(deko9::UPSCALE_BILINEAR), &curve, sizeof(curve));
            std::vector<float> out;
            rendered = RenderOne(program, 16, 16, source, uniforms, &out, &err);
            worst = 0.0f;
            // The pass puts texture row 0 at the top; GL reads rows bottom-up.
            for (int i = 0; rendered && i < 256; ++i)
            {
                const int src = (15 - i / 16) * 16 + i % 16;
                for (int c = 0; c < 3; ++c)
                {
                    const float want = (float)ramp[source[src * 4 + c]] / 65535.0f;
                    worst = std::max(worst, std::fabs(out[i * 4 + c] - want));
                    changed += std::fabs(want - source[src * 4 + c] / 255.0f) > 1.0f / 255.0f;
                }
                alpha = std::min(alpha, out[i * 4 + 3]);
            }
        }
        char name[64], detail[256];
        std::snprintf(name, sizeof(name), "PRESENT_GAMMA_%02d", (int)(gamma * 10.0 + 0.5));
        std::snprintf(detail, sizeof(detail), "exponent=%.5f changed_channels=%zu worst=%.2e (bar %.2e) alpha=%g %s",
                      exponent, changed, worst, 1.5 / 65535.0, alpha, err.c_str());
        Check(rendered && changed > 500 && worst <= 1.5f / 65535.0f && alpha == 1.0f, name, detail);
        if (program)
            gl.glDeleteProgram(program);
    }
    std::printf("%s:DEKO9_TAAU_GL\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
