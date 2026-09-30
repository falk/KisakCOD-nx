// Portable-SIMD port of r_model_skin_sse.cpp's vertex skinning, for targets
// without <xmmintrin.h>/<mmintrin.h> (AArch64 Switch).
//
// Written with GCC vector extensions, which lower to NEON on AArch64 and to
// SSE on x86, so the same source runs in switch_model_skin_simd_test against
// the real SSE implementation on the host. Every step mirrors the SSE file in
// operation order: position = (a0*x + a1*y) + (a2*z + origin), packed unit
// vectors decode as (b - shift) / scale scaled by the decoded w, normals are
// re-encoded as t * scale + shift, rounded to nearest-even and saturated to
// bytes (what cvt_ps2pi + packuswb do for in-range values), and the weighted
// path rebuilds the position's w as a clean +/-1 binormal sign.
//
// Before this, the Switch build fell back to the scalar
// R_SkinXSurfaceSkinned, which cost a substantial share of a worker core
// per frame in XModel skinning (switch_perfTrace xskin).

#include "r_model_skin_simd.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace
{
typedef float v4f __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));
typedef uint32_t v4u __attribute__((vector_size(16)));

const v4f kWeightScale = {1.0f / 65536.0f, 1.0f / 65536.0f, 1.0f / 65536.0f, 1.0f / 65536.0f};
const v4f kEncodeShift = {127.0f, 127.0f, 127.0f, -192.0f};
const v4f kEncodeScale = {127.0f, 127.0f, 127.0f, 255.0f};

