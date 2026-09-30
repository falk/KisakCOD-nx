#pragma once

// Pure (no deko3d, no D3D headers) building blocks of the deko9 native
// submission fast path, kept here so switch_deko9_fastpath_test.cpp can test
// them on the host under ASan/UBSan. Numeric D3DSAMP_* indices are spelled
// out because this header must not need d3d9.h.

#include <cstdint>
#include <cstring>

namespace deko9
{

// ---- sampler state ------------------------------------------------------------

// D3DSAMPLERSTATETYPE values (d3d9types.h).
enum : uint32_t
{
    kSampAddressU = 1,
    kSampAddressV = 2,
    kSampAddressW = 3,
    kSampBorderColor = 4,
    kSampMagFilter = 5,
    kSampMinFilter = 6,
    kSampMipFilter = 7,
    kSampMipLodBias = 8,
    kSampMaxMipLevel = 9,
    kSampMaxAnisotropy = 10,
    kSampSrgbTexture = 11,
    kSampElementIndex = 12,
    kSampDmapOffset = 13,
    kSampStateCount = 14, // rows are indexed by type; [0] is unused
};

// Packs a sampler-state row into 27 bits when it only uses the fields the
// engine sets (filters, address modes, anisotropy) and every other field is
// at its D3D9 default. The packed value keys SamplerIdCache, replacing the
// 60-byte SamplerKey hash + compare on the draw path. Returns false (use the
// full key) for anything else.
inline bool CompactSamplerKey(const uint32_t state[kSampStateCount], bool compare, uint32_t *key)
{
    if (state[kSampBorderColor] || state[kSampMipLodBias] || state[kSampMaxMipLevel] || state[kSampSrgbTexture] ||
        state[kSampElementIndex] || state[kSampDmapOffset])
        return false;
    const uint32_t u = state[kSampAddressU], v = state[kSampAddressV], w = state[kSampAddressW];
    const uint32_t mag = state[kSampMagFilter], min = state[kSampMinFilter], mip = state[kSampMipFilter];
    const uint32_t aniso = state[kSampMaxAnisotropy];
    if ((u | v | w) >= 8 || (mag | min | mip) >= 16 || aniso >= 32)
        return false;
    *key = u | v << 3 | w << 6 | mag << 9 | min << 13 | mip << 17 | aniso << 21 | (compare ? 1u << 26 : 0u);
    return true;
}

// Compact sampler key -> sampler descriptor id. Open addressing, no
// deletion: sampler descriptors are created once and never freed for the
// device's lifetime (Device::SamplerDescriptor), so an entry never goes
// stale. Clear() exists for a device that rebuilds its sampler set.
class SamplerIdCache
{
public:
    static constexpr uint32_t kCapacity = 1024; // power of two, > kSamplerDescriptors / 2

    SamplerIdCache() { Clear(); }
    void Clear()
    {
        std::memset(m_keys, 0xff, sizeof(m_keys));
        m_count = 0;
    }
    // Returns the id, or UINT32_MAX when the key is not present.
    uint32_t Find(uint32_t key) const
    {
        for (uint32_t i = Hash(key);; i = (i + 1) & (kCapacity - 1))
        {
            if (m_keys[i] == key)
                return m_ids[i];
            if (m_keys[i] == kEmpty)
                return UINT32_MAX;
        }
    }
    // False when full (callers then just use the full-key path).
    bool Insert(uint32_t key, uint32_t id)
    {
        if (m_count >= kCapacity / 2)
            return false;
        for (uint32_t i = Hash(key);; i = (i + 1) & (kCapacity - 1))
        {
            if (m_keys[i] == key)
            {
                m_ids[i] = id;
                return true;
            }
            if (m_keys[i] == kEmpty)
            {
                m_keys[i] = key;
                m_ids[i] = id;
                ++m_count;
                return true;
            }
        }
    }
    uint32_t Count() const { return m_count; }

private:
    static constexpr uint32_t kEmpty = UINT32_MAX; // compact keys use 27 bits
    static uint32_t Hash(uint32_t key) { return (key * 0x9E3779B1u) >> 22; } // top 10 bits
    uint32_t m_keys[kCapacity];
    uint32_t m_ids[kCapacity];
    uint32_t m_count = 0;
};

// The engine's decoded sampler state (R_DecodeSamplerState, consumed by
// R_HW_SetSamplerState): bits 0-7 max anisotropy (<= 1: keep the current
// value), 8-11 D3DSAMP_MINFILTER, 12-15 MAGFILTER, 16-19 MIPFILTER, 20-21
// ADDRESSU, 22-23 ADDRESSV, 24-25 ADDRESSW. Applies `packed` to a sampler
// row exactly as R_HW_SetSamplerState's SetSamplerState calls do (only the
// fields whose bits differ from `oldPacked`) and returns the engine's
// resulting tracked state. *changed reports whether any row value changed.
inline uint32_t ApplyEngineSamplerState(uint32_t row[kSampStateCount], uint32_t packed, uint32_t oldPacked,
                                        bool *changed)
{
    uint32_t final = packed;
    const uint32_t diff = oldPacked ^ packed;
    bool any = false;
    auto set = [&](uint32_t type, uint32_t value) {
        if (row[type] != value)
        {
            row[type] = value;
            any = true;
        }
    };
    if (diff & 0xF00)
        set(kSampMinFilter, (packed & 0xF00) >> 8);
    if (diff & 0xF000)
        set(kSampMagFilter, (packed & 0xF000) >> 12);
    if (diff & 0xFF)
    {
        if ((packed & 0xFF) <= 1)
            final = (oldPacked & 0xFF) | (packed & 0xFFFFFF00);
        else
            set(kSampMaxAnisotropy, packed & 0xFF);
    }
    if (diff & 0xF0000)
        set(kSampMipFilter, (packed & 0xF0000) >> 16);
    if (diff & 0x300000)
        set(kSampAddressU, (packed & 0x300000) >> 20);
    if (diff & 0xC00000)
        set(kSampAddressV, (packed & 0xC00000) >> 22);
    if (diff & 0x3000000)
        set(kSampAddressW, (packed & 0x3000000) >> 24);
    *changed = any;
    return final;
}

// ---- shader constants -------------------------------------------------------------

// CPU mirror of one stage's float4 constant register file plus the set of
// registers the GPU copy (a UBO updated by inline pushes, ordered with the
// draws) does not have yet. Invariant: after Flush, the UBO equals `regs`.
//
// Dirty tracking is per register (a bitmask), replacing one [lo, hi) range
// per stage that pushed everything in between (a draw touching c0 and c60
// pushed 61 registers). A Set that writes the values already held marks
// nothing. Flush pushes coalesced runs: two runs separated by at most
// kMergeGapRegs clean registers become one push, since each push costs a
// 7-word command header (28 bytes) and a clean register 16 bytes; runs are
// split at kMaxPushRegs (some emulators buffer one inline update in a 2 KB array
// and fail on a longer one; hardware has no such limit).
template <uint32_t Regs>
class ConstantFile
{
public:
    static constexpr uint32_t kWords = (Regs + 63) / 64;
    static constexpr uint32_t kMergeGapRegs = 1;
    static constexpr uint32_t kMaxPushRegs = 64; // 1 KB

