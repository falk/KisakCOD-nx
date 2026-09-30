#include "src/platform/switch/switch_pmem.h"

#include <csetjmp>
#include <cstdint>

extern "C"
{
void Switch_PMemAssertFailed(const char *message);
void Switch_PMemOutOfMemory(const char *message);
}

namespace
{
std::jmp_buf s_failure_jump;
unsigned int s_assert_count;
unsigned int s_oom_count;
uint8_t s_small_buffer[0x10000];
const char *const s_phase_a = "phaseA";
const char *const s_phase_b = "phaseB";
const char *const s_inner = "inner";

bool ExpectFailure(void (*operation)())
{
    if (setjmp(s_failure_jump) == 0)
    {
        operation();
        return false;
    }
    return true;
}

void AllocWithoutBegin()
{
    PMem_Alloc(0x1000, 0x1000, 0, 0);
}

void AllocZeroSize()
{
    PMem_BeginAlloc(s_phase_a, 0);
    PMem_Alloc(0, 0x1000, 0, 0);
}

void AllocZeroAlignment()
{
    PMem_BeginAlloc(s_phase_a, 0);
    PMem_Alloc(0x100, 0, 0, 0);
}

void AllocBadType()
{
    PMem_Alloc(0x1000, 0x1000, 0, 2);
}

void EndWrongName()
{
    PMem_EndAlloc(s_phase_b, 0);
}

void FreeMemoryHole()
{
    PMem_Free(s_phase_a, 0);
}

void LowAllocTooBig()
{
    PMem_Alloc(0x20000, 0x1000, 4, 0);
}

void HighAllocTooBig()
{
    PMem_Alloc(0xC000, 0x1000, 4, 1);
}
}

void Switch_PMemAssertFailed(const char *)
{
    ++s_assert_count;
    std::longjmp(s_failure_jump, 1);
}

void Switch_PMemOutOfMemory(const char *)
{
    ++s_oom_count;
    std::longjmp(s_failure_jump, 1);
}

