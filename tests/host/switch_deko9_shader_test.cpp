// Host proof for the deko3d renderer's shader translation
// (src/deko9/deko9_shader.cpp): DXSO -> MojoShader GLSL ES 3 -> strict
// rewrite -> UAM DKSH.
//
// The built-in cases are hand-assembled vs_2_0/ps_2_0 token streams, so the
// gate needs no game data. With KISAK_DEKO_SHADER_CORPUS pointing at a
// directory of *.vs / *.ps programs (see `retail-boot-host ...
// deko-shader-dump`), every retail program must translate and compile too.

#include "src/deko9/deko9_shader.h"
#include "src/deko9/deko9_shaderpack.h"

#include <dirent.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int g_failures;

// Hardware lit-surface specks came from GLSL math whose edge
// cases differ from D3D9: normalize(0) and pow(0, 0) are NaN, and a guarded
// rcp/rsq of a flushed denormal is inf. Translated programs must route these
// through the deko9_ helpers; returns the first raw form found in main().
const char *RawGlslMath(const std::string &glsl)
{
    // vs_1_x/vs_2_x bodies are renamed deko9_main (a colour-saturating
    // main() follows them); check the real body.
    size_t main = glsl.find("void deko9_main()");
    if (main == std::string::npos)
        main = glsl.find("void main()");
    if (main == std::string::npos)
        return "no main()";
    const std::string body = glsl.substr(main);
    if (body.find("normalize(") != std::string::npos)
        return "normalize(";
    for (size_t at = 0; (at = body.find("pow(", at)) != std::string::npos; ++at)
    {
        if (at == 0 || body[at - 1] != '_')
            return "pow(";
    }
    if (body.find("? FLT_MAX : 1.0 /") != std::string::npos)
        return "unclamped rcp";
    if (body.find("? FLT_MAX : inversesqrt(") != std::string::npos)
        return "unclamped rsq";
    return nullptr;
}

void Check(bool ok, const char *what)
{
    if (!ok)
    {
        std::printf("FAIL:DEKO9_SHADER %s\n", what);
        ++g_failures;
    }
}

// vs_2_0: dcl_position v0; mov oPos, v0; mov oT0, c5
const uint32_t kVs[] = {
    0xFFFE0200,
    0x0200001F, 0x80000000, 0x900F0000,
    0x02000001, 0xC00F0000, 0x90E40000,
    0x02000001, 0xE00F0000, 0xA0E40005,
    0x0000FFFF,
};

// vs_2_0: dcl_position v0; mov oPos, v0; mov oD0, c5
const uint32_t kVsColor[] = {
    0xFFFE0200,
    0x0200001F, 0x80000000, 0x900F0000,
    0x02000001, 0xC00F0000, 0x90E40000,
    0x02000001, 0xD00F0000, 0xA0E40005,
    0x0000FFFF,
};

// ps_2_0: dcl t0; dcl_2d s0; texld r0, t0, s0; mul r0, r0, c3; mov oC0, r0
const uint32_t kPs[] = {
    0xFFFF0200,
    0x0200001F, 0x80000000, 0xB00F0000,
    0x0200001F, 0x90000000, 0xA00F0800,
    0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800,
    0x03000005, 0x800F0000, 0x80E40000, 0xA0E40003,
    0x02000001, 0x800F0800, 0x80E40000,
    0x0000FFFF,
};

// Same as kPs but texldp (projected), the form retail shadow lookups use.
const uint32_t kPsProj[] = {
    0xFFFF0200,
    0x0200001F, 0x80000000, 0xB00F0000,
    0x0200001F, 0x90000000, 0xA00F0800,
    0x03010042, 0x800F0000, 0xB0E40000, 0xA0E40800,
    0x02000001, 0x800F0800, 0x80E40000,
    0x0000FFFF,
};

// ps_3_0 retail-shaped PCF offset taps (r_shadowFilter): two taps at
// v0 +- c0 * v0.w through the shadow sampler s0, averaged by an add.
//   dcl_texcoord0 v0; dcl_2d s0
//   mad r1, v0.w, c0, v0; mad r2, v0.w, -c0, v0
//   texldp r1, r1, s0; texldp r2, r2, s0; add r0, r1, r2; mov oC0, r0
const uint32_t kPsShadowTaps[] = {
    0xFFFF0300,
    0x0200001F, 0x80000005, 0x900F0000,
    0x0200001F, 0x90000000, 0xA00F0800,
    0x04000004, 0x800F0001, 0x90FF0000, 0xA0E40000, 0x90E40000,
    0x04000004, 0x800F0002, 0x90FF0000, 0xA1E40000, 0x90E40000,
    0x03010042, 0x800F0001, 0x80E40001, 0xA0E40800,
    0x03010042, 0x800F0002, 0x80E40002, 0xA0E40800,
    0x03000002, 0x800F0000, 0x80E40001, 0x80E40002,
    0x02000001, 0x800F0800, 0x80E40000,
    0x0000FFFF,
};
// The same with `add r1, r1, c1` after the first tap: the two lookups are no
// longer a symmetric pair around one base, so neither may move.
const uint32_t kPsShadowTapsMoved[] = {
    0xFFFF0300,
    0x0200001F, 0x80000005, 0x900F0000,
    0x0200001F, 0x90000000, 0xA00F0800,
    0x04000004, 0x800F0001, 0x90FF0000, 0xA0E40000, 0x90E40000,
    0x03000002, 0x800F0001, 0x80E40001, 0xA0E40001,
    0x04000004, 0x800F0002, 0x90FF0000, 0xA1E40000, 0x90E40000,
    0x03010042, 0x800F0001, 0x80E40001, 0xA0E40800,
    0x03010042, 0x800F0002, 0x80E40002, 0xA0E40800,
    0x03000002, 0x800F0000, 0x80E40001, 0x80E40002,
    0x02000001, 0x800F0800, 0x80E40000,
    0x0000FFFF,
};
// ps_3_0 sun-cascade shape: texldl taps at r0 +- c0 around a register base
// that a later write could clobber (the rewrite snapshots it).
//   dcl_texcoord0 v0; dcl_2d s0; mov r0, v0
//   add r1, r0, c0; add r2, r0, -c0; texldl r1, r1, s0; texldl r2, r2, s0
//   add r0, r1, r2; mov oC0, r0
const uint32_t kPsShadowTapsLod[] = {
    0xFFFF0300,
    0x0200001F, 0x80000005, 0x900F0000,
    0x0200001F, 0x90000000, 0xA00F0800,
    0x02000001, 0x800F0000, 0x90E40000,
    0x03000002, 0x800F0001, 0x80E40000, 0xA0E40000,
    0x03000002, 0x800F0002, 0x80E40000, 0xA1E40000,
    0x0300005F, 0x800F0001, 0x80E40001, 0xA0E40800,
    0x0300005F, 0x800F0002, 0x80E40002, 0xA0E40800,
    0x03000002, 0x800F0000, 0x80E40001, 0x80E40002,
    0x02000001, 0x800F0800, 0x80E40000,
    0x0000FFFF,
};

