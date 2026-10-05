// Host proof for the deko3d renderer's present upscalers
// (src/deko9/deko9_fsr_shaders.cpp, deko9_fsr_reference.h; r_fsrMode):
//   - the vertex, SGSR, bilinear+RCAS, bilinear and gather-probe GLSL
//     compile to DKSH with UAM;
//   - the constant setup;
//   - the SGSR reference (texel loads, reassociated weights) matches a
//     literal transcription of Qualcomm's sgsr1_shader_mobile.frag, whose
//     textureGathers are modelled as the four clamped texels they return;
//   - every mode's reference behaves like an upscaler: flat input stays
//     flat, a step edge stays a monotonic step at the scaled position,
//     a ramp stays a ramp, and rows keep their orientation. Tolerance is
//     1 LSB: RCAS resolves through AMD's medium-precision reciprocal.

#include "src/deko9/deko9_fsr.h"
#include "src/deko9/deko9_fsr_reference.h"
#include "src/deko9/deko9_shader.h"
#include "src/deko9/deko9_taau.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int g_failures;

void Check(bool ok, const char *what, const char *detail = "")
{
    std::printf("%s:DEKO9_FSR_%s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok)
        ++g_failures;
}

// Reconvergence-stack ops of the programs that still branch per pixel: SGSR's
// edge-direction tests, the gather probe, and the TAAU resolve's flat-2x2 and
// history-validity skips (performance branches).
uint32_t KnownDivergentOps(const char *name)
{
    if (!std::strncmp(name, "COMPILE_SGSR", 12))
        return 3;
    if (!std::strcmp(name, "COMPILE_GATHER_PROBE"))
        return 5;
    if (!std::strncmp(name, "COMPILE_TAAU_RESOLVE", 20))
        return 6;
    return 0;
}

using deko9::fsrref::Image;
using deko9::fsrref::Rgb;

Image Make(int w, int h, Rgb (*fn)(int x, int y, int w, int h))
{
    Image img;
    img.width = w;
    img.height = h;
    img.px.resize((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.px[(size_t)y * w + x] = fn(x, y, w, h);
    return img;
}

// Qualcomm's shader line by line (mode 1: green), FP32. ViewportInfo =
// (1/w, 1/h, w, h); textureGather(coord, 1) returns the green of the
// clamped texels (i0, j0+1), (i0+1, j0+1), (i0+1, j0), (i0, j0) with
// (i0, j0) = floor(coord * size - 0.5).
struct Vec4
{
    float x, y, z, w;
};
Vec4 GatherG(const Image &in, float u, float v)
{
    const int i0 = (int)std::floor(u * in.width - 0.5f), j0 = (int)std::floor(v * in.height - 0.5f);
    return {in.At(i0, j0 + 1).g, in.At(i0 + 1, j0 + 1).g, in.At(i0 + 1, j0).g, in.At(i0, j0).g};
}
float FastLanczos2(float x)
{
    float wA = x - 4.0f;
    const float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}
void WeightY(float &aW, float &aY, float dx, float dy, float c, float stdv)
{
    const float x = ((dx * dx) + (dy * dy)) * 0.55f + deko9::fsrref::Sat(std::fabs(c) * stdv);
    const float w = FastLanczos2(x);
    aW += w;
    aY += w * c;
}
Rgb QualcommSgsr(const Image &in, int outW, int outH, int px, int py)
{
    const float u = deko9::fsrref::CentreUv(px, outW), v = deko9::fsrref::CentreUv(py, outH);
    const float vi[4] = {1.0f / in.width, 1.0f / in.height, (float)in.width, (float)in.height};
    Rgb color = deko9::fsrref::Bilinear(in, u, v);
    const float icX = u * vi[2] - 0.5f, icY = v * vi[3] + 0.5f;
    const float pX = std::floor(icX), pY = std::floor(icY);
    float cX = pX * vi[0];
    const float cY = pY * vi[1];
    const float plX = icX - pX, plY = icY - pY;
    Vec4 left = GatherG(in, cX, cY);
    const float edgeVote = std::fabs(left.z - left.y) + std::fabs(color.g - left.y) + std::fabs(color.g - left.z);
    if (edgeVote > 8.0f / 255.0f)
    {
        cX += vi[0];
        Vec4 right = GatherG(in, cX + vi[0], cY);
        const Vec4 up = GatherG(in, cX, cY - vi[1]), down = GatherG(in, cX, cY + vi[1]);
        Vec4 upDown{up.w, up.z, down.y, down.x};
        const float mean = (left.y + left.z + right.x + right.w) * 0.25f;
        for (Vec4 *q : {&left, &right, &upDown})
        {
            q->x -= mean;
            q->y -= mean;
            q->z -= mean;
            q->w -= mean;
        }
        const float colorW = color.g - mean;
        using std::fabs;
        const float sum = (((((fabs(left.x) + fabs(left.y)) + fabs(left.z)) + fabs(left.w)) +
                            (((fabs(right.x) + fabs(right.y)) + fabs(right.z)) + fabs(right.w))) +
                           (((fabs(upDown.x) + fabs(upDown.y)) + fabs(upDown.z)) + fabs(upDown.w)));
        const float stdv = 2.181818f / sum;
        float aW = 0, aY = 0;
        WeightY(aW, aY, plX, plY + 1.0f, upDown.x, stdv);
        WeightY(aW, aY, plX - 1.0f, plY + 1.0f, upDown.y, stdv);
        WeightY(aW, aY, plX - 1.0f, plY - 2.0f, upDown.z, stdv);
        WeightY(aW, aY, plX, plY - 2.0f, upDown.w, stdv);
        WeightY(aW, aY, plX + 1.0f, plY - 1.0f, left.x, stdv);
        WeightY(aW, aY, plX, plY - 1.0f, left.y, stdv);
        WeightY(aW, aY, plX, plY, left.z, stdv);
        WeightY(aW, aY, plX + 1.0f, plY, left.w, stdv);
        WeightY(aW, aY, plX - 1.0f, plY - 1.0f, right.x, stdv);
        WeightY(aW, aY, plX - 2.0f, plY - 1.0f, right.y, stdv);
        WeightY(aW, aY, plX - 2.0f, plY, right.z, stdv);
        WeightY(aW, aY, plX - 1.0f, plY, right.w, stdv);
        float finalY = aY / aW;
        const float maxY = std::fmax(std::fmax(left.y, left.z), std::fmax(right.x, right.w));
        const float minY = std::fmin(std::fmin(left.y, left.z), std::fmin(right.x, right.w));
        finalY = deko9::fsrref::Clamp(2.0f * finalY, minY, maxY);
        const float deltaY = deko9::fsrref::Clamp(finalY - colorW, -23.0f / 255.0f, 23.0f / 255.0f);
        color = {deko9::fsrref::Sat(color.r + deltaY), deko9::fsrref::Sat(color.g + deltaY),
                 deko9::fsrref::Sat(color.b + deltaY)};
    }
    return color;
}

uint32_t Hash(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    return v ^ (v >> 16);
}

} // namespace

int main()
{
    // 1. UAM accepts the programs (and the source-rectangle builds the
    //    dynamic-resolution upscale uses).
    const std::string sgsrRect = deko9::SourceRectVariant(deko9::kSgsrGlsl);
    const std::string rcasRect = deko9::SourceRectVariant(deko9::kBilinearRcasGlsl);
    const std::string bilinearRect = deko9::SourceRectVariant(deko9::kBilinearGlsl);
    Check(sgsrRect.find("#version 460\n#define DEKO9_SOURCE_RECT 1\n") == 0, "SOURCE_RECT_VARIANT_TEXT");
    const std::string taauResolve[4] = {
        deko9::TaauVariant(deko9::kTaauResolveGlsl, 0),
        deko9::TaauVariant(deko9::kTaauResolveGlsl, deko9::kTaauBilinearHistory),
        deko9::TaauVariant(deko9::kTaauResolveGlsl, deko9::kTaauBilinearCurrent),
        deko9::TaauVariant(deko9::kTaauResolveGlsl, deko9::kTaauBilinearHistory | deko9::kTaauBilinearCurrent)};
    const std::string taauReactiveHalf = deko9::TaauVariant(deko9::kTaauOpaqueGlsl, deko9::kTaauHalfReactive);
    struct
    {
        const char *name;
        Deko9Stage stage;
        const char *glsl;
    } programs[] = {{"COMPILE_VS", DEKO9_STAGE_VERTEX, deko9::kFsrVertexGlsl},
                    {"COMPILE_SGSR", DEKO9_STAGE_PIXEL, deko9::kSgsrGlsl},
                    {"COMPILE_BILINEAR_RCAS", DEKO9_STAGE_PIXEL, deko9::kBilinearRcasGlsl},
                    {"COMPILE_BILINEAR", DEKO9_STAGE_PIXEL, deko9::kBilinearGlsl},
                    {"COMPILE_SGSR_RECT", DEKO9_STAGE_PIXEL, sgsrRect.c_str()},
                    {"COMPILE_BILINEAR_RCAS_RECT", DEKO9_STAGE_PIXEL, rcasRect.c_str()},
                    {"COMPILE_BILINEAR_RECT", DEKO9_STAGE_PIXEL, bilinearRect.c_str()},
                    {"COMPILE_GATHER_PROBE", DEKO9_STAGE_PIXEL, deko9::kGatherProbeGlsl},
                    {"COMPILE_FLOATZ", DEKO9_STAGE_PIXEL, deko9::kFloatZGlsl},
                    {"COMPILE_HRP_DEPTH", DEKO9_STAGE_PIXEL, deko9::kHrpDepthGlsl},
                    {"COMPILE_HRP_COMPOSITE", DEKO9_STAGE_PIXEL, deko9::kHrpCompositeGlsl},
                    {"COMPILE_TAAU_RESOLVE", DEKO9_STAGE_PIXEL, taauResolve[0].c_str()},
                    {"COMPILE_TAAU_RESOLVE_BH", DEKO9_STAGE_PIXEL, taauResolve[1].c_str()},
                    {"COMPILE_TAAU_RESOLVE_BC", DEKO9_STAGE_PIXEL, taauResolve[2].c_str()},
                    {"COMPILE_TAAU_RESOLVE_BHBC", DEKO9_STAGE_PIXEL, taauResolve[3].c_str()},
                    {"COMPILE_TAAU_REACTIVE", DEKO9_STAGE_PIXEL, deko9::kTaauOpaqueGlsl},
                    {"COMPILE_TAAU_REACTIVE_HALF", DEKO9_STAGE_PIXEL, taauReactiveHalf.c_str()},
                    {"COMPILE_TAAU_MOTION_VS", DEKO9_STAGE_VERTEX, deko9::kTaauMotionVertexGlsl},
                    {"COMPILE_TAAU_MOTION_PS", DEKO9_STAGE_PIXEL, deko9::kTaauMotionFragmentGlsl}};
    for (const auto &program : programs)
    {
        std::vector<uint8_t> dksh;
        std::string error;
        const bool ok = Deko9_CompileDksh(program.stage, program.glsl, &dksh, &error);
        // DKSH header: code_sz (u32 3, SPH + code), programs_off (u32 4);
        // program header u32 2 = num_gprs. Recorded for the GPU cost table.
        uint32_t hdr[6] = {}, gprs = 0;
        if (dksh.size() >= sizeof(hdr))
            std::memcpy(hdr, dksh.data(), sizeof(hdr));
        if (hdr[4] && hdr[4] + 12 <= dksh.size())
            std::memcpy(&gprs, dksh.data() + hdr[4] + 8, 4);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "bytes=%zu code=%u gprs=%u %s", dksh.size(), hdr[3], gprs,
                      error.c_str());
        Check(ok && dksh.size() > 4 && !std::memcmp(dksh.data(), "DKSH", 4), program.name, detail);
        // The programs read only slots deko9 binds: c[0] (texture handle),
        // c[1] (compiler constants, bound with the program), c[3] (uniform
        // binding 1, bound before the pass). A read of any other slot would
        // hit an address the channel never had programmed (can fault the
        // GPU MMU).
        Deko9DkshInfo scan{};
        std::string scanError;
        const bool scanned = ok && Deko9_ScanDksh(dksh.data(), dksh.size(), &scan, &scanError);
        const uint32_t unbound = scanned ? scan.slotMask & ~Deko9_BoundCbufSlots(scan) : 0xffffffffu;
        std::snprintf(detail, sizeof(detail), "slots=0x%x unbound=0x%x instructions=%u %s", scan.slotMask, unbound,
                      scan.instructions, scanError.c_str());
        char name[64];
        std::snprintf(name, sizeof(name), "CBUF_SLOTS_%s", program.name + 8);
        Check(scanned && scan.instructions > 0 && unbound == 0, name, detail);
        // Every native full-screen program runs per-pixel selections only:
        // no reconvergence-stack control flow (SSY/SYNC/PBK/BRK), which a
        // pixel-divergent branch needs (the branchy composite gave wrong,
        // varying output on Maxwell).
        if (scanned)
        {
            const uint8_t *code = dksh.data() + hdr[2] + scan.entrypoint + 0x50;
            const size_t words = (scan.codeSize - scan.entrypoint - 0x50) / 8;
            uint32_t stack = 0;
            for (size_t i = 0; i < words; ++i)
            {
                if (i % 4 == 0)
                    continue; // scheduling control word
                uint64_t w;
                std::memcpy(&w, code + i * 8, 8);
                const uint32_t op = (uint32_t)(w >> 52);
                stack += op == 0xe29 || op == 0xf0f || op == 0xe2a || op == 0xe34;
            }
            std::snprintf(detail, sizeof(detail), "reconvergence_ops=%u", stack);
            std::snprintf(name, sizeof(name), "NO_DIVERGENCE_%s", program.name + 8);
            // Programs with a known pixel-divergent branch are pinned to
            // their current count: they fail when it changes, so a fix or a
            // new branch is noticed.
            const uint32_t expected = KnownDivergentOps(program.name);
            if (expected)
                std::snprintf(name, sizeof(name), "KNOWN_DIVERGENCE_%s", program.name + 8);
            Check(stack == expected, name, detail);
        }
    }

    // 2. Constants (960x540 -> 1280x720).
    {
        deko9::SgsrConstants s;
        deko9::SgsrSetup(&s, 960, 540);
        Check(s.viewportInfo[0] == 1.0f / 960 && s.viewportInfo[1] == 1.0f / 540 && s.viewportInfo[2] == 960 &&
                  s.viewportInfo[3] == 540 && s.texelRect[0] == 0 && s.texelRect[1] == 0 &&
                  s.texelRect[2] == 959 && s.texelRect[3] == 539 && s.uvTransform[0] == 0.0f &&
                  s.uvTransform[2] == 1.0f && s.uvTransform[3] == 1.0f && s.uvClamp[0] == 0.5f / 960 &&
                  s.uvClamp[3] == (540 - 0.5f) / 540 && sizeof(s) == 64,
              "SGSR_CONSTANTS");
        deko9::BilinearRcasConstants r;
        deko9::BilinearRcasSetup(&r, 0.2f, 1280, 720);
        Check(std::fabs(r.step[2] - std::exp2(-0.2f)) < 1e-7f && r.step[0] == 1.0f / 1280 &&
                  r.step[1] == 1.0f / 720 && r.uvClamp[0] == 0.5f / 1280 && r.uvClamp[3] == 1.0f - 0.5f / 720 &&
                  r.uvTransform[0] == 0.0f && r.uvTransform[2] == 1.0f && r.srcClamp[0] == 0.0f &&
                  r.srcClamp[3] == 1.0f && sizeof(r) == 64,
              "BILINEAR_RCAS_CONSTANTS");
        // Dynamic resolution: the 960x544 top-left of a 1280x720 target.
        deko9::UpscaleSource src;
        deko9::UpscaleSourceSetup(&src, 1280, 720, 0, 0, 960, 544);
        deko9::BilinearConstants b;
        deko9::BilinearSetup(&b, src);
        Check(src.uvTransform[0] == 0.0f && src.uvTransform[1] == 0.0f && src.uvTransform[2] == 0.75f &&
                  src.uvTransform[3] == 544.0f / 720 && src.uvClamp[0] == 0.5f / 1280 &&
                  src.uvClamp[2] == 959.5f / 1280 && src.uvClamp[3] == 543.5f / 720 && src.texelRect[2] == 959 &&
                  src.texelRect[3] == 543 && b.srcClamp[2] == src.uvClamp[2] && sizeof(b) == 32,
              "UPSCALE_SOURCE_CONSTANTS");
        Check(!std::strcmp(deko9::UpscaleModeName(deko9::UPSCALE_SGSR), "sgsr") &&
                  !std::strcmp(deko9::UpscaleModeName(deko9::UPSCALE_BILINEAR_RCAS), "bilinear_rcas") &&
                  !std::strcmp(deko9::UpscaleModeName(deko9::UPSCALE_BILINEAR), "bilinear"),
              "MODE_NAMES");
    }

    const int inW = 96, inH = 54, outW = 128, outH = 72;

    // 3. SGSR port vs Qualcomm's shader, on noise, hard edges and a
    //    non-0.75 ratio (the 1152x648 enum value's 0.9).
    {
        Image noise = Make(inW, inH, [](int x, int y, int, int) {
            const uint32_t h = Hash((uint32_t)(y * 977 + x));
            // Blocks of noise and flat areas: both branches of the edge test.
            const bool flat = ((x / 12) + (y / 9)) & 1;
            if (flat)
                return Rgb{0.3f, 0.5f, 0.7f};
            return Rgb{(h & 0xff) / 255.0f, ((h >> 8) & 0xff) / 255.0f, ((h >> 16) & 0xff) / 255.0f};
        });
        struct
        {
            int w, h;
        } outs[] = {{outW, outH}, {107, 60}};
        float worst = 0;
        int edges = 0, total = 0;
        for (const auto &o : outs)
            for (int y = 0; y < o.h; ++y)
                for (int x = 0; x < o.w; ++x)
                {
                    const Rgb a = deko9::fsrref::Sgsr(noise, o.w, o.h, x, y);
                    const Rgb b = QualcommSgsr(noise, o.w, o.h, x, y);
                    const Rgb bil = deko9::fsrref::BilinearUpscale(noise, o.w, o.h, x, y);
                    worst = std::fmax(worst, std::fmax(std::fabs(a.r - b.r),
                                                       std::fmax(std::fabs(a.g - b.g), std::fabs(a.b - b.b))));
                    edges += a.g != bil.g;
                    ++total;
                }
        char detail[96];
        std::snprintf(detail, sizeof(detail), "max_abs=%.3g edge_pixels=%d of %d", worst, edges, total);
        // Float reassociation only: far below one 8-bit step.
        Check(worst < 1e-4f && edges > total / 8, "SGSR_MATCHES_QUALCOMM", detail);
    }

    // 4. Reference behaviour per mode at the 540p ratio (0.75).
    for (uint32_t mode = 0; mode < deko9::UPSCALE_MODE_COUNT; ++mode)
    {
        const char *name = deko9::UpscaleModeName(mode);
        char check[64];
        {
            Image flat = Make(inW, inH, [](int, int, int, int) { return Rgb{0.2f, 0.6f, 0.9f}; });
            const std::vector<uint8_t> out = deko9::fsrref::UpscaleRgb8(flat, outW, outH, mode, 0.2f);
            const uint8_t want[3] = {deko9::fsrref::ToUnorm8(0.2f), deko9::fsrref::ToUnorm8(0.6f),
                                     deko9::fsrref::ToUnorm8(0.9f)};
            bool ok = true;
            for (size_t i = 0; i < out.size(); ++i)
                ok = ok && std::abs((int)out[i] - (int)want[i % 3]) <= 1 && out[i] == out[i % 3];
            std::snprintf(check, sizeof(check), "REF_FLAT_%s", name);
            Check(ok, check);
        }
        {
            // White | black at input x = 48: the output edge centre is at
            // (47.5 + 0.5) / 0.75 - 0.5 = 63.5.
            Image step = Make(inW, inH, [](int x, int, int w, int) {
                const float v = x < w / 2 ? 1.0f : 0.0f;
                return Rgb{v, v, v};
            });
            const std::vector<uint8_t> out = deko9::fsrref::UpscaleRgb8(step, outW, outH, mode, 0.2f);
            bool ok = true;
            char detail[160] = "";
            for (int y = 0; y < outH && ok; ++y)
            {
                const uint8_t *row = &out[(size_t)y * outW * 3];
                for (int x = 0; x < outW && ok; ++x)
                {
                    const uint8_t v = row[x * 3];
                    if ((x <= 61 && v < 254) || (x >= 66 && v > 1) || (x > 0 && v > row[(x - 1) * 3] + 1))
                    {
                        std::snprintf(detail, sizeof(detail), "row %d x=%d v=%u prev=%u", y, x, v,
                                      x ? row[(x - 1) * 3] : 0);
                        ok = false;
                    }
                }
            }
            std::snprintf(check, sizeof(check), "REF_STEP_EDGE_%s", name);
            Check(ok, check, detail);
        }
        {
            // Horizontal ramp: the output follows the linear ramp at each
            // output pixel's input position.
            Image ramp = Make(inW, inH, [](int x, int, int w, int) {
                const float v = (float)x / (float)(w - 1);
                return Rgb{v, v, v};
            });
            const std::vector<uint8_t> out = deko9::fsrref::UpscaleRgb8(ramp, outW, outH, mode, 0.2f);
            int worst = 0;
            bool monotonic = true;
            for (int y = 0; y < outH; ++y)
                for (int x = 2; x < outW - 2; ++x)
                {
                    const float in = ((float)x + 0.5f) * 0.75f - 0.5f;
                    const int want = (int)std::lround(std::fmin(std::fmax(in / (inW - 1), 0.0f), 1.0f) * 255.0f);
                    const int got = out[((size_t)y * outW + x) * 3];
                    worst = std::max(worst, std::abs(got - want));
                    monotonic = monotonic && got + 1 >= out[((size_t)y * outW + x - 1) * 3];
                }
            char detail[64];
            std::snprintf(detail, sizeof(detail), "max_error=%d", worst);
            std::snprintf(check, sizeof(check), "REF_RAMP_%s", name);
            Check(monotonic && worst <= 2, check, detail);
        }
        {
            // Top quarter red, rest blue: the red band stays at the top.
            Image band = Make(inW, inH, [](int, int y, int, int h) {
                return y < h / 4 ? Rgb{1, 0, 0} : Rgb{0, 0, 1};
            });
            const std::vector<uint8_t> out = deko9::fsrref::UpscaleRgb8(band, outW, outH, mode, 0.2f);
            const uint8_t *top = &out[((size_t)2 * outW + 40) * 3];
            const uint8_t *bottom = &out[((size_t)(outH - 3) * outW + 40) * 3];
            std::snprintf(check, sizeof(check), "REF_ORIENTATION_%s", name);
            Check(top[0] >= 254 && top[2] <= 1 && bottom[0] <= 1 && bottom[2] >= 254, check);
        }
    }

    // 5. Source rectangles: the
    //    programs' texture-space math on a rectangle of a larger texture
    //    equals the upscaler on the cropped image, whatever lies outside the
    //    rectangle (stale pixels of a larger frame must never bleed in at the
    //    right/bottom borders). Texture 128x72 filled with saturated noise;
    //    the rectangle holds a pattern with edges and ramps. Cases: the
    //    top-left sub-rectangles the dynamic scene uses at several scales,
    //    an interior rectangle (letterboxed view), and the whole texture
    //    (the present path) against the plain reference.
    {
        const int texW = 128, texH = 72;
        Image tex = Make(texW, texH, [](int x, int y, int, int) {
            const uint32_t h = Hash((uint32_t)(y * 131 + x) ^ 0x5bd1e995u);
            return Rgb{(h & 1) ? 1.0f : 0.0f, (h & 2) ? 1.0f : 0.0f, (h & 4) ? 1.0f : 0.0f};
        });
        struct Rect
        {
            int x, y, w, h, outW, outH;
        } rects[] = {{0, 0, 128, 72, 160, 90},  {0, 0, 112, 64, 128, 72}, {0, 0, 96, 56, 128, 72},
                     {0, 0, 80, 48, 128, 72},   {0, 0, 101, 57, 128, 72}, {10, 6, 64, 40, 100, 60},
                     {0, 0, 128, 72, 128, 72}};
        for (const Rect &r : rects)
        {
            // Pattern inside the rectangle: diagonal edge, bars, ramp.
            for (int y = 0; y < r.h; ++y)
                for (int x = 0; x < r.w; ++x)
                {
                    Rgb c{(float)x / (r.w - 1), (float)y / (r.h - 1), 0.25f};
                    if (x > y + 5)
                        c = Rgb{0.9f, 0.9f, 0.85f};
                    if ((x / 3) % 4 == 1 && y > r.h / 2)
                        c = Rgb{0.05f, 0.1f, 0.4f};
                    tex.px[(size_t)(r.y + y) * texW + r.x + x] = c;
                }
            const Image crop = deko9::fsrref::Crop(tex, r.x, r.y, r.w, r.h);
            deko9::UpscaleSource src;
            deko9::UpscaleSourceSetup(&src, texW, texH, r.x, r.y, r.w, r.h);
            for (uint32_t mode = 0; mode < deko9::UPSCALE_MODE_COUNT; ++mode)
            {
                const std::vector<uint8_t> want = deko9::fsrref::UpscaleRgb8(crop, r.outW, r.outH, mode, 0.2f);
                const std::vector<uint8_t> got =
                    deko9::fsrref::UpscaleRgb8Source(tex, src, r.outW, r.outH, mode, 0.2f);
                int worst = 0, worstAt = 0;
                for (size_t i = 0; i < want.size(); ++i)
                {
                    const int d = std::abs((int)want[i] - (int)got[i]);
                    if (d > worst)
                    {
                        worst = d;
                        worstAt = (int)(i / 3);
                    }
                }
                char name[96], detail[96];
                std::snprintf(name, sizeof(name), "SOURCE_RECT_%dx%d_AT_%d_%d_TO_%dx%d_%s", r.w, r.h, r.x, r.y,
                              r.outW, r.outH, deko9::UpscaleModeName(mode));
                std::snprintf(detail, sizeof(detail), "max_diff=%d at (%d,%d)", worst, worstAt % r.outW,
                              worstAt / r.outW);
                // Float rounding of the transformed UV only.
                Check(worst <= 1, name, detail);
            }
        }
    }

    // 6. Native float-Z (r_deko9NativeFloatZ): the constant setup inverts the
    // engine's projection + viewport depth range. The engine's infinite
    // projection (k = 1 - 2^-11, zNear 4, depth hack 0.1), the scene in
    // 1/64..1 and depth-hack geometry in 0..1/64: FloatZReference on the
    // exact (unquantised) window depth returns +z for the scene and -z for
    // the viewmodel to float precision, the D24 quantisation stays below
    // 0.1% of z up to 8000 units, and a cleared or far-plane depth returns
    // the build-floatz clear value.
    {
        const double k = 0.99951172, zNear = 4.0, zHack = 0.1;
        deko9::FloatZRange scene{(float)k, (float)(-zNear * k), 0.015625f, 1.0f};
        deko9::FloatZRange viewmodel{(float)k, (float)-zHack, 0.0f, 0.015625f};
        deko9::FloatZConstants c;
        deko9::FloatZSetup(&c, scene, viewmodel, 2000000.0f);
        Check(sizeof(c) == 32 && c.viewmodel[2] == 0.015625f && c.scene[2] == 2000000.0f, "FLOATZ_CONSTANTS");
        struct Case
        {
            double z;
            bool hack;
        } cases[] = {{4.0, false},  {4.5, false},  {40.0, false}, {400.0, false}, {4000.0, false},
                     {8000.0, false}, {0.1, true}, {1.0, true},    {6.0, true},     {30.0, true}};
        for (const Case &cs : cases)
        {
            const deko9::FloatZRange &r = cs.hack ? viewmodel : scene;
            const double ndc = (double)r.m22 + (double)r.m32 / cs.z;
            const double d = r.minZ + ((double)r.maxZ - r.minZ) * ndc;
            const double want = cs.hack ? -cs.z : cs.z;
            const float exact = deko9::FloatZReference(c, (float)d);
            const double dQ = std::floor(d * 16777215.0 + 0.5) / 16777215.0;
            const float quant = deko9::FloatZReference(c, (float)dQ);
            const double relExact = std::fabs(exact - want) / cs.z, relQuant = std::fabs(quant - want) / cs.z;
            char detail[160], name[64];
            std::snprintf(detail, sizeof(detail), "z=%g d=%.9f exact=%.7g quantised=%.7g rel=%.3g rel_d24=%.3g",
                          want, d, exact, quant, relExact, relQuant);
            std::snprintf(name, sizeof(name), "FLOATZ_REF_%s_Z%g", cs.hack ? "VIEWMODEL" : "SCENE", cs.z);
            // Exact depth: float rounding of d (1 ulp at 1.0 = 6e-8)
            // amplified by z / zNear; quantised: one D24 step likewise.
            Check(relExact < 2e-4 && relQuant < 1e-3 && (exact < 0) == cs.hack, name, detail);
        }
        Check(deko9::FloatZReference(c, 1.0f) == 2000000.0f && deko9::FloatZReference(c, 0.99999994f) == 2000000.0f,
              "FLOATZ_REF_CLEAR");
    }

    std::printf("%s:DEKO9_FSR\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
