#include "src/platform/switch/switch_critical_section.h"

#include "src/universal/critical_section.h"

#include <atomic>
#include <chrono>
#include <csetjmp>
#include <thread>

#ifdef KISAK_SWITCH_CRITICAL_SECTION_PROOF_HOST
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#endif

static std::jmp_buf s_failure_jump;
static unsigned int s_failure_count;

#ifdef KISAK_SWITCH_CRITICAL_SECTION_PROOF_HOST
static void TestTimeout(int)
{
    std::_Exit(1);
}
#endif

static void TestFailureHook(void *)
{
    ++s_failure_count;
    std::longjmp(s_failure_jump, 1);
}

static bool ExpectFailure(void (*operation)())
{
    if (setjmp(s_failure_jump) == 0)
    {
        operation();
        return false;
    }
    return true;
}

static void EnterInvalid()
{
    Sys_EnterCriticalSection(CRITSECT_COUNT);
}

static void EnterBeforeInitialize()
{
    Sys_EnterCriticalSection(CRITSECT_CONSOLE);
}

static void LeaveUnowned()
{
    Sys_LeaveCriticalSection(CRITSECT_CONSOLE);
}

int main()
{
#ifdef KISAK_SWITCH_CRITICAL_SECTION_PROOF_HOST
    std::signal(SIGALRM, TestTimeout);
    alarm(2);
#endif
    Switch_CriticalSectionSetFailureHook(TestFailureHook, nullptr);
    if (!ExpectFailure(EnterBeforeInitialize))
        return 1;
    Sys_InitializeCriticalSections();

    Sys_EnterCriticalSection(CRITSECT_CONSOLE);
    Sys_EnterCriticalSection(CRITSECT_CONSOLE);
    Sys_EnterCriticalSection(CRITSECT_DEBUG_SOCKET);
    Sys_LeaveCriticalSection(CRITSECT_DEBUG_SOCKET);
    Sys_LeaveCriticalSection(CRITSECT_CONSOLE);
    Sys_LeaveCriticalSection(CRITSECT_CONSOLE);

    if (!ExpectFailure(EnterInvalid) || !ExpectFailure(LeaveUnowned) || s_failure_count != 3)
        return 1;

#ifdef KISAK_SWITCH_CRITICAL_SECTION_PROOF_HOST
    std::atomic<bool> entered(false);
    std::atomic<bool> acquired(false);
    Sys_EnterCriticalSection(CRITSECT_CONSOLE);
    std::thread thread([&]() {
        entered.store(true, std::memory_order_release);
        Sys_EnterCriticalSection(CRITSECT_CONSOLE);
        acquired.store(true, std::memory_order_release);
        Sys_LeaveCriticalSection(CRITSECT_CONSOLE);
    });
    while (!entered.load(std::memory_order_acquire))
    {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool excluded = !acquired.load(std::memory_order_acquire);
    Sys_LeaveCriticalSection(CRITSECT_CONSOLE);
    thread.join();
    if (!excluded || !acquired.load(std::memory_order_acquire))
        return 1;
#endif

    Switch_CriticalSectionSetFailureHook(nullptr, nullptr);
#ifdef KISAK_SWITCH_CRITICAL_SECTION_PROOF_HOST
    alarm(0);
#endif
    return 0;
}
