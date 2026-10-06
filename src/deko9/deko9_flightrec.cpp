// Command flight recorder of the deko3d renderer (always on): one record per
// GPFIFO segment submitted, one event per command-chunk transition, tagged
// poison of freed command memory (r_deko9CmdPoison) and the recording-thread
// check (r_deko9CmdOwnerCheck). Pure parts in deko9_flightrec.h.

#include <switch.h>

#include "deko9_internal.h"

#include <algorithm>
#include <cstdio>

#include <platform/switch/switch_port_log.h>

extern "C" int Switch_PortLogRingActive(void);

namespace deko9
{

namespace
{
constexpr uint32_t kSegRing = 4096;  // ~2.5 s of segments at ~25 per frame and 60 fps
constexpr uint32_t kEventRing = 1024;
constexpr uint32_t kMaxListSegments = 1024;
constexpr uint32_t kQuarantineLists = 2;
constexpr uint32_t kWriterEventsPerList = 4;
constexpr uint64_t kDumpIntervalNs = 5000000000ull;
constexpr uint32_t kMaxDumps = 32;

struct Recorder
{
    fr::Ring<fr::SegRec, kSegRing> segs;
    fr::Ring<fr::ChunkEv, kEventRing> evs;
    fr::ChunkTable chunks;
    fr::ThreadTable threads;
    std::atomic<uint32_t> frame{0};
    std::atomic<uint32_t> badFirst{0}, hashChanged{0}, foreign{0};
    std::atomic<bool> binaryPending{false};
    std::atomic<bool> earlyTest{false};
    // Dump budgets: [0] the stall watchdog, [1] the GPU stall watcher (which
    // also fires on long legitimate GPU work), so one cannot use up the other.
    std::atomic<uint32_t> dumps[2] = {};
    std::atomic<uint64_t> lastDumpNs[2] = {};
    std::atomic_flag dumping = ATOMIC_FLAG_INIT;
    // Cost, recording thread only (the heartbeat reads them racily).
    std::atomic<uint64_t> recordNs{0}, poisonNs{0}, poisonWords{0};
    std::atomic<bool> reportedThread{false}, reportedSegment{false}, reportedHash{false};
};

Recorder g_fr;
std::atomic<Device *> s_frDevice{nullptr};

uint8_t Tid()
{
    static thread_local uint8_t t_index = 0;
    if (!t_index)
        t_index = g_fr.threads.Index(ThreadTag());
    return t_index;
}

uint64_t NowNs() { return armTicksToNs(armGetSystemTick()); }

// Each line straight into the SD log ring (drained to the card file and the
// nxlink log host, and to svcOutputDebugString): it survives a process kill
// once flushed. Without the ring (self-test) stdout carries it.
void Emit(char *text)
{
    const bool ring = Switch_PortLogRingActive();
    if (!ring)
    {
        Port_Log(text);
        return;
    }
    char *line = text;
    while (*line)
    {
        char *end = std::strchr(line, '\n');
        if (!end)
        {
            Port_LogRaw(line);
            break;
        }
        const char keep = end[1];
        end[1] = 0;
        Port_LogRaw(line);
        end[1] = keep;
        line = end + 1;
    }
}

uint32_t ListKind(const Device::ListStats &l)
{
    if (l.present >= 0)
        return fr::kKindPresent;
    if (l.draws || l.clears)
        return fr::kKindDraw;
    if (l.uploads || l.blits)
        return fr::kKindUpload;
    return fr::kKindOther;
}
} // namespace

uint32_t Device::FrChunkId(const GpuAlloc &chunk)
{
    return g_fr.chunks.Id(chunk.gpu, chunk.cpu, chunk.size / 4);
}

void Device::FrEvent(uint32_t chunk, uint32_t ev, uint64_t seq, uint32_t aux)
{
    g_fr.evs.Push({(uint32_t)seq, (uint32_t)m_completedSeq, fr::EvInfo(chunk, ev, Tid()), aux});
}

void Device::RecordSlow(uintptr_t self)
{
    const uintptr_t owner = m_lock.OwnerTag();
    if (m_ownerCheck && fr::ForeignRecord(self, owner, m_captureTag))
    {
        g_fr.foreign.fetch_add(1, std::memory_order_relaxed);
        FrEvent(fr::kNoChunk, fr::kEvForeign, m_openSeq, g_fr.threads.Index(owner));
        if (!g_fr.reportedThread.exchange(true))
            Fail("CMD_THREAD",
                 "thread t%u (0x%llx) records into list seq=%llu while the device lock is %s%s; recording must hold "
                 "the device lock and never enter another thread's bake capture (later ones: FR foreign=)",
                 Tid(), (unsigned long long)self, (unsigned long long)m_openSeq,
                 owner ? "held by another thread" : "free",
                 m_captureTag && m_captureTag != self ? " during another thread's capture" : "");
    }
    if (self != m_recWriter)
    {
        // A few per list: threads may alternate between every draw (lock
        // hand-offs), which would flush the chunk history out of the ring.
        m_recWriter = self;
        if (m_frWriterEvents++ < kWriterEventsPerList)
            FrEvent(fr::kNoChunk, fr::kEvWriter, m_openSeq, g_fr.threads.Index(owner));
    }
}

void Device::FrNoteOpen(const GpuAlloc &chunk, uint32_t ev)
{
    const uint32_t id = FrChunkId(chunk);
    const fr::ChunkRec prev = g_fr.chunks.Get(id);
    g_fr.chunks.Set(id, fr::kChunkOpen, (uint32_t)m_openSeq);
    g_fr.chunks.SetUsed(id, 0);
    if (ev == fr::kEvReuse)
        g_fr.chunks.NoteReuse(id);
    FrEvent(id, ev, m_openSeq, ev == fr::kEvReuse ? (uint32_t)m_openSeq - prev.freedOpen : chunk.size / 4);
}

void Device::FrNoteBusy(const GpuAlloc &chunk, uint64_t seq)
{
    const uint32_t id = FrChunkId(chunk);
    g_fr.chunks.Set(id, fr::kChunkBusy, (uint32_t)(seq ? seq : m_openSeq));
    FrEvent(id, seq ? fr::kEvBusy : fr::kEvBusyError, seq ? seq : m_openSeq, g_fr.chunks.Get(id).used);
}

void Device::FrNoteFree(const GpuAlloc &chunk, uint64_t seq)
{
    const uint32_t id = FrChunkId(chunk);
    g_fr.chunks.Set(id, fr::kChunkFree, (uint32_t)seq);
    g_fr.chunks.SetFreed(id, (uint32_t)m_openSeq);
    FrEvent(id, fr::kEvFree, seq, (uint32_t)m_openSeq);
    if (m_cmdPoison)
        FrPoisonFreed(chunk);
}

void Device::FrNoteDrop(const GpuAlloc &chunk)
{
    const uint32_t id = FrChunkId(chunk);
    g_fr.chunks.Set(id, fr::kChunkHeap, (uint32_t)m_openSeq);
    FrEvent(id, fr::kEvDrop, m_openSeq, chunk.size / 4);
}

int Device::FrPickFree(uint32_t size)
{
    const auto &free = m_freeCmdChunks;
    return fr::PickReuse(
        free.size(), m_openSeq, m_cmdPoison ? kQuarantineLists : 0,
        [&](size_t i) { return (uint64_t)g_fr.chunks.Get(FrChunkId(free[i])).freedOpen; },
        [&](size_t i) { return free[i].size == size; });
}

void Device::FrSubmit(DkCmdList list, uint32_t slot)
{
    if (!list)
        return;
    const uint64_t t0 = NowNs();
    static CmdSegment segs[kMaxListSegments];
    const int32_t n = ListSegments(reinterpret_cast<const void *>(list), segs, kMaxListSegments);
    if (n < 0)
    {
        if (!g_fr.reportedSegment.exchange(true))
            Fail("CMD_SEGMENT", "seq=%llu: unknown deko3d control command; no segment records for this list",
                 (unsigned long long)m_openSeq);
        return;
    }
    const uint32_t count = std::min<uint32_t>((uint32_t)n, kMaxListSegments);
    for (const GpuAlloc &c : m_openCmdChunks)
        g_fr.chunks.SetUsed(FrChunkId(c), 0);
    const uint32_t kind = ListKind(Stats());
    const uint32_t frame = g_fr.frame.load(std::memory_order_relaxed);
    const uint8_t tid = Tid();
    uint32_t first = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const CmdSegment &g = segs[i];
        const uint64_t end = g.gpu + 4ull * g.words;
        uint32_t chunk = fr::kNoChunk, off = 0, w0 = 0, hash = 0, flags = fr::kSegNoChunk;
        for (const GpuAlloc &c : m_openCmdChunks)
        {
            const uint64_t cEnd = c.gpu + c.size;
            if (g.gpu >= cEnd || end <= c.gpu)
                continue;
            // A coalesced entry can run on into the adjacent chunk: each
            // chunk it covers counts its words as used.
            const uint32_t id = FrChunkId(c);
            const uint32_t usedEnd = (uint32_t)((std::min(end, cEnd) - c.gpu) / 4);
            if (usedEnd > g_fr.chunks.Get(id).used)
                g_fr.chunks.SetUsed(id, usedEnd);
            if (g.gpu < c.gpu || chunk != fr::kNoChunk)
                continue;
            chunk = id;
            off = (uint32_t)((g.gpu - c.gpu) / 4);
            const volatile uint32_t *cpu = reinterpret_cast<const volatile uint32_t *>(c.cpu) + off;
            w0 = cpu[0];
            hash = fr::HeadHash(cpu, g.words);
            flags = 0;
        }
        if (flags == 0 && !fr::ValidSegmentStart(w0))
        {
            flags |= fr::kSegBadFirst;
            g_fr.badFirst.fetch_add(1, std::memory_order_relaxed);
            FrEvent(chunk, fr::kEvBadFirst, m_openSeq, w0);
            if (!g_fr.reportedSegment.exchange(true))
                Fail("CMD_SEGMENT",
                     "seq=%llu segment %u starts with 0x%08x (chunk %u word %u, %u words): not a method header; "
                     "the GPU rejects or desyncs on it",
                     (unsigned long long)m_openSeq, i, w0, chunk, off, g.words);
        }
        const uint32_t idx = g_fr.segs.Push({(uint32_t)m_openSeq, frame, fr::SegInfo(i, kind, slot, tid, flags),
                                             fr::ChunkOff(chunk, off), g.words, (uint32_t)g.gpu, w0, hash});
        if (i == 0)
            first = idx;
    }
    m_frPending[m_openSeq % kFenceRing] = {m_openSeq, first, count};
    m_frWriterEvents = 0;
    FrEvent(count, fr::kEvSubmit, m_openSeq, slot);
    if (m_cmdPoison)
    {
        // Tag B after each chunk's last written word: never part of this
        // list's segments, and the chunk is not written again before it is
        // freed (the next list starts on a fresh chunk).
        for (const GpuAlloc &c : m_openCmdChunks)
        {
            const uint32_t id = FrChunkId(c);
            if (id == fr::kNoChunk)
                continue;
            const uint32_t words = c.size / 4, used = g_fr.chunks.Get(id).used;
            const uint32_t to = std::min(words, used + fr::kTailWords);
            fr::FillTag(reinterpret_cast<uint32_t *>(c.cpu), id, used, to, fr::PoisonShift(words), true);
            if (to > used)
                FrEvent(id, fr::kEvTail, m_openSeq, to - used);
        }
        if (g_fr.earlyTest.exchange(false))
            FrStallTest(m_openSeq);
    }
    else if (g_fr.earlyTest.exchange(false))
    {
        Log("FR test early_retire refused: r_deko9CmdPoison is 0");
    }
    g_fr.recordNs.fetch_add(NowNs() - t0, std::memory_order_relaxed);
}

