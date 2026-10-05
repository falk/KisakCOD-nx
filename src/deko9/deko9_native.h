#pragma once

// Native submission API of the deko3d renderer (deko9), for the Switch
// engine's gfx_d3d code. These bypass the IDirect3DDevice9 interface on the
// per-draw path: the deko3d renderer is not bound to D3D9 semantics.
// `device` is the engine's dx.device, which on Switch is always a deko9
// device.
//
// Threading: every call takes the device lock. Deko9_BeginBatch/EndBatch
// hold it across a stretch of draws (the engine wraps R_DrawSurfs), so the
// calls inside only pay an inline re-entry. A batch must not wait on another
// thread that may itself call into the device.

#include <cstdint>

#include "deko9_shader.h"

struct IDirect3DDevice9;
struct IDirect3DBaseTexture9;
struct IDirect3DResource9;
struct IDirect3DSurface9;
struct IDirect3DQuery9;
struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;
struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;

void Deko9_BeginBatch(IDirect3DDevice9 *device);
void Deko9_EndBatch(IDirect3DDevice9 *device);

// SetTexture without a COM reference: the slot is cleared if the texture is
// destroyed while bound (a later draw sampling it fails loudly).
void Deko9_SetTexture(IDirect3DDevice9 *device, uint32_t sampler, IDirect3DBaseTexture9 *texture);

// R_HW_SetSamplerState in one call: applies the engine's decoded sampler
// state (only the fields that differ from oldPacked) and returns the state
// the engine tracks afterwards.
uint32_t Deko9_SetSamplerPacked(IDirect3DDevice9 *device, uint32_t sampler, uint32_t packed, uint32_t oldPacked);
// MIPMAPLODBIAS of every engine sampler (the packed path above never sets
// it), rounded to -k/8 for k in 0..15 so sampler keys stay compact: r_taau
// sharpens the scene's textures when it renders below the output size.
// Applies to the samplers already set too.
void Deko9_SetEngineLodBias(IDirect3DDevice9 *device, float bias);

// Fast-path verification (r_deko9Verify): each draw re-derives its binding,
// vertex input and constants the slow way and compares; mismatches report
// FAIL:DEKO9_FASTPATH_MISMATCH, and every 600 frames a
// PASS/FAIL:DEKO9_FASTPATH_VERIFY verdict line is logged.
void Deko9_SetVerify(IDirect3DDevice9 *device, bool enable);

// Present upscaler (deko9_fsr.h), used only when the back buffer (the
// engine's render resolution, r_renderResolution) is smaller than the
// display. Sharpness: RCAS stops below maximum for bilinear_rcas (engine dvar
// r_fsrSharpness; 0 = sharpest, +1 halves it). Mode: r_fsrMode's index
// (0 sgsr, 1 bilinear_rcas, 2 bilinear); takes effect at the next present.
void Deko9_SetUpscaleSharpness(IDirect3DDevice9 *device, float stops);
void Deko9_SetUpscaleMode(IDirect3DDevice9 *device, uint32_t mode);
// Selftest probe: renders textureGather(source, corner of texels
// (x..x+1, y..y+1), component) into each pixel (x, y) of the render target
// `target`; the caller reads it back with GetRenderTargetData. False (and a
// DEKO9 log line) when the probe cannot run.
bool Deko9_GatherProbe(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *source, IDirect3DSurface9 *target,
                       int component);

// Shader pipeline pack: writes
// any DKSH compiled since the pack was loaded (or last flush) back to
// sdmc:/switch/kisakcod/deko9-cache/. No device param -- the pack is
// process-wide, not per-Device -- and no device lock is taken. Called once
// per successful zone load (db_registry.cpp, next to
// KILLHOUSE_LOAD_SHADERS), never per shader; a no-op when nothing new
// compiled.
void Deko9_FlushShaderPack();

// Load-time shader variant prebake. A draw selects a compiled variant of its
// shaders by depth-compare mask, early-Z and instance layout; one missing
// is translated and compiled at draw time with the device lock held. The
// caller describes every loaded material pass; the device enumerates every
// variant those passes can select (deko9_variant_plan.h), builds the ones
// not installed yet without holding the device lock, and installs them.
// Building runs on one thread created on core `cpuId` at Horizon priority
// `priority` (pass a core and a priority below the frame workers' so the
// build only fills their idle time), or on the calling thread when
// cpuId < 0 or the thread cannot start. Blocks the caller until every
// variant is installed: call it from the loading thread, never from the
// main thread. A variant that a draw still has to build afterwards is
// reported (FAIL:DEKO9_SHADER_PREBAKE, drawBuilds= on the perf line) and
// built as before.
struct Deko9PassVariants
{
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    // Pixel sampler registers bound to code images that can be a
    // depth-format texture (the hardware shadow maps).
    uint32_t depthSamplerMask;
    // Some state-bits entry of the material enables the alpha test here.
    uint8_t alphaTest;
    // Registers of the pass's instanced static-model draws (0: none).
    uint8_t instanceRegCount;
    uint8_t instanceRegs[16];
};
struct Deko9PrebakeResult
{
    uint32_t passes;    // descriptions accepted
    uint32_t planned;   // distinct variants the passes can select
    uint32_t present;   // already installed
    uint32_t built;     // installed now
    uint32_t packHits;  // of those, served by the shader pack
    uint32_t failed;    // could not be built (reported)
    uint32_t threads;   // compile threads used (0: the calling thread)
    uint64_t planUs, buildUs, wallUs;
};
bool Deko9_PrebakeVariants(IDirect3DDevice9 *device, const Deko9PassVariants *passes, uint32_t count, int cpuId,
                           int priority, Deko9PrebakeResult *result);