// Slots the program reads that deko9 never binds for its stage (0 = none):
// the static check behind the GPU MMU fault investigation (GCC read at a VA
// past the address-space limit). Any such read would hit an address the
// channel never had programmed.
uint32_t UnboundCbufSlots(const std::vector<uint8_t> &dksh, std::string *error, Deko9DkshInfo *infoOut = nullptr)
{
    Deko9DkshInfo info{};
    if (!Deko9_ScanDksh(dksh.data(), dksh.size(), &info, error))
        return 0xffffffffu;
    if (!info.instructions)
    {
        *error = "no instructions decoded";
        return 0xffffffffu;
    }
    if (infoOut)
        *infoOut = info;
    return info.slotMask & ~Deko9_BoundCbufSlots(info);
}

bool TranslateAndCompile(const void *code, size_t bytes, uint32_t shadowMask,
                         Deko9ShaderInfo *info, std::string *glsl, std::string *error,
                         std::vector<uint8_t> *dkshOut = nullptr)
{
    if (!Deko9_TranslateShader(code, bytes, shadowMask, glsl, info, error))
        return false;
    std::vector<uint8_t> dksh;
    if (!Deko9_CompileDksh(info->stage, *glsl, &dksh, error) || dksh.size() <= 256)
        return false;
    if (dkshOut)
        *dkshOut = std::move(dksh);
    return true;
}

// ps_2_0: kPs plus three more multiplies (r0 *= c3 three times).
const uint32_t kPsLonger[] = {
    0xFFFF0200,
    0x0200001F, 0x80000000, 0xB00F0000,
    0x0200001F, 0x90000000, 0xA00F0800,
    0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800,
    0x03000005, 0x800F0000, 0x80E40000, 0xA0E40003,
    0x03000005, 0x800F0000, 0x80E40000, 0xA0E40004,
    0x03000005, 0x800F0000, 0x80E40000, 0xA0E40005,
    0x03000005, 0x800F0000, 0x80E40000, 0xA0E40006,
    0x02000001, 0x800F0800, 0x80E40000,
    0x0000FFFF,
};

// ps_2_0: dcl t0; texkill t0; mov oC0, c0 (a discarding program)
const uint32_t kPsKill[] = {
    0xFFFF0200,
    0x0200001F, 0x80000000, 0xB00F0000,
    0x01000041, 0xB00F0000,
    0x02000001, 0x800F0800, 0xA0E40000,
    0x0000FFFF,
};
// ps_2_0: mov oC0, c0; mov oDepth, c0.x (a depth-writing program)
const uint32_t kPsDepth[] = {
    0xFFFF0200,
    0x02000001, 0x800F0800, 0xA0E40000,
    0x02000001, 0x900F0800, 0xA0000000,
    0x0000FFFF,
};

// r_shadowFilter 1: PCF offset taps of a depth-compare sampler move to
// their centre and compile to one depth-compare fetch; filter 0 and
// non-shadow samplers keep the retail lookups.
void ShadowFilterCases()
{
    Deko9ShaderInfo info{};
    std::string glsl, error;
    std::vector<uint8_t> dksh;
    Deko9DkshStats retail{}, centre{};
    const bool retailOk = Deko9_TranslateShader(kPsShadowTaps, sizeof(kPsShadowTaps), 1u, &glsl, &info, &error) &&
                          Deko9_CompileDksh(info.stage, glsl, &dksh, &error) &&
                          Deko9_DkshStats(dksh.data(), dksh.size(), &retail);
    if (!retailOk)
        std::printf("shadow taps error: %s\n%s\n", error.c_str(), glsl.c_str());
    Check(retailOk && info.shadowCenterTaps == 0 && glsl.find("deko9_shadowProj(ps_s0, ps_r1)") != std::string::npos,
          "shadow filter 0 keeps the offset taps");
    const bool centreOk =
        Deko9_TranslateShader(kPsShadowTaps, sizeof(kPsShadowTaps), 1u, &glsl, &info, &error, nullptr, 0, false, 1) &&
        Deko9_CompileDksh(info.stage, glsl, &dksh, &error) && Deko9_DkshStats(dksh.data(), dksh.size(), &centre);
    if (!centreOk)
        std::printf("shadow centre error: %s\n%s\n", error.c_str(), glsl.c_str());
    Check(centreOk && info.shadowCenterTaps == 2 && glsl.find("deko9_shadowProj(ps_s0, ps_r1)") == std::string::npos &&
              glsl.find("deko9_shadowProj(ps_s0, ps_v0)") != std::string::npos,
          "shadow filter 1 moves both taps to the centre");
    if (retailOk && centreOk)
        std::printf("DEKO9_SHADOW_FILTER taps: retail instrs=%u tex=%u, centre instrs=%u tex=%u\n", retail.instrs,
                    retail.tex, centre.instrs, centre.tex);
    Check(retailOk && centreOk && retail.tex == 2 && centre.tex == 1 && centre.instrs < retail.instrs,
          "shadow filter 1: the two depth-compare fetches become one");
    Check(Deko9_TranslateShader(kPsShadowTapsMoved, sizeof(kPsShadowTapsMoved), 1u, &glsl, &info, &error, nullptr, 0,
                                false, 1) &&
              info.shadowCenterTaps == 0 && glsl.find("deko9_shadowProj(ps_s0, ps_r1)") != std::string::npos &&
              glsl.find("deko9_shadowProj(ps_s0, ps_r2)") != std::string::npos,
          "shadow filter 1 leaves an asymmetric pair of taps alone");
    const bool lodOk = Deko9_TranslateShader(kPsShadowTapsLod, sizeof(kPsShadowTapsLod), 1u, &glsl, &info, &error,
                                             nullptr, 0, false, 1) &&
                       Deko9_CompileDksh(info.stage, glsl, &dksh, &error);
    if (!lodOk || info.shadowCenterTaps != 2)
        std::printf("shadow lod taps error: %s\n%s\n", error.c_str(), glsl.c_str());
    Check(lodOk && info.shadowCenterTaps == 2 && glsl.find("vec4 deko9_sc0 = ps_r0;") != std::string::npos &&
              glsl.find("deko9_shadowLod(ps_s0, deko9_sc0, deko9_sc0.w)") != std::string::npos,
          "shadow filter 1 moves register-based texldl taps to a snapshot of the base");
    Check(Deko9_TranslateShader(kPsShadowTaps, sizeof(kPsShadowTaps), 0u, &glsl, &info, &error, nullptr, 0, false, 1) &&
              info.shadowCenterTaps == 0,
          "shadow filter 1 leaves non-shadow samplers alone");
    Check(!Deko9_TranslateShader(kPsShadowTaps, sizeof(kPsShadowTaps), 1u, &glsl, &info, &error, nullptr, 0, false,
                                 DEKO9_SHADOW_FILTER_MODES),
          "unknown shadow filter mode is rejected");
}

