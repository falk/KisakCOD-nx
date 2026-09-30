#pragma once
// audren sound backend: the OpenAL source/buffer subset the sound driver
// uses, implemented over libnx's audio renderer (audrv over audren).
//
// Why an OpenAL-shaped surface: snd_driver_openal.cpp, snd_openal.cpp and
// snd_stream_openal.cpp hold all of the engine-facing channel bookkeeping
// (2D/3D/stream channels, volume groups, slave lerp, pitch, loops, start
// offsets, pause, save info, streamed music/dialogue, cinematic audio) and
// talk to the mixer only through ~25 AL calls.  On Switch those calls are
// routed here unconditionally (snd_al_dispatch.h), so the engine-facing
// files stay exactly the same code non-Switch builds run against
// openal-soft directly; only who mixes differs -- the audio DSP here
// (per-voice resampling, volume and mix), with this file only keeping
// bookkeeping on the CPU.
//
// Model: one AL source = one audren voice (same index); one AL buffer = PCM16
// in DSP-visible pool memory (8-bit data is widened on upload); a static
// source plays its buffer as one or two wave buffers (a looping sound started
// mid-file needs [offset, end) once, then [0, end) looping); a streaming
// source appends one wave buffer per queued AL buffer.  State, processed
// counts and offsets are derived from the renderer's wave-buffer progress
// exactly as OpenAL defines them (a drained streaming source is STOPPED, a
// stopped source has every queued buffer processed, play restarts the queue).
// Gains follow snd_audren_pan.h.
//
// EFX reverb (the one aux effect slot CoD4 uses, snd_audren_reverb.h) runs
// on the DSP: every voice mixes into a 4-buffer submix whose buffers 0/1 are
// the dry stereo pair and 2/3 the reverb send bus; an I3DL2 reverb effect on
// that submix turns the send bus into wet reverb in place, and the submix
// sums both pairs into the final mix.  A source's AL_AUXILIARY_SEND_FILTER
// (slot + lowpass filter used as a gain carrier, SND_ApplyReverbSend) sets
// its send-bus mix factors; the slot's EAX-reverb effect becomes the DSP
// effect's I3DL2 parameters.  EFX features with no DSP counterpart (direct
// filters, per-send HF damping) fail loudly and are counted.
//
// The DSP side is behind SndArDevice so the whole model runs on the host
// (switch_snd_audren_test.cpp) against a fake renderer.

#include <cstddef>
#include <cstdint>

#include <AL/al.h>
#ifndef AL_ALEXT_PROTOTYPES
#define AL_ALEXT_PROTOTYPES // as snd_local.h: EFX is linked directly
#endif
#include <AL/efx.h>

#include "snd_audren_reverb.h"

// One wave buffer submitted to the renderer.  The device keeps its own
// per-wave state in `device` (libnx's AudioDriverWaveBuf on the Switch).
struct SndArWave
{
    const int16_t *data;   // start of the buffer (frame 0)
    uint64_t bytes;        // whole buffer
    int32_t start;         // first frame to play
    int32_t end;           // one past the last frame
    bool loop;
    alignas(8) unsigned char device[64];
};

// The renderer as the model needs it.  Every call is made with the model's
// lock held; Update() is the only one that talks to the DSP.
class SndArDevice
{
public:
    virtual ~SndArDevice() {}
    virtual bool VoiceInit(int voice, int channels, int sampleRate) = 0; // stopped, mix all 0
    virtual void VoiceDrop(int voice) = 0;
    virtual void VoiceStop(int voice) = 0;           // every wave -> done
    virtual void VoiceSetPaused(int voice, bool paused) = 0;
    virtual bool VoiceAddWave(int voice, SndArWave *wave) = 0;
    virtual bool VoiceIsPlaying(int voice) = 0;      // started and a wave left
    virtual uint32_t VoicePlayedSamples(int voice) = 0;
    virtual void VoiceSetVolume(int voice, float volume) = 0;
    virtual void VoiceSetPitch(int voice, float pitch) = 0;
    // outChannel 0/1: dry left/right; kSndArSendBus + 0/1: reverb send bus.
    virtual void VoiceSetMix(int voice, int srcChannel, int outChannel, float gain) = 0;
    // The DSP reverb on the send bus: parameters (applied from the next
    // update, reverb state kept) or nullptr = bypassed.  False when the
    // renderer has no reverb effect.
    virtual bool SetReverb(const SndArI3dl2 *params) = 0;
    virtual bool WaveDone(const SndArWave *wave) = 0;
    virtual void WaveReset(SndArWave *wave) = 0;     // back to free before a re-submit
    // 0x1000-aligned, DSP-visible memory of `bytes` (a multiple of 0x1000),
    // never returned; nullptr when the pool budget is spent.
    virtual void *AllocPoolChunk(size_t bytes) = 0;
    virtual void FlushData(const void *data, size_t bytes) = 0;
    // Renderer updates completed so far (memory freed by the model is reused
    // only after two more, when no DSP frame can still read it).
    virtual uint64_t UpdateCount() = 0;
    // One renderer update (audrvUpdate): sends parameter changes, collects
    // wave-buffer progress.  False when the renderer refused it.
    virtual bool Update() = 0;
};

