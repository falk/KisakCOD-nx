#pragma once

// Per-thread memo in front of the mutex-guarded live D3D texture set
// (Image_IsLiveD3DTexture). R_SetSampler checks liveness on every sampler
// binding change, thousands of times a frame; the memo answers repeats
// without the mutex or the hash lookup.
//
// Exact, not heuristic: an entry records the set's removal generation at the
// time the pointer was seen live, and is used only while no pointer has been
// removed since (UnregisterLiveD3DTexture bumps the generation under the set
// mutex). Insertions never invalidate: they cannot make a live pointer dead.
//
// Pure C++ (host-tested by switch_deko9_fastpath_test.cpp).

#include <cstdint>
#include <cstring>

struct LivePointerMemo
{
    static constexpr uint32_t kSlots = 256; // power of two

    const void *ptr[kSlots];
    uint32_t gen[kSlots];

    LivePointerMemo() { std::memset(this, 0, sizeof(*this)); }

    static uint32_t Slot(const void *p)
    {
        const uint64_t v = reinterpret_cast<uintptr_t>(p); // widening; never narrowed
        return (uint32_t)((v * 0x9E3779B97F4A7C15ull) >> 56) & (kSlots - 1);
    }
    // `removalGen` must be non-zero (0 marks an empty entry).
    bool Find(const void *p, uint32_t removalGen) const
    {
        const uint32_t i = Slot(p);
        return ptr[i] == p && gen[i] == removalGen;
    }
    void Store(const void *p, uint32_t removalGen)
    {
        const uint32_t i = Slot(p);
        ptr[i] = p;
        gen[i] = removalGen;
    }
};
