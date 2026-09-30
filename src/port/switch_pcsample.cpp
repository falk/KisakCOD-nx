// Statistical CPU profiler for hardware runs behind the `switch_pcSample`
// dvar (samples per second, 0 = off).
//
// SWITCH_PERF scopes only time what someone thought to wrap, and guessing
// from them has sent perf work after the wrong phase before.  This samples
// instead: a high-priority thread pauses each registered game thread with
// svcSetThreadActivity, reads its registers with svcGetThreadContext3 (both
// are allowed on threads of the calling process), resumes it, and records
// the pc, lr and the frame-pointer chain.  Stacks are aggregated per thread
// and printed every window as
//
//   PCSAMPLE base=<addr of SwitchPcSample_SetRate> hz=<n> window_ms=<n>
//   PCS <tag> <count> <pc>,<lr>,<ret1>,<ret2>,...
//   PCSAMPLE end samples=<n> dropped=<n>
//
// with absolute addresses; offline tooling rebases them with
// the ELF's SwitchPcSample_SetRate and symbolizes.  The chain only goes past
// lr when the build keeps frame pointers (KISAK_SWITCH_FRAME_POINTERS=ON).

#include "switch_pcsample.h"

#if defined(__SWITCH__)
#include <switch.h>

#include <stdio.h>
#include <string.h>

#include <atomic>

extern "C" int Switch_NxlinkStdioActive(void) __attribute__((weak));

namespace
{
constexpr int kMaxThreads = 8;
constexpr int kMaxDepth = 16;
constexpr uint32_t kTableSize = 8192; // power of two
constexpr uint64_t kWindowMs = 5000;
constexpr uint64_t kFrameSpan = 1u << 20; // frame records stay within 1 MB of sp

struct Registered
{
    Handle handle;
    int tag;
};

struct StackEntry
{
    uint64_t hash;
    uint32_t count;
    uint8_t tag;
    uint8_t depth;
    uint64_t addrs[kMaxDepth];
};

Registered s_threads[kMaxThreads];
std::atomic<int> s_threadCount{0};
std::atomic<int> s_rate{0};
std::atomic<bool> s_started{false};
Thread s_sampler;
StackEntry s_table[kTableSize];
uint32_t s_used;
uint64_t s_samples;
uint64_t s_dropped;
uint64_t s_textLo, s_textHi; // the game module's executable segment

uint64_t HashStack(int tag, const uint64_t *addrs, int depth)
{
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)tag;
    for (int i = 0; i < depth; ++i)
    {
        h ^= addrs[i];
        h *= 0x100000001b3ull;
    }
    return h ? h : 1;
}

void Record(int tag, const uint64_t *addrs, int depth)
{
    ++s_samples;
    const uint64_t hash = HashStack(tag, addrs, depth);
    for (uint32_t probe = 0; probe < kTableSize; ++probe)
    {
        StackEntry &e = s_table[(hash + probe) & (kTableSize - 1)];
        if (e.hash == 0)
        {
            if (s_used >= kTableSize * 3 / 4)
                break;
            ++s_used;
            e.hash = hash;
            e.count = 1;
            e.tag = (uint8_t)tag;
            e.depth = (uint8_t)depth;
            memcpy(e.addrs, addrs, depth * sizeof(uint64_t));
            return;
        }
        if (e.hash == hash && e.tag == tag && e.depth == depth &&
            !memcmp(e.addrs, addrs, depth * sizeof(uint64_t)))
        {
            ++e.count;
            return;
        }
    }
    ++s_dropped;
}

// Called with the target paused, so its stack cannot change under the walk.
int Unwind(const ThreadContext &ctx, uint64_t *addrs)
{
    int depth = 0;
    addrs[depth++] = ctx.pc.x;
    addrs[depth++] = ctx.lr;
    uint64_t fp = ctx.fp;
    const uint64_t lo = ctx.sp;
    while (depth < kMaxDepth && fp >= lo && fp < lo + kFrameSpan && !(fp & 15))
    {
        const uint64_t *record = reinterpret_cast<const uint64_t *>(fp);
        const uint64_t next = record[0];
        const uint64_t ret = record[1];
        // Code built without frame pointers (prebuilt libraries) uses x29
        // as a plain register; a return address outside the module's code
        // means the chain is no longer a chain.
        if (ret < s_textLo || ret >= s_textHi)
            break;
        // A leaf that never saved lr leaves its caller's record at fp: skip
        // the duplicate of lr instead of counting its caller twice.
        if (!(depth == 2 && ret == ctx.lr))
            addrs[depth++] = ret;
        if (next <= fp)
            break;
        fp = next;
    }
    return depth;
}

