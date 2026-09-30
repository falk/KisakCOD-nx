// Host-only prelude for switch_excluded_boundaries_test.cpp.
//
// The real Switch build force-includes src/platform/switch/switch_compat.h, which
// supplies the Win32 spelling HWND before sound/snd_public.h declares
// SND_SetHWND.  switch_compat.h itself includes <switch.h> (libnx) and cannot
// be force-included on the host, so this narrow prelude provides the one
// typedef the stub translation units need before their own includes run.
// The vendored d3d9-headers' windows_base.h later repeats the same
// underlying typedef, which is valid C++.
#pragma once

typedef void *HWND;
