#include "switch_perf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../platform/switch/switch_profile_format.h"

#if defined(__SWITCH__)
// The AArch64 TSC intrinsic does not exist on Horizon; the supported physical
// counter is armGetSystemTick (see switch_compat.h).  Its frequency is read
// once, so the scopes are just a register read each.
#include <switch.h>
#include <platform/switch/switch_port_log.h>
static uint64_t sp_now(void) { return armGetSystemTick(); }
static uint64_t sp_freq(void) { return (uint64_t)armGetSystemTickFreq(); }
#else
#include <time.h>
static uint64_t sp_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t sp_freq(void) { return 1000000000ull; }
#endif

// Weak: host perf tests link this file without switch_thread.cpp.
size_t Switch_ServerStackUsed(void) __attribute__((weak));
// switch_clocks.cpp (Switch only): SWITCH_CLOCKS line when the rates change.
void Switch_ClocksReport(void) __attribute__((weak));

int SwitchPerf_g_enabled = 0;
uint64_t SwitchPerf_g_ticks[SWITCH_PERF_COUNTER_COUNT];
thread_local int SwitchPerf_t_workerThread = 0;
static uint64_t s_worker_ticks[SWITCH_PERF_WORKER_COUNT];
static uint64_t s_worker_window_ticks[SWITCH_PERF_WORKER_COUNT];
static uint64_t s_worker_counter_ticks[SWITCH_PERF_COUNTER_COUNT];
static uint64_t s_worker_counter_window[SWITCH_PERF_COUNTER_COUNT];
static uint64_t s_backend_counter_ticks[SWITCH_PERF_COUNTER_COUNT];
static uint64_t s_backend_counter_window[SWITCH_PERF_COUNTER_COUNT];
static uint64_t s_server_ticks;
static uint64_t s_server_frames;
static uint64_t s_server_window_ticks;
static uint64_t s_server_window_frames;
static uint64_t s_events[SWITCH_PERF_EVENT_COUNT];
static uint64_t s_events_window[SWITCH_PERF_EVENT_COUNT];
static uint64_t s_pretess_used_peak;
static uint64_t s_pretess_used_peak_window;
static uint64_t s_pretess_cap;
static SwitchPerfPrintFn s_print = 0;
static uint64_t s_window_start_ticks = 0;
static uint64_t s_frames = 0;
static int s_inited = 0;
static int s_structured;
static uint64_t s_frame_id, s_window_first, s_begin_tick, s_prev_begin;
static bool s_group_seen[G_COUNT][3];
static uint64_t s_record_seq[G_COUNT][3], s_sample_seq[2], s_loss_seq, s_busy_seq;
struct FrameSample { uint64_t frame, time, begin, wall, period; bool hasPeriod; };
static FrameSample s_samples[128];
static unsigned s_sample_count;
static uint64_t s_sample_dropped;

thread_local uint64_t SwitchPerf_t_wrkChildTicks = 0;
// Worker-command accounting: [thread kind][type] live (atomic, any thread)
// and the window snapshot taken at report time.
static uint64_t s_wrk_ticks[2][SWITCH_PERF_WRKCMD_SLOTS];
static uint64_t s_wrk_count[2][SWITCH_PERF_WRKCMD_SLOTS];
static uint64_t s_wrk_idle_ticks[2];
static uint64_t s_wrk_idle_count[2];
static uint64_t s_wrk_wait_ticks[SWITCH_PERF_WRKWAIT_SLOTS];
static uint64_t s_wrk_wait_count[SWITCH_PERF_WRKWAIT_SLOTS];
static uint64_t s_wrkw_ticks[2][SWITCH_PERF_WRKCMD_SLOTS];
static uint64_t s_wrkw_count[2][SWITCH_PERF_WRKCMD_SLOTS];
static uint64_t s_wrkw_idle_ticks[2];
static uint64_t s_wrkw_idle_count[2];
static uint64_t s_wrkw_wait_ticks[SWITCH_PERF_WRKWAIT_SLOTS];
static uint64_t s_wrkw_wait_count[SWITCH_PERF_WRKWAIT_SLOTS];
static const char *s_wrk_names[SWITCH_PERF_WRKCMD_SLOTS];
// Frame extras and the slow-frame capture.
static const char *s_extra_names[SWITCH_PERF_EXTRA_SLOTS];
static int s_extra_count;
static uint64_t s_extra_ns[SWITCH_PERF_EXTRA_SLOTS];
static uint64_t s_slow_wall_us = 20000, s_slow_gpu_us = 20000;
static uint64_t s_slow_begin;
static uint64_t s_slow_ticks[SWITCH_PERF_COUNTER_COUNT];
static uint64_t s_slow_extra[SWITCH_PERF_EXTRA_SLOTS];
static uint64_t s_slow_idle, s_slow_wrk;
static uint64_t s_slow_logged;
static int s_wrk_name_count;
static uint64_t s_wrk_seq[3];

