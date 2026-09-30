// Switch cinematic video decoder (FFmpeg-backed).
//
// The retail Bink SDK is licensed Win32/x86 code and is never ported (the
// Bink decision).  This is the narrow decode core of
// the replacement backend: FFmpeg's binkvideo decoder (devkitPro's Switch
// portlib) feeding the engine's Y/Cr/Cb cinematic code images.  Deliberately
// free of engine and platform includes so the host proof can link it against
// the host FFmpeg and exercise real .bik files without a D3D device.
//
// No hardware Bink decoder exists on Tegra: NVDEC covers H.264/VP8/VP9/AV1,
// not Bink.  Decode is software; the GPU only receives the uploaded planes.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct SwitchCinematicVideoInfo
{
    int width;
    int height;
    int chromaWidth;
    int chromaHeight;
    double fps;
    int64_t frameCount;   // -1 when the container does not say
    int64_t durationMs;   // -1 when unknown
};

class SwitchCinematicVideoDecoder
{
public:
    SwitchCinematicVideoDecoder();
    ~SwitchCinematicVideoDecoder();

    SwitchCinematicVideoDecoder(const SwitchCinematicVideoDecoder &) = delete;
    SwitchCinematicVideoDecoder &operator=(const SwitchCinematicVideoDecoder &) = delete;

    // Opens `path` (a real filesystem path, e.g. sdmc:/switch/kisakcod/
    // main/video/killhouse_load.bik) and selects the first video stream.
    bool Open(const char *path, char *errText, size_t errTextSize);
    void Close();
    bool IsOpen() const;

    const SwitchCinematicVideoInfo &Info() const { return m_info; }

    // Decodes the next video frame into the owned YUV420P plane buffer.
    // Returns 1 on a produced frame, 0 at end of stream, -1 on error.
    int DecodeNext(char *errText, size_t errTextSize);

    // Presentation time of the last produced frame, in milliseconds.
    int64_t FrameTimeMs() const { return m_frameTimeMs; }

    // Returns the stream back to its first frame (loop playback).
    bool SeekToStart(char *errText, size_t errTextSize);

    // 0 = Y, 1 = Cr, 2 = Cb.  Never null while a frame is held.
    const uint8_t *Plane(int index) const;
    int PlaneStride(int index) const;

private:
    int ReceiveFrame(char *errText, size_t errTextSize);
    void ConvertFrame(char *errText, size_t errTextSize);

    struct Impl;
    Impl *m_impl;
    SwitchCinematicVideoInfo m_info;
    int64_t m_frameTimeMs;
};

struct SwitchCinematicAudioInfo
{
    int sampleRate;  // decoded (output) rate, always 48000
    int channels;    // decoded (output) channels, always 2
};

// Switch cinematic audio decoder (FFmpeg BinkAudio-backed).
//
// The retail Bink SDK's sound system (BinkOpenMiles) is Win32/x86 and never
// ported; on Switch the FFmpeg backend decodes the movie's BinkAudio track
// itself and pushes the PCM to the sound system's cinematic stream.  Like the
// video core this class is engine- and platform-free so the host proof can
// exercise real .bik files with the host FFmpeg.
//
// The track is decoded incrementally (one decoded frame at a time, at most
// ~0.4 s of resampled PCM held) rather than up front: the audio-bearing retail
// movies run up to ~100 s (~19 MB as S16 stereo 48 kHz, and killhouse_load is
// ~7 MB) and are played while a level load owns the frame, so the decoder
// never holds more than the next chunk.
class SwitchCinematicAudioDecoder
{
public:
    SwitchCinematicAudioDecoder();
    ~SwitchCinematicAudioDecoder();

    SwitchCinematicAudioDecoder(const SwitchCinematicAudioDecoder &) = delete;
    SwitchCinematicAudioDecoder &operator=(const SwitchCinematicAudioDecoder &) = delete;

    // Opens `path` and prepares the audio stream.  Returns false when the file
    // has no audio stream or decoding fails; callers treat that as a silent
    // movie rather than an error.
    bool Open(const char *path, char *errText, size_t errTextSize);
    void Close();
    bool IsOpen() const;

    const SwitchCinematicAudioInfo &Info() const { return m_info; }

    // Decodes up to `maxFrames` interleaved S16 frames into `dst`.  Returns
    // the number of frames written (0 at the end of the track) or -1 on
    // error.  The caller retries (or stops, on error) on a later update, so
    // this never blocks on the sound system.
    int Decode(int16_t *dst, int maxFrames, char *errText, size_t errTextSize);

    // Rewinds to the first audio frame (looped cinematics).
    bool SeekToStart(char *errText, size_t errTextSize);

private:
    void CompactPending();
    void ConvertPendingFrame();
    void DrainResampler();

    struct Impl;
    Impl *m_impl;
    SwitchCinematicAudioInfo m_info;
};
