#pragma once
// Streamed sounds for the OpenAL backend, decoded on a dedicated thread.
//
// Retail Miles streamed from its own service thread.  Doing the decode,
// seeks and refills on the game thread stalled the frame (every sound in the
// iwds is deflated, so a seek inflates up to its target and an MP3 length scan
// inflates the whole file) and let any long frame drain the queue (music
// dropouts).  The game thread keeps all channel bookkeeping and only:
//   - opens the file and parses the header (SND_StreamOpen: cheap, and every
//     fsh[] table mutation stays on this thread),
//   - hands the stream over (SND_StreamBegin) and later stops it,
//   - states whether the source should be playing (SND_StreamSetWantPlay);
//     the stream thread owns every play/pause/stop/queue call on the source,
//   - polls status once per frame (SND_StreamPoll).
// Gain, pitch and position stay game-thread AL calls on the source.

#include <cstdint>

// Stream channels are g_snd.chaninfo[40..52] (SND_FIRST_STREAM_CHANNEL..
// SND_MAX_CHANNELS; snd_driver_openal.cpp static_asserts the match).
constexpr int SND_STREAM_FIRST_CHANNEL = 40;
constexpr int SND_STREAM_CHANNEL_END = 53;

// The AL source of channel `index` (alGlob.source in the driver).
unsigned int SND_StreamSource(int index);

enum SndStreamState
{
    SND_STREAM_NONE = 0,
    SND_STREAM_LOADING = 1,  // handed over, not yet seeked/prefilled
    SND_STREAM_PLAYING = 2,  // prefilled; totalFrames/startFrame are final
    SND_STREAM_ENDED = 3,    // non-looping stream fully played out
    SND_STREAM_FAILED = 4,   // decode/seek failure; the channel should stop
};

struct SndStreamOpenInfo
{
    int channels;
    int sampleRate;
    int64_t totalFrames;     // -1 when unknown (uncached MP3: scanned on the stream thread)
};

struct SndStreamStatus
{
    SndStreamState state;
    int64_t totalFrames;     // valid from SND_STREAM_PLAYING on
    int64_t startFrame;      // frame playback started from
    uint64_t framesQueued;   // decode position in the current loop pass
};

// Start of a stream: either an absolute frame (length known on the game
// thread) or a fraction of the length / a native-time offset resolved by the
// stream thread once it knows the length.
struct SndStreamStart
{
    int64_t frame;           // >= 0: absolute start frame
    float fraction;          // used when frame < 0: [0,1) of the length
    int nativeMsec;          // used when frame < 0 and fraction == 0
};

// Opens `realname` for stream channel `index` and parses its header.  The
// opened stream stays pending on this channel until SND_StreamBegin or
// SND_StreamCancel.  Returns false (and logs) when the file cannot be opened
// or decoded.
bool SND_StreamOpen(int index, const char *realname, bool isMp3, SndStreamOpenInfo *out);
// Drops a pending open that will not be played.
void SND_StreamCancel(int index);
// Hands the pending stream to the stream thread.
void SND_StreamBegin(int index, bool looping, const SndStreamStart &start, bool wantPlay);
void SND_StreamSetWantPlay(int index, bool play);
void SND_StreamStop(int index);
bool SND_StreamIsActive(int index);
void SND_StreamPoll(int index, SndStreamStatus *out);
// Stops every stream and joins the thread (sound shutdown).
void SND_StreamShutdown();
// Stops every stream and waits until the stream thread has retired every job,
// then closes their file handles; the thread keeps running.  FS_Shutdown
// calls it before it closes every open handle: a stream handle belongs to the
// stream thread until its job is retired, so closing it any earlier reads a
// freed zip handle on the stream thread and double-closes the slot later.
void SND_StreamReleaseAll();
// Game-thread housekeeping each sound frame: closes file handles the stream
// thread finished with.
void SND_StreamServiceMainThread();
// Underrun restarts since boot (SND_OPENAL_EVIDENCE).
uint32_t SND_StreamUnderrunCount();
// Once per second under switch_perfTrace.
void SND_StreamEmitPerf(int nowMs);
