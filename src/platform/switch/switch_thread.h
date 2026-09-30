#ifndef SWITCH_THREAD_H
#define SWITCH_THREAD_H

#if !defined(__SWITCH__)
#error "Switch thread owner requires __SWITCH__"
#endif
#if !defined(KISAK_SP) || defined(KISAK_MP) || defined(KISAK_NO_FASTFILES)
#error "Switch thread owner requires KISAK_SP without MP or KISAK_NO_FASTFILES"
#endif

#include <stddef.h>
#include <stdint.h>

#ifndef __cdecl
#define __cdecl
#endif

// Same shared guard qcommon/threads.h and gfx_d3d/rb_backend.h use: whichever
// header is included first defines ThreadContext_t, the others skip theirs.
// Without it a translation unit that needs both this file's lifecycle API and
// threads.h's wait/signal API (switch_thread_sync.cpp) fails on a redefinition
// even though the enumerators are identical.
#ifndef THREAD_CONTEXT_ENUM_DEFINED
#define THREAD_CONTEXT_ENUM_DEFINED
enum ThreadContext_t : int32_t
{
    THREAD_CONTEXT_MAIN = 0x0,
    THREAD_CONTEXT_BACKEND = 0x1,
    THREAD_CONTEXT_WORKER0 = 0x2,
    THREAD_CONTEXT_WORKER1 = 0x3,
    THREAD_CONTEXT_WORKER2 = 0x4,
    THREAD_CONTEXT_SERVER = 0x5,
    THREAD_CONTEXT_TRACE_COUNT = 0x6,
    THREAD_CONTEXT_TRACE_LAST = 0x5,
    THREAD_CONTEXT_CINEMATIC = 0x6,
    THREAD_CONTEXT_TITLE_SERVER = 0x7,
    THREAD_CONTEXT_DATABASE = 0x8,
    THREAD_CONTEXT_STREAM = 0x9,
    THREAD_CONTEXT_SNDSTREAMPACKETCALLBACK = 10,
    THREAD_CONTEXT_SERVER_DEMO = 11,
    THREAD_CONTEXT_COUNT = 12,
};
#endif

extern uint32_t threadId[THREAD_CONTEXT_COUNT];
extern void *threadHandle[THREAD_CONTEXT_COUNT];

typedef void (*SwitchThreadFailureHook)(void *context);

void Switch_ThreadSetFailureHook(SwitchThreadFailureHook hook, void *context);
void Switch_ThreadWaitForExit(ThreadContext_t threadContext);

void __cdecl Sys_InitMainThread();
uint32_t __cdecl Sys_GetCurrentThreadId();
void __cdecl Sys_CreateThread(void (__cdecl *function)(uint32_t), ThreadContext_t threadContext);
uint32_t __cdecl Sys_ThreadMain(ThreadContext_t threadContext);
void __cdecl Sys_SuspendThread(ThreadContext_t threadContext);
void __cdecl Sys_ResumeThread(ThreadContext_t threadContext);
// Cores this process may schedule on (Horizon process core mask).
uint32_t Switch_ProcessCoreCount();
// Deepest server-thread stack use seen so far, in bytes (0 before it runs).
size_t Switch_ServerStackUsed();
// Core the SP server thread (sv_smp 1) is pinned to: the highest core of the
// process mask other than the calling (main) thread's, or libnx's default
// core (-2) when the process has only one.
int Switch_ServerThreadCpuId();
// switch_thread_sync.cpp: retail's renderer handshake events (threads.cpp:145-149),
// created by Sys_SpawnRenderThread.
void Switch_CreateRendererEvents();
bool __cdecl Sys_IsMainThread();
void __cdecl Sys_BeginLoadThreadPriorities();
void __cdecl Sys_EndLoadThreadPriorities();

// APM CPU boost (ApmCpuBoostMode_FastLoad) held while zone loads run: the
// retail-zone decode is entirely CPU-bound (decode + inflate), so the
// ~1.75x clock is the platform's own load-time lever. Reference-counted so
// the map-load window (Sys_Begin/EndLoadThreadPriorities) and each zone
// load (DB_TryLoadXFileInternal) can nest. No-op refcount only under the
// host proof.
void Switch_CpuBoostAcquire();
void Switch_CpuBoostRelease();
bool Switch_CpuBoostActive();

#endif
