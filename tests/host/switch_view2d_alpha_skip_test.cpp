// Host proof for r_view2d_alpha_skip::StateBits0AlphaZeroIsNoOp
// (src/gfx_d3d/r_view2d_alpha_skip.h): the predicate R_AddCmdDrawStretchPic
// (r_rendercmds.cpp) uses to drop a fully-transparent 2D quad before it is
// ever queued as a render command. Found costing measurable GPU time in the
// view2d pass: full-screen `white`/`gasmask_overlay` quads with
// blend srcalpha/invsrcalpha, queued every frame regardless of alpha.
//
// Exhaustively enumerates every (blendOpRgb, srcRgb, dstRgb) the 4-bit
// GfxStateBits[0] fields can hold -- with the alpha channel both mirrored
// (unset, the common case for real 2D materials) and set explicitly to the
// same values -- and checks the predicate's one safety-critical property
// against a from-scratch blend simulation: whenever the predicate says
// skip=true, does src*srcFactor + dst*dstFactor at alpha==0 actually equal
// dst exactly, for every source and destination colour and destination alpha sampled (soundness --
// never skip a draw that would have changed the target)? The predicate is
// deliberately narrower than "every blend mode that happens to be an
// identity at alpha==0" (e.g. ZERO/ONE is trivially a no-op for *any*
// alpha, not just 0, but isn't one of the two alpha-gated patterns it
// looks for), so the sweep does not require the converse. This is the
// "over the blend modes" sweep, not just the two known-safe pairs.

#include "src/gfx_d3d/r_view2d_alpha_skip.h"

#include <cstdio>

