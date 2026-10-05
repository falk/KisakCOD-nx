#pragma once

// Buffer rename without the device lock.
//
// A Lock that may overwrite bytes the GPU can still read (no NOOVERWRITE or
// READONLY, and the buffer's last list has not completed) moves the buffer
// to other memory. The moved-from memory may be reused only once every list
// that could reference it completed: its stamp is the newest list open at
// the rename (and at least the buffer's last use), and it is reusable once
// the completed list sequence reaches that stamp -- the same rule as freeing
// it after the open list. Instead of returning it to the device heap (which
// needs the device lock) the buffer keeps it as a spare, so a thread that
// does not record (main filling its mesh, skinning and pre-tessellation
// buffers) renames with no device lock at all once each buffer has its few
// spares. Only a grow (no spare free yet) and an eviction past kMaxSpares
// go through the device, via the caller's callbacks.
//
// A non-recording thread may lock a buffer only while no list being
// recorded draws from it: the engine hands a buffer between the front end
// and the back end with the frame data, which orders the CPU accesses. The
// recording thread notices the new address through the device's rename
// epoch (a relaxed counter checked before each draw's input bind).
// BufferUse checks that rule at every draw stamp.
//
// Pure C++ (host-testable: switch_deko9_lockfree_test.cpp,
// switch_deko9_hazards_test.cpp).

#include <atomic>
#include <cstdint>

namespace deko9
{

// The two words of one renamable buffer that cross threads: the newest
// open list that read it (stamped by the recording thread at each draw
// that binds it, read by a thread that locks it without the device lock)
// and the thread holding it locked (0 when unlocked). A draw from a buffer
// another thread holds locked breaks the rule above (the lock may move or
// overwrite the memory the draw binds): Stamp returns false. Both sides
// use sequentially consistent accesses (stamp, then holder load; holder
// store, then stamp load), so of a lock and a draw that overlap at least
// one sees the other: either the stamp reports the draw, or the lock sees
// the stamp and renames past the list that drew.
class BufferUse
{
public:
    // Recording thread: list `seq` reads the buffer. False when another
    // thread holds it locked.
    bool Stamp(uint64_t seq, uintptr_t self)
    {
        m_lastUse.store(seq, std::memory_order_seq_cst);
        const uintptr_t holder = m_lockedBy.load(std::memory_order_seq_cst);
        return !holder || holder == self;
    }
    uint64_t LastUse() const { return m_lastUse.load(std::memory_order_seq_cst); }
    // Fresh memory after a rename: no list has read it yet.
    void ClearUse() { m_lastUse.store(0, std::memory_order_relaxed); }
    // Locking thread: false when the buffer is already locked.
    bool BeginLock(uintptr_t self)
    {
        uintptr_t expected = 0;
        return m_lockedBy.compare_exchange_strong(expected, self, std::memory_order_seq_cst);
    }
    void EndLock() { m_lockedBy.store(0, std::memory_order_release); }
    bool Locked() const { return m_lockedBy.load(std::memory_order_acquire) != 0; }

private:
    std::atomic<uint64_t> m_lastUse{0};
    std::atomic<uintptr_t> m_lockedBy{0};
};

template <typename Mem> class RenameSpares
{
public:
    static constexpr uint32_t kMaxSpares = 2;

    // Takes the oldest spare whose stamp `completed` reached.
    bool Take(uint64_t completed, Mem *out)
    {
        for (uint32_t i = 0; i < m_count; ++i)
        {
            if (m_spares[i].stamp <= completed)
            {
                *out = m_spares[i].mem;
                for (uint32_t j = i + 1; j < m_count; ++j)
                    m_spares[j - 1] = m_spares[j];
                --m_count;
                return true;
            }
        }
        return false;
    }
    // Keeps `mem` until list `stamp` completed. Past kMaxSpares the oldest
    // spare is handed back in *evicted (the caller frees it after its stamp).
    bool Retire(const Mem &mem, uint64_t stamp, Mem *evicted, uint64_t *evictedStamp)
    {
        bool evict = false;
        if (m_count == kMaxSpares)
        {
            *evicted = m_spares[0].mem;
            *evictedStamp = m_spares[0].stamp;
            for (uint32_t j = 1; j < m_count; ++j)
                m_spares[j - 1] = m_spares[j];
            --m_count;
            evict = true;
        }
        m_spares[m_count++] = {mem, stamp};
        return evict;
    }
    uint32_t Count() const { return m_count; }
    // Empties the spares (buffer release): f(mem, stamp) for each.
    template <typename F> void Drain(F &&f)
    {
        for (uint32_t i = 0; i < m_count; ++i)
            f(m_spares[i].mem, m_spares[i].stamp);
        m_count = 0;
    }

private:
    struct Spare
    {
        Mem mem;
        uint64_t stamp;
    };
    Spare m_spares[kMaxSpares] = {};
    uint32_t m_count = 0;
};

struct RenameResult
{
    bool ok = true;      // false: no memory (the lock fails)
    bool renamed = false; // *current is new memory; *previous holds the old
    bool grew = false;    // the new memory came from allocLocked
    bool evicted = false; // a spare went back through freeLocked
};

// The rename decision and bookkeeping of one Lock. `busyPossible`: the lock
// may overwrite bytes a list read (no NOOVERWRITE/READONLY). `completed` and
// `open`: the device's published completed and open list sequences.
// allocLocked(Mem *) and freeLocked(const Mem &, uint64_t stamp) run only on
// a grow or an eviction and take the device lock themselves.
template <typename Mem, typename AllocLocked, typename FreeLocked>
RenameResult RenameForLock(Mem *current, Mem *previous, uint64_t lastUse, bool busyPossible, uint64_t completed,
                           uint64_t open, RenameSpares<Mem> &spares, AllocLocked &&allocLocked,
                           FreeLocked &&freeLocked)
{
    RenameResult r;
    if (!busyPossible || lastUse <= completed)
        return r;
    Mem fresh{};
    if (!spares.Take(completed, &fresh))
    {
        if (!allocLocked(&fresh))
        {
            r.ok = false;
            return r;
        }
        r.grew = true;
    }
    *previous = *current;
    *current = fresh;
    Mem evicted{};
    uint64_t evictedStamp = 0;
    if (spares.Retire(*previous, lastUse > open ? lastUse : open, &evicted, &evictedStamp))
    {
        freeLocked(evicted, evictedStamp);
        r.evicted = true;
    }
    r.renamed = true;
    return r;
}

} // namespace deko9
