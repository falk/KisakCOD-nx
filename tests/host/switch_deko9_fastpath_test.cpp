// Host test (ASan/UBSan) for the pure parts of the deko9 native submission
// fast path: the device lock and single-submitter rule, sampler-state packing
// and caches, the engine's
// live-texture memo, and the static-texture
// hazard-skip model against the full per-draw tracker. Run by ./test host
// (deko9_fastpath_sanitizer_check).

#include "src/deko9/deko9_fastpath.h"
#include "src/deko9/deko9_hazard_model.h"
#include "src/deko9/deko9_lock.h"
#include "src/deko9/deko9_shader_stats.h"
#include "src/gfx_d3d/r_image_live_memo.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

namespace
{

int g_failures;

void Check(bool ok, const char *name)
{
    if (!ok)
    {
        std::printf("FAIL:DEKO9_FASTPATH %s\n", name);
        ++g_failures;
    }
}

// ---- shader build accounting ----------------------------------------------------

// Pack hits/misses, compile time and max, the slow-compile log budget, bake
// and first-bind lock time, the window reset and the perf-line fields.
void TestShaderBuildStats()
{
    using deko9::ShaderBuildStats;
    ShaderBuildStats stats;
    stats.NotePackLookup(true);
    stats.NotePackLookup(true);
    stats.NotePackLookup(false);
    stats.NoteTranslate(400000);
    bool logged[7];
    for (int i = 0; i < 7; ++i)
        logged[i] = stats.NoteCompile(i == 0 ? 1000000 : 6000000 + (uint64_t)i * 1000); // 1 fast, 6 slow
    Check(!logged[0], "shader stats: a fast compile is not logged");
    Check(logged[1] && logged[2] && logged[3] && logged[4], "shader stats: the first slow compiles are logged");
    Check(!logged[5] && !logged[6], "shader stats: slow compiles past the window budget are not logged");
    stats.NoteBake(7000000, true);
    stats.NoteBake(2000, false);
    stats.NoteFirstBind(7000000);
    stats.NoteFirstBind(0);
    // Concurrent adds from several threads (relaxed atomics).
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&stats] {
            for (int i = 0; i < 1000; ++i)
                stats.NotePackLookup(true);
        });
    for (std::thread &t : threads)
        t.join();
    const deko9::ShaderBuildWindow w = stats.Take();
    Check(w.packHits == 4002 && w.packMisses == 1, "shader stats: pack hits/misses");
    Check(w.compiles == 7 && w.compileMaxNs == 6006000, "shader stats: compile count and max");
    Check(w.slowCompiles == 6 && w.slowUnlogged == 2, "shader stats: slow compile count / unlogged");
    Check(w.bakes == 2 && w.bakesUnlocked == 1 && w.bakeMaxNs == 7000000, "shader stats: bakes");
    Check(w.firstBinds == 2 && w.firstBindBakeNs == 7000000 && w.firstBindBakeMaxNs == 7000000,
          "shader stats: first binds");
    char line[400];
    ShaderBuildStats::Format(w, line, sizeof(line));
    Check(std::strstr(line, " spHits=4002 spMisses=1 translates=1 translateUs=400 compiles=7 ") == line,
          "shader stats: perf-line prefix");
    Check(std::strstr(line, " compileMaxUs=6006 bakes=2 bakeUs=7002 bakeMaxUs=7000 bakesUnlocked=1 firstBinds=2 "
                            "firstBindBakeUs=7000 firstBindBakeMaxUs=7000 slowCompiles=6 slowUnlogged=2") != nullptr,
          "shader stats: perf-line fields");
    // The window resets, and the log budget with it.
    const deko9::ShaderBuildWindow empty = stats.Take();
    Check(empty.packHits == 0 && empty.compiles == 0 && empty.compileMaxNs == 0, "shader stats: window reset");
    Check(stats.NoteCompile(ShaderBuildStats::kSlowCompileNs), "shader stats: budget renews per window");
}

// ---- device lock ----------------------------------------------------------------

// One hand-off between draws with a real waiter thread: the caller holds
// `batch` (any depth); a waiter blocks on it; the caller hands off until the
// waiter has run, then joins it.
//
// HandOffIfContended is best-effort by design (bounded spin): the woken
// waiter can lose the race for the mutex to the holder's re-lock (glibc
// mutexes barge; observed ~1.5% of runs under TSan). The holder releases the
// lock again at the next hand-off or at batch end, so the game never waits
// forever, but a test that joins the waiter while still holding the lock
// after one hand-off deadlocks (the 18-minute ./test host hang: main in
// std::thread::join, the waiter in DeviceLock::lock's m_mutex.lock). So the
// round retries hand-offs against a deadline and, if the waiter still has
// not run, releases the lock completely before joining: a failure, never a
// hang.
struct HandOffRound
{
    bool waiterCounted = false; // HasWaiters() saw the blocked waiter
    bool waiterEarly = false;   // the waiter got in before any hand-off
    bool waiterRan = false;     // it ran within the deadline
    uint64_t handOffs = 0;      // hand-offs it took (1 unless the waiter lost the race)
};

