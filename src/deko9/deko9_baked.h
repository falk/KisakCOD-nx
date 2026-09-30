#pragma once

// Baked per-pass state of the optional deko3d renderer (deko9): pure parts
// (no deko3d, no D3D headers), host-tested by switch_deko9_baked_test.cpp.
//
// A material technique pass fixes the rasterizer, depth/stencil, color and
// blend state (through the D3D render states the engine sets for its
// GfxStateBits) and the program: vertex shader, pixel shader, vertex
// declaration, the pixel shader's depth-compare sampler mask and, for
// instanced draws, the instance register layout. The device resolves each
// once into deko3d command words (captured with dkCmdBufBeginCaptureCmds on
// a private command buffer) and binds it later by replaying the words
// (dkCmdBufReplayCmds: one reserve + memcpy), instead of re-deriving every
// deko3d state struct, comparing each with the last one recorded, and
// calling one dkCmdBuf* entry per struct.
//
// Why capture/replay and not dkCmdBufFinishList + dkCmdBufCallList: a call
// is a control command, which ends the current GPFIFO entry and adds two
// (the call and the return). libnx queues at most 2048 GPFIFO entries per
// submit, and each entry costs the GPU front end a fetch, so one call per
// pass change would multiply the entry count. Replayed words stay inline
// in the list.
//
// Keys hold exactly the inputs that change the emitted words, normalized so
// inputs a disabled feature ignores do not split entries (stencil ops with
// stencil off, blend factors with blending off, the alpha reference with
// alpha test off, the depth format's bias unit without a bias). Everything
// else a draw depends on is outside the baked unit and applied per draw:
// the override set, see BakedOverrides below.

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <memory>
#include <vector>

namespace deko9
{

// D3DRENDERSTATETYPE values (d3d9types.h), checked against d3d9.h by
// static_asserts in deko9_draw.cpp.
constexpr uint32_t kRsZEnable = 7;
constexpr uint32_t kRsFillMode = 8;
constexpr uint32_t kRsZWriteEnable = 14;
constexpr uint32_t kRsAlphaTestEnable = 15;
constexpr uint32_t kRsSrcBlend = 19;
constexpr uint32_t kRsDestBlend = 20;
constexpr uint32_t kRsCullMode = 22;
constexpr uint32_t kRsZFunc = 23;
constexpr uint32_t kRsAlphaRef = 24;
constexpr uint32_t kRsAlphaFunc = 25;
constexpr uint32_t kRsAlphaBlendEnable = 27;
constexpr uint32_t kRsStencilEnable = 52;
constexpr uint32_t kRsStencilFail = 53;
constexpr uint32_t kRsStencilZFail = 54;
constexpr uint32_t kRsStencilPass = 55;
constexpr uint32_t kRsStencilFunc = 56;
constexpr uint32_t kRsStencilRef = 57;
constexpr uint32_t kRsStencilMask = 58;
constexpr uint32_t kRsStencilWriteMask = 59;
constexpr uint32_t kRsColorWriteEnable = 168;
constexpr uint32_t kRsBlendOp = 171;
constexpr uint32_t kRsSlopeScaleDepthBias = 175;
constexpr uint32_t kRsTwoSidedStencilMode = 185;
constexpr uint32_t kRsCcwStencilFail = 186;
constexpr uint32_t kRsCcwStencilZFail = 187;
constexpr uint32_t kRsCcwStencilPass = 188;
constexpr uint32_t kRsCcwStencilFunc = 189;
constexpr uint32_t kRsColorWriteEnable1 = 190;
constexpr uint32_t kRsColorWriteEnable2 = 191;
constexpr uint32_t kRsColorWriteEnable3 = 192;
constexpr uint32_t kRsBlendFactor = 193;
constexpr uint32_t kRsDepthBias = 195;
constexpr uint32_t kRsSeparateAlphaBlendEnable = 206;
constexpr uint32_t kRsSrcBlendAlpha = 207;
constexpr uint32_t kRsDestBlendAlpha = 208;
constexpr uint32_t kRsBlendOpAlpha = 209;

// The bound depth-stencil surface, as far as raster state depends on it.
struct DepthTarget
{
    bool present = false; // a depth-stencil surface is bound
    bool stencil = false; // its format has stencil bits
    bool d16 = false;     // D3DFMT_D16 (bias unit 2^-16 instead of 2^-24)
};

// The baked override set: state a draw depends on that is NOT part of a
// baked unit and is applied per draw on top of it. Every item enters the
// device through its own setter and dirty flag, so a baked replay never
// overwrites it and never needs invalidating when it changes:
//   - viewport incl. depth range (the engine's depth hack / depth-range
//     changes: R_ChangeDepthRange -> SetViewport), scissor rect and
//     D3DRS_SCISSORTESTENABLE (m_dirtyViewport, ApplyViewportScissor);
//   - render targets (m_dirtyTargets);
//   - textures, sampler states and their hazard barriers (m_dirtyTextures,
//     per-draw BeforeSample);
//   - shader constants (ConstantFile dirty bits) and instance data;
//   - vertex streams and the index buffer (m_dirtyInput, per-draw index bind).
// Per-draw engine state that IS raster state reaches the key as render
// states: polygon offset (R_ForceSetPolygonOffset: D3DRS_DEPTHBIAS/
// SLOPESCALEDEPTHBIAS from r_polygonOffset*/sm_polygonOffset* dvars),
// shadow and decal stencil (D3DRS_STENCIL*), color-write masks, cull and
// fill mode (r_* debug views), alpha test and blend. A render-state dvar
// change therefore produces a different key, never a stale hit.

// Normalized key of the rasterizer + depth/stencil + color + blend state.
// Plain bytes, zero-initialized, compared with memcmp.
struct RasterKey
{
    uint32_t depthBias;   // float bits, 0 when zero (either sign)
    uint32_t slopeBias;   // float bits, 0 when zero
    uint32_t blendFactor; // D3DRS_BLENDFACTOR, 0 unless blending
    uint8_t cull, fill, zfunc, flags;
    uint8_t stencilOps[4];    // fail, zfail, pass, func (front)
    uint8_t ccwStencilOps[4]; // back; 0 unless two-sided stencil
    uint8_t stencilValues[3]; // write mask, ref, read mask
    uint8_t alphaFunc, alphaRef;
    uint8_t blend[6];         // src, dst, op, srcA, dstA, opA (raw D3D values)
    uint8_t colorWrite[4];
    uint8_t pad[1];

