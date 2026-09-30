// Host proof for the OpenAL stream thread (src/sound/snd_stream_openal.cpp).
//
// The module runs against an in-memory filesystem with the engine's FS_*
// contract (retail seek origins and zip-entry seek quirks, 13 stream handles) and a fake OpenAL whose
// sources consume their buffer queues when the test advances "playback" and
// record every sample they play.  That makes the audible result checkable:
// a ramp-valued WAV must come out as an unbroken ramp across loop wraps and
// seeks, whatever the stream thread's timing.
//
// Checks:
//   1. looping WAV from a start frame: exact sample continuity through the
//      loop wrap; the game thread never reads the file beyond its header;
//   2. non-looping WAV plays out, reports ENDED, and its handle is closed on
//      the game thread (SND_StreamServiceMainThread), never the stream thread;
//   3. underrun: consuming more than the queue holds stops the source, the
//      stream thread restarts it and counts the underrun, data stays
//      continuous;
//   4. pause/unpause through want-play: no consumption while paused;
//   5. MP3 without an Xing header (ffmpeg -write_xing 0, VBR): open reports
//      an unknown length, never seeks to END and reads only the head on the
//      game thread; the stream thread counts frames, honours a fractional
//      start, and a second open of the file gets the cached length;
//   6. a start beyond the end of a non-looping stream ends at once;
//   7. stop + restart on one channel (generations) and handle pressure: 13
//      active streams, then a stop/start that needs the stopped stream's
//      handle back;
//   8. a stream restarted on a stopped channel plays from its first frame;
//   9. stop/replace/restart/cancel churn racing the decode never touches a
//      closed handle, never double-closes, closes only on the game thread;
//  10. FS shutdown under running streams: SND_StreamReleaseAll hands every
//      handle back first;
//  11. shutdown joins the thread and closes every handle.
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include <dr_libs/dr_wav.h>
#include <dr_libs/dr_mp3.h>

#include "snd_stream_host_fakes.h"
#include "src/sound/snd_stream_openal.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
int g_failures;
std::thread::id g_mainThread;

#define CHECK(cond)                                                                 \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// --- fake filesystem --------------------------------------------------------
struct FakeFile
{
    std::vector<uint8_t> bytes;
};
struct FakeHandle
{
    bool open;
    const FakeFile *file;
    size_t pos;
};
std::mutex g_fsLock;
std::map<std::string, FakeFile> g_files;
FakeHandle g_handles[14]; // 1..13 usable, like FS_THREAD_STREAM's range
std::atomic<int> g_seekEnd{0};
std::atomic<int> g_closesOffMain{0};
std::atomic<uint64_t> g_mainThreadBytes{0};
// Lifetime violations: FS_Read/FS_Seek/FS_FTell on a handle that is not open
// (the engine's zip handle is gone by then: DataAbort in _read_r) and
// FS_FCloseFile of a handle that is not open (a reused slot closed under its
// new owner).
std::atomic<int> g_useAfterClose{0};
std::atomic<int> g_doubleClose{0};
std::atomic<int> g_readDelayUs{0};

void AddFile(const std::string &name, std::vector<uint8_t> bytes)
{
    g_files[name].bytes = std::move(bytes);
}

int OpenHandleCount()
{
    std::lock_guard<std::mutex> lock(g_fsLock);
    int count = 0;
    for (int h = 1; h < 14; ++h)
        count += g_handles[h].open ? 1 : 0;
    return count;
}

// --- fake OpenAL --------------------------------------------------------------
struct FakeBuffer
{
    std::vector<int16_t> samples; // interleaved
    int channels;
};
struct FakeSource
{
    std::deque<ALuint> queue;
    size_t processed;   // leading entries of `queue` fully played
    size_t offset;      // frames into queue[processed]
    ALint state = AL_INITIAL;
    std::vector<int16_t> played; // channel 0 of everything played
    int plays = 0;
};
std::recursive_mutex g_alLock;
std::map<ALuint, FakeBuffer> g_buffers;
ALuint g_nextBuffer = 1000;
FakeSource g_sources[64];
ALenum g_alError = AL_NO_ERROR;

void SetAlError(ALenum error)
{
    if (g_alError == AL_NO_ERROR)
        g_alError = error;
}

