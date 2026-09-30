// Host test (ASan/UBSan) for deko9's native frame pacing and event-query
// completion points (src/deko9/deko9_framepace.h), against a fake in-order
// GPU queue: a flat stream of commands (draws, in-list query markers, list
// ends, frame fences) that the "GPU" retires one at a time. Run by ./test host
// (deko9_framepace_sanitizer_check).

#include "src/deko9/deko9_framepace.h"

#include <cstdio>
#include <vector>

namespace
{

int g_failures;

void Check(bool ok, const char *name)
{
    if (!ok)
    {
        std::printf("FAIL:DEKO9_FRAMEPACE %s\n", name);
        ++g_failures;
    }
}

using namespace deko9;

// One in-order queue. Commands are appended when a list is submitted (as the
// real queue only sees a list at dkQueueSubmitCommands); the GPU retires them
// in order.
struct FakeQueue
{
    enum Kind
    {
        Draw,
        Marker,
        ListEnd,
        FrameFence
    };
    struct Cmd
    {
        Kind kind;
        uint64_t id; // marker id / list seq / frame id; draws: frame id
    };
    std::vector<Cmd> stream;     // submitted, in order
    size_t gpu = 0;              // commands retired
    std::vector<Cmd> open;       // the list being recorded
    uint64_t openSeq = 1;
    bool listHasWork = false;
    uint64_t nextMarker = 1;

