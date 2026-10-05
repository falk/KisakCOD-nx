// Host stress test (ASan/UBSan and TSan) for the device-lock-free frame loop
// of the deko9 renderer: while a simulated render back end holds the device
// lock across long draw batches and presents frames, a simulated main thread
// runs the per-frame API (buffer Lock/Unlock with DISCARD renames, frame
// arena allocation, end-fence and GPU-sync polls, sliced frame waits)
// through the same pure pieces the device uses (deko9_framepace.h
// FramePublish/PublishedFrameDone/PublishedFrameWait, deko9_rename.h,
// deko9_arena.h, deko9_lock.h). A fake in-order GPU thread executes the
// submitted lists after a fixed latency and checks that every byte a list
// reads still holds what was written for it (no memory reused before its
// list completed), and every "frame done" answer is checked against the
// GPU's real progress.
//
// Gates: zero device-lock acquisitions by the main thread per frame in
// steady state; no lifetime error; no early "done". Negative controls: the
// previous lock-taking paths are measured by the same counter and must show
// acquisitions and blocking (so the zero check is not vacuous), and (outside
// TSan, where it is a deliberate race) a rename that ignores the GPU's
// progress must be caught by the lifetime check. Run by ./test host
// (deko9_lockfree_sanitizer_check).

#include "src/deko9/deko9_arena.h"
#include "src/deko9/deko9_framepace.h"
#include "src/deko9/deko9_lock.h"
#include "src/deko9/deko9_rename.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__)
#define LOCKFREE_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LOCKFREE_TEST_TSAN 1
#endif
#endif