    float regs[Regs][4];

    ConstantFile() { Reset(); }

    // Zeroes the file and marks every register dirty (device reset).
    void Reset()
    {
        std::memset(regs, 0, sizeof(regs));
        for (uint32_t w = 0; w < kWords; ++w)
            m_dirty[w] = ~0ull;
        if (Regs % 64)
            m_dirty[kWords - 1] = (1ull << (Regs % 64)) - 1;
        m_any = true;
    }

    // Copies `count` registers from `data` at `start` (caller validated the
    // range); returns how many registers actually changed.
    uint32_t Set(uint32_t start, const float *data, uint32_t count)
    {
        uint32_t changed = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            float *reg = regs[start + i];
            const float *src = data + i * 4;
            if (!std::memcmp(reg, src, 16))
                continue;
            std::memcpy(reg, src, 16);
            const uint32_t r = start + i;
            m_dirty[r >> 6] |= 1ull << (r & 63);
            ++changed;
        }
        m_any |= changed != 0;
        return changed;
    }

    // Marks every register dirty, keeping the values (forces a full push).
    void MarkAllDirty()
    {
        for (uint32_t w = 0; w < kWords; ++w)
            m_dirty[w] = ~0ull;
        if (Regs % 64)
            m_dirty[kWords - 1] = (1ull << (Regs % 64)) - 1;
        m_any = true;
    }

    // O(1) form of Dirty(): true from any register change until Flush
    // (equal to Dirty() at all times; r_deko9Verify checks it per draw).
    bool AnyDirty() const { return m_any; }

    bool Dirty() const
    {
        uint64_t any = 0;
        for (uint32_t w = 0; w < kWords; ++w)
            any |= m_dirty[w];
        return any != 0;
    }
    bool IsDirty(uint32_t reg) const { return (m_dirty[reg >> 6] >> (reg & 63)) & 1; }

    // Calls push(firstReg, regCount) for each coalesced run and clears the
    // dirty set. Returns the number of pushes.
    template <typename Push>
    uint32_t Flush(Push &&push)
    {
        uint32_t pushes = 0;
        uint32_t reg = NextDirty(0);
        while (reg < Regs)
        {
            // Extend the run over dirty registers and short clean gaps.
            uint32_t end = reg + 1; // exclusive, last dirty register + 1
            for (;;)
            {
                const uint32_t next = NextDirty(end);
                if (next >= Regs || next - end > kMergeGapRegs)
                    break;
                end = next + 1;
            }
            for (uint32_t first = reg; first < end; first += kMaxPushRegs)
            {
                const uint32_t n = end - first < kMaxPushRegs ? end - first : kMaxPushRegs;
                push(first, n);
                ++pushes;
            }
            reg = NextDirty(end);
        }
        for (uint32_t w = 0; w < kWords; ++w)
            m_dirty[w] = 0;
        m_any = false;
        return pushes;
    }

private:
    // First dirty register >= from, or Regs.
    uint32_t NextDirty(uint32_t from) const
    {
        if (from >= Regs)
            return Regs;
        uint32_t w = from >> 6;
        uint64_t bits = m_dirty[w] & (~0ull << (from & 63));
        for (;;)
        {
            if (bits)
            {
                const uint32_t r = (w << 6) + (uint32_t)__builtin_ctzll(bits);
                return r < Regs ? r : Regs;
            }
            if (++w >= kWords)
                return Regs;
            bits = m_dirty[w];
        }
    }

    uint64_t m_dirty[kWords];
    bool m_any = true;
};

} // namespace deko9
