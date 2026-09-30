#include "src/platform/switch/switch_thread.h"
// critical_section.h before threads.h: it is what defines __int32 for
// threads.h's enums on GCC (the engine's TUs get it from the same header
// being pulled in earlier by their own include order).
#include "src/universal/critical_section.h"
#include "src/qcommon/threads.h"
#include "src/platform/switch/switch_event.h"

#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
extern "C"
{
#include <switch/kernel/svc.h>
#include <switch/kernel/thread.h>
}
#endif

void __cdecl Sys_CreateEvent(bool manualReset, bool initialState, void **event);
void __cdecl Sys_ResetEvent(void **event);
void __cdecl Sys_SetEvent(void **event);
void __cdecl Sys_WaitForSingleObject(void **event);
bool __cdecl Sys_WaitForSingleObjectTimeout(void **event, uint32_t msec);

// The two engine symbols switch_thread_sync.cpp's ported bodies need, provided
// here as doubles.  The clock is controllable on purpose: the server-timeout
// arithmetic (qcommon/threads.cpp:818-908) is deadline based, so a wall clock
// would make this proof slow and flaky.  C linkage on Sys_Milliseconds to
// match q_shared.h's extern "C" declaration (the real definition is the C
// platform layer).
std::atomic<uint32_t> s_proof_ms(0);
float com_timescaleValue = 1.0f;

extern "C" uint32_t Sys_Milliseconds(void)
{
    return s_proof_ms.load(std::memory_order_acquire);
}

void Sys_Sleep(uint32_t)
{
    std::this_thread::yield();
}

