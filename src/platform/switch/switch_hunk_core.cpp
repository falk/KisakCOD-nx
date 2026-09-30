#include "switch_hunk_core.h"

#ifdef __SWITCH__
#include "switch_platform.h"

#include <cstring>
#include <limits>

extern void* Z_Malloc(int size, const char* name, int type) __attribute__((weak));
extern void Z_Free(void* ptr, int type) __attribute__((weak));

namespace
{
constexpr size_t kPageSize = 4096;
constexpr uint32_t kTempMagic = 0x89537892;

struct hunkHeader_t
{
    uint32_t magic;
    int size;
    const char* name;
    int dummy;
    uint64_t padding;
};

static_assert(sizeof(hunkHeader_t) == 32, "LP64 temp hunk headers must preserve 16-byte payload alignment");

bool Add(uintptr_t value, size_t bytes, uintptr_t* result)
{
    if (bytes > UINTPTR_MAX - value)
        return false;
    *result = value + bytes;
    return true;
}

bool AlignUp(uintptr_t value, size_t alignment, uintptr_t* result)
{
    const uintptr_t mask = static_cast<uintptr_t>(alignment - 1);
    if (value > UINTPTR_MAX - mask)
        return false;
    *result = (value + mask) & ~mask;
    return true;
}

uintptr_t AlignDown(uintptr_t value, size_t alignment)
{
    return value & ~static_cast<uintptr_t>(alignment - 1);
}

bool PageRange(void* address, int bytes, uintptr_t* begin, size_t* length)
{
    uintptr_t end;
    uintptr_t rounded_end;
    if (address == NULL || bytes < 0 || !Add(reinterpret_cast<uintptr_t>(address), static_cast<size_t>(bytes), &end))
        return false;
    *begin = AlignDown(reinterpret_cast<uintptr_t>(address), kPageSize);
    return AlignUp(end, kPageSize, &rounded_end) && rounded_end >= *begin &&
        (*length = static_cast<size_t>(rounded_end - *begin), true);
}

bool ValidAlignment(int alignment)
{
    return alignment >= 1 && alignment <= 4096 && (alignment & (alignment - 1)) == 0;
}

void Report(const char* message)
{
    Sys_Print(message);
    Sys_Print("\n");
}

void Fatal(const char* message)
{
    if (Switch_HunkCoreFatal != NULL)
        Switch_HunkCoreFatal(message);
    else
        Report(message);
}

void Drop(const char* message)
{
    if (Switch_HunkCoreDrop != NULL)
        Switch_HunkCoreDrop(message);
    else
        Report(message);
}

void OutOfMemory(const char* message)
{
    if (Switch_HunkCoreOutOfMemory != NULL)
        Switch_HunkCoreOutOfMemory(message);
    else
        Report(message);
}

bool CheckThread(int allowRenderThread)
{
    if (Switch_HunkCoreThreadCheck == NULL)
    {
        Report("Switch hunk core thread hook is unavailable");
        return false;
    }
    return Switch_HunkCoreThreadCheck(allowRenderThread) != 0;
}

void Track(const char* event, int amount, const char* name, int type)
{
    if (Switch_HunkCoreTrack != NULL)
        Switch_HunkCoreTrack(event, amount, name, type);
}

bool HunkReady()
{
    if (s_hunkData == NULL || s_hunkTotal <= 0)
    {
        Fatal("Switch hunk core is not initialized");
        return false;
    }
    return true;
}

bool HunkRange(int low, int high)
{
    return low >= 0 && high >= 0 && low <= s_hunkTotal && high <= s_hunkTotal - low;
}

bool CommitLow(uintptr_t old_end, uintptr_t new_end)
{
    uintptr_t begin;
    uintptr_t end;
    return AlignUp(old_end, kPageSize, &begin) && AlignUp(new_end, kPageSize, &end) && end >= begin &&
        (end == begin || Z_TryVirtualCommitInternal(reinterpret_cast<void*>(begin), static_cast<int>(end - begin)));
}

bool CommitHigh(uintptr_t new_begin, uintptr_t old_begin)
{
    const uintptr_t begin = AlignDown(new_begin, kPageSize);
    const uintptr_t end = AlignDown(old_begin, kPageSize);
    return end >= begin &&
        (end == begin || Z_TryVirtualCommitInternal(reinterpret_cast<void*>(begin), static_cast<int>(end - begin)));
}

void DecommitLow(uintptr_t old_end, uintptr_t new_end)
{
    uintptr_t begin;
    uintptr_t end;
    if (AlignUp(new_end, kPageSize, &begin) && AlignUp(old_end, kPageSize, &end) && end > begin)
        Z_VirtualDecommit(reinterpret_cast<void*>(begin), static_cast<int>(end - begin));
}

void DecommitHigh(uintptr_t old_begin, uintptr_t new_begin)
{
    const uintptr_t begin = AlignDown(old_begin, kPageSize);
    const uintptr_t end = AlignDown(new_begin, kPageSize);
    if (end > begin)
        Z_VirtualDecommit(reinterpret_cast<void*>(begin), static_cast<int>(end - begin));
}
}

