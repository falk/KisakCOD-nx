// Scene render scale and the dynamic resolution controller for the deko3d
// renderer (see r_dynres.h).

#include "r_dynres.h"


#include <deko9/deko9_native.h>
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>

#include "r_dvars.h"
#include "r_dynres_controller.h"
#include "r_image.h"
#include "r_init.h"
#include "r_rendercmds.h"
#include "r_render_scale.h"
#include "r_rendertarget.h"
#include "r_state.h"
#include "r_taau.h"
#include "rb_state.h"
#include "rb_backend.h"
#include "rb_gpupass.h"
#include "rb_shade.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

extern uint32_t s_smpFrame; // r_rendercmds.cpp: frontEndDataOut = &s_backEndData[s_smpFrame]

namespace
{
int s_enabled = -1;    // the scene layout; -1: not read yet
bool s_controller;     // r_dynres when the layout was chosen
bool s_controllerHeld; // R_DynResHoldController: r_renderScale sets the size
float s_nativeScaleSeen = 1.0f; // r_renderScale last seen with the layout off

dynres::Config s_cfg;
dynres::Controller s_ctl;
bool s_ctlReady;

// Front end: the size of the frame being built, and per SMP frame the size
// each frame's back end must apply.
dynres::Size s_front;
struct FrameSize
{
    const GfxBackEndData *data;
    dynres::Size size;
};
FrameSize s_frames[2];
uint32_t s_lastFrontFrame = UINT32_MAX;
uint32_t s_lastGpuCount;

// Back end: the size the targets are laid out at.
dynres::Size s_back;

// The scene colour texture (R_RENDERTARGET_SCENE's image-less colour
// surface lives in it) and depth surface; references held here.
IDirect3DTexture9 *s_sceneTexture;
uint64_t s_moves;
IDirect3DSurface9 *s_sceneDepth;

struct Target
{
    GfxRenderTargetId id;
    int shift; // picmip of the target (post-effect / ping-pong: 2)
};
std::vector<Target> s_targets;

// 60-frame report.
struct Stats
{
    uint32_t frames = 0, samples = 0, changes = 0;
    float scaleMin = 1e9f, scaleMax = 0.0f;
    double scaleSum = 0.0, gpuSum = 0.0;
    float gpuMax = 0.0f;
    uint64_t drops0 = 0, raises0 = 0;
} s_stats;

render_scale::Mode FrameMode()
{
    return render_scale::ModeFor(R_SceneLayoutEnabled(), s_controller && !s_controllerHeld);
}

dynres::Config ConfigFromDvars()
{
    dynres::Config c;
    c.maxWidth = (int)vidConfig.sceneWidth;
    c.maxHeight = (int)vidConfig.sceneHeight;
    c.budgetMs = dynres::BudgetForFrameCap(com_maxfps ? com_maxfps->current.integer : 0);
    c.minScale = std::min(r_dynresMin->current.value, r_dynresMax->current.value);
    c.maxScale = std::max(r_dynresMin->current.value, r_dynresMax->current.value);
    return c;
}

bool SameConfig(const dynres::Config &a, const dynres::Config &b)
{
    return a.maxWidth == b.maxWidth && a.maxHeight == b.maxHeight && a.budgetMs == b.budgetMs &&
           a.minScale == b.minScale && a.maxScale == b.maxScale;
}

float FakeGpuMs(uint32_t width, uint32_t height)
{
    const float full = r_dynresFakeGpuMs->current.value;
    const int wave = r_dynresFakeWave->current.integer;
    const float pixels = (float)width * (float)height / ((float)s_cfg.maxWidth * (float)s_cfg.maxHeight);
    const bool light = wave > 0 && (rg.frontEndFrameCount / (uint32_t)wave) & 1;
    return full * pixels * (light ? 0.6f : 1.0f);
}

void Report(const dynres::Size &size)
{
    const float scale = (float)size.width / (float)s_cfg.maxWidth;
    s_stats.scaleMin = std::min(s_stats.scaleMin, scale);
    s_stats.scaleMax = std::max(s_stats.scaleMax, scale);
    s_stats.scaleSum += scale;
    if (++s_stats.frames < 60)
        return;
    Com_Printf(CON_CHANNEL_GFX,
               "DEKO9 dynres frames=%u mode=%s upscaler=%s render=%dx%d scale_min=%.3f scale_avg=%.3f "
               "scale_max=%.3f "
               "gpu_avg=%.2fms gpu_max=%.2fms samples=%u changes=%u drops=%llu raises=%llu budget=%.1fms "
               "min=%.3f max=%.3f fake=%.1f backoff=%d fsr=%s\n",
               s_stats.frames, render_scale::ModeName(FrameMode()),
               render_scale::UpscalerName(render_scale::UpscalerFor(FrameMode(), R_TaauActive())), size.width,
               size.height, s_stats.scaleMin,
               s_stats.scaleSum / s_stats.frames, s_stats.scaleMax,
               s_stats.samples ? s_stats.gpuSum / s_stats.samples : 0.0, s_stats.gpuMax, s_stats.samples,
               s_stats.changes, (unsigned long long)(s_ctl.drops - s_stats.drops0),
               (unsigned long long)(s_ctl.raises - s_stats.raises0), s_cfg.budgetMs, s_cfg.minScale, s_cfg.maxScale,
               r_dynresFakeGpuMs->current.value, s_ctl.Backoff(),
               r_fsrMode ? Dvar_EnumToString(r_fsrMode) : "?");
    const uint64_t drops = s_ctl.drops, raises = s_ctl.raises;
    s_stats = Stats();
    s_stats.drops0 = drops;
    s_stats.raises0 = raises;
}

IDirect3DResource9 *ColorResource(const Target &t)
{
    if (t.id == R_RENDERTARGET_SCENE)
        return s_sceneTexture;
    GfxImage *image = gfxRenderTargets[t.id].image;
    return image ? image->texture.map : nullptr;
}

void Apply(const dynres::Size &size)
{
    std::vector<IDirect3DResource9 *> done;
    for (const Target &t : s_targets)
    {
        const uint32_t w = std::max(1, size.width >> t.shift), h = std::max(1, size.height >> t.shift);
        IDirect3DResource9 *color = ColorResource(t);
        if (color && std::find(done.begin(), done.end(), color) == done.end())
        {
            if (!Deko9_ResizeRenderTarget(dx.device, color, w, h))
                Com_Error(ERR_FATAL, "r_renderScale: cannot resize %s to %ux%u", R_RenderTargetName(t.id), w, h);
            done.push_back(color);
        }
        GfxRenderTarget &rt = gfxRenderTargets[t.id];
        rt.width = (uint16_t)w;
        rt.height = (uint16_t)h;
        if (rt.image)
        {
            rt.image->width = (uint16_t)w;
            rt.image->height = (uint16_t)h;
        }
    }
    if (!Deko9_ResizeRenderTarget(dx.device, s_sceneDepth, (uint32_t)size.width, (uint32_t)size.height))
        Com_Error(ERR_FATAL, "r_renderScale: cannot resize the scene depth to %dx%d", size.width, size.height);
    s_back = size;
}

} // namespace

