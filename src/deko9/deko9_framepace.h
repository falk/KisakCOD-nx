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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>

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

// FramePublish: the frame ring as seen by threads that do not record (the
// main thread's GPU-sync and end-fence polls, the swap wait), with no device
// lock. The recording thread updates its FrameRing under the device lock and
// then publishes here: after a Present, the frame's fence copy and last list
// sequence in its slot (a seqlock over atomic words, so a reader never sees
// a torn fence), then the presented count; after MarkDone, the done count.
// A reader that sees a frame's fence signalled records it (NoteObserved);
// the recording thread folds that into its FrameRing at its next poll, so
// readers never mutate the ring. Rules this keeps:
//  - "done" is only ever reported once the frame's own fence (or a later
//    frame's, or every one of its lists) was observed passed;
//  - a slot holding a later frame than the one asked for means the asked
//    frame is done: FrameRing::Present refuses to reuse a slot before the
//    frame in it is done, and only a successful Present is published.
template <uint32_t N, typename Fence> class FramePublish
{
    static_assert(std::is_trivially_copyable<Fence>::value, "fences are copied word by word");
    static constexpr uint32_t kWords = (uint32_t)((sizeof(Fence) + 7) / 8);

public:
    enum class State
    {
        Done,
        Pending, // presented, not known done: *fence and *endSeq are its slot's
        NotPresented,
    };

    // Recording thread, right after FrameRing::Present(endSeq) succeeded for
    // `frame` and its fence was signalled into `fence`.
    void PublishPresent(uint64_t frame, uint64_t endSeq, const Fence &fence)
    {
        Slot &s = m_slots[frame % N];
        uint64_t words[kWords] = {};
        std::memcpy(words, &fence, sizeof(Fence));
        // Seqlock without fences: release data stores keep the odd version
        // ahead of them, acquire data loads keep the reader's second version
        // load behind them.
        const uint32_t v = s.version.load(std::memory_order_relaxed);
        s.version.store(v + 1, std::memory_order_relaxed); // odd: being written
        s.frame.store(frame, std::memory_order_release);
        s.endSeq.store(endSeq, std::memory_order_release);
        for (uint32_t i = 0; i < kWords; ++i)
            s.words[i].store(words[i], std::memory_order_release);
        s.version.store(v + 2, std::memory_order_release);
        m_presented.store(frame, std::memory_order_release);
    }
    // Recording thread, after FrameRing::MarkDone: every frame up to
    // `through` is done.
    void PublishDone(uint64_t through)
    {
        if (through > m_done.load(std::memory_order_relaxed))
            m_done.store(through, std::memory_order_release);
    }

    // Any thread.
    uint64_t Presented() const { return m_presented.load(std::memory_order_acquire); }
    uint64_t Recording() const { return Presented() + 1; }
    // Frames known done: by the recording thread's ring or by any reader's
    // observation of a fence.
    uint64_t DoneThrough() const
    {
        const uint64_t done = m_done.load(std::memory_order_acquire);
        const uint64_t seen = m_observed.load(std::memory_order_acquire);
        return done > seen ? done : seen;
    }
    // Highest frame a reader saw done that the recording thread may not
    // have folded into its ring yet.
    uint64_t Observed() const { return m_observed.load(std::memory_order_acquire); }
    // A reader saw `frame`'s fence (or every list of it) passed.
    void NoteObserved(uint64_t frame)
    {
        uint64_t cur = m_observed.load(std::memory_order_relaxed);
        while (frame > cur && !m_observed.compare_exchange_weak(cur, frame, std::memory_order_release,
                                                                std::memory_order_relaxed))
        {
        }
    }

    // Where `frame` stands; for Pending, a copy of its fence and its last
    // list sequence. Lock-free; retries only while its slot is being
    // rewritten (which needs the frame done, so the retry then answers Done).
    State Read(uint64_t frame, Fence *fence, uint64_t *endSeq) const
    {
        for (;;)
        {
            if (frame <= DoneThrough())
                return State::Done;
            if (frame > Presented())
                return State::NotPresented;
            const Slot &s = m_slots[frame % N];
            const uint32_t v1 = s.version.load(std::memory_order_acquire);
            if (v1 & 1)
            {
                std::this_thread::yield();
                continue;
            }
            const uint64_t slotFrame = s.frame.load(std::memory_order_acquire);
            const uint64_t slotEnd = s.endSeq.load(std::memory_order_acquire);
            uint64_t words[kWords];
            for (uint32_t i = 0; i < kWords; ++i)
                words[i] = s.words[i].load(std::memory_order_acquire);
            if (s.version.load(std::memory_order_relaxed) != v1)
                continue;
            if (slotFrame > frame)
                return State::Done; // the slot was reused: the frame passed
            if (slotFrame != frame)
                continue; // cannot happen once Presented() >= frame was seen
            std::memcpy(fence, words, sizeof(Fence));
            *endSeq = slotEnd;
            return State::Pending;
        }
    }

private:
    struct Slot
    {
        std::atomic<uint32_t> version{0};
        std::atomic<uint64_t> frame{0};
        std::atomic<uint64_t> endSeq{0};
        std::atomic<uint64_t> words[kWords] = {};
    };
    Slot m_slots[N];
    std::atomic<uint64_t> m_presented{0};
    std::atomic<uint64_t> m_done{0};
    std::atomic<uint64_t> m_observed{0};
};

