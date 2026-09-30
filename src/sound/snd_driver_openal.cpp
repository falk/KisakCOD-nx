#ifdef KISAK_OPENAL

#include <universal/q_shared.h>
#include "snd_local.h"
#include "snd_public.h"
#include <qcommon/qcommon.h>
#include <universal/com_files.h>
#include <gfx_d3d/r_cinematic.h>
#include <universal/com_sndalias.h>
#include <universal/profile.h>

#ifdef KISAK_MP
#include <cgame_mp/cg_local_mp.h>
#elif KISAK_SP
#include <cgame/cg_main.h>
#endif

#include <dr_libs/dr_wav.h>
#include <dr_libs/dr_mp3.h>
#include <AL/efx-presets.h>
#include <fstream>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <database/db_retail_walk.h>

AlLocal alGlob;

// Switch sound backend: the audio DSP (snd_audren_al.cpp), EFX reverb
// included (snd_audren_reverb.h), routed unconditionally through
// snd_al_dispatch.h -- openal-soft's software mix is a meaningful share of a
// core on hardware. Every other build (upstream KisakCOD, non-Switch) links
// openal-soft directly, unmodified.
#include "snd_al_dispatch.h"

// Streamed sounds decode on their own thread (snd_stream_openal.cpp).
#include "snd_stream_openal.h"
static_assert(SND_STREAM_FIRST_CHANNEL == SND_FIRST_STREAM_CHANNEL, "stream channel range drift");
static_assert(SND_STREAM_CHANNEL_END == SND_MAX_CHANNELS, "stream channel range drift");

unsigned int SND_StreamSource(int index)
{
    return alGlob.source[index];
}

#include <port/switch_perf.h>
#ifdef __SWITCH__
#include <platform/switch/switch_diag_dvars.h>

// Play evidence: how many sources this backend actually started, by
// channel class, plus the AL error count.  SND_AlEmitEvidence prints it from
// SND_Update under com_diagMarkers so a device log can show
// that sound requests reached OpenAL rather than a stub.
static uint32_t s_alPlays[3];
static uint32_t s_alErrors;
static uint32_t s_alErrorCodes[6]; // AL_INVALID_NAME..AL_OUT_OF_MEMORY + unknown
static uint32_t s_alLastError;
static void SND_AlNotePlay(int kind)
{
    ++s_alPlays[kind];
    const ALenum err = alGetError();
    if (err != AL_NO_ERROR)
    {
        ++s_alErrors;
        s_alLastError = (uint32_t)err;
        const int slot = (err >= AL_INVALID_NAME && err <= AL_OUT_OF_MEMORY) ? (err - AL_INVALID_NAME) : 5;
        ++s_alErrorCodes[slot];
        if (s_alErrors <= 8)
            Com_Printf(0, "SND_OPENAL_ERROR kind=%d err=0x%x\n", kind, (unsigned)err);
    }
}
void SND_AlEmitEvidence()
{
    static int s_lastEmit = -100000;
    if (!com_diagMarkers || !com_diagMarkers->current.enabled)
        return;
    if (g_snd.time - s_lastEmit < 5000)
        return;
    s_lastEmit = g_snd.time;
    extern uint32_t SND_AlRetainedCount(), SND_AlRetainedBytes(), SND_AlRetainedAdpcm(), SND_AlRetainedRejected();
    extern uint64_t SND_AlRetainedMs();
    // Stream channels the engine still owns, and how many of them the AL
    // source is actually playing: a stream that is "active" but never reaches
    // AL_PLAYING is silent no matter how healthy the decode side looks.
    int streamHelds = 0;
    int streamPlaying = 0;
    const char *streamName = "";
    if (g_snd.max_stream_channels > 0)
    {
        for (int i = SND_FIRST_STREAM_CHANNEL; i < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels; ++i)
        {
            if (SND_IsStreamChannelFree(i))
                continue;
            ++streamHelds;
            ALint state;
            alGetSourcei(alGlob.source[i], AL_SOURCE_STATE, &state);
            if (state == AL_PLAYING)
                ++streamPlaying;
            if (!streamName[0] && g_snd.chaninfo[i].alias0 && g_snd.chaninfo[i].alias0->aliasName)
                streamName = g_snd.chaninfo[i].alias0->aliasName;
        }
    }
    Com_Printf(0, "SND_OPENAL_EVIDENCE plays2d=%u plays3d=%u streams=%u alerr=%u last=0x%x "
               "invname=%u invenum=%u invvalue=%u invop=%u oom=%u device=%s "
               "loaded=%u loaded_bytes=%u adpcm=%u rejected=%u retain_ms=%llu "
               "stream_held=%d stream_playing=%d stream_underrun=%u first=%s\n",
               s_alPlays[0], s_alPlays[1], s_alPlays[2], s_alErrors, (unsigned)s_alLastError,
               s_alErrorCodes[0], s_alErrorCodes[1], s_alErrorCodes[2], s_alErrorCodes[3],
               s_alErrorCodes[4], "audren",
               SND_AlRetainedCount(), SND_AlRetainedBytes(), SND_AlRetainedAdpcm(), SND_AlRetainedRejected(),
               static_cast<unsigned long long>(SND_AlRetainedMs()),
               streamHelds, streamPlaying, SND_StreamUnderrunCount(), streamName);
}
void SND_AlCheck(const char *where)
{
    const ALenum err = alGetError();
    if (err == AL_NO_ERROR)
        return;
    static uint32_t s_probeLog;
    if (s_probeLog < 16 && com_diagMarkers && com_diagMarkers->current.enabled)
    {
        ++s_probeLog;
        Com_Printf(0, "SND_OPENAL_PROBE err=0x%x at %s\n", (unsigned)err, where);
    }
}
#else
static void SND_AlNotePlay(int) {}
static void SND_AlCheck(const char *) {}
#endif

const dvar_t *snd_khz;
const dvar_t *snd_outputConfiguration;

void __cdecl TRACK_snd_driver()
{
    track_static_alloc_internal(&alGlob, sizeof(alGlob), "alGlob", 13);
}

bool __cdecl SND_IsMultiChannel()
{
    return alGlob.isMultiChannel;
}

char __cdecl SND_InitDriver()
{
    snd_khz = Dvar_RegisterInt("snd_khz", 44, (DvarLimits)0x2C0000000BLL, DVAR_ARCHIVE | DVAR_LATCH, "The game sound frequency.");
    snd_outputConfiguration = Dvar_RegisterEnum(
        "snd_outputConfiguration",
        snd_outputConfigurationStrings,
        0,
        DVAR_ARCHIVE | DVAR_LATCH,
        "Sound output configuration");
#if defined(__SWITCH__)
    // The only Switch sound backend: the audio DSP (snd_audren_al.cpp),
    // routed unconditionally by snd_al_dispatch.h. Non-Switch builds link
    // openal-soft directly (below, MSS_Init/MSS_Startup).
    g_sndAudrenFramesPerUpdate = Dvar_RegisterInt(
        "snd_audrenUpdateFrames", 1, DvarLimits(1, 4), DVAR_LATCH,
        "audren backend: 5 ms renderer frames per audrvUpdate (update-thread cost vs start latency)")
        ->current.integer;
    Com_Printf(CON_CHANNEL_SOUND, "SND_BACKEND audren\n");
#endif

    if (MSS_Startup())
    {
        if (MSS_Init())
        {
            MSS_InitChannels();
            MSS_InitEq();
            return 1;
        }
        else
        {
            MSS_ShutdownCleanup();
            MSS_InitFailed();
            return 0;
        }
    }
    else
    {
        MSS_InitFailed();
        return 0;
    }
}

void __cdecl SND_ShutdownDriver()
{
    R_Cinematic_StopPlayback();
    R_Cinematic_SyncNow();
    MSS_ShutdownCleanup();
}

int __cdecl SND_GetDriverCPUPercentage()
{
    // KISAK_OPENAL TODO: no direct OpenAL equivalent to Miles' per-driver CPU usage stat.
    return 0;
}

void __cdecl SND_Set3DPosition(int index, const float *org)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    float delta[3];
    int listenerIndex = SND_GetListenerIndexNearestToOrigin(org);
    Vec3Sub(org, g_snd.listeners[listenerIndex].orient.origin, delta);
    float transformed[3];
    MatrixTransposeTransformVector(delta, g_snd.listeners[listenerIndex].orient.axis, transformed);
    alSource3f(alGlob.source[index], AL_POSITION, -transformed[1], transformed[2], -transformed[0]);
}

#ifdef KISAK_OPENAL
void SND_ReleaseChannelBuffer(int index)
{
    if (alGlob.channelBuffer[index])
    {
        // AL_BUFFER may only be changed on a stopped/initial source; a
        // channel reused while its previous sound still plays (looping
        // aliases, early re-triggers) otherwise fails with
        // AL_INVALID_OPERATION and keeps the old buffer -- an error on
        // nearly every such play.
        alSourceStop(alGlob.source[index]);
        alSourcei(alGlob.source[index], AL_BUFFER, 0);
        // The buffer belongs to the loaded sound (SND_GetLoadedSoundBuffer),
        // shared by every channel playing it: only detach it here.
        alGlob.channelBuffer[index] = 0;
    }
}

// One AL buffer per loaded sound, created on its first play and shared by
// every channel that plays it.  A per-play alGenBuffers + alBufferData copied
// the whole PCM (retail loaded sounds are big uncompressed wavs: ~104 MB over
// 866 sounds on the killhouse load) on the game thread for every gunshot and
// footstep, and every voice held its own copy.  Keyed by the PCM pointer,
// which DB_RemoveLoadedSound frees when the zone unloads; it retires the
// entry first (SND_ReleaseLoadedSoundBuffer), so a later sound reusing the
// address never finds a stale buffer.  alBufferData keeps its own copy, so
// the PCM can be freed at once and the AL buffer deleted on the next sound
// frame, after any channel still holding it is stopped.
namespace
{
std::mutex s_loadedBufferLock;
std::unordered_map<const void *, ALuint> s_loadedBuffers;
std::vector<ALuint> s_retiredBuffers;
}

static ALuint SND_GetLoadedSoundBuffer(const MssSoundCOD4 *sound)
{
    std::lock_guard<std::mutex> lock(s_loadedBufferLock);
    const auto found = s_loadedBuffers.find(sound->data);
    if (found != s_loadedBuffers.end())
        return found->second;

    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    if (!buffer)
        return 0;
    const ALenum format = (sound->info.channels == 2)
        ? (sound->info.bits == 8 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16)
        : (sound->info.bits == 8 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16);
    alBufferData(buffer, format, sound->data, sound->info.data_len, sound->info.rate);
    if (alGetError() != AL_NO_ERROR)
    {
        alDeleteBuffers(1, &buffer);
        return 0;
    }
    s_loadedBuffers.emplace(sound->data, buffer);
    return buffer;
}

