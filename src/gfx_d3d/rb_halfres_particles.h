#pragma once

// Off-screen soft particles for the deko3d renderer (r_halfResParticles).
//
// In a view's emissive list, every material run whose emissive technique is
// a soft particle (r_halfres_particles_rules.h hrp::Classify, decided once
// per material) draws into an off-screen premultiplied target at 1/factor
// of the scene viewport per axis instead of the scene: colour = the
// particles composited over black, alpha = the remaining transmittance
// (start 1). Its depth test and soft-edge fade use an off-screen depth and
// float-Z built once per view from the scene's before the list (the
// nearest of each factor x factor footprint: Deko9_ParticleDepth). The
// pending off-screen content is composited over the scene (dst * T + C,
// Deko9_ParticleComposite, nearest-depth upsample at silhouettes) before
// the first full-res run of a later sort key and at the end of the list
// (r_halfResParticlesOrder 1; 0 = before every full-res run).
//
// r_halfResParticles 0 off, 1 half resolution, 2 full resolution (every
// pass runs at scale 1: the image must match 0 within rounding, the
// plumbing proof).

#include <cstdint>

struct GfxViewInfo;
struct GfxDrawSurfListInfo;
struct Material;

#include "rb_backend.h"

// Set while a redirected run draws: R_SetupPass passes its state bits
// through RB_HrpRemapStateBits0.
extern bool g_hrpRedirecting;

// R_DrawEmissive, around its draw call.
void RB_HrpBeginView(const GfxViewInfo *viewInfo);
void RB_HrpEndView();
// R_DrawSurfs: whether `info` is the active view's emissive list; before each
// material run; after the list's final R_TessEnd.
bool RB_HrpListActive(const GfxDrawSurfListInfo *info);
void RB_HrpBeforeMaterial(GfxCmdBufContext context, GfxCmdBufContext prepassContext,
                          const GfxDrawSurfListInfo *info, uint32_t drawSurfIndex);
void RB_HrpEndList(GfxCmdBufContext context);
// The redirected pass's blend (hrp::RemapStateBits0); an unsupported blend
// logs FAIL:HRP_BLEND once per material and is drawn unchanged.
uint32_t RB_HrpRemapStateBits0(const Material *material, uint32_t bits0);
// Renderer shutdown (R_ShutdownRenderTargets): releases the targets.
void RB_HrpShutdown();
