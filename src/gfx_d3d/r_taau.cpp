// Temporal anti-aliased upscaling for the deko3d renderer (see r_taau.h).

#include "r_taau.h"
#include "r_taau_motion.h"

#include <deko9/deko9_native.h>
#include <deko9/deko9_taau.h>
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>

#include "r_dvars.h"
#include "r_dynres.h"
#include "r_init.h"
#include "r_rendercmds.h"
#include "r_scene.h"
#include "r_state.h"
#include "rb_gpupass.h"
#include "rb_shade.h"

#include <cstring>
#include <vector>

namespace
{
const dvar_t *r_taau;
const dvar_t *r_taauBlend;
const dvar_t *r_taauReactive;
const dvar_t *r_taauLodBias;
const dvar_t *r_taauAntiFlicker;
const dvar_t *r_taauBilinearHistory;
const dvar_t *r_taauBilinearCurrent;
const dvar_t *r_taauReactiveHalf;
const dvar_t *r_taauFlat;
const dvar_t *r_taauBilinearRange;

// A view that moved or turned more than this between two frames starts a
// new history.
constexpr float kCutMove = 64.0f;
constexpr float kCutTurnCos = 0.5f;
// A zoom by more than this in one frame (a scope overlay snapping in, a
// cinematic cut to another lens) starts a new history; an ADS transition
// zooms gradually and reprojects.
constexpr float kZoomCut = 1.2f;

struct History
{
    bool valid = false;
    const void *world = nullptr;
    int frame = 0;
    int32_t width = 0, height = 0; // output size: a render size change keeps the history
    float zoom = 0.0f;             // the projection's x scale
    deko9::TaauMat viewProj{};
    float origin[3] = {}, forward[3] = {}, viewOffset[3] = {};
} s_history;

std::vector<deko9::TaauMotionDraw> s_motionDraws;
} // namespace

bool R_TaauActive()
{
    // Any scene-layout frame, fixed or adaptive, 1 included; native has no
    // scene target to resolve.
    return render_scale::UpscalerFor(R_RenderScaleMode(), r_taau && r_taau->current.integer) ==
           render_scale::Upscaler::Taau;
}

void R_TaauRegisterDvars()
{
    // On: the temporal resolve replaces the spatial upscaler (r_fsrMode) on the scene layout.
    r_taau = Dvar_RegisterInt("r_taau", 1, DvarLimits(0, 1), DVAR_NOFLAG,
                              "deko3d renderer, when the scene renders at r_renderScale below 1 or under r_dynres: "
                              "temporal anti-aliased upscaling instead of r_fsrMode: jittered scene, per-object "
                              "motion, reactive transparents and texture LOD bias resolved to the output size "
                              "(native r_renderScale 1 without r_dynres: no effect)");
    DvarLimits blendLimits;
    blendLimits.value.min = 0.01f;
    blendLimits.value.max = 1.0f;
    r_taauBlend = Dvar_RegisterFloat("r_taauBlend", 0.1f, blendLimits, DVAR_NOFLAG,
                                     "deko3d renderer: r_taau weight of the current frame where a sample lands on "
                                     "the output pixel");
    DvarLimits reactiveLimits;
    reactiveLimits.value.min = 0.0f;
    reactiveLimits.value.max = 1.0f;
    r_taauReactive = Dvar_RegisterFloat("r_taauReactive", 0.6f, reactiveLimits, DVAR_NOFLAG,
                                        "deko3d renderer: r_taau weight of the current frame where particles and "
                                        "other transparents changed the pixel (0 = no reactive passes)");
    DvarLimits lodLimits;
    lodLimits.value.min = -1.0f;
    lodLimits.value.max = 1.0f;
    r_taauLodBias = Dvar_RegisterFloat("r_taauLodBias", 0.0f, lodLimits, DVAR_NOFLAG,
                                       "deko3d renderer: added to r_taau's scene texture LOD bias of log2(render / "
                                       "output width); the sum is clamped to -15/8..0 in 1/8 steps");
    r_taauAntiFlicker = Dvar_RegisterFloat("r_taauAntiFlicker", 0.5f, reactiveLimits, DVAR_NOFLAG,
                                           "deko3d renderer: how much less r_taau weighs the current frame where its "
                                           "luma agrees with the history (0 = never)");
    r_taauBilinearHistory = Dvar_RegisterBool("r_taauBilinearHistory", false, DVAR_NOFLAG,
                                              "deko3d renderer: r_taau fetches the history with one bilinear tap "
                                              "instead of Catmull-Rom (cheaper, blurrier under camera motion)");
    r_taauBilinearCurrent = Dvar_RegisterBool("r_taauBilinearCurrent", false, DVAR_NOFLAG,
                                              "deko3d renderer: r_taau weighs the 2x2 scene texels under the output "
                                              "pixel bilinearly instead of by the polynomial kernel (cheaper, softer)");
    r_taauReactiveHalf = Dvar_RegisterBool("r_taauReactiveHalf", true, DVAR_NOFLAG,
                                           "deko3d renderer: r_taau keeps the transparents' luma change at half the "
                                           "scene size (cheaper reactive passes, a softer mask)");
    DvarLimits flatLimits;
    flatLimits.value.min = 0.0f;
    flatLimits.value.max = 16.0f;
    r_taauFlat = Dvar_RegisterFloat("r_taauFlat", deko9::kTaauFlatDefault, flatLimits, DVAR_NOFLAG,
                                    "deko3d renderer: r_taau skips the history where the 2x2 scene texels around "
                                    "the output pixel differ by no more than this (8-bit units; the history would "
                                    "be clamped to that range anyway)");
    r_taauBilinearRange = Dvar_RegisterFloat(
        "r_taauBilinearRange", deko9::kTaauBilinearRangeDefault, flatLimits, DVAR_NOFLAG,
        "deko3d renderer: r_taau fetches the Catmull-Rom history with one bilinear tap where the 2x2 scene texels "
        "around the output pixel differ by less than this (8-bit units; the history is clamped to that range, so "
        "the filters cannot differ by more; 0 = always the five taps)");
}