// The engine's zone-load prebake switch (r_deko9Prebake, registered with
// this default). Off, a zone load plans and builds nothing and every
// variant is built at its first draw, with the device lock held.
constexpr bool DEKO9_DEFAULT_PREBAKE = true;
// Runs `prebake` (the zone's whole plan and build) only when `enabled`;
// returns whether it ran.
template <typename Prebake> inline bool Deko9_PrebakeIfEnabled(bool enabled, Prebake &&prebake)
{
    if (!enabled)
        return false;
    prebake();
    return true;
}

// ---- native float-Z (r_deko9NativeFloatZ) -------------------------------------
// Rebuilds the engine's float-Z target (signed view depth, see deko9_fsr.h
// kFloatZGlsl / FloatZSetup / FloatZReference) from the scene depth-stencil
// surface in one full-screen pass, replacing the engine's "build floatz"
// geometry pass. `target` is a float render target no larger than `depth`;
// neither stays bound (the next draw rebinds the engine's targets). False
// (and a FAIL:DEKO9_FLOATZ line) when the pass cannot run.
namespace deko9
{
struct FloatZConstants;
}
bool Deko9_BuildFloatZ(IDirect3DDevice9 *device, IDirect3DSurface9 *depth, IDirect3DSurface9 *target,
                       const deko9::FloatZConstants *constants);
// ---- off-screen particles (r_halfResParticles) -------------------------------
// Programs kHrpDepthGlsl / kHrpCompositeGlsl, CPU
// reference deko9_particles_reference.h.
//
// Deko9_ParticleDepth: the off-screen depth-stencil `dstDepth` (cleared by
// the caller beforehand; the pass writes every covered texel) and float-Z
// target `dstFloatZ` (both at least ceil(rect.w / factor) x ceil(rect.h /
// factor)) get, at texel (x, y) from their origin, the nearest (smallest
// window depth) of the scene texels rect.xy + (x, y) * factor + (0..factor-1)
// of `depth` (the scene depth-stencil surface) and that texel's float-Z from
// `floatZ` (the scene float-Z texture). factor 1 or 2; rect {x, y, w, h}.
//
// Deko9_ParticleComposite: blends the off-screen (C, T) of `color` into the
// colour target `dst` over dstRect {x, y, w, h}: dst * T + C (ONE /
// SRCALPHA, the target's alpha kept), sampling as hrpref::CompositeSample
// with `halfZ` (off-screen float-Z) and `fullZ` (scene float-Z).
//
// Neither leaves anything bound (the next draw re-applies the engine's
// state). False (and a FAIL:DEKO9_HRP_DEPTH / FAIL:DEKO9_HRP_COMPOSITE line)
// when the pass cannot run.
namespace deko9
{
namespace hrpref
{
struct CompositeConstants;
}
} // namespace deko9
bool Deko9_ParticleDepth(IDirect3DDevice9 *device, IDirect3DSurface9 *depth, IDirect3DBaseTexture9 *floatZ,
                         const int32_t rect[4], uint32_t factor, IDirect3DSurface9 *dstDepth,
                         IDirect3DSurface9 *dstFloatZ);
bool Deko9_ParticleComposite(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *color, IDirect3DBaseTexture9 *halfZ,
                             IDirect3DBaseTexture9 *fullZ, IDirect3DSurface9 *dst, const int32_t dstRect[4],
                             const deko9::hrpref::CompositeConstants *constants);
// Passes recorded since the device started.
void Deko9_GetParticleCounts(IDirect3DDevice9 *device, uint64_t *depthPasses, uint64_t *composites);
// Hardware rules of the two passes,
// all on by default; clearing one is a diagnostic A/B only:
//   DEKO9_HRP_RULE_UNCOMPRESSED: the off-screen colour, float-Z and depth
//     must be uncompressed images (DkImageFlags_HwCompression off); both
//     passes refuse (FAIL) compressed ones.
//   DEKO9_HRP_RULE_ZCULL: the depth pass writes gl_FragDepth, which zcull
//     cannot track; the pass invalidates zcull after its draw so the
//     off-screen particles' depth tests never cull against stale bounds.
enum : uint32_t
{
    DEKO9_HRP_RULE_UNCOMPRESSED = 1u,
    DEKO9_HRP_RULE_ZCULL = 2u,
    DEKO9_HRP_RULES_ALL = 3u,
};
void Deko9_SetParticleRules(IDirect3DDevice9 *device, uint32_t rules);
// Zcull invalidations the depth pass recorded (DEKO9_HRP_RULE_ZCULL).
uint64_t Deko9_GetParticleZcullInvalidates(IDirect3DDevice9 *device);
// Bisection aid: records the strongest deko3d barrier (wait for idle, then
// invalidate texture, shader, descriptor and zcull caches and flush + invalidate
// L2) regardless of the hazard tracker, so a missing or too weak sync edge
// around the off-screen passes shows up as a changed image on hardware.
void Deko9_ParticleFullBarrier(IDirect3DDevice9 *device);
// Bisection aid: the depth pass's and the composite's constant memory as the
// GPU holds it (read after the GPU is idle): depth[8] = rect x, y, last x,
// last y, factor - 1, 0, 0, 0; composite[12] = CompositeConstants words.
// False before the passes ran.
bool Deko9_ReadParticleConstants(IDirect3DDevice9 *device, uint32_t depth[8], uint32_t composite[12]);
uint64_t Deko9_GetParticleFullBarriers(IDirect3DDevice9 *device);