    void RecordDraw(uint64_t frame)
    {
        open.push_back({Draw, frame});
        listHasWork = true;
    }
    uint64_t RecordMarker()
    {
        open.push_back({Marker, nextMarker});
        listHasWork = true;
        return nextMarker++;
    }
    void Submit()
    {
        for (const Cmd &c : open)
            stream.push_back(c);
        stream.push_back({ListEnd, openSeq});
        open.clear();
        ++openSeq;
        listHasWork = false;
    }
    void SignalFrame(uint64_t frame) { stream.push_back({FrameFence, frame}); }
    bool Step()
    {
        if (gpu == stream.size())
            return false;
        ++gpu;
        return true;
    }
    bool Passed(Kind kind, uint64_t id) const
    {
        for (size_t i = 0; i < gpu; ++i)
            if (stream[i].kind == kind && stream[i].id == id)
                return true;
        return false;
    }
    bool SeqDone(uint64_t seq) const { return !seq || Passed(ListEnd, seq); }
    // Draws of `frame` not yet retired (submitted or still open).
    size_t PendingDraws(uint64_t frame) const
    {
        size_t n = 0;
        for (size_t i = gpu; i < stream.size(); ++i)
            n += stream[i].kind == Draw && stream[i].id == frame;
        for (const Cmd &c : open)
            n += c.kind == Draw && c.id == frame;
        return n;
    }
};

// The device's Query::Issue(END) + Done() against the fake queue.
struct FakeQuery
{
    EventPoint point;
    uint64_t marker = 0;
    void Issue(FakeQueue &q)
    {
        point = IssueEventPoint(q.openSeq, q.listHasWork);
        marker = point.marker ? q.RecordMarker() : 0;
    }
    bool Done(const FakeQueue &q) const
    {
        return EventDone(
            point, q.openSeq, [&](uint64_t seq) { return q.SeqDone(seq); },
            [&]() { return q.Passed(FakeQueue::Marker, marker); });
    }
};

// Issue after N draws: done exactly when those N draws have completed, not
// one command earlier and not at the end of the list.
void TestIssueAfterDraws()
{
    for (int n = 1; n <= 6; ++n)
    {
        FakeQueue q;
        for (int i = 0; i < n; ++i)
            q.RecordDraw(1);
        FakeQuery query;
        query.Issue(q);
        Check(query.point.marker && query.point.seq == 1, "issue with work records an in-list marker");
        for (int i = 0; i < 5; ++i)
            q.RecordDraw(2); // later work in the same list
        Check(EventNeedsSubmit(query.point, q.openSeq), "point in the open list needs a submit");
        Check(!query.Done(q), "never done before its list is submitted");
        q.Submit();
        Check(!EventNeedsSubmit(query.point, q.openSeq), "submitted point needs no submit");
        int retired = 0;
        bool early = false, late = false;
        do
        {
            const bool draws1Done = q.PendingDraws(1) == 0;
            const bool markerDone = q.Passed(FakeQueue::Marker, query.marker);
            const bool done = query.Done(q);
            early |= done && !draws1Done;
            // Not later: done as soon as the marker right after the draws retired,
            // while the list's later draws are still pending.
            late |= markerDone && !done;
            ++retired;
        } while (q.Step());
        Check(!early, "event query never signals before the draws issued before it");
        Check(!late, "event query signals at its point, not at the end of the list");
        // It signalled while frame 2's draws were still pending.
        FakeQueue r;
        for (int i = 0; i < n; ++i)
            r.RecordDraw(1);
        FakeQuery rq;
        rq.Issue(r);
        for (int i = 0; i < 5; ++i)
            r.RecordDraw(2);
        r.Submit();
        while (!rq.Done(r))
            if (!r.Step())
            {
                Check(false, "marker point completes");
                break;
            }
        Check(r.PendingDraws(2) == 5 && r.PendingDraws(1) == 0,
              "done after exactly the earlier draws; the later ones are still queued");
    }
}

// Issue on an empty open list (the engine's swap fence right after Present):
// the point is the newest submitted list, never the list about to hold the
// next frame. This is the serialization the fix removes: the old rule bound
// it to the open list, which only completes after the next frame's draws.
void TestIssueOnEmptyList()
{
    FakeQueue q;
    FakeQuery none;
    none.Issue(q);
    Check(!none.point.marker && none.point.seq == 0 && none.Done(q), "nothing submitted: done at once");
    for (int i = 0; i < 3; ++i)
        q.RecordDraw(1);
    q.Submit(); // Present of frame 1: list 1
    FakeQuery swap;
    swap.Issue(q);
    Check(!swap.point.marker && swap.point.seq == 1, "empty open list: the newest submitted list");
    Check(!EventNeedsSubmit(swap.point, q.openSeq), "no submit needed for an empty-list point");
    for (int i = 0; i < 4; ++i)
        q.RecordDraw(2); // the next frame is recorded after the query
    while (!swap.Done(q))
        if (!q.Step())
        {
            Check(false, "swap point completes");
            break;
        }
    Check(q.PendingDraws(1) == 0, "done only after frame 1's draws");
    Check(q.PendingDraws(2) == 4, "done without waiting for frame 2 (still being recorded)");
}

// Frame ring: slot reuse only after the old frame's fence, in-flight bound,
// states.
void TestFrameRingRules()
{
    FrameRing<2> ring;
    Check(ring.Recording() == 1 && ring.Query(0) == FrameRing<2>::State::Done, "frame 0 is done");
    Check(ring.Query(1) == FrameRing<2>::State::NotPresented, "recording frame is not presented");
    Check(ring.Present(10) && ring.Present(20), "first N frames need no wait");
    Check(ring.InFlight() == 2, "two frames in flight");
    Check(!ring.Present(30), "frame 3 refused while frame 1 has not passed");
    Check(ring.Presented() == 2, "refused present changes nothing");
    ring.MarkDone(5);
    Check(ring.DoneThrough() == 0, "a frame not presented cannot be marked done");
    ring.MarkDone(1);
    Check(ring.Query(1) == FrameRing<2>::State::Done && ring.Query(2) == FrameRing<2>::State::Pending,
          "done frame 1, pending frame 2");
    Check(ring.Present(30), "frame 3 takes frame 1's slot after its fence");
    Check(FrameRing<2>::Slot(3) == FrameRing<2>::Slot(1) && ring.SlotFrame(FrameRing<2>::Slot(3)) == 3 &&
              ring.SlotEndSeq(FrameRing<2>::Slot(3)) == 30,
          "slot now owned by frame 3");
    ring.MarkDone(3);
    Check(ring.DoneThrough() == 3 && ring.Query(2) == FrameRing<2>::State::Done, "in-order: 3 done implies 2 done");
    ring.MarkDone(2);
    Check(ring.DoneThrough() == 3, "done never moves back");
    Check(FrameRing<2>::ReuseFrame(1) == 0 && FrameRing<2>::ReuseFrame(2) == 0 && FrameRing<2>::ReuseFrame(5) == 3,
          "reuse frame = frame - N");
}

// The whole pacing loop against the fake GPU, as the device and the engine
// drive it: record a frame (resources stamped with the frame id into its
// slot), wait at present for frame F - N, signal F's fence. The GPU is slower
// than the CPU, so frames pile up to the bound. A frame's slot resource must
// never be rewritten while the GPU still has that frame's draws.
void TestPacingLoop()
{
    constexpr uint32_t N = kFramesInFlight;
    FakeQueue q;
    FrameRing<N> ring;
    uint64_t slotOwner[N] = {}; // per-frame resource (e.g. a frame's upload chunk)
    uint64_t maxInFlight = 0;
    bool overwriteInUse = false, refused = false;
    const auto frameDone = [&](uint64_t f) {
        if (ring.Query(f) == FrameRing<N>::State::Done)
            return true;
        if (ring.Query(f) == FrameRing<N>::State::NotPresented)
            return false;
        if (!q.Passed(FakeQueue::FrameFence, f))
            return false;
        ring.MarkDone(f);
        return true;
    };
    for (uint64_t frame = 1; frame <= 40; ++frame)
    {
        // Engine-side swap wait before presenting `frame` (RB_BackendTimeout):
        // frame - N done. The device's own reuse wait is the same frame.
        const uint64_t prev = FrameRing<N>::ReuseFrame(frame);
        const uint32_t slot = FrameRing<N>::Slot(frame);
        for (int i = 0; i < 4; ++i)
        {
            q.RecordDraw(frame);
            q.Step(); // CPU records 4 draws while the GPU retires ~4 commands
        }
        q.Submit();
        while (!frameDone(prev))
            if (!q.Step())
            {
                Check(false, "GPU progresses while the CPU waits");
                break;
            }
        // Presenting hands the slot (its fence and what the frame owns) to
        // this frame: legal only once the previous owner's draws retired.
        if (slotOwner[slot] && q.PendingDraws(slotOwner[slot]))
            overwriteInUse = true;
        slotOwner[slot] = frame;
        refused |= !ring.Present(q.openSeq - 1);
        q.SignalFrame(frame);
        if (ring.InFlight() > maxInFlight)
            maxInFlight = ring.InFlight();
        Check(ring.InFlight() <= N, "never more than N frames in flight");
        // The GPU is slower: it has 4 draws + list end + fence per frame but
        // only retires ~4 commands while the next frame is recorded.
    }
    Check(!refused, "every present found its slot free");
    Check(!overwriteInUse, "no frame resource reused while the GPU still reads it");
    Check(maxInFlight == N, "a GPU-bound loop keeps N frames in flight");
}

} // namespace

int main()
{
    TestIssueAfterDraws();
    TestIssueOnEmptyList();
    TestFrameRingRules();
    TestPacingLoop();
    if (g_failures)
    {
        std::printf("FAIL:DEKO9_FRAMEPACE %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS:DEKO9_FRAMEPACE\n");
    return 0;
}
