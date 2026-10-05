// The real Switch SP entry point.  win32/win_main.cpp's WinMain is the only
// place any platform calls Com_Init from, and it is unconditionally excluded
// from this target (WIN32_SRC is removed from SP_SOURCES for __SWITCH__ in
// scripts/sp/CMakeLists.txt) -- correctly, since it references Win32-only
// state (HINSTANCE, g_wv.hWnd, Win_RegisterClass, AllocConsole) that does not
// exist here.  Without this file the KisakCOD-sp
// target has no replacement entry point at all.  zlib's maketree.c (a
// standalone code-generation tool explicitly marked "should *not* be used
// by applications") must stay out of SP_SOURCES: it supplies a `main` that
// would link in this one's place.
//
// This is a deliberately narrow, faithful port of WinMain's real
// (non-Win32) sequence: every call below is the exact same call WinMain
// makes, in the exact same order, for every subsystem that is not itself
// Win32-specific.  Skipped, and why:
//   - AllocConsole/SetConsoleTitleA/console redirection: Win32 debug
//     console, no Switch equivalent needed (the engine has its own
//     in-game console).
//   - Win_InitLocalization/Sys_FindInfo: both win32-only implementations
//     (win_localize.cpp, and win_main.cpp's own CPUID-based Sys_FindInfo),
//     already excluded with WIN32_SRC; sys_info is a static initializer
//     in switch_misc_stubs.cpp instead (its SSE flag selects the NEON
//     skinning path, r_model_skin_simd.cpp).
//   - Sys_GetSemaphoreFileName/Sys_CheckCrashOrRerun: desktop
//     multiple-instance detection; meaningless for a single homebrew NRO.
//   - hInstance/splash window/Win_RegisterClass/SetFocus(g_wv.hWnd): g_wv
//     and WinVars_t are themselves guarded out for __SWITCH__ in
//     win32/win_local.h.
//   - Steam_Init/KISAK_NULLSUB: Steam SDK and a no-op debug marker.
//   - padInitializeDefault/hid setup: switch_input.c's own IN_Init path
//     already calls padInitializeDefault internally as part of Com_Init's
//     normal subsystem chain; duplicating it here would double-initialize
//     the pad state.
//
#if defined(__SWITCH__)
#include <switch.h>
#include <switch/runtime/nxlink.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <cstdio>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <qcommon/mem_track.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <database/database.h>
#include <gfx_d3d/r_init.h>
#include <universal/critical_section.h>
#include <universal/profile.h>
#include <universal/q_parse.h>
#include <platform/switch/switch_critical_section.h>
#include <platform/switch/switch_platform.h>
#include <platform/switch/switch_port_log.h>
#include <platform/switch/switch_watchdog.h>
#include <platform/switch/switch_thread.h>
#include <port/switch_pcsample.h>
#include <port/switch_heapcheck.h>
#include <platform/switch/switch_sys_event.h>
#include <universal/timing.h>

// Port policy dvars (com_hardware, com_diagMarkers) and the Killhouse proof
// ledgers (default-bind counter, frame evidence, excluded-boundary reach).
#include "switch_diag_dvars.h"
#include <platform/switch/switch_hardware.h>
#include <port/switch_quicksave.h>
#include <port/switch_gyro.h>
#include <port/switch_rumble.h>
#include <script/scr_vm.h>

// switch_misc_stubs.cpp: registers the handful of otherwise-excluded-
// subsystem dvars ordinary, reachable-from-menu code (cinematic volume
// scaling, etc.) reads unconditionally even with sound/physics excluded.
// Previously defined but never called from anywhere -- a real, latent
// null-dvar-dereference bug this file's new Com_Init call path can now
// actually reach.
void Switch_RegisterExcludedSubsystemDvars();
extern "C" void Switch_BootLog(const char *msg);

// Strong overrides of libnx's weak applet globals: the default applet memory
// allocation is too small for this engine's GPU/asset working set, and
// without these libnx's weak defaults win silently (readelf showed both WEAK
// and __nx_applet_type = 0xffffffff, AppletType_Default, before this fix).
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
// The original engine's main thread ran on the Windows reference 1 MiB
// stack (default PE header reserve), and deep renderer frames actually
// observed live -- R_RenderScene -> R_GenerateSortedDrawSurfs ->
// R_AddAllStaticModelSurfacesCamera (its staticModelLodList alone is 4 KiB)
// -> R_CacheStaticModelIndices -- overflowed libnx's 128 KiB NRO default on
// some emulators with a guard-page fault whose PC landed inside the first function
// to touch the exhausted stack. 1 MiB matches the reference ABI's contract.
size_t __nx_stack_size = 0x100000;

