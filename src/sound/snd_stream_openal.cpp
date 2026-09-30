#ifdef KISAK_OPENAL
// Streamed sounds on a dedicated thread; see snd_stream_openal.h for the
// ownership split between the game thread and the stream thread.
// The engine surface this file uses is small on purpose: the stream sources
// (SND_StreamSource), FS_* stream reads, Com_Printf/Com_Error and the AL
// source/buffer API.  The host proof (switch_snd_stream_test.cpp) swaps all
// of it for fakes that model an AL source consuming its queue.
#ifdef KISAK_SND_STREAM_HOST_TEST
#include "snd_stream_host_fakes.h"
#else
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <universal/com_files.h>
#include <AL/al.h>
#endif
#include "snd_stream_openal.h"
#include "snd_al_dispatch.h"

#include <dr_libs/dr_wav.h>
#include <dr_libs/dr_mp3.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <port/switch_perf.h>
#ifdef __SWITCH__
#include <switch.h>
#include <port/switch_pcsample.h>
#else
#include <thread>
#endif

namespace
{
constexpr int kMaxChannels = SND_STREAM_CHANNEL_END;
constexpr int kFirstStream = SND_STREAM_FIRST_CHANNEL;
// 8 x 8192 frames is ~1.5 s at 44.1 kHz.  The stream thread refills every few
// milliseconds, so the depth only has to cover the stream thread itself being
// busy (an MP3 length scan or a deflated seek on another channel).
constexpr int kBufferCount = 8;
constexpr int kBufferFrames = 8192;
constexpr int kTickMs = 4;

struct StreamJob
{
    int index;
    uint32_t gen;
    int fsHandle;
    bool isMp3;
    bool looping;
    drwav wav;
    drmp3 mp3;
    uint32_t channels;
    uint32_t sampleRate;
    int64_t totalFrames;
    SndStreamStart start;
    uint64_t framesQueued;
    bool exhausted;
    bool loaded;
    bool everPlayed;
    bool ended;
    ALuint buffers[kBufferCount];
    ALuint freeBuffers[kBufferCount];
    int freeCount;
    char name[96];
};

// --- decoder I/O over the engine filesystem --------------------------------
// FS_Seek (zip and loose files alike, see FileWrapper_Seek) takes the retail
// origin enum: 0 = current, 1 = end, 2 = set.  dr_wav/dr_mp3 pass SET/CUR/END
// as 0/1/2; every seek is issued as an absolute retail SET.  SEEK_END is
// refused: the only user is dr_mp3's ID3v1/APE tag probe, which on a deflated
// iwd entry inflates the whole file twice.
size_t StreamRead(void *user, void *out, size_t bytes)
{
    StreamJob *job = static_cast<StreamJob *>(user);
    const uint32_t got = FS_Read(static_cast<uint8_t *>(out), static_cast<uint32_t>(bytes), job->fsHandle);
    return got == static_cast<uint32_t>(-1) ? 0 : got;
}

bool StreamSeek(StreamJob *job, int offset, int origin)
{
    if (origin < 0 || origin > 1)
        return false;
    // FS_Seek on a zip entry is a skip that reports success only when it
    // skipped a byte, so a seek to the current position comes back as a
    // failure (dr_wav/dr_mp3 re-seek to where they already are).  Resolve
    // the target here and treat "already there" as done.
    const int64_t here = FS_FTell(job->fsHandle);
    const int64_t target = origin == 0 ? offset : here + offset;
    if (target < 0)
        return false;
    if (target == here)
        return true;
    return FS_Seek(job->fsHandle, static_cast<int>(target), 2) == 0; // 2 = retail SET
}

drwav_bool32 WavSeek(void *user, int offset, drwav_seek_origin origin)
{
    return StreamSeek(static_cast<StreamJob *>(user), offset, static_cast<int>(origin)) ? DRWAV_TRUE : DRWAV_FALSE;
}

drwav_bool32 WavTell(void *user, drwav_int64 *cursor)
{
    *cursor = FS_FTell(static_cast<StreamJob *>(user)->fsHandle);
    return DRWAV_TRUE;
}

drmp3_bool32 Mp3Seek(void *user, int offset, drmp3_seek_origin origin)
{
    return StreamSeek(static_cast<StreamJob *>(user), offset, static_cast<int>(origin)) ? DRMP3_TRUE : DRMP3_FALSE;
}

drmp3_bool32 Mp3Tell(void *user, drmp3_int64 *cursor)
{
    *cursor = FS_FTell(static_cast<StreamJob *>(user)->fsHandle);
    return DRMP3_TRUE;
}

// --- shared state -----------------------------------------------------------
enum CmdType
{
    CMD_BEGIN,
    CMD_STOP,
};

struct Cmd
{
    CmdType type;
    int index;
    uint32_t gen;
    StreamJob *job;
};

constexpr int kCmdRing = 128;
std::mutex s_cmdLock;
Cmd s_cmds[kCmdRing];
uint32_t s_cmdHead; // next write
uint32_t s_cmdTail; // next read
std::atomic<uint32_t> s_cmdPosted{0};
std::atomic<uint32_t> s_cmdDone{0};

std::mutex s_statusLock;
SndStreamStatus s_status[kMaxChannels];
uint32_t s_statusGen[kMaxChannels];
std::atomic<bool> s_wantPlay[kMaxChannels];

// File handles the stream thread is done with; closed on the game thread so
// every fsh[] table mutation stays there (FS_HandleForFile scans the table
// without a lock).
std::mutex s_closeLock;
int s_closeHandles[64];
int s_closeCount;

std::mutex s_lengthLock;
std::unordered_map<std::string, int64_t> s_mp3Lengths;

// Game-thread state.
StreamJob *s_pending[kMaxChannels];
bool s_mainActive[kMaxChannels];
uint32_t s_mainGen[kMaxChannels];

// Stream-thread state.
StreamJob *s_cur[kMaxChannels];

std::atomic<bool> s_quit{false};
bool s_threadStarted;
#ifdef __SWITCH__
Thread s_thread;
#else
std::thread s_thread;
#endif

// Perf (switch_perfTrace): written by the stream thread, read once a second.
struct StreamPerf
{
    uint32_t starts;
    uint64_t openUs;      // game thread: open + header
    uint64_t loadUs;      // stream thread: length scan + seek + prefill
    uint64_t loadMaxUs;
    char slowest[96];
    uint32_t fills;
    uint64_t fillUs;
    uint64_t fillMaxUs;
    uint32_t underruns;
    uint32_t alErrors;
    int maxTickGapMs;
    int minQueuedMs;
    int lastPrintMs;
};
std::mutex s_perfLock;
StreamPerf MakePerf()
{
    StreamPerf perf{};
    perf.minQueuedMs = -1; // no sample yet
    return perf;
}
StreamPerf s_perf = MakePerf();
std::atomic<uint32_t> s_totalUnderruns{0};

uint64_t NowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool PerfEnabled()
{
#ifdef __SWITCH__
    return SwitchPerf_g_enabled != 0;
#else
    return false;
#endif
}

void PublishStatus(int index, uint32_t gen, SndStreamState state, const StreamJob *job)
{
    std::lock_guard<std::mutex> lock(s_statusLock);
    s_statusGen[index] = gen;
    s_status[index].state = state;
    if (job)
    {
        s_status[index].totalFrames = job->totalFrames;
        s_status[index].startFrame = job->start.frame;
        s_status[index].framesQueued = job->framesQueued;
    }
}

void PublishQueued(int index, uint32_t gen, uint64_t framesQueued)
{
    std::lock_guard<std::mutex> lock(s_statusLock);
    if (s_statusGen[index] == gen)
        s_status[index].framesQueued = framesQueued;
}

void QueueClose(int fsHandle)
{
    if (!fsHandle)
        return;
    std::lock_guard<std::mutex> lock(s_closeLock);
    if (s_closeCount < static_cast<int>(sizeof(s_closeHandles) / sizeof(s_closeHandles[0])))
        s_closeHandles[s_closeCount++] = fsHandle;
}

void UninitDecoder(StreamJob *job)
{
    if (job->isMp3)
        drmp3_uninit(&job->mp3);
    else
        drwav_uninit(&job->wav);
}

// Stream thread: stop the source and free everything the job owns.
void ReleaseJob(StreamJob *job)
{
    const ALuint source = SND_StreamSource(job->index);
    alSourceStop(source);
    alSourcei(source, AL_BUFFER, 0);
    if (job->loaded)
        alDeleteBuffers(kBufferCount, job->buffers);
    UninitDecoder(job);
    QueueClose(job->fsHandle);
    delete job;
}

uint64_t DecodeFrames(StreamJob *job, int16_t *out, uint64_t frames)
{
    return job->isMp3 ? drmp3_read_pcm_frames_s16(&job->mp3, frames, out)
                      : drwav_read_pcm_frames_s16(&job->wav, frames, out);
}

bool SeekFrame(StreamJob *job, uint64_t frame)
{
    return job->isMp3 ? drmp3_seek_to_pcm_frame(&job->mp3, frame) != DRMP3_FALSE
                      : drwav_seek_to_pcm_frame(&job->wav, frame) != DRWAV_FALSE;
}

// Decodes one chunk into `buffer`; handles the loop wrap.  Returns false when
// a non-looping stream has nothing left.
bool FillBuffer(StreamJob *job, ALuint buffer)
{
    static int16_t chunk[kBufferFrames * 2];
    uint64_t frames = DecodeFrames(job, chunk, kBufferFrames);
    if (frames == 0)
    {
        if (!job->looping || !SeekFrame(job, 0))
        {
            job->exhausted = true;
            return false;
        }
        job->framesQueued = 0;
        frames = DecodeFrames(job, chunk, kBufferFrames);
        if (frames == 0)
        {
            job->exhausted = true; // zero-length file: never spin
            return false;
        }
    }
    const ALenum format = job->channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
    alBufferData(buffer, format, chunk, static_cast<ALsizei>(frames * job->channels * sizeof(int16_t)),
                 static_cast<ALsizei>(job->sampleRate));
    alSourceQueueBuffers(SND_StreamSource(job->index), 1, &buffer);
    job->framesQueued += frames;
    return true;
}

void LoadJob(StreamJob *job)
{
    const uint64_t t0 = NowUs();
    if (job->totalFrames < 0)
    {
        // No Xing/Info header (none of the retail MP3s has one): count frames
        // once and remember it for the next start of the same file.
        job->totalFrames = static_cast<int64_t>(drmp3_get_pcm_frame_count(&job->mp3));
        std::lock_guard<std::mutex> lock(s_lengthLock);
        s_mp3Lengths[job->name] = job->totalFrames;
    }
    if (job->totalFrames <= 0)
    {
        PublishStatus(job->index, job->gen, SND_STREAM_FAILED, job);
        return;
    }

    int64_t startFrame = job->start.frame;
    if (startFrame < 0)
    {
        if (job->start.fraction > 0.0f)
            startFrame = static_cast<int64_t>(static_cast<double>(job->start.fraction) * job->totalFrames);
        else
            startFrame = static_cast<int64_t>(job->start.nativeMsec) * job->sampleRate / 1000;
    }
    if (startFrame >= job->totalFrames)
    {
        if (!job->looping)
        {
            job->start.frame = startFrame;
            PublishStatus(job->index, job->gen, SND_STREAM_ENDED, job);
            job->ended = true;
            return;
        }
        startFrame %= job->totalFrames;
    }
    if (startFrame > 0 && !SeekFrame(job, static_cast<uint64_t>(startFrame)))
    {
        startFrame = 0;
        if (!SeekFrame(job, 0))
        {
            PublishStatus(job->index, job->gen, SND_STREAM_FAILED, job);
            return;
        }
    }
    job->start.frame = startFrame;
    job->framesQueued = static_cast<uint64_t>(startFrame);

    alGenBuffers(kBufferCount, job->buffers);
    job->loaded = true;
    job->freeCount = 0;
    for (int i = 0; i < kBufferCount; ++i)
    {
        if (!FillBuffer(job, job->buffers[i]))
        {
            for (int j = i; j < kBufferCount; ++j)
                job->freeBuffers[job->freeCount++] = job->buffers[j];
            break;
        }
    }
    PublishStatus(job->index, job->gen, SND_STREAM_PLAYING, job);

    if (PerfEnabled())
    {
        const uint64_t spent = NowUs() - t0;
        std::lock_guard<std::mutex> lock(s_perfLock);
        s_perf.loadUs += spent;
        if (spent > s_perf.loadMaxUs)
        {
            s_perf.loadMaxUs = spent;
            I_strncpyz(s_perf.slowest, job->name, sizeof(s_perf.slowest));
        }
    }
}

void ServiceJob(StreamJob *job)
{
    if (!job->loaded || job->ended)
        return;
    const bool perf = PerfEnabled();
    const uint64_t t0 = perf ? NowUs() : 0;
    const ALuint source = SND_StreamSource(job->index);

    // A STOPPED source reports every queued buffer as processed, including
    // the prefill this job queued onto it after the previous stream on the
    // channel stopped it (OpenAL 1.1, openal-soft, the audren model alike).
    // Nothing is processed before this job's first play: trusting the count
    // there unqueued the prefill unheard, so a line that fit in it ended
    // silently and a longer one lost its first kBufferCount buffers
    // (switch_snd_stream_test case 8).
    ALint processed = 0;
    if (job->everPlayed)
        alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0 && job->freeCount < kBufferCount)
    {
        ALuint buffer = 0;
        alSourceUnqueueBuffers(source, 1, &buffer);
        job->freeBuffers[job->freeCount++] = buffer;
    }