void SND_ReleaseLoadedSoundBuffer(const void *data)
{
    if (!data)
        return;
    std::lock_guard<std::mutex> lock(s_loadedBufferLock);
    const auto found = s_loadedBuffers.find(data);
    if (found == s_loadedBuffers.end())
        return;
    s_retiredBuffers.push_back(found->second);
    s_loadedBuffers.erase(found);
}

// Game thread, once per sound frame.
void SND_ServiceLoadedSoundBuffers()
{
    std::vector<ALuint> retired;
    {
        std::lock_guard<std::mutex> lock(s_loadedBufferLock);
        retired.swap(s_retiredBuffers);
    }
    for (const ALuint buffer : retired)
    {
        for (int index = 0; index < SND_FIRST_STREAM_CHANNEL; ++index)
        {
            if (alGlob.channelBuffer[index] != buffer)
                continue;
            if (index < g_snd.max_2D_channels && !SND_Is2DChannelFree(index))
                SND_Stop2DChannel(index);
            else if (index >= 8 && index < 8 + g_snd.max_3D_channels && !SND_Is3DChannelFree(index))
                SND_Stop3DChannel(index);
            else
                SND_ReleaseChannelBuffer(index);
        }
        alDeleteBuffers(1, &buffer);
    }
}

// Sound shutdown, after the sources are deleted (nothing is attached).
void SND_FreeLoadedSoundBuffers()
{
    std::lock_guard<std::mutex> lock(s_loadedBufferLock);
    for (const auto &entry : s_loadedBuffers)
        alDeleteBuffers(1, &entry.second);
    for (const ALuint buffer : s_retiredBuffers)
        alDeleteBuffers(1, &buffer);
    s_loadedBuffers.clear();
    s_retiredBuffers.clear();
}

// Defined near SND_SetRoomtype (Phase 6) below; forward-declared here since
// SND_StartAlias2D/3DSample and SND_StartAliasStreamOnChannel call it before that point.
void SND_ApplyReverbSend(int index, const snd_alias_t *alias);

#endif

void __cdecl SND_Stop2DChannel(int index)
{
    iassert((index >= 0 && index < 0 + g_snd.max_2D_channels));
    SND_ReleaseChannelBuffer(index);
    SND_ResetChannelInfo(index);
    SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
}

void __cdecl SND_Pause2DChannel(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);
    alSourcePause(alGlob.source[index]);
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_Unpause2DChannel(int index, int timeshift)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    if (!g_snd.chaninfo[index].startDelay)
    {
        alSourcePlay(alGlob.source[index]);
    }

    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_Is2DChannelFree(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    return !g_snd.chaninfo[index].paused && !g_snd.chaninfo[index].startDelay && g_snd.chaninfo[index].alias0 == 0;
}

void __cdecl SND_Stop3DChannel(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    SND_ReleaseChannelBuffer(index);
    SND_ResetChannelInfo(index);
    SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
}

void __cdecl SND_Pause3DChannel(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);
    alSourcePause(alGlob.source[index]);
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_Unpause3DChannel(int index, int timeshift)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    if (!g_snd.chaninfo[index].startDelay)
    {
        alSourcePlay(alGlob.source[index]);
    }

    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_Is3DChannelFree(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    return !g_snd.chaninfo[index].paused && !g_snd.chaninfo[index].startDelay && g_snd.chaninfo[index].alias0 == 0;
}

void __cdecl SND_StopStreamChannel(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    if (SND_DebugStartsEnabled() && g_snd.chaninfo[index].alias0)
    {
        // Every stream-channel stop with its reason and the channel's age, so
        // a line cut short (age << len) names who cut it.
        const snd_channel_info_t *ci = &g_snd.chaninfo[index];
        SndStreamStatus status;
        SND_StreamPoll(index, &status);
        Com_Printf(CON_CHANNEL_SOUND,
                   "SND_STOPDBG alias=%s ch=%d ent=%d reason=%s age=%d len=%d stream=%d frames=%lld/%lld t=%d\n",
                   ci->alias0->aliasName ? ci->alias0->aliasName : "?", index, ci->sndEnt.field.entIndex,
                   snd_stopReason ? snd_stopReason : "other", g_snd.time - ci->startTime, ci->totalMsec,
                   (int)status.state, (long long)status.framesQueued, (long long)status.totalFrames, g_snd.time);
    }
    snd_stopReason = nullptr;
    SND_StreamStop(index);
    SND_ResetChannelInfo(index);
    SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
}

void __cdecl SND_PauseStreamChannel(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);
    SND_StreamSetWantPlay(index, false); // the stream thread owns play state
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_UnpauseStreamChannel(int index, int timeshift)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    if (!g_snd.chaninfo[index].startDelay)
    {
        SND_StreamSetWantPlay(index, true);
    }

    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_IsStreamChannelFree(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    if (!SND_StreamIsActive(index))
        return 1;

    if (g_snd.chaninfo[index].paused || g_snd.chaninfo[index].startDelay)
        return 0;

    return g_snd.chaninfo[index].alias0 == 0;
}

void __cdecl SND_ApplyChannelMap(ALuint handle, const snd_alias_t *alias, int srcChannelCount)
{
    // KISAK_OPENAL TODO (Phase 4): per-speaker channel-level mapping. 
}

// start_msec is pitched time (total_msec above is computed at rate * pitch),
// while AL offsets are native buffer time.  Without the pitch a pitch < 1
// random start (alias flag 0x20) landed past the end of the buffer, which
// OpenAL rejects with AL_INVALID_VALUE.
static void SND_SetLoadedStartOffset(ALuint source, int start_msec, float pitch, const MssSoundCOD4 *sound)
{
    int64_t frame = (int64_t)((double)start_msec * pitch * sound->info.rate / 1000.0);
    if (frame < 0)
        frame = 0;
    if (sound->info.samples > 0 && frame >= (int64_t)sound->info.samples)
        frame = (int64_t)sound->info.samples - 1;
    alSourcei(source, AL_SAMPLE_OFFSET, (ALint)frame);
}

int __cdecl SND_StartAlias2DSample(SndStartAliasInfo *startAliasInfo, int *pChannel)
{
    iassert(startAliasInfo->alias0);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias0->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile);
    iassert(startAliasInfo->alias0->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias0->soundFile->exists);
    iassert(startAliasInfo->alias1);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias1->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile);
    iassert(startAliasInfo->alias1->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias1->soundFile->exists);

    int entchannel = SNDALIASFLAGS_GET_CHANNEL(startAliasInfo->alias0->flags);
    if (!SND_HasFreeVoice(entchannel))
        return -1;

    int index = SND_FindFree2DChannel(startAliasInfo, entchannel);
    if (pChannel)
        *pChannel = index;

    if (index < 0)
        return -1;

    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    ALuint source = alGlob.source[index];
    MssSoundCOD4 *sound = &startAliasInfo->alias0->soundFile->u.loadSnd->sound;

    // dr_wav (Phase 3) always decoded loaded sounds to 16-bit PCM, so the format is always
    // mono/stereo 16-bit - no need to replicate MSS_DigitalFormatType's branching here.
    SND_ReleaseChannelBuffer(index);
    if (!sound->data)
        return -1; // payload not retained (no backend at walk time); never play null
    const ALuint buffer = SND_GetLoadedSoundBuffer(sound);
    if (!buffer)
        return -1;
    alGlob.channelBuffer[index] = buffer;
    alSourcei(source, AL_BUFFER, buffer);

    MSS_ApplyEqFilter(source, entchannel);

    // AL_PITCH is already a ratio relative to the buffer's native (embedded) rate, unlike
    // Miles' AIL_set_sample_playback_rate which needed the current absolute rate multiplied
    // out by hand - so this is simpler than the Miles branch above, not just a translation.
    float pitch = startAliasInfo->timescale ? startAliasInfo->pitch * (float)g_snd.timescale : startAliasInfo->pitch;
    alSourcef(source, AL_PITCH, pitch);

    float realVolume = startAliasInfo->volume
        * g_snd.volume
        * g_snd.channelvol->channelvol[SNDALIASFLAGS_GET_CHANNEL(startAliasInfo->alias0->flags)].volume;

    if (g_snd.slaveLerp != 0.0 && !startAliasInfo->master && (startAliasInfo->alias0->flags & 4) != 0)
        realVolume = SND_GetLerpedSlavePercentage(startAliasInfo->alias0->slavePercentage) * realVolume;

    SND_ApplyChannelMap(source, startAliasInfo->alias0, sound->info.channels);
    SND_Set2DChannelVolume(index, realVolume);
    alSourcei(source, AL_LOOPING, (startAliasInfo->alias0->flags & 1) != 0 ? AL_TRUE : AL_FALSE); // matches AIL_set_sample_loop_count's polarity above: flags bit 0 set == looping
    SND_ApplyReverbSend(index, startAliasInfo->alias0);

    // Duration at the pitch-adjusted rate, not the buffer's native rate - matches Miles'
    // behavior above, which queries AIL_sample_ms_position() *after* setting the pitched
    // playback rate, so a pitched-up sound reports a proportionally shorter duration.
    float effectiveRate = sound->info.rate * pitch;
    int total_msec = effectiveRate > 0.0f ? (int)((int64_t)sound->info.samples * 1000 / effectiveRate) : 0;

    if (startAliasInfo->timeshift >= total_msec)
        return SND_SetPlaybackIdNotPlayed(index);

    int start_msec;
    if (startAliasInfo->fraction == 0.0)
    {
        if (startAliasInfo->timeshift)
        {
            start_msec = startAliasInfo->timeshift;
        }
        else if ((startAliasInfo->alias0->flags & 0x20) != 0)
        {
            start_msec = SnapFloatToInt(random() * (float)total_msec) & 0xFFFFFF80;
        }
        else
        {
            start_msec = 0;
        }
    }
    else
    {
        start_msec = SnapFloatToInt((float)total_msec * startAliasInfo->fraction);
    }
    if (start_msec)
        startAliasInfo->startDelay = 0;

    SND_SetLoadedStartOffset(source, start_msec, pitch, sound);
    if (!startAliasInfo->startDelay
        && (!g_snd.paused || !g_snd.pauseSettings[(startAliasInfo->alias0->flags & 0x3F00) >> 8]))
    {
        alSourcePlay(source);
        SND_AlNotePlay(0);
    }

    total_msec += startAliasInfo->startDelay;
    if ((startAliasInfo->alias0->flags & 1) != 0)
        total_msec = 0;
    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, sound->info.channels, sound->info.rate, total_msec, start_msec, SFLS_LOADED);
    int playbackId = SND_AcquirePlaybackId(index, total_msec);

    if (playbackId != -1)
        SND_AddVoice(entchannel);

    return playbackId;
}

void __cdecl SND_Apply3DSpatializationTweaks(ALuint handle, const snd_alias_t *alias)
{
    // KISAK_OPENAL TODO (Phase 4)
}