/* Shared with com_memory.cpp's retained Hunk_ClearData lifecycle boundary. */
hunkUsed_t hunk_high;
hunkUsed_t hunk_low;
unsigned char* s_hunkData;
uint8_t* s_origHunkData;
int s_hunkTotal;

void* __cdecl Z_VirtualReserve(int size)
{
    uintptr_t rounded;
    if (size <= 0 || !AlignUp(static_cast<uintptr_t>(size), kPageSize, &rounded) || rounded > SIZE_MAX)
    {
        Fatal("Z_VirtualReserve: invalid size");
        return NULL;
    }
    void* result = Switch_VirtualReserve(static_cast<size_t>(rounded));
    if (result == NULL)
        Fatal("Z_VirtualReserve: reserve failed");
    return result;
}

void __cdecl Z_VirtualDecommitInternal(void* ptr, int size)
{
    uintptr_t begin;
    size_t length;
    if (!PageRange(ptr, size, &begin, &length) || (length != 0 && !Switch_VirtualDecommit(reinterpret_cast<void*>(begin), length)))
        Fatal("Z_VirtualDecommit: invalid range");
}

void __cdecl Z_VirtualFreeInternal(void* ptr)
{
    if (ptr == NULL || !Switch_VirtualRelease(ptr))
        Fatal("Z_VirtualFree: invalid reservation");
}

void* __cdecl Z_TryVirtualAllocInternal(int size)
{
    void* ptr = Z_VirtualReserve(size);
    if (ptr != NULL && Z_TryVirtualCommitInternal(ptr, size))
        return ptr;
    if (ptr != NULL)
        Z_VirtualFreeInternal(ptr);
    return NULL;
}

bool __cdecl Z_TryVirtualCommitInternal(void* ptr, int size)
{
    uintptr_t begin;
    size_t length;
    return PageRange(ptr, size, &begin, &length) &&
        (length == 0 || Switch_VirtualCommit(reinterpret_cast<void*>(begin), length) != 0);
}

void __cdecl Z_VirtualCommitInternal(void* ptr, int size)
{
    if (!Z_TryVirtualCommitInternal(ptr, size))
        OutOfMemory("Z_VirtualCommit: commit failed");
}

void __cdecl Z_VirtualFree(void* ptr) { Z_VirtualFreeInternal(ptr); }
void __cdecl Z_VirtualDecommit(void* ptr, int size) { Z_VirtualDecommitInternal(ptr, size); }

char* __cdecl Z_TryVirtualAlloc(int size, const char* name, int type)
{
    char* result = static_cast<char*>(Z_TryVirtualAllocInternal(size));
    if (result != NULL)
    {
        uintptr_t rounded;
        if (AlignUp(static_cast<uintptr_t>(size), kPageSize, &rounded))
            Track("z_commit", static_cast<int>(rounded), name, type);
    }
    return result;
}

