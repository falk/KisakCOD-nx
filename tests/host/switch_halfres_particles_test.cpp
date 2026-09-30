// Host proof for off-screen soft particles (r_halfResParticles): the engine rules
// (src/gfx_d3d/r_halfres_particles_rules.h) and the CPU reference of the
// GPU passes (src/deko9/deko9_particles_reference.h, the deko9 selftest's
// oracle). Checks:
//   - blend remap: over / additive / srcalpha-additive keep their colour
//     blend and get the transmittance alpha blend with alpha writes; every
//     blend the composite cannot reproduce is refused;
//   - classification of the retail emissive techniques seen at the
//     Cargoship/Killhouse census spots (rain, snow, smog: off-screen;
//     beams, flares, non-feathered sprites, depth writers: full res);
//   - the premultiplied scheme: N layers blended off-screen then composited
//     equal the same layers blended directly (exact in real arithmetic,
//     within UNORM8 rounding in 8-bit targets, faint 2-3% layers included);
//   - the depth rule: nearest of the footprint, identity at factor 1, clamp
//     at the rectangle edge;
//   - the upsample: identity at scale 1 (both modes), bilinear in flat
//     depth, nearest-depth at a silhouette, and a whole scale-1 image
//     (depth-tested particle layers over a scene) equal to direct drawing.

