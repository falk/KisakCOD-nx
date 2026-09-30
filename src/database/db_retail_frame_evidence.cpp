#include "db_retail_frame_evidence.h"
#include "db_retail_decode_world.h"

#include "database.h"
#include "../qcommon/com_bsp.h"
#include "../qcommon/qcommon.h"
#include "../gfx_d3d/r_bsp.h"
#include "../gfx_d3d/r_dpvs.h"
#include "../gfx_d3d/r_dvars.h"
#include "../gfx_d3d/r_init.h"
#include "../gfx_d3d/r_material.h"
#include "../gfx_d3d/r_scene.h"
#include "../gfx_d3d/r_state.h"
#include "../gfx_d3d/r_workercmds.h"
#include "../gfx_d3d/rb_backend.h"
#include "../universal/assertive.h"
#include "../universal/com_math.h"
#include "../xanim/xanim.h"
#include "../xanim/dobj.h"
#include "../xanim/dobj_utils.h"
#include "../universal/q_shared.h"
#include "../game/g_local.h"
#include "../game/g_main.h"
#include "../game/game_public.h"
#include "../script/scr_const.h"
#include "../server/sv_game.h"

#include <cstdio>
#include <cmath>
#include <cstring>

namespace
{
// Process-lifetime count of default-image substitutions. Never gated or
// sealed, so a production movement checkpoint can prove that the interval
// it observed performed no generic texture/sampler substitution.
uint32_t s_defaultBinds = 0;
} // namespace


void RetailKillhouseNoteDefaultBind()
{
    ++s_defaultBinds;
}

uint32_t RetailKillhouseDefaultBindCount()
{
    return s_defaultBinds;
}




// walk-window frame evidence. Armed by the P5 move proof only, so the
// counts are exactly the frames the movement checkpoint recorded. The classes
// are the original surface types the backend tess table already dispatches on
// (r_material.h surfaceType_t) plus the shadow-build partitions and the
// worker commands the DPVS/skin passes queue; nothing here re-implements or
// approximates a renderer decision. forced_idle counts entries to the
// r_gpuSync=0 substitute path (r_rendercmds.cpp), so a run that only forced
// the GPU idle cannot present itself as real renderer work.
namespace
{
bool s_frameEvidenceArmed = false;
bool s_frameEvidenceEmitted = false;
uint32_t s_frameEvidenceFrames = 0;
uint32_t s_frameEvidenceDrawLists = 0;
uint32_t s_frameEvidenceDrawSurfs = 0;
uint32_t s_frameEvidenceTechniquePasses = 0;
uint32_t s_frameEvidenceSkipped = 0;
uint32_t s_frameEvidenceBsp = 0;
uint32_t s_frameEvidenceBmodel = 0;
uint32_t s_frameEvidenceStatic = 0;
uint32_t s_frameEvidenceStaticSkinned = 0;
uint32_t s_frameEvidenceXmodel = 0;
uint32_t s_frameEvidenceXmodelSkinned = 0;
uint32_t s_frameEvidenceFx = 0;
uint32_t s_frameEvidenceLit = 0;
uint32_t s_frameEvidenceSunShadow = 0;
uint32_t s_frameEvidenceSpotShadow = 0;
uint32_t s_frameEvidenceDpvsStatic = 0;
uint32_t s_frameEvidenceDpvsSceneEnt = 0;
uint32_t s_frameEvidenceDpvsDynModel = 0;
uint32_t s_frameEvidenceDpvsDynBrush = 0;
uint32_t s_frameEvidenceDpvsEntity = 0;
uint32_t s_frameEvidenceAddSceneEnt = 0;
uint32_t s_frameEvidenceSpotShadowEnt = 0;
uint32_t s_frameEvidenceSkinCmds = 0;
uint32_t s_frameEvidenceSkinStaticCmds = 0;
uint32_t s_frameEvidenceForcedGpuIdle = 0;
uint32_t s_frameEvidenceExcluded[RKE_KIND_COUNT] = {0};
bool s_frameEvidenceExcludedReported[RKE_KIND_COUNT] = {false};

// per-excluded-entry-point attribution. Each site records how often it
// was reached and the frame index of its first reach; first_frame == 0 means
// the reach happened before the first rendered frame of the window (the
// load/precache phase), otherwise it happened while frames were being drawn.
struct FrameEvidenceExcludedSiteInfo
{
    const char *name;
    uint32_t kind;
};

const FrameEvidenceExcludedSiteInfo s_frameEvidenceExcludedSites[RKE_SITE_COUNT] = {
    {"cine_set_next_playback", RKE_KIND_CINE},
    {"thread_fake_spawn", RKE_KIND_THREAD},
};

uint32_t s_frameEvidenceExcludedSite[RKE_SITE_COUNT] = {0};
uint32_t s_frameEvidenceExcludedSiteFirstFrame[RKE_SITE_COUNT] = {0};
uint32_t s_frameEvidenceExcludedSiteLastFrame[RKE_SITE_COUNT] = {0};
bool s_frameEvidenceExcludedSiteSeen[RKE_SITE_COUNT] = {false};

const char *FrameEvidenceExcludedName(uint32_t kind)
{
    switch (kind)
    {
    case RKE_KIND_CINE: return "cine";
    case RKE_KIND_THREAD: return "thread";
    default: return "unknown";
    }
}
uint32_t s_frameEvidenceSkippedPrepass = 0;
uint32_t s_frameEvidenceSkippedLit = 0;
uint32_t s_frameEvidenceSkippedShadow = 0;
uint32_t s_frameEvidenceSkippedEmissive = 0;
uint32_t s_frameEvidenceSkippedOther = 0;
uint32_t s_frameEvidenceAssertAtArm = 0;

bool FrameEvidenceShadowPass(uint32_t baseTechType)
{
    return baseTechType == TECHNIQUE_BUILD_SHADOWMAP_DEPTH
           || baseTechType == TECHNIQUE_BUILD_SHADOWMAP_COLOR;
}

bool FrameEvidenceLitPass(uint32_t baseTechType)
{
    return baseTechType >= TECHNIQUE_LIT_BEGIN && baseTechType < TECHNIQUE_LIT_END;
}
} // namespace

