// Switch HD Rumble: vibration device lifecycle, dvars, and the per-frame
// mixer/gate around the pure envelope model in switch_rumble.h.
//
// Rumble path found: CoD4 SP does carry a rumble *event* pipeline end to
// end -- EV_PLAY_RUMBLE_ON_ENT/_ON_POS/_LOOP_ON_ENT/_LOOP_ON_POS/
// EV_STOP_RUMBLE/EV_STOP_ALL_RUMBLES (bgame/bg_public.h), the script
// builtins playRumbleOnEntity/playRumbleLoopOnEntity/stopRumble
// (src/game/g_scr_main.cpp ScrCmd_PlayRumbleOnEntity_Internal, event 70/72,
// and ScrCmd_StopRumble), CS_RUMBLES configstrings that carry a rumble
// asset *name* string, and WeaponDef::fireRumble/meleeImpactRumble name
// fields (bg_weapons_load_obj.cpp) -- but every client-side consumer that
// would turn a rumble asset name into an actual curve is a no-op
// (cg_event.cpp's EV_PLAY_RUMBLE_* cases, CG_PlayRumble_f in
// cg_consolecmds.cpp, CG_FireWeapon's fireRumble path in cg_weapons.cpp).
// There is no rumble asset TYPE in the zone/database layer at all
// (src/database has no ASSET_TYPE_RUMBLE*/RumbleInfo/rumble-graph reader):
// the low/high-frequency motor-intensity-over-time "rumble graphs"
// are Xenon/PS3 XAsset data this PC-derived retail zone
// format never carries, so there is nothing to evaluate even where the
// event plumbing does reach a hook.
//
// Path taken: the minimal engine-side events the task allows for exactly
// this case. Three call sites feed Switch_RumbleNotify*: CG_FireWeapon
// (src/cgame/cg_weapons.cpp, local player's own fire, weapon class),
// CG_DamageFeedback (src/cgame/cg_playerstate.cpp, local player's own
// damageCount) and the grenade/rocket/custom explosion cases in
// cg_event.cpp (local-player distance from the blast origin against the
// weapon's iExplosionRadius; flashbang is not wired -- it carries no
// iExplosionRadius in that event case). Each queues a short two-band decay
// envelope (switch_rumble.h); this file mixes and sends them.
#ifdef __SWITCH__

#include "switch_rumble.h"

#include <switch.h>

#include <qcommon/qcommon.h>
#include <platform/switch/switch_input.h>
#include <platform/switch/switch_input_lifecycle.h>

extern "C" uint32_t Sys_Milliseconds(void);
// cl_input.cpp: true while the local player is being driven with no UI key
// catcher up (same gameplay gate CL_GamepadMove and switch_quicksave.cpp
// use).
extern "C" int Switch_ClientGameplayActive(void);

namespace
{

const dvar_t *s_enable;
const dvar_t *s_intensity;

SwitchRumbleState s_state;
uint32_t s_lastFrameMs;
bool s_haveLastFrameMs;

bool s_devicesReady;
u32 s_deviceStyle;
HidVibrationDeviceHandle s_handles[2];
s32 s_handleCount;

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

void SendValues(float ampLow, float ampHigh)
{
    if (!s_devicesReady || s_handleCount <= 0)
        return;

    HidVibrationValue value{};
    value.amp_low = ampLow;
    value.freq_low = SWITCH_RUMBLE_LOW_FREQ_HZ;
    value.amp_high = ampHigh;
    value.freq_high = SWITCH_RUMBLE_HIGH_FREQ_HZ;

    HidVibrationValue values[2] = {value, value};
    hidSendVibrationValues(s_handles, values, s_handleCount);
}

// Re-acquires the vibration device handle(s) for the pad's current style.
// JoyDual reports two independent handles (left, right); both are driven
// with the same value pair (the same mixed feedback either hand feels),
// matching how a split Joy-Con pair is a single logical controller
// everywhere else in this port.
bool EnsureDevices(const PadState *pad)
{
    if (pad == nullptr || !padIsConnected(pad))
    {
        s_devicesReady = false;
        return false;
    }

    const u32 styleTag = PickStyleTag(padGetStyleSet(pad));
    if (styleTag == 0)
    {
        s_devicesReady = false;
        return false;
    }

    if (s_devicesReady && s_deviceStyle == styleTag)
        return true;

    s_handleCount = (styleTag == HidNpadStyleTag_NpadJoyDual) ? 2 : 1;
    const Result rc = hidInitializeVibrationDevices(s_handles, s_handleCount, HidNpadIdType_No1,
                                                    (HidNpadStyleTag)styleTag);
    if (R_FAILED(rc))
    {
        s_devicesReady = false;
        return false;
    }

    s_deviceStyle = styleTag;
    s_devicesReady = true;
    return true;
}

} // namespace

