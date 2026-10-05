#include "switch_thread.h"
#include <universal/spin_pause.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>

#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
#include <chrono>
#include <thread>
#else
extern "C"
{
#include <switch/kernel/thread.h>
#include <switch/kernel/svc.h>
#include <switch/result.h>
#include <switch/services/applet.h>
#include <switch/services/apm.h>
}
#endif

extern void __cdecl Sys_InitThread(ThreadContext_t threadContext);
void __cdecl Sys_CreateEvent(bool manualReset, bool initialState, void **event);
void __cdecl Sys_ResetEvent(void **event);
void __cdecl Sys_SetEvent(void **event);
bool __cdecl Sys_WaitForSingleObjectTimeout(void **event, uint32_t msec);
void __cdecl Sys_WaitForSingleObject(void **event);

// Forward-declared, not #included: q_shared.h/profile.h pull in far more
// than this file (and its narrow host test, which compiles it with no -I
// search path at all) needs just to call these two.
void __cdecl Com_InitThreadData(int threadContext);
void __cdecl Profile_InitContext(int profileContext);

// Weak: the thread proofs link this file without the sampler.
void SwitchPcSample_RegisterCurrentThread(int tag) __attribute__((weak));

namespace
{
// Was 0x8000 (32KB) with no documented rationale. Confirmed too small via a
// real stack overflow on the DB_Thread background loader thread: guest
// crash "Invalid memory access" at a virtual address ~64KB below SP, deep
// in DB_Thread -> DB_TryLoadXFileInternal -> RetailWalkLoadZoneAssets ->
// RetailWalkLiveLoadTechniqueSet -> WidenInlineTechnique
// (src/database/db_retail_decode_techniqueset.cpp), whose own two locals
// alone -- FsRetailFastfileMaterialPass passes[64] (4096 bytes) and
// FsRetailFastfileMaterialShaderArgument args[512] (6144 bytes) -- already
// total ~10KB in a single frame, before the rest of that call chain's own
// locals. Bumped with generous headroom rather than tuned to the minimum;
// this is one background thread's stack, not a hot per-frame allocation.
constexpr size_t kThreadStackSize = 0x40000;
// The SP server thread runs G_RunFrame (G_MoverPush alone keeps two
// MAX_GENTITIES int arrays on its stack).  Sized generously until its real
// peak is known (Switch_ServerStackUsed reports it on hardware).
constexpr size_t kServerThreadStackSize = 0x100000;
constexpr int kThreadPriority = 0x3B;
constexpr int kThreadCpuId = -2;
#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
constexpr int32_t kLoadThreadPriority = 0x3B;
constexpr int32_t kMaxThreadPriority = 0x3F;
int32_t s_main_thread_normal_priority;
#endif
bool s_load_priorities_active(false);

typedef void (__cdecl *ThreadFuncFn)(uint32_t);
// std::atomic, not a plain array: Sys_CreateThread's write here must be
// guaranteed visible to the newly created OS thread's very first read in
// Sys_ThreadMain.  A real OS thread-start call is normally itself a
// sufficient happens-before edge for a plain write, but that guarantee is
// not something the C++ memory model gives a non-atomic global on its own,
// and it was observed to fail intermittently under Eden's HLE thread
// creation on Switch (the new thread's first read of its own slot raced
// with this write and read a stale/garbage function pointer, crashing on
// an indirect call to it).  An explicit release/acquire pair removes the
// dependency on that platform guarantee.
std::atomic<ThreadFuncFn> threadFunc[THREAD_CONTEXT_COUNT];

#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
struct HostThread
{
    std::thread thread;
    bool waited;
    void *argument;
};
using SwitchThread = HostThread;
#else
using SwitchThread = Thread;
#endif

// Win32 created every engine thread with CREATE_SUSPENDED and the owner
// resumed it; the renderer workers rely on that (R_InitHardware resumes only
// the workers whose r_smp_worker_thread dvar is enabled).  A context created
// with start=false stays unstarted here until its first Sys_ResumeThread.
std::atomic<bool> s_awaitingStart[THREAD_CONTEXT_COUNT];
// Win32 suspend count (SuspendThread/ResumeThread): only 0->1 pauses and only
// 1->0 resumes; ResumeThread at 0 is a no-op.  A context created suspended
// starts at 1.  Owner-thread operations only (the main thread), like retail.
int32_t s_suspendCount[THREAD_CONTEXT_COUNT];

std::atomic<uint32_t> s_next_thread_id(0);
thread_local uint32_t t_thread_id(0);
SwitchThreadFailureHook s_failure_hook;
void *s_failure_context;

uint32_t ReserveThreadId()
{
    return s_next_thread_id.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool IsValidContext(ThreadContext_t threadContext)
{
    return threadContext >= 0 && threadContext < THREAD_CONTEXT_COUNT;
}

[[noreturn]] void Fail()
{
    if (s_failure_hook != nullptr)
        s_failure_hook(s_failure_context);
    std::abort();
}

// Stack watermark for the server thread: its unused stack is filled with a
// pattern at thread start and Switch_ServerStackUsed() finds the deepest
// byte ever written.  Bounds come from svcQueryMemory on the live SP, so the
// fill never leaves the stack mapping.
constexpr uint64_t kStackPattern = 0x5354414b5354414bull; // "KATSKATS"
std::atomic<uintptr_t> s_serverStackLo{0}, s_serverStackHi{0};

#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
void MarkServerStack()
{
    uintptr_t sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    MemoryInfo info{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&info, &page, sp)))
        return;
    const uintptr_t lo = info.addr + 64;
    const uintptr_t fillEnd = (sp - 4096) & ~(uintptr_t)7;
    for (uint64_t *p = reinterpret_cast<uint64_t *>(lo); reinterpret_cast<uintptr_t>(p) < fillEnd; ++p)
        *p = kStackPattern;
    s_serverStackHi.store(info.addr + info.size, std::memory_order_relaxed);
    s_serverStackLo.store(lo, std::memory_order_release);
}
#endif

