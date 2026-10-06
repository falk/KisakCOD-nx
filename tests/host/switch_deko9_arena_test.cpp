// Host test (ASan/UBSan) for deko9's frame arena (src/deko9/deko9_arena.h):
// linear per-frame memory for the back-end dynamic VB/IB, replacing the
// Buffer::Lock DISCARD/NOOVERWRITE rename protocol. Pure header, no
// deko3d calls: a fake chunk factory hands out real host heap memory (so
// ASan catches any out-of-bounds write) with made-up GPU addresses. Run by
// ./test host (deko9_arena_sanitizer_check).

#include "src/deko9/deko9_arena.h"

#include <cstdio>
#include <memory>
#include <set>
#include <vector>

namespace
{

int g_failures;

void Check(bool ok, const char *name)
{
    if (!ok)
    {
        std::printf("FAIL:DEKO9_ARENA %s\n", name);
        ++g_failures;
    }
}

using namespace deko9;

// Backs every chunk the arena creates with real (ASan-visible) host memory;
// `gpu` is just a unique counter -- FrameArena never dereferences it, only
// arithmetic-offsets it, so a fake value is fine.
struct FakeHeap
{
    std::vector<std::unique_ptr<uint8_t[]>> blocks;
    uint64_t nextGpu = 0x1000;
    uint32_t factoryCalls = 0;

