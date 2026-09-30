#include "src/platform/switch/switch_platform.h"

#include <stdint.h>

static uint64_t fake_nanoseconds;

static uint64_t FakeNanoseconds(void)
{
    return fake_nanoseconds;
}

int main(void)
{
    uint32_t raw_before;
    uint32_t raw_after;
    uint32_t elapsed_before;
    uint32_t elapsed_after;

    Switch_HostMonotonicNanoseconds = FakeNanoseconds;
    fake_nanoseconds = UINT64_C(0xfffffff0) * UINT64_C(1000000);
    raw_before = Sys_MillisecondsRaw();
    elapsed_before = Sys_Milliseconds();

    fake_nanoseconds += UINT64_C(200000000);
    raw_after = Sys_MillisecondsRaw();
    elapsed_after = Sys_Milliseconds();

    if (raw_after != 184 ||
        (uint32_t)(raw_after - raw_before) != 200 ||
        (uint32_t)(elapsed_after - elapsed_before) != 200)
        return 1;

    return 0;
}
