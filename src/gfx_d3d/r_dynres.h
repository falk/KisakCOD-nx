#pragma once

// Dynamic render resolution for the deko3d renderer (r_dynres; controller in
// r_dynres_controller.h).
//
// Layout (r_dynres 1 at startup): the display, back buffer and HUD stay at
// the 1280x720 output; the 3D scene gets its own colour + depth target
// (R_RENDERTARGET_SCENE, no longer the back buffer) and every scene-sized
// target (float-Z, resolved scene, post-effect and ping-pong at 1/4) is
// allocated for the output size. Each frame the front end picks the scene
// size (controller, r_dynresForceScale, or fixed), the back end re-lays out
// those targets at that size before drawing (Deko9_ResizeRenderTarget: same
// objects, fresh memory per resize, old memory retired after the frames in
// flight; no pipeline rebuild), the engine renders the scene and
// its post effects exactly as it does at any r_renderResolution, and
// RB_DynResResolveView upscales the scene into the back buffer (r_fsrMode;
// a copy at full size) before the view's 2D, which then draws at 1280x720.
//
// Because each target is re-laid out at the frame's size (not rendered
// into a corner of a larger layout), every sampler of the scene -- the
// distortion resolve, postfx_color, the glow chain, depth of field, the
// soft-particle float-Z reads, sunpost, the upscaler -- sees UV 0..1 = the
// active size and clamp-to-edge at its right/bottom border: no UV scale to
// get wrong and no stale texels to bleed in.
//
// Off (the default): nothing here runs; r_renderResolution and the present
// upscaler work as before.

#include <cstdint>

struct GfxViewInfo;
struct GfxBackEndData;
struct GfxRenderTarget;

// The layout is on (r_dynres, read once at the first R_SetWndParms).
bool R_DynResEnabled();
// Render-target setup (r_rendertarget.cpp), layout on only: creates the
// scene colour texture and depth surface at the output size into `scene`
// and makes the depth the shared single-sample depth of the other
// scene-sized targets.
void R_DynResInitSceneTarget(GfxRenderTarget *scene, uint32_t d3dFormat);
// After all targets exist: records the scene-sized ones and their scale.
void R_DynResRegisterTargets();
// R_ShutdownRenderTargets: drops the references kept here.
void R_DynResShutdownTargets();
// Front end, R_BeginFrame: picks the frame's scene size (once per frame)
// from the latest GPU frame time, r_dynresForceScale or the fixed size.
void R_DynResBeginFrame();
// Front end: the scene size of the frame being built (vidConfig.scene*
// when the layout is off). R_SetSceneParms maps refdef to it.
uint32_t R_DynResSceneWidth();
uint32_t R_DynResSceneHeight();
// Back end, RB_BeginFrame: re-lays out the scene-sized targets at the
// frame's size and tags the frame for the GPU time it reports.
void RB_DynResBeginFrame(const GfxBackEndData *data);
// Where post effects and sunpost draw: the scene target with the layout on,
// else the back buffer (the same surface as the scene there).
int RB_DynResPostTarget();
// After a view's post effects: scene -> back buffer (the view's rectangle),
// leaving the back buffer bound for the view's 2D. No-op when off.
void RB_DynResResolveView(const GfxViewInfo *viewInfo, bool firstView);
// The retail post code targets FRAME_BUFFER, which is the scene's surface
// there; with the layout on the scene is its own target.
#define R_RENDERTARGET_POST ((GfxRenderTargetId)RB_DynResPostTarget())