inline v4f Load(const float *p)
{
    v4f v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline v4f Splat(float f)
{
    return v4f{f, f, f, f};
}

inline const DObjSkelMat *BoneAt(const DObjSkelMat *boneMatrix, unsigned byteOffset)
{
    return (const DObjSkelMat *)((const char *)boneMatrix + byteOffset);
}

// (x, y, z, w) -> (x, y, z, copysign(1, w)) [Black Ops binormal fix]
inline v4f LoadSkinPosition(v4f p)
{
    const v4u bits = (v4u)p;
    const uint32_t w = (bits[3] & 0x80000000u) | 0x3F800000u;
    float wf;
    std::memcpy(&wf, &w, sizeof(wf));
    v4f out = p;
    out[3] = wf;
    return out;
}

inline v4f TransformPoint(const DObjSkelMat *m, v4f p)
{
    return (Load(m->axis[0]) * Splat(p[0]) + Load(m->axis[1]) * Splat(p[1])) +
           (Load(m->axis[2]) * Splat(p[2]) + Load(m->origin));
}

inline v4f TransformDir(const DObjSkelMat *m, v4f d)
{
    return (Load(m->axis[0]) * Splat(d[0]) + Load(m->axis[1]) * Splat(d[1])) + Load(m->axis[2]) * Splat(d[2]);
}

inline v4f PackXyzW(v4f xyz, v4f wSource)
{
    return v4f{xyz[0], xyz[1], xyz[2], wSource[3]};
}

inline v4f DecodeUnitVec(uint32_t packed)
{
    const v4f bytes = {(float)(packed & 0xFF), (float)((packed >> 8) & 0xFF), (float)((packed >> 16) & 0xFF),
                       (float)(packed >> 24)};
    const v4f d = (bytes - kEncodeShift) / kEncodeScale;
    return d * Splat(d[3]);
}

inline v4f SkinUnitVec(const DObjSkelMat *m, uint32_t packed)
{
    const v4f transformed = TransformDir(m, DecodeUnitVec(packed));
    return PackXyzW(transformed, Load(m->origin)) * kEncodeScale + kEncodeShift;
}

inline v4f DecodeWeight(uint16_t weight)
{
    return Splat((float)weight) * kWeightScale;
}

inline uint8_t PackByte(float f)
{
    const float r = std::nearbyint(f); // round-to-nearest-even, as cvt_ps2pi
    return r <= 0.0f ? 0 : r >= 255.0f ? 255 : (uint8_t)r;
}

inline uint64_t PackNormalTangent(v4f n, v4f t)
{
#if defined(__aarch64__)
    // Round to nearest-even, then saturate int32 -> u16 -> u8: the same
    // clamp(rint(x), 0, 255) as PackByte (and cvt_ps2pi + packuswb) for the
    // encoded range, in three vector instructions instead of eight scalar
    // round-and-branch sequences.
    const int32x4_t ni = vcvtnq_s32_f32((float32x4_t)n);
    const int32x4_t ti = vcvtnq_s32_f32((float32x4_t)t);
    const uint8x8_t bytes = vqmovn_u16(vcombine_u16(vqmovun_s32(ni), vqmovun_s32(ti)));
    return vget_lane_u64(vreinterpret_u64_u8(bytes), 0);
#else
    const uint8_t b[8] = {PackByte(n[0]), PackByte(n[1]), PackByte(n[2]), PackByte(n[3]),
                          PackByte(t[0]), PackByte(t[1]), PackByte(t[2]), PackByte(t[3])};
    uint64_t packed;
    std::memcpy(&packed, b, sizeof(packed));
    return packed;
#endif
}

inline void Store(GfxPackedVertex *dst, v4f pos, const GfxPackedVertex *src, uint64_t normalTangent)
{
    std::memcpy(dst, &pos, 16);
    std::memcpy((char *)dst + 16, (const char *)src + 16, 8); // color + texCoord
    std::memcpy((char *)dst + 24, &normalTangent, 8);
}

inline uint32_t Word(const GfxPackedVertex *v, int index)
{
    uint32_t w;
    std::memcpy(&w, (const char *)v + 4 * index, sizeof(w));
    return w;
}

template <int numWeights> inline v4f BlendPosition(const GfxPackedVertex *src, const uint16_t *blend,
                                                   const DObjSkelMat *boneMatrix, const DObjSkelMat **bone0Out)
{
    const v4f position = LoadSkinPosition(Load(src->xyz));
    const DObjSkelMat *bone0 = BoneAt(boneMatrix, blend[0]);
    const v4f pos0 = PackXyzW(TransformPoint(bone0, position), position);
    v4f outPos = pos0;
    for (int w = 1; w <= numWeights; ++w)
    {
        const DObjSkelMat *bone = BoneAt(boneMatrix, blend[2 * w - 1]);
        const v4f bonePos = PackXyzW(TransformPoint(bone, position), position);
        outPos = outPos + DecodeWeight(blend[2 * w]) * (bonePos - pos0);
    }
    *bone0Out = bone0;
    return outPos;
}

template <int numWeights>
void SkinWeightBlock(const GfxPackedVertex *srcVerts, const uint16_t *blend, int vertCount,
                     const DObjSkelMat *boneMatrix, GfxPackedVertex *dstVerts, GfxPackedVertexNormal *dstNormals,
                     const GfxPackedVertexNormal *srcNormals, int *pVertexIndex)
{
    int vertIndex = *pVertexIndex;
    for (int i = 0; i < vertCount; ++i, ++vertIndex, blend += 2 * numWeights + 1)
    {
        const GfxPackedVertex *src = &srcVerts[vertIndex];
        const DObjSkelMat *bone0;
        const v4f outPos = BlendPosition<numWeights>(src, blend, boneMatrix, &bone0);
        uint64_t normalTangent;
        if (srcNormals)
            std::memcpy(&normalTangent, &srcNormals[vertIndex], sizeof(normalTangent));
        else
            normalTangent = PackNormalTangent(SkinUnitVec(bone0, Word(src, 6)), SkinUnitVec(bone0, Word(src, 7)));
        Store(&dstVerts[vertIndex], outPos, src, normalTangent);
        if (dstNormals)
            std::memcpy(&dstNormals[vertIndex], &normalTangent, sizeof(normalTangent));
    }
    *pVertexIndex = vertIndex;
}

void SkinWeight(const GfxPackedVertex *inVerts, const XSurfaceVertexInfo *vertexInfo, const DObjSkelMat *boneMatrix,
                GfxPackedVertex *outVerts, GfxPackedVertexNormal *outNormals, const GfxPackedVertexNormal *inNormals)
{
    int vertIndex = 0;
    const uint16_t *blend = vertexInfo->vertsBlend;
    if (vertexInfo->vertCount[0])
    {
        SkinWeightBlock<0>(inVerts, blend, vertexInfo->vertCount[0], boneMatrix, outVerts, outNormals, inNormals,
                           &vertIndex);
        blend += vertexInfo->vertCount[0];
    }
    if (vertexInfo->vertCount[1])
    {
        SkinWeightBlock<1>(inVerts, blend, vertexInfo->vertCount[1], boneMatrix, outVerts, outNormals, inNormals,
                           &vertIndex);
        blend += 3 * vertexInfo->vertCount[1];
    }
    if (vertexInfo->vertCount[2])
    {
        SkinWeightBlock<2>(inVerts, blend, vertexInfo->vertCount[2], boneMatrix, outVerts, outNormals, inNormals,
                           &vertIndex);
        blend += 5 * vertexInfo->vertCount[2];
    }
    if (vertexInfo->vertCount[3])
        SkinWeightBlock<3>(inVerts, blend, vertexInfo->vertCount[3], boneMatrix, outVerts, outNormals, inNormals,
                           &vertIndex);
}

// Rigid: one bone per vertex list, raw binormal sign (matches retail).
void SkinRigid(const XSurface *surf, const DObjSkelMat *boneMatrix, GfxPackedVertex *dst,
               GfxPackedVertexNormal *dstNormals, const GfxPackedVertexNormal *srcNormals)
{
    const GfxPackedVertex *src = (const GfxPackedVertex *)surf->verts0;
    for (unsigned list = 0; list < surf->vertListCount; ++list)
    {
        const XRigidVertList *vertList = &surf->vertList[list];
        const DObjSkelMat *bone = BoneAt(boneMatrix, vertList->boneOffset);
        for (int i = 0; i < vertList->vertCount; ++i, ++src, ++dst)
        {
            __builtin_prefetch(&src[4]);
            const v4f srcPos = Load(src->xyz);
            const v4f outPos = PackXyzW(TransformPoint(bone, srcPos), srcPos);
            uint64_t normalTangent;
            if (srcNormals)
                std::memcpy(&normalTangent, srcNormals++, sizeof(normalTangent));
            else
                normalTangent = PackNormalTangent(SkinUnitVec(bone, Word(src, 6)), SkinUnitVec(bone, Word(src, 7)));
            Store(dst, outPos, src, normalTangent);
            if (dstNormals)
                std::memcpy(dstNormals++, &normalTangent, sizeof(normalTangent));
        }
    }
}
} // namespace