void Device::FrStallTest(uint64_t seq)
{
    // TEST ONLY: treat the list's first chunk as retired one list early and
    // poison it before the GPU runs it, so the first segment starts on tag A.
    if (m_openCmdChunks.empty())
        return;
    const GpuAlloc &c = m_openCmdChunks.front();
    const uint32_t id = FrChunkId(c);
    if (id == fr::kNoChunk)
        return;
    const uint32_t words = c.size / 4, used = g_fr.chunks.Get(id).used;
    const uint32_t shift = fr::PoisonShift(words);
    fr::FillTag(reinterpret_cast<uint32_t *>(c.cpu), id, 0, used, shift, false);
    const uint32_t expect = fr::PoisonTagA(id, 0, shift);
    FrEvent(id, fr::kEvEarly, seq, expect);
    char line[192];
    std::snprintf(line, sizeof(line),
                  "FR test early_retire seq=%llu chunk=%u words=%u expect_shadow=0x%08x (the GPU should fault)\n",
                  (unsigned long long)seq, id, used, expect);
    Emit(line);
}

void Device::FrVerifyRetired()
{
    if (m_frVerified >= m_completedSeq)
        return;
    const uint64_t t0 = NowNs();
    FrVerifyLists();
    g_fr.recordNs.fetch_add(NowNs() - t0, std::memory_order_relaxed);
}

