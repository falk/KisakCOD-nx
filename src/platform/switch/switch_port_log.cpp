#include "switch_port_log.h"

#include <cstdio>
#include <cstring>

#ifdef __SWITCH__
#include <switch.h>

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/iosupport.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "switch_log_ring.h"
#endif

#ifdef __SWITCH__
namespace
{
constexpr size_t kRingBytes = 512 * 1024;
constexpr size_t kFileCapBytes = 2 * 1024 * 1024;
constexpr uint64_t kFileFlushMs = 250;
constexpr uint32_t kNetStallMs = 250;

alignas(16) char s_ringStorage[kRingBytes];
LogRing s_ring(s_ringStorage, kRingBytes);
std::atomic<bool> s_started{false};
std::atomic<bool> s_quit{false};
std::atomic<uint32_t> s_flushReq{0};
std::atomic<uint32_t> s_flushDone{0};
int s_netFd = -1;
Thread s_thread;
bool s_threadRunning = false;

uint64_t NowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ull; }

ssize_t RingWrite(struct _reent *, void *, const char *ptr, size_t len)
{
    s_ring.Push(ptr, len);
    return (ssize_t)len;
}

int RingFstat(struct _reent *, void *, struct stat *st)
{
    std::memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR; // line-buffered stdio
    return 0;
}

const devoptab_t s_ringDevice = {
    "kisaklog", 0, nullptr, nullptr, RingWrite, nullptr, nullptr, RingFstat,
};

struct DrainState
{
    LogPump pump{kNetStallMs};
    LogFileRing file;
};

long NetSend(const char *p, size_t n)
{
    const ssize_t r = send(s_netFd, p, n, MSG_DONTWAIT);
    return r > 0 ? (long)r : 0;
}

void DrainOnce(DrainState &st)
{
    auto file = [&](const char *p, size_t n) { st.file.Write(p, n); };
    st.pump.Step(s_ring, NowMs(), s_netFd >= 0, NetSend, file);
}

// Drains everything currently queued, then flushes the card file.
void DrainAll(DrainState &st)
{
    auto file = [&](const char *p, size_t n) { st.file.Write(p, n); };
    for (int i = 0; i < 256; ++i)
        if (!st.pump.Step(s_ring, NowMs(), s_netFd >= 0, NetSend, file))
            break;
    st.file.Flush();
}

DrainState *s_state = nullptr;

void DrainMain(void *)
{
    DrainState &st = *s_state;
    uint64_t lastFlush = NowMs();
    while (!s_quit.load(std::memory_order_acquire))
    {
        const uint32_t req = s_flushReq.load(std::memory_order_acquire);
        if (req != s_flushDone.load(std::memory_order_relaxed))
        {
            DrainAll(st);
            s_flushDone.store(req, std::memory_order_release);
            lastFlush = NowMs();
            continue;
        }
        auto file = [&](const char *p, size_t n) { st.file.Write(p, n); };
        const bool did = st.pump.Step(s_ring, NowMs(), s_netFd >= 0, NetSend, file);
        const uint64_t now = NowMs();
        if (now - lastFlush >= kFileFlushMs)
        {
            st.file.Flush();
            lastFlush = now;
        }
        if (!did)
            svcSleepThread(10ull * 1000000ull);
        else if (st.pump.Blocked())
            svcSleepThread(5ull * 1000000ull);
    }
}
} // namespace

extern "C" int Switch_PortLogRingActive(void)
{
    return s_started.load(std::memory_order_acquire);
}

extern "C" void Port_LogStart(int netFd)
{
    if (s_started.load(std::memory_order_acquire))
        return;
    s_netFd = netFd;
    if (netFd >= 0)
        fcntl(netFd, F_SETFL, fcntl(netFd, F_GETFL, 0) | O_NONBLOCK);
    s_state = new DrainState();
    s_state->file.Open("sdmc:/switch/kisakcod/port_log_ring", kFileCapBytes);
    // Every printf/Com_Printf/Sys_Print byte now lands in the ring, whatever
    // stdout used to point at (the nxlink socket).
    std::fflush(stdout);
    std::fflush(stderr);
    devoptab_list[STD_OUT] = &s_ringDevice;
    devoptab_list[STD_ERR] = &s_ringDevice;
    s_started.store(true, std::memory_order_release);
    if (R_SUCCEEDED(threadCreate(&s_thread, DrainMain, nullptr, nullptr, 0x8000, 0x3B, -2)) &&
        R_SUCCEEDED(threadStart(&s_thread)))
        s_threadRunning = true;
}

extern "C" void Port_LogRaw(const char *line)
{
    if (!line)
        return;
    s_ring.Push(line, std::strlen(line));
    svcOutputDebugString(line, std::strlen(line));
}

extern "C" void Port_LogFlush(uint32_t maxWaitMs)
{
    if (!s_threadRunning)
        return;
    const uint32_t req = s_flushReq.fetch_add(1, std::memory_order_acq_rel) + 1;
    for (uint32_t waited = 0; waited < maxWaitMs; ++waited)
    {
        if ((int32_t)(s_flushDone.load(std::memory_order_acquire) - req) >= 0)
            return;
        svcSleepThread(1000000ull);
    }
}

extern "C" void Port_LogShutdown(void)
{
    if (!s_threadRunning)
        return;
    std::fflush(stdout);
    Port_LogFlush(300);
    s_quit.store(true, std::memory_order_release);
    threadWaitForExit(&s_thread);
    threadClose(&s_thread);
    s_threadRunning = false;
    s_state->file.Close();
}
#else
extern "C" void Port_LogStart(int) {}
extern "C" void Port_LogRaw(const char *line) { std::fputs(line, stderr); }
extern "C" void Port_LogFlush(uint32_t) {}
extern "C" void Port_LogShutdown(void) {}
#endif

namespace
{
// deko9_d3d.cpp's original Emit, generalized to a raw byte passthrough (no
// added newline): Switch_BootLog builds one logical line from several
// consecutive calls, and audren's lines already carry their own trailing
// "\n" in their format strings, so a caller-added newline here would split
// or double them.
//
// stdout is the log ring on hardware (drained to nxlink and the SD ring
// file off-thread); the debug string reaches emulator and debugger logs.
// FAIL lines are also flushed to the SD card before returning.
void PortLogFanOut(const char *line)
{
#ifdef __SWITCH__
    std::fputs(line, stdout);
    std::fflush(stdout);
    svcOutputDebugString(line, std::strlen(line));
    if (std::strncmp(line, "FAIL:", 5) != 0)
        return;
    static FILE *s_file = std::fopen("sdmc:/switch/kisakcod/port_fail.log", "w");
    if (s_file)
    {
        std::fprintf(s_file, "%llu %s\n",
                     (unsigned long long)armTicksToNs(armGetSystemTick()) / 1000000ull, line);
        std::fflush(s_file);
    }
    Port_LogFlush(200);
#else
    std::fputs(line, stderr);
    std::fflush(stderr);
#endif
}
} // namespace

void Port_Log(const char *line)
{
    if (line)
        PortLogFanOut(line);
}

void Port_Fail(const char *line)
{
    if (line)
        PortLogFanOut(line);
}
