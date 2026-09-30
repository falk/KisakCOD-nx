#include "switch_perf.h"

#include <stdio.h>
#include <string.h>

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

static void sp_reset_window(void)
{
    memset(SwitchPerf_g_ticks, 0, sizeof(SwitchPerf_g_ticks));
    for (int counter = 0; counter < SWITCH_PERF_COUNTER_COUNT; ++counter)
    {
        s_worker_counter_window[counter] = 0;
        s_backend_counter_window[counter] = 0;
    }
    for (int worker = 0; worker < SWITCH_PERF_WORKER_COUNT; ++worker)
        s_worker_window_ticks[worker] = __atomic_exchange_n(&s_worker_ticks[worker], 0, __ATOMIC_RELAXED);
    s_server_window_ticks = __atomic_exchange_n(&s_server_ticks, 0, __ATOMIC_RELAXED);
    s_server_window_frames = __atomic_exchange_n(&s_server_frames, 0, __ATOMIC_RELAXED);
    for (int event = 0; event < SWITCH_PERF_EVENT_COUNT; ++event)
        s_events_window[event] = __atomic_exchange_n(&s_events[event], 0, __ATOMIC_RELAXED);
    s_pretess_used_peak_window = __atomic_exchange_n(&s_pretess_used_peak, 0, __ATOMIC_RELAXED);
    s_frames = 0;
    s_window_start_ticks = sp_now();
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

void SwitchPerf_SetEnabled(int enabled)
{
    SwitchPerf_g_enabled = enabled ? 1 : 0;
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
    return freq ? (ticks * 1000000ull) / freq : 0ull;
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

void SwitchPerf_ReportNow(void)
{
    if (!s_inited)
        SwitchPerf_Init();

    const uint64_t frames = s_frames ? s_frames : 1;
    const uint64_t elapsed_ticks = sp_now() - s_window_start_ticks;
    const double elapsed_ms = SwitchPerf_TicksToMs(elapsed_ticks);
    const double fps = elapsed_ms > 0.0 ? ((double)frames * 1000.0) / elapsed_ms : 0.0;

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
        // One line per group.  A group total (e.g. FRAME render) includes the
        // child counters that run inside it; groups do not overlap.
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

            s_print(line);
        }
    }

    sp_reset_window();
    if (s_print)
    {
        // Busy time each renderer worker spent processing commands in the
        // window just closed (sp_reset_window swapped it out), per frame.
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
        if (Switch_ClocksReport)
            Switch_ClocksReport();
    }
}

void SwitchPerf_BeginFrame(void)
{
    if (!s_inited)
        SwitchPerf_Init();
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
    if (sp_now() - s_window_start_ticks >= sp_freq())
        SwitchPerf_ReportNow();
}
