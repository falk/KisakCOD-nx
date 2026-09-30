#include "switch_critical_section.h"

#include <universal/critical_section.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace
{
std::recursive_mutex s_critical_sections[CRITSECT_COUNT];
std::mutex s_state_mutex;
std::thread::id s_owners[CRITSECT_COUNT];
unsigned int s_recursion_counts[CRITSECT_COUNT];
std::atomic<bool> s_initialized(false);
SwitchCriticalSectionFailureHook s_failure_hook;
void *s_failure_context;

bool IsValid(int crit_sect)
{
    return crit_sect >= 0 && crit_sect < CRITSECT_COUNT;
}

[[noreturn]] void Fail()
{
    if (s_failure_hook != nullptr)
        s_failure_hook(s_failure_context);
    std::abort();
}
}

void Switch_CriticalSectionSetFailureHook(SwitchCriticalSectionFailureHook hook, void *context)
{
    s_failure_hook = hook;
    s_failure_context = context;
}

void Sys_InitializeCriticalSections()
{
    bool already_initialized;
    {
        std::lock_guard<std::mutex> lock(s_state_mutex);
        already_initialized = s_initialized.load(std::memory_order_relaxed);
        if (!already_initialized)
            s_initialized.store(true, std::memory_order_release);
    }
    if (already_initialized)
        Fail();
}

void Sys_EnterCriticalSection(int crit_sect)
{
    if (!s_initialized.load(std::memory_order_acquire) || !IsValid(crit_sect))
        Fail();

    s_critical_sections[crit_sect].lock();
    bool corrupt_state;
    {
        std::lock_guard<std::mutex> lock(s_state_mutex);
        const std::thread::id current_thread = std::this_thread::get_id();
        corrupt_state = s_recursion_counts[crit_sect] != 0 && s_owners[crit_sect] != current_thread;
        if (!corrupt_state)
        {
            s_owners[crit_sect] = current_thread;
            ++s_recursion_counts[crit_sect];
        }
    }
    if (corrupt_state)
    {
        s_critical_sections[crit_sect].unlock();
        Fail();
    }
}

void Sys_LeaveCriticalSection(int crit_sect)
{
    if (!s_initialized.load(std::memory_order_acquire) || !IsValid(crit_sect))
        Fail();

    bool invalid_owner;
    {
        std::lock_guard<std::mutex> lock(s_state_mutex);
        invalid_owner = s_recursion_counts[crit_sect] == 0 || s_owners[crit_sect] != std::this_thread::get_id();
        if (!invalid_owner && --s_recursion_counts[crit_sect] == 0)
            s_owners[crit_sect] = std::thread::id();
    }
    if (invalid_owner)
        Fail();

    s_critical_sections[crit_sect].unlock();
}