int __cdecl SND_StartAlias3DSample(SndStartAliasInfo *startAliasInfo, int *pChannel)
{
    iassert(startAliasInfo->alias0);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias0->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile);
    iassert(startAliasInfo->alias0->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias0->soundFile->exists);
    iassert(startAliasInfo->alias1);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias1->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile);
    iassert(startAliasInfo->alias1->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias1->soundFile->exists);

    int entchannel = (startAliasInfo->alias0->flags & 0x3F00) >> 8;
    if (!SND_HasFreeVoice(entchannel))
        return -1;
    int index = SND_FindFree3DChannel(startAliasInfo, entchannel);
    if (pChannel)
        *pChannel = index;
    if (index < 0)
        return -1;
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    ALuint source = alGlob.source[index];
    MssSoundCOD4 *sound = &startAliasInfo->alias0->soundFile->u.loadSnd->sound;
    float distMin = (1.0f - startAliasInfo->lerp) * startAliasInfo->alias0->distMin
        + startAliasInfo->alias1->distMin * startAliasInfo->lerp;
    float distMax = (1.0f - startAliasInfo->lerp) * startAliasInfo->alias0->distMax
        + startAliasInfo->alias1->distMax * startAliasInfo->lerp;

    SND_AlCheck("3d:enter");
    SND_ReleaseChannelBuffer(index);
    SND_AlCheck("3d:release");
    if (!sound->data)
        return -1; // payload not retained (no backend at walk time); never play null
    const ALuint buffer = SND_GetLoadedSoundBuffer(sound);
    if (!buffer)
        return -1;
    alGlob.channelBuffer[index] = buffer;
    alSourcei(source, AL_BUFFER, buffer);
    SND_AlCheck("3d:buffer");

    MSS_ApplyEqFilter(source, entchannel);
    SND_AlCheck("3d:eq");

    const float *listener = g_snd.listeners[SND_GetListenerIndexNearestToOrigin(startAliasInfo->org)].orient.origin;
    float diff[3];
    Vec3Sub(listener, startAliasInfo->org, diff);
    float distance = Vec3Length(diff);
    float attenuation = SND_Attenuate(startAliasInfo->alias0->volumeFalloffCurve, distance, distMin, distMax);
    float realVolume = startAliasInfo->volume
        * attenuation
        * g_snd.channelvol->channelvol[(startAliasInfo->alias0->flags & 0x3F00) >> 8].volume;
    realVolume = realVolume * g_snd.volume;
    if (g_snd.slaveLerp != 0.0 && !startAliasInfo->master && (startAliasInfo->alias0->flags & 4) != 0)
        realVolume = SND_GetLerpedSlavePercentage(startAliasInfo->alias0->slavePercentage) * realVolume;

    SND_Apply3DSpatializationTweaks(source, startAliasInfo->alias0);
    SND_Set3DChannelVolume(index, realVolume);
    // distMin/distMax feed OpenAL's automatic distance-attenuation model, which is disabled
    // (alDistanceModel(AL_NONE), see MSS_Init) since SND_Attenuate's curve above already
    // computes the final gain by hand - so unlike Miles' AIL_set_sample_3D_distances there's
    // nothing to set here at all.

    float pitch = startAliasInfo->timescale ? startAliasInfo->pitch * (float)g_snd.timescale : startAliasInfo->pitch;
    alSourcef(source, AL_PITCH, pitch);
    SND_Set3DPosition(index, startAliasInfo->org);
    alSourcei(source, AL_LOOPING, (startAliasInfo->alias0->flags & 1) != 0 ? AL_TRUE : AL_FALSE); // matches AIL_set_sample_loop_count's polarity above: flags bit 0 set == looping
    SND_AlCheck("3d:params");
    SND_ApplyReverbSend(index, startAliasInfo->alias0);
    SND_AlCheck("3d:reverb");

    // Duration at the pitch-adjusted rate - see the matching comment in
    // SND_StartAlias2DSample above.
    float effectiveRate = sound->info.rate * pitch;
    int total_msec = effectiveRate > 0.0f ? (int)((int64_t)sound->info.samples * 1000 / effectiveRate) : 0;

    if (startAliasInfo->timeshift >= total_msec)
        return SND_SetPlaybackIdNotPlayed(index);

    int start_msec;
    if (startAliasInfo->fraction == 0.0)
    {
        if (startAliasInfo->timeshift)
        {
            start_msec = startAliasInfo->timeshift;
        }
        else if ((startAliasInfo->alias0->flags & 0x20) != 0)
        {
            start_msec = SnapFloatToInt(random() * (float)total_msec) & 0xFFFFFF80;
        }
        else
        {
            start_msec = 0;
        }
    }
    else
    {
        start_msec = SnapFloatToInt((float)total_msec * startAliasInfo->fraction);
    }
    if (start_msec)
        startAliasInfo->startDelay = 0;

    SND_SetLoadedStartOffset(source, start_msec, pitch, sound);
    if (!startAliasInfo->startDelay
        && (!g_snd.paused || !g_snd.pauseSettings[(startAliasInfo->alias0->flags & 0x3F00) >> 8]))
    {
        alSourcePlay(source);
        SND_AlNotePlay(1);
    }
    int totalMsecForChan = total_msec + startAliasInfo->startDelay;
    if ((startAliasInfo->alias0->flags & 1) != 0)
        totalMsecForChan = 0;
    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, sound->info.channels, sound->info.rate, totalMsecForChan, start_msec, SFLS_LOADED);
    int playbackId = SND_AcquirePlaybackId(index, totalMsecForChan);
    if (playbackId != -1)
        SND_AddVoice(entchannel);
    return playbackId;
}

void __cdecl SND_Set3DStreamPosition(int index, int listenerIndex, const float *org)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    float delta[3];
    Vec3Sub(org, g_snd.listeners[listenerIndex].orient.origin, delta);
    float transformed[3];
    MatrixTransposeTransformVector(delta, g_snd.listeners[listenerIndex].orient.axis, transformed);
    alSource3f(alGlob.source[index], AL_POSITION, -transformed[1], transformed[2], -transformed[0]);
}

double __cdecl SND_GetStream3DVolumeFallOff(int index, int listenerIndex)
{
    float diff[3]; // [esp+10h] [ebp-24h] BYREF
    float maxdist; // [esp+1Ch] [ebp-18h]
    float dist; // [esp+20h] [ebp-14h]
    float lerp; // [esp+24h] [ebp-10h]
    const snd_alias_t *alias1; // [esp+28h] [ebp-Ch]
    const snd_alias_t *alias0; // [esp+2Ch] [ebp-8h]
    float mindist; // [esp+30h] [ebp-4h]

    iassert(index >= ((0 + 8) + 32) && index < g_snd.max_stream_channels + ((0 + 8) + 32));

    alias0 = g_snd.chaninfo[index].alias0;
    alias1 = g_snd.chaninfo[index].alias1;
    if (!SND_IsAliasChannel3D(SNDALIASFLAGS_GET_CHANNEL(alias0->flags)))
        MyAssertHandler(
            ".\\win32\\snd_driver.cpp",
            585,
            0,
            "%s",
            "SND_IsAliasChannel3D( SNDALIASFLAGS_GET_CHANNEL( alias0->flags ) )");
    Vec3Sub(g_snd.listeners[listenerIndex].orient.origin, g_snd.chaninfo[index].org, diff);
    dist = Vec3Length(diff);
    lerp = g_snd.chaninfo[index].lerp;
    mindist = (1.0 - lerp) * alias0->distMin + alias1->distMin * lerp;
    maxdist = (1.0 - lerp) * alias0->distMax + alias1->distMax * lerp;
    return SND_Attenuate(alias0->volumeFalloffCurve, dist, mindist, maxdist);
}


// Stream channels whose length was unknown at start (uncached MP3); the
// stream thread reports it and SND_UpdateStreamChannel applies it.
static bool s_streamLengthPending[SND_MAX_CHANNELS - SND_FIRST_STREAM_CHANNEL];

