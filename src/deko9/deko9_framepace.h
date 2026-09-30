// deko9 native frame pacing and GPU completion points (pure logic, no
// deko3d calls, so ./test host drives it with a fake queue:
// switch_deko9_framepace_test.cpp).
//
// FrameRing: the explicit per-frame model. Each presented frame F owns fence
// slot F % N; the device signals that slot's DkFence on the queue right after
// F's last command list (dkQueueSignalFence) and, before signalling it for
// F + N, waits until F's signal has passed. So at most N frames are queued on
// the GPU, and a frame id's state is always answerable: done, pending in its
// slot, or not presented yet. Every per-frame GPU resource is stamped with the
// sequence of the command list that last used it; each list belongs to one
// frame, and queue fences signal in submission order, so "frame F's fence
// passed" implies every such resource of F (and of earlier frames) is free
// (ownership table: "Lifetime rules").
//
// EventPoint: where a D3DISSUE_END lands. D3D9 event semantics: the query
// signals once every command recorded before the Issue has completed, not
// later. If the open list holds recorded work, the device records an in-list
// fence (dkCmdBufSignalFence) at that point; if it is empty, the point is the
// end of the newest submitted list.
#pragma once

#include <cstdint>

namespace deko9
{

// Frames the GPU may have queued before the CPU waits (frame F reuses the
// fence slot, and the frame-owned resources, of frame F - kFramesInFlight).
constexpr uint32_t kFramesInFlight = 2;

template <uint32_t N> class FrameRing
{
    static_assert(N >= 1, "at least one frame in flight");

public:
    enum class State
    {
        Done,         // its fence (or a later frame's) was observed signalled
        Pending,      // presented; its fence is in Slot(frame)
        NotPresented, // still being recorded, or a future frame
    };

    // Frames presented so far. Ids start at 1; frame Presented() + 1 is the
    // one being recorded.
    uint64_t Presented() const { return m_presented; }
    uint64_t Recording() const { return m_presented + 1; }
    uint64_t DoneThrough() const { return m_doneThrough; }
    uint64_t InFlight() const { return m_presented - m_doneThrough; }
    static uint32_t Slot(uint64_t frame) { return (uint32_t)(frame % N); }
    // The frame whose fence must have passed before `frame` may take its slot
    // (0: the slot was never used).
    static uint64_t ReuseFrame(uint64_t frame) { return frame > N ? frame - N : 0; }
    uint64_t SlotFrame(uint32_t slot) const { return m_slotFrame[slot % N]; }
    uint64_t SlotEndSeq(uint32_t slot) const { return m_slotEndSeq[slot % N]; }

    // Frame Recording() was submitted up to list `endSeq` and its fence is
    // about to be signalled in its slot. Refuses (returns false, no change)
    // while the slot's previous frame is not known done: that would reuse
    // the fence and the frame's resources before the GPU finished them.
    bool Present(uint64_t endSeq)
    {
        const uint64_t frame = m_presented + 1;
        const uint64_t prev = ReuseFrame(frame);
        if (prev > m_doneThrough)
            return false;
        m_slotFrame[Slot(frame)] = frame;
        m_slotEndSeq[Slot(frame)] = endSeq;
        m_presented = frame;
        return true;
    }

    State Query(uint64_t frame) const
    {
        if (frame <= m_doneThrough)
            return State::Done;
        if (frame > m_presented)
            return State::NotPresented;
        // Present() keeps every frame <= m_presented - N done, so a pending
        // frame is always the newest owner of its slot.
        return State::Pending;
    }

    // Frame `frame`'s fence was observed signalled: it and every earlier
    // frame are done (one queue, in-order signals). Ignores frames not
    // presented yet.
    void MarkDone(uint64_t frame)
    {
        if (frame > m_doneThrough && frame <= m_presented)
            m_doneThrough = frame;
    }

private:
    uint64_t m_presented = 0;
    uint64_t m_doneThrough = 0;
    uint64_t m_slotFrame[N] = {};
    uint64_t m_slotEndSeq[N] = {};
};

// Where a D3DISSUE_END completes: command list `seq`, and with `marker` an
// in-list fence recorded at the Issue (the list had work before it).
struct EventPoint
{
    uint64_t seq = 0;
    bool marker = false;
};

// Issue(END) with list `openSeq` being recorded: at an in-list marker when the
// list holds recorded work, else at the end of the newest submitted list
// (openSeq - 1; 0 when none was ever submitted = nothing to wait for).
inline EventPoint IssueEventPoint(uint64_t openSeq, bool listHasWork)
{
    if (listHasWork)
        return {openSeq, true};
    return {openSeq - 1, false};
}

// The point's list is still being recorded: nothing can signal until it is
// submitted (GetData(FLUSH) from the recording thread submits it).
inline bool EventNeedsSubmit(const EventPoint &point, uint64_t openSeq)
{
    return point.seq >= openSeq;
}

// Whether the point has completed. `seqDone(seq)`: list `seq` finished (its
// queue fence passed); `markerPassed()`: the in-list fence signalled (asked
// only for a submitted list that has not finished).
template <typename SeqDone, typename MarkerPassed>
inline bool EventDone(const EventPoint &point, uint64_t openSeq, SeqDone &&seqDone, MarkerPassed &&markerPassed)
{
    if (!point.seq)
        return true;
    if (EventNeedsSubmit(point, openSeq))
        return false;
    if (seqDone(point.seq))
        return true;
    return point.marker && markerPassed();
}

} // namespace deko9