#if defined(__aarch64__) && !defined(KISAK_SKIN_SIMD_PORTABLE)
// Hand-scheduled NEON version of the portable path above, for the Switch's
// Cortex-A57. It produces the same bytes as the portable path does when GCC
// compiles it for AArch64 (-ffp-contract=fast fuses its a*b+c into fmla; the
// intrinsics below spell out exactly those fusions), which
// switch_model_skin_simd_test checks under qemu-aarch64 (./test host,
// model_skin_neon_check). What changed versus the portable lowering:
//  - the unit-vector decode's two Q-form FDIVs per vertex (unpipelined on
//    A57) become a reciprocal multiply plus one FMA correction step; for the
//    512 possible (byte - shift) / scale inputs this is the correctly rounded
//    quotient, which the test proves exhaustively (DecodeDivisionIsExact);
//  - the eight packed-byte extracts (ubfx + fmov + ins from general
//    registers) become one 8-byte widen (uxtl, uxtl, uxtl2, ucvtf);
//  - the rigid bone matrix stays in registers for a whole vertex list, the
//    vertex is loaded and stored as two Q registers, and the weighted path's
//    binormal-sign fix and per-bone w lane inserts are bitwise vector ops /
//    one final lane copy (the per-bone w lanes cancel exactly in the old
//    code: bonePos.w - pos0.w == 0, so outPos.w == position.w).
namespace neon
{
struct Bone
{
    float32x4_t a0, a1, a2, o;
};

inline Bone LoadBone(const DObjSkelMat *m)
{
    return {vld1q_f32(m->axis[0]), vld1q_f32(m->axis[1]), vld1q_f32(m->axis[2]), vld1q_f32(m->origin)};
}

// (a0*x + a1*y) + (a2*z + o), fused as GCC fuses the portable source.
inline float32x4_t XformPoint(const Bone &b, float32x4_t p)
{
    const float32x4_t t1 = vfmaq_laneq_f32(vmulq_laneq_f32(b.a1, p, 1), b.a0, p, 0);
    const float32x4_t t2 = vfmaq_laneq_f32(b.o, b.a2, p, 2);
    return vaddq_f32(t2, t1);
}

// (a0*x + a1*y) + a2*z, fused as GCC fuses the portable source.
inline float32x4_t XformDir(const Bone &b, float32x4_t d)
{
    return vfmaq_laneq_f32(vfmaq_laneq_f32(vmulq_laneq_f32(b.a1, d, 1), b.a0, d, 0), b.a2, d, 2);
}

struct Consts
{
    float32x4_t shift, scale, recip;
};

inline Consts MakeConsts()
{
    const float32x4_t scale = {127.0f, 127.0f, 127.0f, 255.0f};
    const float32x4_t one = vdupq_n_f32(1.0f);
    return {float32x4_t{127.0f, 127.0f, 127.0f, -192.0f}, scale, vdivq_f32(one, scale)};
}

// (bytes - shift) / scale, exactly (see DecodeDivisionIsExact in the test).
inline float32x4_t DivScale(const Consts &k, float32x4_t x)
{
    const float32x4_t q0 = vmulq_f32(x, k.recip);
    const float32x4_t e = vfmsq_f32(x, q0, k.scale);
    return vfmaq_f32(q0, e, k.recip);
}

inline float32x4_t DecodeScaled(const Consts &k, uint16x4_t b)
{
    const float32x4_t d = DivScale(k, vsubq_f32(vcvtq_f32_u32(vmovl_u16(b)), k.shift));
    return vmulq_laneq_f32(d, d, 3);
}

// Skinned normal+tangent from the vertex's upper 8 bytes, packed to bytes.
inline uint8x8_t SkinNormalTangent(const Consts &k, const Bone &b, uint8x8_t packed)
{
    const uint16x8_t wide = vmovl_u8(packed);
    const float32x4_t n = XformDir(b, DecodeScaled(k, vget_low_u16(wide)));
    const float32x4_t t = XformDir(b, DecodeScaled(k, vget_high_u16(wide)));
    const float32x4_t ne = vfmaq_f32(k.shift, vcopyq_laneq_f32(n, 3, b.o, 3), k.scale);
    const float32x4_t te = vfmaq_f32(k.shift, vcopyq_laneq_f32(t, 3, b.o, 3), k.scale);
    const uint16x8_t h = vcombine_u16(vqmovun_s32(vcvtnq_s32_f32(ne)), vqmovun_s32(vcvtnq_s32_f32(te)));
    return vqmovn_u16(h);
}

inline void StoreVertex(GfxPackedVertex *dst, float32x4_t pos, uint8x16_t hi, uint8x8_t normalTangent)
{
    vst1q_f32(dst->xyz, pos);
    vst1q_u8((uint8_t *)dst + 16, vcombine_u8(vget_low_u8(hi), normalTangent));
}

template <bool kSrcNormals, bool kDstNormals>
inline void RigidVert(const Consts &k, const Bone &b, const GfxPackedVertex *src, GfxPackedVertex *dst,
                      const GfxPackedVertexNormal *srcNormal, GfxPackedVertexNormal *dstNormal)
{
    const float32x4_t p = vld1q_f32(src->xyz);
    const uint8x16_t hi = vld1q_u8((const uint8_t *)src + 16);
    const float32x4_t pos = vcopyq_laneq_f32(XformPoint(b, p), 3, p, 3);
    const uint8x8_t nt = kSrcNormals ? vld1_u8((const uint8_t *)srcNormal) : SkinNormalTangent(k, b, vget_high_u8(hi));
    StoreVertex(dst, pos, hi, nt);
    if (kDstNormals)
        vst1_u8((uint8_t *)dstNormal, nt);
}

template <bool kSrcNormals, bool kDstNormals>
void SkinRigid(const XSurface *surf, const DObjSkelMat *boneMatrix, GfxPackedVertex *dst,
               GfxPackedVertexNormal *dstNormals, const GfxPackedVertexNormal *srcNormals)
{
    const Consts k = MakeConsts();
    const GfxPackedVertex *src = (const GfxPackedVertex *)surf->verts0;
    for (unsigned list = 0; list < surf->vertListCount; ++list)
    {
        const XRigidVertList *vertList = &surf->vertList[list];
        const Bone b = LoadBone(BoneAt(boneMatrix, vertList->boneOffset));
        int i = 0;
        const int count = vertList->vertCount;
        // Two vertices (one 64-byte line) per iteration, one prefetch per
        // line, four lines ahead of the load stream.
        for (; i + 2 <= count; i += 2, src += 2, dst += 2)
        {
            __builtin_prefetch(&src[8]);
            RigidVert<kSrcNormals, kDstNormals>(k, b, &src[0], &dst[0], srcNormals, dstNormals);
            RigidVert<kSrcNormals, kDstNormals>(k, b, &src[1], &dst[1], srcNormals + kSrcNormals,
                                                dstNormals + kDstNormals);
            srcNormals += 2 * kSrcNormals;
            dstNormals += 2 * kDstNormals;
        }
        if (i < count)
        {
            RigidVert<kSrcNormals, kDstNormals>(k, b, src, dst, srcNormals, dstNormals);
            ++src;
            ++dst;
            srcNormals += kSrcNormals;
            dstNormals += kDstNormals;
        }
    }
}

// (x, y, z, w) -> (x, y, z, copysign(1, w)) with two bitwise vector ops.
inline float32x4_t SkinPosition(float32x4_t p)
{
    const uint32x4_t keep = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x80000000u};
    const uint32x4_t one = {0u, 0u, 0u, 0x3F800000u};
    return vreinterpretq_f32_u32(vorrq_u32(vandq_u32(vreinterpretq_u32_f32(p), keep), one));
}