void R_TaauJitterView(GfxViewParms *viewParms, const GfxViewport &sceneViewport)
{
    if (!R_TaauActive() || sceneViewport.width <= 0 || sceneViewport.height <= 0)
        return;
    float jx, jy, m20, m21;
    deko9::TaauHalton(rg.frontEndFrameCount, &jx, &jy);
    deko9::TaauJitterClip(jx, jy, (uint32_t)sceneViewport.width, (uint32_t)sceneViewport.height, &m20, &m21);
    viewParms->projectionMatrix.m[2][0] += m20;
    viewParms->projectionMatrix.m[2][1] += m21;
    R_SetupViewProjectionMatrices(viewParms);
}

// The frame was built for r_taau: its projection carries the jitter (Halton
// never yields (0, 0)).
static bool Jittered(const GfxViewParms &vp)
{
    return vp.projectionMatrix.m[2][0] != 0.0f || vp.projectionMatrix.m[2][1] != 0.0f;
}

void RB_TaauBeginFrame(float scale)
{
    const float bias = R_TaauActive() ? deko9::TaauLodBias(scale, r_taauLodBias->current.value) : 0.0f;
    Deko9_SetEngineLodBias(dx.device, bias);
}

namespace
{
// The reactive passes around the first view's transparents.
void ReactivePass(const GfxViewInfo *viewInfo, bool after)
{
    const GfxBackEndData *data = viewInfo->input.data;
    IDirect3DBaseTexture9 *scene = RB_DynResSceneTexture();
    if (!R_TaauActive() || r_taauReactive->current.value <= 0.0f || !scene || !data ||
        viewInfo != &data->viewInfo[0] || !Jittered(viewInfo->viewParms))
        return;
    if (tess.indexCount)
        RB_EndTessSurface();
    RB_GPU_PASS(TaauReactive);
    const GfxViewport &sv = viewInfo->sceneViewport;
    const int32_t src[4] = {sv.x, sv.y, sv.width, sv.height};
    if (!Deko9_TaauOpaque(dx.device, scene, src, after, r_taauReactiveHalf->current.enabled))
        Com_Error(ERR_FATAL, "r_taau: reactive pass %dx%d failed (see FAIL:DEKO9_TAAU_OPAQUE)", sv.width, sv.height);
    // The emissive draws around the passes keep their own bucket.
    RB_GPU_PASS(Emissive);
}
} // namespace

void RB_TaauBeforeEmissive(const GfxViewInfo *viewInfo)
{
    ReactivePass(viewInfo, false);
}

void RB_TaauAfterEmissive(const GfxViewInfo *viewInfo)
{
    ReactivePass(viewInfo, true);
}

