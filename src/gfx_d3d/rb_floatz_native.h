#pragma once

// Native float-Z for the deko3d renderer (r_deko9NativeFloatZ).
//
// D3D9 cannot sample a depth buffer, so the engine renders the opaque scene
// a second time ("build floatz" technique, RB_StandardDrawCommands' setup
// pass) into R_RENDERTARGET_FLOAT_Z, an R32F target holding each pixel's
// signed view depth: clip w (the build-floatz vertex shaders output
// dot(clip, depthFromClip) with depthFromClip = (0, 0, 0, 1), negated under
// depth hack), and 2,000,000 (the shadowClear quad) where nothing was drawn.
// Its consumers sample it through TEXTURE_SRC_CODE_FLOATZ (floatZSampler,
// s4): the emissive pass's soft particles (zfeather*: abs(floatz)), light
// beams (floatz / w reconstructs the position) and depth of field
// (rb_postfx: the sign picks the viewmodel equation).
//
// With deko3d the scene depth buffer can be sampled: mode 1 skips the
// geometry pass (the scene pass then clears and fills depth itself) and
// rebuilds the float-Z target from the finished depth buffer in one
// full-screen pass (Deko9_BuildFloatZ) just before the emissive pass, the
// first consumer. (The former mode 2, an in-game compare of both, had no
// executable consumer; the selftest's FLOATZ_* checks prove the rebuild
// against a CPU reference.)
//
// Views that keep the geometry pass in every mode: shadow cookies
// (sc_enable; their receivers need the setup pass's depth anyway),
// multisampled scenes, and a projection that is not perspective with
// clip.w = view depth.

struct GfxViewInfo;
struct GfxBackEndData;
struct GfxCmdBuf;

// 0 = the engine's geometry pass, 1 = native rebuild.
int RB_NativeFloatZMode(const GfxViewInfo *viewInfo);
// Bracket the geometry pass (draw counts for the periodic report).
void RB_LegacyFloatZBegin();
void RB_LegacyFloatZEnd();
// Right before the emissive pass. Mode 1 rebuilds the target (fatal error
// if it cannot).
void RB_NativeFloatZBeforeEmissive(const GfxViewInfo *viewInfo, int mode);