#include "src/deko9/deko9_particles_reference.h"
#include "src/gfx_d3d/r_halfres_particles_rules.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace
{
int g_failures;

void Check(bool ok, const char *what, const char *detail = "")
{
    std::printf("%s:HRP_%s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok)
        ++g_failures;
}

using deko9::hrpref::Rgba;
using RefBlend = deko9::hrpref::Blend;

uint32_t Bits(uint32_t src, uint32_t dst, uint32_t op = hrp::kOpAdd)
{
    return src | (dst << hrp::kDstRgbShift) | (op << hrp::kOpRgbShift) | hrp::kColorWriteRgb;
}

void TestRemap()
{
    struct Case
    {
        uint32_t src, dst;
        uint32_t wantDstAlpha;
    } ok[] = {{hrp::kBlendSrcAlpha, hrp::kBlendInvSrcAlpha, hrp::kBlendInvSrcAlpha},
              {hrp::kBlendOne, hrp::kBlendOne, hrp::kBlendOne},
              {hrp::kBlendSrcAlpha, hrp::kBlendOne, hrp::kBlendOne}};
    bool good = true;
    for (const Case &c : ok)
    {
        // Other state (alpha test, cull) must survive.
        const uint32_t in = Bits(c.src, c.dst) | 0x1000u /* atest gt0 */ | 0x8000u /* cull back */;
        uint32_t out = 0;
        good = good && hrp::RemapStateBits0(in, &out) && (out & 0x7FF) == (in & 0x7FF) &&
               ((out >> hrp::kSrcAlphaShift) & 0xF) == hrp::kBlendZero &&
               ((out >> hrp::kDstAlphaShift) & 0xF) == c.wantDstAlpha &&
               ((out >> hrp::kOpAlphaShift) & 7) == hrp::kOpAdd && (out & hrp::kColorWriteAlpha) &&
               (out & hrp::kColorWriteRgb) && (out & 0xF000u) == 0x9000u;
    }
    Check(good, "REMAP_SUPPORTED");
    // Screen (invdstcolor:one, the floodlight beams), multiply, one:zero,
    // blend off, subtract/revsub ops, and a material with its own alpha blend.
    // A material's own alpha blend is replaced (the retail rain carries one).
    uint32_t own = 0;
    const bool ownAlpha = hrp::RemapStateBits0(Bits(5, 6) | (1u << hrp::kOpAlphaShift) | (2u << hrp::kSrcAlphaShift) |
                                                   (1u << hrp::kDstAlphaShift),
                                               &own) &&
                          ((own >> hrp::kSrcAlphaShift) & 0xF) == hrp::kBlendZero &&
                          ((own >> hrp::kDstAlphaShift) & 0xF) == hrp::kBlendInvSrcAlpha;
    Check(ownAlpha, "REMAP_REPLACES_OWN_ALPHA_BLEND");
    const uint32_t refused[] = {Bits(10, 2), Bits(9, 1), Bits(2, 1), 0u, Bits(5, 6, 2), Bits(2, 2, 3)};
    bool refusedAll = true;
    for (uint32_t b : refused)
    {
        uint32_t out;
        refusedAll = refusedAll && !hrp::RemapStateBits0(b, &out) && hrp::BlendOf(b) == hrp::Blend::Unsupported;
    }
    Check(refusedAll, "REMAP_REFUSES_UNSUPPORTED");
}

void TestClassify()
{
    const uint32_t floatZ = hrp::kTechUsesFloatZ;
    const uint32_t over[2] = {Bits(5, 6), 0x0Cu /* lessequal, no write */};
    const uint32_t add[2] = {Bits(2, 2), 0x0Cu};
    const uint32_t screen[2] = {Bits(10, 2), 0x0Cu};
    const uint32_t overWrite[2] = {Bits(5, 6), 0x0Du};
    const uint32_t overNoTest[2] = {Bits(5, 6), 0x02u};
    const uint32_t overStencil[2] = {Bits(5, 6), 0x4Cu};
    struct Case
    {
        const char *technique;
        uint32_t flags, passes;
        const uint32_t *bits;
        hrp::Reason want;
    } cases[] = {
        {"zfeather_outdoor_fog_dtex", floatZ, 1, over, hrp::Reason::Offscreen}, // gfx_wind_rain_atlas
        {"zfeather_fog_dtex", floatZ, 1, over, hrp::Reason::Offscreen},         // snow sheets, fog wisps
        {"zfeather_add_fog_dtex", floatZ, 1, add, hrp::Reason::Offscreen},      // Killhouse smog
        {"zfeather_add_dtex", floatZ, 1, add, hrp::Reason::Offscreen},          // drips, glows
        {"zfeather_add_falloff_dtex", floatZ, 1, screen, hrp::Reason::ViewDependent},
        {"zfeather_add_falloff_dtex", floatZ, 1, add, hrp::Reason::ViewDependent},
        {"zfeather_add_eyeofs_falloff_dtex", floatZ, 1, add, hrp::Reason::ViewDependent},
        {"zfeather_add_eyeofs_dtex", floatZ, 1, add, hrp::Reason::ViewDependent},
        {"zfeather_screen_dtex", floatZ, 1, screen, hrp::Reason::Blend},
        {"effect_add_dtex", 0, 1, add, hrp::Reason::NotSoftParticle},
        {"vertcol_simple_fog_dtex", 0, 1, over, hrp::Reason::NotSoftParticle},
        {"particle_cloud_outdoor", 0, 1, over, hrp::Reason::NotSoftParticle},
        {"zfeather_fog_dtex", 0, 1, over, hrp::Reason::NotSoftParticle}, // no float-Z sample
        {"zfeather_fog_dtex", floatZ, 2, over, hrp::Reason::MultiPass},
        {"zfeather_fog_dtex", floatZ | hrp::kTechNeedsResolvedPostSun, 1, over, hrp::Reason::NeedsResolve},
        {"zfeather_fog_dtex", floatZ, 1, overWrite, hrp::Reason::DepthState},
        {"zfeather_fog_dtex", floatZ, 1, overNoTest, hrp::Reason::DepthState},
        {"zfeather_fog_dtex", floatZ, 1, overStencil, hrp::Reason::DepthState},
        {nullptr, floatZ, 1, over, hrp::Reason::NoTechnique},
    };
    bool good = true;
    char detail[160] = "";
    for (const Case &c : cases)
    {
        const hrp::Reason got = hrp::Classify(c.technique, c.flags, c.passes, c.bits);
        if (got != c.want && good)
        {
            std::snprintf(detail, sizeof(detail), "%s: %s want %s", c.technique ? c.technique : "(null)",
                          hrp::ReasonName(got), hrp::ReasonName(c.want));
            good = false;
        }
    }
    Check(good, "CLASSIFY", detail);
    int w, h;
    hrp::OffscreenSize(960, 544, 2, &w, &h);
    bool size = w == 480 && h == 272;
    hrp::OffscreenSize(1279, 719, 2, &w, &h);
    size = size && w == 640 && h == 360;
    hrp::OffscreenSize(1280, 720, 1, &w, &h);
    Check(size && w == 1280 && h == 720, "OFFSCREEN_SIZE");
}

RefBlend RandomBlend(std::mt19937 &rng)
{
    return (RefBlend)(rng() % 3);
}

Rgba RandomLayer(std::mt19937 &rng, bool faint)
{
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    Rgba s{u(rng), u(rng), u(rng), faint ? 0.015f + 0.02f * u(rng) : u(rng)};
    return s;
}

// Scheme exactness: layers blended off-screen then composited == blended
// directly, in real arithmetic and in 8-bit targets.
void TestScheme()
{
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    double worstExact = 0.0;
    int worstLsb = 0;
    double sumLsb = 0.0;
    uint64_t samples = 0;
    int worstFaint = 0;
    for (int trial = 0; trial < 200000; ++trial)
    {
        const bool faint = trial & 1;
        const int n = 1 + (int)(rng() % 8);
        Rgba scene{u(rng), u(rng), u(rng), 1.0f};
        scene = deko9::hrpref::Unorm8(scene);
        Rgba directF = scene, direct8 = scene, accF{0, 0, 0, 1}, acc8{0, 0, 0, 1};
        bool additiveSaturates = false;
        for (int i = 0; i < n; ++i)
        {
            Rgba s = RandomLayer(rng, faint);
            // The GPU's shader outputs are float, the blend reads them as is.
            const RefBlend b = RandomBlend(rng);
            directF = deko9::hrpref::BlendDirect(directF, s, b, false);
            direct8 = deko9::hrpref::BlendDirect(direct8, s, b, true);
            accF = deko9::hrpref::BlendOffscreen(accF, s, b, false);
            acc8 = deko9::hrpref::BlendOffscreen(acc8, s, b, true);
            additiveSaturates = additiveSaturates || directF.r > 1.0f || directF.g > 1.0f || directF.b > 1.0f ||
                                accF.r > 1.0f || accF.g > 1.0f || accF.b > 1.0f;
        }
        const Rgba viaF = deko9::hrpref::Composite(scene, accF, false);
        const Rgba via8 = deko9::hrpref::Composite(scene, acc8, true);
        if (!additiveSaturates)
        {
            worstExact = std::max({worstExact, (double)std::fabs(viaF.r - directF.r),
                                   (double)std::fabs(viaF.g - directF.g), (double)std::fabs(viaF.b - directF.b)});
        }
        for (float d : {via8.r - direct8.r, via8.g - direct8.g, via8.b - direct8.b})
        {
            const int lsb = (int)std::lround(std::fabs(d) * 255.0f);
            // Saturation differs by design where additive layers exceed 1
            // (the direct target clamps after each add, the off-screen one
            // clamps C before T scales the scene): count the rounding bound
            // only where no channel saturated.
            if (additiveSaturates)
                continue;
            worstLsb = std::max(worstLsb, lsb);
            if (faint)
                worstFaint = std::max(worstFaint, lsb);
            sumLsb += lsb;
            ++samples;
        }
    }
    char detail[160];
    std::snprintf(detail, sizeof(detail), "max_abs_err=%.3g", worstExact);
    Check(worstExact < 2e-6, "SCHEME_EXACT_FLOAT", detail);
    const double mean = samples ? sumLsb / (double)samples : 0.0;
    std::snprintf(detail, sizeof(detail), "max_lsb=%d faint_max_lsb=%d mean_lsb=%.3f samples=%llu", worstLsb,
                  worstFaint, mean, (unsigned long long)samples);
    // Up to 8 layers, each off-screen and direct step rounds by <= 0.5 LSB
    // in C and in T: the bound is 4 LSB, the typical error well under 1.
    Check(worstLsb <= 4 && worstFaint <= 4 && mean < 0.5, "SCHEME_UNORM8", detail);
}

void TestDownsample()
{
    // 5x3 scene with a near object in the middle column.
    const int W = 5, H = 3;
    const float depth[W * H] = {0.9f, 0.8f, 0.2f, 0.9f, 0.95f, //
                                0.9f, 0.7f, 0.2f, 0.3f, 0.95f, //
                                0.6f, 0.9f, 0.9f, 0.9f, 0.10f};
    const int rect[4] = {0, 0, W, H};
    int x, y;
    bool good = true;
    // factor 2: texel (0,0) covers (0..1, 0..1): nearest 0.7 at (1,1).
    deko9::hrpref::DownsamplePick(depth, W, rect, 2, 0, 0, &x, &y);
    good = good && x == 1 && y == 1;
    // texel (1,0) covers (2..3, 0..1): 0.2 at (2,0) first, (2,1) equal: first wins.
    deko9::hrpref::DownsamplePick(depth, W, rect, 2, 1, 0, &x, &y);
    good = good && x == 2 && y == 0;
    // texel (2,1) covers (4, 2) clamped: only (4,2).
    deko9::hrpref::DownsamplePick(depth, W, rect, 2, 2, 1, &x, &y);
    good = good && x == 4 && y == 2;
    // texel (0,1): rows 2..3 clamp to 2: (0,2)=0.6, (1,2)=0.9 -> (0,2).
    deko9::hrpref::DownsamplePick(depth, W, rect, 2, 0, 1, &x, &y);
    good = good && x == 0 && y == 2;
    Check(good, "DOWNSAMPLE_NEAREST");
    bool identity = true;
    for (int yy = 0; yy < H; ++yy)
        for (int xx = 0; xx < W; ++xx)
        {
            deko9::hrpref::DownsamplePick(depth, W, rect, 1, xx, yy, &x, &y);
            identity = identity && x == xx && y == yy;
        }
    // A rectangle with an origin (a view not at 0,0).
    const int sub[4] = {1, 1, 4, 2};
    deko9::hrpref::DownsamplePick(depth, W, sub, 2, 1, 0, &x, &y);
    // covers (3..4, 1..2): 0.3, 0.95, 0.9, 0.10 -> (4, 2).
    Check(identity && x == 4 && y == 2, "DOWNSAMPLE_IDENTITY_AND_ORIGIN");
}

void TestUpsample()
{
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    // Scale 1: identity for every pixel and both modes.
    const int W = 17, H = 9;
    std::vector<Rgba> color(W * H);
    std::vector<float> z(W * H);
    for (int i = 0; i < W * H; ++i)
    {
        color[i] = {u(rng), u(rng), u(rng), u(rng)};
        z[i] = 10.0f + 1000.0f * u(rng);
    }
    bool identity = true;
    for (int mode = 0; mode < 2; ++mode)
    {
        deko9::hrpref::CompositeConstants c;
        const int origin[2] = {3, 2}, view[2] = {W, H}, off[2] = {W, H};
        deko9::hrpref::CompositeSetup(&c, origin, view, off, mode, 0.1f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const Rgba got = deko9::hrpref::CompositeSample(c, color.data(), z.data(), W, z[y * W + x],
                                                                x + origin[0], y + origin[1]);
                const Rgba &want = color[y * W + x];
                identity = identity && got.r == want.r && got.g == want.g && got.b == want.b && got.a == want.a;
            }
    }
    Check(identity, "UPSAMPLE_IDENTITY_SCALE1");

    // Half scale, 4x2 off-screen texels for an 8x4 view. Left half at depth
    // 100 (near object), right half 1000; the particles differ per side.
    const int OW = 4, OH = 2;
    Rgba off[OW * OH];
    float hz[OW * OH];
    for (int y = 0; y < OH; ++y)
        for (int x = 0; x < OW; ++x)
        {
            const bool nearSide = x < 2;
            off[y * OW + x] = nearSide ? Rgba{0.0f, 0.0f, 0.0f, 1.0f} : Rgba{0.5f, 0.5f, 0.5f, 0.5f};
            hz[y * OW + x] = nearSide ? 100.0f : 1000.0f;
        }
    deko9::hrpref::CompositeConstants bil, nd;
    const int origin[2] = {0, 0}, view[2] = {8, 4}, offs[2] = {OW, OH};
    deko9::hrpref::CompositeSetup(&bil, origin, view, offs, 0, 0.1f);
    deko9::hrpref::CompositeSetup(&nd, origin, view, offs, 1, 0.1f);
    // Pixel 4 (first far pixel): h = 4.5 * 0.5 - 0.5 = 1.75 -> blends texels 1 (near) and 2 (far).
    const Rgba b = deko9::hrpref::CompositeSample(bil, off, hz, OW, 1000.0f, 4, 1);
    const Rgba n = deko9::hrpref::CompositeSample(nd, off, hz, OW, 1000.0f, 4, 1);
    // Pixel 3 (last near pixel): nearest-depth keeps the near texel.
    const Rgba n3 = deko9::hrpref::CompositeSample(nd, off, hz, OW, 100.0f, 3, 1);
    // Flat far region: pixel 6 blends texels 2 and 3 (identical) -> exact.
    const Rgba n6 = deko9::hrpref::CompositeSample(nd, off, hz, OW, 1000.0f, 6, 1);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "bilinear_at_edge=%.3f/%.3f nearest=%.3f/%.3f near=%.3f", b.r, b.a, n.r,
                  n.a, n3.a);
    // h = 1.75: 25% of the near texel (black, T = 1) leaks into the first far
    // pixel: the mixed-resolution halo the nearest-depth mode removes.
    Check(std::fabs(b.r - 0.375f) < 1e-6f && std::fabs(b.a - 0.625f) < 1e-6f, "UPSAMPLE_BILINEAR_HALO", detail);
    Check(n.r == 0.5f && n.a == 0.5f && n3.r == 0.0f && n3.a == 1.0f && n6.r == 0.5f, "UPSAMPLE_NEAREST_DEPTH",
          detail);
}

// Whole scale-1 image: a scene with depths, particle layers depth-tested
// against it, drawn directly vs off-screen (depth copy at factor 1,
// composite in nearest-depth mode) in 8-bit targets.
void TestImageScale1()
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const int W = 32, H = 16, N = 6;
    std::vector<Rgba> scene(W * H), direct, acc(W * H, Rgba{0, 0, 0, 1});
    std::vector<float> depth(W * H), fz(W * H), halfDepth(W * H), halfZ(W * H);
    for (int i = 0; i < W * H; ++i)
    {
        scene[i] = deko9::hrpref::Unorm8(Rgba{u(rng), u(rng), u(rng), 1.0f});
        depth[i] = (i % W) < W / 2 ? 0.3f : 0.9f;
        fz[i] = (i % W) < W / 2 ? 50.0f : 900.0f;
    }
    direct = scene;
    const int rect[4] = {0, 0, W, H};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            int px, py;
            deko9::hrpref::DownsamplePick(depth.data(), W, rect, 1, x, y, &px, &py);
            halfDepth[y * W + x] = depth[py * W + px];
            halfZ[y * W + x] = fz[py * W + px];
        }
    for (int layer = 0; layer < N; ++layer)
    {
        const float pz = 0.2f + 0.15f * layer; // some layers behind the near half
        const RefBlend b = (RefBlend)(layer % 3);
        for (int i = 0; i < W * H; ++i)
        {
            // Additive layers faint (no saturation: the clamp point differs
            // by design, see TestScheme).
            Rgba s = RandomLayer(rng, (layer & 1) || b != RefBlend::Over);
            if (b == RefBlend::Add)
                s.r *= 0.1f, s.g *= 0.1f, s.b *= 0.1f;
            if (pz <= depth[i])
                direct[i] = deko9::hrpref::BlendDirect(direct[i], s, b, true);
            if (pz <= halfDepth[i])
                acc[i] = deko9::hrpref::BlendOffscreen(acc[i], s, b, true);
        }
    }
    deko9::hrpref::CompositeConstants c;
    const int origin[2] = {0, 0}, view[2] = {W, H}, off[2] = {W, H};
    deko9::hrpref::CompositeSetup(&c, origin, view, off, 1, 0.1f);
    int worst = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const Rgba a = deko9::hrpref::CompositeSample(c, acc.data(), halfZ.data(), W, fz[y * W + x], x, y);
            const Rgba out = deko9::hrpref::Composite(scene[y * W + x], a, true);
            const Rgba &d = direct[y * W + x];
            for (float e : {out.r - d.r, out.g - d.g, out.b - d.b})
                worst = std::max(worst, (int)std::lround(std::fabs(e) * 255.0f));
        }
    char detail[64];
    std::snprintf(detail, sizeof(detail), "max_lsb=%d", worst);
    // Same rounding bound as SCHEME_UNORM8 (<= 0.5 LSB per layer and target).
    Check(worst <= 4, "IMAGE_SCALE1", detail);
}

} // namespace

int main()
{
    TestRemap();
    TestClassify();
    TestScheme();
    TestDownsample();
    TestUpsample();
    TestImageScale1();
    std::printf("%s:HALFRES_PARTICLES_HOST\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
