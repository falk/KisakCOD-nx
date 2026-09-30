#pragma once

// D3D9 shader bytecode -> deko3d DKSH for the optional deko3d renderer.
//
// MojoShader's GLSL ES 3 profile decodes the DXSO tokens; a strict rewrite
// turns its output into GLSL 4.60 with the fixed interface the deko9 device
// binds against; UAM compiles that to DKSH. The rewrite accepts only the
// constructs it maps and fails on anything else, so an unmapped shader is
// reported instead of drawn wrong.
//
// Interface contract (shared with deko9_device.cpp):
//   - float constants: one std140 UBO at binding 0 per stage holding the
//     whole D3D9 register file (cN is element N); the device keeps it in
//     sync with dkCmdBufPushConstants.
//   - samplers: sampler register N is texture binding N of its stage.
//   - VS inputs: input register vN is attribute location N.
//   - varyings: COLORn -> location n, TEXCOORDn -> location 2 + n.
//   - PS outputs: COLORn -> location n.

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

enum Deko9Stage : uint8_t
{
    DEKO9_STAGE_VERTEX,
    DEKO9_STAGE_PIXEL,
};

enum Deko9SamplerDim : uint8_t
{
    DEKO9_SAMPLER_NONE,
    DEKO9_SAMPLER_2D,
    DEKO9_SAMPLER_3D,
    DEKO9_SAMPLER_CUBE,
};

constexpr uint32_t DEKO9_VS_CONST_REGS = 256;
constexpr uint32_t DEKO9_PS_CONST_REGS = 224;
constexpr uint32_t DEKO9_MAX_SAMPLERS = 16;
constexpr uint32_t DEKO9_MAX_VS_INPUTS = 16;

struct Deko9ShaderInfo
{
    Deko9Stage stage;
    // Highest float constant register read, plus one (0 = none).
    uint32_t constRegs;
    // Bit N: sampler register N is declared; samplerDim[N] is its type.
    uint32_t samplerMask;
    Deko9SamplerDim samplerDim[DEKO9_MAX_SAMPLERS];
    // Vertex shaders: bit N: input register vN is declared with D3D
    // declaration usage inputUsage[N] / inputUsageIndex[N].
    uint32_t inputMask;
    uint8_t inputUsage[DEKO9_MAX_VS_INPUTS];
    uint8_t inputUsageIndex[DEKO9_MAX_VS_INPUTS];
    // Varying locations written (VS) or read (PS).
    uint32_t varyingMask;
    // Pixel shaders: the program can discard (texkill/clip -> `discard`),
    // and it writes depth (oDepth -> gl_FragDepth).
    bool kills;
    bool writesDepth;
    // Shadow-map lookups the shadowFilter rewrite moved to their centre
    // (0 without the rewrite or when nothing matched).
    uint32_t shadowCenterTaps;
};

// The shader pack (deko9_shaderpack.h) memcpy's this struct whole into and
// out of its on-disk records; it must stay trivially copyable (no vtable,
// no non-trivial member) or that round trip silently corrupts instead of
// failing loudly.
static_assert(std::is_trivially_copyable<Deko9ShaderInfo>::value, "Deko9ShaderInfo must stay trivially copyable");

uint64_t Deko9_HashBytecode(const void *bytecode, size_t bytes);

// Translates one DXSO program. shadowSamplerMask marks 2D sampler registers
// bound to depth textures: D3D9 compares against them (hardware shadow
// maps), so those become sampler2DShadow.
//
// instanceRegs (vertex shaders): constant registers the instanced variant
// reads from vertex attribute location 16 - instanceRegCount + i (the top
// locations; they must not overlap the shader's inputs, else it fails)
// instead of the constant buffer (instanced draws carry per-instance
// values, e.g. a static model's world matrix, in an instance-rate stream).
// The values are the ones the constant buffer would have held, so the
// variant computes exactly what the ordinary one does per instance.
//
// earlyFragmentTests (pixel shaders): the early-Z variant declares
// `layout(early_fragment_tests) in;`, so the depth/stencil test runs before
// the shader even though it can discard (Maxwell otherwise tests late for
// such programs and runs the whole shader for hidden pixels). Only exact
// for draws that write neither depth nor stencil (the device picks it only
// then: a discarded fragment then changes nothing but color). Fails for a
// program that writes depth.
//
// shadowFilter (pixel shaders, r_shadowFilter): 0 translates the shadow-map
// lookups as retail wrote them. 1 replaces each retail PCF kernel of a
// depth-compare sampler (shadowSamplerMask) by one lookup at its centre: the
// shader model 3 lit shaders average four hardware-compare taps at
// symmetric offsets around one position (spot: texldp at V +- pixelAdjust *
// V.w, `ps_rX = (V.wwww * [-]ps_rY) + V;` with V an interpolated input; sun
// cascades: texldl at B +- K, `ps_rX = ps_rB + [-]K;` with B a register,
// snapshotted before the taps). Every tap of a symmetric group moves to the
// base, the four lookups become identical and the compiler merges them into
// one hardware 2x2-PCF fetch, which is the shader model 2 filter. Lookups
// outside such groups (the shader model 2 cascade pair, manual-compare
// shaders) are untouched. info->shadowCenterTaps counts the moved lookups.
constexpr uint32_t DEKO9_MAX_INSTANCE_REGS = 16;
constexpr uint32_t DEKO9_SHADOW_FILTER_MODES = 2;
// Bumped whenever the shadowFilter rewrite's output changes (part of the
// cached DKSH name of the filtered variants).
constexpr uint32_t DEKO9_SHADOW_FILTER_VERSION = 2;
bool Deko9_TranslateShader(const void *bytecode, size_t bytes, uint32_t shadowSamplerMask,
                           std::string *glsl, Deko9ShaderInfo *info, std::string *error,
                           const uint8_t *instanceRegs = nullptr, uint32_t instanceRegCount = 0,
                           bool earlyFragmentTests = false, uint32_t shadowFilter = 0);

