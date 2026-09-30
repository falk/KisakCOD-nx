#include "src/platform/switch/switch_platform.h"

#include <stdint.h>

#define PAGE_SIZE ((size_t)4096)
#define RESERVATION_SIZE (PAGE_SIZE * 16)

int main(void)
{
    unsigned char *base;
    unsigned char *last_page;

    if (Switch_VirtualReserve(0) != NULL || Switch_VirtualReserve(PAGE_SIZE - 1) != NULL)
        return 1;
    if (Switch_VirtualRelease(NULL) != 0)
        return 1;

    base = (unsigned char *)Switch_VirtualReserve(RESERVATION_SIZE);
    if (base == NULL || (uintptr_t)base % PAGE_SIZE != 0)
        return 1;
    last_page = base + RESERVATION_SIZE - PAGE_SIZE;

    if (Switch_VirtualCommit(base, 0) != 0 ||
        Switch_VirtualCommit(base + 1, PAGE_SIZE) != 0 ||
        Switch_VirtualCommit(base, PAGE_SIZE - 1) != 0 ||
        Switch_VirtualCommit(base + RESERVATION_SIZE, PAGE_SIZE) != 0 ||
        Switch_VirtualDecommit(base + 1, PAGE_SIZE) != 0 ||
        Switch_VirtualDecommit(last_page, PAGE_SIZE - 1) != 0 ||
        Switch_VirtualDecommit(base + RESERVATION_SIZE, PAGE_SIZE) != 0)
        return 1;

    if (Switch_VirtualCommit(base, PAGE_SIZE) != 1 ||
        Switch_VirtualCommit(last_page, PAGE_SIZE) != 1)
        return 1;
    base[0] = 0x41;
    last_page[PAGE_SIZE - 1] = 0x7c;
    if (base[0] != 0x41 || last_page[PAGE_SIZE - 1] != 0x7c)
        return 1;

    if (Switch_VirtualDecommit(base, PAGE_SIZE) != 1 ||
        Switch_VirtualCommit(base, PAGE_SIZE) != 1)
        return 1;
    if (base[0] != 0)
        return 1;
    base[0] = 0x24;
    if (base[0] != 0x24)
        return 1;

    if (Switch_VirtualRelease(base + PAGE_SIZE) != 0 ||
        Switch_VirtualRelease(base) != 1 ||
        Switch_VirtualRelease(base) != 0)
        return 1;
    return 0;
}
