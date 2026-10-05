// Switch cinematic video backend: FFmpeg Bink decode over the retail
// R_Cinematic_* interface.
//
// Policy: the licensed Bink SDK is never ported; Bink
// video/BinkAudio are decoded through FFmpeg behind this narrow shim.  There
// is no hardware Bink decoder on Tegra (NVDEC does not cover Bink), so decode
// is software and the GPU only receives the Y/Cr/Cb plane uploads.
//
// This replaces switch_cinematic_stubs.cpp on Horizon.  The behavior mirrors
// r_cinematic.cpp's CINEMA build with the worker-thread/background-IO
// handshake removed: UpdateFrame decodes and uploads at most a bounded number
// of frames per rendered frame, skipping late frames the way Bink did, and
// publishes the current frame through the same
// TEXTURE_SRC_CODE_CINEMATIC_* code images the retail `cinematic` material
// samples.  The movie's BinkAudio track is decoded up front by
// SwitchCinematicAudioDecoder and pushed to the sound system's dedicated
// cinematic stream (SND_*CinematicStream in snd_public.h) as the wall clock
// advances, so audio and video stay locked without a second decoder thread.
#include <universal/q_shared.h>
#include <sound/snd_public.h>
#include <platform/switch/switch_platform.h>
#include <qcommon/qcommon.h>
#include "gfx_d3d/r_cinematic.h"
#include "switch_cinematic_plane.h"
#include "gfx_d3d/rb_state.h"
#include "gfx_d3d/r_init.h"
#include "gfx_d3d/r_image.h"
#include "gfx_d3d/r_rendercmds.h"
#include "gfx_d3d/r_state.h"
#include <qcommon/threads.h>
#include <database/database.h>
#include <database/db_retail_frame_evidence.h>
#include <platform/switch/switch_diag_dvars.h>
#include "switch_cinematic_decode.h"

#ifdef __SWITCH__

CinematicGlob cinematicGlob;

// Must be declared at global scope: an extern inside the anonymous namespace
// below would get internal linkage and fail to resolve the engine's dvar.
extern const dvar_t *com_diagMarkers;