namespace
{

int g_failures;

void Check(bool ok, const char *name)
{
    if (!ok)
    {
        std::printf("FAIL:DEKO9_LOCKFREE %s\n", name);
        ++g_failures;
    }
}

using Clock = std::chrono::steady_clock;

void SleepUs(int us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

// ---- FramePublish: no torn fence copies --------------------------------------

// A three-word fence whose words must always agree, so a torn copy shows.
struct WideFence
{
    uint64_t a, b, c;
};

void TestPublishNoTear()
{
    constexpr uint32_t N = 2;
    deko9::FramePublish<N, WideFence> pub;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> torn{0}, wrong{0}, reads{0};
    std::thread writer([&] {
        for (uint64_t f = 1; f <= 200000; ++f)
        {
            // The ring rule: frame f reuses slot f % N only once f - N is done.
            if (f > N)
                pub.PublishDone(f - N);
            pub.PublishPresent(f, f * 10, WideFence{f, f, f});
        }
        stop.store(true);
    });
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r)
    {
        readers.emplace_back([&] {
            while (!stop.load())
            {
                const uint64_t p = pub.Presented();
                for (uint64_t f = p > 2 ? p - 2 : 1; f <= p + 1; ++f)
                {
                    WideFence fence{};
                    uint64_t endSeq = 0;
                    if (pub.Read(f, &fence, &endSeq) == deko9::FramePublish<N, WideFence>::State::Pending)
                    {
                        if (fence.a != fence.b || fence.b != fence.c)
                            torn.fetch_add(1);
                        else if (fence.a != f || endSeq != f * 10)
                            wrong.fetch_add(1);
                        reads.fetch_add(1);
                    }
                }
            }
        });
    }
    writer.join();
    for (std::thread &t : readers)
        t.join();
    Check(torn.load() == 0, "publish: a reader saw a torn fence copy");
    Check(wrong.load() == 0, "publish: a reader got another frame's fence");
    Check(reads.load() > 0, "publish: readers saw pending frames");
    WideFence fence{};
    uint64_t endSeq = 0;
    Check(pub.Read(200000 - 2, &fence, &endSeq) == deko9::FramePublish<N, WideFence>::State::Done,
          "publish: a frame whose slot was reused reads done");
    Check(pub.Read(200001, &fence, &endSeq) == deko9::FramePublish<N, WideFence>::State::NotPresented,
          "publish: the recording frame reads not presented");
    Check(pub.Read(0, &fence, &endSeq) == deko9::FramePublish<N, WideFence>::State::Done, "publish: frame 0 is done");
    std::printf("lockfree publish: %llu pending reads, %llu torn, %llu wrong\n", (unsigned long long)reads.load(),
                (unsigned long long)torn.load(), (unsigned long long)wrong.load());
}

void TestRenameSpares()
{
    struct M
    {
        int id;
    };
    deko9::RenameSpares<M> spares;
    M out{};
    Check(!spares.Take(100, &out), "spares: empty takes nothing");
    M ev{};
    uint64_t evStamp = 0;
    Check(!spares.Retire({1}, 10, &ev, &evStamp), "spares: first retire keeps");
    Check(!spares.Retire({2}, 20, &ev, &evStamp), "spares: second retire keeps");
    Check(!spares.Take(9, &out), "spares: nothing reusable before its stamp completed");
    Check(spares.Take(15, &out) && out.id == 1, "spares: the stamp-completed spare is reused");
    Check(!spares.Take(15, &out), "spares: the later one still waits");
    Check(!spares.Retire({3}, 30, &ev, &evStamp), "spares: refill");
    Check(spares.Retire({4}, 40, &ev, &evStamp) && ev.id == 2 && evStamp == 20,
          "spares: past capacity the oldest is evicted with its stamp");
    // RenameForLock: not busy -> in place; busy -> spare or grow.
    int grows = 0, frees = 0;
    M cur{100}, prev{};
    deko9::RenameSpares<M> s2;
    auto alloc = [&](M *m) {
        ++grows;
        *m = {200 + grows};
        return true;
    };
    auto fr = [&](const M &, uint64_t) { ++frees; };
    deko9::RenameResult r = deko9::RenameForLock(&cur, &prev, 5, true, 5, 9, s2, alloc, fr);
    Check(!r.renamed && cur.id == 100, "rename: last use completed -> in place");
    r = deko9::RenameForLock(&cur, &prev, 6, false, 5, 9, s2, alloc, fr);
    Check(!r.renamed, "rename: NOOVERWRITE/READONLY never renames");
    r = deko9::RenameForLock(&cur, &prev, 6, true, 5, 9, s2, alloc, fr);
    Check(r.renamed && r.grew && cur.id == 201 && prev.id == 100 && s2.Count() == 1,
          "rename: busy with no free spare grows, the old memory becomes a spare");
    r = deko9::RenameForLock(&cur, &prev, 12, true, 8, 13, s2, alloc, fr);
    Check(r.renamed && r.grew, "rename: the spare is stamped with the open list (9), not reusable at 8");
    r = deko9::RenameForLock(&cur, &prev, 14, true, 9, 15, s2, alloc, fr);
    Check(r.renamed && !r.grew && cur.id == 100, "rename: reusable once the open list at its rename completed");
}

// ---- the frame-loop stress ---------------------------------------------------

struct FakeFence
{
    uint64_t seq; // signals once the GPU completed list `seq`
};

struct Mem
{
    uint8_t *cpu = nullptr;
    uint32_t size = 0;
};

constexpr uint32_t kN = deko9::kFramesInFlight;
constexpr uint32_t kBufBytes = 2048;
constexpr uint32_t kPools = 3;      // engine data pools (filling / drawing / previous frame)
constexpr uint32_t kPoolBuffers = 2; // buffers main fills per pool (mesh VB, pre-tess IB)
constexpr uint32_t kMaxFrames = 512;

enum class Paths
{
    LockFree, // this branch
    Locked,   // the previous paths: every call under the device lock
};

struct Options
{
    Paths paths = Paths::LockFree;
    bool ignoreGpuProgress = false; // negative control: renames believe everything completed
    uint32_t frames = 200;
    uint32_t warmup = 40;
    int batchHoldUs = 1800;     // back end holds the lock this long per batch
    int gpuLatencyUs = 3000;    // a list executes this long after its submit
};

struct Results
{
    uint64_t otherAcqSteady = 0;      // main-thread acquisitions after warmup
    uint64_t capacityAcqSteady = 0;   // of those: grow/evict (capacity, not per-frame work)
    uint64_t bufferGrows = 0, bufferEvicts = 0; // main-thread Buffer::Lock grow/evict (whole run)
    uint32_t arenaChunks = 0;                   // chunks the frame arena ever created
    uint64_t otherContended = 0;      // main-thread blocks behind the back end (whole run)
    uint64_t otherWaitNs = 0;
    uint64_t lifetimeErrors = 0;
    uint64_t earlyDone = 0;
    uint64_t renamesSteady = 0;
    uint64_t sparesReusedSteady = 0;
    uint64_t donePolls = 0, doneTrue = 0, waits = 0;
    uint64_t handoffs = 0;
    uint64_t foreignRebinds = 0;
    std::string callers;
    bool callerTotalsMatch = false; // per-site waiter table sums to the lock's totals
    uint64_t waitFrameBlocked = 0;  // blocked acquisitions attributed to WaitFrameFor
};

class Sim
{
public:
    explicit Sim(const Options &o) : m_o(o)
    {
        m_arena.SetFactory([this](uint32_t size, deko9::ArenaChunkMemory *out) {
            deko9::DeviceLockGuard lock(m_lock, "FrameArena grow");
            out->cpu = NewMem(size);
            out->gpu = (uint64_t)(uintptr_t)out->cpu;
            out->size = size;
            return true;
        });
        for (auto &pool : m_buffers)
            for (Buffer &b : pool)
                b.cur = {NewMem(kBufBytes), kBufBytes};
        for (auto &e : m_frameEnd)
            e.store(0);
    }
    ~Sim()
    {
        for (uint8_t *p : m_mem)
            delete[] p;
    }

