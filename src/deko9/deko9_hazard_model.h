// Pure model of the deko9 image hazard tracker: per-draw (old) and static-store (new).
//
// Both trackers below implement the epoch arithmetic of the device's
// HazardCommit over a plain array of store epochs (HazardCheck itself is the
// device's own function), with no deko3d/switch dependency, so a host test
// can drive a long random trace through both and assert they make the same
// barrier decision (position and kind) at every step: for every draw, a
// barrier is recorded iff the per-draw tracker would record one.
//
// OldTracker: today's per-draw model -- every draw adds every currently
// sampled store (regardless of "static") and every render target to one
// batch, then commits once. HazardCommit's stamp loop runs unconditionally
// on every entry in the batch, hit or not, to "now" (the clock after any
// barrier *this* commit recorded) -- so a store's readEpoch tracks the
// current clock as of every draw that samples it, whether or not that draw
// itself needed a barrier.
//
// NewTracker: a store flagged `attachment` (ever a render/depth/blit
// target) still gets the per-draw check. A "static" store (!attachment) is
// only added to the real hazard batch: (1) the draw it is newly bound (a
// cache miss, not the same store the slot already held), matching
// ApplyTextures' newly-resolved branch; or (2) when `pendingRaw` is set,
// matching a copy-write that landed on it while it stayed bound (see
// CopyBufferToImage). Either way Draw() still gives every store it samples
// (skipped or not) OldTracker's unconditional post-commit stamp: readEpoch =
// the current clock, regardless of whether this draw's own commit hit
// anything. Two wrong designs this model ruled out, both caught within the
// first ~40 random ops:
//   - Re-stamping bound static stores on *any* barrier (including one with
//     no draw at all, e.g. ReadImage's Barrier(), or a bare copy/blit
//     commit unrelated to this store) recorded an extra barrier: such a
//     barrier does not mean any store was "just read", so OldTracker would
//     not have touched its readEpoch either.
//   - Restamping only when *this draw's own* commit records a barrier
//     missed one: a store sampled by a draw whose own evaluation hits
//     nothing still needs the catch-up when some earlier, unrelated event
//     (e.g. a ForceBarrier) already moved the clock since its last real
//     stamp -- OldTracker's per-entry stamp is unconditional on hit, not on
//     whether *this* commit barriers.
//
// See ImageStore::attachment / ::pendingRaw (deko9_internal.h) and PrepareDraw's
// post-HazardCommit restamp (deko9_draw.cpp) for the real implementation
// this models.
#pragma once

#include <cstdint>
#include <vector>

namespace deko9_hazard_model
{

enum class Access : uint8_t
{
    Sample,
    Render,
    CopyRead,
    CopyWrite,
    BlitWrite,
};

struct StoreEpochs
{
    uint64_t renderEpoch = 0, copyWriteEpoch = 0, blitEpoch = 0, readEpoch = 0, copyReadEpoch = 0;
    bool attachment = false;
};

// The access check of one pending access against clock c, shared by the
// device (deko9::ImageStore, Device::Access) and this model: *hit when it
// needs a barrier, *engine when the copy or 2D engine is involved.
template <typename Store, typename AccessT>
inline void HazardCheck(const Store &s, AccessT access, uint64_t c, bool *hit, bool *engine)
{
    const bool copied = s.copyWriteEpoch == c || s.blitEpoch == c;
    switch (access)
    {
    case AccessT::Sample:
        *hit = copied || s.renderEpoch == c;
        *engine = copied;
        break;
    case AccessT::Render:
        // Render after render needs nothing; after a read, copy or blit it does.
        *hit = copied || s.readEpoch == c;
        *engine = copied || s.copyReadEpoch == c;
        break;
    case AccessT::CopyRead:
        *hit = s.renderEpoch == c || copied;
        *engine = true;
        break;
    case AccessT::CopyWrite:
        // Copy after copy (successive mip uploads) is ordered on the copy
        // engine; reads, 3D writes and 2D-engine writes conflict.
        *hit = s.readEpoch == c || s.renderEpoch == c || s.blitEpoch == c;
        *engine = true;
        break;
    case AccessT::BlitWrite:
        *hit = s.readEpoch == c || s.renderEpoch == c || s.copyWriteEpoch == c;
        *engine = true;
        break;
    default:
        *hit = *engine = false;
        break;
    }
}

struct PendingAccess
{
    uint32_t store;
    Access access;
};

// One HazardAdd*/HazardCommit batch over an external store array, verbatim
// port of Device::HazardCommit's decision and stamping.
struct HazardBatch
{
    std::vector<StoreEpochs> *stores;
    uint64_t *writeClock;
    std::vector<PendingAccess> pending;
    bool lastBarrier = false;
    bool lastEngine = false;

    void Add(uint32_t store, Access access) { pending.push_back({store, access}); }

    void Commit()
    {
        const uint64_t c = *writeClock;
        bool conflict = false, copyEngine = false;
        for (const PendingAccess &p : pending)
        {
            bool hit, engine;
            HazardCheck((*stores)[p.store], p.access, c, &hit, &engine);
            if (hit)
            {
                conflict = true;
                copyEngine |= engine;
            }
        }
        lastBarrier = conflict;
        lastEngine = copyEngine;
        if (conflict)
            ++*writeClock;
        const uint64_t now = *writeClock;
        for (const PendingAccess &p : pending)
        {
            StoreEpochs &s = (*stores)[p.store];
            switch (p.access)
            {
            case Access::Sample: s.readEpoch = now; break;
            case Access::CopyRead: s.readEpoch = s.copyReadEpoch = now; break;
            case Access::Render: s.renderEpoch = now; break;
            case Access::CopyWrite: s.copyWriteEpoch = now; break;
            case Access::BlitWrite: s.blitEpoch = now; break;
            }
        }
        pending.clear();
    }
};

// ---- OldTracker: today's full per-draw hazard tracking ---------------------
struct OldTracker
{
    std::vector<StoreEpochs> stores;
    uint64_t writeClock = 1;
    uint32_t barriers = 0;

