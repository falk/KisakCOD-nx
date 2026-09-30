#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include <universal/q_shared.h>
#include "cl_scrn.h"
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_rendercmds.h>
#include <universal/com_files.h>
#include <database/database.h>
#include <platform/switch/switch_diag_dvars.h>
#include "client.h"
#include <ui/ui.h>
#include <game/g_local.h>
#include "cl_demo.h"
#include <cgame/cg_view.h>
#include <devgui/devgui.h>
#include <qcommon/threads.h>
#include <port/switch_perf.h>
#ifdef __SWITCH__
#include <platform/switch/switch_platform.h>
#include <client/cl_input.h>
extern bool Sys_IsMainThread();
#else
#include <win32/win_local.h>
#endif
#include <qcommon/cmd.h>
#include <gfx_d3d/r_screenshot.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_drawsurf.h>
#include <gfx_d3d/rb_stats.h>
#include <gfx_d3d/r_cinematic.h>
#include <gfx_d3d/r_add_staticmodel.h>

const char *WeaponStateNames_51[27] =
{
  "WEAPON_READY",
  "WEAPON_RAISING",
  "WEAPON_RAISING_ALTSWITCH",
  "WEAPON_DROPPING",
  "WEAPON_DROPPING_QUICK",
  "WEAPON_FIRING",
  "WEAPON_RECHAMBERING",
  "WEAPON_RELOADING",
  "WEAPON_RELOADING_INTERUPT",
  "WEAPON_RELOAD_START",
  "WEAPON_RELOAD_START_INTERUPT",
  "WEAPON_RELOAD_END",
  "WEAPON_MELEE_INIT",
  "WEAPON_MELEE_FIRE",
  "WEAPON_MELEE_END",
  "WEAPON_OFFHAND_INIT",
  "WEAPON_OFFHAND_PREPARE",
  "WEAPON_OFFHAND_HOLD",
  "WEAPON_OFFHAND_START",
  "WEAPON_OFFHAND",
  "WEAPON_OFFHAND_END",
  "WEAPON_DETONATING",
  "WEAPON_SPRINT_RAISE",
  "WEAPON_SPRINT_LOOP",
  "WEAPON_SPRINT_DROP",
  "WEAPON_NIGHTVISION_WEAR",
  "WEAPON_NIGHTVISION_REMOVE"
};

const char *szShotName[6] =
{ "_rt", "_lf", "_bk", "_ft", "_up", "_dn" };


int scr_initialized;
bool updateScreenCalled;

void __cdecl TRACK_cl_srcn()
{
    ;
}

void __cdecl SCR_DrawSmallStringExt(unsigned int x, int y, const char *string, const float *setColor)
{
    R_AddCmdDrawText(string, 0x7FFFFFFF, cls.consoleFont, x, y, 1.0f, 1.0f, 0.0f, setColor, 0);
}

void __cdecl SCR_Init()
{
    scr_initialized = 1;
}

bool __cdecl CL_IsCGameRendering()
{
    return cls.uiStarted && !UI_IsFullscreen() && clientUIActives[0].connectionState == CA_ACTIVE;
}

DemoType CL_GetDemoType()
{
    if (G_DemoPlaying())
        return DEMO_TYPE_SERVER;
    else
        return (DemoType)CL_DemoPlaying();
}

int __cdecl CL_CGameRendering()
{
    int animFrametime; // r31
    DemoType DemoType; // r3

    if (clientUIActives[0].connectionState != CA_ACTIVE)
        return 0;
    if (UI_IsFullscreen())
        return 0;
    R_BeginClientCmdList2D();
    animFrametime = cls.animFrametime;
    DemoType = CL_GetDemoType();
    if (!CG_DrawActiveFrame(0, clients[0].serverTime, DemoType, CUBEMAPSHOT_NONE, 0, animFrametime))
        return 0;
    if ((clientUIActives[0].keyCatchers & KEYCATCH_UI) != 0)
        UI_Refresh();
    R_AddCmdEndOfList();
    return 1;
}

