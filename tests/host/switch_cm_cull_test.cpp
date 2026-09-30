// Host proof for src/qcommon/cm_cull.h and the BG_EvaluateTrajectory fast
// path (src/bgame/bg_local.h):
//   CM_CullBoxInline      == retail CM_CullBox (Vec3Sub/Vec3Add/Q_fabs calls,
//                            (double) comparisons), bool for every input;
//   BoxOnPlaneSideInline  == retail BoxOnPlaneSide switch, side for every input;
//   CM_CullFabs           == retail Q_fabs integer mask, bits for every float
//                            (all 2^32 patterns);
//   BG_TrajectoryIsBaseCopy == the trTypes the retail switch copies trBase for.
// Inputs mix exact small values, random magnitudes, near-tie thresholds and
// specials (NaN with payloads, +-inf, +-0, denormals, FLT_MAX).
//
// Built three ways by ./test: x86 under ASan/UBSan (one binary), and for
// AArch64 under qemu with the code under test compiled by the Switch compiler
// (devkitA64, -O2, FMA contraction on and off: -DCM_CULL_UNITS) linked into an
// aarch64-linux driver (-DCM_CULL_DRIVER), so the FMA contraction of the cross
// axis tests matches the NRO.

#include <cstdint>
#include <cstring>

#if !defined(CM_CULL_DRIVER) || defined(CM_CULL_UNITS)
#define CM_CULL_BUILD_UNITS 1
#endif
#if !defined(CM_CULL_UNITS) || defined(CM_CULL_DRIVER)
#define CM_CULL_BUILD_DRIVER 1
#endif

namespace cmcull
{
// The traceWork_t / cplane_s fields the functions read.
struct TraceWork
{
    float midpoint[3];
    float halfDelta[3];
    float halfDeltaAbs[3];
    float size[3];
    bool axialCullOnly;
};

struct Plane
{
    float normal[3];
    float dist;
    uint8_t type;
    uint8_t signbits;
    uint8_t pad[2];
};

bool NewCullBox(const TraceWork *tw, const float *origin, const float *halfSize);
bool RefCullBox(const TraceWork *tw, const float *origin, const float *halfSize);
int NewBoxOnPlaneSide(const float *emins, const float *emaxs, const Plane *p);
int RefBoxOnPlaneSide(const float *emins, const float *emaxs, const Plane *p);
uint32_t NewFabsBits(uint32_t bits);
uint32_t RefFabsBits(uint32_t bits);
bool NewIsBaseCopy(int32_t trType);
bool RefIsBaseCopy(int32_t trType);
} // namespace cmcull

#ifdef CM_CULL_BUILD_UNITS
#include <qcommon/cm_cull.h>

// BG_TrajectoryIsBaseCopy, verbatim from src/bgame/bg_local.h (that header
// pulls in the whole engine).
constexpr bool BG_TrajectoryIsBaseCopy(int32_t trType)
{
    // TR_STATIONARY, TR_INTERPOLATE, TR_PHYSICS, TR_RAGDOLL_INTERPOLATE
    return (uint32_t)trType < 32u && ((0x903u >> (uint32_t)trType) & 1u) != 0;
}