// Top-level phases of R_GenerateSortedDrawSurfs: scopes that sit directly in
// that function and never nest in one another.  resid= is SCENE_TOTAL minus
// their main-thread sum.  A counter recorded inside one of these (portalwalk,
// cellstatic, smc_skin, dobjcull, xskin, ...) must not be listed here.
static const int s_scene_top_level[] = {
    SWITCH_PERF_SCENE_SETUP, SWITCH_PERF_SCENE_FILTERENTS, SWITCH_PERF_SCENE_DPVS,
    SWITCH_PERF_SCENE_SUNSETUP, SWITCH_PERF_SCENE_BSP_CAMERA, SWITCH_PERF_SCENE_BSP_EMISSIVE,
    SWITCH_PERF_SCENE_BSP_SUNSHADOW, SWITCH_PERF_SCENE_SMODEL_CAMERA, SWITCH_PERF_SCENE_SMODEL_SORT,
    SWITCH_PERF_SCENE_SMODEL_SUNSHADOW, SWITCH_PERF_SCENE_SCENEENT, SWITCH_PERF_SCENE_DYNENT,
    SWITCH_PERF_SCENE_FX, SWITCH_PERF_SCENE_SHADOWEMIT, SWITCH_PERF_SCENE_MERGE, SWITCH_PERF_SCENE_SORT,
    SWITCH_PERF_SCENE_SMODEL_LIGHT, SWITCH_PERF_SCENE_SCENEENT_SUNSHADOW, SWITCH_PERF_SCENE_LIGHTS,
    SWITCH_PERF_SCENE_SOUND, SWITCH_PERF_SCENE_FX_PHYSICS, SWITCH_PERF_SCENE_MARKS,
    SWITCH_PERF_SCENE_PRETESS, SWITCH_PERF_SCENE_WAIT, SWITCH_PERF_SCENE_MISC,
};

static double sp_scene_resid_us(uint64_t frames)
{
    uint64_t children = 0;
    for (size_t i = 0; i < sizeof(s_scene_top_level) / sizeof(s_scene_top_level[0]); ++i)
        children += SwitchPerf_g_ticks[s_scene_top_level[i]];
    const double total = (double)SwitchPerf_TicksToUs(SwitchPerf_g_ticks[SWITCH_PERF_SCENE_TOTAL]);
    return (total - (double)SwitchPerf_TicksToUs(children)) / (double)(frames ? frames : 1);
}

static void sp_wrk_snapshot(void)
{
    for (int kind = 0; kind < 2; ++kind)
    {
        for (int type = 0; type < SWITCH_PERF_WRKCMD_SLOTS; ++type)
        {
            s_wrkw_ticks[kind][type] = __atomic_exchange_n(&s_wrk_ticks[kind][type], 0, __ATOMIC_RELAXED);
            s_wrkw_count[kind][type] = __atomic_exchange_n(&s_wrk_count[kind][type], 0, __ATOMIC_RELAXED);
        }
        s_wrkw_idle_ticks[kind] = __atomic_exchange_n(&s_wrk_idle_ticks[kind], 0, __ATOMIC_RELAXED);
        s_wrkw_idle_count[kind] = __atomic_exchange_n(&s_wrk_idle_count[kind], 0, __ATOMIC_RELAXED);
    }
    for (int slot = 0; slot < SWITCH_PERF_WRKWAIT_SLOTS; ++slot)
    {
        s_wrkw_wait_ticks[slot] = __atomic_exchange_n(&s_wrk_wait_ticks[slot], 0, __ATOMIC_RELAXED);
        s_wrkw_wait_count[slot] = __atomic_exchange_n(&s_wrk_wait_count[slot], 0, __ATOMIC_RELAXED);
    }
}

static const char *sp_wrk_name(int type, char *scratch, size_t size)
{
    if (type < s_wrk_name_count && s_wrk_names[type])
        return s_wrk_names[type];
    snprintf(scratch, size, "c%d", type);
    return scratch;
}

// Highest command type that has a name or saw any activity, plus one.
static int sp_wrk_types(void)
{
    int types = s_wrk_name_count;
    for (int type = SWITCH_PERF_WRKCMD_SLOTS - 1; type >= types; --type)
    {
        if (s_wrkw_count[0][type] || s_wrkw_count[1][type] || s_wrkw_wait_count[type])
        {
            types = type + 1;
            break;
        }
    }
    return types;
}

