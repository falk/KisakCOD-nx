#include "switch_high_address_fixture.h"

#include <cstdint>

int main()
{
    SwitchHighAddressAllocation allocation{};
    if (!SwitchHighAddressAllocate(4096, &allocation))
        return 1;
    if (!allocation.address || reinterpret_cast<uintptr_t>(allocation.address) <= UINT32_MAX)
        return 1;

    auto *words = static_cast<uintptr_t *>(allocation.address);
    words[0] = reinterpret_cast<uintptr_t>(allocation.address);
    words[1] = UINT64_C(0x4b4953414b535031);
    const bool preserved = words[0] == reinterpret_cast<uintptr_t>(allocation.address) &&
                           words[1] == UINT64_C(0x4b4953414b535031) &&
                           static_cast<uint32_t>(words[0]) != words[0];
    SwitchHighAddressRelease(&allocation);
    return preserved ? 0 : 1;
}
