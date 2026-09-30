#include "src/platform/switch/switch_hunk_user.h"
#include "src/platform/switch/switch_platform.h"

#include <cstdint>
#include <cstring>

namespace
{
constexpr size_t kPageSize = 4096;

bool IsAligned(const void* address, size_t alignment)
{
    return reinterpret_cast<uintptr_t>(address) % alignment == 0;
}
}

int main()
{
    if (Hunk_UserCreate(0, "invalid", true, false, 0) != nullptr ||
        Hunk_UserCreate(1, "invalid", true, false, 0) != nullptr)
        return 1;

    HunkUser* fixed = Hunk_UserCreate(static_cast<int>(kPageSize * 8), "fixed", true, false, 3);
    if (fixed == nullptr || !IsAligned(fixed->buf, 32))
        return 1;

    void* one = Hunk_UserAlloc(fixed, 4032, 1);
    // The script compiler captures positions with zero-length allocations, so
    // Hunk_UserAlloc(user, 0, 1) must succeed even when the bump position has
    // just landed exactly on a page boundary (where the required commit span
    // is zero). This used to return NULL and corrupt every compiled offset
    // that happened to line up on a page.
    void* const zero_at_page_boundary = Hunk_UserAlloc(fixed, 0, 1);
    if (zero_at_page_boundary == nullptr || zero_at_page_boundary != fixed->pos)
        return 1;
    void* four = Hunk_UserAlloc(fixed, 4096, 4);
    void* thirty_two = Hunk_UserAlloc(fixed, 4096, 32);
    void* page = Hunk_UserAlloc(fixed, 4096, 4096);
    if (one == nullptr || four == nullptr || thirty_two == nullptr || page == nullptr ||
        !IsAligned(one, 1) || !IsAligned(four, 4) || !IsAligned(thirty_two, 32) || !IsAligned(page, kPageSize))
        return 1;
    std::memset(one, 0x11, 4032);
    std::memset(four, 0x22, 4096);
    std::memset(thirty_two, 0x33, 4096);
    std::memset(page, 0x44, 4096);

    uint8_t* const mark = fixed->pos;
    void* const after_mark = Hunk_UserAlloc(fixed, 32, 32);
    Hunk_UserSetPos(fixed, mark);
    if (after_mark == nullptr || Hunk_UserAlloc(fixed, 32, 32) != after_mark)
        return 1;
    if (Hunk_UserAlloc(fixed, 1, 3) != nullptr || Hunk_UserAlloc(fixed, UINT32_MAX, 1) != nullptr ||
        Hunk_UserAlloc(nullptr, 1, 1) != nullptr)
        return 1;

    fixed->buf[0] = 0x5a;
    Hunk_UserReset(fixed);
    if (fixed->pos != fixed->buf || fixed->current != fixed || fixed->next != nullptr || fixed->buf[0] != 0)
        return 1;
    if (Hunk_UserAllocAlignStrict(fixed, 16) != fixed->buf)
        return 1;

    void* const fixed_base = fixed;
    Hunk_UserDestroy(fixed);
    if (Switch_VirtualCommit(fixed_base, kPageSize) != 0 || Switch_VirtualRelease(fixed_base) != 0)
        return 1;

    HunkUser* grow = Hunk_UserCreate(static_cast<int>(kPageSize), "grow", false, true, 7);
    if (grow == nullptr || Hunk_UserAlloc(grow, 4000, 1) == nullptr || Hunk_UserAlloc(grow, 128, 1) == nullptr ||
        grow->next == nullptr || grow->current != grow->next)
        return 1;
    HunkUser* const child = grow->next;
    void* const grow_base = grow;
    void* const child_base = child;
    char* const copy = Hunk_CopyString(grow, "mutable");
    if (copy == nullptr || std::strcmp(copy, "mutable") != 0)
        return 1;
    copy[0] = 'M';
    if (std::strcmp(copy, "Mutable") != 0)
        return 1;
    Hunk_UserDestroy(grow);
    if (Switch_VirtualCommit(grow_base, kPageSize) != 0 || Switch_VirtualRelease(grow_base) != 0 ||
        Switch_VirtualCommit(child_base, kPageSize) != 0 || Switch_VirtualRelease(child_base) != 0)
        return 1;

    HunkUser* reset_grow = Hunk_UserCreate(static_cast<int>(kPageSize), "reset-grow", false, false, 0);
    if (reset_grow == nullptr || Hunk_UserAlloc(reset_grow, 4000, 1) == nullptr ||
        Hunk_UserAlloc(reset_grow, 128, 1) == nullptr || reset_grow->next == nullptr)
        return 1;
    void* const reset_base = reset_grow;
    void* const reset_child_base = reset_grow->next;
    Hunk_UserReset(reset_grow);
    if (reset_grow->current != reset_grow || reset_grow->next != nullptr || reset_grow->pos != reset_grow->buf ||
        Switch_VirtualCommit(reset_child_base, kPageSize) != 0 || Switch_VirtualRelease(reset_child_base) != 0)
        return 1;
    Hunk_UserDestroy(reset_grow);
    if (Switch_VirtualCommit(reset_base, kPageSize) != 0 || Switch_VirtualRelease(reset_base) != 0)
        return 1;

    return 0;
}