// Plays `frames` frames on every playing source; a source whose queue runs
// dry stops, exactly like a real AL source.
void AdvancePlayback(size_t frames)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    for (FakeSource &src : g_sources)
    {
        if (src.state != AL_PLAYING)
            continue;
        size_t left = frames;
        while (left > 0)
        {
            if (src.processed >= src.queue.size())
            {
                src.state = AL_STOPPED;
                break;
            }
            const FakeBuffer &buf = g_buffers[src.queue[src.processed]];
            const size_t total = buf.samples.size() / buf.channels;
            const size_t take = std::min(left, total - src.offset);
            for (size_t f = 0; f < take; ++f)
                src.played.push_back(buf.samples[(src.offset + f) * buf.channels]);
            src.offset += take;
            left -= take;
            if (src.offset >= total)
            {
                ++src.processed;
                src.offset = 0;
            }
        }
        if (src.processed >= src.queue.size())
            src.state = AL_STOPPED;
    }
}

FakeSource &Source(int index)
{
    return g_sources[index];
}

ALint SrcState(int index)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    return g_sources[index].state;
}

template <typename Pred>
bool WaitFor(Pred pred, int timeoutMs = 3000)
{
    for (int waited = 0; waited < timeoutMs; ++waited)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

SndStreamState StateOf(int index)
{
    SndStreamStatus status;
    SND_StreamPoll(index, &status);
    return status.state;
}

// --- test media -------------------------------------------------------------
// Mono PCM16 whose sample n is (n % 30000) so any gap, repeat or misplaced
// seek shows up in the played sequence.
std::vector<uint8_t> MakeRampWav(uint32_t frames, uint32_t rate)
{
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); };
    auto u16 = [&](uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); };
    const uint32_t dataBytes = frames * 2;
    out.insert(out.end(), { 'R', 'I', 'F', 'F' });
    u32(4 + 8 + 16 + 8 + 12 + 8 + dataBytes);
    out.insert(out.end(), { 'W', 'A', 'V', 'E' });
    // An extra chunk before fmt, like retail's bext/minf/elm1.
    out.insert(out.end(), { 'b', 'e', 'x', 't' });
    u32(12);
    for (int i = 0; i < 12; ++i)
        out.push_back(0);
    out.insert(out.end(), { 'f', 'm', 't', ' ' });
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    out.insert(out.end(), { 'd', 'a', 't', 'a' });
    u32(dataBytes);
    for (uint32_t n = 0; n < frames; ++n)
        u16(uint16_t(int16_t(n % 30000)));
    return out;
}

