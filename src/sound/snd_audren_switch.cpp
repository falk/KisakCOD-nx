// Switch audio renderer device for the audren sound backend
// (snd_audren_al.h): libnx audrv over audren, one voice per AL source,
// stereo 48 kHz final mix, and an update thread that runs audrvUpdate once
// per renderer frame (5 ms).
//
// Mix graph (all on the DSP), one 4-buffer final mix:
//   voices --> buffers 0/1 (dry L/R) and 2/3 (reverb send L/R)
//   effect 0: I3DL2 reverb in place on 2/3 (dry gain 0: 2/3 become wet)
//   effect 1: buffer mix 2 -> 0, 3 -> 1 at unit gain
//   sink plays 0/1.
// (A submix with a 4x2 matrix into a stereo final mix would avoid the
// buffer-mix effect, but libnx's audrvCreate refuses num_mix_buffers above
// the final mix's channel count -- rc 0x1759 on some emulators -- so it cannot
// reserve buffers for a submix.)
// libnx's audrv has no effect support: its update sends no effect section.
// The renderer is created with one effect, and the link wraps
// audrenRequestUpdateAudioRenderer (-Wl,--wrap, scripts/sp/CMakeLists.txt)
// so every update audrv makes gets the effect record spliced in after the
// voices and the effect status cut back out of the output
// (SndAr_SpliceEffects / SndAr_UnspliceEffects, snd_audren_reverb.h).
#if defined(__SWITCH__) && defined(KISAK_OPENAL)

#include <switch.h>

#include "snd_audren_al.h"
#include <port/switch_pcsample.h>

#include <atomic>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <vector>

#include <platform/switch/switch_port_log.h>

// Declared in snd_al_dispatch.h; repeated here to keep this file free of the
// engine headers.
bool SND_AudrenOpen();
void SND_AudrenClose();
void SND_AudrenEmitPerf(uint64_t *busyUs, uint32_t *updates);
void SND_AudrenEvidence(char *out, size_t size);

// snd_audrenUpdateFrames (latched): renderer frames (5 ms each) per
// audrvUpdate.  Set by SND_InitDriver before SND_AudrenOpen.
int g_sndAudrenFramesPerUpdate = 1;

namespace
{
// Voices: one per AL source (53 engine channels + the cinematic source).
// audren gives every voice channel its own slot out of num_voices, so a
// stereo voice takes two; 128 covers 54 stereo voices.  Mempools come to
// 4 * num_voices = 512; the arena uses one per 16 MB chunk.
constexpr AudioRendererConfig kConfig = {
    AudioRendererOutputRate_48kHz, // output_rate
    128,                           // num_voices
    2,                             // num_effects (I3DL2 reverb + wet fold)
    1,                             // num_sinks
    1,                             // num_mix_objs (the final mix)
    4,                             // num_mix_buffers (dry stereo + send stereo)
};
constexpr int kMixBuffers = 4;
constexpr int kEffects = 2;
// I3DL2 reverb work memory (delay lines).  The renderer's delay lines at
// 48 kHz need ~1.05 s of float samples (Ryujinx Reverb3dState: FDN 45.7 +
// 82.8 + 149.9 + 271.6 ms, 2 x 4 decay allpasses 46 ms, pre-delay 400 ms,
// centre 5 ms) = ~200 KB; 1 MB leaves the DSP ample room.
constexpr size_t kReverbWorkBytes = 1u << 20;
// Loaded sounds are copied into pool memory on first play, as openal-soft
// copies them into its buffers; the budget only stops a runaway.
constexpr size_t kPoolBudget = 384u << 20;

static_assert(sizeof(AudioDriverWaveBuf) <= sizeof(SndArWave::device), "SndArWave::device too small");
static_assert(sizeof(AudioRendererUpdateDataHeader) == sizeof(SndArUpdateHeader), "update header");
static_assert(offsetof(AudioRendererUpdateDataHeader, effects_sz) == offsetof(SndArUpdateHeader, effectsSz), "effects_sz");
static_assert(offsetof(AudioRendererUpdateDataHeader, total_sz) == offsetof(SndArUpdateHeader, totalSz), "total_sz");
static_assert(sizeof(AudioRendererBehaviorInfoOut) == 0xB0, "BehaviourErrorInfoOutStatus is 0xB0 bytes");

// The effect section the update wrapper splices in; guarded by the model's
// lock like every other driver access (all updates run under it).
struct EffectSection
{
    bool active = false;
    SndArEffectInWire in[kEffects]{};   // [0] reverb, [1] wet fold
    SndArEffectOutWire out[kEffects]{};
    void *spliceIn = nullptr;
    size_t spliceInCap = 0;
    void *spliceOut = nullptr;
    size_t spliceOutCap = 0;
    uint64_t rendererErrors = 0;
    uint32_t lastErrorCode = 0;
    uint32_t spliceFails = 0;
};
EffectSection s_fx;

void Emit(const char *line)
{
    Port_Log(line);
}

class AudrvDevice final : public SndArDevice
{
public:
    AudioDriver drv{};
    void *reverbWork = nullptr;
    uint64_t updates = 0;
    uint32_t updateFails = 0;
    size_t poolBytes = 0;
    uint32_t poolCount = 0;
    std::vector<void *> chunks;
    // The driver's voice drop counter restarts with every voice init/stop;
    // Update() folds each voice's increments into one cumulative total.
    uint32_t lastDrops[128] = {};
    std::atomic<uint64_t> totalDrops{0};