int __cdecl SND_StartAliasStreamOnChannel(SndStartAliasInfo *startAliasInfo, int index)
{
    iassert(startAliasInfo->alias0);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias0->flags) == SAT_STREAMED);
    iassert(startAliasInfo->alias1);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias1->flags) == SAT_STREAMED);
    iassert((index >= ((0 + 8) + 32) && index < ((0 + 8) + 32) + g_snd.max_stream_channels));

    bool fsInitialized = FS_Initialized();
    iassert(fsInitialized);

    int entchannel = SNDALIASFLAGS_GET_CHANNEL(startAliasInfo->alias0->flags);
    const int dbgEnt = startAliasInfo->sndEnt.field.entIndex;
    if (!SND_HasFreeVoice(entchannel))
    {
        SND_DebugStartFail(startAliasInfo->alias0, dbgEnt, "stream_no_free_voice");
        return -1;
    }

    char filename[128];
    Com_GetSoundFileName(startAliasInfo->alias0, filename, 128);

    if (com_diagMarkers && com_diagMarkers->current.enabled)
    {
        static uint32_t s_streamLogs = 0;
        if (s_streamLogs < 16u)
        {
            ++s_streamLogs;
            Com_Printf(0, "KILLHOUSE_SNDSTREAM alias=%s file=%s\n",
                       startAliasInfo->alias0 && startAliasInfo->alias0->aliasName ? startAliasInfo->alias0->aliasName : "(null)",
                       filename);
        }
    }
    if (!startAliasInfo->alias0->soundFile->exists)
    {
        Com_DPrintf(
            CON_CHANNEL_SOUND,
            "Tried to play streamed sound '%s' from alias '%s', but it was not found at load time.\n",
            filename,
            startAliasInfo->alias0->aliasName);
        SND_DebugStartFail(startAliasInfo->alias0, dbgEnt, "stream_file_missing_at_load");
        return SND_SetPlaybackIdNotPlayed(index);
    }

    char realname[256];
    Com_sprintf(realname, 0x100u, "sound/%s", filename);

    SND_StreamStop(index); // close any previous stream still open on this channel

    size_t nameLen = strlen(filename);
    const bool isMp3 = nameLen >= 4 && I_stricmp(filename + nameLen - 4, ".mp3") == 0;
    SndStreamOpenInfo openInfo{};
    if (!SND_StreamOpen(index, realname, isMp3, &openInfo))
    {
        Com_PrintError(CON_CHANNEL_SOUND, "Couldn't play stream '%s' from alias '%s' - file not found or invalid format\n",
                       realname, startAliasInfo->alias0->aliasName);
        SND_DebugStartFail(startAliasInfo->alias0, dbgEnt, "stream_open_failed");
        return SND_SetPlaybackIdNotPlayed(index);
    }

    const int srcChannelCount = openInfo.channels;
    const int baserate = openInfo.sampleRate;
    const bool looping = (startAliasInfo->alias0->flags & 1) != 0; // matches AIL_set_stream_loop_count's polarity: flags bit 0 set == looping
    // An MP3 without an Xing/Info header (all retail ones) has no length until
    // its frames are counted; the stream thread does that off this thread and
    // the length is filled in by SND_UpdateStreamChannel (retail's unknown-
    // length state, totalMsec < 0, queues length notifies until then).
    const bool lengthKnown = openInfo.totalFrames >= 0;

    ALuint source = alGlob.source[index];
    MSS_ApplyEqFilter(source, entchannel);

    float pitch = startAliasInfo->timescale ? startAliasInfo->pitch * (float)g_snd.timescale : startAliasInfo->pitch;
    alSourcef(source, AL_PITCH, pitch);

    // Duration at the pitch-adjusted rate, not the stream's native rate - see
    // the matching comment in SND_StartAlias2DSample.
    float effectiveRate = baserate * pitch;
    int total_msec = lengthKnown && effectiveRate > 0.0f ? (int)(openInfo.totalFrames * 1000 / effectiveRate) : -1;

    float realVolume = startAliasInfo->volume
        * g_snd.volume
        * g_snd.channelvol->channelvol[(startAliasInfo->alias0->flags & 0x3F00) >> 8].volume;
    if (g_snd.slaveLerp != 0.0 && !startAliasInfo->master && (startAliasInfo->alias0->flags & 4) != 0)
        realVolume = SND_GetLerpedSlavePercentage(startAliasInfo->alias0->slavePercentage) * realVolume;

    alSourcei(source, AL_LOOPING, AL_FALSE); // looping is handled by the stream thread re-queuing, not AL_LOOPING
    SND_ApplyReverbSend(index, startAliasInfo->alias0);

    if (lengthKnown && startAliasInfo->timeshift >= total_msec && total_msec != 0)
    {
        SND_DebugStartFail(startAliasInfo->alias0, dbgEnt, "stream_timeshift_past_end");
        SND_StreamCancel(index);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    if (lengthKnown && openInfo.totalFrames == 0)
    {
        SND_DebugStartFail(startAliasInfo->alias0, dbgEnt, "stream_zero_length");
        SND_StreamCancel(index);
        Com_PrintError(CON_CHANNEL_ERROR, "ERROR: Sound file '%s' is zero length, invalid\n", realname);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    // Start position.  start_msec is in pitched time (what the engine's
    // bookkeeping uses); the stream seeks in native frames, so the offset is
    // converted with the pitch -- a pitched random start must still land
    // inside the file.
    SndStreamStart start{};
    start.frame = -1;
    int start_msec = 0;
    float startFraction = 0.0f;
    if (startAliasInfo->fraction != 0.0)
        startFraction = startAliasInfo->fraction;
    else if (!startAliasInfo->timeshift && (startAliasInfo->alias0->flags & 0x20) != 0)
        startFraction = random();
    if (startFraction > 0.0f)
    {
        if (lengthKnown)
        {
            start.frame = (int64_t)((double)startFraction * openInfo.totalFrames);
            start_msec = SnapFloatToInt((float)total_msec * startFraction);
        }
        else
        {
            start.fraction = startFraction;
            start_msec = 1; // non-zero: a mid-file start never waits on startDelay
        }
    }
    else if (startAliasInfo->timeshift)
    {
        start_msec = startAliasInfo->timeshift;
        const int nativeMsec = (int)((float)start_msec * pitch);
        if (lengthKnown)
            start.frame = (int64_t)nativeMsec * baserate / 1000;
        else
            start.nativeMsec = nativeMsec;
    }
    else
    {
        start.frame = 0;
    }
    if (start_msec)
        startAliasInfo->startDelay = 0;

    const bool pausedAtStart = g_snd.paused && g_snd.pauseSettings[(startAliasInfo->alias0->flags & 0x3F00) >> 8];
    const bool wantPlay = !startAliasInfo->startDelay && !pausedAtStart;
    SND_StreamBegin(index, looping, start, wantPlay);
    if (wantPlay)
        SND_AlNotePlay(2);

    int totalMsecForChan = lengthKnown ? total_msec + startAliasInfo->startDelay : -1;
    if (looping)
        totalMsecForChan = 0;

    float *org = g_snd.chaninfo[index].org;
    *org = startAliasInfo->org[0];
    org[1] = startAliasInfo->org[1];
    org[2] = startAliasInfo->org[2];
    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, srcChannelCount, baserate, totalMsecForChan < 0 ? 0 : totalMsecForChan,
                                lengthKnown ? start_msec : 0, SFLS_LOADED);
    s_streamLengthPending[index - SND_FIRST_STREAM_CHANNEL] = totalMsecForChan < 0;

    if (SND_IsAliasChannel3D((g_snd.chaninfo[index].alias0->flags & 0x3F00) >> 8))
    {
        SND_GetCurrent3DPosition(g_snd.chaninfo[index].sndEnt, g_snd.chaninfo[index].offset, g_snd.chaninfo[index].org);
        int listenerIndex = SND_GetListenerIndexNearestToOrigin(g_snd.chaninfo[index].org);
        SND_Set3DStreamPosition(index, listenerIndex, g_snd.chaninfo[index].org);
        realVolume = (float)SND_GetStream3DVolumeFallOff(index, listenerIndex) * realVolume;
    }
    else
    {
        SND_ApplyChannelMap(source, startAliasInfo->alias0, srcChannelCount);
    }
    SND_SetStreamChannelVolume(index, realVolume);

    int playbackId = SND_AcquirePlaybackId(index, totalMsecForChan);
    if (playbackId != -1)
        SND_AddVoice(entchannel);
    return playbackId;
}

// snd_roomStrings[27] (snd.cpp) is the exact same 26-name, same-order I3DL2/EAX preset list
// as these - verified name-for-name, not assumed - so this is a mechanical 1:1 copy, not a
// design decision. SND_RoomtypeFromString (snd.cpp) maps a string to an index into this
// same list, so the two arrays must stay in lockstep.
static const EFXEAXREVERBPROPERTIES AL_RoomPresets[26] =
{
    EFX_REVERB_PRESET_GENERIC,
    EFX_REVERB_PRESET_PADDEDCELL,
    EFX_REVERB_PRESET_ROOM,
    EFX_REVERB_PRESET_BATHROOM,
    EFX_REVERB_PRESET_LIVINGROOM,
    EFX_REVERB_PRESET_STONEROOM,
    EFX_REVERB_PRESET_AUDITORIUM,
    EFX_REVERB_PRESET_CONCERTHALL,
    EFX_REVERB_PRESET_CAVE,
    EFX_REVERB_PRESET_ARENA,
    EFX_REVERB_PRESET_HANGAR,
    EFX_REVERB_PRESET_CARPETEDHALLWAY,
    EFX_REVERB_PRESET_HALLWAY,
    EFX_REVERB_PRESET_STONECORRIDOR,
    EFX_REVERB_PRESET_ALLEY,
    EFX_REVERB_PRESET_FOREST,
    EFX_REVERB_PRESET_CITY,
    EFX_REVERB_PRESET_MOUNTAINS,
    EFX_REVERB_PRESET_QUARRY,
    EFX_REVERB_PRESET_PLAIN,
    EFX_REVERB_PRESET_PARKINGLOT,
    EFX_REVERB_PRESET_SEWERPIPE,
    EFX_REVERB_PRESET_UNDERWATER,
    EFX_REVERB_PRESET_DRUGGED,
    EFX_REVERB_PRESET_DIZZY,
    EFX_REVERB_PRESET_PSYCHOTIC,
};

static void AL_ApplyReverbPreset(ALuint effect, const EFXEAXREVERBPROPERTIES &props)
{
    alEffectf(effect, AL_EAXREVERB_DENSITY, props.flDensity);
    alEffectf(effect, AL_EAXREVERB_DIFFUSION, props.flDiffusion);
    alEffectf(effect, AL_EAXREVERB_GAIN, props.flGain);
    alEffectf(effect, AL_EAXREVERB_GAINHF, props.flGainHF);
    alEffectf(effect, AL_EAXREVERB_GAINLF, props.flGainLF);
    alEffectf(effect, AL_EAXREVERB_DECAY_TIME, props.flDecayTime);
    alEffectf(effect, AL_EAXREVERB_DECAY_HFRATIO, props.flDecayHFRatio);
    alEffectf(effect, AL_EAXREVERB_DECAY_LFRATIO, props.flDecayLFRatio);
    alEffectf(effect, AL_EAXREVERB_REFLECTIONS_GAIN, props.flReflectionsGain);
    alEffectf(effect, AL_EAXREVERB_REFLECTIONS_DELAY, props.flReflectionsDelay);
    alEffectfv(effect, AL_EAXREVERB_REFLECTIONS_PAN, props.flReflectionsPan);
    alEffectf(effect, AL_EAXREVERB_LATE_REVERB_GAIN, props.flLateReverbGain);
    alEffectf(effect, AL_EAXREVERB_LATE_REVERB_DELAY, props.flLateReverbDelay);
    alEffectfv(effect, AL_EAXREVERB_LATE_REVERB_PAN, props.flLateReverbPan);
    alEffectf(effect, AL_EAXREVERB_ECHO_TIME, props.flEchoTime);
    alEffectf(effect, AL_EAXREVERB_ECHO_DEPTH, props.flEchoDepth);
    alEffectf(effect, AL_EAXREVERB_MODULATION_TIME, props.flModulationTime);
    alEffectf(effect, AL_EAXREVERB_MODULATION_DEPTH, props.flModulationDepth);
    alEffectf(effect, AL_EAXREVERB_AIR_ABSORPTION_GAINHF, props.flAirAbsorptionGainHF);
    alEffectf(effect, AL_EAXREVERB_HFREFERENCE, props.flHFReference);
    alEffectf(effect, AL_EAXREVERB_LFREFERENCE, props.flLFReference);
    alEffectf(effect, AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, props.flRoomRolloffFactor);
    alEffecti(effect, AL_EAXREVERB_DECAY_HFLIMIT, props.iDecayHFLimit);
}

// Sets the per-channel wet-send *gain* (AL_AUXILIARY_SEND_FILTER has no gain parameter of
// its own - see AlLocal::sendFilter's comment in snd_local.h for why a lowpass filter is
// used purely as a gain carrier here) and wires the channel's source to the global reverb
// aux slot. MSS_GetDryLevel() is hardcoded to 1.0f in the existing Miles code (dead code,
// see WORK.md Phase 6) so there's no dry-side control to port - the dry/direct path is left
// at its default (unfiltered, full AL_GAIN) which already matches "dry always 1.0".
void SND_ApplyReverbSend(int index, const snd_alias_t *alias)
{
    float wet = MSS_GetWetLevel(alias);
    alFilterf(alGlob.sendFilter[index], AL_LOWPASS_GAIN, wet);
    alFilterf(alGlob.sendFilter[index], AL_LOWPASS_GAINHF, 1.0f);
    alSource3i(alGlob.source[index], AL_AUXILIARY_SEND_FILTER, alGlob.auxSlot, 0, alGlob.sendFilter[index]);
}

void __cdecl SND_SetRoomtype(int roomtype)
{
    iassert(roomtype >= 0 && roomtype < ARRAY_COUNT(AL_RoomPresets));
    AL_ApplyReverbPreset(alGlob.reverbEffect, AL_RoomPresets[roomtype]);
    alAuxiliaryEffectSloti(alGlob.auxSlot, AL_EFFECTSLOT_EFFECT, alGlob.reverbEffect);
}

