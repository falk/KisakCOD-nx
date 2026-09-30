// Host proof for the Switch cinematic decode core (switch_cinematic_decode.*).
//
// The Switch backend (switch_cinematic_ffmpeg.cpp) is a thin D3D9/engine
// shell over this FFmpeg decode core; a run on the console or an emulator proves the upload and
// draw side, and this binary proves the codec side against a real retail Bink
// file with the same FFmpeg APIs (host libav*).  It asserts the properties the
// engine's R_Cinematic_* contract depends on:
//   - Bink video opens through the file: protocol with real dimensions/fps,
//   - the advertised frame count is decoded exactly (within one trailing
//     frame), with monotonically non-decreasing presentation times,
//   - Y/Cr/Cb planes are present at the expected sizes/strides,
//   - frame content actually changes (not a frozen/black decode),
//   - SeekToStart rewinds to the first frame (in-game loop cinematics).
//
// With a second (audio-bearing) file it proves the BinkAudio side the same
// way: the track decodes incrementally one requested chunk at a time, is not
// silent, its duration matches the video, and SeekToStart reproduces the first
// samples (the loop path).
//
// The files are passed on the command line (the test script stages the retail
// movies); this binary never embeds game data.
//
// Prints PASS:SWITCH_CINEMATIC_DECODE / PASS:SWITCH_CINEMATIC_AUDIO (and the
// matching FAIL: forms) and exits nonzero on failure.
#include "src/port/switch_cinematic_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *why, const char *detail)
{
    printf("FAIL:SWITCH_CINEMATIC_DECODE %s%s%s\n", why, detail ? " " : "", detail ? detail : "");
    return 1;
}