char* __cdecl Z_VirtualAlloc(int size, const char* name, int type)
{
    char* result = Z_TryVirtualAlloc(size, name, type);
    if (result == NULL)
        OutOfMemory("Z_VirtualAlloc: allocation failed");
    return result;
}

void __cdecl Z_VirtualCommit(void* ptr, int size) { Z_VirtualCommitInternal(ptr, size); }

void __cdecl Hunk_ClearToMarkLow(int mark)
{
    if (!CheckThread(0) || !HunkReady() || mark < 0 || mark > hunk_low.permanent || hunk_low.temp != hunk_low.permanent)
    {
        Fatal("Hunk_ClearToMarkLow: invalid mark or outstanding temp memory");
        return;
    }
    const uintptr_t old_end = reinterpret_cast<uintptr_t>(s_hunkData) + static_cast<size_t>(hunk_low.temp);
    hunk_low.permanent = hunk_low.temp = mark;
    Hunk_ClearData();
    DecommitLow(old_end, reinterpret_cast<uintptr_t>(s_hunkData) + static_cast<size_t>(mark));
    Track("hunk_clear_low", mark, NULL, 0);
}

void Hunk_Clear()
{
    if (!CheckThread(0) || !HunkReady())
        return;
    hunk_low.permanent = hunk_low.temp = 0;
    hunk_high.permanent = hunk_high.temp = 0;
    Hunk_ClearData();
    Z_VirtualDecommit(s_hunkData, s_hunkTotal);
    Track("hunk_clear", 0, NULL, 0);
}

int __cdecl Hunk_Used()
{
    if (!CheckThread(1) || !HunkReady())
        return 0;
    return hunk_high.permanent + hunk_low.permanent;
}

uint8_t* __cdecl Hunk_Alloc(uint32_t size, const char* name, int type) { return Hunk_AllocAlign(size, 32, name, type); }

uint8_t* __cdecl Hunk_AllocAlign(uint32_t size, int alignment, const char* name, int type)
{
    if (!CheckThread(0) || !HunkReady() || !ValidAlignment(alignment) || hunk_high.temp != hunk_high.permanent)
    {
        Fatal("Hunk_AllocAlign: invalid alignment, hunk, or outstanding temp memory");
        return NULL;
    }
    uintptr_t total;
    if (!AlignUp(static_cast<uintptr_t>(hunk_high.permanent) + size, static_cast<size_t>(alignment), &total) ||
        total > static_cast<uintptr_t>(s_hunkTotal) || !HunkRange(hunk_low.temp, static_cast<int>(total)))
    {
        Drop("Hunk_AllocAlign: hunk collision");
        return NULL;
    }
    const int old = hunk_high.permanent;
    const uintptr_t old_begin = reinterpret_cast<uintptr_t>(s_hunkData) + static_cast<size_t>(s_hunkTotal - old);
    const uintptr_t result = reinterpret_cast<uintptr_t>(s_hunkData) + static_cast<size_t>(s_hunkTotal - total);
    if (!CommitHigh(result, old_begin))
    {
        OutOfMemory("Hunk_AllocAlign: commit failed");
        return NULL;
    }
    hunk_high.permanent = hunk_high.temp = static_cast<int>(total);
    std::memset(reinterpret_cast<void*>(result), 0, size);
    Track("hunk_high", hunk_high.permanent - old, name, type);
    return reinterpret_cast<uint8_t*>(result);
}

