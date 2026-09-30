// Host test (ASan/UBSan) for the pure parts of the deko9 GPU-fault black box
// (src/deko9/deko9_gpufault.h): memory event ring, per-draw record ring,
// breadcrumb encoding, stall detector and the TIC address decode. Run by
// ./test host (deko9_gpufault_sanitizer_check).

#include "src/deko9/deko9_gpufault.h"

#include <cstdio>
#include <memory>
#include <string>

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
}

void TestCrumbs()
{
    Check(CrumbValue(0x12345, 7) == 0x23450007u, "crumb packs low 16 bits of seq");
    Check(CrumbDraw(CrumbValue(3, 70000)) == kCrumbListEnd - 1, "draw index saturates below the end marker");
    Check(CrumbDraw(CrumbEnd(3)) == kCrumbListEnd, "end marker");
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

void TestTic()
{
    uint32_t words[8] = {0x11111111u, 0x23916000u, 0xabcd0005u, 0, 0, 0, 0, 0};
    Check(TicAddress(words) == 0x523916000ull, "TIC address ignores the fields above address_high");
    words[2] = 0x0000ffa6u;
    Check(TicAddress(words) == 0xffa623916000ull, "16-bit address_high");
    const uint32_t handle = 1234u | (56u << 20);
    Check(HandleImage(handle) == 1234 && HandleSampler(handle) == 56, "texture handle split");
}

} // namespace

int main()
{
    TestEventRing();
    TestDrawRecords();
    TestCrumbs();
    TestStallDetector();
    TestTic();
    if (g_failures)
        return 1;
    std::printf("PASS:DEKO9_GPUFAULT\n");
    return 0;
}
