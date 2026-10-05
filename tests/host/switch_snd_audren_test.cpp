// Host proof for the audren sound backend (src/sound/snd_audren_al.cpp,
// src/sound/snd_audren_pan.h), run by `./test host` under ASan/UBSan.
//
// 1. Pan/gain law: renders a DC source through the host's openal-soft
//    (dlopen libopenal.so.1, ALC_SOFT_loopback) with the port's OpenAL
//    configuration (AL_NONE distance model, listener at the origin, stereo
//    output) and compares every case with SndAr_ComputeMix.  This is the only
//    check of the pan law against openal-soft: the Switch build no longer
//    links openal-soft at all (there is no on-device comparison against a
//    linked portlib), so this host cross-check is the sole verifier.
// 2. Voice bookkeeping: drives the OpenAL-subset model against a fake
//    renderer that follows libnx's audrv wave-buffer rules (a voice plays its
//    wave buffers in order, a looping one never finishes, stop marks every
//    wave done, a drained started voice stops playing) and checks the OpenAL
//    semantics the sound driver and stream thread rely on.
// 3. EFX reverb on the DSP: the EFX -> I3DL2 conversion of all 26
//    room presets against the formulas, the renderer wire layout, the
//    update splice, and the send routing onto the reverb bus.
#include "src/sound/snd_audren_al.h"
#include "src/sound/snd_audren_pan.h"

#include <AL/alc.h>
#include <AL/efx-presets.h>
#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <deque>
#include <string>
#include <thread>
#include <vector>

static int g_failures;
static std::vector<std::string> g_lines;