namespace
{
std::jmp_buf s_failure_jump;
unsigned int s_failure_count;
std::atomic<int> s_init_count_worker0(0);
std::atomic<int> s_init_count_worker1(0);
std::atomic<bool> s_worker0_done(false);
std::atomic<bool> s_worker0_init_before_run(false);
std::atomic<bool> s_worker0_is_main_thread(true);
std::atomic<uint32_t> s_worker0_thread_id(0);
std::atomic<bool> s_worker1_ran(false);
std::atomic<uint32_t> s_worker1_thread_id(0);

void *s_started_event;
void *s_release_event;

// Real pause/resume coverage (aarch64 arm only, see Sys_SuspendThread).
#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
std::atomic<uint64_t> s_pause_counter(0);
void *s_pause_stop_event;
#endif

// Event-semantics section for switch_event.cpp's owner.
void *s_manual_event;
void *s_auto_event;
void *s_pre_signalled_event;

void TestFailureHook(void *)
{
    ++s_failure_count;
    std::longjmp(s_failure_jump, 1);
}

bool ExpectFailure(void (*operation)())
{
    if (setjmp(s_failure_jump) == 0)
    {
        operation();
        return false;
    }
    return true;
}

void __cdecl Worker0Thread(uint32_t)
{
    s_worker0_init_before_run.store(
        s_init_count_worker0.load(std::memory_order_acquire) != 0, std::memory_order_release);
    s_worker0_thread_id.store(Sys_GetCurrentThreadId(), std::memory_order_release);
    s_worker0_is_main_thread.store(Sys_IsMainThread(), std::memory_order_release);
    Sys_SetEvent(&s_started_event);
    for (int attempt = 0; attempt < 1000; ++attempt)
    {
        if (Sys_WaitForSingleObjectTimeout(&s_release_event, 10))
            break;
    }
    s_worker0_done.store(true, std::memory_order_release);
}

void *s_fx_go_event;
std::atomic<bool> s_fx_backend_set(false);

// Stands in for the render/worker side of the FX pipeline: it completes the
// non-dependent pass only after main has reset the event and released it.
// --- Renderer back-end handshake (retail threads.cpp:339-554) -------------
// The back-end half mirrors RB_RenderThread (rb_backend.cpp) with the smp
// branch of RB_RenderThreadIdle; the main half mirrors R_ToggleSmpFrameCmd
// (release, wait for the previous frame, wake) followed by the next
// R_SyncRenderThread (Sys_FrontEndSleep).  s_idle_poll_delay_ms stands in for
// the back end being descheduled inside its idle poll: with it, main releases,
// wakes and reclaims ownership before the back end ever looks.  The port's
// earlier polled-atomic handshake deadlocked on exactly that schedule (main
// waiting for a pause, back end idle having never seen the release).
std::atomic<bool> s_render_quit(false);
std::atomic<int> s_idle_poll_delay_ms(0);
std::atomic<bool> s_main_owns_renderer(true);
std::atomic<bool> s_backend_rendering(false);
std::atomic<int> s_render_frames(0);
std::atomic<int> s_render_overlaps(0);
std::atomic<int> s_backend_idle_entries(0);

void ProofRenderFrame(void *data)
{
    s_backend_rendering.store(true, std::memory_order_seq_cst);
    if (s_main_owns_renderer.load(std::memory_order_seq_cst) || data == nullptr)
        s_render_overlaps.fetch_add(1, std::memory_order_acq_rel);
    std::this_thread::yield();
    s_render_frames.fetch_add(1, std::memory_order_acq_rel);
    s_backend_rendering.store(false, std::memory_order_seq_cst);
    Sys_RenderCompleted();
}

// R_ProcessWorkerCmdsWithTimeout(timeout, 1) with no worker commands queued.
bool ProofWaitFor(int (*timeout)())
{
    while (!timeout())
    {
        if (s_render_quit.load(std::memory_order_acquire))
            return false;
        Sys_WaitForWorkerCmd();
    }
    return true;
}

void RenderProofBackend()
{
    void *data = nullptr;
    while (!s_render_quit.load(std::memory_order_acquire))
    {
        while (true)
        {
            if (!ProofWaitFor(Sys_WaitBackendEvent))
                return;
            if (Sys_FinishRenderer())
            {
                data = Sys_RendererSleep();
                if (data)
                    ProofRenderFrame(data);
                Sys_StopRenderer();
                s_backend_idle_entries.fetch_add(1, std::memory_order_acq_rel);
                // RB_RenderThreadIdle, smp branch.
                while (!Sys_IsMainThreadReady())
                {
                    if (s_render_quit.load(std::memory_order_acquire))
                        return;
                    const int delay = s_idle_poll_delay_ms.load(std::memory_order_acquire);
                    if (delay)
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    else
                        Sys_WaitForWorkerCmd();
                }
                Sys_StartRenderer();
            }
            if (!data)
                break;
            data = nullptr;
        }
        data = Sys_RendererSleep();
        if (data)
        {
            ProofRenderFrame(data);
            data = nullptr;
        }
    }
}

void __cdecl FxBackendThread(uint32_t)
{
    Sys_WaitForSingleObject(&s_fx_go_event);
    s_fx_backend_set.store(true, std::memory_order_release);
    Sys_SetUpdateNonDependentEffectsEvent();
    Sys_SetUpdateSpotLightEffectEvent();
    RenderProofBackend();
}

int s_proof_frame_data[2];

// One main-thread frame: R_ToggleSmpFrameCmd, then the next frame's
// R_SyncRenderThread.
void ProofMainFrame(int frame)
{
    s_main_owns_renderer.store(false, std::memory_order_seq_cst);
    Sys_ReleaseThreadOwnership();
    ProofWaitFor(Sys_IsRendererReady);
    Sys_WakeRenderer(&s_proof_frame_data[frame & 1]);
    Sys_FrontEndSleep();
    s_main_owns_renderer.store(true, std::memory_order_seq_cst);
    if (s_backend_rendering.load(std::memory_order_seq_cst))
        s_render_overlaps.fetch_add(1, std::memory_order_acq_rel);
}

bool RenderHandshakeCheck()
{
    std::atomic<bool> done(false);
    std::thread watchdog([&done]() {
        for (int waited = 0; waited < 20000; waited += 10)
        {
            if (done.load(std::memory_order_acquire))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::fprintf(stderr, "FAIL:SWITCH_RENDER_HANDSHAKE deadlock frames=%d idle=%d\n",
                     s_render_frames.load(), s_backend_idle_entries.load());
        std::_Exit(1);
    });

    // Late idle poll: main reclaims before the back end has seen the release.
    s_idle_poll_delay_ms.store(20, std::memory_order_release);
    int frame = 0;
    for (; frame < 5; ++frame)
        ProofMainFrame(frame);
    // Unforced schedules.
    s_idle_poll_delay_ms.store(0, std::memory_order_release);
    for (; frame < 2000; ++frame)
        ProofMainFrame(frame);

    done.store(true, std::memory_order_release);
    watchdog.join();

    s_render_quit.store(true, std::memory_order_release);
    Sys_ReleaseThreadOwnership();
    Sys_NotifyRenderer();
    Sys_SetWorkerCmdEvent();
    Switch_ThreadWaitForExit(THREAD_CONTEXT_BACKEND);
    Sys_ResetWorkerCmdEvent();

    const int frames = s_render_frames.load(std::memory_order_acquire);
    const int overlaps = s_render_overlaps.load(std::memory_order_acquire);
    if (frames != frame || overlaps != 0)
    {
        std::fprintf(stderr, "FAIL:SWITCH_RENDER_HANDSHAKE frames=%d/%d overlaps=%d\n", frames, frame, overlaps);
        return false;
    }
    std::fprintf(stderr, "PASS:SWITCH_RENDER_HANDSHAKE frames=%d late_idle_poll=5 overlaps=0\n", frames);
    return true;
}

// threads.cpp:151-152/559-583: Sys_SpawnRenderThread creates both FX events
// manual-reset and set; the non-dependent wait passes immediately until reset
// and then blocks until the other thread sets it again.
bool FxEventsCheck()
{
    Sys_CreateEvent(true, false, &s_fx_go_event);
    if (!Sys_SpawnRenderThread(FxBackendThread))
        return false;
    Sys_WaitUpdateNonDependentEffectsCompleted();
    Sys_WaitUpdateNonDependentEffectsCompleted();
    Sys_ResetUpdateSpotLightEffectEvent();
    Sys_ResetUpdateNonDependentEffectsEvent();
    Sys_SetEvent(&s_fx_go_event);
    Sys_WaitUpdateNonDependentEffectsCompleted();
    if (!s_fx_backend_set.load(std::memory_order_acquire))
        return false;
    // The back end stays up for RenderHandshakeCheck, which joins it.
    return true;
}

void __cdecl Worker1Thread(uint32_t)
{
    s_worker1_thread_id.store(Sys_GetCurrentThreadId(), std::memory_order_release);
    s_worker1_ran.store(true, std::memory_order_release);
}

void CreateDuplicateWorker0()
{
    Sys_CreateThread(Worker0Thread, THREAD_CONTEXT_WORKER0);
}

void CreateMainThreadAgain()
{
    Sys_CreateThread(Worker0Thread, THREAD_CONTEXT_MAIN);
}

void CreateOutOfRangeThread()
{
    Sys_CreateThread(Worker0Thread, static_cast<ThreadContext_t>(THREAD_CONTEXT_COUNT));
}

#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
// Host-arm-only negative controls: on aarch64 a live-context suspend/resume
// succeeds (see the real pause/resume section in main), so these two exist to
// pin the host shim's documented trap.
void SuspendWorker0()
{
    Sys_SuspendThread(THREAD_CONTEXT_WORKER0);
}

void ResumeWorker0()
{
    Sys_ResumeThread(THREAD_CONTEXT_WORKER0);
}
#endif

// TITLE_SERVER, not SERVER: the server-thread proof below spawns a real
// THREAD_CONTEXT_SERVER, so the negative control has to name a context this
// test never spawns.
void WaitOnUncreatedThread()
{
    Switch_ThreadWaitForExit(THREAD_CONTEXT_TITLE_SERVER);
}

void SuspendUncreatedThread()
{
    Sys_SuspendThread(THREAD_CONTEXT_TITLE_SERVER);
}

void ResumeUncreatedThread()
{
    Sys_ResumeThread(THREAD_CONTEXT_TITLE_SERVER);
}

void WaitOutOfRange()
{
    Switch_ThreadWaitForExit(static_cast<ThreadContext_t>(-1));
}

#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
void __cdecl PauseProofThread(uint32_t)
{
    while (!Sys_WaitForSingleObjectTimeout(&s_pause_stop_event, 0))
        s_pause_counter.fetch_add(1, std::memory_order_relaxed);
}
#endif

void EndLoadPrioritiesWithoutBegin()
{
    Sys_EndLoadThreadPriorities();
}

void BeginLoadPrioritiesTwice()
{
    Sys_BeginLoadThreadPriorities();
    Sys_BeginLoadThreadPriorities();
}
}

// --- server-thread handshake ------------------------------------------
// The same exchange SV_ServerThread performs (sv_main.cpp:522-600), in order,
// against the real ported event surface -- with no engine linked.  Each step
// hands control back to main so the assertions describe a defined state rather
// than a race: the thread advances an ack counter and then blocks, main waits
// for the counter, asserts, and releases it.
std::atomic<int> s_server_ack(0);
std::atomic<bool> s_server_context_ok(false);
std::atomic<int> s_server_park_result(-1);
std::atomic<int> s_server_wake_result(-1);
std::atomic<bool> s_server_ran_frame(false);
std::atomic<bool> s_server_client_message_woken(false);
std::atomic<bool> s_server_passed_send_gate(false);
void *s_to_server;
void *s_never_signalled;

void ServerAck()
{
    s_server_ack.fetch_add(1, std::memory_order_release);
}

bool WaitForServerAck(int step)
{
    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        if (s_server_ack.load(std::memory_order_acquire) >= step)
            return true;
        Sys_WaitForSingleObjectTimeout(&s_never_signalled, 1);
    }
    return false;
}