static void sp_append(char *line, size_t size, int *off, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void sp_append(char *line, size_t size, int *off, const char *fmt, ...)
{
    if (*off < 0 || *off >= (int)size - 1)
        return;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(line + *off, size - (size_t)*off, fmt, args);
    va_end(args);
    if (n > 0)
        *off += (*off + n < (int)size) ? n : ((int)size - 1 - *off);
}

static void sp_wrk_report(uint64_t frames)
{
    if (!s_print)
        return;
    const double f = (double)(frames ? frames : 1);
    const int types = sp_wrk_types();
    char scratch[16];
    char line[1024];
    int off = snprintf(line, sizeof(line), "SWITCH_PERF wrkcmd");
    sp_append(line, sizeof(line), &off, " idle.m=%.0f idle.w=%.0f",
              (double)SwitchPerf_TicksToUs(s_wrkw_idle_ticks[0]) / f, (double)SwitchPerf_TicksToUs(s_wrkw_idle_ticks[1]) / f);
    for (int type = 0; type < types; ++type)
    {
        const char *name = sp_wrk_name(type, scratch, sizeof(scratch));
        sp_append(line, sizeof(line), &off, " m.%s=%.0f w.%s=%.0f", name,
                  (double)SwitchPerf_TicksToUs(s_wrkw_ticks[0][type]) / f, name,
                  (double)SwitchPerf_TicksToUs(s_wrkw_ticks[1][type]) / f);
    }
    s_print(line);

    off = snprintf(line, sizeof(line), "SWITCH_PERF wrkwait front=%.0f all=%.0f",
                   (double)SwitchPerf_TicksToUs(s_wrkw_wait_ticks[SWITCH_PERF_WRKWAIT_FRONT]) / f,
                   (double)SwitchPerf_TicksToUs(s_wrkw_wait_ticks[SWITCH_PERF_WRKWAIT_ALL]) / f);
    for (int type = 0; type < types; ++type)
        sp_append(line, sizeof(line), &off, " %s=%.0f", sp_wrk_name(type, scratch, sizeof(scratch)),
                  (double)SwitchPerf_TicksToUs(s_wrkw_wait_ticks[type]) / f);
    s_print(line);

    off = snprintf(line, sizeof(line), "SWITCH_PERF wrkcount idle.m=%.1f idle.w=%.1f wait.front=%.1f wait.all=%.1f",
                   (double)s_wrkw_idle_count[0] / f, (double)s_wrkw_idle_count[1] / f,
                   (double)s_wrkw_wait_count[SWITCH_PERF_WRKWAIT_FRONT] / f,
                   (double)s_wrkw_wait_count[SWITCH_PERF_WRKWAIT_ALL] / f);
    for (int type = 0; type < types; ++type)
    {
        const char *name = sp_wrk_name(type, scratch, sizeof(scratch));
        sp_append(line, sizeof(line), &off, " m.%s=%.1f w.%s=%.1f wait.%s=%.1f", name,
                  (double)s_wrkw_count[0][type] / f, name, (double)s_wrkw_count[1][type] / f, name,
                  (double)s_wrkw_wait_count[type] / f);
    }
    s_print(line);
}



#if defined(__SWITCH__)
static void sp_default_print(const char *line)
{
    // Port_Log writes exact bytes, no added newline; report lines don't
    // carry their own (they used to rely on Com_Printf's "%s\n" format).
    char buf[1040];
    snprintf(buf, sizeof(buf), "%s\n", line);
    Port_Log(buf);
}
#endif

static const char *sp_group_name(int group)
{
    switch (group)
    {
    case G_FRAME:  return "frame";
    case G_RENDER: return "render";
    case G_ISSUE:  return "issue";
    case G_SCENE:  return "scene";
    case G_CGAME:  return "cgame";
    case G_OTHER:  return "other";
    case G_ENTS:   return "ents";
    case G_SNAP:   return "snap";
    case G_GAME:   return "game";
    case G_BACKEND: return "backend";
    default:       return "?";
    }
}

static void sp_snapshot_busy()
{
    for (int worker = 0; worker < SWITCH_PERF_WORKER_COUNT; ++worker)
        s_worker_window_ticks[worker] = __atomic_exchange_n(&s_worker_ticks[worker], 0, __ATOMIC_RELAXED);
    s_server_window_ticks = __atomic_exchange_n(&s_server_ticks, 0, __ATOMIC_RELAXED);
    s_server_window_frames = __atomic_exchange_n(&s_server_frames, 0, __ATOMIC_RELAXED);
    for (int event = 0; event < SWITCH_PERF_EVENT_COUNT; ++event)
        s_events_window[event] = __atomic_exchange_n(&s_events[event], 0, __ATOMIC_RELAXED);
    s_pretess_used_peak_window = __atomic_exchange_n(&s_pretess_used_peak, 0, __ATOMIC_RELAXED);
}

static void sp_reset_window(uint64_t boundary = 0, bool snapshot = true)
{
    memset(SwitchPerf_g_ticks, 0, sizeof(SwitchPerf_g_ticks));
    for (int counter = 0; counter < SWITCH_PERF_COUNTER_COUNT; ++counter)
    {
        s_worker_counter_window[counter] = 0;
        s_backend_counter_window[counter] = 0;
    }
    if (snapshot) { sp_snapshot_busy(); sp_wrk_snapshot(); }
    s_frames = 0;
    s_slow_logged = 0;
    s_sample_count = 0;
    s_sample_dropped = 0;
    s_window_first = 0;
    s_window_start_ticks = boundary ? boundary : sp_now();
}

void SwitchPerf_Init(void)
{
    if (s_inited)
        return;
#if defined(__SWITCH__)
    s_print = sp_default_print;
#endif
    sp_reset_window();
    s_inited = 1;
}

void SwitchPerf_SetPrintSink(SwitchPerfPrintFn fn)
{
    s_print = fn;
}

void SwitchPerf_PrintStage(const char *name, uint64_t ticks)
{
    char line[96];
    snprintf(line, sizeof(line), "SWITCH_PERF stage %s %llu us", name, (unsigned long long)SwitchPerf_TicksToUs(ticks));
    if (s_print)
        s_print(line);
}

void SwitchPerf_SetEnabled(int enabled)
{
    const int want = enabled ? 1 : 0;
    if (want != SwitchPerf_g_enabled) {
        for (int counter = 0; counter < SWITCH_PERF_COUNTER_COUNT; ++counter) {
            __atomic_exchange_n(&s_worker_counter_ticks[counter], 0, __ATOMIC_RELAXED);
            __atomic_exchange_n(&s_backend_counter_ticks[counter], 0, __ATOMIC_RELAXED);
        }
        sp_reset_window();
        s_begin_tick = s_prev_begin = 0;
    }
    SwitchPerf_g_enabled = want;
}

void SwitchPerf_SetStructured(int enabled)
{
    const int want = enabled ? 1 : 0;
    if (want != s_structured) {
        s_sample_count = 0; s_sample_dropped = 0;
        s_begin_tick = s_prev_begin = 0;
    }
    s_structured = want;
}

int SwitchPerf_Enabled(void)
{
    return SwitchPerf_g_enabled;
}

uint64_t SwitchPerf_NowTicks(void)
{
    return sp_now();
}

uint64_t SwitchPerf_TicksToUs(uint64_t ticks)
{
    const uint64_t freq = sp_freq();
    return freq ? (ticks / freq) * 1000000ull + ((ticks % freq) * 1000000ull) / freq : 0ull;
}

double SwitchPerf_TicksToMs(uint64_t ticks)
{
    const uint64_t freq = sp_freq();
    return freq ? ((double)ticks * 1000.0) / (double)freq : 0.0;
}

void SwitchPerf_AddTicks(int counter, uint64_t ticks)
{
    if (counter >= 0 && counter < SWITCH_PERF_COUNTER_COUNT)
        SwitchPerf_g_ticks[counter] += ticks;
}

void SwitchPerf_AddThreadTicks(int counter, uint64_t ticks)
{
    if (counter < 0 || counter >= SWITCH_PERF_COUNTER_COUNT)
        return;
    if (SwitchPerf_t_workerThread)
        __atomic_fetch_add(&s_worker_counter_ticks[counter], ticks, __ATOMIC_RELAXED);
    else
        SwitchPerf_g_ticks[counter] += ticks;
}

void SwitchPerf_MarkWorkerThread(void)
{
    SwitchPerf_t_workerThread = 1;
}

void SwitchPerf_MarkBackendThread(void)
{
    SwitchPerf_t_workerThread = 2;
}

void SwitchPerf_AddBackendTicks(int counter, uint64_t ticks)
{
    if (counter >= 0 && counter < SWITCH_PERF_COUNTER_COUNT)
        __atomic_fetch_add(&s_backend_counter_ticks[counter], ticks, __ATOMIC_RELAXED);
}

uint64_t SwitchPerf_WindowBackendTicks(int counter)
{
    if (counter < 0 || counter >= SWITCH_PERF_COUNTER_COUNT)
        return 0;
    return __atomic_load_n(&s_backend_counter_ticks[counter], __ATOMIC_RELAXED);
}

void SwitchPerf_AddWorkerTicks(int worker, uint64_t ticks)
{
    if (worker >= 0 && worker < SWITCH_PERF_WORKER_COUNT)
        __atomic_fetch_add(&s_worker_ticks[worker], ticks, __ATOMIC_RELAXED);
}

void SwitchPerf_AddServerThreadTicks(uint64_t ticks, int frames)
{
    __atomic_fetch_add(&s_server_ticks, ticks, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_server_frames, (uint64_t)(frames > 0 ? frames : 0), __ATOMIC_RELAXED);
}

void SwitchPerf_AddEvent(int event, uint64_t count)
{
    if (event >= 0 && event < SWITCH_PERF_EVENT_COUNT)
        __atomic_fetch_add(&s_events[event], count, __ATOMIC_RELAXED);
}

int SwitchPerf_RegisterFrameExtra(const char *name)
{
    static int s_lock;
    while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE))
    {
    }
    int slot = -1;
    const int count = __atomic_load_n(&s_extra_count, __ATOMIC_RELAXED);
    for (int i = 0; i < count; ++i)
        if (s_extra_names[i] == name || (name && s_extra_names[i] && !strcmp(s_extra_names[i], name)))
            slot = i;
    if (slot < 0 && name && count < SWITCH_PERF_EXTRA_SLOTS)
    {
        slot = count;
        s_extra_names[slot] = name;
        __atomic_store_n(&s_extra_count, count + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE);
    return slot;
}