#define CHECK(cond)                                                                  \
    do                                                                               \
    {                                                                                \
        if (!(cond))                                                                 \
        {                                                                            \
            std::printf("FAIL:SND_AUDREN_TEST %s:%d %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

static bool Near(float a, float b, float tol = 1e-4f)
{
    return std::fabs(a - b) <= tol;
}

// --- fake renderer --------------------------------------------------------------
enum WaveState : unsigned char
{
    kFree = 0,
    kQueued = 1,
    kDone = 4,
};

struct WavePriv
{
    unsigned char state;
    int64_t cursor;
};
static_assert(sizeof(WavePriv) <= sizeof(SndArWave::device), "wave priv");

static WavePriv *Priv(const SndArWave *wave)
{
    return reinterpret_cast<WavePriv *>(const_cast<unsigned char *>(wave->device));
}

class FakeDevice final : public SndArDevice
{
public:
    struct Voice
    {
        bool used;
        int channels;
        int rate;
        bool started;
        std::deque<SndArWave *> waves;
        uint32_t played;
        float volume;
        float pitch;
        float mix[2][4];
        double frac;
    };
    Voice voices[kSndArMaxSources]{};
    uint64_t updates = 0;
    bool failInit = false;
    size_t poolLimit = 64u << 20;
    size_t poolUsed = 0;
    std::vector<void *> chunks;
    uint32_t inits = 0;
    uint32_t badMix = 0;
    uint32_t reverbCalls = 0;
    bool reverbOn = false;
    bool hasReverb = true;
    SndArI3dl2 reverb{};

    ~FakeDevice() override
    {
        for (void *chunk : chunks)
            std::free(chunk);
    }
    bool VoiceInit(int v, int channels, int rate) override
    {
        if (failInit || channels < 1 || channels > 2)
            return false;
        VoiceStop(v);
        voices[v] = Voice{};
        voices[v].used = true;
        voices[v].channels = channels;
        voices[v].rate = rate;
        voices[v].volume = 1.0f;
        voices[v].pitch = 1.0f;
        ++inits;
        return true;
    }
    void VoiceDrop(int v) override
    {
        VoiceStop(v);
        voices[v] = Voice{};
    }
    void VoiceStop(int v) override
    {
        for (SndArWave *w : voices[v].waves)
            Priv(w)->state = kDone;
        voices[v].waves.clear();
        voices[v].started = false;
        voices[v].played = 0;
        voices[v].frac = 0.0;
    }
    void VoiceSetPaused(int v, bool paused) override { voices[v].started = !paused; }
    bool VoiceAddWave(int v, SndArWave *w) override
    {
        if (!voices[v].used || (Priv(w)->state != kFree && Priv(w)->state != kDone))
            return false;
        if (reinterpret_cast<uintptr_t>(w->data) & (kSndArWaveAlign - 1))
            return false;
        Priv(w)->state = kQueued;
        Priv(w)->cursor = w->start;
        voices[v].waves.push_back(w);
        return true;
    }
    bool VoiceIsPlaying(int v) override { return voices[v].used && voices[v].started && !voices[v].waves.empty(); }
    uint32_t VoicePlayedSamples(int v) override { return voices[v].played; }
    void VoiceSetVolume(int v, float volume) override { voices[v].volume = volume; }
    void VoiceSetPitch(int v, float pitch) override { voices[v].pitch = pitch; }
    void VoiceSetMix(int v, int s, int o, float g) override
    {
        if (s < 0 || s > 1 || o < 0 || o > 3)
        {
            ++badMix;
            return;
        }
        voices[v].mix[s][o] = g;
    }
    bool SetReverb(const SndArI3dl2 *params) override
    {
        ++reverbCalls;
        if (!params)
        {
            reverbOn = false;
            return true;
        }
        if (!hasReverb)
            return false;
        reverbOn = true;
        reverb = *params;
        return true;
    }
    bool WaveDone(const SndArWave *w) override { return Priv(w)->state == kDone; }
    void WaveReset(SndArWave *w) override { std::memset(w->device, 0, sizeof(w->device)); }
    void *AllocPoolChunk(size_t bytes) override
    {
        if (poolUsed + bytes > poolLimit)
            return nullptr;
        void *mem = std::aligned_alloc(0x1000, bytes);
        poolUsed += bytes;
        chunks.push_back(mem);
        return mem;
    }
    void FlushData(const void *, size_t) override {}
    uint64_t UpdateCount() override { return updates; }
    bool Update() override
    {
        ++updates;
        return true;
    }

    // Renders `outFrames` output frames (48 kHz) on every started voice.
    void Advance(int outFrames)
    {
        for (Voice &voice : voices)
        {
            if (!voice.used || !voice.started)
                continue;
            voice.frac += outFrames * voice.pitch * voice.rate / 48000.0;
            int64_t want = static_cast<int64_t>(voice.frac);
            voice.frac -= want;
            while (want > 0 && !voice.waves.empty())
            {
                SndArWave *w = voice.waves.front();
                WavePriv *p = Priv(w);
                const int64_t left = w->end - p->cursor;
                const int64_t take = want < left ? want : left;
                p->cursor += take;
                want -= take;
                voice.played += static_cast<uint32_t>(take);
                if (p->cursor >= w->end)
                {
                    if (w->loop)
                    {
                        p->cursor = w->start;
                    }
                    else
                    {
                        p->state = kDone;
                        voice.waves.pop_front();
                    }
                }
            }
        }
    }
};

static void CapturePrint(const char *line)
{
    g_lines.push_back(line);
}

static int CountLines(const char *needle)
{
    int n = 0;
    for (const std::string &line : g_lines)
        n += line.find(needle) != std::string::npos ? 1 : 0;
    return n;
}

static ALint State(ALuint source)
{
    ALint state = 0;
    SndAr_alGetSourcei(source, AL_SOURCE_STATE, &state);
    return state;
}

static ALint Geti(ALuint source, ALenum param)
{
    ALint value = -1;
    SndAr_alGetSourcei(source, param, &value);
    return value;
}

// --- 1. pan law vs host openal-soft ------------------------------------------------
struct HostAl
{
    void *lib = nullptr;
    ALCdevice *(*loopbackOpen)(const ALCchar *) = nullptr;
    void (*render)(ALCdevice *, ALCvoid *, ALCsizei) = nullptr;
    ALCcontext *(*createContext)(ALCdevice *, const ALCint *) = nullptr;
    ALCboolean (*makeCurrent)(ALCcontext *) = nullptr;
    void (*destroyContext)(ALCcontext *) = nullptr;
    ALCboolean (*closeDevice)(ALCdevice *) = nullptr;
    void *(*alcProc)(ALCdevice *, const ALCchar *) = nullptr;
    void (*distanceModel)(ALenum) = nullptr;
    void (*listenerfv)(ALenum, const ALfloat *) = nullptr;
    void (*genBuffers)(ALsizei, ALuint *) = nullptr;
    void (*bufferData)(ALuint, ALenum, const ALvoid *, ALsizei, ALsizei) = nullptr;
    void (*genSources)(ALsizei, ALuint *) = nullptr;
    void (*sourcei)(ALuint, ALenum, ALint) = nullptr;
    void (*sourcef)(ALuint, ALenum, ALfloat) = nullptr;
    void (*source3f)(ALuint, ALenum, ALfloat, ALfloat, ALfloat) = nullptr;
    void (*play)(ALuint) = nullptr;
    void (*stop)(ALuint) = nullptr;

    template <typename T>
    bool Sym(T *out, const char *name)
    {
        *out = reinterpret_cast<T>(dlsym(lib, name));
        return *out != nullptr;
    }
    bool Load()
    {
        lib = dlopen("libopenal.so.1", RTLD_NOW);
        if (!lib)
            return false;
        bool ok = Sym(&createContext, "alcCreateContext") && Sym(&makeCurrent, "alcMakeContextCurrent") &&
                  Sym(&destroyContext, "alcDestroyContext") && Sym(&closeDevice, "alcCloseDevice") &&
                  Sym(&alcProc, "alcGetProcAddress") && Sym(&distanceModel, "alDistanceModel") &&
                  Sym(&listenerfv, "alListenerfv") && Sym(&genBuffers, "alGenBuffers") &&
                  Sym(&bufferData, "alBufferData") && Sym(&genSources, "alGenSources") && Sym(&sourcei, "alSourcei") &&
                  Sym(&sourcef, "alSourcef") && Sym(&source3f, "alSource3f") && Sym(&play, "alSourcePlay") &&
                  Sym(&stop, "alSourceStop");
        if (!ok)
            return false;
        loopbackOpen = reinterpret_cast<decltype(loopbackOpen)>(alcProc(nullptr, "alcLoopbackOpenDeviceSOFT"));
        render = reinterpret_cast<decltype(render)>(alcProc(nullptr, "alcRenderSamplesSOFT"));
        return loopbackOpen && render;
    }
};

static void TestPanAgainstHostOpenAl()
{
    // Closed-form spot values first (the formulas of snd_audren_pan.h).
    float l, r;
    SndAr_PanMono(0, 0, 0, &l, &r);
    CHECK(Near(l, 0.59566f, 1e-4f) && Near(r, 0.59566f, 1e-4f)); // 2D: straight ahead
    SndAr_PanMono(1, 0, 0, &l, &r);
    CHECK(Near(l, 0.0f) && Near(r, 1.0f));
    SndAr_PanMono(0, 0, 1, &l, &r);
    CHECK(Near(l, 0.40434f, 1e-4f) && Near(r, 0.40434f, 1e-4f));
    CHECK(SndAr_ClampGain(2.0f) == 1.0f && SndAr_ClampGain(-1.0f) == 0.0f && SndAr_ClampGain(NAN) == 0.0f);

    HostAl al;
    if (!al.Load())
    {
        std::printf("FAIL:SND_AUDREN_PAN_HOST libopenal.so.1 with ALC_SOFT_loopback is required on the test host\n");
        ++g_failures;
        return;
    }
    ALCdevice *device = al.loopbackOpen(nullptr);
    const ALCint attrs[] = {0x1990 /*ALC_FORMAT_CHANNELS_SOFT*/, 0x1501 /*ALC_STEREO_SOFT*/,
                            0x1991 /*ALC_FORMAT_TYPE_SOFT*/, 0x1406 /*ALC_FLOAT_SOFT*/, ALC_FREQUENCY, 48000, 0};
    ALCcontext *context = device ? al.createContext(device, attrs) : nullptr;
    if (!context || !al.makeCurrent(context))
    {
        std::printf("FAIL:SND_AUDREN_PAN_HOST loopback context\n");
        ++g_failures;
        return;
    }
    al.distanceModel(AL_NONE);
    const ALfloat orientation[6] = {0, 0, -1, 0, 1, 0};
    al.listenerfv(AL_ORIENTATION, orientation);
    const int frames = 48000;
    std::vector<short> mono(frames, 16384), left(frames * 2, 0), right(frames * 2, 0);
    for (int i = 0; i < frames; ++i)
    {
        left[2 * i] = 16384;
        right[2 * i + 1] = 16384;
    }
    ALuint buffers[3];
    al.genBuffers(3, buffers);
    al.bufferData(buffers[0], AL_FORMAT_MONO16, mono.data(), frames * 2, 48000);
    al.bufferData(buffers[1], AL_FORMAT_STEREO16, left.data(), frames * 4, 48000);
    al.bufferData(buffers[2], AL_FORMAT_STEREO16, right.data(), frames * 4, 48000);
    ALuint source;
    al.genSources(1, &source);
    std::vector<float> out(4096 * 2);

    struct Case
    {
        int channels;
        float x, y, z, gain;
    };
    std::vector<Case> cases;
    for (int deg = 0; deg < 360; deg += 10)
    {
        const float rad = deg * 3.14159265f / 180.0f;
        cases.push_back({1, std::sin(rad) * 250.0f, 0.0f, -std::cos(rad) * 250.0f, 1.0f});
        cases.push_back({1, std::sin(rad) * 40.0f, 25.0f, -std::cos(rad) * 40.0f, 0.6f});
    }
    cases.push_back({1, 0, 0, 0, 1});
    cases.push_back({1, 0, 1, 0, 1});
    cases.push_back({1, 0, -1, 0, 1});
    cases.push_back({1, 1, 0, 0, 3.0f});
    cases.push_back({2, 0, 0, 0, 1});
    cases.push_back({2, 300, 0, 0, 0.5f});
    float maxErr = 0.0f;
    int count = 0;
    for (const Case &c : cases)
    {
        const float pos[3] = {c.x, c.y, c.z};
        const SndArMix mix = SndAr_ComputeMix(c.channels, c.gain, pos);
        for (int ch = 0; ch < c.channels; ++ch)
        {
            al.stop(source);
            al.sourcei(source, AL_BUFFER, static_cast<ALint>(c.channels == 1 ? buffers[0] : buffers[1 + ch]));
            al.sourcei(source, AL_LOOPING, AL_TRUE);
            al.sourcef(source, AL_GAIN, c.gain);
            al.source3f(source, AL_POSITION, c.x, c.y, c.z);
            al.play(source);
            for (int k = 0; k < 3; ++k)
                al.render(device, out.data(), 4096);
            const float gl = out[6000] / 0.5f, gr = out[6001] / 0.5f;
            const float err = std::fmax(std::fabs(gl - mix.gain[ch][0]), std::fabs(gr - mix.gain[ch][1]));
            maxErr = std::fmax(maxErr, err);
            ++count;
            if (err > 0.012f)
            {
                std::printf("FAIL:SND_AUDREN_PAN_HOST ch=%d/%d pos=%.1f,%.1f,%.1f gain=%.2f openal=%.4f,%.4f ours=%.4f,%.4f\n",
                            ch, c.channels, c.x, c.y, c.z, c.gain, gl, gr, mix.gain[ch][0], mix.gain[ch][1]);
                ++g_failures;
            }
        }
    }
    al.makeCurrent(nullptr);
    al.destroyContext(context);
    al.closeDevice(device);
    std::printf("SND_AUDREN_PAN_HOST cases=%d max_err=%.4f\n", count, maxErr);
}

// --- 2. voice bookkeeping ---------------------------------------------------------------
static std::vector<int16_t> Ramp(int frames, int channels)
{
    std::vector<int16_t> pcm(static_cast<size_t>(frames) * channels);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(i);
    return pcm;
}

static void TestLoadedSounds(FakeDevice &dev)
{
    // Engine layout: 8 2D + 32 3D + 13 stream sources, then the cinematic one.
    ALuint sources[53];
    SndAr_alGenSources(53, sources);
    ALuint cinematic = 0;
    SndAr_alGenSources(1, &cinematic);
    CHECK(sources[0] == 1 && sources[52] == 53 && cinematic == 54);
    CHECK(SndAr_alGetError() == AL_NO_ERROR);

    // A 1 s mono 44.1 kHz sound on a 2D channel: straight ahead, at AL_GAIN.
    std::vector<int16_t> pcm = Ramp(44100, 1);
    ALuint buffer;
    SndAr_alGenBuffers(1, &buffer);
    SndAr_alBufferData(buffer, AL_FORMAT_MONO16, pcm.data(), static_cast<ALsizei>(pcm.size() * 2), 44100);
    const ALuint s2d = sources[0];
    SndAr_alSourcei(s2d, AL_BUFFER, static_cast<ALint>(buffer));
    SndAr_alSourcef(s2d, AL_PITCH, 1.0f);
    SndAr_alSourcef(s2d, AL_GAIN, 0.5f);
    SndAr_alSourcei(s2d, AL_LOOPING, AL_FALSE);
    CHECK(State(s2d) == AL_INITIAL);
    SndAr_alSourcePlay(s2d);
    const int v = SndAr_SourceVoice(s2d);
    CHECK(State(s2d) == AL_PLAYING);
    CHECK(dev.voices[v].rate == 44100 && dev.voices[v].channels == 1);
    CHECK(Near(dev.voices[v].volume, 0.5f));
    CHECK(Near(dev.voices[v].mix[0][0], 0.59566f, 1e-4f) && Near(dev.voices[v].mix[0][1], 0.59566f, 1e-4f));
    CHECK((reinterpret_cast<uintptr_t>(dev.voices[v].waves.front()->data) & 0x3F) == 0);

    // Half a second in: offset reads back in source frames and seconds.
    dev.Advance(24000);
    CHECK(Geti(s2d, AL_SAMPLE_OFFSET) == 22050);
    ALfloat sec = 0;
    SndAr_alGetSourcef(s2d, AL_SEC_OFFSET, &sec);
    CHECK(Near(sec, 0.5f, 1e-3f));

    // Pause holds the position; play resumes it (no restart).
    SndAr_alSourcePause(s2d);
    CHECK(State(s2d) == AL_PAUSED);
    dev.Advance(24000);
    CHECK(Geti(s2d, AL_SAMPLE_OFFSET) == 22050);
    SndAr_alSourcePlay(s2d);
    CHECK(State(s2d) == AL_PLAYING && Geti(s2d, AL_SAMPLE_OFFSET) == 22050);

    // Played out: STOPPED, the driver's "finished" signal.
    dev.Advance(30000);
    CHECK(State(s2d) == AL_STOPPED);

    // AL_BUFFER is refused while playing, a buffer in use cannot be deleted.
    SndAr_alSourcePlay(s2d);
    SndAr_alSourcei(s2d, AL_BUFFER, 0);
    CHECK(SndAr_alGetError() == AL_INVALID_OPERATION);
    SndAr_alDeleteBuffers(1, &buffer);
    CHECK(SndAr_alGetError() == AL_INVALID_OPERATION);

    // 3D channel: position pans, gain above 1 is clamped like AL_MAX_GAIN.
    const ALuint s3d = sources[8];
    SndAr_alSourcei(s3d, AL_BUFFER, static_cast<ALint>(buffer));
    SndAr_alSourcef(s3d, AL_GAIN, 4.0f);
    SndAr_alSource3f(s3d, AL_POSITION, 300.0f, 0.0f, 0.0f); // hard right
    SndAr_alSourcePlay(s3d);
    const int v3 = SndAr_SourceVoice(s3d);
    CHECK(Near(dev.voices[v3].volume, 1.0f));
    CHECK(Near(dev.voices[v3].mix[0][0], 0.0f) && Near(dev.voices[v3].mix[0][1], 1.0f));
    ALfloat gain = 0;
    SndAr_alGetSourcef(s3d, AL_GAIN, &gain);
    CHECK(gain == 4.0f); // AL returns the set value
    SndAr_alSource3f(s3d, AL_POSITION, -300.0f, 0.0f, 0.0f); // moves while playing
    CHECK(Near(dev.voices[v3].mix[0][0], 1.0f) && Near(dev.voices[v3].mix[0][1], 0.0f));
    SndAr_alSource3f(s3d, AL_POSITION, NAN, 0.0f, 0.0f); // rejected like OpenAL
    CHECK(SndAr_alGetError() == AL_INVALID_VALUE);
    ALfloat x, y, z;
    SndAr_alGetSource3f(s3d, AL_POSITION, &x, &y, &z);
    CHECK(x == -300.0f);
    SndAr_alSourcef(s3d, AL_PITCH, 100.0f);
    CHECK(Near(dev.voices[v3].pitch, 8.0f));

    // Looping from a random start: [offset, end) once, then [0, end) forever.
    const ALuint loopSrc = sources[9];
    SndAr_alSourcei(loopSrc, AL_BUFFER, static_cast<ALint>(buffer));
    SndAr_alSourcei(loopSrc, AL_LOOPING, AL_TRUE);
    SndAr_alSourcei(loopSrc, AL_SAMPLE_OFFSET, 40000);
    SndAr_alSourcePlay(loopSrc);
    const int vl = SndAr_SourceVoice(loopSrc);
    CHECK(dev.voices[vl].waves.size() == 2);
    CHECK(dev.voices[vl].waves[0]->start == 40000 && !dev.voices[vl].waves[0]->loop);
    CHECK(dev.voices[vl].waves[1]->start == 0 && dev.voices[vl].waves[1]->loop);
    dev.Advance(48000 * 3);
    CHECK(State(loopSrc) == AL_PLAYING);
    const int expect = (40000 + 44100 * 3 - 44100) % 44100;
    CHECK(std::abs(Geti(loopSrc, AL_SAMPLE_OFFSET) - expect) <= 1);
    SndAr_alSourceStop(loopSrc);
    CHECK(State(loopSrc) == AL_STOPPED && Geti(loopSrc, AL_SAMPLE_OFFSET) == 0);

    // Retire as the driver does on zone unload: stop, detach, delete.
    SndAr_alSourceStop(s2d);
    SndAr_alSourcei(s2d, AL_BUFFER, 0);
    SndAr_alSourceStop(s3d);
    SndAr_alSourcei(s3d, AL_BUFFER, 0);
    SndAr_alSourcei(loopSrc, AL_BUFFER, 0);
    SndAr_alDeleteBuffers(1, &buffer);
    CHECK(SndAr_alGetError() == AL_NO_ERROR);

    // Freed pool memory is reused only after two renderer updates.
    ALuint again;
    SndAr_alGenBuffers(1, &again);
    SndAr_alBufferData(again, AL_FORMAT_MONO16, pcm.data(), static_cast<ALsizei>(pcm.size() * 2), 44100);
    SndArStats stats;
    SndAr_GetStats(&stats);
    const uint64_t poolBefore = stats.poolBytes;
    SndAr_Update();
    SndAr_Update();
    ALuint third;
    SndAr_alGenBuffers(1, &third);
    SndAr_alBufferData(third, AL_FORMAT_MONO16, pcm.data(), static_cast<ALsizei>(pcm.size() * 2), 44100);
    SndAr_GetStats(&stats);
    CHECK(stats.poolBytes == poolBefore); // same chunk, no growth

    // 8-bit PCM is widened to 16-bit.
    const uint8_t pcm8[4] = {0, 128, 255, 64};
    ALuint b8;
    SndAr_alGenBuffers(1, &b8);
    SndAr_alBufferData(b8, AL_FORMAT_MONO8, pcm8, 4, 22050);
    SndAr_alSourcei(sources[1], AL_BUFFER, static_cast<ALint>(b8));
    SndAr_alSourcePlay(sources[1]);
    const int16_t *data = dev.voices[SndAr_SourceVoice(sources[1])].waves.front()->data;
    CHECK(data[0] == -32768 && data[1] == 0 && data[2] == 32512 && data[3] == -16384);

    // Stereo buffers are not spatialised.
    std::vector<int16_t> st = Ramp(1000, 2);
    ALuint bs;
    SndAr_alGenBuffers(1, &bs);
    SndAr_alBufferData(bs, AL_FORMAT_STEREO16, st.data(), static_cast<ALsizei>(st.size() * 2), 48000);
    SndAr_alSourcei(sources[2], AL_BUFFER, static_cast<ALint>(bs));
    SndAr_alSource3f(sources[2], AL_POSITION, 500.0f, 0.0f, 0.0f);
    SndAr_alSourcePlay(sources[2]);
    const FakeDevice::Voice &vs = dev.voices[SndAr_SourceVoice(sources[2])];
    CHECK(vs.channels == 2 && vs.mix[0][0] == 1.0f && vs.mix[0][1] == 0.0f && vs.mix[1][1] == 1.0f && vs.mix[1][0] == 0.0f);

    // A start that cannot get a voice fails loudly and leaves the source STOPPED.
    dev.failInit = true;
    const int failsBefore = CountLines("FAIL: SND_AUDREN_VOICE");
    SndAr_alSourcei(sources[3], AL_BUFFER, static_cast<ALint>(bs));
    SndAr_alSourcePlay(sources[3]);
    CHECK(State(sources[3]) == AL_STOPPED);
    CHECK(CountLines("FAIL: SND_AUDREN_VOICE") == failsBefore + 1);
    dev.failInit = false;
    SndAr_GetStats(&stats);
    CHECK(stats.playFails >= 1);

    // More sources than voices is refused loudly.
    ALuint extra[16];
    SndAr_alGenSources(16, extra);
    CHECK(extra[0] == 0 && SndAr_alGetError() == AL_OUT_OF_MEMORY);
    CHECK(CountLines("FAIL: SND_AUDREN_SOURCES") == 1);

    // A send to no slot is applied (send off), not skipped.
    SndAr_alSource3i(sources[0], AL_AUXILIARY_SEND_FILTER, 0, 0, 0);
    SndAr_GetStats(&stats);
    CHECK(stats.reverbSends == 1 && stats.reverbSendsSkipped == 0 && SndAr_alGetError() == AL_NO_ERROR);

    ALuint all[53];
    for (int i = 0; i < 53; ++i)
        all[i] = sources[i];
    SndAr_alDeleteSources(53, all);
    SndAr_alDeleteSources(1, &cinematic);
    ALuint rest[4] = {again, third, b8, bs};
    SndAr_alDeleteBuffers(4, rest);
    CHECK(SndAr_alGetError() == AL_NO_ERROR);
}

// The stream thread's cycle (snd_stream_openal.cpp ServiceJob): unqueue
// processed, refill, queue, play when not playing.
static void TestStreaming(FakeDevice &dev)
{
    ALuint source;
    SndAr_alGenSources(1, &source);
    const int kFrames = 8192;
    std::vector<int16_t> chunk = Ramp(kFrames, 2);
    ALuint buffers[8];
    SndAr_alGenBuffers(8, buffers);
    for (int i = 0; i < 3; ++i)
    {
        SndAr_alBufferData(buffers[i], AL_FORMAT_STEREO16, chunk.data(), kFrames * 4, 44100);
        SndAr_alSourceQueueBuffers(source, 1, &buffers[i]);
    }
    CHECK(Geti(source, AL_BUFFERS_QUEUED) == 3 && Geti(source, AL_BUFFERS_PROCESSED) == 0);
    CHECK(State(source) == AL_INITIAL);
    // A queued buffer cannot be re-specified.
    SndAr_alBufferData(buffers[0], AL_FORMAT_STEREO16, chunk.data(), kFrames * 4, 44100);
    CHECK(SndAr_alGetError() == AL_INVALID_OPERATION);
    SndAr_alSourcePlay(source);
    const int v = SndAr_SourceVoice(source);
    CHECK(State(source) == AL_PLAYING && dev.voices[v].waves.size() == 3);

    // 1.5 buffers in: one processed, offset inside the second.
    const int out15 = static_cast<int>(kFrames * 1.5 * 48000 / 44100);
    dev.Advance(out15);
    CHECK(Geti(source, AL_BUFFERS_PROCESSED) == 1);
    CHECK(std::abs(Geti(source, AL_SAMPLE_OFFSET) - kFrames / 2) <= 2);
    ALuint done = 0;
    SndAr_alSourceUnqueueBuffers(source, 1, &done);
    CHECK(done == buffers[0] && Geti(source, AL_BUFFERS_QUEUED) == 2);
    CHECK(std::abs(Geti(source, AL_SAMPLE_OFFSET) - kFrames / 2) <= 2);
    // Unqueueing more than processed is an error.
    ALuint tooMany[2];
    SndAr_alSourceUnqueueBuffers(source, 2, tooMany);
    CHECK(SndAr_alGetError() == AL_INVALID_VALUE);

    // Refill while playing goes straight to the voice.
    SndAr_alBufferData(done, AL_FORMAT_STEREO16, chunk.data(), kFrames * 4, 44100);
    SndAr_alSourceQueueBuffers(source, 1, &done);
    CHECK(dev.voices[v].waves.size() == 3);
    // A different format on the same queue is refused.
    SndAr_alBufferData(buffers[5], AL_FORMAT_MONO16, chunk.data(), kFrames * 2, 44100);
    SndAr_alSourceQueueBuffers(source, 1, &buffers[5]);
    CHECK(SndAr_alGetError() == AL_INVALID_OPERATION);

    // Pause keeps the queue, nothing is consumed.
    SndAr_alSourcePause(source);
    dev.Advance(48000);
    CHECK(State(source) == AL_PAUSED && Geti(source, AL_BUFFERS_PROCESSED) == 0);
    SndAr_alSourcePlay(source);

    // Underrun: the queue drains, the source STOPS with every buffer
    // processed; the stream thread unqueues, refills, plays again.
    dev.Advance(48000);
    CHECK(State(source) == AL_STOPPED);
    CHECK(Geti(source, AL_BUFFERS_PROCESSED) == 3);
    ALuint back[3];
    SndAr_alSourceUnqueueBuffers(source, 3, back);
    CHECK(Geti(source, AL_BUFFERS_QUEUED) == 0);
    for (int i = 0; i < 2; ++i)
    {
        SndAr_alBufferData(back[i], AL_FORMAT_STEREO16, chunk.data(), kFrames * 4, 44100);
        SndAr_alSourceQueueBuffers(source, 1, &back[i]);
    }
    CHECK(Geti(source, AL_BUFFERS_PROCESSED) == 2); // stopped source: OpenAL counts all as processed
    SndAr_alSourcePlay(source);
    CHECK(State(source) == AL_PLAYING && Geti(source, AL_BUFFERS_PROCESSED) == 0);
    CHECK(dev.voices[v].waves.size() == 2 && Geti(source, AL_SAMPLE_OFFSET) == 0);

    // ReleaseJob: stop, detach, delete.
    SndAr_alSourceStop(source);
    SndAr_alSourcei(source, AL_BUFFER, 0);
    CHECK(Geti(source, AL_BUFFERS_QUEUED) == 0);
    SndAr_alDeleteBuffers(8, buffers);
    CHECK(SndAr_alGetError() == AL_NO_ERROR);
    SndAr_alDeleteSources(1, &source);

    // Pool budget exhaustion is loud.
    dev.poolLimit = dev.poolUsed;
    ALuint big;
    SndAr_alGenBuffers(1, &big);
    std::vector<int16_t> huge(kSndArChunkBytes); // 2x a chunk
    SndAr_alBufferData(big, AL_FORMAT_MONO16, huge.data(), static_cast<ALsizei>(huge.size() * 2), 48000);
    CHECK(SndAr_alGetError() == AL_OUT_OF_MEMORY);
    CHECK(CountLines("FAIL: SND_AUDREN_POOL") == 1);
    SndAr_alDeleteBuffers(1, &big);
}

// --- 4. EFX reverb on the DSP -----------------------------
// The 26 room presets SND_SetRoomtype loads (snd_driver_openal.cpp
// AL_RoomPresets, same order as snd_roomStrings).
static const EFXEAXREVERBPROPERTIES kRoomPresets[26] = {
    EFX_REVERB_PRESET_GENERIC,        EFX_REVERB_PRESET_PADDEDCELL,    EFX_REVERB_PRESET_ROOM,
    EFX_REVERB_PRESET_BATHROOM,       EFX_REVERB_PRESET_LIVINGROOM,    EFX_REVERB_PRESET_STONEROOM,
    EFX_REVERB_PRESET_AUDITORIUM,     EFX_REVERB_PRESET_CONCERTHALL,   EFX_REVERB_PRESET_CAVE,
    EFX_REVERB_PRESET_ARENA,          EFX_REVERB_PRESET_HANGAR,        EFX_REVERB_PRESET_CARPETEDHALLWAY,
    EFX_REVERB_PRESET_HALLWAY,        EFX_REVERB_PRESET_STONECORRIDOR, EFX_REVERB_PRESET_ALLEY,
    EFX_REVERB_PRESET_FOREST,         EFX_REVERB_PRESET_CITY,          EFX_REVERB_PRESET_MOUNTAINS,
    EFX_REVERB_PRESET_QUARRY,         EFX_REVERB_PRESET_PLAIN,         EFX_REVERB_PRESET_PARKINGLOT,
    EFX_REVERB_PRESET_SEWERPIPE,      EFX_REVERB_PRESET_UNDERWATER,    EFX_REVERB_PRESET_DRUGGED,
    EFX_REVERB_PRESET_DIZZY,          EFX_REVERB_PRESET_PSYCHOTIC,
};

// What SND_SetRoomtype sends, as the model's EFX property block.
static SndArEfxReverb FromPreset(const EFXEAXREVERBPROPERTIES &p)
{
    SndArEfxReverb e;
    e.density = p.flDensity;
    e.diffusion = p.flDiffusion;
    e.gain = p.flGain;
    e.gainHF = p.flGainHF;
    e.gainLF = p.flGainLF;
    e.decayTime = p.flDecayTime;
    e.decayHFRatio = p.flDecayHFRatio;
    e.decayLFRatio = p.flDecayLFRatio;
    e.reflectionsGain = p.flReflectionsGain;
    e.reflectionsDelay = p.flReflectionsDelay;
    e.lateReverbGain = p.flLateReverbGain;
    e.lateReverbDelay = p.flLateReverbDelay;
    e.echoTime = p.flEchoTime;
    e.echoDepth = p.flEchoDepth;
    e.modulationTime = p.flModulationTime;
    e.modulationDepth = p.flModulationDepth;
    e.airAbsorptionGainHF = p.flAirAbsorptionGainHF;
    e.hfReference = p.flHFReference;
    e.lfReference = p.flLFReference;
    e.roomRolloffFactor = p.flRoomRolloffFactor;
    e.decayHFLimit = p.iDecayHFLimit;
    return e;
}

// Independent (double precision) statement of the conversion documented in
// snd_audren_reverb.h: I3DL2 millibels = 2000 log10(linear gain) = 100 x dB.
static double RefMb(double gain, double lo, double hi)
{
    if (gain <= 0.0)
        return lo;
    const double mb = 20.0 * std::log10(gain) * 100.0;
    return mb < lo ? lo : (mb > hi ? hi : mb);
}

static double RefHfRatio(const EFXEAXREVERBPROPERTIES &p)
{
    double ratio = p.flDecayHFRatio;
    if (p.iDecayHFLimit && p.flAirAbsorptionGainHF < 1.0f)
    {
        // Air absorption over the distance sound travels during the decay:
        // the HF ratio may not exceed the one that absorption alone gives.
        const double len = std::log10((double)p.flAirAbsorptionGainHF) * p.flDecayTime / std::log10(0.001);
        const double limit = 1.0 / (343.3 * len);
        ratio = limit < ratio ? limit : ratio;
    }
    return ratio < 0.1 ? 0.1 : (ratio > 2.0 ? 2.0 : ratio);
}

static void TestReverbConversion()
{
    int checked = 0;
    for (const EFXEAXREVERBPROPERTIES &p : kRoomPresets)
    {
        const SndArI3dl2 r = SndAr_EfxToI3dl2(FromPreset(p));
        CHECK(Near(r.roomGain, (float)RefMb(p.flGain, -10000, 0), 0.05f));
        CHECK(Near(r.roomHf, (float)RefMb(p.flGainHF, -10000, 0), 0.05f));
        CHECK(Near(r.reflectionsGain, (float)RefMb(p.flReflectionsGain, -10000, 1000), 0.05f));
        CHECK(Near(r.reverbGain, (float)RefMb(p.flLateReverbGain, -10000, 2000), 0.05f));
        CHECK(Near(r.decayTime, p.flDecayTime, 1e-6f));
        CHECK(Near(r.hfDecayRatio, (float)RefHfRatio(p), 1e-4f));
        CHECK(Near(r.reflectionDelay, p.flReflectionsDelay, 1e-6f));
        CHECK(Near(r.reverbDelay, p.flLateReverbDelay, 1e-6f));
        CHECK(Near(r.diffusion, 100.0f * p.flDiffusion, 1e-4f));
        CHECK(Near(r.density, 100.0f * p.flDensity, 1e-4f));
        CHECK(Near(r.hfReference, p.flHFReference, 1e-3f));
        CHECK(r.dryGain == 0.0f);
        // Inside the renderer's I3DL2 ranges.
        CHECK(r.roomGain >= -10000 && r.roomGain <= 0 && r.roomHf >= -10000 && r.roomHf <= 0);
        CHECK(r.reflectionsGain >= -10000 && r.reflectionsGain <= 1000);
        CHECK(r.reverbGain >= -10000 && r.reverbGain <= 2000);
        CHECK(r.decayTime >= 0.1f && r.decayTime <= 20.0f && r.hfDecayRatio >= 0.1f && r.hfDecayRatio <= 2.0f);
        CHECK(r.reflectionDelay >= 0 && r.reflectionDelay <= 0.3f && r.reverbDelay >= 0 && r.reverbDelay <= 0.1f);
        // The renderer's 10^(mB/2000) gives back the EFX linear gain.
        CHECK(Near(std::pow(10.0f, r.roomGain / 2000.0f), p.flGain, 1e-4f));
        CHECK(Near(std::pow(10.0f, r.reverbGain / 2000.0f), p.flLateReverbGain, 1e-3f));
        ++checked;
    }
    // Anchors: EFX GENERIC is the I3DL2/EAX GENERIC environment
    // (room -1000, roomHF -100, reflections -2602, reverb 200 mB).
    const SndArI3dl2 g = SndAr_EfxToI3dl2(FromPreset(kRoomPresets[0]));
    CHECK(Near(g.roomGain, -1000.0f, 1.0f) && Near(g.roomHf, -100.0f, 1.0f));
    CHECK(Near(g.reflectionsGain, -2602.0f, 1.0f) && Near(g.reverbGain, 200.0f, 1.0f));
    CHECK(Near(g.decayTime, 1.49f, 1e-6f) && Near(g.hfDecayRatio, 0.83f, 1e-6f));
    // HANGAR (10 s decay) is limited by air absorption only when the ratio
    // exceeds it; a 20 s, ratio-2 room is capped well below 2.
    SndArEfxReverb longRoom = FromPreset(kRoomPresets[0]);
    longRoom.decayTime = 20.0f;
    longRoom.decayHFRatio = 2.0f;
    const SndArI3dl2 lr = SndAr_EfxToI3dl2(longRoom);
    CHECK(lr.hfDecayRatio < 0.5f && lr.hfDecayRatio > 0.1f);
    longRoom.decayHFLimit = 0;
    CHECK(Near(SndAr_EfxToI3dl2(longRoom).hfDecayRatio, 2.0f, 1e-6f));
    SndArEfxReverb silent;
    silent.gain = 0.0f;
    silent.reflectionsGain = 0.0f;
    CHECK(SndAr_EfxToI3dl2(silent).roomGain == -10000.0f && SndAr_EfxToI3dl2(silent).reflectionsGain == -10000.0f);

    // Wire encoding: reference-renderer Reverb3dParameter field offsets.
    SndArEffectInWire fx{};
    SndAr_EncodeReverb3d(g, 2, 48000, kSndArUsageNew, &fx);
    auto f32 = [&](size_t off) {
        float v;
        std::memcpy(&v, fx.specific + off, 4);
        return v;
    };
    CHECK(fx.specific[0] == 2 && fx.specific[1] == 3 && fx.specific[6] == 2 && fx.specific[7] == 3);
    CHECK(fx.specific[0x0C] == 2 && fx.specific[0x0E] == 2); // channel count max / count
    uint32_t rate;
    std::memcpy(&rate, fx.specific + 0x14, 4);
    CHECK(rate == 48000);
    CHECK(f32(0x18) == g.roomHf && f32(0x1C) == g.hfReference && f32(0x20) == g.decayTime);
    CHECK(f32(0x24) == g.hfDecayRatio && f32(0x28) == g.roomGain && f32(0x2C) == g.reflectionsGain);
    CHECK(f32(0x30) == g.reverbGain && f32(0x34) == g.diffusion && f32(0x38) == g.reflectionDelay);
    CHECK(f32(0x3C) == g.reverbDelay && f32(0x40) == g.density && f32(0x44) == 0.0f);
    CHECK(fx.specific[0x48] == kSndArUsageNew);
    // Wet fold: BufferMixParameter {Input[24], Output[24], Volumes[24], MixesCount}.
    SndArEffectInWire fold{};
    SndAr_EncodeBusFold(2, &fold);
    uint32_t mixes;
    std::memcpy(&mixes, fold.specific + 0x90, 4);
    float vol0, vol1;
    std::memcpy(&vol0, fold.specific + 0x30, 4);
    std::memcpy(&vol1, fold.specific + 0x34, 4);
    CHECK(fold.specific[0] == 2 && fold.specific[1] == 3 && fold.specific[24] == 0 && fold.specific[25] == 1);
    CHECK(mixes == 2 && vol0 == 1.0f && vol1 == 1.0f);
    std::printf("SND_AUDREN_REVERB_CONVERSION presets=%d generic_room_mb=%.1f generic_reverb_mb=%.1f\n", checked,
                (double)g.roomGain, (double)g.reverbGain);
}

// The update splice libnx's audrv needs (snd_audren_switch.cpp wraps its
// renderer call with these two).
static void TestReverbSplice()
{
    // An effect-less audrv input: header | behavior 0x10 | mempools 0x40 |
    // channels 0x70 | voices 0x170 | mixes 0x930 | sinks 0x140 | perf 0x10.
    SndArUpdateHeader h{};
    h.behaviorSz = 0x10;
    h.mempoolsSz = 0x40;
    h.channelsSz = 0x70;
    h.voicesSz = 0x170;
    h.mixesSz = 0x930;
    h.sinksSz = 0x140;
    h.perfmgrSz = 0x10;
    const size_t head = 0x40 + 0x10 + 0x40 + 0x70 + 0x170;
    const size_t tail = 0x930 + 0x140 + 0x10;
    h.totalSz = static_cast<uint32_t>(head + tail);
    std::vector<unsigned char> in(h.totalSz);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<unsigned char>(i * 7 + 1);
    std::memcpy(in.data(), &h, sizeof(h));
    SndArEffectInWire fx{};
    fx.type = kSndArEffectTypeReverb3d;
    fx.mixId = 1;
    std::vector<unsigned char> out(in.size() + sizeof(fx));
    CHECK(SndAr_SpliceEffects(in.data(), in.size(), &fx, sizeof(fx), out.data(), out.size() - 1) == 0);
    const size_t n = SndAr_SpliceEffects(in.data(), in.size(), &fx, sizeof(fx), out.data(), out.size());
    CHECK(n == in.size() + 0xC0);
    SndArUpdateHeader oh;
    std::memcpy(&oh, out.data(), sizeof(oh));
    CHECK(oh.effectsSz == 0xC0 && oh.totalSz == n && oh.voicesSz == h.voicesSz && oh.mixesSz == h.mixesSz);
    CHECK(std::memcmp(out.data() + sizeof(oh), in.data() + sizeof(oh), head - sizeof(oh)) == 0);
    CHECK(std::memcmp(out.data() + head, &fx, sizeof(fx)) == 0);
    CHECK(std::memcmp(out.data() + head + sizeof(fx), in.data() + head, tail) == 0);
    // Already spliced / truncated inputs are refused, not mangled.
    CHECK(SndAr_SpliceEffects(out.data(), n, &fx, sizeof(fx), in.data(), in.size()) == 0);
    CHECK(SndAr_SpliceEffects(in.data(), in.size() - 4, &fx, sizeof(fx), out.data(), out.size()) == 0);

    // Renderer output: header | mempools 0x20 | voices 0x80 | effects 0x10 |
    // sinks 0x40 | perf 0x10 | behavior 0xB0 with 2 error records.
    SndArUpdateHeader r{};
    r.mempoolsSz = 0x20;
    r.voicesSz = 0x80;
    r.effectsSz = 0x10;
    r.sinksSz = 0x40;
    r.perfmgrSz = 0x10;
    r.behaviorSz = 0xB0;
    const size_t rsize = 0x40 + 0x20 + 0x80 + 0x10 + 0x40 + 0x10 + 0xB0;
    r.totalSz = static_cast<uint32_t>(rsize);
    std::vector<unsigned char> ro(rsize);
    for (size_t i = 0; i < ro.size(); ++i)
        ro[i] = static_cast<unsigned char>(i * 3 + 5);
    std::memcpy(ro.data(), &r, sizeof(r));
    ro[0x40 + 0x20 + 0x80] = kSndArEffectStateEnabled;
    const size_t beh = 0x40 + 0x20 + 0x80 + 0x10 + 0x40 + 0x10;
    const uint32_t code = 0x2a07, count = 2;
    std::memcpy(ro.data() + beh, &code, 4);
    std::memcpy(ro.data() + beh + 0xA0, &count, 4);
    std::vector<unsigned char> lo(rsize - 0x10);
    SndArEffectOutWire st{};
    uint32_t first = 0;
    CHECK(SndAr_UnspliceEffects(ro.data(), ro.size(), lo.data(), lo.size(), &st, 1, &first) == 2);
    CHECK(st.state == kSndArEffectStateEnabled && first == code);
    SndArUpdateHeader lh;
    std::memcpy(&lh, lo.data(), sizeof(lh));
    CHECK(lh.effectsSz == 0 && lh.totalSz == rsize - 0x10 && lh.voicesSz == 0x80);
    CHECK(std::memcmp(lo.data() + 0x40, ro.data() + 0x40, 0xA0) == 0);            // mempools + voices
    CHECK(std::memcmp(lo.data() + 0xE0, ro.data() + 0xF0, rsize - 0xF0) == 0);    // sinks.. after
    CHECK(SndAr_UnspliceEffects(ro.data(), ro.size(), lo.data(), lo.size(), &st, 2, &first) == -1);
    std::printf("SND_AUDREN_REVERB_SPLICE in=%zu spliced=%zu\n", in.size(), n);
}

// Sends and the slot/effect state as SND_SetRoomtype / SND_ApplyReverbSend
// drive them, against the fake renderer's mix factors.
static void TestReverbSends(FakeDevice &dev)
{
    const size_t lines0 = g_lines.size();
    SndArStats st0{}, st{};
    SndAr_GetStats(&st0);
    ALuint slot = 0, effect = 0, filters[3] = {};
    SndAr_alGenAuxiliaryEffectSlots(1, &slot);
    SndAr_alGenEffects(1, &effect);
    SndAr_alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
    SndAr_alGenFilters(3, filters);
    for (ALuint f : filters)
        SndAr_alFilteri(f, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    CHECK(slot && effect && filters[2] && SndAr_alGetError() == AL_NO_ERROR);

    ALuint src[2];
    SndAr_alGenSources(2, src);
    std::vector<int16_t> mono = Ramp(4800, 1), stereo = Ramp(4800, 2);
    ALuint buf[2];
    SndAr_alGenBuffers(2, buf);
    SndAr_alBufferData(buf[0], AL_FORMAT_MONO16, mono.data(), static_cast<ALsizei>(mono.size() * 2), 48000);
    SndAr_alBufferData(buf[1], AL_FORMAT_STEREO16, stereo.data(), static_cast<ALsizei>(stereo.size() * 2), 48000);
    SndAr_alSourcei(src[0], AL_BUFFER, static_cast<ALint>(buf[0]));
    SndAr_alSourcei(src[1], AL_BUFFER, static_cast<ALint>(buf[1]));
    SndAr_alSourcei(src[0], AL_LOOPING, AL_TRUE);
    SndAr_alSourcei(src[1], AL_LOOPING, AL_TRUE);
    SndAr_alSourcef(src[0], AL_GAIN, 0.5f);
    SndAr_alSourcePlay(src[0]);
    SndAr_alSourcePlay(src[1]);
    const int v0 = SndAr_SourceVoice(src[0]), v1 = SndAr_SourceVoice(src[1]);

    // SND_ApplyReverbSend before any room: the slot is empty, nothing wet.
    SndAr_alFilterf(filters[0], AL_LOWPASS_GAIN, 0.8f);
    SndAr_alFilterf(filters[0], AL_LOWPASS_GAINHF, 1.0f);
    SndAr_alSource3i(src[0], AL_AUXILIARY_SEND_FILTER, static_cast<ALint>(slot), 0, static_cast<ALint>(filters[0]));
    CHECK(dev.voices[v0].mix[0][2] == 0.0f && dev.voices[v0].mix[0][3] == 0.0f);
    CHECK(!dev.reverbOn);

    // SND_SetRoomtype(stoneroom): effect properties, then into the slot.
    const SndArEfxReverb stone = FromPreset(kRoomPresets[5]);
    const EFXEAXREVERBPROPERTIES &sp = kRoomPresets[5];
    SndAr_alEffectf(effect, AL_EAXREVERB_DENSITY, sp.flDensity);
    SndAr_alEffectf(effect, AL_EAXREVERB_DIFFUSION, sp.flDiffusion);
    SndAr_alEffectf(effect, AL_EAXREVERB_GAIN, sp.flGain);
    SndAr_alEffectf(effect, AL_EAXREVERB_GAINHF, sp.flGainHF);
    SndAr_alEffectf(effect, AL_EAXREVERB_GAINLF, sp.flGainLF);
    SndAr_alEffectf(effect, AL_EAXREVERB_DECAY_TIME, sp.flDecayTime);
    SndAr_alEffectf(effect, AL_EAXREVERB_DECAY_HFRATIO, sp.flDecayHFRatio);
    SndAr_alEffectf(effect, AL_EAXREVERB_DECAY_LFRATIO, sp.flDecayLFRatio);
    SndAr_alEffectf(effect, AL_EAXREVERB_REFLECTIONS_GAIN, sp.flReflectionsGain);
    SndAr_alEffectf(effect, AL_EAXREVERB_REFLECTIONS_DELAY, sp.flReflectionsDelay);
    SndAr_alEffectfv(effect, AL_EAXREVERB_REFLECTIONS_PAN, sp.flReflectionsPan);
    SndAr_alEffectf(effect, AL_EAXREVERB_LATE_REVERB_GAIN, sp.flLateReverbGain);
    SndAr_alEffectf(effect, AL_EAXREVERB_LATE_REVERB_DELAY, sp.flLateReverbDelay);
    SndAr_alEffectfv(effect, AL_EAXREVERB_LATE_REVERB_PAN, sp.flLateReverbPan);
    SndAr_alEffectf(effect, AL_EAXREVERB_ECHO_TIME, sp.flEchoTime);
    SndAr_alEffectf(effect, AL_EAXREVERB_ECHO_DEPTH, sp.flEchoDepth);
    SndAr_alEffectf(effect, AL_EAXREVERB_MODULATION_TIME, sp.flModulationTime);
    SndAr_alEffectf(effect, AL_EAXREVERB_MODULATION_DEPTH, sp.flModulationDepth);
    SndAr_alEffectf(effect, AL_EAXREVERB_AIR_ABSORPTION_GAINHF, sp.flAirAbsorptionGainHF);
    SndAr_alEffectf(effect, AL_EAXREVERB_HFREFERENCE, sp.flHFReference);
    SndAr_alEffectf(effect, AL_EAXREVERB_LFREFERENCE, sp.flLFReference);
    SndAr_alEffectf(effect, AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, sp.flRoomRolloffFactor);
    SndAr_alEffecti(effect, AL_EAXREVERB_DECAY_HFLIMIT, sp.iDecayHFLimit);
    CHECK(SndAr_alGetError() == AL_NO_ERROR);
    CHECK(!dev.reverbOn); // nothing reaches the DSP until the slot takes the effect
    SndAr_alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, static_cast<ALint>(effect));
    const SndArI3dl2 want = SndAr_EfxToI3dl2(stone);
    CHECK(dev.reverbOn && std::memcmp(&dev.reverb, &want, sizeof(want)) == 0);
    // Mono: wet/2 on each send-bus channel (the DSP reverb sums them), dry untouched.
    CHECK(Near(dev.voices[v0].mix[0][2], 0.4f) && Near(dev.voices[v0].mix[0][3], 0.4f));
    CHECK(Near(dev.voices[v0].mix[0][0], 0.59566f, 1e-4f) && Near(dev.voices[v0].mix[0][1], 0.59566f, 1e-4f));
    // DSP input of the reverb = voice volume x (bus L + bus R) = AL_GAIN x send gain.
    CHECK(Near(dev.voices[v0].volume * (dev.voices[v0].mix[0][2] + dev.voices[v0].mix[0][3]), 0.5f * 0.8f));

    // OpenAL copies the filter at send time: a later filter change alone does nothing.
    SndAr_alFilterf(filters[0], AL_LOWPASS_GAIN, 0.2f);
    CHECK(Near(dev.voices[v0].mix[0][2], 0.4f));
    SndAr_alSource3i(src[0], AL_AUXILIARY_SEND_FILTER, static_cast<ALint>(slot), 0, static_cast<ALint>(filters[0]));
    CHECK(Near(dev.voices[v0].mix[0][2], 0.1f) && Near(dev.voices[v0].mix[0][3], 0.1f));

    // Stereo: each channel onto its own bus channel at the send gain.
    SndAr_alFilterf(filters[1], AL_LOWPASS_GAIN, 0.6f);
    SndAr_alSource3i(src[1], AL_AUXILIARY_SEND_FILTER, static_cast<ALint>(slot), 0, static_cast<ALint>(filters[1]));
    CHECK(Near(dev.voices[v1].mix[0][2], 0.6f) && Near(dev.voices[v1].mix[1][3], 0.6f));
    CHECK(dev.voices[v1].mix[0][3] == 0.0f && dev.voices[v1].mix[1][2] == 0.0f);
    CHECK(dev.voices[v1].mix[0][0] == 1.0f && dev.voices[v1].mix[1][1] == 1.0f);
    SndAr_GetStats(&st);
    CHECK(st.reverbOn && st.reverbSends - st0.reverbSends == 3 && st.reverbSendsSkipped == 0 && st.wetVoices == 2);
    CHECK(st.reverbPresets == 1 && st.efxSkipped == 0);

    // A restarted voice keeps its send.
    SndAr_alSourceStop(src[0]);
    SndAr_alSourcePlay(src[0]);
    CHECK(Near(dev.voices[SndAr_SourceVoice(src[0])].mix[0][2], 0.1f));

    // Room change: new parameters, sends stay.
    const SndArEfxReverb cave = FromPreset(kRoomPresets[8]);
    SndAr_alEffectf(effect, AL_EAXREVERB_DECAY_TIME, cave.decayTime);
    SndAr_alEffectf(effect, AL_EAXREVERB_DECAY_HFRATIO, cave.decayHFRatio);
    SndAr_alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, static_cast<ALint>(effect));
    CHECK(Near(dev.reverb.decayTime, cave.decayTime) && Near(dev.voices[v1].mix[0][2], 0.6f));

    // Empty slot: the DSP reverb is bypassed and nothing is sent to it
    // (a bypassed in-place effect would pass the send bus through dry).
    SndAr_alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, 0);
    CHECK(!dev.reverbOn && dev.voices[v1].mix[0][2] == 0.0f && dev.voices[v1].mix[1][3] == 0.0f);
    SndAr_alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, static_cast<ALint>(effect));
    CHECK(dev.reverbOn && Near(dev.voices[v1].mix[1][3], 0.6f));

    // No slot / no filter: send off / unit gain.
    SndAr_alSource3i(src[1], AL_AUXILIARY_SEND_FILTER, 0, 0, 0);
    CHECK(dev.voices[v1].mix[0][2] == 0.0f);
    SndAr_alSource3i(src[1], AL_AUXILIARY_SEND_FILTER, static_cast<ALint>(slot), 0, 0);
    CHECK(Near(dev.voices[v1].mix[0][2], 1.0f));
    CHECK(SndAr_alGetError() == AL_NO_ERROR);

    // Unsupported: second send, per-send HF cut, a second reverb slot, other
    // effect types -- each a FAIL line and a counter, never silent.
    SndAr_alSource3i(src[1], AL_AUXILIARY_SEND_FILTER, static_cast<ALint>(slot), 1, 0);
    CHECK(SndAr_alGetError() == AL_INVALID_VALUE);
    SndAr_alFilterf(filters[2], AL_LOWPASS_GAINHF, 0.5f);
    SndAr_alSource3i(src[1], AL_AUXILIARY_SEND_FILTER, static_cast<ALint>(slot), 0, static_cast<ALint>(filters[2]));
    ALuint slot2 = 0;
    SndAr_alGenAuxiliaryEffectSlots(1, &slot2);
    SndAr_alAuxiliaryEffectSloti(slot2, AL_EFFECTSLOT_EFFECT, static_cast<ALint>(effect));
    CHECK(SndAr_alGetError() == AL_INVALID_OPERATION);
    ALuint chorus = 0;
    SndAr_alGenEffects(1, &chorus);
    SndAr_alEffecti(chorus, AL_EFFECT_TYPE, AL_EFFECT_CHORUS);
    CHECK(SndAr_alGetError() == AL_INVALID_VALUE);
    SndAr_alEffectf(effect, AL_EAXREVERB_DECAY_TIME, 50.0f);
    CHECK(SndAr_alGetError() == AL_INVALID_VALUE);
    SndAr_GetStats(&st);
    CHECK(st.reverbSendsSkipped == 1 && st.efxSkipped == 3);
    CHECK(CountLines("FAIL: SND_AUDREN_REVERB_SEND") >= 1 && CountLines("FAIL: SND_AUDREN_EFX") >= 3);

    // A renderer without a reverb effect fails loudly and sends nothing.
    SndAr_alDeleteAuxiliaryEffectSlots(1, &slot2);
    dev.hasReverb = false;
    SndAr_alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, 0);
    SndAr_alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, static_cast<ALint>(effect));
    CHECK(SndAr_alGetError() == AL_INVALID_OPERATION);
    CHECK(dev.voices[v1].mix[0][2] == 0.0f && CountLines("renderer has no reverb effect") == 1);
    dev.hasReverb = true;

    // Shutdown order of MSS_Shutdown: sources, filters, slot, effect.
    SndAr_alDeleteSources(2, src);
    SndAr_alDeleteFilters(3, filters);
    SndAr_alDeleteAuxiliaryEffectSlots(1, &slot);
    SndAr_alDeleteEffects(1, &effect);
    SndAr_alDeleteEffects(1, &chorus);
    SndAr_alDeleteBuffers(2, buf);
    CHECK(!dev.reverbOn && dev.badMix == 0);
    SndAr_alGetError();
    std::printf("SND_AUDREN_REVERB_SENDS sends=%u reverb_calls=%u fail_lines=%zu\n", st.reverbSends, dev.reverbCalls,
                g_lines.size() - lines0);
}