void ReleaseServer()
{
    // Auto-reset: each release lets exactly one of the thread's waits through,
    // and a release made before the thread reaches that wait is retained.
    Sys_SetEvent(&s_to_server);
}

void __cdecl ServerProofThread(uint32_t threadContext)
{
    s_server_context_ok.store(threadContext == THREAD_CONTEXT_SERVER, std::memory_order_release);
    Sys_InitServerEvents();

    // Publish the parked state, then deliberately delay the first poll.  Main
    // performs Wake -> Sleep during this window.  Retail's Sys_SleepServer
    // (threads.cpp:863-876) is a 0 ms try-wait plus a reset: it withdraws a
    // wake the server has not taken yet and never waits for the server.  The
    // poll must therefore see no wake.
    Sys_ServerCompleted();
    ServerAck();
    Sys_WaitForSingleObjectTimeout(&s_never_signalled, 20);
    s_server_park_result.store(Sys_WaitStartServer(0), std::memory_order_release);
    ServerAck();
    Sys_WaitForSingleObject(&s_to_server);

    // Deadlock shape: the server took the wake and ran its frame, and is
    // now parked in SV_ServerThread's "wait send msg" poll
    // (R_ProcessWorkerCmdsWithTimeout(Sys_CanSendClientMessages, 1)) with
    // sending disallowed.  Main woke it again meanwhile (SV_FrameInternal) and
    // now calls SV_WaitServer -> Sys_SleepServer (a Cbuf server command,
    // Com_SyncThreads, a fullscreen menu).  Only main can allow sending, so
    // Sys_SleepServer must return without the server's co-operation.
    s_server_park_result.store(Sys_WaitStartServer(0), std::memory_order_release);
    Sys_ServerCompleted();
    ServerAck();
    while (!Sys_CanSendClientMessages())
        Sys_WaitForSingleObjectTimeout(&s_never_signalled, 1);
    s_server_passed_send_gate.store(true, std::memory_order_release);
    Sys_WaitStartServer(0);
    ServerAck();
    Sys_WaitForSingleObject(&s_to_server);

    // Main woke us: the frame runs here, then the engine's post-frame exchange.
    s_server_wake_result.store(Sys_WaitStartServer(0), std::memory_order_release);
    s_server_ran_frame.store(true, std::memory_order_release);
    Sys_ServerCompleted();
    ServerAck();
    Sys_WaitForSingleObject(&s_to_server);

    // Clear the client-message flag and then block on it: the wake can only
    // come from main's Sys_ClientMessageReceived, so the thread is released by
    // the event rather than by a poll.
    Sys_ClearClientMessage();
    ServerAck();
    Sys_WaitClientMessageReceived();
    s_server_client_message_woken.store(true, std::memory_order_release);
    Sys_ServerSnapshotCompleted();
    ServerAck();
    Sys_WaitForSingleObject(&s_to_server);
}