void RetailKillhouseFrameEvidenceArm()
{
    s_frameEvidenceArmed = true;
    s_frameEvidenceEmitted = false;
    s_frameEvidenceFrames = 0;
    s_frameEvidenceDrawLists = 0;
    s_frameEvidenceDrawSurfs = 0;
    s_frameEvidenceTechniquePasses = 0;
    s_frameEvidenceSkipped = 0;
    s_frameEvidenceBsp = 0;
    s_frameEvidenceBmodel = 0;
    s_frameEvidenceStatic = 0;
    s_frameEvidenceStaticSkinned = 0;
    s_frameEvidenceXmodel = 0;
    s_frameEvidenceXmodelSkinned = 0;
    s_frameEvidenceFx = 0;
    s_frameEvidenceLit = 0;
    s_frameEvidenceSunShadow = 0;
    s_frameEvidenceSpotShadow = 0;
    s_frameEvidenceDpvsStatic = 0;
    s_frameEvidenceDpvsSceneEnt = 0;
    s_frameEvidenceDpvsDynModel = 0;
    s_frameEvidenceDpvsDynBrush = 0;
    s_frameEvidenceDpvsEntity = 0;
    s_frameEvidenceAddSceneEnt = 0;
    s_frameEvidenceSpotShadowEnt = 0;
    s_frameEvidenceSkinCmds = 0;
    s_frameEvidenceSkinStaticCmds = 0;
    s_frameEvidenceForcedGpuIdle = 0;
    for (uint32_t kind = 0; kind < ARRAY_COUNT(s_frameEvidenceExcluded); ++kind)
    {
        s_frameEvidenceExcluded[kind] = 0;
        s_frameEvidenceExcludedReported[kind] = false;
    }
    for (uint32_t site = 0; site < RKE_SITE_COUNT; ++site)
    {
        s_frameEvidenceExcludedSite[site] = 0;
        s_frameEvidenceExcludedSiteFirstFrame[site] = 0;
        s_frameEvidenceExcludedSiteLastFrame[site] = 0;
        s_frameEvidenceExcludedSiteSeen[site] = false;
    }
    s_frameEvidenceSkippedPrepass = 0;
    s_frameEvidenceSkippedLit = 0;
    s_frameEvidenceSkippedShadow = 0;
    s_frameEvidenceSkippedEmissive = 0;
    s_frameEvidenceSkippedOther = 0;
    s_frameEvidenceAssertAtArm = Assert_GetCount();
}