HandOffRound RunHandOffRound(deko9::DeviceLock &batch)
{
    using Clock = std::chrono::steady_clock;
    HandOffRound r;
    const uint64_t h0 = batch.HandOffs();
    std::atomic<bool> waiterIn{false}, waiterDone{false};
    std::thread waiter([&] {
        waiterIn.store(true);
        deko9::DeviceLockGuard g(batch);
        waiterDone.store(true);
    });
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
    while (!waiterIn.load() && Clock::now() < deadline)
        std::this_thread::yield();
    // Let it reach the contended path (try_lock fails, waiters++).
    while (!batch.HasWaiters() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    r.waiterCounted = batch.HasWaiters();
    r.waiterEarly = waiterDone.load();
    while (!waiterDone.load() && Clock::now() < deadline)
        batch.HandOffIfContended();
    r.waiterRan = waiterDone.load();
    r.handOffs = batch.HandOffs() - h0;
    if (!r.waiterRan)
    {
        // Never join while holding the lock the waiter blocks on.
        const uint32_t depth = batch.ReleaseForBlocking();
        waiter.join();
        batch.ReacquireAfterBlocking(depth);
    }
    else
    {
        waiter.join();
    }
    return r;
}

// Regression for the hand-off hang: many rounds, counting the ones where the
// first hand-off lost the race (the case that deadlocked the old test).
// Bounded by the per-round deadline and the process watchdog.
void TestHandOffStress()
{
    deko9::DeviceLock batch;
    batch.lock();
    batch.lock();
    constexpr int kRounds = 2000;
    int ran = 0, lostRace = 0;
    for (int i = 0; i < kRounds; ++i)
    {
        const HandOffRound r = RunHandOffRound(batch);
        ran += r.waiterRan;
        lostRace += r.handOffs > 1;
    }
    Check(ran == kRounds, "lock stress: every waiter ran through a hand-off");
    Check(batch.OwnedByCaller() && batch.Depth() == 2 && !batch.HasWaiters(), "lock stress: holder resumed at its depth");
    batch.unlock();
    batch.unlock();
    std::printf("DEKO9_FASTPATH lock stress: rounds=%d lost_race=%d handoffs=%llu\n", kRounds, lostRace,
                (unsigned long long)batch.HandOffs());
}

void TestLock()
{
    deko9::DeviceLock lock;
    Check(!lock.OwnedByCaller(), "lock: fresh lock not owned");
    lock.lock();
    Check(lock.OwnedByCaller() && lock.Depth() == 1, "lock: owned after lock");
    lock.lock();
    lock.lock();
    Check(lock.Depth() == 3, "lock: recursion depth");
    Check(lock.Acquisitions() == 1 && lock.Entries() == 3, "lock: re-entries are not acquisitions");
    lock.unlock();
    lock.unlock();
    Check(lock.OwnedByCaller(), "lock: still owned at depth 1");
    // Another thread must not see ownership, and must block until released.
    bool otherOwned = true;
    std::thread([&] { otherOwned = lock.OwnedByCaller(); }).join();
    Check(!otherOwned, "lock: other thread does not own it");
    lock.unlock();
    Check(!lock.OwnedByCaller(), "lock: released");

    // Mutual exclusion under contention, with nested (batch-style) entries.
    long counter = 0;
    constexpr int kIters = 20000;
    auto worker = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            deko9::DeviceLockGuard outer(lock); // batch
            deko9::DeviceLockGuard inner(lock); // per-call re-entry
            const long v = counter;
            counter = v + 1;
        }
    };
    std::thread a(worker), b(worker), c(worker);
    a.join();
    b.join();
    c.join();
    Check(counter == 3L * kIters, "lock: mutual exclusion");
    Check(!lock.OwnedByCaller() && lock.Depth() == 0, "lock: balanced after contention");

    // Contention accounting (DEKO9 perf lock): a thread that blocks behind
    // the holder is counted once with the time it waited, attributed to the
    // draw thread only when it carries the draw tag; uncontended takes and
    // re-entries count nothing.
    {
        deko9::DeviceLock held;
        const uint64_t c0 = held.Contended(false), c1 = held.Contended(true);
        held.lock();
        held.lock();
        held.unlock();
        held.unlock();
        Check(held.Contended(false) == c0 && held.Contended(true) == c1 && held.WaitNs(false) == 0,
              "lock: uncontended takes are not contention");
        std::atomic<bool> waiting{false};
        held.lock(); // "batch" held by this thread
        held.SetDrawTag(deko9::ThreadTag());
        std::thread other([&] {
            waiting.store(true);
            deko9::DeviceLockGuard g(held); // blocks until the batch ends
        });
        while (!waiting.load())
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        held.unlock();
        other.join();
        held.lock();
        Check(held.Contended(false) == 1 && held.Contended(true) == 0, "lock: the blocked other thread is counted once");
        Check(held.WaitNs(false) >= 10000000ull, "lock: the wait time is the time blocked (>= 10 ms of 20)");
        held.unlock();
        // The draw thread blocking behind another thread's lock lands in the draw bucket.
        std::atomic<bool> holderReady{false}, release{false};
        std::thread holder([&] {
            deko9::DeviceLockGuard g(held);
            holderReady.store(true);
            while (!release.load())
                std::this_thread::yield();
        });
        while (!holderReady.load())
            std::this_thread::yield();
        std::thread releaser([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            release.store(true);
        });
        held.lock(); // this thread carries the draw tag
        Check(held.Contended(true) == 1 && held.WaitNs(true) > 0, "lock: draw-thread waits are attributed to the draw bucket");
        held.unlock();
        holder.join();
        releaser.join();
    }

    // Hand-off between draws: a waiter blocked behind a held batch gets the
    // lock when the holder yields, runs, and the holder resumes owning at
    // the same depth; with no waiter the yield is a no-op.
    {
        deko9::DeviceLock batch;
        batch.lock();
        batch.lock(); // depth 2, like a native call inside a batch
        batch.HandOffIfContended();
        Check(batch.OwnedByCaller() && batch.Depth() == 2 && batch.HandOffs() == 0, "lock: no waiter, no hand-off");
        const HandOffRound r = RunHandOffRound(batch);
        Check(r.waiterCounted, "lock: the blocked waiter is counted");
        Check(!r.waiterEarly, "lock: the waiter is blocked behind the batch");
        Check(r.waiterRan, "lock: the waiter ran inside the hand-off");
        Check(batch.OwnedByCaller() && batch.Depth() == 2, "lock: the holder resumed at its depth");
        Check(batch.HandOffs() == r.handOffs && r.handOffs >= 1 && !batch.HasWaiters(), "lock: hand-offs counted");
        batch.unlock();
        batch.unlock();
        Check(!batch.OwnedByCaller() && batch.Depth() == 0, "lock: balanced after the hand-off");
    }

    // Release-for-blocking scope (the present's swapchain acquire and fence
    // wait): the holder gives the lock up entirely, a waiter runs meanwhile,
    // and the holder owns it again at the same depth when the scope ends.
    {
        deko9::DeviceLock held;
        held.lock();
        held.lock();
        std::atomic<bool> ran{false};
        std::thread other;
        {
            deko9::DeviceUnlockScope unlocked(held);
            Check(!held.OwnedByCaller() && held.Depth() == 0, "unlock scope: fully released");
            other = std::thread([&] {
                deko9::DeviceLockGuard g(held);
                ran.store(true);
            });
            other.join();
            Check(ran.load(), "unlock scope: another thread ran while released");
        }
        Check(held.OwnedByCaller() && held.Depth() == 2, "unlock scope: reacquired at depth 2");
        held.unlock();
        held.unlock();
        Check(!held.OwnedByCaller() && held.Depth() == 0, "unlock scope: balanced");
    }

    // Holder-site attribution: a waiter's time lands on the section the
    // holder named when the wait began; an unlock scope clears the site.
    {
        static const char kSite[] = "batch";
        deko9::DeviceLock held;
        held.lock();
        std::atomic<bool> in{false};
        std::thread w;
        {
            deko9::DeviceLockSite site(held, kSite);
            w = std::thread([&] {
                in.store(true);
                deko9::DeviceLockGuard g(held);
            });
            while (!held.HasWaiters())
                std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        held.unlock();
        w.join();
        held.lock();
        const deko9::DeviceLock::SiteWait *sw = held.SiteWaits();
        Check(sw[0].site == kSite && sw[0].count == 1 && sw[0].ns >= 4000000ull, "lock sites: wait attributed to the holder's section");
        {
            deko9::DeviceLockSite site(held, kSite);
            deko9::DeviceUnlockScope unlocked(held);
            std::thread([&] { deko9::DeviceLockGuard g(held); }).join();
        }
        Check(sw[0].count == 1, "lock sites: no wait recorded while released");
        held.unlock();
    }
}