namespace
{
// Playback flag bits (r_cinematic.cpp call sites): 0x1 no background IO,
// 0x2 loop, 0x4 high priority, 0x8 memory resident, 0x20 fastfile rawfile.
constexpr uint32_t CINE_FLAG_LOOP = 0x2;

// Frames decoded per UpdateFrame before yielding the CPU back to the load.
// Bink skipped late frames; we decode through them without uploading so the
// movie stays on the wall clock even when the level load is saturating the
// cores.
constexpr uint32_t CINE_MAX_DECODE_PER_UPDATE = 6;

// How far ahead of the movie's wall clock the BinkAudio track is queued on the
// sound system's cinematic stream.  It must stay below that stream's queue
// depth (8 x 8192 frames = ~1.36 s at 48 kHz) so a full queue is not the
// steady state, and be large enough to ride out one slow UpdateFrame.
constexpr int64_t CINE_AUDIO_LEAD_MS = 500;

// Frames decoded per audio push; the stream chunks them itself.
constexpr int CINE_AUDIO_PUSH_FRAMES = 16384;

struct CinematicTexture
{
    GfxImage source; // IMG_FLAG_SYSTEMMEM, written by the CPU
    GfxImage draw;   // IMG_FLAG_DYNAMIC, sampled by the cinematic material
    bool created;
};

struct CinematicPlayer
{
    SwitchCinematicVideoDecoder decoder;
    SwitchCinematicAudioDecoder audio;
    bool open;
    bool eof;
    bool haveFrame;
    bool underrun;
    bool texturesCreated;
    bool audioOpen;         // a BinkAudio track was decoded and its stream started
    bool audioPaused;       // mirrors the cinematic stream's pause state
    int64_t audioCursor;    // frames already queued on the cinematic stream
    int16_t audioCarry[CINE_AUDIO_PUSH_FRAMES * 2]; // decoded, not yet queued
    int audioCarryFrames;
    int audioCarryOffset;
    uint32_t width;
    uint32_t height;
    uint32_t chromaWidth;
    uint32_t chromaHeight;
    uint32_t playStartMs;   // adjusted when playback is resumed
    uint32_t pausedAtMs;    // 0 while running
    int64_t lastDecodedMs;  // pts of the newest decoded frame
    int64_t lastUploadedMs; // pts of the frame the code images point at
    uint32_t frameCount;    // decoded this playback
    uint32_t uploadedFrames;
    uint32_t skippedFrames;
    uint32_t openFailures;
    uint64_t decodeMs;      // accumulated this playback (hardware decode budget)
    uint64_t uploadMs;
    CinematicTexture y;
    CinematicTexture cr;
    CinematicTexture cb;
};

CinematicPlayer s_player;
bool s_initialized;
uint32_t s_lastReportMs;

bool CinematicDiagEnabled()
{
    return com_diagMarkers && com_diagMarkers->current.enabled;
}

void CinematicDiag(const char *fmt, ...)
{
    if (!CinematicDiagEnabled())
        return;
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    Com_Printf(16, "SWITCH_CINEMATIC %s\n", text);
}

// Resolves a cinematic name to a loose .bik path.  The PC path (with flags
// 0x20) opens a fastfile rawfile in memory; the retail videos are installed
// as loose main/video files under the retail root (with the rest of the game
// data) and the fastfile-flag callers (GSC cinematicingameloopfromfastfile)
// name the same assets, so the loose file is the single source here.
bool CinematicResolvePath(const char *name, char *outPath, size_t outSize)
{
    char normalized[264];
    size_t write = 0;
    for (size_t read = 0; name[read] && write + 1 < sizeof(normalized); ++read)
    {
        char c = name[read];
        if (c == '\\')
            c = '/';
        if (c == ':' || (c == '/' && write == 0))
            continue; // never let an asset name escape to an absolute path
        normalized[write++] = c;
    }
    normalized[write] = 0;
    if (write == 0)
        return false;

    const char *root = Sys_DefaultInstallPath();
    snprintf(outPath, outSize, "%s/main/video/%s.bik", root, normalized);
    if (FILE *probe = fopen(outPath, "rb"))
    {
        fclose(probe);
        return true;
    }
    snprintf(outPath, outSize, "%s/raw/video/%s.bik", root, normalized);
    if (FILE *probe = fopen(outPath, "rb"))
    {
        fclose(probe);
        return true;
    }
    return false;
}

void CinematicMakeImage(GfxImage *image, const char *name, int width, int height, int flags)
{
    memset(image, 0, sizeof(*image));
    image->name = name;
    Image_Setup(image, width, height, 1, flags | IMG_FLAG_NOPICMIP | IMG_FLAG_NOMIPMAPS, D3DFMT_L8);
}

void CinematicReleaseTexture(CinematicTexture &texture)
{
    if (!texture.created)
        return;
    // The back end's sampler cache keys on the GfxImage (&image->texture),
    // and this player re-creates its textures inside the same structs: a
    // rebuild in place (CinematicCreateTextures, a new movie while the
    // previous set is retiring) left the cache saying the image was still
    // bound while the device had forgotten the freed texture, so the first
    // frame of the next movie sampled destroyed textures
    // (FAIL:DEKO9_TEXTURE_BIND samplers 4-6 at cargoship_fade, with and
    // without r_smp_backend). Retail's R_UnbindImage drops the stale
    // binding; this runs on the thread that owns gfxCmdBufState (the
    // update is RB_BeginFrame's).
    R_UnbindImage(&gfxCmdBufState, &texture.draw);
    Image_Release(&texture.source);
    Image_Release(&texture.draw);
    texture.created = false;
}

void CinematicReleaseTextures()
{
    CinematicReleaseTexture(s_player.y);
    CinematicReleaseTexture(s_player.cr);
    CinematicReleaseTexture(s_player.cb);
    s_player.texturesCreated = false;
    s_player.haveFrame = false;
    cinematicGlob.activeImageFrame = CINEMATIC_INVALID_IMAGE_FRAME;
}

// Views copy gfxCmdBufInput -- including &s_player.y.draw and friends -- when
// the scene is built (R_GenerateSortedDrawSurfs -> viewInfo->input), but
// R_Cinematic_UpdateFrame runs later, in RB_BeginFrame, and the backend then
// draws those views.  A movie ending there must not free the textures the
// already-built views sample: releasing them immediately left 'cinematicY'
// without a D3D texture in the same frame's draw (cargoship_fade -> fatal
// R_SetSampler).  Retail's Bink path double-buffered its texture sets.  So a
// stop only retires the textures (new views get the black/gray code images
// via haveFrame=false) and they are released kRetireUpdates UpdateFrames
// later, after every view that captured them has been drawn.
constexpr int kRetireUpdates = 3;
int s_texturesRetireUpdates;

void CinematicRetireTextures()
{
    s_player.haveFrame = false;
    cinematicGlob.activeImageFrame = CINEMATIC_INVALID_IMAGE_FRAME;
    if (s_player.texturesCreated)
        s_texturesRetireUpdates = kRetireUpdates;
}

bool CinematicCreateTextures()
{
    if (s_player.texturesCreated && s_texturesRetireUpdates)
    {
        // A new movie before the retired set expired: rebuild in place (the
        // size may differ).  The structs views point at get live textures
        // again immediately, so this release is safe.
        s_texturesRetireUpdates = 0;
        CinematicReleaseTextures();
    }
    if (s_player.texturesCreated)
        return true;
    if (!dx.device || dx.deviceLost)
        return false;

    CinematicMakeImage(&s_player.y.source, "cinematicY_src", s_player.width, s_player.height, IMG_FLAG_SYSTEMMEM);
    CinematicMakeImage(&s_player.y.draw, "cinematicY", s_player.width, s_player.height, IMG_FLAG_DYNAMIC);
    CinematicMakeImage(&s_player.cr.source, "cinematicCr_src", s_player.chromaWidth, s_player.chromaHeight, IMG_FLAG_SYSTEMMEM);
    CinematicMakeImage(&s_player.cr.draw, "cinematicCr", s_player.chromaWidth, s_player.chromaHeight, IMG_FLAG_DYNAMIC);
    CinematicMakeImage(&s_player.cb.source, "cinematicCb_src", s_player.chromaWidth, s_player.chromaHeight, IMG_FLAG_SYSTEMMEM);
    CinematicMakeImage(&s_player.cb.draw, "cinematicCb", s_player.chromaWidth, s_player.chromaHeight, IMG_FLAG_DYNAMIC);

    s_player.y.created = true;
    s_player.cr.created = true;
    s_player.cb.created = true;
    s_player.texturesCreated = true;
    return true;
}

void CinematicApplyCodeImages()
{
    if (s_player.haveFrame && s_player.texturesCreated
        && s_player.y.draw.texture.basemap && s_player.cr.draw.texture.basemap && s_player.cb.draw.texture.basemap)
    {
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y] = &s_player.y.draw;
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CR] = &s_player.cr.draw;
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CB] = &s_player.cb.draw;
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] = rgp.whiteImage;
        cinematicGlob.activeImageFrame = 0;
        cinematicGlob.activeImageFrameTextureSet = 0;
        return;
    }
    // No signal: match r_cinematic.cpp's invalid-frame images so menu
    // materials sampling the cinematic slots never hit R_TextureFromCodeError.
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y] = rgp.blackImage;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CR] = rgp.grayImage;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CB] = rgp.grayImage;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] = rgp.blackImage;
    cinematicGlob.activeImageFrame = CINEMATIC_INVALID_IMAGE_FRAME;
}