// Not Sys_Print: that keeps unlocked line state for the main thread.  Same
// routing, though: stdout once nxlink owns it, else the debug log emulators
// show.
void Emit(const char *line)
{
    if (Switch_NxlinkStdioActive && Switch_NxlinkStdioActive())
        fputs(line, stdout);
    else
        svcOutputDebugString(line, strlen(line));
}

void Flush(int hz, uint64_t windowMs)
{
    char line[48 + kMaxDepth * 19];
    snprintf(line, sizeof(line), "PCSAMPLE base=%p hz=%d window_ms=%llu\n",
             reinterpret_cast<void *>(&SwitchPcSample_SetRate), hz, (unsigned long long)windowMs);
    Emit(line);
    for (uint32_t i = 0; i < kTableSize; ++i)
    {
        StackEntry &e = s_table[i];
        if (!e.hash)
            continue;
        int n = snprintf(line, sizeof(line), "PCS %u %u ", e.tag, e.count);
        for (int d = 0; d < e.depth && n < (int)sizeof(line) - 20; ++d)
            n += snprintf(line + n, sizeof(line) - n, d ? ",%llx" : "%llx", (unsigned long long)e.addrs[d]);
        snprintf(line + n, sizeof(line) - n, "\n");
        Emit(line);
    }
    snprintf(line, sizeof(line), "PCSAMPLE end samples=%llu dropped=%llu\n", (unsigned long long)s_samples,
             (unsigned long long)s_dropped);
    Emit(line);
    fflush(stdout);
    memset(s_table, 0, sizeof(s_table));
    s_used = 0;
    s_samples = 0;
    s_dropped = 0;
}

void SamplerMain(void *)
{
    MemoryInfo info{};
    u32 page = 0;
    if (R_SUCCEEDED(svcQueryMemory(&info, &page, reinterpret_cast<uintptr_t>(&SwitchPcSample_SetRate))))
    {
        s_textLo = info.addr;
        s_textHi = info.addr + info.size;
    }
    uint64_t windowStart = armGetSystemTick();
    const uint64_t freq = armGetSystemTickFreq();
    for (;;)
    {
        const int hz = s_rate.load(std::memory_order_relaxed);
        if (hz <= 0)
        {
            if (s_samples)
                Flush(0, (armGetSystemTick() - windowStart) * 1000 / freq);
            svcSleepThread(100000000ll);
            windowStart = armGetSystemTick();
            continue;
        }
        const int count = s_threadCount.load(std::memory_order_acquire);
        for (int t = 0; t < count; ++t)
        {
            const Registered &r = s_threads[t];
            if (R_FAILED(svcSetThreadActivity(r.handle, ThreadActivity_Paused)))
                continue;
            ThreadContext ctx;
            uint64_t addrs[kMaxDepth];
            int depth = 0;
            if (R_SUCCEEDED(svcGetThreadContext3(&ctx, r.handle)))
                depth = Unwind(ctx, addrs);
            svcSetThreadActivity(r.handle, ThreadActivity_Runnable);
            if (depth)
                Record(r.tag, addrs, depth);
        }
        const uint64_t elapsedMs = (armGetSystemTick() - windowStart) * 1000 / freq;
        if (elapsedMs >= kWindowMs)
        {
            Flush(hz, elapsedMs);
            windowStart = armGetSystemTick();
        }
        svcSleepThread(1000000000ll / hz);
    }
}
} // namespace

void SwitchPcSample_RegisterCurrentThread(int tag)
{
    const int index = s_threadCount.load(std::memory_order_relaxed);
    if (index >= kMaxThreads)
        return;
    s_threads[index] = {threadGetCurHandle(), tag};
    s_threadCount.store(index + 1, std::memory_order_release);
}

void SwitchPcSample_SetRate(int hz)
{
    s_rate.store(hz, std::memory_order_relaxed);
    if (hz <= 0 || s_started.exchange(true))
        return;
    // Core 1 (the renderer worker's) at a priority above every game thread:
    // a sample is a few microseconds, then the sampler sleeps.
    if (R_FAILED(threadCreate(&s_sampler, SamplerMain, nullptr, nullptr, 0x4000, 0x1C, 1)) ||
        R_FAILED(threadStart(&s_sampler)))
        Emit("PCSAMPLE FAIL: sampler thread not started\n");
}

#else

void SwitchPcSample_RegisterCurrentThread(int) {}
void SwitchPcSample_SetRate(int) {}

#endif
