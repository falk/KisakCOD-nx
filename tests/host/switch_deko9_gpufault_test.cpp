// Host test (ASan/UBSan) for the pure parts of the deko9 GPU-fault black box
// (src/deko9/deko9_gpufault.h): memory event ring, per-draw record ring,
// breadcrumb encoding and order, stall detector and verdict, and the TIC
// address decode. Run by
// ./test host (deko9_gpufault_sanitizer_check).

#include "src/deko9/deko9_gpufault.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{

int g_failures;

void Check(bool ok, const char *name)
{
    if (!ok)
    {
        std::printf("FAIL:DEKO9_GPUFAULT %s\n", name);
        ++g_failures;
    }
}

using namespace deko9;

void TestEventRing()
{
    GpuEventRing<8> ring;
    GpuEvent out[16];
    Check(ring.Newest(out, 16) == 0, "empty ring returns nothing");
    for (uint32_t i = 0; i < 5; ++i)
        ring.Push({0x500000000ull + i * 0x100, i, 0x100, kGpuEventAlloc, 0, 0});
    uint32_t n = ring.Newest(out, 16);
    Check(n == 5 && out[0].seq == 0 && out[4].seq == 4, "partial ring oldest first");
    n = ring.Newest(out, 2);
    Check(n == 2 && out[0].seq == 3 && out[1].seq == 4, "newest subset");
    for (uint32_t i = 5; i < 21; ++i)
        ring.Push({0, i, 0, kGpuEventFreeDeferred, 0, 0});
    n = ring.Newest(out, 16);
    Check(n == 8 && out[0].seq == 13 && out[7].seq == 20, "wrapped ring keeps the newest N in order");
    Check(ring.Pushed() == 21, "push count");
    Check(std::string(GpuEventName(kGpuEventFreeRetired)) == "free_retired" &&
              std::string(GpuEventName(200)) == "?",
          "event names");
}

void TestDrawRecords()
{
    auto ring = std::make_unique<DrawRecordRing<4, 3>>();
    ring->BeginList(10);
    for (uint32_t i = 0; i < 5; ++i)
    {
        DrawRecord r{};
        r.draw = i;
        r.psHash = 0xabc0 + i;
        ring->Add(10, r);
    }
    Check(ring->Count(10) == 5, "count includes draws past capacity");
    Check(ring->Get(10, 2) && ring->Get(10, 2)->psHash == 0xabc2, "kept record");
    Check(!ring->Get(10, 3), "draw past capacity not kept");
    Check(!ring->Get(11, 0) && ring->Count(11) == 0, "other seq in no slot");
    // seq 14 reuses seq 10's slot: the old records are gone, not misattributed.
    ring->BeginList(14);
    Check(ring->Count(10) == 0 && !ring->Get(10, 0), "reused slot forgets the old list");
    DrawRecord r{};
    r.draw = 0;
    ring->Add(10, r); // a stale writer for the old seq must not land in the new list
    Check(ring->Count(14) == 0, "add for a stale seq ignored");
    Check(!ring->Last(14), "no newest record in an empty list");
    r.draw = 0;
    ring->Add(14, r);
    r.draw = 1;
    ring->Add(14, r);
    DrawRecord *last = ring->Last(14);
    Check(last && last->draw == 1, "newest record");
    last->count = 0x7fffffff;
    Check(ring->Get(14, 1)->count == 0x7fffffff, "completing the newest record");
    for (uint32_t i = 2; i < 5; ++i)
        ring->Add(14, r);
    Check(!ring->Last(14), "no newest record once past capacity (it was not kept)");
    Check(std::string(NativeDrawName(kNativeHrpDepth)) == "hrp_depth" &&
              std::string(NativeDrawName(kNativeNone)) == "-" && std::string(NativeDrawName(200)) == "?",
          "native draw names");
}