bool CinematicUploadPlane(CinematicTexture &texture, const uint8_t *data, int stride, int width, int height)
{
    if (!texture.created || !data || !dx.device || dx.deviceLost)
        return false;
    IDirect3DTexture9 *source = texture.source.texture.map;
    IDirect3DTexture9 *draw = texture.draw.texture.map;
    if (!source || !draw)
        return false;

    D3DLOCKED_RECT locked;
    if (FAILED(source->LockRect(0, &locked, nullptr, 0)))
        return false;
    uint8_t *dst = static_cast<uint8_t *>(locked.pBits);
    for (int row = 0; row < height; ++row)
        memcpy(dst + static_cast<size_t>(row) * locked.Pitch, data + static_cast<size_t>(row) * stride, width);
    source->UnlockRect(0);

    return SUCCEEDED(dx.device->UpdateTexture(source, draw));
}

bool CinematicUploadFrame()
{
    if (!CinematicCreateTextures())
        return false;
    bool ok = CinematicUploadPlane(s_player.y, s_player.decoder.Plane(0), s_player.decoder.PlaneStride(0),
                                   s_player.width, s_player.height);
    ok = CinematicUploadPlane(s_player.cr, s_player.decoder.Plane(1), s_player.decoder.PlaneStride(1),
                              s_player.chromaWidth, s_player.chromaHeight) && ok;
    ok = CinematicUploadPlane(s_player.cb, s_player.decoder.Plane(2), s_player.decoder.PlaneStride(2),
                              s_player.chromaWidth, s_player.chromaHeight) && ok;
    return ok;
}