bool R_SceneLayoutEnabled()
{
    if (s_enabled < 0)
    {
        s_controller = r_dynres && r_dynres->current.enabled;
        s_controllerHeld = false;
        s_enabled = render_scale::SceneLayoutFor(s_controller, r_renderScale ? r_renderScale->current.value : 1.0f)
                        ? 1
                        : 0;
    }
    return s_enabled == 1;
}

bool R_DynResControllerEnabled()
{
    R_SceneLayoutEnabled();
    return s_controller;
}

render_scale::Mode R_RenderScaleMode()
{
    return FrameMode();
}

void R_DynResHoldController()
{
    s_controllerHeld = true;
}

void R_DynResInitSceneTarget(GfxRenderTarget *scene, uint32_t d3dFormat)
{
    const uint32_t w = vidConfig.sceneWidth, h = vidConfig.sceneHeight;
    HRESULT hr = dx.device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, (D3DFORMAT)d3dFormat, D3DPOOL_DEFAULT,
                                          &s_sceneTexture, nullptr);
    if (FAILED(hr) || !s_sceneTexture)
        Com_Error(ERR_FATAL, "r_renderScale: couldn't create the %ux%u scene target: %s", w, h, R_ErrorDescription(hr));
    hr = s_sceneTexture->GetSurfaceLevel(0, &scene->surface.color);
    if (FAILED(hr))
        Com_Error(ERR_FATAL, "r_renderScale: scene target surface: %s", R_ErrorDescription(hr));
    hr = dx.device->CreateDepthStencilSurface(w, h, dx.depthStencilFormat, D3DMULTISAMPLE_NONE, 0, 0,
                                              &scene->surface.depthStencil, nullptr);
    if (FAILED(hr))
        Com_Error(ERR_FATAL, "r_renderScale: couldn't create the %ux%u scene depth: %s", w, h, R_ErrorDescription(hr));
    s_sceneDepth = scene->surface.depthStencil;
    s_sceneDepth->AddRef();
    scene->image = nullptr; // never sampled by materials; the upscale reads s_sceneTexture
    scene->width = (uint16_t)w;
    scene->height = (uint16_t)h;
    // Every other scene-sized render target shares this depth
    // (R_AssignSingleSampleDepthStencilSurface), so one resize covers them.
    dx.singleSampleDepthStencilSurface = s_sceneDepth;
    Com_Printf(CON_CHANNEL_GFX,
               "r_renderScale: scene targets %ux%u (output %dx%d), %s, upscale %s (r_fsrMode %s) before the 2D "
               "pass\n",
               w, h, vidConfig.displayWidth, vidConfig.displayHeight,
               s_controller ? va("adaptive r_dynres %.3f..%.3f budget %.1f ms", r_dynresMin->current.value,
                                 r_dynresMax->current.value,
                                 dynres::BudgetForFrameCap(com_maxfps ? com_maxfps->current.integer : 0))
                            : va("fixed r_renderScale %.3f", r_renderScale->current.value),
               render_scale::UpscalerName(render_scale::UpscalerFor(FrameMode(), R_TaauActive())),
               Dvar_EnumToString(r_fsrMode));
}

