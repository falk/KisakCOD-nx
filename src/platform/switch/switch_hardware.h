#pragma once

// Physical-hardware execution mode for the Switch port (SP-first).
//
// `com_hardware` (registered in switch_diag_dvars.cpp, default
// false, set per-run with `+set com_hardware 1` in
// sdmc:/switch/kisakcod/kisak_diag.cfg) marks a physical-Switch run.
//
// This header is deliberately portable pure policy (no dvar/engine
// includes) so every target can use it. The dvar wiring stays in the SP TUs
// that already own those dvars (switch_diag_dvars.cpp,
// switch_sp_main.cpp).
//
// Highest clean handheld Horizon APM performance configuration (switchbrew
// PTM_services "PerformanceConfiguration"): CPU 1020 MHz / GPU 460.8 MHz /
// MEM 1600 MHz. Index 0 selects it for Normal mode; the Boost slot (index 1)
// stays 0 so load-boost behavior is unchanged. A firmware-supported
// operating point requested through the normal APM interface: no sys-clk,
// no direct clkrst/pcv manipulation.
#define KISAK_SWITCH_PERFCONFIG_HANDHELD_NORMAL 0x92220007u
#define KISAK_SWITCH_PERFCONFIG_HANDHELD_BOOST 0x00000000u