    ALint queued = 0;
    alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
    if (perf && queued > 0 && job->sampleRate > 0)
    {
        ALint offset = 0;
        alGetSourcei(source, AL_SAMPLE_OFFSET, &offset);
        const int64_t ahead = static_cast<int64_t>(queued) * kBufferFrames - offset;
        const int aheadMs = ahead > 0 ? static_cast<int>(ahead * 1000 / job->sampleRate) : 0;
        std::lock_guard<std::mutex> lock(s_perfLock);
        if (s_perf.minQueuedMs < 0 || aheadMs < s_perf.minQueuedMs)
            s_perf.minQueuedMs = aheadMs;
    }

    bool filled = false;
    while (job->freeCount > 0 && !job->exhausted)
    {
        const ALuint buffer = job->freeBuffers[job->freeCount - 1];
        if (!FillBuffer(job, buffer))
            break;
        --job->freeCount;
        ++queued;
        filled = true;
    }

    const bool want = s_wantPlay[job->index].load(std::memory_order_acquire);
    ALint state = AL_INITIAL;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    if (want)
    {
        if (state != AL_PLAYING)
        {
            if (queued > 0)
            {
                // A source that ran its queue dry stops by itself; queuing more
                // never restarts it.
                if (job->everPlayed && state == AL_STOPPED)
                {
                    s_totalUnderruns.fetch_add(1, std::memory_order_relaxed);
                    if (perf)
                    {
                        std::lock_guard<std::mutex> lock(s_perfLock);
                        ++s_perf.underruns;
                    }
                }
                alSourcePlay(source);
                job->everPlayed = true;
            }
            else if (job->exhausted)
            {
                job->ended = true;
                PublishStatus(job->index, job->gen, SND_STREAM_ENDED, job);
                return;
            }
        }
    }
    else if (state == AL_PLAYING)
    {
        alSourcePause(source);
    }

