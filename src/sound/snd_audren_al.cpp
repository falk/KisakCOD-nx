#if defined(KISAK_OPENAL) || defined(KISAK_SND_AUDREN_HOST_TEST)
// OpenAL subset over the audio renderer; see snd_audren_al.h.
#include "snd_audren_al.h"
#include "snd_audren_pan.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <vector>

namespace
{
constexpr float kMinPitch = 1.0f / 16.0f;
constexpr float kMaxPitch = 8.0f;

struct Chunk
{
    unsigned char *base;
    size_t size;
    std::map<size_t, size_t> freeRanges; // offset -> length
};

struct Buffer
{
    bool live;
    int16_t *data;
    int chunk;
    size_t offset;
    size_t cap;        // reserved bytes
    uint32_t frames;
    uint32_t channels;
    uint32_t rate;
    int refs;          // static attachments + queue entries
};

struct QueueEntry
{
    ALuint buffer;
    bool submitted;
    SndArWave wave;
};

struct Source
{
    bool live;
    ALint state;
    float gain;
    float pitch;
    float pos[3];
    bool looping;
    ALuint staticBuffer;
    uint32_t pendingOffset;
    bool voiceReady;
    int voiceChannels;
    SndArWave staticWaves[2];
    uint32_t staticStart;
    QueueEntry queue[kSndArQueueDepth];
    int qHead;
    int qCount;
    uint64_t framesUnqueued; // played frames of entries unqueued since the last play
    ALuint sendSlot;         // AL_AUXILIARY_SEND_FILTER send 0: slot (0 = none)
    float sendGain;          // the send filter's gain when the send was set
};

// EFX objects (names are index + 1).  A filter is only a gain carrier here
// (SND_ApplyReverbSend); OpenAL copies its parameters when a send is set.
struct Filter
{
    bool live;
    ALint type;
    float gain;
    float gainHF;
};

struct Effect
{
    bool live;
    ALint type;
    SndArEfxReverb props;
};

struct Slot
{
    bool live;
    ALint type;        // effect type loaded (AL_EFFECT_NULL / AL_EFFECT_EAXREVERB)
    SndArEfxReverb props;
    bool dsp;          // this slot drives the DSP reverb
};

struct PendingFree
{
    int chunk;
    size_t offset;
    size_t length;
    uint64_t after;
};

std::mutex g_lock;
SndArDevice *g_dev;
void (*g_print)(const char *line);
Source g_src[kSndArMaxSources];
std::vector<Buffer> g_buf;
std::vector<ALuint> g_freeNames;
std::vector<Chunk> g_chunks;
std::vector<PendingFree> g_pending;
ALenum g_error = AL_NO_ERROR;
SndArStats g_stats;
uint32_t g_failLines;
Filter g_filters[kSndArMaxFilters];
Effect g_effects[kSndArMaxEffects];
Slot g_slots[kSndArMaxSlots];

// The update thread holds the lock across audrvUpdate (the renderer IPC):
// libnx's update both reads the parameters and rewrites the wave-buffer
// lists, so it cannot run beside a model call.  Every AL call from the game
// and stream threads therefore records how long it waited, so a slow update
// shows up as sndmix lock_wait_* instead of as unexplained frame time.
class ApiLock
{
public:
    ApiLock()
    {
        if (g_lock.try_lock())
            return;
        const auto t0 = std::chrono::steady_clock::now();
        g_lock.lock();
        const uint64_t us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        ++g_stats.lockWaits;
        g_stats.lockWaitUs += us;
        if (us > g_stats.lockWaitUsMax)
            g_stats.lockWaitUsMax = static_cast<uint32_t>(us);
    }
    ~ApiLock() { g_lock.unlock(); }
    ApiLock(const ApiLock &) = delete;
    ApiLock &operator=(const ApiLock &) = delete;
};

void Print(const char *fmt, ...)
{
    if (!g_print)
        return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    g_print(line);
}

void SetError(ALenum error)
{
    ++g_stats.alErrors;
    if (g_error == AL_NO_ERROR)
        g_error = error;
}

// A start that cannot get a voice or a wave buffer must not be silent.
void Fail(const char *what, ALuint source, const char *detail)
{
    ++g_failLines;
    if (g_failLines <= 32 || (g_failLines & 255) == 0)
        Print("FAIL: SND_AUDREN_%s source=%u %s (count=%u)\n", what, source, detail ? detail : "", g_failLines);
}

Source *GetSource(ALuint name)
{
    if (name == 0 || name > (ALuint)kSndArMaxSources || !g_src[name - 1].live)
    {
        SetError(AL_INVALID_NAME);
        return nullptr;
    }
    return &g_src[name - 1];
}

Buffer *LookupBuffer(ALuint name)
{
    if (name == 0 || name > g_buf.size() || !g_buf[name - 1].live)
        return nullptr;
    return &g_buf[name - 1];
}

int VoiceOf(const Source *src)
{
    return static_cast<int>(src - g_src);
}

// --- pool arena -------------------------------------------------------------
size_t AlignUp(size_t value, size_t align)
{
    return (value + align - 1) & ~(align - 1);
}

bool ArenaAlloc(size_t bytes, int *chunkOut, size_t *offsetOut, size_t *lengthOut)
{
    const size_t length = AlignUp(bytes ? bytes : 1, kSndArWaveAlign);
    for (size_t c = 0; c < g_chunks.size(); ++c)
    {
        auto &ranges = g_chunks[c].freeRanges;
        for (auto it = ranges.begin(); it != ranges.end(); ++it)
        {
            if (it->second < length)
                continue;
            const size_t offset = it->first;
            const size_t remaining = it->second - length;
            ranges.erase(it);
            if (remaining)
                ranges[offset + length] = remaining;
            *chunkOut = static_cast<int>(c);
            *offsetOut = offset;
            *lengthOut = length;
            return true;
        }
    }
    const size_t chunkBytes = length > kSndArChunkBytes ? AlignUp(length, 0x1000) : kSndArChunkBytes;
    void *base = g_dev->AllocPoolChunk(chunkBytes);
    if (!base)
        return false;
    g_stats.poolBytes += chunkBytes;
    Chunk chunk;
    chunk.base = static_cast<unsigned char *>(base);
    chunk.size = chunkBytes;
    if (chunkBytes > length)
        chunk.freeRanges[length] = chunkBytes - length;
    g_chunks.push_back(chunk);
    *chunkOut = static_cast<int>(g_chunks.size() - 1);
    *offsetOut = 0;
    *lengthOut = length;
    return true;
}

void ArenaFreeNow(int chunk, size_t offset, size_t length)
{
    auto &ranges = g_chunks[chunk].freeRanges;
    auto next = ranges.lower_bound(offset);
    if (next != ranges.end() && offset + length == next->first)
    {
        length += next->second;
        next = ranges.erase(next);
    }
    if (next != ranges.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset)
        {
            prev->second += length;
            return;
        }
    }
    ranges[offset] = length;
}

