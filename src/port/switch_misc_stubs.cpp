// Remaining "engine seams the Win32 shell used to provide" from the whole
// normal-SP-target link closure: none of these are reachable from the
// bounded UI_LINK_CENSUS menu-path closure (console=0, engine_leaves=109), but the normal KisakCOD-sp executable links every subsystem,
// so they still need real symbols.  Each group below explains why a no-op/
// safe-default body (not a fatal trap) is correct: none of them are
// reachable during real gameplay/save/server operation yet on this port
// (single-threaded boot, no remote script debugger, no save-storage
// backend), so returning "nothing to wait for" / "not connected" lets
// ordinary callers proceed instead of deadlocking or crashing.
#include <universal/q_shared.h>
#include <qcommon/threads.h>
#include <win32/win_net_debug.h>
#include <win32/win_local.h>
#include <win32/win_localize.h>
#include <win32/win_storage.h>
#include <sound/snd_public.h>
#include <gfx_d3d/r_cinematic.h>
#include <gfx_d3d/r_screenshot.h>
#include <physics/phys_local.h>
#include <gfx_d3d/r_model_skin.h>
#include "switch_dirtree.h"

#ifdef __SWITCH__
#include <switch.h>
#include <platform/switch/switch_wake_flag.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>

// --- Process exit -----------------------------------------------------------
// Every path off this platform reaches newlib's exit(), which is not
// reentrant: __call_exitprocs walks the handler list and then _exit() runs
// __appExit(), whose first step is fsdevUnmountAll().  Unmounting NULLs the
// devoptab entry behind every open fd, so a second thread that races past an
// exitproc list the first thread is still walking can unmount the filesystem
// out from under a driver worker that is mid-write -- the worker then
// resolves devoptab_list[dev] to NULL and faults.  That is exactly what the
// shader-cache queue hit: appletMainLoop() goes false on every thread at
// once, so switch_sp_main's main loop returns into exit(0) at the same
// instant Sys_Error's own applet loop breaks into exit(1).
//
// One thread owns the exit; every other caller parks instead of entering
// exit() a second time.  Parking (rather than returning) is correct here:
// both callers below are non-returning by contract, and the process is going
// away under the winner regardless.
static std::atomic<bool> s_exitStarted(false);

void Switch_ExitOnce(int code)
{
    bool expected = false;
    if (!s_exitStarted.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel))
    {
        for (;;)
            svcSleepThread(1000000000ull);
    }

    exit(code);
    __builtin_unreachable();
}

// TEMP DIAGNOSTIC: some emulators do not forward guest stderr, so a
// newlib/libstdc++ abort() is a silent SIGABRT with no line of context.
// Override it with a marker through the debug log and leave via the orderly
// exit path already used for PMem failures. Remove once the aborter is known.
extern "C" __attribute__((noreturn)) void abort(void)
{
    static const char kAbortMarker[] = "SWITCH_ABORT: guest abort() reached\n";
    svcOutputDebugString(kAbortMarker, sizeof(kAbortMarker) - 1);
    exit(1);
    __builtin_unreachable();
}

// --- Fatal error / install path -------------------------------------------
// switch_compat.h documents these two as "resolved at link by common.cpp
// (SP)"; common.cpp itself never did, so this closes that stated intent.
void Sys_Error(const char *error, ...)
{
    char message[1024];
    va_list args;
    va_start(args, error);
    vsnprintf(message, sizeof(message), error, args);
    va_end(args);
    char line[1088];
    const int len = snprintf(line, sizeof(line), "FATAL: %s\n", message);
    svcOutputDebugString(line, (size_t)(len > 0 ? len : 0));
    fwrite(line, 1, (size_t)(len > 0 ? len : 0), stdout);
    fwrite(line, 1, (size_t)(len > 0 ? len : 0), stderr);
    fflush(stdout);
    fflush(stderr);
    while (appletMainLoop())
        svcSleepThread(1000000000ull);
    Switch_ExitOnce(1);
}

