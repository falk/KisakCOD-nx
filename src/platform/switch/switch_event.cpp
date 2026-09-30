#include "switch_event.h"

#include <cstdint>
#include <cstdlib>
#include <new>

#ifdef KISAK_SWITCH_EVENT_PROOF_HOST
#include <chrono>
#include <condition_variable>
#include <mutex>
#else
extern "C"
{
#include <switch/arm/counter.h>
#include <switch/kernel/levent.h>
}
#endif

namespace
{
#ifdef KISAK_SWITCH_EVENT_PROOF_HOST
// This host-only type emulates libnx LEvent behavior for sanitizer coverage.
struct HostEvent
{
    std::mutex mutex;
    std::condition_variable condition;
    bool signaled;
    bool autoclear;
    HostEvent *next;
};

void EventInit(HostEvent *event, bool initial_state, bool autoclear)
{
    event->signaled = initial_state;
    event->autoclear = autoclear;
}

bool EventTryWait(HostEvent *event)
{
    std::lock_guard<std::mutex> lock(event->mutex);
    if (!event->signaled)
        return false;
    if (event->autoclear)
        event->signaled = false;
    return true;
}

bool EventWait(HostEvent *event, uint64_t timeout_nanoseconds)
{
    std::unique_lock<std::mutex> lock(event->mutex);
    const auto ready = [event]() { return event->signaled; };
    if (timeout_nanoseconds == UINT64_MAX)
        event->condition.wait(lock, ready);
    else if (!event->condition.wait_for(lock, std::chrono::nanoseconds(timeout_nanoseconds), ready))
        return false;
    if (event->autoclear)
        event->signaled = false;
    return true;
}

void EventSignal(HostEvent *event)
{
    std::lock_guard<std::mutex> lock(event->mutex);
    event->signaled = true;
    event->condition.notify_all();
}

void EventClear(HostEvent *event)
{
    std::lock_guard<std::mutex> lock(event->mutex);
    event->signaled = false;
}

using SwitchEventHandle = HostEvent;

HostEvent *s_host_events;

struct HostEventCleanup
{
    ~HostEventCleanup()
    {
        while (s_host_events != nullptr)
        {
            HostEvent *event = s_host_events;
            s_host_events = event->next;
            delete event;
        }
    }
} s_host_event_cleanup;
#else
// Every light event is created manual-reset at the libnx level, and Win32
// auto-reset is done here: wait for "signaled" (2), then take it with one
// 2 -> 0 compare-exchange; a waiter that loses the exchange waits again.
//
// libnx's own autoclear mode is not usable.  Its signal is
// svcSignalToAddress(SignalAndModifyBasedOnWaitingThreadCountIfEqual, 1, 1),
// and a woken autoclear waiter only returns once it reads 2; otherwise it
// treats the wake as spurious and sleeps again (levent.c _leventWait,
// unchanged since libnx 4.0).  With exactly one blocked waiter, some emulators'
// SignalAndModifyIfEqual leaves the counter at 1 (it counts
// the woken waiter itself against `count`), so that waiter went back to sleep
// with the signal gone: the render handshake hung main in
// Sys_FrontEndSleep's renderPausedEvent wait after the back end had set it.
// The manual-reset signal
// (SignalAndIncrementIfEqual, count -1) is 1 -> 2 and wakes every waiter on
// both such emulators and the Horizon kernel, and it also keeps a signal that lands
// after an expired timed wait left the waiter mark (1) behind, so timed
// waits block in the kernel instead of polling.
struct SwitchEvent
{
    LEvent light;
    bool autoReset;
};

void EventInit(SwitchEvent *event, bool initial_state, bool autoclear)
{
    leventInit(&event->light, initial_state, false);
    event->autoReset = autoclear;
}

bool EventConsume(SwitchEvent *event)
{
    uint32_t signaled = 2;
    return __atomic_compare_exchange_n(&event->light.counter, &signaled, 0u, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

bool EventTryWait(SwitchEvent *event)
{
    return event->autoReset ? EventConsume(event) : leventTryWait(&event->light);
}

bool EventWait(SwitchEvent *event, uint64_t timeout_nanoseconds)
{
    if (!event->autoReset)
        return leventWait(&event->light, timeout_nanoseconds);

    const bool has_timeout = timeout_nanoseconds != UINT64_MAX;
    const uint64_t deadline = has_timeout ? armGetSystemTick() + armNsToTicks(timeout_nanoseconds) : 0;
    while (!EventConsume(event))
    {
        uint64_t wait_nanoseconds = UINT64_MAX;
        if (has_timeout)
        {
            const uint64_t now = armGetSystemTick();
            if (now >= deadline)
                return false;
            wait_nanoseconds = armTicksToNs(deadline - now);
        }
        if (!leventWait(&event->light, wait_nanoseconds))
            return EventConsume(event);
    }
    return true;
}

void EventSignal(SwitchEvent *event)
{
    leventSignal(&event->light);
}

void EventClear(SwitchEvent *event)
{
    leventClear(&event->light);
}

using SwitchEventHandle = SwitchEvent;
#endif

SwitchEventFailureHook s_failure_hook;
void *s_failure_context;

[[noreturn]] void Fail()
{
    if (s_failure_hook != nullptr)
        s_failure_hook(s_failure_context);
    std::abort();
}

SwitchEventHandle *GetEvent(void **event)
{
    if (event == nullptr || *event == nullptr)
        Fail();
    return static_cast<SwitchEventHandle *>(*event);
}
}

void Switch_EventSetFailureHook(SwitchEventFailureHook hook, void *context)
{
    s_failure_hook = hook;
    s_failure_context = context;
}

void __cdecl Sys_CreateEvent(bool manualReset, bool initialState, void **event)
{
    if (event == nullptr || *event != nullptr)
        Fail();

    SwitchEventHandle *created_event = new (std::nothrow) SwitchEventHandle;
    if (created_event == nullptr)
        Fail();
    EventInit(created_event, initialState, !manualReset);
    *event = created_event;
#ifdef KISAK_SWITCH_EVENT_PROOF_HOST
    created_event->next = s_host_events;
    s_host_events = created_event;
#endif
}

void __cdecl Sys_ResetEvent(void **event)
{
    EventClear(GetEvent(event));
}

void __cdecl Sys_SetEvent(void **event)
{
    EventSignal(GetEvent(event));
}

void __cdecl Sys_WaitForSingleObject(void **event)
{
    EventWait(GetEvent(event), UINT64_MAX);
}

bool __cdecl Sys_WaitForSingleObjectTimeout(void **event, uint32_t msec)
{
    SwitchEventHandle *wait_event = GetEvent(event);
    if (msec == UINT32_MAX)
        Fail();
    if (msec == 0)
        return EventTryWait(wait_event);
    return EventWait(wait_event, static_cast<uint64_t>(msec) * UINT64_C(1000000));
}