void Switch_ThreadEntry(void *argument)
{
    const ThreadContext_t threadContext =
        static_cast<ThreadContext_t>(reinterpret_cast<intptr_t>(argument));
#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
    if (threadContext == THREAD_CONTEXT_SERVER)
        MarkServerStack();
#endif
    if (t_thread_id == 0)
        t_thread_id = threadId[threadContext];
    // Only the threads that carry frame work; idle ones would just fill
    // the sampler's table with their wait sites.
    if (SwitchPcSample_RegisterCurrentThread &&
        (threadContext == THREAD_CONTEXT_BACKEND || threadContext == THREAD_CONTEXT_WORKER0 ||
         threadContext == THREAD_CONTEXT_SERVER || threadContext == THREAD_CONTEXT_DATABASE))
        SwitchPcSample_RegisterCurrentThread(threadContext);
    Sys_ThreadMain(threadContext);
}
}

uint32_t threadId[THREAD_CONTEXT_COUNT];
void *threadHandle[THREAD_CONTEXT_COUNT];

void Switch_ThreadSetFailureHook(SwitchThreadFailureHook hook, void *context)
{
    s_failure_hook = hook;
    s_failure_context = context;
}

void Switch_ThreadWaitForExit(ThreadContext_t threadContext)
{
    if (!IsValidContext(threadContext) || threadHandle[threadContext] == nullptr)
        Fail();
#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
    HostThread *thread = static_cast<HostThread *>(threadHandle[threadContext]);
    if (!thread->waited)
    {
        thread->thread.join();
        thread->waited = true;
    }
#else
    if (R_FAILED(threadWaitForExit(static_cast<SwitchThread *>(threadHandle[threadContext]))))
        Fail();
#endif
}

void __cdecl Sys_InitMainThread()
{
    threadId[THREAD_CONTEXT_MAIN] = Sys_GetCurrentThreadId();
#if !defined(KISAK_SWITCH_THREAD_PROOF_HOST) && !defined(KISAK_SWITCH_THREAD_NARROW_PROOF)
    Com_InitThreadData(THREAD_CONTEXT_MAIN);
#endif
}