void __cdecl SND_UpdateEqs()
{
    // Mirrors the Miles loop, but MSS_ApplyEqFilter (snd_al.cpp) is a deliberate no-op
    for (int channelIndex = 0; channelIndex < 53; ++channelIndex)
    {
        MSS_ApplyEqFilter(alGlob.source[channelIndex], g_snd.chaninfo[channelIndex].entchannel);
    }
}

void __cdecl SND_SetEqParams(
    uint32_t entchannel,
    int eqIndex,
    uint32_t band,
    SND_EQTYPE type,
    float gain,
    float freq,
    float q)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);
    iassert(freq >= 0 && freq <= 20000);
    iassert(q > 0);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));

    alGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    alGlob.eq[eqIndex].params[band][entchannel].gain = gain;
    alGlob.eq[eqIndex].params[band][entchannel].freq = freq;
    alGlob.eq[eqIndex].params[band][entchannel].q = q;
    alGlob.eq[eqIndex].params[band][entchannel].type = type;

#ifndef KISAK_XBOX
    SND_UpdateEqs();
#endif
}

void __cdecl SND_SetEqType(uint32_t entchannel, int eqIndex, uint32_t band, SND_EQTYPE type)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));

    alGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    alGlob.eq[eqIndex].params[band][entchannel].type = type;
}

void __cdecl SND_SetEqFreq(uint32_t entchannel, int eqIndex, uint32_t band, float freq)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);
    iassert(freq >= 0 && freq <= 20000);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));

    alGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    alGlob.eq[eqIndex].params[band][entchannel].freq = freq;
}

void __cdecl SND_SetEqGain(uint32_t entchannel, int eqIndex, uint32_t band, float gain)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));
    alGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    alGlob.eq[eqIndex].params[band][entchannel].gain = gain;
}

void __cdecl SND_SetEqQ(uint32_t entchannel, int eqIndex, uint32_t band, float q)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);
    iassert(q > 0);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));

    alGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    alGlob.eq[eqIndex].params[band][entchannel].q = q;

#ifndef KISAK_XBOX
    SND_UpdateEqs();
#endif
}

void __cdecl SND_DisableEq(uint32_t entchannel, int eqIndex, uint32_t band)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));

    alGlob.eq[eqIndex].params[band][entchannel].enabled = 0;
}

void __cdecl SND_SaveEq(MemoryFile *memFile)
{
    int band; // [esp+0h] [ebp-Ch]
    int entchannel; // [esp+4h] [ebp-8h]
    int eqIndex; // [esp+8h] [ebp-4h]

    for (eqIndex = 0; eqIndex < 2; ++eqIndex)
    {
        for (band = 0; band < 3; ++band)
        {
            for (entchannel = 0; entchannel < 64; ++entchannel)
            {
                MemFile_WriteData(memFile, 20, &alGlob.eq[eqIndex].params[band][entchannel]);
            }
        }
    }
}

void __cdecl SND_RestoreEq(MemoryFile *memFile)
{
    int band; // [esp+0h] [ebp-Ch]
    int entchannel; // [esp+4h] [ebp-8h]
    int eqIndex; // [esp+8h] [ebp-4h]

    for (eqIndex = 0; eqIndex < 2; ++eqIndex)
    {
        for (band = 0; band < 3; ++band)
        {
            for (entchannel = 0; entchannel < 64; ++entchannel)
            {
                MemFile_ReadData(memFile, 20, (uint8_t *)&alGlob.eq[eqIndex].params[band][entchannel]);
            }
        }
    }
}

void __cdecl SND_PrintEqParams()
{
    float *v0; // edx
    snd_entchannel_info_t *channelName; // [esp+18h] [ebp-24h]
    int band; // [esp+1Ch] [ebp-20h]
    int entchannel; // [esp+20h] [ebp-1Ch]
    int eqIndex; // [esp+24h] [ebp-18h]

    Com_Printf(CON_CHANNEL_SOUND, "Current EQ Settings\n---------------\n");
    for (entchannel = 0; entchannel < g_snd.entchannel_count; ++entchannel)
    {
        channelName = SND_GetEntChannelName(entchannel);
        Com_Printf(CON_CHANNEL_SOUND, "+ %s\n", channelName->name);
        for (eqIndex = 0; eqIndex < 2; ++eqIndex)
        {
            for (band = 0; band < 3; ++band)
            {
                v0 = (float *)&alGlob.eq[eqIndex].params[band][entchannel];
                if ((uint8_t) * ((uint32_t *)v0 + 4))
                    Com_Printf(CON_CHANNEL_SOUND, "\t%i %s %f Hz %f dB %f q\n", band, snd_eqTypeStrings[*(uint32_t *)v0], v0[2], v0[1], v0[3]);
            }
        }
    }
}

float __cdecl SND_Get2DChannelVolume(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    // Unlike Miles' separate left/right levels, AL_GAIN is a single scalar regardless of
    // channel count - no srcChannelCount branching needed on this side.
    ALfloat gain;
    alGetSourcef(alGlob.source[index], AL_GAIN, &gain);
    return gain;
}

void __cdecl SND_Set2DChannelVolume(int index, float volume)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    alSourcef(alGlob.source[index], AL_GAIN, volume);
}

float __cdecl SND_Get3DChannelVolume(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    ALfloat gain;
    alGetSourcef(alGlob.source[index], AL_GAIN, &gain);
    return gain;
}

void __cdecl SND_Set3DChannelVolume(int index, float volume)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    alSourcef(alGlob.source[index], AL_GAIN, volume);
}

float __cdecl SND_GetStreamChannelVolume(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    ALfloat gain;
    alGetSourcef(alGlob.source[index], AL_GAIN, &gain);
    return gain;
}

void __cdecl SND_SetStreamChannelVolume(int index, float volume)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    alSourcef(alGlob.source[index], AL_GAIN, volume);
}

int __cdecl SND_Get2DChannelPlaybackRate(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    ALfloat pitch;
    alGetSourcef(alGlob.source[index], AL_PITCH, &pitch);
    return SnapFloatToInt(pitch * g_snd.chaninfo[index].soundFileInfo.baserate);
}

void __cdecl SND_Set2DChannelPlaybackRate(int index, int rate)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);
    int baserate = g_snd.chaninfo[index].soundFileInfo.baserate;
    const float pitch = baserate ? (float)rate / baserate : 1.0f;
    alSourcef(alGlob.source[index], AL_PITCH, pitch);
}

int __cdecl SND_Get3DChannelPlaybackRate(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    ALfloat pitch;
    alGetSourcef(alGlob.source[index], AL_PITCH, &pitch);
    return SnapFloatToInt(pitch * g_snd.chaninfo[index].soundFileInfo.baserate);
}

void __cdecl SND_Set3DChannelPlaybackRate(int index, int rate)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    int baserate = g_snd.chaninfo[index].soundFileInfo.baserate;
    const float pitch = baserate ? (float)rate / baserate : 1.0f;
    alSourcef(alGlob.source[index], AL_PITCH, pitch);
}

int __cdecl SND_GetStreamChannelPlaybackRate(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    // See SND_Get2DChannelPlaybackRate - same AL_PITCH-is-a-ratio conversion.
    ALfloat pitch;
    alGetSourcef(alGlob.source[index], AL_PITCH, &pitch);
    return SnapFloatToInt(pitch * g_snd.chaninfo[index].soundFileInfo.baserate);
}

void __cdecl SND_SetStreamChannelPlaybackRate(int index, int rate)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    int baserate = g_snd.chaninfo[index].soundFileInfo.baserate;
    const float pitch = baserate ? (float)rate / baserate : 1.0f;
    alSourcef(alGlob.source[index], AL_PITCH, pitch);
}

void __cdecl SND_Update2DChannelReverb(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    SND_ApplyReverbSend(index, g_snd.chaninfo[index].alias0);
}

void __cdecl SND_Update3DChannelReverb(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    SND_ApplyReverbSend(index, g_snd.chaninfo[index].alias0);
}

void __cdecl SND_UpdateStreamChannelReverb(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    SND_ApplyReverbSend(index, g_snd.chaninfo[index].alias0);
}

int __cdecl SND_Get2DChannelLength(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    return g_snd.chaninfo[index].totalMsec;
}

int __cdecl SND_Get3DChannelLength(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    return g_snd.chaninfo[index].totalMsec;
}

int __cdecl SND_GetStreamChannelLength(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    return g_snd.chaninfo[index].totalMsec;
}

void __cdecl SND_Get2DChannelSaveInfo(int index, snd_save_2D_sample_t *info)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    ALuint source = alGlob.source[index];
    ALfloat offsetSec = 0.0f;
    alGetSourcef(source, AL_SEC_OFFSET, &offsetSec);
    int totalMsec = g_snd.chaninfo[index].totalMsec;
    info->fraction = totalMsec > 0 ? (offsetSec * 1000.0f) / totalMsec : 0.0f;
    info->pitch = g_snd.chaninfo[index].pitch;

    ALfloat gain = 0.0f;
    alGetSourcef(source, AL_GAIN, &gain);
    if (g_snd.volume == 0.0)
        info->volume = g_snd.chaninfo[index].basevolume;
    else
        info->volume = gain / g_snd.volume;
}

void __cdecl SND_Set2DChannelFromSaveInfo(int index, snd_save_2D_sample_t *info)
{
    float volume; // [esp+4h] [ebp-4h]

    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    volume = info->volume * g_snd.volume;
    SND_Set2DChannelVolume(index, volume);
}

void __cdecl SND_Get3DChannelSaveInfo(int index, snd_save_3D_sample_t *info)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    ALuint source = alGlob.source[index];
    ALfloat offsetSec = 0.0f;
    alGetSourcef(source, AL_SEC_OFFSET, &offsetSec);
    int totalMsec = g_snd.chaninfo[index].totalMsec;
    info->fraction = totalMsec > 0 ? (offsetSec * 1000.0f) / totalMsec : 0.0f;
    info->pitch = g_snd.chaninfo[index].pitch;

    ALfloat gain = 0.0f;
    alGetSourcef(source, AL_GAIN, &gain);
    if (g_snd.volume == 0.0)
        info->volume = g_snd.chaninfo[index].basevolume;
    else
        info->volume = gain / g_snd.volume;

    alGetSource3f(source, AL_POSITION, &info->org[0], &info->org[1], &info->org[2]);
}