// Early fragment tests (r_deko9EarlyZ, default on): a pixel shader that can
// discard (texkill, or the alpha test on) is depth-tested late on Maxwell,
// so pixels behind geometry run the whole shader. For draws that test depth
// but write neither depth nor stencil, and outside occlusion queries, the
// device binds the shader's variant with layout(early_fragment_tests): the
// discard then only affects color, so the result is identical. Off: always
// the ordinary variant. Takes effect at the next draw.
void Deko9_SetEarlyZ(IDirect3DDevice9 *device, bool enable);
// Per-draw CPU fast paths (r_deko9HazardCache, r_deko9ConstFast,
// r_deko9TexIncremental, r_deko9StaticHazard), each re-derived by
// r_deko9Verify. Bits:
//   DEKO9_PERDRAW_HAZARD: a draw whose sampled images and targets are the
//     previous draw's, with no barrier or other hazard-tracked operation
//     since, skips hazard evaluation (it would find nothing to do).
//   DEKO9_PERDRAW_CONSTS: constant pushes driven by a per-stage "any dirty"
//     flag instead of scanning the dirty bitmask.
//   DEKO9_PERDRAW_TEXTURES: a texture-binding change re-resolves only the
//     sampler slots whose texture or sampler state changed.
//   DEKO9_PERDRAW_STATICTEX (task/deko9-static-hazards, S4a): a store never
//     used as a render/depth/blit target (ImageStore::attachment) is added
//     to a draw's hazard set only when newly bound or pendingRaw (a
//     copy-write landed on it while it stayed bound); off, every sampled
//     store is added every draw as before the optimization -- the pixel
//     A/B proof toggles this alone via r_deko9EmissiveTourShots.
// Takes effect at the next draw.
enum : uint32_t
{
    DEKO9_PERDRAW_HAZARD = 1u << 0,
    DEKO9_PERDRAW_CONSTS = 1u << 1,
    DEKO9_PERDRAW_TEXTURES = 1u << 2,
    DEKO9_PERDRAW_STATICTEX = 1u << 3,
};
void Deko9_SetPerDraw(IDirect3DDevice9 *device, uint32_t flags);

// Defaults of the device settings the engine applies from its dvars once
// per frame (Deko9_Set*). The device starts with these values, so whatever
// it builds before the first frame applies the dvars (the shaders created
// while the first zones load, their prebaked variants) already matches;
// each one equals the registered default of the dvar named beside it.
constexpr bool DEKO9_DEFAULT_VERIFY = false;      // r_deko9Verify
constexpr bool DEKO9_DEFAULT_EARLY_Z = true;      // r_deko9EarlyZ
// r_deko9HazardCache, r_deko9ConstFast, r_deko9TexIncremental, r_deko9StaticHazard (one bit each)
constexpr uint32_t DEKO9_DEFAULT_PERDRAW =
    DEKO9_PERDRAW_HAZARD | DEKO9_PERDRAW_CONSTS | DEKO9_PERDRAW_TEXTURES | DEKO9_PERDRAW_STATICTEX;