// Early-Z variant (r_deko9EarlyZ): discard and depth-write detection, the
// forced early tests in the GLSL and in the DKSH program header, and the
// refusals (vertex shader, depth-writing pixel shader).
void EarlyZCases()
{
    Deko9ShaderInfo info{};
    std::string glsl, error;
    std::vector<uint8_t> base, ez;
    Check(TranslateAndCompile(kPsKill, sizeof(kPsKill), 0, &info, &glsl, &error, &base), "texkill ps translate+compile");
    Check(info.kills && !info.writesDepth, "texkill ps: kills, no depth write");
    Check(glsl.find("early_fragment_tests") == std::string::npos, "ordinary variant has no early tests");
    Deko9DkshInfo scan{};
    Check(Deko9_ScanDksh(base.data(), base.size(), &scan, &error) && !scan.earlyFragmentTests,
          "ordinary variant DKSH: early tests not forced");
    const bool ezOk = Deko9_TranslateShader(kPsKill, sizeof(kPsKill), 0, &glsl, &info, &error, nullptr, 0, true) &&
                      Deko9_CompileDksh(DEKO9_STAGE_PIXEL, glsl, &ez, &error);
    Check(ezOk, "early-Z variant translate+compile");
    Check(glsl.find("layout(early_fragment_tests) in;") != std::string::npos && glsl.find("discard") != std::string::npos,
          "early-Z variant declares early tests and keeps the discard");
    Check(ezOk && Deko9_ScanDksh(ez.data(), ez.size(), &scan, &error) && scan.earlyFragmentTests &&
              !(scan.slotMask & ~Deko9_BoundCbufSlots(scan)),
          "early-Z variant DKSH: early tests forced, bound slots only");
    Check(TranslateAndCompile(kPs, sizeof(kPs), 0, &info, &glsl, &error) && !info.kills && !info.writesDepth,
          "texld ps: no discard, no depth write");
    Check(TranslateAndCompile(kPsDepth, sizeof(kPsDepth), 0, &info, &glsl, &error) && info.writesDepth,
          "oDepth ps: writes depth");
    Check(!Deko9_TranslateShader(kPsDepth, sizeof(kPsDepth), 0, &glsl, &info, &error, nullptr, 0, true),
          "early-Z variant of a depth-writing ps is refused");
    Check(!Deko9_TranslateShader(kVs, sizeof(kVs), 0, &glsl, &info, &error, nullptr, 0, true),
          "early-Z variant of a vertex shader is refused");
}

// Static cost read from a DKSH image (r_deko9EmissiveCensus): GPRs from the
// program header, executed instruction slots without scheduling words or
// padding. A longer program must count more instructions; a truncated or
// non-DKSH image must be rejected.
void DkshStatsCases()
{
    Deko9ShaderInfo info{};
    std::string glsl, error;
    std::vector<uint8_t> shortDksh, longDksh;
    Check(TranslateAndCompile(kPs, sizeof(kPs), 0, &info, &glsl, &error, &shortDksh) &&
              TranslateAndCompile(kPsLonger, sizeof(kPsLonger), 0, &info, &glsl, &error, &longDksh),
          "dksh stats inputs compile");
    Deko9DkshStats a{}, b{};
    const bool okA = Deko9_DkshStats(shortDksh.data(), shortDksh.size(), &a);
    const bool okB = Deko9_DkshStats(longDksh.data(), longDksh.size(), &b);
    std::printf("DEKO9_DKSH_STATS short gprs=%u instrs=%u nops=%u ipa=%u tex=%u mufu=%u long gprs=%u instrs=%u "
                "nops=%u ipa=%u tex=%u mufu=%u\n",
                a.gprs, a.instrs, a.nops, a.ipa, a.tex, a.mufu, b.gprs, b.instrs, b.nops, b.ipa, b.tex, b.mufu);
    Check(okA && okB, "dksh stats parse");
    // texld + mul + output: at least a texture op, an FMUL per channel or a
    // vector form, and EXIT; far below the 64-slot padding granularity.
    Check(a.gprs >= 2 && a.gprs <= 255 && a.instrs >= 3 && a.instrs < 64, "short ps stats in range");
    Check(b.instrs > a.instrs, "three more multiplies count more instructions");
    // Instruction classes: one texld = one texture op; its 2D coordinate is
    // two interpolations plus the perspective IPA of 1/w (whose reciprocal
    // is the only MUFU); the extra multiplies of the longer program are
    // FP32-pipe work and change none of the three.
    Check(a.tex == 1 && a.ipa >= 3 && a.mufu == 1, "short ps: 1 tex, >= 3 ipa, 1 mufu");
    Check(b.tex == a.tex && b.ipa == a.ipa && b.mufu == a.mufu, "more multiplies keep the ipa/tex/mufu classes");
    Check(a.ipa + a.tex + a.mufu < a.instrs, "classes leave room for the FP32 work and EXIT");
    Deko9DkshStats c{};
    Check(!Deko9_DkshStats(shortDksh.data(), 20, &c), "truncated dksh rejected");
    std::vector<uint8_t> bad = shortDksh;
    bad[0] = 'X';
    Check(!Deko9_DkshStats(bad.data(), bad.size(), &c), "non-DKSH magic rejected");
}