void TestCrumbs()
{
    Check(CrumbValue(0x12345, 7) == 0x23450007u, "crumb packs low 16 bits of seq");
    Check(CrumbDraw(CrumbValue(3, 70000)) == kCrumbListBegin - 1, "draw index saturates below the markers");
    Check(CrumbDraw(CrumbEnd(3)) == kCrumbListEnd, "end marker");
    Check(CrumbDraw(CrumbBegin(3)) == kCrumbListBegin, "begin marker");
    // Stream order: begin < draws < end, then the next list.
    Check(CrumbOrder(CrumbBegin(7), 9) < CrumbOrder(CrumbValue(7, 0), 9) &&
              CrumbOrder(CrumbValue(7, 0), 9) < CrumbOrder(CrumbValue(7, 1273), 9) &&
              CrumbOrder(CrumbValue(7, 1273), 9) < CrumbOrder(CrumbEnd(7), 9) &&
              CrumbOrder(CrumbEnd(7), 9) < CrumbOrder(CrumbBegin(8), 9),
          "crumb stream order");
    Check(CrumbOrder(CrumbEnd(0xffff), 0x10001) < CrumbOrder(CrumbBegin(0x10000), 0x10001),
          "crumb order across a 16-bit wrap");
    Check(CrumbSeq(CrumbValue(0x12345, 1), 0x12347) == 0x12345, "seq recovered near newest");
    Check(CrumbSeq(CrumbValue(0x1fffe, 1), 0x20001) == 0x1fffe, "seq recovered across a 16-bit wrap");
    Check(CrumbSeq(CrumbValue(0x20001, 1), 0x20001) == 0x20001, "newest itself");
}

void TestStallDetector()
{
    StallThresholds t;
    t.midListNs = 20;
    t.betweenListsNs = 250;
    StallDetector d(t);
    // Nothing submitted: never a stall.
    Check(!d.Sample(0, 0, false, 0) && !d.Sample(0, 0, false, 1000), "idle before any submit");
    // GPU mid-list in seq 5, draw 3, with seq 5 the newest submitted.
    const uint32_t mid = CrumbValue(5, 3);
    Check(!d.Sample(mid, 5, true, 1000), "first sample arms");
    Check(!d.Sample(mid, 5, true, 1019), "below the mid-list threshold");
    Check(d.Sample(mid, 5, true, 1020), "stall reported at the threshold");
    Check(!d.Sample(mid, 5, true, 5000), "one report per episode");
    Check(d.StalledNs(5000) == 4000, "stall age");
    // Progress re-arms.
    Check(!d.Sample(CrumbValue(5, 4), 5, true, 5001), "progress re-arms");
    Check(d.Sample(CrumbValue(5, 4), 5, true, 5021), "second episode reported");
    // Between lists (end of seq 5, seq 6 submitted): long threshold (acquire / present waits).
    Check(!d.Sample(CrumbEnd(5), 6, true, 6000), "list end arms");
    Check(!d.Sample(CrumbEnd(5), 6, true, 6200), "vsync-length wait between lists is not a stall");
    Check(d.Sample(CrumbEnd(5), 6, true, 6250), "long gap between lists is");
    // GPU finished the newest list: idle, however long.
    StallDetector idle(t);
    Check(!idle.Sample(CrumbEnd(9), 9, true, 0) && !idle.Sample(CrumbEnd(9), 9, true, 1000000), "idle GPU");
    // A new submit after a long idle starts the clock at the submit, not at the idle start.
    Check(!idle.Sample(CrumbEnd(9), 10, true, 1000010), "fresh work after idle");
    Check(!idle.Sample(CrumbEnd(9), 10, true, 1000200), "clock started at the idle end");
}

// The watcher tick (Device::WatchMain): a reported stall whose dump fails
// (throttled) calls RetryReport, so the next tick reports it again until a
// dump succeeds; after that the episode stays reported.
static int StallTicks(StallDetector &d, uint32_t crumb, uint64_t from, uint64_t to, int failDumps, bool retry,
                      int *dumpsOk)
{
    int reports = 0;
    for (uint64_t now = from; now <= to; now += 2)
    {
        if (!d.Sample(crumb, 5, true, now))
            continue;
        ++reports;
        const bool dumped = failDumps-- <= 0;
        *dumpsOk += dumped;
        if (!dumped && retry)
            d.RetryReport();
    }
    return reports;
}