constexpr bool DEKO9_DEFAULT_GPU_PASSES = false;  // r_deko9GpuPasses
constexpr uint32_t DEKO9_DEFAULT_DRAW_PROBE = 0;  // r_deko9DrawProbe
constexpr uint32_t DEKO9_DEFAULT_DRAW_SPLIT = 1;  // r_deko9DrawSplit
constexpr uint32_t DEKO9_DEFAULT_BARRIER_MODE = 0; // r_deko9LightBarriers
constexpr uint32_t DEKO9_DEFAULT_TILED_CACHE = 0; // r_deko9TiledCache
constexpr bool DEKO9_DEFAULT_ZCULL_STATS = false; // r_deko9ZcullStats
constexpr uint32_t DEKO9_DEFAULT_SHADOW_FILTER = 0; // r_shadowFilter
constexpr uint32_t DEKO9_DEFAULT_SHADER_OPT = 0;  // r_deko9ShaderOpt
constexpr bool DEKO9_DEFAULT_CENSUS = false;      // r_deko9Census
constexpr uint32_t DEKO9_DEFAULT_FAULT_TRACE = 0; // r_deko9FaultTrace
constexpr uint32_t DEKO9_DEFAULT_GPU_MAP = 0;     // r_deko9GpuMap
constexpr bool DEKO9_DEFAULT_RT_COMPRESSION = true; // r_deko9RtCompression
constexpr float DEKO9_DEFAULT_UPSCALE_SHARPNESS = 0.2f; // r_fsrSharpness
constexpr uint32_t DEKO9_DEFAULT_UPSCALE_MODE = 0; // r_fsrMode
constexpr uint32_t DEKO9_DEFAULT_CMD_CHUNK_KB = 0; // r_deko9CmdChunkKB (0: the device's chunk size)
// r_deko9DrawProbe / r_deko9DrawSplit: GPU per-draw cost probe. Pixels are
// unchanged; only the number of GPU draws or the per-draw feeding changes.
// `split` > 1 issues each triangle-list draw as that many consecutive draws.
enum : uint32_t
{
    DEKO9_PROBE_CONSTS = 1u << 0,    // re-push every shader constant each draw
    DEKO9_PROBE_TEXTURES = 1u << 1,  // re-resolve textures/samplers each draw
    DEKO9_PROBE_STREAMS = 1u << 2,   // re-bind vertex streams and index buffer each draw
    DEKO9_PROBE_SUBCONSTS = 1u << 3, // each split sub-draw also re-pushes constants
};
void Deko9_SetDrawProbe(IDirect3DDevice9 *device, uint32_t flags, uint32_t split);
// Submits the open list and waits until the GPU has finished everything.
void Deko9_WaitForGpuIdle(IDirect3DDevice9 *device);
// Single-submitter rule: the calling thread becomes the render owner, the
// only thread allowed to submit command lists (SubmitOpenList and the
// present block on the GPU with the device lock released, which is correct
// only while no other thread submits). The engine calls it where render
// ownership moves: RB_BeginFrame (whichever thread renders the frame) and
// R_SyncRenderThread (main, the back end idle). A list submitted by another
// thread logs FAIL:DEKO9_SUBMIT_THREAD once; later ones are counted on the
// `DEKO9 perf lock` line (foreignSubmits=).
void Deko9_ClaimSubmitThread(IDirect3DDevice9 *device);
// ---- per-pass GPU timing (r_deko9GpuPasses) --------------------------------
// The engine marks the start of each render phase; the device records a
// deko3d timestamp report (DkCounter_Timestamp: after all earlier 3D work has
// left the ROP) per marker and at every list boundary, reads them back when
// the list's fence completes, and attributes the GPU time between a marker and
// the next one to the marker's pass. Time between lists (GPU idle) is not
// counted. Every 60 presented frames it logs one `DEKO9 gpupass` line with the
// average GPU ms per frame for each pass and their total. Off (the default),
// a marker is one load and a branch.
enum Deko9GpuPass : uint32_t
{
    Deko9GpuPass_Other = 0,    // before the first marker of a frame (uploads, 2D-only frames)
    Deko9GpuPass_ShadowMap,    // sun + spot shadow map renders
    Deko9GpuPass_FloatZ,       // float-Z (or shadow cookie) setup target: clear + depth prepass
    Deko9GpuPass_SceneClear,   // scene target bind + clear
    Deko9GpuPass_DepthPrepass, // depth prepass into the scene target
    Deko9GpuPass_Lit,          // R_DrawLit
    Deko9GpuPass_Decal,        // R_DrawDecal
    Deko9GpuPass_Sun,          // RB_DrawSun
    Deko9GpuPass_PointLights,  // R_DrawLights / R_DrawPointLitSurfs
    Deko9GpuPass_Resolve,      // distortion resolve (post-sun scene copy)
    Deko9GpuPass_Emissive,     // R_DrawEmissive (emissive + effects/particles)
    Deko9GpuPass_PostFx,       // RB_ProcessPostEffects (dof/color, glow, blur) + debug post
    Deko9GpuPass_SunPost,      // RB_DrawSunPostEffects (flare)
    Deko9GpuPass_View2D,       // per-view 2D command list (viewInfo->cmds)
    Deko9GpuPass_Hud2D,        // frame 2D command list (backEndData->cmds: HUD, menus)
    Deko9GpuPass_Present,      // back buffer -> swapchain blit (device internal)
    Deko9GpuPass_Upscale,      // r_renderScale: scene -> back buffer upscale (or copy) before the 2D pass
    Deko9GpuPass_Hrp,          // r_halfResParticles: off-screen depth downsample + composites
    Deko9GpuPass_TaauResolve,  // r_taau: temporal resolve (replaces Upscale)
    Deko9GpuPass_TaauMotion,   // r_taau: per-object motion draws
    Deko9GpuPass_TaauReactive, // r_taau: the two luma passes around the transparents
    Deko9GpuPass_Count
};

// Applied at the next Present (a frame is never half recorded).
void Deko9_SetGpuPasses(IDirect3DDevice9 *device, bool enable);
void Deko9_GpuMarker(IDirect3DDevice9 *device, uint32_t pass);
// Cumulative GPU ns per pass and recorded frames since the device started
// (while r_deko9GpuPasses is on); callers difference two snapshots.
void Deko9_GetGpuPassTotals(IDirect3DDevice9 *device, uint64_t ns[Deko9GpuPass_Count], uint64_t *frames);

// ---- draw census (r_deko9DrawCensus) ----------------------------------------
// Mode 0 off (the only cost is one branch per draw), 1 counts samples passed
// and GPU time per bracket, 2 also fragment shader invocations (hardware
// counter; some emulators do not implement it). The device brackets every
// draw itself, merging consecutive draws of one key (gpupass, material label,
// pixel shader, blend state) into one bracket, for the passes whose bit
// (1 << Deko9GpuPass) is set in passMask (0 = all), and accumulates per key
// as lists retire. The timestamp pair waits for earlier work to leave the
// pipeline, so the per-bracket time is serialized cost, an upper bound that
// ranks well but does not sum exactly to the pass time. Tracks the pass even
// with r_deko9GpuPasses off.
void Deko9_SetDrawCensus(IDirect3DDevice9 *device, uint32_t mode, uint32_t passMask);
// Logs `DEKO9 dcensus label=...` lines (per frame since the last report): a
// frame header, one line per pass (draws, prims, samples, fragment
// invocations, serialized GPU time, estimated cost mpxi = million shaded
// pixels x PS instructions, and the pass's gpupass time), one lights-pass
// line, then rows ranked by estimated cost (global top 40 plus each pass's
// top 12 and the top 12 by GPU time), and resets; a null label only resets.
void Deko9_CensusReport(IDirect3DDevice9 *device, const char *label, uint32_t width, uint32_t height);
// Material label of the next draws that bind `pixelShader` (the engine's
// R_SetupPass; strings must outlive the frame).
void Deko9_CensusLabel(IDirect3DDevice9 *device, const char *material, const char *technique, const char *shader,
                       const char *vertexShader, const void *pixelShader);