// A failed step exits at once with its line: the server thread may be
// parked on an event nobody will set, and joining it at exit would hang.
#define PROOF_FAIL() \
    do { std::fprintf(stderr, "FAIL:SWITCH_SERVER_HANDSHAKE line=%d\n", __LINE__); std::_Exit(1); } while (0)

bool ServerHandshakeCheck()
{
    const uint32_t main_thread_id = Sys_GetCurrentThreadId();

    // Sys_WaitStartServer/Sys_SleepServer take CRITSECT_START_SERVER, and the
    // critical-section owner refuses an entry before this one-time init.
    Sys_InitializeCriticalSections();

    Sys_CreateEvent(false, false, &s_to_server);
    Sys_CreateEvent(true, false, &s_never_signalled);

    if (Sys_SpawnServerThread(ServerProofThread) != 1)
        PROOF_FAIL();
    if (threadHandle[THREAD_CONTEXT_SERVER] == nullptr)
        PROOF_FAIL();
    if (threadId[THREAD_CONTEXT_SERVER] == 0 || threadId[THREAD_CONTEXT_SERVER] == main_thread_id)
        PROOF_FAIL();
    if (Sys_IsServerThread())
        PROOF_FAIL();

    // Step 1: the server has published its parked state but has not polled the
    // wake yet.  Wake followed immediately by Sleep withdraws the wake without
    // waiting for the server (retail Sys_SleepServer), so its poll sees none.
    if (!WaitForServerAck(1))
        PROOF_FAIL();
    Sys_WakeServer();
    Sys_SleepServer();
    if (!WaitForServerAck(2))
        PROOF_FAIL();
    if (s_server_park_result.load(std::memory_order_acquire) != 0)
        PROOF_FAIL();

    // Step 1b: the deadlock.  Wake the server, let it finish its frame and
    // park behind the disallowed send gate, wake it again (a wake it cannot
    // take until it passes that gate), then Sleep.  A watchdog opens the gate
    // after 2 s so a regression fails instead of hanging; the proof requires
    // Sleep to return before the server passed the gate.
    Sys_DisallowSendClientMessages();
    Sys_WakeServer();
    ReleaseServer();
    if (!WaitForServerAck(3))
        PROOF_FAIL();
    if (s_server_park_result.load(std::memory_order_acquire) != 1)
        PROOF_FAIL();
    Sys_WakeServer();
    std::atomic<bool> sleep_returned(false);
    std::thread watchdog([&sleep_returned]() {
        for (int waited = 0; waited < 2000 && !sleep_returned.load(std::memory_order_acquire); ++waited)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!sleep_returned.load(std::memory_order_acquire))
            Sys_AllowSendClientMessages();
    });
    Sys_SleepServer();
    const bool slept_without_server = !s_server_passed_send_gate.load(std::memory_order_acquire);
    sleep_returned.store(true, std::memory_order_release);
    watchdog.join();
    if (!slept_without_server)
    {
        std::fprintf(stderr, "FAIL:SWITCH_SERVER_SLEEP_NONBLOCKING Sys_SleepServer waited on the server's send gate\n");
        PROOF_FAIL();
    }
    std::fprintf(stderr, "PASS:SWITCH_SERVER_SLEEP_NONBLOCKING send_gate=closed sleep=returned\n");
    // The withdrawn wake stays withdrawn; open the gate and the server reaches
    // its start poll exactly as SV_ServerThread's post-frame does.
    Sys_AllowSendClientMessages();
    if (!WaitForServerAck(4))
        PROOF_FAIL();

    // Step 2: wake it.  The 1ms probe main uses in the SMP path (SV_WaitServer)
    // must observe the server's completion signal.
    Sys_WakeServer();
    ReleaseServer();
    if (!WaitForServerAck(5))
        PROOF_FAIL();
    if (s_server_wake_result.load(std::memory_order_acquire) != 1)
        PROOF_FAIL();
    if (!s_server_ran_frame.load(std::memory_order_acquire))
        PROOF_FAIL();
    if (!Sys_WaitServer())
        PROOF_FAIL();

    // Consuming the wake is Sys_SleepServer's job in the engine's SV_WaitServer.
    if (Sys_WaitStartServer(0) != 1)
        PROOF_FAIL();
    Sys_SleepServer();
    if (Sys_WaitStartServer(0) != 0)
        PROOF_FAIL();

    // The snapshot event is auto-reset: one taker, then clear again.
    Sys_ServerSnapshotCompleted();
    if (!Sys_WaitServerSnapshot())
        PROOF_FAIL();
    if (Sys_WaitServerSnapshot())
        PROOF_FAIL();

    // allow/disallow gates the engine's send path both ways.
    Sys_DisallowSendClientMessages();
    if (Sys_CanSendClientMessages() != 0)
        PROOF_FAIL();
    Sys_AllowSendClientMessages();
    if (Sys_CanSendClientMessages() != 1)
        PROOF_FAIL();

    // Step 3: release the thread's blocking client-message wait, and require
    // the snapshot it publishes afterwards.
    ReleaseServer();
    if (!WaitForServerAck(6))
        PROOF_FAIL();
    Sys_ClientMessageReceived();
    if (!WaitForServerAck(7))
        PROOF_FAIL();
    if (!s_server_client_message_woken.load(std::memory_order_acquire))
        PROOF_FAIL();

    // Server timeout arithmetic on the controlled clock.
    s_proof_ms.store(0, std::memory_order_release);
    Sys_SetServerTimeout(0);
    if (Sys_ServerTimeout() != 1)
        PROOF_FAIL(); // no deadline armed: the caller does not wait
    Sys_SetServerTimeout(50);
    if (Sys_ServerTimeout() != 0)
        PROOF_FAIL(); // not due yet
    s_proof_ms.store(60, std::memory_order_release);
    if (Sys_ServerTimeout() != 1)
        PROOF_FAIL(); // expired
    if (Sys_ServerTimeout() != 0)
        PROOF_FAIL(); // and re-armed 50ms past the expiry
    Sys_SetServerTimeout(0);

    // Step 4: let it finish and join.
    ReleaseServer();
    Switch_ThreadWaitForExit(THREAD_CONTEXT_SERVER);
    return s_server_context_ok.load(std::memory_order_acquire);
}