void SwitchPerf_AddFrameExtra(int slot, uint64_t ns)
{
    if (slot >= 0 && slot < SWITCH_PERF_EXTRA_SLOTS)
        __atomic_fetch_add(&s_extra_ns[slot], ns, __ATOMIC_RELAXED);
}

void SwitchPerf_SetSlowFrameThresholds(uint64_t wallUs, uint64_t gpuUs)
{
    s_slow_wall_us = wallUs;
    s_slow_gpu_us = gpuUs;
}

void SwitchPerf_SetSlowFrameMs(int ms)
{
    const uint64_t us = ms > 0 ? (uint64_t)ms * 1000u : 0;
    SwitchPerf_SetSlowFrameThresholds(us, us);
}

static uint64_t sp_wrk_main_ticks(void)
{
    uint64_t sum = 0;
    for (int type = 0; type < SWITCH_PERF_WRKCMD_SLOTS; ++type)
        sum += __atomic_load_n(&s_wrk_ticks[0][type], __ATOMIC_RELAXED);
    return sum;
}

static void sp_slow_begin(void)
{
    memcpy(s_slow_ticks, SwitchPerf_g_ticks, sizeof(s_slow_ticks));
    for (int i = 0; i < SWITCH_PERF_EXTRA_SLOTS; ++i)
        s_slow_extra[i] = __atomic_load_n(&s_extra_ns[i], __ATOMIC_RELAXED);
    s_slow_idle = __atomic_load_n(&s_wrk_idle_ticks[0], __ATOMIC_RELAXED);
    s_slow_wrk = sp_wrk_main_ticks();
    s_slow_begin = sp_now();
}

static uint64_t sp_delta(uint64_t now, uint64_t then) { return now >= then ? now - then : now; }