// Lights pass: lightIndex = point-light partition + 1 while its surfaces
// draw, 0 after; viewLights != 0 (with lightIndex 0) starts a view with that
// many point lights.
void Deko9_CensusLight(IDirect3DDevice9 *device, uint32_t lightIndex, uint32_t viewLights);
// gpupass name ("lit", "lights", ...) or null.
const char *Deko9_GpuPassName(uint32_t pass);

// ---- render-target compression (r_deko9RtCompression) ------------------------
// Process-wide: applies to render targets and depth-stencil images created
// afterwards (DkImageFlags_HwCompression), including the back buffer when set
// before CreateDevice. Returns whether a created surface/texture got it.
void Deko9_SetRtCompression(bool enable);
// Per calling thread, for the render targets / depth-stencil images it
// creates next: 0 uncompressed, 1 compressed, -1 back to the process-wide
// setting above (r_halfResParticles creates its off-screen targets
// uncompressed this way.
void Deko9_SetRtCompressionOverride(int compress);
bool Deko9_IsCompressed(IDirect3DResource9 *resource);

// ---- scene render scale (r_renderScale, r_dynres) ---------------------------
// Re-lays out a single-level 2D render-target texture or render-target /
// depth-stencil surface at width x height (up to the size it was created
// with): the object keeps its identity, the new layout gets fresh memory of
// the same capacity and the old memory is released after the frames in
// flight complete.
// Every later bind, clear, blit and sampled view addresses exactly the new
// size (UV 0..1 = the new size, clamp-to-edge at its borders; GetDesc
// reports it). Contents are undefined afterwards. False (and a
// FAIL:DEKO9_RESIZE_RENDER_TARGET line) when the size does not fit.
bool Deko9_ResizeRenderTarget(IDirect3DDevice9 *device, IDirect3DResource9 *resource, uint32_t width,
                              uint32_t height);
// Replaces a whole-image copy src -> dst when src is about to be fully
// overwritten: dst takes src's image, src gets dst's old one. Until an opaque
// full-target draw or clear rewrites src, any other use of it fails
// (FAIL:DEKO9_MOVE_STALE). Both must be single-level 2D colour render targets
// of the same format, size and compression; false otherwise.
bool Deko9_MoveContents(IDirect3DDevice9 *device, IDirect3DResource9 *src, IDirect3DResource9 *dst);
// Marks `surface` as the colour target D3D9 requires for a depth-only pass:
// binding it as RT0 binds no colour attachment and colour
// clears skip it. False unless it is a colour render target.
bool Deko9_SetColorless(IDirect3DSurface9 *surface);
// The r_fsrMode upscaler (Deko9_SetUpscaleMode/Sharpness) from `srcRect` of
// the render-target texture `src` into `dstRect` of the color render target
// `dst`; rectangles are {x, y, width, height}, null = whole. Equal
// rectangle sizes: a plain copy. Times its GPU work (a `DEKO9 fsr ...
// path=scene` line every 60 calls). Neither stays bound. False (and
// FAIL:DEKO9_UPSCALE_RECT) when it cannot run.
bool Deko9_UpscaleSurface(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *src, const int32_t *srcRect,
                          IDirect3DSurface9 *dst, const int32_t *dstRect);
// r_taau (deko9_taau.h): temporal resolve of `color`'s srcRect, with the
// scene `depth` of the same size, into dstRect of `dst`. The device keeps the
// history (two images the size of `dst`, recreated and reset when it
// changes); frame->reset ignores it. Logs `DEKO9 taau frames=60 gpu=...`
// every 60 calls. False (and FAIL:DEKO9_TAAU) when it cannot run.
namespace deko9
{
struct TaauFrame;
struct TaauMotionDraw;
struct TaauMotionView;
}
// r_taau per-object motion: draws `count` moving surfaces (deko9_taau.h)
// into the device's motion texture against the scene `depth` (srcRect is
// the scene viewport); the next Deko9_TaauResolve reads it. A draw whose
// index or vertex range (vertexCount vertices from each offset) leaves its
// buffers is skipped and counted (motion_skips= in the "taau" line). False
// (and FAIL:DEKO9_TAAU_MOTION) when the pass cannot run.
bool Deko9_TaauMotion(IDirect3DDevice9 *device, IDirect3DSurface9 *depth, const int32_t srcRect[4],
                      const deko9::TaauMotionView *view, const deko9::TaauMotionDraw *draws, uint32_t count);
// r_taau reactive weighting: called before a view's transparent passes
// (after = false: snapshots the luma of `color`'s srcRect) and after them
// (after = true: keeps the change they made) for the next
// Deko9_TaauResolve; `half` keeps the change at half the scene size (both
// calls of a frame agree). False (and FAIL:DEKO9_TAAU_OPAQUE) when it
// cannot run.
bool Deko9_TaauOpaque(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *color, const int32_t srcRect[4], bool after,
                      bool half);
bool Deko9_TaauResolve(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *color, IDirect3DSurface9 *depth,
                       const int32_t srcRect[4], IDirect3DSurface9 *dst, const int32_t dstRect[4],
                       const deko9::TaauFrame *frame);
// The engine's scene size for the frame being recorded (0x0 = the back
// buffer): published with the frame's GPU time and printed as render= on
// the `DEKO9 perf` line.
void Deko9_SetFrameTag(IDirect3DDevice9 *device, uint32_t width, uint32_t height);
// Latest frame whose lists have all completed on the GPU: its GPU busy time
// (sum of its lists' timestamp spans, the `DEKO9 perf gpu=` measure), its
// frame tag, and a count that advances with each published frame. Never
// waits and never takes the device lock. False before the first frame.
bool Deko9_GetGpuFrame(IDirect3DDevice9 *device, float *gpuMs, uint32_t *width, uint32_t *height, uint32_t *count);