void CL_DrawScreen()
{
    if (clientUIActives[0].connectionState == CA_ACTIVE)
    {
        //Profile_Begin(349);
        CG_DrawFullScreenDebugOverlays(0);
        //Profile_EndInternal(0);
    }
    R_AddCmdDrawProfile();
    Con_DrawConsole(0);
    DevGui_Draw(0);
}

static void SCR_ClearScreen()
{
    // Loading is rendered over a solid dark field.  The old purple diagnostic
    // clear made the status overlay look like a placeholder and, when the
    // clear was skipped by the backend, exposed the white swapchain surface.
    static const float s_clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    R_AddCmdClearScreen(1, s_clearColor, 1.0, 0);
}

// Quake-style loading readout for the retail fastfile boot loads. Runs in
// the existing clear-only branches below (which is all the render thread
// pumps while the database thread streams zones), so it needs no new frame
// pump and never touches UI state that the DB thread may be registering.
// Text draws only once the real console font asset exists (before that the
// handle is a glyph-less default); the progress bar only needs the white
// material, which always has its built-in default.
//
// A mission load is not one of those branches: retail starts the mission's
// Bink movie there (CL_MapLoading_StartCinematic -> R_Cinematic_StartPlayback)
// and the briefing menu paints it with ownerdraw 277 fullscreen.  That movie
// *is* the loading screen -- its bottom edge already carries the game's own
// segmented progress bar and the mission brief text -- so the readout draws
// only while no cinematic is on screen, to avoid stacking its own bar on top
// of the movie's. Suppressing it there also covers a movie that failed to
// open and a load that outlasts the movie, where the readout is the only
// feedback the port has.
static void SCR_DrawLoadingStatus()
{
    if (R_Cinematic_IsStarted())
        return;
    uint32_t percent;
    if (!FS_GetRetailLoadStatus(nullptr, 0, &percent))
        return;
    if (!cls.rendererStarted || !cls.whiteMaterial)
        return;
    // Keep the track opaque so an uninitialised/white surface cannot bleed
    // through the loader while the first frame is being presented.
    static const float s_barBack[4] = { 0.08f, 0.08f, 0.08f, 1.0f };
    static const float s_barFront[4] = { 0.25f, 0.8f, 0.25f, 1.0f };
    static const float s_textColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    // 2D draws use real framebuffer pixels (R_Set2D maps the viewport, not a
    // 640x480 virtual space), so anchor to the actual display size.
    const float barW = 360.0f;
    const float barH = 14.0f;
    const float barX = (cls.vidConfig.displayWidth - barW) * 0.5f;
    const float barY = cls.vidConfig.displayHeight - 96.0f;
    R_AddCmdDrawStretchPic(barX, barY, barW, barH, 0.0f, 0.0f, 1.0f, 1.0f, s_barBack, cls.whiteMaterial);
    if (percent > 100u)
        percent = 100u;
    if (percent)
        R_AddCmdDrawStretchPic(barX, barY, barW * percent / 100.0f, barH, 0.0f, 0.0f, 1.0f, 1.0f,
                               s_barFront, cls.whiteMaterial);
    // Only the percentage: the fastfile path this is derived from is a port
    // detail, not something the game ever puts on a loading screen.
    if (cls.consoleFont && DB_XAssetExists(ASSET_TYPE_FONT, "fonts/consoleFont"))
    {
        char line[128];
        Com_sprintf(line, sizeof(line), "Loading... %u%%", percent);
        R_AddCmdDrawText(line, 0x7FFFFFFF, cls.consoleFont, barX, barY - 24.0f, 0.6f, 0.6f, 0.0f,
                         s_textColor, 0);
    }
    // Evidence for the gate above: a device log shows when the
    // readout really reached the screen (and that it did not while a movie
    // was playing), the same way the cinematic draw diag proves ownerdraw 277
    // is painted.
    static uint32_t s_readoutDraws;
    static uint32_t s_lastReadoutReportMs;
    ++s_readoutDraws;
    extern const dvar_t *com_diagMarkers;
    const uint32_t reportNow = Sys_Milliseconds();
    if (com_diagMarkers && com_diagMarkers->current.enabled && reportNow - s_lastReadoutReportMs >= 1000)
    {
        s_lastReadoutReportMs = reportNow;
        Com_Printf(0, "SCR_LOADING_READOUT draw calls=%u percent=%u cinematic=0\n", s_readoutDraws,
                   percent);
    }
}