    const bool alError = alGetError() != AL_NO_ERROR;
    PublishQueued(job->index, job->gen, job->framesQueued);
    if (perf && (filled || alError))
    {
        const uint64_t spent = NowUs() - t0;
        std::lock_guard<std::mutex> lock(s_perfLock);
        if (filled)
        {
            ++s_perf.fills;
            s_perf.fillUs += spent;
            if (spent > s_perf.fillMaxUs)
                s_perf.fillMaxUs = spent;
        }
        if (alError)
            ++s_perf.alErrors;
    }
}

void ProcessCommands()
{
    Cmd local[kCmdRing];
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(s_cmdLock);
        while (s_cmdTail != s_cmdHead)
            local[count++] = s_cmds[s_cmdTail++ % kCmdRing];
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        const Cmd &cmd = local[i];
        if (cmd.type == CMD_BEGIN)
        {
            if (s_cur[cmd.index])
                ReleaseJob(s_cur[cmd.index]);
            s_cur[cmd.index] = cmd.job;
            LoadJob(cmd.job);
            // Keep every other stream fed while a slow load ran.
            for (int c = kFirstStream; c < kMaxChannels; ++c)
                if (s_cur[c] && c != cmd.index)
                    ServiceJob(s_cur[c]);
        }
        else if (s_cur[cmd.index] && s_cur[cmd.index]->gen == cmd.gen)
        {
            ReleaseJob(s_cur[cmd.index]);
            s_cur[cmd.index] = nullptr;
            PublishStatus(cmd.index, cmd.gen, SND_STREAM_NONE, nullptr);
        }
        s_cmdDone.fetch_add(1, std::memory_order_release);
    }
}