bool RetailKillhouseFrameEvidenceActive()
{
    return s_frameEvidenceArmed && !s_frameEvidenceEmitted;
}

void RetailKillhouseFrameEvidenceNoteDraw(uint32_t baseTechType, uint32_t surfType,
                                          uint32_t executedDrawSurfs, uint32_t techniquePasses,
                                          uint32_t setupSkipped, uint32_t shadowIsPointOrigin)
{
    if (!RetailKillhouseFrameEvidenceActive())
        return;
    if (setupSkipped)
    {
        // A dropped sublist is counted, never folded into a drawn count.
        // Classifying the skip separates original-behavior prepass filtering
        // (prepass == MTL_PREPASS_NONE) from a dropped drawable list.
        ++s_frameEvidenceSkipped;
        if (FrameEvidenceShadowPass(baseTechType))
            ++s_frameEvidenceSkippedShadow;
        else if (FrameEvidenceLitPass(baseTechType))
            ++s_frameEvidenceSkippedLit;
        else if (baseTechType == TECHNIQUE_EMISSIVE || baseTechType == TECHNIQUE_EMISSIVE_SHADOW)
            ++s_frameEvidenceSkippedEmissive;
        else if (baseTechType <= TECHNIQUE_BUILD_FLOAT_Z)
            ++s_frameEvidenceSkippedPrepass;
        else
            ++s_frameEvidenceSkippedOther;
        return;
    }
    ++s_frameEvidenceDrawLists;
    s_frameEvidenceDrawSurfs += executedDrawSurfs;
    s_frameEvidenceTechniquePasses += techniquePasses;
    switch (surfType)
    {
    case SF_TRIANGLES:
    case SF_TRIANGLES_PRETESS:
        s_frameEvidenceBsp += executedDrawSurfs;
        break;
    case SF_BMODEL:
        s_frameEvidenceBmodel += executedDrawSurfs;
        break;
    case SF_STATICMODEL_RIGID:
    case SF_STATICMODEL_PRETESS:
    case SF_STATICMODEL_CACHED:
        s_frameEvidenceStatic += executedDrawSurfs;
        break;
    case SF_STATICMODEL_SKINNED:
        s_frameEvidenceStaticSkinned += executedDrawSurfs;
        break;
    case SF_XMODEL_RIGID:
        s_frameEvidenceXmodel += executedDrawSurfs;
        break;
    case SF_XMODEL_RIGID_SKINNED:
    case SF_XMODEL_SKINNED:
        s_frameEvidenceXmodelSkinned += executedDrawSurfs;
        break;
    case SF_CODE_MESH:
    case SF_MARK_MESH:
    case SF_PARTICLE_CLOUD:
        s_frameEvidenceFx += executedDrawSurfs;
        break;
    default:
        break;
    }
    if (FrameEvidenceShadowPass(baseTechType))
    {
        if (shadowIsPointOrigin)
            s_frameEvidenceSpotShadow += executedDrawSurfs;
        else
            s_frameEvidenceSunShadow += executedDrawSurfs;
    }
    else if (FrameEvidenceLitPass(baseTechType))
    {
        s_frameEvidenceLit += executedDrawSurfs;
    }
}