inline float DecodeWeight(uint16_t weight)
{
    return (float)weight * (1.0f / 65536.0f);
}

template <int numWeights, bool kSrcNormals, bool kDstNormals>
void SkinWeightBlock(const Consts &k, const GfxPackedVertex *src, const uint16_t *blend, int vertCount,
                     const DObjSkelMat *boneMatrix, GfxPackedVertex *dst, GfxPackedVertexNormal *dstNormals,
                     const GfxPackedVertexNormal *srcNormals)
{
    for (int i = 0; i < vertCount; ++i, blend += 2 * numWeights + 1)
    {
        __builtin_prefetch(&src[i + 4]);
        const float32x4_t position = SkinPosition(vld1q_f32(src[i].xyz));
        const uint8x16_t hi = vld1q_u8((const uint8_t *)&src[i] + 16);
        const Bone b0 = LoadBone(BoneAt(boneMatrix, blend[0]));
        // Lane 3 of pos0/outPos is don't-care until the final copy.
        const float32x4_t pos0 = XformPoint(b0, position);
        float32x4_t outPos = pos0;
#pragma GCC unroll 4
        for (int w = 1; w <= numWeights; ++w)
        {
            const Bone bw = LoadBone(BoneAt(boneMatrix, blend[2 * w - 1]));
            const float32x4_t diff = vsubq_f32(XformPoint(bw, position), pos0);
            outPos = vfmaq_n_f32(outPos, diff, DecodeWeight(blend[2 * w]));
        }
        outPos = vcopyq_laneq_f32(outPos, 3, position, 3);
        const uint8x8_t nt =
            kSrcNormals ? vld1_u8((const uint8_t *)&srcNormals[i]) : SkinNormalTangent(k, b0, vget_high_u8(hi));
        StoreVertex(&dst[i], outPos, hi, nt);
        if (kDstNormals)
            vst1_u8((uint8_t *)&dstNormals[i], nt);
    }
}

