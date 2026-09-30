// excluded-boundary reach reporting for the walk-window frame-evidence
// audit.
//
// These entry points are the excluded runtime paths that could stand in for
// real walk evidence if they were reached. All fake thread
// spawns are retired. The Win32 screenshot command used to be a site here; the
// Switch `screenshot` command is real now (switch_screenshot.cpp over
// RB_SwapBuffers' backbuffer readback). The final fake thread-spawn
// body is retired; this translation unit remains as the owner for any future explicitly
// excluded boundary instead of silently growing such a body elsewhere.
//
// Kept in its own translation unit (not switch_misc_stubs.cpp) so the host
// excluded-boundary proof can link and exercise the real bodies.
#include <universal/q_shared.h>
#include <qcommon/threads.h>
#include <database/db_retail_frame_evidence.h>

#ifdef __SWITCH__

// server/save-history spawns are real in switch_thread_sync.cpp and the
// renderer worker spawn is real in switch_thread.cpp. No thread boundary is
// substituted here anymore.

#endif