void BuiltInCases()
{
    Deko9ShaderInfo info{};
    std::string glsl, error;

    const bool vsOk = TranslateAndCompile(kVs, sizeof(kVs), 0, &info, &glsl, &error);
    if (!vsOk)
        std::printf("vs error: %s\n%s\n", error.c_str(), glsl.c_str());
    Check(vsOk, "synthetic vs_2_0 translate+compile");
    Check(glsl.find("invariant gl_Position;") != std::string::npos, "vs position declared invariant");
    Check(info.stage == DEKO9_STAGE_VERTEX, "vs stage");
    Check(info.constRegs == 6, "vs constRegs == 6 (c5 read)");
    Check(info.inputMask == 1u && info.inputUsage[0] == 0, "vs v0 POSITION input");
    Check(info.varyingMask == (1u << 2), "vs oT0 -> location 2");
    Check(glsl.find("layout(location = 0) in vec4 vs_v0;") != std::string::npos,
          "vs input location");
    Check(glsl.find("#define vs_c5 vs_c_file[5]") != std::string::npos,
          "vs c5 addresses register file slot 5");

    // D3D9 saturates vs_1_x/vs_2_x COLOR outputs; MojoShader does not.
    const bool colorOk = TranslateAndCompile(kVsColor, sizeof(kVsColor), 0, &info, &glsl, &error);
    if (!colorOk)
        std::printf("vs color error: %s\n%s\n", error.c_str(), glsl.c_str());
    Check(colorOk, "synthetic vs_2_0 oD0 translate+compile");
    Check(glsl.find("io_10_0 = clamp(io_10_0, 0.0, 1.0);") != std::string::npos &&
              glsl.find("void deko9_main()") != std::string::npos,
          "vs_2_0 oD0 saturated after the shader body");

    const bool psOk = TranslateAndCompile(kPs, sizeof(kPs), 0, &info, &glsl, &error);
    if (!psOk)
        std::printf("ps error: %s\n%s\n", error.c_str(), glsl.c_str());
    Check(psOk, "synthetic ps_2_0 translate+compile");
    Check(info.stage == DEKO9_STAGE_PIXEL, "ps stage");
    Check(info.constRegs == 4, "ps constRegs == 4 (c3 read)");
    Check(info.samplerMask == 1u && info.samplerDim[0] == DEKO9_SAMPLER_2D, "ps s0 2D");
    Check(info.varyingMask == (1u << 2), "ps t0 <- location 2");
    Check(glsl.find("layout(binding = 0) uniform sampler2D ps_s0;") != std::string::npos,
          "ps sampler binding");

    // A plain texld on a depth-compare sampler compares against the full
    // source register (D3D9 uses its .z).
    const bool texldShadowOk = TranslateAndCompile(kPs, sizeof(kPs), 1u, &info, &glsl, &error);
    if (!texldShadowOk)
        std::printf("texld shadow error: %s\n%s\n", error.c_str(), glsl.c_str());
    Check(texldShadowOk && glsl.find("deko9_shadow(ps_s0, ps_t0)") != std::string::npos,
          "non-projected shadow lookup uses the whole coordinate");
    const bool shadowOk = TranslateAndCompile(kPsProj, sizeof(kPsProj), 1u, &info, &glsl, &error);
    if (!shadowOk)
        std::printf("shadow error: %s\n%s\n", error.c_str(), glsl.c_str());
    Check(shadowOk, "projected shadow lookup translate+compile");
    Check(glsl.find("sampler2DShadow ps_s0") != std::string::npos &&
              glsl.find("deko9_shadowProj(ps_s0,") != std::string::npos,
          "shadow sampler rewritten");
    Check(!Deko9_TranslateShader(kPsProj, sizeof(kPsProj), 2u, &glsl, &info, &error),
          "shadow mask naming an undeclared sampler is rejected");

    // Instanced variant: c5 comes from instance attribute 16 + 0, other
    // registers from the constant file; pixel shaders and oversized layouts
    // are rejected.
    const uint8_t instRegs[2] = {2, 5};
    const bool instOk = Deko9_TranslateShader(kVs, sizeof(kVs), 0, &glsl, &info, &error, instRegs, 2);
    std::vector<uint8_t> instDksh;
    const bool instCompiled = instOk && Deko9_CompileDksh(info.stage, glsl, &instDksh, &error);
    if (!instCompiled)
        std::printf("instanced vs error: %s\n%s\n", error.c_str(), glsl.c_str());
    Check(instCompiled, "instanced vs translate+compile");
    Check(glsl.find("layout(location = 15) in vec4 deko9_inst1;\n#define vs_c5 deko9_inst1") != std::string::npos &&
              glsl.find("vs_c_file[5]") == std::string::npos,
          "instanced vs reads c5 from instance attribute 15 (top locations)");
    Check(!Deko9_TranslateShader(kPs, sizeof(kPs), 0, &glsl, &info, &error, instRegs, 2),
          "instance layout on a pixel shader is rejected");
    uint8_t overlap[16];
    for (uint8_t i = 0; i < 16; ++i)
        overlap[i] = i;
    Check(!Deko9_TranslateShader(kVs, sizeof(kVs), 0, &glsl, &info, &error, overlap, 16),
          "instance attributes overlapping a vs input are rejected");
    uint8_t tooMany[DEKO9_MAX_INSTANCE_REGS + 1] = {};
    Check(!Deko9_TranslateShader(kVs, sizeof(kVs), 0, &glsl, &info, &error, tooMany, sizeof(tooMany)),
          "instance layout over 16 registers is rejected");

    const uint32_t garbage[] = {0x12345678, 0x0000FFFF};
    Check(!Deko9_TranslateShader(garbage, sizeof(garbage), 0, &glsl, &info, &error),
          "garbage bytecode is rejected");
    std::vector<uint8_t> dksh;
    Check(!Deko9_CompileDksh(DEKO9_STAGE_PIXEL, "#version 460\nvoid main() { oops }\n", &dksh,
                             &error),
          "invalid GLSL is rejected by UAM");
    Check(Deko9_HashBytecode("abc", 3) == 0xe71fa2190541574bull, "FNV-1a 64 hash");

    // Constant-buffer slot scan (Deko9_ScanDksh). The translated ps reads
    // its register file (c[2]) and the driver constbuf for its sampler
    // handle; the control program reads uniform binding 5 = c[7], which
    // deko9 never binds, and the scan must say so (the runtime refuses such
    // a program, the corpus check below fails on one).
    std::vector<uint8_t> psDksh;
    Deko9DkshInfo scan{};
    const bool psScanOk = TranslateAndCompile(kPs, sizeof(kPs), 0, &info, &glsl, &error, &psDksh) &&
                          UnboundCbufSlots(psDksh, &error, &scan) == 0;
    if (!psScanOk)
        std::printf("ps scan error: %s\n", error.c_str());
    Check(psScanOk, "translated ps reads only bound constant-buffer slots");
    Check(scan.programType == 1 && scan.entrypoint == 0x30 && scan.instructions > 4 && (scan.slotMask & 4u),
          "ps scan decodes the fragment program and its c[2] register-file reads");
    std::vector<uint8_t> controlDksh;
    Deko9DkshInfo control{};
    const bool controlOk =
        Deko9_CompileDksh(DEKO9_STAGE_PIXEL,
                          "#version 460\nlayout(location = 0) out vec4 o;\n"
                          "layout(std140, binding = 5) uniform B { vec4 v; };\nvoid main() { o = v; }\n",
                          &controlDksh, &error) &&
        UnboundCbufSlots(controlDksh, &error, &control) == (1u << 7);
    if (!controlOk)
        std::printf("control scan error: %s slots=0x%x\n", error.c_str(), control.slotMask);
    Check(controlOk, "scan reports the control program's unbound c[7] read (binding 5)");
    std::vector<uint8_t> vsDksh;
    const bool vsScanOk = TranslateAndCompile(kVs, sizeof(kVs), 0, &info, &glsl, &error, &vsDksh) &&
                          UnboundCbufSlots(vsDksh, &error, &scan) == 0 && scan.programType == 0 &&
                          (scan.slotMask & 4u);
    Check(vsScanOk, "translated vs reads only c[2] of the bound slots");
}

bool ReadFile(const std::string &path, std::vector<uint8_t> *bytes)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    bytes->resize(size > 0 ? (size_t)size : 0);
    const bool ok = size > 0 && std::fread(bytes->data(), 1, bytes->size(), file) == bytes->size();
    std::fclose(file);
    return ok;
}