void TestStallRetry()
{
    StallThresholds t;
    t.midListNs = 20;
    t.betweenListsNs = 250;
    const uint32_t mid = CrumbValue(5, 3);
    // Three throttled dumps, then one succeeds: 4 reports, 1 dump, then silence.
    StallDetector d(t);
    int ok = 0;
    Check(!d.Sample(mid, 5, true, 1000), "retry: arms");
    Check(StallTicks(d, mid, 1020, 1026, 3, true, &ok) == 4 && ok == 1, "retry: reported each tick until the dump succeeds");
    Check(StallTicks(d, mid, 1028, 3000, 0, true, &ok) == 0 && ok == 1, "retry: no repeat after a successful dump");
    // Direct contract: Sample, RetryReport, Sample fires again.
    StallDetector direct(t);
    Check(!direct.Sample(mid, 5, true, 0) && direct.Sample(mid, 5, true, 20), "retry: first report");
    direct.RetryReport();
    Check(direct.Sample(mid, 5, true, 22), "retry: reported again after RetryReport");
    Check(!direct.Sample(mid, 5, true, 24), "retry: once without RetryReport");
    // Negative control: without RetryReport a throttled first dump drops the stall.
    StallDetector old(t);
    ok = 0;
    Check(!old.Sample(mid, 5, true, 1000), "no-retry: arms");
    Check(StallTicks(old, mid, 1020, 3000, 1, false, &ok) == 1 && ok == 0, "no-retry (old behavior): stall never dumped");
}

void TestStallVerdict()
{
    // The OLED hang shapes: the CROP cell stopped at the end of a list with the
    // next lists submitted.
    const uint32_t cropEnd = CrumbEnd(9681);
    Check(std::string(StallVerdict(CrumbEnd(9681), true, cropEnd, 9684)) == "front_end",
          "front end never reached the next list: a host-level wait (acquire) or an unscheduled channel");
    Check(std::string(StallVerdict(CrumbBegin(9682), true, cropEnd, 9684)) == "engine",
          "next list fetched, its preamble never drained");
    // Mid-list: CROP stopped before draw 1273.
    const uint32_t cropMid = CrumbValue(5237, 1273);
    Check(std::string(StallVerdict(CrumbValue(5237, 1290), true, cropMid, 5240)) == "engine",
          "draws past the stuck one fetched: the draw at the CROP position never completes");
    Check(std::string(StallVerdict(CrumbValue(5237, 1273), true, cropMid, 5240)) == "front_end",
          "front end stopped at the same draw");
    Check(std::string(StallVerdict(CrumbValue(5237, 1200), true, cropMid, 5240)) == "unknown",
          "top behind CROP is a torn read");
    Check(std::string(StallVerdict(0, false, cropMid, 5240)) == "unknown", "no top cell");
}

void TestTic()
{
    uint32_t words[8] = {0x11111111u, 0x23916000u, 0xabcd0005u, 0, 0, 0, 0, 0};
    Check(TicAddress(words) == 0x523916000ull, "TIC address ignores the fields above address_high");
    words[2] = 0x0000ffa6u;
    Check(TicAddress(words) == 0xffa623916000ull, "16-bit address_high");
    const uint32_t handle = 1234u | (56u << 20);
    Check(HandleImage(handle) == 1234 && HandleSampler(handle) == 56, "texture handle split");
}

uint32_t Hdr(uint32_t op, uint32_t subc, uint32_t methodBytes, uint32_t arg)
{
    return op << 29 | (arg & 0x1fff) << 16 | subc << 13 | (methodBytes / 4);
}

