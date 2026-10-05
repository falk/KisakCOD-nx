#pragma once

// Temporal anti-aliased upscaling for the deko3d renderer (r_taau; device
// pass in src/deko9/deko9_taau.h). Needs the scene layout (r_renderScale
// below 1 or r_dynres): the resolve replaces the scene upscale before the
// 2D pass, at any fixed or adaptive scale.
//
// Front end: the main scene's projection gets a sub-pixel Halton jitter
// sized to the frame's scene viewport, so every scene pass (and the
// depth-hack viewmodel, which shares the projection) renders jittered; the
// 2D/HUD projections are separate and untouched. r_taau 0 leaves the matrix
// as it was.
// Back end: the resolve reads the frame's jitter back from its projection,
// builds the reprojection from the unjittered view-projection of this and
// the previous resolved frame, and resets the history on the first frame,
// a map change, a skipped frame (menus, cinematics), an output size change,
// a camera cut and a zoom jump. A render size change (r_renderScale, r_dynres) keeps it:
// the history is at the output size and the resolve maps each frame's
// render pixels onto it.
// Moving models get their own motion (r_taau_motion.cpp): the front end
// tags each DObj surface with where it was last frame, the back end draws
// the moving ones into the motion texture before the resolve. Moving brush
// models and FX-spawned models without a DObj reproject by depth; their
// history is clipped like any other disocclusion.

#include <cstdint>

struct DObj_s;
struct GfxSceneEntity;
struct GfxViewParms;
struct GfxViewport;
struct GfxViewInfo;
struct IDirect3DBaseTexture9;
struct IDirect3DSurface9;

void R_TaauRegisterDvars();
// r_taau on a scene-layout frame (fixed or adaptive scale).
bool R_TaauActive();
// R_SkinSceneDObjModels, once the entity's surface records (`surfs`,
// `surfCount` of them) are final: tags them with last frame's state.
void R_TaauNoteDObj(const GfxSceneEntity *sceneEnt, const DObj_s *obj, void *surfs, uint32_t surfCount);
// R_RenderScene, after the scene viewport is known.
void R_TaauJitterView(GfxViewParms *viewParms, const GfxViewport &sceneViewport);
// RB_DynResResolveView: true when r_taau resolved the view (the upscale is
// then skipped).
// RB_DynResBeginFrame, with the frame's render/output scale: the scene's
// texture LOD bias (log2 of the scale plus r_taauLodBias) until the resolve.
void RB_TaauBeginFrame(float scale);
// R_DrawEmissive, around the view's transparent passes: the luma change
// they make, for the resolve's reactive weighting (r_taauReactive).
void RB_TaauBeforeEmissive(const GfxViewInfo *viewInfo);
void RB_TaauAfterEmissive(const GfxViewInfo *viewInfo);
bool RB_TaauResolveView(const GfxViewInfo *viewInfo, IDirect3DBaseTexture9 *scene, IDirect3DSurface9 *depth,
                        const int32_t srcRect[4], IDirect3DSurface9 *dst, const int32_t dstRect[4]);
