#ifndef SWITCH_RUMBLE_HD_H
#define SWITCH_RUMBLE_HD_H

// Pure HD Rumble player: effects are short keyframe tracks that vary
// amplitude and frequency per band over time; concurrent voices are mixed
// per motor. No libnx dependency so it runs under the host test.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

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

// A script rumble and a port effect of the same category (damage, blast)
// that start within this window describe one event: the second is dropped.
#define SWITCH_RUMBLE_HD_DEDUPE_WINDOW_SEC 0.30f

typedef enum SwitchRumbleHdSource
{
    SWITCH_RUMBLE_SRC_WEAPON,
    SWITCH_RUMBLE_SRC_DAMAGE,
    SWITCH_RUMBLE_SRC_EXPLOSION,
    SWITCH_RUMBLE_SRC_LAND,
    SWITCH_RUMBLE_SRC_MELEE,
    SWITCH_RUMBLE_SRC_SCRIPT,
    SWITCH_RUMBLE_SRC_RELOAD,
    SWITCH_RUMBLE_SRC_COUNT
} SwitchRumbleHdSource;

// Dedupe categories shared by port effects and script rumbles.
typedef enum SwitchRumbleHdCategory
{
    SWITCH_RUMBLE_CAT_NONE,
    SWITCH_RUMBLE_CAT_DAMAGE,
    SWITCH_RUMBLE_CAT_BLAST,
    SWITCH_RUMBLE_CAT_COUNT
} SwitchRumbleHdCategory;

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
    // Reload stages: subtle, high band, short.
    SWITCH_RUMBLE_HD_RELOAD_MAG_OUT,
    SWITCH_RUMBLE_HD_RELOAD_MAG_IN,
    SWITCH_RUMBLE_HD_RELOAD_BOLT,
    // Script rumble effects (port-authored curves for the retail names).
    SWITCH_RUMBLE_HD_SCRIPT_TANK,
    SWITCH_RUMBLE_HD_SCRIPT_ROTOR,
    SWITCH_RUMBLE_HD_SCRIPT_BLAST,
    SWITCH_RUMBLE_HD_SCRIPT_DAMAGE_LIGHT,
    SWITCH_RUMBLE_HD_SCRIPT_DAMAGE_HEAVY,
    SWITCH_RUMBLE_HD_SCRIPT_MINIGUN,
    SWITCH_RUMBLE_HD_SCRIPT_LOCK,
    SWITCH_RUMBLE_HD_SCRIPT_GENERIC,
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
    // RELOAD_MAG_OUT: faint high-band click and slide.
    {3, 0, {{0.00f, 0.00f, 140.0f, 0.22f, 760.0f},
            {0.03f, 0.05f, 140.0f, 0.12f, 620.0f},
            {0.07f, 0.00f, 140.0f, 0.00f, 500.0f}}},
    // RELOAD_MAG_IN: slightly firmer seat, still light.
    {4, 0, {{0.00f, 0.00f, 140.0f, 0.18f, 700.0f},
            {0.02f, 0.10f, 120.0f, 0.30f, 640.0f},
            {0.06f, 0.04f, 120.0f, 0.08f, 500.0f},
            {0.10f, 0.00f, 120.0f, 0.00f, 500.0f}}},
    // RELOAD_BOLT: two quick ticks, chamber then lock.
    {5, 0, {{0.00f, 0.00f, 140.0f, 0.25f, 740.0f},
            {0.03f, 0.00f, 140.0f, 0.05f, 600.0f},
            {0.06f, 0.08f, 120.0f, 0.28f, 700.0f},
            {0.10f, 0.00f, 120.0f, 0.00f, 500.0f},
            {0.12f, 0.00f, 120.0f, 0.00f, 500.0f}}},
    // SCRIPT_TANK: engine idle cycle, 0.40 s so it loops as a steady rumble.
    {4, 1, {{0.00f, 0.30f, 60.0f, 0.04f, 200.0f},
            {0.10f, 0.45f, 50.0f, 0.06f, 200.0f},
            {0.25f, 0.30f, 55.0f, 0.04f, 200.0f},
            {0.40f, 0.30f, 60.0f, 0.04f, 200.0f}}},
    // SCRIPT_ROTOR: fast chop (helicopter / jet), 0.20 s cycle.
    {3, 1, {{0.00f, 0.15f, 80.0f, 0.30f, 300.0f},
            {0.10f, 0.35f, 70.0f, 0.12f, 250.0f},
            {0.20f, 0.15f, 80.0f, 0.30f, 300.0f}}},
    // SCRIPT_BLAST: large distant blast with a long tail.
    {5, 0, {{0.00f, 0.80f, 60.0f, 0.40f, 400.0f},
            {0.08f, 0.95f, 45.0f, 0.35f, 300.0f},
            {0.50f, 0.55f, 40.0f, 0.15f, 250.0f},
            {1.10f, 0.15f, 40.0f, 0.00f, 200.0f},
            {1.50f, 0.00f, 40.0f, 0.00f, 200.0f}}},
    // SCRIPT_DAMAGE_LIGHT: brief jolt.
    {3, 0, {{0.00f, 0.30f, 110.0f, 0.40f, 480.0f},
            {0.06f, 0.20f, 90.0f, 0.10f, 360.0f},
            {0.16f, 0.00f, 90.0f, 0.00f, 300.0f}}},
    // SCRIPT_DAMAGE_HEAVY: heavy jolt, cycle-able for loops (0.30 s).
    {4, 1, {{0.00f, 0.60f, 90.0f, 0.55f, 450.0f},
            {0.06f, 0.80f, 70.0f, 0.30f, 350.0f},
            {0.18f, 0.40f, 60.0f, 0.10f, 300.0f},
            {0.30f, 0.00f, 60.0f, 0.00f, 300.0f}}},
    // SCRIPT_MINIGUN: spin-up chatter, 0.12 s cycle.
    {3, 1, {{0.00f, 0.25f, 90.0f, 0.35f, 420.0f},
            {0.06f, 0.40f, 80.0f, 0.15f, 320.0f},
            {0.12f, 0.25f, 90.0f, 0.35f, 420.0f}}},
    // SCRIPT_LOCK: light high pulse, 0.30 s cycle (missile lock tone).
    {3, 1, {{0.00f, 0.00f, 140.0f, 0.35f, 800.0f},
            {0.10f, 0.00f, 140.0f, 0.05f, 800.0f},
            {0.30f, 0.00f, 140.0f, 0.35f, 800.0f}}},
    // SCRIPT_GENERIC: medium thump for names with no known pattern.
    {4, 0, {{0.00f, 0.45f, 90.0f, 0.30f, 400.0f},
            {0.05f, 0.60f, 70.0f, 0.20f, 320.0f},
            {0.20f, 0.20f, 60.0f, 0.05f, 250.0f},
            {0.35f, 0.00f, 60.0f, 0.00f, 250.0f}}},
};