void CinematicStopNow()
{
    SND_StopCinematicStream();
    s_player.audio.Close();
    s_player.audioOpen = false;
    s_player.audioPaused = false;
    s_player.audioCursor = 0;
    s_player.audioCarryFrames = 0;
    s_player.audioCarryOffset = 0;
    s_player.decoder.Close();
    CinematicRetireTextures();
    s_player.open = false;
    s_player.eof = false;
    s_player.underrun = false;
    s_player.pausedAtMs = 0;
}

// Keeps the decoded BinkAudio track CINE_AUDIO_LEAD_MS ahead of the video wall
// clock.  A full stream queue (the normal case while running ahead) simply
// leaves the remainder in the carry buffer for the next update.
void CinematicPushAudio(int64_t elapsedMs, char *errText, size_t errTextSize)
{
    if (!s_player.audioOpen)
        return;
    const SwitchCinematicAudioInfo &info = s_player.audio.Info();
    int64_t want = (elapsedMs + CINE_AUDIO_LEAD_MS) * info.sampleRate / 1000 - s_player.audioCursor;
    while (want > 0)
    {
        if (s_player.audioCarryFrames == 0)
        {
            int chunk = (int)(want < CINE_AUDIO_PUSH_FRAMES ? want : CINE_AUDIO_PUSH_FRAMES);
            const int decoded = s_player.audio.Decode(s_player.audioCarry, chunk, errText, errTextSize);
            if (decoded < 0)
            {
                CinematicDiag("audiodecodefail t=%lld err=%s", (long long)elapsedMs, errText);
                SND_StopCinematicStream();
                s_player.audioOpen = false;
                return;
            }
            if (decoded == 0)
                return; // end of track; the queued audio plays out
            s_player.audioCarryFrames = decoded;
            s_player.audioCarryOffset = 0;
        }
        const int pushed = SND_PushCinematicPCM(
            s_player.audioCarry + (size_t)s_player.audioCarryOffset * info.channels,
            s_player.audioCarryFrames);
        if (pushed <= 0)
            return; // queue full
        s_player.audioCarryOffset += pushed;
        s_player.audioCarryFrames -= pushed;
        s_player.audioCursor += pushed;
        want -= pushed;
    }
}