template <bool kSrcNormals, bool kDstNormals>
void SkinWeight(const GfxPackedVertex *verts, const XSurfaceVertexInfo *vertexInfo, const DObjSkelMat *boneMatrix,
                GfxPackedVertex *outVerts, GfxPackedVertexNormal *outNormals, const GfxPackedVertexNormal *inNormals)
{
    const Consts k = MakeConsts();
    int base = 0;
    const uint16_t *blend = vertexInfo->vertsBlend;
    const int n0 = vertexInfo->vertCount[0], n1 = vertexInfo->vertCount[1], n2 = vertexInfo->vertCount[2],
              n3 = vertexInfo->vertCount[3];
    // Same block walk as the portable SkinWeight (a zero count skips the
    // block, blend advances by the block's stride).
#define KISAK_SKIN_BLOCK(W, N)                                                                                         \
    if (N)                                                                                                             \
    {                                                                                                                  \
        if ((N) > 0)                                                                                                   \
        {                                                                                                              \
            SkinWeightBlock<W, kSrcNormals, kDstNormals>(k, verts + base, blend, (N), boneMatrix, outVerts + base,    \
                                                         kDstNormals ? outNormals + base : nullptr,                    \
                                                         kSrcNormals ? inNormals + base : nullptr);                    \
            base += (N);                                                                                               \
        }                                                                                                              \
        blend += (2 * (W) + 1) * (N);                                                                                  \
    }
    KISAK_SKIN_BLOCK(0, n0)
    KISAK_SKIN_BLOCK(1, n1)
    KISAK_SKIN_BLOCK(2, n2)
    KISAK_SKIN_BLOCK(3, n3)
#undef KISAK_SKIN_BLOCK
}

