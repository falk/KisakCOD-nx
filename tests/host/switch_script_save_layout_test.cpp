// Host ASan proof for the LP64 script save-stack layout.
//
// VariableStackBuffer is a byte-oriented record whose first byte is the
// value type and whose payload is a native pointer-sized union.  The VM
// archives entries with a 1 + sizeof(uintptr_t) stride.  This proof exercises
// the exact in-memory layout with high-half pointer values and redzones; an
// ILP32 5-byte walk either loses the high half or reads the second record at
// the wrong address before PASS can be emitted.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <universal/q_shared.h>
#include <script/scr_variable.h>

namespace
{
constexpr std::size_t kStackValueSize = 1 + sizeof(uintptr_t);
constexpr std::size_t kStackHeaderSize = offsetof(VariableStackBuffer, buf);

bool Check(bool condition, const char *stage)
{
    if (condition)
        return true;
    std::fprintf(stderr, "FAIL:SCRIPT_SAVE_LAYOUT stage=%s\n", stage);
    return false;
}
}

int main()
{
#if UINTPTR_MAX == UINT32_MAX
    std::fprintf(stderr, "FAIL:SCRIPT_SAVE_LAYOUT stage=host-not-lp64\n");
    return 1;
#else
    static_assert(sizeof(uintptr_t) == 8, "the proof must exercise the LP64 ABI");
    static_assert(sizeof(VariableUnion) >= sizeof(uintptr_t),
                  "pointer-bearing script values must retain their native width");
    static_assert(kStackValueSize == 9, "LP64 stack records are type + uintptr_t");
    static_assert(kStackHeaderSize == offsetof(VariableStackBuffer, buf),
                  "stack allocation must use the native header offset");

    // Keep the record pointer aligned as MT_Alloc does.  Each payload remains
    // aligned too: the native header ends at offset 15 and the first pointer
    // starts at offset 16.
    std::vector<std::uint8_t> storage(kStackHeaderSize + 2 * kStackValueSize + 32, 0xa5);
    auto *stack = reinterpret_cast<VariableStackBuffer *>(storage.data());
    stack->size = 2;
    stack->bufLen = static_cast<std::uint16_t>(kStackHeaderSize + 2 * kStackValueSize);

    const uintptr_t first = static_cast<uintptr_t>(0xfeed000012345678ULL);
    const uintptr_t second = static_cast<uintptr_t>(0xbeef000087654321ULL);
    stack->buf[0] = static_cast<char>(VAR_POINTER);
    std::memcpy(stack->buf + 1, &first, sizeof(first));
    stack->buf[kStackValueSize] = static_cast<char>(VAR_STACK);
    std::memcpy(stack->buf + kStackValueSize + 1, &second, sizeof(second));

    uintptr_t observed[2] = {};
    for (unsigned int i = 0; i < stack->size; ++i)
    {
        const auto *record = reinterpret_cast<const std::uint8_t *>(stack->buf) +
                             i * kStackValueSize;
        std::memcpy(&observed[i], record + 1, sizeof(observed[i]));
    }

    if (!Check(observed[0] == first && observed[1] == second, "native-round-trip"))
        return 1;
    if (!Check(observed[0] >> 32 == 0xfeed0000U && observed[1] >> 32 == 0xbeef0000U,
               "pointer-high-half"))
        return 1;
    if (!Check(stack->bufLen == kStackHeaderSize + 2 * kStackValueSize,
               "native-buf-length"))
        return 1;
    for (std::size_t i = kStackHeaderSize + 2 * kStackValueSize; i < storage.size(); ++i)
    {
        if (!Check(storage[i] == 0xa5, "tail-canary"))
            return 1;
    }

    std::printf("PASS:SCRIPT_SAVE_LAYOUT abi=lp64 header=%zu stride=%zu high_halves=preserved\n",
                kStackHeaderSize,
                kStackValueSize);
    return 0;
#endif
}