void R_DynResRegisterTargets()
{
    s_targets.clear();
    if (!R_SceneLayoutEnabled())
        return;
    static const Target kTargets[] = {
        {R_RENDERTARGET_SCENE, 0},         {R_RENDERTARGET_FLOAT_Z, 0},       {R_RENDERTARGET_RESOLVED_SCENE, 0},
        {R_RENDERTARGET_RESOLVED_POST_SUN, 0}, {R_RENDERTARGET_DYNAMICSHADOWS, 0}, {R_RENDERTARGET_POST_EFFECT_0, 2},
        {R_RENDERTARGET_POST_EFFECT_1, 2}, {R_RENDERTARGET_PINGPONG_0, 2},    {R_RENDERTARGET_PINGPONG_1, 2},
    };
    for (const Target &t : kTargets)
    {
        if (gfxRenderTargets[t.id].surface.color)
            s_targets.push_back(t);
    }
    s_back = {(int)vidConfig.sceneWidth, (int)vidConfig.sceneHeight};
    s_front = s_back;
    s_frames[0] = s_frames[1] = {nullptr, s_back};
    s_cfg = ConfigFromDvars();
    s_ctl.Reset(s_cfg, dynres::TopLevel(s_cfg));
    s_ctlReady = true;
    s_lastFrontFrame = UINT32_MAX;
    s_stats = Stats();
}

void R_DynResShutdownTargets()
{
    if (s_sceneTexture)
        s_sceneTexture->Release();
    if (s_sceneDepth)
        s_sceneDepth->Release();
    s_sceneTexture = nullptr;
    s_sceneDepth = nullptr;
    s_targets.clear();
    s_ctlReady = false;
    s_enabled = -1; // vid_restart re-reads r_dynres and r_renderScale
}

void R_DynResBeginFrame()
{
    if (!R_SceneLayoutEnabled())
    {
        // Started native: the scene has no target of its own to resize.
        if (r_renderScale && r_renderScale->current.value != s_nativeScaleSeen)
        {
            s_nativeScaleSeen = r_renderScale->current.value;
            if (s_nativeScaleSeen < 1.0f)
                Com_Printf(CON_CHANNEL_GFX,
                           "r_renderScale %.3f applies after vid_restart: the game started at native size\n",
                           s_nativeScaleSeen);
        }
        return;
    }
    if (!s_ctlReady || !dx.device)
        return;
    if (s_lastFrontFrame != rg.frontEndFrameCount)
    {
        s_lastFrontFrame = rg.frontEndFrameCount;
        const dynres::Config cfg = ConfigFromDvars();
        if (!SameConfig(cfg, s_cfg))
        {
            s_cfg = cfg;
            s_ctl.Configure(cfg);
        }
        float gpuMs;
        uint32_t w, h, count;
        if (Deko9_GetGpuFrame(dx.device, &gpuMs, &w, &h, &count) && count != s_lastGpuCount)
        {
            s_lastGpuCount = count;
            if (r_dynresFakeGpuMs->current.value > 0.0f)
                gpuMs = FakeGpuMs(w, h);
            ++s_stats.samples;
            s_stats.gpuSum += gpuMs;
            s_stats.gpuMax = std::max(s_stats.gpuMax, gpuMs);
            if (FrameMode() == render_scale::Mode::Adaptive)
                s_ctl.Sample(gpuMs, {(int)w, (int)h});
        }
        // The controller is one driver of the scale; the size below is all
        // the rest of the frame sees.
        const dynres::Size controller = s_ctl.Current();
        const float scale = render_scale::FrameScale(FrameMode(), r_renderScale->current.value,
                                                     (float)controller.width / (float)s_cfg.maxWidth);
        dynres::Size size = controller;
        if (FrameMode() != render_scale::Mode::Adaptive)
        {
            dynres::Config any = s_cfg;
            any.minScale = render_scale::kMinScale;
            any.maxScale = 1.0f;
            size = dynres::SizeForLevel(any, dynres::LevelForScale(any, scale));
        }
        if (size != s_front)
            ++s_stats.changes;
        s_front = size;
        Report(size);
    }
    s_frames[s_smpFrame & 1] = {frontEndDataOut, s_front};
}