// Clean highest-speed handheld APM profile: Horizon's official 0x92220007
// (CPU 1020 MHz / GPU 460.8 MHz / MEM 1600 MHz; switchbrew PTM_services
// "PerformanceConfiguration"). libnx applies index 0 to Normal mode at applet
// startup for AppletType_Application and index 1 to Boost mode (libnx
// nx/source/services/applet.c: "the array index is the PerformanceMode");
// Boost stays 0 so load-boost behavior is unchanged. A firmware-supported
// operating point through the normal APM interface -- no sys-clk, no direct
// clkrst/pcv manipulation. Strong definition overriding libnx's weak
// `{0, 0}` default, beside the other applet overrides above.
u32 __nx_applet_PerformanceConfiguration[2] = {
    KISAK_SWITCH_PERFCONFIG_HANDHELD_NORMAL,
    KISAK_SWITCH_PERFCONFIG_HANDHELD_BOOST
};

#ifndef KISAK_SWITCH_LOG_HOST
#define KISAK_SWITCH_LOG_HOST ""
#endif

static int s_nxlinkSocket = -1;
static bool s_nxlinkBsdInitialized = false;

static void Switch_NxlinkExit()
{
    std::fflush(stdout);
    std::fflush(stderr);
    Port_LogShutdown();
    if (s_nxlinkSocket >= 0)
    {
        close(s_nxlinkSocket);
        s_nxlinkSocket = -1;
    }
    if (s_nxlinkBsdInitialized)
    {
        socketExit();
        s_nxlinkBsdInitialized = false;
    }
}

// switch_platform.c's Sys_Print: only when nxlink stdio is up does stdout
// reach anyone; emulators (and a hardware run without a log host) only
// see svcOutputDebugString.
extern "C" int Switch_NxlinkStdioActive(void)
{
    return s_nxlinkSocket >= 0;
}

static void Switch_NxlinkInit()
{
    if (KISAK_SWITCH_LOG_HOST[0] &&
        inet_pton(AF_INET, KISAK_SWITCH_LOG_HOST, &__nxlink_host) != 1)
    {
        static const char kInvalidHost[] = "NXLINK: invalid KISAK_SWITCH_LOG_HOST\n";
        svcOutputDebugString(kInvalidHost, sizeof(kInvalidHost) - 1);
        return;
    }
    if (__nxlink_host.s_addr == 0 || R_FAILED(socketInitializeDefault()))
        return;

    s_nxlinkBsdInitialized = true;
    s_nxlinkSocket = nxlinkStdio();
    if (s_nxlinkSocket < 0)
    {
        socketExit();
        s_nxlinkBsdInitialized = false;
        static const char kConnectFailed[] = "NXLINK: callback connection failed\n";
        svcOutputDebugString(kConnectFailed, sizeof(kConnectFailed) - 1);
    }
}

// Runs once nxlink (if any) is connected: from here a stalled link can only
// drop log lines, never block a thread that prints.
static void Switch_LogStart()
{
    Port_LogStart(s_nxlinkSocket);
    Switch_WatchdogStart();
}

// The critical-section and sys-event Switch ports abort() on misuse with no
// output of their own; their failure hooks exist for exactly this and were
// never wired. Report the owner subsystem before the abort so a silent SIGABRT
// is attributable (and visible in emulator logs) instead of anonymous.
static void Switch_FatalSubsystemHook(const char *what)
{
    char line[128];
    std::snprintf(line, sizeof(line), "SWITCH_FATAL: %s failure\n", what);
    Sys_Print(line);
}

static void Switch_CriticalSectionFailedHook(void *)
{
    Switch_FatalSubsystemHook("critical-section");
}

static void Switch_SysEventFailedHook(void *)
{
    Switch_FatalSubsystemHook("sys-event");
}

static void Switch_ThreadFailedHook(void *)
{
    Switch_FatalSubsystemHook("thread-context");
}