void Switch_RumbleRegisterDvars(void)
{
    s_enable = Dvar_RegisterBool("rumble_enable", true, DVAR_ARCHIVE, "Enable HD Rumble feedback");
    s_intensity = Dvar_RegisterFloat(
        "rumble_intensity", 0.8f, 0.0f, 2.0f, DVAR_ARCHIVE, "Global HD Rumble intensity scale");
}

void Switch_RumbleNotifyWeaponFire(int32_t weapClass)
{
    float ampLow;
    float ampHigh;
    float duration;
    Switch_RumbleWeaponFireEnvelope(weapClass, &ampLow, &ampHigh, &duration);
    Switch_RumbleQueueImpulse(&s_state, ampLow, ampHigh, duration);
}

void Switch_RumbleNotifyDamage(int32_t damage)
{
    if (damage <= 0)
        return;
    float ampLow;
    float ampHigh;
    float duration;
    Switch_RumbleDamageEnvelope(damage, &ampLow, &ampHigh, &duration);
    Switch_RumbleQueueImpulse(&s_state, ampLow, ampHigh, duration);
}

void Switch_RumbleNotifyExplosion(float distance, float radius)
{
    float ampLow;
    float ampHigh;
    float duration;
    Switch_RumbleExplosionEnvelope(distance, radius, &ampLow, &ampHigh, &duration);
    if (ampLow <= 0.0f && ampHigh <= 0.0f)
        return;
    Switch_RumbleQueueImpulse(&s_state, ampLow, ampHigh, duration);
}

void Switch_RumbleFrame(void)
{
    const uint32_t nowMs = Sys_Milliseconds();
    float dtSeconds = 0.0f;
    if (s_haveLastFrameMs && nowMs >= s_lastFrameMs)
        dtSeconds = (float)(nowMs - s_lastFrameMs) / 1000.0f;
    s_lastFrameMs = nowMs;
    s_haveLastFrameMs = true;

    float mixLow;
    float mixHigh;
    Switch_RumbleAdvance(&s_state, dtSeconds, &mixLow, &mixHigh);

    // The single gate every "stop all vibration" case in the task reduces
    // to: focus lost (suspend/HOME menu/library applet), paused, a menu
    // owns input, or disconnected. Continuous, not event-based, so a path
    // this file's three notify hooks were not told about (e.g. a future
    // menu screen) still cannot leave the motors running.
    const bool inFocus = appletGetFocusState() == AppletFocusState_InFocus;
    const bool gameplayActive = Switch_ClientGameplayActive() != 0;
    const bool enabled = inFocus && gameplayActive && s_enable && s_enable->current.enabled;

    const float intensity = s_intensity ? s_intensity->current.value : 0.0f;
    float outLow;
    float outHigh;
    Switch_RumbleApplyIntensity(mixLow, mixHigh, intensity, enabled ? 1 : 0, &outLow, &outHigh);

    const SwitchInputState *input = Switch_GetInputState();
    if (input == nullptr || !EnsureDevices(&input->pad))
        return;

    SendValues(outLow, outHigh);
}

void Switch_RumbleStopAllDevices(void)
{
    Switch_RumbleStopAll(&s_state);
    if (s_devicesReady)
        SendValues(0.0f, 0.0f);
}

#endif // __SWITCH__