namespace cmcull
{
// ---- retail reference, copied from cm_mesh.cpp / com_math.cpp /
// cm_showcollision.cpp / bg_misc.cpp before the change. The helpers stay out
// of line like the engine's separate translation units, so no FMA contraction
// crosses them.
__attribute__((noinline)) void Vec3Sub(const float *a, const float *b, float *diff)
{
    diff[0] = a[0] - b[0];
    diff[1] = a[1] - b[1];
    diff[2] = a[2] - b[2];
}

__attribute__((noinline)) void Vec3Add(const float *a, const float *b, float *sum)
{
    sum[0] = a[0] + b[0];
    sum[1] = a[1] + b[1];
    sum[2] = a[2] + b[2];
}

__attribute__((noinline)) float Q_fabs(float f)
{
    int tmp;
    std::memcpy(&tmp, &f, sizeof(tmp)); // retail: *(int *)&f
    tmp &= 0x7FFFFFFF;
    float out;
    std::memcpy(&out, &tmp, sizeof(out));
    return out;
}
#define I_fabs Q_fabs

__attribute__((noinline)) bool RefCullBox(const TraceWork *tw, const float *origin, const float *halfSize)
{
    float v4; // [esp+0h] [ebp-78h]
    float v5; // [esp+4h] [ebp-74h]
    float v6; // [esp+8h] [ebp-70h]
    float v7; // [esp+Ch] [ebp-6Ch]
    float v8; // [esp+10h] [ebp-68h]
    float v9; // [esp+14h] [ebp-64h]
    float v10; // [esp+18h] [ebp-60h]
    float v11; // [esp+1Ch] [ebp-5Ch]
    float v12; // [esp+20h] [ebp-58h]
    float v13; // [esp+24h] [ebp-54h]
    float v14; // [esp+28h] [ebp-50h]
    float v15; // [esp+2Ch] [ebp-4Ch]
    float v16; // [esp+34h] [ebp-44h]
    float v17; // [esp+3Ch] [ebp-3Ch]
    float v18; // [esp+44h] [ebp-34h]
    float centerDelta[3]; // [esp+60h] [ebp-18h] BYREF
    float halfBoxSize[3]; // [esp+6Ch] [ebp-Ch] BYREF

    Vec3Sub(tw->midpoint, origin, centerDelta);
    Vec3Add(halfSize, tw->size, halfBoxSize);
    v15 = I_fabs(centerDelta[0]);
    v14 = halfBoxSize[0] + tw->halfDeltaAbs[0];
    if (v15 > (double)v14)
        return 1;
    v13 = I_fabs(centerDelta[1]);
    v12 = halfBoxSize[1] + tw->halfDeltaAbs[1];
    if (v13 > (double)v12)
        return 1;
    v11 = I_fabs(centerDelta[2]);
    v10 = halfBoxSize[2] + tw->halfDeltaAbs[2];
    if (v11 > (double)v10)
        return 1;
    if (tw->axialCullOnly)
        return 0;
    v18 = centerDelta[2] * tw->halfDelta[1] - centerDelta[1] * tw->halfDelta[2];
    v9 = I_fabs(v18);
    v8 = halfBoxSize[1] * tw->halfDeltaAbs[2] + halfBoxSize[2] * tw->halfDeltaAbs[1];
    if (v9 > (double)v8)
        return 1;
    v17 = centerDelta[0] * tw->halfDelta[2] - centerDelta[2] * tw->halfDelta[0];
    v7 = I_fabs(v17);
    v6 = halfBoxSize[2] * tw->halfDeltaAbs[0] + halfBoxSize[0] * tw->halfDeltaAbs[2];
    if (v7 > (double)v6)
        return 1;
    v16 = centerDelta[1] * tw->halfDelta[0] - centerDelta[0] * tw->halfDelta[1];
    v5 = I_fabs(v16);
    v4 = halfBoxSize[0] * tw->halfDeltaAbs[1] + halfBoxSize[1] * tw->halfDeltaAbs[0];
    return v5 > (double)v4;
}

__attribute__((noinline)) int RefBoxOnPlaneSide(const float *emins, const float *emaxs, const Plane *p)
{
    float v3;
    float v4;

    switch (p->signbits)
    {
    case 0:
        v3 = (p->normal[0] * emaxs[0]) + (emaxs[1] * p->normal[1]) + (emaxs[2] * p->normal[2]);
        v4 = (p->normal[0] * emins[0]) + (emins[1] * p->normal[1]) + (emins[2] * p->normal[2]);
        break;
    case 1:
        v3 = (p->normal[0] * emins[0]) + (emaxs[1] * p->normal[1]) + (emaxs[2] * p->normal[2]);
        v4 = (p->normal[0] * emaxs[0]) + (emins[1] * p->normal[1]) + (emins[2] * p->normal[2]);
        break;
    case 2:
        v3 = (p->normal[0] * emaxs[0]) + (emaxs[2] * p->normal[2]) + (emins[1] * p->normal[1]);
        v4 = (p->normal[0] * emins[0]) + (emins[2] * p->normal[2]) + (emaxs[1] * p->normal[1]);
        break;
    case 3:
        v3 = (p->normal[0] * emins[0]) + (emaxs[2] * p->normal[2]) + (emins[1] * p->normal[1]);
        v4 = (p->normal[0] * emaxs[0]) + (emins[2] * p->normal[2]) + (emaxs[1] * p->normal[1]);
        break;
    case 4:
        v3 = (p->normal[0] * emaxs[0]) + (emaxs[1] * p->normal[1]) + (emins[2] * p->normal[2]);
        v4 = (p->normal[0] * emins[0]) + (emins[1] * p->normal[1]) + (emaxs[2] * p->normal[2]);
        break;
    case 5:
        v3 = (p->normal[0] * emins[0]) + (emaxs[1] * p->normal[1]) + (emins[2] * p->normal[2]);
        v4 = (p->normal[0] * emaxs[0]) + (emins[1] * p->normal[1]) + (emaxs[2] * p->normal[2]);
        break;
    case 6:
        v3 = (p->normal[0] * emaxs[0]) + (emins[1] * p->normal[1]) + (emins[2] * p->normal[2]);
        v4 = (p->normal[0] * emins[0]) + (emaxs[1] * p->normal[1]) + (emaxs[2] * p->normal[2]);
        break;
    case 7:
        v3 = (p->normal[0] * emins[0]) + (emins[1] * p->normal[1]) + (emins[2] * p->normal[2]);
        v4 = (p->normal[0] * emaxs[0]) + (emaxs[1] * p->normal[1]) + (emaxs[2] * p->normal[2]);
        break;
    default:
        return -1; // retail: assert + __debugbreak; never generated here
    }

    return (2 * (v4 < p->dist)) | (v3 > p->dist);
}

__attribute__((noinline)) uint32_t RefFabsBits(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    f = Q_fabs(f);
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// The retail BG_EvaluateTrajectory switch: which trTypes copy trBase.
__attribute__((noinline)) bool RefIsBaseCopy(int32_t trType)
{
    switch (trType)
    {
    case 0x0: // TR_STATIONARY
    case 0x1: // TR_INTERPOLATE
    case 0x8: // TR_PHYSICS
    case 0xB: // TR_RAGDOLL_INTERPOLATE
        return true;
    default:
        return false;
    }
}

// ---- new code under test (the engine's inline helpers, inlined here into
// an out-of-line wrapper like into their engine callers).
__attribute__((noinline)) bool NewCullBox(const TraceWork *tw, const float *origin, const float *halfSize)
{
    return CM_CullBoxInline(tw, origin, halfSize);
}

static int InvalidSide(const float *, const float *, const Plane *)
{
    return -1;
}

__attribute__((noinline)) int NewBoxOnPlaneSide(const float *emins, const float *emaxs, const Plane *p)
{
    return BoxOnPlaneSideInline(emins, emaxs, p, InvalidSide);
}

__attribute__((noinline)) uint32_t NewFabsBits(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    f = CM_CullFabs(f);
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

__attribute__((noinline)) bool NewIsBaseCopy(int32_t trType)
{
    return BG_TrajectoryIsBaseCopy(trType);
}
} // namespace cmcull
#endif // CM_CULL_BUILD_UNITS

#ifdef CM_CULL_BUILD_DRIVER
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>

namespace
{
using namespace cmcull;

struct Rng
{
    std::mt19937 gen;
    explicit Rng(uint32_t seed) : gen(seed) {}
    uint32_t U() { return gen(); }
    float Uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(gen); }
    float Bits()
    {
        uint32_t b = gen();
        float f;
        std::memcpy(&f, &b, sizeof(f));
        return f;
    }
};

float NaNWithPayload(uint32_t payload, bool negative, bool signaling)
{
    uint32_t b = 0x7F800000u | (payload & 0x3FFFFFu) | (signaling ? 0u : 0x400000u);
    if (signaling && (b & 0x3FFFFFu) == 0)
        b |= 1;
    if (negative)
        b |= 0x80000000u;
    float f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

// A world-ish value: exact small integers (ties), map-scale floats, tiny
// values, raw bit patterns and IEEE specials.
float Value(Rng &r, float scale)
{
    switch (r.U() % 40)
    {
    case 0:
        return NaNWithPayload(r.U(), r.U() & 1, r.U() & 1);
    case 1:
        return std::numeric_limits<float>::infinity();
    case 2:
        return -std::numeric_limits<float>::infinity();
    case 3:
        return 0.0f;
    case 4:
        return -0.0f;
    case 5:
        return FLT_MAX;
    case 6:
        return -FLT_MAX;
    case 7:
        return 1.0e-40f; // denormal
    case 8:
        return -1.0e-40f;
    case 9:
        return FLT_MIN;
    case 10:
        return r.Bits();
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
        return (float)((int)(r.U() % 17) - 8); // exact: equal sums / ties
    case 17:
    case 18:
        return r.Uniform(-1.0e-3f, 1.0e-3f);
    case 19:
        return r.Uniform(-3.0e38f, 3.0e38f); // overflow on add / multiply
    default:
        return r.Uniform(-scale, scale);
    }
}

float NonNegValue(Rng &r, float scale)
{
    float v = Value(r, scale);
    return (r.U() % 8) ? std::fabs(v) : v; // mostly the engine's >= 0 sizes
}

int failures = 0;

void Fail(const char *what, uint64_t i)
{
    if (failures++ < 20)
        std::printf("FAIL:CM_CULL %s mismatch at case %llu\n", what, (unsigned long long)i);
}

// Which retail test exits (coverage only): 0-2 axial cull, 3 axialCullOnly
// keep, 4-6 cross cull, 7 final keep, 8 = NaN-driven or overflow cases
// counted by axis-0 comparison falling through.
int CullStage(const TraceWork *tw, const float *origin, const float *halfSize)
{
    float cd[3], hb[3];
    for (int i = 0; i < 3; ++i)
    {
        cd[i] = tw->midpoint[i] - origin[i];
        hb[i] = halfSize[i] + tw->size[i];
    }
    for (int i = 0; i < 3; ++i)
        if (std::fabs(cd[i]) > hb[i] + tw->halfDeltaAbs[i])
            return i;
    if (tw->axialCullOnly)
        return 3;
    float c0 = cd[2] * tw->halfDelta[1] - cd[1] * tw->halfDelta[2];
    if (std::fabs(c0) > hb[1] * tw->halfDeltaAbs[2] + hb[2] * tw->halfDeltaAbs[1])
        return 4;
    float c1 = cd[0] * tw->halfDelta[2] - cd[2] * tw->halfDelta[0];
    if (std::fabs(c1) > hb[2] * tw->halfDeltaAbs[0] + hb[0] * tw->halfDeltaAbs[2])
        return 5;
    float c2 = cd[1] * tw->halfDelta[0] - cd[0] * tw->halfDelta[1];
    if (std::fabs(c2) > hb[0] * tw->halfDeltaAbs[1] + hb[1] * tw->halfDeltaAbs[0])
        return 6;
    return 7;
}

void CheckCullBox(uint32_t seed, uint64_t count)
{
    Rng r(seed);
    uint64_t stages[8] = {}, culled = 0, nanInputs = 0;
    for (uint64_t i = 0; i < count; ++i)
    {
        TraceWork tw;
        float origin[3], halfSize[3];
        // Mostly engine-shaped traces (halfDeltaAbs = |halfDelta|, sizes >= 0,
        // boxes near the segment so every axis decides some cases), plus
        // fully arbitrary inputs.
        const bool shaped = (r.U() % 4) != 0;
        const float scale = (r.U() % 2) ? 64.0f : 4096.0f;
        for (int k = 0; k < 3; ++k)
        {
            tw.midpoint[k] = Value(r, scale);
            tw.halfDelta[k] = Value(r, scale);
            tw.halfDeltaAbs[k] = shaped ? std::fabs(tw.halfDelta[k]) : Value(r, scale);
            tw.size[k] = shaped ? NonNegValue(r, 32.0f) : Value(r, scale);
            halfSize[k] = shaped ? NonNegValue(r, scale * 0.5f) : Value(r, scale);
            origin[k] = shaped && (r.U() % 2) ? tw.midpoint[k] + r.Uniform(-2.0f, 2.0f) * (halfSize[k] + tw.size[k] + tw.halfDeltaAbs[k])
                                              : Value(r, scale);
        }
        // Exact-tie axis: |centerDelta| == halfBox + halfDeltaAbs.
        if ((r.U() % 8) == 0)
        {
            int k = r.U() % 3;
            tw.midpoint[k] = (float)(int)(r.U() % 64);
            halfSize[k] = (float)(int)(r.U() % 16);
            tw.size[k] = (float)(int)(r.U() % 8);
            tw.halfDeltaAbs[k] = (float)(int)(r.U() % 8);
            origin[k] = tw.midpoint[k] - (halfSize[k] + tw.size[k] + tw.halfDeltaAbs[k]) * ((r.U() & 1) ? 1.0f : -1.0f);
        }
        tw.axialCullOnly = (r.U() % 4) == 0;
        for (int k = 0; k < 3; ++k)
            if (std::isnan(tw.midpoint[k]) || std::isnan(origin[k]) || std::isnan(halfSize[k]) || std::isnan(tw.size[k]) ||
                std::isnan(tw.halfDelta[k]) || std::isnan(tw.halfDeltaAbs[k]))
            {
                ++nanInputs;
                break;
            }
        const bool ref = RefCullBox(&tw, origin, halfSize);
        const bool got = NewCullBox(&tw, origin, halfSize);
        if (ref != got)
            Fail("CM_CullBox", i);
        culled += ref;
        ++stages[CullStage(&tw, origin, halfSize)];
    }
    std::printf("cm_cull: CM_CullBox %llu cases (culled %llu, NaN inputs %llu), exits axis0 %llu axis1 %llu axis2 %llu "
                "axial-only %llu cross0 %llu cross1 %llu cross2 %llu kept %llu\n",
                (unsigned long long)count, (unsigned long long)culled, (unsigned long long)nanInputs,
                (unsigned long long)stages[0], (unsigned long long)stages[1], (unsigned long long)stages[2],
                (unsigned long long)stages[3], (unsigned long long)stages[4], (unsigned long long)stages[5],
                (unsigned long long)stages[6], (unsigned long long)stages[7]);
    for (int s = 0; s < 8; ++s)
        if (stages[s] < count / 1000)
        {
            std::printf("FAIL:CM_CULL CM_CullBox exit %d covered only %llu times\n", s, (unsigned long long)stages[s]);
            ++failures;
        }
}

void CheckBoxOnPlaneSide(uint32_t seed, uint64_t count)
{
    Rng r(seed);
    uint64_t sides[4] = {};
    for (uint64_t i = 0; i < count; ++i)
    {
        float mins[3], maxs[3];
        Plane p = {};
        const float scale = (r.U() % 2) ? 64.0f : 8192.0f;
        const bool shaped = (r.U() % 4) != 0;
        for (int k = 0; k < 3; ++k)
        {
            mins[k] = Value(r, scale);
            maxs[k] = shaped ? mins[k] + NonNegValue(r, scale * 0.25f) : Value(r, scale);
            p.normal[k] = shaped ? r.Uniform(-1.0f, 1.0f) : Value(r, 2.0f);
        }
        if (shaped && (r.U() % 3) == 0)
        {
            const int axis = r.U() % 3; // axial plane like most of the dpvs tree
            p.normal[0] = p.normal[1] = p.normal[2] = 0.0f;
            p.normal[axis] = (r.U() & 1) ? 1.0f : -1.0f;
        }
        p.signbits = 0;
        for (int k = 0; k < 3; ++k)
            if (std::signbit(p.normal[k]))
                p.signbits |= 1 << k;
        if (!shaped)
            p.signbits = r.U() % 8; // any case vs any normal
        p.dist = (r.U() % 3) ? r.Uniform(-scale, scale) : Value(r, scale);
        if ((r.U() % 6) == 0) // dist exactly on a box face term
            p.dist = p.normal[0] * mins[0] + mins[1] * p.normal[1] + mins[2] * p.normal[2];
        const int ref = RefBoxOnPlaneSide(mins, maxs, &p);
        const int got = NewBoxOnPlaneSide(mins, maxs, &p);
        if (ref != got)
            Fail("BoxOnPlaneSide", i);
        if (ref >= 0 && ref < 4)
            ++sides[ref];
    }
    std::printf("cm_cull: BoxOnPlaneSide %llu cases, side0 %llu front %llu back %llu both %llu\n",
                (unsigned long long)count, (unsigned long long)sides[0], (unsigned long long)sides[1],
                (unsigned long long)sides[2], (unsigned long long)sides[3]);
    for (int s = 0; s < 4; ++s)
        if (!sides[s])
        {
            std::printf("FAIL:CM_CULL BoxOnPlaneSide side %d never produced\n", s);
            ++failures;
        }
}

void CheckFabsExhaustive(uint32_t stride)
{
    uint64_t checked = 0;
    for (uint64_t b = 0; b <= 0xFFFFFFFFull; b += stride)
    {
        if (NewFabsBits((uint32_t)b) != RefFabsBits((uint32_t)b))
            Fail("Q_fabs", b);
        ++checked;
    }
    std::printf("cm_cull: Q_fabs %llu bit patterns (stride %u)\n", (unsigned long long)checked, stride);
}

void CheckTrajectoryTypes()
{
    uint64_t checked = 0;
    for (int64_t t = -4096; t <= 4096; ++t, ++checked)
        if (NewIsBaseCopy((int32_t)t) != RefIsBaseCopy((int32_t)t))
            Fail("BG_TrajectoryIsBaseCopy", (uint64_t)t);
    const int32_t edges[] = {INT32_MIN, INT32_MIN + 1, -33, -32, -31, 31, 32, 33, 63, 64, INT32_MAX - 1, INT32_MAX};
    for (int32_t t : edges)
    {
        ++checked;
        if (NewIsBaseCopy(t) != RefIsBaseCopy(t))
            Fail("BG_TrajectoryIsBaseCopy", (uint64_t)(uint32_t)t);
    }
    std::printf("cm_cull: BG_TrajectoryIsBaseCopy %llu trTypes\n", (unsigned long long)checked);
}
} // namespace

int main(int argc, char **argv)
{
    // argv[1]: case-count scale (qemu runs use fewer). argv[2]: Q_fabs stride
    // (1 = all 2^32 patterns).
    const uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2000000;
    const uint32_t fabsStride = argc > 2 ? (uint32_t)std::strtoul(argv[2], nullptr, 10) : 1;
    CheckCullBox(0xC011B0Au, n);
    CheckBoxOnPlaneSide(0xB0B5u, n);
    CheckFabsExhaustive(fabsStride ? fabsStride : 1);
    CheckTrajectoryTypes();
    if (failures)
    {
        std::printf("FAIL:CM_CULL %d mismatches\n", failures);
        return 1;
    }
    std::printf("PASS:CM_CULL\n");
    return 0;
}
#endif // CM_CULL_BUILD_DRIVER