void Device::FrVerifyLists()
{
    if (m_completedSeq > m_frVerified + kFenceRing)
        m_frVerified = m_completedSeq - kFenceRing;
    while (m_frVerified < m_completedSeq)
    {
        const uint64_t seq = ++m_frVerified;
        FrPending &p = m_frPending[seq % kFenceRing];
        if (p.seq != seq)
            continue;
        for (uint32_t i = 0; i < p.count; ++i)
        {
            fr::SegRec r;
            if (!g_fr.segs.Read(p.first + i, &r) || r.seq != (uint32_t)seq ||
                (fr::SegFlags(r.info) & fr::kSegNoChunk))
                continue;
            const uint32_t chunk = fr::RecChunk(r.chunkOff);
            const uint32_t *base = g_fr.chunks.Cpu(chunk);
            if (!base)
                continue;
            const uint32_t now = fr::HeadHash(reinterpret_cast<const volatile uint32_t *>(base) + fr::RecOff(r.chunkOff),
                                              r.words);
            if (now == r.hash)
                continue;
            g_fr.hashChanged.fetch_add(1, std::memory_order_relaxed);
            FrEvent(chunk, fr::kEvHash, seq, fr::SegIndex(r.info));
            if (!g_fr.reportedHash.exchange(true))
            {
                const uint32_t *w = base + fr::RecOff(r.chunkOff);
                Fail("CMD_OVERWRITE",
                     "seq=%llu segment %u (chunk %u word %u of %u, %u words) changed between submit and retire: "
                     "hash 0x%08x -> 0x%08x, first word 0x%08x -> 0x%08x, now %08x %08x %08x %08x %08x %08x %08x "
                     "%08x; command memory was written while the GPU owned it",
                     (unsigned long long)seq, fr::SegIndex(r.info), chunk, fr::RecOff(r.chunkOff),
                     g_fr.chunks.Get(chunk).words, r.words, r.hash, now, r.first, w[0], w[0], w[1], w[2], w[3], w[4],
                     w[5], w[6], w[7]);
                FrDump("cmd_overwrite", false);
            }
        }
        p.seq = 0;
    }
}