void __cdecl SND_GetStreamChannelSaveInfo(int index, snd_save_stream_t *info)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    SndStreamStatus status;
    SND_StreamPoll(index, &status);
    int baserate = g_snd.chaninfo[index].soundFileInfo.baserate;
    if (status.totalFrames > 0)
    {
        // Decode position: up to one queue's worth ahead of what is audible.
        info->fraction = (float)status.framesQueued / status.totalFrames;
        if (info->fraction >= 1.0f)
            info->fraction = 0.0f;
    }
    else
    {
        info->fraction = 0.0f;
    }

    ALuint source = alGlob.source[index];
    ALfloat alPitch = 1.0f;
    alGetSourcef(source, AL_PITCH, &alPitch);
    int rate = SnapFloatToInt(alPitch * baserate);
    if (g_snd.chaninfo[index].timescale)
        rate = SnapFloatToInt((float)rate / g_snd.timescale);
    info->rate = rate;

    info->basevolume = g_snd.chaninfo[index].basevolume;
    ALfloat gain = 0.0f;
    alGetSourcef(source, AL_GAIN, &gain);
    if (g_snd.volume == 0.0)
        info->volume = g_snd.chaninfo[index].basevolume;
    else
        info->volume = gain / g_snd.volume;

    float *org = g_snd.chaninfo[index].org;
    info->org[0] = org[0];
    info->org[1] = org[1];
    info->org[2] = org[2];
}

void __cdecl SND_SetStreamChannelFromSaveInfo(int index, snd_save_stream_t *info)
{
    float volume; // [esp+4h] [ebp-4h]

    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    volume = info->volume * g_snd.volume;
    SND_SetStreamChannelVolume(index, volume);
}

int __cdecl SND_GetSoundFileSize(uint32_t *pSoundFile)
{
    iassert(pSoundFile);

    return pSoundFile[2];
}

// snd_debugStreams (port diagnostic, off by default): once a second, every
// busy stream channel with the terms its AL gain is the product of, the AL
// source state, and the entchannel voice bookkeeping.  A dialogue line that is
// decoded (SWITCH_PERF sndstream fills) but inaudible shows up here as a zero
// term, a stopped/paused source, or an entchannel pinned at maxVoices.
static void SND_DebugStreams()
{
    static int s_lastMs;
    if (!snd_debugStreams || !snd_debugStreams->current.enabled)
        return;
    const int now = Sys_Milliseconds();
    if (now - s_lastMs < 1000)
        return;
    s_lastMs = now;
    int group = 0;
    for (int i = 0; i < SND_CHANNELVOLPRIO_COUNT; ++i)
        if (g_snd.channelvol == &g_snd.channelVolGroups[i])
            group = i;
#ifdef __SWITCH__
    const char *backend = "audren";
#else
    const char *backend = "openal";
#endif
    Com_Printf(9, "SND_STREAMDBG t=%d backend=%s master=%.2f slaveLerp=%.2f volgroup=%d paused=%d listener=%.0f,%.0f,%.0f\n",
               g_snd.time, backend, g_snd.volume, g_snd.slaveLerp, group, g_snd.paused ? 1 : 0,
               g_snd.listeners[0].orient.origin[0], g_snd.listeners[0].orient.origin[1],
               g_snd.listeners[0].orient.origin[2]);
    for (int i = SND_FIRST_STREAM_CHANNEL; i < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels; ++i)
    {
        const snd_channel_info_t *ci = &g_snd.chaninfo[i];
        if (!ci->alias0)
            continue;
        const int entchannel = SNDALIASFLAGS_GET_CHANNEL(ci->alias0->flags);
        const bool is3d = SND_IsAliasChannel3D(entchannel);
        float dist = -1.0f, falloff = 1.0f;
        if (is3d && ci->alias1)
        {
            float diff[3];
            const int listenerIndex = SND_GetListenerIndexNearestToOrigin(ci->org);
            Vec3Sub(g_snd.listeners[listenerIndex].orient.origin, ci->org, diff);
            dist = Vec3Length(diff);
            falloff = (float)SND_GetStream3DVolumeFallOff(i, listenerIndex);
        }
        ALint state = 0, queued = 0;
        ALfloat gain = -1.0f;
        alGetSourcei(alGlob.source[i], AL_SOURCE_STATE, &state);
        alGetSourcei(alGlob.source[i], AL_BUFFERS_QUEUED, &queued);
        alGetSourcef(alGlob.source[i], AL_GAIN, &gain);
        SndStreamStatus status;
        SND_StreamPoll(i, &status);
        Com_Printf(9, "SND_STREAMDBG ch=%d alias=%s ent=%u entchan=%s 3d=%d base=%.2f chanvol=%.2f falloff=%.2f dist=%.0f "
                      "al_gain=%.3f al_state=%s queued=%d stream=%d paused=%d delay=%d master=%d voices=%d/%d\n",
                   i, ci->alias0->aliasName ? ci->alias0->aliasName : "?", ci->sndEnt.field.entIndex,
                   g_snd.entchaninfo[entchannel].name, is3d ? 1 : 0, ci->basevolume,
                   g_snd.channelvol->channelvol[entchannel].volume, falloff, dist, gain,
                   state == AL_PLAYING ? "playing" : state == AL_PAUSED ? "paused" : state == AL_STOPPED ? "stopped" : "initial",
                   queued, (int)status.state, ci->paused ? 1 : 0, ci->startDelay, ci->master ? 1 : 0,
                   g_snd.entchaninfo[entchannel].voiceCount, g_snd.entchaninfo[entchannel].maxVoices);
    }
    // Voice bookkeeping: SND_HasFreeVoice refuses an entchannel once
    // voiceCount reaches maxVoices, so a count above the channels actually
    // holding that entchannel is a leak that silences it for good.
    int busy[64] = {};
    for (int i = 0; i < SND_MAX_CHANNELS; ++i)
    {
        const snd_channel_info_t *ci = &g_snd.chaninfo[i];
        if (ci->alias0 && ci->entchannel >= 0 && ci->entchannel < 64)
            ++busy[ci->entchannel];
    }
    for (int e = 0; e < g_snd.entchannel_count && e < 64; ++e)
    {
        const snd_entchannel_info_t *info = &g_snd.entchaninfo[e];
        if (info->voiceCount != busy[e] || info->voiceCount >= info->maxVoices)
            Com_Printf(9, "SND_VOICEDBG entchan=%s voices=%d busy=%d max=%d%s\n", info->name, info->voiceCount, busy[e],
                       info->maxVoices, info->voiceCount > busy[e] ? " LEAK" : "");
    }
}

#ifdef __SWITCH__
// SWITCH_PERF sndmix (switch_perfTrace): CPU time the audren backend's update
// thread spent since the last window, once a second (busy is the update
// thread's audrvUpdate cost -- IPC to the renderer, wave-buffer bookkeeping;
// the mix itself runs on the audio DSP, whose voice drops show on the extra
// fields).  busy_pct is of one core.
static void SND_MixEmitPerf()
{
    static uint64_t s_lastBusyUs;
    static uint32_t s_lastCalls;
    static uint64_t s_lastPrintTick;
    static bool s_primed;
    const uint64_t now = SwitchPerf_NowTicks();
    uint64_t busy = 0;
    uint32_t calls = 0;
    SND_AudrenEmitPerf(&busy, &calls);
    if (!s_primed)
    {
        s_primed = true;
        s_lastBusyUs = busy;
        s_lastCalls = calls;
        s_lastPrintTick = now;
        return;
    }
    const uint64_t windowUs = SwitchPerf_TicksToUs(now - s_lastPrintTick);
    if (windowUs < 1000000ull)
        return;
    const uint64_t busyUs = busy - s_lastBusyUs;
    char extra[512] = "";
    SND_AudrenEvidence(extra, sizeof(extra));
    Com_Printf(16, "SWITCH_PERF sndmix backend=audren busy_us=%llu window_us=%llu busy_pct=%.2f calls=%u us_per_call=%.1f%s%s\n",
               (unsigned long long)busyUs, (unsigned long long)windowUs,
               windowUs ? 100.0 * (double)busyUs / (double)windowUs : 0.0, calls - s_lastCalls,
               calls != s_lastCalls ? (double)busyUs / (double)(calls - s_lastCalls) : 0.0, extra[0] ? " " : "", extra);
    s_lastBusyUs = busy;
    s_lastCalls = calls;
    s_lastPrintTick = now;
}
#endif

void __cdecl SND_DriverPostUpdate()
{
#ifndef KISAK_XBOX
    SND_UpdateEqs();
#endif
    SND_DebugStreams();
#ifdef __SWITCH__
    if (SwitchPerf_g_enabled)
        SND_MixEmitPerf();
#endif
    KISAK_NULLSUB();
}

void __cdecl SND_Update2DChannel(int i, int frametime)
{
    float v2; // [esp+4h] [ebp-18h]
    float volume; // [esp+8h] [ebp-14h]
    float volumea; // [esp+8h] [ebp-14h]
    const snd_alias_t *alias1; // [esp+Ch] [ebp-10h]
    const snd_alias_t *alias0; // [esp+10h] [ebp-Ch]
    snd_channel_info_t *chaninfo; // [esp+18h] [ebp-4h]

    iassert(i >= 0 && i < 0 + g_snd.max_2D_channels);

    chaninfo = &g_snd.chaninfo[i];
    if (!chaninfo->paused)
    {
        alias0 = chaninfo->alias0;
        alias1 = chaninfo->alias1;
        iassert(alias0);
        iassert(alias1);

        volume = chaninfo->basevolume;

        ALint state;
        alGetSourcei(alGlob.source[i], AL_SOURCE_STATE, &state);
        bool finishedOrChaining = (!chaninfo->startDelay && state == AL_STOPPED)
            || (alias0->chainAliasName && chaninfo->totalMsec + chaninfo->startTime - g_snd.time <= 0);
        if (finishedOrChaining)
        {
            SND_StopChannelAndPlayChainAlias(i);
        }
        else
        {
            if (g_snd.slaveLerp != 0.0 && !g_snd.chaninfo[i].master && (alias0->flags & 4) != 0)
                volume = SND_GetLerpedSlavePercentage(alias0->slavePercentage) * volume;
            iassert(SNDALIASFLAGS_GET_CHANNEL(alias0->flags) < 64);
            volumea = volume * g_snd.channelvol->channelvol[(alias0->flags & 0x3F00) >> 8].volume;
            v2 = volumea * g_snd.volume;
            SND_Set2DChannelVolume(i, v2);
            MSS_ResumeSample(i, frametime);
        }
    }
}

