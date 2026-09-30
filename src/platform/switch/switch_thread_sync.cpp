// the Switch owner for the engine-facing half of retail
// qcommon/threads.cpp -- the events the engine blocks on and wakes, plus the
// spawns that create them.  qcommon/threads.cpp itself is excluded from this
// link (scripts/sp/CMakeLists.txt): it is the Win32 CreateThread/CreateEvent/
// SEH backend.  switch_thread.cpp owns the other half of the same retail file
// (Sys_CreateThread itself, thread ids/handles, the APM load priorities,
// pause/resume); this file owns what a reader looking for `Sys_WakeServer`
// needs, next to the retail bodies in src/qcommon/threads.cpp:585-944.
//
// Everything here is a faithful port, *including the timeouts*: retail's
// Sys_WaitServer and Sys_WaitServerSnapshot are 1 ms probes whose return value
// the caller acts on (SV_WaitServerSnapshot loops on it, sv_main.cpp:775), and
// Sys_CanSendClientMessages is a 0 ms try-wait.  The abandoned SMP experiment
// (416ee29b) is the durable evidence for why that matters: turning one of
// these probes into a blocking wait with no signaller hung boot at
// "R_InitHardware: syncing database assets" because the waiter never re-tested
// its predicate.  Keep the timeouts.
//
// Host proof: `./test host`'s server-thread proof compiles this file with
// KISAK_SWITCH_THREAD_PROOF_HOST/KISAK_SWITCH_EVENT_PROOF_HOST and supplies
// Sys_Milliseconds/com_timescaleValue doubles, so the timeout arithmetic runs
// on a controlled clock instead of wall time.
#include <platform/switch/switch_thread.h>
#include <universal/assertive.h>
#include <universal/critical_section.h>
#include <qcommon/threads.h>

#include <atomic>
#include <cstdint>

// Declared by hand, not #included, for the same reason switch_thread.cpp
// declares Com_InitThreadData/Profile_InitContext that way: the host proof for
// this file builds it against the portable headers only and supplies its own
// doubles, so dragging in q_shared.h's whole include universe for two symbols
// would make the proof heavier for no gain.  Both are defined in the game link
// -- com_timescaleValue in com_math.cpp, Sys_Milliseconds in switch_platform.c
// -- and C linkage is not optional: q_shared.h declares them inside an
// extern "C" block (q_shared.h:786-792) precisely because the definition is C.
extern "C"
{
extern float com_timescaleValue;
uint32_t __cdecl Sys_Milliseconds(void);
}

namespace
{
// --- Server thread (retail threads.cpp:32-40, 727-908) ---------------------
// Created by Sys_SpawnServerThread, exactly like retail: nothing creates them
// at start-up, and SV_ServerThread calls Sys_InitServerEvents itself
// (sv_main.cpp:521) before its loop.
void *wakeServerEvent;
void *serverCompletedEvent;
void *allowSendClientMessagesEvent;
void *serverSnapshotEvent;
void *clientMessageReceived;

// Absolute deadline the server thread is allowed to run to, or 0 for "no
// timeout".  Retail reads/writes it with InterlockedCompareExchange
// (threads.cpp:840); an atomic gives the same guarantee portably.
std::atomic<int> s_serverTimeout(0);

// Read by Sys_WaitStartServer below and written by Sys_DatabaseCompleted
// (stage 3): while a database load is initializing, a start-server wake must
// not be reported as a start.
int isDoingDatabaseInit;

// --- Save-history (demo) thread (retail threads.cpp:919-944) ---------------
void *g_saveHistoryEvent;
void *g_saveHistoryDoneEvent;
}

// --- Server thread --------------------------------------------------------

int Sys_WaitStartServer(uint32_t timeout)
{
    Sys_EnterCriticalSection(CRITSECT_START_SERVER);
    const bool signalled = Sys_WaitForSingleObjectTimeout(&wakeServerEvent, timeout);
    int result = signalled ? 1 : 0;
    if (isDoingDatabaseInit)
    {
        result = 0;
    }
    else if (signalled)
    {
        Sys_ResetEvent(&serverCompletedEvent);
    }
    Sys_LeaveCriticalSection(CRITSECT_START_SERVER);
    return result;
}

