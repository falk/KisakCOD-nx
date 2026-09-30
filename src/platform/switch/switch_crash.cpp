// In-process crash reporter. See switch_crash.h for the design and the
// duckstation-switch attribution.

#include "switch_crash.h"

#include <cstdio>
#include <cstring>

const char *SwitchCrash_DescName(uint32_t error_desc)
{
    switch (error_desc)
    {
    case 0x100: return "InstructionAbort";
    case 0x101: return "DataAbort"; // libnx's own enum names this "Other" (EC <= 0x34, uncategorized); in
                                     // practice this is where an EL0 data abort (permission/translation fault
                                     // on a load or store) surfaces, so name it what it usually is.
    case 0x102: return "MisalignedPC";
    case 0x103: return "MisalignedSP";
    case 0x104: return "Trap"; // uncategorized/illegal-instruction/system-register trap
    case 0x106: return "SError";
    case 0x301: return "BadSvc";
    default: return "Unknown";
    }
}

int SwitchCrash_WalkFrames(uint64_t pc, uint64_t lr, uint64_t fp, uint64_t stackLo, uint64_t stackHi,
                            uint64_t textLo, uint64_t textHi, uint64_t *outAddrs, int maxFrames)
{
    if (maxFrames > kSwitchCrashMaxFrames)
        maxFrames = kSwitchCrashMaxFrames;
    if (maxFrames < 1 || !outAddrs)
        return 0;
    int depth = 0;
    outAddrs[depth++] = pc;
    if (depth < maxFrames)
        outAddrs[depth++] = lr;
    uint64_t curFp = fp;
    const bool checkText = textLo != textHi;
    while (depth < maxFrames && curFp >= stackLo && curFp < stackHi && !(curFp & 15))
    {
        const uint64_t *record = reinterpret_cast<const uint64_t *>(curFp);
        const uint64_t next = record[0];
        const uint64_t ret = record[1];
        // A leaf that never saved lr leaves its caller's record at fp: skip
        // the duplicate of lr instead of counting its caller twice (same
        // rule as switch_pcsample.cpp's Unwind).
        if (checkText && (ret < textLo || ret >= textHi))
            break;
        if (!(depth == 2 && ret == lr))
            outAddrs[depth++] = ret;
        if (next <= curFp)
            break;
        curFp = next;
    }
    return depth;
}

size_t SwitchCrash_FormatLines(const SwitchCrashRegs &regs, const uint64_t *stackAddrs, int stackDepth,
                                char *outBuf, size_t outBufSize)
{
    if (!outBuf || outBufSize == 0)
        return 0;
    size_t len = 0;
    int n;

#define CRASH_APPEND(...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (len >= outBufSize)                                                                                        \
            goto done;                                                                                                \
        n = std::snprintf(outBuf + len, outBufSize - len, __VA_ARGS__);                                              \
        if (n > 0)                                                                                                    \
            len += (size_t)n;                                                                                         \
    } while (0)

    CRASH_APPEND("CRASH:BASE base=0x%llx\n", (unsigned long long)regs.base);
    CRASH_APPEND("CRASH:INFO desc=%s(0x%x) pc=0x%llx lr=0x%llx sp=0x%llx fp=0x%llx far=0x%llx esr=0x%x "
                 "afsr0=0x%x afsr1=0x%x pstate=0x%x thread=%u name=%s\n",
                 SwitchCrash_DescName(regs.error_desc), regs.error_desc, (unsigned long long)regs.pc,
                 (unsigned long long)regs.lr, (unsigned long long)regs.sp, (unsigned long long)regs.fp,
                 (unsigned long long)regs.far, regs.esr, regs.afsr0, regs.afsr1, regs.pstate, regs.threadId,
                 regs.threadName ? regs.threadName : "unknown");
    CRASH_APPEND("CRASH:REGS");
    for (int i = 0; i < 29 && len < outBufSize; ++i)
        CRASH_APPEND(" x%d=0x%llx", i, (unsigned long long)regs.x[i]);
    CRASH_APPEND("\n");
    CRASH_APPEND("CRASH:STACK");
    // No "0x" prefix and comma-joined: the same shape as switch_pcsample.cpp's
    // "PCS ... pc,lr,..." line, so offline tooling can reuse
    // the same parsing/rebase approach.
    for (int i = 0; i < stackDepth && len < outBufSize; ++i)
        CRASH_APPEND(i ? ",%llx" : " %llx", (unsigned long long)stackAddrs[i]);
    CRASH_APPEND("\n");

#undef CRASH_APPEND
done:
    if (len >= outBufSize)
    {
        len = outBufSize - 1;
        outBuf[len] = 0;
    }
    return len;
}

#if defined(__SWITCH__)
#include <switch.h>

#include <cstdlib>
#include <unistd.h>

#include <port/switch_pcsample.h> // &SwitchPcSample_SetRate: same module-base technique as PCSAMPLE
#include "switch_thread.h"   // Sys_GetCurrentThreadId(), threadId[]/ThreadContext_t names