// --- save-history (demo) thread handshake -----------------------------
// SV_SaveHistoryLoop's shape (sv_demo.cpp:1167-1185) against the real events:
// block on the publish event, do the work, signal done.  The engine route the
// publish comes from (SV_DemoGetBuffer) is only reachable while a demo is
// playing or recording, which no gate in this repo drives -- hence this proof
// of the event pair itself.
std::atomic<bool> s_demo_ran_save(false);

void __cdecl SaveHistoryProofThread(uint32_t threadContext)
{
    if (threadContext != THREAD_CONTEXT_SERVER_DEMO)
        return;
    Sys_WaitForSaveHistory();
    s_demo_ran_save.store(true, std::memory_order_release);
    Sys_SetSaveHistoryDoneEvent();
}

bool SaveHistoryCheck()
{
    if (Sys_SpawnServerDemoThread(SaveHistoryProofThread) != 1)
        return false;
    if (threadHandle[THREAD_CONTEXT_SERVER_DEMO] == nullptr)
        return false;

    // Nothing published: the 2000ms wait has to expire and report failure
    // rather than block forever (sv_demo.cpp:365 depends on that answer).
    if (Sys_WaitForSaveHistoryDone())
        return false;

    Sys_SetSaveHistoryEvent();
    bool done = false;
    for (int attempt = 0; attempt < 400 && !done; ++attempt)
    {
        done = Sys_WaitForSaveHistoryDone();
        if (!done)
            Sys_WaitForSingleObjectTimeout(&s_never_signalled, 5);
    }
    if (!done)
        return false;
    if (!s_demo_ran_save.load(std::memory_order_acquire))
        return false;
    // Auto-reset: the completion above consumed the signal.
    if (Sys_WaitForSaveHistoryDone())
        return false;

    Switch_ThreadWaitForExit(THREAD_CONTEXT_SERVER_DEMO);
    return true;
}