uintptr_t __cdecl Hunk_AllocateTempMemoryHigh(int size, const char* name)
{
    if (!CheckThread(0) || !HunkReady() || size < 0)
    {
        Fatal("Hunk_AllocateTempMemoryHigh: invalid size or hunk");
        return 0;
    }
    uintptr_t total;
    if (!AlignUp(static_cast<uintptr_t>(hunk_high.temp) + static_cast<size_t>(size), 16, &total) || total > static_cast<uintptr_t>(s_hunkTotal) ||
        !HunkRange(hunk_low.temp, static_cast<int>(total)))
    {
        Drop("Hunk_AllocateTempMemoryHigh: hunk collision");
        return 0;
    }
    const uintptr_t old_begin = reinterpret_cast<uintptr_t>(s_hunkData) + static_cast<size_t>(s_hunkTotal - hunk_high.temp);
    const uintptr_t result = reinterpret_cast<uintptr_t>(s_hunkData) + static_cast<size_t>(s_hunkTotal - total);
    if (!CommitHigh(result, old_begin))
    {
        OutOfMemory("Hunk_AllocateTempMemoryHigh: commit failed");
        return 0;
    }
    hunk_high.temp = static_cast<int>(total);
    std::memset(reinterpret_cast<void*>(result), 0, static_cast<size_t>(size));
    Track("temp_high", size, name, 0);
    return result;
}

void Hunk_ClearTempMemoryHigh()
{
    if (!CheckThread(0) || !HunkReady()) return;
    const uintptr_t old_begin = reinterpret_cast<uintptr_t>(s_hunkData) + static_cast<size_t>(s_hunkTotal - hunk_high.temp);
    const uintptr_t new_begin = reinterpret_cast<uintptr_t>(s_hunkData) + static_cast<size_t>(s_hunkTotal - hunk_high.permanent);
    hunk_high.temp = hunk_high.permanent;
    DecommitHigh(old_begin, new_begin);
    Track("temp_high_clear", hunk_high.permanent, NULL, 0);
}

uint8_t* __cdecl Hunk_AllocLow(uint32_t size, const char* name, int type) { return Hunk_AllocLowAlign(size, 32, name, type); }

