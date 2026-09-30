// Host proof for the clean highest-speed handheld APM selection (SP-first).
//
// The shipping Switch entry (src/platform/switch/switch_sp_main.cpp) strong-defines
// libnx's weak __nx_applet_PerformanceConfiguration as { NORMAL, BOOST } so
// Horizon runs the official 0x92220007 operating point (CPU 1020 MHz /
// GPU 460.8 MHz / MEM 1600 MHz) through the normal APM interface -- no
// sys-clk, no direct clkrst/pcv manipulation. That TU only compiles under
// devkitA64, so this host test pins the portable half of the contract: the
// shared IDs in switch_hardware.h match the published profile (switchbrew
// PTM_services "PerformanceConfiguration") and the Boost slot stays 0, so
// load-boost behavior is unchanged. The source-enrollment half (the strong
// definition itself) is owned by the source gate.
#include "src/platform/switch/switch_hardware.h"

#include <cstdio>

int main()
{
    if (KISAK_SWITCH_PERFCONFIG_HANDHELD_NORMAL != 0x92220007u)
        return 1;
    if (KISAK_SWITCH_PERFCONFIG_HANDHELD_BOOST != 0x00000000u)
        return 1;
    std::printf("PASS:HANDHELD_PERFCONFIG normal=0x92220007 boost=0\n");
    return 0;
}