// The Switch runs SndAr_Update on the renderer-frame thread while the game
// thread starts/stops sounds and the stream thread refills queues; the model
// lock must make that race-free (run under TSan by ./test host).
class ConsumingDevice final : public SndArDevice
{
public:
    FakeDevice inner;
    bool VoiceInit(int v, int c, int r) override { return inner.VoiceInit(v, c, r); }
    void VoiceDrop(int v) override { inner.VoiceDrop(v); }
    void VoiceStop(int v) override { inner.VoiceStop(v); }
    void VoiceSetPaused(int v, bool p) override { inner.VoiceSetPaused(v, p); }
    bool VoiceAddWave(int v, SndArWave *w) override { return inner.VoiceAddWave(v, w); }
    bool VoiceIsPlaying(int v) override { return inner.VoiceIsPlaying(v); }
    uint32_t VoicePlayedSamples(int v) override { return inner.VoicePlayedSamples(v); }
    void VoiceSetVolume(int v, float x) override { inner.VoiceSetVolume(v, x); }
    void VoiceSetPitch(int v, float x) override { inner.VoiceSetPitch(v, x); }
    void VoiceSetMix(int v, int s, int o, float g) override { inner.VoiceSetMix(v, s, o, g); }
    bool SetReverb(const SndArI3dl2 *p) override { return inner.SetReverb(p); }
    bool WaveDone(const SndArWave *w) override { return inner.WaveDone(w); }
    void WaveReset(SndArWave *w) override { inner.WaveReset(w); }
    void *AllocPoolChunk(size_t b) override { return inner.AllocPoolChunk(b); }
    void FlushData(const void *d, size_t b) override { inner.FlushData(d, b); }
    uint64_t UpdateCount() override { return inner.UpdateCount(); }
    bool Update() override
    {
        inner.Advance(240); // one 5 ms renderer frame
        return inner.Update();
    }
};