bool IsRamp(const std::vector<int16_t> &played, size_t from, size_t count, uint32_t firstValue, uint32_t period)
{
    for (size_t i = 0; i < count; ++i)
    {
        const int16_t want = int16_t((firstValue + i) % period % 30000);
        if (played[from + i] != want)
        {
            std::fprintf(stderr, "ramp break at %zu: got %d want %d\n", from + i, played[from + i], want);
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> ReadFile(const char *path)
{
    std::vector<uint8_t> bytes;
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return bytes;
    uint8_t chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
        bytes.insert(bytes.end(), chunk, chunk + got);
    std::fclose(f);
    return bytes;
}
} // namespace

// --- engine surface ---------------------------------------------------------
void Com_Printf(int, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
}

void Com_Error(errorParm_t, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::abort();
}

void I_strncpyz(char *dest, const char *src, int destsize)
{
    std::snprintf(dest, static_cast<size_t>(destsize), "%s", src);
}

unsigned int SND_StreamSource(int index)
{
    return static_cast<unsigned int>(index);
}

uint32_t FS_FOpenFileReadStream(const char *filename, int *file)
{
    std::lock_guard<std::mutex> lock(g_fsLock);
    *file = 0;
    const auto found = g_files.find(filename);
    if (found == g_files.end())
        return 0xFFFFFFFFu;
    for (int h = 1; h < 14; ++h)
    {
        if (!g_handles[h].open)
        {
            g_handles[h] = FakeHandle{ true, &found->second, 0 };
            *file = h;
            return static_cast<uint32_t>(found->second.bytes.size());
        }
    }
    return 0xFFFFFFFFu; // FS_HandleForFile: none free
}

uint32_t FS_Read(uint8_t *buffer, uint32_t len, int h)
{
    if (const int delay = g_readDelayUs.load())
        std::this_thread::sleep_for(std::chrono::microseconds(delay));
    std::lock_guard<std::mutex> lock(g_fsLock);
    FakeHandle &handle = g_handles[h];
    if (!handle.open)
    {
        ++g_useAfterClose;
        return 0xFFFFFFFFu;
    }
    const size_t avail = handle.file->bytes.size() - handle.pos;
    const size_t take = std::min<size_t>(len, avail);
    std::memcpy(buffer, handle.file->bytes.data() + handle.pos, take);
    handle.pos += take;
    if (std::this_thread::get_id() == g_mainThread)
        g_mainThreadBytes += take;
    return static_cast<uint32_t>(take);
}

// Mirrors FS_Seek's zip-entry branch (com_files.cpp) exactly, quirks
// included: every iwd sound is a deflated zip entry.  A seek is a skip
// (reopen first when going backwards) and reports success only when it
// skipped at least one byte, so a seek to the current position "fails";
// offset 0 from SET reopens and succeeds; offset 0 from CUR is a no-op.
int FS_Seek(int f, int offset, int origin)
{
    std::lock_guard<std::mutex> lock(g_fsLock);
    FakeHandle &handle = g_handles[f];
    if (!handle.open)
    {
        ++g_useAfterClose;
        return -1;
    }
    const int64_t size = static_cast<int64_t>(handle.file->bytes.size());
    auto skip = [&](int64_t bytes) -> int64_t {
        const int64_t take = std::max<int64_t>(0, std::min<int64_t>(bytes, size - static_cast<int64_t>(handle.pos)));
        handle.pos += static_cast<size_t>(take);
        return take;
    };
    if (!offset && origin == 2)
    {
        handle.pos = 0;
        return 0;
    }
    if (!offset && !origin)
        return 0;
    const int64_t pos = static_cast<int64_t>(handle.pos);
    int64_t skipped = 0;
    switch (origin)
    {
    case 0:
        if (offset >= 0)
            skipped = skip(offset);
        else
        {
            handle.pos = 0;
            skipped = skip(offset + pos);
        }
        break;
    case 1:
        ++g_seekEnd;
        if (offset + size >= pos)
            skipped = skip(offset + size - pos);
        else
        {
            handle.pos = 0;
            skipped = skip(offset + size);
        }
        break;
    case 2:
        if (offset >= pos)
            skipped = skip(offset - pos);
        else
        {
            handle.pos = 0;
            skipped = skip(offset);
        }
        break;
    default:
        return -1;
    }
    return skipped ? 0 : -1;
}

uint32_t FS_FTell(int f)
{
    std::lock_guard<std::mutex> lock(g_fsLock);
    if (!g_handles[f].open)
        ++g_useAfterClose;
    return static_cast<uint32_t>(g_handles[f].pos);
}

void FS_FCloseFile(int h)
{
    std::lock_guard<std::mutex> lock(g_fsLock);
    if (std::this_thread::get_id() != g_mainThread)
        ++g_closesOffMain;
    if (!g_handles[h].open)
        ++g_doubleClose;
    g_handles[h] = FakeHandle{};
}

void alGenBuffers(ALsizei n, ALuint *buffers)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    for (ALsizei i = 0; i < n; ++i)
    {
        buffers[i] = g_nextBuffer++;
        g_buffers[buffers[i]] = FakeBuffer{ {}, 1 };
    }
}

void alDeleteBuffers(ALsizei n, const ALuint *buffers)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    for (ALsizei i = 0; i < n; ++i)
    {
        for (const FakeSource &src : g_sources)
            for (ALuint queued : src.queue)
                if (queued == buffers[i])
                {
                    SetAlError(AL_INVALID_OPERATION); // still attached
                    return;
                }
        g_buffers.erase(buffers[i]);
    }
}

void alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    FakeBuffer &buf = g_buffers[buffer];
    buf.channels = format == AL_FORMAT_STEREO16 ? 2 : 1;
    buf.samples.assign(static_cast<const int16_t *>(data), static_cast<const int16_t *>(data) + size / 2);
}