// ---- hazard barrier strength (r_deko9LightBarriers) -----------------------------
// 0 (default): every hazard records DkBarrier_Full + texture/L2 invalidate.
// 1: hazards between 3D-pipe accesses only (render -> sample, sample ->
//    render) record DkBarrier_Primitives + texture-cache invalidate.
// 2: as 1 with DkBarrier_Fragments (assumes no vertex-shader texture reads of
//    a target rendered since the last barrier).
// Hazards involving the copy or 2D engine always record the full barrier.
void Deko9_SetBarrierMode(IDirect3DDevice9 *device, uint32_t mode);
// r_deko9TiledCache: 0 off, 1 = tiled caching (128x128 tiles), 2 = 64x64 tiles.
void Deko9_SetTiledCache(IDirect3DDevice9 *device, uint32_t mode);

// ---- zcull (r_deko9ZcullStats; deko9_zcull.cpp) --------------------------------
// Zcull is always on (the queue created at init; deko3d has no
// per-command-buffer switch). Stats: a CPU model of the zcull region per pass
// (depth-tested draws, draws while the region is invalidated, depth-target
// switches, full/partial depth clears, compare direction flips) and, with
// r_deko9GpuPasses, the DkCounter_ZcullStats hardware counters per pass,
// logged as `DEKO9 zcull` every 60 frames. Applies at the next Present.
// ---- shadow-map filtering (r_shadowFilter) -------------------------------------
// 0 (default): retail lookups. 1: the lit shaders' PCF offset taps around one
// shadow-space position are moved to that position, so the four hardware
// 2x2-PCF fetches of a shader model 3 lookup become one (deko9_shader.h
// Deko9_TranslateShader shadowFilter). Changing it drops the baked programs;
// each affected pixel shader compiles (or loads from the SD cache) the other
// variant at its next draw.
void Deko9_SetShadowFilter(IDirect3DDevice9 *device, uint32_t mode);
// r_deko9ShaderOpt: DEKO9_SHADER_OPT_* bits (deko9_shader.h) every
// translation uses; a change drops the baked programs like the filter.
void Deko9_SetShaderOpt(IDirect3DDevice9 *device, uint32_t mask);

void Deko9_SetZcullStats(IDirect3DDevice9 *device, bool enable);
// The zcull region model's counters of one pass (Deko9GpuPass) since stats
// were turned on or last reported (every 60 frames); false with stats off.
struct Deko9ZcullPassStats
{
    uint64_t ztest, invalid, binds, fullClears, partialClears, flips, alwaysWrites;
};
bool Deko9_GetZcullPassStats(IDirect3DDevice9 *device, uint32_t pass, Deko9ZcullPassStats *out);

// ---- native frame pacing -----------------------------------------------------
// deko9 owns the frame model (deko9_framepace.h): every Present closes frame
// F (ids from 1), signals F's fence on the queue after its last command list,
// and waits for frame F - Deko9_FramesInFlight() before reusing that fence
// slot, so at most that many frames are queued on the GPU and no frame-owned
// resource is reused before its fence. The engine's frame sync (swap wait,
// adaptive GPU sync, end-of-scene fence) waits on frame ids through these
// calls instead of D3D9 event queries.
uint32_t Deko9_FramesInFlight();
// These three never take the device lock, from any thread: they read the
// frame state the recording thread publishes at each Present.
// The frame being recorded (frames presented + 1).
uint64_t Deko9_FrameRecording(IDirect3DDevice9 *device);
// Whether every GPU command of frame `frame` has completed. Frame 0 is done;
// a frame not presented yet is not (nothing of it was submitted as a frame).
bool Deko9_FrameDone(IDirect3DDevice9 *device, uint64_t frame);
// Sleeps (50 us fence-poll slices, no device lock) until
// Deko9_FrameDone(frame) or `timeoutNs`; returns it. A frame not presented
// yet returns false at once. Only polls inside a Deko9Batch.
bool Deko9_WaitFrame(IDirect3DDevice9 *device, uint64_t frame, int64_t timeoutNs);

// ---- frame arena --------------------
// Linear per-frame memory for the back end's dynamic VB/IB, replacing the
// Buffer::Lock DISCARD/NOOVERWRITE rename protocol. A chunk is owned by the
// frame id that consumed it and freed only once that frame is done
// (Deko9_FrameDone); the GPU address is fixed at allocation, so the engine
// writes through `cpu` with plain stores and the buffer binds by `gpu`.
struct Deko9Span
{
    void *cpu;
    uint64_t gpu;
    uint32_t size;
};
// Transient memory valid for frame `frame` (Deko9_FrameRecording() for the
// back end; at most Deko9_FrameRecording() + 1 for a front-end producer).
// `bytes`/`align` as for a plain allocation (align >= 256 for GPU-visible
// vertex/index data, matching every other CPU-written buffer in deko9).
// False on allocation failure; `out` is left untouched. Takes the arena's
// own mutex, not the device lock (except to grow the arena by a chunk).
bool Deko9_FrameAlloc(IDirect3DDevice9 *device, uint64_t frame, uint32_t bytes, uint32_t align, Deko9Span *out);
// Re-points a D3D9 vertex/index buffer's storage at `span` (its GPU base
// changes, offsets into it stay valid): the engine's GfxVertexBufferState/
// GfxIndexBufferState ring (used/total) and every R_SetStreamSource/
// R_ChangeIndices site are unchanged. A window buffer's Lock/Unlock is
// FAIL:DEKO9_WINDOW_LOCK (the two protocols must not be mixed).
void Deko9_BindWindow(IDirect3DVertexBuffer9 *vb, const Deko9Span &span);
void Deko9_BindWindow(IDirect3DIndexBuffer9 *ib, const Deko9Span &span);

