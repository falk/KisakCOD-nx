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
#include "switch_rumble_hd.h"

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
const dvar_t *s_hd;

SwitchRumbleHdPlayer s_hdPlayer;
SwitchRumbleHdBand s_lastSent[2];
uint32_t s_lastSendMs;

bool HdOn()
{
    return s_hd == nullptr || s_hd->current.enabled;
}

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

HidVibrationValue ToValue(const SwitchRumbleHdBand &b)
{
    HidVibrationValue v{};
    v.amp_low = b.ampLow;
    v.freq_low = b.freqLow;
    v.amp_high = b.ampHigh;
    v.freq_high = b.freqHigh;
    return v;
}

// left/right map to the two Joy-Con motors; a one-motor pad gets the louder
// of the two.
void SendMotors(const SwitchRumbleHdBand &left, const SwitchRumbleHdBand &right, bool force)
{
    if (!s_devicesReady || s_handleCount <= 0)
        return;

    SwitchRumbleHdBand out[2] = {left, right};
    if (s_handleCount == 1)
    {
        SwitchRumbleHdBand &m = out[0];
        const SwitchRumbleHdBand &r = right;
        if (r.ampLow > m.ampLow)
        {
            m.ampLow = r.ampLow;
            m.freqLow = r.freqLow;
        }
        if (r.ampHigh > m.ampHigh)
        {
            m.ampHigh = r.ampHigh;
            m.freqHigh = r.freqHigh;
        }
        out[1] = m;
    }

    const uint32_t nowMs = Sys_Milliseconds();
    const uint32_t since = nowMs - s_lastSendMs;
    if (!force && !SwitchRumbleHd_ShouldSend(&s_lastSent[0], &out[0], since) &&
        !SwitchRumbleHd_ShouldSend(&s_lastSent[1], &out[1], since))
        return;

    HidVibrationValue values[2] = {ToValue(out[0]), ToValue(out[1])};
    hidSendVibrationValues(s_handles, values, s_handleCount);
    s_lastSent[0] = out[0];
    s_lastSent[1] = out[1];
    s_lastSendMs = nowMs;
}

void SendValues(float ampLow, float ampHigh)
{
    SwitchRumbleHdBand b{ampLow, SWITCH_RUMBLE_LOW_FREQ_HZ, ampHigh, SWITCH_RUMBLE_HIGH_FREQ_HZ};
    SendMotors(b, b, true);
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

    // Attached Joy-Cons answer as the handheld controller (both motors), not as
    // player 1; asking No1 for handheld devices gives handles that never buzz.
    const bool twoMotors =
        styleTag == HidNpadStyleTag_NpadJoyDual || styleTag == HidNpadStyleTag_NpadHandheld;
    const HidNpadIdType id =
        styleTag == HidNpadStyleTag_NpadHandheld ? HidNpadIdType_Handheld : HidNpadIdType_No1;
    s_handleCount = twoMotors ? 2 : 1;
    const Result rc = hidInitializeVibrationDevices(s_handles, s_handleCount, id, (HidNpadStyleTag)styleTag);
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
    s_hd = Dvar_RegisterBool("rumble_hd", true, DVAR_ARCHIVE, "Shaped HD Rumble effects (0 = fixed-frequency fallback)");
    SwitchRumbleHd_Reset(&s_hdPlayer);
}

void Switch_RumbleNotifyWeaponFire(int32_t weapClass, int32_t boltAction)
{
    if (HdOn())
    {
        SwitchRumbleHd_TriggerWeaponFire(&s_hdPlayer, weapClass, boltAction);
        return;
    }
    float ampLow;
    float ampHigh;
    float duration;
    Switch_RumbleWeaponFireEnvelope(weapClass, &ampLow, &ampHigh, &duration);
    Switch_RumbleQueueImpulse(&s_state, ampLow, ampHigh, duration);
}

void Switch_RumbleNotifyDamageFrom(int32_t damage, float side)
{
    if (damage <= 0)
        return;
    if (HdOn())
    {
        SwitchRumbleHd_TriggerDamage(&s_hdPlayer, damage, side);
        return;
    }
    Switch_RumbleNotifyDamage(damage);
}

void Switch_RumbleNotifyMelee(void)
{
    if (HdOn())
        SwitchRumbleHd_Trigger(&s_hdPlayer, SWITCH_RUMBLE_HD_MELEE, 1.0f, 0.0f);
    else
        Switch_RumbleQueueImpulse(&s_state, 0.5f, 0.1f, 0.08f);
}

void Switch_RumbleNotifyLand(int32_t hard)
{
    if (HdOn())
        SwitchRumbleHd_Trigger(&s_hdPlayer, hard ? SWITCH_RUMBLE_HD_LAND_HARD : SWITCH_RUMBLE_HD_LAND, 1.0f, 0.0f);
    else
        Switch_RumbleQueueImpulse(&s_state, hard ? 0.6f : 0.3f, 0.1f, hard ? 0.2f : 0.1f);
}

void Switch_RumbleNotifyDamage(int32_t damage)
{
    if (damage <= 0)
        return;
    if (HdOn())
    {
        SwitchRumbleHd_TriggerDamage(&s_hdPlayer, damage, 0.0f);
        return;
    }
    float ampLow;
    float ampHigh;
    float duration;
    Switch_RumbleDamageEnvelope(damage, &ampLow, &ampHigh, &duration);
    Switch_RumbleQueueImpulse(&s_state, ampLow, ampHigh, duration);
}

void Switch_RumbleNotifyExplosion(float distance, float radius)
{
    if (HdOn())
    {
        SwitchRumbleHd_TriggerExplosion(&s_hdPlayer, distance, radius);
        return;
    }
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

    // Gate: focus lost, paused, a menu owns input, or disabled. Continuous
    // rather than event-based so an unhooked menu cannot leave motors running.
    const bool inFocus = appletGetFocusState() == AppletFocusState_InFocus;
    const bool gameplayActive = Switch_ClientGameplayActive() != 0;
    const bool enabled = inFocus && gameplayActive && s_enable && s_enable->current.enabled;
    const float intensity = s_intensity ? s_intensity->current.value : 0.0f;

    SwitchRumbleHdOut hdOut;
    if (HdOn())
    {
        SwitchRumbleHd_Advance(&s_hdPlayer, dtSeconds, &hdOut);
        if (!enabled)
            SwitchRumbleHd_Reset(&s_hdPlayer);
        SwitchRumbleHd_ApplyIntensity(&hdOut, intensity, enabled ? 1 : 0);
    }
    else
    {
        float mixLow;
        float mixHigh;
        float outLow;
        float outHigh;
        Switch_RumbleAdvance(&s_state, dtSeconds, &mixLow, &mixHigh);
        Switch_RumbleApplyIntensity(mixLow, mixHigh, intensity, enabled ? 1 : 0, &outLow, &outHigh);
        hdOut.left = {outLow, SWITCH_RUMBLE_LOW_FREQ_HZ, outHigh, SWITCH_RUMBLE_HIGH_FREQ_HZ};
        hdOut.right = hdOut.left;
    }

    const SwitchInputState *input = Switch_GetInputState();
    if (input == nullptr || !EnsureDevices(&input->pad))
        return;

    SendMotors(hdOut.left, hdOut.right, false);
}

void Switch_RumbleStopAllDevices(void)
{
    Switch_RumbleStopAll(&s_state);
    SwitchRumbleHd_Reset(&s_hdPlayer);
    if (s_devicesReady)
        SendValues(0.0f, 0.0f);
}

#endif // __SWITCH__
