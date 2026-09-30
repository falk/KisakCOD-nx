// Early platform compatibility for the Horizon (devkitA64/libnx) build.
// Force-included before every Switch SP translation unit (see pre_build.cmake).
// Keep this file valid C as well as C++: it is also force-included into the
// C sources (zlib) that remain in the Switch boot closure.
#pragma once

#ifdef __SWITCH__
#include <switch.h>
// Engine Sys declarations that the Win32 shell used to provide; resolved at
// link by common.cpp (SP) or the verifier's loud implementations (proofs).
void Sys_Error(const char *error, ...);
char *Sys_DefaultInstallPath(void);
void Sys_OutOfMemErrorInternal(const char *filename, int line);
// Single-entry process exit (switch_misc_stubs.cpp): only the first caller
// runs exit(), every later one parks.  newlib's exit() is not reentrant and
// its tail unmounts the filesystem, so a second caller can pull fsdev out
// from under a driver thread the first is still waiting on.
void Switch_ExitOnce(int code) __attribute__((noreturn));
#ifdef __cplusplus
extern "C"
#endif
void Switch_BootLog(const char *msg);
// The engine's cooperative sleep (win_net.h NET_Sleep) rides on the libnx
// scheduler seam.
#define NET_Sleep(msec) svcSleepThread(((uint64_t)(msec)) * 1000000ull)
#include <strings.h>
#ifndef __int64
#define __int64 long long
#endif
#ifndef __int32
#define __int32 int
#endif
#ifndef __int16
#define __int16 short
#endif
#ifndef __int8
#define __int8 char
#endif
typedef unsigned char byte;
#define _stricmp(a, b) strcasecmp((a), (b))
#define _strnicmp(a, b, n) strncasecmp((a), (b), (n))
#define _snprintf snprintf
#include <time.h>
#include <stdio.h>
#define _time64(t) time(t)
#define _localtime64(t) localtime(t)
#define _ctime64(t) ctime(t)
#include <math.h>
#define _isnan(x) isnan(x)

// MSVC secure CRT fopen.  A plain function (not a macro) so header
// declarations are not rewritten.  Callers only branch on the error code.
#ifdef __cplusplus
static inline int fopen_s(FILE **file, const char *name, const char *mode)
{
    if (!file || !name || !mode)
        return 22;
    *file = fopen(name, mode);
    return *file ? 0 : 2;
}
#endif
#define _strdup(str) strdup(str)
#define __debugbreak() __builtin_trap()
// Win32 interlocked compare-exchange: returns the previous value.
#define InterlockedCompareExchange(pointer, value, comparand) \
    __sync_val_compare_and_swap((pointer), (comparand), (value))
// Win32 file deletion used by the database profile/rawfile writers; a
// missing file is not an error for the callers.
#define DeleteFileA(name) remove(name)
#endif

// newlib <stdlib.h> declares the BSD `long random(void)`; CoD4 declares its
// own `float random()`/`crandom()` in com_math.h.  Rename the engine functions
// for this build after libc has been processed (the same collision-resolution
// pattern the POSIX prior art uses).  A stray libc call would surface as a
// loud undefined-symbol link error, never a silent substitution.
#include <stdlib.h>
#include <stdio.h>
#define random switch_random
#define crandom switch_crandom

// MSVC's stdio spells FILE's struct tag `_iobuf`; newlib spells it `__sFILE`.
// The decompiled signatures reference the tag directly.
#define _iobuf __sFILE

// Minimal windef scalar subset.  Widths match the vendored d3d9-headers
// (32-bit LONG/DWORD) and snd_public.h's fallback so all three can coexist.
#ifndef _WINDEF_
typedef int BOOL;
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef unsigned char BYTE;
typedef unsigned int UINT;
typedef void *HWND;
typedef void *HINSTANCE;
typedef void *HMODULE;
typedef void *LPVOID;
#endif

// The vendored d3d9-headers own the win32 opaque-handle namespace on
// Horizon: HWND/HINSTANCE/HMODULE are void*.  The engine declares members as
// `HWND__ *hwnd` / `HINSTANCE__ *hinst`, so widen the tags to the same
// opaque pointer.
typedef void HWND__;
typedef void HINSTANCE__;

#ifdef __cplusplus
#include <cstdio>
#endif