    Results Run()
    {
        std::thread gpu([this] { GpuThread(); });
        std::thread back([this] { BackEndThread(); });
        MainThread();
        back.join();
        {
            std::lock_guard<std::mutex> g(m_gpuMutex);
            m_gpuQuit = true;
        }
        m_gpuCv.notify_all();
        gpu.join();
        Results r = m_r;
        // Threads joined: the lock's owner-only counters are stable.
        r.otherAcqSteady = m_otherAtPresent[m_o.frames] - m_otherAtPresent[m_o.warmup];
        r.capacityAcqSteady = m_capacityAtPresent[m_o.frames] - m_capacityAtPresent[m_o.warmup];
        r.otherContended = m_lock.Contended(false);
        r.otherWaitNs = m_lock.WaitNs(false);
        r.handoffs = m_lock.HandOffs();
        r.lifetimeErrors = m_lifetimeErrors.load();
        r.earlyDone = m_earlyDone.load();
        const deko9::DeviceLock::CallerStats *cs = m_lock.Callers();
        uint64_t acqSum = 0, blockedSum = 0, nsSum = 0;
        for (uint32_t i = 0; i < deko9::DeviceLock::kCallerSlots && cs[i].site; ++i)
        {
            acqSum += cs[i].acquisitions;
            blockedSum += cs[i].contended;
            nsSum += cs[i].ns;
            if (!std::strcmp(cs[i].site, "WaitFrameFor"))
                r.waitFrameBlocked += cs[i].contended;
            if (!std::strcmp(cs[i].site, "Buffer::Lock grow"))
                r.bufferGrows += cs[i].acquisitions;
            if (!std::strcmp(cs[i].site, "Buffer::Lock evict"))
                r.bufferEvicts += cs[i].acquisitions;
            char line[160];
            std::snprintf(line, sizeof(line), " %s=%llu/%llu/%.2fms", cs[i].site,
                          (unsigned long long)cs[i].acquisitions, (unsigned long long)cs[i].contended,
                          cs[i].ns / 1e6);
            r.callers += line;
        }
        r.callerTotalsMatch = acqSum == m_lock.OtherAcquisitions() && blockedSum == r.otherContended &&
                              nsSum == r.otherWaitNs;
        r.arenaChunks = m_arena.ChunkCount();
        return r;
    }

private:
    struct Buffer
    {
        Mem cur;
        uint64_t lastUse = 0;
        deko9::RenameSpares<Mem> spares;
        bool locked = false;
        uint8_t pattern = 0;
    };
    struct GpuRead
    {
        const uint8_t *mem;
        uint32_t size;
        uint8_t expect;
    };
    struct List
    {
        Clock::time_point submitted;
        std::vector<GpuRead> reads;
    };
    struct MainSpan
    {
        uint64_t frame = 0;
        deko9::ArenaSpan span;
        uint8_t pattern = 0;
    };

    const Options m_o;
    Results m_r;
    deko9::DeviceLock m_lock;

