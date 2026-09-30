#include "switch_pmem.h"

#ifdef __SWITCH__
#include "switch_platform.h"

#include <cstdlib>
#include <cstring>

namespace
{
constexpr uint32_t kPmemTotalBytes = 0x20000000;
constexpr uint32_t kMaxPhysicalAllocations = 32;

void Report(const char *message)
{
    Sys_Print(message);
    Sys_Print("\n");
}

[[noreturn]] void Assert(const char *message)
{
    Report(message);
    if (Switch_PMemAssertFailed != NULL)
        Switch_PMemAssertFailed(message);
    std::abort();
}

[[noreturn]] void OutOfMemory(const char *message)
{
    Report(message);
    if (Switch_PMemOutOfMemory != NULL)
        Switch_PMemOutOfMemory(message);
    std::abort();
}
}

PhysicalMemory g_mem;
int g_overAllocatedSize;

void __cdecl PMem_Init()
{
    void *memory = Switch_VirtualReserve(kPmemTotalBytes);
    if (memory == NULL)
        Assert("PMem_Init could not reserve physical memory");
    if (!Switch_VirtualCommit(memory, kPmemTotalBytes))
        Assert("PMem_Init could not commit physical memory");
    PMem_InitPhysicalMemory(&g_mem, static_cast<uint8_t *>(memory), kPmemTotalBytes);
}

void __cdecl PMem_InitPhysicalMemory(PhysicalMemory *pmem, uint8_t *memory, uint32_t memorySize)
{
    if (pmem == NULL)
        Assert("PMem_InitPhysicalMemory: pmem");
    if (memory == NULL)
        Assert("PMem_InitPhysicalMemory: memory");
    memset(pmem, 0, sizeof(PhysicalMemory));
    pmem->buf = memory;
    pmem->prim[1].pos = memorySize;
}

void __cdecl PMem_BeginAlloc(const char *name, uint32_t allocType)
{
    if (allocType >= 2)
        Assert("PMem_BeginAlloc: allocType doesn't index PHYS_ALLOC_COUNT");
    PMem_BeginAllocInPrim(&g_mem.prim[allocType], name);
}

void __cdecl PMem_BeginAllocInPrim(PhysicalMemoryPrim *prim, const char *name)
{
    if (prim->allocName != NULL)
        Assert("PMem_BeginAllocInPrim: !prim->allocName");
    if (prim->allocListCount >= kMaxPhysicalAllocations)
        Assert("PMem_BeginAllocInPrim: allocListCount < MAX_PHYSICAL_ALLOCATIONS");
    prim->allocName = name;
    PhysicalMemoryAllocation *allocEntry = &prim->allocList[prim->allocListCount++];
    allocEntry->name = name;
    allocEntry->pos = prim->pos;
}

void __cdecl PMem_EndAlloc(const char *name, uint32_t allocType)
{
    if (allocType >= 2)
        Assert("PMem_EndAlloc: allocType doesn't index PHYS_ALLOC_COUNT");
    PMem_EndAllocInPrim(&g_mem.prim[allocType], name);
}

void __cdecl PMem_EndAllocInPrim(PhysicalMemoryPrim *prim, const char *name)
{
    if (prim->allocName != name)
        Assert("PMem_EndAllocInPrim: prim->allocName == name");
    prim->allocName = NULL;
    if (prim->allocListCount == 0)
        Assert("PMem_EndAllocInPrim: prim->allocListCount > 0");
}

void __cdecl PMem_Free(const char *name, uint32_t allocType)
{
    if (allocType >= 2)
        Assert("PMem_Free: allocType doesn't index PHYS_ALLOC_COUNT");
    PMem_FreeInPrim(&g_mem.prim[allocType], name);
}

void __cdecl PMem_FreeInPrim(PhysicalMemoryPrim *prim, const char *name)
{
    for (uint32_t allocIndex = 0; allocIndex < prim->allocListCount; ++allocIndex)
    {
        if (prim->allocList[allocIndex].name == name)
        {
            PMem_FreeIndex(prim, allocIndex);
            return;
        }
    }
}

void __cdecl PMem_FreeIndex(PhysicalMemoryPrim *prim, uint32_t allocIndex)
{
    if (prim->allocName != NULL)
        Assert("PMem_FreeIndex: !prim->allocName");
    PhysicalMemoryAllocation *allocEntry = &prim->allocList[allocIndex];
    const char *name = allocEntry->name;
    if (name == NULL)
        Assert("PMem_FreeIndex: name");
    allocEntry->name = NULL;
    if (allocIndex == prim->allocListCount - 1)
    {
        do
        {
            prim->pos = allocEntry->pos;
            if (prim->allocListCount == 0)
                Assert("PMem_FreeIndex: prim->allocListCount");
            if (--prim->allocListCount == 0)
                break;
            allocEntry = &prim->allocList[prim->allocListCount - 1];
        } while (allocEntry->name == NULL);
    }
    else
    {
        if (allocIndex + 1 >= prim->allocListCount)
            Assert("PMem_FreeIndex: allocIndex + 1 < prim->allocListCount");
        Assert("PMem_FreeIndex: freeing allocation caused a memory hole");
    }
}

int __cdecl PMem_GetOverAllocatedSize()
{
    return g_overAllocatedSize;
}

uint8_t *__cdecl PMem_Alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t allocType)
{
    (void)type;
    if (allocType >= 2)
        Assert("PMem_Alloc: allocType == PHYS_ALLOC_HIGH");
    PhysicalMemoryPrim *prim = &g_mem.prim[allocType];
    if (prim->allocName == NULL)
        Assert("PMem_Alloc: prim->allocName");
    if (size == 0)
        Assert("PMem_Alloc: size");
    if (alignment == 0)
        Assert("PMem_Alloc: alignment");

    const uint32_t mask = alignment - 1;
    uint32_t lowPos;
    if (allocType == 1)
    {
        lowPos = (prim->pos - size) & ~mask;
        g_overAllocatedSize = static_cast<int>(g_mem.prim[0].pos - lowPos);
        if (g_overAllocatedSize > 0)
            OutOfMemory("PMem_Alloc: high allocation exhausted physical memory");
        prim->pos = lowPos;
    }
    else
    {
        lowPos = (prim->pos + mask) & ~mask;
        g_overAllocatedSize = static_cast<int>(size + lowPos - g_mem.prim[1].pos);
        if (g_overAllocatedSize > 0)
            OutOfMemory("PMem_Alloc: Need more bytes of ram for alloc to succeed");
        prim->pos = size + lowPos;
    }
    return &g_mem.buf[lowPos];
}

uint32_t __cdecl PMem_GetFreeAmount()
{
    return g_mem.prim[1].pos - g_mem.prim[0].pos;
}

bool __cdecl PMem_ShrinkLastAlloc(const void *memory, uint32_t oldSize, uint32_t newSize,
                                  uint32_t allocType)
{
    if (allocType != 0 || memory == NULL || newSize > oldSize)
        return false;
    PhysicalMemoryPrim *prim = &g_mem.prim[0];
    const uint8_t *bytes = static_cast<const uint8_t *>(memory);
    if (prim->allocName != NULL || prim->allocListCount == 0 || bytes < g_mem.buf)
        return false;
    const uintptr_t offset = static_cast<uintptr_t>(bytes - g_mem.buf);
    if (offset > prim->pos || prim->pos - offset != oldSize ||
        offset < prim->allocList[prim->allocListCount - 1].pos)
        return false;
    prim->pos = static_cast<uint32_t>(offset) + newSize;
    return true;
}

#endif
