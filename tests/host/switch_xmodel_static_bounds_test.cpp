// Host proof for src/xanim/xmodel_static_bounds.h: XModelStaticBounds (NEON
// on AArch64) must give bit-identical mins/maxs to the retail scalar loop
// (XModelStaticBoundsScalar, MatrixTransformVector's double expression).
// ./test runs it on x86 under ASan/UBSan (scalar vs scalar: harness check)
// and built for AArch64 under qemu-aarch64 (NEON vs scalar).

#include <xanim/xmodel_static_bounds.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace
{
struct Surf // the fields XModelCollSurf_s contributes
{
    float mins[3];
    float maxs[3];
};

float Special(std::mt19937 &rng, float v)
{
    switch (rng() % 24)
    {
    case 0:
        return std::numeric_limits<float>::quiet_NaN();
    case 1:
        return std::numeric_limits<float>::infinity();
    case 2:
        return -std::numeric_limits<float>::infinity();
    case 3:
        return 0.0f;
    case 4:
        return -0.0f;
    case 5:
        return 3.0e38f; // products and sums overflow float on narrowing
    case 6:
        return 1.0e-40f; // denormal
    default:
        return v;
    }
}

void RandomAxis(std::mt19937 &rng, float (&axis)[3][3], bool specials)
{
    std::uniform_real_distribution<float> angle(-3.2f, 3.2f), any(-2.0f, 2.0f);
    switch (rng() % 3)
    {
    case 0: // identity (G_GetModelBounds)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                axis[r][c] = r == c ? 1.0f : 0.0f;
        break;
    case 1: // rotation about z then x
    {
        const float a = angle(rng), b = angle(rng);
        const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
        const float m[3][3] = {{ca, sa, 0}, {-sa * cb, ca * cb, sb}, {sa * sb, -ca * sb, cb}};
        std::memcpy(axis, m, sizeof(m));
        break;
    }
    default:
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                axis[r][c] = any(rng);
    }
    if (specials)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                if (rng() % 6 == 0)
                    axis[r][c] = Special(rng, axis[r][c]);
}

bool SameBits(const float *a, const float *b)
{
    return !std::memcmp(a, b, 3 * sizeof(float));
}
} // namespace

int main()
{
    std::mt19937 rng(0x5B0Du);
    std::uniform_real_distribution<float> coord(-300.0f, 300.0f);
    int failures = 0, cases = 0, nanCases = 0;
    for (int round = 0; round < 40000; ++round)
    {
        const bool specials = round % 3 == 0;
        // Every 4th round: +/-1 axes and coordinates of wildly different
        // magnitudes, so (p0 + p1) + p2 and any other summation order give
        // different floats (catastrophic cancellation): the order is tested.
        const bool cancel = round % 4 == 1;
        float axis[3][3];
        RandomAxis(rng, axis, specials);
        if (cancel)
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    axis[r][c] = (rng() & 1) ? 1.0f : -1.0f;
        std::vector<Surf> surfs(1 + rng() % 12);
        for (Surf &s : surfs)
        {
            if (cancel)
            {
                static const float kMags[] = {1.0e30f, 1.0f, 3.0f, 1.0e-3f, 7.0e20f};
                for (int a = 0; a < 3; ++a)
                {
                    const float m = kMags[rng() % 5];
                    s.mins[a] = (rng() & 1) ? m : -m;
                    s.maxs[a] = (rng() & 1) ? kMags[rng() % 5] : -kMags[rng() % 5];
                }
                continue;
            }
            for (int a = 0; a < 3; ++a)
            {
                s.mins[a] = coord(rng);
                s.maxs[a] = s.mins[a] + std::fabs(coord(rng));
                if (rng() % 7 == 0)
                    s.maxs[a] = s.mins[a];
                if (specials && rng() % 5 == 0)
                {
                    s.mins[a] = Special(rng, s.mins[a]);
                    s.maxs[a] = Special(rng, s.maxs[a]);
                }
            }
        }
        float refMins[3], refMaxs[3], mins[3], maxs[3];
        XModelStaticBoundsScalar(surfs.data(), (int)surfs.size(), axis, refMins, refMaxs);
        XModelStaticBounds(surfs.data(), (int)surfs.size(), axis, mins, maxs);
        ++cases;
        nanCases += std::isnan(refMins[0] + refMins[1] + refMins[2]) || refMins[0] == FLT_MAX;
        if (!SameBits(refMins, mins) || !SameBits(refMaxs, maxs))
        {
            if (++failures <= 4)
                std::printf("  round %d retail mins=%.9g,%.9g,%.9g maxs=%.9g,%.9g,%.9g new mins=%.9g,%.9g,%.9g "
                            "maxs=%.9g,%.9g,%.9g\n",
                            round, refMins[0], refMins[1], refMins[2], refMaxs[0], refMaxs[1], refMaxs[2], mins[0],
                            mins[1], mins[2], maxs[0], maxs[1], maxs[2]);
        }
    }
    std::printf("XMODEL_STATIC_BOUNDS cases=%d all_nan_or_empty=%d failures=%d\n", cases, nanCases, failures);
    if (failures)
    {
        std::printf("FAIL:XMODEL_STATIC_BOUNDS\n");
        return 1;
    }
#if defined(__aarch64__) && !defined(KISAK_XMODEL_BOUNDS_SCALAR)
    std::printf("PASS:XMODEL_STATIC_BOUNDS neon\n");
#else
    std::printf("PASS:XMODEL_STATIC_BOUNDS\n");
#endif
    return 0;
}