    bool Used(int voice) const { return drv.in_voices[voice].is_used; }

    bool VoiceInit(int voice, int channels, int sampleRate) override
    {
        lastDrops[voice] = 0;
        if (!audrvVoiceInit(&drv, voice, channels, PcmFormat_Int16, sampleRate))
            return false;
        audrvVoiceSetDestinationMix(&drv, voice, AUDREN_FINAL_MIX_ID);
        return true;
    }
    void VoiceDrop(int voice) override
    {
        lastDrops[voice] = 0;
        if (Used(voice))
            audrvVoiceDrop(&drv, voice);
    }
    void VoiceStop(int voice) override
    {
        lastDrops[voice] = 0;
        if (Used(voice))
            audrvVoiceStop(&drv, voice);
    }
    void VoiceSetPaused(int voice, bool paused) override
    {
        if (Used(voice))
            audrvVoiceSetPaused(&drv, voice, paused);
    }
    bool VoiceAddWave(int voice, SndArWave *wave) override
    {
        AudioDriverWaveBuf *buf = reinterpret_cast<AudioDriverWaveBuf *>(wave->device);
        buf->data_raw = wave->data;
        buf->size = wave->bytes;
        buf->start_sample_offset = wave->start;
        buf->end_sample_offset = wave->end;
        buf->is_looping = wave->loop;
        buf->context_addr = nullptr;
        buf->context_sz = 0;
        return Used(voice) && audrvVoiceAddWaveBuf(&drv, voice, buf);
    }
    bool VoiceIsPlaying(int voice) override { return Used(voice) && audrvVoiceIsPlaying(&drv, voice); }
    uint32_t VoicePlayedSamples(int voice) override
    {
        return Used(voice) ? audrvVoiceGetPlayedSampleCount(&drv, voice) : 0;
    }
    void VoiceSetVolume(int voice, float volume) override
    {
        if (Used(voice))
            audrvVoiceSetVolume(&drv, voice, volume);
    }
    void VoiceSetPitch(int voice, float pitch) override
    {
        if (Used(voice))
            audrvVoiceSetPitch(&drv, voice, pitch);
    }
    void VoiceSetMix(int voice, int srcChannel, int outChannel, float gain) override
    {
        if (Used(voice) && srcChannel < (int)drv.in_voices[voice].channel_count && outChannel < kMixBuffers)
            audrvVoiceSetMixFactor(&drv, voice, gain, srcChannel, outChannel);
    }
    bool SetReverb(const SndArI3dl2 *params) override
    {
        if (!s_fx.active)
            return false;
        if (!params)
        {
            s_fx.in[0].isEnabled = false;
            return true;
        }
        // Disabled -> enabled goes out as a new effect: the renderer builds
        // the reverb state only for a new effect's first enabled frame (its
        // parameter status is Enabled after any frame, enabled or not, and
        // status New only re-derives coefficients of an existing state --
        // Ryujinx Reverb3dEffect.Update / Reverb3dCommand.Process).  A room
        // change while enabled keeps the state, so the tail rings on.
        if (!s_fx.in[0].isEnabled)
            s_fx.in[0].isNew = true;
        SndAr_EncodeReverb3d(*params, kSndArSendBus, 48000, kSndArUsageNew, &s_fx.in[0]);
        s_fx.in[0].isEnabled = true;
        return true;
    }
    bool WaveDone(const SndArWave *wave) override
    {
        return reinterpret_cast<const AudioDriverWaveBuf *>(wave->device)->state == AudioDriverWaveBufState_Done;
    }
    void WaveReset(SndArWave *wave) override
    {
        std::memset(wave->device, 0, sizeof(wave->device)); // AudioDriverWaveBufState_Free
    }
    void *AllocPoolChunk(size_t bytes) override
    {
        if (poolBytes + bytes > kPoolBudget)
            return nullptr;
        void *mem = memalign(AUDREN_MEMPOOL_ALIGNMENT, bytes);
        if (!mem)
            return nullptr;
        std::memset(mem, 0, bytes);
        armDCacheFlush(mem, bytes);
        const int id = audrvMemPoolAdd(&drv, mem, bytes);
        if (id < 0 || !audrvMemPoolAttach(&drv, id))
        {
            if (id >= 0)
                audrvMemPoolRemove(&drv, id);
            free(mem);
            return nullptr;
        }
        // Attach now, not with the next frame's update: a sound queued from
        // this memory in the same breath must find the pool mapped.
        Update();
        poolBytes += bytes;
        ++poolCount;
        chunks.push_back(mem);
        return mem;
    }
    void FlushData(const void *data, size_t bytes) override { armDCacheFlush(const_cast<void *>(data), bytes); }
    uint64_t UpdateCount() override { return updates; }
    bool Update() override
    {
        const Result rc = audrvUpdate(&drv);
        ++updates;
        if (R_FAILED(rc))
        {
            if (++updateFails <= 8)
            {
                char line[96];
                snprintf(line, sizeof(line), "FAIL: SND_AUDREN_UPDATE rc=0x%x\n", (unsigned)rc);
                Emit(line);
            }
            return false;
        }
        uint64_t added = 0;
        for (int v = 0; v < kConfig.num_voices; ++v)
        {
            if (!Used(v))
                continue;
            const uint32_t now = audrvVoiceGetVoiceDropsCount(&drv, v);
            added += now >= lastDrops[v] ? now - lastDrops[v] : now;
            lastDrops[v] = now;
        }
        if (added)
            totalDrops.fetch_add(added, std::memory_order_relaxed);
        return true;
    }
};

AudrvDevice *s_device;
bool s_audrenInit;
Thread s_thread;
bool s_threadStarted;
std::atomic<bool> s_quit{false};
std::atomic<uint64_t> s_busyTicks{0};
std::atomic<uint32_t> s_updateCalls{0};
std::atomic<uint32_t> s_maxUpdateUs{0};

void UpdateThread(void *)
{
    SwitchPcSample_RegisterCurrentThread(kSwitchPcSampleTagSndMix);
    const int every = g_sndAudrenFramesPerUpdate < 1 ? 1 : (g_sndAudrenFramesPerUpdate > 4 ? 4 : g_sndAudrenFramesPerUpdate);
    int frame = 0;
    while (!s_quit.load(std::memory_order_acquire))
    {
        audrenWaitFrame();
        if (s_quit.load(std::memory_order_acquire))
            break;
        if (++frame < every)
            continue;
        frame = 0;
        const u64 t0 = armGetSystemTick();
        SndAr_Update();
        const u64 spent = armGetSystemTick() - t0;
        s_busyTicks.fetch_add(spent, std::memory_order_relaxed);
        s_updateCalls.fetch_add(1, std::memory_order_relaxed);
        const uint32_t us = static_cast<uint32_t>(armTicksToNs(spent) / 1000);
        if (us > s_maxUpdateUs.load(std::memory_order_relaxed))
            s_maxUpdateUs.store(us, std::memory_order_relaxed);
    }
}

void FailOpen(const char *what, Result rc)
{
    char line[128];
    snprintf(line, sizeof(line), "FAIL: SND_AUDREN_OPEN %s rc=0x%x\n", what, (unsigned)rc);
    Emit(line);
}

// The reverb work pool and the two effect records (reverb bypassed until a
// room is set, wet fold always on); sent with the first update.
bool SetupReverbGraph(AudrvDevice *dev)
{
    AudioDriver *d = &dev->drv;
    void *work = memalign(AUDREN_MEMPOOL_ALIGNMENT, kReverbWorkBytes);
    if (!work)
    {
        FailOpen("reverb work memory", 0);
        return false;
    }
    std::memset(work, 0, kReverbWorkBytes);
    armDCacheFlush(work, kReverbWorkBytes);
    const int pool = audrvMemPoolAdd(d, work, kReverbWorkBytes);
    if (pool < 0 || !audrvMemPoolAttach(d, pool))
    {
        free(work);
        FailOpen("reverb work pool", 0);
        return false;
    }
    dev->reverbWork = work;

    s_fx = EffectSection{};
    s_fx.spliceInCap = (audrenGetInputParamSize(&kConfig) + sizeof(s_fx.in) + 0xFFF) & ~size_t(0xFFF);
    s_fx.spliceOutCap = audrenGetOutputParamSize(&kConfig) + sizeof(s_fx.out);
    s_fx.spliceIn = memalign(AUDREN_INPUT_PARAM_ALIGNMENT, s_fx.spliceInCap);
    s_fx.spliceOut = memalign(AUDREN_OUTPUT_PARAM_ALIGNMENT, s_fx.spliceOutCap);
    if (!s_fx.spliceIn || !s_fx.spliceOut)
    {
        FailOpen("effect splice buffers", 0);
        return false;
    }
    SndArEffectInWire &reverb = s_fx.in[0];
    reverb.type = kSndArEffectTypeReverb3d;
    reverb.isNew = true;
    reverb.isEnabled = false;
    reverb.mixId = AUDREN_FINAL_MIX_ID;
    reverb.bufferBase = reinterpret_cast<uintptr_t>(work);
    reverb.bufferSize = kReverbWorkBytes;
    reverb.processingOrder = 0;
    SndAr_EncodeReverb3d(SndAr_EfxToI3dl2(SndArEfxReverb{}), kSndArSendBus, 48000, kSndArUsageNew, &reverb);
    SndArEffectInWire &fold = s_fx.in[1];
    fold.type = kSndArEffectTypeBufferMix;
    fold.isNew = true;
    fold.isEnabled = true;
    fold.mixId = AUDREN_FINAL_MIX_ID;
    fold.processingOrder = 1; // after the reverb
    SndAr_EncodeBusFold(kSndArSendBus, &fold);
    s_fx.active = true;
    return true;
}
} // namespace

