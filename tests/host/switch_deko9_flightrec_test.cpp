// Host test (ASan/UBSan, then TSan) for the deko9 command flight recorder
// (src/deko9/deko9_flightrec.h): ring wraparound and torn-slot rejection,
// concurrent writers against a lock-free reader, record layout, the poison
// tag encode/decode round trip and its invalidity as a pushbuffer word for
// every chunk id and offset, the segment-start check, the captured-stream
// walk, quarantined chunk reuse, the fault verdict, and the dump size
// bound, the command-chunk guard, the image sentinels and the
// one-buffer-per-call vertex binding rule (with a source gate). Every
// property has a negative control. Run by ./test host
// (deko9_flightrec_sanitizer_check); `--concurrency` runs only the threaded
// part (the TSan build).

#include "src/deko9/deko9_flightrec.h"
#include "src/deko9/deko9_vtxbind.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace deko9
{
namespace fr
{
struct RingTestAccess
{
    // A writer descheduled after marking the slot busy and storing one word.
    template <class Rec, uint32_t N>
    static void TearSlot(Ring<Rec, N> &ring, uint32_t idx, uint32_t word, uint32_t value)
    {
        auto &slot = ring.m_slots[idx & (N - 1)];
        slot.stamp.store(0, std::memory_order_relaxed);
        slot.w[word].store(value, std::memory_order_relaxed);
    }
    // What a reader without the stamp check would return.
    template <class Rec, uint32_t N>
    static Rec ReadWordsOnly(const Ring<Rec, N> &ring, uint32_t idx)
    {
        uint32_t w[sizeof(Rec) / 4];
        const auto &slot = ring.m_slots[idx & (N - 1)];
        for (uint32_t i = 0; i < sizeof(Rec) / 4; ++i)
            w[i] = slot.w[i].load(std::memory_order_relaxed);
        Rec r;
        std::memcpy(&r, w, sizeof(r));
        return r;
    }
};
} // namespace fr
} // namespace deko9

