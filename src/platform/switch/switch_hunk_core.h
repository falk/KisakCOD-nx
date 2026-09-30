#pragma once

#include <stdint.h>

#ifndef __cdecl
#define __cdecl
#endif

#ifdef __SWITCH__
struct hunkUsed_t
{
    int32_t permanent;
    int32_t temp;
};

extern hunkUsed_t hunk_high;
extern hunkUsed_t hunk_low;

void* __cdecl Z_VirtualReserve(int size);
void __cdecl Z_VirtualDecommitInternal(void* ptr, int size);
void __cdecl Z_VirtualFreeInternal(void* ptr);
void* __cdecl Z_TryVirtualAllocInternal(int size);
bool __cdecl Z_TryVirtualCommitInternal(void* ptr, int size);
void __cdecl Z_VirtualCommitInternal(void* ptr, int size);
void __cdecl Z_VirtualFree(void* ptr);
void __cdecl Z_VirtualDecommit(void* ptr, int size);
char* __cdecl Z_TryVirtualAlloc(int size, const char* name, int type);
char* __cdecl Z_VirtualAlloc(int size, const char* name, int type);
void __cdecl Z_VirtualCommit(void* ptr, int size);

void __cdecl Hunk_ClearToMarkLow(int mark);
void Hunk_Clear();
int __cdecl Hunk_Used();
uint8_t* __cdecl Hunk_Alloc(uint32_t size, const char* name, int type);
uint8_t* __cdecl Hunk_AllocAlign(uint32_t size, int alignment, const char* name, int type);
uintptr_t __cdecl Hunk_AllocateTempMemoryHigh(int size, const char* name);
void Hunk_ClearTempMemoryHigh();
uint8_t* __cdecl Hunk_AllocLow(uint32_t size, const char* name, int type);
uint8_t* __cdecl Hunk_AllocLowAlign(uint32_t size, int alignment, const char* name, int type);
uint32_t* __cdecl Hunk_AllocateTempMemory(int size, const char* name);
void __cdecl Hunk_FreeTempMemory(char* buf);
void Hunk_ClearTempMemory();
void Hunk_CheckTempMemoryClear();
void Hunk_CheckTempMemoryHighClear();

extern unsigned char* s_hunkData;
extern uint8_t* s_origHunkData;
extern int s_hunkTotal;

extern "C" void Switch_HunkCoreFatal(const char* message) __attribute__((weak));
extern "C" void Switch_HunkCoreDrop(const char* message) __attribute__((weak));
extern "C" void Switch_HunkCoreOutOfMemory(const char* message) __attribute__((weak));
extern "C" int Switch_HunkCoreThreadCheck(int allowRenderThread) __attribute__((weak));
extern "C" void Switch_HunkCoreTrack(const char* event, int amount, const char* name, int type) __attribute__((weak));

void Hunk_ClearData();
#endif
