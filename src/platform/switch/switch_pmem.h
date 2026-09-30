#pragma once

#include <stdint.h>

#ifdef __SWITCH__

#ifndef __cdecl
#define __cdecl
#endif

struct PhysicalMemoryAllocation
{
    const char *name;
    uint32_t pos;
};

struct PhysicalMemoryPrim
{
    const char *allocName;
    uint32_t allocListCount;
    uint32_t pos;
    PhysicalMemoryAllocation allocList[32];
};

struct PhysicalMemory
{
    uint8_t *buf;
    PhysicalMemoryPrim prim[2];
};

extern PhysicalMemory g_mem;
extern int g_overAllocatedSize;

extern "C" void Switch_PMemAssertFailed(const char *message) __attribute__((weak));
extern "C" void Switch_PMemOutOfMemory(const char *message) __attribute__((weak));

void __cdecl PMem_Init();
void __cdecl PMem_InitPhysicalMemory(PhysicalMemory *pmem, uint8_t *memory, uint32_t memorySize);
void __cdecl PMem_BeginAlloc(const char *name, uint32_t allocType);
void __cdecl PMem_BeginAllocInPrim(PhysicalMemoryPrim *prim, const char *name);
void __cdecl PMem_EndAlloc(const char *name, uint32_t allocType);
void __cdecl PMem_EndAllocInPrim(PhysicalMemoryPrim *prim, const char *name);
void __cdecl PMem_Free(const char *name, uint32_t allocType);
void __cdecl PMem_FreeInPrim(PhysicalMemoryPrim *prim, const char *name);
void __cdecl PMem_FreeIndex(PhysicalMemoryPrim *prim, uint32_t allocIndex);
int __cdecl PMem_GetOverAllocatedSize();
uint8_t *__cdecl PMem_Alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t allocType);
uint32_t __cdecl PMem_GetFreeAmount();
// Shrink the most recent allocation of a closed low-prim (allocType 0) group
// in place: `memory` must be that allocation, its end must be the prim's
// current top (nothing allocated after it), and newSize <= oldSize. The freed
// tail returns to the pool; the group itself stays live. Returns false (and
// changes nothing) when any of those does not hold.
bool __cdecl PMem_ShrinkLastAlloc(const void *memory, uint32_t oldSize, uint32_t newSize,
                                  uint32_t allocType);
void __cdecl PMem_DumpMemStats();

#endif