void Sys_InitServerEvents()
{
    Sys_ResetEvent(&wakeServerEvent);
    Sys_ResetEvent(&serverCompletedEvent);
    Sys_SetEvent(&allowSendClientMessagesEvent);
    Sys_ResetEvent(&serverSnapshotEvent);
    Sys_SetEvent(&clientMessageReceived);
    s_serverTimeout.store(0, std::memory_order_release);
}

void Sys_ClientMessageReceived()
{
    Sys_SetEvent(&clientMessageReceived);
}

void Sys_ClearClientMessage()
{
    Sys_ResetEvent(&clientMessageReceived);
}

int Sys_SpawnServerThread(void (*function)(uint32_t))
{
    // Retail creates all five unconditionally (threads.cpp:769-773); the
    // port's Sys_CreateEvent refuses a slot that already holds an event, so a
    // second spawn of the same context fails loudly instead of leaking the
    // first handle the way the Win32 original would.
    Sys_CreateEvent(true, false, &wakeServerEvent);
    Sys_CreateEvent(true, false, &serverCompletedEvent);
    Sys_CreateEvent(true, false, &allowSendClientMessagesEvent);
    Sys_CreateEvent(false, false, &serverSnapshotEvent);
    Sys_CreateEvent(true, true, &clientMessageReceived);

    // Sys_CreateThread starts the thread outright, so retail's
    // Sys_ResumeThread(THREAD_CONTEXT_SERVER) step (threads.cpp:780) has no
    // counterpart here: the thread is already running by the time this
    // returns.  It parks in SV_ServerThread's start-server wait until
    // SV_InitSnapshot / SV_WakeServer wakes it.
    Sys_CreateThread(function, THREAD_CONTEXT_SERVER);
    return threadHandle[THREAD_CONTEXT_SERVER] != nullptr;
}

void Sys_WaitClientMessageReceived()
{
    Sys_WaitForSingleObject(&clientMessageReceived);
}

void Sys_ServerSnapshotCompleted()
{
    Sys_SetEvent(&serverSnapshotEvent);
}

bool Sys_WaitServerSnapshot()
{
    return Sys_WaitForSingleObjectTimeout(&serverSnapshotEvent, 1);
}

void Sys_AllowSendClientMessages()
{
    Sys_SetEvent(&allowSendClientMessagesEvent);
}

void Sys_DisallowSendClientMessages()
{
    Sys_ResetEvent(&allowSendClientMessagesEvent);
}

int Sys_CanSendClientMessages()
{
    return Sys_WaitForSingleObjectTimeout(&allowSendClientMessagesEvent, 0) ? 1 : 0;
}

void Sys_ServerCompleted()
{
    Sys_SetEvent(&serverCompletedEvent);
}

int Sys_ServerTimeout()
{
    const int timeout = s_serverTimeout.load(std::memory_order_acquire);
    if (!timeout)
        return 1;

    const int now = static_cast<int>(Sys_Milliseconds());
    if (now - timeout >= 0)
    {
        const int nextTimeout = now - static_cast<int>(-50.0f / com_timescaleValue);

        // Retail's compare-exchange loop: extend the deadline unless another
        // thread changed it first.
        while (true)
        {
            const int current = s_serverTimeout.load(std::memory_order_acquire);
            if (current != timeout)
                break;

            int expected = current;
            if (s_serverTimeout.compare_exchange_strong(expected, nextTimeout))
                return 1;
        }
        return 1;
    }
    return 0;
}

void Sys_WakeServer()
{
    Sys_SetEvent(&wakeServerEvent);
}

bool Sys_WaitServer()
{
    return Sys_WaitForSingleObjectTimeout(&serverCompletedEvent, 1);
}