// Called at EndFrame before the window can roll, so the frame's deltas are
// against the snapshot sp_slow_begin took at its BeginFrame.
static void sp_slow_check(uint64_t end)
{
    if (!s_slow_begin)
        return;
    const uint64_t wallUs = SwitchPerf_TicksToUs(end - s_slow_begin);
    s_slow_begin = 0;
    int gpuSlot = -1;
    for (int i = 0; i < s_extra_count; ++i)
        if (s_extra_names[i] && !strcmp(s_extra_names[i], "gpu"))
            gpuSlot = i;
    const uint64_t gpuUs = gpuSlot >= 0
        ? sp_delta(__atomic_load_n(&s_extra_ns[gpuSlot], __ATOMIC_RELAXED), s_slow_extra[gpuSlot]) / 1000 : 0;
    const bool wallHit = s_slow_wall_us && wallUs >= s_slow_wall_us;
    const bool gpuHit = s_slow_gpu_us && gpuUs >= s_slow_gpu_us;
    if (!wallHit && !gpuHit)
        return;
    SwitchPerf_AddEvent(SWITCH_PERF_EV_SLOW_FRAMES, 1);
    if (s_slow_logged >= 4 || !s_print)
    {
        SwitchPerf_AddEvent(SWITCH_PERF_EV_SLOW_FRAMES_UNLOGGED, 1);
        return;
    }
    ++s_slow_logged;

    // Top main-thread counters by this frame's delta.
    static const char *const kNames[SWITCH_PERF_COUNTER_COUNT] = {
#define SP_NAME(id, name, grp) name,
        SWITCH_PERF_COUNTERS(SP_NAME)
#undef SP_NAME
    };
    static const int kGroups[SWITCH_PERF_COUNTER_COUNT] = {
#define SP_GROUP(id, name, grp) grp,
        SWITCH_PERF_COUNTERS(SP_GROUP)
#undef SP_GROUP
    };
    uint64_t delta[SWITCH_PERF_COUNTER_COUNT];
    for (int c = 0; c < SWITCH_PERF_COUNTER_COUNT; ++c)
        delta[c] = sp_delta(SwitchPerf_g_ticks[c], s_slow_ticks[c]);
    uint64_t children = 0;
    for (size_t i = 0; i < sizeof(s_scene_top_level) / sizeof(s_scene_top_level[0]); ++i)
        children += delta[s_scene_top_level[i]];
    const double resid = (double)SwitchPerf_TicksToUs(delta[SWITCH_PERF_SCENE_TOTAL]) - (double)SwitchPerf_TicksToUs(children);

    char line[1024];
    int off = snprintf(line, sizeof(line), "SWITCH_PERF slowframe frame=%llu trigger=%s wall=%llu idle.m=%llu wrk.m=%llu resid=%.0f",
                       (unsigned long long)s_frame_id, wallHit ? "wall" : "gpu", (unsigned long long)wallUs,
                       (unsigned long long)SwitchPerf_TicksToUs(sp_delta(__atomic_load_n(&s_wrk_idle_ticks[0], __ATOMIC_RELAXED), s_slow_idle)),
                       (unsigned long long)SwitchPerf_TicksToUs(sp_delta(sp_wrk_main_ticks(), s_slow_wrk)), resid);
    bool used[SWITCH_PERF_COUNTER_COUNT] = {};
    used[SWITCH_PERF_FRAME_TOTAL] = true;
    for (int n = 0; n < 10; ++n)
    {
        int best = -1;
        for (int c = 0; c < SWITCH_PERF_COUNTER_COUNT; ++c)
            if (!used[c] && delta[c] && (best < 0 || delta[c] > delta[best]))
                best = c;
        if (best < 0)
            break;
        used[best] = true;
        sp_append(line, sizeof(line), &off, " %s.%s=%llu", sp_group_name(kGroups[best]), kNames[best],
                  (unsigned long long)SwitchPerf_TicksToUs(delta[best]));
    }
    uint64_t extra[SWITCH_PERF_EXTRA_SLOTS];
    bool extraUsed[SWITCH_PERF_EXTRA_SLOTS] = {};
    const int extras = __atomic_load_n(&s_extra_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < extras; ++i)
        extra[i] = sp_delta(__atomic_load_n(&s_extra_ns[i], __ATOMIC_RELAXED), s_slow_extra[i]);
    for (int n = 0; n < 8; ++n)
    {
        int best = -1;
        for (int i = 0; i < extras; ++i)
            if (!extraUsed[i] && extra[i] >= 1000 && (best < 0 || extra[i] > extra[best]))
                best = i;
        if (best < 0)
            break;
        extraUsed[best] = true;
        sp_append(line, sizeof(line), &off, " x.%s=%llu", s_extra_names[best], (unsigned long long)(extra[best] / 1000));
    }
    s_print(line);
}

void SwitchPerf_SetWorkerCmdNames(const char *const *names, int count)
{
    if (count > SWITCH_PERF_WRKCMD_SLOTS)
        count = SWITCH_PERF_WRKCMD_SLOTS;
    for (int i = 0; i < count; ++i)
        s_wrk_names[i] = names ? names[i] : 0;
    s_wrk_name_count = count > 0 ? count : 0;
}

void SwitchPerf_AddWorkerCmd(int type, int worker, uint64_t ticks)
{
    if (type < 0 || type >= SWITCH_PERF_WRKCMD_SLOTS)
        return;
    const int kind = worker ? 1 : 0;
    __atomic_fetch_add(&s_wrk_ticks[kind][type], ticks, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_wrk_count[kind][type], 1, __ATOMIC_RELAXED);
}

void SwitchPerf_AddWorkerCmdIdle(int worker, uint64_t ticks)
{
    const int kind = worker ? 1 : 0;
    __atomic_fetch_add(&s_wrk_idle_ticks[kind], ticks, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_wrk_idle_count[kind], 1, __ATOMIC_RELAXED);
}

void SwitchPerf_AddWorkerCmdWait(int slot, uint64_t ticks)
{
    if (slot < 0 || slot >= SWITCH_PERF_WRKWAIT_SLOTS)
        return;
    __atomic_fetch_add(&s_wrk_wait_ticks[slot], ticks, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_wrk_wait_count[slot], 1, __ATOMIC_RELAXED);
}

