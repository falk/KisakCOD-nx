#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef __cdecl
#define __cdecl
#endif

struct HunkUser
{
    HunkUser* current;
    HunkUser* next;
#ifdef __SWITCH__
    size_t maxSize;
    uint8_t* end;
    uint8_t* pos;
#else
    int maxSize;
    int end;
    int pos;
#endif
    const char* name;
    bool fixed;
    bool tempMem;
    int type;
#ifdef __SWITCH__
    alignas(32) uint8_t buf[1];
#else
    uint8_t buf[1];
#endif
};

HunkUser* __cdecl Hunk_UserCreate(int maxSize, const char* name, bool fixed, bool tempMem, int type);
void* Hunk_UserAlloc(HunkUser* user, uint32_t size, int alignment);
void* Hunk_UserAllocAlignStrict(HunkUser* user, uint32_t size);
void __cdecl Hunk_UserSetPos(HunkUser* user, uint8_t* pos);
void __cdecl Hunk_UserReset(HunkUser* user);
void __cdecl Hunk_UserDestroy(HunkUser* user);
char* __cdecl Hunk_CopyString(HunkUser* user, const char* in);
