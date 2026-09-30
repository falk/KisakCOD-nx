#define _POSIX_C_SOURCE 200809L

#include "src/platform/switch/switch_platform.h"

#include <time.h>

int main(void)
{
    struct timespec delay = { 0, 10000000 };
    uint32_t before = Sys_Milliseconds();

    if (nanosleep(&delay, NULL) != 0)
        return 1;

    return (uint32_t)(Sys_Milliseconds() - before) > 0 ? 0 : 1;
}
