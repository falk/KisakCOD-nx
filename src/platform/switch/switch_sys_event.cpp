#include "switch_sys_event.h"

#include <qcommon/sys_event.h>
#include "switch_platform.h"

#include <atomic>
#include <cstdlib>
extern "C" uint32_t Sys_Milliseconds(void);

namespace
{
constexpr unsigned int kEventCapacity = 256;
sysEvent_t s_events[kEventCapacity];
unsigned int s_read_index;
unsigned int s_count;
std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
SwitchSysEventFailureHook s_failure_hook;
void *s_failure_context;

void Lock()
{
    while (s_lock.test_and_set(std::memory_order_acquire))
    {
    }
}

void Unlock()
{
    s_lock.clear(std::memory_order_release);
}

[[noreturn]] void Fail()
{
    if (s_failure_hook != nullptr)
        s_failure_hook(s_failure_context);
    std::abort();
}
}

void Switch_SysEventSetFailureHook(SwitchSysEventFailureHook hook, void *context)
{
    s_failure_hook = hook;
    s_failure_context = context;
}

void __cdecl Sys_QueEvent(uint32_t time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr)
{
    if (type != SE_KEY || ptrLength != 0 || ptr != nullptr)
        Fail();

    Lock();
    if (s_count == kEventCapacity)
    {
        s_read_index = (s_read_index + 1) % kEventCapacity;
        --s_count;
    }
    s_events[(s_read_index + s_count) % kEventCapacity] = {
        static_cast<int>(time != 0 ? time : Sys_Milliseconds()), type, value, value2, 0, nullptr
    };
    ++s_count;
    Unlock();
}

sysEvent_t *__cdecl Sys_GetEvent(sysEvent_t *result)
{
    if (result == nullptr)
        Fail();

    Lock();
    if (s_count != 0)
    {
        *result = s_events[s_read_index];
        s_read_index = (s_read_index + 1) % kEventCapacity;
        --s_count;
        Unlock();
        return result;
    }
    Unlock();

    *result = { static_cast<int>(Sys_Milliseconds()), SE_NONE, 0, 0, 0, nullptr };
    return result;
}