// Opens `name` (falling back to the stock "default" movie when it is absent,
// like r_cinematic.cpp) and primes the first frame.
bool CinematicStartPlayback(const char *name, uint32_t playbackFlags)
{
    char path[300];
    char errText[256] = { 0 };
    const char *openedName = name;

    if (!CinematicResolvePath(openedName, path, sizeof(path))
        || !s_player.decoder.Open(path, errText, sizeof(errText)))
    {
        CinematicDiag("openfail name=%s err=%s", name, errText[0] ? errText : "not found");
        ++s_player.openFailures;
        openedName = "default";
        if (!CinematicResolvePath(openedName, path, sizeof(path))
            || !s_player.decoder.Open(path, errText, sizeof(errText)))
        {
            CinematicDiag("openfail name=%s err=%s", openedName, errText[0] ? errText : "not found");
            cinematicGlob.cinematicFinished = 1;
            CinematicRetireTextures();
            return false;
        }
    }

    const SwitchCinematicVideoInfo &info = s_player.decoder.Info();
    if (info.width <= 0 || info.height <= 0)
    {
        cinematicGlob.cinematicFinished = 1;
        CinematicStopNow();
        return false;
    }
    s_player.width = info.width;
    s_player.height = info.height;
    s_player.chromaWidth = info.chromaWidth;
    s_player.chromaHeight = info.chromaHeight;
    s_player.open = true;
    s_player.eof = false;
    s_player.underrun = false;
    s_player.haveFrame = false;
    s_player.frameCount = 0;
    s_player.uploadedFrames = 0;
    s_player.skippedFrames = 0;
    s_player.decodeMs = 0;
    s_player.uploadMs = 0;
    s_player.lastDecodedMs = 0;
    s_player.lastUploadedMs = 0;
    s_player.playStartMs = Sys_Milliseconds();
    s_player.pausedAtMs = 0;

    // The movie's BinkAudio track, decoded from the same file and pushed to
    // the sound system's cinematic stream as the wall clock advances.  A
    // movie without audio (or with the sound system not up yet) still plays.
    s_player.audioOpen = false;
    s_player.audioPaused = false;
    s_player.audioCursor = 0;
    s_player.audioCarryFrames = 0;
    s_player.audioCarryOffset = 0;
    errText[0] = 0;
    if (s_player.audio.Open(path, errText, sizeof(errText)))
    {
        const SwitchCinematicAudioInfo &audioInfo = s_player.audio.Info();
        if (SND_StartCinematicStream(audioInfo.sampleRate, audioInfo.channels))
        {
            SND_SetCinematicStreamVolume(cinematicGlob.playbackVolume);
            s_player.audioOpen = true;
        }
        else
        {
            CinematicDiag("audiostartfail rate=%d ch=%d", audioInfo.sampleRate, audioInfo.channels);
            s_player.audio.Close();
        }
    }
    else
    {
        CinematicDiag("audiofail name=%s err=%s", openedName, errText[0] ? errText : "no audio track");
        s_player.audio.Close();
    }

    int err = s_player.decoder.DecodeNext(errText, sizeof(errText));
    if (err < 0)
    {
        CinematicDiag("decodefail name=%s err=%s", openedName, errText);
        cinematicGlob.cinematicFinished = 1;
        CinematicStopNow();
        return false;
    }
    if (err == 0)
        s_player.eof = true;
    ++s_player.frameCount;
    s_player.lastDecodedMs = s_player.decoder.FrameTimeMs();

    CinematicDiag("open name=%s path=%s %ux%u fps=%.3f frames=%d dur=%dms flags=0x%x audio=%d rate=%d",
                  openedName, path, s_player.width, s_player.height, info.fps,
                  (int)info.frameCount, (int)info.durationMs, playbackFlags,
                  s_player.audioOpen ? 1 : 0, s_player.audio.Info().sampleRate);
    cinematicGlob.usingAlpha = 0;
    return true;
}

