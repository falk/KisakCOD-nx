// Switch gyro aiming: six-axis sensor lifecycle, dvars, and the per-frame
// glue around the pure math in switch_gyro.h.
//
// Input path found: src/platform/switch/switch_input.c/.h already read the pad
// (padUpdate/padGetButtons/padGetStickPos) through a single PadState
// (padConfigureInput(1, HidNpadStyleSet_NpadStandard)); CL_GamepadMove
// (src/client/cl_input.cpp) turns the stick axes into view-angle deltas
// through the stick's own ramped turn-rate curve (cl_yawspeed/
// cl_pitchspeed * frametime * input_viewSensitivity). Gyro deliberately
// does not reuse that curve (see switch_gyro.h): its delta is added
// straight into clients[0].viewangles at the same CL_GamepadMove site,
// right after the stick contribution, so it is frame-rate independent and
// stacks with the stick rather than fighting its curve.
#ifdef __SWITCH__

#include "switch_gyro.h"

#include <switch.h>

#include <qcommon/qcommon.h>
#include <platform/switch/switch_input.h>
#include <platform/switch/switch_input_lifecycle.h>

extern "C" uint32_t Sys_Milliseconds(void);

namespace
{

const dvar_t *s_enable;
const dvar_t *s_space;
const dvar_t *s_sensitivityYaw;
const dvar_t *s_sensitivityPitch;
const dvar_t *s_ySensitivityScale;
const dvar_t *s_invertYaw;
const dvar_t *s_invertPitch;
const dvar_t *s_deadzone;
const dvar_t *s_smoothing;
const dvar_t *s_ratchetButton;

SwitchGyroFilterState s_filter;
float s_lastYawDeltaDeg;
float s_lastPitchDeltaDeg;
uint32_t s_lastFrameMs;
bool s_haveLastFrameMs;

// Sensor handle lifecycle: re-acquired whenever the pad's active style
// changes (dock<->handheld, Joy-Con pair<->Pro Controller, reconnects).
bool s_sensorStarted;
u32 s_sensorStyle; // the HidNpadStyleTag bit the current handle was opened for
HidSixAxisSensorHandle s_sensorHandle;

// Picks one style bit out of a PadState's active style_set, preferring the
// most capable/likely-intentional controller when more than one bit is
// set (padGetStyleSet's own doc only promises "the set of input styles
// supported"; in practice one bit is set for a single connected source,
// but the priority order fails safe if that ever changes).
u32 PickStyleTag(u32 styleSet)
{
    static const u32 kPriority[] = {
        HidNpadStyleTag_NpadFullKey,
        HidNpadStyleTag_NpadJoyDual,
        HidNpadStyleTag_NpadHandheld,
        HidNpadStyleTag_NpadJoyLeft,
        HidNpadStyleTag_NpadJoyRight,
    };
    for (u32 tag : kPriority)
    {
        if (styleSet & tag)
            return tag;
    }
    return 0;
}

void StopSensor()
{
    if (s_sensorStarted)
    {
        hidStopSixAxisSensor(s_sensorHandle);
        s_sensorStarted = false;
    }
}

// Ensures a started six-axis handle exists for the pad's current style,
// restarting it if the style changed since the last frame (handheld <->
// docked Joy-Con pair, Joy-Con pair <-> Pro Controller, or a reconnect that
// changed which controller answers HidNpadIdType_No1). JoyDual reports two
// handles (left, right); this uses the right Joy-Con (handles[1]), the
// conventional "dominant hand" gyro in split Joy-Con play.
bool EnsureSensor(const PadState *pad)
{
    if (pad == nullptr || !padIsConnected(pad))
    {
        StopSensor();
        return false;
    }

    const u32 styleTag = PickStyleTag(padGetStyleSet(pad));
    if (styleTag == 0)
    {
        StopSensor();
        return false;
    }

    if (s_sensorStarted && s_sensorStyle == styleTag)
        return true;

    StopSensor();

    HidSixAxisSensorHandle handles[2] = {};
    Result rc;
    if (styleTag == HidNpadStyleTag_NpadJoyDual)
    {
        rc = hidGetSixAxisSensorHandles(handles, 2, HidNpadIdType_No1, (HidNpadStyleTag)styleTag);
        s_sensorHandle = handles[1];
    }
    else
    {
        rc = hidGetSixAxisSensorHandles(handles, 1, HidNpadIdType_No1, (HidNpadStyleTag)styleTag);
        s_sensorHandle = handles[0];
    }
    if (R_FAILED(rc))
        return false;

    rc = hidStartSixAxisSensor(s_sensorHandle);
    if (R_FAILED(rc))
        return false;

    s_sensorStarted = true;
    s_sensorStyle = styleTag;
    // A style change (different physical source) is a discontinuity, not a
    // fast rotation: drop the low-pass filter's memory instead of letting it
    // blend the old source's last sample into the new one.
    Switch_GyroFilterReset(&s_filter);
    return true;
}

SwitchGyroConfig BuildConfig()
{
    SwitchGyroConfig cfg{};
    cfg.enable = s_enable ? s_enable->current.integer : SWITCH_GYRO_ENABLE_ADS_ONLY;
    cfg.space = s_space ? s_space->current.integer : SWITCH_GYRO_SPACE_LOCAL;
    cfg.sensitivityYaw = s_sensitivityYaw ? s_sensitivityYaw->current.value : 1.0f;
    cfg.sensitivityPitch = s_sensitivityPitch ? s_sensitivityPitch->current.value : 0.0f;
    cfg.ySensitivityScale = s_ySensitivityScale ? s_ySensitivityScale->current.value : 1.0f;
    cfg.invertYaw = s_invertYaw ? s_invertYaw->current.enabled : 0;
    cfg.invertPitch = s_invertPitch ? s_invertPitch->current.enabled : 0;
    cfg.deadzoneDegPerSec = s_deadzone ? s_deadzone->current.value : 0.5f;
    cfg.smoothingHz = s_smoothing ? s_smoothing->current.value : 30.0f;
    // SWITCH_GYRO_SPACE_PLAYER is accepted by the dvar but not implemented
    // this pass (switch_gyro.h's SwitchGyroSpace comment); fall back to
    // local space rather than silently computing something else.
    if (cfg.space != SWITCH_GYRO_SPACE_LOCAL)
        cfg.space = SWITCH_GYRO_SPACE_LOCAL;
    return cfg;
}

} // namespace

