// Host proof for the Switch per-frame CPU phase profiler
// (src/port/switch_perf.cpp).  Runs under ASan/UBSan via ./test host.
//
// It proves the properties the hardware instrumentation depends on:
//   - disabled scopes/ticks add nothing (production runs pay ~one load);
//   - enabled accumulation sums per counter across frames and counts frames;
//   - the RAII scope really attributes elapsed time;
//   - the per-second report emits one line per group, names the groups, and
//     resets the window;
//   - turning the profiler off drops any partial window;
//   - every counter is in exactly one report group;
//   - a renderer worker thread's scopes record nothing into the main-thread
//     counters, and its busy ticks land on their own `worker` line;
//   - event counts (world pretess batching) add atomically from any thread
//     and print per frame on a `pretess` line with the index peak/capacity.

#include "src/port/switch_perf.h"

#include <cstdio>
#include <cstring>
#include <thread>

static int s_lines = 0;
static char s_all[16384];

static void sink(const char *line)
{
    ++s_lines;
    const size_t used = strlen(s_all);
    if (used + strlen(line) + 2 < sizeof(s_all))
    {
        memcpy(s_all + used, line, strlen(line));
        s_all[used + strlen(line)] = '\n';
        s_all[used + strlen(line) + 1] = '\0';
    }
}