// The DSP may still be inside a frame that reads this memory: reuse it only
// after two more renderer updates.
void ArenaFreeLater(int chunk, size_t offset, size_t length)
{
    g_pending.push_back(PendingFree{chunk, offset, length, g_dev->UpdateCount() + 2});
}

void ReleaseStorage(Buffer *buffer)
{
    if (buffer->cap)
    {
        ArenaFreeLater(buffer->chunk, buffer->offset, buffer->cap);
        g_stats.bufferBytes -= static_cast<uint64_t>(buffer->frames) * buffer->channels * sizeof(int16_t);
    }
    buffer->data = nullptr;
    buffer->cap = 0;
    buffer->frames = 0;
}

// --- EFX objects ----------------------------------------------------------------
Filter *GetFilter(ALuint name)
{
    if (name == 0 || name > (ALuint)kSndArMaxFilters || !g_filters[name - 1].live)
    {
        SetError(AL_INVALID_NAME);
        return nullptr;
    }
    return &g_filters[name - 1];
}

Effect *GetEffect(ALuint name)
{
    if (name == 0 || name > (ALuint)kSndArMaxEffects || !g_effects[name - 1].live)
    {
        SetError(AL_INVALID_NAME);
        return nullptr;
    }
    return &g_effects[name - 1];
}

Slot *GetSlot(ALuint name)
{
    if (name == 0 || name > (ALuint)kSndArMaxSlots || !g_slots[name - 1].live)
    {
        SetError(AL_INVALID_NAME);
        return nullptr;
    }
    return &g_slots[name - 1];
}

// The send level the DSP applies: the send's gain while its slot holds the
// DSP reverb, else nothing (an empty slot in openal-soft outputs silence).
float SendWet(const Source *src)
{
    const ALuint slot = src->sendSlot;
    if (slot == 0 || slot > (ALuint)kSndArMaxSlots || !g_slots[slot - 1].live || !g_slots[slot - 1].dsp)
        return 0.0f;
    return src->sendGain;
}

void ApplyMix(Source *src);

void RefreshSends(ALuint slot)
{
    for (int i = 0; i < kSndArMaxSources; ++i)
    {
        if (g_src[i].live && g_src[i].sendSlot == slot)
            ApplyMix(&g_src[i]);
    }
}

// --- voices -------------------------------------------------------------------
// AL_GAIN is the voice volume; the mix factors carry only the pan and the
// reverb send, so a gain change (every channel, every frame) skips the pan
// trigonometry and a position change leaves the volume alone.
void ApplyVolume(Source *src)
{
    if (src->voiceReady)
        g_dev->VoiceSetVolume(VoiceOf(src), SndAr_ClampGain(src->gain));
}

void ApplyMix(Source *src)
{
    if (!src->voiceReady)
        return;
    const int voice = VoiceOf(src);
    if (src->voiceChannels == 2)
    {
        g_dev->VoiceSetMix(voice, 0, 0, 1.0f);
        g_dev->VoiceSetMix(voice, 0, 1, 0.0f);
        g_dev->VoiceSetMix(voice, 1, 0, 0.0f);
        g_dev->VoiceSetMix(voice, 1, 1, 1.0f);
    }
    else
    {
        float left = 0.0f, right = 0.0f;
        SndAr_PanMono(src->pos[0], src->pos[1], src->pos[2], &left, &right);
        g_dev->VoiceSetMix(voice, 0, 0, left);
        g_dev->VoiceSetMix(voice, 0, 1, right);
    }
    // Reverb send bus.  The DSP's I3DL2 reverb feeds the sum of its input
    // channels into its pre-delay line (the reference renderer's Reverb3dCommand), and
    // openal-soft's reverb input for a source is the source's omni (W)
    // component: 1 x a mono source, L + R for a stereo one.  So a mono voice
    // sends wet/2 to each bus channel and a stereo voice sends each channel
    // at wet, which gives the reverb the same input openal-soft's has.
    const float wet = SendWet(src);
    if (src->voiceChannels == 2)
    {
        g_dev->VoiceSetMix(voice, 0, kSndArSendBus + 0, wet);
        g_dev->VoiceSetMix(voice, 0, kSndArSendBus + 1, 0.0f);
        g_dev->VoiceSetMix(voice, 1, kSndArSendBus + 0, 0.0f);
        g_dev->VoiceSetMix(voice, 1, kSndArSendBus + 1, wet);
    }
    else
    {
        g_dev->VoiceSetMix(voice, 0, kSndArSendBus + 0, 0.5f * wet);
        g_dev->VoiceSetMix(voice, 0, kSndArSendBus + 1, 0.5f * wet);
    }
}

void ApplyPitch(Source *src)
{
    if (!src->voiceReady)
        return;
    float pitch = src->pitch;
    if (pitch < kMinPitch || pitch > kMaxPitch)
    {
        ++g_stats.pitchClamps;
        pitch = pitch < kMinPitch ? kMinPitch : kMaxPitch;
    }
    g_dev->VoiceSetPitch(VoiceOf(src), pitch);
}

// A PLAYING source whose voice ran out of wave buffers has stopped.
void Refresh(Source *src)
{
    if (src->state == AL_PLAYING && (!src->voiceReady || !g_dev->VoiceIsPlaying(VoiceOf(src))))
        src->state = AL_STOPPED;
}

void FillWave(SndArWave *wave, const Buffer *buffer, int32_t start, bool loop)
{
    g_dev->WaveReset(wave);
    wave->data = buffer->data;
    wave->bytes = static_cast<uint64_t>(buffer->frames) * buffer->channels * sizeof(int16_t);
    wave->start = start;
    wave->end = static_cast<int32_t>(buffer->frames);
    wave->loop = loop;
}