// ---- single submitter (Device::SubmitOpenList) --------------------------------------

// The device's protocol with two real threads: `main` and a render back end.
// Every call runs under the device lock, as in deko9.
void TestSubmitOwner()
{
    deko9::DeviceLock lock;
    deko9::SubmitOwner owner;
    uint32_t reported = 0;
    // A list submit by the calling thread; true when it was allowed.
    auto submit = [&] {
        deko9::DeviceLockGuard g(lock);
        bool first = false;
        const bool ok = owner.Check(deko9::ThreadTag(), &first);
        reported += first;
        return ok;
    };
    auto claim = [&] {
        deko9::DeviceLockGuard g(lock);
        owner.Claim(deko9::ThreadTag());
    };
    // Unclaimed (device creation, the selftest): the first submitter owns it.
    Check(submit() && owner.Owner() == deko9::ThreadTag(), "submit owner: first submitter claims");
    Check(submit() && submit(), "submit owner: owner submits freely");
    // The back end takes a frame (RB_BeginFrame claims) and submits.
    bool backOk = false;
    std::thread([&] {
        claim();
        backOk = submit() && submit();
    }).join();
    Check(backOk && owner.Violations() == 0, "submit owner: back end submits after its claim");
    // Main submits while the back end owns rendering: a violation, reported
    // once, then only counted.
    Check(!submit(), "submit owner: foreign submit refused");
    Check(!submit() && owner.Violations() == 2 && reported == 1, "submit owner: reported once, counted every time");
    // R_SyncRenderThread: main claims (the back end is idle) and submits.
    claim();
    Check(submit() && owner.Violations() == 2, "submit owner: main submits after its claim");
    // Now the back end, without claiming, is the foreign thread; still one
    // report in total.
    bool backRefused = false;
    std::thread([&] { backRefused = !submit(); }).join();
    Check(backRefused && owner.Violations() == 3 && reported == 1, "submit owner: back end without a claim refused");
    Check(owner.Claims() == 3, "submit owner: claims count owner changes");
}

