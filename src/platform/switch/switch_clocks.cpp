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

#include "switch_hardware.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern void Com_Printf(int channel, const char *fmt, ...);
struct dvar_s;
const dvar_s *Dvar_RegisterString(const char *dvarName, const char *value, uint16_t flags, const char *description);
const char *Dvar_GetString(const char *dvarName);

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

uint32_t s_requestedConfig; // 0 = the port's own
int s_forceReports;         // windows that print even when nothing changed
bool s_apmInit;
Result s_apmRc;

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

// Requests an APM performance configuration for the Normal (handheld) mode;
// 0 puts the port's own back. The clocks settle over the next windows, so the
// report prints for a few windows after the change (a run log then names the
// rates each phase really ran at).
static void SetPerfConfig(uint32_t id)
{
    if (!s_apmInit)
    {
        s_apmInit = true;
        s_apmRc = apmInitialize();
    }
    const uint32_t config = id ? id : KISAK_SWITCH_PERFCONFIG_HANDHELD_NORMAL;
    Result rc = s_apmRc;
    if (R_SUCCEEDED(rc))
        rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, config);
    s_requestedConfig = id;
    s_forceReports = 3;
    Com_Printf(16, "SWITCH_PERFCONFIG request=0x%08x rc=0x%x\n", (unsigned)config, (unsigned)rc);
}

void Switch_StallTestFrame(void); // switch_stall_test.cpp

// Main thread, once per frame: applies r_switchPerfConfig when it changes
// (apm is not safe from the render threads).
void Switch_PerfConfigFrame(void)
{
    Switch_StallTestFrame();
    static bool registered;
    static char applied[24] = "0";
    if (!registered)
    {
        registered = true;
        // A string: ids such as 0x92220008 do not fit the int dvar range.
        Dvar_RegisterString(
            "r_switchPerfConfig", "0", 0,
            "Handheld APM performance configuration id (decimal or 0x hex) to request for clock A/B runs "
            "(0x92220008 = GPU 384 / EMC 1600, 0x00020004 = 384/1331, 0x00020006 = 384/1066, "
            "0x00020001 = 307/1600); 0 restores the port's own configuration; the SWITCH_CLOCKS line names the "
            "rates actually running");
    }
    const char *want = Dvar_GetString("r_switchPerfConfig");
    if (!want || !strncmp(want, applied, sizeof(applied)))
        return;
    strncpy(applied, want, sizeof(applied) - 1);
    SetPerfConfig((uint32_t)strtoull(applied, nullptr, 0));
}

// Weak-referenced by switch_perf.cpp (host tests link that file alone).
void Switch_ClocksReport(void)
{
    const ClockSample s = Sample();
    const bool force = s_forceReports > 0;
    if (force)
        --s_forceReports;
    if (!force && s_haveLast && s.cpuHz == s_last.cpuHz && s.gpuHz == s_last.gpuHz && s.emcHz == s_last.emcHz &&
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
    Com_Printf(16, "SWITCH_CLOCKS cpu=%.1fMHz gpu=%.1fMHz emc=%.1fMHz mode=%s perfMode=%d req=0x%x gpu_vs_stock=%s\n",
               s.cpuHz / 1e6, s.gpuHz / 1e6, s.emcHz / 1e6, mode, s.perfMode, (unsigned)s_requestedConfig,
               s.gpuHz == stockGpuHz ? "stock" : (s.gpuHz > stockGpuHz ? "ABOVE" : "below"));
}
