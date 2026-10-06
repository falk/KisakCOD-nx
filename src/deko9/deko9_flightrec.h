#pragma once

// Command-submission flight recorder of the deko3d renderer: the pure parts
// (host-testable: switch_deko9_flightrec_test.cpp).
//
// A GPU channel fault on a pushbuffer word (PBDMA PBENTRY) reports only the
// raw bad dword (HDR_SHADOW) and that it was the first word of a GPFIFO
// segment. To say where that word came from, the device keeps, always on:
//   - one 32-byte record per GPFIFO segment it submits (list seq, frame,
//     segment index, list kind, fence slot, submitting thread, command chunk,
//     word offset, length, address, first word, hash of the first words);
//   - one 16-byte event per command-chunk transition (alloc, reuse, busy,
//     free, poison, drop) and per change of the thread recording commands;
//   - a table of the command chunks (address, size, state, words used).
// Both rings are lock-free and multi-writer safe; a reader (the watchdog
// thread) copies them without any lock and skips a slot torn by a writer.
//
// Tagged poison: with poison on, a freed command chunk is filled with tag A
// and the 64 words after each chunk's last written word with tag B. Both are
// obsolete-format pushbuffer words (SEC_OP 31:29 = 0 and TERT_OP 17:16 = 0,
// never 0), which the GPU always rejects, so a fault on them names the
// chunk and the offset:
//   tag A = ((chunk + 1) << 18) | (offset >> shift)        (bit 28 clear)
//   tag B = tag A | (1 << 28)
//   decode: chunk = ((shadow >> 18) & 0x3ff) - 1, offset = (shadow & 0xffff) << shift,
//           tail (tag B) = (shadow >> 28) & 1;
// shift is 0 for chunks of at most 65536 words (256 KiB) and grows by one
// per doubling above that (4 MiB chunks: 4).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace deko9
{
namespace fr
{

// ---- pushbuffer words (Maxwell host FIFO_DMA format) -------------------------------

// SEC_OP 31:29: 1 INC, 3 NON_INC, 4 IMMD, 5 INC_ONCE are method headers;
// 0 (and 2) select TERT_OP 17:16, where SEC_OP 0 / TERT_OP 0 is the obsolete
// incrementing format the GPU rejects (0 itself is the NOP); 6 and 7 are
// reserved and END_SEG, neither of which deko3d emits.
enum class PbWord : uint8_t
{
    Nop,
    Method,
    Control,
    Obsolete,
    Other,
};

constexpr PbWord ClassifyPbWord(uint32_t w)
{
    if (w == 0)
        return PbWord::Nop;
    const uint32_t sec = w >> 29;
    if (sec == 1 || sec == 3 || sec == 4 || sec == 5)
        return PbWord::Method;
    if (sec == 0)
        return ((w >> 16) & 3) ? PbWord::Control : PbWord::Obsolete;
    return PbWord::Other;
}

// Every segment deko3d cuts starts at a command boundary: a method header on
// one of the subchannels it binds (3D 0, compute 1, inline 2, 2D 3, copy 4,
// host 6). Anything else there faults the channel or desyncs it.
constexpr bool ValidSegmentStart(uint32_t w)
{
    if (ClassifyPbWord(w) != PbWord::Method)
        return false;
    const uint32_t sub = (w >> 13) & 7;
    return sub <= 4 || sub == 6;
}

// Walks a run of command words as headers plus payloads (INC, NON_INC and
// INC_ONCE carry COUNT 28:16 payload words, IMMD none): ok when every header
// is a valid segment start and the walk ends exactly at `count`.
struct WalkResult
{
    bool ok = true;
    uint32_t at = 0;   // word index of the offending header
    uint32_t word = 0; // its value
};

inline WalkResult WalkWords(const uint32_t *words, uint32_t count)
{
    WalkResult r;
    uint32_t i = 0;
    while (i < count)
    {
        const uint32_t w = words[i];
        if (!ValidSegmentStart(w))
        {
            r = {false, i, w};
            return r;
        }
        const uint32_t payload = (w >> 29) == 4 ? 0 : (w >> 16) & 0x1fff;
        if (payload > count - i - 1)
        {
            r = {false, i, w};
            return r;
        }
        i += 1 + payload;
    }
    return r;
}

// ---- tagged poison ---------------------------------------------------------------

constexpr uint32_t kMaxChunkIds = 1023; // ids 0..1022 (chunk + 1 fits 10 bits)
constexpr uint32_t kNoChunk = 0x3ff;    // not a poisonable chunk
constexpr uint32_t kTailBit = 1u << 28;
constexpr uint32_t kTailWords = 64;

constexpr uint32_t PoisonShift(uint32_t chunkWords)
{
    uint32_t shift = 0;
    while (shift < 16 && (chunkWords > (65536u << shift)))
        ++shift;
    return shift;
}

constexpr uint32_t PoisonTagA(uint32_t chunk, uint32_t wordOffset, uint32_t shift)
{
    return (((chunk + 1) & 0x3ff) << 18) | ((wordOffset >> shift) & 0xffff);
}

constexpr uint32_t PoisonTagB(uint32_t chunk, uint32_t wordOffset, uint32_t shift)
{
    return PoisonTagA(chunk, wordOffset, shift) | kTailBit;
}

struct PoisonTag
{
    bool tag = false;  // has the tag shape (says nothing yet about a known chunk)
    bool tail = false; // tag B
    uint32_t chunk = 0;
    uint32_t field = 0; // offset >> shift
};

constexpr PoisonTag DecodePoison(uint32_t w)
{
    PoisonTag t;
    const uint32_t id1 = (w >> 18) & 0x3ff;
    if ((w & 0xe0030000u) != 0 || id1 == 0)
        return t;
    t.tag = true;
    t.tail = (w & kTailBit) != 0;
    t.chunk = id1 - 1;
    t.field = w & 0xffff;
    return t;
}

// ---- command-chunk guard ------------------------------------------------------
// Every command chunk is followed by kGuardWords words the command buffer is
// never given. A writer that runs past its chunk's end lands here instead of
// in the next chunk (which may belong to a list the GPU is still reading).
// The pattern is neither a poison tag nor a valid method header.

constexpr uint32_t kGuardWords = 64;

constexpr uint32_t GuardWord(uint32_t i)
{
    return 0xC6A50000u | (i & 0xffff);
}

inline void FillGuard(uint32_t *guard)
{
    for (uint32_t i = 0; i < kGuardWords; ++i)
        guard[i] = GuardWord(i);
}

// Number of guard words that differ from the pattern; *firstDirty gets the
// index of the first one (kGuardWords when clean).
inline uint32_t CheckGuard(const volatile uint32_t *guard, uint32_t *firstDirty)
{
    uint32_t dirty = 0, first = kGuardWords;
    for (uint32_t i = 0; i < kGuardWords; ++i)
    {
        if (guard[i] != GuardWord(i))
        {
            if (first == kGuardWords)
                first = i;
            ++dirty;
        }
    }
    *firstDirty = first;
    return dirty;
}

// ---- image sentinels ---------------------------------------------------------
// GPU address space is handed out in creation order, and an image memblock's
// pitch range is followed by two kernel aliases of it. A sentinel memblock
// created right after each image memblock therefore sits where a GPU write
// running past the end of the image range would land (before this, the
// command memblock sat there). It is filled once and sampled every
// kSentinelStrideWords words, which covers every 512-byte GOB.

constexpr uint32_t kSentinelBytes = 1u << 20;
constexpr uint32_t kSentinelStrideWords = 128;

constexpr uint32_t SentinelWord(uint32_t i)
{
    return 0xC6A60000u | ((i * 0x9E37u) & 0xffff);
}

inline void FillSentinel(uint32_t *words, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        words[i] = SentinelWord(i);
}

// Samples one word per stride; returns how many differ, *firstDirty = word
// index of the first (count when clean).
inline uint32_t CheckSentinel(const volatile uint32_t *words, uint32_t count, uint32_t *firstDirty)
{
    uint32_t dirty = 0, first = count;
    for (uint32_t i = 0; i < count; i += kSentinelStrideWords)
    {
        if (words[i] != SentinelWord(i))
        {
            if (first == count)
                first = i;
            ++dirty;
        }
    }
    *firstDirty = first;
    return dirty;
}

// Fills words [from, to) of a chunk with its tag A (or B). The words are
// GPU-cached, CPU-uncached command memory: plain stores, no cache maintenance
// (the GPU sees them exactly like the commands later written there).
inline void FillTag(uint32_t *chunkWords, uint32_t chunk, uint32_t from, uint32_t to, uint32_t shift, bool tail)
{
    const uint32_t bit = tail ? kTailBit : 0;
    for (uint32_t i = from; i < to; ++i)
        chunkWords[i] = PoisonTagA(chunk, i, shift) | bit;
}

// FNV-1a over the first min(words, n) words, read once each.
inline uint32_t HeadHash(const volatile uint32_t *p, uint32_t words, uint32_t n = 4)
{
    uint32_t h = 2166136261u;
    const uint32_t m = words < n ? words : n;
    for (uint32_t i = 0; i < m; ++i)
    {
        const uint32_t w = p[i];
        for (int b = 0; b < 4; ++b)
            h = (h ^ ((w >> (8 * b)) & 0xff)) * 16777619u;
    }
    return h;
}

// ---- records ---------------------------------------------------------------------

enum : uint8_t
{
    kKindOther = 0,
    kKindDraw = 1,
    kKindPresent = 2,
    kKindUpload = 3,
};

enum : uint8_t
{
    kSegBadFirst = 1u << 0,   // first word not a valid segment start at submit
    kSegNoChunk = 1u << 1,    // address outside the list's command chunks
};

// One GPFIFO segment as submitted.
struct SegRec
{
    uint32_t seq;      // list sequence (low 32 bits)
    uint32_t frame;    // presents so far (low 32 bits)
    uint32_t info;     // seg:8 | kind:4 | slot:4 | tid:8 | flags:8
    uint32_t chunkOff; // chunk:10 | word offset in the chunk:22
    uint32_t words;
    uint32_t iova; // low 32 bits of the GPU address
    uint32_t first;
    uint32_t hash; // HeadHash of the first 4 words at submit
};
static_assert(sizeof(SegRec) == 32, "segment record layout is the flightrec.bin layout");

constexpr uint32_t SegInfo(uint32_t seg, uint32_t kind, uint32_t slot, uint32_t tid, uint32_t flags)
{
    return (seg & 0xff) | ((kind & 0xf) << 8) | ((slot & 0xf) << 12) | ((tid & 0xff) << 16) | ((flags & 0xff) << 24);
}
constexpr uint32_t SegIndex(uint32_t info) { return info & 0xff; }
constexpr uint32_t SegKind(uint32_t info) { return (info >> 8) & 0xf; }
constexpr uint32_t SegSlot(uint32_t info) { return (info >> 12) & 0xf; }
constexpr uint32_t SegTid(uint32_t info) { return (info >> 16) & 0xff; }
constexpr uint32_t SegFlags(uint32_t info) { return info >> 24; }
constexpr uint32_t ChunkOff(uint32_t chunk, uint32_t off) { return ((chunk & 0x3ff) << 22) | (off & 0x3fffff); }
constexpr uint32_t RecChunk(uint32_t chunkOff) { return chunkOff >> 22; }
constexpr uint32_t RecOff(uint32_t chunkOff) { return chunkOff & 0x3fffff; }

enum : uint8_t
{
    kEvAlloc = 1,     // new chunk from the heap; aux = words
    kEvReuse = 2,     // free chunk handed to the open list; aux = lists since freed
    kEvBusy = 3,      // chunk retired with its submitted list (seq); aux = words used
    kEvBusyError = 4, // retired unsubmitted (queue error); seq = the list's
    kEvFree = 5,      // list completed, chunk free; aux = open seq
    kEvPoison = 6,    // tag A written; aux = words
    kEvDrop = 7,      // returned to the heap (other size)
    kEvWriter = 8,    // a different thread records into the open list; aux = lock owner tid
    kEvHash = 9,      // a segment's head changed between submit and retire; aux = seg index
    kEvBadFirst = 10, // invalid segment start at submit; aux = the word
    kEvSubmit = 11,   // list submitted; chunk = segment count, aux = fence slot
    kEvForeign = 12,  // recording without the device lock or into another thread's capture
    kEvEarly = 13,    // test: chunk poisoned before its list ran; aux = expected shadow
    kEvTail = 14,     // tag B written after the chunk's last word; aux = words
    kEvCount
};

inline const char *EvName(uint32_t ev)
{
    static const char *const kNames[kEvCount] = {"?",    "alloc",  "reuse",    "busy",   "busy_err",
                                                 "free", "poison", "drop",     "writer", "hash",
                                                 "bad_first", "submit", "foreign", "early", "tail"};
    return ev < kEvCount ? kNames[ev] : "?";
}

struct ChunkEv
{
    uint32_t seq;       // the list concerned (low 32 bits)
    uint32_t completed; // completed list sequence at the event (low 32 bits)
    uint32_t info;      // chunk:16 | ev:8 | tid:8
    uint32_t aux;
};
static_assert(sizeof(ChunkEv) == 16, "chunk event layout is the flightrec.bin layout");

constexpr uint32_t EvInfo(uint32_t chunk, uint32_t ev, uint32_t tid)
{
    return (chunk & 0xffff) | ((ev & 0xff) << 16) | ((tid & 0xff) << 24);
}
constexpr uint32_t EvChunk(uint32_t info) { return info & 0xffff; }
constexpr uint32_t EvKind(uint32_t info) { return (info >> 16) & 0xff; }
constexpr uint32_t EvTid(uint32_t info) { return info >> 24; }

// ---- lock-free ring --------------------------------------------------------------

// Multi-writer ring of fixed-size records. A writer claims an index with one
// atomic add, marks the slot busy (stamp 0), stores the words, then publishes
// the slot's stamp (index + 1). A reader accepts a slot only when the stamp it
// reads before and after copying equals the index it wants, so a slot being
// rewritten is skipped, never returned half old, half new.
struct RingTestAccess; // host test only: simulates a writer caught mid-record

template <class Rec, uint32_t N>
class Ring
{
    friend struct RingTestAccess;
    static_assert((N & (N - 1)) == 0, "power of two");
    static constexpr uint32_t kWords = sizeof(Rec) / 4;

public:
    uint32_t Push(const Rec &r)
    {
        const uint32_t idx = m_head.fetch_add(1, std::memory_order_relaxed);
        Slot &s = m_slots[idx & (N - 1)];
        uint32_t w[kWords];
        std::memcpy(w, &r, sizeof(w));
        // Release word stores: a reader that sees any new word also sees the
        // busy stamp stored before it (no fences: TSan cannot model them).
        s.stamp.store(0, std::memory_order_relaxed);
        for (uint32_t i = 0; i < kWords; ++i)
            s.w[i].store(w[i], std::memory_order_release);
        s.stamp.store(idx + 1, std::memory_order_release);
        return idx;
    }

    // The record pushed as `idx`, if it is still in the ring and not torn.
    bool Read(uint32_t idx, Rec *out) const
    {
        const Slot &s = m_slots[idx & (N - 1)];
        const uint32_t before = s.stamp.load(std::memory_order_acquire);
        if (before != idx + 1)
            return false;
        uint32_t w[kWords];
        for (uint32_t i = 0; i < kWords; ++i)
            w[i] = s.w[i].load(std::memory_order_acquire);
        if (s.stamp.load(std::memory_order_relaxed) != before)
            return false;
        std::memcpy(out, w, sizeof(w));
        return true;
    }

    uint32_t Head() const { return m_head.load(std::memory_order_acquire); }
    static constexpr uint32_t Capacity() { return N; }

    // Oldest first: at most max of the newest records; returns the count.
    uint32_t Newest(Rec *out, uint32_t max) const
    {
        const uint32_t head = Head();
        const uint32_t span = head < N ? head : N;
        const uint32_t want = span < max ? span : max;
        uint32_t n = 0;
        for (uint32_t i = head - want; i != head; ++i)
            if (Read(i, &out[n]))
                ++n;
        return n;
    }

private:
    struct Slot
    {
        std::atomic<uint32_t> stamp{0};
        std::atomic<uint32_t> w[kWords]{};
    };
    Slot m_slots[N];
    std::atomic<uint32_t> m_head{0};
};

// ---- recording ownership ---------------------------------------------------------

// Recording is single-writer at any instant: the thread holding the device
// lock, and while a bake capture runs only the capturing thread (another
// thread's commands would land in the baked unit). True for a violation.
constexpr bool ForeignRecord(uintptr_t self, uintptr_t lockOwner, uintptr_t captureTag)
{
    return lockOwner != self || (captureTag != 0 && captureTag != self);
}

// ---- small thread ids ------------------------------------------------------------

// Thread tags (deko9::ThreadTag) folded to 1..31 in order of first use; 0 =
// table full. Lock-free: a new thread claims a slot with one compare-exchange.
class ThreadTable
{
public:
    static constexpr uint32_t kSlots = 32;
    uint8_t Index(uintptr_t tag)
    {
        for (uint32_t i = 1; i < kSlots; ++i)
        {
            uintptr_t cur = m_tags[i].load(std::memory_order_acquire);
            if (cur == tag)
                return (uint8_t)i;
            if (cur == 0)
            {
                if (m_tags[i].compare_exchange_strong(cur, tag, std::memory_order_acq_rel))
                    return (uint8_t)i;
                if (cur == tag)
                    return (uint8_t)i;
            }
        }
        return 0;
    }
    uintptr_t Tag(uint32_t i) const { return i < kSlots ? m_tags[i].load(std::memory_order_acquire) : 0; }

private:
    std::atomic<uintptr_t> m_tags[kSlots]{};
};

// ---- command chunk table ---------------------------------------------------------

enum : uint8_t
{
    kChunkHeap = 0, // back in the heap (or never seen)
    kChunkOpen = 1, // part of the open list
    kChunkBusy = 2, // submitted, list not complete
    kChunkFree = 3, // reusable
};

inline const char *ChunkStateName(uint32_t s)
{
    static const char *const kNames[] = {"heap", "open", "busy", "free"};
    return s < 4 ? kNames[s] : "?";
}

// The binary form of one chunk entry in flightrec.bin.
struct ChunkRec
{
    uint64_t gpu;
    uint32_t words;
    uint32_t state;
    uint32_t seq;
    uint32_t used;      // words the newest list wrote
    uint32_t freedOpen; // open list sequence when it was freed
    uint32_t reuses;
};
static_assert(sizeof(ChunkRec) == 32, "chunk entry layout is the flightrec.bin layout");

// Written under the device lock by the recording side; read lock-free by the
// dump (each field atomic, so a reader sees a consistent value per field).
class ChunkTable
{
public:
    static constexpr uint32_t kEntries = 256;

    // The chunk's id (assigned on first sight of its address), kNoChunk when full.
    uint32_t Id(uint64_t gpu, void *cpu, uint32_t words)
    {
        const uint32_t n = m_count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i)
            if (m_e[i].gpu.load(std::memory_order_relaxed) == gpu &&
                m_e[i].words.load(std::memory_order_relaxed) == words)
                return i;
        if (n == kEntries)
            return kNoChunk;
        Entry &e = m_e[n];
        e.gpu.store(gpu, std::memory_order_relaxed);
        e.cpu.store((uintptr_t)cpu, std::memory_order_relaxed);
        e.words.store(words, std::memory_order_relaxed);
        m_count.store(n + 1, std::memory_order_release);
        return n;
    }

    // The id of the chunk containing `gpu`, kNoChunk if none (live chunks only).
    uint32_t Containing(uint64_t gpu) const
    {
        const uint32_t n = Count();
        for (uint32_t i = 0; i < n; ++i)
        {
            const Entry &e = m_e[i];
            const uint64_t base = e.gpu.load(std::memory_order_relaxed);
            if (e.state.load(std::memory_order_relaxed) != kChunkHeap && gpu >= base &&
                gpu < base + 4ull * e.words.load(std::memory_order_relaxed))
                return i;
        }
        return kNoChunk;
    }

    uint32_t Count() const { return m_count.load(std::memory_order_acquire); }

    void Set(uint32_t id, uint32_t state, uint32_t seq)
    {
        if (id >= kEntries)
            return;
        m_e[id].state.store(state, std::memory_order_relaxed);
        m_e[id].seq.store(seq, std::memory_order_relaxed);
    }
    void SetUsed(uint32_t id, uint32_t used)
    {
        if (id < kEntries)
            m_e[id].used.store(used, std::memory_order_relaxed);
    }
    void SetFreed(uint32_t id, uint32_t openSeq)
    {
        if (id < kEntries)
            m_e[id].freedOpen.store(openSeq, std::memory_order_relaxed);
    }
    void NoteReuse(uint32_t id)
    {
        if (id < kEntries)
            m_e[id].reuses.fetch_add(1, std::memory_order_relaxed);
    }

    uint32_t *Cpu(uint32_t id) const
    {
        return id < Count() ? reinterpret_cast<uint32_t *>(m_e[id].cpu.load(std::memory_order_relaxed)) : nullptr;
    }
    ChunkRec Get(uint32_t id) const
    {
        ChunkRec r{};
        if (id >= kEntries)
            return r;
        const Entry &e = m_e[id];
        r.gpu = e.gpu.load(std::memory_order_relaxed);
        r.words = e.words.load(std::memory_order_relaxed);
        r.state = e.state.load(std::memory_order_relaxed);
        r.seq = e.seq.load(std::memory_order_relaxed);
        r.used = e.used.load(std::memory_order_relaxed);
        r.freedOpen = e.freedOpen.load(std::memory_order_relaxed);
        r.reuses = e.reuses.load(std::memory_order_relaxed);
        return r;
    }

private:
    struct Entry
    {
        std::atomic<uint64_t> gpu{0};
        std::atomic<uintptr_t> cpu{0};
        std::atomic<uint32_t> words{0}, state{0}, seq{0}, used{0}, freedOpen{0}, reuses{0};
    };
    Entry m_e[kEntries];
    std::atomic<uint32_t> m_count{0};
};

// ---- free-chunk reuse ------------------------------------------------------------

// Which free chunk the open list takes, or -1 for a fresh allocation.
// Without quarantine (poison off): the most recently freed one (the back),
// as before. With quarantine: the oldest freed one whose list completed at
// least `quarantine` lists ago, so a chunk keeps its poison visible to the
// GPU across that many lists. `freedOpen(i)` is the open sequence at which
// entry i was freed, `fits(i)` whether it has the wanted size.
template <class FreedOpen, class Fits>
int PickReuse(size_t count, uint64_t openSeq, uint32_t quarantine, FreedOpen freedOpen, Fits fits)
{
    if (!quarantine)
        return count && fits(count - 1) ? (int)(count - 1) : -1;
    for (size_t i = 0; i < count; ++i)
        if (fits(i) && freedOpen(i) + quarantine <= openSeq)
            return (int)i;
    return -1;
}

// ---- verdict ---------------------------------------------------------------------

enum class Verdict : uint8_t
{
    TagA,          // the GPU fetched a chunk after it was freed (premature retire)
    TagB,          // a segment ran past the chunk's last written word
    BadFirst,      // a segment was submitted with an invalid first word
    HashChanged,   // a segment's words changed between submit and retire
    UnknownStale,  // none of the above: stale GPU view, wrong address, foreign memory
};

inline const char *VerdictName(Verdict v)
{
    switch (v)
    {
    case Verdict::TagA:
        return "TAG_A_FREED_CHUNK_FETCHED";
    case Verdict::TagB:
        return "TAG_B_PAST_WRITTEN_END";
    case Verdict::BadFirst:
        return "BAD_FIRST_WORD_AT_SUBMIT";
    case Verdict::HashChanged:
        return "HASH_CHANGED_AFTER_SUBMIT";
    case Verdict::UnknownStale:
        break;
    }
    return "UNKNOWN_STALE";
}

struct VerdictResult
{
    Verdict verdict = Verdict::UnknownStale;
    uint32_t chunk = kNoChunk;
    uint32_t offset = 0; // words (tag verdicts)
    bool exact = false;  // BadFirst: the recorded first word equals the shadow
};

// Classifies a fault's raw HDR_SHADOW word against a recorder snapshot. A
// tag counts only for a chunk the recorder knows, at an offset inside it,
// with poison on (an arbitrary obsolete word also has the tag shape).
inline VerdictResult Classify(uint32_t shadow, bool poisonOn, const ChunkRec *chunks, uint32_t chunkCount,
                              const SegRec *segs, uint32_t segCount, const ChunkEv *evs, uint32_t evCount)
{
    VerdictResult r;
    const PoisonTag tag = DecodePoison(shadow);
    if (poisonOn && tag.tag && tag.chunk < chunkCount && chunks[tag.chunk].words)
    {
        const uint32_t off = tag.field << PoisonShift(chunks[tag.chunk].words);
        if (off < chunks[tag.chunk].words)
        {
            r.verdict = tag.tail ? Verdict::TagB : Verdict::TagA;
            r.chunk = tag.chunk;
            r.offset = off;
            return r;
        }
    }
    bool anyBad = false, hash = false;
    for (uint32_t i = 0; i < segCount; ++i)
    {
        if (!(SegFlags(segs[i].info) & kSegBadFirst))
            continue;
        anyBad = true;
        if (segs[i].first == shadow)
        {
            r.verdict = Verdict::BadFirst;
            r.chunk = RecChunk(segs[i].chunkOff);
            r.offset = RecOff(segs[i].chunkOff);
            r.exact = true;
            return r;
        }
    }
    for (uint32_t i = 0; i < evCount; ++i)
    {
        const uint32_t ev = EvKind(evs[i].info);
        anyBad |= ev == kEvBadFirst;
        if (ev == kEvHash)
        {
            hash = true;
            r.chunk = EvChunk(evs[i].info);
        }
    }
    if (hash)
        r.verdict = Verdict::HashChanged;
    else if (anyBad)
        r.verdict = Verdict::BadFirst;
    else
        r.chunk = kNoChunk;
    return r;
}

// ---- dump text -------------------------------------------------------------------

// Device state the dump prints in its header line.
struct DumpHeader
{
    const char *reason = "";
    uint64_t open = 0, submitted = 0, completed = 0, frames = 0;
    int queueError = -1; // -1 not checked
    uint32_t poison = 0, chunkBytes = 0;
    uint32_t writerTid = 0, lockTid = 0, submitTid = 0, captureTid = 0;
    uint32_t segPushed = 0, evPushed = 0;
    uint32_t badFirst = 0, hashChanged = 0, foreign = 0;
};

// Bounded line writer: never writes past cap, counts what it dropped.
class TextOut
{
public:
    TextOut(char *buf, size_t cap) : m_buf(buf), m_cap(cap) {}
    template <class... A>
    void Line(const char *fmt, A... args)
    {
        if (m_len + 1 >= m_cap)
        {
            ++m_dropped;
            return;
        }
        const int n = std::snprintf(m_buf + m_len, m_cap - m_len, fmt, args...);
        if (n < 0 || (size_t)n >= m_cap - m_len)
        {
            m_buf[m_len] = 0;
            ++m_dropped;
            return;
        }
        m_len += (size_t)n;
    }
    size_t Size() const { return m_len; }
    uint32_t Dropped() const { return m_dropped; }

private:
    char *m_buf;
    size_t m_cap;
    size_t m_len = 0;
    uint32_t m_dropped = 0;
};

// Newest records the text dump prints (the rest is in flightrec.bin).
constexpr uint32_t kDumpSegs = 64;
constexpr uint32_t kDumpEvents = 48;
constexpr uint32_t kDumpChunks = 16;
constexpr size_t kDumpCap = 15 * 1024;

inline void FormatDump(TextOut &out, const DumpHeader &h, const SegRec *segs, uint32_t segCount, const ChunkEv *evs,
                       uint32_t evCount, const ChunkRec *chunks, uint32_t chunkCount, const ThreadTable *threads)
{
    out.Line("FR hdr reason=%s open=%llu submitted=%llu completed=%llu frames=%llu queueError=%d poison=%u "
             "chunkKB=%u writer=%u lock=%u submit=%u capture=%u segs=%u evs=%u bad_first=%u hash=%u foreign=%u\n",
             h.reason, (unsigned long long)h.open, (unsigned long long)h.submitted, (unsigned long long)h.completed,
             (unsigned long long)h.frames, h.queueError, h.poison, h.chunkBytes >> 10, h.writerTid, h.lockTid,
             h.submitTid, h.captureTid, h.segPushed, h.evPushed, h.badFirst, h.hashChanged, h.foreign);
    if (threads)
        for (uint32_t i = 1; i < ThreadTable::kSlots; ++i)
            if (const uintptr_t tag = threads->Tag(i))
                out.Line("FR thr t=%u tag=0x%llx\n", i, (unsigned long long)tag);
    // Live chunks first (open, busy, free), newest ids last.
    uint32_t shown = 0;
    for (uint32_t i = chunkCount; i-- > 0 && shown < kDumpChunks;)
    {
        const ChunkRec &c = chunks[i];
        if (c.state == kChunkHeap)
            continue;
        ++shown;
        out.Line("FR chunk id=%u st=%s q=%u used=%u words=%u gpu=0x%llx freed=%u reuse=%u\n", i,
                 ChunkStateName(c.state), c.seq, c.used, c.words, (unsigned long long)c.gpu, c.freedOpen, c.reuses);
    }
    const uint32_t s0 = segCount > kDumpSegs ? segCount - kDumpSegs : 0;
    for (uint32_t i = s0; i < segCount; ++i)
    {
        const SegRec &s = segs[i];
        // va, w0, h and fl in hex without 0x.
        out.Line("FR seg q=%u f=%u s=%u k=%u sl=%u t=%u fl=%x c=%u o=%u n=%u va=%08x w0=%08x h=%08x\n", s.seq,
                 s.frame, SegIndex(s.info), SegKind(s.info), SegSlot(s.info), SegTid(s.info), SegFlags(s.info),
                 RecChunk(s.chunkOff), RecOff(s.chunkOff), s.words, s.iova, s.first, s.hash);
    }
    const uint32_t e0 = evCount > kDumpEvents ? evCount - kDumpEvents : 0;
    for (uint32_t i = e0; i < evCount; ++i)
    {
        const ChunkEv &e = evs[i];
        out.Line("FR ev e=%s q=%u done=%u c=%u t=%u a=0x%x\n", EvName(EvKind(e.info)), e.seq, e.completed,
                 EvChunk(e.info), EvTid(e.info), e.aux);
    }
    out.Line("FR end reason=%s dropped=%u\n", h.reason, out.Dropped());
}

// ---- flightrec.bin ---------------------------------------------------------------

// File layout: BinHeader, then segCount SegRec, evCount ChunkEv and
// chunkCount ChunkRec, all oldest first, little endian.
struct BinHeader
{
    char magic[4]; // "FRB1"
    uint32_t version;
    uint32_t headerBytes;
    uint32_t segCount, evCount, chunkCount;
    uint32_t segPushed, evPushed;
    uint32_t poison, queueError; // queueError: 0, 1, or 0xffffffff not checked
    uint64_t open, submitted, completed, frames;
    uint64_t timeMs;
};
static_assert(sizeof(BinHeader) == 80, "flightrec.bin header");

} // namespace fr
} // namespace deko9