// Advances playback to the wall clock.  Returns false when the movie ended
// (and is not looping).
bool CinematicAdvance(uint32_t playbackFlags)
{
    if (!s_player.open)
        return false;

    const uint32_t now = Sys_Milliseconds();
    if (cinematicGlob.targetPaused == CINEMATIC_PAUSED)
    {
        if (s_player.pausedAtMs == 0)
            s_player.pausedAtMs = now ? now : 1;
        if (s_player.audioOpen && !s_player.audioPaused)
        {
            s_player.audioPaused = true;
            SND_PauseCinematicStream(true);
        }
        return true;
    }
    if (s_player.pausedAtMs != 0)
    {
        s_player.playStartMs += now - s_player.pausedAtMs;
        s_player.pausedAtMs = 0;
    }
    if (s_player.audioPaused)
    {
        s_player.audioPaused = false;
        SND_PauseCinematicStream(false);
    }

    const int64_t elapsedMs = (int64_t)(now - s_player.playStartMs);
    char errText[256] = { 0 };
    bool decoded = false;
    uint32_t budget = CINE_MAX_DECODE_PER_UPDATE;
    bool failed = false;

    while (!s_player.eof && budget > 0 && s_player.lastDecodedMs < elapsedMs)
    {
        const uint32_t decodeBegin = Sys_Milliseconds();
        const int err = s_player.decoder.DecodeNext(errText, sizeof(errText));
        s_player.decodeMs += Sys_Milliseconds() - decodeBegin;
        if (err < 0)
        {
            CinematicDiag("decodefail t=%lld err=%s", (long long)elapsedMs, errText);
            failed = true;
            break;
        }
        if (err == 0)
        {
            s_player.eof = true;
            break;
        }
        ++s_player.frameCount;
        if (decoded)
            ++s_player.skippedFrames; // decoded past the wall clock; not shown
        decoded = true;
        s_player.lastDecodedMs = s_player.decoder.FrameTimeMs();
        --budget;
    }
    s_player.underrun = decoded && budget == 0 && s_player.lastDecodedMs + 100 < elapsedMs;

    // The primed frame is uploaded on the first update even though no
    // wall-clock frame boundary has passed yet.
    if (decoded || !s_player.haveFrame)
    {
        const uint32_t uploadBegin = Sys_Milliseconds();
        const bool uploaded = CinematicUploadFrame();
        s_player.uploadMs += Sys_Milliseconds() - uploadBegin;
        if (uploaded)
        {
            s_player.haveFrame = true;
            s_player.lastUploadedMs = s_player.lastDecodedMs;
            ++s_player.uploadedFrames;
            if (s_player.uploadedFrames == 1)
                CinematicDiag("firstframe %ux%u", s_player.width, s_player.height);
        }
        else if (decoded)
        {
            CinematicDiag("uploadfail w=%u h=%u", s_player.width, s_player.height);
        }
    }

    if (failed)
        return false;

    CinematicPushAudio(elapsedMs, errText, sizeof(errText));

    if (!s_player.eof)
        return true;

    if ((playbackFlags & CINE_FLAG_LOOP) != 0)
    {
        if (s_player.decoder.SeekToStart(errText, sizeof(errText)))
        {
            s_player.eof = false;
            s_player.lastDecodedMs = 0;
            s_player.lastUploadedMs = 0;
            s_player.playStartMs = now;
            if (s_player.audioOpen)
            {
                // The stream still holds the tail of the passed iteration;
                // rewind and restart it so the looped audio matches the
                // looped video.
                char audioErr[128] = { 0 };
                const int rate = s_player.audio.Info().sampleRate;
                const int channels = s_player.audio.Info().channels;
                SND_StopCinematicStream();
                if (!s_player.audio.SeekToStart(audioErr, sizeof(audioErr)))
                {
                    CinematicDiag("loopaudiofail err=%s", audioErr);
                    s_player.audioOpen = false;
                }
                else if (!SND_StartCinematicStream(rate, channels))
                {
                    s_player.audioOpen = false;
                }
                else
                {
                    SND_SetCinematicStreamVolume(cinematicGlob.playbackVolume);
                }
                s_player.audioCursor = 0;
                s_player.audioCarryFrames = 0;
                s_player.audioCarryOffset = 0;
                s_player.audioPaused = false;
            }
            return true;
        }
        CinematicDiag("loopseekfail err=%s", errText);
    }
    return false;
}
} // namespace

void __cdecl R_Cinematic_Init()
{
    if (s_initialized)
        return;
    memset(&cinematicGlob, 0, sizeof(cinematicGlob));
    cinematicGlob.activeImageFrame = CINEMATIC_INVALID_IMAGE_FRAME;
    cinematicGlob.currentPaused = CINEMATIC_NOT_PAUSED;
    cinematicGlob.targetPaused = CINEMATIC_NOT_PAUSED;
    s_player.open = false;
    s_player.eof = false;
    s_player.haveFrame = false;
    s_player.underrun = false;
    s_player.texturesCreated = false;
    s_player.pausedAtMs = 0;
    s_initialized = true;
}

void __cdecl R_Cinematic_Shutdown()
{
    if (!s_initialized)
        return;
    CinematicStopNow();
    cinematicGlob.currentCinematicName[0] = 0;
    cinematicGlob.targetCinematicName[0] = 0;
    cinematicGlob.nextCinematicName[0] = 0;
    s_initialized = false;
}

void __cdecl R_Cinematic_StartPlayback(char *name, uint32_t playbackFlags, float volume)
{
    if (!s_initialized)
        R_Cinematic_Init();
    I_strncpyz(cinematicGlob.targetCinematicName, name ? name : "", 256);
    cinematicGlob.targetCinematicChanged = 1;
    cinematicGlob.cinematicFinished = 0;
    cinematicGlob.targetPaused = CINEMATIC_NOT_PAUSED;
    cinematicGlob.playbackFlags = playbackFlags;
    cinematicGlob.playbackVolume = volume;
}