namespace
{
int g_failures;
int g_sweepFailures;

void Check(bool ok, const char *what, const char *detail = "")
{
    std::printf("%s:VIEW2D_ALPHA_SKIP_%s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok)
        ++g_failures;
}

using namespace r_view2d_alpha_skip;

uint32_t Pack(uint32_t blendOpRgb, uint32_t srcRgb, uint32_t dstRgb, uint32_t blendOpAlpha, uint32_t srcAlpha,
              uint32_t dstAlpha)
{
    uint32_t s0 = 0;
    s0 |= (srcRgb << kSrcBlendRgbShift) & kSrcBlendRgbMask;
    s0 |= (dstRgb << kDstBlendRgbShift) & kDstBlendRgbMask;
    s0 |= (blendOpRgb << kBlendOpRgbShift) & kBlendOpRgbMask;
    s0 |= (srcAlpha << kSrcBlendAlphaShift) & kSrcBlendAlphaMask;
    s0 |= (dstAlpha << kDstBlendAlphaShift) & kDstBlendAlphaMask;
    s0 |= (blendOpAlpha << kBlendOpAlphaShift) & kBlendOpAlphaMask;
    return s0;
}

// One channel of `factor` in `src*srcFactor + dst*dstFactor` (BLENDOP_ADD
// only -- the only op the predicate accepts) for one (srcC, srcA, dstC,
// dstA) sample. Factors are the D3DBLEND ABI values 1..10 this engine's
// s_blendTable_30 (r_state.cpp) maps 1:1 (identity), covering every factor
// the retail material data can actually select for these fields.
float BlendFactor(uint32_t factor, float srcC, float srcA, float dstC, float dstA)
{
    switch (factor)
    {
    case 1: return 0.0f;         // ZERO
    case 2: return 1.0f;         // ONE
    case 3: return srcC;         // SRCCOLOR
    case 4: return 1.0f - srcC;  // INVSRCCOLOR
    case 5: return srcA;         // SRCALPHA
    case 6: return 1.0f - srcA;  // INVSRCALPHA
    case 7: return dstA;         // DESTALPHA
    case 8: return 1.0f - dstA;  // INVDESTALPHA
    case 9: return dstC;         // DESTCOLOR
    case 10: return 1.0f - dstC; // INVDESTCOLOR
    default: return -1000.0f;    // out of range: never sampled as identity below
    }
}

// True iff, for every src colour / dst colour / dst alpha this stateBits0
// could ever be asked to composite with an alpha==0 source, the ADD result
// equals the original dst exactly -- the condition the predicate must
// recognise.
bool BlendIsIdentityAtAlphaZero(uint32_t stateBits0)
{
    const uint32_t blendOpRgb = (stateBits0 & kBlendOpRgbMask) >> kBlendOpRgbShift;
    if (!blendOpRgb)
        return false; // blend disabled: always an opaque draw (never a no-op skip)
    const uint32_t blendOpAlphaField = stateBits0 & kBlendOpAlphaMask;
    const uint32_t blendOpAlpha = blendOpAlphaField ? (blendOpAlphaField >> kBlendOpAlphaShift) : blendOpRgb;
    if (blendOpRgb != kBlendOpAdd || blendOpAlpha != kBlendOpAdd)
        return false; // only ADD's src*f+dst*f is modelled below

    const uint32_t srcRgb = (stateBits0 & kSrcBlendRgbMask) >> kSrcBlendRgbShift;
    const uint32_t dstRgb = (stateBits0 & kDstBlendRgbMask) >> kDstBlendRgbShift;
    const uint32_t srcAlphaField = stateBits0 & kSrcBlendAlphaMask;
    const uint32_t dstAlphaField = stateBits0 & kDstBlendAlphaMask;
    const uint32_t srcAlpha = srcAlphaField ? (srcAlphaField >> kSrcBlendAlphaShift) : srcRgb;
    const uint32_t dstAlpha = dstAlphaField ? (dstAlphaField >> kDstBlendAlphaShift) : dstRgb;

    static const float kSamples[] = {0.0f, 0.25f, 0.6f, 1.0f};
    for (float srcC : kSamples)
    {
        for (float dstC : kSamples)
        {
            for (float dstA : kSamples)
            {
                const float a = 0.0f; // the case we're skipping
                const float outC =
                    srcC * BlendFactor(srcRgb, srcC, a, dstC, dstA) + dstC * BlendFactor(dstRgb, srcC, a, dstC, dstA);
                const float outA =
                    a * BlendFactor(srcAlpha, srcC, a, dstC, dstA) + dstA * BlendFactor(dstAlpha, srcC, a, dstC, dstA);
                if (outC < dstC - 1e-5f || outC > dstC + 1e-5f)
                    return false;
                if (outA < dstA - 1e-5f || outA > dstA + 1e-5f)
                    return false;
            }
        }
    }
    return true;
}

// One sweep case: the predicate's soundness property, that skip=true implies
// the simulation confirms an alpha==0 identity (without spamming a PASS/FAIL
// line per combination -- 8*11*11*2 of them).
void SweepCase(uint32_t stateBits0)
{
    const bool got = StateBits0AlphaZeroIsNoOp(stateBits0);
    if (got && !BlendIsIdentityAtAlphaZero(stateBits0))
    {
        std::printf(
            "FAIL:VIEW2D_ALPHA_SKIP_SWEEP_CASE stateBits0=0x%08x predicate said skip but the target changes\n",
            stateBits0);
        ++g_sweepFailures;
    }
}
} // namespace

int main()
{
    // The two combinations the finding names as safe.
    Check(StateBits0AlphaZeroIsNoOp(Pack(kBlendOpAdd, kBlendSrcAlpha, kBlendInvSrcAlpha, 0, 0, 0)),
          "SRCALPHA_INVSRCALPHA_MIRRORED", "srcalpha/invsrcalpha, alpha fields unset (mirrors RGB)");
    Check(StateBits0AlphaZeroIsNoOp(
              Pack(kBlendOpAdd, kBlendSrcAlpha, kBlendOne, kBlendOpAdd, kBlendSrcAlpha, kBlendOne)),
          "SRCALPHA_ONE_EXPLICIT", "srcalpha/one, alpha fields explicit and matching");

    // Known-unsafe: still changes the target at alpha==0.
    Check(!StateBits0AlphaZeroIsNoOp(Pack(kBlendOpAdd, kBlendSrcAlpha, 1 /*ZERO*/, 0, 0, 0)), "SRCALPHA_ZERO",
          "fades to black at alpha=0: must still draw");
    Check(!StateBits0AlphaZeroIsNoOp(Pack(kBlendOpAdd, kBlendOne, 1 /*ZERO*/, 0, 0, 0)), "ONE_ZERO",
          "opaque replace, src not gated by alpha: must still draw");
    Check(!StateBits0AlphaZeroIsNoOp(0), "DISABLED", "blend disabled entirely: must still draw");
    Check(!StateBits0AlphaZeroIsNoOp(Pack(3 /*SUBTRACT*/, kBlendSrcAlpha, kBlendInvSrcAlpha, 0, 0, 0)),
          "SRCALPHA_INVSRCALPHA_SUBTRACT", "right factor pair but not ADD: must still draw");
    Check(!StateBits0AlphaZeroIsNoOp(
              Pack(kBlendOpAdd, kBlendSrcAlpha, kBlendInvSrcAlpha, kBlendOpAdd, kBlendSrcAlpha, 1 /*ZERO*/)),
          "MISMATCHED_ALPHA_CHANNEL", "RGB looks safe but the explicit alpha-channel dst factor is ZERO");

    // Exhaustive sweep: every (blendOpRgb 0..7, srcRgb 0..10, dstRgb 0..10)
    // the fields can encode (0 and reserved-looking values included), with
    // the alpha channel both mirrored and set explicitly to match RGB.
    int checked = 0;
    for (uint32_t blendOpRgb = 0; blendOpRgb <= 7; ++blendOpRgb)
    {
        for (uint32_t srcRgb = 0; srcRgb <= 10; ++srcRgb)
        {
            for (uint32_t dstRgb = 0; dstRgb <= 10; ++dstRgb)
            {
                SweepCase(Pack(blendOpRgb, srcRgb, dstRgb, 0, 0, 0)); // mirrored alpha
                SweepCase(Pack(blendOpRgb, srcRgb, dstRgb, blendOpRgb, srcRgb, dstRgb)); // explicit, matching
                checked += 2;
            }
        }
    }
    g_failures += g_sweepFailures;
    char detail[64];
    std::snprintf(detail, sizeof(detail), "%d combinations, %d mismatches", checked, g_sweepFailures);
    Check(g_sweepFailures == 0, "SWEEP", detail);

    return g_failures ? 1 : 0;
}