    // Recording-thread state (under the lock, like the device's).
    deko9::FrameRing<kN> m_ring;
    uint64_t m_openSeq = 1, m_completedSeq = 0;
    uint64_t m_renameEpochSeen = 0;
    uint64_t m_otherAtPresent[kMaxFrames + 1] = {};
    uint64_t m_capacityAtPresent[kMaxFrames + 1] = {};

    // Published for threads without the lock.
    deko9::FramePublish<kN, FakeFence> m_pub;
    std::atomic<uint64_t> m_openSeqPub{1}, m_completedSeqPub{0}, m_renameEpoch{0};
    deko9::FrameArena m_arena;

    // Fake GPU.
    std::mutex m_gpuMutex;
    std::condition_variable m_gpuCv;
    std::map<uint64_t, List> m_lists;
    bool m_gpuQuit = false;
    std::atomic<uint64_t> m_gpuDone{0};
    std::atomic<uint64_t> m_lifetimeErrors{0}, m_earlyDone{0};
    std::atomic<uint64_t> m_frameEnd[kMaxFrames + 2];

    // Engine data handed from main to the back end each frame.
    Buffer m_buffers[kPools][kPoolBuffers];
    MainSpan m_mainSpans[kPools];
    std::mutex m_handMutex;
    std::condition_variable m_handCv;
    uint64_t m_handed = 0, m_finished = 0;

    std::mutex m_memMutex;
    std::vector<uint8_t *> m_mem;

    uint8_t *NewMem(uint32_t size)
    {
        uint8_t *p = new uint8_t[size];
        std::memset(p, 0, size);
        std::lock_guard<std::mutex> g(m_memMutex);
        m_mem.push_back(p);
        return p;
    }

    static bool Holds(const uint8_t *p, uint32_t size, uint8_t v)
    {
        return p[0] == v && p[size / 2] == v && p[size - 1] == v;
    }

    // ---- fake GPU: executes lists in order, `gpuLatencyUs` after submit.
    void GpuThread()
    {
        std::unique_lock<std::mutex> g(m_gpuMutex);
        for (;;)
        {
            const uint64_t next = m_gpuDone.load(std::memory_order_relaxed) + 1;
            auto it = m_lists.find(next);
            if (it == m_lists.end())
            {
                if (m_gpuQuit)
                    return;
                m_gpuCv.wait_for(g, std::chrono::milliseconds(1));
                continue;
            }
            const Clock::time_point due = it->second.submitted + std::chrono::microseconds(m_o.gpuLatencyUs);
            if (Clock::now() < due)
            {
                g.unlock();
                std::this_thread::sleep_until(due);
                g.lock();
                continue;
            }
            for (const GpuRead &r : it->second.reads)
            {
                if (!Holds(r.mem, r.size, r.expect))
                    m_lifetimeErrors.fetch_add(1, std::memory_order_relaxed);
            }
            m_lists.erase(it);
            m_gpuDone.store(next, std::memory_order_release);
        }
    }

    // ---- recording thread (owner) side ----
    void NoteFrameDone(uint64_t frame)
    {
        m_ring.MarkDone(frame);
        m_pub.PublishDone(m_ring.DoneThrough());
        m_arena.RetireThrough(m_ring.DoneThrough());
    }
    void FoldObservedFrames()
    {
        const uint64_t seen = m_pub.Observed();
        if (seen > m_ring.DoneThrough())
            NoteFrameDone(seen);
    }
    void Collect()
    {
        m_completedSeq = m_gpuDone.load(std::memory_order_acquire);
        m_completedSeqPub.store(m_completedSeq, std::memory_order_release);
    }
    bool OwnerFrameDone(uint64_t frame)
    {
        FoldObservedFrames();
        switch (m_ring.Query(frame))
        {
        case deko9::FrameRing<kN>::State::Done:
            return true;
        case deko9::FrameRing<kN>::State::NotPresented:
            return false;
        case deko9::FrameRing<kN>::State::Pending:
            break;
        }
        if (m_gpuDone.load(std::memory_order_acquire) < m_ring.SlotEndSeq(deko9::FrameRing<kN>::Slot(frame)))
            return false;
        NoteFrameDone(frame);
        Collect();
        return true;
    }
    void Submit(std::vector<GpuRead> reads)
    {
        {
            std::lock_guard<std::mutex> g(m_gpuMutex);
            m_lists[m_openSeq] = {Clock::now(), std::move(reads)};
        }
        m_gpuCv.notify_all();
        ++m_openSeq;
        m_openSeqPub.store(m_openSeq, std::memory_order_release);
    }
    void Draw(Buffer &b, std::vector<GpuRead> *reads)
    {
        b.lastUse = m_openSeq;
        reads->push_back({b.cur.cpu, b.cur.size, b.pattern});
    }

