#ifndef SWITCH_RUMBLE_H
#define SWITCH_RUMBLE_H

// Pure, host-testable rumble model: no libnx dependency.  The retail SP
// source has no console rumble-graph asset (see switch_rumble.cpp's top
// comment for what was actually found), so this is the "minimal
// engine-side events" fallback the task calls for: weapon fire (by weapon
// class), damage taken (scaled by damage) and nearby explosions (by
// distance) each queue a short two-band decay envelope; every frame the
// active envelopes are mixed (band max, not sum, so several events at once
// do not clip past 1.0) into one HidVibrationValue-shaped pair of
// amplitudes for switch_rumble.cpp to send to libnx.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Classic two-motor HD Rumble band centers (switch brew community
// convention: ~160 Hz "low"/coarse motor, ~320 Hz "high"/fine motor -- the
// pair most HD Rumble games map their old dual-ERM low/high frequency
// rumble motors onto).
#define SWITCH_RUMBLE_LOW_FREQ_HZ 160.0f
#define SWITCH_RUMBLE_HIGH_FREQ_HZ 320.0f

#define SWITCH_RUMBLE_MAX_IMPULSES 4

typedef struct SwitchRumbleImpulse
{
    float ampLow;
    float ampHigh;
    float durationSeconds;
    float elapsedSeconds;
    int32_t active;
} SwitchRumbleImpulse;

typedef struct SwitchRumbleState
{
    SwitchRumbleImpulse impulses[SWITCH_RUMBLE_MAX_IMPULSES];
} SwitchRumbleState;

