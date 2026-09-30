#pragma once

// GPU-fault black box of the deko3d renderer (deko9): pure parts, no deko3d
// and no D3D headers, host-tested by switch_deko9_gpufault_test.cpp.
//
// A GPU MMU fault (erpt 2520-0000: GPC GCC read, VA limit violation) can
// kill the console mid-run, and the process is gone before any
// in-process crash handler runs. These pieces make the
// next fault self-describing without relying on the process surviving:
//
//  - GpuEventRing: the last memory events (memblock creation, allocations,
//    deferred / retired frees, image creation / resize, descriptor slots),
//    each with its GPU VA range and list sequence number.
//  - DrawRecordRing: per submitted list (by sequence number modulo the
//    fence ring), what each draw bound: pass, VS/PS program hashes and code
//    addresses, sampled texture handles and the first render target.
//  - Breadcrumb encoding: the GPU writes (seq << 16 | draw) after the work
//    before it reached CROP (dkCmdBufReportValue), so one 32-bit cell says
//    how far the GPU got; a stopped GPU leaves it at the faulting draw.
//  - StallDetector: a watcher thread samples the cell every few
//    milliseconds; work pending and a cell that has not moved for the
//    threshold is a stall, reported once per episode (a mid-list stall
//    uses a short threshold, the gap between lists -- present / acquire
//    waits -- a long one).
//  - TicAddress: the image VA inside a Maxwell texture header (TIC), read
//    from the descriptor memory the GPU itself uses.

#include <atomic>
#include <cstdint>
#include <cstring>

