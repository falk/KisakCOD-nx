#pragma once

// Load-time variant plan of the deko3d renderer (deko9): which compiled
// variants of a material pass's vertex and pixel shader a draw can ask for,
// so they are translated and compiled while the zone loads instead of at
// draw time (where a pack miss runs MojoShader and UAM on the back-end
// thread with the device lock held).
//
// A variant is selected per draw by:
//   - pixel shader: the depth-compare mask (2D samplers bound to a
//     depth-format texture) and early-Z (the program may discard or the
//     alpha test is on, it does not write depth, and the raster state lets
//     the depth test run first); the r_shadowFilter rewrite applies only
//     to a non-zero mask;
//   - vertex shader: the instance register layout of an instanced draw;
//   - both: the r_deko9ShaderOpt bits.
// The draw path and the plan share the selection rules below, so a planned
// set that misses a draw's variant is a bug in the inputs (a depth texture
// on a sampler the pass does not declare as a shadow map, an undeclared
// alpha test), never a rule mismatch.
//
// Pure C++ (no deko3d, no D3D headers), host-tested by
// switch_deko9_variant_plan_test.cpp.

#include "deko9_baked.h"
#include "deko9_native.h"
#include "deko9_shader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace deko9
{

// Everything a compiled variant depends on besides the shader's bytecode.
struct VariantSelect
{
    uint32_t shadowMask = 0;
    InstanceLayout instance{};
    bool earlyZ = false;
    uint32_t shadowFilter = 0;
    uint32_t shaderOpt = 0;
    bool operator==(const VariantSelect &o) const
    {
        return shadowMask == o.shadowMask && instance == o.instance && earlyZ == o.earlyZ &&
               shadowFilter == o.shadowFilter && shaderOpt == o.shaderOpt;
    }
    bool operator!=(const VariantSelect &o) const { return !(*this == o); }
};

// The installed variants of one shader. Every call runs with the device
// lock held: the prebake installs from the loading thread, a draw looks up
// (and on a miss builds and installs) on the recording thread, and a
// release clears it. A variant's address is stable until Clear.
template <typename Variant> class VariantSet
{
public:
    const Variant *Find(const VariantSelect &select) const
    {
        for (const Entry &e : m_entries)
        {
            if (e.select == select)
                return e.variant.get();
        }
        return nullptr;
    }
    // The installed variant for `select`: the existing one when another
    // thread installed it first, else make() (a std::unique_ptr<Variant>,
    // null when loading failed) is added.
    template <typename Make> const Variant *Install(const VariantSelect &select, Make &&make)
    {
        if (const Variant *existing = Find(select))
            return existing;
        std::unique_ptr<Variant> variant = make();
        if (!variant)
            return nullptr;
        m_entries.push_back({select, std::move(variant)});
        return m_entries.back().variant.get();
    }
    template <typename F> void ForEach(F &&f) const
    {
        for (const Entry &e : m_entries)
            f(*e.variant);
    }
    void Clear() { m_entries.clear(); }
    size_t Size() const { return m_entries.size(); }

private:
    struct Entry
    {
        VariantSelect select;
        std::unique_ptr<Variant> variant;
    };
    std::vector<Entry> m_entries;
};

// A sampler compares (sampler2DShadow) when a depth-format texture is bound
// to a 2D sampler register.
inline bool CompareSampler(bool depthFormat, Deko9SamplerDim dim)
{
    return depthFormat && dim == DEKO9_SAMPLER_2D;
}

// Pixel shader half of the early-Z rule: the program can discard or the
// draw alpha-tests, and the program does not write depth. The draw adds the
// raster half (RasterAllowsEarlyZ), no open occlusion query, r_deko9EarlyZ.
inline bool EarlyZShaderEligible(const Deko9ShaderInfo &ps, bool alphaTest)
{
    return !ps.writesDepth && (ps.kills || alphaTest);
}

// r_shadowFilter rewrites only pixel shaders with a depth-compare sampler
// (the rewrite is inert without one), so only those get another variant.
inline uint32_t VariantShadowFilter(Deko9Stage stage, uint32_t shadowMask, uint32_t shadowFilter)
{
    return shadowMask && stage == DEKO9_STAGE_PIXEL ? shadowFilter : 0;
}

// The layout Deko9_BeginInstances builds from the caller's register list
// (sorted, unique). False when the list is empty or does not fit.
inline bool LayoutFromRegs(const uint8_t *regs, uint32_t count, InstanceLayout *out)
{
    InstanceLayout layout;
    if (!regs || !count || count > kMaxInstanceRegs)
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!AddInstanceRegs(&layout, regs[i], 1))
            return false;
    }
    *out = layout;
    return true;
}

