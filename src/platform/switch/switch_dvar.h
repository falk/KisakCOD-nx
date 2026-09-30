#pragma once

#include <stdint.h>

#ifdef __SWITCH__

#ifndef __cdecl
#define __cdecl
#endif

enum DvarType : uint8_t
{
    DVAR_TYPE_BOOL = 0x0,
    DVAR_TYPE_FLOAT = 0x1,
    DVAR_TYPE_FLOAT_2 = 0x2,
    DVAR_TYPE_FLOAT_3 = 0x3,
    DVAR_TYPE_FLOAT_4 = 0x4,
    DVAR_TYPE_INT = 0x5,
    DVAR_TYPE_ENUM = 0x6,
    DVAR_TYPE_STRING = 0x7,
    DVAR_TYPE_COLOR = 0x8,
};

enum DvarSetSource : int32_t
{
    DVAR_SOURCE_INTERNAL = 0x0,
    DVAR_SOURCE_EXTERNAL = 0x1,
    DVAR_SOURCE_SCRIPT = 0x2,
    DVAR_SOURCE_DEVGUI = 0x3,
};

enum DvarFlags : uint16_t
{
    DVAR_USERINFO = 0x2,
    DVAR_SERVERINFO = 0x4,
    DVAR_SYSTEMINFO = 0x8,
    DVAR_INIT = 0x10,
    DVAR_LATCH = 0x20,
    DVAR_ROM = 0x40,
    DVAR_CHEAT = 0x80,
    DVAR_TEMP = 0x100,
    DVAR_AUTOEXEC = 0x200,
    DVAR_DEVGUI_LATCH = 0x800,
    DVAR_SAVED = 0x1000,
    DVAR_EXTERNAL = 0x4000,
    DVAR_CHANGEABLE_RESET = 0x8000,
};

union DvarValue
{
    DvarValue() { integer = 0; }
    DvarValue(int i) { integer = i; }
    bool enabled;
    int integer;
    uint32_t unsignedInt;
    float value;
    float vector[4];
    uint8_t color[4];
    const char *string;
};

union DvarLimits
{
    DvarLimits()
    {
        integer.min = INT32_MIN;
        integer.max = INT32_MAX;
    }
    DvarLimits(int min, int max)
    {
        integer.min = min;
        integer.max = max;
    }
    DvarLimits(float min, float max)
    {
        value.min = min;
        value.max = max;
    }
    struct
    {
        int stringCount;
        const char **strings;
    } enumeration;
    struct
    {
        int min;
        int max;
    } integer;
    struct
    {
        float min;
        float max;
    } value;
};

struct dvar_s
{
    const char *name;
    const char *description;
    uint16_t flags;
    uint8_t type;
    bool modified;
    DvarValue current;
    DvarValue latched;
    DvarValue reset;
    DvarLimits domain;
    bool (__cdecl *domainFunc)(dvar_s *, DvarValue);
    dvar_s *hashNext;
};

using dvar_t = dvar_s;

constexpr uint32_t kMaxDvars = 4096;
constexpr uint32_t kDvarHashBuckets = 0x100;

extern int dvar_modifiedFlags;
extern const dvar_s *dvar_cheats;

extern "C" void Switch_DvarAssertFailed(const char *message) __attribute__((weak));

uint32_t __cdecl Dvar_Count();
const dvar_s *__cdecl Dvar_RegisterBool(const char *dvarName, bool value, uint16_t flags, const char *description);
const dvar_s *__cdecl Dvar_RegisterInt(
    const char *dvarName, int value, int min, int max, uint16_t flags, const char *description);
const dvar_s *__cdecl Dvar_RegisterInt(
    const char *dvarName, int value, DvarLimits domain, uint16_t flags, const char *description);
const dvar_s *__cdecl Dvar_RegisterFloat(
    const char *dvarName, float value, float min, float max, uint16_t flags, const char *description);
const dvar_s *__cdecl Dvar_RegisterString(
    const char *dvarName, const char *value, uint16_t flags, const char *description);
dvar_s *__cdecl Dvar_FindMalleableVar(const char *dvarName);
const dvar_s *__cdecl Dvar_FindVar(const char *dvarName);
bool __cdecl Dvar_GetBool(const char *dvarName);
int __cdecl Dvar_GetInt(const char *dvarName);
double __cdecl Dvar_GetFloat(const char *dvarName);
const char *__cdecl Dvar_GetString(const char *dvarName);
void __cdecl Dvar_SetBoolByName(const char *dvarName, bool value);
void __cdecl Dvar_SetIntByName(const char *dvarName, int value);
void __cdecl Dvar_SetFloatByName(const char *dvarName, float value);
void __cdecl Dvar_SetStringByName(const char *dvarName, char *value);
void __cdecl Dvar_SetBool(dvar_s *dvar, bool value);
void __cdecl Dvar_SetInt(dvar_s *dvar, int value);
void __cdecl Dvar_SetFloat(dvar_s *dvar, float value);
void __cdecl Dvar_SetString(dvar_s *dvar, char *value);
void __cdecl Dvar_SetBoolFromSource(dvar_s *dvar, bool value, DvarSetSource source);
void __cdecl Dvar_SetIntFromSource(dvar_s *dvar, int value, DvarSetSource source);
void __cdecl Dvar_SetFloatFromSource(dvar_s *dvar, float value, DvarSetSource source);
void __cdecl Dvar_SetStringFromSource(dvar_s *dvar, char *value, DvarSetSource source);
void __cdecl Dvar_SetVariant(dvar_s *dvar, DvarValue value, DvarSetSource source);
void __cdecl Dvar_SetLatchedValue(dvar_s *dvar, DvarValue value);
void __cdecl Dvar_SetModified(dvar_s *dvar);
void __cdecl Dvar_ClearModified(dvar_s *dvar);
bool __cdecl Dvar_HasLatchedValue(const dvar_s *dvar);
bool __cdecl Dvar_ValuesEqual(uint8_t type, DvarValue val0, DvarValue val1);
bool __cdecl Dvar_ValueInDomain(uint8_t type, DvarValue value, DvarLimits domain);
void __cdecl Dvar_SetDomainFunc(dvar_s *dvar, bool (__cdecl *domainFunc)(dvar_s *, DvarValue));

#endif
