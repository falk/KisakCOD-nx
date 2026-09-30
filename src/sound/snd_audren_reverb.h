#pragma once
// EFX reverb on the audio DSP for the audren sound backend:
// the EFX -> I3DL2 parameter conversion and the renderer's wire
// structs for one I3DL2 reverb effect ("Reverb3d" in the renderer).  Pure
// data and arithmetic, so switch_snd_audren_test.cpp checks it on the host.
//
// Why I3DL2: CoD4 drives exactly one EFX EAX-reverb effect (snd_openal.cpp /
// SND_SetRoomtype in snd_driver_openal.cpp) loaded from openal-soft's
// efx-presets.h, which are the I3DL2/EAX environment presets written in EFX
// units.  The Switch audio renderer's I3DL2 reverb effect takes the I3DL2
// parameter set directly, so the preset maps field for field:
//
//   I3DL2 (renderer)          EFX (EAX reverb)             conversion
//   RoomGain       [mB]       AL_EAXREVERB_GAIN            2000 log10(g), [-10000, 0]
//   RoomHf         [mB]       AL_EAXREVERB_GAINHF          2000 log10(g), [-10000, 0]
//   DecayTime      [s]        AL_EAXREVERB_DECAY_TIME      same, [0.1, 20]
//   HfDecayRatio              AL_EAXREVERB_DECAY_HFRATIO   same, [0.1, 2], limited by
//                                                          AIR_ABSORPTION_GAINHF when
//                                                          DECAY_HFLIMIT (openal-soft's
//                                                          CalcLimitedHfRatio)
//   ReflectionsGain [mB]      AL_EAXREVERB_REFLECTIONS_GAIN 2000 log10(g), [-10000, 1000]
//   ReflectionDelay [s]       AL_EAXREVERB_REFLECTIONS_DELAY same, [0, 0.3]
//   ReverbGain     [mB]       AL_EAXREVERB_LATE_REVERB_GAIN 2000 log10(g), [-10000, 2000]
//   ReverbDelayTime [s]       AL_EAXREVERB_LATE_REVERB_DELAY same, [0, 0.1]
//   Diffusion      [%]        AL_EAXREVERB_DIFFUSION       100 x, [0, 100]
//   Density        [%]        AL_EAXREVERB_DENSITY         100 x, [0, 100]
//   HfReference    [Hz]       AL_EAXREVERB_HFREFERENCE     same, [20, 20000]
//   DryGain                   (none)                       0: the effect runs on a
//                                                          wet-only bus, the dry path
//                                                          is each voice's direct mix
//
// (mB = millibel; 2000 log10 is 100 x the dB amplitude ratio.  The renderer
// turns them back with 10^(mB/2000), the reference renderer's Reverb3dState.UpdateParameter.)
// EFX-only fields have no I3DL2 counterpart and are dropped: GAINLF,
// DECAY_LFRATIO, LFREFERENCE (every CoD4 preset except UNDERWATER..PSYCHOTIC
// keeps them neutral), REFLECTIONS_PAN / LATE_REVERB_PAN (all presets 0),
// ECHO_* and MODULATION_* (the three effect presets DRUGGED/DIZZY/PSYCHOTIC
// use them) and ROOM_ROLLOFF_FACTOR (the port runs AL_NONE distance, so
// openal-soft applies no rolloff either).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// EFX EAX-reverb property block (the fields SND_SetRoomtype sets), defaults
// = AL_EAXREVERB_DEFAULT_*.
struct SndArEfxReverb
{
    float density = 1.0f;
    float diffusion = 1.0f;
    float gain = 0.32f;
    float gainHF = 0.89f;
    float gainLF = 1.0f;
    float decayTime = 1.49f;
    float decayHFRatio = 0.83f;
    float decayLFRatio = 1.0f;
    float reflectionsGain = 0.05f;
    float reflectionsDelay = 0.007f;
    float reflectionsPan[3] = {0.0f, 0.0f, 0.0f};
    float lateReverbGain = 1.26f;
    float lateReverbDelay = 0.011f;
    float lateReverbPan[3] = {0.0f, 0.0f, 0.0f};
    float echoTime = 0.25f;
    float echoDepth = 0.0f;
    float modulationTime = 0.25f;
    float modulationDepth = 0.0f;
    float airAbsorptionGainHF = 0.994f;
    float hfReference = 5000.0f;
    float lfReference = 250.0f;
    float roomRolloffFactor = 0.0f;
    int decayHFLimit = 1;
};

// The renderer's I3DL2 parameter set (units as in the table above).
struct SndArI3dl2
{
    float roomGain;
    float roomHf;
    float decayTime;
    float hfDecayRatio;
    float reflectionsGain;
    float reflectionDelay;
    float reverbGain;
    float reverbDelay;
    float diffusion;
    float density;
    float hfReference;
    float dryGain;
};

