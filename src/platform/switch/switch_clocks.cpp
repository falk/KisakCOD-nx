// SWITCH_CLOCKS: the clock rates the console is actually running at, for GPU
// and CPU perf runs. The APM performance configuration
// the port requests (switch_hardware.h: handheld 0x92220007 = CPU 1020 MHz /
// GPU 460.8 MHz / EMC 1600 MHz; docked default = GPU 768 MHz) is only a
// request: an overclock sysmodule (sys-clk) replaces it behind the game's
// back, and every ms number from such a run describes a different GPU. The
// rates come from clkrst (firmware 8.0+; pcv before it), which the homebrew
// loader can open; a failure prints the result codes instead of rates.
//
// Printed from the SWITCH_PERF window (switch_perf.cpp, weak hook) the first
// time and whenever a rate or the operation mode changes, so a run log names
// its clocks next to its frame times. Offline tooling reports it.

#include <switch.h>

#include <cstdio>

extern void Com_Printf(int channel, const char *fmt, ...);

namespace
{
struct ClockSample
{
    uint32_t cpuHz = 0, gpuHz = 0, emcHz = 0;
    Result rc = 0;
    int opMode = -1, perfMode = -1;
};

Result ReadRate(PcvModuleId module, PcvModule legacy, uint32_t *hz)
{
    if (hosversionAtLeast(8, 0, 0))
    {
        ClkrstSession session;
        Result rc = clkrstOpenSession(&session, module, 3);
        if (R_FAILED(rc))
            return rc;
        rc = clkrstGetClockRate(&session, hz);
        clkrstCloseSession(&session);
        return rc;
    }
    return pcvGetClockRate(legacy, hz);
}

bool s_init;
Result s_initRc;
ClockSample s_last;
bool s_haveLast;

ClockSample Sample()
{
    ClockSample s;
    if (!s_init)
    {
        s_init = true;
        s_initRc = hosversionAtLeast(8, 0, 0) ? clkrstInitialize() : pcvInitialize();
    }
    s.opMode = (int)appletGetOperationMode();
    s.perfMode = (int)appletGetPerformanceMode();
    if (R_FAILED(s_initRc))
    {
        s.rc = s_initRc;
        return s;
    }
    Result rc = ReadRate(PcvModuleId_CpuBus, PcvModule_CpuBus, &s.cpuHz);
    if (R_SUCCEEDED(rc))
        rc = ReadRate(PcvModuleId_GPU, PcvModule_GPU, &s.gpuHz);
    if (R_SUCCEEDED(rc))
        rc = ReadRate(PcvModuleId_EMC, PcvModule_EMC, &s.emcHz);
    s.rc = rc;
    return s;
}
} // namespace

// Weak-referenced by switch_perf.cpp (host tests link that file alone).
void Switch_ClocksReport(void)
{
    const ClockSample s = Sample();
    if (s_haveLast && s.cpuHz == s_last.cpuHz && s.gpuHz == s_last.gpuHz && s.emcHz == s_last.emcHz &&
        s.opMode == s_last.opMode && s.rc == s_last.rc)
        return;
    s_last = s;
    s_haveLast = true;
    const char *mode = s.opMode == AppletOperationMode_Handheld  ? "handheld"
                       : s.opMode == AppletOperationMode_Console ? "docked"
                                                                 : "unknown";
    if (R_FAILED(s.rc))
    {
        Com_Printf(16, "SWITCH_CLOCKS unavailable rc=0x%x mode=%s perfMode=%d\n", (unsigned)s.rc, mode, s.perfMode);
        return;
    }
    if (!s.cpuHz || !s.gpuHz || !s.emcHz)
    {
        // Some emulators' clkrst answers success with 0 Hz: not a clock reading.
        Com_Printf(16, "SWITCH_CLOCKS unavailable rates=%u,%u,%u Hz (stubbed service) mode=%s perfMode=%d\n",
                   (unsigned)s.cpuHz, (unsigned)s.gpuHz, (unsigned)s.emcHz, mode, s.perfMode);
        return;
    }
    // Stock ceilings the port asks for: handheld GPU 460.8 MHz, docked 768 MHz.
    const uint32_t stockGpuHz = s.opMode == AppletOperationMode_Console ? 768000000u : 460800000u;
    Com_Printf(16, "SWITCH_CLOCKS cpu=%.1fMHz gpu=%.1fMHz emc=%.1fMHz mode=%s perfMode=%d gpu_vs_stock=%s\n",
               s.cpuHz / 1e6, s.gpuHz / 1e6, s.emcHz / 1e6, mode, s.perfMode,
               s.gpuHz == stockGpuHz ? "stock" : (s.gpuHz > stockGpuHz ? "ABOVE" : "below"));
}