constexpr int kSndArMaxSources = 64;
constexpr int kSndArQueueDepth = 16;
constexpr int kSndArMaxBuffers = 8192;
constexpr size_t kSndArChunkBytes = 16u << 20;
constexpr size_t kSndArWaveAlign = 0x40;
constexpr int kSndArSendBus = 2;      // first send-bus channel of the voice mix
constexpr int kSndArMaxFilters = 256;
constexpr int kSndArMaxEffects = 8;
constexpr int kSndArMaxSlots = 4;

struct SndArStats
{
    uint32_t plays;          // alSourcePlay that started a voice
    uint32_t playFails;      // plays that could not get a voice/wave (FAIL lines)
    uint32_t oomFails;       // buffer uploads refused for pool memory
    uint32_t pitchClamps;
    uint32_t reverbSends;    // AL_AUXILIARY_SEND_FILTER sends applied to the DSP send bus
    uint32_t reverbSendsSkipped; // sends that could not be applied (FAIL lines)
    uint32_t reverbPresets;  // reverb parameter sets handed to the DSP (room changes)
    uint32_t efxSkipped;     // EFX features with no DSP counterpart (FAIL lines)
    bool reverbOn;           // the DSP reverb is enabled
    uint32_t wetVoices;      // playing voices with a non-zero send now
    SndArI3dl2 reverb;       // last parameters handed to the DSP
    uint32_t alErrors;
    uint32_t activeVoices;   // voices playing or paused now
    uint32_t peakVoices;
    uint32_t buffers;        // live AL buffers
    uint64_t bufferBytes;    // PCM bytes in those buffers
    uint64_t poolBytes;      // pool memory reserved from the device
    uint32_t lockWaits;      // AL calls that waited for the update thread
    uint64_t lockWaitUs;     // total wait (cumulative)
    uint32_t lockWaitUsMax;  // longest wait since the last SndAr_GetStats
};

// Lifecycle (engine thread).
void SndAr_Attach(SndArDevice *device, void (*print)(const char *line));
void SndAr_Detach(); // after every source and buffer is deleted
bool SndAr_Attached();
// Renderer update + deferred frees; the device's update thread calls this.
void SndAr_Update();
void SndAr_GetStats(SndArStats *out);

// The OpenAL subset (same contracts as the AL entry points of the same name).
void SndAr_alGenSources(ALsizei n, ALuint *sources);
void SndAr_alDeleteSources(ALsizei n, const ALuint *sources);
void SndAr_alGenBuffers(ALsizei n, ALuint *buffers);
void SndAr_alDeleteBuffers(ALsizei n, const ALuint *buffers);
void SndAr_alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq);
void SndAr_alSourcei(ALuint source, ALenum param, ALint value);
void SndAr_alSource3i(ALuint source, ALenum param, ALint v1, ALint v2, ALint v3);
void SndAr_alSourcef(ALuint source, ALenum param, ALfloat value);
void SndAr_alSource3f(ALuint source, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3);
void SndAr_alGetSourcei(ALuint source, ALenum param, ALint *value);
void SndAr_alGetSourcef(ALuint source, ALenum param, ALfloat *value);
void SndAr_alGetSource3f(ALuint source, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3);
void SndAr_alSourcePlay(ALuint source);
void SndAr_alSourcePause(ALuint source);
void SndAr_alSourceStop(ALuint source);
void SndAr_alSourceQueueBuffers(ALuint source, ALsizei n, const ALuint *buffers);
void SndAr_alSourceUnqueueBuffers(ALuint source, ALsizei n, ALuint *buffers);
ALenum SndAr_alGetError();
// EFX subset: lowpass filters as send-gain carriers, EAX-reverb effects,
// auxiliary effect slots (one may hold a reverb: the DSP's).
void SndAr_alGenFilters(ALsizei n, ALuint *filters);
void SndAr_alDeleteFilters(ALsizei n, const ALuint *filters);
void SndAr_alFilteri(ALuint filter, ALenum param, ALint value);
void SndAr_alFilterf(ALuint filter, ALenum param, ALfloat value);
void SndAr_alGenEffects(ALsizei n, ALuint *effects);
void SndAr_alDeleteEffects(ALsizei n, const ALuint *effects);
void SndAr_alEffecti(ALuint effect, ALenum param, ALint value);
void SndAr_alEffectf(ALuint effect, ALenum param, ALfloat value);
void SndAr_alEffectfv(ALuint effect, ALenum param, const ALfloat *values);
void SndAr_alGenAuxiliaryEffectSlots(ALsizei n, ALuint *slots);
void SndAr_alDeleteAuxiliaryEffectSlots(ALsizei n, const ALuint *slots);
void SndAr_alAuxiliaryEffectSloti(ALuint slot, ALenum param, ALint value);

// Voice index of a source (tests / debug); -1 when not a live source.
int SndAr_SourceVoice(ALuint source);
