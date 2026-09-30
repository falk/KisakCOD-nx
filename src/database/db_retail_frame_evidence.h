#pragma once

#include <universal/q_shared.h>
#include <platform/switch/switch_diag_dvars.h>

// Ungated, process-lifetime default-substitution counter.
void RetailKillhouseNoteDefaultBind();
uint32_t RetailKillhouseDefaultBindCount();

// Walk-window frame evidence. The P5 movement
// proof arms this ledger immediately before its walk and emits the
// summary just before the terminal marker. Every note is a cheap early-out
// while unarmed, so ordinary frames pay one branch. The renderer reports
// through production funnels: executed draw sublists by surface/technique
// class (rb_backend.cpp R_RenderDrawSurfListMaterial), DPVS/skin worker
// commands (r_workercmds.cpp R_ProcessWorkerCmdInternal), rendered backend
// frames (rb_backend.cpp RB_Draw3D), and forced GPU idles (r_rendercmds.cpp).
// A verifier requires the recorded classes, not just a plausible frame.
void RetailKillhouseFrameEvidenceArm();
bool RetailKillhouseFrameEvidenceActive();
// shadowIsPointOrigin distinguishes the two shadow-map build lists: the sun
// partition sets viewOrigin[3] to 0 and the spot/omni partitions set the
// light origin with w = 1 (r_sunshadow.cpp R_MergeAndEmitSunShadowMapsSurfs,
// r_spotshadow.cpp R_EmitSpotShadowMapSurfs); info->light is not populated on
// either path.
void RetailKillhouseFrameEvidenceNoteDraw(uint32_t baseTechType, uint32_t surfType,
                                          uint32_t executedDrawSurfs, uint32_t techniquePasses,
                                          uint32_t setupSkipped, uint32_t shadowIsPointOrigin);
void RetailKillhouseFrameEvidenceNoteWorkerCmd(uint32_t workerCmdType);
void RetailKillhouseFrameEvidenceNoteFrame();
void RetailKillhouseFrameEvidenceNoteForcedGpuIdle();
// Audit reaches into the CMake-excluded runtime subsystems during
// the walk, attributed per excluded entry point instead of per
// subsystem, so the boundary claim is re-earned at each site: every stub
// entry point names itself here and the walk verifier classifies each site's ability to change rendered evidence. The site enum is
// the shared vocabulary between the stub bodies (switch_thread.cpp,
// switch_excluded_boundaries.cpp), the production ledger, and the verifier.
//
// The subsystem kind stays derived from the site (EXCLUDED_KIND_*), never
// passed separately, so a site cannot be recorded under the wrong boundary.
typedef enum RetailKillhouseExcludedSite_e
{
    // The sound, physics and screenshot sites are gone -- those paths are
    // real on Switch now (OpenAL over libnx audren, the vendored ODE, the
    // PNG screenshot command over the backbuffer readback), so there is no
    // excluded reach left to count for them.
    RKE_SITE_CINE_SET_NEXT_PLAYBACK = 0,
    RKE_SITE_THREAD_FAKE_SPAWN,
    RKE_SITE_COUNT
} RetailKillhouseExcludedSite;

typedef enum RetailKillhouseExcludedKind_e
{
    RKE_KIND_CINE = 0,
    RKE_KIND_THREAD = 1,
    RKE_KIND_COUNT = 2
} RetailKillhouseExcludedKind;

// Counted only while the frame-evidence window is armed. The walk verifier
// hard-rejects every remaining kind (cine/thread): each is a fabricating
// path that could stand in for the rendered frame.
void RetailKillhouseFrameEvidenceNoteExcluded(uint32_t site);
void RetailKillhouseFrameEvidenceEmit();