void __cdecl R_Cinematic_StartNextPlayback()
{
    if (cinematicGlob.nextCinematicName[0] == 0)
        return;
    R_Cinematic_StartPlayback(cinematicGlob.nextCinematicName,
                              cinematicGlob.nextCinematicPlaybackFlags,
                              cinematicGlob.playbackVolume);
    cinematicGlob.nextCinematicName[0] = 0;
}

void __cdecl R_Cinematic_StopPlayback()
{
    cinematicGlob.targetCinematicName[0] = 0;
    cinematicGlob.targetCinematicChanged = 1;
    cinematicGlob.cinematicFinished = 0;
}

void R_Cinematic_UnsetNextPlayback()
{
    cinematicGlob.nextCinematicName[0] = 0;
    cinematicGlob.nextCinematicPlaybackFlags = 0;
}

void R_Cinematic_SetNextPlayback(const char *name, uint32_t playbackFlags)
{
    I_strncpyz(cinematicGlob.nextCinematicName, name ? name : "", 256);
    cinematicGlob.nextCinematicPlaybackFlags = playbackFlags;
    // An in-game cinematic replaces the rendered frame; the walk evidence
    // verifier hard-rejects a walk window that reached one.
    RetailKillhouseFrameEvidenceNoteExcluded(RKE_SITE_CINE_SET_NEXT_PLAYBACK);
}

void __cdecl R_Cinematic_SetPaused(CinematicEnum paused)
{
    cinematicGlob.targetPaused = paused;
}

void __cdecl R_Cinematic_UpdateFrame()
{
    if (!s_initialized)
        R_Cinematic_Init();
    // One single-buffered player: the update runs on the thread that draws
    // (RB_BeginFrame). While the render back end runs on its own thread and
    // owns the renderer (r_smp_backend 1, frame handed off), a main-thread
    // update would create/release the textures the back end is sampling.
    if (r_glob.startedRenderThread && !r_glob.haveThreadOwnership && !Sys_IsRenderThread())
    {
        static bool reported;
        if (!reported)
        {
            reported = true;
            Com_PrintError(0, "FAIL:CINEMATIC_THREAD R_Cinematic_UpdateFrame off the render thread while the back end owns the renderer\n");
        }
        return;
    }

    if (s_texturesRetireUpdates && --s_texturesRetireUpdates == 0)
        CinematicReleaseTextures();

    if (cinematicGlob.targetCinematicChanged)
    {
        cinematicGlob.targetCinematicChanged = 0;
        char target[256];
        I_strncpyz(target, cinematicGlob.targetCinematicName, 256);
        const uint32_t flags = cinematicGlob.playbackFlags;

        if (cinematicGlob.currentCinematicName[0])
            CinematicStopNow();
        if (target[0] && CinematicStartPlayback(target, flags))
            I_strncpyz(cinematicGlob.currentCinematicName, target, 256);
        else
            cinematicGlob.currentCinematicName[0] = 0;
    }

    if (cinematicGlob.currentCinematicName[0] && s_player.open)
    {
        if (!CinematicAdvance(cinematicGlob.playbackFlags))
        {
            CinematicDiag("end name=%s decoded=%u uploaded=%u skipped=%u last=%dms decode_ms=%llu upload_ms=%llu "
                          "audio_pushed=%lld",
                          cinematicGlob.currentCinematicName, s_player.frameCount,
                          s_player.uploadedFrames, s_player.skippedFrames, (int)s_player.lastUploadedMs,
                          (unsigned long long)s_player.decodeMs, (unsigned long long)s_player.uploadMs,
                          (long long)s_player.audioCursor);
            cinematicGlob.cinematicFinished = 1;
            CinematicStopNow();
        }
        else
        {
            const uint32_t now = Sys_Milliseconds();
            if (now - s_lastReportMs >= 1000)
            {
                s_lastReportMs = now;
                CinematicDiag("frame decoded=%u uploaded=%u skipped=%u t=%dms active=%d decode_ms=%llu upload_ms=%llu "
                              "audio_ms=%lld",
                              s_player.frameCount, s_player.uploadedFrames, s_player.skippedFrames,
                              (int)s_player.lastUploadedMs, cinematicGlob.activeImageFrame != CINEMATIC_INVALID_IMAGE_FRAME,
                              (unsigned long long)s_player.decodeMs, (unsigned long long)s_player.uploadMs,
                              s_player.audioOpen
                                  ? (long long)((int64_t)s_player.audioCursor * 1000 / s_player.audio.Info().sampleRate)
                                  : -1LL);
            }
        }
    }
    else if (cinematicGlob.currentCinematicName[0] && !s_player.open)
    {
        cinematicGlob.cinematicFinished = 1;
    }

    CinematicApplyCodeImages();
    cinematicGlob.timeInMsec = s_player.haveFrame ? (uint32_t)s_player.lastUploadedMs : 0;
}