static inline float SwitchRumble_Clamp01(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

static inline void Switch_RumbleStopAll(SwitchRumbleState *state)
{
    int32_t i;
    if (state == NULL)
        return;
    for (i = 0; i < SWITCH_RUMBLE_MAX_IMPULSES; ++i)
    {
        state->impulses[i].ampLow = 0.0f;
        state->impulses[i].ampHigh = 0.0f;
        state->impulses[i].durationSeconds = 0.0f;
        state->impulses[i].elapsedSeconds = 0.0f;
        state->impulses[i].active = 0;
    }
}

// Queues one decaying impulse: picks a free slot, or (all four busy) the
// slot nearest completion so a burst of fire events keeps the strongest/
// newest feedback rather than silently dropping new ones.
static inline void Switch_RumbleQueueImpulse(SwitchRumbleState *state, float ampLow, float ampHigh,
                                             float durationSeconds)
{
    int32_t slot;
    int32_t i;
    float bestRemaining;

    if (state == NULL)
        return;

    slot = -1;
    bestRemaining = 0.0f;
    for (i = 0; i < SWITCH_RUMBLE_MAX_IMPULSES; ++i)
    {
        if (!state->impulses[i].active)
        {
            slot = i;
            break;
        }
        {
            float remaining = state->impulses[i].durationSeconds - state->impulses[i].elapsedSeconds;
            if (slot < 0 || remaining < bestRemaining)
            {
                slot = i;
                bestRemaining = remaining;
            }
        }
    }
    if (slot < 0)
        return;

    state->impulses[slot].ampLow = SwitchRumble_Clamp01(ampLow);
    state->impulses[slot].ampHigh = SwitchRumble_Clamp01(ampHigh);
    state->impulses[slot].durationSeconds = durationSeconds > 0.0f ? durationSeconds : 0.01f;
    state->impulses[slot].elapsedSeconds = 0.0f;
    state->impulses[slot].active = 1;
}

// Advances every active impulse by dtSeconds and mixes them (per-band max
// of a linear decay-to-zero envelope) into one output band pair. Call once
// per engine frame even when nothing was queued (dtSeconds still needs to
// tick down active impulses).
static inline void Switch_RumbleAdvance(SwitchRumbleState *state, float dtSeconds,
                                        float *outAmpLow, float *outAmpHigh)
{
    int32_t i;
    float mixLow = 0.0f;
    float mixHigh = 0.0f;

    if (outAmpLow != NULL)
        *outAmpLow = 0.0f;
    if (outAmpHigh != NULL)
        *outAmpHigh = 0.0f;
    if (state == NULL)
        return;
    if (dtSeconds < 0.0f)
        dtSeconds = 0.0f;

    for (i = 0; i < SWITCH_RUMBLE_MAX_IMPULSES; ++i)
    {
        SwitchRumbleImpulse *imp = &state->impulses[i];
        float decay;
        float low;
        float high;

        if (!imp->active)
            continue;
        imp->elapsedSeconds += dtSeconds;
        if (imp->elapsedSeconds >= imp->durationSeconds)
        {
            imp->active = 0;
            continue;
        }
        decay = 1.0f - (imp->elapsedSeconds / imp->durationSeconds);
        low = imp->ampLow * decay;
        high = imp->ampHigh * decay;
        if (low > mixLow)
            mixLow = low;
        if (high > mixHigh)
            mixHigh = high;
    }

    if (outAmpLow != NULL)
        *outAmpLow = mixLow;
    if (outAmpHigh != NULL)
        *outAmpHigh = mixHigh;
}

// Global enable/intensity applied after mixing, right before the band pair
// is handed to libnx. enabled=0 or intensity<=0 forces silence (also the
// path pause/menu/map-change/disconnect/focus-loss stop through: the
// engine-side gate zeroes `enabled` for the frame rather than needing a
// second code path).
static inline void Switch_RumbleApplyIntensity(float ampLow, float ampHigh, float intensity,
                                               int32_t enabled, float *outAmpLow, float *outAmpHigh)
{
    if (outAmpLow != NULL)
        *outAmpLow = 0.0f;
    if (outAmpHigh != NULL)
        *outAmpHigh = 0.0f;
    if (!enabled || intensity <= 0.0f)
        return;
    if (outAmpLow != NULL)
        *outAmpLow = SwitchRumble_Clamp01(ampLow * intensity);
    if (outAmpHigh != NULL)
        *outAmpHigh = SwitchRumble_Clamp01(ampHigh * intensity);
}

// Weapon classes the fire-envelope preset switches on. Mirrors bgame's
// weapClass_t (bg_weapons.h) by value so the engine glue can pass
// (int32_t)weaponDef->weapClass straight through without either side
// depending on the other's header (this file stays libnx- and
// engine-free for the host test).
enum SwitchRumbleWeaponClass
{
    SWITCH_RUMBLE_WEAPCLASS_RIFLE = 0,
    SWITCH_RUMBLE_WEAPCLASS_MG = 1,
    SWITCH_RUMBLE_WEAPCLASS_SMG = 2,
    SWITCH_RUMBLE_WEAPCLASS_SPREAD = 3, // shotgun
    SWITCH_RUMBLE_WEAPCLASS_PISTOL = 4,
    SWITCH_RUMBLE_WEAPCLASS_GRENADE = 5,
    SWITCH_RUMBLE_WEAPCLASS_ROCKETLAUNCHER = 6,
    SWITCH_RUMBLE_WEAPCLASS_TURRET = 7,
};

// Fixed presets, not data-driven: there is no rumble-graph asset in this
// port's data to read curves from (see switch_rumble.cpp). Values are a
// deliberately conservative first pass (short, punchy, biased toward the
// low band for the heavier classes) -- tune from the hardware test note's
// feedback, not from a spec.
static inline void Switch_RumbleWeaponFireEnvelope(int32_t weapClass, float *outAmpLow,
                                                   float *outAmpHigh, float *outDurationSeconds)
{
    float ampLow = 0.30f;
    float ampHigh = 0.40f;
    float duration = 0.05f;

    switch (weapClass)
    {
    case SWITCH_RUMBLE_WEAPCLASS_PISTOL: ampLow = 0.25f; ampHigh = 0.55f; duration = 0.05f; break;
    case SWITCH_RUMBLE_WEAPCLASS_SMG: ampLow = 0.20f; ampHigh = 0.45f; duration = 0.04f; break;
    case SWITCH_RUMBLE_WEAPCLASS_RIFLE: ampLow = 0.35f; ampHigh = 0.50f; duration = 0.06f; break;
    case SWITCH_RUMBLE_WEAPCLASS_MG: ampLow = 0.50f; ampHigh = 0.50f; duration = 0.08f; break;
    case SWITCH_RUMBLE_WEAPCLASS_SPREAD: ampLow = 0.65f; ampHigh = 0.40f; duration = 0.09f; break;
    case SWITCH_RUMBLE_WEAPCLASS_ROCKETLAUNCHER: ampLow = 0.80f; ampHigh = 0.60f; duration = 0.15f; break;
    case SWITCH_RUMBLE_WEAPCLASS_TURRET: ampLow = 0.55f; ampHigh = 0.45f; duration = 0.07f; break;
    default: break; // GRENADE (thrown, not fired) and anything unmapped: the default preset above.
    }

    if (outAmpLow != NULL)
        *outAmpLow = ampLow;
    if (outAmpHigh != NULL)
        *outAmpHigh = ampHigh;
    if (outDurationSeconds != NULL)
        *outDurationSeconds = duration;
}

// damage: raw hit-points taken this event (playerState_s.damageCount),
// clamped/scaled against a nominal 0..100 range.
static inline void Switch_RumbleDamageEnvelope(int32_t damage, float *outAmpLow, float *outAmpHigh,
                                               float *outDurationSeconds)
{
    float t = (float)damage / 100.0f;
    t = SwitchRumble_Clamp01(t);
    if (outAmpLow != NULL)
        *outAmpLow = 0.30f + 0.60f * t;
    if (outAmpHigh != NULL)
        *outAmpHigh = 0.15f + 0.35f * t;
    if (outDurationSeconds != NULL)
        *outDurationSeconds = 0.12f + 0.15f * t;
}

// distance/radius: local-player distance from the explosion origin and the
// weapon's iExplosionRadius. radius<=0 or distance>=radius produces
// silence (out of blast range).
static inline void Switch_RumbleExplosionEnvelope(float distance, float radius, float *outAmpLow,
                                                  float *outAmpHigh, float *outDurationSeconds)
{
    float falloff;
    float shaped;

    if (outAmpLow != NULL)
        *outAmpLow = 0.0f;
    if (outAmpHigh != NULL)
        *outAmpHigh = 0.0f;
    if (outDurationSeconds != NULL)
        *outDurationSeconds = 0.0f;
    if (radius <= 0.0f || distance >= radius)
        return;

    falloff = SwitchRumble_Clamp01(1.0f - (distance / radius));
    shaped = falloff * falloff * (3.0f - 2.0f * falloff); // smoothstep

    if (outAmpLow != NULL)
        *outAmpLow = 0.40f + 0.60f * shaped;
    if (outAmpHigh != NULL)
        *outAmpHigh = 0.20f + 0.30f * shaped;
    if (outDurationSeconds != NULL)
        *outDurationSeconds = 0.20f + 0.30f * shaped;
}

// Hardware glue (switch_rumble.cpp; #if defined(__SWITCH__), no-ops on
// host). Declared unconditionally so engine call sites (cg_weapons.cpp,
// cg_playerstate.cpp, cg_event.cpp, cl_main.cpp) need no #ifdef ladder of
// their own; only the definitions are Switch-only.

// Registers the archived rumble_* dvars. Call once, after Dvar_Init/
// Com_Init (same ordering switch_sp_main.cpp uses for
// Switch_QuickSaveRegisterDvars).
void Switch_RumbleRegisterDvars(void);

// Queues the corresponding envelope (weapClass: SwitchRumbleWeaponClass).
void Switch_RumbleNotifyWeaponFire(int32_t weapClass);
// damage: playerState_s.damageCount (0..100-ish, not pre-clamped).
void Switch_RumbleNotifyDamage(int32_t damage);
// distance/radius: local-player distance from the blast origin and the
// weapon's iExplosionRadius, both in map units.
void Switch_RumbleNotifyExplosion(float distance, float radius);

// Advances the decay envelopes, applies rumble_enable/rumble_intensity and
// the pause/menu/focus-loss/disconnect gate, and sends the mixed band pair
// to the active vibration device(s). Call once per engine frame (IN_Frame).
void Switch_RumbleFrame(void);

// Hard reset: clears every pending impulse and sends one all-zero vibration
// value immediately. Wired into CL_StopControllerRumbles (pause/menu/
// console command) as an explicit cut on top of Switch_RumbleFrame's
// continuous gate.
void Switch_RumbleStopAllDevices(void);

#ifdef __cplusplus
}
#endif

#endif
