// PGO training hooks, compiled only into KISAK_SWITCH_PGO=gen builds
// (scripts/sp/CMakeLists.txt adds this file and KISAK_SWITCH_PGO_GEN).
//
// libgcov writes the .gcda files from a destructor at exit(), which a Switch
// run under an emulator or nxlink rarely reaches cleanly, so the training script
// dumps explicitly: switch_pgoReset zeroes every counter (e.g. once the level
// is loaded, to weight the profile toward gameplay) and switch_pgoDump merges
// the counters into the .gcda files under the -fprofile-generate directory
// on the SD card. A dump can be repeated; each one adds to the files there.
#include "switch_pgo.h"

#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef KISAK_SWITCH_PGO_SDDIR
#error "switch_pgo.cpp is built only with KISAK_SWITCH_PGO=gen (KISAK_SWITCH_PGO_SDDIR)"
#endif

#include <stdlib.h>
#include <switch.h>

extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);

// ---- thread pointer for libgcov ---------------------------------------------
// devkitA64's libgcov is compiled for hardware TLS: its indirect-call profiler
// (__gcov_indirect_call_profiler_v4[_atomic], called from the prologue of
// every instrumented function whose address is taken) finds the thread-local
// __gcov_indirect_call through `mrs tpidr_el0`. libnx code is -mtp=soft: the
// thread pointer comes from __aarch64_read_tp() (ThreadVars in TPIDRRO_EL0)
// and TPIDR_EL0 is left 0, so the first indirect call crashed the training
// build at FS_Startup (read of 0x1000 + tprel). Both sides use the same
// local-exec tprel offsets, so copying the libnx thread pointer into
// TPIDR_EL0 on every thread makes libgcov and the instrumented callers agree
// on __gcov_indirect_call. The main thread is set from .preinit_array (before
// any constructor runs), every other thread by the threadCreate wrapper
// (-Wl,--wrap=threadCreate in the gen build), before its entry runs. These
// two functions are themselves called indirectly, so they must not carry the
// instrumentation prologue.
__attribute__((no_profile_instrument_function)) static inline void SwitchPgo_SyncHwThreadPointer()
{
    void *tp = __builtin_thread_pointer(); // -mtp=soft: __aarch64_read_tp()
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(tp));
}

__attribute__((no_profile_instrument_function)) static void SwitchPgo_PreinitMainThread()
{
    SwitchPgo_SyncHwThreadPointer();
}
__attribute__((used, section(".preinit_array"))) static void (*const s_pgoPreinit)() = SwitchPgo_PreinitMainThread;

struct SwitchPgoThreadStart
{
    ThreadFunc entry;
    void *arg;
};

__attribute__((no_profile_instrument_function)) static void SwitchPgo_ThreadTrampoline(void *p)
{
    SwitchPgo_SyncHwThreadPointer();
    SwitchPgoThreadStart start = *static_cast<SwitchPgoThreadStart *>(p);
    free(p);
    start.entry(start.arg);
}

extern "C" Result __real_threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz,
                                      int prio, int cpuid);

extern "C" Result __wrap_threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz,
                                      int prio, int cpuid)
{
    SwitchPgoThreadStart *start = static_cast<SwitchPgoThreadStart *>(malloc(sizeof(SwitchPgoThreadStart)));
    if (!start)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    start->entry = entry;
    start->arg = arg;
    const Result rc = __real_threadCreate(t, SwitchPgo_ThreadTrampoline, start, stack_mem, stack_sz, prio, cpuid);
    if (R_FAILED(rc))
        free(start);
    return rc;
}

static cmd_function_s s_pgoDumpVar;
static cmd_function_s s_pgoResetVar;
static cmd_function_s s_pgoClearVar;

// Counts (and with remove, deletes) the .gcda files in the profile directory.
static int SwitchPgo_ScanGcda(bool remove)
{
    DIR *dir = opendir(KISAK_SWITCH_PGO_SDDIR);
    if (!dir)
        return -1;
    int count = 0;
    char path[512];
    while (const dirent *e = readdir(dir))
    {
        const size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 5, ".gcda") != 0)
            continue;
        ++count;
        if (remove)
        {
            snprintf(path, sizeof(path), "%s/%s", KISAK_SWITCH_PGO_SDDIR, e->d_name);
            unlink(path);
        }
    }
    closedir(dir);
    return count;
}

// One dump at the end of a training run: libgcov merges every object's
// counters into its .gcda (read-add-write per file), which blocks the main
// thread for the time printed here -- fine once, at the end.
static void SwitchPgo_Dump_f()
{
    Com_Printf(CON_CHANNEL_DONT_FILTER, "PGO_DUMP begin dir=%s\n", KISAK_SWITCH_PGO_SDDIR);
    const u64 t0 = armGetSystemTick();
    __gcov_dump();
    // __gcov_dump marks the counters dumped; reset re-arms the next dump and
    // starts a fresh interval (the old counts are already merged on disk).
    __gcov_reset();
    const u64 ms = armTicksToNs(armGetSystemTick() - t0) / 1000000;
    Com_Printf(CON_CHANNEL_DONT_FILTER, "PGO_DUMP done files=%d ms=%llu\n", SwitchPgo_ScanGcda(false),
               (unsigned long long)ms);
}

static void SwitchPgo_Reset_f()
{
    __gcov_reset();
    Com_Printf(CON_CHANNEL_DONT_FILTER, "PGO_RESET done\n");
}

// Removes the .gcda files an earlier run left: libgcov would otherwise merge
// this run into them (same build) or drop them with a checksum warning (a
// different build). The training cfg runs it once right after boot; leave it
// out to accumulate several runs of the same NRO into one profile.
static void SwitchPgo_Clear_f()
{
    Com_Printf(CON_CHANNEL_DONT_FILTER, "PGO_CLEAR removed=%d\n", SwitchPgo_ScanGcda(true));
}

void SwitchPgo_Init()
{
    // devkitA64's libgcov is built without directory creation (no mkdir in
    // its imports); -fprofile-prefix-path keeps every .gcda directly in this
    // one directory, so creating it here is enough.
    if (mkdir(KISAK_SWITCH_PGO_SDDIR, 0777) != 0 && errno != EEXIST)
        Com_PrintError(CON_CHANNEL_DONT_FILTER, "FAIL:PGO_DIR mkdir %s: %s\n", KISAK_SWITCH_PGO_SDDIR, strerror(errno));
    else
        Com_Printf(CON_CHANNEL_DONT_FILTER, "PGO_DIR %s ready\n", KISAK_SWITCH_PGO_SDDIR);
    Cmd_AddCommandInternal("switch_pgoDump", SwitchPgo_Dump_f, &s_pgoDumpVar);
    Cmd_AddCommandInternal("switch_pgoReset", SwitchPgo_Reset_f, &s_pgoResetVar);
    Cmd_AddCommandInternal("switch_pgoClear", SwitchPgo_Clear_f, &s_pgoClearVar);
}
