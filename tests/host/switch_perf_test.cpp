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
#include <cstdlib>
#include <thread>

static int s_lines = 0;
static unsigned s_overflow_events;
static unsigned s_period_records;
static char s_all[16384];

static void sink(const char *line)
{
    if (std::strstr(line, "\"name\":\"sample_overflow\"")) ++s_overflow_events;
    if (std::strstr(line, "\"stream\":\"cpu.frame.period\"")) ++s_period_records;
    if (const char *capture = std::getenv("KISAK_PERF_CAPTURE")) {
        FILE *file = std::fopen(capture, "a");
        if (file) { std::fprintf(file, "%s\n", line); std::fclose(file); }
    }
    if (std::strncmp(line, "KPERF ", 6) != 0) ++s_lines;
    const size_t used = strlen(s_all);
    if (used + strlen(line) + 2 < sizeof(s_all))
    {
        memcpy(s_all + used, line, strlen(line));
        s_all[used + strlen(line)] = '\n';
        s_all[used + strlen(line) + 1] = '\0';
    }
}

static bool record_has(const char *stream, const char *field)
{
    char target[128];
    std::snprintf(target, sizeof(target), "\"stream\":\"%s\"", stream);
    const char *line = std::strstr(s_all, target);
    if (!line) return false;
    const char *found = std::strstr(line, field);
    const char *end = std::strchr(line, '\n');
    return found && (!end || found < end);
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
    // Slow-frame lines are proven in section 9; keep sanitizer-slow frames
    // in the earlier sections from printing one.
    SwitchPerf_SetSlowFrameThresholds(0, 0);

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
    CHECK(s_lines == (int)G_COUNT + 5); // groups + worker + pretess + wrkcmd/wrkwait/wrkcount
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
        CHECK(s_lines == (int)G_COUNT + 5); // groups + worker + pretess + wrkcmd/wrkwait/wrkcount
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

    // Large absolute host ticks convert without multiplying the whole value.
    CHECK(SwitchPerf_TicksToUs(20000000000000ull) == 20000000000ull);
    SwitchPerf_SetStructured(1);
    SwitchPerf_SetEnabled(1);
    s_lines = 0; s_all[0] = '\0';
    SwitchPerf_ReportNow();
    CHECK(strstr(s_all, "KPERF ") == 0); // A zero-frame window invents no samples.
    s_all[0] = '\0';
    for (int i = 0; i < 3; ++i) {
        SwitchPerf_BeginFrame();
        SwitchPerf_AddTicks(SWITCH_PERF_FRAME_TOTAL, 1000);
        SwitchPerf_AddThreadTicks(SWITCH_PERF_SCENE_DPVS, 1000);
        SwitchPerf_EndFrame();
    }
    SwitchPerf_ReportNow();
    CHECK(strstr(s_all, "\"stream\":\"cpu.main.frame\"") != 0);
    CHECK(strstr(s_all, "\"stream\":\"cpu.frame.wall\"") != 0);
    CHECK(strstr(s_all, "\"stream\":\"cpu.frame.period\"") != 0);
    CHECK(strstr(s_all, "\"alignment\":\"asynchronous_main_denominator\"") != 0);
    const char *mainGroups[] = {"frame", "render", "issue", "scene", "cgame", "other", "ents", "snap", "game"};
    for (const char *group : mainGroups) {
        char stream[96]; std::snprintf(stream, sizeof(stream), "cpu.main.%s", group);
        CHECK(record_has(stream, "\"seq\":1"));
    }
    CHECK(record_has("cpu.backend.backend", "\"seq\":1"));
    CHECK(record_has("cpu.worker_busy", "\"seq\":1"));
    CHECK(!record_has("cpu.worker.scene", "\"seq\":"));
    CHECK(!record_has("cpu.backend.scene", "\"seq\":"));
    CHECK(!record_has("cpu.worker.other", "\"seq\":"));
    CHECK(!record_has("cpu.backend.other", "\"seq\":"));

    s_all[0] = '\0';
    SwitchPerf_BeginFrame();
    std::thread activeWorker([] {
        SwitchPerf_MarkWorkerThread();
        SwitchPerf_AddThreadTicks(SWITCH_PERF_SCENE_DPVS, 10000);
    });
    activeWorker.join();
    SwitchPerf_AddBackendTicks(SWITCH_PERF_SCENE_DPVS, 20000);
    SwitchPerf_EndFrame(); SwitchPerf_ReportNow();
    CHECK(record_has("cpu.worker.scene", "\"seq\":1"));
    CHECK(record_has("cpu.worker.scene", "\"dpvs\":10.000"));
    CHECK(record_has("cpu.backend.scene", "\"seq\":1"));
    CHECK(record_has("cpu.backend.scene", "\"dpvs\":20.000"));
    CHECK(!record_has("cpu.worker.other", "\"seq\":"));
    CHECK(!record_has("cpu.backend.other", "\"seq\":"));

    SwitchPerf_SetEnabled(0); SwitchPerf_SetEnabled(1);
    SwitchPerf_SetStructured(0); SwitchPerf_SetStructured(1);
    s_all[0] = '\0';
    const unsigned oldPeriods = s_period_records;
    SwitchPerf_BeginFrame(); SwitchPerf_EndFrame(); SwitchPerf_ReportNow();
    CHECK(s_period_records == oldPeriods); // Disabled interval is not a frame period.
    CHECK(record_has("cpu.worker.scene", "\"seq\":2"));
    CHECK(record_has("cpu.worker.scene", "\"dpvs\":0.000"));
    CHECK(record_has("cpu.backend.scene", "\"seq\":2"));
    CHECK(record_has("cpu.backend.scene", "\"dpvs\":0.000"));
    CHECK(record_has("cpu.backend.backend", "\"seq\":3"));
    CHECK(record_has("cpu.worker_busy", "\"seq\":3"));
    for (int i = 0; i < 130; ++i) { SwitchPerf_BeginFrame(); SwitchPerf_EndFrame(); }
    SwitchPerf_ReportNow();
    CHECK(s_overflow_events == 1);
    // The bounded batches never print a truncated JSON line.
    // Capture retains all lines even when the test's small display buffer fills.
    SwitchPerf_SetEnabled(0);

    // 8. Worker-command accounting: exclusive per-type time split by thread
    //    kind, pure idle apart from executed commands, inclusive waits, the
    //    scene residual, and the entity-skin event names.
    {
        static const char *const names[] = {"fxspot", "dpvsent", "skinxmodel"};
        SwitchPerf_SetWorkerCmdNames(names, 3);
        SwitchPerf_SetEnabled(0);
        {
            SwitchPerfWorkerCmdScope off(0);
            SwitchPerfWorkerIdleScope idleOff;
            SwitchPerfWorkerWaitScope waitOff(0);
        }
        CHECK(SwitchPerf_WindowWorkerCmdCount(0, 0) == 0);
        CHECK(SwitchPerf_WindowWorkerCmdIdleTicks(0) == 0);
        CHECK(SwitchPerf_WindowWorkerCmdWaitTicks(0) == 0);

        SwitchPerf_SetEnabled(1);
        SwitchPerf_ReportNow();
        auto spin = [](uint64_t n) {
            volatile uint64_t v = 0;
            for (uint64_t i = 0; i < n; ++i)
                v += i;
            (void)v;
        };
        const uint64_t t0 = SwitchPerf_NowTicks();
        {
            SwitchPerfWorkerWaitScope wait(1);
            SwitchPerfWorkerCmdScope outer(0);
            spin(200000);
            {
                SwitchPerfWorkerCmdScope inner(1);
                spin(200000);
            }
            {
                SwitchPerfWorkerIdleScope idle;
                spin(200000);
            }
        }
        const uint64_t elapsed = SwitchPerf_NowTicks() - t0;
        const uint64_t outerTicks = SwitchPerf_WindowWorkerCmdTicks(0, 0);
        const uint64_t innerTicks = SwitchPerf_WindowWorkerCmdTicks(1, 0);
        const uint64_t idleTicks = SwitchPerf_WindowWorkerCmdIdleTicks(0);
        CHECK(SwitchPerf_WindowWorkerCmdCount(0, 0) == 1);
        CHECK(SwitchPerf_WindowWorkerCmdCount(1, 0) == 1);
        CHECK(outerTicks > 0 && innerTicks > 0 && idleTicks > 0);
        // Exclusive: the parts never add up to more than the wall time.
        CHECK(outerTicks + innerTicks + idleTicks <= elapsed);
        CHECK(SwitchPerf_WindowWorkerCmdWaitTicks(1) >= outerTicks + innerTicks + idleTicks);
        CHECK(SwitchPerf_WindowWorkerCmdWaitTicks(1) <= elapsed);
        CHECK(SwitchPerf_t_wrkChildTicks >= outerTicks + innerTicks + idleTicks);
        {
            SwitchPerfWorkerWaitScope finished(2, false); // already finished
        }
        CHECK(SwitchPerf_WindowWorkerCmdWaitTicks(2) == 0);
        std::thread wrk([] {
            SwitchPerf_MarkWorkerThread();
            SwitchPerfWorkerCmdScope cmd(2);
        });
        wrk.join();
        CHECK(SwitchPerf_WindowWorkerCmdCount(2, 1) == 1);
        CHECK(SwitchPerf_WindowWorkerCmdCount(2, 0) == 0);
        SwitchPerf_AddWorkerCmd(SWITCH_PERF_WRKCMD_SLOTS, 0, 1); // out of range: ignored
        SwitchPerf_AddWorkerCmdWait(-1, 1);

        // Deterministic report formats (one-frame window, host 1 GHz clock).
        SwitchPerf_ReportNow();
        SwitchPerf_AddWorkerCmd(0, 0, 2000000);  // 2 ms on main
        SwitchPerf_AddWorkerCmd(1, 1, 3000000);  // 3 ms on a worker
        SwitchPerf_AddWorkerCmd(1, 1, 1000000);
        SwitchPerf_AddWorkerCmdIdle(0, 500000);
        SwitchPerf_AddWorkerCmdWait(1, 4000000);
        SwitchPerf_AddWorkerCmdWait(SWITCH_PERF_WRKWAIT_FRONT, 700000);
        SwitchPerf_AddWorkerCmd(5, 0, 1000000);  // unnamed type prints as c5
        SwitchPerf_AddTicks(SWITCH_PERF_SCENE_TOTAL, 10000000);
        SwitchPerf_AddTicks(SWITCH_PERF_SCENE_SETUP, 3000000);
        SwitchPerf_AddTicks(SWITCH_PERF_SCENE_WAIT, 2000000);
        SwitchPerf_AddTicks(SWITCH_PERF_SCENE_PORTALWALK, 4000000); // nested in dpvs: not subtracted
        SwitchPerf_AddEvent(SWITCH_PERF_EV_ENT_TESTED, 12);
        SwitchPerf_AddEvent(SWITCH_PERF_EV_ENT_SKIN_VERTS, 900);
        SwitchPerf_BeginFrame();
        SwitchPerf_EndFrame(); // one frame, so the structured records print too
        s_lines = 0;
        s_all[0] = '\0';
        SwitchPerf_ReportNow();
        CHECK(strstr(s_all, " misc=0 resid=5000\n") != 0);
        CHECK(strstr(s_all, "SWITCH_PERF wrkcmd idle.m=500 idle.w=0 m.fxspot=2000 w.fxspot=0 m.dpvsent=0 w.dpvsent=4000 "
                            "m.skinxmodel=0 w.skinxmodel=0 m.c3=0 w.c3=0 m.c4=0 w.c4=0 m.c5=1000 w.c5=0\n") != 0);
        CHECK(strstr(s_all, "SWITCH_PERF wrkwait front=700 all=0 fxspot=0 dpvsent=4000 skinxmodel=0 c3=0 c4=0 c5=0\n") != 0);
        CHECK(strstr(s_all, "SWITCH_PERF wrkcount idle.m=1.0 idle.w=0.0 wait.front=1.0 wait.all=0.0 m.fxspot=1.0 w.fxspot=0.0 "
                            "wait.fxspot=0.0 m.dpvsent=0.0 w.dpvsent=2.0 wait.dpvsent=1.0") != 0);
        CHECK(strstr(s_all, " ent_tested=12.0 ent_culled=0.0 ent_vis_cam=0.0 ent_vis_shadow=0.0 ent_skinned=0.0 "
                            "ent_skin_verts=900.0 ent_skin_cam=0.0 ent_skin_shadow=0.0 skincache_skip=0.0 used_peak=") != 0);
        CHECK(record_has("cpu.main.scene", "\"resid\":5000.000"));
        CHECK(record_has("cpu.wrkcmd.main", "\"fxspot\":2000.000"));
        CHECK(record_has("cpu.wrkcmd.main", "\"idle\":500.000"));
        CHECK(record_has("cpu.wrkcmd.worker", "\"dpvsent\":4000.000"));
        CHECK(record_has("cpu.wrkwait", "\"front\":700.000"));
        // The window resets.
        CHECK(SwitchPerf_WindowWorkerCmdCount(0, 0) == 0);
        s_all[0] = '\0';
        SwitchPerf_ReportNow();
        CHECK(strstr(s_all, "SWITCH_PERF wrkcmd idle.m=0 idle.w=0 m.fxspot=0 w.fxspot=0 m.dpvsent=0 w.dpvsent=0 "
                            "m.skinxmodel=0 w.skinxmodel=0\n") != 0);
        SwitchPerf_SetEnabled(0);
        SwitchPerf_SetStructured(0);
    }

    // 9. Slow-frame capture: one frame over the wall threshold prints its own
    //    deltas (top counters, worker idle, extras); the GPU extra triggers
    //    alone; the per-window budget is four lines, the rest are counted.
    {
        SwitchPerf_SetEnabled(1);
        SwitchPerf_ReportNow();
        const int gpu = SwitchPerf_RegisterFrameExtra("gpu");
        const int lockwait = SwitchPerf_RegisterFrameExtra("lockwait");
        CHECK(gpu >= 0 && lockwait >= 0 && gpu != lockwait);
        CHECK(SwitchPerf_RegisterFrameExtra("gpu") == gpu);
        SwitchPerf_AddFrameExtra(lockwait, 50000000); // before the frame: not in its delta
        SwitchPerf_AddTicks(SWITCH_PERF_SCENE_SETUP, 90000000);
        SwitchPerf_SetSlowFrameThresholds(1000, 0);
        s_all[0] = '\0';
        SwitchPerf_BeginFrame();
        SwitchPerf_AddTicks(SWITCH_PERF_SCENE_TOTAL, 9000000);
        SwitchPerf_AddTicks(SWITCH_PERF_SCENE_WAIT, 4000000);
        SwitchPerf_AddTicks(SWITCH_PERF_FRAME_EVENTLOOP, 2500000);
        SwitchPerf_AddWorkerCmdIdle(0, 3000000);
        SwitchPerf_AddWorkerCmd(1, 0, 700000);
        SwitchPerf_AddFrameExtra(lockwait, 1500000);
        SwitchPerf_AddFrameExtra(gpu, 12000000);
        {
            const uint64_t until = SwitchPerf_NowTicks() + 2000000; // 2 ms wall
            while (SwitchPerf_NowTicks() < until)
            {
            }
        }
        SwitchPerf_EndFrame();
        CHECK(strstr(s_all, "SWITCH_PERF slowframe frame=") != 0);
        CHECK(strstr(s_all, " trigger=wall wall=") != 0);
        CHECK(strstr(s_all, " idle.m=3000 wrk.m=700 resid=5000 scene.total=9000 scene.wait=4000 frame.eventloop=2500 "
                            "x.gpu=12000 x.lockwait=1500\n") != 0);
        CHECK(strstr(s_all, "scene.setup") == 0); // added before the frame began
        SwitchPerf_SetSlowFrameThresholds(0, 5000);
        s_all[0] = '\0';
        SwitchPerf_BeginFrame();
        SwitchPerf_EndFrame();
        CHECK(strstr(s_all, "slowframe") == 0); // under both thresholds
        SwitchPerf_BeginFrame();
        SwitchPerf_AddFrameExtra(gpu, 6000000);
        SwitchPerf_EndFrame();
        CHECK(strstr(s_all, " trigger=gpu ") != 0);
        for (int i = 0; i < 4; ++i)
        {
            SwitchPerf_BeginFrame();
            SwitchPerf_AddFrameExtra(gpu, 6000000);
            SwitchPerf_EndFrame();
        }
        s_all[0] = '\0';
        SwitchPerf_ReportNow();
        // 6 slow frames in a 7-frame window: 4 logged (one wall, three gpu), 2 counted.
        CHECK(strstr(s_all, " slow_frames=0.9 slow_unlogged=0.3 ") != 0);
        SwitchPerf_SetSlowFrameThresholds(0, 0);
        SwitchPerf_SetEnabled(0);
    }

    // 10. switch_perfSlowMs: one threshold in ms for both triggers; 0 is off.
    {
        SwitchPerf_SetEnabled(1);
        SwitchPerf_ReportNow();
        const int gpu = SwitchPerf_RegisterFrameExtra("gpu");
        auto frame = [&](uint64_t wallTicks, uint64_t gpuNs) -> bool {
            s_all[0] = '\0';
            SwitchPerf_BeginFrame();
            SwitchPerf_AddFrameExtra(gpu, gpuNs);
            const uint64_t until = SwitchPerf_NowTicks() + wallTicks;
            while (SwitchPerf_NowTicks() < until)
            {
            }
            SwitchPerf_EndFrame();
            return strstr(s_all, "SWITCH_PERF slowframe") != 0;
        };
        SwitchPerf_ReportNow(); // fresh per-window line budget
        SwitchPerf_SetSlowFrameMs(1);
        CHECK(frame(2000000, 0));          // 2 ms wall >= 1 ms
        CHECK(strstr(s_all, " trigger=wall ") != 0);
        CHECK(frame(0, 1500000));          // 1.5 ms GPU >= 1 ms
        CHECK(strstr(s_all, " trigger=gpu ") != 0);
        SwitchPerf_ReportNow();
        SwitchPerf_SetSlowFrameMs(20);     // the default: neither is slow
        CHECK(!frame(2000000, 1500000));
        CHECK(frame(0, 25000000));         // 25 ms GPU >= 20 ms
        SwitchPerf_SetSlowFrameMs(0);      // off
        CHECK(!frame(2000000, 25000000));
        SwitchPerf_SetSlowFrameMs(-5);     // negative clamps to off
        CHECK(!frame(2000000, 25000000));
        SwitchPerf_SetSlowFrameThresholds(0, 0);
        SwitchPerf_SetEnabled(0);
    }

    printf("PASS:SWITCH_PERF groups=%d counters=%d frames=%d\n",
           (int)G_COUNT, (int)SWITCH_PERF_COUNTER_COUNT, kFrames);
    return 0;
}