void StreamThreadMain(void *)
{
#ifdef __SWITCH__
    SwitchPcSample_RegisterCurrentThread(kSwitchPcSampleTagSndStream);
#endif
    uint64_t lastTick = NowUs();
    while (!s_quit.load(std::memory_order_acquire))
    {
        const uint64_t now = NowUs();
        if (PerfEnabled())
        {
            const int gapMs = static_cast<int>((now - lastTick) / 1000);
            std::lock_guard<std::mutex> lock(s_perfLock);
            if (gapMs > s_perf.maxTickGapMs)
                s_perf.maxTickGapMs = gapMs;
        }
        lastTick = now;
        ProcessCommands();
        for (int c = kFirstStream; c < kMaxChannels; ++c)
            if (s_cur[c])
                ServiceJob(s_cur[c]);
#ifdef __SWITCH__
        svcSleepThread(static_cast<int64_t>(kTickMs) * 1000000);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
#endif
    }
    ProcessCommands();
    for (int c = kFirstStream; c < kMaxChannels; ++c)
    {
        if (s_cur[c])
        {
            ReleaseJob(s_cur[c]);
            s_cur[c] = nullptr;
        }
    }
}

void EnsureThread()
{
    if (s_threadStarted)
        return;
    s_quit.store(false, std::memory_order_release);
#ifdef __SWITCH__
    // Core 2 beside the audren update thread (0x20): below it, above the
    // renderer worker (0x3B) and the game thread (0x2C), so a refill is never
    // starved by frame work.
    if (R_FAILED(threadCreate(&s_thread, StreamThreadMain, nullptr, nullptr, 0x20000, 0x2A, 2)) ||
        R_FAILED(threadStart(&s_thread)))
        Com_Error(ERR_FATAL, "SND: stream thread creation failed");
#else
    s_thread = std::thread(StreamThreadMain, nullptr);
#endif
    s_threadStarted = true;
}