// libnx audrv's only path to the renderer (-Wl,--wrap=audrenRequestUpdateAudioRenderer).
extern "C" Result __real_audrenRequestUpdateAudioRenderer(const void *in_param_buf, size_t in_param_buf_size,
                                                          void *out_param_buf, size_t out_param_buf_size,
                                                          void *perf_buf, size_t perf_buf_size);
extern "C" Result __wrap_audrenRequestUpdateAudioRenderer(const void *in_param_buf, size_t in_param_buf_size,
                                                          void *out_param_buf, size_t out_param_buf_size,
                                                          void *perf_buf, size_t perf_buf_size)
{
    if (!s_fx.active)
        return __real_audrenRequestUpdateAudioRenderer(in_param_buf, in_param_buf_size, out_param_buf,
                                                       out_param_buf_size, perf_buf, perf_buf_size);
    const size_t inSize = SndAr_SpliceEffects(in_param_buf, in_param_buf_size, s_fx.in, sizeof(s_fx.in),
                                              s_fx.spliceIn, s_fx.spliceInCap);
    const size_t outSize = out_param_buf_size + sizeof(s_fx.out);
    if (!inSize || outSize > s_fx.spliceOutCap)
    {
        if (++s_fx.spliceFails <= 4)
            Emit("FAIL: SND_AUDREN_REVERB splice (update input not in audrv's layout)\n");
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    const Result rc = __real_audrenRequestUpdateAudioRenderer(s_fx.spliceIn, inSize, s_fx.spliceOut, outSize,
                                                              perf_buf, perf_buf_size);
    if (R_FAILED(rc))
        return rc;
    uint32_t code = 0;
    const int errors = SndAr_UnspliceEffects(s_fx.spliceOut, outSize, out_param_buf, out_param_buf_size, s_fx.out,
                                             kEffects, &code);
    if (errors < 0)
    {
        if (++s_fx.spliceFails <= 4)
            Emit("FAIL: SND_AUDREN_REVERB unsplice (renderer output without the effect section)\n");
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    if (errors > 0)
    {
        if (s_fx.rendererErrors < 8)
        {
            char line[128];
            snprintf(line, sizeof(line), "FAIL: SND_AUDREN_RENDERER_ERROR count=%d code=0x%x\n", errors,
                     (unsigned)code);
            Emit(line);
        }
        s_fx.rendererErrors += (uint64_t)errors;
        s_fx.lastErrorCode = code;
    }
    // Sent once: the DSP (re)initialises the effect on IsNew and re-derives
    // its coefficients on parameter status New; afterwards they stand.
    for (SndArEffectInWire &fx : s_fx.in)
        fx.isNew = false;
    reinterpret_cast<SndArReverb3dWire *>(s_fx.in[0].specific)->parameterStatus = kSndArUsageEnabled;
    return rc;
}


bool SND_AudrenOpen()
{
    if (s_device)
        return true;
    Result rc = audrenInitialize(&kConfig);
    if (R_FAILED(rc))
    {
        FailOpen("audrenInitialize", rc);
        return false;
    }
    s_audrenInit = true;
    s_device = new AudrvDevice();
    rc = audrvCreate(&s_device->drv, &kConfig, kMixBuffers);
    if (R_FAILED(rc))
    {
        FailOpen("audrvCreate", rc);
        delete s_device;
        s_device = nullptr;
        audrenExit();
        s_audrenInit = false;
        return false;
    }
    if (!SetupReverbGraph(s_device))
    {
        SND_AudrenClose();
        return false;
    }
    static const u8 kSinkChannels[] = {0, 1};
    if (audrvDeviceSinkAdd(&s_device->drv, AUDREN_DEFAULT_DEVICE_NAME, 2, kSinkChannels) < 0)
    {
        FailOpen("audrvDeviceSinkAdd", 0);
        SND_AudrenClose();
        return false;
    }
    rc = audrvUpdate(&s_device->drv);
    if (R_SUCCEEDED(rc))
        rc = audrenStartAudioRenderer();
    if (R_FAILED(rc))
    {
        FailOpen("audrenStartAudioRenderer", rc);
        SND_AudrenClose();
        return false;
    }
    SndAr_Attach(s_device, Emit);
    s_quit.store(false, std::memory_order_release);
    // Core 2 at 0x20, above the stream thread (0x2A): the renderer's wave
    // buffers only need a top-up every frame, but a late update delays starts.
    rc = threadCreate(&s_thread, UpdateThread, nullptr, nullptr, 0x8000, 0x20, 2);
    if (R_SUCCEEDED(rc))
        rc = threadStart(&s_thread);
    if (R_FAILED(rc))
    {
        FailOpen("update thread", rc);
        SND_AudrenClose();
        return false;
    }
    s_threadStarted = true;
    char line[160];
    snprintf(line, sizeof(line),
             "SND_AUDREN_OPEN voices=%d mix_buffers=%d rate=48000 pool_budget_mb=%u revision=0x%x effects=%d "
             "reverb=i3dl2 mix_buffers_send=%d reverb_work_kb=%u\n",
             kConfig.num_voices, kConfig.num_mix_buffers, (unsigned)(kPoolBudget >> 20), (unsigned)audrenGetRevision(),
             kConfig.num_effects, kSndArSendBus, (unsigned)(kReverbWorkBytes >> 10));
    Emit(line);
    return true;
}

void SND_AudrenClose()
{
    if (s_threadStarted)
    {
        s_quit.store(true, std::memory_order_release);
        threadWaitForExit(&s_thread); // wakes on the next renderer frame
        threadClose(&s_thread);
        s_threadStarted = false;
    }
    if (SndAr_Attached())
        SndAr_Detach();
    if (s_device)
    {
        if (s_audrenInit)
            audrenStopAudioRenderer();
        audrvClose(&s_device->drv);
        for (void *chunk : s_device->chunks)
            free(chunk);
        free(s_device->reverbWork);
        free(s_fx.spliceIn);
        free(s_fx.spliceOut);
        s_fx = EffectSection{};
        delete s_device;
        s_device = nullptr;
    }
    if (s_audrenInit)
    {
        audrenExit();
        s_audrenInit = false;
    }
}

void SND_AudrenEmitPerf(uint64_t *busyUs, uint32_t *updates)
{
    *busyUs = armTicksToNs(s_busyTicks.load(std::memory_order_relaxed)) / 1000;
    *updates = s_updateCalls.load(std::memory_order_relaxed);
}

void SND_AudrenEvidence(char *out, size_t size)
{
    SndArStats stats{};
    SndAr_GetStats(&stats);
    const unsigned long long drops = s_device ? s_device->totalDrops.load(std::memory_order_relaxed) : 0;
    // The effect status the renderer reported last (3 enabled, 4 disabled).
    const unsigned fxState = s_fx.out[0].state;
    snprintf(out, size,
             "voices=%u peak_voices=%u plays=%u play_fails=%u dsp_drops=%llu buffers=%u buffer_kb=%llu pool_mb=%llu "
             "oom=%u pitch_clamps=%u reverb=%s reverb_dsp_state=%u reverb_presets=%u reverb_sends=%u "
             "reverb_sends_skipped=%u reverb_wet_voices=%u reverb_room_mb=%.0f reverb_decay_s=%.2f "
             "renderer_errors=%llu renderer_error_code=0x%x efx_skipped=%u al_errors=%u max_update_us=%u "
             "frames_per_update=%d lock_waits=%u lock_wait_ms=%llu lock_wait_max_us=%u",
             stats.activeVoices, stats.peakVoices, stats.plays, stats.playFails, drops, stats.buffers,
             (unsigned long long)(stats.bufferBytes >> 10), (unsigned long long)(stats.poolBytes >> 20),
             stats.oomFails, stats.pitchClamps, stats.reverbOn ? "on" : "off", fxState, stats.reverbPresets,
             stats.reverbSends, stats.reverbSendsSkipped, stats.wetVoices, (double)stats.reverb.roomGain,
             (double)stats.reverb.decayTime, (unsigned long long)s_fx.rendererErrors, (unsigned)s_fx.lastErrorCode,
             stats.efxSkipped, stats.alErrors, s_maxUpdateUs.exchange(0, std::memory_order_relaxed),
             g_sndAudrenFramesPerUpdate, stats.lockWaits, (unsigned long long)(stats.lockWaitUs / 1000),
             stats.lockWaitUsMax);
}

#endif