void alSourceQueueBuffers(ALuint source, ALsizei n, const ALuint *buffers)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    for (ALsizei i = 0; i < n; ++i)
        g_sources[source].queue.push_back(buffers[i]);
}

void alSourceUnqueueBuffers(ALuint source, ALsizei n, ALuint *buffers)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    FakeSource &src = g_sources[source];
    if (src.state == AL_STOPPED)
        src.processed = src.queue.size();
    if (static_cast<size_t>(n) > src.processed)
    {
        SetAlError(AL_INVALID_VALUE);
        return;
    }
    for (ALsizei i = 0; i < n; ++i)
    {
        buffers[i] = src.queue.front();
        src.queue.pop_front();
    }
    src.processed -= static_cast<size_t>(n);
}

void alSourcei(ALuint source, ALenum param, ALint value)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    FakeSource &src = g_sources[source];
    if (param == AL_BUFFER && value == 0)
    {
        if (src.state == AL_PLAYING || src.state == AL_PAUSED)
        {
            SetAlError(AL_INVALID_OPERATION);
            return;
        }
        // openal-soft keeps the state: a stopped source stays STOPPED with
        // an empty queue (host libopenal and portlib 1.21.1 alike).
        src.queue.clear();
        src.processed = 0;
        src.offset = 0;
    }
}

// A STOPPED source reports every queued buffer as processed, including
// buffers queued onto it after it stopped (OpenAL 1.1; openal-soft).
size_t ProcessedOf(const FakeSource &src)
{
    return src.state == AL_STOPPED ? src.queue.size() : src.processed;
}

void alGetSourcei(ALuint source, ALenum param, ALint *value)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    const FakeSource &src = g_sources[source];
    switch (param)
    {
    case AL_SOURCE_STATE: *value = src.state; break;
    case AL_BUFFERS_QUEUED: *value = static_cast<ALint>(src.queue.size()); break;
    case AL_BUFFERS_PROCESSED: *value = static_cast<ALint>(ProcessedOf(src)); break;
    case AL_SAMPLE_OFFSET: *value = static_cast<ALint>(src.offset); break;
    default: SetAlError(AL_INVALID_VALUE); break;
    }
}

void alSourcePlay(ALuint source)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    FakeSource &src = g_sources[source];
    if (src.state == AL_STOPPED)
    {
        // A replayed stopped source rewinds over everything still queued.
        src.processed = 0;
        src.offset = 0;
    }
    if (src.queue.empty())
    {
        src.state = AL_STOPPED;
        return;
    }
    src.state = AL_PLAYING;
    ++src.plays;
}

void alSourcePause(ALuint source)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    if (g_sources[source].state == AL_PLAYING)
        g_sources[source].state = AL_PAUSED;
}

void alSourceStop(ALuint source)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    FakeSource &src = g_sources[source];
    if (src.state == AL_INITIAL)
        return;
    src.state = AL_STOPPED;
    src.processed = src.queue.size();
    src.offset = 0;
}

ALenum alGetError(void)
{
    std::lock_guard<std::recursive_mutex> lock(g_alLock);
    const ALenum error = g_alError;
    g_alError = AL_NO_ERROR;
    return error;
}

