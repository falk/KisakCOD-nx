// Host test (ASan/UBSan and TSan) of the deko9 renderer's cross-thread
// hazards, driven through the same pure pieces the device uses:
//
//  fence  SubmitOpenList's fence-slot choice (deko9_framepace.h
//         ReserveListSlot) around WaitSeq's unlocked GPU wait
//         (deko9_lock.h DeviceUnlockScope). A forced interleaving: thread A
//         starts a submit whose slot is still busy and waits without the
//         lock; thread B (a readback or WaitIdle) submits the open list in
//         that window; A resumes. The slot computed before the wait (the
//         previous code, the negative control) is B's: A re-signals it,
//         overwrites B's fence and leaves its own slot unsignalled, so the
//         completed sequence stops at B's list for good. Then a stress run
//         with two submitters and a fake in-order GPU.
//  buffer The rename protocol's cross-thread words (deko9_rename.h
//         BufferUse): a front end / back end frame handoff (the engine's
//         rule) never reports; a draw from a buffer another thread holds
//         locked is reported, and a stamp seen by the lock renames past
//         the list. Negative control (--plain-stamp-race, TSan child run):
//         the previous plain stamp word races under the same overlap.
//  taau   The skinned-cache pool (the engine's size, parsed) under that
//         handoff with TAAU motion binding the previous frame's pool:
//         no locked draw; 2 pools (the negative control) report one each
//         frame.
//  pins   Prebake jobs build without the device lock (deko9_prebake_queue.h
//         PrebakeQueue); an engine Release of a shader during its build.
//         Pinned (ShaderPins) the shader outlives the last install and is
//         really deleted afterwards (ASan checks the memory); unpinned (the
//         negative control) the build and install touch a released shader.
//  starve The prebake installer with its build thread held blocked: it
//         builds the unclaimed jobs itself and, once only the build thread's
//         job is left, calls drain() (the device raises that thread's
//         priority there). Negative control: the previous installer, which
//         only waited for the build thread, is stuck past a bounded wait.
//  merged A prebake build thread + loading-thread installs into VariantSet
//         under the device lock, a draw thread looking up / building
//         variants under the device lock in batches with handoffs, and a
//         main thread on the lock-free frame APIs (FramePublish,
//         PublishedFrameDone, RenameForLock with BufferUse).
//  defaults Every device default a dvar mirrors (deko9_native.h
//         DEKO9_DEFAULT_*) against the dvar's registered default in the
//         engine sources (argv[1] = repository root); every dvar a
//         Deko9_Set* call passes must be covered. Negative control: the
//         previous device defaults (shader options all on, no
//         static-texture hazard bit) must show exactly those two
//         mismatches.
//  switch r_deko9Prebake 0 skips the zone-load prebake: the gate runs
//         nothing when off (on, the negative control, runs it once), and the
//         engine's entry point collects and builds only through it.
//
// Run by ./test host (deko9_hazards_sanitizer_check).

#include "src/deko9/deko9_framepace.h"
#include "src/deko9/deko9_lock.h"
#include "src/deko9/deko9_native.h"
#include "src/deko9/deko9_prebake_queue.h"
#include "src/deko9/deko9_rename.h"
#include "src/deko9/deko9_variant_plan.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__)
#define HAZARDS_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define HAZARDS_TEST_TSAN 1
#endif
#endif

