#ifndef SWITCH_RUMBLE_HD_H
#define SWITCH_RUMBLE_HD_H

// Pure HD Rumble player: effects are short keyframe tracks that vary
// amplitude and frequency per band over time; concurrent voices are mixed
// per motor. No libnx dependency so it runs under the host test.

#include <stdint.h>
#include <stddef.h>

#define SWITCH_RUMBLE_HD_MAX_KEYS 6
#define SWITCH_RUMBLE_HD_MAX_VOICES 8

#define SWITCH_RUMBLE_HD_LOW_MIN_HZ 40.0f
#define SWITCH_RUMBLE_HD_LOW_MAX_HZ 640.0f
#define SWITCH_RUMBLE_HD_HIGH_MIN_HZ 80.0f
#define SWITCH_RUMBLE_HD_HIGH_MAX_HZ 1280.0f

// Summed bands are clamped here; full-scale HD Rumble clips and feels harsh.
#define SWITCH_RUMBLE_HD_MIX_CAP 0.85f

// Shots of one weapon class closer than this play the lighter automatic
// variant, so sustained fire is a rattle instead of a wall of thumps.
#define SWITCH_RUMBLE_HD_AUTO_WINDOW_SEC 0.14f

// How much the far motor is attenuated for a fully one-sided event.
#define SWITCH_RUMBLE_HD_PAN_FAR_GAIN 0.25f

typedef enum SwitchRumbleHdEffectId
{
    SWITCH_RUMBLE_HD_PISTOL,
    SWITCH_RUMBLE_HD_RIFLE,
    SWITCH_RUMBLE_HD_RIFLE_AUTO,
    SWITCH_RUMBLE_HD_SMG,
    SWITCH_RUMBLE_HD_SMG_AUTO,
    SWITCH_RUMBLE_HD_MG,
    SWITCH_RUMBLE_HD_SHOTGUN,
    SWITCH_RUMBLE_HD_SNIPER,
    SWITCH_RUMBLE_HD_LAUNCHER,
    SWITCH_RUMBLE_HD_TURRET,
    SWITCH_RUMBLE_HD_MELEE,
    SWITCH_RUMBLE_HD_THROW,
    SWITCH_RUMBLE_HD_EXPLOSION,
    SWITCH_RUMBLE_HD_DAMAGE,
    SWITCH_RUMBLE_HD_LAND,
    SWITCH_RUMBLE_HD_LAND_HARD,
    SWITCH_RUMBLE_HD_EFFECT_COUNT
} SwitchRumbleHdEffectId;

typedef struct SwitchRumbleHdKey
{
    float t;
    float ampLow;
    float freqLow;
    float ampHigh;
    float freqHigh;
} SwitchRumbleHdKey;

typedef struct SwitchRumbleHdEffect
{
    int32_t keyCount;
    // Same effect retriggering restarts its voice instead of stacking.
    int32_t retrigger;
    SwitchRumbleHdKey keys[SWITCH_RUMBLE_HD_MAX_KEYS];
} SwitchRumbleHdEffect;

