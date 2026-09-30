#pragma once

#include <cstdint>

#ifndef __cdecl
#define __cdecl
#endif

typedef enum
{
    SE_NONE = 0x0,
    SE_KEY = 0x1,
    SE_CHAR = 0x2,
    SE_CONSOLE = 0x3,
} sysEventType_t;

struct sysEvent_t
{
    int evTime;
    sysEventType_t evType;
    int evValue;
    int evValue2;
    int evPtrLength;
    void *evPtr;
};

void __cdecl Sys_QueEvent(uint32_t time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr);
sysEvent_t *__cdecl Sys_GetEvent(sysEvent_t *result);