namespace
{

int g_failures;

void Check(bool ok, const char *name)
{
    if (!ok)
    {
        std::printf("FAIL:DEKO9_HAZARDS %s\n", name);
        ++g_failures;
    }
}

void SpinUntil(const std::atomic<bool> &flag)
{
    while (!flag.load(std::memory_order_acquire))
        std::this_thread::yield();
}

// An engine source file (empty when unreadable).
std::string ReadFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---- fence: fence-slot choice around the unlocked wait ----------------------

// A fake in-order GPU: list `seq` has completed once done >= seq.
struct SimGpu
{
    std::atomic<uint64_t> done{0};
};

// SubmitOpenList / WaitSeq / CompletedSeq of deko9::Device with the deko3d
// calls replaced: fence[slot] is the list whose completion signals the
// slot's DkFence (the device copies a DkFence to wait on it; here the copy
// is that value). Everything but the GPU is touched under the device lock.
template <uint32_t N> struct SimSubmitter
{
    deko9::DeviceLock lock;
    SimGpu *gpu = nullptr;
    bool oldSlotChoice = false; // the slot computed once, before the wait
    uint64_t openSeq = 1, completedSeq = 0;
    uint64_t slotSeq[N] = {};
    uint64_t fence[N] = {};
    // Outcomes.
    uint64_t staleSlots = 0;     // a list signalled a slot that is not its own
    uint64_t neverSubmitted = 0; // WaitSeq found a list missing from its slot
    std::vector<uint64_t> queue; // submitted lists in queue order
    std::mutex queueLock;        // the GPU thread reads `queue`
    // Called inside the unlocked GPU wait (test interleavings).
    std::function<void()> unlockedHook;

    bool Signalled(uint64_t fenceSeq) const { return gpu->done.load(std::memory_order_acquire) >= fenceSeq; }

    uint64_t CompletedSeq()
    {
        while (completedSeq + 1 < openSeq)
        {
            const uint64_t next = completedSeq + 1;
            if (slotSeq[next % N] != next || !Signalled(fence[next % N]))
                break;
            completedSeq = next;
        }
        return completedSeq;
    }

    void WaitSeq(uint64_t seq)
    {
        deko9::DeviceLockGuard guard(lock, "waitseq");
        if (seq >= openSeq)
            Submit();
        while (CompletedSeq() < seq)
        {
            const uint64_t next = completedSeq + 1;
            if (slotSeq[next % N] != next)
            {
                ++neverSubmitted; // Device::WaitSeq: FAIL:DEKO9_FENCE_WAIT "never submitted"
                break;
            }
            const uint64_t copy = fence[next % N];
            {
                deko9::DeviceUnlockScope unlocked(lock);
                if (unlockedHook)
                    unlockedHook();
                while (!Signalled(copy))
                    std::this_thread::yield();
            }
            if (completedSeq + 1 == next)
                completedSeq = next;
        }
    }

    void Submit()
    {
        deko9::DeviceLockGuard guard(lock, "submit");
        uint32_t slot;
        if (oldSlotChoice)
        {
            slot = (uint32_t)(openSeq % N);
            if (slotSeq[slot] && slotSeq[slot] > completedSeq)
                WaitSeq(slotSeq[slot]);
        }
        else
        {
            slot = deko9::ReserveListSlot(slotSeq, openSeq, completedSeq, [&](uint64_t seq) {
                WaitSeq(seq);
                return completedSeq >= seq;
            });
        }
        if (slot != openSeq % N)
            ++staleSlots;
        {
            std::lock_guard<std::mutex> q(queueLock);
            queue.push_back(openSeq);
        }
        fence[slot] = openSeq; // dkQueueSignalFence(&m_fences[slot])
        slotSeq[slot] = openSeq;
        ++openSeq;
    }
};

struct InterleaveOutcome
{
    uint64_t staleSlots, neverSubmitted, completedAfterDrain, lastSubmitted;
};

// The forced interleaving of the header comment, with a 2-slot ring.
InterleaveOutcome RunForcedInterleave(bool oldSlotChoice)
{
    SimGpu gpu;
    SimSubmitter<2> dev;
    dev.gpu = &gpu;
    dev.oldSlotChoice = oldSlotChoice;
    dev.Submit(); // seq 1 -> slot 1
    dev.Submit(); // seq 2 -> slot 0
    std::atomic<bool> aWaiting{false}, aGate{false};
    std::atomic<std::thread::id> aId{};
    std::atomic<int> aHookCalls{0};
    dev.unlockedHook = [&] {
        if (std::this_thread::get_id() != aId.load() || aHookCalls.fetch_add(1) != 0)
            return;
        aWaiting.store(true, std::memory_order_release);
        SpinUntil(aGate);
    };
    // A: seq 3 needs slot 1, still held by seq 1: waits without the lock.
    std::thread a([&] {
        aId.store(std::this_thread::get_id());
        dev.Submit();
    });
    SpinUntil(aWaiting);
    // The GPU finishes seq 1; B submits the open list meanwhile.
    gpu.done.store(1, std::memory_order_release);
    std::thread b([&] { dev.Submit(); });
    b.join();
    // A resumes; the GPU finishes seq 2 for A's re-derived slot.
    aGate.store(true, std::memory_order_release);
    gpu.done.store(2, std::memory_order_release);
    a.join();
    InterleaveOutcome out{};
    {
        deko9::DeviceLockGuard guard(dev.lock, "drain");
        out.lastSubmitted = dev.openSeq - 1;
        gpu.done.store(out.lastSubmitted, std::memory_order_release);
        dev.unlockedHook = nullptr;
        dev.WaitSeq(out.lastSubmitted); // Device::WaitIdle
        out.completedAfterDrain = dev.CompletedSeq();
        out.staleSlots = dev.staleSlots;
        out.neverSubmitted = dev.neverSubmitted;
    }
    return out;
}

void TestFenceSlotInterleave()
{
    const InterleaveOutcome fixed = RunForcedInterleave(false);
    const InterleaveOutcome old = RunForcedInterleave(true);
    std::printf("fence interleave: fixed stale=%llu neverSubmitted=%llu completed=%llu/%llu; "
                "old stale=%llu neverSubmitted=%llu completed=%llu/%llu\n",
                (unsigned long long)fixed.staleSlots, (unsigned long long)fixed.neverSubmitted,
                (unsigned long long)fixed.completedAfterDrain, (unsigned long long)fixed.lastSubmitted,
                (unsigned long long)old.staleSlots, (unsigned long long)old.neverSubmitted,
                (unsigned long long)old.completedAfterDrain, (unsigned long long)old.lastSubmitted);
    Check(fixed.lastSubmitted == 4, "fence: four lists submitted (two, then B's, then A's)");
    Check(fixed.staleSlots == 0, "fence: every list signals its own slot after a foreign submit in the wait");
    Check(fixed.neverSubmitted == 0, "fence: no list goes missing from the ring");
    Check(fixed.completedAfterDrain == fixed.lastSubmitted, "fence: every submitted list completes");
    // Negative control: the slot computed before the wait.
    Check(old.staleSlots == 1, "fence negative control: the old slot choice re-signals the foreign list's slot");
    Check(old.neverSubmitted >= 1, "fence negative control: the overwritten list is reported missing");
    Check(old.completedAfterDrain == 2, "fence negative control: completion stops before the foreign list");
}

// Two submitters (the render owner and a readback thread) and a fake GPU
// that completes lists in order a little later.
void TestFenceSlotStress(bool oldSlotChoice, uint64_t *staleOut, uint64_t *missingOut, bool *drainedOut)
{
    SimGpu gpu;
    SimSubmitter<4> dev;
    dev.gpu = &gpu;
    dev.oldSlotChoice = oldSlotChoice;
    std::atomic<bool> stop{false};
    std::thread gpuThread([&] {
        size_t next = 0;
        while (!stop.load(std::memory_order_acquire))
        {
            uint64_t seq = 0;
            {
                std::lock_guard<std::mutex> q(dev.queueLock);
                if (next < dev.queue.size())
                    seq = dev.queue[next++];
            }
            if (!seq)
            {
                std::this_thread::yield();
                continue;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(20));
            gpu.done.store(seq, std::memory_order_release);
        }
    });
    constexpr int kOwnerLists = 3000, kForeignLists = 600;
    std::thread owner([&] {
        for (int i = 0; i < kOwnerLists; ++i)
            dev.Submit();
    });
    std::thread readback([&] {
        for (int i = 0; i < kForeignLists; ++i)
        {
            // Device::ReadbackImage: record, then wait for the open list
            // (WaitSeq submits it from this thread).
            uint64_t seq;
            {
                deko9::DeviceLockGuard guard(dev.lock, "readback");
                seq = dev.openSeq;
            }
            dev.WaitSeq(seq);
        }
    });
    owner.join();
    readback.join();
    bool drained;
    {
        deko9::DeviceLockGuard guard(dev.lock, "drain");
        const uint64_t last = dev.openSeq - 1;
        // A corrupted ring never completes its missing list: bound the wait.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (gpu.done.load(std::memory_order_acquire) < last && std::chrono::steady_clock::now() < deadline)
        {
            deko9::DeviceUnlockScope unlocked(dev.lock);
            std::this_thread::yield();
        }
        drained = dev.CompletedSeq() == last;
        *staleOut = dev.staleSlots;
        *missingOut = dev.neverSubmitted;
    }
    stop.store(true, std::memory_order_release);
    gpuThread.join();
    *drainedOut = drained;
}

void TestFenceSlotStressRuns()
{
    uint64_t stale = 0, missing = 0, oldStale = 0, oldMissing = 0;
    bool drained = false, oldDrained = false;
    TestFenceSlotStress(false, &stale, &missing, &drained);
    TestFenceSlotStress(true, &oldStale, &oldMissing, &oldDrained);
    std::printf("fence stress (3000 owner + 600 readback lists, 4 slots): fixed stale=%llu missing=%llu "
                "drained=%d; old stale=%llu missing=%llu drained=%d (old: not gated, timing-dependent)\n",
                (unsigned long long)stale, (unsigned long long)missing, drained ? 1 : 0,
                (unsigned long long)oldStale, (unsigned long long)oldMissing, oldDrained ? 1 : 0);
    Check(stale == 0 && missing == 0, "fence stress: no stale slot, no missing list");
    Check(drained, "fence stress: every list completes");
}

// ---- buffer: rename protocol cross-thread words ------------------------------

struct SimMem
{
    uint64_t gpu = 0;
    uint64_t *cpu = nullptr;
};

// One engine buffer: the device Buffer's m_memory/m_spares/m_use.
struct SimBuffer
{
    SimMem memory;
    deko9::RenameSpares<SimMem> spares;
    deko9::BufferUse use;
};

struct MemPool
{
    std::mutex lock;
    std::vector<std::unique_ptr<uint64_t[]>> blocks;
    uint64_t nextGpu = 0x1000;
    SimMem Alloc()
    {
        std::lock_guard<std::mutex> g(lock);
        blocks.emplace_back(new uint64_t[4]());
        SimMem m;
        m.gpu = nextGpu;
        nextGpu += 0x100;
        m.cpu = blocks.back().get();
        return m;
    }
};

// The engine's handoff: the front end (main) fills frame F's buffers and
// hands F to the back end; the back end draws F (stamps, reads the
// address) and reports it done; main fills F+2 into F's buffers only after
// that (two frame slots). Every lock is a DISCARD rename. No report may
// fire and TSan must see no race.
void TestBufferHandoff()
{
    constexpr int kFrames = 400;
    MemPool pool;
    deko9::DeviceLock deviceLock;
    SimBuffer buffers[2];
    for (SimBuffer &b : buffers)
        b.memory = pool.Alloc();
    std::atomic<uint64_t> openSeq{1}, completed{0};
    std::mutex handoff;
    std::condition_variable cv;
    int handed = 0, drawn = 0; // frames handed to / finished by the back end
    uint64_t reports = 0, wrongAddress = 0, grows = 0;
    std::thread backEnd([&] {
        for (int f = 0; f < kFrames; ++f)
        {
            {
                std::unique_lock<std::mutex> g(handoff);
                cv.wait(g, [&] { return handed > f; });
            }
            SimBuffer &b = buffers[f % 2];
            {
                deko9::DeviceLockGuard guard(deviceLock, "draw");
                deviceLock.SetDrawTag(deko9::ThreadTag());
                const uint64_t seq = openSeq.load(std::memory_order_relaxed);
                const uint64_t gpu = b.memory.gpu; // ApplyVertexStreams: address, then stamp
                if (b.memory.cpu[0] != (uint64_t)f)
                    ++wrongAddress;
                if (!b.use.Stamp(seq, deko9::ThreadTag()))
                    ++reports;
                (void)gpu;
                // Present: submit; the fake GPU completes two lists behind.
                openSeq.store(seq + 1, std::memory_order_release);
                if (seq > 2)
                    completed.store(seq - 2, std::memory_order_release);
            }
            {
                std::lock_guard<std::mutex> g(handoff);
                drawn = f + 1;
            }
            cv.notify_all();
        }
    });
    for (int f = 0; f < kFrames; ++f)
    {
        {
            std::unique_lock<std::mutex> g(handoff);
            cv.wait(g, [&] { return drawn >= f - 1; }); // slot f % 2 is free
        }
        SimBuffer &b = buffers[f % 2];
        Check(b.use.BeginLock(deko9::ThreadTag()), "buffer handoff: lock of an unlocked buffer");
        SimMem previous;
        const deko9::RenameResult r = deko9::RenameForLock(
            &b.memory, &previous, b.use.LastUse(), true, completed.load(std::memory_order_acquire),
            openSeq.load(std::memory_order_acquire), b.spares,
            [&](SimMem *out) {
                deko9::DeviceLockGuard guard(deviceLock, "grow");
                *out = pool.Alloc();
                ++grows;
                return true;
            },
            [&](const SimMem &, uint64_t) { deko9::DeviceLockGuard guard(deviceLock, "evict"); });
        if (r.renamed)
            b.use.ClearUse();
        b.memory.cpu[0] = (uint64_t)f;
        b.use.EndLock();
        {
            std::lock_guard<std::mutex> g(handoff);
            handed = f + 1;
        }
        cv.notify_all();
    }
    backEnd.join();
    std::printf("buffer handoff: frames=%d reports=%llu wrongAddress=%llu grows=%llu\n", kFrames,
                (unsigned long long)reports, (unsigned long long)wrongAddress, (unsigned long long)grows);
    Check(reports == 0, "buffer handoff: the engine's frame handoff never reports a locked draw");
    Check(wrongAddress == 0, "buffer handoff: every draw reads its frame's data");
}

// A draw from a buffer another thread holds locked, both orders.
void TestBufferOverlapDetected()
{
    // Lock first: the draw's stamp sees the holder.
    {
        deko9::BufferUse use;
        std::atomic<bool> locked{false}, drawn{false};
        bool stampOk = true;
        std::thread main([&] {
            use.BeginLock(deko9::ThreadTag());
            locked.store(true, std::memory_order_release);
            SpinUntil(drawn);
            use.EndLock();
        });
        std::thread draw([&] {
            SpinUntil(locked);
            stampOk = use.Stamp(7, deko9::ThreadTag());
            drawn.store(true, std::memory_order_release);
        });
        main.join();
        draw.join();
        Check(!stampOk, "buffer overlap: a draw from a buffer locked by another thread is reported");
    }
    // Draw first: the lock sees the open list's stamp and renames past it.
    {
        deko9::BufferUse use;
        deko9::RenameSpares<SimMem> spares;
        MemPool pool;
        SimMem memory = pool.Alloc();
        std::atomic<bool> drawn{false};
        bool renamed = false;
        uint64_t stamp = 0;
        std::thread draw([&] {
            Check(use.Stamp(9, deko9::ThreadTag()), "buffer overlap: an unlocked buffer stamps");
            drawn.store(true, std::memory_order_release);
        });
        std::thread main([&] {
            SpinUntil(drawn);
            use.BeginLock(deko9::ThreadTag());
            SimMem previous;
            const deko9::RenameResult r = deko9::RenameForLock(
                &memory, &previous, use.LastUse(), true, /*completed*/ 8, /*open*/ 9, spares,
                [&](SimMem *out) {
                    *out = pool.Alloc();
                    return true;
                },
                [](const SimMem &, uint64_t) {});
            renamed = r.renamed;
            spares.Drain([&](const SimMem &, uint64_t s) { stamp = s; });
            use.EndLock();
        });
        draw.join();
        main.join();
        Check(renamed && stamp >= 9, "buffer overlap: a lock that sees the stamp renames past the drawing list");
    }
    // Hammered overlap (no handoff): reported, and TSan sees only atomics.
    {
        deko9::BufferUse use;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> reports{0};
        std::thread draw([&] {
            for (uint64_t seq = 1; !stop.load(std::memory_order_relaxed); ++seq)
            {
                if (!use.Stamp(seq, deko9::ThreadTag()))
                    reports.fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (int i = 0; i < 20000 || reports.load() == 0; ++i)
        {
            if (use.BeginLock(deko9::ThreadTag()))
            {
                (void)use.LastUse();
                std::this_thread::yield();
                use.EndLock();
            }
            if (i > 2000000)
                break;
        }
        stop.store(true);
        draw.join();
        std::printf("buffer overlap (hammered): reports=%llu\n", (unsigned long long)reports.load());
        Check(reports.load() > 0, "buffer overlap: concurrent locks and draws are reported");
    }
}

// The skinned-cache pool under the engine's frame handoff with TAAU motion:
// main locks frame f's pool (f % pools, DISCARD) at its frame start and
// keeps it locked until it hands f over, which is after the back end
// finished f-1; meanwhile the back end draws f-1 from its pool and TAAU
// binds frame f-2's pool as `prev` (both stamped). The overlap is forced
// every frame: the back end draws f-1 only once main holds f locked.
// Reports = draws stamped while another thread held the buffer locked.
// `readPrev` reads prev's address as the real draw does (the negative
// control skips it: there the read would race the rename).
uint64_t RunTaauSkinPool(uint32_t pools, bool readPrev, uint64_t *wrongPrevOut)
{
    constexpr int kFrames = 300;
    MemPool memPool;
    deko9::DeviceLock deviceLock;
    std::unique_ptr<SimBuffer[]> buffers(new SimBuffer[pools]);
    for (uint32_t i = 0; i < pools; ++i)
        buffers[i].memory = memPool.Alloc();
    std::atomic<uint64_t> openSeq{1}, completed{0};
    std::mutex handoff;
    std::condition_variable cv;
    int handed = 0, drawn = 0, locked = -1;
    uint64_t reports = 0, wrongPrev = 0;
    std::thread backEnd([&] {
        for (int f = 0; f < kFrames; ++f)
        {
            {
                std::unique_lock<std::mutex> g(handoff);
                cv.wait(g, [&] { return handed > f && locked >= f + 1; });
            }
            SimBuffer &cur = buffers[f % pools];
            {
                deko9::DeviceLockGuard guard(deviceLock, "draw");
                const uint64_t seq = openSeq.load(std::memory_order_relaxed);
                if (!cur.use.Stamp(seq, deko9::ThreadTag()))
                    ++reports;
                if (f > 0)
                {
                    SimBuffer &prev = buffers[(f + pools - 1) % pools]; // RB_TaauBuildMotion's prevSkinVb
                    if (readPrev && prev.memory.cpu[0] != (uint64_t)(f - 1))
                        ++wrongPrev;
                    if (!prev.use.Stamp(seq, deko9::ThreadTag())) // TaauMotion stamps prev
                        ++reports;
                }
                openSeq.store(seq + 1, std::memory_order_release);
                if (seq > 2)
                    completed.store(seq - 2, std::memory_order_release);
            }
            {
                std::lock_guard<std::mutex> g(handoff);
                drawn = f + 1;
            }
            cv.notify_all();
        }
    });
    for (int f = 0; f <= kFrames; ++f)
    {
        SimBuffer &b = buffers[f % pools];
        b.use.BeginLock(deko9::ThreadTag()); // R_LockSkinnedCache
        SimMem previous;
        const deko9::RenameResult r = deko9::RenameForLock(
            &b.memory, &previous, b.use.LastUse(), true, completed.load(std::memory_order_acquire),
            openSeq.load(std::memory_order_acquire), b.spares,
            [&](SimMem *out) {
                deko9::DeviceLockGuard guard(deviceLock, "grow");
                *out = memPool.Alloc();
                return true;
            },
            [&](const SimMem &, uint64_t) { deko9::DeviceLockGuard guard(deviceLock, "evict"); });
        if (r.renamed)
            b.use.ClearUse();
        b.memory.cpu[0] = (uint64_t)f;
        {
            std::unique_lock<std::mutex> g(handoff);
            locked = f;
            cv.notify_all();
            cv.wait(g, [&] { return drawn >= f; }); // R_IssueRenderCommands: the back end finished f-1
        }
        b.use.EndLock(); // R_UnlockSkinnedCache
        if (f == kFrames)
            break;
        {
            std::lock_guard<std::mutex> g(handoff);
            handed = f + 1;
        }
        cv.notify_all();
    }
    backEnd.join();
    *wrongPrevOut = wrongPrev;
    return reports;
}

// The engine's pool size, parsed from its declaration (0 when not found).
uint32_t EngineSkinPoolCount(const std::string &gfxHeader)
{
    const std::string decl = "skinnedCacheVbPool[";
    const size_t at = gfxHeader.find(decl);
    if (at == std::string::npos)
        return 0;
    return (uint32_t)std::strtoul(gfxHeader.c_str() + at + decl.size(), nullptr, 10);
}

void TestTaauSkinPool(const std::string &root)
{
    const uint32_t pools = EngineSkinPoolCount(ReadFile(root + "/src/gfx_d3d/r_gfx.h"));
    const std::string rotate = ReadFile(root + "/src/gfx_d3d/r_rendercmds.cpp");
    const std::string taau = ReadFile(root + "/src/gfx_d3d/r_taau_motion.cpp");
    // The engine rotates over the whole pool and TAAU reads the previous
    // entry of that rotation.
    const bool rotation =
        rotate.find("(gfxBuf.dynamicBufferFrame + 1) % ARRAY_COUNT(gfxBuf.skinnedCacheVbPool)") != std::string::npos &&
        taau.find("skinnedCacheVbPool[(pool + kPools - 1) % kPools]") != std::string::npos;
    uint64_t wrongPrev = 0, wrongPrevTwo = 0;
    const uint64_t reports = pools ? RunTaauSkinPool(pools, true, &wrongPrev) : ~0ull;
    const uint64_t twoReports = RunTaauSkinPool(2, false, &wrongPrevTwo);
    std::printf("taau skin pool: engine pools=%u rotation=%d reports=%llu wrongPrev=%llu; 2 pools reports=%llu\n",
                pools, rotation ? 1 : 0, (unsigned long long)reports, (unsigned long long)wrongPrev,
                (unsigned long long)twoReports);
    Check(pools >= 3, "taau skin pool: the engine's skinned-cache pool has at least 3 entries");
    Check(rotation, "taau skin pool: the engine rotates the whole pool and TAAU binds the previous entry");
    Check(reports == 0, "taau skin pool: no draw from a buffer main holds locked with TAAU reading prev");
    Check(wrongPrev == 0, "taau skin pool: TAAU's prev holds the previous frame's data");
    Check(twoReports > 0, "taau skin pool negative control: 2 pools put TAAU's prev under main's lock");
}

// Negative control, run as its own TSan process: the previous plain stamp
// word, written by the draw thread and read by main. Relaxed turn flags
// force read, write, read rounds while both threads live, and relaxed
// atomics give TSan no happens-before edge, so every round is an unordered
// write/read pair: TSan must report a data race on every run, whatever the
// scheduling (a stop flag let main finish before the draw thread started).
volatile uint64_t g_plainSink;
int RunPlainStampRace()
{
    constexpr int kRounds = 64;
    uint64_t lastUse = 0; // the previous Buffer::lastUse
    std::atomic<int> turn{0}; // even: main reads, odd: the draw thread writes
    auto waitTurn = [&](int want) {
        while (turn.load(std::memory_order_relaxed) != want)
            std::this_thread::yield();
        std::atomic_signal_fence(std::memory_order_seq_cst);
    };
    std::thread draw([&] {
        for (int r = 0; r < kRounds; ++r)
        {
            waitTurn(2 * r + 1);
            lastUse = (uint64_t)r + 1;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            turn.store(2 * r + 2, std::memory_order_relaxed);
        }
    });
    for (int r = 0; r < kRounds; ++r)
    {
        waitTurn(2 * r);
        g_plainSink = lastUse;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        turn.store(2 * r + 1, std::memory_order_relaxed);
    }
    waitTurn(2 * kRounds);
    g_plainSink = lastUse;
    draw.join();
    std::printf("plain stamp race ran %d rounds (TSan must have reported it)\n", kRounds);
    return 0;
}

// ---- pins: shader release during the unlocked prebake build ------------------

struct FakeVariant
{
    uint64_t code = 0;
};

uint64_t VariantCode(uint64_t shaderId, const deko9::VariantSelect &select)
{
    return shaderId * 1000003ull + select.shadowMask * 31ull + (select.earlyZ ? 7ull : 0ull) + select.shaderOpt;
}

std::atomic<int> g_shadersDeleted{0};

// A shader object with a COM-style reference count. Normally the last
// Release deletes it (ASan checks every later access); `poisonOnly` (the
// negative control) only marks it dead so the use after release is counted.
struct FakeShader
{
    std::atomic<int> refs{1};
    std::atomic<bool> dead{false};
    bool poisonOnly = false;
    uint64_t id = 0;
    std::vector<uint64_t> bytecode;
    deko9::VariantSet<FakeVariant> variants; // device lock held
    ~FakeShader() { g_shadersDeleted.fetch_add(1); }
    void AddRef() { refs.fetch_add(1, std::memory_order_relaxed); }
    void Release()
    {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
            return;
        if (poisonOnly)
            dead.store(true, std::memory_order_release);
        else
            delete this;
    }
};

struct FakeJob
{
    FakeShader *shader;
    deko9::VariantSelect select;
    std::vector<uint8_t> dksh;
    bool ok = false;
    bool packHit = false;
};

uint64_t NowNs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Device::PrebakeVariants with the build and install replaced: a build
// thread builds every job without the device lock while this thread
// installs under it; the engine releases `victim` while its first job
// builds.
struct PinOutcome
{
    int usedDead = 0;          // builds/installs that touched a released shader
    int deletedBeforeEnd = 0;  // shaders deleted while the prebake still ran
    int deletedAfterPins = 0;  // shaders deleted once the pins were dropped
    size_t victimVariants = 0; // variants installed into the victim
};

PinOutcome RunPrebakeRelease(bool pinned)
{
    constexpr int kShaders = 6;
    g_shadersDeleted.store(0);
    std::vector<FakeShader *> shaders;
    for (int i = 0; i < kShaders; ++i)
    {
        FakeShader *s = new FakeShader;
        s->id = (uint64_t)i + 1;
        s->poisonOnly = !pinned;
        s->bytecode.assign(64, s->id);
        shaders.push_back(s);
    }
    FakeShader *victim = shaders[2];
    deko9::DeviceLock deviceLock;
    std::atomic<bool> victimBuilding{false}, victimReleased{false};
    std::atomic<int> usedDead{0};
    PinOutcome out;
    {
        deko9::ShaderPins<FakeShader> pins;
        if (pinned)
        {
            for (FakeShader *s : shaders)
                pins.Pin(s);
        }
        std::vector<FakeJob> jobs;
        for (FakeShader *s : shaders)
        {
            for (uint32_t mask = 0; mask < 3; ++mask)
            {
                deko9::VariantSelect select;
                select.shadowMask = mask;
                jobs.push_back(FakeJob{s, select, {}, false, false});
            }
        }
        std::thread engine([&] {
            SpinUntil(victimBuilding);
            victim->Release(); // the zone's material drops the shader
            victimReleased.store(true, std::memory_order_release);
        });
        auto queue = deko9::MakePrebakeQueue(
            jobs,
            [&](FakeJob &job) {
                if (job.shader == victim && !victimBuilding.exchange(true))
                    SpinUntil(victimReleased);
                if (job.shader->dead.load(std::memory_order_acquire))
                    usedDead.fetch_add(1);
                uint64_t sum = 0;
                for (uint64_t w : job.shader->bytecode) // the translator reads the bytecode
                    sum += w;
                job.dksh.assign(8, (uint8_t)sum);
                return true;
            },
            NowNs);
        std::thread builder([&] { queue.BuildAll(); });
        queue.InstallAll([&](FakeJob &job) {
            deko9::DeviceLockGuard guard(deviceLock, "prebake install");
            if (job.shader->dead.load(std::memory_order_acquire))
                usedDead.fetch_add(1);
            job.shader->variants.Install(job.select, [&] {
                std::unique_ptr<FakeVariant> v(new FakeVariant);
                v->code = VariantCode(job.shader->id, job.select);
                return v;
            });
        });
        builder.join();
        engine.join();
        out.deletedBeforeEnd = g_shadersDeleted.load();
        if (pinned || !victim->dead.load())
            out.victimVariants = victim->variants.Size();
    }
    out.deletedAfterPins = g_shadersDeleted.load();
    out.usedDead = usedDead.load();
    // The engine's remaining references.
    for (FakeShader *s : shaders)
    {
        if (s == victim)
            continue;
        s->Release();
    }
    if (!pinned)
        delete victim; // poisoned, never freed by Release
    return out;
}

void TestPrebakePins()
{
    const PinOutcome pinned = RunPrebakeRelease(true);
    const PinOutcome unpinned = RunPrebakeRelease(false);
    std::printf("prebake release: pinned usedDead=%d deletedDuring=%d deletedAfterPins=%d victimVariants=%zu; "
                "unpinned usedDead=%d\n",
                pinned.usedDead, pinned.deletedBeforeEnd, pinned.deletedAfterPins, pinned.victimVariants,
                unpinned.usedDead);
    Check(pinned.usedDead == 0, "pins: no build or install touches a released shader");
    Check(pinned.deletedBeforeEnd == 0, "pins: a shader released during the prebake lives until its last install");
    Check(pinned.deletedAfterPins == 1, "pins: the released shader is deleted once the pins drop");
    Check(pinned.victimVariants == 3, "pins: the released shader's variants were installed");
    Check(unpinned.usedDead > 0, "pins negative control: without pins the build and install use a released shader");
}

// ---- starve: a build thread that never runs cannot stall the install --------

struct StarveOutcome
{
    bool finished = false;     // every job installed within the bound
    int installed = 0;
    int installedAtBound = 0;  // installed when the bounded wait ended
    int builtByInstaller = 0;
    int builtByBuilder = 0;
    bool drained = false;      // InstallAll asked to raise the build thread
};

// One prebake of kJobs jobs with its build thread held blocked: before it
// claims anything (`holdInJob` false: starved from the start, released only
// after the install finished or timed out), or inside the first job it
// claims until InstallAll's drain() (the device's priority raise) runs.
// `waitOnly` is the negative control: the previous installer, which only
// waited for the build thread's results.
StarveOutcome RunStarvedBuilder(bool holdInJob, bool waitOnly, std::chrono::milliseconds bound)
{
    constexpr int kJobs = 24;
    std::vector<FakeJob> jobs;
    FakeShader shader;
    shader.id = 77;
    for (int i = 0; i < kJobs; ++i)
    {
        deko9::VariantSelect select;
        select.shadowMask = (uint32_t)i;
        jobs.push_back(FakeJob{&shader, select, {}, false, false});
    }
    std::atomic<bool> release{false}, drained{false}, builderHeld{false};
    std::atomic<int> byInstaller{0}, byBuilder{0};
    std::atomic<std::thread::id> builderId{};
    // The previous installer's view: results in completion order.
    std::mutex resultsLock;
    std::condition_variable resultsReady;
    std::deque<FakeJob *> results;
    auto queue = deko9::MakePrebakeQueue(
        jobs,
        [&](FakeJob &job) {
            const bool onBuilder = std::this_thread::get_id() == builderId.load();
            if (onBuilder && holdInJob && !builderHeld.exchange(true))
            {
                while (!drained.load(std::memory_order_acquire) && !release.load(std::memory_order_acquire))
                    std::this_thread::yield();
            }
            (onBuilder ? byBuilder : byInstaller).fetch_add(1);
            job.dksh.assign(8, (uint8_t)job.select.shadowMask);
            {
                std::lock_guard<std::mutex> g(resultsLock);
                results.push_back(&job);
            }
            resultsReady.notify_one();
            return true;
        },
        NowNs);
    std::thread builder([&] {
        builderId.store(std::this_thread::get_id());
        if (!holdInJob)
        {
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
        }
        queue.BuildAll();
    });
    if (holdInJob)
        SpinUntil(builderHeld); // the build thread holds its first job before the install starts
    StarveOutcome out;
    std::atomic<int> installed{0};
    std::mutex doneLock;
    std::condition_variable doneCv;
    bool done = false;
    std::thread installer([&] {
        if (waitOnly)
        {
            for (int i = 0; i < kJobs; ++i)
            {
                std::unique_lock<std::mutex> g(resultsLock);
                if (!resultsReady.wait_for(g, bound, [&] { return !results.empty(); }))
                    break; // stuck: the build thread never delivered
                results.pop_front();
                installed.fetch_add(1);
            }
        }
        else
        {
            queue.InstallAll([&](FakeJob &job) { installed.fetch_add(job.ok ? 1 : 0); },
                             [&] { drained.store(true, std::memory_order_release); });
        }
        std::lock_guard<std::mutex> g(doneLock);
        done = true;
        doneCv.notify_all();
    });
    {
        std::unique_lock<std::mutex> g(doneLock);
        out.finished = doneCv.wait_for(g, bound, [&] { return done; }) && installed.load() == kJobs;
        out.installedAtBound = installed.load();
    }
    // Let a still-held build thread finish so everything joins.
    release.store(true, std::memory_order_release);
    builder.join();
    if (waitOnly)
        queue.BuildAll();
    installer.join();
    out.installed = installed.load();
    out.builtByInstaller = byInstaller.load();
    out.builtByBuilder = byBuilder.load();
    out.drained = drained.load();
    return out;
}

void TestPrebakeStarvedBuilder()
{
    const auto bound = std::chrono::milliseconds(20000);
    const StarveOutcome starved = RunStarvedBuilder(false, false, bound);
    const StarveOutcome heldInJob = RunStarvedBuilder(true, false, bound);
    const StarveOutcome waitOnly = RunStarvedBuilder(false, true, std::chrono::milliseconds(300));
    std::printf("prebake starved builder: starved finished=%d installed=%d byInstaller=%d byBuilder=%d; held in job "
                "finished=%d installed=%d byInstaller=%d byBuilder=%d drained=%d; previous wait-only finished=%d "
                "installed=%d (300 ms bound)\n",
                starved.finished ? 1 : 0, starved.installed, starved.builtByInstaller, starved.builtByBuilder,
                heldInJob.finished ? 1 : 0, heldInJob.installed, heldInJob.builtByInstaller, heldInJob.builtByBuilder,
                heldInJob.drained ? 1 : 0, waitOnly.finished ? 1 : 0, waitOnly.installedAtBound);
    Check(starved.finished && starved.builtByInstaller == starved.installed,
          "starve: a build thread that never runs does not stall the install (the installer builds every job)");
    Check(heldInJob.finished && heldInJob.drained && heldInJob.builtByBuilder == 1,
          "starve: a build thread held inside a job is raised by drain() and its job is installed");
    Check(!waitOnly.finished && waitOnly.installedAtBound == 0,
          "starve negative control: the previous wait-only installer is stuck behind a starved build thread");
}

// ---- merged: prebake installs, draw lookups, lock-free frame APIs ------------

struct SimFence
{
    uint64_t seq;
};

void TestMergedInteraction()
{
    constexpr int kShaders = 12, kSelects = 6, kFrames = 240, kDrawsPerFrame = 40;
    constexpr uint32_t kInFlight = 2;
    deko9::DeviceLock deviceLock;
    std::vector<std::unique_ptr<FakeShader>> shaders;
    for (int i = 0; i < kShaders; ++i)
    {
        shaders.emplace_back(new FakeShader);
        shaders.back()->id = (uint64_t)i + 1;
        shaders.back()->bytecode.assign(32, (uint64_t)i);
    }
    const auto build = [](const FakeShader &shader, const deko9::VariantSelect &select) {
        uint64_t sum = 0;
        for (uint64_t w : shader.bytecode)
            sum += w;
        (void)sum;
        return VariantCode(shader.id, select);
    };
    deko9::FramePublish<kInFlight, SimFence> pub;
    std::atomic<uint64_t> gpuDone{0}, submitted{0};
    std::atomic<uint64_t> frameEnd[kFrames + 2] = {};
    std::atomic<bool> drawFinished{false}, prebakeFinished{false};
    std::atomic<uint64_t> wrongCode{0}, drawBuilds{0}, earlyDone{0}, mainPolls{0}, mainDone{0};

    std::thread gpu([&] {
        while (!drawFinished.load(std::memory_order_acquire) ||
               gpuDone.load(std::memory_order_relaxed) < submitted.load(std::memory_order_acquire))
        {
            const uint64_t target = submitted.load(std::memory_order_acquire);
            if (gpuDone.load(std::memory_order_relaxed) < target)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                gpuDone.store(gpuDone.load(std::memory_order_relaxed) + 1, std::memory_order_release);
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });

    // Loading thread: prebake every shader's first four selects.
    std::thread loading([&] {
        std::vector<FakeJob> jobs;
        deko9::ShaderPins<FakeShader> pins;
        for (auto &s : shaders)
        {
            pins.Pin(s.get());
            for (uint32_t m = 0; m < 4; ++m)
            {
                deko9::VariantSelect select;
                select.shadowMask = m;
                jobs.push_back(FakeJob{s.get(), select, {}, false, false});
            }
        }
        auto queue = deko9::MakePrebakeQueue(
            jobs,
            [&](FakeJob &job) {
                const uint64_t code = build(*job.shader, job.select);
                job.dksh.assign((const uint8_t *)&code, (const uint8_t *)&code + sizeof(code));
                std::this_thread::sleep_for(std::chrono::microseconds(300)); // a compile
                return true;
            },
            NowNs);
        std::thread builder([&] { queue.BuildAll(); });
        queue.InstallAll([&](FakeJob &job) {
            deko9::DeviceLockGuard guard(deviceLock, "prebake install");
            uint64_t code;
            std::memcpy(&code, job.dksh.data(), sizeof(code));
            job.shader->variants.Install(job.select, [&] {
                std::unique_ptr<FakeVariant> v(new FakeVariant);
                v->code = code;
                return v;
            });
        });
        builder.join();
        prebakeFinished.store(true, std::memory_order_release);
    });
    // Pins are released by the loading thread; the shaders stay owned here.
    for (auto &s : shaders)
        s->AddRef();

    // Draw thread: batches under the device lock, a lookup per draw (a miss
    // builds under the lock like a draw-time Variant), one present per frame.
    std::thread draw([&] {
        uint32_t rng = 12345;
        uint64_t seq = 1;
        for (uint64_t frame = 1; frame <= (uint64_t)kFrames; ++frame)
        {
            // Frames in flight: frame - kInFlight must be done before its slot is reused.
            if (frame > kInFlight)
            {
                while (gpuDone.load(std::memory_order_acquire) < frameEnd[frame - kInFlight].load())
                    std::this_thread::yield();
                pub.PublishDone(frame - kInFlight);
            }
            {
                deko9::DeviceLockGuard batch(deviceLock, "batch");
                deviceLock.SetDrawTag(deko9::ThreadTag());
                for (int d = 0; d < kDrawsPerFrame; ++d)
                {
                    rng = rng * 1664525u + 1013904223u;
                    FakeShader &shader = *shaders[(rng >> 8) % kShaders];
                    deko9::VariantSelect select;
                    select.shadowMask = (rng >> 16) % kSelects;
                    const FakeVariant *v = shader.variants.Find(select);
                    if (!v)
                    {
                        drawBuilds.fetch_add(1, std::memory_order_relaxed);
                        const uint64_t code = build(shader, select);
                        v = shader.variants.Install(select, [&] {
                            std::unique_ptr<FakeVariant> nv(new FakeVariant);
                            nv->code = code;
                            return nv;
                        });
                    }
                    if (!v || v->code != VariantCode(shader.id, select))
                        wrongCode.fetch_add(1, std::memory_order_relaxed);
                    deviceLock.HandOffIfContended();
                }
                // Present: the frame's last list.
                frameEnd[frame].store(seq);
                submitted.store(seq, std::memory_order_release);
                pub.PublishPresent(frame, seq, SimFence{seq});
                ++seq;
            }
        }
        drawFinished.store(true, std::memory_order_release);
    });

    // Main thread: the lock-free frame APIs (end-fence and GPU-sync polls).
    while (!drawFinished.load(std::memory_order_acquire))
    {
        const uint64_t presented = pub.Presented();
        for (uint64_t f = presented > 3 ? presented - 3 : 1; f <= presented + 1; ++f)
        {
            mainPolls.fetch_add(1, std::memory_order_relaxed);
            const bool done = deko9::PublishedFrameDone(pub, 0, f, [&](const SimFence &fence) {
                return gpuDone.load(std::memory_order_acquire) >= fence.seq;
            });
            if (!done)
                continue;
            mainDone.fetch_add(1, std::memory_order_relaxed);
            const uint64_t end = f <= (uint64_t)kFrames ? frameEnd[f].load() : 0;
            if (!end || gpuDone.load(std::memory_order_acquire) < end)
                earlyDone.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::yield();
    }
    draw.join();
    loading.join();
    gpu.join();
    size_t maxVariants = 0;
    {
        deko9::DeviceLockGuard guard(deviceLock, "check");
        for (auto &s : shaders)
            maxVariants = std::max(maxVariants, s->variants.Size());
    }
    for (auto &s : shaders)
    {
        // Back to the single owning reference (the unique_ptr frees it).
        s->refs.fetch_sub(1);
    }
    std::printf("merged: draws=%d drawBuilds=%llu wrongCode=%llu maxVariants=%zu mainPolls=%llu mainDone=%llu "
                "earlyDone=%llu prebake=%d\n",
                kFrames * kDrawsPerFrame, (unsigned long long)drawBuilds.load(),
                (unsigned long long)wrongCode.load(), maxVariants, (unsigned long long)mainPolls.load(),
                (unsigned long long)mainDone.load(), (unsigned long long)earlyDone.load(),
                prebakeFinished.load() ? 1 : 0);
    Check(wrongCode.load() == 0, "merged: every lookup returns the variant for its select");
    Check(maxVariants <= (size_t)kSelects, "merged: one installed variant per select (no duplicate install)");
    Check(earlyDone.load() == 0, "merged: no frame reported done before the GPU passed it");
    Check(mainDone.load() > 0, "merged: the main thread saw frames complete");
    Check(prebakeFinished.load(), "merged: the prebake finished");
}

// ---- defaults: device defaults equal the registered dvar defaults ------------

// The body of the function defined as `signature` (brace-matched; empty when
// absent).
std::string FunctionBody(const std::string &src, const std::string &signature)
{
    const size_t at = src.find(signature);
    if (at == std::string::npos)
        return std::string();
    const size_t open = src.find('{', at);
    if (open == std::string::npos)
        return std::string();
    int depth = 0;
    for (size_t i = open; i < src.size(); ++i)
    {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}' && --depth == 0)
            return src.substr(open, i - open + 1);
    }
    return std::string();
}


double ParseNumber(const std::string &token, bool *ok)
{
    std::string t = token;
    while (!t.empty() && (t.back() == 'f' || t.back() == 'F' || t.back() == ' ' || t.back() == '\n'))
        t.pop_back();
    if (t == "true")
        return 1;
    if (t == "false")
        return 0;
    char *end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    *ok = end && *end == 0 && !t.empty();
    return v;
}

// Splits the arguments of the call whose '(' is at `open` (top-level commas
// only; string literals and nested parentheses are skipped).
std::vector<std::string> CallArgs(const std::string &src, size_t open)
{
    std::vector<std::string> args;
    std::string cur;
    int depth = 0;
    bool inString = false;
    for (size_t i = open + 1; i < src.size(); ++i)
    {
        const char c = src[i];
        if (inString)
        {
            cur += c;
            if (c == '\\' && i + 1 < src.size())
                cur += src[++i];
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
            inString = true;
        else if (c == '(')
            ++depth;
        else if (c == ')' && depth-- == 0)
        {
            args.push_back(cur);
            break;
        }
        if (c == ',' && depth == 0)
        {
            args.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    for (std::string &a : args)
    {
        const size_t b = a.find_first_not_of(" \t\r\n");
        const size_t e = a.find_last_not_of(" \t\r\n");
        a = b == std::string::npos ? std::string() : a.substr(b, e - b + 1);
    }
    return args;
}

// The registered default of `name`: the second argument of
// Dvar_Register<Type>("name", ...), the third for an enum (names first).
bool RegisteredDefault(const std::string &sources, const std::string &name, double *value)
{
    const std::string quoted = "\"" + name + "\"";
    for (size_t at = sources.find(quoted); at != std::string::npos; at = sources.find(quoted, at + 1))
    {
        // Back over whitespace to the '(' of a Dvar_Register<Type> call.
        size_t open = at;
        while (open > 0 && std::isspace((unsigned char)sources[open - 1]))
            --open;
        if (!open || sources[open - 1] != '(')
            continue;
        --open;
        size_t nameStart = open;
        while (nameStart > 0 && (std::isalnum((unsigned char)sources[nameStart - 1]) || sources[nameStart - 1] == '_'))
            --nameStart;
        const std::string fn = sources.substr(nameStart, open - nameStart);
        if (fn.compare(0, 13, "Dvar_Register") != 0)
            continue;
        const std::vector<std::string> args = CallArgs(sources, open);
        const bool isEnum = fn == "Dvar_RegisterEnum";
        if (args.size() < (isEnum ? 3u : 2u))
            return false;
        bool ok = true;
        *value = ParseNumber(args[isEnum ? 2 : 1], &ok);
        return ok;
    }
    return false;
}

// Every r_* identifier dereferenced in the arguments of a Deko9_Set* call.
std::set<std::string> SetterDvars(const std::string &sources)
{
    std::set<std::string> names;
    for (size_t at = sources.find("Deko9_Set"); at != std::string::npos; at = sources.find("Deko9_Set", at + 1))
    {
        size_t open = at + 9;
        while (open < sources.size() && (std::isalnum((unsigned char)sources[open]) || sources[open] == '_'))
            ++open;
        if (open >= sources.size() || sources[open] != '(')
            continue;
        const size_t end = sources.find(';', open);
        const std::string args = sources.substr(open, end - open);
        for (size_t r = args.find("r_"); r != std::string::npos; r = args.find("r_", r + 1))
        {
            if (r && (std::isalnum((unsigned char)args[r - 1]) || args[r - 1] == '_'))
                continue;
            size_t e = r;
            while (e < args.size() && (std::isalnum((unsigned char)args[e]) || args[e] == '_'))
                ++e;
            if (args.compare(e, 2, "->") == 0)
                names.insert(args.substr(r, e - r));
        }
    }
    return names;
}

struct DeviceDefault
{
    const char *dvar;
    double value;
};

std::vector<DeviceDefault> CurrentDefaults()
{
    const uint32_t pd = DEKO9_DEFAULT_PERDRAW;
    return {
        {"r_deko9Verify", DEKO9_DEFAULT_VERIFY ? 1.0 : 0.0},
        {"r_deko9EarlyZ", DEKO9_DEFAULT_EARLY_Z ? 1.0 : 0.0},
        {"r_deko9HazardCache", (pd & DEKO9_PERDRAW_HAZARD) ? 1.0 : 0.0},
        {"r_deko9ConstFast", (pd & DEKO9_PERDRAW_CONSTS) ? 1.0 : 0.0},
        {"r_deko9TexIncremental", (pd & DEKO9_PERDRAW_TEXTURES) ? 1.0 : 0.0},
        {"r_deko9StaticHazard", (pd & DEKO9_PERDRAW_STATICTEX) ? 1.0 : 0.0},
        {"r_deko9GpuPasses", DEKO9_DEFAULT_GPU_PASSES ? 1.0 : 0.0},
        {"r_deko9DrawProbe", (double)DEKO9_DEFAULT_DRAW_PROBE},
        {"r_deko9DrawSplit", (double)DEKO9_DEFAULT_DRAW_SPLIT},
        {"r_deko9LightBarriers", (double)DEKO9_DEFAULT_BARRIER_MODE},
        {"r_deko9TiledCache", (double)DEKO9_DEFAULT_TILED_CACHE},
        {"r_deko9ZcullStats", DEKO9_DEFAULT_ZCULL_STATS ? 1.0 : 0.0},
        {"r_shadowFilter", (double)DEKO9_DEFAULT_SHADOW_FILTER},
        {"r_deko9ShaderOpt", (double)DEKO9_DEFAULT_SHADER_OPT},
        {"r_deko9Census", DEKO9_DEFAULT_CENSUS ? 1.0 : 0.0},
        {"r_deko9FaultTrace", (double)DEKO9_DEFAULT_FAULT_TRACE},
        {"r_deko9GpuMap", (double)DEKO9_DEFAULT_GPU_MAP},
        {"r_deko9RtCompression", DEKO9_DEFAULT_RT_COMPRESSION ? 1.0 : 0.0},
        {"r_fsrSharpness", (double)DEKO9_DEFAULT_UPSCALE_SHARPNESS},
        {"r_fsrMode", (double)DEKO9_DEFAULT_UPSCALE_MODE},
        {"r_deko9CmdChunkKB", (double)DEKO9_DEFAULT_CMD_CHUNK_KB},
        {"r_deko9Prebake", DEKO9_DEFAULT_PREBAKE ? 1.0 : 0.0},
        // The device starts with the draw census off (its slots are set up
        // by the first non-zero mode).
        {"r_deko9DrawCensus", 0.0},
    };
}

// Mismatching dvars, or "?name" for a dvar whose registration was not found.
std::vector<std::string> CompareDefaults(const std::string &sources, const std::vector<DeviceDefault> &defaults)
{
    std::vector<std::string> bad;
    for (const DeviceDefault &d : defaults)
    {
        double registered = 0;
        if (!RegisteredDefault(sources, d.dvar, &registered))
            bad.push_back(std::string("?") + d.dvar);
        else if ((float)registered != (float)d.value)
            bad.push_back(d.dvar);
    }
    return bad;
}

void TestDefaults(const std::string &root)
{
    const char *files[] = {"src/gfx_d3d/r_dvars.cpp", "src/gfx_d3d/rb_ab_tour.cpp", "src/gfx_d3d/rb_backend.cpp",
                           "src/gfx_d3d/r_init.cpp"};
    std::string sources;
    for (const char *f : files)
        sources += ReadFile(root + "/" + f);
    Check(sources.size() > 100000, "defaults: engine sources readable (argv[1] = repository root)");
    const std::vector<DeviceDefault> current = CurrentDefaults();
    const std::vector<std::string> bad = CompareDefaults(sources, current);
    for (const std::string &b : bad)
        std::printf("defaults mismatch: %s\n", b.c_str());
    Check(bad.empty(), "defaults: every device default equals its dvar's registered default");

    // Coverage: every r_* dvar a Deko9_Set* call passes has a default row.
    std::set<std::string> covered;
    for (const DeviceDefault &d : current)
        covered.insert(d.dvar);
    std::set<std::string> uncovered;
    for (const std::string &name : SetterDvars(sources))
    {
        // A string filter for the census mode, not a device default.
        if (name != "r_deko9DrawCensusPasses" && !covered.count(name))
            uncovered.insert(name);
    }
    for (const std::string &u : uncovered)
        std::printf("defaults: %s is applied by a Deko9_Set* call but has no device default row\n", u.c_str());
    Check(uncovered.empty(), "defaults: every dvar a Deko9_Set* call applies is covered");

    // Negative control: the previous device defaults.
    std::vector<DeviceDefault> old = current;
    for (DeviceDefault &d : old)
    {
        if (!std::strcmp(d.dvar, "r_deko9ShaderOpt"))
            d.value = (double)DEKO9_SHADER_OPT_ALL;
        if (!std::strcmp(d.dvar, "r_deko9StaticHazard"))
            d.value = 0.0;
    }
    const std::vector<std::string> oldBad = CompareDefaults(sources, old);
    std::printf("defaults: %zu rows checked, %zu mismatches; previous device defaults: %zu mismatches\n",
                current.size(), bad.size(), oldBad.size());
    Check(oldBad.size() == 2 && oldBad[0] == "r_deko9StaticHazard" && oldBad[1] == "r_deko9ShaderOpt",
          "defaults negative control: the previous device defaults differ in exactly r_deko9StaticHazard and "
          "r_deko9ShaderOpt");
    // The plan's options are the device's defaults (what a prebake before the
    // first frame would use).
    const deko9::PlanOptions plan;
    Check(plan.shaderOpt == DEKO9_DEFAULT_SHADER_OPT && plan.earlyZ == DEKO9_DEFAULT_EARLY_Z &&
              plan.shadowFilter == DEKO9_DEFAULT_SHADOW_FILTER,
          "defaults: the variant plan's options are the device defaults");

    // The zones loaded before the first frame (created and prebaked with the
    // device's options) see the dvars' values, not only the defaults: device
    // creation applies every option the variant plan reads.
    const char *planSetters[] = {"Deko9_SetEarlyZ(", "Deko9_SetShadowFilter(", "Deko9_SetShaderOpt("};
    const auto appliesPlanOptions = [&](const std::string &createBody) {
        const size_t created = createBody.find("CreateDevice(");
        if (created == std::string::npos)
            return false;
        for (const char *setter : planSetters)
        {
            if (createBody.find(setter, created) == std::string::npos)
                return false;
        }
        return true;
    };
    const std::string create = FunctionBody(ReadFile(root + "/src/gfx_d3d/r_init.cpp"),
                                            "HRESULT __cdecl R_CreateDeviceInternal(");
    // Negative control: the previous creation applied only r_deko9GpuMap.
    const std::string previousCreate = "{ hr = dx.d3d9->CreateDevice(a, t, h, b, p, &dx.device); if (hr >= 0) { "
                                       "Deko9_SetGpuMap(dx.device, 0u); break; } }";
    const bool applied = appliesPlanOptions(create), previousApplied = appliesPlanOptions(previousCreate);
    // An unregistered r_deko9ShaderOpt falls back to the device default.
    const bool allFallback = ReadFile(root + "/src/gfx_d3d/rb_backend.cpp").find(": DEKO9_SHADER_OPT_ALL)") !=
                             std::string::npos;
    std::printf("defaults: device creation applies the plan options=%d (previous=%d); ShaderOpt ALL fallback=%d\n",
                applied ? 1 : 0, previousApplied ? 1 : 0, allFallback ? 1 : 0);
    Check(applied, "defaults: device creation applies r_deko9EarlyZ, r_shadowFilter and r_deko9ShaderOpt");
    Check(!previousApplied, "defaults negative control: the previous device creation is detected");
    Check(!allFallback, "defaults: no Deko9_SetShaderOpt fallback to every option");
}

// ---- prebake switch: r_deko9Prebake 0 skips the zone-load prebake -----------

// The zone-load entry point gates all of its work on r_deko9Prebake: it
// collects and builds only inside the Deko9_PrebakeIfEnabled call.
bool EntryIsGated(const std::string &entryBody)
{
    const size_t gate = entryBody.find("Deko9_PrebakeIfEnabled(");
    return gate != std::string::npos && entryBody.find("r_deko9Prebake") < gate &&
           entryBody.find("Deko9_PrebakeVariants(") == std::string::npos &&
           entryBody.find("DB_EnumXAssets(") == std::string::npos;
}

void TestPrebakeSwitch(const std::string &root)
{
    // The gate itself: off runs nothing, on (the negative control) runs the
    // prebake once.
    int runsOff = 0, runsOn = 0;
    const bool ranOff = Deko9_PrebakeIfEnabled(false, [&] { ++runsOff; });
    const bool ranOn = Deko9_PrebakeIfEnabled(true, [&] { ++runsOn; });
    // The engine's entry point routes the collection and the device call
    // through it.
    const std::string src = ReadFile(root + "/src/port/switch_shader_prebake.cpp");
    const std::string entry = FunctionBody(src, "void R_PrebakeShaderVariants(const char *zoneName)");
    const std::string zone = FunctionBody(src, "void R_PrebakeZoneVariants(const char *zoneName)");
    const bool gated = EntryIsGated(entry);
    const bool zoneBuilds = zone.find("Deko9_PrebakeVariants(") != std::string::npos &&
                            zone.find("DB_EnumXAssets(") != std::string::npos;
    // Negative control: the previous entry point, which collected and built
    // unconditionally.
    const std::string previous = "{ if (!dx.device) return; PrebakeCollect collect; DB_EnumXAssets(ASSET_TYPE_MATERIAL, "
                                 "R_PrebakeCollectMaterial, &collect, true); Deko9_PrebakeVariants(dx.device, "
                                 "collect.passes.data(), n, cpu, prio, &result); }";
    const bool previousGated = EntryIsGated(previous);
    std::printf("prebake switch: off runs=%d ran=%d; on runs=%d ran=%d; entry gated=%d zone builds=%d; previous "
                "entry gated=%d\n",
                runsOff, ranOff ? 1 : 0, runsOn, ranOn ? 1 : 0, gated ? 1 : 0, zoneBuilds ? 1 : 0,
                previousGated ? 1 : 0);
    Check(runsOff == 0 && !ranOff, "prebake switch: r_deko9Prebake 0 skips the prebake");
    Check(runsOn == 1 && ranOn, "prebake switch negative control: r_deko9Prebake 1 runs it once");
    Check(gated, "prebake switch: R_PrebakeShaderVariants collects and builds only through the r_deko9Prebake gate");
    Check(zoneBuilds, "prebake switch: the gated zone function collects the materials and builds their variants");
    Check(!previousGated, "prebake switch negative control: the previous ungated entry point is detected");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "--plain-stamp-race"))
        return RunPlainStampRace();
    const std::string root = argc > 1 ? argv[1] : ".";
    TestFenceSlotInterleave();
    TestFenceSlotStressRuns();
    TestBufferHandoff();
    TestBufferOverlapDetected();
    TestTaauSkinPool(root);
    TestPrebakePins();
    TestPrebakeStarvedBuilder();
    TestMergedInteraction();
    TestDefaults(root);
    TestPrebakeSwitch(root);
    if (g_failures)
    {
        std::printf("FAIL:DEKO9_HAZARDS failures=%d\n", g_failures);
        return 1;
    }
    std::printf("PASS:DEKO9_HAZARDS\n");
    return 0;
}
