#include "src/platform/switch/switch_event.h"

#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <thread>

void __cdecl Sys_CreateEvent(bool manualReset, bool initialState, void **event);
void __cdecl Sys_ResetEvent(void **event);
void __cdecl Sys_SetEvent(void **event);
void __cdecl Sys_WaitForSingleObject(void **event);
bool __cdecl Sys_WaitForSingleObjectTimeout(void **event, uint32_t msec);

namespace
{
std::jmp_buf s_failure_jump;
unsigned int s_failure_count;
void *s_invalid_event;

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

void CreateWithNullOutput()
{
    Sys_CreateEvent(false, false, nullptr);
}

void ResetNullEvent()
{
    void *event = nullptr;
    Sys_ResetEvent(&event);
}

void WaitWithInfiniteTimeout()
{
    Sys_WaitForSingleObjectTimeout(&s_invalid_event, UINT32_MAX);
}
}

int main()
{
    void *manual_event = nullptr;
    void *auto_event = nullptr;
    void *initial_manual_event = nullptr;
    void *initial_auto_event = nullptr;
    void *infinite_event = nullptr;
    void *timed_auto_event = nullptr;
    void *timed_manual_event = nullptr;
    void *blocked_auto_event = nullptr;

    Sys_CreateEvent(true, false, &manual_event);
    s_invalid_event = manual_event;
    Switch_EventSetFailureHook(TestFailureHook, nullptr);
    if (!ExpectFailure(CreateWithNullOutput) || !ExpectFailure(ResetNullEvent) || !ExpectFailure(WaitWithInfiniteTimeout))
        return 1;
    if (s_failure_count != 3)
        return 1;
    if (Sys_WaitForSingleObjectTimeout(&manual_event, 0))
        return 1;
    Sys_SetEvent(&manual_event);
    if (!Sys_WaitForSingleObjectTimeout(&manual_event, 0) || !Sys_WaitForSingleObjectTimeout(&manual_event, 0))
        return 1;
    Sys_ResetEvent(&manual_event);
    if (Sys_WaitForSingleObjectTimeout(&manual_event, 1))
        return 1;

    Sys_CreateEvent(false, false, &auto_event);
    Sys_SetEvent(&auto_event);
    if (!Sys_WaitForSingleObjectTimeout(&auto_event, 0) || Sys_WaitForSingleObjectTimeout(&auto_event, 0))
        return 1;

    Sys_CreateEvent(true, true, &initial_manual_event);
    if (!Sys_WaitForSingleObjectTimeout(&initial_manual_event, 0) ||
        !Sys_WaitForSingleObjectTimeout(&initial_manual_event, 0))
        return 1;

    Sys_CreateEvent(false, true, &initial_auto_event);
    if (!Sys_WaitForSingleObjectTimeout(&initial_auto_event, 0) ||
        Sys_WaitForSingleObjectTimeout(&initial_auto_event, 0))
        return 1;

    // a signal arriving between repeated finite probes must remain
    // pending until exactly one probe consumes it.  The Horizon backend keeps
    // every light event manual-reset and consumes the signal itself, so an
    // expired waiter's mark cannot swallow it.
    Sys_CreateEvent(false, false, &timed_auto_event);
    std::thread timed_signaler([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        Sys_SetEvent(&timed_auto_event);
    });
    bool consumed_timed_signal = false;
    for (int attempt = 0; attempt < 20 && !consumed_timed_signal; ++attempt)
        consumed_timed_signal = Sys_WaitForSingleObjectTimeout(&timed_auto_event, 1);
    timed_signaler.join();
    if (!consumed_timed_signal || Sys_WaitForSingleObjectTimeout(&timed_auto_event, 0))
        return 1;

    // Renderer handshake: a manual-reset timed wait blocks in leventWait,
    // so an expired wait may leave its waiter mark behind.  A later signal must
    // still be kept, and a signal during a long timed wait must wake it early
    // instead of at the end of a sleep quantum.
    Sys_CreateEvent(true, false, &timed_manual_event);
    if (Sys_WaitForSingleObjectTimeout(&timed_manual_event, 1))
        return 1;
    Sys_SetEvent(&timed_manual_event);
    if (!Sys_WaitForSingleObjectTimeout(&timed_manual_event, 0) ||
        !Sys_WaitForSingleObjectTimeout(&timed_manual_event, 1))
        return 1;
    Sys_ResetEvent(&timed_manual_event);
    if (Sys_WaitForSingleObjectTimeout(&timed_manual_event, 0))
        return 1;
    std::thread manual_signaler([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        Sys_SetEvent(&timed_manual_event);
    });
    const auto manual_wait_start = std::chrono::steady_clock::now();
    const bool manual_woken = Sys_WaitForSingleObjectTimeout(&timed_manual_event, 2000);
    const auto manual_wait = std::chrono::steady_clock::now() - manual_wait_start;
    manual_signaler.join();
    if (!manual_woken || manual_wait > std::chrono::milliseconds(1000))
        return 1;

    // one thread already blocked on an auto-reset event must be woken by
    // a single signal, which consumes it (libnx's autoclear mode lost this
    // wake on some emulators: renderPausedEvent in Sys_FrontEndSleep).
    Sys_CreateEvent(false, false, &blocked_auto_event);
    std::atomic<bool> blocked_auto_woken(false);
    std::thread blocked_auto_waiter([&]() {
        Sys_WaitForSingleObject(&blocked_auto_event);
        blocked_auto_woken.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Sys_SetEvent(&blocked_auto_event);
    for (int attempt = 0; attempt < 2000 && !blocked_auto_woken.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!blocked_auto_woken.load(std::memory_order_acquire))
        std::_Exit(1); // the waiter is still blocked, so it cannot be joined
    blocked_auto_waiter.join();
    if (Sys_WaitForSingleObjectTimeout(&blocked_auto_event, 0))
        return 1;

    Sys_CreateEvent(false, false, &infinite_event);
    std::atomic<bool> release_waiter(false);
    std::thread signaler([&]() {
        while (!release_waiter.load(std::memory_order_acquire))
        {
        }
        Sys_SetEvent(&infinite_event);
    });
    release_waiter.store(true, std::memory_order_release);
    Sys_WaitForSingleObject(&infinite_event);
    signaler.join();

    Switch_EventSetFailureHook(nullptr, nullptr);
    return 0;
}
