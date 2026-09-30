#pragma once
// The Switch sound backend (`src/sound/snd_audren_al.cpp` etc.): an OpenAL
// subset implemented on libnx's audio renderer (DSP mixing + I3DL2 reverb).
// Every OpenAL call the sound driver makes (snd_openal.cpp,
// snd_driver_openal.cpp, snd_stream_openal.cpp) is routed to it unconditionally
// on Switch, so those three files stay backend-agnostic engine code (channel
// bookkeeping, volume groups, slave lerp, pitch, loops, streaming) with the
// mixer swapped out from under them. Include after the AL headers, only in
// those three files. alc* calls are not routed: MSS_Init/MSS_Startup/
// MSS_ShutdownCleanup branch on `__SWITCH__` explicitly, since only the PC
// build ever opens a real ALC device/context.
//
// Non-Switch builds (upstream KisakCOD) never include this in a state where
// it does anything: the macros below only exist inside the `__SWITCH__`
// branch, so PC's al* calls reach openal-soft directly, unmodified.

#if defined(__SWITCH__) && !defined(KISAK_SND_STREAM_HOST_TEST)
#include "snd_audren_al.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

#define alGenSources(...) SndAr_alGenSources(__VA_ARGS__)
#define alDeleteSources(...) SndAr_alDeleteSources(__VA_ARGS__)
#define alGenBuffers(...) SndAr_alGenBuffers(__VA_ARGS__)
#define alDeleteBuffers(...) SndAr_alDeleteBuffers(__VA_ARGS__)
#define alBufferData(...) SndAr_alBufferData(__VA_ARGS__)
#define alSourcei(...) SndAr_alSourcei(__VA_ARGS__)
#define alSource3i(...) SndAr_alSource3i(__VA_ARGS__)
#define alSourcef(...) SndAr_alSourcef(__VA_ARGS__)
#define alSource3f(...) SndAr_alSource3f(__VA_ARGS__)
#define alGetSourcei(...) SndAr_alGetSourcei(__VA_ARGS__)
#define alGetSourcef(...) SndAr_alGetSourcef(__VA_ARGS__)
#define alGetSource3f(...) SndAr_alGetSource3f(__VA_ARGS__)
#define alSourcePlay(...) SndAr_alSourcePlay(__VA_ARGS__)
#define alSourcePause(...) SndAr_alSourcePause(__VA_ARGS__)
#define alSourceStop(...) SndAr_alSourceStop(__VA_ARGS__)
#define alSourceQueueBuffers(...) SndAr_alSourceQueueBuffers(__VA_ARGS__)
#define alSourceUnqueueBuffers(...) SndAr_alSourceUnqueueBuffers(__VA_ARGS__)
#define alGetError() SndAr_alGetError()

// Listener/distance state is fixed at init (snd_openal.cpp) and is exactly
// what snd_audren_pan.h assumes, so audren has nothing to do for it.
#define alDistanceModel(...) ((void)0)
#define alListener3f(...) ((void)0)
#define alListenerfv(...) ((void)0)

// EFX (the reverb bus and per-channel send filters): the DSP runs the reverb
// (snd_audren_reverb.h, snd_audren_al.h).
#define alGenFilters(...) SndAr_alGenFilters(__VA_ARGS__)
#define alDeleteFilters(...) SndAr_alDeleteFilters(__VA_ARGS__)
#define alFilteri(...) SndAr_alFilteri(__VA_ARGS__)
#define alFilterf(...) SndAr_alFilterf(__VA_ARGS__)
#define alGenEffects(...) SndAr_alGenEffects(__VA_ARGS__)
#define alDeleteEffects(...) SndAr_alDeleteEffects(__VA_ARGS__)
#define alEffecti(...) SndAr_alEffecti(__VA_ARGS__)
#define alEffectf(...) SndAr_alEffectf(__VA_ARGS__)
#define alEffectfv(...) SndAr_alEffectfv(__VA_ARGS__)
#define alGenAuxiliaryEffectSlots(...) SndAr_alGenAuxiliaryEffectSlots(__VA_ARGS__)
#define alDeleteAuxiliaryEffectSlots(...) SndAr_alDeleteAuxiliaryEffectSlots(__VA_ARGS__)
#define alAuxiliaryEffectSloti(...) SndAr_alAuxiliaryEffectSloti(__VA_ARGS__)

// Switch audren device (snd_audren_switch.cpp).
extern int g_sndAudrenFramesPerUpdate; // snd_audrenUpdateFrames, read at open
bool SND_AudrenOpen();   // renderer + driver + update thread; false (and FAIL line) when unavailable
void SND_AudrenClose();  // after every source/buffer is deleted
void SND_AudrenEmitPerf(uint64_t *busyUs, uint32_t *updates); // cumulative update-thread cost
void SND_AudrenEvidence(char *out, size_t size); // one-line stats for SND_OPENAL_EVIDENCE / sndmix
#endif