void __cdecl SCR_DrawScreenField(int refreshedUI)
{
    connstate_t connectionState; // r31
    Material *v4; // r4
    const float *v5; // r3

    R_BeginSharedCmdList();
    R_AddCmdProjectionSet2D();
    static const float s_uiMatColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    R_AddCmdSetMaterialColor(s_uiMatColor);
    if (!cls.uiStarted
        || (connectionState = clientUIActives[0].connectionState, clientUIActives[0].connectionState == CA_MAP_RESTART))
    {
    LABEL_2:
        SCR_ClearScreen();
        SCR_DrawLoadingStatus();
    }
    else
    {
        UI_UpdateTime(cls.realtime);
        if (!UI_IsFullscreen())
        {
            switch (connectionState)
            {
            case CA_DISCONNECTED:
                goto LABEL_2;
            case CA_CINEMATIC:
                SCR_ClearScreen();
                SCR_DrawCinematic(0);
                goto LABEL_12;
            case CA_LOGO:
                SCR_ClearScreen();
                CL_DrawLogo();
                goto LABEL_12;
            case CA_LOADING:
                SCR_ClearScreen();
                // UI_Refresh appends the fullscreen menu commands.  Draw the
                // status last so the menu cannot cover the progress bar.
                UI_Refresh();
                refreshedUI = 1;
                SCR_DrawLoadingStatus();
                break;
            case CA_ACTIVE:
                goto LABEL_12;
            default:
                goto LABEL_11;
            }
        }
        switch (connectionState)
        {
        case CA_DISCONNECTED:
        case CA_LOGO:
            SCR_ClearScreen();
            SCR_DrawLoadingStatus();
            break;
        case CA_LOADING:
            SCR_ClearScreen();
            UI_Refresh();
            refreshedUI = 1;
            // Keep this after UI_Refresh for the same reason as the
            // non-fullscreen branch above: the loader is an overlay.
            SCR_DrawLoadingStatus();
            break;
        case CA_CINEMATIC:
        case CA_ACTIVE:
            break;
        default:
        LABEL_11:
            Com_Error(ERR_FATAL, "SCR_DrawScreenField: bad clcState");
            break;
        }
    LABEL_12:
        if (!refreshedUI && Key_IsCatcherActive(0, KEYCATCH_UI))
        {
        LABEL_14:
            UI_Refresh();
            refreshedUI = 1;
        }
    }
}

float __cdecl CL_GetMenuBlurRadius(int localClientNum)
{
    double BlurRadius; // fp1

    if (localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\client\\cl_scrn.cpp",
            271,
            0,
            "%s\n\t(localClientNum) = %i",
            "(localClientNum == 0)",
            localClientNum);
    if (Key_IsCatcherActive(0, KEYCATCH_UI) && cls.uiStarted && clientUIActives[0].connectionState != CA_CINEMATIC)
        BlurRadius = UI_GetBlurRadius();
    else
        BlurRadius = 0.0;
    return *((float *)&BlurRadius + 1);
}

void __cdecl SCR_UpdateRumble()
{
    // KISAKTODO
    //int v0; // r3
    //
    //if (!cl_paused)
    //    MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\client\\cl_scrn.cpp", 284, 0, "%s", "cl_paused");
    //v0 = CL_ControllerIndexFromClientNum(0);
    //if (clientUIActives[0].connectionState != CA_ACTIVE || cl_paused->current.integer)
    //    GPad_StopRumbles(v0);
    //else
    //    GPad_UpdateRumbles(v0);
}