// Which counter an effect started through the generic Trigger belongs to.
static inline int32_t SwitchRumbleHd_EffectSource(int32_t effect)
{
    if (effect >= SWITCH_RUMBLE_HD_PISTOL && effect <= SWITCH_RUMBLE_HD_TURRET)
        return SWITCH_RUMBLE_SRC_WEAPON;
    switch (effect)
    {
    case SWITCH_RUMBLE_HD_MELEE: return SWITCH_RUMBLE_SRC_MELEE;
    case SWITCH_RUMBLE_HD_THROW: return SWITCH_RUMBLE_SRC_WEAPON;
    case SWITCH_RUMBLE_HD_EXPLOSION: return SWITCH_RUMBLE_SRC_EXPLOSION;
    case SWITCH_RUMBLE_HD_DAMAGE: return SWITCH_RUMBLE_SRC_DAMAGE;
    case SWITCH_RUMBLE_HD_LAND:
    case SWITCH_RUMBLE_HD_LAND_HARD: return SWITCH_RUMBLE_SRC_LAND;
    case SWITCH_RUMBLE_HD_RELOAD_MAG_OUT:
    case SWITCH_RUMBLE_HD_RELOAD_MAG_IN:
    case SWITCH_RUMBLE_HD_RELOAD_BOLT: return SWITCH_RUMBLE_SRC_RELOAD;
    default: return SWITCH_RUMBLE_SRC_SCRIPT;
    }
}

