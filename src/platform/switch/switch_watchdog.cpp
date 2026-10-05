#include "switch_watchdog.h"

#include <cstdio>

#include "switch_port_log.h"


#ifdef __SWITCH__
#include <switch.h>

#include <deko9/deko9_native.h>

namespace
{
constexpr uint64_t kStallMs = 3000;
// A zone load legitimately keeps the main thread out of Com_Frame.
constexpr uint64_t kLoadStallMs = 15000;
constexpr uint64_t kPollMs = 250;
const char *const kSlotNames[CRUMB_COUNT] = {"main_frame", "backend",  "worker0",  "worker1",
                                             "sound_mix",  "stream",   "database", "gpu_hb"};

uint64_t NowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ull; }

void Report(const uint32_t *last, const uint64_t *changedAt, uint64_t now)
{
    char line[160];
    std::snprintf(line, sizeof(line), "STALL: main frame stuck %llu ms, thread breadcrumbs follow\n",
                  (unsigned long long)(now - changedAt[CRUMB_MAIN_FRAME]));
    Port_LogRaw(line);
    for (int i = 0; i < CRUMB_COUNT; ++i)
    {
        std::snprintf(line, sizeof(line), "STALL: crumb %s count=%u site=%u idle_ms=%llu\n", kSlotNames[i],
                      last[i], g_crumbs[i].site.load(std::memory_order_relaxed),
                      (unsigned long long)(now - changedAt[i]));
        Port_LogRaw(line);
    }
    Deko9_BlackBoxDump("watchdog");
    Port_LogFlush(500);
}

void WatchdogMain(void *)
{
    StallTrigger trigger(kStallMs);
    uint32_t last[CRUMB_COUNT] = {};
    uint64_t changedAt[CRUMB_COUNT] = {};
    for (;;)
    {
        svcSleepThread(kPollMs * 1000000ull);
        const uint64_t now = NowMs();
        for (int i = 0; i < CRUMB_COUNT; ++i)
        {
            const uint32_t c = g_crumbs[i].count.load(std::memory_order_relaxed);
            if (c != last[i] || changedAt[i] == 0)
            {
                last[i] = c;
                changedAt[i] = now;
            }
        }
        trigger.SetThreshold(g_crumbs[CRUMB_DATABASE].site.load(std::memory_order_relaxed) == 2 ? kLoadStallMs
                                                                                              : kStallMs);
        // Nothing to judge before the first frame (boot and map loads run
        // outside Com_Frame's counter only until it starts ticking).
        if (last[CRUMB_MAIN_FRAME] != 0 && trigger.Sample(last[CRUMB_MAIN_FRAME], now))
            Report(last, changedAt, now);
    }
}

Thread s_thread;
} // namespace

extern "C" void Switch_WatchdogStart(void)
{
    if (R_SUCCEEDED(threadCreate(&s_thread, WatchdogMain, nullptr, nullptr, 0x4000, 0x3B, -2)))
        threadStart(&s_thread);
}
#else
extern "C" void Switch_WatchdogStart(void) {}
#endif