// Win32 file-API declarations the database zone loader still names. Only
// CreateFileA/WriteFile/CloseHandle are defined on Horizon, as fail-loud
// stubs in switch_misc_stubs.cpp (DB_EndReorderZone's reorder dump).
#ifdef __cplusplus
extern "C" {
#endif
void *CreateFileA(const char *name, unsigned int desiredAccess, unsigned int shareMode,
                  void *securityAttributes, unsigned int creationDisposition,
                  unsigned int flagsAndAttributes, void *templateFile);
unsigned int GetFileSize(void *handle, unsigned int *highDWORD);
int CloseHandle(void *handle);
int ReadFile(void *handle, void *buffer, unsigned int bytesToRead,
             unsigned int *bytesRead, void *overlapped);
int WriteFile(void *handle, const void *buffer, unsigned int bytesToWrite,
              unsigned int *bytesWritten, void *overlapped);
#ifdef __cplusplus
}
#endif
#define _strdup(str) strdup(str)

#ifdef __cplusplus
// The engine's build/version string macro is only defined for WIN32 in
// q_shared.h; Horizon is an LP64 AArch64 target with its own identity.
#ifndef CPUSTRING
#define CPUSTRING "switch-aarch64"
#endif

// MSVC force-inline hint used by the decompiled sources.
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

// The vendored d3d9-headers name the RECT struct tag only; the engine
// spells the type as the bare tag.
// The engine spells Windows' RECT as tagRECT in a few places; the vendored
// windows_base.h (included later by the actual D3D9 headers) already defines
// the real `struct RECT`.  Alias by macro rather than declaring a second,
// layout-identical-but-distinct tagRECT type, which would not implicitly
// convert to the `const RECT*` the D3D9 API expects.
#define tagRECT RECT

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

// Win32 interlocked primitive used by the renderer front-end; AArch64 owns
// it through GCC atomics with the same acquire/release-free SEQ_CST semantics
// the Interlocked family provides.
#define InterlockedExchangeAdd(ptr, val) __atomic_fetch_add((ptr), (val), __ATOMIC_SEQ_CST)
#define InterlockedExchange(ptr, val) __atomic_exchange_n((ptr), (val), __ATOMIC_SEQ_CST)
#define InterlockedIncrement(ptr) __atomic_add_fetch((ptr), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(ptr) __atomic_sub_fetch((ptr), 1, __ATOMIC_SEQ_CST)

// MSVC CRT integer-to-string helper.  Only decimal use exists in the engine
// closure; other radixes fail loudly instead of emitting wrong digits.
static inline char *switch_itoa(int value, char *dest, int radix)
{
    if (radix != 10)
    {
        dest[0] = 0;
        return dest;
    }
    std::snprintf(dest, 33, "%d", value);
    return dest;
}
#define _itoa(value, dest, radix) switch_itoa((value), (dest), (radix))

// Profile timestamps on AArch64 come from Horizon's supported physical
// counter path; reading cntvct_el0 directly raises a data abort on hardware.
// The x86 TSC intrinsic does not exist here. Same monotonic-role use,
// different clock.
static inline uint64_t switch_rdtsc()
{
    return armGetSystemTick();
}
#define __rdtsc() switch_rdtsc()

// MSVC's _BitScanReverse(&index, mask): returns 0 if mask is zero, otherwise
// sets index to the bit position of the most significant set bit and returns
// nonzero.  MSVC's "unsigned long" here is always 32-bit, but this engine's
// decompiled callers spell the index/mask type inconsistently (DWORD/
// uint32_t vs. "unsigned long", 4 vs. 8 bytes on this LP64 target); overload
// on both so every call site's argument types resolve without a cast at the
// call site.  __builtin_clz is undefined for a zero argument, hence the
// guard.
#ifdef __cplusplus
static inline unsigned char switch_BitScanReverse(uint32_t *Index, uint32_t Mask)
{
    if (!Mask)
        return 0;
    *Index = 31u - (unsigned)__builtin_clz(Mask);
    return 1;
}
static inline unsigned char switch_BitScanReverse(unsigned long *Index, unsigned long Mask)
{
    if (!Mask)
        return 0;
    *Index = (unsigned long)(sizeof(unsigned long) * 8 - 1 - (unsigned)__builtin_clzl(Mask));
    return 1;
}
#else
static inline unsigned char switch_BitScanReverse(uint32_t *Index, uint32_t Mask)
{
    if (!Mask)
        return 0;
    *Index = 31u - (unsigned)__builtin_clz(Mask);
    return 1;
}
#endif
#define _BitScanReverse(index, mask) switch_BitScanReverse((index), (mask))

// MMX/x87 FPU state has no AArch64 equivalent; _m_empty()/EMMS is a no-op.
#define _m_empty() ((void)0)
#endif