void __cdecl SND_Update3DChannel(int i, int frametime)
{
    double v2; // st7
    double LerpedSlavePercentage; // st7
    float v4; // [esp+Ch] [ebp-48h]
    float radius; // [esp+10h] [ebp-44h]
    snd_listener *a; // [esp+14h] [ebp-40h]
    float diff[3]; // [esp+1Ch] [ebp-38h] BYREF
    float volume; // [esp+28h] [ebp-2Ch]
    float lerp; // [esp+2Ch] [ebp-28h]
    const snd_alias_t *alias1; // [esp+30h] [ebp-24h]
    float distMin; // [esp+34h] [ebp-20h]
    const snd_alias_t *alias0; // [esp+38h] [ebp-1Ch]
    float org[3]; // [esp+3Ch] [ebp-18h] BYREF
    int timeleft; // [esp+48h] [ebp-Ch]
    float distMax; // [esp+4Ch] [ebp-8h]
    snd_channel_info_t *chaninfo; // [esp+50h] [ebp-4h]

    iassert(i >= (0 + 8) && i < (0 + 8) + g_snd.max_3D_channels);
    chaninfo = &g_snd.chaninfo[i];
    if (!chaninfo->paused)
    {
        alias0 = chaninfo->alias0;
        alias1 = chaninfo->alias1;
        iassert(alias0);
        iassert(alias1);
        lerp = chaninfo->lerp;
        volume = chaninfo->basevolume;
        timeleft = chaninfo->totalMsec + chaninfo->startTime - g_snd.time;
        ALint state;
        alGetSourcei(alGlob.source[i], AL_SOURCE_STATE, &state);
        bool finishedOrChaining = (!chaninfo->startDelay && state == AL_STOPPED)
            || (alias0->chainAliasName && timeleft <= 0);
        if (finishedOrChaining)
        {
            SND_StopChannelAndPlayChainAlias(i);
        }
        else
        {
            SND_GetCurrent3DPosition(g_snd.chaninfo[i].sndEnt, g_snd.chaninfo[i].offset, org);
            SND_Set3DPosition(i, org);
            distMin = (1.0 - lerp) * alias0->distMin + alias1->distMin * lerp;
            distMax = (1.0 - lerp) * alias0->distMax + alias1->distMax * lerp;
            a = &g_snd.listeners[SND_GetListenerIndexNearestToOrigin(org)];
            Vec3Sub(a->orient.origin, org, diff);
            radius = Vec3Length(diff);
            v2 = SND_Attenuate(alias0->volumeFalloffCurve, radius, distMin, distMax);
            volume = v2 * volume;
            if (g_snd.slaveLerp != 0.0 && !g_snd.chaninfo[i].master && (alias0->flags & 4) != 0)
            {
                LerpedSlavePercentage = SND_GetLerpedSlavePercentage(alias0->slavePercentage);
                volume = LerpedSlavePercentage * volume;
            }
            iassert(SNDALIASFLAGS_GET_CHANNEL(alias0->flags) < 64);
            volume = volume * g_snd.channelvol->channelvol[(alias0->flags & 0x3F00) >> 8].volume;
            v4 = volume * g_snd.volume;
            SND_Set3DChannelVolume(i, v4);
            MSS_ResumeSample(i, frametime);
        }
    }
}

void __cdecl SND_UpdateStreamChannel(int i, int frametime)
{
    int v2; // [esp+4h] [ebp-1Ch]
    float volume; // [esp+Ch] [ebp-14h]
    float volumea; // [esp+Ch] [ebp-14h]
    float volumeb; // [esp+Ch] [ebp-14h]
    int listenerIndex; // [esp+10h] [ebp-10h]
    const snd_alias_t *alias1; // [esp+14h] [ebp-Ch]
    const snd_alias_t *alias0; // [esp+18h] [ebp-8h]
    snd_channel_info_t *chaninfo; // [esp+1Ch] [ebp-4h]

    iassert(i >= ((0 + 8) + 32) && i < ((0 + 8) + 32) + g_snd.max_stream_channels);

    chaninfo = &g_snd.chaninfo[i];
    if (!chaninfo->paused && (i >= 45 || SND_UpdateBackgroundVolume(i - 40, frametime)))
    {
        alias0 = chaninfo->alias0;
        alias1 = chaninfo->alias1;
        iassert(alias0);
        iassert(alias1);
        volume = chaninfo->basevolume;
        SndStreamStatus status;
        SND_StreamPoll(i, &status);
        if (status.state == SND_STREAM_FAILED)
        {
            Com_PrintError(9, "Stream for alias '%s' failed to decode; stopping it\n", alias0->aliasName);
            snd_stopReason = "stream_failed";
            SND_StopChannelAndPlayChainAlias(i);
            return;
        }
        bool *lengthPending = &s_streamLengthPending[i - SND_FIRST_STREAM_CHANNEL];
        if (*lengthPending && (status.state == SND_STREAM_PLAYING || status.state == SND_STREAM_ENDED))
        {
            // The stream thread counted an MP3's frames: fill in retail's
            // unknown length and deliver the length notifies it queued.
            *lengthPending = false;
            const int baserate = chaninfo->soundFileInfo.baserate;
            const float pitch = chaninfo->pitch > 0.0f ? chaninfo->pitch : 1.0f;
            if (baserate > 0 && status.totalFrames > 0)
            {
                const int totalMsec = (int)((double)(status.totalFrames - status.startFrame) * 1000.0 / (baserate * pitch));
                chaninfo->soundFileInfo.endtime = chaninfo->startTime + totalMsec;
                SND_SetKnownLength(i, totalMsec + chaninfo->startDelay);
            }
            else
            {
                SND_SetKnownLength(i, 0);
            }
        }
        // Loading counts as playing; the stream thread reports the end once a
        // non-looping stream has fully played out.
        bool stillPlaying = g_snd.chaninfo[i].startDelay || status.state != SND_STREAM_ENDED;
        if (stillPlaying)
        {
            if (SND_IsAliasChannel3D(SNDALIASFLAGS_GET_CHANNEL(alias0->flags)))
            {
                SND_GetCurrent3DPosition(g_snd.chaninfo[i].sndEnt, g_snd.chaninfo[i].offset, g_snd.chaninfo[i].org);
                listenerIndex = SND_GetListenerIndexNearestToOrigin(g_snd.chaninfo[i].org);
                SND_Set3DStreamPosition(i, listenerIndex, g_snd.chaninfo[i].org);
                volume = SND_GetStream3DVolumeFallOff(i, listenerIndex) * volume;
            }
            if (g_snd.slaveLerp != 0.0 && !g_snd.chaninfo[i].master && (alias0->flags & 4) != 0)
                volume = SND_GetLerpedSlavePercentage(alias0->slavePercentage) * volume;

            iassert(SNDALIASFLAGS_GET_CHANNEL(alias0->flags) < 64);

            volumea = volume * g_snd.channelvol->channelvol[(alias0->flags & 0x3F00) >> 8].volume;
            volumeb = volumea * g_snd.volume;
            SND_SetStreamChannelVolume(i, volumeb);
            if (g_snd.chaninfo[i].startDelay)
            {
                if (g_snd.chaninfo[i].startDelay - frametime > 0)
                    v2 = g_snd.chaninfo[i].startDelay - frametime;
                else
                    v2 = 0;
                g_snd.chaninfo[i].startDelay = v2;
                if (!g_snd.chaninfo[i].startDelay)
                    SND_StreamSetWantPlay(i, true);
            }
        }
        else
        {
            snd_stopReason = "stream_ended";
            SND_StopChannelAndPlayChainAlias(i);
        }
    }
}

void __cdecl SND_SetHWND(HWND hwnd)
{
    // OpenAL has no window-handle dependency (no DirectSound backend to bind).
}

#ifdef __SWITCH__
// embedded LoadedSound payload retention for the retail zone walk (see
// RetailWalkRetainLoadedSoundData in db_retail_walk.h).  The payload sits in
// the zone's rewinding block 0 while the alias list is walked, so it is
// copied into sound memory here, at walk time.  Wave format 17 (IMA ADPCM,
// the form most retail loaded sounds ship in) is decoded to 16-bit PCM
// because the OpenAL playback path queues raw PCM16 buffers; format 1 (PCM)
// is copied as-is.  No resampling: SND_Init has not necessarily run yet
// (boot zones load first) and OpenAL resamples at mix time anyway.
namespace
{
const int kImaIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kImaStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};

uint32_t s_retained, s_retainedBytes, s_adpcm, s_rejected;
// Wall time spent inside retail walk-time sound retention (copy + IMA
// decode). Shown on the OpenAL evidence line so the load profile can
// attribute the sound decode cost without a separate run.
uint64_t s_retainedMs;

// Nibble-delta table: delta[nibble][stepIndex] is the exact value the original
// four bit-tests + shifts produced. One indexed load replaces the branch chain
// and the step-table load; the predictor chain is serial by definition, so the
// win is removing per-sample branches, not SIMD (which cannot break the
// predictor dependency).
struct ImaDeltaTable
{
    int32_t delta[16][89];

    ImaDeltaTable()
    {
        for (int nibble = 0; nibble < 16; ++nibble)
        {
            for (int index = 0; index < 89; ++index)
            {
                const int step = kImaStepTable[index];
                int diff = step >> 3;
                if (nibble & 1)
                    diff += step >> 2;
                if (nibble & 2)
                    diff += step >> 1;
                if (nibble & 4)
                    diff += step;
                if (nibble & 8)
                    diff = -diff;
                delta[nibble][index] = diff;
            }
        }
    }
};

const ImaDeltaTable &ImaDeltas()
{
    static const ImaDeltaTable table;
    return table;
}

inline int32_t ImaClampPredictor(int32_t predictor)
{
    if (predictor > 32767)
        return 32767;
    if (predictor < -32768)
        return -32768;
    return predictor;
}

inline int32_t ImaClampIndex(int32_t index)
{
    if (index < 0)
        return 0;
    if (index > 88)
        return 88;
    return index;
}

// Microsoft IMA ADPCM block layout (WAVE_FORMAT_IMA_ADPCM): per channel a
// 4-byte header (int16 predictor, uint8 step index, reserved), then 4-byte
// words of 8 nibbles interleaved per channel. Mono and stereo get their own
// unrolled loops (the original channel loop recomputed the channel stride and
// byte address per nibble).
uint32_t ImaDecodeBlocks(const uint8_t *src, uint32_t srcLen, uint32_t blockSize, uint32_t channels,
                         int16_t *dst, uint32_t dstFrames)
{
    if (channels < 1 || channels > 2 || blockSize < 4u * channels)
        return 0;
    const int32_t (*delta)[89] = ImaDeltas().delta;
    uint32_t frame = 0;
    for (uint32_t off = 0; off + 4u * channels <= srcLen && frame < dstFrames; off += blockSize)
    {
        const uint8_t *blk = src + off;
        const uint32_t avail = srcLen - off < blockSize ? srcLen - off : blockSize;
        int32_t predictor0 = static_cast<int16_t>(blk[0] | (blk[1] << 8));
        int32_t index0 = blk[2];
        if (index0 > 88)
            index0 = 88;
        int32_t predictor1 = 0;
        int32_t index1 = 0;
        if (channels == 2)
        {
            predictor1 = static_cast<int16_t>(blk[4] | (blk[5] << 8));
            index1 = blk[6];
            if (index1 > 88)
                index1 = 88;
        }
        dst[frame * channels] = static_cast<int16_t>(predictor0);
        if (channels == 2)
            dst[frame * channels + 1] = static_cast<int16_t>(predictor1);
        ++frame;

        if (channels == 1)
        {
            uint32_t pos = 4;
            while (pos + 4 <= avail && frame < dstFrames)
            {
                for (uint32_t n = 0; n < 8 && frame + n < dstFrames; ++n)
                {
                    const uint8_t byte = blk[pos + (n >> 1)];
                    const uint32_t nibble = (n & 1) ? (byte >> 4) : (byte & 0xF);
                    predictor0 = ImaClampPredictor(predictor0 + delta[nibble][index0]);
                    index0 = ImaClampIndex(index0 + kImaIndexTable[nibble]);
                    dst[frame + n] = static_cast<int16_t>(predictor0);
                }
                frame += 8;
                pos += 4;
            }
        }
        else
        {
            uint32_t pos = 8;
            while (pos + 8 <= avail && frame < dstFrames)
            {
                for (uint32_t n = 0; n < 8 && frame + n < dstFrames; ++n)
                {
                    const uint8_t byte0 = blk[pos + (n >> 1)];
                    const uint8_t byte1 = blk[pos + 4 + (n >> 1)];
                    const uint32_t nibble0 = (n & 1) ? (byte0 >> 4) : (byte0 & 0xF);
                    const uint32_t nibble1 = (n & 1) ? (byte1 >> 4) : (byte1 & 0xF);
                    predictor0 = ImaClampPredictor(predictor0 + delta[nibble0][index0]);
                    index0 = ImaClampIndex(index0 + kImaIndexTable[nibble0]);
                    predictor1 = ImaClampPredictor(predictor1 + delta[nibble1][index1]);
                    index1 = ImaClampIndex(index1 + kImaIndexTable[nibble1]);
                    dst[(frame + n) * 2] = static_cast<int16_t>(predictor0);
                    dst[(frame + n) * 2 + 1] = static_cast<int16_t>(predictor1);
                }
                frame += 8;
                pos += 8;
            }
        }
    }
    return frame < dstFrames ? frame : dstFrames;
}
} // namespace