    void BackEndThread()
    {
        for (uint64_t d = 1; d <= m_o.frames; ++d)
        {
            {
                std::unique_lock<std::mutex> g(m_handMutex);
                m_handCv.wait(g, [&] { return m_handed >= d; });
            }
            const uint32_t pool = (uint32_t)(d % kPools), prevPool = (uint32_t)((d + kPools - 1) % kPools);
            m_lock.lock("Deko9_BeginBatch");
            m_lock.SetSite("batch");
            m_lock.SetDrawTag(deko9::ThreadTag());
            for (int batch = 0; batch < 2; ++batch)
            {
                std::vector<GpuRead> reads;
                // The device's NoteForeignRenames before an input bind.
                const uint64_t epoch = m_renameEpoch.load(std::memory_order_relaxed);
                if (epoch != m_renameEpochSeen)
                {
                    m_renameEpochSeen = epoch;
                    ++m_r.foreignRebinds;
                }
                for (Buffer &b : m_buffers[pool])
                    Draw(b, &reads);
                // The previous frame's buffers too (TAAU motion reads the
                // previous frame's skinned vertices).
                if (d > 1)
                    for (Buffer &b : m_buffers[prevPool])
                        Draw(b, &reads);
                // Back-end arena span for the frame being recorded.
                const uint64_t frame = m_ring.Recording();
                deko9::ArenaSpan span;
                if (!m_arena.Alloc(deko9::ThreadTag(), frame, 512, 256, &span))
                    Check(false, "stress: back-end arena alloc failed");
                const uint8_t arenaPattern = (uint8_t)(frame * 7 + batch);
                std::memset(span.cpu, arenaPattern, span.size);
                reads.push_back({span.cpu, span.size, arenaPattern});
                // The span main allocated for this frame's data.
                const MainSpan &ms = m_mainSpans[pool];
                if (ms.frame < frame)
                    m_lifetimeErrors.fetch_add(1); // stamped for a frame that retires before this one
                reads.push_back({ms.span.cpu, ms.span.size, ms.pattern});
                // Hold the lock for the batch, handing it to a waiter between
                // draws like the device does.
                const Clock::time_point end = Clock::now() + std::chrono::microseconds(m_o.batchHoldUs);
                while (Clock::now() < end)
                {
                    SleepUs(100);
                    m_lock.HandOffIfContended();
                }
                Collect();
                Submit(std::move(reads));
            }
            m_lock.SetSite(nullptr);
            m_lock.unlock();
            Present(d);
            {
                std::lock_guard<std::mutex> g(m_handMutex);
                m_finished = d;
            }
            m_handCv.notify_all();
        }
    }

    void Present(uint64_t d)
    {
        deko9::DeviceLockGuard lock(m_lock, "PresentFrame");
        deko9::DeviceLockSite site(m_lock, "present");
        FoldObservedFrames();
        const uint64_t frame = m_ring.Recording();
        Check(frame == d, "stress: one presented frame per data item");
        const uint64_t prev = deko9::FrameRing<kN>::ReuseFrame(frame);
        while (prev && !OwnerFrameDone(prev))
        {
            deko9::DeviceUnlockScope unlocked(m_lock);
            SleepUs(50);
        }
        Submit({}); // the present list
        const uint64_t endSeq = m_openSeq - 1;
        m_frameEnd[frame].store(endSeq, std::memory_order_release);
        const bool presented = m_ring.Present(endSeq);
        Check(presented, "stress: ring present");
        if (presented)
            m_pub.PublishPresent(frame, endSeq, FakeFence{endSeq});
        Collect();
        m_otherAtPresent[frame] = m_lock.OtherAcquisitions();
        uint64_t capacity = 0;
        const deko9::DeviceLock::CallerStats *cs = m_lock.Callers();
        for (uint32_t i = 0; i < deko9::DeviceLock::kCallerSlots && cs[i].site; ++i)
        {
            if (!std::strcmp(cs[i].site, "Buffer::Lock grow") || !std::strcmp(cs[i].site, "Buffer::Lock evict") ||
                !std::strcmp(cs[i].site, "FrameArena grow"))
                capacity += cs[i].acquisitions;
        }
        m_capacityAtPresent[frame] = capacity;
    }

