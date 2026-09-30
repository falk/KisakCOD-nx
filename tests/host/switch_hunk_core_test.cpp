#include "src/platform/switch/switch_hunk_core.h"
#include "src/platform/switch/switch_platform.h"

#include <cstdint>
#include <cstring>

namespace
{
constexpr size_t kPageSize = 4096;
constexpr int kHunkBytes = static_cast<int>(kPageSize * 16);
int g_fatals;
int g_drops;
int g_out_of_memory;
int g_clear_data;

bool IsAligned(const void* address, size_t alignment)
{
    return reinterpret_cast<uintptr_t>(address) % alignment == 0;
}
}

extern "C" void Switch_HunkCoreFatal(const char*) { ++g_fatals; }
extern "C" void Switch_HunkCoreDrop(const char*) { ++g_drops; }
extern "C" void Switch_HunkCoreOutOfMemory(const char*) { ++g_out_of_memory; }
extern "C" int Switch_HunkCoreThreadCheck(int) { return 1; }
extern "C" void Switch_HunkCoreTrack(const char*, int, const char*, int) {}

void Hunk_ClearData() { ++g_clear_data; }

int main()
{
    if (Z_VirtualReserve(0) != nullptr || g_fatals != 1)
        return 1;

    char* z = Z_TryVirtualAlloc(1, "z", 7);
    if (z == nullptr || z[0] != 0 || z[kPageSize - 1] != 0)
        return 1;
    z[0] = 0x5a;
    z[kPageSize - 1] = 0x6b;
    Z_VirtualDecommit(z + 1, 1);
    Z_VirtualCommit(z + 1, 1);
    if (z[0] != 0 || z[kPageSize - 1] != 0)
        return 1;
    Z_VirtualFree(z);

    s_hunkData = static_cast<unsigned char*>(Z_VirtualReserve(kHunkBytes));
    s_origHunkData = s_hunkData;
    s_hunkTotal = kHunkBytes;
    if (s_hunkData == nullptr)
        return 1;
    Hunk_Clear();
    if (g_clear_data != 1 || Hunk_Used() != 0)
        return 1;

    uint8_t* low = Hunk_AllocLowAlign(65, 64, "low", 1);
    uint8_t* low_page = Hunk_AllocLowAlign(8, static_cast<int>(kPageSize), "low-page", 1);
    uint8_t* high = Hunk_AllocAlign(65, 64, "high", 2);
    if (low == nullptr || low_page == nullptr || high == nullptr || !IsAligned(low, 64) || !IsAligned(low_page, kPageSize) ||
        !IsAligned(high, 64) || low[0] != 0 || low_page[0] != 0 || high[0] != 0 || Hunk_Used() <= 0)
        return 1;

    uintptr_t temp_high = Hunk_AllocateTempMemoryHigh(33, "temp-high");
    if (temp_high == 0 || !IsAligned(reinterpret_cast<void*>(temp_high), 16) || *reinterpret_cast<uint8_t*>(temp_high) != 0)
        return 1;
    Hunk_ClearTempMemoryHigh();

    uint32_t* first = Hunk_AllocateTempMemory(31, "first");
    uint32_t* second = Hunk_AllocateTempMemory(33, "second");
    if (first == nullptr || second == nullptr || !IsAligned(first, 16) || !IsAligned(second, 16) || first[0] != 0 || second[0] != 0)
        return 1;
    Hunk_FreeTempMemory(reinterpret_cast<char*>(first));
    if (g_fatals != 2)
        return 1;
    Hunk_FreeTempMemory(reinterpret_cast<char*>(second));
    Hunk_FreeTempMemory(reinterpret_cast<char*>(first));

    uint32_t* bad_magic = Hunk_AllocateTempMemory(16, "bad-magic");
    if (bad_magic == nullptr)
        return 1;
    uint32_t* magic = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(bad_magic) - 32);
    const uint32_t saved_magic = *magic;
    *magic = 0;
    Hunk_FreeTempMemory(reinterpret_cast<char*>(bad_magic));
    if (g_fatals != 3)
        return 1;
    *magic = saved_magic;
    Hunk_FreeTempMemory(reinterpret_cast<char*>(bad_magic));

    if (Hunk_AllocLowAlign(1, 3, "bad-align", 0) != nullptr || g_fatals != 4)
        return 1;
    if (Hunk_AllocLowAlign(static_cast<uint32_t>(kPageSize * 14), 1, "near-full", 0) == nullptr ||
        Hunk_AllocAlign(static_cast<uint32_t>(kPageSize * 2), 1, "collision", 0) != nullptr || g_drops != 1)
        return 1;

    Hunk_Clear();
    if (g_clear_data != 2 || Hunk_Used() != 0 || !Switch_VirtualCommit(s_hunkData, kPageSize) || s_hunkData[0] != 0)
        return 1;
    Z_VirtualDecommit(s_hunkData, static_cast<int>(kPageSize));
    void* reservation = s_hunkData;
    s_hunkData = nullptr;
    s_origHunkData = nullptr;
    s_hunkTotal = 0;
    return Switch_VirtualRelease(reservation) ? 0 : 1;
}
