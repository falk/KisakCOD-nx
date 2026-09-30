#pragma once

// In-process crash reporter: turns a guest CPU exception into `CRASH:` lines
// on the same stdout stream nxlink already carries to the host log, so a
// hardware crash shows up live without an FTP fetch of the Atmosphere
// report. Adapted from RSDuck/duckstation-switch-port (GPL): that port
// overrides __libnx_exception_entry via switch_asm.S for its resumable
// fastmem page-fault handler; this port does not need resumption, so it uses
// libnx's own simpler hook instead -- __libnx_exception_handler(ThreadExceptionDump*)
// plus __nx_exception_stack/__nx_exception_stack_size -- following the same
// "print PC/LR/FP walk, then leave" shape duckstation's HandleFault takes.
//
// The register-dump formatting and the frame-pointer walk below are plain
// data in, text out: no libnx types, no allocation, so switch_crash_test.cpp
// exercises them on host under ASan/UBSan. Only the `#if defined(__SWITCH__)`
// tail (switch_crash.cpp) touches ThreadExceptionDump, __nx_exception_stack
// and the write(2)/svcOutputDebugString path.

#include <stddef.h>
#include <stdint.h>

// Up to 32 return addresses: pc, lr, then the frame-pointer chain (matches
// switch_pcsample.cpp's Unwind so the same rebase math in
// offline crash tooling applies to both).
constexpr int kSwitchCrashMaxFrames = 32;

// One CPU register dump, already unpacked from whatever produced it
// (ThreadExceptionDump on hardware, a synthetic struct in the host test).
struct SwitchCrashRegs
{
    uint32_t error_desc; // ThreadExceptionDesc bits (see SwitchCrash_DescName)
    uint32_t esr, afsr0, afsr1;
    uint32_t pstate;
    uint64_t x[29]; // x0..x28
    uint64_t fp, lr, sp, pc, far;
    uint64_t base;    // main module .text lo (0 if unknown); host can rebase with it
    uint32_t threadId;      // engine's own ThreadContext id (0 if unknown/host)
    const char *threadName; // may be nullptr
};

// Maps a ThreadExceptionDesc value to a short name for the CRASH:INFO line.
// 0x101 ("Other" in libnx's own enum comment -- "None of the above") is what
// a plain data abort surfaces as; named DataAbort here since that's what it
// is in practice and what the task asked the line to read.
const char *SwitchCrash_DescName(uint32_t error_desc);

// Frame-pointer walk starting at pc/lr/fp: addrs[0]=pc, addrs[1]=lr, then
// each fp-chain return address, matching switch_pcsample.cpp's Unwind.
// Stops when a candidate fp falls outside [stackLo, stackHi), is not
// 16-byte aligned, or does not advance (fp <= previous fp). textLo/textHi
// (both 0 to skip) additionally reject a chain return address outside the
// module's code, the way Unwind rejects a caller from a frame-pointer-less
// prebuilt library. Returns the number of addresses written (>= 1, capped
// at maxFrames, itself capped at kSwitchCrashMaxFrames).
int SwitchCrash_WalkFrames(uint64_t pc, uint64_t lr, uint64_t fp, uint64_t stackLo, uint64_t stackHi,
                            uint64_t textLo, uint64_t textHi, uint64_t *outAddrs, int maxFrames);

// Formats regs plus the frame chain (stackAddrs[0..stackDepth)) as
// newline-terminated `CRASH:` lines into outBuf (snprintf-bounded, no
// allocation). Returns the number of bytes written (excluding the nul),
// clamped to outBufSize - 1 the way snprintf reports it. Four lines:
//   CRASH:BASE base=<hex>
//   CRASH:INFO desc=<name> pc=<hex> lr=<hex> sp=<hex> fp=<hex> far=<hex>
//              esr=<hex> afsr0=<hex> afsr1=<hex> pstate=<hex> thread=<id>
//              name=<name|unknown>
//   CRASH:REGS x0=<hex> x1=<hex> ... x28=<hex>
//   CRASH:STACK <hex>,<hex>,...  (pc,lr,frame chain; addr2line -4 on every
//                                entry but the first, same as PCS lines)
size_t SwitchCrash_FormatLines(const SwitchCrashRegs &regs, const uint64_t *stackAddrs, int stackDepth, char *outBuf,
                                size_t outBufSize);

#if defined(__SWITCH__)
// Test-only trigger for `+set switch_crashTest 1|2`, called once from
// Com_Frame (src/qcommon/common.cpp). 1 = null-write data abort (error_desc
// DataAbort/0x101). 2 = an `udf #0` illegal-instruction trap (error_desc
// Trap/0x104) -- NOT libc abort(): devkitA64's newlib raise() falls through
// to _kill_r, which devkitPro's libsysbase stubs to set errno=ENOSYS and
// return -1 (verified by disassembling libc_a-signal.o/libsysbase kill.o),
// so plain abort() just calls _exit(1) with no CPU exception at all on this
// platform and would never reach __libnx_exception_handler. `udf #0` is a
// real synchronous trap and exercises a second, distinct error_desc. Never
// call outside testing the crash handler itself.
void SwitchCrash_RunTest(int mode);
#endif