// Device-wide selectors, read once per plan.
struct PlanOptions
{
    bool earlyZ = DEKO9_DEFAULT_EARLY_Z;
    uint32_t shadowFilter = DEKO9_DEFAULT_SHADOW_FILTER;
    uint32_t shaderOpt = DEKO9_DEFAULT_SHADER_OPT;
};

// One material pass as the plan sees it. `vsId`/`psId` identify the shader
// (bytecode hash on the host, the shader object on the device); the infos
// are the translator's (immutable after creation).
struct PassVariantDesc
{
    uint64_t vsId = 0, psId = 0;
    const Deko9ShaderInfo *vs = nullptr;
    const Deko9ShaderInfo *ps = nullptr;
    // Pixel sampler registers the pass binds to code images that can be a
    // depth-format texture (hardware shadow maps); only these can compare.
    uint32_t depthSamplerMask = 0;
    // Some state-bits entry of the material for this pass enables the
    // alpha test.
    bool alphaTest = false;
    // Instanced static-model layout of the pass (count 0: never instanced).
    InstanceLayout instance{};
};

struct PlannedVariant
{
    uint64_t shader = 0;
    Deko9Stage stage = DEKO9_STAGE_VERTEX;
    VariantSelect select{};
    bool operator==(const PlannedVariant &o) const
    {
        return shader == o.shader && stage == o.stage && select == o.select;
    }
    bool operator<(const PlannedVariant &o) const
    {
        if (shader != o.shader)
            return shader < o.shader;
        if (stage != o.stage)
            return stage < o.stage;
        const VariantSelect &a = select, &b = o.select;
        if (a.shadowMask != b.shadowMask)
            return a.shadowMask < b.shadowMask;
        if (a.instance.count != b.instance.count)
            return a.instance.count < b.instance.count;
        const int regs = std::memcmp(a.instance.regs, b.instance.regs, a.instance.count);
        if (regs)
            return regs < 0;
        if (a.earlyZ != b.earlyZ)
            return a.earlyZ < b.earlyZ;
        if (a.shadowFilter != b.shadowFilter)
            return a.shadowFilter < b.shadowFilter;
        return a.shaderOpt < b.shaderOpt;
    }
};

inline VariantSelect MakeSelect(Deko9Stage stage, uint32_t shadowMask, const InstanceLayout &instance, bool earlyZ,
                                const PlanOptions &options)
{
    VariantSelect s;
    s.shadowMask = shadowMask;
    s.instance = instance;
    s.earlyZ = earlyZ;
    s.shadowFilter = VariantShadowFilter(stage, shadowMask, options.shadowFilter);
    s.shaderOpt = options.shaderOpt;
    return s;
}

// The pixel samplers of `ps` that can compare in this pass.
inline uint32_t PassCompareCandidates(const Deko9ShaderInfo &ps, uint32_t depthSamplerMask)
{
    uint32_t mask = 0;
    for (uint32_t pending = ps.samplerMask & depthSamplerMask & ((1u << DEKO9_MAX_SAMPLERS) - 1u); pending;
         pending &= pending - 1u)
    {
        const uint32_t s = (uint32_t)__builtin_ctz(pending);
        if (CompareSampler(true, ps.samplerDim[s]))
            mask |= 1u << s;
    }
    return mask;
}