static void TestConcurrency()
{
    ConsumingDevice dev;
    SndAr_Attach(&dev, CapturePrint);
    std::atomic<bool> quit{false};
    std::thread updater([&] {
        while (!quit.load())
            SndAr_Update();
    });
    ALuint sources[4];
    SndAr_alGenSources(4, sources);
    ALuint slot = 0, effect = 0, sendFilter = 0;
    SndAr_alGenAuxiliaryEffectSlots(1, &slot);
    SndAr_alGenEffects(1, &effect);
    SndAr_alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
    SndAr_alGenFilters(1, &sendFilter);
    SndAr_alFilteri(sendFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    std::vector<int16_t> pcm = Ramp(2000, 1);
    ALuint loaded;
    SndAr_alGenBuffers(1, &loaded);
    SndAr_alBufferData(loaded, AL_FORMAT_MONO16, pcm.data(), static_cast<ALsizei>(pcm.size() * 2), 44100);
    std::thread streamer([&] {
        ALuint bufs[4];
        SndAr_alGenBuffers(4, bufs);
        int freeCount = 4;
        ALuint freeBufs[4] = {bufs[0], bufs[1], bufs[2], bufs[3]};
        for (int iter = 0; iter < 3000; ++iter)
        {
            ALint processed = 0;
            SndAr_alGetSourcei(sources[3], AL_BUFFERS_PROCESSED, &processed);
            while (processed-- > 0 && freeCount < 4)
                SndAr_alSourceUnqueueBuffers(sources[3], 1, &freeBufs[freeCount++]);
            while (freeCount > 0)
            {
                const ALuint b = freeBufs[--freeCount];
                SndAr_alBufferData(b, AL_FORMAT_MONO16, pcm.data(), 512, 44100);
                SndAr_alSourceQueueBuffers(sources[3], 1, &b);
            }
            ALint state = 0;
            SndAr_alGetSourcei(sources[3], AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING)
                SndAr_alSourcePlay(sources[3]);
        }
        SndAr_alSourceStop(sources[3]);
        SndAr_alSourcei(sources[3], AL_BUFFER, 0);
        SndAr_alDeleteBuffers(4, bufs);
    });
    for (int iter = 0; iter < 3000; ++iter)
    {
        const ALuint s = sources[iter % 3];
        SndAr_alSourceStop(s);
        SndAr_alSourcei(s, AL_BUFFER, static_cast<ALint>(loaded));
        SndAr_alSource3f(s, AL_POSITION, static_cast<float>(iter % 7) - 3.0f, 0.0f, -1.0f);
        SndAr_alSourcef(s, AL_GAIN, 0.5f);
        SndAr_alFilterf(sendFilter, AL_LOWPASS_GAIN, static_cast<float>(iter % 5) * 0.25f);
        SndAr_alSource3i(s, AL_AUXILIARY_SEND_FILTER, static_cast<ALint>(slot), 0, static_cast<ALint>(sendFilter));
        SndAr_alSourcePlay(s);
        ALint state;
        SndAr_alGetSourcei(s, AL_SOURCE_STATE, &state);
        if (iter % 50 == 0) // room changes while the update thread runs
        {
            SndAr_alEffectf(effect, AL_EAXREVERB_DECAY_TIME, 0.5f + static_cast<float>(iter % 7));
            SndAr_alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, iter % 100 ? static_cast<ALint>(effect) : 0);
        }
    }
    streamer.join();
    quit.store(true);
    updater.join();
    for (int i = 0; i < 3; ++i)
    {
        SndAr_alSourceStop(sources[i]);
        SndAr_alSourcei(sources[i], AL_BUFFER, 0);
    }
    SndAr_alDeleteBuffers(1, &loaded);
    SndAr_alDeleteSources(4, sources);
    SndAr_alDeleteFilters(1, &sendFilter);
    SndAr_alDeleteAuxiliaryEffectSlots(1, &slot);
    SndAr_alDeleteEffects(1, &effect);
    SndArStats stats;
    SndAr_GetStats(&stats);
    CHECK(stats.plays > 3000 && stats.playFails == 0 && stats.alErrors == 0);
    CHECK(stats.reverbSends == 3000 && stats.reverbSendsSkipped == 0 && stats.reverbPresets == 30);
    std::printf("SND_AUDREN_CONCURRENCY plays=%u lock_waits=%u\n", stats.plays, stats.lockWaits);
    SndAr_Detach();
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--concurrency") == 0)
    {
        TestConcurrency();
        std::printf(g_failures ? "FAIL:SND_AUDREN_CONCURRENCY\n" : "PASS:SND_AUDREN_CONCURRENCY\n");
        return g_failures ? 1 : 0;
    }
    TestPanAgainstHostOpenAl();
    TestReverbConversion();
    TestReverbSplice();

    FakeDevice dev;
    SndAr_Attach(&dev, CapturePrint);
    TestLoadedSounds(dev);
    TestStreaming(dev);
    TestReverbSends(dev);
    SndAr_Detach();

    // The model's FAIL lines above were provoked on purpose and counted by the
    // checks; only their number is echoed so a log scan for "FAIL:" stays clean.
    std::printf("SND_AUDREN_MODEL provoked_fail_lines=%zu\n", g_lines.size());
    if (g_failures)
    {
        std::printf("FAIL:SND_AUDREN_BACKEND failures=%d\n", g_failures);
        return 1;
    }
    std::printf("PASS:SND_AUDREN_BACKEND\n");
    return 0;
}