// Retail threads.cpp:863-876: a 0 ms try-wait plus a reset, never a wait on
// the server.  A wake the server has not taken yet is withdrawn, which is the
// point: SV_WaitServer only needs the frame in flight to finish
// (serverCompletedEvent), not a new one to start.  A variant that
// waited for the server to acknowledge the wake deadlocked main behind the
// send gate only main opens (SV_ServerThread's "wait send msg").
// switch_thread_test.cpp's SWITCH_SERVER_SLEEP_NONBLOCKING step pins this.
void Sys_SleepServer()
{
    if (Sys_WaitForSingleObjectTimeout(&wakeServerEvent, 0))
    {
        Sys_EnterCriticalSection(CRITSECT_START_SERVER);
        Sys_ResetEvent(&wakeServerEvent);
        Sys_LeaveCriticalSection(CRITSECT_START_SERVER);
    }
}

void Sys_SetServerTimeout(int timeout)
{
    iassert(timeout >= 0);
    iassert(com_timescaleValue);

    if (timeout)
    {
        const int value = static_cast<int>(static_cast<float>(timeout) / com_timescaleValue);
        const int now = static_cast<int>(Sys_Milliseconds());
        const int current = s_serverTimeout.load(std::memory_order_acquire);
        if (current && now - current < 0 && current - (now + value) <= 0)
        {
            // A later deadline is already armed; keep it.
        }
        else
        {
            s_serverTimeout.store(now + value, std::memory_order_release);
        }
    }
    else
    {
        s_serverTimeout.store(0, std::memory_order_release);
    }
}

void Sys_SuspendOtherThreads()
{
#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
    // The host shim cannot suspend a std::thread (see Sys_SuspendThread), and
    // this is not reachable in the SP game anyway: its only callers are
    // CL_startMultiplayer_f, which asserts out first (cl_main.cpp:1101), and
    // win_main.cpp, which is Windows-only.  The real loop is compiled for
    // Switch and exercised by the aarch64 proof.
#else
    const uint32_t currentThreadId = Sys_GetCurrentThreadId();
    for (int threadIndex = 0; threadIndex < THREAD_CONTEXT_COUNT; ++threadIndex)
    {
        if (threadHandle[threadIndex] && threadId[threadIndex] &&
            threadId[threadIndex] != currentThreadId)
        {
            Sys_SuspendThread(static_cast<ThreadContext_t>(threadIndex));
        }
    }
#endif
}

// --- Save-history (demo) thread -------------------------------------------

int Sys_SpawnServerDemoThread(void (*function)(uint32_t))
{
    Sys_CreateEvent(false, false, &g_saveHistoryEvent);
    Sys_CreateEvent(false, false, &g_saveHistoryDoneEvent);

    // Same as the server spawn above: no Sys_ResumeThread step, the thread
    // starts running inside Sys_CreateThread and immediately blocks in
    // SV_SaveHistoryLoop's Sys_WaitForSaveHistory.
    Sys_CreateThread(function, THREAD_CONTEXT_SERVER_DEMO);
    return threadHandle[THREAD_CONTEXT_SERVER_DEMO] != nullptr;
}

void Sys_SetSaveHistoryEvent()
{
    Sys_SetEvent(&g_saveHistoryEvent);
}

void Sys_WaitForSaveHistory()
{
    Sys_WaitForSingleObject(&g_saveHistoryEvent);
}

void Sys_SetSaveHistoryDoneEvent()
{
    Sys_SetEvent(&g_saveHistoryDoneEvent);
}

bool Sys_WaitForSaveHistoryDone()
{
    return Sys_WaitForSingleObjectTimeout(&g_saveHistoryDoneEvent, 2000);
}

