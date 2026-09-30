// Host proof for the Win32 15-bit-rand portability fix.
//
// Retail code assumed RAND_MAX == 32767 (MSVC): `rand() / 32768.0` is uniform
// [0,1) only there. glibc/newlib use 2^31-1, so engine random() returned up
// to ~65535 and every direct `rand()` scaling site was off by ~2000x
// (recoil, sound volume/pitch, FX seeds, particle verts, debris dirs,
// tracers). This verifier (a) gates the shipped source: the fixed sites must
// scale by the real RAND_MAX and the old 15-bit literals must be gone, and
// (b) validates each shipped formula numerically with real libc rand().
// Emits PASS:RANDOM15_PORTABILITY on success, FAIL + nonzero exit otherwise.
//
// Run by ./test host (random15_check), from the repo root.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
int g_failures = 0;

void Check(bool ok, const char *name)
{
    std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
    if (!ok)
        ++g_failures;
}

std::string ReadFile(const char *path)
{
    std::string out;
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return out;
}

// The exact formulas now shipped (kept in sync with the source edits).
float FixedRandom()
{
    float r = (float)(rand() / (RAND_MAX + 1.0));
    if (r >= 1.0f)
        r = 0.99999988f;
    return r;
}
int FixedFxSeed()
{
    return (int)(479.0 * rand() / (RAND_MAX + 1.0));
}
} // namespace

