#pragma once

#include <universal/q_shared.h>

// Port policy dvars owned by the Switch SP entry path (moved out of
// db_retail_killhouse_preflight.{cpp,h}, seam 11 -- that file is now
// db_retail_frame_evidence.{cpp,h} and no longer owns these).
//
// `com_hardware` (set per-run on a real Switch via `+set com_hardware 1` in
// kisak_diag.cfg) marks a physical-hardware run. `com_diagMarkers` gates the
// port's high-volume per-frame marker printing.
extern const dvar_t *com_hardware;
extern const dvar_t *com_diagMarkers;

// Switch startup calls this after Dvar_Init.
void RetailKillhouseRegisterPortDvars();