// --- Renderer back-end handshake (retail threads.cpp:59-65, 110-150, 339-554)
// Retail's own events, flags and bodies.  The port previously replaced them
// with polled atomics and dropped Sys_FrontEndSleep's wait on
// rendererRunningEvent: main could release ownership, wake the renderer and
// take ownership back before the back end's idle poll (RB_RenderThreadIdle)
// ever saw the release, leaving the back end idle and main waiting for a pause
// that never came.  The wait on rendererRunningEvent is what forbids that: main
// cannot reclaim ownership until the back end has left its idle loop.
// switch_thread_test.cpp's RenderHandshakeCheck pins that schedule.
namespace
{
void *renderPausedEvent;
void *renderCompletedEvent;
void *noThreadOwnershipEvent;
void *rendererRunningEvent;
void *backendGenericEvent; // retail backendEvent[BACKEND_EVENT_GENERIC]
std::atomic<void *> smpData(nullptr);
std::atomic<int32_t> renderPausedCount(0);
}

// Sys_SpawnRenderThread's half of threads.cpp:145-149 (switch_thread.cpp owns
// the worker/FX events it creates next to them).
void Switch_CreateRendererEvents()
{
    Sys_CreateEvent(false, false, &renderPausedEvent);
    Sys_CreateEvent(true, true, &renderCompletedEvent);
    Sys_CreateEvent(true, false, &noThreadOwnershipEvent);
    Sys_CreateEvent(true, true, &rendererRunningEvent);
    Sys_CreateEvent(false, false, &backendGenericEvent);
}

int __cdecl Sys_IsRendererReady()
{
    return Sys_WaitForSingleObjectTimeout(&renderCompletedEvent, 0);
}

void *__cdecl Sys_RendererSleep()
{
    return smpData.exchange(nullptr, std::memory_order_acq_rel);
}

int __cdecl Sys_RendererReady()
{
    return smpData.load(std::memory_order_acquire) != nullptr;
}

void __cdecl Sys_RenderCompleted()
{
    Sys_SetEvent(&renderCompletedEvent);
    Sys_SetWorkerCmdEvent();
}

void __cdecl Sys_FrontEndSleep()
{
    iassert(Sys_WaitForSingleObjectTimeout(&noThreadOwnershipEvent, 0));
    Sys_WaitForSingleObject(&rendererRunningEvent);
    Sys_ResetEvent(&noThreadOwnershipEvent);
    Sys_SetEvent(&backendGenericEvent);
    const int32_t newCount = renderPausedCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    vassert(newCount == -1 || newCount == 0, "(newCount) = %i", newCount);
    (void)newCount;
    Sys_WaitForSingleObject(&renderPausedEvent);
}

void __cdecl Sys_WakeRenderer(void *data)
{
    Sys_ResetEvent(&renderCompletedEvent);
    void *old = smpData.exchange(data, std::memory_order_acq_rel);
    vassert(!old, "old = %p", old);
    (void)old;
    Sys_SetEvent(&backendGenericEvent);
    Sys_SetWorkerCmdEvent();
}

void __cdecl Sys_NotifyRenderer()
{
    iassert(backendGenericEvent);
    Sys_SetEvent(&backendGenericEvent);
}

bool __cdecl Sys_FinishRenderer()
{
    return !Sys_WaitForSingleObjectTimeout(&noThreadOwnershipEvent, 0);
}

int __cdecl Sys_IsMainThreadReady()
{
    return Sys_WaitForSingleObjectTimeout(&noThreadOwnershipEvent, 0);
}

void __cdecl Sys_WaitForMainThread()
{
    Sys_WaitForSingleObject(&noThreadOwnershipEvent);
}

void __cdecl Sys_ReleaseThreadOwnership()
{
    iassert(!Sys_WaitForSingleObjectTimeout(&noThreadOwnershipEvent, 0));
    Sys_SetEvent(&noThreadOwnershipEvent);
}

void __cdecl Sys_StopRenderer()
{
    const int32_t newCount = renderPausedCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    vassert(newCount == 0 || newCount == 1, "(newCount) = %i", newCount);
    (void)newCount;
    Sys_ResetEvent(&rendererRunningEvent);
    Sys_SetEvent(&renderPausedEvent);
}

void __cdecl Sys_StartRenderer()
{
    Sys_SetEvent(&rendererRunningEvent);
}

int __cdecl Sys_WaitBackendEvent()
{
    return Sys_WaitForSingleObjectTimeout(&backendGenericEvent, 0);
}