// Compiles translated GLSL to a DKSH image. Serialized internally: UAM's
// GLSL front end keeps global state.
bool Deko9_CompileDksh(Deko9Stage stage, const std::string &glsl, std::vector<uint8_t> *dksh,
                       std::string *error);

// Static scan of a single-program DKSH image: which hardware constant-buffer
// slots (c[0]..c[17]) its instruction stream reads. deko3d binds c[0] (driver
// constbuf: texture handles), c[1] (the program's compiler constants, only
// when constbuf1Size > 0) and c[2 + N] for GLSL uniform block binding N; a
// read from any other slot hits whatever address the channel holds for it,
// which can fault the GPU MMU (GCC read at a VA past the address-space
// limit). Maxwell SM5x encoding: the cbuf-operand opcode
// forms (top byte 0x48..0x4f, 0x51..0x53) carry the slot in bits 34..38 and
// LDC (opcode 0xef9) in bits 36..40; every fourth 64-bit word is a
// scheduling word; graphics programs start with the 0x50-byte SPH.
struct Deko9DkshInfo
{
    uint32_t programType;  // DkshProgramType_*: 0 vertex, 1 fragment, ...
    uint32_t entrypoint;   // code-section offset of the program (SPH first)
    uint32_t numGprs;
    uint32_t constbuf1Off; // compiler constants inside the code section
    uint32_t constbuf1Size;
    uint32_t codeSize;     // code section bytes
    uint32_t slotMask;     // bit N: c[N] is read
    uint32_t instructions; // instruction words decoded (0 = nothing decoded)
    bool earlyFragmentTests; // fragment programs: early depth/stencil tests forced
};
bool Deko9_ScanDksh(const uint8_t *dksh, size_t size, Deko9DkshInfo *info, std::string *error);
// The slots deko9 binds for a program of `info`: c[0], c[1] when it has
// compiler constants, c[2] (the stage's register file, uniform binding 0)
// and, for fragment programs, c[3] (binding 1: the present upscaler and
// probe constants). Bits of info->slotMask outside this mask are reads of
// slots left unbound.
uint32_t Deko9_BoundCbufSlots(const Deko9DkshInfo &info);

// Static cost of one compiled DKSH program (the r_deko9DrawCensus shader
// columns): GPRs from the program header, and the Maxwell instruction slots
// the program executes on a straight pass: every 64-bit slot after the
// 0x50-byte shader program header up to the trailing branch-to-self, minus
// the scheduling control word that leads each group of four and minus
// trailing NOPs.
// The instruction classes that do not run on the FP32 pipe are counted apart,
// since they decide what a pixel-bound shader is really waiting on: IPA
// (attribute interpolation, one per varying scalar read; the zfeather soft
// particles spend 15 of 38 slots there), texture ops (TEX/TEXS/TLD/TLD4/TXD/
// TXQ/TMML) and MUFU (rcp/rsq/lg2/ex2/sin/cos). Everything else, including
// the EXIT, is instrs - ipa - tex - mufu.
struct Deko9DkshStats
{
    uint32_t gprs;
    uint32_t instrs; // executed-slot instructions, NOPs excluded
    uint32_t nops;   // NOPs inside the program
    uint32_t ipa;    // attribute interpolations
    uint32_t tex;    // texture instructions
    uint32_t mufu;   // transcendental unit instructions
};
bool Deko9_DkshStats(const uint8_t *dksh, size_t size, Deko9DkshStats *out);
