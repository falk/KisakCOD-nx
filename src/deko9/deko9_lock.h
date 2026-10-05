#pragma once

// Device lock of the optional deko3d renderer (deko9).
//
// Every D3D9 entry point and every native fast-path call takes it, because
// two threads really can reach the device at once: the render back end
// (r_smp_backend 1) records draws while the main thread creates or locks
// resources, and both record into the one open command list.
//
// It is recursive, and re-entry by the owning thread is inline (a relaxed
// load, a compare and a counter), with no atomic read-modify-write and no
// call into pthread/libnx. The engine holds it across a whole draw-surface
// list (Deko9_BeginBatch / Deko9_EndBatch around R_DrawSurfs), so the
// per-draw D3D9 and native calls inside only pay that re-entry. Before, a
// std::recursive_mutex took and released the underlying libnx mutex on
// every call, which was a measurable share of draw3d CPU time.
//
// Pure C++ (host-testable: switch_deko9_fastpath_test.cpp).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

#if defined(__SWITCH__)
#include <switch/arm/counter.h>
#include <switch/arm/tls.h>
#endif

namespace deko9
{

// A value unique to the calling thread for its lifetime.
inline uintptr_t ThreadTag()
{
#if defined(__SWITCH__)
    return (uintptr_t)armGetTls(); // per-thread TLS block (TPIDRRO_EL0)
#else
    static thread_local char tag;
    return (uintptr_t)&tag;
#endif
}

inline uint64_t LockClockNs()
{
#if defined(__SWITCH__)
    return armTicksToNs(armGetSystemTick());
#else
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}

class DeviceLock
{
public:
    // `caller` names the call site for the waiter table (CallerStats): a
    // static string. The default is the calling function's name (GCC/Clang
    // evaluate __builtin_FUNCTION() at the call site of a default argument).
    void lock(const char *caller = __builtin_FUNCTION())
    {
        const uintptr_t self = ThreadTag();
        // Only this thread ever stores `self`, so a relaxed load that sees
        // it proves ownership; any other value (0 or another thread) means
        // this thread does not own the lock.
        if (m_owner.load(std::memory_order_relaxed) == self)
        {
            ++m_depth;
            ++m_entries;
            return;
        }
        // Contention accounting (DEKO9 perf lock line): with the render
        // back end on its own thread (r_smp_backend 1) the main thread's
        // resource locks and uploads queue behind the batch lock the back
        // end holds across a draw-surface list, and the back end queues
        // behind main's uploads. The counters are written only after the
        // mutex is taken, so no other writer races them.
        bool contended = false;
        uint64_t waited = 0;
        if (!m_mutex.try_lock())
        {
            const uint64_t t0 = LockClockNs();
            // The section the holder is in as this thread starts waiting
            // (a static string set by DeviceLockSite; racy by design, it is
            // attribution, not synchronization).
            const char *holderSite = m_site.load(std::memory_order_relaxed);
            m_waiters.fetch_add(1, std::memory_order_acq_rel);
            m_mutex.lock();
            m_waiters.fetch_sub(1, std::memory_order_acq_rel);
            waited = LockClockNs() - t0;
            const bool draw = self == m_drawTag;
            ++m_contended[draw];
            m_waitNs[draw] += waited;
            AddSiteWait(holderSite, waited);
            contended = true;
        }
        m_owner.store(self, std::memory_order_release);
        m_depth = 1;
        ++m_acquisitions;
        ++m_entries;
        // Every real acquisition by a thread other than the one recording
        // draws is attributed to its call site, contended or not: in steady
        // state those threads (main, workers) should take none at all.
        if (self != m_drawTag)
        {
            ++m_otherAcquisitions;
            AddCallerWait(caller, contended, waited);
        }
    }

    // Threads blocked in lock() right now (the contended path only).
    bool HasWaiters() const { return m_waiters.load(std::memory_order_acquire) != 0; }

    // Owner-only. With the render back end holding the lock across a whole
    // draw-surface list (Deko9_BeginBatch) the main thread's buffer locks,
    // uploads and fence polls would queue behind the entire list, running
    // the two threads serialized. Called between draws, this hands the
    // lock to a waiter and takes it back at the same depth: the waiter's call runs
    // between two draws, exactly where the inline back end interleaves them.
    // Without a waiter it is one relaxed load.
    void HandOffIfContended()
    {
        if (!HasWaiters())
            return;
        const uint32_t depth = ReleaseForBlocking();
        // Give the waiter the chance to take the mutex before re-locking:
        // bounded, so a waiter that gave up (or was descheduled) cannot hold
        // the draw thread.
        for (int spin = 0; spin < 200 && HasWaiters() && m_owner.load(std::memory_order_acquire) == 0; ++spin)
            std::this_thread::yield();
        ReacquireAfterBlocking(depth, "handoff");
        ++m_handoffs;
    }
    uint64_t HandOffs() const { return m_handoffs; }