char *Sys_DefaultInstallPath(void)
{
    static char path[] = "sdmc:/switch/kisakcod/retail";
    return path;
}

BOOL __cdecl Sys_RemoveDirTree(const char *path)
{
    return Switch_RemoveDirTree(path);
}

// switch_sp_main.cpp already performs this port's real one-time platform
// bring-up before calling Com_Init; nothing further is needed at this
// later hook.
void __cdecl Sys_Init()
{}

void __cdecl Sys_Quit()
{
    Switch_ExitOnce(0);
}

void __cdecl Sys_OutOfMemErrorInternal(const char *filename, int line)
{
    Sys_Error("Out of memory: %s:%d\n", filename ? filename : "?", line);
}

void __cdecl Sys_LoadingKeepAlive()
{
    svcSleepThread(0);
}

// The remote debug socket (win32/win_net_debug.cpp) is Windows-only and
// excluded; Sys_IsRemoteDebugClient() already reports "not connected"
// unconditionally, so shutting down/reopening a socket that never existed
// is a safe no-op.
void __cdecl NET_RestartDebug()
{}

void __cdecl NET_ShutdownDebug()
{}

// Windows-only CPU-affinity/thread-lock performance tuning
// (qcommon/threads.cpp, excluded); no special affinity tuning is needed on
// Switch.
void Win_UpdateThreadLock()
{}

// r_screenshot.cpp is excluded alongside cinematic (see scripts/sp/
// CMakeLists.txt); its replacement lives in switch_excluded_boundaries.cpp so
// the host proof can exercise the real body.

// --- Localization fallback --------------------------------------------
// win_localize.cpp (the real string-table-backed implementation) is
// Win32-shell-only.  Returning the reference key itself is a legible
// (if untranslated) fallback rather than a crash, for the handful of
// error-dialog call sites that still reach this on Switch.
char *__cdecl Win_LocalizeRef(const char *ref)
{
    return const_cast<char *>(ref);
}

// --- Remote script debugger (DevGui debug socket) ----------------------
// Sys_IsRemoteDebugClient() gates every other call in this group; reporting
// "not connected" means none of the rest are ever actually exercised.
int g_debugClient;
unsigned __int8 g_debugPacket[1][8192];

int __cdecl Sys_IsRemoteDebugClient()
{
    return 0;
}

int __cdecl Sys_ReadDebugSocketInt()
{
    return 0;
}

void __cdecl Sys_WriteDebugSocketInt(int value)
{
    (void)value;
}

void __cdecl Sys_WriteDebugSocketString(char *text)
{
    (void)text;
}

int __cdecl Sys_UpdateDebugSocket()
{
    return 0;
}

int __cdecl Sys_ReadDebugSocketData(char *buffer, int len, int blocking)
{
    (void)buffer; (void)len; (void)blocking;
    return 0;
}

void __cdecl Sys_ReadDebugSocketStringBuffer(char *buffer, int len)
{
    if (buffer && len > 0)
        buffer[0] = '\0';
}

void __cdecl Sys_FlushDebugSocketData()
{
}

void __cdecl Sys_AckDebugSocket()
{
}

char *__cdecl Sys_ReadDebugSocketString()
{
    return nullptr;
}

void __cdecl Sys_WriteDebugSocketMessageType(unsigned char type)
{
    (void)type;
}

void __cdecl Sys_EndWriteDebugSocket()
{
}

// --- Database worker-thread coordination ----------------------------------
// The database suspend/resume surface is not threaded on this port yet; the
// renderer back-end handshake is retail's, in switch_thread_sync.cpp.
void __cdecl Sys_SuspendDatabaseThread(ThreadOwner owner)
{
    (void)owner;
}

void __cdecl Sys_ResumeDatabaseThread(ThreadOwner owner)
{
    (void)owner;
}

