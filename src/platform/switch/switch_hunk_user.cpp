#include "switch_hunk_user.h"

#include "switch_platform.h"

#include <cstring>
#include <limits>

#ifdef __SWITCH__
namespace
{
constexpr size_t kPageSize = 4096;

static_assert(alignof(HunkUser) >= 32, "Switch HunkUser must preserve 32-byte buffer alignment");
static_assert(offsetof(HunkUser, buf) % 32 == 0, "Switch HunkUser buffer must start at a 32-byte boundary");

bool AddAddress(uintptr_t address, size_t bytes, uintptr_t* result)
{
    if (bytes > UINTPTR_MAX - address)
        return false;
    *result = address + bytes;
    return true;
}

bool AlignAddress(uintptr_t address, size_t alignment, uintptr_t* result)
{
    const size_t mask = alignment - 1;

    if (address > UINTPTR_MAX - mask)
        return false;
    *result = (address + mask) & ~static_cast<uintptr_t>(mask);
    return true;
}

uintptr_t AlignAddressDown(uintptr_t address, size_t alignment)
{
    return address & ~static_cast<uintptr_t>(alignment - 1);
}

bool IsValidAlignment(int alignment)
{
    return alignment >= 1 && alignment <= 4096 && (alignment & (alignment - 1)) == 0;
}
}

HunkUser* __cdecl Hunk_UserCreate(int maxSize, const char* name, bool fixed, bool tempMem, int type)
{
    HunkUser* user;
    uintptr_t end;

    if (maxSize <= 0 || maxSize % static_cast<int>(kPageSize) != 0 ||
        static_cast<size_t>(maxSize) <= offsetof(HunkUser, buf))
        return NULL;

    user = static_cast<HunkUser*>(Switch_VirtualReserve(static_cast<size_t>(maxSize)));
    if (user == NULL || !Switch_VirtualCommit(user, kPageSize))
    {
        if (user != NULL)
            Switch_VirtualRelease(user);
        return NULL;
    }

    if (!AddAddress(reinterpret_cast<uintptr_t>(user), static_cast<size_t>(maxSize), &end))
    {
        Switch_VirtualRelease(user);
        return NULL;
    }

    user->current = user;
    user->next = NULL;
    user->maxSize = static_cast<size_t>(maxSize);
    user->end = reinterpret_cast<uint8_t*>(end);
    user->pos = user->buf;
    user->name = name;
    user->fixed = fixed;
    user->tempMem = tempMem;
    user->type = type;
    return user;
}

void* Hunk_UserAlloc(HunkUser* user, uint32_t size, int alignment)
{
    HunkUser* current;

    if (user == NULL || !IsValidAlignment(alignment) || user->maxSize < offsetof(HunkUser, buf) ||
        static_cast<size_t>(size) > user->maxSize - offsetof(HunkUser, buf))
        return NULL;

    current = user->current;
    while (current != NULL)
    {
        uintptr_t allocation;
        uintptr_t allocation_end;
        uintptr_t committed_begin;
        uintptr_t committed_end;
        const uintptr_t end = reinterpret_cast<uintptr_t>(current->end);

        if (AlignAddress(reinterpret_cast<uintptr_t>(current->pos), static_cast<size_t>(alignment), &allocation))
        {
            committed_begin = AlignAddressDown(allocation, kPageSize);
            if (allocation <= end && static_cast<size_t>(size) <= end - allocation &&
                AddAddress(allocation, static_cast<size_t>(size), &allocation_end) &&
                AlignAddress(allocation_end, kPageSize, &committed_end) &&
                committed_begin <= committed_end &&
                (committed_begin == committed_end ||
                 Switch_VirtualCommit(reinterpret_cast<void*>(committed_begin), committed_end - committed_begin)))
            {
                current->pos = reinterpret_cast<uint8_t*>(allocation_end);
                return reinterpret_cast<void*>(allocation);
            }
        }

        if (user->fixed)
            return NULL;

        if (user->maxSize > static_cast<size_t>(std::numeric_limits<int>::max()))
            return NULL;
        HunkUser* const next = Hunk_UserCreate(static_cast<int>(user->maxSize), user->name, false, user->tempMem, user->type);
        if (next == NULL)
            return NULL;
        user->current = next;
        current->next = next;
        current = next;
    }

    return NULL;
}

void* Hunk_UserAllocAlignStrict(HunkUser* user, uint32_t size)
{
    return Hunk_UserAlloc(user, size, 1);
}

void __cdecl Hunk_UserSetPos(HunkUser* user, uint8_t* pos)
{
    if (user == NULL || !user->fixed || pos == NULL ||
        reinterpret_cast<uintptr_t>(pos) < reinterpret_cast<uintptr_t>(user->buf) ||
        reinterpret_cast<uintptr_t>(pos) > reinterpret_cast<uintptr_t>(user->pos))
        return;
    user->pos = pos;
}

void __cdecl Hunk_UserReset(HunkUser* user)
{
    HunkUser* child;

    if (user == NULL)
        return;

    const size_t max_size = user->maxSize;
    uint8_t* const end = user->end;
    const char* const name = user->name;
    const bool fixed = user->fixed;
    const bool temp_mem = user->tempMem;
    const int type = user->type;

    child = user->next;
    user->next = NULL;
    user->current = user;
    while (child != NULL)
    {
        HunkUser* const next = child->next;
        Switch_VirtualRelease(child);
        child = next;
    }

    if (max_size > kPageSize)
        Switch_VirtualDecommit(reinterpret_cast<uint8_t*>(user) + kPageSize, max_size - kPageSize);

    std::memset(user, 0, kPageSize);
    user->current = user;
    user->maxSize = max_size;
    user->end = end;
    user->pos = user->buf;
    user->name = name;
    user->fixed = fixed;
    user->tempMem = temp_mem;
    user->type = type;
}

void __cdecl Hunk_UserDestroy(HunkUser* user)
{
    HunkUser* child;

    if (user == NULL)
        return;

    child = user->next;
    while (child != NULL)
    {
        HunkUser* const next = child->next;
        Switch_VirtualRelease(child);
        child = next;
    }
    Switch_VirtualRelease(user);
}

char* __cdecl Hunk_CopyString(HunkUser* user, const char* in)
{
    size_t length;
    char* out;

    if (in == NULL)
        return NULL;
    length = std::strlen(in);
    if (length == std::numeric_limits<size_t>::max() || length + 1 > UINT32_MAX)
        return NULL;
    out = static_cast<char*>(Hunk_UserAlloc(user, static_cast<uint32_t>(length + 1), 1));
    if (out != NULL)
        std::memcpy(out, in, length + 1);
    return out;
}
#endif