template <bool kSrcNormals, bool kDstNormals>
void Skin(const XSurface *xsurf, const DObjSkelMat *boneMatrix, const GfxPackedVertexNormal *in,
          GfxPackedVertexNormal *out, GfxPackedVertex *verts)
{
    if (xsurf->deformed)
        SkinWeight<kSrcNormals, kDstNormals>(xsurf->verts0, &xsurf->vertInfo, boneMatrix, verts, out, in);
    else
        SkinRigid<kSrcNormals, kDstNormals>(xsurf, boneMatrix, verts, out, in);
}
} // namespace neon
#endif

void R_SkinXSurfaceSkinnedSimd(const XSurface *xsurf, const DObjSkelMat *boneMatrix,
                               GfxPackedVertexNormal *skinVertNormalIn, GfxPackedVertexNormal *skinVertNormalOut,
                               GfxPackedVertex *skinVerticesOut)
{
    // Same dispatch as R_SkinXSurfaceSkinnedSse: a normal input stream is
    // only used together with an output stream.
    const GfxPackedVertexNormal *in = skinVertNormalOut ? skinVertNormalIn : nullptr;
#if defined(__aarch64__) && !defined(KISAK_SKIN_SIMD_PORTABLE)
    if (in)
        neon::Skin<true, true>(xsurf, boneMatrix, in, skinVertNormalOut, skinVerticesOut);
    else if (skinVertNormalOut)
        neon::Skin<false, true>(xsurf, boneMatrix, nullptr, skinVertNormalOut, skinVerticesOut);
    else
        neon::Skin<false, false>(xsurf, boneMatrix, nullptr, nullptr, skinVerticesOut);
    return;
#endif
    if (xsurf->deformed)
        SkinWeight(xsurf->verts0, &xsurf->vertInfo, boneMatrix, skinVerticesOut, skinVertNormalOut, in);
    else
        SkinRigid(xsurf, boneMatrix, skinVerticesOut, skinVertNormalOut, in);
}

#if defined(KISAK_SKIN_SIMD_TEST) && defined(KISAK_SKIN_SIMD_DECODE_HOOK)
// Test hook (switch_model_skin_simd_test, NEON run): the decoded unit vector
// before it is transformed, so the reciprocal decode is compared bit for bit
// instead of only after byte quantization (which hides 1-ulp differences).
void KISAK_SKIN_SIMD_DECODE_HOOK(uint32_t packed, float out[4])
{
#if defined(__aarch64__) && !defined(KISAK_SKIN_SIMD_PORTABLE)
    const uint16x8_t wide = vmovl_u8(vcreate_u8(packed));
    vst1q_f32(out, neon::DecodeScaled(neon::MakeConsts(), vget_low_u16(wide)));
#else
    const v4f d = DecodeUnitVec(packed);
    std::memcpy(out, &d, sizeof(d));
#endif
}
#endif

#if defined(__aarch64__) && !defined(KISAK_SKIN_SIMD_TEST)
// The engine's SIMD skinning entry point (r_model_skin.cpp, gated by
// sys_SSE && r_sse_skinning); r_model_skin_sse.cpp provides it on x86.
void __cdecl R_SkinXSurfaceSkinnedSse(const XSurface *xsurf, const DObjSkelMat *boneMatrix,
                                      GfxPackedVertexNormal *skinVertNormalIn,
                                      GfxPackedVertexNormal *skinVertNormalOut, GfxPackedVertex *skinVerticesOut)
{
    R_SkinXSurfaceSkinnedSimd(xsurf, boneMatrix, skinVertNormalIn, skinVertNormalOut, skinVerticesOut);
}
#endif