    // flags
    static constexpr uint8_t kDepthTest = 1, kDepthWrite = 2, kStencil = 4, kTwoSided = 8, kAlphaTest = 16,
                             kBlend = 32, kSeparateAlpha = 64, kD16Bias = 128;

    bool operator==(const RasterKey &o) const { return !std::memcmp(this, &o, sizeof(*this)); }
    bool operator!=(const RasterKey &o) const { return !(*this == o); }
};
// memcmp equality and HashKey read every byte: no padding allowed.
static_assert(std::has_unique_object_representations_v<RasterKey>, "RasterKey must stay padding-free");

inline uint32_t FloatBitsOrZero(uint32_t bits)
{
    return (bits & 0x7fffffffu) ? bits : 0u; // +0 and -0 are "no bias"
}

// Builds the key from the D3D render-state array (256 entries) exactly as
// Device::ApplyRasterDepthColor consumes it. Values are truncated to bytes
// only where every valid D3D value fits (enums < 256, 8-bit masks); an
// out-of-range enum is kept distinguishable (clamped to 255) and the slow
// derivation behind the miss reports it.
inline RasterKey BuildRasterKey(const uint32_t *rs, DepthTarget depth)
{
    auto u8 = [](uint32_t v) -> uint8_t { return v > 255u ? 255u : (uint8_t)v; };
    RasterKey k;
    std::memset(&k, 0, sizeof(k));
    k.cull = u8(rs[kRsCullMode]);
    k.fill = u8(rs[kRsFillMode]);
    k.depthBias = FloatBitsOrZero(rs[kRsDepthBias]);
    k.slopeBias = FloatBitsOrZero(rs[kRsSlopeScaleDepthBias]);
    uint8_t flags = 0;
    if ((k.depthBias || k.slopeBias) && depth.present && depth.d16)
        flags |= RasterKey::kD16Bias;
    const bool depthTest = depth.present && rs[kRsZEnable] != 0;
    if (depthTest)
        flags |= RasterKey::kDepthTest;
    if (depthTest && rs[kRsZWriteEnable])
        flags |= RasterKey::kDepthWrite;
    k.zfunc = u8(rs[kRsZFunc]); // recorded even with the test off
    if (depth.present && depth.stencil && rs[kRsStencilEnable])
    {
        flags |= RasterKey::kStencil;
        k.stencilOps[0] = u8(rs[kRsStencilFail]);
        k.stencilOps[1] = u8(rs[kRsStencilZFail]);
        k.stencilOps[2] = u8(rs[kRsStencilPass]);
        k.stencilOps[3] = u8(rs[kRsStencilFunc]);
        if (rs[kRsTwoSidedStencilMode])
        {
            flags |= RasterKey::kTwoSided;
            k.ccwStencilOps[0] = u8(rs[kRsCcwStencilFail]);
            k.ccwStencilOps[1] = u8(rs[kRsCcwStencilZFail]);
            k.ccwStencilOps[2] = u8(rs[kRsCcwStencilPass]);
            k.ccwStencilOps[3] = u8(rs[kRsCcwStencilFunc]);
        }
        k.stencilValues[0] = (uint8_t)rs[kRsStencilWriteMask];
        k.stencilValues[1] = (uint8_t)rs[kRsStencilRef];
        k.stencilValues[2] = (uint8_t)rs[kRsStencilMask];
    }
    if (rs[kRsAlphaTestEnable])
    {
        flags |= RasterKey::kAlphaTest;
        k.alphaFunc = u8(rs[kRsAlphaFunc]);
        k.alphaRef = (uint8_t)rs[kRsAlphaRef];
    }
    if (rs[kRsAlphaBlendEnable])
    {
        flags |= RasterKey::kBlend;
        k.blend[0] = u8(rs[kRsSrcBlend]);
        k.blend[1] = u8(rs[kRsDestBlend]);
        k.blend[2] = u8(rs[kRsBlendOp]);
        if (rs[kRsSeparateAlphaBlendEnable])
        {
            flags |= RasterKey::kSeparateAlpha;
            k.blend[3] = u8(rs[kRsSrcBlendAlpha]);
            k.blend[4] = u8(rs[kRsDestBlendAlpha]);
            k.blend[5] = u8(rs[kRsBlendOpAlpha]);
        }
        k.blendFactor = rs[kRsBlendFactor];
    }
    k.colorWrite[0] = (uint8_t)(rs[kRsColorWriteEnable] & 0xf);
    k.colorWrite[1] = (uint8_t)(rs[kRsColorWriteEnable1] & 0xf);
    k.colorWrite[2] = (uint8_t)(rs[kRsColorWriteEnable2] & 0xf);
    k.colorWrite[3] = (uint8_t)(rs[kRsColorWriteEnable3] & 0xf);
    k.flags = flags;
    return k;
}

// True when a render state feeds BuildRasterKey (SetRenderState marks the
// raster state dirty only for these; others have their own flags or none).
inline bool IsRasterRenderState(uint32_t state)
{
    switch (state)
    {
    case kRsZEnable: case kRsFillMode: case kRsZWriteEnable: case kRsAlphaTestEnable: case kRsSrcBlend:
    case kRsDestBlend: case kRsCullMode: case kRsZFunc: case kRsAlphaRef: case kRsAlphaFunc:
    case kRsAlphaBlendEnable: case kRsStencilEnable: case kRsStencilFail: case kRsStencilZFail:
    case kRsStencilPass: case kRsStencilFunc: case kRsStencilRef: case kRsStencilMask:
    case kRsStencilWriteMask: case kRsColorWriteEnable: case kRsBlendOp: case kRsSlopeScaleDepthBias:
    case kRsTwoSidedStencilMode: case kRsCcwStencilFail: case kRsCcwStencilZFail: case kRsCcwStencilPass:
    case kRsCcwStencilFunc: case kRsColorWriteEnable1: case kRsColorWriteEnable2: case kRsColorWriteEnable3:
    case kRsBlendFactor: case kRsDepthBias: case kRsSeparateAlphaBlendEnable: case kRsSrcBlendAlpha:
    case kRsDestBlendAlpha: case kRsBlendOpAlpha:
        return true;
    default:
        return false;
    }
}

// ---- early fragment tests --------------------------------------------------------
//
// A pixel shader that can discard (texkill -> `discard`) or runs with the
// alpha test on is depth-tested late on Maxwell: hidden pixels still run the
// whole shader. Forcing early tests (the pixel shader's early-Z variant) is
// exact only when the discard cannot change depth or stencil: the draw writes
// neither. The depth test itself must be on (otherwise there is nothing to
// reject early). Pure function of the render states and the bound depth
// target, as ApplyRasterDepthColor consumes them (depth writes need the test
// on; stencil ops apply only with a stencil-format target).
constexpr uint32_t kD3DStencilOpKeep = 1;
inline bool RasterAllowsEarlyZ(const uint32_t *rs, DepthTarget depth)
{
    if (!depth.present || !rs[kRsZEnable] || rs[kRsZWriteEnable])
        return false;
    if (depth.stencil && rs[kRsStencilEnable] && (rs[kRsStencilWriteMask] & 0xffu))
    {
        auto keeps = [](uint32_t fail, uint32_t zfail, uint32_t pass) {
            return fail == kD3DStencilOpKeep && zfail == kD3DStencilOpKeep && pass == kD3DStencilOpKeep;
        };
        if (!keeps(rs[kRsStencilFail], rs[kRsStencilZFail], rs[kRsStencilPass]))
            return false;
        if (rs[kRsTwoSidedStencilMode] && !keeps(rs[kRsCcwStencilFail], rs[kRsCcwStencilZFail], rs[kRsCcwStencilPass]))
            return false;
    }
    return true;
}

// ---- instanced draws ------------------------------------------------------------

// Vertex shader constant registers that vary per instance, in order; the
// instanced shader variant reads register regs[i] from vertex attribute
// location InstanceAttribBase(count) + i (an instance-rate stream holding
// one float4 per register per instance) instead of its constant buffer.
// UAM's GLSL front end accepts attribute locations 0..15 only (D3D9 input
// registers v0..v15 use the same range), so instance attributes take the
// top `count` locations and must not overlap the shader's inputs
// (InstanceFits); a draw that does not fit is drawn per instance.
constexpr uint32_t kMaxInstanceRegs = 16;
constexpr uint32_t kVertexAttribLocations = 16;
inline uint32_t InstanceAttribBase(uint32_t count) { return kVertexAttribLocations - count; }
inline bool InstanceFits(uint32_t vsInputMask, uint32_t count)
{
    return count && count <= kMaxInstanceRegs && !(vsInputMask >> InstanceAttribBase(count));
}
struct InstanceLayout
{
    uint8_t count = 0;
    uint8_t regs[kMaxInstanceRegs] = {};
    bool operator==(const InstanceLayout &o) const
    {
        return count == o.count && !std::memcmp(regs, o.regs, count);
    }
    bool operator!=(const InstanceLayout &o) const { return !(*this == o); }
    // Stable 64-bit identity (SD shader-cache file names, program keys).
    uint64_t Hash() const
    {
        uint64_t h = 0xcbf29ce484222325ull ^ count;
        for (uint32_t i = 0; i < count; ++i)
            h = (h ^ regs[i]) * 0x100000001b3ull;
        return count ? h : 0;
    }
};

// Adds rows [first, first + rows) to the layout, keeping registers sorted
// and unique. False (layout unchanged) when a register is >= 256 or the
// layout would exceed kMaxInstanceRegs: the caller then draws per instance.
inline bool AddInstanceRegs(InstanceLayout *layout, uint32_t first, uint32_t rows)
{
    InstanceLayout next = *layout;
    for (uint32_t r = first; r < first + rows; ++r)
    {
        if (r >= 256)
            return false;
        uint32_t at = 0;
        while (at < next.count && next.regs[at] < r)
            ++at;
        if (at < next.count && next.regs[at] == r)
            continue;
        if (next.count >= kMaxInstanceRegs)
            return false;
        std::memmove(next.regs + at + 1, next.regs + at, next.count - at);
        next.regs[at] = (uint8_t)r;
        ++next.count;
    }
    *layout = next;
    return true;
}

// Splits a sequence of instances into runs drawable by one instanced draw:
// consecutive instances whose per-instance-invariant state (`group`, e.g.
// the reflection probe a lit static model binds) is equal. Order is kept,
// so blending and depth-equal passes see the same draw order as before.
// Calls emit(first, count) per run; runs of one instance are included (the
// caller decides whether to draw those singly).
template <typename GroupOf, typename Emit>
uint32_t ForEachInstanceRun(uint32_t count, GroupOf &&groupOf, Emit &&emit)
{
    uint32_t runs = 0;
    uint32_t first = 0;
    while (first < count)
    {
        const auto group = groupOf(first);
        uint32_t end = first + 1;
        while (end < count && groupOf(end) == group)
            ++end;
        emit(first, end - first);
        ++runs;
        first = end;
    }
    return runs;
}

// ---- program key --------------------------------------------------------------------

// Everything the bound program (shader pair + vertex attribute layout)
// depends on. Shader and declaration ids are unique for the device's
// lifetime (Device::NextId), so a released object's entries can never be
// hit again; they are still purged on release (ForgetProgramObject) so the
// cache does not keep dead variants.
struct ProgramKey
{
    uint32_t vs, ps, decl, psShadowMask;
    uint64_t instance; // InstanceLayout::Hash(), 0 = not instanced
    uint32_t psEarlyZ; // 1: the pixel shader's early-Z variant (RasterAllowsEarlyZ)
    bool operator==(const ProgramKey &o) const
    {
        return vs == o.vs && ps == o.ps && decl == o.decl && psShadowMask == o.psShadowMask &&
               instance == o.instance && psEarlyZ == o.psEarlyZ;
    }
    bool operator!=(const ProgramKey &o) const { return !(*this == o); }
    bool Uses(uint32_t id) const { return vs == id || ps == id || decl == id; }
};

inline uint64_t MixHash(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}
inline uint64_t HashKey(const RasterKey &k)
{
    uint64_t words[(sizeof(RasterKey) + 7) / 8] = {};
    std::memcpy(words, &k, sizeof(RasterKey));
    uint64_t h = 0;
    for (uint64_t w : words)
        h = MixHash(h, w * 0xff51afd7ed558ccdull);
    return h;
}
inline uint64_t HashKey(const ProgramKey &k)
{
    uint64_t h = MixHash(0, ((uint64_t)k.vs << 32 | k.ps) * 0xff51afd7ed558ccdull);
    h = MixHash(h, ((uint64_t)k.decl << 32 | k.psShadowMask) * 0xc4ceb9fe1a85ec53ull);
    return MixHash(MixHash(h, k.instance), k.psEarlyZ);
}

// Key -> baked unit, open addressing with linear probing. Units are heap
// objects, so a pointer to one stays valid until RemoveIf/Clear drops it
// (the device resets its "last replayed" pointers on both). Lookup is the
// hot operation (once per pass change); Insert happens on a miss (first use
// of a pass); RemoveIf (object release) rebuilds the table.
template <class Key, class Unit>
class BakedCache
{
public:
    BakedCache() { Rehash(64); }