void Device::FrPoisonFreed(const GpuAlloc &chunk)
{
    // The chunk's list completed (seq <= the published completed sequence),
    // so the GPU no longer reads it; the next list that takes it overwrites
    // the poison with its commands exactly as it would overwrite old ones.
    const uint32_t id = FrChunkId(chunk);
    if (id == fr::kNoChunk || !chunk.cpu)
        return;
    const uint64_t t0 = NowNs();
    const uint32_t words = chunk.size / 4;
    uint32_t used = g_fr.chunks.Get(id).used;
    if (!used || used > words)
        used = words;
    fr::FillTag(reinterpret_cast<uint32_t *>(chunk.cpu), id, 0, used, fr::PoisonShift(words), false);
    FrEvent(id, fr::kEvPoison, g_fr.chunks.Get(id).seq, used);
    g_fr.poisonNs.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    g_fr.poisonWords.fetch_add(used, std::memory_order_relaxed);
}

void Device::FrApplySettings()
{
    g_fr.frame.store((uint32_t)m_frames, std::memory_order_relaxed);
    const bool poison = m_cmdPoisonWanted.load(std::memory_order_relaxed);
    if (poison != m_cmdPoison)
    {
        m_cmdPoison = poison;
        Log("cmdpoison %s (freed command chunks: tag A, chunk tails: tag B, %u-list quarantine)",
            poison ? "on" : "off", poison ? kQuarantineLists : 0);
    }
    const bool check = m_ownerCheckWanted.load(std::memory_order_relaxed);
    if (check != m_ownerCheck)
    {
        m_ownerCheck = check;
        Log("cmdownercheck %s", check ? "on" : "off");
    }
    s_frDevice.store(this, std::memory_order_release);
}