inline float SndAr_Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Linear amplitude -> millibel, clamped; 0 (silence) -> lo.
inline float SndAr_GainToMb(float gain, float lo, float hi)
{
    if (!(gain > 0.0f))
        return lo;
    return SndAr_Clampf(2000.0f * std::log10(gain), lo, hi);
}

// openal-soft 1.21 alc/effects/reverb.cpp CalcLimitedHfRatio: the HF decay
// ratio is capped so late-reverb HF never decays slower than air absorption
// (0.001 = the -60 dB decay gain, 343.3 m/s the speed of sound).
inline float SndAr_LimitedHfRatio(float hfRatio, float airAbsorptionGainHF, float decayTime)
{
    const float decayLength = std::log10(airAbsorptionGainHF) * decayTime / std::log10(0.001f);
    const float limit = 1.0f / (343.3f * decayLength);
    return limit < hfRatio ? limit : hfRatio;
}

inline SndArI3dl2 SndAr_EfxToI3dl2(const SndArEfxReverb &e)
{
    SndArI3dl2 r{};
    float hfRatio = e.decayHFRatio;
    if (e.decayHFLimit && e.airAbsorptionGainHF < 1.0f && e.airAbsorptionGainHF > 0.0f)
        hfRatio = SndAr_LimitedHfRatio(hfRatio, e.airAbsorptionGainHF, e.decayTime);
    r.roomGain = SndAr_GainToMb(e.gain, -10000.0f, 0.0f);
    r.roomHf = SndAr_GainToMb(e.gainHF, -10000.0f, 0.0f);
    r.decayTime = SndAr_Clampf(e.decayTime, 0.1f, 20.0f);
    r.hfDecayRatio = SndAr_Clampf(hfRatio, 0.1f, 2.0f);
    r.reflectionsGain = SndAr_GainToMb(e.reflectionsGain, -10000.0f, 1000.0f);
    r.reflectionDelay = SndAr_Clampf(e.reflectionsDelay, 0.0f, 0.3f);
    r.reverbGain = SndAr_GainToMb(e.lateReverbGain, -10000.0f, 2000.0f);
    r.reverbDelay = SndAr_Clampf(e.lateReverbDelay, 0.0f, 0.1f);
    r.diffusion = SndAr_Clampf(e.diffusion * 100.0f, 0.0f, 100.0f);
    r.density = SndAr_Clampf(e.density * 100.0f, 0.0f, 100.0f);
    r.hfReference = SndAr_Clampf(e.hfReference, 20.0f, 20000.0f);
    r.dryGain = 0.0f;
    return r;
}

// --- renderer wire format ------------------------------------------------------
// Layouts from Ryujinx.Audio/Renderer (the reference implementation of the
// Switch audio renderer's update protocol): EffectInParameterVersion1 (0xC0),
// EffectOutStatusVersion1 (0x10), Parameter/Effect/Reverb3dParameter and
// Common/EffectType.  libnx's audrv sends no effect section at all; the
// device (snd_audren_switch.cpp) splices one in (SndAr_SpliceEffects).

enum : uint8_t
{
    kSndArEffectTypeBufferMix = 1, // EffectType.BufferMix
    kSndArEffectTypeReverb3d = 5,  // EffectType.Reverb3d (I3DL2)
};
enum : uint8_t
{
    kSndArUsageInvalid = 0, // UsageState: the DSP (re)initialises the effect state
    kSndArUsageNew = 1,     // parameters changed: the DSP re-derives its coefficients
    kSndArUsageEnabled = 2, // unchanged
};
enum : uint8_t
{
    kSndArEffectStateEnabled = 3, // EffectState (out status)
    kSndArEffectStateDisabled = 4,
};

#pragma pack(push, 1)
struct SndArReverb3dWire
{
    uint8_t input[6];     // mix-buffer indices, relative to the effect's mix
    uint8_t output[6];
    uint16_t channelCountMax;
    uint16_t channelCount;
    uint32_t reserved;
    uint32_t sampleRate;  // Hz
    float roomHf;
    float hfReference;
    float decayTime;
    float hfDecayRatio;
    float roomGain;
    float reflectionsGain;
    float reverbGain;
    float diffusion;
    float reflectionDelay;
    float reverbDelayTime;
    float density;
    float dryGain;
    uint8_t parameterStatus; // kSndArUsage*
};