    // ---- main thread: the per-frame API, new or previous paths ----
    bool FencePassed(FakeFence f) const { return m_gpuDone.load(std::memory_order_acquire) >= f.seq; }

    void CheckDoneAnswer(uint64_t frame)
    {
        ++m_r.doneTrue;
        if (!frame)
            return;
        const uint64_t end = m_frameEnd[frame].load(std::memory_order_acquire);
        if (!end || m_gpuDone.load(std::memory_order_acquire) < end)
            m_earlyDone.fetch_add(1);
    }

    uint64_t FrameRecording()
    {
        if (m_o.paths == Paths::Locked)
        {
            deko9::DeviceLockGuard lock(m_lock, "Deko9_FrameRecording");
            return m_ring.Recording();
        }
        return m_pub.Recording();
    }

    bool FrameDone(uint64_t frame)
    {
        ++m_r.donePolls;
        bool done;
        if (m_o.paths == Paths::Locked)
        {
            if (frame <= m_pub.DoneThrough())
                done = true;
            else
            {
                deko9::DeviceLockGuard lock(m_lock, "Deko9_FrameDone");
                done = OwnerFrameDone(frame);
            }
        }
        else
        {
            done = deko9::PublishedFrameDone(m_pub, m_completedSeqPub.load(std::memory_order_acquire), frame,
                                             [&](FakeFence f) { return FencePassed(f); });
        }
        if (done)
            CheckDoneAnswer(frame);
        return done;
    }

    bool SleepPoll(FakeFence f, int64_t timeoutNs)
    {
        const Clock::time_point end = Clock::now() + std::chrono::nanoseconds(timeoutNs);
        bool done;
        while (!(done = FencePassed(f)) && Clock::now() < end)
            SleepUs(50);
        return done;
    }

    bool WaitFrame(uint64_t frame, int64_t timeoutNs)
    {
        ++m_r.waits;
        bool done;
        if (m_o.paths == Paths::Locked)
        {
            FakeFence fence{};
            {
                deko9::DeviceLockGuard lock(m_lock, "WaitFrameFor");
                if (OwnerFrameDone(frame))
                {
                    CheckDoneAnswer(frame);
                    return true;
                }
                if (m_ring.Query(frame) != deko9::FrameRing<kN>::State::Pending || m_lock.Depth() > 1)
                    return false;
                fence = {m_ring.SlotEndSeq(deko9::FrameRing<kN>::Slot(frame))};
            }
            SleepPoll(fence, timeoutNs);
            deko9::DeviceLockGuard lock(m_lock, "WaitFrameFor");
            done = OwnerFrameDone(frame);
        }
        else
        {
            done = deko9::PublishedFrameWait(
                m_pub, m_completedSeqPub.load(std::memory_order_acquire), frame, m_lock.OwnedByCaller(),
                [&](FakeFence f) { return FencePassed(f); }, [&](FakeFence f) { return SleepPoll(f, timeoutNs); });
        }
        if (done)
            CheckDoneAnswer(frame);
        return done;
    }