bool Device::FrDump(const char *reason, bool checkQueue)
{
    const uint64_t now = NowNs();
    const bool queueError = reason[0] == 'q';
    const int budget = std::strncmp(reason, "watchdog", 8) == 0 ? 0 : 1;
    if (!queueError)
    {
        const uint64_t last = g_fr.lastDumpNs[budget].load(std::memory_order_relaxed);
        if ((last && now - last < kDumpIntervalNs) ||
            g_fr.dumps[budget].load(std::memory_order_relaxed) >= kMaxDumps)
            return false;
    }
    if (g_fr.dumping.test_and_set(std::memory_order_acquire))
        return false;
    g_fr.lastDumpNs[budget].store(now, std::memory_order_relaxed);
    g_fr.dumps[budget].fetch_add(1, std::memory_order_relaxed);

    static fr::SegRec segs[fr::kDumpSegs];
    static fr::ChunkEv evs[fr::kDumpEvents];
    static fr::ChunkRec chunks[fr::ChunkTable::kEntries];
    static char text[fr::kDumpCap];
    fr::DumpHeader h;
    h.reason = reason;
    h.open = m_openSeqPub.load(std::memory_order_acquire);
    h.submitted = m_submittedSeq.load(std::memory_order_acquire);
    h.completed = m_completedSeqPub.load(std::memory_order_acquire);
    h.frames = g_fr.frame.load(std::memory_order_relaxed);
    if (queueError)
        h.queueError = 1;
    else if (checkQueue)
    {
        // A timed wait (1 us) makes deko3d read the channel's error state,
        // which the hot path's zero-timeout polls never do.
        DkFence fence = m_fences[h.submitted % kFenceRing];
        dkFenceWait(&fence, 1000);
        h.queueError = dkQueueIsInErrorState(m_queue) ? 1 : 0;
    }
    h.poison = m_cmdPoison;
    h.chunkBytes = m_cmdChunkBytes;
    h.writerTid = g_fr.threads.Index(m_recWriter);
    h.lockTid = g_fr.threads.Index(m_lock.OwnerTag());
    h.submitTid = g_fr.threads.Index(m_submitOwner.Owner());
    h.captureTid = m_captureTag ? g_fr.threads.Index(m_captureTag) : 0;
    h.segPushed = g_fr.segs.Head();
    h.evPushed = g_fr.evs.Head();
    h.badFirst = g_fr.badFirst.load(std::memory_order_relaxed);
    h.hashChanged = g_fr.hashChanged.load(std::memory_order_relaxed);
    h.foreign = g_fr.foreign.load(std::memory_order_relaxed);
    const uint32_t ns = g_fr.segs.Newest(segs, fr::kDumpSegs);
    const uint32_t ne = g_fr.evs.Newest(evs, fr::kDumpEvents);
    const uint32_t nc = g_fr.chunks.Count();
    for (uint32_t i = 0; i < nc; ++i)
        chunks[i] = g_fr.chunks.Get(i);
    fr::TextOut out(text, sizeof(text));
    fr::FormatDump(out, h, segs, ns, evs, ne, chunks, nc, &g_fr.threads);
    Emit(text);
    g_fr.binaryPending.store(true, std::memory_order_release);
    g_fr.dumping.clear(std::memory_order_release);
    return true;
}

void Device::FrHeartbeat()
{
    static uint64_t s_lastRec, s_lastPoison, s_lastWords;
    static uint32_t s_lastFrame;
    const uint64_t now = NowNs();
    const uint32_t frame = g_fr.frame.load(std::memory_order_relaxed);
    const uint64_t rec = g_fr.recordNs.load(std::memory_order_relaxed);
    const uint64_t poison = g_fr.poisonNs.load(std::memory_order_relaxed);
    const uint64_t words = g_fr.poisonWords.load(std::memory_order_relaxed);
    const uint32_t frames = frame - s_lastFrame ? frame - s_lastFrame : 1;
    uint32_t live = 0;
    for (uint32_t i = 0, n = g_fr.chunks.Count(); i < n; ++i)
        live += g_fr.chunks.Get(i).state != fr::kChunkHeap;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "FR hb t=%llu open=%llu completed=%llu frames=%u segs=%u evs=%u chunks=%u live=%u poison=%u "
                  "bad_first=%u hash=%u foreign=%u rec_us_per_frame=%.1f poison_us_per_frame=%.1f "
                  "poison_kwords_per_frame=%.1f\n",
                  (unsigned long long)(now / 1000000ull),
                  (unsigned long long)m_openSeqPub.load(std::memory_order_relaxed),
                  (unsigned long long)m_completedSeqPub.load(std::memory_order_relaxed), frame, g_fr.segs.Head(),
                  g_fr.evs.Head(), g_fr.chunks.Count(), live, (unsigned)m_cmdPoison,
                  g_fr.badFirst.load(std::memory_order_relaxed), g_fr.hashChanged.load(std::memory_order_relaxed),
                  g_fr.foreign.load(std::memory_order_relaxed), (rec - s_lastRec) / 1000.0 / frames,
                  (poison - s_lastPoison) / 1000.0 / frames, (words - s_lastWords) / 1000.0 / frames);
    s_lastRec = rec;
    s_lastPoison = poison;
    s_lastWords = words;
    s_lastFrame = frame;
    Emit(line);
}

