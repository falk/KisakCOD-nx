// Host proof for the in-process crash reporter (src/platform/switch/switch_crash.h,
// src/platform/switch/switch_crash.cpp). Runs under ASan/UBSan via ./test host.
//
// The frame-pointer walk and the CRASH: line formatting are plain data in,
// text out (no libnx types, no allocation), so this exercises them exactly
// as switch_crash.cpp's __libnx_exception_handler does on hardware, without
// needing a real exception. See "Hardware crash log" for
// why this host proof exists: some emulators do not deliver a guest CPU exception
// to __libnx_exception_handler, so the formatting/walk logic must be provable without it.

#include "src/platform/switch/switch_crash.h"

#include <cstdio>
#include <cstring>

#define CHECK(cond)                                                                                                  \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            printf("FAIL:SWITCH_CRASH %s (line %d)\n", #cond, __LINE__);                                              \
            return 1;                                                                                                 \
        }                                                                                                             \
    } while (0)

namespace
{
// A synthetic call stack in plain host memory: three 16-byte-aligned frame
// records (fp0 -> fp1 -> fp2 -> 0), matching the AArch64 frame layout
// [saved fp, saved lr] that switch_pcsample.cpp's Unwind and
// SwitchCrash_WalkFrames both read.
struct FakeStack
{
    alignas(16) uint64_t frame0[2]; // [fp1, lr0]
    alignas(16) uint64_t frame1[2]; // [fp2, lr1]
    alignas(16) uint64_t frame2[2]; // [0,   lr2] (chain end)
};
} // namespace

