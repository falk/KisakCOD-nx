#pragma once

// Present upscalers of the optional deko3d renderer: the engine renders the
// whole game at a lower resolution (r_renderResolution) and deko9 upscales
// the finished back buffer straight into the swapchain image at present, in
// one full-screen pass whose program r_fsrMode selects:
//   sgsr           Qualcomm Snapdragon Game Super Resolution v1 (default):
//                  bilinear colour plus an edge-directed 12-tap luma
//                  (green) correction with built-in sharpening, only on
//                  pixels whose 2x2 neighbourhood has an edge.
//   bilinear_rcas  hardware bilinear upscale fused with AMD FSR 1 RCAS: the
//                  RCAS cross is five bilinear samples of the back buffer at
//                  this output pixel and its neighbours (no intermediate
//                  image), sharpness from r_fsrSharpness.
//   bilinear       plain hardware bilinear (cheapest; A/B baseline).
//
// This header and deko9_fsr_shaders.cpp are host-compilable (no deko3d):
// switch_deko9_fsr_test compiles the GLSL with the host UAM and checks the
// constant setup and the CPU references (deko9_fsr_reference.h); the device
// integration is in deko9_fsr.cpp.
//
// Licenses: SGSR v1 is BSD-3-Clause (Qualcomm Innovation Center), RCAS is
// MIT (AMD FidelityFX FSR 1); both texts are in deko9_fsr_shaders.cpp.

#include <cstdint>
#include <string>

