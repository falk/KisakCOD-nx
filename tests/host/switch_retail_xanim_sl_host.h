#pragma once

// Host-only prelude for compiling the REAL scr_stringlist.cpp /
// scr_memorytree.cpp into host proofs (B3). Production gains these exact
// spellings from platform/switch/switch_compat.h, force-included by
// scripts/pre_build.cmake; host recipes use narrow compat headers instead,
// so this file mirrors the compat definitions without touching production
// sources (same pattern as switch_script_parsetree_test_preamble.h).
#include <stdint.h>

// Minimal windef scalar subset, mirroring switch_compat.h (guarded the
// same way so all three spellings coexist).
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

#ifndef HIWORD
#define HIWORD(x) ((uint16_t)(((x) >> 16) & 0xFFFF))
#endif
#ifndef LOWORD
#define LOWORD(x) ((uint16_t)((x) & 0xFFFF))
#endif

// switch_compat.h array-size helper.
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

// switch_compat.h: Win32 interlocked operations via GCC atomics.
#define InterlockedCompareExchange(pointer, value, comparand) \
    __sync_val_compare_and_swap((pointer), (comparand), (value))
#define InterlockedIncrement(ptr) __atomic_add_fetch((ptr), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(ptr) __atomic_sub_fetch((ptr), 1, __ATOMIC_SEQ_CST)