int ProcessedCount(Source *src)
{
    Refresh(src);
    if (src->staticBuffer)
        return 0;
    if (src->state == AL_STOPPED)
        return src->qCount;
    if (src->state == AL_INITIAL)
        return 0;
    int processed = 0;
    for (int i = 0; i < src->qCount; ++i)
    {
        const QueueEntry &entry = src->queue[(src->qHead + i) % kSndArQueueDepth];
        if (!entry.submitted || !g_dev->WaveDone(&entry.wave))
            break;
        ++processed;
    }
    return processed;
}

uint32_t SampleOffset(Source *src)
{
    Refresh(src);
    if (src->state != AL_PLAYING && src->state != AL_PAUSED)
        return 0;
    const uint64_t played = g_dev->VoicePlayedSamples(VoiceOf(src));
    if (src->staticBuffer)
    {
        const Buffer *buffer = LookupBuffer(src->staticBuffer);
        if (!buffer || !buffer->frames)
            return 0;
        uint64_t pos = src->staticStart + played;
        if (pos >= buffer->frames)
            pos = src->looping ? (pos - buffer->frames) % buffer->frames : buffer->frames;
        return static_cast<uint32_t>(pos);
    }
    uint64_t consumed = src->framesUnqueued;
    for (int i = 0; i < src->qCount; ++i)
    {
        const QueueEntry &entry = src->queue[(src->qHead + i) % kSndArQueueDepth];
        if (!entry.submitted || !g_dev->WaveDone(&entry.wave))
            break;
        const Buffer *buffer = LookupBuffer(entry.buffer);
        consumed += buffer ? buffer->frames : 0;
    }
    return played > consumed ? static_cast<uint32_t>(played - consumed) : 0;
}

const Buffer *FormatBuffer(const Source *src)
{
    if (src->staticBuffer)
        return LookupBuffer(src->staticBuffer);
    if (src->qCount)
        return LookupBuffer(src->queue[src->qHead].buffer);
    return nullptr;
}

void StopVoice(Source *src)
{
    if (src->voiceReady)
        g_dev->VoiceStop(VoiceOf(src));
}

void Play(Source *src, ALuint name)
{
    Refresh(src);
    const int voice = VoiceOf(src);
    if (src->state == AL_PAUSED && src->voiceReady)
    {
        g_dev->VoiceSetPaused(voice, false);
        src->state = AL_PLAYING;
        return;
    }
    StopVoice(src);
    const Buffer *format = FormatBuffer(src);
    if (!format || !format->frames || !format->data)
    {
        // OpenAL plays a source without data straight to STOPPED.
        src->state = AL_STOPPED;
        src->pendingOffset = 0;
        return;
    }
    if (!g_dev->VoiceInit(voice, static_cast<int>(format->channels), static_cast<int>(format->rate)))
    {
        src->voiceReady = false;
        src->state = AL_STOPPED;
        ++g_stats.playFails;
        SetError(AL_OUT_OF_MEMORY);
        char detail[64];
        snprintf(detail, sizeof(detail), "voice_init channels=%u rate=%u", format->channels, format->rate);
        Fail("VOICE", name, detail);
        return;
    }
    src->voiceReady = true;
    src->voiceChannels = static_cast<int>(format->channels);
    ApplyVolume(src);
    ApplyMix(src);
    ApplyPitch(src);

    bool ok = true;
    if (src->staticBuffer)
    {
        const uint32_t offset = src->pendingOffset < format->frames ? src->pendingOffset : 0;
        FillWave(&src->staticWaves[0], format, static_cast<int32_t>(offset), src->looping && offset == 0);
        ok = g_dev->VoiceAddWave(voice, &src->staticWaves[0]);
        if (ok && src->looping && offset > 0)
        {
            FillWave(&src->staticWaves[1], format, 0, true);
            ok = g_dev->VoiceAddWave(voice, &src->staticWaves[1]);
        }
        src->staticStart = offset;
    }
    else
    {
        for (int i = 0; ok && i < src->qCount; ++i)
        {
            QueueEntry &entry = src->queue[(src->qHead + i) % kSndArQueueDepth];
            const Buffer *buffer = LookupBuffer(entry.buffer);
            FillWave(&entry.wave, buffer, 0, false);
            ok = g_dev->VoiceAddWave(voice, &entry.wave);
            entry.submitted = ok;
        }
        src->framesUnqueued = 0;
    }
    src->pendingOffset = 0;
    if (!ok)
    {
        g_dev->VoiceStop(voice);
        src->state = AL_STOPPED;
        ++g_stats.playFails;
        SetError(AL_INVALID_OPERATION);
        Fail("WAVE", name, "wave_add");
        return;
    }
    g_dev->VoiceSetPaused(voice, false);
    src->state = AL_PLAYING;
    ++g_stats.plays;
    uint32_t active = 0;
    for (int i = 0; i < kSndArMaxSources; ++i)
        active += g_src[i].live && (g_src[i].state == AL_PLAYING || g_src[i].state == AL_PAUSED) ? 1 : 0;
    if (active > g_stats.peakVoices)
        g_stats.peakVoices = active;
}

void ClearQueue(Source *src)
{
    for (int i = 0; i < src->qCount; ++i)
    {
        QueueEntry &entry = src->queue[(src->qHead + i) % kSndArQueueDepth];
        if (Buffer *buffer = LookupBuffer(entry.buffer))
            --buffer->refs;
        entry = QueueEntry{};
    }
    src->qHead = 0;
    src->qCount = 0;
    src->framesUnqueued = 0;
}

void ClearStatic(Source *src)
{
    if (Buffer *buffer = LookupBuffer(src->staticBuffer))
        --buffer->refs;
    src->staticBuffer = 0;
}

void ResetSource(Source *src)
{
    std::memset(static_cast<void *>(src), 0, sizeof(*src));
    src->state = AL_INITIAL;
    src->gain = 1.0f;
    src->pitch = 1.0f;
}

void FinishPendingFrees()
{
    const uint64_t now = g_dev->UpdateCount();
    size_t keep = 0;
    for (size_t i = 0; i < g_pending.size(); ++i)
    {
        if (g_pending[i].after <= now)
            ArenaFreeNow(g_pending[i].chunk, g_pending[i].offset, g_pending[i].length);
        else
            g_pending[keep++] = g_pending[i];
    }
    g_pending.resize(keep);
}
} // namespace

