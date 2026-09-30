// Host test for the retail PC server frame-rate smoothing budget
// (iw3sp SV_FrameRateSmoothing 0x5c8390, src/server/sv_framesmoothing.h) and
// the retail-TSC tick conversion (src/universal/retail_tsc.h).
//
// The reference model below is a line-by-line transcription of retail's
// 32-bit x86 arithmetic (wrapping adds, imul 0x66666667 / sar 2 / sign-fix
// divisions by 10) and is checked against the port helpers on hand-picked and
// seeded random sequences, including counter wrap.

#include "src/server/sv_framesmoothing.h"
#include "src/universal/retail_tsc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                              \
    do                                                                           \
    {                                                                            \
        if (!(cond))                                                             \
        {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

// Retail's signed divide by 10 as emitted: imul 0x66666667; sar edx,2;
// add edx, edx>>31.
static int32_t RetailDiv10(int32_t v)
{
    const int64_t product = (int64_t)v * 0x66666667LL;
    int32_t hi = (int32_t)(product >> 32);
    hi >>= 2;
    return hi + (int32_t)((uint32_t)hi >> 31);
}

struct RetailModel
{
    uint32_t lastTime; // 0x014bfb34
    uint32_t index;    // 0x014bfb38
    uint32_t delta[10];
    uint32_t slack[10];
};

// 0x5c8409..0x5c849e: ESI = sum(slack) clamped at 0, EAX = sum(delta),
// EDI = lastTime - now - ESI + EAX.
static int32_t RetailBudget(const RetailModel &m, uint32_t now, uint32_t *esiOut)
{
    uint32_t esi = 0, eax = 0;
    for (int i = 0; i < 10; ++i)
    {
        esi += m.slack[i];
        eax += m.delta[i];
    }
    if ((int32_t)esi < 0)
        esi = 0;
    *esiOut = esi;
    return (int32_t)(m.lastTime - now - esi + eax);
}

// 0x5c8538..0x5c8639.
static void RetailRecord(RetailModel &m, uint32_t now, int32_t edi, uint32_t esi, uint32_t end)
{
    const int32_t slot = (int32_t)m.index - RetailDiv10((int32_t)m.index) * 10;
    if (edi > 0)
    {
        int32_t v = (int32_t)(now - end + (uint32_t)edi);
        const int32_t sign = v >> 31;
        v = (v ^ sign) - sign; // cdq; xor; sub
        m.slack[slot] = (uint32_t)RetailDiv10(v);
    }
    else
    {
        const int32_t v = (int32_t)(esi + (uint32_t)edi);
        m.slack[slot] = v < 0 ? (uint32_t)RetailDiv10(v) : 0u;
    }
    m.delta[slot] = (uint32_t)RetailDiv10((int32_t)(end - m.lastTime));
    m.lastTime = end;
    m.index += 1;
}

static void CompareStates(const RetailModel &m, const SvFrameSmoothing &s)
{
    CHECK((uint32_t)s.lastTime == m.lastTime);
    CHECK((uint32_t)s.frameIndex == m.index);
    for (int i = 0; i < 10; ++i)
    {
        CHECK((uint32_t)s.frameDelta[i] == m.delta[i]);
        CHECK((uint32_t)s.slack[i] == m.slack[i]);
    }
}

static uint32_t g_rng = 12345u;
static uint32_t NextRand()
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

// Drive both models through `frames` frames.  Each frame: the sync ends at
// `now`, the optional slice ends somewhere around the budget (sometimes early,
// sometimes overrunning), then the next frame starts a frame period later.
static void RunSequence(uint32_t start, uint32_t period, uint32_t jitter, int frames, int *slices)
{
    RetailModel m;
    memset(&m, 0, sizeof(m));
    m.lastTime = start;
    SvFrameSmoothing s;
    memset(&s, 0, sizeof(s));
    s.lastTime = (int32_t)start;

    uint32_t now = start;
    for (int f = 0; f < frames; ++f)
    {
        now += period / 2 + (jitter ? NextRand() % jitter : 0);
        uint32_t esi = 0;
        const int32_t retailBudget = RetailBudget(m, now, &esi);
        const int32_t budget = SV_FrameSmoothing_Budget(&s, (int32_t)now);
        CHECK(budget == retailBudget);
        uint32_t end = now + 7;
        if (budget > 0)
        {
            ++*slices;
            // A slice ends at its cap give or take the time of one stage.
            end = now + (uint32_t)budget + (NextRand() % 64) - 32;
        }
        RetailRecord(m, now, retailBudget, esi, end);
        SV_FrameSmoothing_Record(&s, (int32_t)now, budget, (int32_t)end);
        CompareStates(m, s);
        now = end + period / 2;
    }
}

static void TestBudgetMatchesRetail()
{
    int slices = 0;
    // 30 fps at the Switch counter (19.2 MHz): 640,000 ticks per frame.
    RunSequence(1000u, 640000u, 20000u, 200, &slices);
    CHECK(slices > 0);
    // Counter wrap: start just below 2^32.
    slices = 0;
    RunSequence(0xFFFF0000u, 640000u, 50000u, 200, &slices);
    CHECK(slices > 0);
    // Large irregular periods (hitches) and zero jitter.
    slices = 0;
    RunSequence(77u, 5000000u, 0u, 50, &slices);
    RunSequence(0x7FFFFF00u, 1234567u, 999999u, 100, &slices);
}

static void TestSteadyStateBudget()
{
    // Ten frames of exactly P ticks: the ring averages to P, so right after
    // a sync that ended X ticks into the frame the budget is P - X.
    SvFrameSmoothing s;
    memset(&s, 0, sizeof(s));
    const int32_t P = 640000;
    for (int i = 0; i < 10; ++i)
        s.frameDelta[i] = P / 10;
    s.lastTime = 5000;
    CHECK(SV_FrameSmoothing_Budget(&s, 5000 + 200000) == P - 200000);
    // Positive slack (recent slices overran) is subtracted.
    for (int i = 0; i < 10; ++i)
        s.slack[i] = 1000;
    CHECK(SV_FrameSmoothing_Budget(&s, 5000 + 200000) == P - 200000 - 10000);
    // A negative slack sum is clamped to 0, never added.
    for (int i = 0; i < 10; ++i)
        s.slack[i] = -1000;
    CHECK(SV_FrameSmoothing_Budget(&s, 5000 + 200000) == P - 200000);
    // Past the average frame: no budget.
    CHECK(SV_FrameSmoothing_Budget(&s, 5000 + P + 1) < 0);
}

static void TestRetailTsc()
{
    // Switch: 19.2 MHz => msecPerRawTimerTick = 1000 / 19.2e6.
    const double switchMsecPerTick = 1000.0 / 19200000.0;
    // 1,000,000 ticks at the 2.4 GHz reference = 416.67 us = 8000 ticks.
    CHECK(RetailTsc_ToRawTicks(1000000.0, switchMsecPerTick) == 8000u);
    // 20,000 ticks = 8.33 us = 160 ticks.
    CHECK(RetailTsc_ToRawTicks(20000.0, switchMsecPerTick) == 160u);
    // Same duration on a 2.4 GHz TSC host is the literal itself.
    CHECK(RetailTsc_ToRawTicks(1000000.0, 1000.0 / 2400000000.0) == 1000000u);
    // Never collapses a non-zero budget to 0; uninitialised timing passes
    // the literal through.
    CHECK(RetailTsc_ToRawTicks(1.0, switchMsecPerTick) == 1u);
    CHECK(RetailTsc_ToRawTicks(0.0, switchMsecPerTick) == 0u);
    CHECK(RetailTsc_ToRawTicks(20000.0, 0.0) == 20000u);
}

int main()
{
    TestRetailTsc();
    TestSteadyStateBudget();
    TestBudgetMatchesRetail();
    if (g_failures)
    {
        printf("FAIL:SV_FRAME_SMOOTHING failures=%d\n", g_failures);
        return 1;
    }
    printf("PASS:SV_FRAME_SMOOTHING\n");
    return 0;
}
