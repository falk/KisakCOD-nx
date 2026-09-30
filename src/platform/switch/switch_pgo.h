#pragma once

// Profile-guided optimization training hooks (KISAK_SWITCH_PGO=gen builds
// only; see switch_pgo.cpp). Registers the console
// commands switch_pgoReset / switch_pgoDump / switch_pgoClear.
void SwitchPgo_Init();