void SCR_UpdateFrame()
{
    int refreshedUI; // r31
#ifdef __SWITCH__
    const uint32_t perf_begin = Sys_Milliseconds();
#endif

    iassert(Sys_IsMainThread() || Sys_IsRenderThread());
#ifdef __SWITCH__
    {
        SWITCH_PERF_SCOPE(SWITCH_PERF_RENDER_BEGIN);
        R_BeginFrame();
    }
    const uint32_t perf_after_begin = Sys_Milliseconds();
#else
    R_BeginFrame();
#endif
    SND_InitFXSounds();
#ifdef __SWITCH__
    {
        SWITCH_PERF_SCOPE(SWITCH_PERF_RENDER_CGAME);
        refreshedUI = CL_CGameRendering();
    }
#else
    refreshedUI = CL_CGameRendering();
#endif
    if (Sys_IsMainThread() && !refreshedUI)
        CL_UpdateSound();
#ifdef __SWITCH__
    {
        SWITCH_PERF_SCOPE(SWITCH_PERF_RENDER_SCENE);
        SCR_DrawScreenField(refreshedUI);
    }
#else
    SCR_DrawScreenField(refreshedUI);
#endif
    if (clientUIActives[0].connectionState == CA_ACTIVE)
    {
        CG_DrawFullScreenDebugOverlays(0);
    }
#ifdef __SWITCH__
    const uint32_t perf_after_scene = Sys_Milliseconds();
    {
        SWITCH_PERF_SCOPE(SWITCH_PERF_RENDER_UI);
        R_AddCmdDrawProfile();
        Con_DrawConsole(0);
        DevGui_Draw(0);
    }
#else
    R_AddCmdDrawProfile();
    Con_DrawConsole(0);
    DevGui_Draw(0);
#endif
#ifdef __SWITCH__
    const uint32_t perf_after_ui = Sys_Milliseconds();
    {
        SWITCH_PERF_SCOPE(SWITCH_PERF_RENDER_END);
        R_EndFrame();
    }
    const uint32_t perf_after_end = Sys_Milliseconds();
    {
        SWITCH_PERF_SCOPE(SWITCH_PERF_RENDER_ISSUE);
        R_IssueRenderCommands(0xFFFFFFFF);
    }
    const uint32_t perf_after_issue = Sys_Milliseconds();
    const dvar_t *performance = Dvar_FindVar("performance");
    if (performance && performance->type == DVAR_TYPE_BOOL && performance->current.enabled)
    {
        static uint32_t report_begin;
        static uint32_t frames;
        static uint64_t begin_sum;
        static uint64_t scene_sum;
        static uint64_t ui_sum;
        static uint64_t end_sum;
        static uint64_t issue_sum;
        if (!report_begin)
            report_begin = perf_begin;
        ++frames;
        begin_sum += perf_after_begin - perf_begin;
        scene_sum += perf_after_scene - perf_after_begin;
        ui_sum += perf_after_ui - perf_after_scene;
        end_sum += perf_after_end - perf_after_ui;
        issue_sum += perf_after_issue - perf_after_end;
        const uint32_t elapsed = perf_after_issue - report_begin;
        if (elapsed >= 1000)
        {
            const double inv_frames = 1.0 / (double)frames;
            // Split camera-visible surfaces (types 0..14: BSP/smodel/ent lit,
            // decal, emissive -- what's actually in frame) from shadow-pass
            // surfaces (types 15..33: 2 sun cascades + 4 spot lights, each
            // redrawing BSP/smodel/ent again, plus the shadow cookie) so a
            // swing in "issue" (backend submission cost, R_IssueRenderCommands)
            // can be attributed to which of those dominates, instead of
            // guessed at from view direction alone.
            long cameraSurfs = 0;
            long shadowSurfs = 0;
            // Which asset kind is behind the surf count, regardless of pass:
            // BSP (world geo), smodel (placed static props -- Killhouse's
            // shelves/crates/racks), ent (dynamic entities), or FX. Indices
            // follow the fixed lit/decal/emissive x{sun0,sun1,spot0-3} grid
            // in r_drawsurf.h's DrawSurfType.
            static const int kBspIdx[] = { 0, 3, 9, 15, 18, 21, 24, 27, 30 };
            static const int kSmodelIdx[] = { 1, 4, 10, 16, 19, 22, 25, 28, 31 };
            static const int kEntIdx[] = { 2, 5, 11, 17, 20, 23, 26, 29, 32 };
            long bspSurfs = 0;
            long smodelSurfs = 0;
            long entSurfs = 0;
            for (int i = 0; i < DRAW_SURF_TYPE_COUNT; ++i)
            {
                if (i < DRAW_SURF_SUNSHADOW_0_BEGIN)
                    cameraSurfs += scene.drawSurfCount[i];
                else
                    shadowSurfs += scene.drawSurfCount[i];
            }
            for (int idx : kBspIdx)
                bspSurfs += scene.drawSurfCount[idx];
            for (int idx : kSmodelIdx)
                smodelSurfs += scene.drawSurfCount[idx];
            for (int idx : kEntIdx)
                entSurfs += scene.drawSurfCount[idx];
            // Actual DrawIndexedPrimitive calls the backend just issued, per
            // retail's own prim-stats funnels. Valid here because the backend
            // runs inline (sys_smp_allowed=0) and RB_ResetStatTracking only
            // clears it at the next RB_BeginFrame. A draw-surf entry is not a
            // draw call: the cached static-model funnel issues one draw per
            // entry, the rigid funnel one per instance in the entry.
            int drawsCam[GFX_PRIM_STATS_COUNT];
            int drawsShadow[GFX_PRIM_STATS_COUNT];
            int drawsCamTotal = 0;
            int drawsShadowTotal = 0;
            long trisTotal = 0;
            for (int i = 0; i < GFX_PRIM_STATS_COUNT; ++i)
            {
                drawsCam[i] = g_frameStatsCur.viewStats[0].primStats[i].primCount;
                drawsShadow[i] = g_frameStatsCur.viewStats[1].primStats[i].primCount;
                drawsCamTotal += drawsCam[i];
                drawsShadowTotal += drawsShadow[i];
                trisTotal += g_frameStatsCur.viewStats[0].primStats[i].triCount
                    + g_frameStatsCur.viewStats[1].primStats[i].triCount;
            }
            // Per-frame average of the funnel decisions over this window.
            static uint32_t prevFunnel[SMODEL_FUNNEL_COUNT];
            double funnelPerFrame[SMODEL_FUNNEL_COUNT];
            for (int i = 0; i < SMODEL_FUNNEL_COUNT; ++i)
            {
                funnelPerFrame[i] = (double)(g_smodelFunnelStats[i] - prevFunnel[i]) * inv_frames;
                prevFunnel[i] = g_smodelFunnelStats[i];
            }
            Com_Printf(16,
                "PERF_SMODEL cached=%.0f rigid_disabled=%.0f rigid_ineligible=%.0f "
                "rigid_allocfail=%.0f skinned=%.0f\n",
                funnelPerFrame[SMODEL_FUNNEL_CACHED],
                funnelPerFrame[SMODEL_FUNNEL_RIGID_DISABLED],
                funnelPerFrame[SMODEL_FUNNEL_RIGID_INELIGIBLE],
                funnelPerFrame[SMODEL_FUNNEL_RIGID_ALLOC_FAIL],
                funnelPerFrame[SMODEL_FUNNEL_SKINNED]);
            Com_Printf(16,
                "PERF_RENDER begin=%.1f scene=%.1f ui=%.1f end=%.1f issue=%.1f "
                "surfs_camera=%ld surfs_shadow=%ld surfs_bsp=%ld surfs_smodel=%ld surfs_ent=%ld "
                "draws_cam=%d(world=%d smc=%d smr=%d xm=%d bm=%d fx=%d hud=%d) draws_shadow=%d(smc=%d smr=%d) tris=%ld\n",
                (double)begin_sum * inv_frames,
                (double)scene_sum * inv_frames,
                (double)ui_sum * inv_frames,
                (double)end_sum * inv_frames,
                (double)issue_sum * inv_frames,
                cameraSurfs, shadowSurfs, bspSurfs, smodelSurfs, entSurfs,
                drawsCamTotal, drawsCam[GFX_PRIM_STATS_WORLD],
                drawsCam[GFX_PRIM_STATS_SMODELCACHED], drawsCam[GFX_PRIM_STATS_SMODELRIGID],
                drawsCam[GFX_PRIM_STATS_XMODELRIGID] + drawsCam[GFX_PRIM_STATS_XMODELSKINNED],
                drawsCam[GFX_PRIM_STATS_BMODEL], drawsCam[GFX_PRIM_STATS_FX],
                drawsCam[GFX_PRIM_STATS_HUD],
                drawsShadowTotal, drawsShadow[GFX_PRIM_STATS_SMODELCACHED],
                drawsShadow[GFX_PRIM_STATS_SMODELRIGID], trisTotal);
            report_begin = perf_after_issue;
            frames = 0;
            begin_sum = scene_sum = ui_sum = end_sum = issue_sum = 0;
        }
    }
#endif
    //Profile_EndInternal(0);
#ifdef KISAK_XBOX
    if (R_SkinCacheReachedThreshold() && g_allowRemoveCorpse)
    {
        CL_AddReliableCommand(0, "removecorpse");
        g_allowRemoveCorpse = 0;
    }
#endif
    //Profile_EndInternal(0);
}