// The last key of every effect is silent, so a voice ends at its final t.
static const SwitchRumbleHdEffect kSwitchRumbleHdEffects[SWITCH_RUMBLE_HD_EFFECT_COUNT] = {
    // PISTOL: sharp high click, little body.
    {3, 0, {{0.00f, 0.10f, 140.0f, 0.70f, 700.0f},
            {0.03f, 0.10f, 140.0f, 0.40f, 500.0f},
            {0.07f, 0.00f, 140.0f, 0.00f, 320.0f}}},
    // RIFLE: click then a short low thump.
    {4, 1, {{0.00f, 0.20f, 150.0f, 0.60f, 640.0f},
            {0.02f, 0.50f, 100.0f, 0.30f, 450.0f},
            {0.08f, 0.20f, 80.0f, 0.00f, 300.0f},
            {0.12f, 0.00f, 80.0f, 0.00f, 300.0f}}},
    {3, 1, {{0.00f, 0.10f, 150.0f, 0.50f, 600.0f},
            {0.03f, 0.30f, 110.0f, 0.15f, 400.0f},
            {0.06f, 0.00f, 110.0f, 0.00f, 300.0f}}},
    // SMG: lighter than the rifle, snappier.
    {3, 1, {{0.00f, 0.10f, 160.0f, 0.55f, 640.0f},
            {0.02f, 0.30f, 120.0f, 0.25f, 450.0f},
            {0.06f, 0.00f, 120.0f, 0.00f, 300.0f}}},
    {3, 1, {{0.00f, 0.05f, 160.0f, 0.45f, 640.0f},
            {0.02f, 0.18f, 130.0f, 0.15f, 450.0f},
            {0.05f, 0.00f, 130.0f, 0.00f, 300.0f}}},
    // MG: heavier chop that rides the fire rate.
    {3, 1, {{0.00f, 0.30f, 100.0f, 0.50f, 500.0f},
            {0.05f, 0.40f, 90.0f, 0.20f, 350.0f},
            {0.09f, 0.00f, 90.0f, 0.00f, 300.0f}}},
    // SHOTGUN: heavy thump, decaying tail.
    {5, 0, {{0.00f, 0.55f, 90.0f, 0.50f, 500.0f},
            {0.03f, 0.85f, 70.0f, 0.35f, 400.0f},
            {0.12f, 0.45f, 60.0f, 0.10f, 300.0f},
            {0.25f, 0.15f, 50.0f, 0.00f, 250.0f},
            {0.35f, 0.00f, 50.0f, 0.00f, 250.0f}}},
    // SNIPER: one big thump with a slow tail.
    {5, 0, {{0.00f, 0.40f, 120.0f, 0.60f, 600.0f},
            {0.03f, 0.80f, 80.0f, 0.30f, 400.0f},
            {0.15f, 0.40f, 60.0f, 0.10f, 300.0f},
            {0.30f, 0.12f, 50.0f, 0.00f, 250.0f},
            {0.45f, 0.00f, 50.0f, 0.00f, 250.0f}}},
    // LAUNCHER: big boom, long rumble decay.
    {5, 0, {{0.00f, 0.60f, 80.0f, 0.40f, 400.0f},
            {0.05f, 0.90f, 55.0f, 0.30f, 300.0f},
            {0.25f, 0.60f, 45.0f, 0.15f, 250.0f},
            {0.60f, 0.25f, 40.0f, 0.05f, 200.0f},
            {0.90f, 0.00f, 40.0f, 0.00f, 200.0f}}},
    // TURRET: slow heavy chop.
    {3, 1, {{0.00f, 0.45f, 80.0f, 0.40f, 420.0f},
            {0.06f, 0.50f, 70.0f, 0.20f, 320.0f},
            {0.10f, 0.00f, 70.0f, 0.00f, 300.0f}}},
    // MELEE: dull knock.
    {3, 0, {{0.00f, 0.50f, 70.0f, 0.15f, 200.0f},
            {0.05f, 0.40f, 60.0f, 0.05f, 200.0f},
            {0.12f, 0.00f, 60.0f, 0.00f, 200.0f}}},
    // THROW: light tick.
    {2, 0, {{0.00f, 0.15f, 100.0f, 0.10f, 300.0f},
            {0.08f, 0.00f, 100.0f, 0.00f, 300.0f}}},
    // EXPLOSION: low boom plus a high crackle that dies first.
    {6, 0, {{0.00f, 0.70f, 80.0f, 0.45f, 650.0f},
            {0.04f, 0.90f, 55.0f, 0.50f, 500.0f},
            {0.20f, 0.55f, 50.0f, 0.30f, 350.0f},
            {0.45f, 0.25f, 45.0f, 0.12f, 250.0f},
            {0.70f, 0.08f, 40.0f, 0.00f, 200.0f},
            {0.85f, 0.00f, 40.0f, 0.00f, 200.0f}}},
    // DAMAGE: short hit.
    {4, 0, {{0.00f, 0.40f, 120.0f, 0.60f, 520.0f},
            {0.05f, 0.65f, 90.0f, 0.35f, 400.0f},
            {0.15f, 0.25f, 70.0f, 0.08f, 300.0f},
            {0.25f, 0.00f, 70.0f, 0.00f, 300.0f}}},
    // LAND: soft thud.
    {3, 0, {{0.00f, 0.35f, 70.0f, 0.10f, 200.0f},
            {0.06f, 0.25f, 55.0f, 0.00f, 200.0f},
            {0.14f, 0.00f, 55.0f, 0.00f, 200.0f}}},
    {4, 0, {{0.00f, 0.70f, 70.0f, 0.30f, 250.0f},
            {0.05f, 0.80f, 50.0f, 0.15f, 220.0f},
            {0.20f, 0.35f, 45.0f, 0.00f, 200.0f},
            {0.35f, 0.00f, 45.0f, 0.00f, 200.0f}}},
};