uint64_t SwitchPerf_WindowWorkerCmdTicks(int type, int worker)
{
    if (type < 0 || type >= SWITCH_PERF_WRKCMD_SLOTS)
        return 0;
    return __atomic_load_n(&s_wrk_ticks[worker ? 1 : 0][type], __ATOMIC_RELAXED);
}

uint64_t SwitchPerf_WindowWorkerCmdCount(int type, int worker)
{
    if (type < 0 || type >= SWITCH_PERF_WRKCMD_SLOTS)
        return 0;
    return __atomic_load_n(&s_wrk_count[worker ? 1 : 0][type], __ATOMIC_RELAXED);
}

uint64_t SwitchPerf_WindowWorkerCmdIdleTicks(int worker)
{
    return __atomic_load_n(&s_wrk_idle_ticks[worker ? 1 : 0], __ATOMIC_RELAXED);
}

uint64_t SwitchPerf_WindowWorkerCmdWaitTicks(int slot)
{
    if (slot < 0 || slot >= SWITCH_PERF_WRKWAIT_SLOTS)
        return 0;
    return __atomic_load_n(&s_wrk_wait_ticks[slot], __ATOMIC_RELAXED);
}

void SwitchPerf_NotePreTessUsed(uint64_t used, uint64_t capacity)
{
    uint64_t peak = __atomic_load_n(&s_pretess_used_peak, __ATOMIC_RELAXED);
    while (used > peak &&
           !__atomic_compare_exchange_n(&s_pretess_used_peak, &peak, used, true, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
    {
    }
    __atomic_store_n(&s_pretess_cap, capacity, __ATOMIC_RELAXED);
}

uint64_t SwitchPerf_WindowWorkerTicks(int worker)
{
    if (worker < 0 || worker >= SWITCH_PERF_WORKER_COUNT)
        return 0;
    return __atomic_load_n(&s_worker_ticks[worker], __ATOMIC_RELAXED);
}

uint64_t SwitchPerf_WindowFrames(void)
{
    return s_frames;
}

uint64_t SwitchPerf_WindowTicks(int counter)
{
    if (counter < 0 || counter >= SWITCH_PERF_COUNTER_COUNT)
        return 0;
    return SwitchPerf_g_ticks[counter];
}

static void sp_structured_report(uint64_t end, uint64_t frames)
{
    if (!s_structured || !s_print || !s_frames) return;
    for (int group = 0; group < G_COUNT; ++group) {
        for (int owner = 0; owner < 3; ++owner) {
            if (group == G_BACKEND && owner == 0) continue;
            if (group == G_BACKEND && owner != 2) continue;
            // Never-active asynchronous groups carry no useful timeline. Once
            // active, zero windows remain visible so stale activity clears.
            if (owner != 0 && group != G_BACKEND) {
                bool active = false;
#define SP_GROUP_ACTIVE(id, name, grp) \
                if ((grp) == group && (owner == 1 ? s_worker_counter_window[SWITCH_PERF_##id] : s_backend_counter_window[SWITCH_PERF_##id])) active = true;
                SWITCH_PERF_COUNTERS(SP_GROUP_ACTIVE)
#undef SP_GROUP_ACTIVE
                s_group_seen[group][owner] = s_group_seen[group][owner] || active;
                if (!s_group_seen[group][owner]) continue;
            }
            char stream[96];
            snprintf(stream, sizeof(stream), "cpu.%s.%s", owner == 0 ? "main" : owner == 1 ? "worker" : "backend", sp_group_name(group));
            kisakperf::Json j;
            kisakperf::Window(j, stream, ++s_record_seq[group][owner], SwitchPerf_TicksToUs(s_window_start_ticks),
                SwitchPerf_TicksToUs(end), frames, s_window_first, s_frame_id,
                owner == 0 ? "main_window" : "asynchronous_main_denominator");
            bool comma = false;
#define SP_NATIVE(id, name, grp) \
            if ((grp) == group) { \
                uint64_t ticks = owner == 0 ? SwitchPerf_g_ticks[SWITCH_PERF_##id] : owner == 1 ? s_worker_counter_window[SWITCH_PERF_##id] : s_backend_counter_window[SWITCH_PERF_##id]; \
                j.Raw("%s\"%s\":%.3f", comma ? "," : "", name, (double)SwitchPerf_TicksToUs(ticks) / frames); comma = true; \
            }
            SWITCH_PERF_COUNTERS(SP_NATIVE)
#undef SP_NATIVE
            if (group == G_SCENE && owner == 0)
                j.Raw(",\"resid\":%.3f", sp_scene_resid_us(frames));
            j.Raw("}}"); s_print(j.Line());
        }
    }
    // Worker-command service and wait time (SwitchPerf_AddWorkerCmd).
    for (int kind = 0; kind < 3; ++kind) {
        static const char *const streams[3] = {"cpu.wrkcmd.main", "cpu.wrkcmd.worker", "cpu.wrkwait"};
        kisakperf::Json j;
        kisakperf::Window(j, streams[kind], ++s_wrk_seq[kind], SwitchPerf_TicksToUs(s_window_start_ticks),
            SwitchPerf_TicksToUs(end), frames, s_window_first, s_frame_id,
            kind == 0 ? "main_window" : "asynchronous_main_denominator");
        char scratch[16];
        if (kind < 2)
            j.Raw("\"idle\":%.3f", (double)SwitchPerf_TicksToUs(s_wrkw_idle_ticks[kind]) / frames);
        else
            j.Raw("\"front\":%.3f,\"all\":%.3f", (double)SwitchPerf_TicksToUs(s_wrkw_wait_ticks[SWITCH_PERF_WRKWAIT_FRONT]) / frames,
                (double)SwitchPerf_TicksToUs(s_wrkw_wait_ticks[SWITCH_PERF_WRKWAIT_ALL]) / frames);
        const int types = sp_wrk_types();
        for (int type = 0; type < types; ++type) {
            const uint64_t ticks = kind < 2 ? s_wrkw_ticks[kind][type] : s_wrkw_wait_ticks[type];
            j.Raw(",");
            j.String(sp_wrk_name(type, scratch, sizeof(scratch)));
            j.Raw(":%.3f", (double)SwitchPerf_TicksToUs(ticks) / frames);
        }
        j.Raw("}}"); s_print(j.Line());
    }
    {
        kisakperf::Json j;
        kisakperf::Window(j, "cpu.worker_busy", ++s_busy_seq, SwitchPerf_TicksToUs(s_window_start_ticks),
            SwitchPerf_TicksToUs(end), frames, s_window_first, s_frame_id, "asynchronous_main_denominator");
        for (int worker = 0; worker < SWITCH_PERF_WORKER_COUNT; ++worker)
            j.Raw("%s\"w%d\":%.3f", worker ? "," : "", worker, (double)SwitchPerf_TicksToUs(s_worker_window_ticks[worker]) / frames);
        j.Raw(",\"server\":%.3f}}", (double)SwitchPerf_TicksToUs(s_server_window_ticks) / frames);
        s_print(j.Line());
    }
    for (int kind = 0; kind < 2; ++kind) {
        unsigned cursor = 0;
        while (cursor < s_sample_count) {
            kisakperf::Json j;
            kisakperf::Samples(j, kind ? "cpu.frame.period" : "cpu.frame.wall", kind ? "period" : "wall",
                ++s_sample_seq[kind], "\"frame\",\"time\",\"value\"");
            unsigned emitted = 0;
            while (cursor < s_sample_count && emitted < 8) {
                const FrameSample &v = s_samples[cursor++];
                if (kind && !v.hasPeriod) continue;
                j.Raw("%s[%llu,%llu,%llu]", emitted ? "," : "", (unsigned long long)v.frame,
                    (unsigned long long)(kind ? v.begin : v.time), (unsigned long long)(kind ? v.period : v.wall));
                ++emitted;
            }
            if (emitted) { j.Raw("],\"tags\":{\"frame_domain\":\"cpu\",\"timing\":\"elapsed_inclusive\"}}"); s_print(j.Line()); }
            else --s_sample_seq[kind];
        }
    }
    if (s_sample_dropped) {
        kisakperf::Json j;
        j.Raw("{\"v\":1,\"type\":\"event\",\"stream\":\"cpu.frame_loss\",\"seq\":%llu,\"clock\":\"monotonic_us\",\"time\":%llu,\"name\":\"sample_overflow\",\"unit\":\"frames\",\"message\":\"Frame sample buffer exceeded capacity\",\"count\":%llu}",
            (unsigned long long)++s_loss_seq, (unsigned long long)SwitchPerf_TicksToUs(end), (unsigned long long)s_sample_dropped);
        s_print(j.Line());
    }
}

void SwitchPerf_ReportNow(void)
{
    if (!s_inited)
        SwitchPerf_Init();

    const uint64_t frames = s_frames ? s_frames : 1;
    const uint64_t report_end = sp_now();
    const uint64_t elapsed_ticks = report_end - s_window_start_ticks;
    const double elapsed_ms = SwitchPerf_TicksToMs(elapsed_ticks);
    const double fps = elapsed_ms > 0.0 ? ((double)frames * 1000.0) / elapsed_ms : 0.0;

    sp_snapshot_busy();
    sp_wrk_snapshot();
    // Snapshot worker category time before printing. Work that races this
    // exchange belongs to the next window, just like worker busy time.
    for (int counter = 0; counter < SWITCH_PERF_COUNTER_COUNT; ++counter)
    {
        s_worker_counter_window[counter] =
            __atomic_exchange_n(&s_worker_counter_ticks[counter], 0, __ATOMIC_RELAXED);
        s_backend_counter_window[counter] =
            __atomic_exchange_n(&s_backend_counter_ticks[counter], 0, __ATOMIC_RELAXED);
    }

    if (s_print)
    {
        // Scopes measure inclusive elapsed time. Nested counters and groups
        // can overlap, so their values must not be summed into CPU execution.
        for (int group = 0; group < G_COUNT; ++group)
        {
            char line[1024];
            int off;
            if (group == G_BACKEND)
            {
                // The back-end thread's own accumulator: its own counters
                // always, plus every other counter it ran (group.name), so
                // the loading-screen front end shows up here, not in main's.
                off = snprintf(line, sizeof(line), "SWITCH_PERF backend");
#define SWITCH_PERF_APPEND_BACKEND(id, name, grp) \
                if (((grp) == G_BACKEND || s_backend_counter_window[SWITCH_PERF_##id]) && off > 0 && off < (int)sizeof(line) - 1) \
                { \
                    const int n = snprintf(line + off, sizeof(line) - (size_t)off, " %s%s%s=%.0f", \
                                           (grp) == G_BACKEND ? "" : sp_group_name(grp), (grp) == G_BACKEND ? "" : ".", (name), \
                                           (double)SwitchPerf_TicksToUs(s_backend_counter_window[SWITCH_PERF_##id]) / (double)frames); \
                    if (n > 0) \
                        off += (off + n < (int)sizeof(line)) ? n : ((int)sizeof(line) - 1 - off); \
                }
                SWITCH_PERF_COUNTERS(SWITCH_PERF_APPEND_BACKEND)
#undef SWITCH_PERF_APPEND_BACKEND
                s_print(line);
                continue;
            }
            if (group == G_FRAME)
            {
                off = snprintf(line, sizeof(line), "SWITCH_PERF frame fps=%.1f frames=%llu elapsed=%.1fms",
                               fps, (unsigned long long)frames, elapsed_ms);
            }
            else
            {
                off = snprintf(line, sizeof(line), "SWITCH_PERF %s", sp_group_name(group));
            }
            if (off < 0)
                continue;
            if (off > (int)sizeof(line) - 1)
                off = (int)sizeof(line) - 1;

#define SWITCH_PERF_APPEND_ENTRY(id, name, grp) \
            if ((grp) == group && off < (int)sizeof(line) - 1) \
            { \
                const int n = snprintf(line + off, sizeof(line) - (size_t)off, " %s=%.0f", \
                                       (name), (double)SwitchPerf_TicksToUs(SwitchPerf_g_ticks[SWITCH_PERF_##id] + s_worker_counter_window[SWITCH_PERF_##id]) / (double)frames); \
                if (n > 0) \
                    off += (off + n < (int)sizeof(line)) ? n : ((int)sizeof(line) - 1 - off); \
            }
            SWITCH_PERF_COUNTERS(SWITCH_PERF_APPEND_ENTRY)
#undef SWITCH_PERF_APPEND_ENTRY
            if (group == G_SCENE)
                sp_append(line, sizeof(line), &off, " resid=%.0f", sp_scene_resid_us(frames));

            s_print(line);
        }
    }

    sp_structured_report(report_end, frames);
    sp_reset_window(report_end, false);
    if (s_print)
    {
        // Busy time each renderer worker spent processing commands in the
        // window just closed (snapshotted before printing), per frame.
        char line[1024];
        int off = snprintf(line, sizeof(line), "SWITCH_PERF worker");
        for (int worker = 0; worker < SWITCH_PERF_WORKER_COUNT && off > 0 && off < (int)sizeof(line); ++worker)
        {
            off += snprintf(line + off, sizeof(line) - (size_t)off, " w%d=%.0f", worker,
                            (double)SwitchPerf_TicksToUs(s_worker_window_ticks[worker]) / (double)frames);
        }
        if (off > 0 && off < (int)sizeof(line))
        {
            const double svUs = (double)SwitchPerf_TicksToUs(s_server_window_ticks);
            snprintf(line + off, sizeof(line) - (size_t)off, " sv=%.0f svframes=%llu svframe=%.0f svstack=%zu",
                     svUs / (double)frames, (unsigned long long)s_server_window_frames,
                     s_server_window_frames ? svUs / (double)s_server_window_frames : 0.0,
                     Switch_ServerStackUsed ? Switch_ServerStackUsed() : (size_t)0);
        }
        s_print(line);

        // Event counts for the window just closed, per displayed frame.
        off = snprintf(line, sizeof(line), "SWITCH_PERF pretess");
#define SWITCH_PERF_EVENT_APPEND(id, name) \
        if (off > 0 && off < (int)sizeof(line)) \
            off += snprintf(line + off, sizeof(line) - (size_t)off, " %s=%.1f", (name), \
                            (double)s_events_window[SWITCH_PERF_EV_##id] / (double)frames);
        SWITCH_PERF_EVENTS(SWITCH_PERF_EVENT_APPEND)
#undef SWITCH_PERF_EVENT_APPEND
        if (off > 0 && off < (int)sizeof(line))
            snprintf(line + off, sizeof(line) - (size_t)off, " used_peak=%llu cap=%llu",
                     (unsigned long long)s_pretess_used_peak_window,
                     (unsigned long long)__atomic_load_n(&s_pretess_cap, __ATOMIC_RELAXED));
        s_print(line);
        sp_wrk_report(frames);
        if (Switch_ClocksReport)
            Switch_ClocksReport();
    }
}

void SwitchPerf_BeginFrame(void)
{
    if (!s_inited) SwitchPerf_Init();
    if (SwitchPerf_g_enabled && s_structured) s_begin_tick = sp_now();
    if (SwitchPerf_g_enabled) sp_slow_begin();
    else s_slow_begin = 0;
}

void SwitchPerf_EndFrame(void)
{
    if (!s_inited)
        SwitchPerf_Init();

    if (!SwitchPerf_g_enabled)
    {
        // Drop any partial window from an enabled period so re-enabling starts
        // clean instead of reporting stale ticks.
        for (int counter = 0; counter < SWITCH_PERF_COUNTER_COUNT; ++counter)
            __atomic_exchange_n(&s_worker_counter_ticks[counter], 0, __ATOMIC_RELAXED);
        if (s_frames || SwitchPerf_g_ticks[SWITCH_PERF_FRAME_TOTAL])
            sp_reset_window();
        return;
    }

    ++s_frames;
    ++s_frame_id;
    if (!s_window_first) s_window_first = s_frame_id;
    const uint64_t end = sp_now();
    sp_slow_check(end);
    if (s_begin_tick) {
        if (s_sample_count < 128) {
            s_samples[s_sample_count++] = {s_frame_id, SwitchPerf_TicksToUs(end), SwitchPerf_TicksToUs(s_begin_tick),
                SwitchPerf_TicksToUs(end - s_begin_tick),
                s_prev_begin ? SwitchPerf_TicksToUs(s_begin_tick - s_prev_begin) : 0, s_prev_begin != 0};
        } else ++s_sample_dropped;
        s_prev_begin = s_begin_tick;
        s_begin_tick = 0;
    }
    if (sp_now() - s_window_start_ticks >= sp_freq())
        SwitchPerf_ReportNow();
}