// ---- GPU-sync waits ----------------------------------------------------------
// Sleeps the calling thread (50 us slices between fence polls) until the
// GPU has passed the query's last D3DISSUE_END or `timeoutNs` elapses, and
// returns whether it has (the same answer GetData then gives). Never reports done early: it
// waits on the fence of the command list holding the END, and completion is
// still retired in order by the device. Does not submit: a query whose END
// is still in the open list returns false at once (GetData with
// D3DGETDATA_FLUSH submits it). Only polls when the caller holds the device
// lock (inside a Deko9Batch). For the engine's GPU-sync spin
// (RB_AdaptiveGpuSyncFinal), which otherwise calls GetData in a tight loop.
bool Deko9_WaitQuery(IDirect3DQuery9 *query, int64_t timeoutNs);
// Self-test oracle: whether the fence of the command list holding the
// query's END has signalled, read raw (bypasses the cached completed
// sequence). GPU progress only moves forward, so once GetData has returned
// S_OK this must be true.
bool Deko9_DebugQueryGpuPassed(IDirect3DQuery9 *query);

// RAII batch scope.
struct Deko9Batch
{
    explicit Deko9Batch(IDirect3DDevice9 *device) : m_device(device) { Deko9_BeginBatch(device); }
    ~Deko9Batch() { Deko9_EndBatch(m_device); }
    Deko9Batch(const Deko9Batch &) = delete;
    Deko9Batch &operator=(const Deko9Batch &) = delete;

private:
    IDirect3DDevice9 *m_device;
};

// ---- instanced draws -----------------------------------------------------------
//
// One draw for a run of instances of one mesh whose ordinary draws differ
// only in some vertex shader constant registers (a rigid static model's
// placement: world matrix and derived code matrices, lighting coords).
// Protocol, all inside one batch:
//   Deko9_BeginInstances(regs)            registers that vary per instance
//   per instance: set its constants as for an ordinary draw (the engine's
//                 per-prim argument setup), then Deko9_AddInstance
//   Deko9_DrawIndexedInstances(...)       same arguments as the ordinary
//                                         DrawIndexedPrimitive (triangle list)
// The device captures the listed registers from its constant file at each
// AddInstance, uploads them as an instance-rate vertex stream and draws
// with a vertex shader variant that reads those registers from the stream
// (deko9_shader.h instanceRegs): per instance the shader sees exactly the
// values its ordinary draw would have. Everything else (pixel constants,
// textures, render state) must stay unchanged across the run; the caller
// splits runs where it does not (e.g. a lit static model's reflection
// probe). Deko9_BeginInstances returns false (draw per instance instead)
// when the layout has no or more than 16 registers.
bool Deko9_BeginInstances(IDirect3DDevice9 *device, const uint8_t *regs, uint32_t regCount);
void Deko9_AddInstance(IDirect3DDevice9 *device);
// Returns an HRESULT (negative on failure, reported loudly by the device).
int32_t Deko9_DrawIndexedInstances(IDirect3DDevice9 *device, int32_t baseVertex, uint32_t minIndex,
                                   uint32_t numVertices, uint32_t startIndex, uint32_t primCount);
// Counts ordinary per-instance draws the caller made instead (perf line).
void Deko9_NoteInstanceFallback(IDirect3DDevice9 *device, uint32_t draws);

// Several indexed triangle-list draws that share every piece of state (the
// bound index buffer, streams, shaders, constants): the same as `count`
// DrawIndexedPrimitive(D3DPT_TRIANGLELIST, ranges[i].baseVertex, 0, numVertices,
// ranges[i].firstIndex, ranges[i].triCount) calls back to back, recorded with
// one state application and one deko3d draw per range. Used for static index
// data (r_deko9StaticPretess): the visible runs of a world material batch in
// the static world index buffer, or one static model's shared triangles
// once per cached instance (baseVertex = the instance's cache slot).
// Returns an HRESULT.
struct Deko9IndexRange
{
    uint32_t firstIndex;
    uint32_t triCount;
    int32_t baseVertex;
};
int32_t Deko9_DrawIndexedRanges(IDirect3DDevice9 *device, uint32_t numVertices, const Deko9IndexRange *ranges,
                                uint32_t count);

// Work counters since the last per-60-frame perf report (the self-test
// reads deltas between two calls without presenting in between), plus the
// current baked-unit counts and the cumulative verify totals.
struct Deko9Counters
{
    uint64_t draws, bakedHits, bakedMisses, bakedReplays, instancedDraws, instances, instanceFallbacks;
    uint32_t rasterUnits, programUnits;
    uint64_t verifiedDraws, verifyMismatches;
    // r_deko9Census Census_UpdateTexture entry (deko9_callcensus.h): calls and
    // bytes deko9 actually memcpy'd/uploaded for IDirect3DDevice9::UpdateTexture,
    // i.e. only the source's accumulated D3D9 dirty region, not the whole
    // subresource. Only accumulates while the census is on (Deko9_SetCensus);
    // a caller that wants a clean delta calls Deko9_SetCensus(true), presents
    // once (the census reset is applied at the next Present, like GpuPasses),
    // then reads counters before/after the calls under test.
    uint64_t updateTextureCalls, updateTextureBytes;
    // Draws that qualified for the pixel shader's early-Z variant and draws
    // that used it (Deko9_SetEarlyZ).
    uint64_t earlyZCandidates, earlyZDraws;
    // Cumulative since device creation (never reset): draw calls and CPU
    // time spent inside the Draw* entry points (per-draw CPU cost =
    // drawCpuNsTotal / drawsTotal over a window).
    uint64_t drawsTotal, drawCpuNsTotal;
    // Per-draw fast paths (Deko9_SetPerDraw), per-60-frame window like draws:
    // draws that skipped hazard evaluation.
    uint64_t hazardSkips;
    uint64_t probeExtraDraws; // Deko9_SetDrawProbe split sub-draws
    // Deko9_DrawIndexedRanges calls and the deko3d draws (ranges) they
    // recorded.
    uint64_t rangeCalls, rangeDraws;
    // Cumulative since device creation: texture uploads / blits into an
    // image that a draw or copy read since the last barrier (D3D9: the
    // earlier draw must still see the old contents). Each one records a
    // full barrier (host wait-for-idle) before the copy.
    uint64_t uploadAfterReadBarriers;
};
void Deko9_GetCounters(IDirect3DDevice9 *device, Deko9Counters *out);