namespace
{
void ResetObjects()
{
    for (int i = 0; i < kSndArMaxSources; ++i)
        ResetSource(&g_src[i]);
    g_buf.clear();
    g_freeNames.clear();
    g_chunks.clear();
    g_pending.clear();
    std::memset(static_cast<void *>(g_filters), 0, sizeof(g_filters));
    for (Effect &effect : g_effects)
        effect = Effect{};
    for (Slot &slot : g_slots)
        slot = Slot{};
}
} // namespace

void SndAr_Attach(SndArDevice *device, void (*print)(const char *line))
{
    ApiLock lock;
    ResetObjects();
    g_dev = device;
    g_print = print;
    g_error = AL_NO_ERROR;
    g_stats = SndArStats{};
    g_failLines = 0;
}

void SndAr_Detach()
{
    ApiLock lock;
    ResetObjects();
    g_dev = nullptr;
}

bool SndAr_Attached()
{
    ApiLock lock;
    return g_dev != nullptr;
}

void SndAr_Update()
{
    std::lock_guard<std::mutex> lock(g_lock); // the update thread itself is not timed
    if (!g_dev)
        return;
    g_dev->Update();
    FinishPendingFrees();
}

void SndAr_GetStats(SndArStats *out)
{
    ApiLock lock;
    uint32_t active = 0;
    uint32_t wet = 0;
    uint32_t buffers = 0;
    if (g_dev)
    {
        for (int i = 0; i < kSndArMaxSources; ++i)
        {
            if (!g_src[i].live)
                continue;
            Refresh(&g_src[i]);
            const bool on = g_src[i].state == AL_PLAYING || g_src[i].state == AL_PAUSED;
            active += on ? 1 : 0;
            wet += on && SendWet(&g_src[i]) > 0.0f ? 1 : 0;
        }
    }
    g_stats.wetVoices = wet;
    for (const Buffer &buffer : g_buf)
        buffers += buffer.live ? 1 : 0;
    g_stats.activeVoices = active;
    g_stats.buffers = buffers;
    *out = g_stats;
    g_stats.lockWaitUsMax = 0;
}

int SndAr_SourceVoice(ALuint source)
{
    ApiLock lock;
    if (source == 0 || source > (ALuint)kSndArMaxSources || !g_src[source - 1].live)
        return -1;
    return static_cast<int>(source - 1);
}

void SndAr_alGenSources(ALsizei n, ALuint *sources)
{
    ApiLock lock;
    int made = 0;
    for (int i = 0; i < kSndArMaxSources && made < n; ++i)
    {
        if (g_src[i].live)
            continue;
        ResetSource(&g_src[i]);
        g_src[i].live = true;
        sources[made++] = static_cast<ALuint>(i + 1);
    }
    if (made < n)
    {
        for (int i = 0; i < made; ++i)
            g_src[sources[i] - 1].live = false;
        for (int i = 0; i < n; ++i)
            sources[i] = 0;
        SetError(AL_OUT_OF_MEMORY);
        Fail("SOURCES", 0, "more AL sources than kSndArMaxSources");
    }
}

void SndAr_alDeleteSources(ALsizei n, const ALuint *sources)
{
    ApiLock lock;
    for (int i = 0; i < n; ++i)
    {
        Source *src = GetSource(sources[i]);
        if (!src)
            continue;
        if (src->voiceReady)
            g_dev->VoiceDrop(VoiceOf(src));
        ClearQueue(src);
        ClearStatic(src);
        ResetSource(src);
    }
}

void SndAr_alGenBuffers(ALsizei n, ALuint *buffers)
{
    ApiLock lock;
    for (int i = 0; i < n; ++i)
    {
        ALuint name = 0;
        if (!g_freeNames.empty())
        {
            name = g_freeNames.back();
            g_freeNames.pop_back();
        }
        else if (g_buf.size() < (size_t)kSndArMaxBuffers)
        {
            g_buf.push_back(Buffer{});
            name = static_cast<ALuint>(g_buf.size());
        }
        else
        {
            SetError(AL_OUT_OF_MEMORY);
            Fail("BUFFERS", 0, "more AL buffers than kSndArMaxBuffers");
            buffers[i] = 0;
            continue;
        }
        g_buf[name - 1] = Buffer{};
        g_buf[name - 1].live = true;
        buffers[i] = name;
    }
}

void SndAr_alDeleteBuffers(ALsizei n, const ALuint *buffers)
{
    ApiLock lock;
    for (int i = 0; i < n; ++i)
    {
        if (buffers[i] == 0)
            continue;
        Buffer *buffer = LookupBuffer(buffers[i]);
        if (!buffer)
        {
            SetError(AL_INVALID_NAME);
            continue;
        }
        if (buffer->refs > 0)
        {
            SetError(AL_INVALID_OPERATION); // still attached, as OpenAL refuses
            continue;
        }
        ReleaseStorage(buffer);
        buffer->live = false;
        g_freeNames.push_back(buffers[i]);
    }
}

void SndAr_alBufferData(ALuint name, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq)
{
    ApiLock lock;
    Buffer *buffer = LookupBuffer(name);
    if (!buffer)
    {
        SetError(AL_INVALID_NAME);
        return;
    }
    if (buffer->refs > 0)
    {
        SetError(AL_INVALID_OPERATION);
        return;
    }
    uint32_t channels, bytesPerSample;
    switch (format)
    {
    case AL_FORMAT_MONO8: channels = 1; bytesPerSample = 1; break;
    case AL_FORMAT_MONO16: channels = 1; bytesPerSample = 2; break;
    case AL_FORMAT_STEREO8: channels = 2; bytesPerSample = 1; break;
    case AL_FORMAT_STEREO16: channels = 2; bytesPerSample = 2; break;
    default: SetError(AL_INVALID_ENUM); return;
    }
    if (size < 0 || freq <= 0 || (size > 0 && !data))
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    const uint32_t frames = static_cast<uint32_t>(size) / (channels * bytesPerSample);
    const size_t need = static_cast<size_t>(frames) * channels * sizeof(int16_t);
    g_stats.bufferBytes -= static_cast<uint64_t>(buffer->frames) * buffer->channels * sizeof(int16_t);
    buffer->frames = 0;
    if (buffer->cap < need)
    {
        if (buffer->cap)
            ArenaFreeLater(buffer->chunk, buffer->offset, buffer->cap);
        buffer->cap = 0;
        buffer->data = nullptr;
        int chunk = 0;
        size_t offset = 0, length = 0;
        if (!ArenaAlloc(need, &chunk, &offset, &length))
        {
            ++g_stats.oomFails;
            SetError(AL_OUT_OF_MEMORY);
            char detail[64];
            snprintf(detail, sizeof(detail), "pool_oom bytes=%zu pool=%llu", need,
                     static_cast<unsigned long long>(g_stats.poolBytes));
            Fail("POOL", name, detail);
            return;
        }
        buffer->chunk = chunk;
        buffer->offset = offset;
        buffer->cap = length;
        buffer->data = reinterpret_cast<int16_t *>(g_chunks[chunk].base + offset);
    }
    if (bytesPerSample == 2)
    {
        std::memcpy(buffer->data, data, need);
    }
    else
    {
        const uint8_t *src = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < static_cast<size_t>(frames) * channels; ++i)
            buffer->data[i] = static_cast<int16_t>((static_cast<int>(src[i]) - 128) * 256);
    }
    if (need)
        g_dev->FlushData(buffer->data, need);
    buffer->frames = frames;
    buffer->channels = channels;
    buffer->rate = static_cast<uint32_t>(freq);
    g_stats.bufferBytes += need;
}