    // Owner-only: give the lock up completely for a call that blocks on the
    // GPU or the display (swapchain acquire, fence wait) and take it back at
    // the same depth afterwards. Every other thread's device calls run in
    // between, so the caller re-reads any device state it cached before.
    // Reacquisition counts as contention like any lock().
    uint32_t ReleaseForBlocking()
    {
        const uint32_t depth = m_depth;
        m_depth = 0;
        m_owner.store(0, std::memory_order_relaxed);
        m_mutex.unlock();
        return depth;
    }
    void ReacquireAfterBlocking(uint32_t depth, const char *caller = "reacquire")
    {
        lock(caller);
        m_depth = depth;
    }

    // Holder-site attribution (DEKO9 perf lock sites=): the owner names the
    // section it is in with a static string; a thread that blocks records
    // the wait against the section the holder was in when it started
    // waiting. Owner-only set; returns the previous site for restoring.
    const char *SetSite(const char *site)
    {
        const char *prev = m_site.load(std::memory_order_relaxed);
        m_site.store(site, std::memory_order_relaxed);
        return prev;
    }
    static constexpr uint32_t kSiteSlots = 12;
    struct SiteWait
    {
        const char *site;
        uint64_t count;
        uint64_t ns;
    };
    // Read while owned.
    const SiteWait *SiteWaits() const { return m_siteWaits; }

    // Waiter-site attribution (DEKO9 perf waiters): every non-recursive
    // acquisition by a thread other than the draw thread, per call site
    // (the `caller` passed to lock()), with how many blocked and for how
    // long. Sites past the table's size share the last slot ("overflow").
    static constexpr uint32_t kCallerSlots = 32;
    struct CallerStats
    {
        const char *site;
        uint64_t acquisitions;
        uint64_t contended;
        uint64_t ns;
    };
    // Read while owned.
    const CallerStats *Callers() const { return m_callers; }
    // Non-recursive acquisitions by threads other than the draw thread.
    // Read while owned.
    uint64_t OtherAcquisitions() const { return m_otherAcquisitions; }

    void unlock()
    {
        if (--m_depth)
            return;
        m_owner.store(0, std::memory_order_relaxed);
        m_mutex.unlock();
    }

    bool OwnedByCaller() const { return m_owner.load(std::memory_order_relaxed) == ThreadTag(); }
    uint32_t Depth() const { return m_depth; }
    // Non-recursive acquisitions (the underlying mutex was taken), for the
    // per-60-frame perf line. Written only while owned.
    uint64_t Acquisitions() const { return m_acquisitions; }
    // Every lock() (re-entries included): what a plain recursive mutex
    // would have paid a full acquisition for.
    uint64_t Entries() const { return m_entries; }
    // Acquisitions that found the mutex taken and blocked, and the time
    // they blocked: [1] by the draw thread (SetDrawTag), [0] by any other
    // thread (the main thread's creates/locks/uploads with r_smp_backend 1).
    // Read while owned.
    uint64_t Contended(bool drawThread) const { return m_contended[drawThread]; }
    uint64_t WaitNs(bool drawThread) const { return m_waitNs[drawThread]; }
    // The thread that records draws (Device::PrepareDraw); owner-only.
    void SetDrawTag(uintptr_t tag) { m_drawTag = tag; }

private:
    std::atomic<uintptr_t> m_owner{0};
    uint32_t m_depth = 0; // owner-only
    uint64_t m_acquisitions = 0; // owner-only
    uint64_t m_entries = 0;      // owner-only
    uint64_t m_contended[2] = {0, 0}; // owner-only (written right after taking the mutex)
    uint64_t m_waitNs[2] = {0, 0};
    uint64_t m_handoffs = 0; // owner-only
    uintptr_t m_drawTag = 0;
    std::atomic<uint32_t> m_waiters{0};
    std::atomic<const char *> m_site{nullptr};
    SiteWait m_siteWaits[kSiteSlots] = {}; // owner-only (written right after taking the mutex)
    CallerStats m_callers[kCallerSlots] = {}; // owner-only (written right after taking the mutex)
    uint64_t m_otherAcquisitions = 0;         // owner-only
    std::mutex m_mutex;