bool __cdecl Sys_HaveSuspendedDatabaseThread(ThreadOwner owner)
{
    (void)owner;
    return false;
}

// libnx lock/condition adapters for WakeFlag (zero-initialized = unlocked /
// no waiters, so the static flag needs no init ordering).
namespace
{
struct NxLock
{
    Mutex m = 0;
    void lock() { mutexLock(&m); }
    void unlock() { mutexUnlock(&m); }
};
struct NxCond
{
    CondVar c = 0;
    void notify_one() { condvarWakeOne(&c); }
    void wait(NxLock &lock) { condvarWait(&c, &lock.m); }
};
WakeFlag<NxLock, NxCond> s_wakeDatabase;
} // namespace
static std::atomic<bool> s_databaseCompleted(true);
static std::atomic<bool> s_databaseCompleted2(true);

bool __cdecl Sys_IsDatabaseReady()
{
    return s_databaseCompleted.load(std::memory_order_acquire);
}

void __cdecl Sys_DatabaseCompleted2()
{
    s_databaseCompleted2.store(true, std::memory_order_release);
}

bool __cdecl Sys_IsDatabaseReady2()
{
    return s_databaseCompleted2.load(std::memory_order_acquire);
}

void __cdecl Sys_WaitStartDatabase()
{
    s_wakeDatabase.Wait();
}

void __cdecl Sys_DatabaseCompleted()
{
    s_databaseCompleted.store(true, std::memory_order_release);
}

void __cdecl Sys_SyncDatabase()
{
    while (!s_databaseCompleted.load(std::memory_order_acquire))
    {
        svcSleepThread(1000000ull);
    }
}

void __cdecl Sys_WakeDatabase()
{
    s_databaseCompleted.store(false, std::memory_order_release);
}

void __cdecl Sys_WakeDatabase2()
{
    s_databaseCompleted2.store(false, std::memory_order_release);
}

void __cdecl Sys_NotifyDatabase()
{
    s_wakeDatabase.Notify();
}

void Sys_Sleep(uint32_t msec)
{
    svcSleepThread((uint64_t)msec * 1000000ull);
}

// --- Server thread coordination -----------------------------------------
// the server thread, its whole event surface and the save-history (demo)
// thread are real now -- retail qcommon/threads.cpp's engine-facing half lives
// in switch_thread_sync.cpp, thread creation in switch_thread.cpp -- so none of
// it is stubbed from here any more.  The renderer back-end handshake moved
// there too; the database handshake above is what remains of the seam.
int __cdecl LiveStorage_GetStat(int a1, int index)
{
    (void)a1; (void)index;
    return 0;
}

void __cdecl LiveStorage_NewUser()
{
}

// --- Raw Win32 file API (db_registry.cpp's dev-only zone-reorder export) --
// DB_EndReorderZone's CreateFileA/WriteFile/CloseHandle calls are gated by
// s_dbReorder.entryCount, which only becomes nonzero after DB_BeginReorderZone
// runs, which itself only runs when the "zone_reorder" dvar (default: empty
// string) is manually set to a loading zone's name.  Dead by default, so a
// literal "file open always fails" is correct, not a placeholder: real
// callers already handle CreateFile failure (HANDLE)-1 gracefully.
HANDLE __cdecl CreateFileA(const char *name, DWORD access, DWORD shareMode,
                           void *security, DWORD creation, DWORD flags, HANDLE templateFile)
{
    (void)name; (void)access; (void)shareMode; (void)security;
    (void)creation; (void)flags; (void)templateFile;
    return (HANDLE)(intptr_t)-1;
}

int __cdecl WriteFile(HANDLE file, const void *buffer, DWORD bytesToWrite,
                       DWORD *bytesWritten, void *overlapped)
{
    (void)file; (void)buffer; (void)bytesToWrite; (void)overlapped;
    if (bytesWritten)
        *bytesWritten = 0;
    return 0;
}

