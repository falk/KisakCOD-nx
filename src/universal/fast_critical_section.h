// Fast reader/writer lock shared by the database hash and dvar pools.
// Platform-neutral declarations: Win32 implements these in win_common.cpp,
// Horizon in switch_fast_critical_section.cpp.
#pragma once

#include <universal/q_shared.h>

struct FastCriticalSection
{
	volatile uint32_t readCount;
	volatile uint32_t writeCount;
};

void Sys_LockWrite(FastCriticalSection* critSect);
void Sys_UnlockWrite(FastCriticalSection* critSect);
int Sys_InterlockedIncrement(uint *addend);
int Sys_InterlockedDecrement(uint *addend);
