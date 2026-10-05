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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
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

// Draws deko9 records itself (full-screen passes outside the D3D9 draw path).
enum NativeDrawKind : uint8_t
{
    kNativeNone = 0,
    kNativeUpscale,
    kNativeUpscaleRect,
    kNativeGather,
    kNativeFloatZ,
    kNativeHrpDepth,
    kNativeHrpComposite,
    kNativeCount
};

inline const char *NativeDrawName(uint8_t kind)
{
    static const char *const kNames[kNativeCount] = {"-",      "upscale",   "upscale_rect",
                                                     "gather", "floatz", "hrp_depth", "hrp_composite"};
    return kind < kNativeCount ? kNames[kind] : "?";
}

struct DrawRecord
{
    uint32_t draw;     // index of the draw in its list (the breadcrumb value)
    uint16_t pass;     // Deko9_GpuPassName id
    uint16_t texCount; // pixel-stage texture handles bound (first kDrawRecordTextures kept)
    uint32_t vsHash, psHash;
    uint64_t vsCode, psCode; // program code VA (code segment)
    uint32_t tex[kDrawRecordTextures]; // DkResHandle: image descriptor | sampler << 20
    uint64_t rt0;      // first render target's memory VA (pitch mapping), 0 if none
    uint64_t depth;    // depth-stencil memory VA, 0 if none
    // The GPU draw itself: a runaway count or a buffer address outside every
    // heap is what a draw that never finishes (or faults) would show.
    uint8_t prim;      // DkPrimitive
    uint8_t indexed;
    uint8_t native;    // NativeDrawKind (kNativeNone for a D3D9 draw)
    uint8_t pad;
    uint16_t gpuDraws; // GPU draws this record stands for (ranged draws emit several)
    uint32_t count;    // vertices or indices per instance
    uint32_t instances;
    uint32_t first;    // first vertex / first index
    int32_t base;      // base vertex (indexed)
    uint32_t vb0Size;
    uint64_t ib;       // index buffer VA (indexed draws)
    uint64_t vb0;      // stream 0 VA incl. offset
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
    // The newest record of seq (to complete it once the draw's arguments are
    // known); null when none is kept.
    DrawRecord *Last(uint64_t seq)
    {
        Slot &s = m_slots[seq % Lists];
        if (s.seq != seq || !s.count || s.count > Draws)
            return nullptr;
        return &s.records[s.count - 1];
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
// end of the list (written just before its fence), kCrumbListBegin the start
// of the list before its preamble (top-of-pipe cell only).
constexpr uint32_t kCrumbListEnd = 0xffff;
constexpr uint32_t kCrumbListBegin = 0xfffe;

inline uint32_t CrumbValue(uint64_t seq, uint32_t draw)
{
    return (uint32_t)((seq & 0xffff) << 16) | (draw < kCrumbListBegin ? draw : kCrumbListBegin - 1);
}
inline uint32_t CrumbEnd(uint64_t seq) { return (uint32_t)((seq & 0xffff) << 16) | kCrumbListEnd; }
inline uint32_t CrumbBegin(uint64_t seq) { return (uint32_t)((seq & 0xffff) << 16) | kCrumbListBegin; }
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

// Stream order of a crumb: list start < draws 0.. < list end, lists by seq.
inline uint64_t CrumbOrder(uint32_t crumb, uint64_t newestSubmitted)
{
    const uint32_t draw = CrumbDraw(crumb);
    const uint64_t within = draw == kCrumbListBegin ? 0 : draw == kCrumbListEnd ? 0x10000 : (uint64_t)draw + 1;
    return CrumbSeq(crumb, newestSubmitted) * 0x10001ull + within;
}

// Where a stalled GPU sits, from two cells written by the same command
// stream: `crop` once the work before it reached CROP (the end of the 3D
// pipeline), `top` once the channel's front end fetched past it (a host
// semaphore release, no wait for idle).
//  - front_end: top is not past crop. The channel is not fetching: it is
//    blocked in a host-level wait at that point (a swapchain acquire or
//    another semaphore / syncpoint between lists), or the channel is not
//    scheduled at all. Mid-list this can also be the engine's method FIFO
//    backed up behind the draw; between lists it cannot.
//  - engine: top is past crop. The commands were fetched but the work at the
//    crop position (the next draw, or a list preamble) never drains: a draw
//    that never completes, an engine hang or an MMU fault in progress.
//  - unknown: no top cell (older build or top crumbs off), or top behind crop
//    (a torn read).
inline const char *StallVerdict(uint32_t top, bool haveTop, uint32_t crop, uint64_t newestSubmitted)
{
    if (!haveTop)
        return "unknown";
    const uint64_t t = CrumbOrder(top, newestSubmitted), c = CrumbOrder(crop, newestSubmitted);
    if (t == c)
        return "front_end";
    return t > c ? "engine" : "unknown";
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
    // A throttled dump must not consume this stall's only report.
    void RetryReport() { m_reported = false; }
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

// ---- command stream ----------------------------------------------------------------

// A draw whose top crumb was fetched but whose successor's was not stopped
// the channel somewhere in between; the words there say what the front end
// was given (a garbage word means the command memory itself was overwritten).

// Index of the method header of a top-of-pipe crumb release (header, address
// high, address low, payload) carrying `crumb`, or -1.
inline int64_t FindTopCrumb(const uint32_t *words, uint32_t count, uint64_t cellVa, uint32_t crumb)
{
    const uint32_t hi = (uint32_t)(cellVa >> 32), lo = (uint32_t)cellVa;
    for (uint32_t i = 1; i + 2 < count; ++i)
    {
        if (words[i + 2] == crumb && words[i + 1] == lo && words[i] == hi)
            return (int64_t)i - 1;
    }
    return -1;
}

// Maxwell pushbuffer headers as "s<subchannel>:<method byte offset>" plus
// '+' incrementing, '=' non-incrementing, '~' increment-once (count data
// words, the first one or two shown) or '#' inline immediate. A header with
// a sec_op deko3d never emits is printed as "BAD:<word>" and ends the decode.
// Returns the words consumed (at most count); *bad is set on a bad header.
inline uint32_t FormatPushbuffer(const uint32_t *words, uint32_t count, uint32_t maxMethods, char *out, size_t cap,
                                 bool *bad)
{
    size_t len = 0;
    uint32_t i = 0, methods = 0;
    *bad = false;
    if (cap)
        out[0] = 0;
    const auto put = [&](const char *fmt, auto... args) {
        if (len < cap)
        {
            const int n = std::snprintf(out + len, cap - len, fmt, args...);
            len += n > 0 ? (size_t)n : 0;
        }
    };
    while (i < count && methods < maxMethods)
    {
        const uint32_t w = words[i];
        const uint32_t op = w >> 29, arg = (w >> 16) & 0x1fff, subc = (w >> 13) & 7, method = (w & 0x1fff) * 4;
        if (op != 1 && op != 3 && op != 4 && op != 5)
        {
            put("%sBAD:%08x", methods ? " " : "", (unsigned)w);
            *bad = true;
            return i + 1;
        }
        ++methods;
        if (op == 4)
        {
            put("%ss%u:%x#%x", methods > 1 ? " " : "", (unsigned)subc, (unsigned)method, (unsigned)arg);
            ++i;
            continue;
        }
        put("%ss%u:%x%c%u", methods > 1 ? " " : "", (unsigned)subc, (unsigned)method,
            op == 1 ? '+' : op == 3 ? '=' : '~', (unsigned)arg);
        const uint32_t avail = count - i - 1;
        if (arg && avail)
            put("[%x", (unsigned)words[i + 1]);
        if (arg > 1 && avail > 1)
            put(arg > 2 ? ",%x..]" : ",%x]", (unsigned)words[i + 2]);
        else if (arg && avail)
            put("%s", "]");
        i += 1 + (arg < avail ? arg : avail);
    }
    return i;
}

// ---- GPFIFO segments ----------------------------------------------------------------

// A command list reaches the channel as GPFIFO entries, one per contiguous
// run of command words; a command-memory chunk switch starts a new entry and
// the old chunk's remaining words are never fetched.
struct CmdSegment
{
    uint64_t gpu;
    uint32_t words;
};

// deko3d's finished-list control stream: 8-byte headers {type:8, extra:24,
// arg:32}; Jump/Call and the fence commands carry one pointer; GpfifoList is
// followed by `arg` entries {iova, numCmds, flags}.
enum : uint32_t
{
    kCtrlReturn = 0,
    kCtrlJump = 1,
    kCtrlCall = 2,
    kCtrlGpfifoList = 3,
    kCtrlWaitFence = 4,
    kCtrlSignalFence = 5,
};

// The list's segments in fetch order: the count found (at most max are
// stored), or -1 on a control command this walker does not know (compute)
// or a malformed stream.
inline int32_t ListSegments(const void *list, CmdSegment *out, uint32_t max)
{
    const uint8_t *stack[4];
    uint32_t depth = 0, n = 0, steps = 0;
    const uint8_t *cur = static_cast<const uint8_t *>(list);
    while (cur)
    {
        if (++steps > (1u << 20))
            return -1;
        uint64_t header;
        std::memcpy(&header, cur, sizeof(header));
        const uint32_t type = (uint32_t)(header & 0xff), arg = (uint32_t)(header >> 32);
        const uint8_t *ptr = nullptr;
        switch (type)
        {
        case kCtrlReturn:
            cur = depth ? stack[--depth] : nullptr;
            break;
        case kCtrlJump:
        case kCtrlCall:
            std::memcpy(&ptr, cur + 8, sizeof(ptr));
            if (type == kCtrlCall)
            {
                if (depth == 4)
                    return -1;
                stack[depth++] = cur + 16;
            }
            cur = ptr;
            break;
        case kCtrlGpfifoList:
            for (uint32_t i = 0; i < arg; ++i, ++n)
            {
                uint64_t iova;
                uint32_t words;
                std::memcpy(&iova, cur + 8 + i * 16, sizeof(iova));
                std::memcpy(&words, cur + 8 + i * 16 + 8, sizeof(words));
                if (n < max)
                    out[n] = {iova, words};
            }
            cur += 8 + (size_t)arg * 16;
            break;
        case kCtrlWaitFence:
        case kCtrlSignalFence:
            cur += 16;
            break;
        default:
            return -1;
        }
    }
    return (int32_t)n;
}

// A recorded segment with the CPU view of its words.
struct SegmentWords
{
    const uint32_t *cpu;
    uint64_t gpu;
    uint32_t words;
};

struct SegmentPos
{
    uint32_t seg;
    uint32_t word;
};

// The top crumb release carrying `crumb` at or after `from`, in fetch order
// (a release is one reservation, so it never straddles two segments).
inline bool FindCrumbAcross(const SegmentWords *segs, uint32_t n, uint64_t cellVa, uint32_t crumb, SegmentPos from,
                            SegmentPos *at)
{
    for (uint32_t s = from.seg; s < n; ++s)
    {
        const uint32_t skip = s == from.seg ? from.word : 0;
        if (skip >= segs[s].words)
            continue;
        const int64_t i = FindTopCrumb(segs[s].cpu + skip, segs[s].words - skip, cellVa, crumb);
        if (i >= 0)
        {
            *at = {s, skip + (uint32_t)i};
            return true;
        }
    }
    return false;
}

// Decodes [from, to) in fetch order; each GPFIFO entry switch shows as
// " |seg N@<gpu>|". Returns the number of entry switches crossed.
inline uint32_t FormatAcross(const SegmentWords *segs, uint32_t n, SegmentPos from, SegmentPos to,
                             uint32_t maxMethods, char *out, size_t cap, bool *bad)
{
    size_t len = 0;
    uint32_t switches = 0;
    *bad = false;
    if (cap)
        out[0] = 0;
    for (uint32_t s = from.seg; s < n && s <= to.seg && !*bad; ++s)
    {
        const uint32_t begin = s == from.seg ? from.word : 0;
        const uint32_t end = s == to.seg ? std::min(to.word, segs[s].words) : segs[s].words;
        if (s != from.seg)
        {
            ++switches;
            if (len < cap)
            {
                const int k = std::snprintf(out + len, cap - len, "%s|seg %u@%llx|", len ? " " : "", s,
                                            (unsigned long long)segs[s].gpu);
                len += k > 0 ? (size_t)k : 0;
            }
        }
        if (begin >= end)
            continue;
        if (len && len + 1 < cap)
            out[len++] = ' ', out[len] = 0;
        if (len >= cap)
            break;
        FormatPushbuffer(segs[s].cpu + begin, end - begin, maxMethods, out + len, cap - len, bad);
        len += std::strlen(out + len);
    }
    return switches;
}

} // namespace deko9
