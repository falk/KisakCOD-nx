#pragma once
// Host-only shims for the UI expression proof.  The production UI
// expression translation units include Windows/console spellings
// (HWND, _iobuf, _snprintf, _time64, _stricmp, ARRAYSIZE) that the host
// toolchain and the narrow compat prelude do not otherwise provide.
#include <stdio.h>
#include <time.h>
#include <strings.h>

struct HWND__;
struct HINSTANCE__;

typedef void *HWND;
typedef FILE _iobuf;

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static inline time_t _time64(time_t *t)
{
    return time(t);
}

static inline char *_ctime64(const time_t *t)
{
    time_t value = *t;
    return ctime(&value);
}

#define _snprintf snprintf
#define _stricmp strcasecmp
