#pragma once

#include <math.h>

// Hot box-culling helpers, inlined into their callers (CM_TraceThroughAabbTree_r,
// CM_PositionTestInAabbTree_r, R_FilterEntIntoCells_r). The retail functions
// called out-of-line Vec3Sub/Vec3Add/Q_fabs and BoxOnPlaneSide per node;
// both run tens of millions of times per profiled session.
//
// Both are templates over the struct type so switch_cm_cull_test.cpp can run
// them against verbatim copies of the retail code without the engine headers.
// Every expression keeps the retail operand order and grouping, so the
// results (and the compiler's FMA contraction on AArch64) are the same bit
// for bit; the test checks this under x86 sanitizers and under qemu-aarch64
// with the Switch compiler.

// |f| by clearing the sign bit: identical to the retail Q_fabs integer mask
// for every input, NaN payloads included (FABS on AArch64, ANDPS on x86).
inline float CM_CullFabs(float f)
{
    return fabsf(f);
}

// CM_CullBox: segment-swept box vs AABB separating-axis test. true = culled.
// Axis 0 is computed and tested first: most calls exit there, so the
// remaining axes are only computed when needed (the retail code computed the
// full centre delta and half box via two calls first).
template <typename TraceWork>
inline bool CM_CullBoxInline(const TraceWork *tw, const float *origin, const float *halfSize)
{
    const float centerDelta0 = tw->midpoint[0] - origin[0];
    const float halfBoxSize0 = halfSize[0] + tw->size[0];
    if (CM_CullFabs(centerDelta0) > halfBoxSize0 + tw->halfDeltaAbs[0])
        return true;
    const float centerDelta1 = tw->midpoint[1] - origin[1];
    const float halfBoxSize1 = halfSize[1] + tw->size[1];
    if (CM_CullFabs(centerDelta1) > halfBoxSize1 + tw->halfDeltaAbs[1])
        return true;
    const float centerDelta2 = tw->midpoint[2] - origin[2];
    const float halfBoxSize2 = halfSize[2] + tw->size[2];
    if (CM_CullFabs(centerDelta2) > halfBoxSize2 + tw->halfDeltaAbs[2])
        return true;
    if (tw->axialCullOnly)
        return false;
    const float cross0 = centerDelta2 * tw->halfDelta[1] - centerDelta1 * tw->halfDelta[2];
    const float extent0 = halfBoxSize1 * tw->halfDeltaAbs[2] + halfBoxSize2 * tw->halfDeltaAbs[1];
    if (CM_CullFabs(cross0) > extent0)
        return true;
    const float cross1 = centerDelta0 * tw->halfDelta[2] - centerDelta2 * tw->halfDelta[0];
    const float extent1 = halfBoxSize2 * tw->halfDeltaAbs[0] + halfBoxSize0 * tw->halfDeltaAbs[2];
    if (CM_CullFabs(cross1) > extent1)
        return true;
    const float cross2 = centerDelta1 * tw->halfDelta[0] - centerDelta0 * tw->halfDelta[1];
    const float extent2 = halfBoxSize0 * tw->halfDeltaAbs[1] + halfBoxSize1 * tw->halfDeltaAbs[0];
    return CM_CullFabs(cross2) > extent2;
}

// BoxOnPlaneSide: 1 = front, 2 = back, 3 = both. Per-signbits sums keep the
// retail term order (cases 2 and 3 add x, z, y). signbits > 7 is invalid and
// goes to `invalid` (the out-of-line BoxOnPlaneSide asserts and breaks).
template <typename Plane, typename Invalid>
inline int BoxOnPlaneSideInline(const float *emins, const float *emaxs, const Plane *p, Invalid invalid)
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
        return invalid(emins, emaxs, p);
    }

    return (2 * (v4 < p->dist)) | (v3 > p->dist);
}