void RetailKillhouseFrameEvidenceNoteWorkerCmd(uint32_t workerCmdType)
{
    if (!RetailKillhouseFrameEvidenceActive())
        return;
    switch (workerCmdType)
    {
    case WRKCMD_DPVS_CELL_STATIC:
        ++s_frameEvidenceDpvsStatic;
        break;
    case WRKCMD_DPVS_CELL_SCENE_ENT:
        ++s_frameEvidenceDpvsSceneEnt;
        break;
    case WRKCMD_DPVS_CELL_DYN_MODEL:
        ++s_frameEvidenceDpvsDynModel;
        break;
    case WRKCMD_DPVS_CELL_DYN_BRUSH:
        ++s_frameEvidenceDpvsDynBrush;
        break;
    case WRKCMD_DPVS_ENTITY:
        ++s_frameEvidenceDpvsEntity;
        break;
    case WRKCMD_ADD_SCENE_ENT:
        ++s_frameEvidenceAddSceneEnt;
        break;
    case WRKCMD_SPOT_SHADOW_ENT:
        ++s_frameEvidenceSpotShadowEnt;
        break;
    case WRKCMD_SKIN_XMODEL:
        ++s_frameEvidenceSkinCmds;
        break;
    case WRKCMD_SKIN_CACHED_STATICMODEL:
        ++s_frameEvidenceSkinStaticCmds;
        break;
    default:
        break;
    }
}

void RetailKillhouseFrameEvidenceNoteFrame()
{
    if (!RetailKillhouseFrameEvidenceActive())
        return;
    ++s_frameEvidenceFrames;
}

void RetailKillhouseFrameEvidenceNoteForcedGpuIdle()
{
    if (!RetailKillhouseFrameEvidenceActive())
        return;
    ++s_frameEvidenceForcedGpuIdle;
}

void RetailKillhouseFrameEvidenceNoteExcluded(uint32_t site)
{
    if (!RetailKillhouseFrameEvidenceActive() || site >= RKE_SITE_COUNT)
        return;
    const uint32_t kind = s_frameEvidenceExcludedSites[site].kind;
    ++s_frameEvidenceExcluded[kind];
    ++s_frameEvidenceExcludedSite[site];
    if (!s_frameEvidenceExcludedSiteSeen[site])
    {
        s_frameEvidenceExcludedSiteSeen[site] = true;
        s_frameEvidenceExcludedSiteFirstFrame[site] = s_frameEvidenceFrames;
    }
    s_frameEvidenceExcludedSiteLastFrame[site] = s_frameEvidenceFrames;
    // A reached excluded path must never be silent: the first reach of each
    // kind is printed while the walk window is open, so a verifier can
    // hard-fail on the fabricating kinds (cine/shot/thread) and gate the
    // sound/physics sites by their pixel impact.
    if (!s_frameEvidenceExcludedReported[kind])
    {
        s_frameEvidenceExcludedReported[kind] = true;
        Com_Printf(0, "KILLHOUSE_EXCLUDED_REACH kind=%s site=%s count=%u first_frame=%u\n",
                   FrameEvidenceExcludedName(kind), s_frameEvidenceExcludedSites[site].name,
                   s_frameEvidenceExcluded[kind],
                   s_frameEvidenceExcludedSiteFirstFrame[site]);
    }
}

// one line naming every excluded entry point with its reach
// count and phase, written as <count>@<first_frame>:<last_frame> (frame 0 is
// before the first backend frame of the window, so a site whose last reach is
// frame 0 was only reached during load/precache while a site whose last reach
// is nonzero kept being reached while frames were drawn). Emitting the complete
// site set, zeros included, lets the verifier fail an unknown or missing site
// instead of silently accepting a partial breakdown.
static void FrameEvidenceEmitExcludedSites()
{
    char buffer[2048];
    int written = snprintf(buffer, sizeof(buffer), "KILLHOUSE_EXCLUDED_SITES");
    for (uint32_t site = 0; written > 0 && site < RKE_SITE_COUNT
                            && written < (int)sizeof(buffer); ++site)
    {
        written += snprintf(buffer + written, sizeof(buffer) - (size_t)written,
                            " %s=%u@%u:%u", s_frameEvidenceExcludedSites[site].name,
                            s_frameEvidenceExcludedSite[site],
                            s_frameEvidenceExcludedSiteFirstFrame[site],
                            s_frameEvidenceExcludedSiteLastFrame[site]);
    }
    Com_Printf(0, "%s\n", buffer);
}