void Device::FrDetach()
{
    Device *self = this;
    s_frDevice.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
}

bool Device::FrWriteBinary(const char *path)
{
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    static char vbuf[16384];
    std::setvbuf(f, vbuf, _IOFBF, sizeof(vbuf));
    fr::BinHeader h{};
    std::memcpy(h.magic, "FRB1", 4);
    h.version = 1;
    h.headerBytes = sizeof(h);
    h.segPushed = g_fr.segs.Head();
    h.evPushed = g_fr.evs.Head();
    h.chunkCount = g_fr.chunks.Count();
    h.segCount = std::min(h.segPushed, kSegRing);
    h.evCount = std::min(h.evPushed, kEventRing);
    h.poison = m_cmdPoison;
    h.queueError = dkQueueIsInErrorState(m_queue) ? 1 : 0;
    h.open = m_openSeqPub.load(std::memory_order_acquire);
    h.submitted = m_submittedSeq.load(std::memory_order_acquire);
    h.completed = m_completedSeqPub.load(std::memory_order_acquire);
    h.frames = g_fr.frame.load(std::memory_order_relaxed);
    h.timeMs = NowNs() / 1000000ull;
    // Counts are rewritten below once the torn slots are known.
    bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1;
    uint32_t segs = 0, evs = 0;
    for (uint32_t i = h.segPushed - h.segCount; ok && i != h.segPushed; ++i)
    {
        fr::SegRec r;
        if (g_fr.segs.Read(i, &r))
            ok = std::fwrite(&r, sizeof(r), 1, f) == 1, ++segs;
    }
    for (uint32_t i = h.evPushed - h.evCount; ok && i != h.evPushed; ++i)
    {
        fr::ChunkEv e;
        if (g_fr.evs.Read(i, &e))
            ok = std::fwrite(&e, sizeof(e), 1, f) == 1, ++evs;
    }
    for (uint32_t i = 0; ok && i < h.chunkCount; ++i)
    {
        const fr::ChunkRec c = g_fr.chunks.Get(i);
        ok = std::fwrite(&c, sizeof(c), 1, f) == 1;
    }
    h.segCount = segs;
    h.evCount = evs;
    ok = ok && std::fseek(f, 0, SEEK_SET) == 0 && std::fwrite(&h, sizeof(h), 1, f) == 1;
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

} // namespace deko9

void Deko9_SetCmdPoison(IDirect3DDevice9 *device, bool on)
{
    static_cast<deko9::Device *>(device)->SetCmdPoison(on);
}

void Deko9_SetCmdOwnerCheck(IDirect3DDevice9 *device, bool on)
{
    static_cast<deko9::Device *>(device)->SetCmdOwnerCheck(on);
}

void Deko9_FlightRecDump(const char *reason)
{
    if (deko9::Device *d = deko9::s_frDevice.load(std::memory_order_acquire))
        d->FrDump(reason, true);
}

void Deko9_FlightRecHeartbeat(void)
{
    if (deko9::Device *d = deko9::s_frDevice.load(std::memory_order_acquire))
        d->FrHeartbeat();
}

bool Deko9_FlightRecBinaryPending(void)
{
    return deko9::g_fr.binaryPending.load(std::memory_order_acquire);
}

void Deko9_FlightRecWriteBinary(void)
{
    deko9::Device *d = deko9::s_frDevice.load(std::memory_order_acquire);
    if (!d || !deko9::g_fr.binaryPending.exchange(false))
        return;
    const char *path = "sdmc:/switch/kisakcod/flightrec.bin";
    const uint64_t t0 = deko9::NowNs();
    const bool ok = d->FrWriteBinary(path);
    char line[160];
    std::snprintf(line, sizeof(line), "FR bin path=%s ok=%d ms=%llu\n", path, (int)ok,
                  (unsigned long long)((deko9::NowNs() - t0) / 1000000ull));
    deko9::Emit(line);
}

void Deko9_FlightRecEarlyRetireTest(void)
{
    deko9::g_fr.earlyTest.store(true, std::memory_order_release);
}