void PostCmd(const Cmd &cmd)
{
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(s_cmdLock);
            if (s_cmdHead - s_cmdTail < static_cast<uint32_t>(kCmdRing))
            {
                s_cmds[s_cmdHead++ % kCmdRing] = cmd;
                s_cmdPosted.fetch_add(1, std::memory_order_release);
                return;
            }
        }
#ifdef __SWITCH__
        svcSleepThread(1000000);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
}

// Waits (bounded) until the stream thread has processed everything posted.
// Returns false on timeout.
bool SyncThread(int maxMs)
{
    if (!s_threadStarted)
        return true;
    const uint32_t target = s_cmdPosted.load(std::memory_order_acquire);
    for (int waited = 0; waited < maxMs; ++waited)
    {
        if (static_cast<int32_t>(s_cmdDone.load(std::memory_order_acquire) - target) >= 0)
            return true;
#ifdef __SWITCH__
        svcSleepThread(1000000);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
    return static_cast<int32_t>(s_cmdDone.load(std::memory_order_acquire) - target) >= 0;
}

bool ValidIndex(int index)
{
    return index >= kFirstStream && index < kMaxChannels;
}

// Stops the stream running on `index` (if any); a pending open is untouched.
void StopActive(int index)
{
    if (!s_mainActive[index])
        return;
    s_mainActive[index] = false;
    s_wantPlay[index].store(false, std::memory_order_release);
    PostCmd(Cmd{ CMD_STOP, index, s_mainGen[index], nullptr });
}
} // namespace