int main()
{
    PMem_Init();
    if (g_mem.buf == nullptr)
        return 1;
    if (g_mem.prim[0].pos != 0 || g_mem.prim[1].pos != 0x20000000u)
        return 1;
    if (PMem_GetFreeAmount() != 0x20000000u)
        return 1;

    PMem_BeginAlloc(s_phase_a, 1);
    uint8_t *high = PMem_Alloc(0x2000, 0x1000, 4, 1);
    if (high != g_mem.buf + 0x20000000u - 0x2000u || g_mem.prim[1].pos != 0x20000000u - 0x2000u)
        return 1;
    if (PMem_GetFreeAmount() != 0x20000000u - 0x2000u)
        return 1;
    PMem_EndAlloc(s_phase_a, 1);
    PMem_Free(s_phase_a, 1);
    if (g_mem.prim[1].pos != 0x20000000u || PMem_GetFreeAmount() != 0x20000000u)
        return 1;

    PMem_BeginAlloc(s_phase_a, 0);
    uint8_t *low_a = PMem_Alloc(0x1000, 0x1000, 4, 0);
    uint8_t *low_b = PMem_Alloc(0x80, 0x80, 4, 0);
    if (low_a != g_mem.buf || low_b != g_mem.buf + 0x1000)
        return 1;
    if (g_mem.prim[0].pos != 0x1080u || PMem_GetFreeAmount() != 0x20000000u - 0x1080u)
        return 1;
    PMem_EndAlloc(s_phase_a, 0);
    PMem_Free(s_phase_a, 0);
    if (g_mem.prim[0].pos != 0 || PMem_GetFreeAmount() != 0x20000000u)
        return 1;

    // In-place shrink of a closed group's last allocation (retail zone
    // native arena trim): only the top allocation of the low prim, only
    // outside an open group, never grow, never the high prim.
    PMem_BeginAlloc(s_phase_a, 0);
    uint8_t *shrink_a = PMem_Alloc(0x1000, 0x10, 4, 0);
    uint8_t *shrink_b = PMem_Alloc(0x8000, 0x10, 4, 0);
    if (PMem_ShrinkLastAlloc(shrink_b, 0x8000, 0x100, 0))
        return 1; // group still open
    PMem_EndAlloc(s_phase_a, 0);
    if (PMem_ShrinkLastAlloc(shrink_a, 0x1000, 0x10, 0) ||
        PMem_ShrinkLastAlloc(shrink_b, 0x8000, 0x9000, 0) ||
        PMem_ShrinkLastAlloc(shrink_b, 0x7000, 0x100, 0) ||
        PMem_ShrinkLastAlloc(shrink_b, 0x8000, 0x100, 1))
        return 1;
    if (!PMem_ShrinkLastAlloc(shrink_b, 0x8000, 0x100, 0) || g_mem.prim[0].pos != 0x1100u ||
        PMem_GetFreeAmount() != 0x20000000u - 0x1100u)
        return 1;
    PMem_Free(s_phase_a, 0);
    if (g_mem.prim[0].pos != 0 || g_mem.prim[0].allocListCount != 0)
        return 1;

    if (!ExpectFailure(AllocWithoutBegin) ||
        !ExpectFailure(AllocBadType))
        return 1;
    if (!ExpectFailure(AllocZeroSize))
        return 1;
    PMem_EndAlloc(s_phase_a, 0);
    if (!ExpectFailure(AllocZeroAlignment))
        return 1;
    PMem_EndAlloc(s_phase_a, 0);
    PMem_BeginAlloc(s_phase_a, 0);
    if (!ExpectFailure(EndWrongName))
        return 1;
    PMem_EndAlloc(s_phase_a, 0);

    PMem_InitPhysicalMemory(&g_mem, s_small_buffer, sizeof(s_small_buffer));
    if (g_mem.prim[1].pos != sizeof(s_small_buffer) || PMem_GetFreeAmount() != sizeof(s_small_buffer))
        return 1;

    PMem_BeginAlloc(s_inner, 0);
    if (!ExpectFailure(LowAllocTooBig))
        return 1;
    PMem_EndAlloc(s_inner, 0);

    PMem_BeginAlloc(s_phase_a, 0);
    PMem_Alloc(0x8000, 0x1000, 4, 0);
    PMem_EndAlloc(s_phase_a, 0);
    PMem_BeginAlloc(s_phase_b, 0);
    PMem_Alloc(0x1000, 0x1000, 4, 0);
    PMem_EndAlloc(s_phase_b, 0);
    if (g_mem.prim[0].allocListCount != 3 || g_mem.prim[0].pos != 0x9000u)
        return 1;
    PMem_Free(s_phase_b, 0);
    if (g_mem.prim[0].allocListCount != 2 || g_mem.prim[0].pos != 0x8000u)
        return 1;
    PMem_Free(s_phase_a, 0);
    if (g_mem.prim[0].allocListCount != 1 || g_mem.prim[0].pos != 0)
        return 1;
    PMem_Free(s_inner, 0);
    if (g_mem.prim[0].allocListCount != 0 || g_mem.prim[0].pos != 0)
        return 1;

    PMem_BeginAlloc(s_phase_a, 0);
    PMem_Alloc(0x8000, 0x1000, 4, 0);
    PMem_EndAlloc(s_phase_a, 0);
    PMem_BeginAlloc(s_phase_b, 1);
    if (!ExpectFailure(HighAllocTooBig))
        return 1;
    if (PMem_GetOverAllocatedSize() != 0x4000)
        return 1;
    PMem_EndAlloc(s_phase_b, 1);

    PMem_BeginAlloc(s_phase_a, 0);
    PMem_Alloc(0x1000, 0x1000, 4, 0);
    PMem_EndAlloc(s_phase_a, 0);
    PMem_BeginAlloc(s_phase_b, 0);
    PMem_Alloc(0x1000, 0x1000, 4, 0);
    PMem_EndAlloc(s_phase_b, 0);
    if (!ExpectFailure(FreeMemoryHole))
        return 1;

    if (s_assert_count != 6 || s_oom_count != 2)
        return 1;
    return 0;
}