    Unit *Find(const Key &key) const
    {
        const uint64_t h = HashKey(key);
        for (uint32_t i = (uint32_t)h & m_mask;; i = (i + 1) & m_mask)
        {
            const Slot &slot = m_slots[i];
            if (!slot.unit)
                return nullptr;
            if (slot.hash == h && slot.unit->key == key)
                return slot.unit;
        }
    }

    // Takes ownership. The key must not be present.
    Unit *Insert(std::unique_ptr<Unit> unit)
    {
        if ((m_units.size() + 1) * 2 > m_slots.size())
            Rehash((uint32_t)m_slots.size() * 2);
        Unit *raw = unit.get();
        m_units.push_back(std::move(unit));
        Place(raw);
        return raw;
    }

    // Drops every unit for which pred(unit) is true; returns how many.
    template <typename Pred>
    uint32_t RemoveIf(Pred &&pred)
    {
        uint32_t removed = 0;
        for (size_t i = 0; i < m_units.size();)
        {
            if (pred(*m_units[i]))
            {
                m_units[i] = std::move(m_units.back());
                m_units.pop_back();
                ++removed;
            }
            else
            {
                ++i;
            }
        }
        if (removed)
            Rehash((uint32_t)m_slots.size());
        return removed;
    }

    void Clear()
    {
        m_units.clear();
        Rehash(64);
    }
    uint32_t Size() const { return (uint32_t)m_units.size(); }

private:
    struct Slot
    {
        uint64_t hash;
        Unit *unit;
    };
    void Place(Unit *unit)
    {
        const uint64_t h = HashKey(unit->key);
        uint32_t i = (uint32_t)h & m_mask;
        while (m_slots[i].unit)
            i = (i + 1) & m_mask;
        m_slots[i] = {h, unit};
    }
    void Rehash(uint32_t capacity)
    {
        while (capacity < 64 || capacity < m_units.size() * 2)
            capacity *= 2;
        m_slots.assign(capacity, Slot{0, nullptr});
        m_mask = capacity - 1;
        for (const auto &unit : m_units)
            Place(unit.get());
    }
    std::vector<Slot> m_slots;
    std::vector<std::unique_ptr<Unit>> m_units;
    uint32_t m_mask = 0;
};

} // namespace deko9
