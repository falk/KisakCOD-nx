#include "switch_dvar.h"

#ifdef __SWITCH__
#include "switch_platform.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace
{
dvar_s s_dvar_pool[kMaxDvars];
dvar_s *s_dvar_hash_table[kDvarHashBuckets];
uint32_t s_dvar_count;
constexpr size_t kStringStorageSize = 1024 * 1024;
char s_string_storage[kStringStorageSize];
size_t s_string_storage_used;

void Report(const char *message)
{
    Sys_Print(message);
    Sys_Print("\n");
}

[[noreturn]] void Fail(const char *message)
{
    Report(message);
    if (Switch_DvarAssertFailed != NULL)
        Switch_DvarAssertFailed(message);
    std::abort();
}

int HashName(const char *name)
{
    if (name == NULL)
        Fail("generateHashValue: null name");
    int hash = 0;
    for (int i = 0; name[i] != '\0'; ++i)
        hash += tolower(static_cast<unsigned char>(name[i])) * (i + 119);
    return static_cast<uint8_t>(hash);
}

bool NamesEqual(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        const int ca = tolower(static_cast<unsigned char>(*a));
        const int cb = tolower(static_cast<unsigned char>(*b));
        if (ca != cb)
            return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

bool TypeSupported(uint8_t type)
{
    return type == DVAR_TYPE_BOOL || type == DVAR_TYPE_FLOAT || type == DVAR_TYPE_INT ||
        type == DVAR_TYPE_STRING;
}

const char *CopyString(const char *value)
{
    if (value == NULL)
        Fail("Dvar string value is null");
    const size_t size = std::strlen(value) + 1;
    if (size > kStringStorageSize - s_string_storage_used)
        Fail("Dvar string storage exhausted");
    char *owned = &s_string_storage[s_string_storage_used];
    std::memcpy(owned, value, size);
    s_string_storage_used += size;
    return owned;
}

const char *OwnStringValue(const dvar_s *dvar, const char *value)
{
    if (dvar->current.string != NULL && std::strcmp(dvar->current.string, value) == 0)
        return dvar->current.string;
    if (dvar->latched.string != NULL && std::strcmp(dvar->latched.string, value) == 0)
        return dvar->latched.string;
    if (dvar->reset.string != NULL && std::strcmp(dvar->reset.string, value) == 0)
        return dvar->reset.string;
    return CopyString(value);
}

bool DomainEquals(uint8_t type, DvarLimits a, DvarLimits b)
{
    if (type == DVAR_TYPE_INT)
        return a.integer.min == b.integer.min && a.integer.max == b.integer.max;
    if (type == DVAR_TYPE_FLOAT)
        return a.value.min == b.value.min && a.value.max == b.value.max;
    return true;
}
}

int dvar_modifiedFlags;
const dvar_s *dvar_cheats;

uint32_t __cdecl Dvar_Count()
{
    return s_dvar_count;
}

bool __cdecl Dvar_ValuesEqual(uint8_t type, DvarValue val0, DvarValue val1)
{
    switch (type)
    {
    case DVAR_TYPE_BOOL:
        return val0.color[0] == val1.color[0];
    case DVAR_TYPE_FLOAT:
        return val0.value == val1.value;
    case DVAR_TYPE_INT:
    case DVAR_TYPE_ENUM:
    case DVAR_TYPE_COLOR:
        return val0.integer == val1.integer;
    case DVAR_TYPE_STRING:
        return val0.string != NULL && val1.string != NULL && std::strcmp(val0.string, val1.string) == 0;
    default:
        Fail("Dvar_ValuesEqual: unhandled dvar type");
    }
}

bool __cdecl Dvar_ValueInDomain(uint8_t type, DvarValue value, DvarLimits domain)
{
    switch (type)
    {
    case DVAR_TYPE_BOOL:
    case DVAR_TYPE_STRING:
        return true;
    case DVAR_TYPE_FLOAT:
        return domain.value.min <= value.value && domain.value.max >= value.value;
    case DVAR_TYPE_INT:
        if (domain.integer.min > domain.integer.max)
            Fail("Dvar_ValueInDomain: domain.integer.min <= domain.integer.max");
        return value.integer >= domain.integer.min && value.integer <= domain.integer.max;
    default:
        Fail("Dvar_ValueInDomain: unhandled dvar type");
    }
}

dvar_s *__cdecl Dvar_FindMalleableVar(const char *dvarName)
{
    for (dvar_s *var = s_dvar_hash_table[HashName(dvarName)]; var != NULL; var = var->hashNext)
    {
        if (NamesEqual(dvarName, var->name))
            return var;
    }
    return NULL;
}

const dvar_s *__cdecl Dvar_FindVar(const char *dvarName)
{
    return Dvar_FindMalleableVar(dvarName);
}

namespace
{
const dvar_s *RegisterNew(
    const char *dvarName, uint8_t type, uint16_t flags, DvarValue value, DvarLimits domain,
    const char *description)
{
    if (s_dvar_count >= kMaxDvars)
        Fail("Dvar_RegisterNew: dvar pool exhausted");
    if (!TypeSupported(type))
        Fail("Dvar_RegisterNew: dvar type is not ported");
    if ((flags & DVAR_EXTERNAL) != 0)
        Fail("Dvar_RegisterNew: external name strings are not ported");
    dvar_s *dvar = &s_dvar_pool[s_dvar_count++];
    dvar->type = type;
    dvar->name = dvarName;
    dvar->current = value;
    dvar->latched = value;
    dvar->reset = value;
    dvar->domain = domain;
    dvar->modified = false;
    dvar->domainFunc = NULL;
    dvar->flags = flags;
    dvar->description = description;
    const int hash = HashName(dvarName);
    dvar->hashNext = s_dvar_hash_table[hash];
    s_dvar_hash_table[hash] = dvar;
    return dvar;
}
}

const dvar_s *__cdecl Dvar_RegisterBool(const char *dvarName, bool value, uint16_t flags, const char *description)
{
    DvarValue dvar_value;
    dvar_value.enabled = value;
    dvar_s *existing = Dvar_FindMalleableVar(dvarName);
    if (existing != NULL)
    {
        if (existing->type != DVAR_TYPE_BOOL || existing->flags != flags ||
            !Dvar_ValuesEqual(DVAR_TYPE_BOOL, existing->reset, dvar_value))
        {
            Fail("Dvar_RegisterBool: conflicting re-registration");
        }
        return existing;
    }
    return RegisterNew(dvarName, DVAR_TYPE_BOOL, flags, dvar_value, DvarLimits(), description);
}

const dvar_s *__cdecl Dvar_RegisterInt(
    const char *dvarName, int value, int min, int max, uint16_t flags, const char *description)
{
    DvarValue dvar_value;
    dvar_value.integer = value;
    DvarLimits domain;
    domain.integer.min = min;
    domain.integer.max = max;
    dvar_s *existing = Dvar_FindMalleableVar(dvarName);
    if (existing != NULL)
    {
        if (existing->type != DVAR_TYPE_INT || existing->flags != flags ||
            !DomainEquals(DVAR_TYPE_INT, existing->domain, domain) ||
            !Dvar_ValuesEqual(DVAR_TYPE_INT, existing->reset, dvar_value))
        {
            Fail("Dvar_RegisterInt: conflicting re-registration");
        }
        return existing;
    }
    return RegisterNew(dvarName, DVAR_TYPE_INT, flags, dvar_value, domain, description);
}

const dvar_s *__cdecl Dvar_RegisterInt(
    const char *dvarName, int value, DvarLimits domain, uint16_t flags, const char *description)
{
    return Dvar_RegisterInt(dvarName, value, domain.integer.min, domain.integer.max, flags, description);
}

const dvar_s *__cdecl Dvar_RegisterFloat(
    const char *dvarName, float value, float min, float max, uint16_t flags, const char *description)
{
    DvarValue dvar_value;
    dvar_value.value = value;
    DvarLimits domain;
    domain.value.min = min;
    domain.value.max = max;
    dvar_s *existing = Dvar_FindMalleableVar(dvarName);
    if (existing != NULL)
    {
        if (existing->type != DVAR_TYPE_FLOAT || existing->flags != flags ||
            !DomainEquals(DVAR_TYPE_FLOAT, existing->domain, domain) ||
            !Dvar_ValuesEqual(DVAR_TYPE_FLOAT, existing->reset, dvar_value))
        {
            Fail("Dvar_RegisterFloat: conflicting re-registration");
        }
        return existing;
    }
    return RegisterNew(dvarName, DVAR_TYPE_FLOAT, flags, dvar_value, domain, description);
}

const dvar_s *__cdecl Dvar_RegisterString(
    const char *dvarName, const char *value, uint16_t flags, const char *description)
{
    if (value == NULL)
        Fail("Dvar_RegisterString: value");
    dvar_s *existing = Dvar_FindMalleableVar(dvarName);
    DvarValue dvar_value;
    dvar_value.string = value;
    if (existing != NULL)
    {
        if (existing->type != DVAR_TYPE_STRING || existing->flags != flags ||
            !Dvar_ValuesEqual(DVAR_TYPE_STRING, existing->reset, dvar_value))
        {
            Fail("Dvar_RegisterString: conflicting re-registration");
        }
        return existing;
    }
    dvar_value.string = CopyString(value);
    return RegisterNew(dvarName, DVAR_TYPE_STRING, flags, dvar_value, DvarLimits(), description);
}

void __cdecl Dvar_SetLatchedValue(dvar_s *dvar, DvarValue value)
{
    if (Dvar_ValuesEqual(dvar->type, dvar->latched, value))
        return;
    switch (dvar->type)
    {
    case DVAR_TYPE_BOOL:
    case DVAR_TYPE_FLOAT:
    case DVAR_TYPE_INT:
        dvar->latched = value;
        break;
    case DVAR_TYPE_STRING:
        dvar->latched.string = OwnStringValue(dvar, value.string);
        break;
    default:
        Fail("Dvar_SetLatchedValue: dvar type is not ported");
    }
}

void __cdecl Dvar_SetVariant(dvar_s *dvar, DvarValue value, DvarSetSource source)
{
    if (dvar == NULL || dvar->name == NULL)
        Fail("Dvar_SetVariant: dvar");
    if (!TypeSupported(dvar->type))
        Fail("Dvar_SetVariant: dvar type is not ported");
    if (!Dvar_ValueInDomain(dvar->type, value, dvar->domain))
    {
        Report("value is not in the dvar domain");
        return;
    }
    if (dvar->domainFunc != NULL && !dvar->domainFunc(dvar, value))
    {
        Report("value is rejected by the dvar domain callback");
        return;
    }
    if (source == DVAR_SOURCE_EXTERNAL || source == DVAR_SOURCE_SCRIPT)
    {
        if ((dvar->flags & DVAR_ROM) != 0)
        {
            Report("dvar is read only");
            return;
        }
        if ((dvar->flags & DVAR_INIT) != 0)
        {
            Report("dvar is write protected");
            return;
        }
        if (source == DVAR_SOURCE_EXTERNAL && (dvar->flags & DVAR_CHEAT) != 0)
        {
            if (dvar_cheats == NULL)
                Fail("Dvar_SetVariant: cheat gate requires the sv_cheats dvar");
            if (!dvar_cheats->current.enabled)
            {
                Report("dvar is cheat protected");
                return;
            }
        }
        if ((dvar->flags & DVAR_LATCH) != 0)
        {
            Dvar_SetLatchedValue(dvar, value);
            return;
        }
    }
    else if (source == DVAR_SOURCE_DEVGUI && (dvar->flags & DVAR_DEVGUI_LATCH) != 0)
    {
        Dvar_SetLatchedValue(dvar, value);
        return;
    }
    if (Dvar_ValuesEqual(dvar->type, dvar->current, value))
    {
        Dvar_SetLatchedValue(dvar, dvar->current);
        return;
    }
    dvar_modifiedFlags |= dvar->flags;
    if (dvar->type == DVAR_TYPE_STRING)
        value.string = OwnStringValue(dvar, value.string);
    dvar->current = value;
    dvar->latched = value;
    dvar->modified = true;
}

void __cdecl Dvar_SetBoolFromSource(dvar_s *dvar, bool value, DvarSetSource source)
{
    if (dvar == NULL || dvar->name == NULL)
        Fail("Dvar_SetBoolFromSource: dvar");
    if (dvar->type != DVAR_TYPE_BOOL)
        Fail("Dvar_SetBoolFromSource: dvar type is not bool");
    DvarValue new_value;
    new_value.enabled = value;
    Dvar_SetVariant(dvar, new_value, source);
}

void __cdecl Dvar_SetIntFromSource(dvar_s *dvar, int value, DvarSetSource source)
{
    if (dvar == NULL || dvar->name == NULL)
        Fail("Dvar_SetIntFromSource: dvar");
    if (dvar->type != DVAR_TYPE_INT && dvar->type != DVAR_TYPE_ENUM)
        Fail("Dvar_SetIntFromSource: dvar type is not int");
    DvarValue new_value;
    new_value.integer = value;
    Dvar_SetVariant(dvar, new_value, source);
}

void __cdecl Dvar_SetFloatFromSource(dvar_s *dvar, float value, DvarSetSource source)
{
    if (dvar == NULL || dvar->name == NULL)
        Fail("Dvar_SetFloatFromSource: dvar");
    if (dvar->type != DVAR_TYPE_FLOAT)
        Fail("Dvar_SetFloatFromSource: dvar type is not float");
    DvarValue new_value;
    new_value.value = value;
    Dvar_SetVariant(dvar, new_value, source);
}

void __cdecl Dvar_SetStringFromSource(dvar_s *dvar, char *value, DvarSetSource source)
{
    if (dvar == NULL || dvar->name == NULL)
        Fail("Dvar_SetStringFromSource: dvar");
    if (dvar->type != DVAR_TYPE_STRING)
        Fail("Dvar_SetStringFromSource: dvar type is not string");
    DvarValue new_value;
    new_value.string = value;
    Dvar_SetVariant(dvar, new_value, source);
}

void __cdecl Dvar_SetBool(dvar_s *dvar, bool value)
{
    Dvar_SetBoolFromSource(dvar, value, DVAR_SOURCE_INTERNAL);
}

void __cdecl Dvar_SetInt(dvar_s *dvar, int value)
{
    Dvar_SetIntFromSource(dvar, value, DVAR_SOURCE_INTERNAL);
}

void __cdecl Dvar_SetFloat(dvar_s *dvar, float value)
{
    Dvar_SetFloatFromSource(dvar, value, DVAR_SOURCE_INTERNAL);
}

void __cdecl Dvar_SetString(dvar_s *dvar, char *value)
{
    Dvar_SetStringFromSource(dvar, value, DVAR_SOURCE_INTERNAL);
}

void __cdecl Dvar_SetBoolByName(const char *dvarName, bool value)
{
    dvar_s *dvar = Dvar_FindMalleableVar(dvarName);
    if (dvar == NULL)
        Fail("Dvar_SetBoolByName: external dvar registration is not ported");
    Dvar_SetBool(dvar, value);
}

void __cdecl Dvar_SetIntByName(const char *dvarName, int value)
{
    dvar_s *dvar = Dvar_FindMalleableVar(dvarName);
    if (dvar == NULL)
        Fail("Dvar_SetIntByName: external dvar registration is not ported");
    Dvar_SetInt(dvar, value);
}

void __cdecl Dvar_SetFloatByName(const char *dvarName, float value)
{
    dvar_s *dvar = Dvar_FindMalleableVar(dvarName);
    if (dvar == NULL)
        Fail("Dvar_SetFloatByName: external dvar registration is not ported");
    Dvar_SetFloat(dvar, value);
}

void __cdecl Dvar_SetStringByName(const char *dvarName, char *value)
{
    dvar_s *dvar = Dvar_FindMalleableVar(dvarName);
    if (dvar == NULL)
        Fail("Dvar_SetStringByName: external dvar registration is not ported");
    Dvar_SetString(dvar, value);
}

bool __cdecl Dvar_GetBool(const char *dvarName)
{
    const dvar_s *dvar = Dvar_FindVar(dvarName);
    if (dvar == NULL)
        return false;
    if (dvar->type != DVAR_TYPE_BOOL)
        Fail("Dvar_GetBool: dvar type is not bool");
    return dvar->current.enabled;
}

int __cdecl Dvar_GetInt(const char *dvarName)
{
    const dvar_s *dvar = Dvar_FindVar(dvarName);
    if (dvar == NULL)
        return 0;
    if (dvar->type != DVAR_TYPE_INT && dvar->type != DVAR_TYPE_ENUM)
        Fail("Dvar_GetInt: dvar type is not int");
    return dvar->current.integer;
}

double __cdecl Dvar_GetFloat(const char *dvarName)
{
    const dvar_s *dvar = Dvar_FindVar(dvarName);
    if (dvar == NULL)
        return 0.0;
    if (dvar->type != DVAR_TYPE_FLOAT)
        Fail("Dvar_GetFloat: dvar type is not float");
    return dvar->current.value;
}

const char *__cdecl Dvar_GetString(const char *dvarName)
{
    const dvar_s *dvar = Dvar_FindVar(dvarName);
    if (dvar == NULL)
        return "";
    if (dvar->type != DVAR_TYPE_STRING)
        Fail("Dvar_GetString: dvar type is not string");
    return dvar->current.string;
}

void __cdecl Dvar_SetDomainFunc(dvar_s *dvar, bool (__cdecl *domainFunc)(dvar_s *, DvarValue))
{
    if (dvar == NULL)
        Fail("Dvar_SetDomainFunc: dvar");
    dvar->domainFunc = domainFunc;
}

void __cdecl Dvar_SetModified(dvar_s *dvar)
{
    dvar_modifiedFlags |= dvar->flags;
    dvar->modified = true;
}

void __cdecl Dvar_ClearModified(dvar_s *dvar)
{
    dvar->modified = false;
}

bool __cdecl Dvar_HasLatchedValue(const dvar_s *dvar)
{
    return !Dvar_ValuesEqual(dvar->type, dvar->current, dvar->latched);
}

#endif