// ---- sampler packing ---------------------------------------------------------------

void DefaultRow(uint32_t row[deko9::kSampStateCount])
{
    std::memset(row, 0, sizeof(uint32_t) * deko9::kSampStateCount);
    row[deko9::kSampAddressU] = row[deko9::kSampAddressV] = row[deko9::kSampAddressW] = 1; // WRAP
    row[deko9::kSampMagFilter] = row[deko9::kSampMinFilter] = 1;                            // POINT
    row[deko9::kSampMipFilter] = 0;
    row[deko9::kSampMaxAnisotropy] = 1;
}

void TestCompactKey()
{
    uint32_t row[deko9::kSampStateCount];
    DefaultRow(row);
    uint32_t a, b;
    Check(deko9::CompactSamplerKey(row, false, &a), "compact: default row packs");
    Check(deko9::CompactSamplerKey(row, true, &b) && a != b, "compact: compare bit distinguishes");
    Check(a != UINT32_MAX && b < (1u << 27), "compact: 27-bit keys");
    for (uint32_t field : {deko9::kSampBorderColor, deko9::kSampMipLodBias, deko9::kSampMaxMipLevel,
                           deko9::kSampSrgbTexture, deko9::kSampElementIndex, deko9::kSampDmapOffset})
    {
        DefaultRow(row);
        row[field] = 1;
        Check(!deko9::CompactSamplerKey(row, false, &a), "compact: non-default extra field falls back");
    }
    // The engine LOD bias (-k/8, k 1..15) packs; any other bias falls back.
    {
        DefaultRow(row);
        uint32_t plain, biased, other;
        Check(deko9::CompactSamplerKey(row, false, &plain), "compact: unbiased row packs");
        const float steps[3] = {-3.0f / 8.0f, -15.0f / 8.0f, -0.3f};
        std::memcpy(&row[deko9::kSampMipLodBias], &steps[0], 4);
        Check(deko9::CompactSamplerKey(row, false, &biased) && biased != plain && biased < (1u << 31),
              "compact: -3/8 bias packs into its own key");
        std::memcpy(&row[deko9::kSampMipLodBias], &steps[1], 4);
        Check(deko9::CompactSamplerKey(row, false, &other) && other != biased, "compact: -15/8 bias packs");
        std::memcpy(&row[deko9::kSampMipLodBias], &steps[2], 4);
        Check(!deko9::CompactSamplerKey(row, false, &other), "compact: an off-step bias falls back");
        const float positive = 0.5f;
        std::memcpy(&row[deko9::kSampMipLodBias], &positive, 4);
        Check(!deko9::CompactSamplerKey(row, false, &other), "compact: a positive bias falls back");
    }
    DefaultRow(row);
    row[deko9::kSampAddressU] = 8;
    Check(!deko9::CompactSamplerKey(row, false, &a), "compact: out-of-range address falls back");
    DefaultRow(row);
    row[deko9::kSampMaxAnisotropy] = 32;
    Check(!deko9::CompactSamplerKey(row, false, &a), "compact: out-of-range anisotropy falls back");

    // Injective over every representable engine-field combination.
    std::set<uint32_t> keys;
    uint64_t combos = 0;
    for (uint32_t u = 1; u <= 5; ++u)
        for (uint32_t v = 1; v <= 5; ++v)
            for (uint32_t w : {1u, 3u})
                for (uint32_t mag = 0; mag <= 3; ++mag)
                    for (uint32_t min = 0; min <= 3; ++min)
                        for (uint32_t mip = 0; mip <= 2; ++mip)
                            for (uint32_t aniso : {1u, 2u, 4u, 16u})
                                for (bool cmp : {false, true})
                                {
                                    DefaultRow(row);
                                    row[deko9::kSampAddressU] = u;
                                    row[deko9::kSampAddressV] = v;
                                    row[deko9::kSampAddressW] = w;
                                    row[deko9::kSampMagFilter] = mag;
                                    row[deko9::kSampMinFilter] = min;
                                    row[deko9::kSampMipFilter] = mip;
                                    row[deko9::kSampMaxAnisotropy] = aniso;
                                    uint32_t key;
                                    if (!deko9::CompactSamplerKey(row, cmp, &key))
                                        Check(false, "compact: engine combination must pack");
                                    keys.insert(key);
                                    ++combos;
                                }
    Check(keys.size() == combos, "compact: injective");
}