uint32_t __cdecl Sys_GetCurrentThreadId()
{
    if (t_thread_id == 0)
        t_thread_id = ReserveThreadId();
    return t_thread_id;
}

namespace
{
void StartCreatedThread(ThreadContext_t threadContext)
{
#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
    HostThread *thread = static_cast<HostThread *>(threadHandle[threadContext]);
    thread->thread = std::thread(Switch_ThreadEntry, thread->argument);
#else
    if (R_FAILED(threadStart(static_cast<SwitchThread *>(threadHandle[threadContext]))))
        Fail();
#endif
}

void CreateThreadOn(void (__cdecl *function)(uint32_t), ThreadContext_t threadContext,
                    int cpuId, bool start)
{
    if (!IsValidContext(threadContext) || threadContext == THREAD_CONTEXT_MAIN ||
        threadFunc[threadContext].load(std::memory_order_relaxed) != nullptr)
    {
        Fail();
    }

    threadFunc[threadContext].store(function, std::memory_order_release);
    threadId[threadContext] = ReserveThreadId();

    SwitchThread *thread = new (std::nothrow) SwitchThread();
    if (thread == nullptr)
        Fail();
    void *argument = reinterpret_cast<void *>(static_cast<intptr_t>(threadContext));
#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
    (void)cpuId;
    thread->waited = false;
    thread->argument = argument;
#else
    const size_t stackSize = threadContext == THREAD_CONTEXT_SERVER ? kServerThreadStackSize : kThreadStackSize;
    if (R_FAILED(threadCreate(thread, Switch_ThreadEntry, argument, nullptr, stackSize,
                              kThreadPriority, cpuId)))
    {
        delete thread;
        threadFunc[threadContext] = nullptr;
        threadId[threadContext] = 0;
        Fail();
    }
#endif
    threadHandle[threadContext] = thread;
    if (start)
        StartCreatedThread(threadContext);
    else
    {
        s_suspendCount[threadContext] = 1;
        s_awaitingStart[threadContext].store(true, std::memory_order_release);
    }
}
}

void __cdecl Sys_CreateThread(void (__cdecl *function)(uint32_t), ThreadContext_t threadContext)
{
    // The SP server thread and the renderer back end each get a core away
    // from main.  On the default core they share core 0 with the main thread
    // at a lower priority, and Horizon never time-slices a lower-priority
    // thread in: G_RunFrame then only runs while main sleeps in
    // SV_WaitServerSnapshot's 1 ms probes, and the back end only while main
    // waits in the render handshake, i.e. serially.
    const bool ownCore = threadContext == THREAD_CONTEXT_SERVER || threadContext == THREAD_CONTEXT_BACKEND;
    const int cpuId = ownCore ? Switch_ServerThreadCpuId() : kThreadCpuId;
#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
    if (ownCore)
    {
        char line[160];
        uint64_t mask = 0;
        svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
        snprintf(line, sizeof(line), "SWITCH_THREAD create ctx=%d core=%d mask=0x%llx main_core=%d\n",
                 (int)threadContext, cpuId, (unsigned long long)mask, (int)svcGetCurrentProcessorNumber());
        svcOutputDebugString(line, strlen(line));
        printf("%s", line);
    }
#endif
    CreateThreadOn(function, threadContext, cpuId, true);
}

// Real, portable half of qcommon/threads.cpp's original Sys_InitThread
// (excluded alongside that file's Windows CreateThread/CreateEvent backend,
// which switch_thread.cpp's own Sys_CreateThread/Sys_CreateEvent already
// replace).  The original also assigns a file-local g_threadLocals pointer
// consumed only by threads.cpp's own Sys_SetValue/Sys_GetValue; this port
// uses its own thread_local t_threadValueSlots storage instead (see
// Sys_SetValue/Sys_GetValue below), which needs no such per-context
// reassignment, so only the two portable calls are ported.
#if !defined(KISAK_SWITCH_THREAD_PROOF_HOST) && !defined(KISAK_SWITCH_THREAD_NARROW_PROOF)
// switch_thread_test.cpp (both the host thread-lifecycle proof, built with
// KISAK_SWITCH_THREAD_PROOF_HOST, and the narrow real-Switch-ELF proof in
// ./test's switch_build(), built with KISAK_SWITCH_THREAD_NARROW_PROOF)
// already defines its own Sys_InitThread stub and links neither
// Com_InitThreadData nor Profile_InitContext; this real version is for the
// actual engine target (KisakCOD-sp) only, which does link both.
void __cdecl Sys_InitThread(ThreadContext_t threadContext)
{
    Com_InitThreadData(threadContext);
    Profile_InitContext(threadContext);
}
#endif