    bool Make(uint32_t size, ArenaChunkMemory *out)
    {
        ++factoryCalls;
        blocks.push_back(std::make_unique<uint8_t[]>(size));
        out->cpu = blocks.back().get();
        out->gpu = nextGpu;
        out->size = size;
        nextGpu += size + 0x10000; // generous gap so ranges can't accidentally touch
        return true;
    }
};

// One arena chunk of memory never overlaps another's [gpu, gpu+size) range,
// which is what "no chunk reused before its frame is done" and "per-thread
// cursors never overlap" both reduce to at the byte level.
bool Overlaps(uint64_t a0, uint32_t aSize, uint64_t b0, uint32_t bSize)
{
    const uint64_t a1 = a0 + aSize, b1 = b0 + bSize;
    return a0 < b1 && b0 < a1;
}

void TestBasicAllocAndRetire()
{
    FakeHeap heap;
    FrameArena arena([&](uint32_t size, ArenaChunkMemory *out) { return heap.Make(size, out); }, 1u << 16);

    ArenaSpan s1{};
    Check(arena.Alloc(1 /*thread*/, 1 /*frame*/, 4096, 256, &s1), "frame 1 alloc succeeds");
    Check(s1.cpu != nullptr && s1.size == 4096, "frame 1 span is usable");
    Check(heap.factoryCalls == 1, "first alloc grows one chunk");
    Check(arena.ChunkCount() == 1 && arena.FreeChunks() == 0, "one chunk, owned (not free)");

    // A second alloc for the SAME thread+frame with room left bumps the
    // same chunk (no new factory call).
    ArenaSpan s2{};
    Check(arena.Alloc(1, 1, 256, 256, &s2), "frame 1 second alloc succeeds");
    Check(heap.factoryCalls == 1, "bump path takes no new chunk");
    Check(!Overlaps(s1.gpu, s1.size, s2.gpu, s2.size), "bumped spans do not overlap");
    Check(s2.gpu >= s1.gpu + s1.size, "bump advances forward, 256-aligned");
    Check((s2.gpu & 0xFF) == 0, "alignment >= 256 honored");

    // Frame 1 isn't done yet: a new frame must NOT reuse its chunk.
    ArenaSpan s3{};
    Check(arena.Alloc(1, 2, 4096, 256, &s3), "frame 2 alloc succeeds");
    Check(heap.factoryCalls == 2, "a different owner forces a new/free chunk, not a bump");
    Check(!Overlaps(s1.gpu, s1.size, s3.gpu, s3.size), "frame 1 and frame 2 chunks never overlap");

    // Retiring frame 1 frees its chunk; a later frame may now reuse it.
    arena.RetireThrough(1);
    Check(arena.FreeChunks() == 1, "retiring frame 1 frees exactly its chunk");
    ArenaSpan s4{};
    Check(arena.Alloc(1, 3, 4096, 256, &s4), "frame 3 alloc succeeds");
    Check(heap.factoryCalls == 2, "frame 3 reused the freed chunk instead of growing");
    Check(s4.gpu == s1.gpu, "frame 3 landed exactly on frame 1's retired chunk");
}

// "chunk reuse only after done under a fake ring with 2 frames queued and a
// front end one ahead": frames 1..N are allocated (front end, one ahead) and
// retired (back end) two behind the newest, the way FrameRing keeps at most
// kFramesInFlight queued; no two frames simultaneously "in flight" ever
// share a chunk's memory.
void TestFrameRingLikeSequence()
{
    constexpr uint32_t N = 2; // kFramesInFlight
    FakeHeap heap;
    FrameArena arena([&](uint32_t size, ArenaChunkMemory *out) { return heap.Make(size, out); }, 1u << 16);

    struct Live
    {
        uint64_t frame;
        ArenaSpan span;
    };
    std::vector<Live> live;

    for (uint64_t frame = 1; frame <= 40; ++frame)
    {
        // The front end is one frame ahead of the back end's retirement:
        // frame F is only retired once F + N has been allocated (mirrors
        // FrameRing::ReuseFrame: frame reuses the slot/resources of
        // frame - N).
        ArenaSpan span{};
        if (!arena.Alloc(1, frame, 8192, 256, &span))
        {
            Check(false, "ring-sequence alloc succeeds");
            return;
        }
        for (const Live &other : live)
        {
            if (Overlaps(span.gpu, span.size, other.span.gpu, other.span.size))
            {
                Check(false, "no two live (not-yet-done) frames share arena memory");
                return;
            }
        }
        live.push_back({frame, span});
        if (frame > N)
        {
            const uint64_t doneFrame = frame - N;
            arena.RetireThrough(doneFrame);
            for (auto it = live.begin(); it != live.end();)
                it = it->frame <= doneFrame ? live.erase(it) : std::next(it);
        }
    }
    Check(live.size() == N, "exactly N frames stay live at the end");
    // With chunks always reused after retirement, the working set should be
    // small (N + a couple, per 2.1's "N + 2 slots" sizing argument) rather
    // than growing with the number of frames processed.
    Check(heap.factoryCalls <= N + 2, "chunk churn is bounded, not one chunk per frame");
}

// "per-thread cursors never overlap": two producers (e.g. a future front-end
// role and the back end) allocating for the SAME frame concurrently must
// never be handed overlapping memory, since each only tracks its own
// current chunk.
void TestPerThreadCursorsNeverOverlap()
{
    FakeHeap heap;
    FrameArena arena([&](uint32_t size, ArenaChunkMemory *out) { return heap.Make(size, out); }, 1u << 16);

    ArenaSpan a{}, b{};
    Check(arena.Alloc(/*thread*/ 0xAAA, /*frame*/ 7, 1024, 256, &a), "thread A alloc succeeds");
    Check(arena.Alloc(/*thread*/ 0xBBB, /*frame*/ 7, 1024, 256, &b), "thread B alloc succeeds");
    Check(!Overlaps(a.gpu, a.size, b.gpu, b.size), "two threads never share a chunk for the same frame");

    // Each thread keeps bumping its own chunk independently.
    ArenaSpan a2{}, b2{};
    Check(arena.Alloc(0xAAA, 7, 512, 256, &a2), "thread A second alloc succeeds");
    Check(arena.Alloc(0xBBB, 7, 512, 256, &b2), "thread B second alloc succeeds");
    Check(a2.gpu >= a.gpu + a.size, "thread A's cursor only advances within its own chunk");
    Check(b2.gpu >= b.gpu + b.size, "thread B's cursor only advances within its own chunk");
    Check(!Overlaps(a2.gpu, a2.size, b2.gpu, b2.size), "threads' second allocs still never overlap");
}

// "large requests": a request bigger than the chunk size gets a dedicated
// chunk of its own, not a truncated/failed allocation.
void TestLargeRequest()
{
    constexpr uint32_t kChunk = 1u << 16; // 64 KiB, deliberately small
    FakeHeap heap;
    FrameArena arena([&](uint32_t size, ArenaChunkMemory *out) { return heap.Make(size, out); }, kChunk);

    ArenaSpan small{};
    Check(arena.Alloc(1, 1, 4096, 256, &small), "small alloc succeeds");

    ArenaSpan big{};
    const uint32_t bigBytes = kChunk * 3 + 17; // well over one chunk
    Check(arena.Alloc(1, 1, bigBytes, 256, &big), "large alloc succeeds");
    Check(big.size == bigBytes, "large span is sized exactly to the request");
    Check(!Overlaps(small.gpu, small.size, big.gpu, big.size), "large request does not alias the small chunk");

    // A dedicated chunk returns to the free list like any other once its
    // frame retires, and a later request that fits reuses it.
    arena.RetireThrough(1);
    const uint32_t before = heap.factoryCalls;
    ArenaSpan reuse{};
    Check(arena.Alloc(1, 2, bigBytes, 256, &reuse), "reuse alloc succeeds");
    Check(heap.factoryCalls == before, "the dedicated chunk was reused, not regrown");
    Check(reuse.gpu == big.gpu, "reuse landed on the retired dedicated chunk");
}

// Alignment: every span must start on an `align`-aligned GPU address (the
// proof requires alignment >= 256, matching every other CPU-written GPU
// allocation in deko9 -- deko9_memory.cpp AllocMemory/Heap::Alloc).
void TestAlignment()
{
    FakeHeap heap;
    FrameArena arena([&](uint32_t size, ArenaChunkMemory *out) { return heap.Make(size, out); }, 1u << 20);

    uint64_t base = 0;
    for (uint32_t i = 0; i < 8; ++i)
    {
        ArenaSpan s{};
        // Odd sizes force misaligned bump offsets unless AlignUp is applied.
        Check(arena.Alloc(1, 1, 17 + i * 3, 256, &s), "odd-size alloc succeeds");
        Check((s.gpu & 0xFF) == 0, "every span is 256-aligned");
        if (i == 0)
            base = s.gpu;
        Check(s.gpu >= base, "spans still advance monotonically");
        base = s.gpu;
    }
}

// A failing factory (simulated OOM) must fail Alloc() cleanly, with no
// chunk left half-registered.
void TestFactoryFailure()
{
    FrameArena arena([](uint32_t, ArenaChunkMemory *) { return false; }, 4096);
    ArenaSpan s{};
    Check(!arena.Alloc(1, 1, 128, 256, &s), "a failing factory fails Alloc, not crashes");
    Check(arena.ChunkCount() == 0, "no chunk was left behind by the failed grow");
}

} // namespace

int main()
{
    TestBasicAllocAndRetire();
    TestFrameRingLikeSequence();
    TestPerThreadCursorsNeverOverlap();
    TestLargeRequest();
    TestAlignment();
    TestFactoryFailure();
    if (g_failures)
    {
        std::printf("FAIL:DEKO9_ARENA %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS:DEKO9_ARENA\n");
    return 0;
}