// The stuck draw's command window: located by its top crumb, decoded up to
// the next one; a word no deko3d header can be stops the decode as BAD.
void TestCommandWindow()
{
    const uint64_t cell = 0x5001b0310ull;
    const uint32_t top1138 = CrumbValue(51669, 1138), top1139 = CrumbValue(51669, 1139);
    std::vector<uint32_t> w = {Hdr(1, 0, 0x1b00, 3), 0x5, 0x1b0300, 0x1000,  // some other release
                               Hdr(1, 3, 0x10, 4), 0x5, 0x1b0310, top1138, 0x1000005,
                               Hdr(4, 0, 0x1288, 0),                          // tiled cache flush
                               Hdr(1, 0, 0x1680, 2), 0x30, 0x48,              // draw arguments
                               Hdr(3, 0, 0x2380, 3), 0xa, 0xb, 0xc,           // constants, non-incrementing
                               Hdr(5, 0, 0x1500, 1), 0x7,
                               Hdr(1, 3, 0x10, 4), 0x5, 0x1b0310, top1139, 0x1000005};
    const int64_t at = FindTopCrumb(w.data(), (uint32_t)w.size(), cell, top1138);
    const int64_t next = FindTopCrumb(w.data(), (uint32_t)w.size(), cell, top1139);
    Check(at == 4 && next == 19, "top crumbs found at their headers");
    Check(FindTopCrumb(w.data(), (uint32_t)w.size(), cell, CrumbValue(51669, 1140)) == -1, "absent crumb");
    Check(FindTopCrumb(w.data(), (uint32_t)w.size(), cell + 16, top1138) == -1, "other cell address");
    char text[512];
    bool bad = true;
    const uint32_t used =
        FormatPushbuffer(w.data() + at, (uint32_t)(next - at), 64, text, sizeof(text), &bad);
    Check(!bad && used == (uint32_t)(next - at), "clean window decodes to its end");
    Check(std::string(text) ==
              "s3:10+4[5,1b0310..] s0:1288#0 s0:1680+2[30,48] s0:2380=3[a,b..] s0:1500~1[7]",
          "method tokens");
    w[13] = 0xdeadbeef; // garbage where the constants' header was
    FormatPushbuffer(w.data() + at, (uint32_t)(next - at), 64, text, sizeof(text), &bad);
    Check(bad && std::string(text).find("BAD:deadbeef") != std::string::npos, "garbage header flagged");
    // A count running past the window is clipped, never read beyond it.
    const uint32_t tail[2] = {Hdr(1, 0, 0x100, 200), 0x1};
    Check(FormatPushbuffer(tail, 2, 8, text, sizeof(text), &bad) == 2 && !bad, "clipped count");
    Check(FormatPushbuffer(tail, 2, 8, text, 8, &bad) == 2, "small output buffer");
}

// deko3d control stream -> GPFIFO segments: entry lists, fence commands,
// Jump and Call/Return are followed; an unknown command stops the walk.
void TestListSegments()
{
    std::vector<uint64_t> a(64, 0), b(16, 0), sub(8, 0);
    const auto header = [](uint32_t type, uint32_t arg) { return (uint64_t)type | (uint64_t)arg << 32; };
    size_t i = 0;
    a[i++] = header(kCtrlGpfifoList, 2);
    a[i++] = 0x50dc00000ull, a[i++] = 65530ull | 1ull << 32;
    a[i++] = 0x50dc40000ull, a[i++] = 1000ull | 1ull << 32;
    a[i++] = header(kCtrlSignalFence, 1), a[i++] = 0x1234;
    a[i++] = header(kCtrlJump, 0), a[i++] = (uint64_t)(uintptr_t)b.data();
    i = 0;
    b[i++] = header(kCtrlCall, 0), b[i++] = (uint64_t)(uintptr_t)sub.data();
    b[i++] = header(kCtrlGpfifoList, 1), b[i++] = 0x50dc80000ull, b[i++] = 7;
    b[i++] = header(kCtrlReturn, 0);
    sub[0] = header(kCtrlGpfifoList, 1), sub[1] = 0x5001b0000ull, sub[2] = 12;
    sub[3] = header(kCtrlReturn, 0);
    CmdSegment segs[8];
    const int32_t n = ListSegments(a.data(), segs, 8);
    Check(n == 4 && segs[0].gpu == 0x50dc00000ull && segs[0].words == 65530 && segs[1].gpu == 0x50dc40000ull &&
              segs[1].words == 1000 && segs[2].gpu == 0x5001b0000ull && segs[2].words == 12 &&
              segs[3].gpu == 0x50dc80000ull && segs[3].words == 7,
          "segments in fetch order through jump and call");
    Check(ListSegments(a.data(), segs, 1) == 4 && segs[0].words == 65530, "count beyond max");
    sub[3] = header(9, 0); // a compute command
    Check(ListSegments(a.data(), segs, 8) == -1, "unknown control command");
}