uint32_t __cdecl Sys_ThreadMain(ThreadContext_t threadContext)
{
    if (!IsValidContext(threadContext))
        Fail();
    ThreadFuncFn function = threadFunc[threadContext].load(std::memory_order_acquire);
    if (function == nullptr)
        Fail();
    Sys_InitThread(threadContext);
    function(threadContext);
    return 0;
}

// threads.cpp:327-337's contract: the caller only ever names a context it has
// already spawned (retail's own spawns, and R_UpdateActiveWorkerThreads in
// r_workercmds.cpp:601 for the renderer workers), so a missing handle is a bug
// and traps.  libnx's threadPause/threadResume are the Horizon equivalents of
// SuspendThread/ResumeThread.
//
// The host proof build keeps the trap even for a live thread: a std::thread
// cannot be paused at an arbitrary point by the shim, and failing loudly beats
// a silent no-op that would let a suspend/resume bug pass the host gate.  The
// real pause/resume is exercised by the narrow aarch64 proof in `./test
// switch-build` (switch_thread_test.cpp), which is the same code the NRO runs.
void __cdecl Sys_SuspendThread(ThreadContext_t threadContext)
{
    if (!IsValidContext(threadContext) || threadHandle[threadContext] == nullptr)
        Fail();
    if (s_suspendCount[threadContext]++ != 0)
        return;
    // A never-started context is already not running.
    if (s_awaitingStart[threadContext].load(std::memory_order_acquire))
        return;
#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
    Fail();
#else
    if (R_FAILED(threadPause(static_cast<SwitchThread *>(threadHandle[threadContext]))))
        Fail();
#endif
}

void __cdecl Sys_ResumeThread(ThreadContext_t threadContext)
{
    if (!IsValidContext(threadContext) || threadHandle[threadContext] == nullptr)
        Fail();
    if (s_suspendCount[threadContext] == 0)
        return; // Win32 ResumeThread on a running thread: count stays 0
    if (--s_suspendCount[threadContext] != 0)
        return;
    if (s_awaitingStart[threadContext].exchange(false, std::memory_order_acq_rel))
    {
        StartCreatedThread(threadContext);
        return;
    }
#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
    Fail();
#else
    if (R_FAILED(threadResume(static_cast<SwitchThread *>(threadHandle[threadContext]))))
        Fail();
#endif
}

bool __cdecl Sys_IsMainThread()
{
    return Sys_GetCurrentThreadId() == threadId[THREAD_CONTEXT_MAIN];
}

bool __cdecl Sys_IsRenderThread()
{
    return Sys_GetCurrentThreadId() == threadId[THREAD_CONTEXT_BACKEND];
}

// Same threadId[]-comparison pattern as Sys_IsMainThread/Sys_IsRenderThread
// above; threads.cpp (excluded, see scripts/sp/CMakeLists.txt) implemented
// these identically against its own threadId[] array.
bool __cdecl Sys_IsDatabaseThread()
{
    return Sys_GetCurrentThreadId() == threadId[THREAD_CONTEXT_DATABASE];
}

bool __cdecl Sys_IsServerThread()
{
    return Sys_GetCurrentThreadId() == threadId[THREAD_CONTEXT_SERVER];
}

// Real thread_local storage, matching threads.cpp's g_threadValues/
// g_threadLocals scheme: each OS thread gets its own 4 value slots
// (indices observed in production callers: 2 is a per-thread jmp_buf* for
// Com_Error-style unwinding in common.cpp/sv_main.cpp, which genuinely must
// not be shared between the main and server threads).
namespace
{
thread_local void *t_threadValueSlots[4];
}