int __cdecl CloseHandle(HANDLE handle)
{
    (void)handle;
    return 0;
}

// --- Storage for globals the excluded ODE/sound .cpp files own --
// SysInfo is safe zero-initialized (no code queries CPU/video info before
// that subsystem would set it); the dvar pointers are registered for real,
// with their original defaults, since something may legitimately query
// .enabled/.value on a debug-toggle dvar even while the feature itself does
// nothing.  CinematicGlob now lives in switch_cinematic_ffmpeg.cpp (the
// FFmpeg-backed video backend that replaced switch_cinematic_stubs.cpp).
SysInfo sys_info = {
    1.02,                                // cpuGHz
    4.0,                                 // configureGHz
    4,                                   // logicalCpuCount
    4,                                   // physicalCpuCount
    1024,                                // sysMB
    "NVIDIA Tegra X1 (GM20B)",           // gpuDescription
    true,                                // SSE: SIMD skinning (r_model_skin_simd.cpp, NEON)
    "ARM",                               // cpuVendor
    "Cortex-A57",                        // cpuName
};

extern "C" int Switch_HunkCoreThreadCheck(int allowRenderThread)
{
    return Sys_IsMainThread() || (allowRenderThread && Sys_IsRenderThread()) || Sys_IsDatabaseThread();
}

extern "C" void Switch_HunkCoreFatal(const char *message)
{
    Sys_Error("HUNK_FATAL: %s\n", message ? message : "(null)");
}

extern "C" void Switch_HunkCoreDrop(const char *message)
{
    Sys_Error("HUNK_DROP: %s\n", message ? message : "(null)");
}

extern "C" void Switch_HunkCoreOutOfMemory(const char *message)
{
    Sys_Error("HUNK_OUT_OF_MEMORY: %s\n", message ? message : "(null)");
}

// switch_pmem.cpp's two fatal paths end in std::abort(), which raises
// SIGABRT and lands straight in _exit() -- skipping __call_exitprocs
// entirely.  Neither Mesa's u_queue drain handler nor the shader-cache
// store's own atexit shutdown ever runs, so fsdevUnmountAll() pulls the
// filesystem out from under a cache worker that is still writing and the
// worker faults on a NULLed devoptab entry.
//
// Measured, not assumed: the crashing run's log carries the PMem_Alloc
// message and __appExit's applet calls, but no `FATAL:` line from Sys_Error
// and no "quitting..." from Com_Quit -- abort() was the only remaining exit
// route into __appExit, and it is the one that skips the handlers.
//
// These two hooks are declared weak in switch_pmem.h exactly so a target can
// choose its own fatal behaviour (the host tests longjmp out of them).  Here
// they turn the abort into an orderly exit, so the exitprocs do run and the
// filesystem is still mounted when the driver's workers are joined.
// Switch_ExitOnce does not return, so the std::abort() behind these stays
// unreachable in this target.
extern "C" void Switch_PMemAssertFailed(const char *message)
{
    (void)message;
    Switch_ExitOnce(1);
}

extern "C" void Switch_PMemOutOfMemory(const char *message)
{
    (void)message;
    Switch_ExitOnce(1);
}

extern "C" void Switch_HunkCoreTrack(const char *event, int amount, const char *name, int type)
{
    (void)event;
    (void)amount;
    (void)name;
    (void)type;
}


// win32/win_configure.cpp (excluded) normally owns this storage; r_dvars.cpp
// itself is not excluded and already assigns these for real through the
// ordinary R_RegisterDvars path, exactly like every other platform -- only
// the global storage was missing, matching the same gap
// src/radiant/engine_stubs.cpp already had to fill for the editor target.
const dvar_t *vid_xpos;
const dvar_t *vid_ypos;
const dvar_t *r_fullscreen;

// the sound and physics modules are real on Switch now, so their
// dvars are registered by SND_Init / Phys_Init like every other target.
void Switch_RegisterExcludedSubsystemDvars()
{
}

#endif