typedef struct SwitchRumbleHdVoice
{
    // Script voices: key identifies (entity, rumble) for stop; loop restarts
    // the track at its end; delay holds the voice silent before it starts;
    // radius > 0 attenuates by distance from the listener to origin.
    int32_t key;
    int32_t loop;
    int32_t category;
    float delay;
    float radius;
    float origin[3];
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
    float now;
    float listener[3];
    // Last time (player clock) a port effect / a script rumble started in
    // each dedupe category.
    float portAt[SWITCH_RUMBLE_CAT_COUNT];
    float scriptAt[SWITCH_RUMBLE_CAT_COUNT];
    // Effects started, by source, and the ones dropped as duplicates.
    uint32_t started[SWITCH_RUMBLE_SRC_COUNT];
    uint32_t deduped;
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
        p->voices[i].key = 0;
        p->voices[i].loop = 0;
        p->voices[i].category = 0;
        p->voices[i].delay = 0.0f;
        p->voices[i].radius = 0.0f;
        p->voices[i].origin[0] = p->voices[i].origin[1] = p->voices[i].origin[2] = 0.0f;
    }
    p->lastFireClass = -1;
    p->sinceFire = 1000.0f;
    // Dedupe history is relative to the player clock, so a reset clears both.
    p->now = 0.0f;
    for (i = 0; i < SWITCH_RUMBLE_CAT_COUNT; ++i)
        p->portAt[i] = p->scriptAt[i] = -1000.0f;
}

// Reset plus the listener and the run counters (which survive a Reset).
static inline void SwitchRumbleHd_Init(SwitchRumbleHdPlayer *p)
{
    int32_t i;
    if (p == NULL)
        return;
    SwitchRumbleHd_Reset(p);
    p->listener[0] = p->listener[1] = p->listener[2] = 0.0f;
    for (i = 0; i < SWITCH_RUMBLE_SRC_COUNT; ++i)
        p->started[i] = 0;
    p->deduped = 0;
}

typedef struct SwitchRumbleHdOpts
{
    int32_t key;       // non-zero: addressable by SwitchRumbleHd_Stop
    int32_t loop;      // restart the track at its end until stopped
    int32_t category;  // SwitchRumbleHdCategory, for duplicate suppression
    float delay;       // seconds of silence before the track starts
    float radius;      // > 0: distance falloff from the listener to origin
    float origin[3];
} SwitchRumbleHdOpts;

static inline float SwitchRumbleHd_Dist(const float *a, const float *b)
{
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    // Newton iteration: keeps this header free of libm.
    const float d2 = dx * dx + dy * dy + dz * dz;
    float g = d2 > 1.0f ? d2 : 1.0f;
    int32_t i;
    if (d2 <= 0.0f)
        return 0.0f;
    for (i = 0; i < 24; ++i)
        g = 0.5f * (g + d2 / g);
    return g;
}