void SndAr_alSourcei(ALuint name, ALenum param, ALint value)
{
    ApiLock lock;
    Source *src = GetSource(name);
    if (!src)
        return;
    switch (param)
    {
    case AL_BUFFER:
    {
        Refresh(src);
        if (src->state == AL_PLAYING || src->state == AL_PAUSED)
        {
            SetError(AL_INVALID_OPERATION);
            return;
        }
        Buffer *buffer = nullptr;
        if (value)
        {
            buffer = LookupBuffer(static_cast<ALuint>(value));
            if (!buffer)
            {
                SetError(AL_INVALID_VALUE);
                return;
            }
        }
        ClearQueue(src);
        ClearStatic(src);
        if (buffer)
        {
            src->staticBuffer = static_cast<ALuint>(value);
            ++buffer->refs;
        }
        return;
    }
    case AL_LOOPING:
        src->looping = value != 0;
        return;
    case AL_SAMPLE_OFFSET:
    {
        const Buffer *format = FormatBuffer(src);
        if (value < 0 || (format && static_cast<uint32_t>(value) >= format->frames && format->frames))
        {
            SetError(AL_INVALID_VALUE);
            return;
        }
        src->pendingOffset = static_cast<uint32_t>(value);
        Refresh(src);
        if (src->state == AL_PLAYING || src->state == AL_PAUSED)
        {
            // OpenAL seeks a playing source: restart it at the new offset.
            const bool paused = src->state == AL_PAUSED;
            src->state = AL_STOPPED;
            Play(src, name);
            if (paused && src->state == AL_PLAYING)
            {
                g_dev->VoiceSetPaused(VoiceOf(src), true);
                src->state = AL_PAUSED;
            }
        }
        return;
    }
    case AL_SOURCE_RELATIVE:
        // The listener never leaves the origin (snd_openal.cpp), so relative
        // and absolute positions are the same thing.
        return;
    case AL_DIRECT_FILTER:
        // Direct-path filters (only the commented-out EQ path would set one):
        // the DSP has per-voice biquads but nothing maps EFX filters to them.
        if (value != AL_FILTER_NULL)
        {
            ++g_stats.efxSkipped;
            SetError(AL_INVALID_VALUE);
            Fail("EFX", name, "direct filter not supported");
        }
        return;
    default:
        SetError(AL_INVALID_ENUM);
        return;
    }
}

void SndAr_alSource3i(ALuint name, ALenum param, ALint v1, ALint v2, ALint v3)
{
    ApiLock lock;
    Source *src = GetSource(name);
    if (!src)
        return;
    if (param != AL_AUXILIARY_SEND_FILTER)
    {
        SetError(AL_INVALID_ENUM);
        return;
    }
    // v1 = slot (0: none), v2 = send index, v3 = filter (0: unfiltered).
    const ALuint slot = static_cast<ALuint>(v1);
    const ALuint filter = static_cast<ALuint>(v3);
    if (v2 != 0 || (slot != 0 && !GetSlot(slot)))
    {
        if (v2 != 0)
            SetError(AL_INVALID_VALUE);
        ++g_stats.reverbSendsSkipped;
        Fail("REVERB_SEND", name, v2 != 0 ? "send index != 0" : "bad slot");
        return;
    }
    float gain = 1.0f;
    if (filter != 0)
    {
        const Filter *f = GetFilter(filter);
        if (!f)
        {
            ++g_stats.reverbSendsSkipped;
            Fail("REVERB_SEND", name, "bad filter");
            return;
        }
        if (f->type == AL_FILTER_LOWPASS)
        {
            gain = f->gain;
            if (f->gainHF < 1.0f)
            {
                // A per-send HF cut has no DSP counterpart (voice biquads
                // also filter the dry path); the engine always sends 1.
                ++g_stats.efxSkipped;
                Fail("EFX", name, "send lowpass GAINHF < 1 not supported");
            }
        }
        else if (f->type != AL_FILTER_NULL)
        {
            ++g_stats.reverbSendsSkipped;
            SetError(AL_INVALID_VALUE);
            Fail("REVERB_SEND", name, "send filter type not supported");
            return;
        }
    }
    src->sendSlot = slot;
    src->sendGain = gain;
    ++g_stats.reverbSends;
    ApplyMix(src);
}

void SndAr_alSourcef(ALuint name, ALenum param, ALfloat value)
{
    ApiLock lock;
    Source *src = GetSource(name);
    if (!src)
        return;
    if (!(value >= 0.0f) || !std::isfinite(value))
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    switch (param)
    {
    case AL_GAIN:
        src->gain = value;
        ApplyVolume(src);
        return;
    case AL_PITCH:
        src->pitch = value;
        ApplyPitch(src);
        return;
    default:
        SetError(AL_INVALID_ENUM);
        return;
    }
}

void SndAr_alSource3f(ALuint name, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3)
{
    ApiLock lock;
    Source *src = GetSource(name);
    if (!src)
        return;
    if (param != AL_POSITION)
    {
        if (param != AL_VELOCITY && param != AL_DIRECTION)
            SetError(AL_INVALID_ENUM);
        return;
    }
    if (!std::isfinite(v1) || !std::isfinite(v2) || !std::isfinite(v3))
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    src->pos[0] = v1;
    src->pos[1] = v2;
    src->pos[2] = v3;
    ApplyMix(src);
}