    explicit OldTracker(uint32_t n) : stores(n) {}

    // A draw samples every store in `sampled` (both stages, cache hit or
    // not -- the old tracker never skips) and renders every store in
    // `targets`, one batched commit (PrepareDraw today).
    bool Draw(const std::vector<uint32_t> &sampled, const std::vector<uint32_t> &targets, bool *engineOut)
    {
        HazardBatch b{&stores, &writeClock, {}};
        for (uint32_t s : sampled)
            b.Add(s, Access::Sample);
        for (uint32_t t : targets)
            b.Add(t, Access::Render);
        b.Commit();
        barriers += b.lastBarrier;
        *engineOut = b.lastEngine;
        return b.lastBarrier;
    }
    bool CopyWrite(uint32_t store, bool *engineOut)
    {
        HazardBatch b{&stores, &writeClock, {}};
        b.Add(store, Access::CopyWrite);
        b.Commit();
        barriers += b.lastBarrier;
        *engineOut = b.lastEngine;
        return b.lastBarrier;
    }
    bool BlitWrite(uint32_t src, uint32_t dst, bool *engineOut) // StretchRect: CopyRead(src) + BlitWrite(dst)
    {
        HazardBatch b{&stores, &writeClock, {}};
        b.Add(src, Access::CopyRead);
        b.Add(dst, Access::BlitWrite);
        b.Commit();
        barriers += b.lastBarrier;
        *engineOut = b.lastEngine;
        return b.lastBarrier;
    }
    void ForceBarrier(bool copyEngine)
    {
        (void)copyEngine;
        ++writeClock;
        ++barriers;
    }
};

// ---- NewTracker: static-store skip ----------------------------------------
struct NewTracker
{
    std::vector<StoreEpochs> stores;
    std::vector<uint8_t> pendingRaw; // vector<bool> would defeat the (*stores)[i] symmetry below; plain bytes
    // Whether store i is valid in some sampler-cache slot right now (either
    // stage) -- the driver keeps this in sync with its own slot array, the
    // same thing Device::StaticStoreBound scans. Used only to decide
    // pendingRaw; unrelated to the readEpoch restamp below (see file
    // comment for why those must stay separate).
    std::vector<uint8_t> bound;
    uint64_t writeClock = 1;
    uint32_t barriers = 0;

    explicit NewTracker(uint32_t n) : stores(n), pendingRaw(n, 0), bound(n, 0) {}

    // `sampled[i]` is the store ApplyTextures resolved into sampler slot i
    // this draw; `newlyBound[i]` is true when that is a cache miss (the slot
    // held a different store, or none, before this draw) -- the "newly
    // bound" branch that always checks static stores too.
    bool Draw(const std::vector<uint32_t> &sampled, const std::vector<uint8_t> &newlyBound,
              const std::vector<uint32_t> &targets, bool *engineOut)
    {
        HazardBatch b{&stores, &writeClock, {}};
        for (size_t i = 0; i < sampled.size(); ++i)
        {
            const uint32_t s = sampled[i];
            bound[s] = 1;
            if (newlyBound[i] || stores[s].attachment || pendingRaw[s])
            {
                b.Add(s, Access::Sample);
                pendingRaw[s] = 0;
            }
        }
        for (uint32_t t : targets)
            b.Add(t, Access::Render);
        b.Commit();
        barriers += b.lastBarrier;
        *engineOut = b.lastEngine;
        // PrepareDraw, post-HazardCommit, unconditional (not just when this
        // draw's own commit barriers): OldTracker's HazardCommit stamps
        // every entry in its batch to "now" regardless of whether that
        // entry individually hit -- so a store this draw samples gets
        // readEpoch = the current clock whether or not *anything* in this
        // draw conflicted, including a clock some earlier, unrelated event
        // already advanced. A static store the skip above left out of the
        // real batch still needs that same unconditional catch-up, or a
        // later copy-write's WAR check sees a stale epoch and misses a
        // barrier the old tracker would have recorded (this exact miss,
        // found by the host trace: draw with nothing to hit right after an
        // unrelated ForceBarrier, followed by a copy-write).
        for (uint32_t s : sampled)
            if (!stores[s].attachment)
                stores[s].readEpoch = writeClock;
        return b.lastBarrier;
    }
    bool CopyWrite(uint32_t store, bool *engineOut)
    {
        HazardBatch b{&stores, &writeClock, {}};
        b.Add(store, Access::CopyWrite);
        b.Commit();
        barriers += b.lastBarrier;
        *engineOut = b.lastEngine;
        // CopyBufferToImage: flag a bound static store so the next sample
        // commits the real check instead of skipping it.
        if (!stores[store].attachment && bound[store])
            pendingRaw[store] = 1;
        return b.lastBarrier;
    }
    bool BlitWrite(uint32_t src, uint32_t dst, bool *engineOut) // StretchRect
    {
        // dst->Store()->attachment = true (deko9_draw.cpp): a blit
        // destination is promoted before the hazard is even evaluated.
        stores[dst].attachment = true;
        HazardBatch b{&stores, &writeClock, {}};
        b.Add(src, Access::CopyRead);
        b.Add(dst, Access::BlitWrite);
        b.Commit();
        barriers += b.lastBarrier;
        *engineOut = b.lastEngine;
        return b.lastBarrier;
    }
    void ForceBarrier(bool copyEngine)
    {
        (void)copyEngine;
        ++writeClock;
        ++barriers;
    }
};

} // namespace deko9_hazard_model