void __cdecl Sys_InitThread(ThreadContext_t threadContext)
{
    if (threadContext == THREAD_CONTEXT_WORKER0)
        s_init_count_worker0.fetch_add(1, std::memory_order_acq_rel);
    else if (threadContext == THREAD_CONTEXT_WORKER1)
        s_init_count_worker1.fetch_add(1, std::memory_order_acq_rel);
}

int main()
{
    if (Sys_IsMainThread())
        return 1;
    const uint32_t main_thread_id = Sys_GetCurrentThreadId();
    if (Sys_GetCurrentThreadId() != main_thread_id)
        return 1;
    Sys_InitMainThread();
    if (!Sys_IsMainThread())
        return 1;
    if (Sys_GetCurrentThreadId() != main_thread_id)
        return 1;

    Sys_CreateEvent(true, false, &s_started_event);
    Sys_CreateEvent(true, false, &s_release_event);

    // switch_event.cpp's owner semantics, which every ported thread wait in
    // the port depends on.  Manual reset: a signal persists across any number of
    // waits (retail's wakeServerEvent/serverCompletedEvent/allowSend...).
    Sys_CreateEvent(true, false, &s_manual_event);
    Sys_CreateEvent(false, false, &s_auto_event);
    Sys_CreateEvent(true, true, &s_pre_signalled_event);
    if (Sys_WaitForSingleObjectTimeout(&s_manual_event, 0))
        return 1;
    Sys_SetEvent(&s_manual_event);
    if (!Sys_WaitForSingleObjectTimeout(&s_manual_event, 0) ||
        !Sys_WaitForSingleObjectTimeout(&s_manual_event, 0))
        return 1;
    Sys_ResetEvent(&s_manual_event);
    if (Sys_WaitForSingleObjectTimeout(&s_manual_event, 0))
        return 1;
    // Auto reset: the first wait consumes the signal (serverSnapshotEvent).
    Sys_SetEvent(&s_auto_event);
    if (!Sys_WaitForSingleObjectTimeout(&s_auto_event, 0) ||
        Sys_WaitForSingleObjectTimeout(&s_auto_event, 0))
        return 1;
    // Created signalled and waited on later (clientMessageReceived).
    if (!Sys_WaitForSingleObjectTimeout(&s_pre_signalled_event, 0))
        return 1;
    // A timeout on an unsignalled event returns false instead of blocking:
    // retail's Sys_WaitServer/Sys_WaitServerSnapshot are 1ms probes whose
    // return value the caller acts on.
    if (Sys_WaitForSingleObjectTimeout(&s_manual_event, 5))
        return 1;

    if (Sys_GetCpuCount() == 0 || Sys_GetCpuCount() != Switch_ProcessCoreCount())
        return 1;

    // R_InitRenderThread runs before R_InitWorkerThreads and unconditionally,
    // so its events must serve the main thread with no worker spawned: the
    // sys_smp_allowed=0 path resets/waits on the worker event every frame
    // (R_ProcessWorkerCmdsWithTimeout) and drives the FX events inline.
    if (!FxEventsCheck())
        return 1;
    if (!RenderHandshakeCheck())
        return 1;
    Sys_ResetWorkerCmdEvent();
    Sys_WaitForWorkerCmd(); // 1 ms timed wait, nothing signalled
    Sys_SetWorkerCmdEvent();
    Sys_WaitForWorkerCmd();
    Sys_WaitForWorkerCmd(); // manual reset (retail backendEvent[0]): stays set
    Sys_ResetWorkerCmdEvent();

    if (!Sys_SpawnWorkerThread(Worker0Thread, 0) ||
        !Sys_SpawnWorkerThread(Worker1Thread, 1))
        return 1;
    // Created suspended (threads.cpp CREATE_SUSPENDED): nothing runs until the
    // owner resumes it, and suspending a never-started worker is a no-op.
    if (Sys_WaitForSingleObjectTimeout(&s_started_event, 20) ||
        s_worker1_ran.load(std::memory_order_acquire))
        return 1;
    // Win32 suspend counts: an extra suspend on the created-suspended worker
    // needs a matching resume before it starts.
    Sys_SuspendThread(THREAD_CONTEXT_WORKER1);
    Sys_ResumeThread(THREAD_CONTEXT_WORKER1);
    if (Sys_WaitForSingleObjectTimeout(&s_started_event, 20) ||
        s_worker1_ran.load(std::memory_order_acquire))
        return 1;
    Sys_ResumeThread(THREAD_CONTEXT_WORKER0);
    Sys_ResumeThread(THREAD_CONTEXT_WORKER1);
    // ResumeThread on a running thread is a no-op (R_InitHardware resumes the
    // enabled workers every time it runs).
    Sys_ResumeThread(THREAD_CONTEXT_WORKER0);
    Sys_ResumeThread(THREAD_CONTEXT_WORKER0);

    bool started = false;
    for (int attempt = 0; attempt < 1000; ++attempt)
    {
        if (Sys_WaitForSingleObjectTimeout(&s_started_event, 10))
        {
            started = true;
            break;
        }
    }
    if (!started)
        return 1;
    if (!s_worker0_init_before_run.load(std::memory_order_acquire))
        return 1;
    if (s_worker0_is_main_thread.load(std::memory_order_acquire))
        return 1;
    if (s_worker0_thread_id.load(std::memory_order_acquire) != threadId[THREAD_CONTEXT_WORKER0])
        return 1;
    if (s_worker0_thread_id.load(std::memory_order_acquire) == main_thread_id)
        return 1;
    if (!Sys_IsMainThread())
        return 1;
    if (s_worker0_done.load(std::memory_order_acquire))
        return 1;

    Sys_SetEvent(&s_release_event);
    Switch_ThreadWaitForExit(THREAD_CONTEXT_WORKER0);
    if (!s_worker0_done.load(std::memory_order_acquire))
        return 1;

    Switch_ThreadWaitForExit(THREAD_CONTEXT_WORKER1);
    if (!s_worker1_ran.load(std::memory_order_acquire))
        return 1;
    if (s_worker1_thread_id.load(std::memory_order_acquire) != threadId[THREAD_CONTEXT_WORKER1])
        return 1;
    if (s_worker0_thread_id.load(std::memory_order_acquire) ==
        s_worker1_thread_id.load(std::memory_order_acquire))
        return 1;
    Switch_ThreadWaitForExit(THREAD_CONTEXT_WORKER0);

#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
    // Real pause/resume on a live thread (threads.cpp:327-337).  The host
    // shim cannot pause a std::thread, so this section is aarch64-only; the
    // counter going quiet is the evidence that threadPause actually stopped
    // the thread rather than the call being a no-op.
    Sys_CreateEvent(true, false, &s_pause_stop_event);
    Sys_CreateThread(PauseProofThread, THREAD_CONTEXT_WORKER2);
    uint64_t paused = 0;
    for (int attempt = 0; attempt < 1000 && paused == 0; ++attempt)
    {
        Sys_WaitForSingleObjectTimeout(&s_pause_stop_event, 1);
        paused = s_pause_counter.load(std::memory_order_acquire);
    }
    if (paused == 0)
        return 1;
    Sys_SuspendThread(THREAD_CONTEXT_WORKER2);
    const uint64_t paused_counter = s_pause_counter.load(std::memory_order_acquire);
    for (int attempt = 0; attempt < 50; ++attempt)
        Sys_WaitForSingleObjectTimeout(&s_pause_stop_event, 1);
    if (s_pause_counter.load(std::memory_order_acquire) != paused_counter)
        return 1;
    Sys_ResumeThread(THREAD_CONTEXT_WORKER2);
    uint64_t resumed = paused_counter;
    for (int attempt = 0; attempt < 1000 && resumed == paused_counter; ++attempt)
    {
        Sys_WaitForSingleObjectTimeout(&s_pause_stop_event, 1);
        resumed = s_pause_counter.load(std::memory_order_acquire);
    }
    if (resumed == paused_counter)
        return 1;
    Sys_SetEvent(&s_pause_stop_event);
    Switch_ThreadWaitForExit(THREAD_CONTEXT_WORKER2);
#endif

#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
    int32_t priority_before = -1;
    int32_t priority_during = -2;
    int32_t priority_after = -3;
    svcGetThreadPriority(&priority_before, threadGetCurHandle());
#endif
    Sys_BeginLoadThreadPriorities();
    Sys_EndLoadThreadPriorities();
    Sys_BeginLoadThreadPriorities();
#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
    svcGetThreadPriority(&priority_during, threadGetCurHandle());
    if (priority_during == priority_before)
        return 1;
#endif
    Sys_EndLoadThreadPriorities();
#ifndef KISAK_SWITCH_THREAD_PROOF_HOST
    svcGetThreadPriority(&priority_after, threadGetCurHandle());
    if (priority_after != priority_before)
        return 1;
#endif

    if (!ServerHandshakeCheck())
        return 1;

    if (!SaveHistoryCheck())
        return 1;

    Switch_ThreadSetFailureHook(TestFailureHook, nullptr);
    if (!ExpectFailure(CreateDuplicateWorker0) ||
        !ExpectFailure(CreateMainThreadAgain) ||
        !ExpectFailure(CreateOutOfRangeThread) ||
        !ExpectFailure(SuspendUncreatedThread) ||
        !ExpectFailure(ResumeUncreatedThread) ||
        !ExpectFailure(WaitOnUncreatedThread) ||
        !ExpectFailure(WaitOutOfRange) ||
        !ExpectFailure(EndLoadPrioritiesWithoutBegin) ||
        !ExpectFailure(BeginLoadPrioritiesTwice))
        return 1;
    unsigned int expected_failures = 9;
#ifdef KISAK_SWITCH_THREAD_PROOF_HOST
    // The host shim keeps the trap for a live context too (Sys_SuspendThread).
    if (!ExpectFailure(SuspendWorker0) || !ExpectFailure(ResumeWorker0))
        return 1;
    expected_failures = 11;
#endif
    if (s_failure_count != expected_failures)
        return 1;
    Sys_EndLoadThreadPriorities();
    Switch_ThreadSetFailureHook(nullptr, nullptr);
    return 0;
}
