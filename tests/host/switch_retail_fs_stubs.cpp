#include "src/qcommon/qcommon.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct HunkUser;

const dvar_t *useFastFile;

void Com_Printf(int, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    std::vprintf(fmt, va);
    va_end(va);
}
void Com_DPrintf(int, const char *, ...) {}
void Com_PrintWarning(int, const char *, ...) {}

void Com_Error(errorParm_t, const char *, ...)
{
    std::abort();
}

void MyAssertHandler(const char *, int, int, const char *, ...)
{
    std::abort();
}

void Com_Memset(void *dest, const int value, const size_t count)
{
    std::memset(dest, value, count);
}

void Com_Memcpy(void *dest, const void *source, const size_t count)
{
    std::memcpy(dest, source, count);
}

void *Z_Malloc(int size, const char *, int)
{
    void *memory = std::calloc(1, static_cast<size_t>(size));
    if (!memory)
        std::abort();
    return memory;
}

void Z_Free(void *memory, int)
{
    std::free(memory);
}

int Com_BlockChecksumKey32(const uint8_t *data, uint32_t length, uint32_t initialCrc)
{
    uint32_t hash = initialCrc;
    for (uint32_t i = 0; i < length; ++i)
        hash = hash * 33u + data[i];
    return static_cast<int>(hash);
}

void ProfLoad_BeginTrackedValue(MapProfileTrackedValue) {}
void ProfLoad_EndTrackedValue(MapProfileTrackedValue) {}

bool Sys_IsMainThread() { return true; }
bool Sys_IsRenderThread() { return false; }
bool Sys_IsDatabaseThread() { return false; }

int SEH_GetCurrentLanguage() { return 0; }
const char *SEH_GetLanguageName(unsigned int) { return "english"; }
bool SEH_GetLanguageIndexForName(const char *name, int *index)
{
    if (std::strcmp(name, "english") != 0)
        return false;
    *index = 0;
    return true;
}

char *SEH_SafeTranslateString(char *value) { return value; }

int I_strnicmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; ++i)
    {
        const unsigned char ca = static_cast<unsigned char>(a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 'a' - 'A' : a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 'a' - 'A' : b[i]);
        if (ca != cb || ca == 0)
            return static_cast<int>(ca) - static_cast<int>(cb);
    }
    return 0;
}
int I_stricmp(const char *a, const char *b) { return I_strnicmp(a, b, 0x7fffffff); }
int I_strncmp(const char *a, const char *b, int n) { return std::strncmp(a, b, static_cast<size_t>(n)); }
char *I_strlwr(char *value)
{
    for (char *c = value; *c; ++c)
        if (*c >= 'A' && *c <= 'Z')
            *c += 'a' - 'A';
    return value;
}

void I_strncpyz(char *dest, const char *source, int size)
{
    if (size <= 0)
        return;
    std::strncpy(dest, source, static_cast<size_t>(size) - 1);
    dest[size - 1] = '\0';
}

bool I_islower(int c) { return c >= 'a' && c <= 'z'; }
const char *Com_GetExtensionSubString(const char *filename)
{
    const char *dot = std::strrchr(filename, '.');
    return dot ? dot : filename + std::strlen(filename);
}

char *va(const char *format, ...)
{
    static char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

int Com_sprintf(char *dest, unsigned int size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = std::vsnprintf(dest, size, format, args);
    va_end(args);
    return result;
}

void Hunk_UserDestroy(HunkUser *) {}