void CorpusCases(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
    {
        Check(false, "KISAK_DEKO_SHADER_CORPUS is not a readable directory");
        return;
    }
    uint32_t total = 0, passed = 0, shadowVariants = 0, shadowUnmapped = 0, instancedVariants = 0;
    uint32_t shadowFilterVariants = 0;
    uint64_t shadowFilterInstrs[2] = {}, shadowFilterTex[2] = {};
    uint32_t pixelPrograms = 0, killing = 0, depthWriting = 0, earlyZVariants = 0;
    uint64_t guardInstrs[2] = {}, guardBranches[2] = {};
    uint32_t guardTernaries = 0, guardOverRegs = 0;
    const char *statsPath = std::getenv("KISAK_DEKO_SHADER_STATS");
    FILE *statsFile = statsPath ? std::fopen(statsPath, "w") : nullptr;
    while (dirent *entry = readdir(d))
    {
        const size_t len = std::strlen(entry->d_name);
        if (len < 4 || (std::strcmp(entry->d_name + len - 3, ".vs") &&
                        std::strcmp(entry->d_name + len - 3, ".ps")))
            continue;
        ++total;
        std::vector<uint8_t> bytes;
        Deko9ShaderInfo info{};
        std::string glsl, error;
        const std::string path = std::string(dir) + "/" + entry->d_name;
        if (!ReadFile(path, &bytes))
        {
            std::printf("DEKO9_CORPUS_FAIL %s read\n", entry->d_name);
            continue;
        }
        char expected[32];
        std::snprintf(expected, sizeof(expected), "%016llx",
                      (unsigned long long)Deko9_HashBytecode(bytes.data(), bytes.size()));
        if (std::strncmp(entry->d_name, expected, 16))
        {
            std::printf("DEKO9_CORPUS_FAIL %s hash mismatch\n", entry->d_name);
            continue;
        }
        std::vector<uint8_t> dksh;
        if (!TranslateAndCompile(bytes.data(), bytes.size(), 0, &info, &glsl, &error, &dksh))
        {
            std::printf("DEKO9_CORPUS_FAIL %s %s\n", entry->d_name, error.c_str());
            continue;
        }
        if (const uint32_t unbound = UnboundCbufSlots(dksh, &error))
        {
            std::printf("DEKO9_CORPUS_FAIL %s reads unbound constant-buffer slots 0x%x (%s)\n", entry->d_name,
                        unbound, error.c_str());
            continue;
        }
        Deko9DkshStats stats{};
        if (!Deko9_DkshStats(dksh.data(), dksh.size(), &stats))
        {
            std::printf("DEKO9_CORPUS_FAIL %s no DKSH stats\n", entry->d_name);
            continue;
        }
        if (const char *raw = RawGlslMath(glsl))
        {
            std::printf("DEKO9_CORPUS_FAIL %s raw GLSL math left in main(): %s\n", entry->d_name, raw);
            continue;
        }
        // KISAK_DEKO_SHADER_STATS=<file>: "<hash> <stage> <gprs> <instrs>
        // <ipa> <tex> <mufu>" per program (offline cost table; not game data).
        if (statsFile)
            std::fprintf(statsFile, "%.16s %s %u %u %u %u %u\n", entry->d_name,
                         info.stage == DEKO9_STAGE_PIXEL ? "ps" : "vs", stats.gprs, stats.instrs, stats.ipa, stats.tex,
                         stats.mufu);
        const uint32_t version = bytes.size() >= 4 ? (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) : 0;
        if (info.stage == DEKO9_STAGE_VERTEX && ((version >> 8) & 0xff) < 3 &&
            glsl.find(" io_10_") != std::string::npos && glsl.find("= clamp(io_10_") == std::string::npos)
        {
            std::printf("DEKO9_CORPUS_FAIL %s vs_1/2 COLOR output not saturated\n", entry->d_name);
            continue;
        }
        if (info.stage == DEKO9_STAGE_VERTEX && glsl.find("invariant gl_Position;") == std::string::npos)
        {
            std::printf("DEKO9_CORPUS_FAIL %s position not invariant\n", entry->d_name);
            continue;
        }
        // Any 2D sampler could be bound to a depth texture at runtime. Most
        // never are; lookups whose depth reference MojoShader truncated
        // (e.g. .zw coordinates) are counted, and fail loudly at runtime if
        // one is ever bound to a depth texture. A mapped variant must compile.
        bool variantsOk = true;
        const Deko9ShaderInfo base = info;
        for (uint32_t s = 0; s < DEKO9_MAX_SAMPLERS && base.stage == DEKO9_STAGE_PIXEL; ++s)
        {
            if (base.samplerDim[s] != DEKO9_SAMPLER_2D)
                continue;
            ++shadowVariants;
            if (!Deko9_TranslateShader(bytes.data(), bytes.size(), 1u << s, &glsl, &info, &error))
            {
                ++shadowUnmapped;
                continue;
            }
            std::vector<uint8_t> variantDksh;
            if (!Deko9_CompileDksh(info.stage, glsl, &variantDksh, &error))
            {
                std::printf("DEKO9_CORPUS_FAIL %s shadow s%u: %s\n", entry->d_name, s, error.c_str());
                variantsOk = false;
            }
            else if (const uint32_t unbound = UnboundCbufSlots(variantDksh, &error))
            {
                std::printf("DEKO9_CORPUS_FAIL %s shadow s%u reads unbound constant-buffer slots 0x%x (%s)\n",
                            entry->d_name, s, unbound, error.c_str());
                variantsOk = false;
            }
            else
            {
                // r_shadowFilter 1: where it moves taps, the variant must
                // compile and fetch less than the retail one.
                Deko9ShaderInfo sfInfo{};
                std::vector<uint8_t> sfDksh;
                Deko9DkshStats retailStats{}, sfStats{};
                if (!Deko9_TranslateShader(bytes.data(), bytes.size(), 1u << s, &glsl, &sfInfo, &error, nullptr, 0,
                                           false, 1))
                {
                    std::printf("DEKO9_CORPUS_FAIL %s shadow filter s%u: %s\n", entry->d_name, s, error.c_str());
                    variantsOk = false;
                }
                else if (sfInfo.shadowCenterTaps)
                {
                    ++shadowFilterVariants;
                    if (!Deko9_CompileDksh(sfInfo.stage, glsl, &sfDksh, &error) ||
                        !Deko9_DkshStats(variantDksh.data(), variantDksh.size(), &retailStats) ||
                        !Deko9_DkshStats(sfDksh.data(), sfDksh.size(), &sfStats) || sfStats.tex >= retailStats.tex ||
                        sfStats.instrs >= retailStats.instrs || UnboundCbufSlots(sfDksh, &error))
                    {
                        std::printf("DEKO9_CORPUS_FAIL %s shadow filter s%u: compile or cost (tex %u->%u instrs "
                                    "%u->%u) %s\n",
                                    entry->d_name, s, retailStats.tex, sfStats.tex, retailStats.instrs, sfStats.instrs,
                                    error.c_str());
                        variantsOk = false;
                    }
                    else
                    {
                        shadowFilterInstrs[0] += retailStats.instrs;
                        shadowFilterInstrs[1] += sfStats.instrs;
                        shadowFilterTex[0] += retailStats.tex;
                        shadowFilterTex[1] += sfStats.tex;
                    }
                }
            }
        }
        // Instanced variant (static-model instancing reads per-instance
        // registers from attributes): with the first up to 16 constant
        // registers instanced, every vertex program must still compile.
        if (base.stage == DEKO9_STAGE_VERTEX && base.constRegs && !(base.inputMask >> 15))
        {
            uint8_t regs[DEKO9_MAX_INSTANCE_REGS];
            uint32_t inputs = 0; // highest input register + 1
            while (inputs < 16 && (base.inputMask >> inputs))
                ++inputs;
            const uint32_t count = base.constRegs < 16 - inputs ? base.constRegs : 16 - inputs;
            for (uint32_t i = 0; i < count; ++i)
                regs[i] = (uint8_t)i;
            std::vector<uint8_t> instDksh;
            ++instancedVariants;
            if (!Deko9_TranslateShader(bytes.data(), bytes.size(), 0, &glsl, &info, &error, regs, count) ||
                !Deko9_CompileDksh(info.stage, glsl, &instDksh, &error))
            {
                std::printf("DEKO9_CORPUS_FAIL %s instanced: %s\n", entry->d_name, error.c_str());
                variantsOk = false;
            }
            else if (const uint32_t unbound = UnboundCbufSlots(instDksh, &error))
            {
                std::printf("DEKO9_CORPUS_FAIL %s instanced reads unbound constant-buffer slots 0x%x (%s)\n",
                            entry->d_name, unbound, error.c_str());
                variantsOk = false;
            }
        }
        // Early-Z variant: every discarding pixel program that does not
        // write depth must compile with the forced early tests.
        if (base.stage == DEKO9_STAGE_PIXEL)
        {
            ++pixelPrograms;
            killing += base.kills;
            depthWriting += base.writesDepth;
        }
        if (base.stage == DEKO9_STAGE_PIXEL && !base.writesDepth)
        {
            std::vector<uint8_t> ezDksh;
            Deko9DkshInfo scan{};
            ++earlyZVariants;
            if (!Deko9_TranslateShader(bytes.data(), bytes.size(), 0, &glsl, &info, &error, nullptr, 0, true) ||
                !Deko9_CompileDksh(info.stage, glsl, &ezDksh, &error))
            {
                std::printf("DEKO9_CORPUS_FAIL %s early-Z: %s\n", entry->d_name, error.c_str());
                variantsOk = false;
            }
            else if (!Deko9_ScanDksh(ezDksh.data(), ezDksh.size(), &scan, &error) || !scan.earlyFragmentTests ||
                     (scan.slotMask & ~Deko9_BoundCbufSlots(scan)))
            {
                std::printf("DEKO9_CORPUS_FAIL %s early-Z DKSH lacks the flag or reads unbound slots\n",
                            entry->d_name);
                variantsOk = false;
            }
        }
        // DEKO9_SHADER_OPT_GUARDS: every program compiles without a ternary
        // in its body, with the same interface, no more control flow and at
        // most one more slot (two retail programs trade a predicated pair
        // for a select).
        {
            std::vector<uint8_t> optDksh;
            Deko9ShaderInfo optInfo{};
            Deko9DkshStats optStats{};
            std::string optGlsl;
            if (!Deko9_TranslateShader(bytes.data(), bytes.size(), 0, &optGlsl, &optInfo, &error, nullptr, 0, false,
                                       0, DEKO9_SHADER_OPT_GUARDS) ||
                !Deko9_CompileDksh(optInfo.stage, optGlsl, &optDksh, &error) ||
                !Deko9_DkshStats(optDksh.data(), optDksh.size(), &optStats))
            {
                std::printf("DEKO9_CORPUS_FAIL %s guards: %s\n", entry->d_name, error.c_str());
                variantsOk = false;
            }
            else
            {
                const size_t body = optGlsl.find("void deko9_main()") != std::string::npos
                                        ? optGlsl.find("void deko9_main()")
                                        : optGlsl.find("void main()");
                const bool ternary = optGlsl.find(" ? ", body) != std::string::npos;
                guardTernaries += ternary;
                const bool sameInterface = optInfo.constRegs == base.constRegs &&
                                           optInfo.samplerMask == base.samplerMask &&
                                           optInfo.inputMask == base.inputMask &&
                                           optInfo.varyingMask == base.varyingMask && optInfo.kills == base.kills &&
                                           optInfo.writesDepth == base.writesDepth;
                if (ternary || !sameInterface || optStats.branches > stats.branches ||
                    optStats.instrs > stats.instrs + 1)
                {
                    std::printf("DEKO9_CORPUS_FAIL %s guards: ternary=%d interface=%d branches %u->%u slots %u->%u\n",
                                entry->d_name, ternary, sameInterface, stats.branches, optStats.branches,
                                stats.instrs, optStats.instrs);
                    variantsOk = false;
                }
                guardInstrs[0] += stats.instrs;
                guardInstrs[1] += optStats.instrs;
                guardBranches[0] += stats.branches;
                guardBranches[1] += optStats.branches;
                guardOverRegs += optStats.gprs > 32 && stats.gprs <= 32;
            }
        }
        if (!variantsOk)
            continue;
        ++passed;
    }
    closedir(d);
    if (statsFile)
        std::fclose(statsFile);
    std::printf("DEKO9_CORPUS programs=%u passed=%u shadow_variants=%u shadow_unmapped=%u instanced_variants=%u "
                "pixel=%u killing=%u depth_writing=%u earlyz_variants=%u\n",
                total, passed, shadowVariants, shadowUnmapped, instancedVariants, pixelPrograms, killing,
                depthWriting, earlyZVariants);
    std::printf("DEKO9_CORPUS shadow_filter_variants=%u instrs=%llu->%llu tex=%llu->%llu\n", shadowFilterVariants,
                (unsigned long long)shadowFilterInstrs[0], (unsigned long long)shadowFilterInstrs[1],
                (unsigned long long)shadowFilterTex[0], (unsigned long long)shadowFilterTex[1]);
    std::printf("DEKO9_CORPUS guards instrs=%llu->%llu branches=%llu->%llu ternaries=%u over_32_gprs=%u\n",
                (unsigned long long)guardInstrs[0], (unsigned long long)guardInstrs[1],
                (unsigned long long)guardBranches[0], (unsigned long long)guardBranches[1], guardTernaries,
                guardOverRegs);
    Check(total > 0 && passed == total, "every corpus program translates and compiles");
}