void __cdecl SCR_UpdateScreen()
{
    if (!updateScreenCalled)
    {
        //Profile_Begin(34);
        if (clientUIActives[0].connectionState == CA_LOADING)
            Sys_LoadingKeepAlive();
        if (scr_initialized)
        {
            if (!com_errorEntered)
            {
                updateScreenCalled = 1;
                SCR_UpdateFrame();
                updateScreenCalled = 0;
            }
        }
#ifdef __SWITCH__
        else
        {
        }
#endif
        //Profile_EndInternal(0);
    }
}

void __cdecl SCR_UpdateLoadScreen()
{
#ifndef KISAK_XBOX
	if (!IsFastFileLoad())
		SCR_UpdateScreen();
#endif
}

void CL_CubemapShotUsage()
{
    Com_Printf(CON_CHANNEL_DONT_FILTER, "Syntax: cubemapShot size basefilename [lighting r g b | fresnel n0 n1]\n");
    Com_Printf(CON_CHANNEL_DONT_FILTER, "* size must be a power of 2 that is at least 4 and not more than 1024.\n");
    Com_Printf(CON_CHANNEL_DONT_FILTER, "* screenshots will be written to 'env/basefilename_*.tga'\n");
    Com_Printf(CON_CHANNEL_DONT_FILTER, "* basefilename must not exceed %i chars\n", 40);
    Com_Printf(CON_CHANNEL_DONT_FILTER, "* If 'lighting' is specified, a diffuse environment-based lighting cubemap is generated.\n");
    Com_Printf(CON_CHANNEL_DONT_FILTER, "  This takes exponentially longer to make larger image sizes.\n");
    Com_Printf(CON_CHANNEL_DONT_FILTER, "  16 is a good iteration size.  32 is a good final image size.\n");
    Com_Printf(CON_CHANNEL_DONT_FILTER, "* If 'fresnel' is specified, the alpha channel of the cubemap contains the reflection factor.\n");
    Com_Printf(CON_CHANNEL_DONT_FILTER, "  n0 and n1 are the index of refraction of the 'air' and 'water' surfaces, respectively.\n");
    Com_Printf(CON_CHANNEL_DONT_FILTER, "  The index of refraction must always be 1 or greater.\n");
    Com_Printf(CON_CHANNEL_DONT_FILTER, "  This is always calculated, and defaults to air-water interface (n0 = 1, n1 = 1.333).\n");
}