struct SndArEffectInWire
{
    uint8_t type;
    uint8_t isNew;
    uint8_t isEnabled;
    uint8_t reserved1;
    int32_t mixId;
    uint64_t bufferBase;   // work buffer (CPU address inside an attached memory pool)
    uint64_t bufferSize;
    uint32_t processingOrder;
    uint32_t reserved2;
    uint8_t specific[0xA0];
};

// BufferMixParameter: output[i] += input[i] * volume[i] (mix buffers of
// the effect's mix), for i < mixesCount.
struct SndArBufferMixWire
{
    uint8_t input[24];
    uint8_t output[24];
    float volume[24];
    uint32_t mixesCount;
};

struct SndArEffectOutWire
{
    uint8_t state; // kSndArEffectState*
    uint8_t reserved[15];
};
#pragma pack(pop)

static_assert(sizeof(SndArEffectInWire) == 0xC0, "EffectInParameterVersion1 is 0xC0 bytes");
static_assert(offsetof(SndArEffectInWire, mixId) == 0x04, "EffectInParameterVersion1.MixId");
static_assert(offsetof(SndArEffectInWire, bufferBase) == 0x08, "EffectInParameterVersion1.BufferBase");
static_assert(offsetof(SndArEffectInWire, processingOrder) == 0x18, "EffectInParameterVersion1.ProcessingOrder");
static_assert(offsetof(SndArEffectInWire, specific) == 0x20, "EffectInParameterVersion1.SpecificData");
static_assert(sizeof(SndArEffectOutWire) == 0x10, "EffectOutStatusVersion1 is 0x10 bytes");
static_assert(offsetof(SndArReverb3dWire, sampleRate) == 0x14, "Reverb3dParameter.SampleRate");
static_assert(offsetof(SndArReverb3dWire, roomHf) == 0x18, "Reverb3dParameter.RoomHf");
static_assert(offsetof(SndArReverb3dWire, roomGain) == 0x28, "Reverb3dParameter.RoomGain");
static_assert(offsetof(SndArReverb3dWire, dryGain) == 0x44, "Reverb3dParameter.DryGain");
static_assert(offsetof(SndArReverb3dWire, parameterStatus) == 0x48, "Reverb3dParameter.ParameterStatus");
static_assert(sizeof(SndArReverb3dWire) <= 0xA0, "Reverb3dParameter fits the specific data");
static_assert(offsetof(SndArBufferMixWire, volume) == 0x30, "BufferMixParameter.Volumes");
static_assert(offsetof(SndArBufferMixWire, mixesCount) == 0x90, "BufferMixParameter.MixesCount");

// Fills the specific data of a stereo in-place I3DL2 reverb on mix buffers
// {bus, bus + 1} of its mix.
inline void SndAr_EncodeReverb3d(const SndArI3dl2 &p, uint8_t bus, uint32_t sampleRate, uint8_t status,
                                 SndArEffectInWire *effect)
{
    SndArReverb3dWire w{};
    w.input[0] = w.output[0] = bus;
    w.input[1] = w.output[1] = static_cast<uint8_t>(bus + 1);
    w.channelCountMax = 2;
    w.channelCount = 2;
    w.sampleRate = sampleRate;
    w.roomHf = p.roomHf;
    w.hfReference = p.hfReference;
    w.decayTime = p.decayTime;
    w.hfDecayRatio = p.hfDecayRatio;
    w.roomGain = p.roomGain;
    w.reflectionsGain = p.reflectionsGain;
    w.reverbGain = p.reverbGain;
    w.diffusion = p.diffusion;
    w.reflectionDelay = p.reflectionDelay;
    w.reverbDelayTime = p.reverbDelay;
    w.density = p.density;
    w.dryGain = p.dryGain;
    w.parameterStatus = status;
    std::memset(effect->specific, 0, sizeof(effect->specific));
    std::memcpy(effect->specific, &w, sizeof(w));
}

// Fills the specific data of a buffer mix adding buffers {bus, bus + 1}
// into {0, 1} at unit gain: folds the reverb's wet output into the dry pair
// that the sink plays.
inline void SndAr_EncodeBusFold(uint8_t bus, SndArEffectInWire *effect)
{
    SndArBufferMixWire w{};
    w.input[0] = bus;
    w.input[1] = static_cast<uint8_t>(bus + 1);
    w.output[0] = 0;
    w.output[1] = 1;
    w.volume[0] = 1.0f;
    w.volume[1] = 1.0f;
    w.mixesCount = 2;
    std::memset(effect->specific, 0, sizeof(effect->specific));
    std::memcpy(effect->specific, &w, sizeof(w));
}