// The shader pack cache (deko9_shaderpack.h). No game data -- reuses
// the hand-assembled fixtures above to prove round trip, version-mismatch
// rebuild, and corrupt-record rejection.
void ShaderPackCases()
{
    using namespace deko9;

    // Round trip: fixtures translate+compile to real Deko9ShaderInfo/DKSH,
    // go into a pack, get serialized, and a fresh pack parses that buffer
    // back to the exact same info and DKSH bytes for every key.
    struct Fixture
    {
        const void *code;
        size_t bytes;
        uint32_t shadowMask;
        const char *what;
    };
    const Fixture fixtures[] = {
        {kVs, sizeof(kVs), 0, "kVs"},
        {kVsColor, sizeof(kVsColor), 0, "kVsColor"},
        {kPs, sizeof(kPs), 0, "kPs"},
        {kPs, sizeof(kPs), 1u, "kPs shadow-masked"},
        {kPsProj, sizeof(kPsProj), 1u, "kPsProj shadow-masked"},
    };
    ShaderPack pack;
    std::vector<ShaderVariantKey> keys;
    std::vector<Deko9ShaderInfo> infos;
    std::vector<std::vector<uint8_t>> dkshes;
    for (const auto &fx : fixtures)
    {
        Deko9ShaderInfo info{};
        std::string glsl, error;
        std::vector<uint8_t> dksh;
        const bool ok = TranslateAndCompile(fx.code, fx.bytes, fx.shadowMask, &info, &glsl, &error, &dksh);
        if (!ok)
        {
            std::printf("shaderpack fixture %s: %s\n", fx.what, error.c_str());
            Check(false, "shader-pack fixture translates and compiles");
            continue;
        }
        const ShaderVariantKey key{Deko9_HashBytecode(fx.code, fx.bytes), fx.shadowMask, 0, 0, 0, 0};
        pack.Append(key, info, dksh);
        keys.push_back(key);
        infos.push_back(info);
        dkshes.push_back(std::move(dksh));
    }
    Check(pack.Dirty(), "a freshly appended pack is dirty");
    Check(pack.RecordCount() == keys.size(), "record count matches the fixtures appended");
    const std::vector<uint8_t> bytes = pack.Serialize();
    Check(!pack.Dirty(), "Serialize() clears the delta");

    ShaderPack loaded;
    std::string loadError;
    Check(loaded.Load(bytes, &loadError), "round trip: a freshly serialized pack loads");
    for (size_t i = 0; i < keys.size(); ++i)
    {
        const ShaderPackRecord *rec = loaded.Find(keys[i]);
        if (!rec)
        {
            Check(false, "round trip: every appended key is found after load");
            continue;
        }
        Check(rec->dksh == dkshes[i], "round trip: DKSH bytes are exact");
        Check(!std::memcmp(&rec->info, &infos[i], sizeof(Deko9ShaderInfo)),
              "round trip: info from pack == info from translate");
    }
    Check(!loaded.Find(ShaderVariantKey{0xdeadbeefull, 0, 0, 0, 0, 0}), "an absent key is a clean miss");

    // Cold start: an empty file is not corruption.
    ShaderPack cold;
    std::string coldError;
    Check(cold.Load({}, &coldError) && cold.RecordCount() == 0, "an empty file loads as an empty pack");

    // Version mismatch -> rebuild (rejected, not silently loaded).
    {
        ShaderPack v1(1, "pinA", "pinB");
        Deko9ShaderInfo info{};
        v1.Append(ShaderVariantKey{1, 0, 0, 0, 0, 0}, info, std::vector<uint8_t>{1, 2, 3, 4});
        const std::vector<uint8_t> v1Bytes = v1.Serialize();
        ShaderPack v2(2, "pinA", "pinB");
        std::string error;
        Check(!v2.Load(v1Bytes, &error), "a version mismatch fails Load(), not a silent rebuild");
        Check(v2.RecordCount() == 0, "a failed Load() leaves the pack empty, not partially populated");
        ShaderPack pinMismatch(1, "pinX", "pinB");
        std::string pinError;
        Check(!pinMismatch.Load(v1Bytes, &pinError), "a translator pin mismatch fails Load() too");
    }

    // Corrupt record (checksum mismatch) -> rejected, whole pack empty
    // (never partially trusted), not a crash and not silently misused.
    {
        std::vector<uint8_t> corrupt = bytes;
        Check(!corrupt.empty(), "have bytes to corrupt");
        corrupt.back() ^= 0xff;
        ShaderPack corruptPack;
        std::string error;
        Check(!corruptPack.Load(corrupt, &error), "a corrupt record fails Load()");
        Check(corruptPack.RecordCount() == 0, "a corrupt Load() leaves the pack empty");

        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 4);
        ShaderPack truncPack;
        std::string truncError;
        Check(!truncPack.Load(truncated, &truncError), "a truncated header fails Load()");
    }
}
} // namespace