void __cdecl Sys_SetValue(int valueIndex, void *data)
{
    t_threadValueSlots[valueIndex] = data;
}

void *__cdecl Sys_GetValue(int valueIndex)
{
    return t_threadValueSlots[valueIndex];
}

namespace
{
std::atomic<uint32_t> s_cpuBoostRefs(0);
std::atomic<bool> s_cpuBoostApplied(false);

void CpuBoostApply(bool on)
{
#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
    const Result rc = appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad
                                               : ApmCpuBoostMode_Normal);
    s_cpuBoostApplied.store(on && R_SUCCEEDED(rc), std::memory_order_release);
#else
    s_cpuBoostApplied.store(on, std::memory_order_release);
#endif
}
}

void Switch_CpuBoostAcquire()
{
    if (s_cpuBoostRefs.fetch_add(1, std::memory_order_acq_rel) == 0)
        CpuBoostApply(true);
}

void Switch_CpuBoostRelease()
{
    const uint32_t previous = s_cpuBoostRefs.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0)
    {
        // Unbalanced release: restore the count instead of wrapping.
        s_cpuBoostRefs.store(0, std::memory_order_release);
        return;
    }
    if (previous == 1 && s_cpuBoostApplied.load(std::memory_order_acquire))
        CpuBoostApply(false);
}

bool Switch_CpuBoostActive()
{
    return s_cpuBoostApplied.load(std::memory_order_acquire);
}

void __cdecl Sys_BeginLoadThreadPriorities()
{
    if (!Sys_IsMainThread() || s_load_priorities_active)
        Fail();
#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
    int32_t priority = 0;
    if (R_FAILED(svcGetThreadPriority(&priority, threadGetCurHandle())))
        Fail();
    int32_t load_priority = kLoadThreadPriority;
    if (priority >= kLoadThreadPriority)
        load_priority = priority < kMaxThreadPriority ? priority + 1 : kMaxThreadPriority;
    if (R_FAILED(svcSetThreadPriority(threadGetCurHandle(), static_cast<uint32_t>(load_priority))))
        Fail();
    s_main_thread_normal_priority = priority;
#endif
    s_load_priorities_active = true;
    Switch_CpuBoostAcquire();
}

void __cdecl Sys_EndLoadThreadPriorities()
{
    if (!Sys_IsMainThread() || !s_load_priorities_active)
        Fail();
#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
    if (R_FAILED(svcSetThreadPriority(
            threadGetCurHandle(), static_cast<uint32_t>(s_main_thread_normal_priority))))
        Fail();
#endif
    s_load_priorities_active = false;
    Switch_CpuBoostRelease();
}

// Sys_SpawnDatabaseThread/Sys_SpawnRenderThread: real worker-thread entry
// points, backed by Sys_CreateThread's existing libnx threadCreate/
// threadStart primitive above.  DB_Thread/RB_RenderThread (their only
// callers, in db_registry.cpp/rb_backend.cpp) are both real `while (1)`
// service loops that never return, so unlike Radiant's engine_stubs.cpp
// (which safely no-ops every Sys_Spawn*Thread because it draws
// synchronously through its own GUI loop and never calls DB_Thread/
// RB_RenderThread's real bodies at all) neither "don't spawn anything,
// just report success" nor "call the function synchronously inline" is
// correct here: the former means DB_TryLoadXFileInternal (and the
// retail-zone loader) never runs at all, and the latter
// hangs Com_Init before it ever reaches Com_Frame.  A real, concurrent
// thread is genuinely required for both.  Sys_CreateThread already aborts
// loudly on failure, matching that a real thread is not optional here.
namespace
{
void *s_workerCmdEvent;
void *s_updateSpotLightEffectEvent;
void *s_updateEffectsEvent;
}