void Switch_GyroRegisterDvars(void)
{
    s_enable = Dvar_RegisterInt(
        "gyro_enable", SWITCH_GYRO_ENABLE_ADS_ONLY, SWITCH_GYRO_ENABLE_OFF, SWITCH_GYRO_ENABLE_ADS_ONLY,
        DVAR_ARCHIVE,
        "Gyro aiming: 0 off, 1 always on, 2 only while aiming down sights (default)");
    s_space = Dvar_RegisterInt(
        "gyro_space", SWITCH_GYRO_SPACE_LOCAL, SWITCH_GYRO_SPACE_LOCAL, SWITCH_GYRO_SPACE_PLAYER,
        DVAR_ARCHIVE,
        "Gyro yaw reference: 0 local (implemented), 1 player/gravity space (not implemented, falls back to local)");
    s_sensitivityYaw = Dvar_RegisterFloat(
        "gyro_sensitivity", 1.0f, 0.01f, 10.0f, DVAR_ARCHIVE, "Gyro yaw sensitivity multiplier");
    s_sensitivityPitch = Dvar_RegisterFloat(
        "gyro_sensitivityPitch", 0.0f, 0.0f, 10.0f, DVAR_ARCHIVE,
        "Gyro pitch sensitivity multiplier; 0 derives it from gyro_sensitivity * gyro_ySensitivityScale");
    s_ySensitivityScale = Dvar_RegisterFloat(
        "gyro_ySensitivityScale", 1.0f, 0.1f, 4.0f, DVAR_ARCHIVE,
        "Pitch/yaw sensitivity ratio used when gyro_sensitivityPitch is 0");
    s_invertYaw = Dvar_RegisterBool("gyro_invertYaw", false, DVAR_ARCHIVE, "Invert gyro yaw");
    s_invertPitch = Dvar_RegisterBool("gyro_invertPitch", false, DVAR_ARCHIVE, "Invert gyro pitch");
    s_deadzone = Dvar_RegisterFloat(
        "gyro_deadzone", 0.5f, 0.0f, 20.0f, DVAR_ARCHIVE, "Gyro deadzone in degrees/second");
    s_smoothing = Dvar_RegisterFloat(
        "gyro_smoothing", 30.0f, 0.0f, 120.0f, DVAR_ARCHIVE,
        "Gyro low-pass filter cutoff in Hz; 0 disables smoothing");
    s_ratchetButton = Dvar_RegisterInt(
        "gyro_ratchetButton", 0, 0, 0x7FFFFFFF, DVAR_ARCHIVE,
        "SWITCH_INPUT_BUTTON_* bitmask that pauses gyro rotation while held (0 disables; default: none bound)");
}

