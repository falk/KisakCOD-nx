#pragma once

// Retail PC iw3sp times a few budgets in raw x86 time-stamp-counter ticks
// with literal constants (e.g. the 1,000,000-tick server slice after a GPU
// sync poll, 0x5c8370, and the 20,000-tick adaptive GPU-sync decay floor,
// 0x5fed80).  The port's __rdtsc() is not a CPU TSC (on the Switch it is the
// 19.2 MHz ARM system counter), so the same literal would be a very different
// duration.  These helpers keep the duration: a literal is read as ticks of a
// reference retail CPU and converted to the port's raw counter through
// msecPerRawTimerTick (InitTiming).
//
// Reference: 2.4 GHz, the minimum CPU clock CoD4's PC box lists; the TSC of
// the era's CPUs ran at that nominal clock.  1,000,000 ticks => ~417 us,
// 20,000 ticks => ~8.3 us.

#include <stdint.h>

#define RETAIL_TSC_REFERENCE_HZ 2400000000.0

// Microseconds a retail TSC tick count stood for on the reference CPU.
static inline double RetailTsc_TicksToUsec(double retailTicks)
{
    return retailTicks * 1000000.0 / RETAIL_TSC_REFERENCE_HZ;
}

// Retail TSC ticks -> the port's raw __rdtsc() ticks for the same duration.
// msecPerRawTick is msecPerRawTimerTick; a non-positive value (timing not
// initialised) returns the literal unchanged.  Never returns 0 for a non-zero
// input, so a budget cannot collapse to "already expired".
static inline uint64_t RetailTsc_ToRawTicks(double retailTicks, double msecPerRawTick)
{
    if (!(msecPerRawTick > 0.0))
        return (uint64_t)retailTicks;
    const double raw = RetailTsc_TicksToUsec(retailTicks) / (msecPerRawTick * 1000.0);
    if (retailTicks > 0.0 && raw < 1.0)
        return 1;
    return (uint64_t)(raw + 0.5);
}
