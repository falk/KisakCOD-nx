// Async save writer (platform/switch/switch_async_file_writer.h): jobs
// complete, run in order, never expose a torn file, and WaitIdle makes the
// newest file visible to a reader.
#include <platform/switch/switch_async_file_writer.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

static int s_failures = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL:ASYNC_FILE_WRITER %s:%d %s\n", __FILE__, __LINE__, #c);                          \
            ++s_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

static std::string s_dir;
static std::atomic<int> s_okCount(0), s_badCount(0);

static void CountResult(const AsyncFileWriter::Result &r)
{
    ++(r.ok ? s_okCount : s_badCount);
}

static std::vector<uint8_t> Slurp(const std::string &path, bool *exists)
{
    std::vector<uint8_t> out;
    std::FILE *f = std::fopen(path.c_str(), "rb");
    *exists = f != nullptr;
    if (!f)
        return out;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

static AsyncFileWriter::Job MakeJob(const std::string &name, uint8_t fill, size_t size)
{
    AsyncFileWriter::Job job;
    job.tmpPath = s_dir + "/temp.svg";
    job.finalPath = s_dir + "/" + name;
    job.data.assign(size, fill);
    return job;
}

static void MakeSubdirs(const std::string &path)
{
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos)
        mkdir(path.substr(0, slash).c_str(), 0755);
}

static void CompletionAndOrdering()
{
    AsyncFileWriter w;
    w.SetReporter(CountResult);
    s_okCount = s_badCount = 0;
    // Same final path three times: the last submitted content must win.
    w.Submit(MakeJob("a.svg", 1, 300000));
    w.Submit(MakeJob("a.svg", 2, 200000));
    w.Submit(MakeJob("a.svg", 3, 100000));
    w.WaitIdle();
    bool exists;
    std::vector<uint8_t> got = Slurp(s_dir + "/a.svg", &exists);
    CHECK(exists);
    CHECK(got.size() == 100000 && got[0] == 3 && got[99999] == 3);
    CHECK(s_okCount == 3 && s_badCount == 0);
    bool tmpExists;
    Slurp(s_dir + "/temp.svg", &tmpExists);
    CHECK(!tmpExists);
}

static void LoadAfterSaveSeesNewFile()
{
    AsyncFileWriter w;
    for (int i = 0; i < 20; ++i)
    {
        w.Submit(MakeJob("b.svg", (uint8_t)i, 400000));
        w.WaitIdle(); // what OpenDevice/SaveExists do
        bool exists;
        std::vector<uint8_t> got = Slurp(s_dir + "/b.svg", &exists);
        CHECK(exists && got.size() == 400000 && got[0] == (uint8_t)i && got[399999] == (uint8_t)i);
    }
}

static void NoTornFileWhileWriting()
{
    AsyncFileWriter w;
    w.Submit(MakeJob("c.svg", 7, 64));
    w.WaitIdle();
    std::atomic<bool> stop(false);
    std::atomic<int> torn(0);
    std::thread reader([&] {
        while (!stop.load())
        {
            bool exists;
            std::vector<uint8_t> got = Slurp(s_dir + "/c.svg", &exists);
            // Old (64 B of 7) or new (3 MB of 9); anything else is torn.
            const bool oldFile = got.size() == 64 && got[0] == 7;
            const bool newFile = got.size() == 3000000 && got[0] == 9 && got[2999999] == 9;
            if (!exists || !(oldFile || newFile))
                ++torn;
        }
    });
    for (int i = 0; i < 5; ++i)
    {
        w.Submit(MakeJob("c.svg", 9, 3000000));
        w.WaitIdle();
    }
    stop = true;
    reader.join();
    CHECK(torn.load() == 0);
}

static void FailureKeepsOldFileAndReports()
{
    AsyncFileWriter w;
    w.SetReporter(CountResult);
    s_okCount = s_badCount = 0;
    w.Submit(MakeJob("d.svg", 5, 128));
    w.WaitIdle();
    AsyncFileWriter::Job bad = MakeJob("d.svg", 6, 128);
    bad.tmpPath = s_dir + "/missing-dir/temp.svg";
    w.Submit(std::move(bad));
    w.WaitIdle();
    bool exists;
    std::vector<uint8_t> got = Slurp(s_dir + "/d.svg", &exists);
    CHECK(exists && got.size() == 128 && got[0] == 5);
    CHECK(s_okCount == 1 && s_badCount == 1);
}

static void MakeDirsAndSubmitDoesNotBlock()
{
    AsyncFileWriter w(MakeSubdirs);
    AsyncFileWriter::Job job = MakeJob("sub/e.svg", 4, 1 << 20);
    job.tmpPath = s_dir + "/sub/temp.svg";
    const auto t0 = std::chrono::steady_clock::now();
    w.Submit(std::move(job));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 200);
    w.WaitIdle();
    bool exists;
    CHECK(Slurp(s_dir + "/sub/e.svg", &exists).size() == (1u << 20) && exists);
}

static void DestructorDrains()
{
    {
        AsyncFileWriter w;
        w.Submit(MakeJob("f.svg", 8, 500000));
    }
    bool exists;
    CHECK(Slurp(s_dir + "/f.svg", &exists).size() == 500000);
}

static void BufferRecycle()
{
    AsyncFileWriter w;
    for (int i = 0; i < 4; ++i)
    {
        AsyncFileWriter::Job job = MakeJob("g.svg", 1, 1000);
        job.data = w.AcquireBuffer();
        CHECK(job.data.empty());
        job.data.assign(1000, (uint8_t)i);
        w.Submit(std::move(job));
    }
    w.WaitIdle();
    bool exists;
    CHECK(Slurp(s_dir + "/g.svg", &exists)[0] == 3);
}

int main()
{
    char tmpl[] = "/tmp/asyncwriterXXXXXX";
    if (!mkdtemp(tmpl))
        return 1;
    s_dir = tmpl;
    CompletionAndOrdering();
    LoadAfterSaveSeesNewFile();
    NoTornFileWhileWriting();
    FailureKeepsOldFileAndReports();
    MakeDirsAndSubmitDoesNotBlock();
    DestructorDrains();
    BufferRecycle();
    const std::string cmd = "rm -rf " + s_dir;
    if (std::system(cmd.c_str()) != 0)
        return 1;
    if (s_failures)
        return 1;
    std::printf("PASS:ASYNC_FILE_WRITER\n");
    return 0;
}
