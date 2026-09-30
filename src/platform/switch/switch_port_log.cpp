#include "switch_port_log.h"

#include <cstdio>
#include <cstring>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace
{
// deko9_d3d.cpp's original Emit, generalized to a raw byte passthrough (no
// added newline): Switch_BootLog builds one logical line from several
// consecutive calls, and audren's lines already carry their own trailing
// "\n" in their format strings, so a caller-added newline here would split
// or double them. deko9's own Log/LogLine/Fail add their own before
// calling in (they relied on the old Emit doing it for them).
//
// stdout reaches nxlink on hardware, the debug string reaches emulator
// and debugger logs. FAIL lines also go to the SD card, flushed: they survive a dropped
// nxlink stream or a hang. Routine lines stay off the card.
void PortLogFanOut(const char *line)
{
#ifdef __SWITCH__
    std::fputs(line, stdout);
    std::fflush(stdout);
    svcOutputDebugString(line, std::strlen(line));
    if (std::strncmp(line, "FAIL:", 5) != 0)
        return;
    static FILE *s_file = std::fopen("sdmc:/switch/kisakcod/port_fail.log", "w");
    if (s_file)
    {
        std::fprintf(s_file, "%llu %s\n",
                     (unsigned long long)armTicksToNs(armGetSystemTick()) / 1000000ull, line);
        std::fflush(s_file);
    }
#else
    std::fputs(line, stderr);
    std::fflush(stderr);
#endif
}
} // namespace

void Port_Log(const char *line)
{
    if (line)
        PortLogFanOut(line);
}

void Port_Fail(const char *line)
{
    if (line)
        PortLogFanOut(line);
}
