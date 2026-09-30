// Native float-Z for the deko3d renderer (see rb_floatz_native.h).

#include "rb_floatz_native.h"


#include <deko9/deko9_fsr.h>
#include <deko9/deko9_native.h>
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>

#include "r_dvars.h"
#include "r_init.h"
#include "r_state.h"
#include "rb_backend.h"
#include "rb_gpupass.h"

#include <cstdio>

namespace
{
// What the build floatz technique leaves where no geometry was drawn: the
// shadowClear quad RB_DepthPrepassCallback draws first (its pixel shader
// writes 2,000,000).
constexpr float kFloatZClear = 2000000.0f;

// Periodic report (every kReportViews views that needed float-Z).
constexpr uint32_t kReportViews = 300;
uint32_t s_views, s_legacyViews, s_nativeViews;
uint64_t s_legacyDraws, s_drawsAtBegin;

uint64_t DrawCount()
{
    Deko9Counters counters;
    Deko9_GetCounters(dx.device, &counters);
    return counters.draws + counters.instancedDraws;
}

void Report(int mode)
{
    if (++s_views < kReportViews)
        return;
    Com_Printf(CON_CHANNEL_SYSTEM,
               "DEKO9 floatz mode=%d views=%u legacy_views=%u native_views=%u legacy_draws_per_view=%.1f\n", mode,
               s_views, s_legacyViews, s_nativeViews, s_legacyViews ? (double)s_legacyDraws / s_legacyViews : 0.0);
    s_views = s_legacyViews = s_nativeViews = 0;
    s_legacyDraws = 0;
}

bool BuildInto(const GfxViewInfo *viewInfo, IDirect3DSurface9 *target)
{
    const float(*m)[4] = viewInfo->viewParms.projectionMatrix.m;
    deko9::FloatZRange scene, viewmodel;
    scene.m22 = viewmodel.m22 = m[2][2];
    scene.m32 = m[3][2];
    // R_DeriveProjectionMatrix: depth hack replaces only m[3][2].
    viewmodel.m32 = viewInfo->viewParms.depthHackNearClip;
    R_GetDepthRangeValues(GFX_DEPTH_RANGE_SCENE, &scene.minZ, &scene.maxZ);
    R_GetDepthRangeValues(GFX_DEPTH_RANGE_VIEWMODEL, &viewmodel.minZ, &viewmodel.maxZ);
    deko9::FloatZConstants constants;
    deko9::FloatZSetup(&constants, scene, viewmodel, kFloatZClear);
    return Deko9_BuildFloatZ(dx.device, gfxRenderTargets[R_RENDERTARGET_SCENE].surface.depthStencil, target,
                             &constants);
}

} // namespace

int RB_NativeFloatZMode(const GfxViewInfo *viewInfo)
{
    if (!viewInfo->needsFloatZ || !r_deko9NativeFloatZ)
        return 0;
    const int mode = r_deko9NativeFloatZ->current.integer;
    if (!mode)
        return 0;
    static bool s_loggedCookie, s_loggedMsaa, s_loggedProjection;
    if (viewInfo->dynamicShadowType == SHADOW_COOKIE)
    {
        if (!s_loggedCookie)
            Com_Printf(CON_CHANNEL_SYSTEM, "DEKO9 floatz: shadow-cookie view keeps the geometry pass\n");
        s_loggedCookie = true;
        return 0;
    }
    if (dx.multiSampleType != D3DMULTISAMPLE_NONE)
    {
        if (!s_loggedMsaa)
            Com_Printf(CON_CHANNEL_SYSTEM, "DEKO9 floatz: multisampled scene keeps the geometry pass\n");
        s_loggedMsaa = true;
        return 0;
    }
    const float(*m)[4] = viewInfo->viewParms.projectionMatrix.m;
    if (m[0][3] != 0.0f || m[1][3] != 0.0f || m[2][3] != 1.0f || m[3][3] != 0.0f || !(m[2][2] > 0.0f))
    {
        if (!s_loggedProjection)
            Com_Printf(CON_CHANNEL_SYSTEM, "DEKO9 floatz: non-perspective view keeps the geometry pass\n");
        s_loggedProjection = true;
        return 0;
    }
    return mode;
}

void RB_LegacyFloatZBegin()
{
    s_drawsAtBegin = DrawCount();
}

void RB_LegacyFloatZEnd()
{
    s_legacyDraws += DrawCount() - s_drawsAtBegin;
    ++s_legacyViews;
}

void RB_NativeFloatZBeforeEmissive(const GfxViewInfo *viewInfo, int mode)
{
    if (!viewInfo->needsFloatZ)
        return;
    if (mode == 1)
    {
        RB_GPU_PASS(FloatZ);
        if (!BuildInto(viewInfo, gfxRenderTargets[R_RENDERTARGET_FLOAT_Z].surface.color))
            Com_Error(ERR_FATAL, "r_deko9NativeFloatZ: native float-z rebuild failed (see FAIL:DEKO9_FLOATZ)");
        ++s_nativeViews;
    }
    Report(mode);
}