void __cdecl CL_CubemapShot_f()
{
    const char *v0; // r3
    const char *v1; // r11
    const char *v3; // r3
    char *v4; // r11
    int v5; // r10
    const char *v6; // r3
    int v7; // r3
    int v8; // r28
    char v9; // r27
    double v10; // fp29
    double v11; // fp31
    const char *v12; // r3
    const char *v13; // r3
    double v14; // fp2
    const char *v15; // r3
    double v16; // fp2
    const char *v17; // r3
    double v18; // fp2
    const char *v19; // r3
    const char *v20; // r3
    double v21; // fp2
    const char *v22; // r3
    double v23; // fp2
    unsigned int displayWidth; // r11
    CubemapShot i; // r31
    DemoType DemoType; // r3
    CubemapShot v27; // r30
    const char **v28; // r31
    const char *v29; // r3
    float v30; // [sp+50h] [-A0h] BYREF
    float v31; // [sp+54h] [-9Ch]
    float v32; // [sp+58h] [-98h]
    char v33[72]; // [sp+60h] [-90h] BYREF

    if (!CL_IsCgameInitialized(0))
    {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "must be in a map to use this command\n");
        return;
    }
    if (Cmd_Argc() < 3)
        goto LABEL_18;
    v0 = Cmd_Argv(2);
    v1 = v0;
    while (*(unsigned __int8 *)v0++)
        ;
    if ((unsigned int)(v0 - v1 - 1) > 0x28)
        goto LABEL_18;
    v3 = Cmd_Argv(2);
    v4 = v33;
    do
    {
        v5 = *(unsigned __int8 *)v3++;
        *v4++ = v5;
    } while (v5);
    v6 = Cmd_Argv(1);
    v7 = atol(v6);
    v8 = v7;
    if ((unsigned int)(v7 - 4) > 0x3FC || ((v7 - 1) & v7) != 0)
        goto LABEL_18;
    v9 = 0;
    v30 = 0.0;
    v31 = 0.0;
    v32 = 0.0;
    v10 = 1.0;
    v11 = 1.3329999;
    if (Cmd_Argc() != 7)
    {
        if (Cmd_Argc() == 6)
        {
            v19 = Cmd_Argv(3);
            if (!I_stricmp(v19, "fresnel"))
            {
                v20 = Cmd_Argv(4);
                v21 = atof(v20);
                v10 = (float)*(double *)&v21;
                v22 = Cmd_Argv(5);
                v23 = atof(v22);
                v11 = (float)*(double *)&v23;
                if (v10 >= 1.0 && v11 >= 1.0)
                    goto LABEL_20;
            }
        }
        else if (Cmd_Argc() == 3)
        {
            goto LABEL_20;
        }
    LABEL_18:
        CL_CubemapShotUsage();
        return;
    }
    v12 = Cmd_Argv(3);
    if (I_stricmp(v12, "lighting"))
        goto LABEL_18;
    v13 = Cmd_Argv(4);
    v14 = atof(v13);
    v30 = *(double *)&v14;
    v15 = Cmd_Argv(5);
    v16 = atof(v15);
    v31 = *(double *)&v16;
    v17 = Cmd_Argv(6);
    v18 = atof(v17);
    v32 = *(double *)&v18;
    v9 = 1;