// The 2026-10-02 hang: the stuck draw's top crumb sat 57 words before its
// chunk switch. Decoding the chunk to its end read 6 never-written words
// (BAD:00000000); following the list's segments decodes the real continuation.
void TestWindowAcrossSegments()
{
    const uint64_t cell = 0x5001b0310ull;
    const uint32_t top2232 = CrumbValue(18569, 2232), top2233 = CrumbValue(18569, 2233);
    const uint32_t draw[7] = {Hdr(5, 0, 0x3808, 6), 4, 6, 1, 402081, 0, 0};
    std::vector<uint32_t> chunkA = {Hdr(4, 0, 0x1a2c, 0), Hdr(1, 6, 0x10, 4), 0x5, 0x1b0310, top2232, 0x1000005,
                                    Hdr(4, 0, 0xf80, 0)};
    for (int k = 0; k < 3; ++k)
        chunkA.insert(chunkA.end(), draw, draw + 7);
    const uint32_t used = (uint32_t)chunkA.size();
    chunkA.resize(used + 6, 0); // the chunk's tail deko3d skipped
    const std::vector<uint32_t> chunkB = {Hdr(4, 0, 0x3818, 0x1a4), Hdr(3, 0, 0x2390, 1), 0x1800209,
                                          Hdr(1, 0, 0x1b00, 4), 0x5, 0x1b0300, top2233, 0x1000005,
                                          Hdr(1, 6, 0x10, 4), 0x5, 0x1b0310, top2233, 0x1000005};
    char text[1024];
    bool bad = false;
    FormatPushbuffer(chunkA.data() + 1, (uint32_t)chunkA.size() - 1, 64, text, sizeof(text), &bad);
    Check(bad && std::string(text).find("BAD:00000000") != std::string::npos, "chunk tail reads as BAD");
    const SegmentWords segs[2] = {{chunkA.data(), 0x50dc2ff00ull, used}, {chunkB.data(), 0x50dc40000ull, 13}};
    SegmentPos at{}, to{};
    Check(FindCrumbAcross(segs, 2, cell, top2232, {0, 0}, &at) && at.seg == 0 && at.word == 1, "from crumb");
    Check(FindCrumbAcross(segs, 2, cell, top2233, {at.seg, at.word + 1}, &to) && to.seg == 1 && to.word == 8,
          "closing crumb in the next segment");
    Check(!FindCrumbAcross(segs, 2, cell, top2232, {0, 2}, &to), "search starts after from");
    const uint32_t switches = FormatAcross(segs, 2, at, {to.seg, to.word + 5}, 64, text, sizeof(text), &bad);
    const std::string t(text);
    Check(switches == 1 && !bad, "decodes across the switch");
    Check(t.find("s0:3808~6[4,6..] |seg 1@50dc40000| s0:3818#1a4") != std::string::npos, "switch marker in place");
    Check(t.find("BAD") == std::string::npos, "no stale words");
    FormatAcross(segs, 2, at, {to.seg, to.word + 5}, 64, text, 24, &bad);
    Check(std::strlen(text) < 24, "small output buffer across segments");
}

} // namespace

int main()
{
    TestEventRing();
    TestDrawRecords();
    TestCrumbs();
    TestStallDetector();
    TestStallRetry();
    TestStallVerdict();
    TestTic();
    TestCommandWindow();
    TestListSegments();
    TestWindowAcrossSegments();
    if (g_failures)
        return 1;
    std::printf("PASS:DEKO9_GPUFAULT\n");
    return 0;
}