// Whether frame `frame` is done, from any thread and without the device
// lock: the published state first, then the slot's last list against the
// published completed list sequence, then one poll of the fence copy
// (`poll(fence)`: true once signalled). A signalled poll is recorded for the
// recording thread to fold in (NoteObserved).
template <uint32_t N, typename Fence, typename Poll>
inline bool PublishedFrameDone(FramePublish<N, Fence> &pub, uint64_t completedSeq, uint64_t frame, Poll &&poll)
{
    Fence fence;
    uint64_t endSeq = 0;
    switch (pub.Read(frame, &fence, &endSeq))
    {
    case FramePublish<N, Fence>::State::Done:
        return true;
    case FramePublish<N, Fence>::State::NotPresented:
        return false;
    case FramePublish<N, Fence>::State::Pending:
        break;
    }
    if (completedSeq < endSeq && !poll(fence))
        return false;
    pub.NoteObserved(frame);
    return true;
}

// Deko9_WaitFrame from any thread, no device lock: done, or not presented
// (nothing will signal until the recording thread presents it), or a
// caller holding the device lock (never block inside a batch) answer at
// once; otherwise `sleepPoll(fence)` waits on the frame's fence copy (true:
// signalled before its timeout).
template <uint32_t N, typename Fence, typename Poll, typename SleepPoll>
inline bool PublishedFrameWait(FramePublish<N, Fence> &pub, uint64_t completedSeq, uint64_t frame,
                               bool callerHoldsLock, Poll &&poll, SleepPoll &&sleepPoll)
{
    if (PublishedFrameDone(pub, completedSeq, frame, poll))
        return true;
    if (callerHoldsLock)
        return false;
    Fence fence;
    uint64_t endSeq = 0;
    const auto state = pub.Read(frame, &fence, &endSeq);
    if (state != FramePublish<N, Fence>::State::Pending)
        return state == FramePublish<N, Fence>::State::Done;
    if (sleepPoll(fence))
    {
        pub.NoteObserved(frame);
        return true;
    }
    return PublishedFrameDone(pub, completedSeq, frame, poll);
}

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

// The fence slot the open list signals when it is submitted: slot
// openSeq % N, once the list that signalled it last (slotSeq[slot]) has
// completed. `wait(seq)` blocks until list `seq` completed and may give up
// the device lock while it blocks, so another thread can submit the open
// list meanwhile and advance openSeq: the slot is derived again after every
// wait, and the one returned is always the current open list's. A slot
// computed before such a wait names the list that was already submitted,
// and signalling it again would overwrite that list's fence and leave the
// new list's own slot unsignalled (its sequence never completes).
// `openSeq`, `completedSeq` and `slotSeq` are re-read after each wait.
// `wait` returns false when it gave up (a failed or stalled fence, already
// reported): the current open list's slot is then returned unwaited.
template <uint32_t N, typename Wait>
inline uint32_t ReserveListSlot(const uint64_t (&slotSeq)[N], const uint64_t &openSeq, const uint64_t &completedSeq,
                                Wait &&wait)
{
    for (;;)
    {
        const uint32_t slot = (uint32_t)(openSeq % N);
        const uint64_t previous = slotSeq[slot];
        if (!previous || previous <= completedSeq)
            return slot;
        if (!wait(previous))
            return (uint32_t)(openSeq % N);
    }
}

} // namespace deko9