    uint8_t *BufferLock(Buffer &b)
    {
        Check(!b.locked, "stress: buffer locked twice");
        if (m_o.paths == Paths::Locked)
        {
            // The previous Buffer::Lock: busy check, rename into fresh heap
            // memory, old memory freed after the open list (here: kept).
            deko9::DeviceLockGuard lock(m_lock, "Buffer::Lock");
            if (b.lastUse > m_completedSeq)
            {
                b.cur = {NewMem(kBufBytes), kBufBytes};
                b.lastUse = 0;
                m_renameEpoch.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else
        {
            Mem previous;
            const uint64_t completed =
                m_o.ignoreGpuProgress ? ~0ull : m_completedSeqPub.load(std::memory_order_acquire);
            const uint32_t sparesBefore = b.spares.Count();
            const deko9::RenameResult r = deko9::RenameForLock(
                &b.cur, &previous, b.lastUse, true, completed, m_openSeqPub.load(std::memory_order_acquire), b.spares,
                [&](Mem *out) {
                    deko9::DeviceLockGuard lock(m_lock, "Buffer::Lock grow");
                    *out = {NewMem(kBufBytes), kBufBytes};
                    return true;
                },
                [&](const Mem &, uint64_t) { deko9::DeviceLockGuard lock(m_lock, "Buffer::Lock evict"); });
            Check(r.ok, "stress: rename");
            if (r.renamed)
            {
                b.lastUse = 0;
                m_renameEpoch.fetch_add(1, std::memory_order_relaxed);
                if (m_steady)
                {
                    ++m_r.renamesSteady;
                    m_r.sparesReusedSteady += !r.grew && b.spares.Count() <= sparesBefore;
                }
            }
        }
        b.locked = true;
        return b.cur.cpu;
    }
    void BufferUnlock(Buffer &b)
    {
        if (m_o.paths == Paths::Locked)
        {
            deko9::DeviceLockGuard lock(m_lock, "Buffer::Unlock");
            b.locked = false;
            return;
        }
        b.locked = false;
    }

    bool FrameAlloc(uint64_t frame, deko9::ArenaSpan *span)
    {
        if (m_o.paths == Paths::Locked)
        {
            deko9::DeviceLockGuard lock(m_lock, "Deko9_FrameAlloc");
            return m_arena.Alloc(deko9::ThreadTag(), frame, 256, 256, span);
        }
        return m_arena.Alloc(deko9::ThreadTag(), frame, 256, 256, span);
    }

    bool m_steady = false; // main thread only

    void MainThread()
    {
        for (uint64_t d = 1; d <= m_o.frames; ++d)
        {
            m_steady = d > m_o.warmup;
            const uint32_t pool = (uint32_t)(d % kPools);
            // Fill this frame's mesh / pre-tess buffers (DISCARD locks).
            for (Buffer &b : m_buffers[pool])
            {
                uint8_t *p = BufferLock(b);
                b.pattern = (uint8_t)(d * 13 + 1);
                std::memset(p, b.pattern, kBufBytes);
                BufferUnlock(b);
            }
            // A front-end arena span for the frame that will draw it.
            MainSpan &ms = m_mainSpans[pool];
            ms.frame = FrameRecording() + 1;
            if (!FrameAlloc(ms.frame, &ms.span))
                Check(false, "stress: main arena alloc failed");
            ms.pattern = (uint8_t)(d * 5 + 3);
            std::memset(ms.span.cpu, ms.pattern, ms.span.size);
            // R_EndFencePending: the frame that last rendered this data.
            FrameDone(d > kPools ? d - kPools : 0);
            // Hand the data to the back end once it finished the previous one.
            {
                std::unique_lock<std::mutex> g(m_handMutex);
                m_handCv.wait(g, [&] { return m_finished + 1 >= d; });
                m_handed = d;
            }
            m_handCv.notify_all();
            // R_SyncGpu: poll, then sleep in 250 us slices, for the frame
            // before the one being recorded (bounded like R_GpuFenceTimeout).
            const uint64_t rec = FrameRecording();
            const uint64_t syncFrame = rec > 1 ? rec - 1 : 0;
            const Clock::time_point end = Clock::now() + std::chrono::milliseconds(3);
            while (!FrameDone(syncFrame) && Clock::now() < end)
                WaitFrame(syncFrame, 250000);
        }
    }
};

Results RunSim(const Options &o, const char *what)
{
    auto sim = std::make_unique<Sim>(o);
    const Results r = sim->Run();
    std::printf("lockfree %s: frames=%u steady otherAcquires=%llu (%.3f/frame, capacity %llu) otherContended=%llu "
                "otherWait=%.3fms handoffs=%llu lifetimeErrors=%llu earlyDone=%llu renames(steady)=%llu "
                "spareReuses(steady)=%llu foreignRebinds=%llu polls=%llu waits=%llu arenaChunks=%u callers=%s\n",
                what, o.frames, (unsigned long long)r.otherAcqSteady,
                (double)r.otherAcqSteady / (o.frames - o.warmup), (unsigned long long)r.capacityAcqSteady,
                (unsigned long long)r.otherContended, r.otherWaitNs / 1e6, (unsigned long long)r.handoffs,
                (unsigned long long)r.lifetimeErrors, (unsigned long long)r.earlyDone,
                (unsigned long long)r.renamesSteady, (unsigned long long)r.sparesReusedSteady,
                (unsigned long long)r.foreignRebinds, (unsigned long long)r.donePolls, (unsigned long long)r.waits,
                r.arenaChunks, r.callers.empty() ? " none" : r.callers.c_str());
    return r;
}

void TestFrameLoopLockFree()
{
    // Main takes the lock only for capacity, and the handoff bounds that
    // capacity whatever the host scheduling: main fills data D after the
    // back end presented D-1, which waited for frame D-3 and then published
    // the completed sequence, so the spare retired at the fill of D-3
    // (stamped at most the first list of D-3) is always free again: each
    // buffer grows at most once (its first busy lock, which a slow host may
    // push past any fixed warmup) and never evicts. Arena chunks: at most
    // kN + 2 unretired frames per producer thread.
    const Results r = RunSim(Options{}, "lock-free paths");
    Check(r.otherAcqSteady == r.capacityAcqSteady,
          "frame loop: the main thread took the device lock in steady state for per-frame work");
    Check(r.bufferGrows <= (uint64_t)kPools * kPoolBuffers,
          "frame loop: buffer renames grew past one spare per buffer (a spare never became free)");
    Check(r.bufferEvicts == 0, "frame loop: a buffer rename evicted a spare under the device lock");
    Check(r.arenaChunks <= 2 * (kN + 2), "frame loop: the frame arena grew past its unretired-frame bound");
    Check(r.callerTotalsMatch, "waiters: the per-call-site table sums to the lock's other-thread totals");
    Check(r.lifetimeErrors == 0, "frame loop: a list read memory reused before it completed");
    Check(r.earlyDone == 0, "frame loop: a frame was reported done before the GPU finished it");
    Check(r.renamesSteady > 0, "frame loop: busy DISCARD locks renamed (the rename path ran)");
    Check(r.sparesReusedSteady > 0, "frame loop: renames reused spares without the lock");
    Check(r.foreignRebinds > 0, "frame loop: the back end saw main's renames through the epoch");
    Check(r.doneTrue > 0 && r.waits > 0, "frame loop: the GPU-sync loop polled and waited");
}

void TestFrameLoopLockedControl()
{
    Options o;
    o.paths = Paths::Locked;
    o.frames = 64;
    o.warmup = 16;
    const Results r = RunSim(o, "previous locked paths (control)");
    // The same counter must see the previous paths, or the zero above
    // would prove nothing.
    Check(r.otherAcqSteady >= 3 * (uint64_t)(o.frames - o.warmup),
          "control: the previous paths take the lock several times per frame");
    Check(r.otherContended > 0 && r.otherWaitNs > 0, "control: the previous paths block behind the batch lock");
    Check(r.callerTotalsMatch, "control waiters: the per-call-site table sums to the lock's other-thread totals");
    Check(r.waitFrameBlocked > 0, "control waiters: blocking is attributed to the waiting call site");
    Check(r.lifetimeErrors == 0 && r.earlyDone == 0, "control: the previous paths are correct, only serialized");
}

void TestLifetimeCheckControl()
{
#if defined(LOCKFREE_TEST_TSAN)
    // A deliberate data race (the overwrite under a reading GPU), which TSan
    // reports as such; the ASan/UBSan run covers it.
    std::printf("lockfree: lifetime control skipped under TSan\n");
#else
    Options o;
    o.ignoreGpuProgress = true;
    o.frames = 40;
    o.warmup = 8;
    // A GPU well behind the CPU, so the overwrite always lands before the
    // lists that read the old contents execute.
    o.gpuLatencyUs = 12000;
    const Results r = RunSim(o, "rename ignoring GPU progress (control)");
    Check(r.lifetimeErrors > 0, "control: overwriting a buffer the GPU still reads is caught");
#endif
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(120));
        std::printf("FAIL:DEKO9_LOCKFREE watchdog: still running after 120 s (deadlock)\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();
    TestRenameSpares();
    TestPublishNoTear();
    TestFrameLoopLockFree();
    TestFrameLoopLockedControl();
    TestLifetimeCheckControl();
    std::printf("%s:DEKO9_LOCKFREE\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
