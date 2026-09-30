#pragma once

// CM_AreaEntities_r building blocks (cm_world.cpp), split out so
// switch_cm_area_walk_test can run them against the retail scalar forms on
// the host and, for the NEON box test, under qemu-aarch64.
//
// CM_AreaEntities_r was a measurable share of main-thread samples, most of
// that on the first load from each candidate's gentity (r.contents): each
// candidate is a dependent cache miss behind the svEntity chain walk. The
// chain itself lives in the dense svEntities
// array, so it can run ahead: CM_WalkEntityChain gathers a batch of chain
// entries, prefetches every candidate's gentity bounds, then visits them in
// chain order, turning serialized misses into overlapped ones.

#include <cstdint>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

// The query box with a pad lane so both corners load as vectors.
struct CmAreaQueryBox
{
    float mins[4];
    float maxs[4];
};

inline CmAreaQueryBox CM_MakeAreaQueryBox(const float *mins, const float *maxs)
{
    return {{mins[0], mins[1], mins[2], 0.0f}, {maxs[0], maxs[1], maxs[2], 0.0f}};
}

// The retail test, maxs >= absmin && mins <= absmax on x, y and z (any NaN
// in a compared lane fails it, as each ordered scalar compare does).
// absmin and absmax must each be readable for four floats; in
// entityShared_t absmin is followed by absmax, and absmax by currentOrigin.
inline bool CM_AreaBoxOverlapsScalar(const CmAreaQueryBox &q, const float *absmin, const float *absmax)
{
    return q.maxs[0] >= absmin[0] && q.mins[0] <= absmax[0] && q.maxs[1] >= absmin[1] && q.mins[1] <= absmax[1] &&
           q.maxs[2] >= absmin[2] && q.mins[2] <= absmax[2];
}

inline bool CM_AreaBoxOverlaps(const CmAreaQueryBox &q, const float *absmin, const float *absmax)
{
#if defined(__aarch64__) && !defined(KISAK_CM_AREA_SCALAR)
    // Two compares, one and, a narrow and one 64-bit test: one branch in
    // place of up to six data-dependent (poorly predicted) ones.
    const uint32x4_t ge = vcgeq_f32(vld1q_f32(q.maxs), vld1q_f32(absmin));
    const uint32x4_t le = vcleq_f32(vld1q_f32(q.mins), vld1q_f32(absmax));
    const uint32x4_t pad = {0u, 0u, 0u, 0xFFFFFFFFu};
    const uint16x4_t all = vmovn_u32(vorrq_u32(vandq_u32(ge, le), pad));
    return vget_lane_u64(vreinterpret_u64_u16(all), 0) == ~0ull;
#else
    return CM_AreaBoxOverlapsScalar(q, absmin, absmax);
#endif
}

// Prefetch-only lookahead for a chain that the caller walks and relinks as
// it goes (CM_SortNode moves each visited static model to a child sector):
// the cursor stays kDistance entries ahead of the caller, on entries not yet
// visited and therefore not yet relinked, so it follows the original chain.
// Call Advance() once per visited entry. It only reads next links and issues
// prefetches; it never changes what the caller visits.
template <int kDistance, typename Next, typename Prefetch> struct CmChainLookahead
{
    Next next;
    Prefetch prefetch;
    uint32_t ahead;

    CmChainLookahead(uint32_t head, Next nextFn, Prefetch prefetchFn) : next(nextFn), prefetch(prefetchFn), ahead(head)
    {
        for (int i = 0; i < kDistance && ahead; ++i)
        {
            prefetch(ahead);
            ahead = next(ahead);
        }
    }

    void Advance()
    {
        if (ahead)
        {
            prefetch(ahead);
            ahead = next(ahead);
        }
    }
};

template <int kDistance, typename Next, typename Prefetch>
inline CmChainLookahead<kDistance, Next, Prefetch> CM_MakeChainLookahead(uint32_t head, Next next, Prefetch prefetch)
{
    return CmChainLookahead<kDistance, Next, Prefetch>(head, next, prefetch);
}

// Visits head, next(head), ... in chain order. Up to kBatch entries are
// gathered (calling prefetch on each) before any of them is visited. visit
// returns false to stop the walk, and then this returns false. Same visits
// as the plain loop as long as visit does not relink the chain.
template <int kBatch, typename Next, typename Prefetch, typename Visit>
inline bool CM_WalkEntityChain(uint32_t head, Next next, Prefetch prefetch, Visit visit)
{
    uint32_t batch[kBatch];
    uint32_t entnum = head;
    while (entnum)
    {
        int n = 0;
        do
        {
            batch[n++] = entnum;
            prefetch(entnum);
            entnum = next(entnum);
        } while (entnum && n < kBatch);
        for (int i = 0; i < n; ++i)
        {
            if (!visit(batch[i]))
                return false;
        }
    }
    return true;
}
