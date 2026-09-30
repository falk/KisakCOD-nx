#pragma once

// XModelGetStaticBounds' corner loop (xmodel.cpp), split out so
// switch_xmodel_static_bounds_test can run the NEON form against the retail
// scalar form (x86 host and qemu-aarch64).
//
// G_MoverPush -> G_GetModelBounds -> XModelGetStaticBounds is a measurable
// share of main-thread samples (every pushed entity, every frame). The
// retail loop calls MatrixTransformVector out of line for each of 8 corners
// per collision surface (12 fcvt + 3 fmul + 6 fmadd + 3 fcvt) and then does
// six branchy compares.
//
// Exactness: MatrixTransformVector sums float products in double. A
// float*float product is exact in double (48 significant bits, no overflow
// or underflow), so fma(a, b, t) == a*b + t there, and the only roundings
// are the two double adds (p1 + p0, then + p2) and the final narrowing --
// the same whichever way the compiler fuses. The NEON form keeps that order
// and the retail compare-and-replace (strict <, >) per corner, in corner
// order, so NaNs and signed zeros resolve exactly as before.

#include <cfloat>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

// Retail loop body (was inline in XModelGetStaticBounds; the transform is
// MatrixTransformVector's expression).
template <typename Surf>
inline void XModelStaticBoundsScalar(const Surf *surfs, int count, const float (&axis)[3][3], float *mins, float *maxs)
{
    mins[0] = FLT_MAX;
    mins[1] = FLT_MAX;
    mins[2] = FLT_MAX;
    maxs[1] = -FLT_MAX;
    maxs[2] = -FLT_MAX;
    maxs[0] = -FLT_MAX;
    for (int i = 0; i < count; ++i)
    {
        const Surf *csurf = &surfs[i];
        for (int k = 0; k < 8; ++k)
        {
            float corner[3];
            corner[0] = (k & 1) ? csurf->mins[0] : csurf->maxs[0];
            corner[1] = (k & 2) ? csurf->mins[1] : csurf->maxs[1];
            corner[2] = (k & 4) ? csurf->mins[2] : csurf->maxs[2];
            float rotated[3];
            for (int j = 0; j < 3; ++j)
                rotated[j] = (float)((double)corner[0] * (double)axis[0][j] + (double)corner[1] * (double)axis[1][j] +
                                     (double)corner[2] * (double)axis[2][j]);
            for (int j = 0; j < 3; ++j)
            {
                if (rotated[j] < (double)mins[j])
                    mins[j] = rotated[j];
                if (rotated[j] > (double)maxs[j])
                    maxs[j] = rotated[j];
            }
        }
    }
}

#if defined(__aarch64__)
// Lanes: (x, y) in one float64x2, (z, z) in another; the rotated corner is
// narrowed into one float32x4 (x, y, z, z) and min/max are kept in vector
// registers for the whole model. Per corner: 6 FP64 multiply-adds, 2
// narrows, 2 compares, 2 selects -- no calls, no branches.
template <typename Surf>
inline void XModelStaticBoundsNeon(const Surf *surfs, int count, const float (&axis)[3][3], float *mins, float *maxs)
{
    const float64x2_t r0 = vcvt_f64_f32(vld1_f32(&axis[0][0]));
    const float64x2_t r1 = vcvt_f64_f32(vld1_f32(&axis[1][0]));
    const float64x2_t r2 = vcvt_f64_f32(vld1_f32(&axis[2][0]));
    const float64x2_t z0 = vdupq_n_f64((double)axis[0][2]);
    const float64x2_t z1 = vdupq_n_f64((double)axis[1][2]);
    const float64x2_t z2 = vdupq_n_f64((double)axis[2][2]);
    float32x4_t vmin = vdupq_n_f32(FLT_MAX);
    float32x4_t vmax = vdupq_n_f32(-FLT_MAX);
    for (int i = 0; i < count; ++i)
    {
        const Surf *csurf = &surfs[i];
        const double cx[2] = {(double)csurf->maxs[0], (double)csurf->mins[0]};
        const double cy[2] = {(double)csurf->maxs[1], (double)csurf->mins[1]};
        const double cz[2] = {(double)csurf->maxs[2], (double)csurf->mins[2]};
#pragma GCC unroll 8
        for (int k = 0; k < 8; ++k)
        {
            const double x = cx[k & 1], y = cy[(k >> 1) & 1], z = cz[(k >> 2) & 1];
            // (p0 + p1) + p2 with exact products: p1 first, p0 fused in,
            // then p2 -- the order GCC emits for MatrixTransformVector.
            const float64x2_t xy = vfmaq_n_f64(vfmaq_n_f64(vmulq_n_f64(r1, y), r0, x), r2, z);
            const float64x2_t zz = vfmaq_n_f64(vfmaq_n_f64(vmulq_n_f64(z1, y), z0, x), z2, z);
            const float32x4_t rot = vcvt_high_f32_f64(vcvt_f32_f64(xy), zz);
            vmin = vbslq_f32(vcltq_f32(rot, vmin), rot, vmin);
            vmax = vbslq_f32(vcgtq_f32(rot, vmax), rot, vmax);
        }
    }
    mins[0] = vgetq_lane_f32(vmin, 0);
    mins[1] = vgetq_lane_f32(vmin, 1);
    mins[2] = vgetq_lane_f32(vmin, 2);
    maxs[0] = vgetq_lane_f32(vmax, 0);
    maxs[1] = vgetq_lane_f32(vmax, 1);
    maxs[2] = vgetq_lane_f32(vmax, 2);
}
#endif

template <typename Surf>
inline void XModelStaticBounds(const Surf *surfs, int count, const float (&axis)[3][3], float *mins, float *maxs)
{
#if defined(__aarch64__) && !defined(KISAK_XMODEL_BOUNDS_SCALAR)
    XModelStaticBoundsNeon(surfs, count, axis, mins, maxs);
#else
    XModelStaticBoundsScalar(surfs, count, axis, mins, maxs);
#endif
}
