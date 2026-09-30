#ifndef SWITCH_GYRO_H
#define SWITCH_GYRO_H

// Pure, host-testable gyro-aiming math: unit conversion, deadzone,
// low-pass smoothing and the gating decision.  No libnx dependency -- the
// hardware sensor read/handle lifecycle lives in switch_gyro.cpp, guarded by
// `#if defined(__SWITCH__)`, and calls only into the functions below.
//
// Unit assumption (report in the hardware test note): libnx's
// HidSixAxisSensorState.angular_velocity is taken as radians/second, the
// documented convention for Nintendo's six-axis HID reports and the value
// every public libnx gyro-aim homebrew (e.g. Nintendo's own samples,
// community gyro patches) treats it as. Some emulators do not emulate motion
// controls, so this cannot be confirmed there; the hardware test note asks
// for a sanity check of the physical rotation-to-turn-rate feel on a Switch
// and Pro Controller.

#include <stdint.h>
#include <stddef.h>
#include <cmath>

#ifdef __cplusplus
extern "C" {
#endif

enum SwitchGyroEnableMode
{
    SWITCH_GYRO_ENABLE_OFF = 0,
    SWITCH_GYRO_ENABLE_ALWAYS = 1,
    // Default: gyro only turns the view while the player holds ADS (aim
    // down sights). Matches the shipped default of gyro_enable (2).
    SWITCH_GYRO_ENABLE_ADS_ONLY = 2,
};

enum SwitchGyroSpace
{
    // Rotate the view directly from the sensor's own (pitch, yaw) axes.
    // Implemented.
    SWITCH_GYRO_SPACE_LOCAL = 0,
    // "Player space": yaw is measured around the world gravity vector
    // instead of the controller's own axis, so tilting the controller
    // (holding it at an angle) does not mix yaw and roll. Not implemented
    // this pass (the dvar accepts the value and falls back to local space;
    // see switch_gyro.cpp) -- deferred per the task's own "local first,
    // player-space if cheap" ordering, not a masked failure.
    SWITCH_GYRO_SPACE_PLAYER = 1,
};

typedef struct SwitchGyroConfig
{
    int32_t enable;             // SwitchGyroEnableMode
    int32_t space;              // SwitchGyroSpace
    float sensitivityYaw;
    // <= 0: derive from sensitivityYaw * ySensitivityScale (single-knob
    // mode); > 0: independent pitch sensitivity.
    float sensitivityPitch;
    float ySensitivityScale;
    int32_t invertYaw;
    int32_t invertPitch;
    float deadzoneDegPerSec;
    // One-pole low-pass cutoff in Hz. <= 0 disables smoothing.
    float smoothingHz;
} SwitchGyroConfig;

typedef struct SwitchGyroFilterState
{
    float filteredX;
    float filteredY;
    float filteredZ;
    int32_t initialized;
} SwitchGyroFilterState;

static inline void Switch_GyroFilterReset(SwitchGyroFilterState *state)
{
    if (state == NULL)
        return;
    state->filteredX = 0.0f;
    state->filteredY = 0.0f;
    state->filteredZ = 0.0f;
    state->initialized = 0;
}

// One-pole (RC) low-pass filter over one frame's raw angular velocity
// sample (any consistent unit; used here with rad/s). cutoffHz <= 0 or
// dtSeconds <= 0 passes the raw sample through unfiltered.
static inline void Switch_GyroLowPass(SwitchGyroFilterState *state, float x, float y, float z,
                                      float dtSeconds, float cutoffHz,
                                      float *outX, float *outY, float *outZ)
{
    if (state == NULL || outX == NULL || outY == NULL || outZ == NULL)
        return;

    if (!state->initialized)
    {
        state->filteredX = x;
        state->filteredY = y;
        state->filteredZ = z;
        state->initialized = 1;
    }
    else if (cutoffHz > 0.0f && dtSeconds > 0.0f)
    {
        const float rc = 1.0f / (6.28318530717958647692f * cutoffHz);
        const float alpha = dtSeconds / (rc + dtSeconds);
        state->filteredX += alpha * (x - state->filteredX);
        state->filteredY += alpha * (y - state->filteredY);
        state->filteredZ += alpha * (z - state->filteredZ);
    }
    else
    {
        state->filteredX = x;
        state->filteredY = y;
        state->filteredZ = z;
    }

    *outX = state->filteredX;
    *outY = state->filteredY;
    *outZ = state->filteredZ;
}

// Radial deadzone over the (pitch, yaw) angular-rate pair, in degrees/sec:
// below the threshold the pair is zeroed; above it the remainder keeps its
// direction (same shape as Switch_InputApplyStickDeadzone, but not rescaled
// back to a fixed range -- gyro rate has no natural "full deflection").
static inline void Switch_GyroApplyDeadzone(float pitchRateDeg, float yawRateDeg, float deadzoneDeg,
                                            float *outPitchRateDeg, float *outYawRateDeg)
{
    float magnitude;

    if (outPitchRateDeg == NULL || outYawRateDeg == NULL)
        return;
    if (deadzoneDeg < 0.0f)
        deadzoneDeg = 0.0f;

    magnitude = std::sqrt(pitchRateDeg * pitchRateDeg + yawRateDeg * yawRateDeg);
    if (magnitude <= deadzoneDeg || magnitude <= 0.0f)
    {
        *outPitchRateDeg = 0.0f;
        *outYawRateDeg = 0.0f;
        return;
    }

    {
        const float scale = (magnitude - deadzoneDeg) / magnitude;
        *outPitchRateDeg = pitchRateDeg * scale;
        *outYawRateDeg = yawRateDeg * scale;
    }
}

// Converts one already-filtered gyro sample (local sensor axes x=pitch,
// y=yaw, radians/second) plus the frame's elapsed time into view-angle
// deltas in degrees, applying deadzone, sensitivity and invert. This is the
// entire "gyro turn-rate curve": deliberately not cl_yawspeed/cl_pitchspeed
// (the stick's ramped turn-rate curve) -- the caller adds the result
// straight into the view angles at the same site mouse-look deltas are
// added, so gyro motion is frame-rate independent and 1:1 with rotation.
// Returns false (both deltas left at 0) when cfg->enable is
// SWITCH_GYRO_ENABLE_OFF or dtSeconds is non-positive.
static inline int32_t Switch_GyroComputeDelta(const SwitchGyroConfig *cfg,
                                              float angularVelPitchRadPerSec,
                                              float angularVelYawRadPerSec,
                                              float dtSeconds,
                                              float *outYawDeltaDeg, float *outPitchDeltaDeg)
{
    const float kRadToDeg = 57.2957795130823208768f;
    float pitchRateDeg;
    float yawRateDeg;
    float pitchSens;

    if (outYawDeltaDeg == NULL || outPitchDeltaDeg == NULL)
        return 0;
    *outYawDeltaDeg = 0.0f;
    *outPitchDeltaDeg = 0.0f;
    if (cfg == NULL || cfg->enable == SWITCH_GYRO_ENABLE_OFF || dtSeconds <= 0.0f)
        return 0;

    pitchRateDeg = angularVelPitchRadPerSec * kRadToDeg;
    yawRateDeg = angularVelYawRadPerSec * kRadToDeg;
    Switch_GyroApplyDeadzone(pitchRateDeg, yawRateDeg, cfg->deadzoneDegPerSec, &pitchRateDeg, &yawRateDeg);

    pitchSens = cfg->sensitivityPitch > 0.0f
        ? cfg->sensitivityPitch
        : cfg->sensitivityYaw * (cfg->ySensitivityScale > 0.0f ? cfg->ySensitivityScale : 1.0f);

    {
        float yawDeltaDeg = yawRateDeg * dtSeconds * cfg->sensitivityYaw;
        float pitchDeltaDeg = pitchRateDeg * dtSeconds * pitchSens;

        if (cfg->invertYaw)
            yawDeltaDeg = -yawDeltaDeg;
        if (cfg->invertPitch)
            pitchDeltaDeg = -pitchDeltaDeg;

        *outYawDeltaDeg = yawDeltaDeg;
        *outPitchDeltaDeg = pitchDeltaDeg;
    }
    return 1;
}

// Whether gyro rotation may be applied to the view this frame. All of the
// "must not rotate the view" cases the task calls out reduce to one of
// these booleans, decided by the engine-side caller from state this pure
// header cannot see (menu catcher, cl_paused, PMF_FROZEN, pm_type, the
// configured ratchet button): menus/cinematics/pause -> !gameplayActive or
// viewFrozen; dead -> playerDead; vehicles/scripted cameras that forbid
// look already imply viewFrozen (PMF_FROZEN) in the retail state machine.
static inline int32_t Switch_GyroShouldApply(const SwitchGyroConfig *cfg, int32_t gameplayActive,
                                             int32_t viewFrozen, int32_t playerDead,
                                             int32_t adsHeld, int32_t ratchetHeld)
{
    if (cfg == NULL || cfg->enable == SWITCH_GYRO_ENABLE_OFF)
        return 0;
    if (!gameplayActive || viewFrozen || playerDead || ratchetHeld)
        return 0;
    if (cfg->enable == SWITCH_GYRO_ENABLE_ADS_ONLY && !adsHeld)
        return 0;
    return 1;
}

// Hardware glue (switch_gyro.cpp; #if defined(__SWITCH__), no-ops on host).
// Declared unconditionally so callers do not need their own #ifdef ladder;
// only their *definitions* are Switch-only.

// Registers the archived gyro_* dvars. Call once, after Dvar_Init/Com_Init
// (same ordering switch_sp_main.cpp uses for Switch_QuickSaveRegisterDvars).
void Switch_GyroRegisterDvars(void);

// Reads the current six-axis sensor sample (acquiring/refreshing the sensor
// handle for the active pad style as needed), runs it through the low-pass
// filter and Switch_GyroComputeDelta with the live dvars, and stores the
// result for Switch_GyroGetDelta. Call once per engine frame (IN_Frame).
// Safe to call with no controller connected or before six-axis is
// available (produces a zero delta).
void Switch_GyroFrame(void);

// The most recent frame's raw yaw/pitch delta in degrees, before the
// engine-side gating decision (menu/pause/frozen/dead/ratchet/ADS-only).
void Switch_GyroGetDelta(float *outYawDeltaDeg, float *outPitchDeltaDeg);

// Current gyro_enable dvar value (SwitchGyroEnableMode), for the caller's
// Switch_GyroShouldApply gate.
int32_t Switch_GyroEnableMode(void);

// Current gyro_ratchetButton dvar value: a SWITCH_INPUT_BUTTON_* bitmask
// (see switch_input.h) that pauses gyro rotation while held, or 0 if unset.
uint64_t Switch_GyroRatchetButtonMask(void);

#ifdef __cplusplus
}
#endif

#endif