// The update-data header (libnx AudioRendererUpdateDataHeader; Ryujinx
// UpdateDataHeader) as the splice needs it.
struct SndArUpdateHeader
{
    uint32_t revision;
    uint32_t behaviorSz;
    uint32_t mempoolsSz;
    uint32_t voicesSz;
    uint32_t channelsSz;
    uint32_t effectsSz;
    uint32_t mixesSz;
    uint32_t sinksSz;
    uint32_t perfmgrSz;
    uint32_t padding[6];
    uint32_t totalSz;
};
static_assert(sizeof(SndArUpdateHeader) == 0x40, "update data header is 0x40 bytes");

// Input: header | behavior | mempools | channels | voices | effects |
// (splitters) | mixes | sinks | performance.  libnx's audrv writes no effect
// section; this copies `in` to `out` with `effectsBytes` of effect records
// inserted after the voices and the header's effects/total sizes fixed.
// Returns the new size, 0 when `in` is not a well-formed effect-less update
// or `out` is too small.
inline size_t SndAr_SpliceEffects(const void *in, size_t inSize, const void *effects, size_t effectsBytes, void *out,
                                  size_t outCap)
{
    if (inSize < sizeof(SndArUpdateHeader))
        return 0;
    SndArUpdateHeader h;
    std::memcpy(&h, in, sizeof(h));
    const size_t head = sizeof(SndArUpdateHeader) + size_t(h.behaviorSz) + h.mempoolsSz + h.channelsSz + h.voicesSz;
    const size_t tail = size_t(h.mixesSz) + h.sinksSz + h.perfmgrSz;
    if (h.effectsSz != 0 || h.totalSz != inSize || head + tail != inSize || inSize + effectsBytes > outCap)
        return 0;
    unsigned char *o = static_cast<unsigned char *>(out);
    const unsigned char *i = static_cast<const unsigned char *>(in);
    std::memcpy(o, i, head);
    std::memcpy(o + head, effects, effectsBytes);
    std::memcpy(o + head + effectsBytes, i + head, tail);
    h.effectsSz = static_cast<uint32_t>(effectsBytes);
    h.totalSz = static_cast<uint32_t>(inSize + effectsBytes);
    std::memcpy(o, &h, sizeof(h));
    return inSize + effectsBytes;
}

// Output: header | mempools | voices | effects | sinks | performance |
// behavior (error info).  Copies the renderer's output back into the
// effect-less layout libnx's audrv parses (it reads only the mempool and
// voice sections, both before the effects), hands the effect statuses to
// `effectsOut` and returns the number of renderer error records in the
// behavior section (-1 when the output is malformed).
inline int SndAr_UnspliceEffects(const void *rendererOut, size_t rendererOutSize, void *libnxOut, size_t libnxOutSize,
                                 SndArEffectOutWire *effectsOut, size_t effectCount, uint32_t *firstErrorCode)
{
    if (rendererOutSize < sizeof(SndArUpdateHeader))
        return -1;
    SndArUpdateHeader h;
    std::memcpy(&h, rendererOut, sizeof(h));
    const unsigned char *r = static_cast<const unsigned char *>(rendererOut);
    const size_t head = sizeof(SndArUpdateHeader) + size_t(h.mempoolsSz) + h.voicesSz;
    const size_t fx = h.effectsSz;
    if (fx != effectCount * sizeof(SndArEffectOutWire) || head + fx > rendererOutSize)
        return -1;
    std::memcpy(effectsOut, r + head, fx);
    const size_t rest = rendererOutSize - head - fx;
    std::memset(libnxOut, 0, libnxOutSize);
    std::memcpy(libnxOut, r, head < libnxOutSize ? head : libnxOutSize);
    if (head < libnxOutSize)
        std::memcpy(static_cast<unsigned char *>(libnxOut) + head, r + head + fx, rest < libnxOutSize - head ? rest : libnxOutSize - head);
    SndArUpdateHeader lh = h;
    lh.effectsSz = 0;
    lh.totalSz = h.totalSz >= fx ? static_cast<uint32_t>(h.totalSz - fx) : 0;
    std::memcpy(libnxOut, &lh, sizeof(lh) < libnxOutSize ? sizeof(lh) : libnxOutSize);
    // Behavior section = BehaviourErrorInfoOutStatus: 10 x {u32 code, u32 pad,
    // u64 extra} then u32 count.
    const size_t behavior = head + fx + size_t(h.sinksSz) + h.perfmgrSz;
    if (h.behaviorSz < 0xA4 || behavior + 0xA4 > rendererOutSize)
        return 0;
    uint32_t count = 0;
    std::memcpy(&count, r + behavior + 0xA0, sizeof(count));
    if (count && firstErrorCode)
        std::memcpy(firstErrorCode, r + behavior, sizeof(uint32_t));
    return static_cast<int>(count);
}
