#pragma once

// Pure predicate, no engine dependencies (host-testable with a plain
// `#include "r_view2d_alpha_skip.h"`; see switch_view2d_alpha_skip_test.cpp):
// whether a vertex alpha of 0 is a no-op for a given GfxStateBits[0] blend
// mode. R_AddCmdDrawStretchPic (r_rendercmds.cpp) uses this to drop a
// fully-transparent 2D quad before it is ever queued as a render command --
// mostly full-screen `white` and `gasmask_overlay` quads queued every frame
// regardless of their alpha.
//
// The constants below are local copies of src/gfx_d3d/r_state.h's
// GFXS0_* bit layout and the D3DBLEND/D3DBLENDOP ABI values (both fixed,
// retail-derived layouts); r_rendercmds.cpp static_asserts them equal to the
// real macros so a future change to either can't drift silently. Duplicated
// here (rather than included) because r_state.h drags in the D3D9 headers
// through rb_backend.h/r_material.h, which the host build does not have on
// its include path outside the full engine link.

#include <cstdint>

namespace r_view2d_alpha_skip
{
constexpr uint32_t kBlendOpRgbMask = 0x700;
constexpr uint32_t kBlendOpRgbShift = 0x8;
constexpr uint32_t kBlendOpAlphaMask = 0x7000000;
constexpr uint32_t kBlendOpAlphaShift = 0x18;
constexpr uint32_t kSrcBlendRgbMask = 0xF;
constexpr uint32_t kSrcBlendRgbShift = 0x0;
constexpr uint32_t kDstBlendRgbMask = 0xF0;
constexpr uint32_t kDstBlendRgbShift = 0x4;
constexpr uint32_t kSrcBlendAlphaMask = 0xF0000;
constexpr uint32_t kSrcBlendAlphaShift = 0x10;
constexpr uint32_t kDstBlendAlphaMask = 0xF00000;
constexpr uint32_t kDstBlendAlphaShift = 0x14;

// D3DBLENDOP_ADD / D3DBLEND_ONE / D3DBLEND_SRCALPHA / D3DBLEND_INVSRCALPHA.
constexpr uint32_t kBlendOpAdd = 1;
constexpr uint32_t kBlendOne = 2;
constexpr uint32_t kBlendSrcAlpha = 5;
constexpr uint32_t kBlendInvSrcAlpha = 6;

// A vertex alpha of 0 changes nothing on screen for a blend equation that
// reduces to "destination unchanged": the source factor is SRCALPHA (so
// alpha==0 zeroes the source term in that channel) and the destination
// factor is already alpha-independent at alpha==0 -- INVSRCALPHA (== 1-0 ==
// 1, an identity multiply) or a flat ONE (additive, also identity). Any
// other destination factor -- ZERO (paints black), SRCCOLOR/DESTCOLOR/etc --
// still changes the target at alpha==0 and must still draw. Requires ADD:
// the src*factor + dst*factor form this reasoning assumes; SUBTRACT/
// REVSUBTRACT/MIN/MAX combine differently. Blend must be enabled at all
// (kBlendOpRgbMask bits != 0, the same gate R_ChangeState_0 uses,
// r_state.cpp) -- an opaque technique always draws regardless of alpha.
// When no separate alpha blend op/factors are set, R_ChangeState_0 mirrors
// the RGB ones onto alpha (r_state.cpp), so an unset alpha field here falls
// back to the RGB one to match.
inline bool StateBits0AlphaZeroIsNoOp(uint32_t stateBits0)
{
    const uint32_t rgbOpField = stateBits0 & kBlendOpRgbMask;
    if (!rgbOpField)
        return false; // blend disabled: an opaque draw, alpha has no meaning here
    const uint32_t rgbOp = rgbOpField >> kBlendOpRgbShift;
    const uint32_t alphaOpField = stateBits0 & kBlendOpAlphaMask;
    const uint32_t alphaOp = alphaOpField ? (alphaOpField >> kBlendOpAlphaShift) : rgbOp;
    if (rgbOp != kBlendOpAdd || alphaOp != kBlendOpAdd)
        return false;

    const uint32_t srcRgb = (stateBits0 & kSrcBlendRgbMask) >> kSrcBlendRgbShift;
    const uint32_t dstRgb = (stateBits0 & kDstBlendRgbMask) >> kDstBlendRgbShift;
    const uint32_t srcAlphaField = stateBits0 & kSrcBlendAlphaMask;
    const uint32_t dstAlphaField = stateBits0 & kDstBlendAlphaMask;
    const uint32_t srcAlpha = srcAlphaField ? (srcAlphaField >> kSrcBlendAlphaShift) : srcRgb;
    const uint32_t dstAlpha = dstAlphaField ? (dstAlphaField >> kDstBlendAlphaShift) : dstRgb;

    auto srcGatedByAlpha = [](uint32_t f) { return f == kBlendSrcAlpha; };
    auto dstUnchangedAtAlphaZero = [](uint32_t f) { return f == kBlendInvSrcAlpha || f == kBlendOne; };

    return srcGatedByAlpha(srcRgb) && dstUnchangedAtAlphaZero(dstRgb) && srcGatedByAlpha(srcAlpha)
        && dstUnchangedAtAlphaZero(dstAlpha);
}
} // namespace r_view2d_alpha_skip
