#include "src/platform/switch/switch_platform.h"

#include <stdint.h>

static uint64_t fake_nanoseconds;
static uint32_t fake_reads;

static uint64_t FakeNanoseconds(void)
{
    uint64_t now = fake_nanoseconds;

    fake_nanoseconds += UINT64_C(1000000);
    ++fake_reads;
    return now;
}

int main(void)
{
    uint32_t raw_before;
    uint32_t elapsed_before;
    uint32_t raw_after;
    uint32_t elapsed_after;

    Switch_HostMonotonicNanoseconds = FakeNanoseconds;

    /* Nine seconds is already beyond a 32-bit nanosecond intermediate. */
    fake_nanoseconds = UINT64_C(9000000000);
    elapsed_before = Sys_Milliseconds();
    if (elapsed_before != 0 || fake_reads != 1)
        return 1;

    raw_before = Sys_MillisecondsRaw();
    if (raw_before != 9001 || fake_reads != 2)
        return 1;

    raw_after = Sys_MillisecondsRaw();
    elapsed_after = Sys_Milliseconds();

    /* Every API call takes one read from the free-running source. */
    if (raw_after != 9002 || elapsed_after != 3 || fake_reads != 4)
        return 1;

    return 0;
}