namespace
{

using namespace deko9::fr;

int g_failures;

void Check(bool ok, const char *name)
{
    if (!ok)
    {
        std::printf("FAIL:DEKO9_FLIGHTREC %s\n", name);
        ++g_failures;
    }
}

SegRec Seg(uint32_t v)
{
    // Every word derives from v, so a record mixed from two writes shows.
    return {v, v ^ 0x11111111u, v * 3u, v + 7u, v ^ 0xa5a5a5a5u, v * 5u, ~v, v * 0x9e3779b9u};
}

bool Consistent(const SegRec &r)
{
    const SegRec want = Seg(r.seq);
    return std::memcmp(&r, &want, sizeof(r)) == 0;
}

void TestRingWrap()
{
    auto ring = std::make_unique<Ring<SegRec, 64>>();
    SegRec out[128];
    Check(ring->Newest(out, 128) == 0, "ring: empty");
    for (uint32_t i = 0; i < 10; ++i)
        ring->Push(Seg(i));
    uint32_t n = ring->Newest(out, 128);
    Check(n == 10 && out[0].seq == 0 && out[9].seq == 9, "ring: partial fill oldest first");
    for (uint32_t i = 10; i < 1000; ++i)
        ring->Push(Seg(i));
    n = ring->Newest(out, 128);
    Check(n == 64 && out[0].seq == 936 && out[63].seq == 999, "ring: wrapped keeps the newest 64 in order");
    n = ring->Newest(out, 5);
    Check(n == 5 && out[0].seq == 995 && out[4].seq == 999, "ring: newest subset");
    SegRec r;
    Check(!ring->Read(100, &r), "ring: an overwritten index is gone");
    Check(ring->Read(999, &r) && r.seq == 999 && Consistent(r), "ring: newest index readable");
    // Negative control: without the stamp the overwritten slot reads as the
    // newer record that replaced it.
    const SegRec stale = RingTestAccess::ReadWordsOnly(*ring, 100);
    Check(stale.seq != 100, "ring control: a words-only read returns another record for an old index");
    // A slot caught mid-write is skipped, never returned half-written.
    RingTestAccess::TearSlot(*ring, 998, 3, 0xdeadbeef);
    Check(!ring->Read(998, &r), "ring: torn slot rejected");
    n = ring->Newest(out, 3);
    Check(n == 2 && out[0].seq == 997 && out[1].seq == 999, "ring: Newest skips the torn slot");
    const SegRec torn = RingTestAccess::ReadWordsOnly(*ring, 998);
    Check(!Consistent(torn), "ring control: the torn slot's words are inconsistent");
    Check(ring->Head() == 1000, "ring: push count");
}

// Writers on every thread at once (the device's recording thread plus any
// thread that records an event) and a reader that never takes a lock.
void TestConcurrent(uint32_t perThread)
{
    auto ring = std::make_unique<Ring<SegRec, 256>>();
    auto evs = std::make_unique<Ring<ChunkEv, 64>>();
    ThreadTable threads;
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> bad{0}, reads{0};
    std::thread reader([&] {
        std::vector<SegRec> out(256);
        std::vector<ChunkEv> eo(64);
        while (!stop.load(std::memory_order_acquire))
        {
            const uint32_t n = ring->Newest(out.data(), 256);
            for (uint32_t i = 0; i < n; ++i)
                if (!Consistent(out[i]))
                    bad.fetch_add(1);
            const uint32_t m = evs->Newest(eo.data(), 64);
            for (uint32_t i = 0; i < m; ++i)
                if (eo[i].aux != eo[i].seq * 3u)
                    bad.fetch_add(1);
            reads.fetch_add(n + m);
        }
    });
    std::vector<std::thread> writers;
    std::atomic<uint32_t> idsSeen[ThreadTable::kSlots] = {};
    for (uint32_t t = 0; t < 4; ++t)
        writers.emplace_back([&, t] {
            static thread_local char tag;
            const uint8_t id = threads.Index((uintptr_t)&tag);
            idsSeen[id].fetch_add(1);
            for (uint32_t i = 0; i < perThread; ++i)
            {
                const uint32_t v = t * perThread + i;
                ring->Push(Seg(v));
                evs->Push({v, 0, EvInfo(t, kEvFree, id), v * 3u});
            }
        });
    for (auto &w : writers)
        w.join();
    stop.store(true, std::memory_order_release);
    reader.join();
    Check(bad.load() == 0, "concurrent: every record the reader accepted is whole");
    Check(ring->Head() == 4 * perThread && evs->Head() == 4 * perThread, "concurrent: every push counted");
    uint32_t distinct = 0;
    for (uint32_t i = 1; i < ThreadTable::kSlots; ++i)
        distinct += idsSeen[i].load() == 1;
    Check(distinct == 4 && idsSeen[0].load() == 0, "concurrent: four writers, four distinct thread ids");
    SegRec out[256];
    const uint32_t n = ring->Newest(out, 256);
    bool whole = n == 256;
    for (uint32_t i = 0; i < n; ++i)
        whole &= Consistent(out[i]);
    Check(whole, "concurrent: after the writers stop the ring holds 256 whole records");
}

void TestLayout()
{
    static_assert(offsetof(SegRec, seq) == 0 && offsetof(SegRec, first) == 24 && offsetof(SegRec, hash) == 28,
                  "SegRec field offsets");
    static_assert(offsetof(ChunkEv, aux) == 12 && offsetof(ChunkRec, reuses) == 28 &&
                      offsetof(BinHeader, open) == 40 && offsetof(BinHeader, timeMs) == 72,
                  "binary offsets");
    const uint32_t info = SegInfo(255, 15, 15, 255, 255);
    Check(SegIndex(info) == 255 && SegKind(info) == 15 && SegSlot(info) == 15 && SegTid(info) == 255 &&
              SegFlags(info) == 255,
          "layout: segment info fields at their maximum round-trip");
    const uint32_t mid = SegInfo(7, kKindPresent, 9, 3, kSegBadFirst);
    Check(SegIndex(mid) == 7 && SegKind(mid) == kKindPresent && SegSlot(mid) == 9 && SegTid(mid) == 3 &&
              SegFlags(mid) == kSegBadFirst,
          "layout: segment info round trip");
    const uint32_t co = ChunkOff(1022, (1u << 22) - 1);
    Check(RecChunk(co) == 1022 && RecOff(co) == (1u << 22) - 1, "layout: chunk/offset round trip");
    const uint32_t ei = EvInfo(0xffff, kEvTail, 31);
    Check(EvChunk(ei) == 0xffff && EvKind(ei) == kEvTail && EvTid(ei) == 31, "layout: event info round trip");
    // Negative control: a field past its width is masked, not spilled.
    Check(SegKind(SegInfo(256, 1, 0, 0, 0)) == 1 && SegIndex(SegInfo(256, 1, 0, 0, 0)) == 0,
          "layout control: an oversize segment index wraps inside its own field");
    Check(std::string(EvName(kEvHash)) == "hash" && std::string(EvName(200)) == "?" &&
              std::string(ChunkStateName(kChunkFree)) == "free",
          "layout: names");
}

void TestPoison()
{
    // Every chunk id and every offset of a 256 KiB chunk (shift 0).
    uint64_t bad = 0;
    for (uint32_t chunk = 0; chunk < kMaxChunkIds; ++chunk)
        for (uint32_t off = 0; off < 65536; ++off)
        {
            const uint32_t a = PoisonTagA(chunk, off, 0), b = PoisonTagB(chunk, off, 0);
            const PoisonTag da = DecodePoison(a), db = DecodePoison(b);
            bad += !(da.tag && !da.tail && da.chunk == chunk && da.field == off);
            bad += !(db.tag && db.tail && db.chunk == chunk && db.field == off);
            bad += ClassifyPbWord(a) != PbWord::Obsolete || ClassifyPbWord(b) != PbWord::Obsolete;
            bad += ValidSegmentStart(a) || ValidSegmentStart(b);
        }
    Check(bad == 0, "poison: tags A and B round-trip and are invalid words for all 1023 ids x 65536 offsets");
    // Larger chunks store offset >> shift.
    Check(PoisonShift(65536) == 0 && PoisonShift(65537) == 1 && PoisonShift(1u << 20) == 4, "poison: shift");
    const uint32_t t = PoisonTagA(5, 1000000, PoisonShift(1u << 20));
    Check(DecodePoison(t).chunk == 5 && (DecodePoison(t).field << 4) == (1000000u & ~15u), "poison: 4 MiB offset");
    // Documented decode, spelled out.
    const uint32_t s = PoisonTagB(41, 0x1234, 0);
    Check(((s >> 18) & 0x3ff) - 1 == 41 && (s & 0xffff) == 0x1234 && ((s >> 28) & 1) == 1, "poison: decode formula");
    // Fill writes the per-word tag.
    std::vector<uint32_t> words(300, 0x20001234u);
    FillTag(words.data(), 3, 10, 20, 0, false);
    FillTag(words.data(), 3, 20, 20 + kTailWords, 0, true);
    Check(words[9] == 0x20001234u && words[10] == PoisonTagA(3, 10, 0) && words[19] == PoisonTagA(3, 19, 0) &&
              words[20] == PoisonTagB(3, 20, 0) && words[83] == PoisonTagB(3, 83, 0) && words[84] == 0x20001234u,
          "poison: fill covers exactly [from, to)");
    // Negative controls: without the +1 chunk 0 offset 0 is the NOP (valid),
    // and a tag with TERT_OP bits set is a control entry (valid).
    const uint32_t noPlusOne = (0u << 18) | 0u;
    Check(ClassifyPbWord(noPlusOne) == PbWord::Nop && !DecodePoison(noPlusOne).tag,
          "poison control: chunk id without +1 gives the valid NOP");
    Check(ClassifyPbWord(PoisonTagA(2, 3, 0) | (1u << 16)) == PbWord::Control,
          "poison control: a tag with TERT_OP set would be a valid control entry");
    Check(!DecodePoison(0x20010001u).tag && !DecodePoison(0x00030001u).tag, "poison: method/control words are no tag");
}

// Two command chunks back to back in one memblock, as the POOL_CMD heap hands
// them out. A writer that runs k words past chunk A's command area must hit
// A's guard and leave chunk B (another list, maybe still on the GPU) intact.
void TestGuard()
{
    uint64_t bad = 0;
    for (uint32_t i = 0; i < kGuardWords; ++i)
        bad += DecodePoison(GuardWord(i)).tag || ValidSegmentStart(GuardWord(i));
    Check(bad == 0, "guard: pattern is neither a poison tag nor a valid segment start");

    const uint32_t cmdWords = 256;
    const uint32_t bWord0 = 0x20020701u; // a method header, as at the start of a real segment
    for (uint32_t k : {1u, 7u, kGuardWords})
    {
        std::vector<uint32_t> mem(2 * (cmdWords + kGuardWords), 0x20001234u);
        uint32_t *a = mem.data(), *b = mem.data() + cmdWords + kGuardWords;
        FillGuard(a + cmdWords);
        b[0] = bWord0;
        for (uint32_t i = 0; i < k; ++i)
            a[cmdWords + i] = 0x523dffffu; // the overrun
        uint32_t first = 0;
        const uint32_t dirty = CheckGuard(a + cmdWords, &first);
        Check(dirty == k && first == 0 && b[0] == bWord0, "guard: overrun lands in the guard, next chunk intact");
        FillGuard(a + cmdWords);
        Check(CheckGuard(a + cmdWords, &first) == 0 && first == kGuardWords, "guard: refill clears it");
    }
    // Negative control: without the guard the same one-word overrun replaces
    // the next chunk's first word, which is the CMD_OVERWRITE signature.
    std::vector<uint32_t> bare(2 * cmdWords, 0x20001234u);
    bare[cmdWords] = bWord0;
    bare[cmdWords + 0] = 0x523dffffu;
    Check(bare[cmdWords] != bWord0 && !ValidSegmentStart(bare[cmdWords]),
          "guard control: without a guard the overrun corrupts the next chunk's first word");
}

// A GPU overrun writes whole 512-byte GOBs; one sample per 128 words must see
// any single GOB anywhere in the sentinel.
void TestSentinel()
{
    const uint32_t words = kSentinelBytes / 4;
    std::vector<uint32_t> s(words);
    FillSentinel(s.data(), words);
    uint32_t first = 0;
    Check(CheckSentinel(s.data(), words, &first) == 0 && first == words, "sentinel: clean after fill");
    uint64_t missed = 0;
    for (uint32_t gob = 0; gob < words / 128; gob += 37)
    {
        for (uint32_t i = 0; i < 128; ++i)
            s[gob * 128 + i] = 0x523dffffu;
        missed += CheckSentinel(s.data(), words, &first) == 0 || first != gob * 128;
        FillSentinel(s.data(), words);
    }
    Check(missed == 0, "sentinel: every sampled 512-byte GOB write is seen at its offset");
    bool nonePattern = true;
    for (uint32_t i = 0; i < 4096; ++i)
        nonePattern &= !DecodePoison(SentinelWord(i)).tag && !ValidSegmentStart(SentinelWord(i));
    Check(nonePattern, "sentinel: pattern is neither a poison tag nor a valid segment start");
    // Negative control: sampling every 256 words misses a GOB in an odd slot.
    for (uint32_t i = 0; i < 128; ++i)
        s[128 + i] = 0x523dffffu;
    uint32_t coarseDirty = 0;
    for (uint32_t i = 0; i < words; i += 256)
        coarseDirty += s[i] != SentinelWord(i);
    Check(coarseDirty == 0 && CheckSentinel(s.data(), words, &first) == 1,
          "sentinel control: a 1 KiB stride misses a GOB the 512-byte stride catches");
}

// deko3d's multi-buffer vertex bind can write one word past its chunk; the
// one-buffer calls deko9 makes never can, for any space left in the chunk.
void TestVtxBind()
{
    uint32_t perBufferOverruns = 0, multiOverruns = 0, multiMax = 0;
    for (uint32_t freeWords = 0; freeWords <= 64; ++freeWords)
    {
        perBufferOverruns += deko9::VtxBindOverrun(freeWords, 1);
        for (uint32_t n = 2; n <= 4; ++n)
        {
            const uint32_t o = deko9::VtxBindOverrun(freeWords, n);
            multiOverruns += o != 0;
            multiMax = o > multiMax ? o : multiMax;
        }
    }
    Check(perBufferOverruns == 0, "vtxbind: a one-buffer bind never writes past its chunk");
    Check(deko9::VtxBindOverrun(11, 2) == 1 && multiOverruns > 0,
          "vtxbind control: a two-buffer bind with 11 words left writes 1 word past the chunk (the hardware hit)");
    Check(multiMax <= 3, "vtxbind: worst multi-buffer overrun is n-1 words");
    // Source gate: every vertex-buffer bind goes through deko9::BindVtxBuffers.
    uint32_t direct = 0;
    std::string where;
    for (const auto &e : std::filesystem::recursive_directory_iterator("src"))
    {
        if (!e.is_regular_file() || e.path().filename() == "deko9_vtxbind.h")
            continue;
        const auto ext = e.path().extension();
        if (ext != ".cpp" && ext != ".h" && ext != ".c")
            continue;
        std::ifstream in(e.path());
        std::stringstream ss;
        ss << in.rdbuf();
        if (ss.str().find("dkCmdBufBindVtxBuffers(") != std::string::npos)
        {
            ++direct;
            where += " " + e.path().string();
        }
    }
    if (direct)
        std::printf("direct dkCmdBufBindVtxBuffers calls:%s\n", where.c_str());
    Check(direct == 0, "vtxbind: no direct dkCmdBufBindVtxBuffers call outside deko9_vtxbind.h");
}

void TestSegmentStart()
{
    // deko3d headers (method 0:13, subchannel 13:16, count 16:29, mode 29:32).
    auto hdr = [](uint32_t mode, uint32_t count, uint32_t sub, uint32_t method) {
        return (method & 0x1fff) | ((sub & 7) << 13) | ((count & 0x1fff) << 16) | ((mode & 7) << 29);
    };
    bool valid = true;
    for (uint32_t mode : {1u, 3u, 4u, 5u})
        for (uint32_t sub : {0u, 1u, 2u, 3u, 4u, 6u})
            valid &= ValidSegmentStart(hdr(mode, 2, sub, 0x45));
    Check(valid, "segment start: every deko3d mode on every bound subchannel");
    // The four PBENTRY shadows (FIRST=1) and the subchannel-5 one.
    for (uint32_t w : {0x00000100u, 0x1d0400ffu, 0x007ca4ffu, 0x00000005u})
        Check(!ValidSegmentStart(w) && ClassifyPbWord(w) == PbWord::Obsolete, "segment start: ERPT shadow rejected");
    Check(!ValidSegmentStart(hdr(1, 1, 5, 0x10)) && !ValidSegmentStart(hdr(1, 1, 7, 0x10)),
          "segment start: software subchannels 5 and 7 rejected");
    Check(!ValidSegmentStart(0) && !ValidSegmentStart(hdr(7, 0, 0, 0)) && !ValidSegmentStart(hdr(2, 1, 0, 1)) &&
              !ValidSegmentStart(hdr(6, 1, 0, 1)),
          "segment start: NOP, END_SEG, SEC_OP 2 and 6 rejected");
    // Negative control: the method-header class alone accepts subchannel 5
    // (the 51289e66 shadow decoded on it), so the subchannel test matters.
    Check(ClassifyPbWord(hdr(1, 1, 5, 0x10)) == PbWord::Method, "segment start control: SEC_OP alone accepts subchannel 5");

    // Captured-stream walk.
    const uint32_t ok[] = {hdr(1, 2, 0, 0x10), 1, 2, hdr(4, 7, 0, 0x20), hdr(3, 1, 6, 0x4), 9, hdr(5, 3, 0, 0x30), 1, 2, 3};
    Check(WalkWords(ok, 10).ok, "walk: INC/IMMD/NON_INC/INC_ONCE stream parses to the end");
    Check(WalkWords(ok, 0).ok, "walk: empty capture");
    const WalkResult cut = WalkWords(ok, 9);
    Check(!cut.ok && cut.at == 6, "walk: a stream cut inside a payload fails at that header");
    const uint32_t split[] = {2, 3, hdr(1, 1, 0, 0x10), 4}; // capture restarted mid-command
    const WalkResult s = WalkWords(split, 4);
    Check(!s.ok && s.at == 0 && s.word == 2, "walk: a payload word where a header belongs fails");
    const uint32_t wrap[] = {0x38200000u}; // INC count 0x1820 with no payload (09-30 shadow)
    Check(ValidSegmentStart(wrap[0]) && !WalkWords(wrap, 1).ok, "walk: the count-wrap shadow fails the walk");
}

void TestQuarantine()
{
    struct Free
    {
        uint64_t freedOpen;
        uint32_t size;
    };
    std::vector<Free> free = {{10, 256}, {11, 256}, {12, 128}, {12, 256}};
    auto pick = [&](uint64_t open, uint32_t q, uint32_t size) {
        return PickReuse(
            free.size(), open, q, [&](size_t i) { return free[i].freedOpen; },
            [&](size_t i) { return free[i].size == size; });
    };
    Check(pick(12, 2, 256) == 0, "quarantine: oldest chunk out of quarantine first");
    Check(pick(11, 2, 256) == -1, "quarantine: nothing reused before two lists passed");
    Check(pick(13, 2, 256) == 0 && pick(14, 2, 128) == 2, "quarantine: size filter");
    Check(pick(12, 0, 256) == 3, "no quarantine: most recently freed (the back), as before");
    Check(pick(12, 0, 128) == -1, "no quarantine: a mismatched back is not reused");
    // Property over a random free/reuse history: a chunk is never handed out
    // fewer than two lists after it was freed, and FIFO order holds.
    std::mt19937 rng(7);
    std::vector<Free> pool;
    uint64_t open = 1;
    uint32_t violations = 0, fifoBreaks = 0, controlViolations = 0;
    for (int step = 0; step < 20000; ++step)
    {
        if (rng() % 3 == 0)
            pool.push_back({open, 256});
        if (rng() % 2 == 0)
            ++open;
        const int i = PickReuse(
            pool.size(), open, 2, [&](size_t k) { return pool[k].freedOpen; },
            [&](size_t k) { return pool[k].size == 256; });
        const int lifo = PickReuse(
            pool.size(), open, 0, [&](size_t k) { return pool[k].freedOpen; },
            [&](size_t k) { return pool[k].size == 256; });
        if (lifo >= 0 && pool[(size_t)lifo].freedOpen + 2 > open)
            ++controlViolations;
        if (i < 0)
            continue;
        violations += pool[(size_t)i].freedOpen + 2 > open;
        for (int k = 0; k < i; ++k)
            fifoBreaks += pool[(size_t)k].freedOpen + 2 <= open;
        pool.erase(pool.begin() + i);
    }
    Check(violations == 0 && fifoBreaks == 0, "quarantine: random history never reuses early, always oldest");
    Check(controlViolations > 0, "quarantine control: LIFO reuse hands out chunks freed in the current list");
}

void TestOwnership()
{
    const uintptr_t backend = 0x1000, main = 0x2000;
    Check(!ForeignRecord(backend, backend, 0), "owner: lock holder records");
    Check(!ForeignRecord(backend, backend, backend), "owner: lock holder records into its own capture");
    Check(ForeignRecord(main, backend, 0), "owner: recording while another thread holds the lock");
    Check(ForeignRecord(main, 0, 0), "owner: recording with the lock free (inside an unlocked wait)");
    Check(ForeignRecord(main, main, backend), "owner: recording into another thread's capture");
    // Negative control: the previous rule (only the submitter is checked)
    // passes a draw recorded by main while the back end holds the lock.
    auto submitOnly = [](uintptr_t self, uintptr_t submitOwner) { return self != submitOwner; };
    Check(!submitOnly(main, main) && ForeignRecord(main, backend, 0),
          "owner control: a submit-owner check misses a recording without the lock");
}

void TestVerdict()
{
    ChunkRec chunks[4] = {};
    for (auto &c : chunks)
        c.words = 65536;
    SegRec segs[2] = {};
    segs[0].info = SegInfo(0, kKindDraw, 1, 1, 0);
    segs[0].first = 0x20012345u;
    segs[1].info = SegInfo(1, kKindDraw, 1, 1, kSegBadFirst);
    segs[1].first = 0x00000005u;
    segs[1].chunkOff = ChunkOff(2, 40);
    ChunkEv evs[2] = {};
    evs[0].info = EvInfo(1, kEvFree, 1);
    evs[1].info = EvInfo(3, kEvHash, 1);

    VerdictResult r = Classify(PoisonTagA(2, 700, 0), true, chunks, 4, segs, 1, evs, 1);
    Check(r.verdict == Verdict::TagA && r.chunk == 2 && r.offset == 700, "verdict: tag A names chunk and offset");
    r = Classify(PoisonTagB(3, 9, 0), true, chunks, 4, segs, 1, evs, 1);
    Check(r.verdict == Verdict::TagB && r.chunk == 3 && r.offset == 9, "verdict: tag B");
    r = Classify(0x5, true, chunks, 4, segs, 2, evs, 1);
    Check(r.verdict == Verdict::BadFirst && r.exact && r.chunk == 2 && r.offset == 40,
          "verdict: shadow equals a first word flagged at submit");
    r = Classify(0x1d0400ffu, false, chunks, 4, segs, 1, evs, 2);
    Check(r.verdict == Verdict::HashChanged && r.chunk == 3, "verdict: hash changed after submit");
    r = Classify(0x100, true, chunks, 4, segs, 1, evs, 1);
    Check(r.verdict == Verdict::UnknownStale, "verdict: nothing matches -> unknown/stale");
    Check(std::string(VerdictName(Verdict::TagA)) == "TAG_A_FREED_CHUNK_FETCHED" &&
              std::string(VerdictName(Verdict::UnknownStale)) == "UNKNOWN_STALE",
          "verdict: names");
    // Negative controls: the same tag-shaped word without poison, for a
    // chunk the recorder does not know, or past the chunk's end is no tag.
    r = Classify(PoisonTagA(2, 700, 0), false, chunks, 4, segs, 1, evs, 1);
    Check(r.verdict == Verdict::UnknownStale, "verdict control: tag shape with poison off is not a tag");
    r = Classify(PoisonTagA(9, 700, 0), true, chunks, 4, segs, 1, evs, 1);
    Check(r.verdict == Verdict::UnknownStale, "verdict control: unknown chunk id is not a tag");
    chunks[2].words = 512;
    r = Classify(PoisonTagA(2, 700, 0), true, chunks, 4, segs, 1, evs, 1);
    Check(r.verdict == Verdict::UnknownStale, "verdict control: offset past the chunk is not a tag");
}

// The end-to-end shape of a premature retire on host memory: a chunk freed
// and poisoned, then "fetched" at a segment start.
void TestLifecycleModel()
{
    ChunkTable table;
    std::vector<uint32_t> mem(2 * 1024, 0);
    const uint32_t a = table.Id(0x500000000ull, mem.data(), 1024);
    const uint32_t b = table.Id(0x500001000ull, mem.data() + 1024, 1024);
    Check(a == 0 && b == 1 && table.Id(0x500000000ull, mem.data(), 1024) == 0, "model: ids by address");
    Check(table.Containing(0x500000010ull) == kNoChunk, "model: heap chunks are not live");
    table.Set(a, kChunkOpen, 7);
    Check(table.Containing(0x500000010ull) == a, "model: containing a live chunk");
    // List 7 writes 300 words; at submit tag B goes after them.
    for (uint32_t i = 0; i < 300; ++i)
        mem[i] = 0x20010000u | i;
    const uint32_t hash = HeadHash(mem.data(), 300);
    FillTag(mem.data(), a, 300, 300 + kTailWords, 0, true);
    // The GPU running past word 300 reads tag B.
    ChunkRec recs[2] = {table.Get(a), table.Get(b)};
    VerdictResult r = Classify(mem[300], true, recs, 2, nullptr, 0, nullptr, 0);
    Check(r.verdict == Verdict::TagB && r.chunk == a && r.offset == 300, "model: run past the end -> tag B");
    Check(HeadHash(mem.data(), 300) == hash, "model: tag B left the written words alone");
    // Overwritten after submit: the retire re-hash differs.
    mem[1] = 0x12345678u;
    Check(HeadHash(mem.data(), 300) != hash, "model: overwrite after submit changes the head hash");
    // Freed and poisoned, then fetched at a segment start (word 128).
    FillTag(mem.data(), a, 0, 300, 0, false);
    r = Classify(mem[128], true, recs, 2, nullptr, 0, nullptr, 0);
    Check(r.verdict == Verdict::TagA && r.chunk == a && r.offset == 128, "model: freed chunk fetched -> tag A");
    Check(mem[1024] == 0, "model: the neighbouring chunk untouched");
}

void TestDumpBound()
{
    // Worst case: every number at its widest.
    std::vector<SegRec> segs(kDumpSegs + 50);
    for (auto &s : segs)
        s = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    std::vector<ChunkEv> evs(kDumpEvents + 50, ChunkEv{0xffffffffu, 0xffffffffu, EvInfo(0xffff, kEvBusyError, 255), 0xffffffffu});
    std::vector<ChunkRec> chunks(ChunkTable::kEntries,
                                 ChunkRec{~0ull, 0xffffffffu, kChunkBusy, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu});
    ThreadTable threads;
    for (uintptr_t t = 1; t < 64; ++t)
        threads.Index(~(uintptr_t)0 - t);
    DumpHeader h;
    h.reason = "watchdog_1s";
    h.open = h.submitted = h.completed = h.frames = ~0ull;
    h.queueError = 1;
    h.poison = 1;
    h.chunkBytes = 4u << 20;
    h.writerTid = h.lockTid = h.submitTid = h.captureTid = 31;
    h.segPushed = h.evPushed = h.badFirst = h.hashChanged = h.foreign = 0xffffffffu;
    std::vector<char> buf(kDumpCap);
    TextOut out(buf.data(), buf.size());
    FormatDump(out, h, segs.data(), (uint32_t)segs.size(), evs.data(), (uint32_t)evs.size(), chunks.data(),
               (uint32_t)chunks.size(), &threads);
    std::printf("DEKO9_FLIGHTREC dump worst case %zu bytes (cap %zu)\n", out.Size(), kDumpCap);
    Check(out.Dropped() == 0 && out.Size() < kDumpCap && std::strstr(buf.data(), "FR end reason=watchdog_1s"),
          "dump: worst case fits 15 KB with nothing dropped");
    // Golden line shapes.
    SegRec one = {1234, 600, SegInfo(2, kKindPresent, 3, 1, 0), ChunkOff(5, 128), 64, 0x00401000u, 0x20015a00u, 0xabcdef01u};
    ChunkEv ev = {1233, 1230, EvInfo(5, kEvPoison, 2), 300};
    ChunkRec c = {0x500000000ull, 65536, kChunkFree, 1230, 300, 1236, 4};
    std::vector<char> small(2048);
    TextOut o2(small.data(), small.size());
    DumpHeader h2;
    h2.reason = "test";
    FormatDump(o2, h2, &one, 1, &ev, 1, &c, 1, nullptr);
    const std::string text(small.data());
    Check(text.find("FR seg q=1234 f=600 s=2 k=2 sl=3 t=1 fl=0 c=5 o=128 n=64 va=00401000 w0=20015a00 "
                    "h=abcdef01\n") != std::string::npos,
          "dump: segment line golden");
    Check(text.find("FR ev e=poison q=1233 done=1230 c=5 t=2 a=0x12c\n") != std::string::npos, "dump: event golden");
    Check(text.find("FR chunk id=0 st=free q=1230 used=300 words=65536 gpu=0x500000000 freed=1236 reuse=4\n") !=
              std::string::npos,
          "dump: chunk golden");
    // Negative control: a buffer too small drops lines and never overruns.
    std::vector<char> tiny(256, 'x');
    TextOut o3(tiny.data(), 200);
    FormatDump(o3, h, segs.data(), (uint32_t)segs.size(), evs.data(), (uint32_t)evs.size(), chunks.data(),
               (uint32_t)chunks.size(), &threads);
    Check(o3.Dropped() > 0 && o3.Size() < 200 && tiny[200] == 'x', "dump control: small cap drops, stays in bounds");
}

// TSan negative control: the same writer/reader pair on a plain POD ring
// (the shape of the fault trace's segment ring) must be reported as a race.
// Turns alternate through a relaxed atomic (no happens-before for TSan), so
// the accesses overlap on every run whatever the scheduling.
volatile uint32_t g_plainSink;
int PlainRingRace()
{
    constexpr int kRounds = 64;
    static SegRec plain[64];
    std::atomic<int> turn{0}; // even: the reader, odd: the writer
    auto waitTurn = [&](int want) {
        while (turn.load(std::memory_order_relaxed) != want)
            std::this_thread::yield();
        std::atomic_signal_fence(std::memory_order_seq_cst);
    };
    std::thread writer([&] {
        for (int r = 0; r < kRounds; ++r)
        {
            waitTurn(2 * r + 1);
            plain[r & 63].seq = (uint32_t)r + 1;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            turn.store(2 * r + 2, std::memory_order_relaxed);
        }
    });
    for (int r = 0; r < kRounds; ++r)
    {
        waitTurn(2 * r);
        g_plainSink = plain[r & 63].seq;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        turn.store(2 * r + 1, std::memory_order_relaxed);
    }
    waitTurn(2 * kRounds);
    g_plainSink = plain[0].seq;
    writer.join();
    std::printf("plain ring race ran %d rounds (TSan must have reported it)\n", kRounds);
    return 0;
}

// Fixture for the flight recorder's host-side decoder: the
// same scenario as text dump and flightrec.bin, written with the device's
// record structs, so the decoder is checked against this layout.
// Chunk 5 (256 KiB) was freed and poisoned; the fault shadow is its tag A at
// word 128: the decoder must answer TAG_A chunk=5 offset=128.
int EmitFixture(const std::string &dir)
{
    std::vector<ChunkRec> chunks(6);
    for (uint32_t i = 0; i < 6; ++i)
        chunks[i] = {0x500000000ull + i * 0x40000ull, 65536, i == 5 ? (uint32_t)kChunkFree : (uint32_t)kChunkBusy,
                     1200 + i, 300 + i, i == 5 ? 1206u : 0u, i};
    std::vector<SegRec> segs;
    for (uint32_t i = 0; i < 10; ++i)
        segs.push_back({1200 + i / 2, 600, SegInfo(i % 2, kKindDraw, (1200 + i / 2) % 16, 1, 0),
                        ChunkOff(i / 2, (i % 2) * 64), 64, 0x00400000u + i * 0x100, 0x20015a00u + i, 0x1000u + i});
    std::vector<ChunkEv> evs = {{1205, 1205, EvInfo(5, kEvFree, 1), 1206}, {1205, 1205, EvInfo(5, kEvPoison, 1), 305},
                                {1206, 1205, EvInfo(4, kEvBusy, 1), 304}};
    DumpHeader h;
    h.reason = "fixture";
    h.open = 1207;
    h.submitted = 1206;
    h.completed = 1205;
    h.frames = 600;
    h.queueError = 1;
    h.poison = 1;
    h.chunkBytes = 256u << 10;
    std::vector<char> text(kDumpCap);
    TextOut out(text.data(), text.size());
    FormatDump(out, h, segs.data(), (uint32_t)segs.size(), evs.data(), (uint32_t)evs.size(), chunks.data(),
               (uint32_t)chunks.size(), nullptr);
    FILE *t = std::fopen((dir + "/fr_fixture.txt").c_str(), "wb");
    if (!t)
        return 1;
    std::fprintf(t, "[nxlink] unrelated line\n%s", text.data());
    std::fclose(t);
    BinHeader b{};
    std::memcpy(b.magic, "FRB1", 4);
    b.version = 1;
    b.headerBytes = sizeof(b);
    b.segCount = b.segPushed = (uint32_t)segs.size();
    b.evCount = b.evPushed = (uint32_t)evs.size();
    b.chunkCount = (uint32_t)chunks.size();
    b.poison = 1;
    b.queueError = 1;
    b.open = 1207;
    b.submitted = 1206;
    b.completed = 1205;
    b.frames = 600;
    FILE *f = std::fopen((dir + "/fr_fixture.bin").c_str(), "wb");
    if (!f)
        return 1;
    std::fwrite(&b, sizeof(b), 1, f);
    std::fwrite(segs.data(), sizeof(SegRec), segs.size(), f);
    std::fwrite(evs.data(), sizeof(ChunkEv), evs.size(), f);
    std::fwrite(chunks.data(), sizeof(ChunkRec), chunks.size(), f);
    std::fclose(f);
    std::printf("fixture shadow=0x%08x\n", PoisonTagA(5, 128, 0));
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && std::string(argv[1]) == "--plain-ring-race")
        return PlainRingRace();
    if (argc > 2 && std::string(argv[1]) == "--emit-fixture")
        return EmitFixture(argv[2]);
    const bool concurrencyOnly = argc > 1 && std::string(argv[1]) == "--concurrency";
    if (!concurrencyOnly)
    {
        TestRingWrap();
        TestLayout();
        TestPoison();
        TestGuard();
        TestSentinel();
        TestVtxBind();
        TestSegmentStart();
        TestQuarantine();
        TestOwnership();
        TestVerdict();
        TestLifecycleModel();
        TestDumpBound();
    }
    TestConcurrent(concurrencyOnly ? 20000 : 50000);
    if (g_failures)
    {
        std::printf("FAIL:DEKO9_FLIGHTREC failures=%d\n", g_failures);
        return 1;
    }
    std::printf("PASS:DEKO9_FLIGHTREC%s\n", concurrencyOnly ? " concurrency" : "");
    return 0;
}