uint32_t SND_AlRetainedCount() { return s_retained; }
uint32_t SND_AlRetainedBytes() { return s_retainedBytes; }
uint32_t SND_AlRetainedAdpcm() { return s_adpcm; }
uint32_t SND_AlRetainedRejected() { return s_rejected; }
uint64_t SND_AlRetainedMs() { return s_retainedMs; }

uint8_t *RetailWalkRetainLoadedSoundData(const uint8_t *root44, const uint8_t *data,
                                         uint32_t dataLen, uint32_t *outLen,
                                         uint32_t *outFormat, uint32_t *outBits)
{
    auto le32 = [](const uint8_t *p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    };
    const uint32_t format = le32(root44 + 4);
    const uint32_t rate = le32(root44 + 16);
    const uint32_t bits = le32(root44 + 20);
    const uint32_t channels = le32(root44 + 24);
    const uint32_t samples = le32(root44 + 28);
    const uint32_t blockSize = le32(root44 + 32);
    const uint32_t retainStart = static_cast<uint32_t>(Sys_Milliseconds());
    if (format == 17 && channels >= 1 && channels <= 2 && samples)
    {
        const uint32_t pcmLen = samples * channels * sizeof(int16_t);
        uint8_t *pcm = MSS_Alloc(pcmLen, rate);
        if (!pcm)
            return nullptr;
        const uint32_t frames = ImaDecodeBlocks(data, dataLen, blockSize, channels,
                                                reinterpret_cast<int16_t *>(pcm), samples);
        if (frames < samples)
            std::memset(pcm + frames * channels * sizeof(int16_t), 0, (samples - frames) * channels * sizeof(int16_t));
        *outLen = pcmLen;
        *outFormat = 1;
        *outBits = 16;
        ++s_retained; ++s_adpcm; s_retainedBytes += pcmLen;
        s_retainedMs += static_cast<uint32_t>(Sys_Milliseconds()) - retainStart;
        return pcm;
    }
    if (format == 1 && (bits == 8 || bits == 16) && channels >= 1 && channels <= 2)
    {
        uint8_t *pcm = MSS_Alloc(dataLen, rate);
        if (!pcm)
            return nullptr;
        std::memcpy(pcm, data, dataLen);
        *outLen = dataLen;
        *outFormat = 1;
        *outBits = bits;
        ++s_retained; s_retainedBytes += dataLen;
        s_retainedMs += static_cast<uint32_t>(Sys_Milliseconds()) - retainStart;
        return pcm;
    }
    if (++s_rejected <= 8)
        Com_Printf(0, "RetailWalkRetainLoadedSoundData: unsupported format=%u bits=%u channels=%u\n",
                   format, bits, channels);
    return nullptr;
}
#endif

void __cdecl SND_SetData(MssSoundCOD4 *mssSound, void *srcData)
{
    // dr_wav (see SND_LoadFromBuffer, snd_driver_load_obj.cpp) always decodes to 16-bit PCM
    // for us, so unlike the Miles branch above there's no ADPCM format to worry about here.
    if (mssSound->info.rate > g_snd.playback_rate)
    {
        // Resample down to g_snd.playback_rate via simple decimation (nearest-frame
        // resample), matching the halving loop in the Miles branch above. A real
        // low-pass-filtered resample would sound better, but this matches WORK.md Phase 3's
        // stated scope - revisit if downsampled loaded sounds turn out to sound too aliased.
        uint32_t srcFrameCount = mssSound->info.samples;
        uint32_t channels = mssSound->info.channels;
        uint32_t rate = mssSound->info.rate;
        uint32_t frameCount = srcFrameCount;

        while (rate > g_snd.playback_rate)
        {
            rate /= 2;
            frameCount /= 2;
        }

        uint32_t newDataLen = frameCount * channels * sizeof(int16_t);
        mssSound->data = MSS_Alloc(newDataLen, rate);

        const int16_t *src16 = (const int16_t *)srcData;
        int16_t *dst16 = (int16_t *)mssSound->data;
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            uint32_t srcFrame = (uint32_t)((uint64_t)i * srcFrameCount / frameCount);
            for (uint32_t c = 0; c < channels; ++c)
                dst16[i * channels + c] = src16[srcFrame * channels + c];
        }

        mssSound->info.rate = rate;
        mssSound->info.samples = frameCount;
        mssSound->info.data_len = newDataLen;
    }
    else
    {
        mssSound->data = MSS_Alloc(mssSound->info.data_len, mssSound->info.rate);
        Com_Memcpy(mssSound->data, srcData, mssSound->info.data_len);
    }

    mssSound->info.data_ptr = mssSound->data;
    mssSound->info.initial_ptr = mssSound->data;
}

#ifdef KISAK_SP
void SND_SetEqLerp(float lerp)
{
    if (lerp < 0.0 || lerp > 1.0)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\xenon\\snd_driver.cpp",
            1740,
            0,
            "%s\n\t(lerp) = %g",
            HIDWORD(lerp),
            LODWORD(lerp));
#if KISAK_XBOX
    xaGlob.eqLerp = lerp;
#else
    alGlob.eqLerp = (float)lerp;
    SND_UpdateEqs();
#endif
}
#endif











// ─────────────────────────────────────────────────────────────────────────
// Cinematic (Bink movie) audio: a push-PCM 2D stream.
//
// The Switch FFmpeg backend decodes the movie's BinkAudio track itself (the
// retail Bink+Miles sound system is Win32-only) and feeds it here.  A movie
// has no sound alias, so this deliberately bypasses the alias channel
// machinery: one dedicated source, an 8-buffer queue, and the backend's own
// volume (R_Cinematic_StartPlayback's playbackVolume).
// ─────────────────────────────────────────────────────────────────────────
static const int AL_CINEMATIC_BUFFER_COUNT = 8;
static const int AL_CINEMATIC_CHUNK_FRAMES = 8192;

bool __cdecl SND_StartCinematicStream(int sampleRate, int channels)
{
    if (!alGlob.cinematicSource || sampleRate <= 0 || (channels != 1 && channels != 2))
        return false;
    SND_StopCinematicStream();
    alGlob.cinematicRate = sampleRate;
    alGlob.cinematicChannels = channels;
    alGlob.cinematicActive = true;
    alSourcei(alGlob.cinematicSource, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(alGlob.cinematicSource, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcef(alGlob.cinematicSource, AL_PITCH, 1.0f);
    alSourcef(alGlob.cinematicSource, AL_GAIN, 1.0f);
    alSourcei(alGlob.cinematicSource, AL_LOOPING, AL_FALSE);
    return true;
}

void __cdecl SND_SetCinematicStreamVolume(float volume)
{
    if (!alGlob.cinematicSource)
        return;
    if (volume < 0.0f)
        volume = 0.0f;
    if (volume > 1.0f)
        volume = 1.0f;
    alSourcef(alGlob.cinematicSource, AL_GAIN, volume);
}

int __cdecl SND_PushCinematicPCM(const int16_t *frames, int frameCount)
{
    if (!alGlob.cinematicActive || !alGlob.cinematicSource || !frames || frameCount <= 0)
        return 0;

    ALuint source = alGlob.cinematicSource;
    ALint processed = 0;
    alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0)
    {
        ALuint buffer;
        alSourceUnqueueBuffers(source, 1, &buffer);
        alDeleteBuffers(1, &buffer);
    }

    ALint queued = 0;
    alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
    const int bytesPerFrame = alGlob.cinematicChannels * (int)sizeof(int16_t);
    const ALenum format = (alGlob.cinematicChannels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
    int offset = 0;
    while (offset < frameCount && queued < AL_CINEMATIC_BUFFER_COUNT)
    {
        int chunk = frameCount - offset;
        if (chunk > AL_CINEMATIC_CHUNK_FRAMES)
            chunk = AL_CINEMATIC_CHUNK_FRAMES;
        ALuint buffer;
        alGenBuffers(1, &buffer);
        alBufferData(buffer, format, frames + (size_t)offset * alGlob.cinematicChannels,
                     chunk * bytesPerFrame, alGlob.cinematicRate);
        alSourceQueueBuffers(source, 1, &buffer);
        offset += chunk;
        ++queued;
    }

    ALint state = 0;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING && queued > 0)
        alSourcePlay(source);
    SND_AlNotePlay(2);
    return offset;
}

void __cdecl SND_PauseCinematicStream(bool paused)
{
    if (!alGlob.cinematicSource)
        return;
    if (paused)
    {
        alSourcePause(alGlob.cinematicSource);
    }
    else
    {
        ALint state = 0;
        alGetSourcei(alGlob.cinematicSource, AL_SOURCE_STATE, &state);
        if (state == AL_PAUSED)
            alSourcePlay(alGlob.cinematicSource);
    }
}

void __cdecl SND_StopCinematicStream()
{
    if (!alGlob.cinematicSource)
        return;
    alSourceStop(alGlob.cinematicSource);
    ALint queued = 0;
    alGetSourcei(alGlob.cinematicSource, AL_BUFFERS_QUEUED, &queued);
    while (queued-- > 0)
    {
        ALuint buffer;
        alSourceUnqueueBuffers(alGlob.cinematicSource, 1, &buffer);
        alDeleteBuffers(1, &buffer);
    }
    alGlob.cinematicActive = false;
}

#endif