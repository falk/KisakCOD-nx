#pragma once

#include <cstddef>

// Test-only host allocation used by LP64 consumer proofs.  A successful
// allocation is guaranteed to begin above the 32-bit address range; callers
// must release it explicitly.  It is intentionally unavailable on Horizon,
// where the fixture's purpose is host detection of accidental narrowing.
struct SwitchHighAddressAllocation
{
    void *address;
    std::size_t bytes;
};

bool SwitchHighAddressAllocate(std::size_t bytes, SwitchHighAddressAllocation *allocation);
void SwitchHighAddressRelease(SwitchHighAddressAllocation *allocation);