    void AddCallerWait(const char *site, bool contended, uint64_t ns)
    {
        if (!site)
            site = "unnamed";
        uint32_t i = 0;
        for (; i + 1 < kCallerSlots; ++i)
        {
            if (m_callers[i].site == site || !m_callers[i].site)
                break;
        }
        if (i + 1 == kCallerSlots && m_callers[i].site != site)
            site = "overflow";
        CallerStats &c = m_callers[i];
        c.site = site;
        ++c.acquisitions;
        c.contended += contended;
        c.ns += ns;
    }

    void AddSiteWait(const char *site, uint64_t ns)
    {
        if (!site)
            site = "none";
        for (uint32_t i = 0; i < kSiteSlots; ++i)
        {
            if (m_siteWaits[i].site == site || !m_siteWaits[i].site)
            {
                m_siteWaits[i].site = site;
                ++m_siteWaits[i].count;
                m_siteWaits[i].ns += ns;
                return;
            }
        }
    }
};

// Names the section the owning thread is in while the scope lives.
class DeviceLockSite
{
public:
    DeviceLockSite(DeviceLock &lock, const char *site) : m_lock(lock), m_prev(lock.SetSite(site)) {}
    ~DeviceLockSite() { m_lock.SetSite(m_prev); }
    DeviceLockSite(const DeviceLockSite &) = delete;
    DeviceLockSite &operator=(const DeviceLockSite &) = delete;

private:
    DeviceLock &m_lock;
    const char *m_prev;
};

// Scoped DeviceLock that names its call site for the waiter table (the
// calling function by default).
class DeviceLockGuard
{
public:
    explicit DeviceLockGuard(DeviceLock &lock, const char *caller = __builtin_FUNCTION()) : m_lock(lock)
    {
        m_lock.lock(caller);
    }
    ~DeviceLockGuard() { m_lock.unlock(); }
    DeviceLockGuard(const DeviceLockGuard &) = delete;
    DeviceLockGuard &operator=(const DeviceLockGuard &) = delete;

private:
    DeviceLock &m_lock;
};

// Single-submitter rule. SubmitOpenList (its fence-slot reuse wait), the
// present's swapchain acquire and the frames-in-flight wait block on the GPU
// with the device lock released (DeviceUnlockScope), so another thread can
// submit the open list in the meantime. Each of them re-derives what it
// read before the wait (SubmitOpenList its fence slot: ReserveListSlot),
// so such a submit keeps the fence ring intact, but it cuts the owner's
// list at an arbitrary call and is reported. The rule is the
// engine's render ownership, not "the thread that drew last": the thread
// that renders a frame (RB_BeginFrame: the back end with r_smp_backend 1,
// main inline otherwise, the render thread for loading screens) and main
// after R_SyncRenderThread (the back end idle) each claim the submitter
// role, and a list submitted by any other thread is a violation. Unclaimed
// (the selftest, device creation) the first submitter claims it.
// All members are touched under the device lock.
class SubmitOwner
{
public:
    void Claim(uintptr_t tag)
    {
        if (tag != m_owner)
            ++m_claims;
        m_owner = tag;
    }
    // True when `tag` may submit. The first violation returns false with
    // *first set (the caller reports it once, loudly); later ones are only
    // counted, so a bad path costs one line, not one per list.
    bool Check(uintptr_t tag, bool *first)
    {
        *first = false;
        if (tag == m_owner)
            return true;
        if (!m_owner)
        {
            m_owner = tag;
            ++m_claims;
            return true;
        }
        *first = !m_violations++;
        return false;
    }
    uintptr_t Owner() const { return m_owner; }
    uint64_t Violations() const { return m_violations; }
    // Owner changes (claims by a thread other than the current owner).
    uint64_t Claims() const { return m_claims; }

private:
    uintptr_t m_owner = 0;
    uint64_t m_violations = 0;
    uint64_t m_claims = 0;
};

// Scope that releases an owned DeviceLock for a blocking call and takes it
// back at the same depth when the scope ends.
class DeviceUnlockScope
{
public:
    explicit DeviceUnlockScope(DeviceLock &lock)
        : m_lock(lock), m_site(lock.SetSite(nullptr)), m_depth(lock.ReleaseForBlocking())
    {
    }
    ~DeviceUnlockScope()
    {
        m_lock.ReacquireAfterBlocking(m_depth, m_site ? m_site : "reacquire");
        m_lock.SetSite(m_site);
    }
    DeviceUnlockScope(const DeviceUnlockScope &) = delete;
    DeviceUnlockScope &operator=(const DeviceUnlockScope &) = delete;

private:
    DeviceLock &m_lock;
    const char *m_site;
    uint32_t m_depth;
};

} // namespace deko9