// Every variant a draw of `pass` can select: the vertex shader's ordinary
// and (when the layout fits its inputs) instanced variant; the pixel
// shader's variant for every subset of its compare candidates (a shadow-map
// sampler may hold a colour placeholder when that shadow is off), each with
// and without early-Z where the shader qualifies.
template <typename Emit>
void PlanPassVariants(const PassVariantDesc &pass, const PlanOptions &options, Emit &&emit)
{
    if (pass.vs)
    {
        emit(PlannedVariant{pass.vsId, DEKO9_STAGE_VERTEX, MakeSelect(DEKO9_STAGE_VERTEX, 0, {}, false, options)});
        if (pass.instance.count && InstanceFits(pass.vs->inputMask, pass.instance.count))
            emit(PlannedVariant{pass.vsId, DEKO9_STAGE_VERTEX,
                                MakeSelect(DEKO9_STAGE_VERTEX, 0, pass.instance, false, options)});
    }
    if (pass.ps)
    {
        const uint32_t candidates = PassCompareCandidates(*pass.ps, pass.depthSamplerMask);
        const bool earlyZ = options.earlyZ && EarlyZShaderEligible(*pass.ps, pass.alphaTest);
        // Enumerates every subset of `candidates` (including 0).
        uint32_t mask = 0;
        do
        {
            emit(PlannedVariant{pass.psId, DEKO9_STAGE_PIXEL, MakeSelect(DEKO9_STAGE_PIXEL, mask, {}, false, options)});
            if (earlyZ)
                emit(PlannedVariant{pass.psId, DEKO9_STAGE_PIXEL,
                                    MakeSelect(DEKO9_STAGE_PIXEL, mask, {}, true, options)});
            mask = (mask - candidates) & candidates;
        } while (mask);
    }
}

// Sorted, unique variant set.
class VariantPlan
{
public:
    void Add(const PassVariantDesc &pass, const PlanOptions &options)
    {
        PlanPassVariants(pass, options, [&](const PlannedVariant &v) { m_pending.push_back(v); });
    }
    void AddVariant(const PlannedVariant &v) { m_pending.push_back(v); }
    // Sorts and dedups what Add collected; call before Contains/Variants.
    const std::vector<PlannedVariant> &Finish()
    {
        m_variants.insert(m_variants.end(), m_pending.begin(), m_pending.end());
        m_pending.clear();
        std::sort(m_variants.begin(), m_variants.end());
        m_variants.erase(std::unique(m_variants.begin(), m_variants.end()), m_variants.end());
        return m_variants;
    }
    bool Contains(const PlannedVariant &v) const { return std::binary_search(m_variants.begin(), m_variants.end(), v); }
    const std::vector<PlannedVariant> &Variants() const { return m_variants; }

private:
    std::vector<PlannedVariant> m_pending, m_variants;
};

// ---- the draw side ------------------------------------------------------------

// What one draw of a pass binds that selects its variants.
struct DrawVariantState
{
    // Pixel sampler registers bound to a depth-format texture.
    uint32_t depthBoundMask = 0;
    bool alphaTest = false;          // D3DRS_ALPHATESTENABLE
    bool rasterAllowsEarlyZ = false; // RasterAllowsEarlyZ(render states, depth target)
    bool occlusionOpen = false;
    bool instanced = false;          // drawn through Deko9_DrawIndexedInstances
    InstanceLayout instance{};       // its layout
};

// The variants the device selects for this draw (Device::ApplyShaders,
// EarlyZCandidate, DrawInstances). An instanced draw whose layout overlaps
// the vertex shader's inputs is drawn per instance with the ordinary
// variant.
inline void DrawVariants(const PassVariantDesc &pass, const DrawVariantState &draw, const PlanOptions &options,
                         PlannedVariant *vs, PlannedVariant *ps)
{
    const bool instanced = draw.instanced && InstanceFits(pass.vs->inputMask, draw.instance.count);
    *vs = PlannedVariant{pass.vsId, DEKO9_STAGE_VERTEX,
                         MakeSelect(DEKO9_STAGE_VERTEX, 0, instanced ? draw.instance : InstanceLayout{}, false, options)};
    uint32_t mask = 0;
    for (uint32_t pending = pass.ps->samplerMask & ((1u << DEKO9_MAX_SAMPLERS) - 1u); pending; pending &= pending - 1u)
    {
        const uint32_t s = (uint32_t)__builtin_ctz(pending);
        if (CompareSampler((draw.depthBoundMask >> s) & 1u, pass.ps->samplerDim[s]))
            mask |= 1u << s;
    }
    const bool earlyZ = options.earlyZ && EarlyZShaderEligible(*pass.ps, draw.alphaTest) && !draw.occlusionOpen &&
                        draw.rasterAllowsEarlyZ;
    *ps = PlannedVariant{pass.psId, DEKO9_STAGE_PIXEL, MakeSelect(DEKO9_STAGE_PIXEL, mask, {}, earlyZ, options)};
}

} // namespace deko9
