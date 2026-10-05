// Log ring, pump, SD file ring and stall trigger (platform/switch/
// switch_log_ring.h, switch_watchdog.h): producers never wait on a stalled
// sink, drops are counted and announced, order is preserved.
#include <platform/switch/switch_log_ring.h>
#include <platform/switch/switch_watchdog.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>


static int s_failures = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL:LOG_RING %s:%d %s\n", __FILE__, __LINE__, #c);                                   \
            ++s_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

static void OrderingAndWrap()
{
    char store[64];
    LogRing ring(store, sizeof(store));
    std::string expect;
    for (int i = 0; i < 40; ++i)
    {
        char line[16];
        const int n = std::snprintf(line, sizeof(line), "line%d\n", i);
        CHECK(ring.Push(line, (size_t)n));
        expect.append(line, (size_t)n);
        if (i % 3 == 2) // interleave pops so head wraps the 64-byte store
        {
            char got[7];
            size_t g;
            std::string part;
            while ((g = ring.Pop(got, sizeof(got))) != 0)
                part.append(got, g);
            CHECK(part == expect);
            expect.clear();
        }
    }
    CHECK(ring.TakeDropped() == 0);
}

static void FullRingDropsInsteadOfBlocking()
{
    char store[128];
    LogRing ring(store, sizeof(store));
    int accepted = 0, rejected = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i)
        (ring.Push("0123456789\n", 11) ? accepted : rejected)++;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1000);
    CHECK(accepted == 11); // 128 / 11
    CHECK(rejected == 100000 - 11);
    CHECK(ring.TakeDropped() == (uint64_t)rejected);
    CHECK(ring.TakeDropped() == 0);
}

static void ProducersNeverWaitOnStalledConsumer()
{
    static char store[4096];
    LogRing ring(store, sizeof(store));
    std::atomic<uint64_t> worstNs{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 20000; ++i)
            {
                const auto a = std::chrono::steady_clock::now();
                ring.Push("a log line of moderate length\n", 30);
                const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - a)
                                        .count();
                uint64_t w = worstNs.load();
                while (ns > w && !worstNs.compare_exchange_weak(w, ns))
                {
                }
            }
        });
    for (auto &th : threads)
        th.join();
    CHECK(worstNs.load() < 200ull * 1000 * 1000); // no consumer ever ran
    CHECK(ring.TakeDropped() > 0);
}

static void PumpStallDropsAndRecovers()
{
    static char store[1024];
    LogRing ring(store, sizeof(store));
    LogPump pump(100);
    bool netBlocked = true;
    std::string net, file;
    auto send = [&](const char *p, size_t n) -> long {
        if (netBlocked)
            return 0;
        net.append(p, n);
        return (long)n;
    };
    auto sink = [&](const char *p, size_t n) { file.append(p, n); };
    uint64_t now = 1000;

    ring.Push("one\n", 4);
    pump.Step(ring, now, true, send, sink);
    CHECK(pump.Blocked() && !pump.Stalled());
    now += 150;
    pump.Step(ring, now, true, send, sink); // past stallMs: dropped
    CHECK(!pump.Blocked() && pump.Stalled());
    ring.Push("two\n", 4);
    pump.Step(ring, now, true, send, sink); // still stalled: dropped at once
    CHECK(!pump.Blocked());
    CHECK(net.empty());
    netBlocked = false;
    ring.Push("three\n", 6);
    pump.Step(ring, now, true, send, sink);
    ring.Push("four\n", 5);
    pump.Step(ring, now, true, send, sink);
    CHECK(net == "three\nLOG_DROPPED 2\nfour\n");
    CHECK(file == "one\ntwo\nthree\nfour\n"); // the card file never loses to the network
}

static void PumpReportsRingDrops()
{
    static char store[32];
    LogRing ring(store, sizeof(store));
    LogPump pump;
    std::string net, file;
    auto send = [&](const char *p, size_t n) -> long { net.append(p, n); return (long)n; };
    auto sink = [&](const char *p, size_t n) { file.append(p, n); };
    ring.Push("1234567890\n", 11);
    ring.Push("abcdefghij\n", 11);
    CHECK(!ring.Push("abcdefghij\n", 11));
    pump.Step(ring, 5, true, send, sink);
    CHECK(file.find("LOG_DROPPED 1\n") == 0);
    CHECK(net == "LOG_DROPPED 1\n1234567890\nabcdefghij\n");
}

static void FileRingRotates()
{
    char dir[] = "/tmp/logringXXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    const std::string prefix = std::string(dir) + "/ring";
    {
        LogFileRing f;
        f.Open(prefix, 100);
        for (int i = 0; i < 20; ++i)
            f.Write("0123456789\n", 11);
        f.Flush();
    }
    auto size = [](const std::string &p) -> long {
        FILE *fp = std::fopen(p.c_str(), "rb");
        if (!fp)
            return -1;
        std::fseek(fp, 0, SEEK_END);
        const long n = std::ftell(fp);
        std::fclose(fp);
        return n;
    };
    const long a = size(prefix + "_a.txt"), b = size(prefix + "_b.txt");
    CHECK(a > 0 && a <= 100 && b > 0 && b <= 100);
    { // a second run keeps the first as _prev_
        LogFileRing f;
        f.Open(prefix, 100);
    }
    CHECK(size(prefix + "_prev_a.txt") > 0 || size(prefix + "_prev_b.txt") > 0);
    std::string cmd = std::string("rm -rf ") + dir;
    CHECK(std::system(cmd.c_str()) == 0);
}

static void StallTriggerFiresOnce()
{
    StallTrigger t(3000);
    CHECK(!t.Sample(5, 0));
    CHECK(!t.Sample(5, 2999));
    CHECK(t.Sample(5, 3000));
    CHECK(!t.Sample(5, 9000)); // once per stall
    CHECK(!t.Sample(6, 9100)); // moved: re-armed
    CHECK(!t.Sample(6, 12000));
    CHECK(t.Sample(6, 12100));
    t.SetThreshold(15000);
    CHECK(!t.Sample(7, 13000));
    CHECK(!t.Sample(7, 27999));
    CHECK(t.Sample(7, 28000)); // longer threshold while a zone loads
    Watchdog_Crumb(CRUMB_BACKEND, 2);
    Watchdog_Crumb(CRUMB_BACKEND, 1);
    CHECK(g_crumbs[CRUMB_BACKEND].count.load() == 2 && g_crumbs[CRUMB_BACKEND].site.load() == 1);
}

int main()
{
    OrderingAndWrap();
    FullRingDropsInsteadOfBlocking();
    ProducersNeverWaitOnStalledConsumer();
    PumpStallDropsAndRecovers();
    PumpReportsRingDrops();
    FileRingRotates();
    StallTriggerFiresOnce();
    if (s_failures)
        return 1;
    std::puts("PASS:LOG_RING producers non-blocking, drops counted, order kept, watchdog trigger once per stall");
    return 0;
}