void Switch_GyroFrame(void)
{
    s_lastYawDeltaDeg = 0.0f;
    s_lastPitchDeltaDeg = 0.0f;

    const uint32_t nowMs = Sys_Milliseconds();
    float dtSeconds = 0.0f;
    if (s_haveLastFrameMs && nowMs >= s_lastFrameMs)
        dtSeconds = (float)(nowMs - s_lastFrameMs) / 1000.0f;
    s_lastFrameMs = nowMs;
    s_haveLastFrameMs = true;

    const SwitchGyroConfig cfg = BuildConfig();
    if (cfg.enable == SWITCH_GYRO_ENABLE_OFF)
        return;

    // No motion while the applet is out of focus (HOME menu / sleep /
    // library-applet overlay): a stale or garbage sample must not be
    // integrated into the filter, and it must not spend down while
    // unfocused either (that would produce a jump on refocus).
    if (appletGetFocusState() != AppletFocusState_InFocus)
        return;

    const SwitchInputState *input = Switch_GetInputState();
    if (input == nullptr)
        return;

    if (!EnsureSensor(&input->pad))
        return;

    HidSixAxisSensorState states[1] = {};
    const size_t count = hidGetSixAxisSensorStates(s_sensorHandle, states, 1);
    if (count == 0)
        return;

    // Assumed axis convention for a controller held normally (see
    // switch_gyro.h's top comment and the hardware test note): x = pitch
    // (nose up/down), y = yaw (nose left/right). Unverified against real
    // hardware (some emulators do not emulate motion); gyro_invertYaw/
    // gyro_invertPitch are the escape hatch if a build turns out backwards.
    float filteredX;
    float filteredY;
    float filteredZ;
    Switch_GyroLowPass(&s_filter, states[0].angular_velocity.x, states[0].angular_velocity.y,
                       states[0].angular_velocity.z, dtSeconds, cfg.smoothingHz,
                       &filteredX, &filteredY, &filteredZ);

    Switch_GyroComputeDelta(&cfg, filteredX, filteredY, dtSeconds, &s_lastYawDeltaDeg,
                            &s_lastPitchDeltaDeg);
}

void Switch_GyroGetDelta(float *outYawDeltaDeg, float *outPitchDeltaDeg)
{
    if (outYawDeltaDeg != nullptr)
        *outYawDeltaDeg = s_lastYawDeltaDeg;
    if (outPitchDeltaDeg != nullptr)
        *outPitchDeltaDeg = s_lastPitchDeltaDeg;
}

int32_t Switch_GyroEnableMode(void)
{
    return s_enable ? s_enable->current.integer : (int32_t)SWITCH_GYRO_ENABLE_ADS_ONLY;
}

uint64_t Switch_GyroRatchetButtonMask(void)
{
    return s_ratchetButton ? (uint64_t)(uint32_t)s_ratchetButton->current.integer : 0;
}

#endif // __SWITCH__