bool RB_TaauResolveView(const GfxViewInfo *viewInfo, IDirect3DBaseTexture9 *scene, IDirect3DSurface9 *depth,
                        const int32_t srcRect[4], IDirect3DSurface9 *dst, const int32_t dstRect[4])
{
    // The 2D after the scene samples without the scene's bias.
    Deko9_SetEngineLodBias(dx.device, 0.0f);
    const GfxViewParms &vp = viewInfo->viewParms;
    const float(*p)[4] = vp.projectionMatrix.m;
    // The frame was built without jitter (r_taau turned on since): upscale
    // as usual; Halton never yields (0, 0).
    if (!R_TaauActive() || !Jittered(vp))
        return false;
    RB_GPU_PASS(TaauMotion);
    deko9::TaauFrame f{};
    deko9::TaauJitterFromClip(p[2][0], p[2][1], (uint32_t)srcRect[2], (uint32_t)srcRect[3], &f.jitter[0],
                              &f.jitter[1]);
    deko9::TaauMat view, proj;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            view.m[i][j] = vp.viewMatrix.m[i][j];
            proj.m[i][j] = p[i][j];
        }
    proj.m[2][0] = proj.m[2][1] = 0.0;
    const deko9::TaauMat viewProj = deko9::TaauMul(view, proj);

    History &h = s_history;
    bool reset = !h.valid || h.world != rgp.world || h.frame + 1 != r_glob.backEndFrameCount ||
                 h.width != dstRect[2] || h.height != dstRect[3] || deko9::TaauZoomCut(p[0][0], h.zoom, kZoomCut) ||
                 deko9::TaauCameraCut(vp.origin, vp.axis[0], h.origin, h.forward, kCutMove, kCutTurnCos);
    deko9::TaauMat reproj{};
    if (reset || !deko9::TaauReprojection(viewProj, h.viewProj, &reproj))
    {
        reset = true;
        for (int i = 0; i < 4; ++i)
            reproj.m[i][i] = 1.0;
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            f.reproj[i][j] = (float)reproj.m[i][j];
    if (!reset)
    {
        RB_TaauMotionMatrices m;
        m.viewProj = viewProj;
        deko9::TaauMat depthHackProj = proj;
        depthHackProj.m[3][2] = vp.depthHackNearClip;
        m.viewProjDepthHack = deko9::TaauMul(view, depthHackProj);
        m.prevViewProj = h.viewProj;
        std::memcpy(m.viewOffset, viewInfo->sceneDef.viewOffset, sizeof(m.viewOffset));
        std::memcpy(m.prevViewOffset, h.viewOffset, sizeof(m.prevViewOffset));
        const uint32_t count = RB_TaauBuildMotion(viewInfo, m, &s_motionDraws);
        deko9::TaauMotionView mv;
        mv.jitterClip[0] = p[2][0];
        mv.jitterClip[1] = p[2][1];
        R_GetDepthRangeValues(GFX_DEPTH_RANGE_SCENE, &mv.sceneDepth[0], &mv.sceneDepth[1]);
        R_GetDepthRangeValues(GFX_DEPTH_RANGE_VIEWMODEL, &mv.viewmodelDepth[0], &mv.viewmodelDepth[1]);
        if (count && !Deko9_TaauMotion(dx.device, depth, srcRect, &mv, s_motionDraws.data(), count))
            Com_Error(ERR_FATAL, "r_taau: motion pass of %u draws failed (see FAIL:DEKO9_TAAU_MOTION)", count);
    }
    float vmMin;
    R_GetDepthRangeValues(GFX_DEPTH_RANGE_VIEWMODEL, &vmMin, &f.viewmodelSplit);
    R_GetDepthRangeValues(GFX_DEPTH_RANGE_SCENE, &f.sceneMinZ, &f.sceneMaxZ);
    f.farNdcZ = p[2][2];
    f.blend = r_taauBlend->current.value;
    f.reactive = r_taauReactive->current.value;
    f.antiFlicker = r_taauAntiFlicker->current.value;
    f.bilinearHistory = r_taauBilinearHistory->current.enabled;
    f.bilinearCurrent = r_taauBilinearCurrent->current.enabled;
    f.flat = r_taauFlat->current.value / 255.0f;
    f.bilinearRange = r_taauBilinearRange->current.value / 255.0f;
    f.reset = reset;
    RB_GPU_PASS(TaauResolve);
    if (!Deko9_TaauResolve(dx.device, scene, depth, srcRect, dst, dstRect, &f))
        Com_Error(ERR_FATAL, "r_taau: resolve %dx%d -> %dx%d failed (see FAIL:DEKO9_TAAU)", srcRect[2], srcRect[3],
                  dstRect[2], dstRect[3]);
    h.valid = true;
    h.world = rgp.world;
    h.frame = r_glob.backEndFrameCount;
    h.width = dstRect[2];
    h.height = dstRect[3];
    h.zoom = p[0][0];
    h.viewProj = viewProj;
    std::memcpy(h.origin, vp.origin, sizeof(h.origin));
    std::memcpy(h.forward, vp.axis[0], sizeof(h.forward));
    std::memcpy(h.viewOffset, viewInfo->sceneDef.viewOffset, sizeof(h.viewOffset));
    return true;
}