namespace deko9
{

// ---- memory event ring ----------------------------------------------------------

enum GpuEventKind : uint8_t
{
    kGpuEventBlock = 0,        // dkMemBlockCreate (a Heap chunk)
    kGpuEventAlloc = 1,        // Heap::Alloc
    kGpuEventFreeDeferred = 2, // FreeMemoryAfter, GPU may still read it (seq = the list it waits for)
    kGpuEventFreeNow = 3,      // FreeMemoryAfter / Heap::Free with the list already complete
    kGpuEventFreeRetired = 4,  // CollectCompleted returned a deferred free to its heap
    kGpuEventImage = 5,        // image store created (gpu = its image VA from the TIC)
    kGpuEventImageResize = 6,  // ResizeStore re-laid out an image (gpu = the new VA)
    kGpuEventDescAlloc = 7,    // image descriptor slot written (size = slot)
    kGpuEventDescFree = 8,     // image descriptor slot released after seq (size = slot)
    kGpuEventCount
};

inline const char *GpuEventName(uint8_t kind)
{
    static const char *const kNames[kGpuEventCount] = {"block",   "alloc", "free_deferred", "free_now", "free_retired",
                                                       "image",   "resize", "desc_alloc",   "desc_free"};
    return kind < kGpuEventCount ? kNames[kind] : "?";
}

struct GpuEvent
{
    uint64_t gpu;  // VA (0 for descriptor events)
    uint64_t seq;  // list sequence number the event is keyed to (open list at the time)
    uint32_t size; // bytes (descriptor events: the slot index)
    uint8_t kind;
    uint8_t pool;
    uint16_t chunk;
};

// Single writer (under the device lock); the watcher thread may read while
// the writer runs: an entry can be torn, which a diagnostic dump tolerates
// (each entry's index is published after the entry is written).
template <uint32_t N>
class GpuEventRing
{
    static_assert(N && (N & (N - 1)) == 0, "power of two");

public:
    void Push(const GpuEvent &e)
    {
        const uint64_t head = m_head.load(std::memory_order_relaxed);
        m_events[head & (N - 1)] = e;
        m_head.store(head + 1, std::memory_order_release);
    }
    uint64_t Pushed() const { return m_head.load(std::memory_order_acquire); }
    // Copies the newest min(max, stored) events, oldest first; returns the count.
    uint32_t Newest(GpuEvent *out, uint32_t max) const
    {
        const uint64_t head = Pushed();
        const uint64_t stored = head < N ? head : N;
        const uint32_t count = (uint32_t)(stored < max ? stored : max);
        for (uint32_t i = 0; i < count; ++i)
            out[i] = m_events[(head - count + i) & (N - 1)];
        return count;
    }

private:
    GpuEvent m_events[N]{};
    std::atomic<uint64_t> m_head{0};
};

// ---- per-draw records -----------------------------------------------------------

constexpr uint32_t kDrawRecordTextures = 4;

struct DrawRecord
{
    uint32_t draw;     // index of the draw in its list (the breadcrumb value)
    uint16_t pass;     // Deko9_GpuPassName id
    uint16_t texCount; // pixel-stage texture handles bound (first kDrawRecordTextures kept)
    uint32_t vsHash, psHash;
    uint64_t vsCode, psCode; // program code VA (code segment)
    uint32_t tex[kDrawRecordTextures]; // DkResHandle: image descriptor | sampler << 20
    uint64_t rt0;      // first render target's memory VA (pitch mapping), 0 if none
};

// Records of the last `Lists` lists, `Draws` per list; draws past that are
// counted but not kept. Written by the recording thread before the list is
// submitted; read by the watcher after the GPU stopped in it.
template <uint32_t Lists, uint32_t Draws>
class DrawRecordRing
{
public:
    void BeginList(uint64_t seq)
    {
        Slot &s = m_slots[seq % Lists];
        s.seq = seq;
        s.count = 0;
    }
    void Add(uint64_t seq, const DrawRecord &r)
    {
        Slot &s = m_slots[seq % Lists];
        if (s.seq != seq)
            return;
        if (s.count < Draws)
            s.records[s.count] = r;
        ++s.count;
    }
    // Draws recorded for seq (all, including the ones not kept); 0 when the
    // slot has moved on to another list.
    uint32_t Count(uint64_t seq) const
    {
        const Slot &s = m_slots[seq % Lists];
        return s.seq == seq ? s.count : 0;
    }
    const DrawRecord *Get(uint64_t seq, uint32_t index) const
    {
        const Slot &s = m_slots[seq % Lists];
        if (s.seq != seq || index >= s.count || index >= Draws)
            return nullptr;
        return &s.records[index];
    }

private:
    struct Slot
    {
        uint64_t seq = ~0ull;
        uint32_t count = 0;
        DrawRecord records[Draws];
    };
    Slot m_slots[Lists];
};

// ---- breadcrumbs ----------------------------------------------------------------

// The GPU writes (seq & 0xffff) << 16 | draw, where draw counts the draws of
// list seq whose work reached CROP before the write; kCrumbListEnd marks the
// end of the list (written just before its fence).
constexpr uint32_t kCrumbListEnd = 0xffff;

inline uint32_t CrumbValue(uint64_t seq, uint32_t draw)
{
    return (uint32_t)((seq & 0xffff) << 16) | (draw < kCrumbListEnd ? draw : kCrumbListEnd - 1);
}
inline uint32_t CrumbEnd(uint64_t seq) { return (uint32_t)((seq & 0xffff) << 16) | kCrumbListEnd; }
inline uint32_t CrumbDraw(uint32_t crumb) { return crumb & 0xffff; }
// The full sequence number of a crumb, given the newest submitted list (the
// crumb's list is at most 0xffff lists older).
inline uint64_t CrumbSeq(uint32_t crumb, uint64_t newestSubmitted)
{
    const uint64_t low = crumb >> 16;
    uint64_t seq = (newestSubmitted & ~0xffffull) | low;
    if (seq > newestSubmitted)
        seq -= 0x10000;
    return seq;
}

// ---- stall detector ---------------------------------------------------------------

struct StallThresholds
{
    uint64_t midListNs = 20000000ull;     // GPU stopped inside a list (a draw that never completes)
    uint64_t betweenListsNs = 250000000ull; // at a list end: acquire / present / fence waits are legitimate
};

// Fed one sample per watcher tick. Returns true once per stall episode (the
// cell unchanged for the threshold while submitted work is pending); any
// crumb change re-arms it.
class StallDetector
{
public:
    explicit StallDetector(StallThresholds t = {}) : m_t(t) {}
    bool Sample(uint32_t crumb, uint64_t newestSubmitted, bool haveSubmitted, uint64_t nowNs)
    {
        if (!m_valid || crumb != m_crumb)
        {
            m_valid = true;
            m_crumb = crumb;
            m_since = nowNs;
            m_reported = false;
            return false;
        }
        // Idle: the GPU finished the newest submitted list.
        if (!haveSubmitted || crumb == CrumbEnd(newestSubmitted))
        {
            m_since = nowNs;
            return false;
        }
        const uint64_t limit = CrumbDraw(crumb) == kCrumbListEnd ? m_t.betweenListsNs : m_t.midListNs;
        if (m_reported || nowNs - m_since < limit)
            return false;
        m_reported = true;
        return true;
    }
    uint64_t StalledNs(uint64_t nowNs) const { return nowNs - m_since; }

private:
    StallThresholds m_t;
    bool m_valid = false;
    bool m_reported = false;
    uint32_t m_crumb = 0;
    uint64_t m_since = 0;
};

// ---- texture header ---------------------------------------------------------------

// Image VA inside a Maxwell TIC entry (deko3d TextureImageControl: word 1
// address_low, word 2 bits 0..15 address_high).
inline uint64_t TicAddress(const void *descriptor32Bytes)
{
    uint32_t w[8];
    std::memcpy(w, descriptor32Bytes, sizeof(w));
    return (uint64_t)w[1] | ((uint64_t)(w[2] & 0xffffu) << 32);
}

inline uint32_t HandleImage(uint32_t handle) { return handle & 0xfffffu; }
inline uint32_t HandleSampler(uint32_t handle) { return handle >> 20; }

} // namespace deko9
