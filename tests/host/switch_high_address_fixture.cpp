#include "switch_high_address_fixture.h"

#include <cstdint>
#include <cstring>

#if !defined(__SWITCH__) && defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

bool SwitchHighAddressAllocate(std::size_t bytes, SwitchHighAddressAllocation *allocation)
{
    if (!allocation || bytes == 0)
        return false;
    std::memset(allocation, 0, sizeof(*allocation));

#if !defined(__SWITCH__) && defined(__linux__)
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0 || bytes > SIZE_MAX - static_cast<std::size_t>(pageSize - 1))
        return false;
    const std::size_t rounded = (bytes + static_cast<std::size_t>(pageSize - 1)) &
                                ~static_cast<std::size_t>(pageSize - 1);

    // MAP_FIXED_NOREPLACE makes this deterministic without ever displacing an
    // existing mapping.  Spread candidates across the first 2 GiB above 4
    // GiB so ASLR or another test mapping cannot make the fixture flaky.
#ifdef MAP_FIXED_NOREPLACE
    constexpr uintptr_t kFirstCandidate = UINT64_C(0x100000000);
    constexpr uintptr_t kCandidateStride = UINT64_C(0x02000000);
    for (uintptr_t candidate = kFirstCandidate;
         candidate <= kFirstCandidate + kCandidateStride * 63;
         candidate += kCandidateStride)
    {
        void *mapped = mmap(reinterpret_cast<void *>(candidate), rounded,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                            -1, 0);
        if (mapped == MAP_FAILED)
            continue;
        if (reinterpret_cast<uintptr_t>(mapped) > UINT32_MAX)
        {
            allocation->address = mapped;
            allocation->bytes = rounded;
            return true;
        }
        munmap(mapped, rounded);
    }
#endif
#endif
    return false;
}

void SwitchHighAddressRelease(SwitchHighAddressAllocation *allocation)
{
    if (!allocation || !allocation->address)
        return;
#if !defined(__SWITCH__) && defined(__linux__)
    munmap(allocation->address, allocation->bytes);
#endif
    std::memset(allocation, 0, sizeof(*allocation));
}