int main()
{
    // ---- (a) source gates on the real files ----
    const std::string comMath = ReadFile("src/universal/com_math.cpp");
    Check(!comMath.empty(), "source: com_math.cpp readable");
    Check(comMath.find("rand() / (RAND_MAX + 1.0)") != std::string::npos,
          "source: random() scales by RAND_MAX");
    Check(comMath.find("0.99999988f") != std::string::npos,
          "source: random() clamps the float-rounding edge");
    Check(comMath.find("rand() / 32768.0") == std::string::npos,
          "source: old /32768.0 gone from random()");

    const std::string fxSys = ReadFile("src/EffectsCore/fx_system.cpp");
    Check(fxSys.find("479.0 * rand() / (RAND_MAX + 1.0)") != std::string::npos,
          "source: fx seed scales by RAND_MAX");
    Check(fxSys.find("479 * rand() / 0x8000") == std::string::npos,
          "source: old 479*rand()/0x8000 gone");

    const std::string fxUpd = ReadFile("src/EffectsCore/fx_update.cpp");
    Check(fxUpd.find("(double)rand() / (RAND_MAX + 1.0)") != std::string::npos,
          "source: fx emit spacing scales by RAND_MAX");
    Check(fxUpd.find("0.000030517578125") == std::string::npos,
          "source: old 1/32768 literal gone from fx emit");

    const std::string rBuf = ReadFile("src/gfx_d3d/r_buffers.cpp");
    Check(rBuf.find("(double)rand() / (RAND_MAX + 1.0)") != std::string::npos,
          "source: particle verts scale by RAND_MAX");
    Check(rBuf.find("rand() / 32767.0") == std::string::npos,
          "source: old /32767.0 gone from particle verts");

    const std::string pieces = ReadFile("src/DynEntity/DynEntity_pieces.cpp");
    Check(pieces.find("/ (RAND_MAX + 1.0)") != std::string::npos,
          "source: debris dirs scale by RAND_MAX");
    Check(pieces.find("/ 32767.0") == std::string::npos,
          "source: old /32767.0 gone from debris dirs");

    const std::string cgEv = ReadFile("src/cgame/cg_event.cpp");
    Check(cgEv.find("* (RAND_MAX + 1.0) > (double)rand()") != std::string::npos,
          "source: EV_BULLET_TRACER threshold in real rand domain");
    const std::string cgWeap = ReadFile("src/cgame/cg_weapons.cpp");
    {
        std::size_t count = 0, pos = 0;
        const char *needle = "* (RAND_MAX + 1.0) > (double)rand()";
        while ((pos = cgWeap.find(needle, pos)) != std::string::npos)
        {
            ++count;
            ++pos;
        }
        Check(count == 2, "source: both cg_weapons tracer gates fixed");
    }
    Check(cgWeap.find("* 32768.0 > (double)rand()") == std::string::npos &&
              cgWeap.find("* 32768.0f > (float)rand()") == std::string::npos &&
              cgEv.find("* 32768.0f > (float)rand()") == std::string::npos,
          "source: old *32768 tracer thresholds gone");

    // ---- (b) numeric contract of the shipped formulas ----
    Check(RAND_MAX == 0x7fffffff, "env: host RAND_MAX is 31-bit (bug bites here)");
    std::srand(12345);

    // Negative control: the old formula really is broken on this libc.
    {
        float worst = 0.0f;
        for (int i = 0; i < 100000; ++i)
        {
            float r = (float)(rand() / 32768.0);
            if (r > worst)
                worst = r;
        }
        Check(worst > 1.0f, "control: old rand()/32768 exceeds 1.0");
    }

    // Fixed random(): 1M draws all in [0,1), range not collapsed.
    std::srand(12345);
    {
        float lo = 1.0f, hi = 0.0f;
        bool inRange = true;
        for (int i = 0; i < 1000000; ++i)
        {
            float r = FixedRandom();
            if (!(r >= 0.0f && r < 1.0f))
                inRange = false;
            if (r < lo)
                lo = r;
            if (r > hi)
                hi = r;
        }
        Check(inRange, "numeric: fixed random() in [0,1) over 1M draws");
        Check(hi > 0.999f && lo < 0.001f, "numeric: fixed random() spans [0,1)");
        std::printf("info random() min=%f max=%f\n", lo, hi);
    }

    // Boundaries map exactly: 0 -> 0, RAND_MAX -> just under 1.
    Check((double)0 / (RAND_MAX + 1.0) == 0.0, "numeric: zero boundary exact");
    Check((double)RAND_MAX / (RAND_MAX + 1.0) < 1.0, "numeric: max draw < 1.0");
    {
        // Narrowing the max draw to float rounds up to exactly 1.0f, so the
        // shipped clamp must catch it (retail never returned 1.0).
        float narrowed = (float)((double)RAND_MAX / (RAND_MAX + 1.0));
        Check(narrowed >= 1.0f, "control: float narrowing really hits 1.0");
        float clamped = narrowed >= 1.0f ? 0.99999988f : narrowed;
        Check(clamped < 1.0f, "numeric: clamp keeps [0,1) in float");
    }

    // FX seed stays in the retail [0,478] table range.
    std::srand(999);
    {
        int lo = 479, hi = -1;
        for (int i = 0; i < 200000; ++i)
        {
            int s = FixedFxSeed();
            if (s < lo)
                lo = s;
            if (s > hi)
                hi = s;
        }
        Check(lo >= 0 && hi <= 478, "numeric: fx seed in [0,478]");
        Check((int)(479.0 * RAND_MAX / (RAND_MAX + 1.0)) == 478,
              "numeric: fx seed max boundary is 478");
    }

    // Debris direction components stay in [-1,1].
    {
        double v = (double)RAND_MAX;
        double hi = v / (RAND_MAX + 1.0) + v / (RAND_MAX + 1.0) - 1.0;
        Check(hi < 1.0 && hi > 0.999, "numeric: debris dir max boundary");
        Check(0.0 / (RAND_MAX + 1.0) + 0.0 / (RAND_MAX + 1.0) - 1.0 == -1.0,
              "numeric: debris dir min boundary");
    }

    // Tracer gate keeps the retail probability: P(fire) == chance.
    std::srand(4242);
    {
        const double chance = 0.35;
        int fired = 0;
        const int draws = 200000;
        for (int i = 0; i < draws; ++i)
        {
            if (chance * (RAND_MAX + 1.0) > (double)rand())
                ++fired;
        }
        const double rate = (double)fired / draws;
        Check(std::fabs(rate - chance) < 0.005, "numeric: tracer rate == chance");
        std::printf("info tracer rate=%f chance=%f\n", rate, chance);
    }

    // Recoil-shaped draw stays in its weapon-defined band.
    std::srand(12345);
    {
        const float mn = 0.5f, mx = 1.5f;
        bool ok = true;
        for (int i = 0; i < 100000; ++i)
        {
            float kick = FixedRandom() * (mx - mn) + mn;
            if (!(kick >= mn && kick < mx))
                ok = false;
        }
        Check(ok, "numeric: recoil kick inside [min,max)");
    }

    if (g_failures == 0)
        std::printf("PASS:RANDOM15_PORTABILITY\n");
    else
        std::printf("FAIL:RANDOM15_PORTABILITY failures=%d\n", g_failures);
    return g_failures ? 1 : 0;
}