void SndAr_alGetSourcei(ALuint name, ALenum param, ALint *value)
{
    ApiLock lock;
    Source *src = GetSource(name);
    if (!src || !value)
        return;
    switch (param)
    {
    case AL_SOURCE_STATE:
        Refresh(src);
        *value = src->state;
        return;
    case AL_BUFFERS_QUEUED:
        *value = src->staticBuffer ? 1 : src->qCount;
        return;
    case AL_BUFFERS_PROCESSED:
        *value = ProcessedCount(src);
        return;
    case AL_SAMPLE_OFFSET:
        *value = static_cast<ALint>(SampleOffset(src));
        return;
    case AL_LOOPING:
        *value = src->looping ? 1 : 0;
        return;
    case AL_BUFFER:
        *value = static_cast<ALint>(src->staticBuffer ? src->staticBuffer
                                                      : (src->qCount ? src->queue[src->qHead].buffer : 0));
        return;
    default:
        SetError(AL_INVALID_ENUM);
        return;
    }
}

void SndAr_alGetSourcef(ALuint name, ALenum param, ALfloat *value)
{
    ApiLock lock;
    Source *src = GetSource(name);
    if (!src || !value)
        return;
    switch (param)
    {
    case AL_GAIN:
        *value = src->gain;
        return;
    case AL_PITCH:
        *value = src->pitch;
        return;
    case AL_SEC_OFFSET:
    {
        const Buffer *format = FormatBuffer(src);
        const uint32_t offset = SampleOffset(src);
        *value = format && format->rate ? static_cast<float>(offset) / static_cast<float>(format->rate) : 0.0f;
        return;
    }
    default:
        SetError(AL_INVALID_ENUM);
        return;
    }
}

void SndAr_alGetSource3f(ALuint name, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3)
{
    ApiLock lock;
    Source *src = GetSource(name);
    if (!src)
        return;
    if (param != AL_POSITION)
    {
        SetError(AL_INVALID_ENUM);
        return;
    }
    *v1 = src->pos[0];
    *v2 = src->pos[1];
    *v3 = src->pos[2];
}

void SndAr_alSourcePlay(ALuint name)
{
    ApiLock lock;
    if (Source *src = GetSource(name))
        Play(src, name);
}

void SndAr_alSourcePause(ALuint name)
{
    ApiLock lock;
    Source *src = GetSource(name);
    if (!src)
        return;
    Refresh(src);
    if (src->state == AL_PLAYING)
    {
        g_dev->VoiceSetPaused(VoiceOf(src), true);
        src->state = AL_PAUSED;
    }
}

void SndAr_alSourceStop(ALuint name)
{
    ApiLock lock;
    Source *src = GetSource(name);
    if (!src)
        return;
    StopVoice(src);
    src->state = AL_STOPPED;
    src->pendingOffset = 0;
}

void SndAr_alSourceQueueBuffers(ALuint name, ALsizei n, const ALuint *buffers)
{
    ApiLock lock;
    Source *src = GetSource(name);
    if (!src)
        return;
    if (src->staticBuffer || src->qCount + n > kSndArQueueDepth)
    {
        SetError(AL_INVALID_OPERATION);
        if (src->qCount + n > kSndArQueueDepth)
            Fail("QUEUE", name, "queue deeper than kSndArQueueDepth");
        return;
    }
    const Buffer *first = FormatBuffer(src);
    for (int i = 0; i < n; ++i)
    {
        const Buffer *buffer = LookupBuffer(buffers[i]);
        if (!buffer)
        {
            SetError(AL_INVALID_NAME);
            return;
        }
        if (!first)
            first = buffer;
        if (buffer->channels != first->channels || buffer->rate != first->rate)
        {
            SetError(AL_INVALID_OPERATION); // one format per queue, as OpenAL requires
            return;
        }
    }
    Refresh(src);
    const bool live = (src->state == AL_PLAYING || src->state == AL_PAUSED) && src->voiceReady;
    for (int i = 0; i < n; ++i)
    {
        Buffer *buffer = LookupBuffer(buffers[i]);
        QueueEntry &entry = src->queue[(src->qHead + src->qCount) % kSndArQueueDepth];
        entry = QueueEntry{};
        entry.buffer = buffers[i];
        ++buffer->refs;
        ++src->qCount;
        if (live && buffer->frames)
        {
            FillWave(&entry.wave, buffer, 0, false);
            entry.submitted = g_dev->VoiceAddWave(VoiceOf(src), &entry.wave);
            if (!entry.submitted)
            {
                ++g_stats.playFails;
                Fail("WAVE", name, "stream wave_add");
            }
        }
    }
}

void SndAr_alSourceUnqueueBuffers(ALuint name, ALsizei n, ALuint *buffers)
{
    ApiLock lock;
    Source *src = GetSource(name);
    if (!src)
        return;
    if (n > ProcessedCount(src))
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    for (int i = 0; i < n; ++i)
    {
        QueueEntry &entry = src->queue[src->qHead];
        Buffer *buffer = LookupBuffer(entry.buffer);
        if (buffer)
        {
            --buffer->refs;
            if (entry.submitted && (src->state == AL_PLAYING || src->state == AL_PAUSED))
                src->framesUnqueued += buffer->frames;
        }
        buffers[i] = entry.buffer;
        entry = QueueEntry{};
        src->qHead = (src->qHead + 1) % kSndArQueueDepth;
        --src->qCount;
    }
}

ALenum SndAr_alGetError()
{
    ApiLock lock;
    const ALenum error = g_error;
    g_error = AL_NO_ERROR;
    return error;
}