uint8_t* __cdecl Hunk_AllocLowAlign(uint32_t size, int alignment, const char* name, int type)
{
    if (!CheckThread(0) || !HunkReady() || !ValidAlignment(alignment) || hunk_low.temp != hunk_low.permanent)
    {
        Fatal("Hunk_AllocLowAlign: invalid alignment, hunk, or outstanding temp memory");
        return NULL;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(s_hunkData);
    uintptr_t result;
    uintptr_t end;
    if (!AlignUp(base + static_cast<size_t>(hunk_low.permanent), static_cast<size_t>(alignment), &result) || !Add(result, size, &end) ||
        end - base > static_cast<uintptr_t>(s_hunkTotal) || !HunkRange(static_cast<int>(end - base), hunk_high.temp))
    {
        Drop("Hunk_AllocLowAlign: hunk collision");
        return NULL;
    }
    const int old = hunk_low.permanent;
    if (!CommitLow(base + static_cast<size_t>(old), end))
    {
        OutOfMemory("Hunk_AllocLowAlign: commit failed");
        return NULL;
    }
    hunk_low.permanent = hunk_low.temp = static_cast<int>(end - base);
    std::memset(reinterpret_cast<void*>(result), 0, size);
    Track("hunk_low", hunk_low.permanent - old, name, type);
    return reinterpret_cast<uint8_t*>(result);
}

uint32_t* __cdecl Hunk_AllocateTempMemory(int size, const char* name)
{
    if (!CheckThread(0) || size < 0) { Fatal("Hunk_AllocateTempMemory: invalid size or hunk"); return NULL; }
    if (s_hunkData == NULL || s_hunkTotal <= 0)
    {
        if (Z_Malloc != NULL)
            return reinterpret_cast<uint32_t*>(Z_Malloc(size, name, 10));
        Fatal("Switch hunk core is not initialized");
        return NULL;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(s_hunkData);
    const int previous = hunk_low.temp;
    uintptr_t header;
    uintptr_t end;
    if (!AlignUp(base + static_cast<size_t>(previous), 16, &header) || !Add(header, sizeof(hunkHeader_t) + static_cast<size_t>(size), &end) ||
        end - base > static_cast<uintptr_t>(s_hunkTotal) || !HunkRange(static_cast<int>(end - base), hunk_high.temp))
    { Drop("Hunk_AllocateTempMemory: hunk collision"); return NULL; }
    if (!CommitLow(base + static_cast<size_t>(previous), end))
    { OutOfMemory("Hunk_AllocateTempMemory: commit failed"); return NULL; }
    hunk_low.temp = static_cast<int>(end - base);
    hunkHeader_t* hdr = reinterpret_cast<hunkHeader_t*>(header);
    hdr->magic = kTempMagic;
    hdr->size = hunk_low.temp - previous;
    hdr->name = name;
    hdr->dummy = 0;
    hdr->padding = 0;
    std::memset(reinterpret_cast<void*>(header + sizeof(*hdr)), 0, static_cast<size_t>(size));
    Track("temp_low", hdr->size, name, 0);
    return reinterpret_cast<uint32_t*>(header + sizeof(*hdr));
}

void __cdecl Hunk_FreeTempMemory(char* buf)
{
    if (!CheckThread(0) || buf == NULL) { Fatal("Hunk_FreeTempMemory: invalid buffer"); return; }
    const uintptr_t base = reinterpret_cast<uintptr_t>(s_hunkData);
    const uintptr_t buffer = reinterpret_cast<uintptr_t>(buf);
    if (s_hunkData == NULL || s_hunkTotal <= 0 || buffer < base || buffer >= base + static_cast<size_t>(s_hunkTotal))
    {
        if (Z_Free != NULL)
            Z_Free(buf, 10);
        return;
    }
    if (buffer < base + sizeof(hunkHeader_t) || buffer > base + static_cast<size_t>(hunk_low.temp))
    { Fatal("Hunk_FreeTempMemory: invalid buffer"); return; }
    const uintptr_t header = buffer - sizeof(hunkHeader_t);
    hunkHeader_t* hdr = reinterpret_cast<hunkHeader_t*>(header);
    uintptr_t expected;
    if (hdr->magic != kTempMagic || hdr->size <= 0 || hdr->size > hunk_low.temp ||
        !AlignUp(base + static_cast<size_t>(hunk_low.temp - hdr->size), 16, &expected) || header != expected)
    { Fatal("Hunk_FreeTempMemory: non-LIFO or bad header"); return; }
    const uintptr_t old_end = base + static_cast<size_t>(hunk_low.temp);
    hunk_low.temp -= hdr->size;
    hdr->magic++;
    DecommitLow(old_end, base + static_cast<size_t>(hunk_low.temp));
    Track("temp_low_free", hdr->size, hdr->name, 0);
}

void Hunk_ClearTempMemory()
{
    if (!CheckThread(0) || !HunkReady()) return;
    const uintptr_t old_end = reinterpret_cast<uintptr_t>(s_hunkData) + static_cast<size_t>(hunk_low.temp);
    hunk_low.temp = hunk_low.permanent;
    DecommitLow(old_end, reinterpret_cast<uintptr_t>(s_hunkData) + static_cast<size_t>(hunk_low.temp));
    Track("temp_low_clear", hunk_low.permanent, NULL, 0);
}

void Hunk_CheckTempMemoryClear()
{
    if (s_hunkData == NULL || s_hunkTotal <= 0) return;
    if (hunk_low.temp != hunk_low.permanent)
        Fatal("Hunk_CheckTempMemoryClear: outstanding temp memory");
}

void Hunk_CheckTempMemoryHighClear()
{
    if (s_hunkData == NULL || s_hunkTotal <= 0) return;
    if (hunk_high.temp != hunk_high.permanent)
        Fatal("Hunk_CheckTempMemoryHighClear: outstanding high temp memory");
}
#endif