int main(int argc, char **argv)
{
    g_mainThread = std::this_thread::get_id();
    const uint32_t kFrames = 100000;
    AddFile("sound/ramp_loop.wav", MakeRampWav(kFrames, 22050));
    AddFile("sound/ramp_short.wav", MakeRampWav(20000, 22050));

    // 1. Looping WAV from frame 90000: 10000 frames to the end, then the wrap.
    {
        const int ch = 41;
        SndStreamOpenInfo info{};
        g_mainThreadBytes = 0;
        CHECK(SND_StreamOpen(ch, "sound/ramp_loop.wav", false, &info));
        CHECK(info.channels == 1 && info.sampleRate == 22050 && info.totalFrames == kFrames);
        CHECK(g_mainThreadBytes < 256); // header only: no decode or seek on the game thread
        SndStreamStart start{};
        start.frame = 90000;
        SND_StreamBegin(ch, true, start, true);
        CHECK(WaitFor([&] { return StateOf(ch) == SND_STREAM_PLAYING && SrcState(ch) == AL_PLAYING; }));
        for (int step = 0; step < 60; ++step)
        {
            AdvancePlayback(2048);
            std::this_thread::sleep_for(std::chrono::milliseconds(6));
        }
        std::lock_guard<std::recursive_mutex> lock(g_alLock);
        CHECK(Source(ch).played.size() == 60 * 2048);
        CHECK(IsRamp(Source(ch).played, 0, Source(ch).played.size(), 90000, kFrames));
        CHECK(Source(ch).plays == 1); // no underrun restarts
    }
    SND_StreamStop(41);

    // 2. Non-looping WAV plays out and ends; the handle closes on the game thread.
    {
        const int ch = 42;
        SndStreamOpenInfo info{};
        CHECK(SND_StreamOpen(ch, "sound/ramp_short.wav", false, &info));
        SND_StreamBegin(ch, false, SndStreamStart{ 0, 0.0f, 0 }, true);
        CHECK(WaitFor([&] { return SrcState(ch) == AL_PLAYING; }));
        for (int step = 0; step < 40 && StateOf(ch) != SND_STREAM_ENDED; ++step)
        {
            AdvancePlayback(1024);
            std::this_thread::sleep_for(std::chrono::milliseconds(6));
        }
        CHECK(WaitFor([&] { return StateOf(ch) == SND_STREAM_ENDED; }));
        {
            std::lock_guard<std::recursive_mutex> lock(g_alLock);
            CHECK(Source(ch).played.size() == 20000);
            CHECK(IsRamp(Source(ch).played, 0, 20000, 0, 20000));
        }
        const int openBefore = OpenHandleCount();
        SND_StreamStop(ch);
        CHECK(WaitFor([&] { SND_StreamServiceMainThread(); return OpenHandleCount() == openBefore - 1; }));
        CHECK(g_closesOffMain == 0);
    }

    // 3. Underrun: play past the whole queue at once.
    {
        const int ch = 43;
        SndStreamOpenInfo info{};
        CHECK(SND_StreamOpen(ch, "sound/ramp_loop.wav", false, &info));
        SND_StreamBegin(ch, true, SndStreamStart{ 0, 0.0f, 0 }, true);
        CHECK(WaitFor([&] { return SrcState(ch) == AL_PLAYING; }));
        const uint32_t underrunsBefore = SND_StreamUnderrunCount();
        AdvancePlayback(8 * 8192 + 500); // the queue holds 8 x 8192
        CHECK(WaitFor([&] { return SND_StreamUnderrunCount() > underrunsBefore && SrcState(ch) == AL_PLAYING; }));
        AdvancePlayback(4000);
        std::lock_guard<std::recursive_mutex> lock(g_alLock);
        // Starvation loses time, never data: the sequence stays a ramp.
        CHECK(IsRamp(Source(ch).played, 0, Source(ch).played.size(), 0, kFrames));
    }
    SND_StreamStop(43);

    // 4. Pause and resume through want-play.
    {
        const int ch = 44;
        SndStreamOpenInfo info{};
        CHECK(SND_StreamOpen(ch, "sound/ramp_loop.wav", false, &info));
        SND_StreamBegin(ch, true, SndStreamStart{ 0, 0.0f, 0 }, false);
        CHECK(WaitFor([&] { return StateOf(ch) == SND_STREAM_PLAYING; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(SrcState(ch) != AL_PLAYING); // prefilled but held
        SND_StreamSetWantPlay(ch, true);
        CHECK(WaitFor([&] { return SrcState(ch) == AL_PLAYING; }));
        AdvancePlayback(1000);
        SND_StreamSetWantPlay(ch, false);
        CHECK(WaitFor([&] { return SrcState(ch) == AL_PAUSED; }));
        AdvancePlayback(5000);
        {
            std::lock_guard<std::recursive_mutex> lock(g_alLock);
            CHECK(Source(ch).played.size() == 1000);
        }
        SND_StreamSetWantPlay(ch, true);
        CHECK(WaitFor([&] { return SrcState(ch) == AL_PLAYING; }));
        AdvancePlayback(1000);
        std::lock_guard<std::recursive_mutex> lock(g_alLock);
        CHECK(IsRamp(Source(ch).played, 0, 2000, 0, kFrames));
    }
    SND_StreamStop(44);

    // 5. MP3 without an Xing header.
    {
        const char *mp3Path = argc > 1 ? argv[1] : nullptr;
        std::vector<uint8_t> mp3 = mp3Path ? ReadFile(mp3Path) : std::vector<uint8_t>{};
        CHECK(!mp3.empty());
        if (!mp3.empty())
        {
            AddFile("sound/music.mp3", mp3);
            const int ch = 45;
            SndStreamOpenInfo info{};
            g_seekEnd = 0;
            g_mainThreadBytes = 0;
            CHECK(SND_StreamOpen(ch, "sound/music.mp3", true, &info));
            CHECK(info.totalFrames == -1);          // unknown: counted off the game thread
            CHECK(g_seekEnd == 0);                  // no ID3v1/APE probe at the end
            // Head only: dr_mp3 reads one data chunk to find the first frame;
            // the whole-file frame count happens on the stream thread.
            std::printf("mp3 game-thread bytes at open: %llu of %zu\n",
                        (unsigned long long)g_mainThreadBytes.load(), mp3.size());
            CHECK(g_mainThreadBytes <= 128u * 1024u && g_mainThreadBytes < mp3.size());
            SndStreamStart start{};
            start.frame = -1;
            start.fraction = 0.5f;
            SND_StreamBegin(ch, true, start, true);
            CHECK(WaitFor([&] { return StateOf(ch) == SND_STREAM_PLAYING; }));
            SndStreamStatus status;
            SND_StreamPoll(ch, &status);
            CHECK(status.totalFrames > 44100);
            CHECK(status.startFrame == status.totalFrames / 2);
            SND_StreamStop(ch);
            CHECK(WaitFor([&] { SND_StreamServiceMainThread(); return StateOf(ch) == SND_STREAM_NONE; }));
            SndStreamOpenInfo again{};
            CHECK(SND_StreamOpen(ch, "sound/music.mp3", true, &again));
            CHECK(again.totalFrames == status.totalFrames); // cached
            SND_StreamCancel(ch);
        }
    }

    // 6. A start past the end of a non-looping stream ends at once.
    {
        const int ch = 46;
        SndStreamOpenInfo info{};
        CHECK(SND_StreamOpen(ch, "sound/ramp_short.wav", false, &info));
        SND_StreamBegin(ch, false, SndStreamStart{ -1, 0.0f, 5000 }, true); // 5 s > 20000 frames @22050
        CHECK(WaitFor([&] { return StateOf(ch) == SND_STREAM_ENDED; }));
        SND_StreamStop(ch);
    }

    // 7. Restart on one channel, then every stream handle in use.
    {
        SndStreamOpenInfo info{};
        for (int round = 0; round < 5; ++round)
        {
            CHECK(SND_StreamOpen(47, "sound/ramp_loop.wav", false, &info));
            SND_StreamBegin(47, true, SndStreamStart{ round * 1000, 0.0f, 0 }, true);
        }
        CHECK(WaitFor([&] { SndStreamStatus s; SND_StreamPoll(47, &s); return s.state == SND_STREAM_PLAYING && s.startFrame == 4000; }));
        for (int ch = 40; ch < 53; ++ch)
        {
            if (SND_StreamIsActive(ch))
                continue;
            CHECK(SND_StreamOpen(ch, "sound/ramp_loop.wav", false, &info));
            SND_StreamBegin(ch, true, SndStreamStart{ 0, 0.0f, 0 }, true);
        }
        CHECK(WaitFor([&] { SND_StreamServiceMainThread(); return OpenHandleCount() == 13; }));
        // All 13 handles busy: this restart needs channel 50's own handle back.
        SND_StreamStop(50);
        CHECK(SND_StreamOpen(50, "sound/ramp_short.wav", false, &info));
        SND_StreamBegin(50, false, SndStreamStart{ 0, 0.0f, 0 }, true);
        CHECK(WaitFor([&] { return StateOf(50) == SND_STREAM_PLAYING; }));
    }

    // 8. A stream started on a channel whose previous stream stopped plays
    //    from its first frame to its last. The source is STOPPED when the new
    //    job queues its prefill, and a STOPPED source reports all of it
    //    processed: taken at its word, a short line (all of it in the
    //    prefill) ended unheard and a long one lost its first 8 buffers
    //    (SND_STOPDBG showed age << len).
    for (int ch = 40; ch < 53; ++ch)
        SND_StreamStop(ch);
    CHECK(WaitFor([&] { SND_StreamServiceMainThread(); return OpenHandleCount() == 0; }));
    {
        const int ch = 48;
        SndStreamOpenInfo info{};
        CHECK(SND_StreamOpen(ch, "sound/ramp_short.wav", false, &info));
        SND_StreamBegin(ch, false, SndStreamStart{ 0, 0.0f, 0 }, true);
        CHECK(WaitFor([&] { return SrcState(ch) == AL_PLAYING; }));
        for (int step = 0; step < 40 && StateOf(ch) != SND_STREAM_ENDED; ++step)
        {
            AdvancePlayback(1024);
            std::this_thread::sleep_for(std::chrono::milliseconds(6));
        }
        CHECK(WaitFor([&] { return StateOf(ch) == SND_STREAM_ENDED; }));
        SND_StreamStop(ch);
        for (const char *name : { "sound/ramp_short.wav", "sound/ramp_loop.wav" })
        {
            const uint32_t frames = name[11] == 's' ? 20000 : kFrames;
            {
                std::lock_guard<std::recursive_mutex> lock(g_alLock);
                Source(ch).played.clear();
            }
            CHECK(SND_StreamOpen(ch, name, false, &info));
            SND_StreamBegin(ch, false, SndStreamStart{ 0, 0.0f, 0 }, true);
            CHECK(WaitFor([&] { return StateOf(ch) == SND_STREAM_PLAYING; }));
            std::this_thread::sleep_for(std::chrono::milliseconds(30)); // several stream ticks before any playback
            CHECK(StateOf(ch) != SND_STREAM_ENDED);
            for (int step = 0; step < 400 && StateOf(ch) != SND_STREAM_ENDED; ++step)
            {
                AdvancePlayback(1024);
                std::this_thread::sleep_for(std::chrono::milliseconds(6));
            }
            CHECK(WaitFor([&] { return StateOf(ch) == SND_STREAM_ENDED; }));
            std::lock_guard<std::recursive_mutex> lock(g_alLock);
            std::printf("restart on stopped channel: %s played %zu of %u frames\n", name, Source(ch).played.size(), frames);
            CHECK(Source(ch).played.size() == frames);
            CHECK(Source(ch).played.size() == frames && IsRamp(Source(ch).played, 0, frames, 0, frames));
            SND_StreamStop(ch);
        }
    }

    // 9. Stop / replace / restart / cancel racing the decode on every
    //    channel (WAV and MP3, reads slowed so decodes are mid-flight when the
    //    game thread acts).  The stream thread owns a job's handle until it
    //    retires the job; the game thread closes it only after that.  Any FS
    //    call on a closed handle, a close of a closed handle or a close off
    //    the game thread fails.
    for (int ch = 40; ch < 53; ++ch)
        SND_StreamStop(ch);
    CHECK(WaitFor([&] { SND_StreamServiceMainThread(); return OpenHandleCount() == 0; }));
    {
        const bool haveMp3 = g_files.count("sound/music.mp3") != 0;
        g_useAfterClose = 0;
        g_doubleClose = 0;
        g_readDelayUs = 20;
        uint32_t rng = 0x5eed1234u;
        auto next = [&] { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        int begins = 0, stops = 0, cancels = 0;
        for (int op = 0; op < 3000; ++op)
        {
            const int ch = 40 + static_cast<int>(next() % 13);
            const uint32_t kind = next() % 8;
            if (kind <= 2)
            {
                const uint32_t pick = next() % 20;
                const bool mp3 = haveMp3 && pick == 0;
                const char *name = mp3 ? "sound/music.mp3" : (pick & 1) ? "sound/ramp_short.wav" : "sound/ramp_loop.wav";
                SndStreamOpenInfo info{};
                if (SND_StreamOpen(ch, name, mp3, &info))
                {
                    SndStreamStart start{ mp3 ? -1 : static_cast<int64_t>(next() % 30000), mp3 ? 0.25f : 0.0f, 0 };
                    SND_StreamBegin(ch, (next() & 1) != 0, start, (next() % 4) != 0);
                    ++begins;
                }
            }
            else if (kind == 3)
            {
                SND_StreamStop(ch);
                ++stops;
            }
            else if (kind == 4)
            {
                SndStreamOpenInfo info{};
                if (SND_StreamOpen(ch, "sound/ramp_loop.wav", false, &info))
                {
                    SND_StreamCancel(ch);
                    ++cancels;
                }
            }
            else if (kind == 5)
            {
                SND_StreamSetWantPlay(ch, (next() & 1) != 0);
            }
            else if (kind == 6)
            {
                SND_StreamServiceMainThread();
            }
            else
            {
                AdvancePlayback(next() % 4096);
            }
            if ((op & 15) == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (int ch = 40; ch < 53; ++ch)
            SND_StreamStop(ch);
        CHECK(WaitFor([&] { SND_StreamServiceMainThread(); return OpenHandleCount() == 0; }, 10000));
        g_readDelayUs = 0;
        std::printf("stream churn: begins=%d stops=%d cancels=%d use_after_close=%d double_close=%d closes_off_main=%d\n",
                    begins, stops, cancels, g_useAfterClose.load(), g_doubleClose.load(), g_closesOffMain.load());
        CHECK(begins > 500 && stops > 200);
        CHECK(g_useAfterClose == 0);
        CHECK(g_doubleClose == 0);
        CHECK(g_closesOffMain == 0);
    }

    // 10. The filesystem shuts down (FS_Shutdown: FS_Restart, quit) while
    //     streams decode.  FS_Shutdown closes every open handle; before that
    //     SND_StreamReleaseAll must take every stream handle back from the
    //     stream thread and close it, so nothing reads a closed handle and the
    //     stream thread's deferred close does not close a slot again (red
    //     before: FS_Shutdown on Switch left the streams running).
    {
        const bool haveMp3 = g_files.count("sound/music.mp3") != 0;
        g_useAfterClose = 0;
        g_doubleClose = 0;
        for (int ch = 40; ch < 46; ++ch)
        {
            const bool mp3 = haveMp3 && ch == 40;
            SndStreamOpenInfo info{};
            CHECK(SND_StreamOpen(ch, mp3 ? "sound/music.mp3" : "sound/ramp_loop.wav", mp3, &info));
            SND_StreamBegin(ch, true, SndStreamStart{ 0, 0.0f, 0 }, true);
        }
        CHECK(WaitFor([&] {
            for (int ch = 40; ch < 46; ++ch)
                if (StateOf(ch) != SND_STREAM_PLAYING)
                    return false;
            return true;
        }));
        g_readDelayUs = 100;
        for (int step = 0; step < 5; ++step)
        {
            AdvancePlayback(8192);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // FS_Shutdown: hand the stream handles back, then close whatever is
        // still open.
        SND_StreamReleaseAll();
        for (int h = 1; h < 14; ++h)
        {
            bool open;
            {
                std::lock_guard<std::mutex> lock(g_fsLock);
                open = g_handles[h].open;
            }
            if (open)
                FS_FCloseFile(h);
        }
        for (int step = 0; step < 20; ++step)
        {
            AdvancePlayback(8192);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (int ch = 40; ch < 53; ++ch)
            SND_StreamStop(ch);
        SND_StreamShutdown();
        g_readDelayUs = 0;
        std::printf("fs shutdown under streams: use_after_close=%d double_close=%d\n", g_useAfterClose.load(),
                    g_doubleClose.load());
        CHECK(g_useAfterClose == 0);
        CHECK(g_doubleClose == 0);
        for (int ch = 40; ch < 53; ++ch)
            CHECK(!SND_StreamIsActive(ch));
    }

    // 11. Shutdown.
    SND_StreamShutdown();
    CHECK(OpenHandleCount() == 0);
    CHECK(g_closesOffMain == 0);
    for (int ch = 40; ch < 53; ++ch)
        CHECK(!SND_StreamIsActive(ch));

    if (g_failures)
    {
        std::printf("FAIL:SND_STREAM_THREAD failures=%d\n", g_failures);
        return 1;
    }
    std::printf("PASS:SND_STREAM_THREAD\n");
    return 0;
}
