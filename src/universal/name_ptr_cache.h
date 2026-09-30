#pragma once

// Direct-mapped memo for by-name lookups whose callers pass the same string
// pointer frame after frame (config strings, script strings, string
// literals, menu/item names): the slot is picked from the pointer, and a hit
// is proven by content, never by the pointer alone.
//
// A slot hits only when
//   * its key is the queried pointer (cheap reject; a slot can be stale or
//     torn by another thread, the next two checks carry correctness),
//   * its generation equals the caller's current one (the caller bumps it
//     whenever the table behind the lookup changes: entries added, removed,
//     replaced), and
//   * strcmp(query, nameOf(value)) == 0: the found object's own name equals
//     the query byte for byte. A pointer whose characters changed (freed
//     and reused string) therefore misses.
// For a case-insensitive table with case-insensitively unique names (dvars;
// the asset registry's first match in a hash chain), a byte-equal name has
// exactly one answer, so a hit returns what the slow lookup would.
//
// Header-only and engine-free: switch_hot_engine2_test.cpp drives it against
// the slow lookup over randomized names, pointer reuse and generation bumps.

#include <atomic>
#include <cstdint>
#include <cstring>

template <typename T, unsigned kSlots> class NamePtrCache
{
    static_assert((kSlots & (kSlots - 1)) == 0, "kSlots must be a power of two");

public:
    static unsigned SlotOf(const char *name)
    {
        // Fibonacci hash of the address; only the top bits pick the slot.
        const uintptr_t v = reinterpret_cast<uintptr_t>(name) * static_cast<uintptr_t>(0x9E3779B97F4A7C15ull);
        return static_cast<unsigned>(v >> (sizeof(uintptr_t) * 8 - 16)) & (kSlots - 1);
    }

    template <typename NameOf> T *Find(const char *name, uint32_t generation, NameOf nameOf) const
    {
        const Slot &s = m_slots[SlotOf(name)];
        if (s.key.load(std::memory_order_relaxed) != name)
            return nullptr;
        if (s.generation.load(std::memory_order_relaxed) != generation)
            return nullptr;
        T *value = s.value.load(std::memory_order_relaxed);
        if (!value)
            return nullptr;
        const char *valueName = nameOf(value);
        if (!valueName || std::strcmp(name, valueName) != 0)
            return nullptr;
        return value;
    }

    void Store(const char *name, uint32_t generation, T *value)
    {
        Slot &s = m_slots[SlotOf(name)];
        s.key.store(name, std::memory_order_relaxed);
        s.generation.store(generation, std::memory_order_relaxed);
        s.value.store(value, std::memory_order_relaxed);
    }

    void Clear()
    {
        for (Slot &s : m_slots)
        {
            s.key.store(nullptr, std::memory_order_relaxed);
            s.generation.store(0, std::memory_order_relaxed);
            s.value.store(nullptr, std::memory_order_relaxed);
        }
    }

private:
    struct Slot
    {
        std::atomic<const char *> key{nullptr};
        std::atomic<uint32_t> generation{0};
        std::atomic<T *> value{nullptr};
    };
    Slot m_slots[kSlots];
};
