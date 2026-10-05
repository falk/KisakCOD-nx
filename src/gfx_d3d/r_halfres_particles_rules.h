#pragma once

// Off-screen soft particles:
// the engine-wide rules, free of engine types so switch_halfres_particles_test
// drives them on the host.
//
//   - Classification: which emissive techniques draw into the off-screen
//     target. Decided once per material (its emissive technique), never
//     from level asset names.
//   - The blend remap for a redirected draw (colour blend kept, alpha
//     channel turned into transmittance), and the loud refusal of any blend
//     the composite cannot reproduce.
//
// The composite, downsample and CPU reference of the whole scheme are in
// src/deko9/deko9_particles_reference.h.

#include <cstdint>
#include <cstring>

namespace hrp
{

// GfxStateBits loadBits[0]/[1] fields (r_state.h GFXS0_*/GFXS1_*): blend
// factors are D3DBLEND values, ops D3DBLENDOP values (0 = blend off; alpha
// op 0 = same as the colour blend).
enum : uint32_t
{
    kSrcRgbMask = 0xFu,
    kDstRgbShift = 4,
    kOpRgbShift = 8,
    kSrcAlphaShift = 16,
    kDstAlphaShift = 20,
    kOpAlphaShift = 24,
    kBlendAlphaMask = 0x7FF0000u,
    kColorWriteRgb = 0x8000000u,
    kColorWriteAlpha = 0x10000000u,
    kDepthWrite = 0x1u,       // loadBits[1]
    kDepthTestDisable = 0x2u, // loadBits[1]
    kStencilMask = 0xC0u,     // loadBits[1]
};
enum : uint32_t
{
    kBlendZero = 1,
    kBlendOne = 2,
    kBlendSrcAlpha = 5,
    kBlendInvSrcAlpha = 6,
    kOpAdd = 1,
};

// The blends the premultiplied off-screen target reproduces exactly.
// Accumulation (C, T), start (0, 0, 0, 1); composite dst' = dst * T + C:
//   Over     (srcalpha, invsrcalpha): C = s.rgb * a + C * (1 - a), T *= (1 - a)
//   Add      (one, one):              C += s.rgb,                  T unchanged
//   AddAlpha (srcalpha, one):         C += s.rgb * a,              T unchanged
// Anything that reads the destination colour (invdstcolor:one screen
// blends, multiplicative blends) or subtracts cannot be composited later.
enum class Blend : uint8_t
{
    Unsupported,
    Over,
    Add,
    AddAlpha,
};

inline Blend BlendOf(uint32_t bits0)
{
    const uint32_t src = bits0 & kSrcRgbMask, dst = (bits0 >> kDstRgbShift) & 0xF, op = (bits0 >> kOpRgbShift) & 7;
    // The material's own alpha blend (retail FX materials carry one) is
    // replaced: it only ever reached the scene target's alpha channel,
    // which X8R8G8B8 does not store.
    if (op != kOpAdd)
        return Blend::Unsupported;
    if (src == kBlendSrcAlpha && dst == kBlendInvSrcAlpha)
        return Blend::Over;
    if (src == kBlendOne && dst == kBlendOne)
        return Blend::Add;
    if (src == kBlendSrcAlpha && dst == kBlendOne)
        return Blend::AddAlpha;
    return Blend::Unsupported;
}

// Pass state for a redirected draw: the colour blend unchanged, a separate
// alpha blend that keeps the transmittance in the alpha channel (Over:
// ZERO/INVSRCALPHA; the additive ones: ZERO/ONE), alpha writes on. False
// for an unsupported blend (the caller fails loudly).
inline bool RemapStateBits0(uint32_t bits0, uint32_t *out)
{
    const Blend blend = BlendOf(bits0);
    if (blend == Blend::Unsupported)
        return false;
    const uint32_t dstA = blend == Blend::Over ? kBlendInvSrcAlpha : kBlendOne;
    *out = (bits0 & ~kBlendAlphaMask) | (kBlendZero << kSrcAlphaShift) | (dstA << kDstAlphaShift) |
           (kOpAdd << kOpAlphaShift) | kColorWriteRgb | kColorWriteAlpha;
    return true;
}

enum class Class : uint8_t
{
    FullRes,
    Offscreen,
};

// Why a technique stays at full resolution (for the load-time log).
enum class Reason : uint8_t
{
    Offscreen,
    NoTechnique,
    NotSoftParticle, // not a zfeather technique, or no float-Z sample
    ViewDependent,   // falloff / eyeofs: beams and flares keep full resolution
    MultiPass,
    NeedsResolve,    // samples the resolved scene (distortion)
    Blend,
    DepthState,      // writes depth, no depth test, or stencil
};

inline const char *ReasonName(Reason r)
{
    switch (r)
    {
    case Reason::Offscreen: return "offscreen";
    case Reason::NoTechnique: return "no_technique";
    case Reason::NotSoftParticle: return "not_soft_particle";
    case Reason::ViewDependent: return "view_dependent";
    case Reason::MultiPass: return "multi_pass";
    case Reason::NeedsResolve: return "needs_resolve";
    case Reason::Blend: return "blend";
    case Reason::DepthState: return "depth_state";
    }
    return "?";
}

// Technique flags (r_drawsurf.h / R_DoesDrawSurfListInfoNeedFloatz).
enum : uint32_t
{
    kTechNeedsResolvedPostSun = 1,
    kTechNeedsResolvedScene = 2,
    kTechUsesFloatZ = 0x20,
};

// The engine-wide rule. A soft particle is a retail "zfeather*" technique
// (camera-facing sprites that fade against the float-Z scene depth) that
// samples float-Z; the "falloff" and "eyeofs" variants are view-angle
// beams and eye-offset flares, drawn at full resolution. It must be one
// pass, test depth without writing it or stencil, sample no resolved
// scene, and blend in a way the composite reproduces (BlendOf).
inline Reason Classify(const char *techniqueName, uint32_t techniqueFlags, uint32_t passCount,
                       const uint32_t loadBits[2])
{
    if (!techniqueName)
        return Reason::NoTechnique;
    if (std::strncmp(techniqueName, "zfeather", 8) != 0 || !(techniqueFlags & kTechUsesFloatZ))
        return Reason::NotSoftParticle;
    if (std::strstr(techniqueName, "falloff") || std::strstr(techniqueName, "eyeofs"))
        return Reason::ViewDependent;
    if (passCount != 1)
        return Reason::MultiPass;
    if (techniqueFlags & (kTechNeedsResolvedPostSun | kTechNeedsResolvedScene))
        return Reason::NeedsResolve;
    if (BlendOf(loadBits[0]) == Blend::Unsupported)
        return Reason::Blend;
    if ((loadBits[1] & (kDepthWrite | kDepthTestDisable | kStencilMask)) != 0)
        return Reason::DepthState;
    return Reason::Offscreen;
}

// Off-screen target geometry for a scene viewport of w x h at `factor`
// (2 = half resolution, 1 = the full-resolution plumbing proof): the
// covered size, rounded up so every scene pixel has a source texel.
inline void OffscreenSize(int w, int h, int factor, int *ow, int *oh)
{
    *ow = (w + factor - 1) / factor;
    *oh = (h + factor - 1) / factor;
}

// r_halfResParticles 3 (auto): whether the off-screen path is on, decided from
// the measured GPU cost of the particle-bearing passes (Emissive, plus Hrp
// when off-screen) averaged over a window of frames. The pass costs a fixed
// ~2 ms and shrinks the particles' own cost to about a quarter, so it only
// pays above a break-even particle cost; two thresholds and a dwell keep it
// from flapping, and the window restarts at every switch so each decision
// uses only frames drawn in the current mode.
struct AutoGate
{
    bool on = false;
    uint32_t frames = 0;      // frames in the current window
    uint64_t ns = 0;          // particle-pass ns in the window
    uint32_t sinceSwitch = 0; // frames since the last switch

    static constexpr uint32_t kWindow = 30;
    static constexpr uint32_t kDwell = 90;

    // One call per frame with that frame's particle-pass GPU ns; returns
    // whether the next frame draws off-screen.
    bool Update(uint64_t frameNs, double onMs, double offMs)
    {
        ++sinceSwitch;
        ns += frameNs;
        if (++frames < kWindow)
            return on;
        const double ms = (double)ns / frames / 1e6;
        frames = 0;
        ns = 0;
        if (sinceSwitch < kDwell)
            return on;
        if (!on && ms > onMs)
            on = true, sinceSwitch = 0;
        else if (on && ms < offMs)
            on = false, sinceSwitch = 0;
        return on;
    }
    void Reset()
    {
        *this = AutoGate();
    }
};

} // namespace hrp
