#include <universal/q_shared.h>
#include "timing.h"

#if !defined(__SWITCH__)
#include <Windows.h>
#endif
#include <qcommon/threads.h>

double msecPerRawTimerTick;
double qpc2msec;

#if defined(__SWITCH__)
// __rdtsc() (switch_compat.h) uses libnx's supported ARM physical counter.
// Its frequency comes from the matching libnx helper, so no Windows
// QueryPerformanceCounter-style calibration against a second clock is needed.
double __cdecl SecondsPerTick()
{
    const uint64_t frequency = armGetSystemTickFreq();
    qpc2msec = 1000.0 / (double)frequency;
    return 1.0 / (double)frequency;
}
#else
double __cdecl SecondsPerTick()
{
    _LARGE_INTEGER tscStop; // [esp+20h] [ebp-30h]
    _LARGE_INTEGER qpcFrequency; // [esp+28h] [ebp-28h] BYREF
    _LARGE_INTEGER qpcStart; // [esp+30h] [ebp-20h] BYREF
    _LARGE_INTEGER tscStart; // [esp+38h] [ebp-18h]
    _LARGE_INTEGER qpcStop; // [esp+40h] [ebp-10h] BYREF
    double secPerTick; // [esp+48h] [ebp-8h]

    Win_SetThreadLock(THREAD_LOCK_ALL);
    Sleep(0);
    tscStart.QuadPart = 0;
    qpcStart.QuadPart = 0;
    qpcStop.QuadPart = 0;
    QueryPerformanceFrequency(&qpcFrequency);
    qpc2msec = 1000.0 / qpcFrequency.QuadPart;
    QueryPerformanceCounter(&qpcStart);
    tscStart.QuadPart = __rdtsc();
    QueryPerformanceCounter(&qpcStart);
    Sleep(0xFAu);
    tscStop.QuadPart = __rdtsc();
    QueryPerformanceCounter(&qpcStop);
    secPerTick = (double)(qpcStop.QuadPart - qpcStart.QuadPart)
        / ((double)(tscStop.QuadPart - tscStart.QuadPart)
            * (double)qpcFrequency.QuadPart);
    Win_SetThreadLock(THREAD_LOCK_NONE);
    return secPerTick;
}
#endif

void __cdecl InitTiming()
{
	msecPerRawTimerTick = SecondsPerTick() * 1000.0;
}