char __cdecl Sys_SpawnRenderThread(void (__cdecl *function)(uint32_t))
{
    // threads.cpp:150-152: the worker command event (retail backendEvent[0])
    // is manual-reset and starts clear; both FX events are manual-reset and
    // start set, so a wait before the first update does not block.  They are
    // created here, not with the workers, because the main thread resets and
    // waits on the worker event (R_ProcessWorkerCmdsWithTimeout) and drives
    // the FX events even when sys_smp_allowed spawns no worker at all.
    Sys_CreateEvent(true, false, &s_workerCmdEvent);
    Sys_CreateEvent(true, true, &s_updateSpotLightEffectEvent);
    Sys_CreateEvent(true, true, &s_updateEffectsEvent);
    Switch_CreateRendererEvents();
    Sys_CreateThread(function, THREAD_CONTEXT_BACKEND);
    return 1;
}

char __cdecl Sys_SpawnDatabaseThread(void (__cdecl *function)(uint32_t))
{
    Sys_CreateThread(function, THREAD_CONTEXT_DATABASE);
    return 1;
}

// the server and save-history spawns live in switch_thread_sync.cpp next
// to their events. Renderer workers and the main thread share the manual-reset
// worker command event created in Sys_SpawnRenderThread (retail backendEvent[0]);
// the 1 ms timed wait also lets each waiter re-check the queues.

namespace
{
uint64_t ProcessCoreMask()
{
#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
    const unsigned count = std::thread::hardware_concurrency();
    return count >= 64 ? ~0ull : ((1ull << (count ? count : 1)) - 1);
#else
    uint64_t mask = 0;
    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || mask == 0)
        Fail();
    return mask;
#endif
}

// Horizon does not migrate threads: libnx's -2 ("default core") pins a thread
// to the process's default core, which is where the main thread runs.  Worker
// N goes on the Nth core of the process mask other than the caller's (the
// main thread's, R_InitWorkerThreads asserts it), so worker0/worker1 land on
// cores 1/2 of an application's 0-2 mask.  A worker with no core of its own
// keeps the default core; R_RegisterWorkerThreadDvar leaves that one disabled
// (never resumed) because Sys_GetCpuCount counts the same mask.
int WorkerCpuId(uint32_t threadIndex)
{
#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
    (void)threadIndex;
    return kThreadCpuId;
#else
    const uint64_t mask = ProcessCoreMask() & ~(1ull << svcGetCurrentProcessorNumber());
    uint32_t seen = 0;
    for (int core = 0; core < 64; ++core)
    {
        if (!(mask & (1ull << core)))
            continue;
        if (seen++ == threadIndex)
            return core;
    }
    return kThreadCpuId;
#endif
}
}

// threads.cpp's Sys_GetCpuCount reported the logical processor count; here it
// is the number of cores this process may schedule on (0-2 for an
// application), so sys_smp_allowed is on and one front-end worker runs
// (R_RegisterWorkerThreadDvar enables worker N while N + 2 < cpus).
// An application's mask is cores 0-2: main on 0, renderer worker0 on 1
// (WorkerCpuId(0)), so the server takes core 2, which it shares only with the
// audio mixer (0x20) and the sound stream thread (0x2A).  Both outrank the
// server's kThreadPriority (0x3B), so audio keeps preempting it, and nothing
// of lower priority lives on that core for the server to starve.  Worker1
// would also map to core 2 but stays disabled on a 3-core mask
// (R_RegisterWorkerThreadDvar enables worker N while N + 2 < cpus).
int Switch_ServerThreadCpuId()
{
#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
    return kThreadCpuId;
#else
    const uint64_t mask = ProcessCoreMask() & ~(1ull << svcGetCurrentProcessorNumber());
    for (int core = 63; core >= 0; --core)
    {
        if (mask & (1ull << core))
            return core;
    }
    return kThreadCpuId;
#endif
}

size_t Switch_ServerStackUsed()
{
    const uintptr_t lo = s_serverStackLo.load(std::memory_order_acquire);
    const uintptr_t hi = s_serverStackHi.load(std::memory_order_relaxed);
    if (!lo)
        return 0;
    const uint64_t *p = reinterpret_cast<const uint64_t *>(lo);
    while (reinterpret_cast<uintptr_t>(p) < hi && *p == kStackPattern)
        ++p;
    return hi - reinterpret_cast<uintptr_t>(p);
}

