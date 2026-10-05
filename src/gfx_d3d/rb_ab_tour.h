#pragma once

// In-engine A/B measurement for the deko3d renderer (deko3d build only):
// the fixed-viewpoint tour, the emissive material skip, and the engine side
// of the draw census.
//
// r_deko9DrawCensus 1|2 (draw census, every pass): the device brackets every
// draw of the frame itself (Deko9_SetDrawCensus) and keys it by gpupass,
// material (labelled here from R_SetupPass), pixel shader and blend state;
// `DEKO9 dcensus` lines give per-pass draws/prims/samples/fragment
// invocations/GPU time, the lights-pass light counts, and rows ranked by
// estimated cost (shaded pixels x PS instructions). r_deko9DrawCensusPasses
// limits it to comma-separated gpupass names ("" = all; "emissive" for the
// emissive pass alone). Reports every r_deko9DrawCensusFrames frames or,
// under the tour, in the tour's census phases.
//
// r_deko9SkipEmissive "a,b,*": emissive-list materials whose name contains
// one of the comma-separated substrings are not drawn ("*" = all of them,
// "=name" = exactly that material), for GPU A/B runs. Off when empty.
//
// r_deko9EmissiveTour 1 (the A/B tour; the name is historical): after the
// map has run r_deko9EmissiveTourTimes' delay, teleports the player
// (setviewpos, god, notarget) through r_deko9EmissiveTourSpots and, per
// spot, runs a census phase (only when r_deko9DrawCensus was set: the census
// serializes the GPU, so the tour turns it off in every other phase), then
// one phase per group of r_deko9EmissiveTourGroups between two baselines;
// each phase settles, then measures and prints one `EMISSIVE_PHASE` line
// (GPU ms per frame of the emissive pass and the whole frame, from
// r_deko9GpuPasses, which the tour turns on), `PERDRAW_PHASE` and
// `EMISSIVE_PHASE_PASSES` lines and, with the census on, a census report
// labelled with it. Level-neutral: the spots default to "here" (the
// player's own position) and the groups to "-" (none); a run supplies the
// level's spots and groups from its command line or a cfg, e.g.
//   +set r_deko9EmissiveTour 1 +set r_deko9EmissiveTourGroups rain:rain/nofx:@fx_draw
// Formats avoid spaces, ';' and '+' (the command line splits on them):
//   spots  "x,y,z,yaw,pitch[,turn]/..." or "here" (turn: degrees per second
//          the view keeps turning at that spot, + left / - right)
//   groups "name:substr,substr/name:@dvar/name:@dvar=value/..." (an @dvar
//          entry sets that boolean dvar to 0 for the phase instead of
//          skipping materials, @dvar=value sets any dvar to value and
//          restores it after the phase; "-" = no groups: census + one
//          baseline per spot)
//   times  "delay,settle,measure" in seconds

#include <cstdint>

struct GfxDrawSurfListInfo;
struct Material;
struct MaterialTechnique;
struct MaterialPixelShader;
struct MaterialVertexShader;

extern bool g_emissiveSkipOn;      // r_deko9SkipEmissive not empty (this frame)
extern uint32_t g_emissiveSkipped; // material sublists skipped (backend; read by the tour)
extern bool g_drawCensusOn;        // r_deko9DrawCensus != 0 (this frame)

// Draw census labels (only while g_drawCensusOn): the material pass R_SetupPass
// set up, and the point light R_DrawPointLitSurfs draws (index + 1, 0 after;
// viewLights with index 0 starts a view).
void RB_DrawCensusLabel(const Material *material, const MaterialTechnique *technique, const MaterialPixelShader *ps,
                        const MaterialVertexShader *vs);
void RB_DrawCensusLight(uint32_t lightIndex, uint32_t viewLights);

// True when `info` is a view's emissive list.
bool RB_EmissiveListActive(const GfxDrawSurfListInfo *info);
bool RB_EmissiveSkipMaterial(const Material *material);
// Backend, once per frame before drawing (RB_BeginFrame): applies the census
// and skip dvars; periodic census reports when no tour is running.
void RB_AbTourBackendFrame();
// Main thread, once per Com_Frame: the tour state machine.
void R_AbTourFrame();