void TestSamplerIdCache()
{
    static deko9::SamplerIdCache cache; // large: keep off the stack
    Check(cache.Find(123) == UINT32_MAX, "idcache: empty miss");
    std::mt19937 rng(7);
    std::map<uint32_t, uint32_t> truth;
    while (truth.size() < deko9::SamplerIdCache::kCapacity / 2)
    {
        const uint32_t key = rng() & ((1u << 27) - 1);
        const uint32_t id = (uint32_t)truth.size();
        if (truth.count(key))
            continue;
        Check(cache.Insert(key, id), "idcache: insert below capacity");
        truth[key] = id;
    }
    uint32_t fresh = 0;
    while (truth.count(fresh))
        ++fresh;
    Check(!cache.Insert(fresh, 99) && cache.Find(fresh) == UINT32_MAX, "idcache: refuses past half capacity");
    bool allFound = true;
    for (const auto &kv : truth)
        allFound &= cache.Find(kv.first) == kv.second;
    Check(allFound, "idcache: every inserted key found");
    int falseHits = 0;
    for (int i = 0; i < 20000; ++i)
    {
        const uint32_t key = rng() & ((1u << 27) - 1);
        if (!truth.count(key) && cache.Find(key) != UINT32_MAX)
            ++falseHits;
    }
    Check(!falseHits, "idcache: no false hits");
    cache.Clear();
    Check(cache.Find(truth.begin()->first) == UINT32_MAX && !cache.Count(), "idcache: clear");
}

// Reference: the D3D9 calls R_HW_SetSamplerState makes (r_state.cpp),
// applied to a D3D sampler row.
uint32_t ReferenceSetSamplerState(uint32_t row[deko9::kSampStateCount], uint32_t samplerState, uint32_t old)
{
    uint32_t final = samplerState;
    const uint32_t diff = old ^ samplerState;
    if (diff & 0xF00)
        row[deko9::kSampMinFilter] = (uint16_t)(samplerState & 0xF00) >> 8;
    if (diff & 0xF000)
        row[deko9::kSampMagFilter] = (uint16_t)(samplerState & 0xF000) >> 12;
    if ((uint8_t)diff)
    {
        if ((uint8_t)samplerState <= 1u)
            final = (uint8_t)old | (samplerState & 0xFFFFFF00);
        else
            row[deko9::kSampMaxAnisotropy] = (uint8_t)samplerState;
    }
    if (diff & 0xF0000)
        row[deko9::kSampMipFilter] = (samplerState & 0xF0000) >> 16;
    if (diff & 0x3F00000)
    {
        if (diff & 0x300000)
            row[deko9::kSampAddressU] = (samplerState & 0x300000) >> 20;
        if (diff & 0xC00000)
            row[deko9::kSampAddressV] = (samplerState & 0xC00000) >> 22;
        if (diff & 0x3000000)
            row[deko9::kSampAddressW] = (samplerState & 0x3000000) >> 24;
    }
    return final;
}

