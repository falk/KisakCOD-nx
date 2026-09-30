#pragma once

// Per-pass GPU timing markers for the deko3d renderer (r_deko9GpuPasses; see
// src/deko9/deko9_native.h). Each marker starts the named render phase; the
// device attributes GPU time up to the next marker to it.

#include <deko9/deko9_native.h>
#include "r_init.h"
#define RB_GPU_PASS(name) Deko9_GpuMarker(dx.device, Deko9GpuPass_##name)

// Set true only while RB_StandardDrawCommandsCommon (rb_draw3d.cpp) is
// replaying a view's own 2D command list (viewInfo->cmds, the View2D GPU
// pass); false otherwise, including while replaying the shared/HUD list
// (backEndData->cmds, the Hud2D GPU pass). r_view2dAlphaDiag (rb_backend.cpp)
// uses it to label which pass a large 2D quad landed in; platform-independent
// so it also works in the host build.
extern bool g_rbInView2DCmdList;