void __cdecl R_Cinematic_SyncNow()
{
    R_Cinematic_UpdateFrame();
}

bool __cdecl R_Cinematic_IsFinished()
{
    return cinematicGlob.cinematicFinished;
}

bool __cdecl R_Cinematic_IsStarted()
{
    return !R_Cinematic_IsFinished() && cinematicGlob.currentCinematicName[0] != 0;
}

bool R_Cinematic_IsPending()
{
    return cinematicGlob.targetCinematicName[0] != 0;
}

bool __cdecl R_Cinematic_IsNextReady()
{
    return cinematicGlob.nextCinematicName[0] != 0;
}

bool __cdecl R_Cinematic_IsUnderrun()
{
    return s_player.underrun;
}

void __cdecl R_Cinematic_BeginLostDevice()
{
    // The decoded planes stay in CPU memory; the textures are recreated and
    // re-uploaded on the next UpdateFrame.
    CinematicReleaseTextures();
}

void __cdecl R_Cinematic_EndLostDevice()
{
}

void __cdecl R_Cinematic_DrawStretchPic_Letterboxed()
{
    // Draw reach is the other half of the playback proof: decode+upload can
    // be healthy while the menu never paints ownerdraw 277.
    static uint32_t s_drawCalls;
    static uint32_t s_lastDrawReportMs;
    ++s_drawCalls;
    const uint32_t drawNow = Sys_Milliseconds();
    if (drawNow - s_lastDrawReportMs >= 1000)
    {
        const char *menuName = UI_GetTopActiveMenuName(0);
        CinematicDiag("draw calls=%u active=%d uploaded=%u menu=%s",
                      s_drawCalls,
                      cinematicGlob.activeImageFrame != CINEMATIC_INVALID_IMAGE_FRAME,
                      s_player.uploadedFrames,
                      menuName ? menuName : "(none)");
        s_drawCalls = 0;
        s_lastDrawReportMs = drawNow;
    }

    if (!rgp.cinematicMaterial || !rgp.whiteMaterial)
        return;
    const float width = (float)vidConfig.displayWidth;
    const float height = (float)vidConfig.displayHeight;
    float movieHeight = width * vidConfig.aspectRatioDisplayPixel / 1.7777778f;
    if (height < movieHeight)
        movieHeight = height;
    const float letterboxHalfHeight = (height - movieHeight) * 0.5f;

    float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    R_AddCmdDrawStretchPic(0.0f, 0.0f, width, letterboxHalfHeight, 0.0f, 0.0f, 1.0f, 1.0f, color, rgp.whiteMaterial);
    R_AddCmdDrawStretchPic(0.0f, height - letterboxHalfHeight, width, letterboxHalfHeight, 0.0f, 0.0f, 1.0f, 1.0f, color, rgp.whiteMaterial);
    const CinematicPlaneDraw plane = Cinematic_MoviePlaneDraw(
        cinematicGlob.activeImageFrame != CINEMATIC_INVALID_IMAGE_FRAME, rgp.cinematicMaterial, rgp.whiteMaterial,
        color, colorWhite);
    R_AddCmdDrawStretchPic(0.0f, letterboxHalfHeight, width, movieHeight, 0.0f, 0.0f, 1.0f, 1.0f, plane.color, plane.material);
}

#endif // __SWITCH__
