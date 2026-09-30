// Horizon implementation of the fast reader/writer lock (GCC atomics).
// Semantics follow win_common.cpp: writers exclude readers and other
// writers; readers are counted and may starve writers.  The spin yield uses
// the libnx scheduler seam where win_common.cpp used NET_Sleep.
#include <switch.h>

#include <universal/fast_critical_section.h>

int Sys_InterlockedIncrement(uint *addend)
{
    return __atomic_add_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

int Sys_InterlockedDecrement(uint *addend)
{
    return __atomic_sub_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

void Sys_LockWrite(FastCriticalSection *critSect)
{
    while (true)
    {
        if (__atomic_load_n(&critSect->readCount, __ATOMIC_SEQ_CST) == 0)
        {
            if (__atomic_add_fetch(&critSect->writeCount, 1, __ATOMIC_SEQ_CST) == 1 &&
                __atomic_load_n(&critSect->readCount, __ATOMIC_SEQ_CST) == 0)
            {
                break;
            }
            __atomic_sub_fetch(&critSect->writeCount, 1, __ATOMIC_SEQ_CST);
        }
        svcSleepThread(0);
    }
}

void Sys_UnlockWrite(FastCriticalSection *critSect)
{
    iassert(critSect->writeCount > 0);
    __atomic_sub_fetch(&critSect->writeCount, 1, __ATOMIC_SEQ_CST);
}