static int fail_audio(const char *why, const char *detail)
{
    printf("FAIL:SWITCH_CINEMATIC_AUDIO %s%s%s\n", why, detail ? " " : "", detail ? detail : "");
    return 1;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Decodes `path`'s BinkAudio track the way the Switch backend does: chunks are
// pulled with Decode() as playback advances, never as one decoded file.
static int audio_check(const char *path)
{
    SwitchCinematicAudioDecoder audio;
    char err[256] = { 0 };
    if (!audio.Open(path, err, sizeof(err)))
        return fail_audio("open", err);

    const SwitchCinematicAudioInfo &ainfo = audio.Info();
    if (ainfo.sampleRate != 48000 || ainfo.channels != 2)
        return fail_audio("format", "expected S16 stereo 48 kHz");

    const int chunkFrames = 4096;
    int16_t *chunk = (int16_t *)malloc((size_t)chunkFrames * 2 * sizeof(int16_t));
    int16_t *first = (int16_t *)malloc((size_t)chunkFrames * 2 * sizeof(int16_t));
    int16_t *rewoundChunk = (int16_t *)malloc((size_t)chunkFrames * 2 * sizeof(int16_t));
    if (!chunk || !first || !rewoundChunk)
        return fail_audio("alloc", "chunk buffers");

    // The decoder must be able to produce a single frame on demand; an
    // up-front-decode design cannot (and would hold the whole track).
    for (int i = 0; i < 4; ++i)
    {
        const int one = audio.Decode(chunk, 1, err, sizeof(err));
        if (one != 1)
            return fail_audio("incremental", err[0] ? err : "Decode(1 frame) did not return one frame");
    }

    int64_t frames = 0;
    int peak = 0;
    unsigned long long fingerprints[64];
    int fingerprintCount = 0;
    unsigned long long fingerprint = 14695981039ULL;
    int64_t nextFingerprintAt = 0;
    bool haveFirst = false;

    for (;;)
    {
        const int got = audio.Decode(chunk, chunkFrames, err, sizeof(err));
        if (got < 0)
            return fail_audio("decode", err);
        if (got == 0)
            break;
        if (got > chunkFrames)
            return fail_audio("overrun", "Decode wrote past maxFrames");
        if (!haveFirst)
        {
            // Keep the first full chunk for the rewind comparison below.
            memcpy(first, chunk, (size_t)got * 2 * sizeof(int16_t));
            haveFirst = true;
        }
        for (int i = 0; i < got * 2; ++i)
        {
            const int sample = chunk[i];
            const int magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peak)
                peak = magnitude;
            fingerprint = (fingerprint ^ (unsigned long long)(unsigned short)sample) * 1099511628211ULL;
        }
        frames += got;
        if (frames >= nextFingerprintAt && fingerprintCount < 64)
        {
            fingerprints[fingerprintCount++] = fingerprint;
            nextFingerprintAt = frames + 48000; // one fingerprint per second
        }
        if (frames > 48000LL * 60 * 20)
            return fail_audio("runaway", "audio decoder did not reach the end of the track");
    }

    if (frames <= 0)
        return fail_audio("frames", "no audio decoded");
    if (peak < 200)
        return fail_audio("silence", "decoded track is (near) silent");

    int distinct = 0;
    for (int i = 0; i < fingerprintCount; ++i)
    {
        bool seen = false;
        for (int j = 0; j < i; ++j)
            seen = seen || fingerprints[j] == fingerprints[i];
        if (!seen)
            ++distinct;
    }
    if (distinct < 2)
        return fail_audio("content", "audio content never changed");

    // Duration agreement with the video: both streams of a .bik span the same
    // timeline (the resampler adds at most a few ms of delay).
    SwitchCinematicVideoDecoder video;
    const int64_t audioMs = frames * 1000 / ainfo.sampleRate;
    if (video.Open(path, err, sizeof(err)))
    {
        const int64_t videoMs = video.Info().durationMs;
        if (videoMs > 0)
        {
            const int64_t delta = audioMs - videoMs;
            const int64_t tolerance = videoMs / 20 + 1000; // 5% + 1 s
            if (delta < -tolerance || delta > tolerance)
                return fail_audio("duration", "audio and video lengths disagree");
        }
        video.Close();
    }

    // Loop path: rewinding must reproduce the first samples of the track (the
    // four single-frame pulls at the top consumed the first four frames).
    if (!audio.SeekToStart(err, sizeof(err)))
        return fail_audio("seek", err);
    for (int i = 0; i < 4; ++i)
    {
        if (audio.Decode(rewoundChunk, 1, err, sizeof(err)) != 1)
            return fail_audio("seek_skip", err[0] ? err : "short decode after rewind");
    }
    const int rewound = audio.Decode(rewoundChunk, chunkFrames, err, sizeof(err));
    if (rewound != chunkFrames)
        return fail_audio("seek_decode", err[0] ? err : "short decode after rewind");
    if (memcmp(rewoundChunk, first, (size_t)chunkFrames * 2 * sizeof(int16_t)) != 0)
        return fail_audio("seek_samples", "rewound audio differs from the start of the track");

    printf("PASS:SWITCH_CINEMATIC_AUDIO file=%s frames=%lld ms=%lld peak=%d fingerprints=%d distinct=%d rewind=%d\n",
           base_name(path), (long long)frames, (long long)audioMs, peak, fingerprintCount, distinct, rewound);
    free(chunk);
    free(first);
    free(rewoundChunk);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return fail("usage", "switch_cinematic_decode_test <file.bik> [audio.bik]");

    SwitchCinematicVideoDecoder decoder;
    char err[256] = { 0 };
    if (!decoder.Open(argv[1], err, sizeof(err)))
        return fail("open", err);

    const SwitchCinematicVideoInfo &info = decoder.Info();
    if (info.width <= 0 || info.height <= 0 || info.chromaWidth <= 0 || info.chromaHeight <= 0)
        return fail("dimensions", "non-positive video size");
    if (info.fps <= 0.0)
        return fail("fps", "non-positive frame rate");

    int frames = 0;
    int64_t firstMs = -1;
    int64_t lastMs = -1;
    int64_t previousMs = -1;
    int timeRegressions = 0;
    unsigned long long distinctY = 0;
    unsigned long long previousY = 0;
    bool havePreviousY = false;

    for (;;)
    {
        const int result = decoder.DecodeNext(err, sizeof(err));
        if (result < 0)
            return fail("decode", err);
        if (result == 0)
            break;

        const int64_t frameMs = decoder.FrameTimeMs();
        if (previousMs >= 0 && frameMs < previousMs)
            ++timeRegressions;
        previousMs = frameMs;
        if (frames == 0)
            firstMs = frameMs;
        lastMs = frameMs;

        const uint8_t *y = decoder.Plane(0);
        const uint8_t *cr = decoder.Plane(1);
        const uint8_t *cb = decoder.Plane(2);
        const int strideY = decoder.PlaneStride(0);
        const int strideC = decoder.PlaneStride(1);
        if (!y || !cr || !cb || strideY < info.width || strideC < info.chromaWidth)
            return fail("planes", "missing plane or short stride");

        unsigned long long sum = 0;
        for (int row = 0; row < info.height; row += 37)
        {
            for (int col = 0; col < info.width; col += 41)
                sum = sum * 131ULL + y[row * strideY + col];
        }
        if (!havePreviousY || sum != previousY)
        {
            ++distinctY;
            previousY = sum;
            havePreviousY = true;
        }

        ++frames;
        if (frames > 200000)
            return fail("runaway", "decoder did not reach EOF");
    }

    if (frames == 0)
        return fail("frames", "no frames decoded");
    if (info.frameCount > 0)
    {
        const int64_t delta = (int64_t)frames - info.frameCount;
        if (delta < -1 || delta > 1)
            return fail("frame_count", "decoded count differs from header");
    }
    if (timeRegressions != 0)
        return fail("timestamps", "presentation time regressed");
    if (distinctY < 2)
        return fail("content", "every sampled frame is identical");
    if (lastMs <= firstMs)
        return fail("duration", "last frame time did not advance");

    if (!decoder.SeekToStart(err, sizeof(err)))
        return fail("seek", err);
    const int rewound = decoder.DecodeNext(err, sizeof(err));
    if (rewound != 1)
        return fail("seek_decode", err);
    if (decoder.FrameTimeMs() > firstMs)
        return fail("seek_time", "rewound frame time is after the first frame");

    char file[256];
    snprintf(file, sizeof(file), "%s", base_name(argv[1]));
    printf("PASS:SWITCH_CINEMATIC_DECODE file=%s frames=%d distinct=%llu first_ms=%lld last_ms=%lld "
           "size=%dx%d chroma=%dx%d fps=%.3f\n",
           file, frames, distinctY, (long long)firstMs, (long long)lastMs,
           info.width, info.height, info.chromaWidth, info.chromaHeight, info.fps);

    // The BinkAudio half of the same decode core, on a movie that carries a
    // track (default.bik, the video-only fallback, has none).
    if (argc >= 3)
        return audio_check(argv[2]);
    return 0;
}