namespace
{
// 64 KiB, 16-byte aligned: the exception handler's own stack (the crashing
// thread's real stack may itself be the reason for the crash, e.g. a stack
// overflow, so the handler must not run on it).
alignas(16) uint8_t s_exceptionStack[64 * 1024];

const char *ThreadContextName(uint32_t id, uint32_t *outContext)
{
    static const char *const kNames[THREAD_CONTEXT_COUNT] = {
        "main", "backend", "worker0", "worker1", "worker2", "server",
        "cinematic", "titleServer", "database", "stream", "sndStreamPacketCallback", "serverDemo",
    };
    for (uint32_t i = 0; i < THREAD_CONTEXT_COUNT; ++i)
    {
        if (threadId[i] == id)
        {
            *outContext = i;
            return kNames[i];
        }
    }
    return nullptr;
}

// Raw write(2) to fd 1: the nxlink socket is stdout on hardware, and this
// bypasses any stdio buffering/lock the crashing thread might already hold
// (the requirement this handler is built to avoid). Also mirrored to
// svcOutputDebugString, which emulators and attached debuggers surface
// even when nxlink is not the active console.
void EmitCrashBuffer(const char *buf, size_t len)
{
    if (len == 0)
        return;
    write(STDOUT_FILENO, buf, len);
    svcOutputDebugString(buf, len);
}
} // namespace

extern "C"
{
alignas(16) u8 __nx_exception_stack[sizeof(s_exceptionStack)];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    SwitchCrashRegs regs{};
    regs.error_desc = ctx->error_desc;
    regs.esr = ctx->esr;
    regs.afsr0 = ctx->afsr0;
    regs.afsr1 = ctx->afsr1;
    regs.pstate = ctx->pstate;
    for (int i = 0; i < 29; ++i)
        regs.x[i] = ctx->cpu_gprs[i].x;
    regs.fp = ctx->fp.x;
    regs.lr = ctx->lr.x;
    regs.sp = ctx->sp.x;
    regs.pc = ctx->pc.x;
    regs.far = ctx->far.x;

    // Same technique as switch_pcsample.cpp's SamplerMain: svcQueryMemory on
    // an address inside our own module's .text returns that memory block's
    // start, which offline tooling rebases against with the same symbol's
    // link-time address from `nm`.
    uint64_t textLo = 0, textHi = 0;
    MemoryInfo info{};
    u32 page = 0;
    if (R_SUCCEEDED(svcQueryMemory(&info, &page, reinterpret_cast<uintptr_t>(&SwitchPcSample_SetRate))))
    {
        textLo = info.addr;
        textHi = info.addr + info.size;
    }
    regs.base = textLo;

    // Still running as the faulting thread (Horizon's user exception model
    // does not spawn a new one), so its own TLS/id are valid to read here.
    regs.threadId = Sys_GetCurrentThreadId();
    uint32_t context = 0;
    regs.threadName = ThreadContextName(regs.threadId, &context);

    uint64_t stackAddrs[kSwitchCrashMaxFrames];
    // 1 MiB span above sp: generous for any of this engine's thread stacks,
    // same bound switch_pcsample.cpp's Unwind uses.
    // Walk only inside the memory block that actually holds sp (the kernel's
    // mapping), never a guessed size: a frame chain leading past a small
    // stack made the handler fault inside itself.
    uint64_t stackLo = regs.sp, stackHi = regs.sp;
    {
        MemoryInfo stackInfo{};
        u32 stackPage = 0;
        if (R_SUCCEEDED(svcQueryMemory(&stackInfo, &stackPage, regs.sp)) && (stackInfo.perm & Perm_R) &&
            stackInfo.type != MemType_Unmapped)
            stackHi = stackInfo.addr + stackInfo.size;
    }
    const int depth = SwitchCrash_WalkFrames(regs.pc, regs.lr, regs.fp, stackLo, stackHi, textLo, textHi, stackAddrs,
                                              kSwitchCrashMaxFrames);

    static char s_buf[4096]; // static: no stack allocation, no malloc
    const size_t len = SwitchCrash_FormatLines(regs, stackAddrs, depth, s_buf, sizeof(s_buf));
    EmitCrashBuffer(s_buf, len);

    // write(2) above is unbuffered (a raw syscall), so there is nothing left
    // to flush. Returning here falls into libnx's own
    // __libnx_exception_returnentry, which unconditionally calls
    // svcBreak(0,0,0); on hardware without a debugger attached that leaves
    // Atmosphere to write a report and the console stuck rather than back at
    // hbmenu. _exit (not exit: no atexit/global-destructor reentrancy in a
    // process that just crashed) skips that path and terminates directly
    // with a non-zero code, which is what the requirement asks for.
    _exit(1);
}
} // extern "C"

void SwitchCrash_RunTest(int mode)
{
    if (mode == 1)
    {
        // Deliberate null-write data abort.
        volatile uint64_t *bad = reinterpret_cast<volatile uint64_t *>(0);
        *bad = 0xdeadull;
    }
    else if (mode == 2)
    {
        // Not abort(): see switch_crash.h. A real trap so the handler is
        // exercised with a second, distinct error_desc.
        asm volatile("udf #0");
    }
}

#endif // __SWITCH__