typedef struct SwitchRumbleHdVoice
{
    int32_t active;
    int32_t effect;
    float elapsed;
    float scale;
    // -1 = hit from the left (left motor strongest), +1 = right, 0 = centered.
    float balance;
} SwitchRumbleHdVoice;

typedef struct SwitchRumbleHdBand
{
    float ampLow;
    float freqLow;
    float ampHigh;
    float freqHigh;
} SwitchRumbleHdBand;

typedef struct SwitchRumbleHdOut
{
    SwitchRumbleHdBand left;
    SwitchRumbleHdBand right;
} SwitchRumbleHdOut;

typedef struct SwitchRumbleHdPlayer
{
    SwitchRumbleHdVoice voices[SWITCH_RUMBLE_HD_MAX_VOICES];
    int32_t lastFireClass;
    float sinceFire;
} SwitchRumbleHdPlayer;

static inline float SwitchRumbleHd_Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline void SwitchRumbleHd_Reset(SwitchRumbleHdPlayer *p)
{
    int32_t i;
    if (p == NULL)
        return;
    for (i = 0; i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
    {
        p->voices[i].active = 0;
        p->voices[i].elapsed = 0.0f;
        p->voices[i].scale = 0.0f;
        p->voices[i].balance = 0.0f;
        p->voices[i].effect = 0;
    }
    p->lastFireClass = -1;
    p->sinceFire = 1000.0f;
}

// Replays the voice of the same effect if the effect retriggers; otherwise
// takes a free slot, or steals the voice furthest through its track.
static inline void SwitchRumbleHd_Trigger(SwitchRumbleHdPlayer *p, int32_t effect, float scale,
                                          float balance)
{
    int32_t i;
    int32_t slot = -1;
    float bestProgress = -1.0f;

    if (p == NULL || effect < 0 || effect >= SWITCH_RUMBLE_HD_EFFECT_COUNT || scale <= 0.0f)
        return;

    if (kSwitchRumbleHdEffects[effect].retrigger)
    {
        for (i = 0; i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
        {
            if (p->voices[i].active && p->voices[i].effect == effect)
            {
                slot = i;
                break;
            }
        }
    }
    for (i = 0; slot < 0 && i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
    {
        if (!p->voices[i].active)
            slot = i;
    }
    for (i = 0; slot < 0 && i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
    {
        const SwitchRumbleHdEffect *e = &kSwitchRumbleHdEffects[p->voices[i].effect];
        const float progress = p->voices[i].elapsed / e->keys[e->keyCount - 1].t;
        if (progress > bestProgress)
        {
            bestProgress = progress;
            slot = i;
        }
    }

    p->voices[slot].active = 1;
    p->voices[slot].effect = effect;
    p->voices[slot].elapsed = 0.0f;
    p->voices[slot].scale = SwitchRumbleHd_Clamp(scale, 0.0f, 1.0f);
    p->voices[slot].balance = SwitchRumbleHd_Clamp(balance, -1.0f, 1.0f);
}

// weapClass uses the SwitchRumbleWeaponClass values (RIFLE 0 .. TURRET 7).
static inline void SwitchRumbleHd_TriggerWeaponFire(SwitchRumbleHdPlayer *p, int32_t weapClass,
                                                    int32_t boltAction)
{
    int32_t effect;
    const int32_t rapid = p != NULL && p->lastFireClass == weapClass &&
                          p->sinceFire < SWITCH_RUMBLE_HD_AUTO_WINDOW_SEC;

    switch (weapClass)
    {
    case 0: effect = boltAction ? SWITCH_RUMBLE_HD_SNIPER : (rapid ? SWITCH_RUMBLE_HD_RIFLE_AUTO : SWITCH_RUMBLE_HD_RIFLE); break;
    case 1: effect = SWITCH_RUMBLE_HD_MG; break;
    case 2: effect = rapid ? SWITCH_RUMBLE_HD_SMG_AUTO : SWITCH_RUMBLE_HD_SMG; break;
    case 3: effect = SWITCH_RUMBLE_HD_SHOTGUN; break;
    case 4: effect = SWITCH_RUMBLE_HD_PISTOL; break;
    case 6: effect = SWITCH_RUMBLE_HD_LAUNCHER; break;
    case 7: effect = SWITCH_RUMBLE_HD_TURRET; break;
    default: effect = SWITCH_RUMBLE_HD_RIFLE; break;
    }
    if (p == NULL)
        return;
    p->lastFireClass = weapClass;
    p->sinceFire = 0.0f;
    SwitchRumbleHd_Trigger(p, effect, 1.0f, 0.0f);
}

// damage 0..100, side -1 (from the left) .. +1 (from the right).
static inline void SwitchRumbleHd_TriggerDamage(SwitchRumbleHdPlayer *p, int32_t damage, float side)
{
    const float t = SwitchRumbleHd_Clamp((float)damage / 100.0f, 0.0f, 1.0f);
    SwitchRumbleHd_Trigger(p, SWITCH_RUMBLE_HD_DAMAGE, 0.5f + 0.5f * t, side);
}

static inline void SwitchRumbleHd_TriggerExplosion(SwitchRumbleHdPlayer *p, float distance, float radius)
{
    float falloff;
    float shaped;
    if (radius <= 0.0f || distance >= radius)
        return;
    falloff = SwitchRumbleHd_Clamp(1.0f - distance / radius, 0.0f, 1.0f);
    shaped = falloff * falloff * (3.0f - 2.0f * falloff);
    SwitchRumbleHd_Trigger(p, SWITCH_RUMBLE_HD_EXPLOSION, 0.3f + 0.7f * shaped, 0.0f);
}

static inline void SwitchRumbleHd_EvalEffect(const SwitchRumbleHdEffect *e, float t, SwitchRumbleHdKey *out)
{
    int32_t i;
    for (i = 1; i < e->keyCount; ++i)
    {
        if (t < e->keys[i].t)
        {
            const SwitchRumbleHdKey *a = &e->keys[i - 1];
            const SwitchRumbleHdKey *b = &e->keys[i];
            const float f = (t - a->t) / (b->t - a->t);
            out->t = t;
            out->ampLow = a->ampLow + (b->ampLow - a->ampLow) * f;
            out->freqLow = a->freqLow + (b->freqLow - a->freqLow) * f;
            out->ampHigh = a->ampHigh + (b->ampHigh - a->ampHigh) * f;
            out->freqHigh = a->freqHigh + (b->freqHigh - a->freqHigh) * f;
            return;
        }
    }
    *out = e->keys[e->keyCount - 1];
}

static inline void SwitchRumbleHd_BandReset(SwitchRumbleHdBand *b)
{
    b->ampLow = 0.0f;
    b->freqLow = 160.0f;
    b->ampHigh = 0.0f;
    b->freqHigh = 320.0f;
}

// Advances every voice and mixes per motor: amplitudes sum up to the mix
// cap, and each band takes its frequency from its loudest contributor.
static inline void SwitchRumbleHd_Advance(SwitchRumbleHdPlayer *p, float dt, SwitchRumbleHdOut *out)
{
    int32_t i;
    float domLowAmp[2] = {0.0f, 0.0f};
    float domHighAmp[2] = {0.0f, 0.0f};
    SwitchRumbleHdBand *motors[2];

    if (out == NULL)
        return;
    motors[0] = &out->left;
    motors[1] = &out->right;
    SwitchRumbleHd_BandReset(&out->left);
    SwitchRumbleHd_BandReset(&out->right);
    if (p == NULL)
        return;
    if (dt < 0.0f)
        dt = 0.0f;
    p->sinceFire += dt;

    for (i = 0; i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
    {
        SwitchRumbleHdVoice *v = &p->voices[i];
        const SwitchRumbleHdEffect *e;
        SwitchRumbleHdKey k;
        float gain[2];
        int32_t m;

        if (!v->active)
            continue;
        e = &kSwitchRumbleHdEffects[v->effect];
        v->elapsed += dt;
        if (v->elapsed >= e->keys[e->keyCount - 1].t)
        {
            v->active = 0;
            continue;
        }
        SwitchRumbleHd_EvalEffect(e, v->elapsed, &k);
        gain[0] = 1.0f - (1.0f - SWITCH_RUMBLE_HD_PAN_FAR_GAIN) * (v->balance > 0.0f ? v->balance : 0.0f);
        gain[1] = 1.0f - (1.0f - SWITCH_RUMBLE_HD_PAN_FAR_GAIN) * (v->balance < 0.0f ? -v->balance : 0.0f);
        for (m = 0; m < 2; ++m)
        {
            const float aLow = k.ampLow * v->scale * gain[m];
            const float aHigh = k.ampHigh * v->scale * gain[m];
            motors[m]->ampLow += aLow;
            motors[m]->ampHigh += aHigh;
            if (aLow > domLowAmp[m])
            {
                domLowAmp[m] = aLow;
                motors[m]->freqLow = k.freqLow;
            }
            if (aHigh > domHighAmp[m])
            {
                domHighAmp[m] = aHigh;
                motors[m]->freqHigh = k.freqHigh;
            }
        }
    }

    for (i = 0; i < 2; ++i)
    {
        motors[i]->ampLow = SwitchRumbleHd_Clamp(motors[i]->ampLow, 0.0f, SWITCH_RUMBLE_HD_MIX_CAP);
        motors[i]->ampHigh = SwitchRumbleHd_Clamp(motors[i]->ampHigh, 0.0f, SWITCH_RUMBLE_HD_MIX_CAP);
        motors[i]->freqLow = SwitchRumbleHd_Clamp(motors[i]->freqLow, SWITCH_RUMBLE_HD_LOW_MIN_HZ, SWITCH_RUMBLE_HD_LOW_MAX_HZ);
        motors[i]->freqHigh = SwitchRumbleHd_Clamp(motors[i]->freqHigh, SWITCH_RUMBLE_HD_HIGH_MIN_HZ, SWITCH_RUMBLE_HD_HIGH_MAX_HZ);
    }
}

// Scales amplitudes by the user intensity (zero when disabled).
static inline void SwitchRumbleHd_ApplyIntensity(SwitchRumbleHdOut *out, float intensity, int32_t enabled)
{
    SwitchRumbleHdBand *b[2];
    int32_t i;
    if (out == NULL)
        return;
    b[0] = &out->left;
    b[1] = &out->right;
    for (i = 0; i < 2; ++i)
    {
        if (!enabled || intensity <= 0.0f)
        {
            b[i]->ampLow = 0.0f;
            b[i]->ampHigh = 0.0f;
        }
        else
        {
            b[i]->ampLow = SwitchRumbleHd_Clamp(b[i]->ampLow * intensity, 0.0f, 1.0f);
            b[i]->ampHigh = SwitchRumbleHd_Clamp(b[i]->ampHigh * intensity, 0.0f, 1.0f);
        }
    }
}

// Send only on a real change, plus a bounded keepalive while non-silent.
static inline int32_t SwitchRumbleHd_ShouldSend(const SwitchRumbleHdBand *prev, const SwitchRumbleHdBand *cur,
                                                uint32_t msSinceSend)
{
    const float dA = 0.01f;
    const float dF = 3.0f;
    float d;
    const int32_t silent = cur->ampLow <= 0.0f && cur->ampHigh <= 0.0f;
    const int32_t wasSilent = prev->ampLow <= 0.0f && prev->ampHigh <= 0.0f;

    if (silent)
        return !wasSilent;
    if (msSinceSend >= 100)
        return 1;
    d = cur->ampLow - prev->ampLow;
    if (d > dA || d < -dA)
        return 1;
    d = cur->ampHigh - prev->ampHigh;
    if (d > dA || d < -dA)
        return 1;
    d = cur->freqLow - prev->freqLow;
    if (d > dF || d < -dF)
        return 1;
    d = cur->freqHigh - prev->freqHigh;
    return d > dF || d < -dF;
}

#endif