void SND_StreamServiceMainThread()
{
    int handles[64];
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(s_closeLock);
        count = s_closeCount;
        std::memcpy(handles, s_closeHandles, sizeof(int) * count);
        s_closeCount = 0;
    }
    for (int i = 0; i < count; ++i)
        FS_FCloseFile(handles[i]);
}

bool SND_StreamOpen(int index, const char *realname, bool isMp3, SndStreamOpenInfo *out)
{
    if (!ValidIndex(index) || !realname || !out)
        return false;
    const uint64_t t0 = NowUs();
    SND_StreamCancel(index);
    SND_StreamServiceMainThread();

    int fsHandle = 0;
    bool opened = (FS_FOpenFileReadStream(realname, &fsHandle) & 0x80000000) == 0 && fsHandle;
    if (!opened)
    {
        // Every stream handle may be held by streams the stream thread has
        // not finished stopping yet: let it catch up, return them, retry.
        SyncThread(100);
        SND_StreamServiceMainThread();
        fsHandle = 0;
        opened = (FS_FOpenFileReadStream(realname, &fsHandle) & 0x80000000) == 0 && fsHandle;
        if (!opened)
            return false;
    }

    StreamJob *job = new StreamJob();
    job->index = index;
    job->fsHandle = fsHandle;
    job->isMp3 = isMp3;
    I_strncpyz(job->name, realname, sizeof(job->name));
    const bool ok = isMp3 ? drmp3_init(&job->mp3, StreamRead, Mp3Seek, Mp3Tell, NULL, job, NULL) != DRMP3_FALSE
                          : drwav_init(&job->wav, StreamRead, WavSeek, WavTell, job, NULL) != DRWAV_FALSE;
    if (!ok)
    {
        FS_FCloseFile(fsHandle);
        delete job;
        return false;
    }
    job->channels = isMp3 ? job->mp3.channels : job->wav.channels;
    job->sampleRate = isMp3 ? job->mp3.sampleRate : job->wav.sampleRate;
    if (job->channels < 1 || job->channels > 2 || job->sampleRate == 0)
    {
        UninitDecoder(job);
        FS_FCloseFile(fsHandle);
        delete job;
        return false;
    }
    if (isMp3)
    {
        std::lock_guard<std::mutex> lock(s_lengthLock);
        const auto found = s_mp3Lengths.find(job->name);
        job->totalFrames = found != s_mp3Lengths.end() ? found->second : -1;
    }
    else
    {
        job->totalFrames = static_cast<int64_t>(job->wav.totalPCMFrameCount);
    }
    s_pending[index] = job;

    out->channels = static_cast<int>(job->channels);
    out->sampleRate = static_cast<int>(job->sampleRate);
    out->totalFrames = job->totalFrames;

    if (PerfEnabled())
    {
        std::lock_guard<std::mutex> lock(s_perfLock);
        ++s_perf.starts;
        s_perf.openUs += NowUs() - t0;
    }
    return true;
}

void SND_StreamCancel(int index)
{
    if (!ValidIndex(index) || !s_pending[index])
        return;
    StreamJob *job = s_pending[index];
    s_pending[index] = nullptr;
    UninitDecoder(job);
    FS_FCloseFile(job->fsHandle);
    delete job;
}