uint32_t R_DynResSceneWidth()
{
    return R_SceneLayoutEnabled() && s_front.width ? (uint32_t)s_front.width : vidConfig.sceneWidth;
}

uint32_t R_DynResSceneHeight()
{
    return R_SceneLayoutEnabled() && s_front.height ? (uint32_t)s_front.height : vidConfig.sceneHeight;
}

void RB_DynResBeginFrame(const GfxBackEndData *data)
{
    if (!R_SceneLayoutEnabled() || s_targets.empty())
        return;
    dynres::Size size = s_back;
    for (const FrameSize &f : s_frames)
    {
        if (f.data == data)
            size = f.size;
    }
    if (size != s_back)
        Apply(size);
    Deko9_SetFrameTag(dx.device, (uint32_t)s_back.width, (uint32_t)s_back.height);
    RB_TaauBeginFrame((float)s_back.width / (float)s_cfg.maxWidth);
}

int RB_DynResPostTarget()
{
    return R_SceneLayoutEnabled() ? R_RENDERTARGET_SCENE : R_RENDERTARGET_FRAME_BUFFER;
}

IDirect3DBaseTexture9 *RB_DynResSceneTexture()
{
    return R_SceneLayoutEnabled() ? s_sceneTexture : nullptr;
}

void RB_DynResResolveView(const GfxViewInfo *viewInfo, bool firstView)
{
    if (!R_SceneLayoutEnabled() || !s_sceneTexture)
        return;
    if (tess.indexCount)
        RB_EndTessSurface();
    RB_GPU_PASS(Upscale);
    R_SetRenderTargetSize(&gfxCmdBufSourceState, R_RENDERTARGET_FRAME_BUFFER);
    R_SetRenderTarget(gfxCmdBufContext, R_RENDERTARGET_FRAME_BUFFER);
    const GfxViewport &sv = viewInfo->sceneViewport;
    const GfxViewport &dv = viewInfo->displayViewport;
    // A view that does not cover the screen: the rest of the back buffer is
    // what the shared scene target's full clear left there (black), as
    // when the scene rendered into the back buffer.
    if (firstView && !viewInfo->isRenderingFullScreen)
        R_ClearScreen(gfxCmdBufState.prim.device, 1u, colorBlack, 1.0f, 0, nullptr);
    const int32_t src[4] = {sv.x, sv.y, sv.width, sv.height};
    const int32_t dst[4] = {dv.x, dv.y, dv.width, dv.height};
    if (sv.x < 0 || sv.y < 0 || sv.x + sv.width > s_back.width || sv.y + sv.height > s_back.height)
        Com_Error(ERR_FATAL, "r_renderScale: scene viewport %d,%d %dx%d outside the %dx%d scene target", sv.x, sv.y,
                  sv.width, sv.height, s_back.width, s_back.height);
    if (firstView && RB_TaauResolveView(viewInfo, s_sceneTexture, s_sceneDepth, src,
                                        gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER].surface.color, dst))
        return;
    // At scale 1 the upscale is a whole-image copy and the scene is not read
    // again this frame, so hand its image to the back buffer instead; the
    // next frame's scene clear rewrites the scene target.
    IDirect3DSurface9 *back = gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER].surface.color;
    D3DSURFACE_DESC backDesc{};
    if (firstView && viewInfo->isRenderingFullScreen && sv.x == 0 &&
        sv.y == 0 && sv.width == s_back.width && sv.height == s_back.height && dv.x == 0 && dv.y == 0 &&
        dv.width == sv.width && dv.height == sv.height && SUCCEEDED(back->GetDesc(&backDesc)) &&
        (int)backDesc.Width == dv.width && (int)backDesc.Height == dv.height &&
        Deko9_IsCompressed(s_sceneTexture) == Deko9_IsCompressed(back))
    {
        if (!Deko9_MoveContents(dx.device, s_sceneTexture, back))
            Com_Error(ERR_FATAL, "r_renderScale: scene -> back buffer move failed (see FAIL:DEKO9_MOVE_CONTENTS)");
        if (!s_moves++)
            Com_Printf(CON_CHANNEL_SYSTEM, "R_DYNRES_MOVE first scene -> back buffer move %dx%d\n", dv.width,
                       dv.height);
        return;
    }
    if (!Deko9_UpscaleSurface(dx.device, s_sceneTexture, src, gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER].surface.color,
                              dst))
        Com_Error(ERR_FATAL, "r_renderScale: scene upscale %dx%d -> %dx%d failed", sv.width, sv.height, dv.width,
                  dv.height);
}