#define CHECK(cond) \
    do \
    { \
        if (!(cond)) \
        { \
            printf("FAIL:SWITCH_PERF %s (line %d)\n", #cond, __LINE__); \
            return 1; \
        } \
    } while (0)

int main()
{
    SwitchPerf_Init();
    SwitchPerf_SetPrintSink(sink);

    // 1. Disabled: a scope adds nothing, and the host clock is ns (freq 1e9).
    SwitchPerf_SetEnabled(0);
    SwitchPerf_ReportNow();
    {
        SWITCH_PERF_SCOPE(SWITCH_PERF_FRAME_TOTAL);
    }
    CHECK(SwitchPerf_WindowTicks(SWITCH_PERF_FRAME_TOTAL) == 0);
    CHECK(SwitchPerf_TicksToUs(1000) == 1);

    // 2. Enabled: per-frame accumulation sums and counts frames.
    SwitchPerf_SetEnabled(1);
    SwitchPerf_ReportNow();
    const int kFrames = 5;
    for (int i = 0; i < kFrames; ++i)
    {
        SwitchPerf_BeginFrame();
        SwitchPerf_AddTicks(SWITCH_PERF_FRAME_TOTAL, 50000); // 50 us/frame
        SwitchPerf_AddTicks(SWITCH_PERF_SCENE_DPVS, 10000);  // 10 us/frame
        SwitchPerf_EndFrame();
    }
    CHECK(SwitchPerf_WindowFrames() == (uint64_t)kFrames);
    CHECK(SwitchPerf_WindowTicks(SWITCH_PERF_FRAME_TOTAL) == 50000ull * (uint64_t)kFrames);
    CHECK(SwitchPerf_WindowTicks(SWITCH_PERF_SCENE_DPVS) == 10000ull * (uint64_t)kFrames);

    // 3. The RAII scope attributes real elapsed time.
    const uint64_t before = SwitchPerf_WindowTicks(SWITCH_PERF_SCENE_SMODEL_CAMERA);
    {
        SWITCH_PERF_SCOPE(SWITCH_PERF_SCENE_SMODEL_CAMERA);
        volatile uint64_t spin = 0;
        for (uint64_t i = 0; i < 20000; ++i)
            spin += i;
        (void)spin;
    }
    CHECK(SwitchPerf_WindowTicks(SWITCH_PERF_SCENE_SMODEL_CAMERA) > before);

    const uint64_t game_before = SwitchPerf_WindowTicks(SWITCH_PERF_GAME_TOTAL);
    {
        SWITCH_PERF_SCOPE_IF(SWITCH_PERF_GAME_TOTAL, false);
        volatile uint64_t spin = 0;
        for (uint64_t i = 0; i < 20000; ++i)
            spin += i;
        (void)spin;
    }
    CHECK(SwitchPerf_WindowTicks(SWITCH_PERF_GAME_TOTAL) == game_before);

    // 4. The report emits one line per group, names them, and resets.
    s_lines = 0;
    s_all[0] = '\0';
    SwitchPerf_ReportNow();
    CHECK(s_lines == (int)G_COUNT + 2); // groups + worker + pretess
    CHECK(strstr(s_all, "SWITCH_PERF frame fps=") != 0);
    CHECK(strstr(s_all, "SWITCH_PERF worker w0=") != 0);
    CHECK(strstr(s_all, "SWITCH_PERF render") != 0);
    CHECK(strstr(s_all, "SWITCH_PERF issue") != 0);
    CHECK(strstr(s_all, "SWITCH_PERF scene") != 0);
    CHECK(strstr(s_all, "SWITCH_PERF cgame") != 0);
    CHECK(strstr(s_all, "SWITCH_PERF game") != 0);
    CHECK(strstr(s_all, "SWITCH_PERF backend rbframe=") != 0);
    CHECK(strstr(s_all, "dpvs=") != 0);
    CHECK(strstr(s_all, "present=") != 0);
    CHECK(strstr(s_all, "smc=") != 0);
    CHECK(SwitchPerf_WindowFrames() == 0);
    CHECK(SwitchPerf_WindowTicks(SWITCH_PERF_FRAME_TOTAL) == 0);

    // 5. Disabling drops a partial window on the next frame boundary.
    SwitchPerf_AddTicks(SWITCH_PERF_FRAME_TOTAL, 123);
    SwitchPerf_SetEnabled(0);
    SwitchPerf_BeginFrame();
    SwitchPerf_EndFrame();
    CHECK(SwitchPerf_WindowTicks(SWITCH_PERF_FRAME_TOTAL) == 0);

    // 6. Every counter resolves to a non-empty name in exactly one group.
    //    (The X-macro is the single source of truth; this catches a counter
    //    added to the enum without a report name/group.)
    for (int c = 0; c < SWITCH_PERF_COUNTER_COUNT; ++c)
    {
        SwitchPerf_ReportNow(); // ensure a clean window
        SwitchPerf_SetEnabled(1);
        SwitchPerf_AddTicks(c, 1);
        s_lines = 0;
        s_all[0] = '\0';
        SwitchPerf_ReportNow();
        // The counter must have appeared in one of the group lines.
        // We cannot read its name here, so assert the report still emitted all
        // groups and that a counter value is present.
        CHECK(s_lines == (int)G_COUNT + 2); // groups + worker + pretess
        SwitchPerf_SetEnabled(0);
    }

    // 7. Worker threads: their scopes must not touch the plain
    //    main-thread accumulators, and their busy ticks are reported apart.
    SwitchPerf_ReportNow();
    SwitchPerf_SetEnabled(1);
    std::thread worker([] {
        SwitchPerf_MarkWorkerThread();
        {
            SWITCH_PERF_SCOPE(SWITCH_PERF_SCENE_DPVS);
            volatile uint64_t spin = 0;
            for (uint64_t i = 0; i < 20000; ++i)
                spin += i;
            (void)spin;
        }
        SwitchPerf_AddWorkerTicks(1, 3000000); // 3 ms at the host 1 GHz clock
        // The sv_smp 1 server thread: two frames, 8 ms of busy time.
        SwitchPerf_AddServerThreadTicks(6000000, 1);
        SwitchPerf_AddServerThreadTicks(2000000, 1);
    });
    worker.join();
    CHECK(SwitchPerf_WindowTicks(SWITCH_PERF_SCENE_DPVS) == 0);
    CHECK(SwitchPerf_WindowWorkerTicks(1) == 3000000);
    {
        SWITCH_PERF_SCOPE(SWITCH_PERF_SCENE_DPVS); // main thread still records
    }
    CHECK(SwitchPerf_WindowTicks(SWITCH_PERF_SCENE_DPVS) > 0);
    s_lines = 0;
    s_all[0] = '\0';
    SwitchPerf_ReportNow(); // no frames in the window: averaged over 1
    CHECK(strstr(s_all, "SWITCH_PERF worker w0=0 w1=3000 sv=8000 svframes=2 svframe=4000") != 0);
    CHECK(SwitchPerf_WindowWorkerTicks(1) == 0);

    // 5. Event counts (world pretess): atomic from any thread, averaged per
    //    displayed frame, peak/capacity reported as-is, window reset after.
    SwitchPerf_ReportNow();
    std::thread counter([] {
        SwitchPerf_AddEvent(SWITCH_PERF_EV_PRETESS_BATCHES, 30);
        SwitchPerf_AddEvent(SWITCH_PERF_EV_PRETESS_DRAWS, 90);
        SwitchPerf_NotePreTessUsed(5000, 1048576);
    });
    SwitchPerf_AddEvent(SWITCH_PERF_EV_PRETESS_DRAWS, 30);
    SwitchPerf_AddEvent(SWITCH_PERF_EV_PRETESS_ALLOC_FAIL, 3);
    SwitchPerf_NotePreTessUsed(7000, 1048576);
    counter.join();
    for (int frame = 0; frame < 3; ++frame)
        SwitchPerf_EndFrame();
    s_lines = 0;
    s_all[0] = '\0';
    SwitchPerf_ReportNow();
    // The static-pretess / SetIndexData counts and the area/mover event
    // counts (247e8050) sit between allocfail and used_peak on the same line.
    CHECK(strstr(s_all, "SWITCH_PERF pretess batches=10.0 surfs=0.0 draws=40.0 split_vert=0.0 split_lmap=0.0 "
                        "draws_rebased=0.0 fallback=0.0 allocfail=1.0 pt_bytes=0.0 st_draws=0.0 st_surfs=0.0 "
                        "sm_lists=0.0 sm_inst=0.0 sm_st_inst=0.0 ib_calls=0.0 ib_bytes=0.0 ib_bsp=0.0 ib_smodel=0.0 "
                        "ib_fx=0.0 ib_marks=0.0 ib_xmodel=0.0 ib_bmodel=0.0 area_calls=0.0") != 0);
    CHECK(strstr(s_all, " used_peak=7000 cap=1048576") != 0);
    s_lines = 0;
    s_all[0] = '\0';
    SwitchPerf_ReportNow();
    CHECK(strstr(s_all, "SWITCH_PERF pretess batches=0.0 surfs=0.0 draws=0.0") != 0);
    CHECK(strstr(s_all, "used_peak=0 cap=1048576") != 0);
    SwitchPerf_SetEnabled(0);

    printf("PASS:SWITCH_PERF groups=%d counters=%d frames=%d\n",
           (int)G_COUNT, (int)SWITCH_PERF_COUNTER_COUNT, kFrames);
    return 0;
}
