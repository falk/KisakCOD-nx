#pragma once
// Output gains the audren sound backend applies per voice, chosen to match
// what the OpenAL backend produces for the same source state.
//
// The OpenAL path (snd_openal.cpp / snd_driver_openal.cpp) configures
// openal-soft as follows, and this file mirrors exactly that configuration:
//   - alDistanceModel(AL_NONE): no distance attenuation by OpenAL; the engine
//     bakes SND_Attenuate's falloff curve into AL_GAIN itself.
//   - listener fixed at the origin with the default orientation (at -Z, up
//     +Y); every 3D position is pre-transformed into listener space as
//     (-left, up, -forward) = (right, up, back).
//   - AL_GAIN is a plain scalar, clamped by openal-soft to the source's
//     AL_MAX_GAIN, which the port never changes from its default of 1.
//   - stereo output, no HRTF, default "panpot" stereo encoding: openal-soft
//     pans a mono source with its first-order ambisonic stereo decoder in
//     "pairwise" mode, which widens front azimuths by 1.5x (clamped to 90
//     degrees) and keeps the elevation.  With N3D scaling the decoder row
//     for each speaker is {W: 0.5, Y: +/-0.288675135, X: 0.0552305643}; the
//     direction coefficients are {1, sqrt(3) sin(az) cos(ev),
//     sqrt(3) cos(az) cos(ev)}, so
//         L = 0.5 - 0.5 sin(az) cos(ev) + 0.0956621 cos(az) cos(ev)
//         R = 0.5 + 0.5 sin(az) cos(ev) + 0.0956621 cos(az) cos(ev)
//     (az taken to the right of forward).  A source at the listener's
//     position (every 2D channel: its AL_POSITION is never set) pans as
//     straight ahead: 0.5957 per ear.
//   - a stereo buffer is not spatialised: its left channel goes to the left
//     output and its right to the right, at AL_GAIN.
// switch_snd_audren_test.cpp checks these numbers against a loopback render
// of the host's openal-soft (the only verifier: Switch links no openal-soft).

#include <cmath>
#include <cfloat>

struct SndArMix
{
    // gain[src channel][output channel]
    float gain[2][2];
};

constexpr float kSndArDecoderW = 0.5f;
constexpr float kSndArDecoderY = 0.5f;          // 0.288675135 * sqrt(3)
constexpr float kSndArDecoderX = 0.0956621f;    // 0.0552305643 * sqrt(3)
constexpr float kSndArMaxGain = 1.0f;           // openal-soft's default AL_MAX_GAIN

inline float SndAr_ClampGain(float gain)
{
    if (!(gain > 0.0f))
        return 0.0f;
    return gain > kSndArMaxGain ? kSndArMaxGain : gain;
}

// Pan of a mono source at listener-space AL position (x right, y up, z back).
inline void SndAr_PanMono(float x, float y, float z, float *left, float *right)
{
    const float halfPi = 1.57079632679f;
    const float len = std::sqrt(x * x + y * y + z * z);
    float dx = 0.0f, dy = 0.0f, dz = -1.0f;
    if (len > FLT_EPSILON)
    {
        dx = x / len;
        dy = y / len;
        dz = z / len;
    }
    const float ev = std::asin(dy < -1.0f ? -1.0f : (dy > 1.0f ? 1.0f : dy));
    float az = std::atan2(dx, -dz);
    if (std::fabs(az) < halfPi)
    {
        const float scaled = std::fabs(az) * 1.5f;
        az = std::copysign(scaled < halfPi ? scaled : halfPi, az);
    }
    const float ce = std::cos(ev);
    const float s = std::sin(az) * ce;
    const float c = std::cos(az) * ce;
    const float l = kSndArDecoderW - kSndArDecoderY * s + kSndArDecoderX * c;
    const float r = kSndArDecoderW + kSndArDecoderY * s + kSndArDecoderX * c;
    // The decoder can go a hair negative just behind +/-90 degrees (at most
    // -0.01); a DSP mix factor stays non-negative.
    *left = l > 0.0f ? l : 0.0f;
    *right = r > 0.0f ? r : 0.0f;
}

// Full source -> output mix for `channels` (1 or 2) source channels at AL gain
// `gain` and AL position `pos`.
inline SndArMix SndAr_ComputeMix(int channels, float gain, const float pos[3])
{
    SndArMix mix{};
    const float g = SndAr_ClampGain(gain);
    if (channels == 2)
    {
        mix.gain[0][0] = g;
        mix.gain[1][1] = g;
        return mix;
    }
    float l = 0.0f, r = 0.0f;
    SndAr_PanMono(pos[0], pos[1], pos[2], &l, &r);
    mix.gain[0][0] = g * l;
    mix.gain[0][1] = g * r;
    return mix;
}