LABEL_20:
    displayWidth = cls.vidConfig.displayWidth;
    if ((int)cls.vidConfig.displayHeight < (int)cls.vidConfig.displayWidth)
        displayWidth = cls.vidConfig.displayHeight;
    if (v8 <= (int)(displayWidth - 2))
    {
        R_SyncRenderThread();
        for (i = CUBEMAPSHOT_RIGHT; i < CUBEMAPSHOT_COUNT; ++i)
        {
            R_BeginCubemapShot(v8, 1);
            R_BeginFrame();
            R_BeginSharedCmdList();
            R_ClearClientCmdList2D();
            DemoType = CL_GetDemoType();
            CG_DrawActiveFrame(0, clients[0].serverTime, DemoType, i, v8, 0);
            R_EndFrame();
            R_IssueRenderCommands(0xFFFFFFFF);
            R_EndCubemapShot(i);
        }
        if (v9)
            R_LightingFromCubemapShots(&v30);

        v27 = CUBEMAPSHOT_RIGHT;
        for (size_t shotNameIndex = 0; shotNameIndex < ARRAY_COUNT(szShotName); ++shotNameIndex, ++v27)
        {
            v29 = va("env/%s%s.tga", v33, szShotName[shotNameIndex]);
            R_SaveCubemapShot((char*)v29, v27, v10, v11);
        }
    }
    else
    {
        Com_Printf(
            CON_CHANNEL_DONT_FILTER,
            "The cubemapshot size may not exceed %i for this resolution.  Try reducing the cubemapshot size or increasing your "
            "screen resolution.\n",
            displayWidth - 2);
    }
}