void TestEngineSamplerState()
{
    std::mt19937 rng(11);
    uint32_t row[deko9::kSampStateCount], ref[deko9::kSampStateCount];
    DefaultRow(row);
    DefaultRow(ref);
    uint32_t tracked = 0, trackedRef = 0;
    bool ok = true, changedOk = true;
    for (int i = 0; i < 200000; ++i)
    {
        // Engine-shaped states: anisotropy 0/1 (keep) or 2..16, filters,
        // mip, 2-bit address fields.
        const uint32_t anisoPick = rng() % 4;
        const uint32_t aniso = anisoPick == 0 ? 0 : anisoPick == 1 ? 1 : 2 + rng() % 15;
        const uint32_t packed = aniso | (rng() % 4) << 8 | (rng() % 4) << 12 | (rng() % 3) << 16 |
                                (rng() % 4) << 20 | (rng() % 4) << 22 | (rng() % 4) << 24;
        if (packed == tracked)
            continue; // the engine only calls on a difference
        uint32_t before[deko9::kSampStateCount];
        std::memcpy(before, row, sizeof(row));
        bool changed;
        tracked = deko9::ApplyEngineSamplerState(row, packed, tracked, &changed);
        trackedRef = ReferenceSetSamplerState(ref, packed, trackedRef);
        ok &= tracked == trackedRef && !std::memcmp(row, ref, sizeof(row));
        changedOk &= changed == (std::memcmp(before, row, sizeof(row)) != 0);
    }
    Check(ok, "engine sampler: matches the D3D9 call sequence");
    Check(changedOk, "engine sampler: changed flag exact");
}

// ---- live texture memo ----------------------------------------------------------------

void TestLiveMemo()
{
    // Simulates Image_IsLiveD3DTexture: ground-truth set + removal generation.
    std::set<const void *> live;
    uint32_t gen = 1;
    static LivePointerMemo memo;
    auto isLive = [&](const void *p) {
        if (memo.Find(p, gen))
            return true;
        const bool l = live.count(p) != 0;
        if (l)
            memo.Store(p, gen);
        return l;
    };
    std::mt19937 rng(5);
    std::vector<char> arena(4096);
    bool ok = true;
    for (int i = 0; i < 300000; ++i)
    {
        const void *p = &arena[rng() % arena.size()];
        switch (rng() % 4)
        {
        case 0:
            live.insert(p);
            break;
        case 1:
            if (live.erase(p))
                ++gen;
            break;
        default:
            ok &= isLive(p) == (live.count(p) != 0);
            break;
        }
    }
    Check(ok, "live memo: exact against the set");
}

// ---- constants -------------------------------------------------------------------