// --- EFX subset ---------------------------------------------------------------
namespace
{
template <typename T, size_t N>
void GenObjects(T (&pool)[N], ALsizei n, ALuint *out, const char *what)
{
    int made = 0;
    for (size_t i = 0; i < N && made < n; ++i)
    {
        if (pool[i].live)
            continue;
        pool[i] = T{};
        pool[i].live = true;
        out[made++] = static_cast<ALuint>(i + 1);
    }
    if (made < n)
    {
        for (int i = 0; i < made; ++i)
            pool[out[i] - 1].live = false;
        for (int i = 0; i < n; ++i)
            out[i] = 0;
        SetError(AL_OUT_OF_MEMORY);
        Fail("EFX", 0, what);
    }
}

// Pushes a slot's effect to the DSP (the one slot holding a reverb).
void CommitSlot(ALuint name, Slot *slot)
{
    if (!g_dev)
        return;
    if (slot->type == AL_EFFECT_EAXREVERB)
    {
        for (int i = 0; i < kSndArMaxSlots; ++i)
        {
            if (g_slots[i].live && g_slots[i].dsp && &g_slots[i] != slot)
            {
                SetError(AL_INVALID_OPERATION);
                ++g_stats.efxSkipped;
                Fail("EFX", name, "second reverb slot (the DSP runs one reverb)");
                return;
            }
        }
        const SndArI3dl2 params = SndAr_EfxToI3dl2(slot->props);
        if (!g_dev->SetReverb(&params))
        {
            slot->dsp = false;
            ++g_stats.efxSkipped;
            SetError(AL_INVALID_OPERATION);
            Fail("EFX", name, "renderer has no reverb effect");
        }
        else
        {
            slot->dsp = true;
            g_stats.reverbOn = true;
            g_stats.reverb = params;
            ++g_stats.reverbPresets;
        }
    }
    else if (slot->dsp)
    {
        g_dev->SetReverb(nullptr);
        slot->dsp = false;
        g_stats.reverbOn = false;
    }
    RefreshSends(name);
}
} // namespace

void SndAr_alGenFilters(ALsizei n, ALuint *filters)
{
    ApiLock lock;
    GenObjects(g_filters, n, filters, "more filters than kSndArMaxFilters");
    for (ALsizei i = 0; i < n; ++i)
    {
        if (filters[i])
        {
            g_filters[filters[i] - 1].type = AL_FILTER_NULL;
            g_filters[filters[i] - 1].gain = 1.0f;
            g_filters[filters[i] - 1].gainHF = 1.0f;
        }
    }
}

void SndAr_alDeleteFilters(ALsizei n, const ALuint *filters)
{
    ApiLock lock;
    for (ALsizei i = 0; i < n; ++i)
    {
        if (filters[i] && GetFilter(filters[i]))
            g_filters[filters[i] - 1] = Filter{};
    }
}

void SndAr_alFilteri(ALuint name, ALenum param, ALint value)
{
    ApiLock lock;
    Filter *f = GetFilter(name);
    if (!f)
        return;
    if (param != AL_FILTER_TYPE)
    {
        SetError(AL_INVALID_ENUM);
        return;
    }
    if (value != AL_FILTER_NULL && value != AL_FILTER_LOWPASS)
    {
        SetError(AL_INVALID_VALUE);
        ++g_stats.efxSkipped;
        Fail("EFX", name, "filter type not supported");
        return;
    }
    f->type = value;
    f->gain = 1.0f;
    f->gainHF = 1.0f;
}

void SndAr_alFilterf(ALuint name, ALenum param, ALfloat value)
{
    ApiLock lock;
    Filter *f = GetFilter(name);
    if (!f)
        return;
    if (f->type != AL_FILTER_LOWPASS || (param != AL_LOWPASS_GAIN && param != AL_LOWPASS_GAINHF))
    {
        SetError(AL_INVALID_ENUM);
        return;
    }
    if (!(value >= 0.0f && value <= 1.0f))
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    (param == AL_LOWPASS_GAIN ? f->gain : f->gainHF) = value;
}

void SndAr_alGenEffects(ALsizei n, ALuint *effects)
{
    ApiLock lock;
    GenObjects(g_effects, n, effects, "more effects than kSndArMaxEffects");
    for (ALsizei i = 0; i < n; ++i)
    {
        if (effects[i])
            g_effects[effects[i] - 1].type = AL_EFFECT_NULL;
    }
}

void SndAr_alDeleteEffects(ALsizei n, const ALuint *effects)
{
    ApiLock lock;
    for (ALsizei i = 0; i < n; ++i)
    {
        if (effects[i] && GetEffect(effects[i]))
            g_effects[effects[i] - 1] = Effect{};
    }
}

void SndAr_alEffecti(ALuint name, ALenum param, ALint value)
{
    ApiLock lock;
    Effect *e = GetEffect(name);
    if (!e)
        return;
    if (param == AL_EFFECT_TYPE)
    {
        if (value != AL_EFFECT_NULL && value != AL_EFFECT_EAXREVERB)
        {
            SetError(AL_INVALID_VALUE);
            ++g_stats.efxSkipped;
            Fail("EFX", name, "effect type not supported (EAX reverb only)");
            return;
        }
        e->type = value;
        e->props = SndArEfxReverb{};
        return;
    }
    if (e->type == AL_EFFECT_EAXREVERB && param == AL_EAXREVERB_DECAY_HFLIMIT)
    {
        if (value != AL_FALSE && value != AL_TRUE)
        {
            SetError(AL_INVALID_VALUE);
            return;
        }
        e->props.decayHFLimit = value;
        return;
    }
    SetError(AL_INVALID_ENUM);
}