void SND_StreamBegin(int index, bool looping, const SndStreamStart &start, bool wantPlay)
{
    if (!ValidIndex(index) || !s_pending[index])
        return;
    EnsureThread();
    StreamJob *job = s_pending[index];
    s_pending[index] = nullptr;
    StopActive(index); // a restart replaces the running stream, not this open
    job->looping = looping;
    job->start = start;
    job->gen = ++s_mainGen[index];
    PublishStatus(index, job->gen, SND_STREAM_LOADING, nullptr);
    s_wantPlay[index].store(wantPlay, std::memory_order_release);
    s_mainActive[index] = true;
    PostCmd(Cmd{ CMD_BEGIN, index, job->gen, job });
}

void SND_StreamSetWantPlay(int index, bool play)
{
    if (ValidIndex(index))
        s_wantPlay[index].store(play, std::memory_order_release);
}

void SND_StreamStop(int index)
{
    if (!ValidIndex(index))
        return;
    SND_StreamCancel(index);
    StopActive(index);
}

bool SND_StreamIsActive(int index)
{
    return ValidIndex(index) && s_mainActive[index];
}

void SND_StreamPoll(int index, SndStreamStatus *out)
{
    *out = SndStreamStatus{};
    if (!ValidIndex(index) || !s_mainActive[index])
        return;
    std::lock_guard<std::mutex> lock(s_statusLock);
    if (s_statusGen[index] != s_mainGen[index])
    {
        out->state = SND_STREAM_LOADING;
        return;
    }
    *out = s_status[index];
}

void SND_StreamShutdown()
{
    for (int index = kFirstStream; index < kMaxChannels; ++index)
        SND_StreamStop(index);
    if (s_threadStarted)
    {
        s_quit.store(true, std::memory_order_release);
#ifdef __SWITCH__
        threadWaitForExit(&s_thread);
        threadClose(&s_thread);
#else
        s_thread.join();
#endif
        s_threadStarted = false;
    }
    SND_StreamServiceMainThread();
}

void SND_StreamReleaseAll()
{
    for (int index = kFirstStream; index < kMaxChannels; ++index)
        SND_StreamStop(index);
    // A stop is retired at the stream thread's next command pass, after
    // whatever load or refill it is in the middle of (an MP3 length scan of
    // a deflated iwd entry takes up to a few hundred ms).
    if (!SyncThread(10000))
        Com_Error(ERR_FATAL, "SND_StreamReleaseAll: the stream thread did not retire its jobs in 10 s");
    SND_StreamServiceMainThread();
}

uint32_t SND_StreamUnderrunCount()
{
    return s_totalUnderruns.load(std::memory_order_relaxed);
}

void SND_StreamEmitPerf(int nowMs)
{
    StreamPerf perf;
    {
        std::lock_guard<std::mutex> lock(s_perfLock);
        if (nowMs - s_perf.lastPrintMs < 1000)
            return;
        perf = s_perf;
        s_perf = MakePerf();
        s_perf.lastPrintMs = nowMs;
    }
    int active = 0;
    for (int index = kFirstStream; index < kMaxChannels; ++index)
        active += s_mainActive[index] ? 1 : 0;
    Com_Printf(16, "SWITCH_PERF sndstream active=%d starts=%u open_us=%llu load_us=%llu load_max_us=%llu slowest=%s "
               "fills=%u fill_us=%llu fill_max_us=%llu max_tick_gap_ms=%d min_queued_ms=%d underruns=%u al_errors=%u\n",
               active, perf.starts, (unsigned long long)perf.openUs, (unsigned long long)perf.loadUs,
               (unsigned long long)perf.loadMaxUs, perf.slowest[0] ? perf.slowest : "-", perf.fills,
               (unsigned long long)perf.fillUs, (unsigned long long)perf.fillMaxUs, perf.maxTickGapMs,
               perf.minQueuedMs, perf.underruns, perf.alErrors);
}

#endif // KISAK_OPENAL