template <uint32_t Regs>
void TestConstantFileRegs(const char *name)
{
    using File = deko9::ConstantFile<Regs>;
    static File file;
    file.Reset();
    static float ubo[Regs][4];
    std::memset(ubo, 0x7f, sizeof(ubo)); // garbage: the first flush must cover everything
    bool invariant = true, chunked = true, tight = true, noOverlap = true, sameTrace = true;
    uint64_t pushedRegs = 0, changedRegs = 0;
    auto flush = [&] {
        std::vector<bool> dirty(Regs);
        for (uint32_t r = 0; r < Regs; ++r)
            dirty[r] = file.IsDirty(r);
        // Scalar reference preserves the previous coalescing and chunk order.
        std::vector<std::pair<uint32_t, uint32_t>> expected, actual;
        for (uint32_t r = 0; r < Regs; )
        {
            while (r < Regs && !dirty[r]) ++r;
            if (r == Regs) break;
            uint32_t end = r + 1, next = end;
            for (;;)
            {
                next = end;
                while (next < Regs && !dirty[next]) ++next;
                if (next == Regs || next - end > File::kMergeGapRegs) break;
                end = next + 1;
            }
            for (uint32_t first = r; first < end; first += File::kMaxPushRegs)
                expected.emplace_back(first, std::min(end - first, File::kMaxPushRegs));
            r = next;
        }
        std::vector<bool> covered(Regs);
        file.Flush([&](uint32_t reg, uint32_t n) {
            actual.emplace_back(reg, n);
            chunked &= n >= 1 && n <= File::kMaxPushRegs && reg + n <= Regs;
            for (uint32_t r = reg; r < reg + n; ++r)
            {
                noOverlap &= !covered[r];
                covered[r] = true;
            }
            std::memcpy(ubo[reg], file.regs[reg], n * 16);
            pushedRegs += n;
        });
        sameTrace &= actual == expected;
        // Every dirty register pushed; a clean one only inside a merge gap of
        // at most kMergeGapRegs between dirty ones.
        for (uint32_t r = 0; r < Regs; ++r)
        {
            if (dirty[r] && !covered[r])
                tight = false;
            if (covered[r] && !dirty[r])
            {
                uint32_t lo = r, hi = r;
                while (lo > 0 && !dirty[lo - 1] && covered[lo - 1])
                    --lo;
                while (hi + 1 < Regs && !dirty[hi + 1] && covered[hi + 1])
                    ++hi;
                const bool bounded = lo > 0 && dirty[lo - 1] && hi + 1 < Regs && dirty[hi + 1];
                tight &= bounded && hi - lo + 1 <= File::kMergeGapRegs;
            }
        }
        invariant &= !std::memcmp(ubo, file.regs, sizeof(ubo)) && !file.Dirty();
    };
    flush();
    std::mt19937 rng(Regs);
    for (int iter = 0; iter < 20000; ++iter)
    {
        const uint32_t writes = 1 + rng() % 4;
        for (uint32_t w = 0; w < writes; ++w)
        {
            const uint32_t start = rng() % Regs;
            const uint32_t count = 1 + rng() % std::min<uint32_t>(Regs - start, 8);
            float data[8][4];
            for (uint32_t i = 0; i < count; ++i)
            {
                // Half the time rewrite what is there (must mark nothing).
                if (rng() % 2)
                    std::memcpy(data[i], file.regs[start + i], 16);
                else
                    for (float &f : data[i])
                        f = (float)(rng() % 7);
            }
            changedRegs += file.Set(start, &data[0][0], count);
        }
        flush();
    }
    char label[96];
    std::snprintf(label, sizeof(label), "constants<%s>: UBO equals the file after every flush", name);
    Check(invariant, label);
    std::snprintf(label, sizeof(label), "constants<%s>: exact scalar push order/ranges preserved", name);
    Check(sameTrace, label);
    std::snprintf(label, sizeof(label), "constants<%s>: pushes chunked to 1 KB", name);
    Check(chunked, label);
    std::snprintf(label, sizeof(label), "constants<%s>: only dirty registers and short gaps pushed", name);
    Check(tight && noOverlap, label);
    std::snprintf(label, sizeof(label), "constants<%s>: pushed <= 2x changed", name);
    Check(pushedRegs <= 2 * changedRegs + Regs, label);
}

void TestConstantFile()
{
    TestConstantFileRegs<256>("vs");
    TestConstantFileRegs<224>("ps");
    TestConstantFileRegs<65>("partial-word");
    TestConstantFileRegs<1>("single-register");
    // The review case: c0 and c60 dirty -> two 1-register pushes, not 61.
    static deko9::ConstantFile<256> file;
    file.Flush([](uint32_t, uint32_t) {});
    const float one[4] = {1, 2, 3, 4};
    file.Set(0, one, 1);
    file.Set(60, one, 1);
    std::vector<std::pair<uint32_t, uint32_t>> pushes;
    file.Flush([&](uint32_t reg, uint32_t n) { pushes.emplace_back(reg, n); });
    Check(pushes.size() == 2 && pushes[0] == std::make_pair(0u, 1u) && pushes[1] == std::make_pair(60u, 1u),
          "constants: c0 + c60 push 2 registers");
    // A one-register gap merges (16 bytes < a 28-byte push header).
    file.Set(10, one, 1);
    file.Set(12, one, 1);
    pushes.clear();
    file.Flush([&](uint32_t reg, uint32_t n) { pushes.emplace_back(reg, n); });
    Check(pushes.size() == 1 && pushes[0] == std::make_pair(10u, 3u), "constants: gap of one register merges");
    // Rewriting identical values marks nothing.
    Check(file.Set(0, one, 1) == 0 && !file.Dirty(), "constants: identical write is not dirty");
    // A full-file write splits into 64-register pushes.
    static float big[256][4];
    for (uint32_t r = 0; r < 256; ++r)
        big[r][0] = (float)r + 100;
    file.Set(0, &big[0][0], 256);
    pushes.clear();
    file.Flush([&](uint32_t reg, uint32_t n) { pushes.emplace_back(reg, n); });
    Check(pushes.size() == 4 && pushes[3] == std::make_pair(192u, 64u), "constants: 1 KB chunks");
}

