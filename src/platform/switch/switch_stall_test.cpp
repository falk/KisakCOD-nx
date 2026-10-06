// TEST ONLY: `+set switch_stallTest 1|2` dry-fires the stall and GPU-fault
// capture path once, 60 s after the dvar is first seen set (so the recorder
// holds gameplay lists), then resets it to 0.
//   1 = the main thread sleeps 4 s: the watchdog's 1 s flight-recorder dump
//       ("FR ..." lines, flightrec.bin) and its 3 s STALL report must reach
//       the log host and the SD log ring while the game is still alive.
//   2 = with r_deko9CmdPoison 1, the next list's first command chunk is
//       poisoned before submit (a chunk retired one list early): the GPU
//       faults on a tag A word and the crash report's shadow word names the
//       chunk the "FR test early_retire" line printed. This kills the game.

#include <switch.h>

#include <cstdint>
#include <cstdio>

#include <deko9/deko9_native.h>

#include "switch_port_log.h"

struct dvar_s;
const dvar_s *Dvar_RegisterInt(const char *dvarName, int value, uint32_t min, uint32_t max, uint32_t flags,
                               const char *description);
int Dvar_GetInt(const char *dvarName);
void Dvar_SetIntByName(const char *dvarName, int value);

namespace
{
constexpr uint64_t kDelayMs = 60000;
constexpr uint64_t kSleepMs = 4000;

uint64_t NowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ull; }
} // namespace

// Main thread, once per frame (from Switch_PerfConfigFrame).
void Switch_StallTestFrame(void)
{
    static bool registered;
    static uint64_t armedAt;
    if (!registered)
    {
        registered = true;
        Dvar_RegisterInt(
            "switch_stallTest", 0, 0, 2, 0,
            "TEST ONLY: 1 = sleep the main thread 4 s once (60 s after it is set) to prove the stall dumps reach "
            "the log host and the SD log ring; 2 = with r_deko9CmdPoison 1, poison one command chunk a list early "
            "so the GPU faults on a tag the flight recorder names (kills the game); never set outside that test");
    }
    const int mode = Dvar_GetInt("switch_stallTest");
    if (!mode)
    {
        armedAt = 0;
        return;
    }
    const uint64_t now = NowMs();
    if (!armedAt)
        armedAt = now;
    if (now - armedAt < kDelayMs)
        return;
    armedAt = 0;
    Dvar_SetIntByName("switch_stallTest", 0);
    char line[128];
    if (mode == 1)
    {
        std::snprintf(line, sizeof(line), "SWITCH_STALL_TEST mode=1 main thread sleeps %llu ms\n",
                      (unsigned long long)kSleepMs);
        Port_LogRaw(line);
        svcSleepThread(kSleepMs * 1000000ull);
        Port_LogRaw("SWITCH_STALL_TEST mode=1 resumed\n");
    }
    else
    {
        Port_LogRaw("SWITCH_STALL_TEST mode=2 early chunk retire armed\n");
        Deko9_FlightRecEarlyRetireTest();
    }
}