// Starts a voice.  A keyed voice with the same key restarts in place; an
// effect that retriggers restarts its voice; otherwise a free slot is taken,
// or the voice furthest through its track is stolen (loops are stolen last).
static inline void SwitchRumbleHd_TriggerEx(SwitchRumbleHdPlayer *p, int32_t effect, float scale,
                                            float balance, const SwitchRumbleHdOpts *opts)
{
    int32_t i;
    int32_t slot = -1;
    float bestProgress = -1.0f;
    SwitchRumbleHdOpts none = {0, 0, 0, 0.0f, 0.0f, {0.0f, 0.0f, 0.0f}};

    if (p == NULL || effect < 0 || effect >= SWITCH_RUMBLE_HD_EFFECT_COUNT || scale <= 0.0f)
        return;
    if (opts == NULL)
        opts = &none;

    if (opts->key != 0)
    {
        for (i = 0; i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
        {
            if (p->voices[i].active && p->voices[i].key == opts->key)
            {
                slot = i;
                break;
            }
        }
    }
    else if (kSwitchRumbleHdEffects[effect].retrigger)
    {
        for (i = 0; i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
        {
            if (p->voices[i].active && p->voices[i].key == 0 && p->voices[i].effect == effect)
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
        float progress = p->voices[i].elapsed / e->keys[e->keyCount - 1].t;
        if (p->voices[i].loop)
            progress -= 1000.0f;
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
    p->voices[slot].key = opts->key;
    p->voices[slot].loop = opts->loop;
    p->voices[slot].category = opts->category;
    p->voices[slot].delay = opts->delay > 0.0f ? opts->delay : 0.0f;
    p->voices[slot].radius = opts->radius;
    p->voices[slot].origin[0] = opts->origin[0];
    p->voices[slot].origin[1] = opts->origin[1];
    p->voices[slot].origin[2] = opts->origin[2];
    p->started[SwitchRumbleHd_EffectSource(effect)]++;
}

static inline void SwitchRumbleHd_Trigger(SwitchRumbleHdPlayer *p, int32_t effect, float scale,
                                          float balance)
{
    SwitchRumbleHd_TriggerEx(p, effect, scale, balance, NULL);
}

// Port-built effects of a dedupe category yield to a script rumble that
// started just before them, and record themselves for the reverse case.
// Returns 1 when the port effect must be dropped.
static inline int32_t SwitchRumbleHd_PortEffectSuppressed(SwitchRumbleHdPlayer *p, int32_t category)
{
    if (p == NULL || category <= 0 || category >= SWITCH_RUMBLE_CAT_COUNT)
        return 0;
    if (p->now - p->scriptAt[category] < SWITCH_RUMBLE_HD_DEDUPE_WINDOW_SEC)
    {
        p->deduped++;
        return 1;
    }
    p->portAt[category] = p->now;
    return 0;
}

// Starts a script rumble voice (category-deduped against port effects).
static inline void SwitchRumbleHd_TriggerScript(SwitchRumbleHdPlayer *p, int32_t effect, float scale,
                                                const SwitchRumbleHdOpts *opts)
{
    if (p == NULL || opts == NULL)
        return;
    if (!opts->loop && opts->category > 0 && opts->category < SWITCH_RUMBLE_CAT_COUNT)
    {
        if (p->now - p->portAt[opts->category] < SWITCH_RUMBLE_HD_DEDUPE_WINDOW_SEC)
        {
            p->deduped++;
            return;
        }
        p->scriptAt[opts->category] = p->now;
    }
    SwitchRumbleHd_TriggerEx(p, effect, scale, 0.0f, opts);
}

// Stops every voice with this key (0 never matches).  Returns the count.
static inline int32_t SwitchRumbleHd_Stop(SwitchRumbleHdPlayer *p, int32_t key)
{
    int32_t i;
    int32_t n = 0;
    if (p == NULL || key == 0)
        return 0;
    for (i = 0; i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
    {
        if (p->voices[i].active && p->voices[i].key == key)
        {
            p->voices[i].active = 0;
            ++n;
        }
    }
    return n;
}

// Stops every keyed (script) voice; port one-shots keep playing.
static inline void SwitchRumbleHd_StopAllKeyed(SwitchRumbleHdPlayer *p)
{
    int32_t i;
    if (p == NULL)
        return;
    for (i = 0; i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
    {
        if (p->voices[i].key != 0)
            p->voices[i].active = 0;
    }
}

// Drops non-looping voices (focus loss, menu): they would otherwise sound
// late; loops stay so they resume with the game.
static inline void SwitchRumbleHd_DropOneShots(SwitchRumbleHdPlayer *p)
{
    int32_t i;
    if (p == NULL)
        return;
    for (i = 0; i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
    {
        if (!p->voices[i].loop)
            p->voices[i].active = 0;
    }
}

static inline void SwitchRumbleHd_SetListener(SwitchRumbleHdPlayer *p, const float *origin)
{
    if (p == NULL || origin == NULL)
        return;
    p->listener[0] = origin[0];
    p->listener[1] = origin[1];
    p->listener[2] = origin[2];
}

// Moves a keyed voice's falloff origin (entity-bound loops follow the entity).
static inline void SwitchRumbleHd_SetVoiceOrigin(SwitchRumbleHdPlayer *p, int32_t key, const float *origin)
{
    int32_t i;
    if (p == NULL || key == 0 || origin == NULL)
        return;
    for (i = 0; i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
    {
        if (p->voices[i].active && p->voices[i].key == key)
        {
            p->voices[i].origin[0] = origin[0];
            p->voices[i].origin[1] = origin[1];
            p->voices[i].origin[2] = origin[2];
        }
    }
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
    if (SwitchRumbleHd_PortEffectSuppressed(p, SWITCH_RUMBLE_CAT_DAMAGE))
        return;
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
    if (SwitchRumbleHd_PortEffectSuppressed(p, SWITCH_RUMBLE_CAT_BLAST))
        return;
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
    p->now += dt;

    for (i = 0; i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
    {
        SwitchRumbleHdVoice *v = &p->voices[i];
        const SwitchRumbleHdEffect *e;
        SwitchRumbleHdKey k;
        float gain[2];
        int32_t m;

        float fall = 1.0f;
        if (!v->active)
            continue;
        e = &kSwitchRumbleHdEffects[v->effect];
        if (v->delay > 0.0f)
        {
            v->delay -= dt;
            continue;
        }
        v->elapsed += dt;
        if (v->elapsed >= e->keys[e->keyCount - 1].t)
        {
            if (v->loop)
            {
                const float len = e->keys[e->keyCount - 1].t;
                while (v->elapsed >= len)
                    v->elapsed -= len;
            }
            else
            {
                v->active = 0;
                continue;
            }
        }
        if (v->radius > 0.0f)
        {
            const float d = SwitchRumbleHd_Dist(p->listener, v->origin);
            if (d >= v->radius)
                continue;
            fall = 1.0f - d / v->radius;
            fall = fall * fall * (3.0f - 2.0f * fall);
        }
        SwitchRumbleHd_EvalEffect(e, v->elapsed, &k);
        gain[0] = 1.0f - (1.0f - SWITCH_RUMBLE_HD_PAN_FAR_GAIN) * (v->balance > 0.0f ? v->balance : 0.0f);
        gain[1] = 1.0f - (1.0f - SWITCH_RUMBLE_HD_PAN_FAR_GAIN) * (v->balance < 0.0f ? -v->balance : 0.0f);
        for (m = 0; m < 2; ++m)
        {
            const float aLow = k.ampLow * v->scale * gain[m] * fall;
            const float aHigh = k.ampHigh * v->scale * gain[m] * fall;
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

// Reload stages for the local player.  `seconds` is the weapon's full reload
// duration; mag out lands at 30 %, mag in at 70 %.  A reload from empty also
// chambers a round near the end; `stage` selects which events apply.
typedef enum SwitchRumbleHdReloadKind
{
    SWITCH_RUMBLE_RELOAD_NORMAL,   // mag out, mag in
    SWITCH_RUMBLE_RELOAD_EMPTY,    // mag out, mag in, chamber
    SWITCH_RUMBLE_RELOAD_START,    // shell / first-stage insert
    SWITCH_RUMBLE_RELOAD_END,      // chamber / bolt
    SWITCH_RUMBLE_RELOAD_RECHAMBER // bolt cycle between shots
} SwitchRumbleHdReloadKind;

static inline void SwitchRumbleHd_TriggerReload(SwitchRumbleHdPlayer *p, int32_t kind, float seconds)
{
    SwitchRumbleHdOpts o = {0, 0, 0, 0.0f, 0.0f, {0.0f, 0.0f, 0.0f}};
    if (p == NULL)
        return;
    if (seconds <= 0.0f)
        seconds = 2.0f;
    switch (kind)
    {
    case SWITCH_RUMBLE_RELOAD_NORMAL:
    case SWITCH_RUMBLE_RELOAD_EMPTY:
        o.delay = 0.30f * seconds;
        SwitchRumbleHd_TriggerEx(p, SWITCH_RUMBLE_HD_RELOAD_MAG_OUT, 1.0f, 0.0f, &o);
        o.delay = 0.70f * seconds;
        SwitchRumbleHd_TriggerEx(p, SWITCH_RUMBLE_HD_RELOAD_MAG_IN, 1.0f, 0.0f, &o);
        if (kind == SWITCH_RUMBLE_RELOAD_EMPTY)
        {
            o.delay = 0.90f * seconds;
            SwitchRumbleHd_TriggerEx(p, SWITCH_RUMBLE_HD_RELOAD_BOLT, 1.0f, 0.0f, &o);
        }
        break;
    case SWITCH_RUMBLE_RELOAD_START:
        o.delay = 0.25f * seconds;
        SwitchRumbleHd_TriggerEx(p, SWITCH_RUMBLE_HD_RELOAD_MAG_IN, 1.0f, 0.0f, &o);
        break;
    case SWITCH_RUMBLE_RELOAD_END:
    case SWITCH_RUMBLE_RELOAD_RECHAMBER:
        o.delay = 0.20f * seconds;
        SwitchRumbleHd_TriggerEx(p, SWITCH_RUMBLE_HD_RELOAD_BOLT, 1.0f, 0.0f, &o);
        break;
    default: break;
    }
}

// One line for the OLED log: effects started per source and dropped dupes.
static inline int32_t SwitchRumbleHd_FormatCounters(const SwitchRumbleHdPlayer *p, char *buf, size_t size)
{
    if (p == NULL || buf == NULL || size == 0)
        return 0;
    return snprintf(buf, size,
                    "SWITCH_RUMBLE counters weapon=%u damage=%u explosion=%u land=%u melee=%u script=%u reload=%u deduped=%u\n",
                    p->started[SWITCH_RUMBLE_SRC_WEAPON], p->started[SWITCH_RUMBLE_SRC_DAMAGE],
                    p->started[SWITCH_RUMBLE_SRC_EXPLOSION], p->started[SWITCH_RUMBLE_SRC_LAND],
                    p->started[SWITCH_RUMBLE_SRC_MELEE], p->started[SWITCH_RUMBLE_SRC_SCRIPT],
                    p->started[SWITCH_RUMBLE_SRC_RELOAD], p->deduped);
}

#endif