uint32_t Switch_ProcessCoreCount()
{
    return static_cast<uint32_t>(__builtin_popcountll(ProcessCoreMask()));
}

uint32_t __cdecl Sys_GetCpuCount()
{
    return Switch_ProcessCoreCount();
}

namespace
{
// Core each spawned frame worker is pinned to (-1: not spawned).
std::atomic<int> s_workerCpuId[3] = {{-1}, {-1}, {-1}};
} // namespace

bool __cdecl Sys_SpawnWorkerThread(void (__cdecl *function)(uint32_t), uint32_t threadIndex)
{
    if (threadIndex > 2)
        Fail();
    const ThreadContext_t threadContext =
        static_cast<ThreadContext_t>(THREAD_CONTEXT_WORKER0 + threadIndex);
    if (threadHandle[threadContext] != nullptr)
        Fail();
    if (s_workerCmdEvent == nullptr)
        Fail();
    // Created suspended like threads.cpp's CreateThread(CREATE_SUSPENDED):
    // R_InitHardware/R_UpdateActiveWorkerThreads resume the enabled ones.
    const int cpuId = WorkerCpuId(threadIndex);
    CreateThreadOn(function, threadContext, cpuId, false);
    if (threadHandle[threadContext] != nullptr)
        s_workerCpuId[threadIndex].store(cpuId, std::memory_order_release);
    return threadHandle[threadContext] != nullptr;
}

int Switch_WorkerThreadCpuId(uint32_t threadIndex)
{
    if (threadIndex >= 3)
        return -1;
    const int cpuId = s_workerCpuId[threadIndex].load(std::memory_order_acquire);
    return cpuId >= 0 ? cpuId : -1;
}

void __cdecl Sys_WaitForWorkerCmd()
{
    if (s_workerCmdEvent == nullptr)
        Fail();
    Sys_WaitForSingleObjectTimeout(&s_workerCmdEvent, 1);
}

void __cdecl Sys_SetWorkerCmdEvent()
{
    if (s_workerCmdEvent == nullptr)
        Fail();
    Sys_SetEvent(&s_workerCmdEvent);
}

void __cdecl Sys_ResetWorkerCmdEvent()
{
    if (s_workerCmdEvent == nullptr)
        Fail();
    Sys_ResetEvent(&s_workerCmdEvent);
}

// threads.cpp:559-583.  The renderer's FX pipeline resets these before queuing
// the spot-light / non-dependent updates and the workers set them when done;
// R_ProcessCmd_UpdateFxRemaining waits for the non-dependent pass.
void __cdecl Sys_SetUpdateSpotLightEffectEvent()
{
    if (s_updateSpotLightEffectEvent == nullptr)
        Fail();
    Sys_SetEvent(&s_updateSpotLightEffectEvent);
}

void __cdecl Sys_ResetUpdateSpotLightEffectEvent()
{
    if (s_updateSpotLightEffectEvent == nullptr)
        Fail();
    Sys_ResetEvent(&s_updateSpotLightEffectEvent);
}

void __cdecl Sys_WaitUpdateNonDependentEffectsCompleted()
{
    if (s_updateEffectsEvent == nullptr)
        Fail();
    Sys_WaitForSingleObject(&s_updateEffectsEvent);
}

void __cdecl Sys_SetUpdateNonDependentEffectsEvent()
{
    if (s_updateEffectsEvent == nullptr)
        Fail();
    Sys_SetEvent(&s_updateEffectsEvent);
}

void __cdecl Sys_ResetUpdateNonDependentEffectsEvent()
{
    if (s_updateEffectsEvent == nullptr)
        Fail();
    Sys_ResetEvent(&s_updateEffectsEvent);
}

// spin_pause.h.  64 yield hints cover an owner finishing on another core; after
// that each iteration sleeps 1 us, which is what lets a lower-priority owner
// sharing this core (Horizon never time-slices it in) run and publish.
void Sys_SpinPause(uint32_t spin)
{
    if (spin < 64)
    {
#if defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
        return;
    }
#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
    std::this_thread::sleep_for(std::chrono::microseconds(1));
#else
    svcSleepThread(1000);
#endif
}