// ps_2_0 with every guarded D3D9 op: dcl t0; def c1; mov r0, t0;
// rcp r1.x, r0.w; rcp r5.xyz, r0.w; rsq r1.y, r0.z; nrm r2.xyz, r0;
// pow r1.z, r0.x, c1.y; cmp r3.xyz, -r0.x, c1, r2; cmp r3.w, r0.w, c1.x,
// r5.x; mul r3.xyz, r3, r1; add r3.xyz, r3, r5; mov oC0, r3
const uint32_t kPsGuards[] = {
    0xFFFF0200,
    0x0200001F, 0x80000000, 0xB00F0000,
    0x05000051, 0xA00F0001, 0x00000000, 0x40828F5C, 0x3F800000, 0xC0400000,
    0x02000001, 0x800F0000, 0xB0E40000,
    0x02000006, 0x80010001, 0x80FF0000,
    0x02000006, 0x80070005, 0x80FF0000,
    0x02000007, 0x80020001, 0x80AA0000,
    0x02000024, 0x80070002, 0x80E40000,
    0x03000020, 0x80040001, 0x80000000, 0xA0550001,
    0x04000058, 0x80070003, 0x81000000, 0xA0E40001, 0x80E40002,
    0x04000058, 0x80080003, 0x80FF0000, 0xA0000001, 0x80000005,
    0x03000005, 0x80070003, 0x80E40003, 0x80E40001,
    0x03000002, 0x80070003, 0x80E40003, 0x80E40005,
    0x02000001, 0x800F0800, 0x80E40003,
    0x0000FFFF,
};
// vs_2_0: dcl_position v0; dp4 r0.x, v0, v0; rsq r0.y, r0.x; rcp r0.z,
// r0.y; mov r0.w, c5.x; mul oPos, v0, r0.y; mov oT0, r0
const uint32_t kVsGuards[] = {
    0xFFFE0200,
    0x0200001F, 0x80000000, 0x900F0000,
    0x03000009, 0x80010000, 0x90E40000, 0x90E40000,
    0x02000007, 0x80020000, 0x80000000,
    0x02000006, 0x80040000, 0x80550000,
    0x02000001, 0x80080000, 0xA0000005,
    0x03000005, 0xC00F0000, 0x90E40000, 0x80550000,
    0x02000001, 0xE00F0000, 0x80E40000,
    0x0000FFFF,
};

// Host replicas of the guard formulas, old (MojoShader zero test around
// the deko9 helper) and new (DEKO9_SHADER_OPT_GUARDS: the helper alone),
// evaluated the way Maxwell does: denormal operands and results flushed,
// min/max returning the non-NaN operand (fminf/fmaxf).
float Flush(float x)
{
    return std::fpclassify(x) == FP_SUBNORMAL ? std::copysign(0.0f, x) : x;
}
float Clamp38(float x)
{
    return std::fminf(std::fmaxf(Flush(x), -1e38f), 1e38f);
}
float OldRcp(float x)
{
    x = Flush(x);
    return x == 0.0f ? 1e38f : Clamp38(1.0f / x);
}
float NewRcp(float x)
{
    return Clamp38(1.0f / Flush(x));
}
float OldRsq(float x)
{
    x = Flush(x);
    return x == 0.0f ? 1e38f : std::fminf(Flush(1.0f / std::sqrt(std::fabs(x))), 1e38f);
}
float NewRsq(float x)
{
    x = Flush(x);
    return std::fminf(Flush(1.0f / std::sqrt(std::fabs(x))), 1e38f);
}
float OldPow(float a, float b)
{
    return Flush(std::exp2(b == 0.0f ? 0.0f : Flush(b * std::log2(Flush(a)))));
}
float NewPow(float a, float b)
{
    const float t = Flush(b * std::log2(Flush(a)));
    return Flush(std::exp2(b == 0.0f ? 0.0f : t));
}
float OldCmp(float c, float a, float b)
{
    return (-c >= 0.0f) ? a : b;
}
float NewCmp(float c, float a, float b)
{
    const bool pick = -c >= 0.0f;
    return pick ? a : b;
}
bool SameBits(float a, float b)
{
    uint32_t x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    return x == y || (std::isnan(a) && std::isnan(b));
}