namespace deko9
{

// Values of r_fsrMode (engine enum order) and Deko9_SetUpscaleMode.
enum UpscaleMode : uint32_t
{
    UPSCALE_SGSR = 0,
    UPSCALE_BILINEAR_RCAS = 1,
    UPSCALE_BILINEAR = 2,
    UPSCALE_MODE_COUNT
};
const char *UpscaleModeName(uint32_t mode);

// Full-screen triangle (gl_VertexID 0..2, no vertex input); location 0
// carries the output-image UV with v = 0 at the top row (deko9 runs with
// DkDeviceFlags_OriginUpperLeft: NDC +Y is row 0, as in D3D).
extern const char kFsrVertexGlsl[];
// Fragment programs: sampler binding 0 = input image (linear, clamp),
// uniform block binding 1 = the mode's constants (std140; every mode has
// one, see UpscaleSource).
extern const char kSgsrGlsl[];
extern const char kBilinearRcasGlsl[];
extern const char kBilinearGlsl[];
// Selftest probe: each output pixel (x, y) is textureGather(input, uv, c)
// at the texel corner shared by texels (x..x+1, y..y+1), c = component
// from GatherProbeConstants. Records how the hardware gathers each
// component of each format (hardware returned wrong texels for components
// 1/2 of the BGRA8 back buffer).
extern const char kGatherProbeGlsl[];

// Native float-Z (r_deko9NativeFloatZ): the engine's "build floatz" pass
// re-renders the opaque scene into an R32F target holding each pixel's
// signed view depth (clip w, negated for depth-hack/viewmodel geometry;
// 2,000,000 where nothing was drawn) for the soft-particle, light-beam and
// depth-of-field shaders. kFloatZGlsl rebuilds the same values from the
// scene's depth buffer in one full-screen pass: sampler binding 0 = the
// depth image (texelFetch at the output pixel), uniform binding 1 =
// FloatZConstants. Per pixel, with d the stored window depth:
//   d <  viewmodel.z : floatz = viewmodel.x / (viewmodel.y - d)
//   otherwise        : floatz = scene.x / (scene.y - d)
//   denominator <= 0 or |floatz| >= scene.z : floatz = scene.z (the clear)
extern const char kFloatZGlsl[];

// Off-screen particles (r_halfResParticles; deko9_native.h
// Deko9_ParticleDepth / Deko9_ParticleComposite; CPU reference and constant
// setup in deko9_particles_reference.h). kHrpDepthGlsl: sampler 0 = scene
// depth, 1 = scene float-Z, uniform binding 1 = {rect x, y, last x, last y;
// factor - 1}; writes the chosen float-Z and gl_FragDepth. kHrpCompositeGlsl:
// sampler 0 = off-screen colour (C, T), 1 = off-screen float-Z, 2 = scene
// float-Z, uniform binding 1 = hrpref::CompositeConstants.
extern const char kHrpDepthGlsl[];
extern const char kHrpCompositeGlsl[];

struct FloatZConstants
{
    float viewmodel[4]; // A, B, split (window depth where scene geometry starts), 0
    float scene[4];     // A, B, clear value, 0
};

// One projection + viewport depth range. The projection is the engine's
// row-vector perspective (clip = view * M): clip.z = z * m22 + m32 and
// clip.w = z (m23 = 1, m33 = 0); the viewport maps NDC z (0..1, deko9 runs
// DkDeviceFlags_DepthZeroToOne) to window depth minZ..maxZ.
struct FloatZRange
{
    float m22, m32;
    float minZ, maxZ;
};
// Solves d = minZ + (maxZ - minZ) * (m22 + m32 / z) for z. The viewmodel
// range must lie below the scene range (the engine draws depth-hack
// geometry into 0..1/64 and the scene into 1/64..1), and it is negated as
// the build floatz technique does (depthFromClip.w = -1 under depth hack).
void FloatZSetup(FloatZConstants *out, const FloatZRange &scene, const FloatZRange &viewmodel, float clearValue);
// The fragment program's arithmetic on the CPU (same float operations):
// the oracle for the selftest and the engine's verify mode.
inline float FloatZReference(const FloatZConstants &c, float d)
{
    const float a = d < c.viewmodel[2] ? c.viewmodel[0] : c.scene[0];
    const float b = d < c.viewmodel[2] ? c.viewmodel[1] : c.scene[1];
    const float den = b - d;
    const float z = a / den;
    return den > 0.0f && (z < 0.0f ? -z : z) < c.scene[2] ? z : c.scene[2];
}

// Source of an upscale pass: the rectangle (x, y, w, h) of a texW x texH
// texture (dynamic resolution renders the scene into the top-left w x h of
// a larger target; the present path uses the whole texture). The vertex
// shader's UV covers the destination viewport 0..1; every program maps it
// to texture space as uvTransform.xy + uv * uvTransform.zw and clamps each
// bilinear tap to uvClamp (half a texel inside the rectangle) and each
// texel load to texelRect, so no tap reads outside the rectangle: at its
// borders the result equals the same upscaler run on the cropped image with
// clamp-to-edge sampling (deko9_fsr_reference.h UpscaleRgb8 on a crop; the
// host test checks UpscaleRgb8Source against it).
// Each program has two builds: as written (whole-texture source; the source
// fields are ignored, so the present path costs what it did) and
// SourceRectVariant(program) with DEKO9_SOURCE_RECT defined (the transform
// and clamps). A source rectangle equal to the whole texture needs no
// clamp: the sampler's clamp-to-edge gives the same borders.
std::string SourceRectVariant(const char *glsl);
struct UpscaleSource
{
    float uvTransform[4]; // x / texW, y / texH, w / texW, h / texH
    float uvClamp[4];     // (x + 0.5) / texW, (y + 0.5) / texH, (x + w - 0.5) / texW, (y + h - 0.5) / texH
    int32_t texelRect[4]; // x, y, x + w - 1, y + h - 1 (inclusive)
};
void UpscaleSourceSetup(UpscaleSource *out, int texWidth, int texHeight, int x, int y, int w, int h);

// Display gamma (r_gamma / the menu brightness). The engine hands the device
// a 256-entry 16-bit ramp per channel (D3DGAMMARAMP); Horizon has no gamma
// hardware, so the present pass applies it to the whole display, HUD and
// menus included, as the PC's ramp did: GammaVariant(glsl, curve) is any of
// the programs above with the ramp applied to its final colour. An identity
// ramp never reaches the shader (the present path keeps its plain blit /
// upscale program, so the default costs nothing).
//
// Two forms. The engine's ramp is a power curve, 65535 (i / 255) ^ e with
// e = 1 / r_gamma (GammaFitExponent recovers e from the entries): the
// program then computes pow(c, e) per channel, two MUFU operations each,
// which the pass's memory traffic hides. A ramp that is no single power curve
// is looked up in a 256-entry table (linear between entries); that lookup
// reads the uniform block at a per-pixel address, which the constant cache
// serves one unique address at a time, so a warp of 32 pixels pays for every
// distinct level it holds: on the console the table made the 1:1 pass 0.34
// -> 1.09 ms at 1280x720. The table stays as the fallback for a ramp from
// outside R_CalcGammaRamp. Either form is appended to the program's
// binding-1 block at GammaLutOffset(mode).
// Table layout: one vec4 per input level (red, green, blue, 0), so the lookup
// selects the channel statically: UAM compiles a dynamically indexed vec4
// component of a uniform block to a 16-byte-aligned load, which always reads
// component x. The slope to the next level sits in a second array at the
// same index, so both loads share one address register.
struct GammaConstants
{
    float value[256][4]; // output in 0..1 for input level i / 255
    float slope[256][4]; // value[i + 1] - value[i] (0 at level 255)
};
constexpr uint32_t kGammaLutVec4s = 512; // sizeof(GammaConstants) / 16
// The power-curve form's block: the exponent, then zeros.
struct GammaCurveConstants
{
    float curve[4];
};
// The ramp goes to the GPU in pushes of at most this many bytes: Ryujinx
// caches at most 2 KiB of inline constant data per update and aborts on a
// larger single push.
constexpr uint32_t kGammaPushBytes = 1024;
// Byte offset of the ramp inside the program block (the block's own constants
// come first): SGSR and bilinear+RCAS use 64 bytes, bilinear 32.
uint32_t GammaLutOffset(uint32_t mode);
// True when every entry is the identity i * 257.
bool GammaRampIsIdentity(const uint16_t *red, const uint16_t *green, const uint16_t *blue);
void GammaSetup(GammaConstants *out, const uint16_t *red, const uint16_t *green, const uint16_t *blue);
// The program's lookup on the CPU: the colour channel `v` (0..1) of channel
// 0..2 through the ramp, in the same float operations.
float GammaReference(const GammaConstants &c, int channel, float v);
// The exponent e for which every channel's entry i is 65535 (i / 255) ^ e
// rounded, within one 16-bit step; 0 when the ramp is not one power curve
// (the table form applies then). Positive, finite, never the identity's 1.
float GammaFitExponent(const uint16_t *red, const uint16_t *green, const uint16_t *blue);
void GammaCurveSetup(GammaCurveConstants *out, float exponent);
// The power-curve program's output for `v` (0..1) on the CPU.
float GammaCurveReference(float exponent, float v);
// `curve`: the power-curve form (uGammaCurve.x the exponent), else the table.
// Empty when the program lacks the pieces the rewrite needs.
std::string GammaVariant(const char *glsl, bool curve = false);

// std140 layouts of the fragment uniform blocks.
struct SgsrConstants
{
    float viewportInfo[4]; // 1/texW, 1/texH, texW, texH
    int32_t texelRect[4];  // UpscaleSource::texelRect
    float uvTransform[4];  // UpscaleSource::uvTransform
    float uvClamp[4];      // UpscaleSource::uvClamp
};
struct BilinearRcasConstants
{
    float step[4];        // 1/outW, 1/outH, sharpness (linear), 0 (destination-rect UV)
    float uvClamp[4];     // 0.5/outW, 0.5/outH, 1 - 0.5/outW, 1 - 0.5/outH (destination-rect UV)
    float uvTransform[4]; // UpscaleSource::uvTransform
    float srcClamp[4];    // UpscaleSource::uvClamp
};
struct BilinearConstants
{
    float uvTransform[4]; // UpscaleSource::uvTransform
    float srcClamp[4];    // UpscaleSource::uvClamp
};
struct GatherProbeConstants
{
    int32_t component[4]; // x = 0..3
};

// Whole-texture source (the present path): inWidth x inHeight texture.
void SgsrSetup(SgsrConstants *out, int inWidth, int inHeight);
// Sharpness in stops below maximum (0 = sharpest; +1 halves), as FsrRcasCon.
// Whole-texture source; outWidth x outHeight destination.
void BilinearRcasSetup(BilinearRcasConstants *out, float sharpnessStops, int outWidth, int outHeight);
// Rectangle source (any mode's block; `src` from UpscaleSourceSetup).
void SgsrSetup(SgsrConstants *out, int texWidth, int texHeight, const UpscaleSource &src);
void BilinearRcasSetup(BilinearRcasConstants *out, float sharpnessStops, int outWidth, int outHeight,
                       const UpscaleSource &src);
void BilinearSetup(BilinearConstants *out, const UpscaleSource &src);

} // namespace deko9