// ---- static-texture hazard-skip model vs. the full per-draw tracker -------
//
// Drives OldTracker (today: every draw hazard-checks every sampled store)
// and NewTracker (a static store is checked only when newly bound or
// pendingRaw) through the identical random op stream -- bind, draw, copy
// (UpdateTexture/UpdateSurface/CopyBufferToImage), blit (StretchRect), and a
// barrier from elsewhere (e.g. ReadImage) -- and asserts every single
// barrier decision (fired, kind) and the running barrier count agree
// (src/deko9/deko9_hazard_model.h).
void TestStaticHazardModel()
{
    using namespace deko9_hazard_model;
    constexpr uint32_t kStores = 10; // 0: default/dummy, 1-2: render targets, 3-9: static candidates
    constexpr uint32_t kSlots = 4;   // sampler slots (both stages folded together)
    constexpr uint32_t kIters = 1000000;

    std::mt19937 rng(0xDEC0u);
    OldTracker oldT(kStores);
    NewTracker newT(kStores);
    // Stores 1 and 2 are render/depth targets from the start, as if created
    // with D3DUSAGE_RENDERTARGET/DEPTHSTENCIL (CreateStore).
    oldT.stores[1].attachment = oldT.stores[2].attachment = true;
    newT.stores[1].attachment = newT.stores[2].attachment = true;

    uint32_t slotStore[kSlots];
    uint8_t slotDirty[kSlots];
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        slotStore[i] = 0; // unbound slots sample the default/dummy store
        slotDirty[i] = 1;
    }
    auto recomputeBound = [&] {
        std::fill(newT.bound.begin(), newT.bound.end(), (uint8_t)0);
        for (uint32_t i = 0; i < kSlots; ++i)
            newT.bound[slotStore[i]] = 1;
    };
    recomputeBound();

    bool mismatched = false;
    uint32_t draws = 0, copies = 0, blits = 0, forced = 0, binds = 0;
    for (uint32_t iter = 0; iter < kIters && !mismatched; ++iter)
    {
        const uint32_t op = rng() % 100;
        bool oldFired = false, newFired = false, oldEngine = false, newEngine = false;
        if (op < 35) // SetTexture on a random slot
        {
            const uint32_t slot = rng() % kSlots;
            const uint32_t store = 3 + rng() % (kStores - 3); // static candidates only
            slotStore[slot] = store;
            slotDirty[slot] = 1;
            recomputeBound();
            ++binds;
            continue;
        }
        else if (op < 75) // Draw
        {
            std::vector<uint32_t> sampled;
            std::vector<uint8_t> newlyBound;
            for (uint32_t s = 0; s < kSlots; ++s)
            {
                sampled.push_back(slotStore[s]);
                newlyBound.push_back(slotDirty[s]);
                slotDirty[s] = 0;
            }
            const std::vector<uint32_t> targets{1, 2};
            oldFired = oldT.Draw(sampled, targets, &oldEngine);
            newFired = newT.Draw(sampled, newlyBound, targets, &newEngine);
            ++draws;
        }
        else if (op < 90) // UpdateTexture/UpdateSurface/CopyBufferToImage
        {
            const uint32_t store = rng() % kStores;
            oldFired = oldT.CopyWrite(store, &oldEngine);
            newFired = newT.CopyWrite(store, &newEngine);
            ++copies;
        }
        else if (op < 95) // StretchRect
        {
            const uint32_t src = rng() % kStores;
            uint32_t dst = rng() % kStores;
            if (dst == src)
                dst = (dst + 1) % kStores;
            oldFired = oldT.BlitWrite(src, dst, &oldEngine);
            newFired = newT.BlitWrite(src, dst, &newEngine);
            ++blits;
        }
        else // a barrier from elsewhere (e.g. ReadImage's Barrier(true))
        {
            const bool copyEngine = (rng() & 1) != 0;
            oldT.ForceBarrier(copyEngine);
            newT.ForceBarrier(copyEngine);
            ++forced;
        }
        if (oldFired != newFired || (oldFired && oldEngine != newEngine) || oldT.barriers != newT.barriers)
            mismatched = true;
    }
    char label[160];
    std::snprintf(label, sizeof(label),
                  "static hazard model: %u ops (%u binds, %u draws, %u copies, %u blits, %u forced), "
                  "%u barriers (old), %u (new)",
                  kIters, binds, draws, copies, blits, forced, oldT.barriers, newT.barriers);
    Check(!mismatched, label);
}

} // namespace

int main()
{
    // Line-buffered, so a FAIL line printed before a hang reaches the log.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    // Watchdog: the lock tests drive real threads; a lost wake-up or a
    // deadlock must fail loudly, not stall ./test host (the whole binary
    // runs in well under a second; ./test also wraps it in `timeout`).
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        std::printf("FAIL:DEKO9_FASTPATH watchdog: still running after 60 s (deadlock)\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();
    TestLock();
    TestHandOffStress();
    TestSubmitOwner();
    TestCompactKey();
    TestSamplerIdCache();
    TestEngineSamplerState();
    TestLiveMemo();
    TestConstantFile();
    TestStaticHazardModel();
    TestShaderBuildStats();
    std::printf("%s:DEKO9_FASTPATH\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