void RetailKillhouseFrameEvidenceEmit()
{
    if (!s_frameEvidenceArmed || s_frameEvidenceEmitted)
        return;
    s_frameEvidenceEmitted = true;
    const uint32_t asserts = Assert_GetCount() - s_frameEvidenceAssertAtArm;
    const uint32_t fence = r_gpuSync ? (uint32_t)r_gpuSync->current.integer : 0u;
    const uint32_t drawSModels = r_drawSModels ? (uint32_t)r_drawSModels->current.enabled : 0u;
    const uint32_t smpAllowed = sys_smp_allowed
                                    ? (uint32_t)sys_smp_allowed->current.enabled : 0u;
    const uint32_t smpWorker = r_smp_worker ? (uint32_t)r_smp_worker->current.enabled : 0u;
    const uint32_t shadowMaps = sm_enable ? (uint32_t)sm_enable->current.enabled : 0u;
    const uint32_t shadowCookies = sc_enable ? (uint32_t)sc_enable->current.enabled : 0u;

    // Fail loudly, in the same line, when the walk window rendered nothing at
    // all; the verifier additionally requires each scene-required class.
    const char *verdict =
        (s_frameEvidenceFrames == 0 || s_frameEvidenceDrawSurfs == 0) ? "FAIL" : "OK";
    Com_Printf(0,
               "%s:KILLHOUSE_FRAME_EVIDENCE frames=%u lists=%u draw_surfs=%u bsp=%u "
               "bmodel=%u static=%u static_skinned=%u xmodel=%u xmodel_skinned=%u fx=%u "
               "lit=%u sunshadow=%u spotshadow=%u skipped=%u skipped_prepass=%u "
               "skipped_lit=%u skipped_shadow=%u skipped_emissive=%u skipped_other=%u "
               "technique_passes=%u dpvs_static=%u dpvs_scene_ent=%u dpvs_dyn_model=%u "
               "dpvs_dyn_brush=%u dpvs_entity=%u add_scene_ent=%u spot_shadow_ent=%u "
               "skin_cmds=%u skin_static_cmds=%u forced_idle=%u fence=%u draw_smodels=%u "
               "smp_allowed=%u smp_worker=%u sm_enable=%u sc_enable=%u "
               "excl_snd=%u excl_phys=%u excl_cine=%u excl_shot=%u excl_thread=%u "
               "asserts=%u\n",
               verdict, s_frameEvidenceFrames, s_frameEvidenceDrawLists,
               s_frameEvidenceDrawSurfs, s_frameEvidenceBsp, s_frameEvidenceBmodel,
               s_frameEvidenceStatic, s_frameEvidenceStaticSkinned, s_frameEvidenceXmodel,
               s_frameEvidenceXmodelSkinned, s_frameEvidenceFx, s_frameEvidenceLit,
               s_frameEvidenceSunShadow, s_frameEvidenceSpotShadow, s_frameEvidenceSkipped,
               s_frameEvidenceSkippedPrepass, s_frameEvidenceSkippedLit,
               s_frameEvidenceSkippedShadow, s_frameEvidenceSkippedEmissive,
               s_frameEvidenceSkippedOther, s_frameEvidenceTechniquePasses,
               s_frameEvidenceDpvsStatic, s_frameEvidenceDpvsSceneEnt,
               s_frameEvidenceDpvsDynModel, s_frameEvidenceDpvsDynBrush,
               s_frameEvidenceDpvsEntity, s_frameEvidenceAddSceneEnt,
               s_frameEvidenceSpotShadowEnt, s_frameEvidenceSkinCmds,
               s_frameEvidenceSkinStaticCmds, s_frameEvidenceForcedGpuIdle, fence,
               drawSModels, smpAllowed, smpWorker, shadowMaps,
                // excl_snd/excl_phys/excl_shot are retired boundaries
                // (sound, physics and the screenshot command are real); they
                // stay in the line as constant zeros so the recorded evidence
                // format is unchanged.
                shadowCookies, 0u, 0u, s_frameEvidenceExcluded[RKE_KIND_CINE], 0u,
                s_frameEvidenceExcluded[RKE_KIND_THREAD], asserts);
    FrameEvidenceEmitExcludedSites();
}