void SndAr_alEffectf(ALuint name, ALenum param, ALfloat value)
{
    ApiLock lock;
    Effect *e = GetEffect(name);
    if (!e)
        return;
    if (e->type != AL_EFFECT_EAXREVERB)
    {
        SetError(AL_INVALID_ENUM);
        return;
    }
    struct Range
    {
        ALenum param;
        float SndArEfxReverb::*field;
        float lo, hi;
    };
    // AL_EAXREVERB_MIN_* / MAX_* (efx.h).
    static const Range kRanges[] = {
        {AL_EAXREVERB_DENSITY, &SndArEfxReverb::density, AL_EAXREVERB_MIN_DENSITY, AL_EAXREVERB_MAX_DENSITY},
        {AL_EAXREVERB_DIFFUSION, &SndArEfxReverb::diffusion, AL_EAXREVERB_MIN_DIFFUSION, AL_EAXREVERB_MAX_DIFFUSION},
        {AL_EAXREVERB_GAIN, &SndArEfxReverb::gain, AL_EAXREVERB_MIN_GAIN, AL_EAXREVERB_MAX_GAIN},
        {AL_EAXREVERB_GAINHF, &SndArEfxReverb::gainHF, AL_EAXREVERB_MIN_GAINHF, AL_EAXREVERB_MAX_GAINHF},
        {AL_EAXREVERB_GAINLF, &SndArEfxReverb::gainLF, AL_EAXREVERB_MIN_GAINLF, AL_EAXREVERB_MAX_GAINLF},
        {AL_EAXREVERB_DECAY_TIME, &SndArEfxReverb::decayTime, AL_EAXREVERB_MIN_DECAY_TIME, AL_EAXREVERB_MAX_DECAY_TIME},
        {AL_EAXREVERB_DECAY_HFRATIO, &SndArEfxReverb::decayHFRatio, AL_EAXREVERB_MIN_DECAY_HFRATIO,
         AL_EAXREVERB_MAX_DECAY_HFRATIO},
        {AL_EAXREVERB_DECAY_LFRATIO, &SndArEfxReverb::decayLFRatio, AL_EAXREVERB_MIN_DECAY_LFRATIO,
         AL_EAXREVERB_MAX_DECAY_LFRATIO},
        {AL_EAXREVERB_REFLECTIONS_GAIN, &SndArEfxReverb::reflectionsGain, AL_EAXREVERB_MIN_REFLECTIONS_GAIN,
         AL_EAXREVERB_MAX_REFLECTIONS_GAIN},
        {AL_EAXREVERB_REFLECTIONS_DELAY, &SndArEfxReverb::reflectionsDelay, AL_EAXREVERB_MIN_REFLECTIONS_DELAY,
         AL_EAXREVERB_MAX_REFLECTIONS_DELAY},
        {AL_EAXREVERB_LATE_REVERB_GAIN, &SndArEfxReverb::lateReverbGain, AL_EAXREVERB_MIN_LATE_REVERB_GAIN,
         AL_EAXREVERB_MAX_LATE_REVERB_GAIN},
        {AL_EAXREVERB_LATE_REVERB_DELAY, &SndArEfxReverb::lateReverbDelay, AL_EAXREVERB_MIN_LATE_REVERB_DELAY,
         AL_EAXREVERB_MAX_LATE_REVERB_DELAY},
        {AL_EAXREVERB_ECHO_TIME, &SndArEfxReverb::echoTime, AL_EAXREVERB_MIN_ECHO_TIME, AL_EAXREVERB_MAX_ECHO_TIME},
        {AL_EAXREVERB_ECHO_DEPTH, &SndArEfxReverb::echoDepth, AL_EAXREVERB_MIN_ECHO_DEPTH, AL_EAXREVERB_MAX_ECHO_DEPTH},
        {AL_EAXREVERB_MODULATION_TIME, &SndArEfxReverb::modulationTime, AL_EAXREVERB_MIN_MODULATION_TIME,
         AL_EAXREVERB_MAX_MODULATION_TIME},
        {AL_EAXREVERB_MODULATION_DEPTH, &SndArEfxReverb::modulationDepth, AL_EAXREVERB_MIN_MODULATION_DEPTH,
         AL_EAXREVERB_MAX_MODULATION_DEPTH},
        {AL_EAXREVERB_AIR_ABSORPTION_GAINHF, &SndArEfxReverb::airAbsorptionGainHF,
         AL_EAXREVERB_MIN_AIR_ABSORPTION_GAINHF, AL_EAXREVERB_MAX_AIR_ABSORPTION_GAINHF},
        {AL_EAXREVERB_HFREFERENCE, &SndArEfxReverb::hfReference, AL_EAXREVERB_MIN_HFREFERENCE,
         AL_EAXREVERB_MAX_HFREFERENCE},
        {AL_EAXREVERB_LFREFERENCE, &SndArEfxReverb::lfReference, AL_EAXREVERB_MIN_LFREFERENCE,
         AL_EAXREVERB_MAX_LFREFERENCE},
        {AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, &SndArEfxReverb::roomRolloffFactor, AL_EAXREVERB_MIN_ROOM_ROLLOFF_FACTOR,
         AL_EAXREVERB_MAX_ROOM_ROLLOFF_FACTOR},
    };
    for (const Range &r : kRanges)
    {
        if (r.param != param)
            continue;
        if (!(value >= r.lo && value <= r.hi))
        {
            SetError(AL_INVALID_VALUE);
            return;
        }
        e->props.*r.field = value;
        return;
    }
    SetError(AL_INVALID_ENUM);
}

void SndAr_alEffectfv(ALuint name, ALenum param, const ALfloat *values)
{
    ApiLock lock;
    Effect *e = GetEffect(name);
    if (!e)
        return;
    if (e->type != AL_EFFECT_EAXREVERB ||
        (param != AL_EAXREVERB_REFLECTIONS_PAN && param != AL_EAXREVERB_LATE_REVERB_PAN))
    {
        SetError(AL_INVALID_ENUM);
        return;
    }
    float *pan = param == AL_EAXREVERB_REFLECTIONS_PAN ? e->props.reflectionsPan : e->props.lateReverbPan;
    for (int i = 0; i < 3; ++i)
        pan[i] = values[i];
}

void SndAr_alGenAuxiliaryEffectSlots(ALsizei n, ALuint *slots)
{
    ApiLock lock;
    GenObjects(g_slots, n, slots, "more effect slots than kSndArMaxSlots");
    for (ALsizei i = 0; i < n; ++i)
    {
        if (slots[i])
            g_slots[slots[i] - 1].type = AL_EFFECT_NULL;
    }
}

void SndAr_alDeleteAuxiliaryEffectSlots(ALsizei n, const ALuint *slots)
{
    ApiLock lock;
    for (ALsizei i = 0; i < n; ++i)
    {
        if (!slots[i] || !GetSlot(slots[i]))
            continue;
        Slot *slot = &g_slots[slots[i] - 1];
        slot->type = AL_EFFECT_NULL;
        CommitSlot(slots[i], slot); // bypasses the DSP reverb if this slot held it
        *slot = Slot{};
    }
}

void SndAr_alAuxiliaryEffectSloti(ALuint name, ALenum param, ALint value)
{
    ApiLock lock;
    Slot *slot = GetSlot(name);
    if (!slot)
        return;
    if (param == AL_EFFECTSLOT_AUXILIARY_SEND_AUTO)
        return; // distance-driven send adjustment: a no-op under AL_NONE
    if (param != AL_EFFECTSLOT_EFFECT)
    {
        SetError(AL_INVALID_ENUM);
        return;
    }
    // OpenAL copies the effect's current properties into the slot.
    if (value == 0)
    {
        slot->type = AL_EFFECT_NULL;
    }
    else
    {
        const Effect *e = GetEffect(static_cast<ALuint>(value));
        if (!e)
            return;
        slot->type = e->type;
        slot->props = e->props;
    }
    CommitSlot(name, slot);
}
#endif