// DEKO9_SHADER_OPT_GUARDS: the same programs translate without a ternary,
// compile to straight-line code with fewer slots (the compiler predicates
// the retail ternaries' if/else, so both sides issue), and the guard
// formulas agree bit for bit on the edge cases they exist for; the one
// intended difference is the sign of rcp's clamped infinity for -0 (IEEE,
// as D3D9 hardware and DXVK).
void ShaderOptCases()
{
    struct Program
    {
        const char *name;
        const uint32_t *code;
        size_t bytes;
    };
    const Program programs[] = {{"ps guards", kPsGuards, sizeof(kPsGuards)},
                                {"vs guards", kVsGuards, sizeof(kVsGuards)},
                                {"ps", kPs, sizeof(kPs)},
                                {"vs", kVs, sizeof(kVs)}};
    for (const Program &p : programs)
    {
        Deko9ShaderInfo info[2]{};
        std::string glsl[2], error;
        std::vector<uint8_t> dksh[2];
        Deko9DkshStats stats[2]{};
        bool ok = true;
        for (uint32_t opt = 0; opt < 2 && ok; ++opt)
        {
            ok = Deko9_TranslateShader(p.code, p.bytes, 0, &glsl[opt], &info[opt], &error, nullptr, 0, false, 0,
                                       opt ? DEKO9_SHADER_OPT_GUARDS : 0u) &&
                 Deko9_CompileDksh(info[opt].stage, glsl[opt], &dksh[opt], &error) &&
                 Deko9_DkshStats(dksh[opt].data(), dksh[opt].size(), &stats[opt]);
            if (!ok)
                std::printf("%s opt=%u: %s\n%s\n", p.name, opt, error.c_str(), glsl[opt].c_str());
        }
        char what[128];
        std::snprintf(what, sizeof(what), "%s translates and compiles with both translations", p.name);
        Check(ok, what);
        if (!ok)
            continue;
        const size_t body = glsl[1].find("void main()");
        std::snprintf(what, sizeof(what), "%s guards translation has no ternary", p.name);
        Check(body != std::string::npos && glsl[1].find(" ? ", body) == std::string::npos, what);
        std::snprintf(what, sizeof(what), "%s guards translation is straight-line code (%u branches)", p.name,
                      stats[1].branches);
        Check(stats[1].branches == 0, what);
        std::snprintf(what, sizeof(what), "%s guards translation is no longer (%u -> %u slots)", p.name,
                      stats[0].instrs, stats[1].instrs);
        Check(stats[1].instrs <= stats[0].instrs, what);
        std::snprintf(what, sizeof(what), "%s same interface under both translations", p.name);
        Check(info[0].constRegs == info[1].constRegs && info[0].samplerMask == info[1].samplerMask &&
                  info[0].inputMask == info[1].inputMask && info[0].varyingMask == info[1].varyingMask,
              what);
        if (p.code == kPsGuards)
        {
            Check(stats[1].instrs < stats[0].instrs, "ps guards translation saves slots");
            Check(glsl[0].find("((ps_r0.w == 0.0) ? FLT_MAX : deko9_rcp(ps_r0.w))") != std::string::npos &&
                      glsl[0].find("vec3((ps_r0.w == 0.0) ? FLT_MAX : deko9_rcp(ps_r0.w))") != std::string::npos &&
                      glsl[0].find("((ps_r0.z == 0.0) ? FLT_MAX : deko9_rsq(abs(ps_r0.z)))") != std::string::npos &&
                      glsl[0].find("((-ps_r0.x >= 0.0) ? ps_c1.xyz : ps_r2.xyz)") != std::string::npos &&
                      glsl[0].find("((ps_r0.w >= 0.0) ? ps_c1.x : ps_r5.x)") != std::string::npos,
                  "ps guards retail translation keeps the ternaries");
            Check(glsl[1].find("ps_r1.x = (deko9_rcp(ps_r0.w));") != std::string::npos &&
                      glsl[1].find("ps_r5.xyz = vec3(deko9_rcp(ps_r0.w));") != std::string::npos &&
                      glsl[1].find("ps_r1.y = (deko9_rsq(abs(ps_r0.z)));") != std::string::npos &&
                      glsl[1].find("ps_r2.xyz = deko9_nrm(ps_r0.xyz);") != std::string::npos &&
                      glsl[1].find("ps_r1.z = deko9_pow(abs(ps_r0.x), ps_c1.y);") != std::string::npos &&
                      glsl[1].find("ps_r3.xyz = (deko9_cmp(-ps_r0.x, ps_c1.xyz, ps_r2.xyz));") != std::string::npos &&
                      glsl[1].find("ps_r3.w = (deko9_cmp(ps_r0.w, ps_c1.x, ps_r5.x));") != std::string::npos,
                  "ps guards translation rewrites rcp/rsq/cmp to the helpers");
            Check(glsl[1].find("float deko9_pow(float a, float b) { return exp2(mix(b * log2(a), 0.0, b == 0.0)); }") !=
                      std::string::npos,
                  "ps guards translation selects the pow exponent");
        }
        if (p.code == kVsGuards)
            Check(glsl[1].find("vs_r0.y = (deko9_rsq(abs(vs_r0.x)));") != std::string::npos &&
                      glsl[1].find("vs_r0.z = (deko9_rcp(vs_r0.y));") != std::string::npos,
                  "vs guards translation rewrites rsq/rcp");
    }
    Deko9ShaderInfo info{};
    std::string glsl, error;
    Check(!Deko9_TranslateShader(kPs, sizeof(kPs), 0, &glsl, &info, &error, nullptr, 0, false, 0, 0x80u),
          "unknown shader option bits are refused");

    const float denormal = 1e-40f, inf = INFINITY, nan = NAN;
    const float edges[] = {0.0f, -0.0f, denormal, -denormal, FLT_MIN, -FLT_MIN, FLT_TRUE_MIN, 1.0f, -1.0f, 3.0f,
                           -0.5f, 1e-20f, -1e-20f, 1e20f, -1e20f, 1e38f, -1e38f, FLT_MAX, -FLT_MAX, inf, -inf, nan};
    uint32_t rcpSignCases = 0;
    for (float x : edges)
    {
        const float oldRcp = OldRcp(x), newRcp = NewRcp(x);
        if (!SameBits(oldRcp, newRcp))
        {
            // Only a flushed -0: +1e38 before, -1e38 now.
            Check(std::signbit(x) && Flush(x) == 0.0f && oldRcp == 1e38f && newRcp == -1e38f,
                  "rcp guards differ only in the sign of the clamped infinity of -0");
            ++rcpSignCases;
        }
        Check(SameBits(OldRsq(x), NewRsq(x)), "rsq guard formulas agree");
        Check(std::isfinite(NewRcp(x)) == std::isfinite(OldRcp(x)) && (std::isnan(x) || std::isfinite(NewRsq(x))),
              "guards keep rcp/rsq finite");
        for (float y : edges)
        {
            Check(SameBits(OldPow(x, y), NewPow(x, y)), "pow guard formulas agree");
            Check(SameBits(OldCmp(x, y, 3.0f), NewCmp(x, y, 3.0f)), "cmp select formulas agree");
        }
    }
    Check(rcpSignCases == 2, "exactly -0 and the flushed negative denormal take the rcp sign case");
    Check(NewPow(0.0f, 0.0f) == 1.0f && NewPow(0.0f, 2.0f) == 0.0f && OldPow(0.0f, 0.0f) == 1.0f,
          "pow(0, 0) stays 1 and pow(0, 2) stays 0");
}

int main()
{
    BuiltInCases();
    DkshStatsCases();
    EarlyZCases();
    ShadowFilterCases();
    ShaderOptCases();
    ShaderPackCases();
    if (const char *corpus = std::getenv("KISAK_DEKO_SHADER_CORPUS"))
        CorpusCases(corpus);
    if (g_failures)
        return 1;
    std::printf("PASS:DEKO9_SHADER\n");
    return 0;
}
