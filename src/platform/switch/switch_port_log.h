#pragma once

#include <stdint.h>

// One shared diagnostic fan-out (seam 13): stdout (nxlink) + svcOutputDebugString
// (emulator/debugger guest log), with a line that starts "FAIL:" also
// flushed to an SD file so it survives a dropped nxlink stream or a hang.
// This is deko9_d3d.cpp's original Emit/Fail behavior, unchanged, just
// shared: deko9's own Log/Fail, snd_audren_switch.cpp's Emit,
// Switch_BootLog and switch_perf.cpp's default print sink all route
// through it instead of each reimplementing the same fan-out.
//
// The crash handler (switch_crash.cpp) is not routed through here: it must
// stay a raw, signal-context-safe svcOutputDebugString call, never stdio.
#ifdef __cplusplus
extern "C" {
#endif

// Writes exactly the bytes of `line`, no newline added: Switch_BootLog
// builds one logical line from several consecutive calls, and every other
// caller already carries its own trailing "\n" where it wants one.
void Port_Log(const char *line);
// Same fan-out as Port_Log. A distinct name for call sites reporting a
// failure; the "FAIL:" prefix on the line itself is what actually decides
// the SD-card write; a caller that wants that write and forgets the
// prefix should add it, not rely on the function name alone.
void Port_Fail(const char *line);

// Hardware only (no-ops elsewhere). Start redirects stdout/stderr into the
// in-memory log ring and starts the drain thread that feeds the nxlink
// socket `netFd` (-1 for none; made non-blocking) and the SD ring file.
// Flush asks the drain thread to push everything queued to the card and
// waits at most maxWaitMs. Shutdown is the final bounded flush at exit.
void Port_LogStart(int netFd);
// Straight into the ring (no stdio lock) for the stall watchdog.
void Port_LogRaw(const char *line);
void Port_LogFlush(uint32_t maxWaitMs);
void Port_LogShutdown(void);

#ifdef __cplusplus
}
#endif