static void Switch_ReportExit()
{
    char line[512];
    int n = std::snprintf(line, sizeof(line), "SWITCH_EXIT thread=%p core=%d base=%p stack=",
                          (void *)threadGetSelf(), (int)svcGetCurrentProcessorNumber(),
                          reinterpret_cast<void *>(&Switch_ReportExit));
    const uint64_t *fp = static_cast<const uint64_t *>(__builtin_frame_address(0));
    for (int depth = 0; depth < 16 && fp && !(reinterpret_cast<uintptr_t>(fp) & 15) && n < (int)sizeof(line) - 20; ++depth)
    {
        n += std::snprintf(line + n, sizeof(line) - n, depth ? ",%llx" : "%llx", (unsigned long long)fp[1]);
        const uint64_t *next = reinterpret_cast<const uint64_t *>(fp[0]);
        if (next <= fp)
            break;
        fp = next;
    }
    std::snprintf(line + n, sizeof(line) - n, "\n");
    svcOutputDebugString(line, std::strlen(line));
    std::fputs(line, stdout);
    std::fflush(stdout);
}

int main(int argc, char **argv)
{
    std::atexit(Switch_NxlinkExit);
    Switch_NxlinkInit();
    Switch_LogStart();

    Switch_CriticalSectionSetFailureHook(Switch_CriticalSectionFailedHook, nullptr);
    Switch_SysEventSetFailureHook(Switch_SysEventFailedHook, nullptr);
    Switch_ThreadSetFailureHook(Switch_ThreadFailedHook, nullptr);
    Sys_InitializeCriticalSections();
    Sys_InitMainThread();
    track_init();

    Com_InitParse();
    Dvar_Init();
    Switch_RegisterExcludedSubsystemDvars();
    RetailKillhouseRegisterPortDvars();
    const dvar_t *killhouseAutostart = Dvar_RegisterBool(
        "switch_killhouse_autostart", false, DVAR_NOFLAG,
        "Enqueue the normal spmap killhouse command once after boot");
    // Emulator-only stand-in for a human pressing a button on the briefing
    // screen: script one A press right after activation. Off by default; a
    // real player dismisses the briefing themselves. smp-stress.sh sets it
    // to reach gameplay unattended.
    Dvar_RegisterBool(
        "switch_killhouse_pressA", false, DVAR_NOFLAG,
        "Script a single A press after activation to dismiss the briefing (scripted captures)");
    const dvar_t *paramSweep = Dvar_RegisterBool(
        "scr_paramSweep", false, DVAR_NOFLAG,
        "Enumerate every script too-many-parameters mismatch in one run");
    InitTiming();
    Profile_Init();
    Profile_InitContext(0);
    Sys_Milliseconds();

    // Test hygiene: this NRO boots as production by default. The Killhouse
    // first-BSP-frame campaign opts in per run through a small diagnostic
    // command file on the SD card, read here and appended to the command
    // line. That keeps the camera/first-frame/one-shot dvars out of the
    // binary and out of config.cfg (where DVAR_ARCHIVE silently replays a
    // previous run's settings), makes each run self-describing -- the applied
    // text is boot-logged -- and lets a test set renderer dvars or opt into
    // cull-none without a rebuild. All r_killhouse* dvars are DVAR_NOFLAG for
    // the same reason.
    static char commandLine[4096];
    commandLine[0] = '\0';
    {
        FILE *diagFile = std::fopen("sdmc:/switch/kisakcod/kisak_diag.cfg", "rb");
        if (diagFile)
        {
            char diag[2048];
            const size_t diagBytes = std::fread(diag, 1, sizeof(diag) - 1, diagFile);
            std::fclose(diagFile);
            diag[diagBytes] = '\0';
            for (size_t i = 0; i < diagBytes; ++i)
            {
                if (diag[i] == '\n' || diag[i] == '\r')
                    diag[i] = ' ';
            }
            std::snprintf(commandLine, sizeof(commandLine), "%s", diag);
            // Not va() (1 KiB, Com_Error before Com_Init): the cfg is up to 2 KiB.
            char header[80];
            std::snprintf(header, sizeof(header), "switch_sp_main: applied kisak_diag.cfg (%zu bytes): ", diagBytes);
            Switch_BootLog(header);
            Switch_BootLog(diag);
            Switch_BootLog("\n");
        }
        else
        {
            Switch_BootLog("switch_sp_main: no kisak_diag.cfg; production command line\n");
        }
    }
    // nxlink arguments (`nxlink.sh --args "+set ..."`) come after the SD card
    // cfg, so a pushed run can set its dvars without an FTP round trip.
    for (int i = 1; i < argc; ++i)
    {
        const size_t used = std::strlen(commandLine);
        std::snprintf(commandLine + used, sizeof(commandLine) - used, " %s", argv[i]);
    }
    if (argc > 1)
    {
        // Not va(): its buffer is shorter than a long diagnostic command line
        // (Com_Error "va: too short" before Com_Init).
        Switch_BootLog("switch_sp_main: command line with nxlink args: ");
        Switch_BootLog(commandLine);
        Switch_BootLog("\n");
    }
    Switch_BootLog("switch_sp_main: calling Com_Init...\n");
    // Silent-exit tracer: a process that leaves through exit() prints which
    // thread called it and its frame-pointer stack (sv_smp 1 hardware death).
    std::atexit(Switch_ReportExit);
    SwitchPcSample_RegisterCurrentThread(THREAD_CONTEXT_MAIN);
#ifdef KISAK_SWITCH_HEAP_CHECK
    HeapCheck_Start();
#endif
    Com_Init(commandLine);
    // Registered after Com_Init, with the engine's own subsystem state fully up:
    // string dvars reach CanKeepStringPointer/Sys_GetValue(1) and the dvar pool,
    // neither of which exists before the engine initializes them.
    Switch_QuickSaveRegisterDvars();
    Switch_GyroRegisterDvars();
    Switch_RumbleRegisterDvars();
    if (paramSweep && paramSweep->current.enabled)
    {
        Scr_SetParamMismatchSweep(true);
        Com_Printf(0, "SCR_PARAM_SWEEP enabled=1\n");
    }
    Switch_BootLog("switch_sp_main: Com_Init returned! Entering appletMainLoop...\n");

    Com_Printf(0, "MAIN LOOP STARTING! Entering appletMainLoop...\n");
    uint32_t frameNum = 0;
    bool killhouseAutostarted = false;
    bool retailLoadFailureReported = false;
    while (appletMainLoop())
    {
        if (!retailLoadFailureReported &&
            g_switchRetailLoadFailed.load(std::memory_order_acquire))
        {
            retailLoadFailureReported = true;
            char failure[320];
            snprintf(failure, sizeof(failure),
                     "switch_sp_main: retail zone load failed: %s\n",
                     DB_SwitchRetailLoadFailure());
            Switch_BootLog(failure);
            Com_PrintError(10, "%s", failure);
        }
        // switch_killhouse_autostart is the hands-free map trigger: after
        // boot it queues the normal production `spmap killhouse` path (full
        // server/CM/game/cgame setup). Fires at most once per boot.
        //
        // switch_save_autoload takes precedence: it loads a savegame out of
        // the main menu instead (same frame, same one-shot rule), so an
        // iteration can resume where the last run stopped instead of
        // replaying the mission.
        const bool saveBootHandled = Switch_QuickSaveFrame((int)frameNum) != 0;
        Switch_AutoCmdFrame();
        if (!killhouseAutostarted && frameNum == 300 && killhouseAutostart &&
            killhouseAutostart->current.enabled && !saveBootHandled)
        {
            killhouseAutostarted = true;
            Cbuf_AddText(0, "spmap killhouse\n");
            if (com_hardware && com_hardware->current.enabled)
            {
                Com_Printf(0, "KILLHOUSE_HARDWARE_MODE autostart=production map=killhouse\n");
            }
        }
        if (frameNum < 10 || (frameNum % 100) == 0)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "switch_sp_main: Com_Frame frame=%u\n", frameNum);
            Switch_BootLog(buf);
            Com_Printf(0, "Com_Frame: frame=%u\n", frameNum);
        }
        Com_Frame();
        ++frameNum;
    }

    // appletMainLoop() has gone false, so the applet is being torn down and
    // every other thread's own applet loop is breaking at the same instant.
    // Release the D3D9 device here rather than letting crt0 branch straight
    // into exit(): dropping the device is what runs the deko9 backend's own
    // teardown (device/queue/allocator destruction) while the filesystem is
    // still mounted. Leaving it to exit() risks that teardown, or any worker
    // thread it joins, still running when __appExit() unmounts fsdev out
    // from under it.
    //
    // Skipped once a fatal error is unwinding: Sys_Error's thread owns the
    // exit path then, and re-entering renderer teardown on top of the state
    // that killed the run is how a fatal turns into a second crash.
    if (!com_errorEntered && dx.device)
        R_ShutdownDirect3D();

    Switch_ExitOnce(0);
}
#endif
