#pragma once

// Frame arena: linear per-frame memory for deko9's transient (dynamic) VB/IB
// data, replacing the Buffer::Lock DISCARD/NOOVERWRITE rename protocol.
// A chunk is owned by the frame id that consumed it and returns to the free
// list only once that frame is known done, i.e. the rule that "frame F done"
// implies every resource stamped with a list of F or earlier is free, applied
// with the frame id itself as the stamp -- coarser than a list
// sequence, since a window's span must stay live for every list the frame
// records, not just the one that allocated it.
//
// Pure logic: chunk memory comes from a caller-supplied factory
// (deko9_memory.cpp: POOL_BUFFER via Device::AllocMemory; a host test hands
// out any fake buffer -- FrameArena never dereferences cpu/gpu itself), so
// ./test host exercises the ownership/refill/retirement rules with no
// deko3d calls (switch_deko9_arena_test.cpp). The only synchronization is a
// private mutex taken on every call: allocation is rare (at most a couple
// of calls per frame per dynamic role -- one whole-ring span per wrap, not
// per draw), so there is no lock-free fast path worth giving up; the point
// of the mutex is only that it is never the D3D9 DeviceLock (deko9_lock.h),
// so a producer thread's allocation never queues behind a draw-surface
// batch (allocation is CPU-only; producers never record or submit). Only a grow (a brand-new chunk) calls the factory, which may
// take the device lock; it runs with the arena mutex released.

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace deko9
{

// The dynamic VB/IB rings are on the order of 1-2 MB each, so one span is
// usually one chunk; a request bigger than the chunk size gets a dedicated
// chunk of its own.
constexpr uint32_t kArenaChunkBytes = 1u << 20;

// Opaque real memory for one chunk. FrameArena only ever copies these
// fields around; a host test can pass any fake numbers/pointers.
struct ArenaChunkMemory
{
    uint8_t *cpu = nullptr;
    uint64_t gpu = 0;
    uint32_t size = 0;
};

// A span of arena memory, valid for the frame id it was allocated for until
// that frame is retired (RetireThrough).
struct ArenaSpan
{
    uint8_t *cpu = nullptr;
    uint64_t gpu = 0;
    uint32_t size = 0;
};

class FrameArena
{
public:
    // Creates `size` bytes of real chunk memory (already at least
    // kArenaChunkBytes or the request size, whichever is larger -- FrameArena
    // decides that, never the factory); returns false on allocation failure,
    // which Alloc() propagates with no partial state left behind.
    using ChunkFactory = std::function<bool(uint32_t size, ArenaChunkMemory *out)>;

    FrameArena() = default;
    explicit FrameArena(ChunkFactory factory, uint32_t chunkBytes = kArenaChunkBytes)
        : m_factory(std::move(factory)), m_chunkBytes(chunkBytes)
    {
    }

    // Late-bound factory (the device constructs its FrameArena member
    // before AllocMemory has anywhere to allocate from).
    void SetFactory(ChunkFactory factory) { m_factory = std::move(factory); }

    // Frame `frame` needs `bytes` (aligned to `align`, minimum 1) of arena
    // memory valid until it is retired. `thread`: a value unique to the
    // calling producer for its lifetime (deko9::ThreadTag() in production;
    // a host test can use any distinct integers per simulated producer) --
    // it only keeps two producers that call at the same time from ever
    // bumping the same chunk ("per-thread cursors never overlap"); ownership
    // is entirely by `frame`, not by which thread allocated a chunk.
    //
    // Fast path: the calling thread's last chunk is still owned by `frame`
    // and has room -- a bump, no chunk search. Slow path (a chunk refill):
    // the free list, or a brand-new chunk from the factory.
    bool Alloc(uintptr_t thread, uint64_t frame, uint32_t bytes, uint32_t align, ArenaSpan *out)
    {
        if (!bytes || !frame || !out)
            return false;
        if (!align)
            align = 1;
        std::unique_lock<std::mutex> lock(m_mutex);

        auto cur = m_current.find(thread);
        if (cur != m_current.end())
        {
            Chunk &c = m_chunks[cur->second];
            const uint32_t offset = AlignUp(c.used, align);
            if (c.owner == frame && (uint64_t)offset + bytes <= c.mem.size)
            {
                c.used = offset + bytes;
                return Fill(c, offset, bytes, out);
            }
        }

        // Refill: an already-created chunk big enough and free right now.
        for (size_t i = 0; i < m_free.size(); ++i)
        {
            const uint32_t idx = m_free[i];
            Chunk &c = m_chunks[idx];
            if (c.mem.size >= bytes)
            {
                m_free.erase(m_free.begin() + (ptrdiff_t)i);
                c.owner = frame;
                c.used = bytes;
                m_current[thread] = idx;
                return Fill(c, 0, bytes, out);
            }
        }

        // Grow: a new chunk, sized to the request when it exceeds the
        // default chunk size (a large request gets a dedicated chunk).
        // The factory runs without the arena mutex: it may take the device
        // lock, and the device lock's holder retires chunks (RetireThrough)
        // under it, so holding both here would invert that order.
        ArenaChunkMemory mem;
        const uint32_t wantSize = bytes > m_chunkBytes ? bytes : m_chunkBytes;
        lock.unlock();
        const bool made = m_factory && m_factory(wantSize, &mem) && mem.size >= bytes;
        lock.lock();
        if (!made)
            return false;
        m_chunks.push_back({mem, frame, bytes});
        const uint32_t idx = (uint32_t)m_chunks.size() - 1;
        m_current[thread] = idx;
        return Fill(m_chunks[idx], 0, bytes, out);
    }

    // Every chunk owned by `frame` or an earlier one returns to the free
    // list. Call where FrameRing::MarkDone advances (Device::NoteFrameDone):
    // property (b) says a resource stamped with a list of F or earlier is
    // free once F is done, and every chunk here is stamped with exactly one
    // frame id (never rewritten while any of that frame's lists could still
    // be open -- the caller only ever asks for the *current* recording
    // frame, and retirement only ever targets an already-presented one).
    void RetireThrough(uint64_t frame)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (uint32_t i = 0; i < m_chunks.size(); ++i)
        {
            Chunk &c = m_chunks[i];
            if (c.owner && c.owner <= frame)
            {
                c.owner = 0;
                c.used = 0;
                m_free.push_back(i);
            }
        }
    }

    uint32_t ChunkCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return (uint32_t)m_chunks.size();
    }
    uint32_t FreeChunks() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return (uint32_t)m_free.size();
    }

private:
    struct Chunk
    {
        ArenaChunkMemory mem;
        uint64_t owner = 0; // 0 = free
        uint32_t used = 0;
    };

    static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    static bool Fill(const Chunk &c, uint32_t offset, uint32_t bytes, ArenaSpan *out)
    {
        out->cpu = c.mem.cpu ? c.mem.cpu + offset : nullptr;
        out->gpu = c.mem.gpu + offset;
        out->size = bytes;
        return true;
    }

    ChunkFactory m_factory;
    uint32_t m_chunkBytes = kArenaChunkBytes;
    mutable std::mutex m_mutex;
    std::vector<Chunk> m_chunks;
    std::vector<uint32_t> m_free;
    std::unordered_map<uintptr_t, uint32_t> m_current; // thread -> current chunk index
};

} // namespace deko9