int main()
{
    // 1. DescName covers the named cases the task asked for and falls back
    // for anything else.
    CHECK(!strcmp(SwitchCrash_DescName(0x100), "InstructionAbort"));
    CHECK(!strcmp(SwitchCrash_DescName(0x101), "DataAbort"));
    CHECK(!strcmp(SwitchCrash_DescName(0x102), "MisalignedPC"));
    CHECK(!strcmp(SwitchCrash_DescName(0x103), "MisalignedSP"));
    CHECK(!strcmp(SwitchCrash_DescName(0x104), "Trap"));
    CHECK(!strcmp(SwitchCrash_DescName(0x106), "SError"));
    CHECK(!strcmp(SwitchCrash_DescName(0x301), "BadSvc"));
    CHECK(!strcmp(SwitchCrash_DescName(0xdead), "Unknown"));

    // 2. Frame walk over a real (host) fp chain: pc, lr, then each chain
    // return address, stopping at the null-fp chain end.
    FakeStack stack{};
    const uint64_t fp0 = reinterpret_cast<uint64_t>(&stack.frame0);
    const uint64_t fp1 = reinterpret_cast<uint64_t>(&stack.frame1);
    const uint64_t fp2 = reinterpret_cast<uint64_t>(&stack.frame2);
    stack.frame0[0] = fp1;
    stack.frame0[1] = 0x1111; // caller of the crashing function
    stack.frame1[0] = fp2;
    stack.frame1[1] = 0x2222;
    stack.frame2[0] = 0; // chain end: next <= fp -> stop
    stack.frame2[1] = 0x3333;
    const uint64_t stackLo = reinterpret_cast<uint64_t>(&stack);
    const uint64_t stackHi = stackLo + sizeof(stack) + 4096;

    uint64_t addrs[kSwitchCrashMaxFrames];
    int depth = SwitchCrash_WalkFrames(/*pc*/ 0xf00d, /*lr*/ 0xbeef, fp0, stackLo, stackHi, /*textLo*/ 0,
                                        /*textHi*/ 0, addrs, kSwitchCrashMaxFrames);
    CHECK(depth == 5); // pc, lr, 0x1111, 0x2222, 0x3333
    CHECK(addrs[0] == 0xf00d);
    CHECK(addrs[1] == 0xbeef);
    CHECK(addrs[2] == 0x1111);
    CHECK(addrs[3] == 0x2222);
    CHECK(addrs[4] == 0x3333);

    // 3. A crashed function that itself never saved lr (a frame-pointer-less
    // leaf) leaves its caller's record at fp: the chain's first entry
    // duplicating lr is skipped, not double-counted (same rule as
    // switch_pcsample.cpp's Unwind, "depth == 2 && ret == lr").
    depth = SwitchCrash_WalkFrames(/*pc*/ 0xf00d, /*lr*/ 0x1111, fp0, stackLo, stackHi, 0, 0, addrs,
                                    kSwitchCrashMaxFrames);
    CHECK(depth == 4); // pc, lr(=0x1111), 0x2222, 0x3333 -- the fp0 record's 0x1111 duplicate is skipped
    CHECK(addrs[0] == 0xf00d);
    CHECK(addrs[1] == 0x1111);
    CHECK(addrs[2] == 0x2222);
    CHECK(addrs[3] == 0x3333);

    // 4. An fp outside [stackLo, stackHi) stops the walk after pc/lr: a
    // corrupted or foreign frame pointer must not be dereferenced.
    depth = SwitchCrash_WalkFrames(0xf00d, 0xbeef, /*fp*/ stackLo - 0x10000, stackLo, stackHi, 0, 0, addrs, 32);
    CHECK(depth == 2);

    // 5. A misaligned fp (not 16-byte) stops the walk the same way.
    depth = SwitchCrash_WalkFrames(0xf00d, 0xbeef, fp0 + 1, stackLo, stackHi, 0, 0, addrs, 32);
    CHECK(depth == 2);

    // 6. A chain return address outside [textLo, textHi) stops the walk
    // (a prebuilt library frame with no frame pointer, same as Unwind).
    depth = SwitchCrash_WalkFrames(0xf00d, 0xbeef, fp0, stackLo, stackHi, /*textLo*/ 0x5000, /*textHi*/ 0x6000,
                                    addrs, 32);
    CHECK(depth == 2); // 0x1111 is outside [0x5000,0x6000) -> the fp0 record is rejected

    // 7. maxFrames caps the walk (kSwitchCrashMaxFrames is the hard cap too).
    depth = SwitchCrash_WalkFrames(0xf00d, 0xbeef, fp0, stackLo, stackHi, 0, 0, addrs, 2);
    CHECK(depth == 2);

    // 8. Formatting: every field lands in the CRASH: lines, in the shape
    // offline tooling parses (CRASH:STACK bare hex, comma-joined,
    // no "0x", matching switch_pcsample.cpp's "PCS ... pc,lr,..." line).
    SwitchCrashRegs regs{};
    regs.error_desc = 0x101;
    regs.esr = 0x96000004;
    regs.afsr0 = 1;
    regs.afsr1 = 2;
    regs.pstate = 0x60000000;
    for (int i = 0; i < 29; ++i)
        regs.x[i] = 0x1000 + (uint64_t)i;
    regs.fp = fp0;
    regs.lr = 0xbeef;
    regs.sp = stackLo;
    regs.pc = 0xf00d;
    regs.far = 0;
    regs.base = 0x710000000000ull;
    regs.threadId = 3;
    regs.threadName = "worker0";

    uint64_t stackAddrs[5] = {0xf00d, 0xbeef, 0x1111, 0x2222, 0x3333};
    char buf[4096];
    const size_t len = SwitchCrash_FormatLines(regs, stackAddrs, 5, buf, sizeof(buf));
    CHECK(len > 0 && len < sizeof(buf));
    CHECK(buf[len] == '\0'); // snprintf-terminated

    CHECK(strstr(buf, "CRASH:BASE base=0x710000000000\n") != nullptr);
    CHECK(strstr(buf, "CRASH:INFO desc=DataAbort(0x101) pc=0xf00d lr=0xbeef sp=0x") != nullptr);
    CHECK(strstr(buf, "far=0x0 esr=0x96000004 afsr0=0x1 afsr1=0x2 pstate=0x60000000 thread=3 name=worker0\n") !=
          nullptr);
    CHECK(strstr(buf, "CRASH:REGS x0=0x1000 x1=0x1001") != nullptr);
    CHECK(strstr(buf, "x28=0x101c\n") != nullptr);
    CHECK(strstr(buf, "CRASH:STACK f00d,beef,1111,2222,3333\n") != nullptr);

    // 9. Formatting never overruns a small buffer: truncated but nul-terminated.
    char small[24];
    const size_t smallLen = SwitchCrash_FormatLines(regs, stackAddrs, 5, small, sizeof(small));
    CHECK(smallLen == sizeof(small) - 1);
    CHECK(small[smallLen] == '\0');

    // 10. No output buffer or zero size: no crash, nothing written.
    CHECK(SwitchCrash_FormatLines(regs, stackAddrs, 5, nullptr, 100) == 0);
    CHECK(SwitchCrash_FormatLines(regs, stackAddrs, 5, small, 0) == 0);

    printf("PASS:SWITCH_CRASH_FORMAT lines_len=%zu\n", len);
    return 0;
}
