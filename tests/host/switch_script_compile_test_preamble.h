#pragma once

// Host-only prelude for switch_script_compile_test.cpp and the production
// script TUs it links (the real parser, compiler, VM, variable system, string
// list, and hunk/allocator owners).  Mirrors the narrow shims the other
// script host tests use; no production source depends on this file.
//
// The real libc random()/crandom() declarations must be parsed before
// <database/database.h>'s Win32-compat macro renames them.  switch_retail_ui_compat.h
// (force-included just before this one) already includes <stdlib.h> and
// establishes the rename, so this file only supplies what that prelude does
// not.
#include <cstdio>
#include <cstdlib>

// Win32 scalar spellings the decompiled script sources use that the narrow
// UI/SL compat preludes do not define.
#ifndef __int8
#define __int8 char
#endif

// The original Win32 LPVOID spelling; scr_yacc.h skips <Windows.h> under
// __SWITCH__ (the normal SP build gets this from switch_compat.h).
#ifndef LPVOID_DEFINED_BY_SWITCH_RETAIL_XANIM_SL_HOST
typedef void *LPVOID;
#endif

// switch_compat.h declares the engine's platform Sys entry points.  They are
// resolved by the harness's loud stubs below, never by real platform code:
// the script compile/execute path under test does not touch the Win32 shell.
void Sys_Error(const char *error, ...);
void Sys_OutOfMemErrorInternal(const char *filename, int line);

// switch_compat.h maps the x86 TSC intrinsic onto Horizon's counter.  The
// script VM only samples it for its optional profiling path; the host harness
// has no equivalent and does not exercise it.
#ifndef __rdtsc
#define __rdtsc() 0
#endif