// ---- D3D9 entry-point call census (r_deko9Census) --------------------------
// Every deko9-implemented D3D9 method (Device/Texture/Surface/VB/IB/Query),
// plus the native fast-path calls above, counts its calls, the bytes deko9
// itself copied (uploads, lock renames, MANAGED readback-then-copy -- never
// the app's own write into a mapped pointer) and CPU time in a per-entry-
// point table (deko9_callcensus.h CensusId); UpdateTexture and texture
// Lock/Unlock also remember which textures were uploaded (top 10 by bytes,
// see Deko9_SetDebugName) and dynamic VB/IB locks by role (see
// Deko9_SetBufferRole). Applied at the next Present, like Deko9_SetGpuPasses
// (a frame is never half recorded); logs one "DEKO9 calls" block every 60
// presented frames. Off (the default): one load and a branch per gated call.
void Deko9_SetCensus(IDirect3DDevice9 *device, bool enable);

// Attaches an image's engine name (GfxImage::name, a stable interned
// string that outlives the texture) to its backing store, read by the
// census's per-texture upload table. Call once, right after CreateTexture /
// CreateCubeTexture / CreateVolumeTexture succeeds.
void Deko9_SetDebugName(IDirect3DBaseTexture9 *texture, const char *name);

// ---- GPU-fault black box ---------------------------------
// r_deko9FaultTrace: interval N > 0 has the GPU report (list, draw) every N
// draws twice: once the work before it reached CROP (dkCmdBufReportValue)
// and once the channel's front end fetched it (a top-of-pipe host
// semaphore release), plus both at list start and end. It keeps a record
// of every draw of the last 16 lists (pass, programs, pixel textures with
// their TIC addresses, targets, draw arguments and buffer addresses;
// deko9's own full-screen passes as native=<kind>) and runs a watcher
// thread: a "DEKO9 gpuhb" heartbeat every 250 ms, and a "DEKO9 blackbox"
// dump (where the GPU stopped and whether the front end or the engine is
// stuck, each pending list's fence, the newest swapchain acquire fence,
// the draws around the GPU's position, the last memory events) when the
// GPU stops mid-list for 20 ms or between lists for 250 ms, or at a queue
// error. 0 = off. r_deko9GpuMap 1: "DEKO9 gpumap" lines for the static pools
// and every image created/resized/freed/named (the VA the GPU addresses it
// through), so offline tooling can name a fault VA. The trace applies at the next
// Present, the map at once (the engine also sets it right after CreateDevice).
void Deko9_SetFaultTrace(IDirect3DDevice9 *device, uint32_t interval);
// The stall watchdog's way to the same dump (any thread, no device lock);
// does nothing before a device exists.
void Deko9_BlackBoxDump(const char *reason);
// Self-test hooks: the two GPU-written crumb cells (CROP, top of pipe; false
// when the trace is off or the top cell is unavailable), and the newest draw
// record of the open list.
bool Deko9_GetFaultTraceCells(IDirect3DDevice9 *device, uint32_t *crop, uint32_t *top);
struct Deko9DrawRecordInfo
{
    uint32_t draw, texCount, count, instances, gpuDraws;
    uint8_t indexed, native;
    uint64_t vb0, ib;
};
bool Deko9_GetLastDrawRecord(IDirect3DDevice9 *device, Deko9DrawRecordInfo *out);
// Submits the open list and decodes its command words from draw's top crumb
// through the next one (r_deko9FaultTrace on); false when they are not found.
bool Deko9_FaultTraceCommandWindow(IDirect3DDevice9 *device, uint32_t draw, char *out, size_t cap, bool *bad);
// r_deko9CmdChunkKB: command-memory chunk size in bytes (0 = default 256 KiB,
// else clamped to 1 KiB..4 MiB), applied at the next Present. Each chunk switch
// starts a new GPFIFO entry, so small chunks put many into every list.
void Deko9_SetCmdChunkBytes(IDirect3DDevice9 *device, uint32_t bytes);
void Deko9_SetGpuMap(IDirect3DDevice9 *device, uint32_t level);
// Internal hook of Deko9_SetDebugName.
void Deko9_GpuMapNoteName(IDirect3DBaseTexture9 *texture);

// Attaches a stable role label (e.g. "dynamicVB", "preTessIB", a string
// literal) to a dynamic vertex/index buffer, read by the census's per-role
// dynamic-buffer lock table. Call once, right after Create*Buffer succeeds.
void Deko9_SetBufferRole(IDirect3DVertexBuffer9 *vb, const char *role);
void Deko9_SetBufferRole(IDirect3DIndexBuffer9 *ib, const char *role);
