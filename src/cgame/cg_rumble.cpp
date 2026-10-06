#ifdef KISAK_SP
#ifdef __SWITCH__

#include "cg_rumble.h"

#include <universal/q_shared.h>
#include "cg_local.h"
#include "cg_main.h"
#include <bgame/bg_public.h>
#include <client/client.h>
#include <qcommon/qcommon.h>
#include <port/switch_rumble.h>
#include <port/switch_rumble_hd.h>
#include <port/switch_rumble_names.h>

namespace
{
struct BoundVoice
{
    int32_t key;
    int32_t entityNum;
};
BoundVoice s_bound[8];
int32_t s_boundNext;

char s_reported[16][48];
int32_t s_reportedCount;

int32_t KeyFor(const char *name, int32_t entityNum)
{
    uint32_t h = 2166136261u;
    for (const char *c = name; *c; ++c)
    {
        char ch = *c;
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
        h = (h ^ (uint8_t)ch) * 16777619u;
    }
    h ^= (uint32_t)(entityNum + 1) * 0x9E3779B1u;
    return (int32_t)(h | 1u);
}

void ReportUnknown(const char *name, const SwitchRumbleNameInfo &info)
{
    for (int32_t i = 0; i < s_reportedCount; ++i)
    {
        if (SwitchRumbleNames_Equal(s_reported[i], name))
            return;
    }
    if (s_reportedCount < 16)
    {
        strncpy(s_reported[s_reportedCount], name, sizeof(s_reported[0]) - 1);
        s_reported[s_reportedCount][sizeof(s_reported[0]) - 1] = 0;
        ++s_reportedCount;
    }
    Com_Printf(CON_CHANNEL_DONT_FILTER, "SWITCH_RUMBLE unknown script rumble '%s' -> generic effect %d\n", name,
               (int)info.effect);
}

bool IsLocalPlayer(int32_t localClientNum, int32_t entityNum)
{
    return entityNum == CG_GetLocalClientGlobals(localClientNum)->predictedPlayerState.clientNum;
}

void Bind(int32_t key, int32_t entityNum)
{
    for (auto &b : s_bound)
    {
        if (b.key == key)
        {
            b.entityNum = entityNum;
            return;
        }
    }
    s_bound[s_boundNext] = {key, entityNum};
    s_boundNext = (s_boundNext + 1) % 8;
}

void Unbind(int32_t key)
{
    for (auto &b : s_bound)
    {
        if (b.key == key)
            b.key = 0;
    }
}

void EntityOrigin(int32_t localClientNum, int32_t entityNum, float *out)
{
    const centity_s *cent = CG_GetEntity(localClientNum, entityNum);
    out[0] = cent->pose.origin[0];
    out[1] = cent->pose.origin[1];
    out[2] = cent->pose.origin[2];
}

// entityNum < 0: a positional rumble (always falls off with distance).
void Play(int32_t localClientNum, const char *name, int32_t entityNum, const float *origin, bool loop)
{
    if (localClientNum != 0 || name == nullptr || *name == 0)
        return;
    SwitchRumbleHdPlayer *p = Switch_RumbleHdPlayer();
    if (p == nullptr)
        return;

    SwitchRumbleNameInfo info;
    if (!SwitchRumbleNames_Lookup(name, &info))
        ReportUnknown(name, info);

    const bool local = entityNum >= 0 && IsLocalPlayer(localClientNum, entityNum);
    SwitchRumbleHdOpts opts{};
    opts.key = KeyFor(name, entityNum);
    opts.loop = loop ? 1 : 0;
    opts.category = info.category;
    if (!local)
    {
        // Positional (or on another entity): attenuate by listener distance;
        // names with no radius still get a generous one.
        opts.radius = info.radius > 0.0f ? info.radius : 1500.0f;
        if (origin != nullptr)
        {
            opts.origin[0] = origin[0];
            opts.origin[1] = origin[1];
            opts.origin[2] = origin[2];
        }
    }
    SwitchRumbleHd_TriggerScript(p, info.effect, info.scale, &opts);
    if (!local && entityNum >= 0 && loop)
        Bind(opts.key, entityNum);
}
} // namespace

void CG_PlayRumbleOnClient(int32_t localClientNum, const char *name)
{
    Play(localClientNum, name, CG_GetLocalClientGlobals(localClientNum)->predictedPlayerState.clientNum, nullptr, false);
}

void CG_PlayRumbleOnEntity(int32_t localClientNum, const char *name, int32_t entityNum)
{
    float o[3];
    EntityOrigin(localClientNum, entityNum, o);
    Play(localClientNum, name, entityNum, o, false);
}

void CG_PlayRumbleOnPosition(int32_t localClientNum, const char *name, const float *origin)
{
    Play(localClientNum, name, -1, origin, false);
}

void CG_PlayRumbleLoopOnEntity(int32_t localClientNum, const char *name, int32_t entityNum)
{
    float o[3];
    EntityOrigin(localClientNum, entityNum, o);
    Play(localClientNum, name, entityNum, o, true);
}

void CG_PlayRumbleLoopOnPosition(int32_t localClientNum, const char *name, const float *origin)
{
    Play(localClientNum, name, -1, origin, true);
}

void CG_StopRumble(int32_t localClientNum, int32_t entityNum, const char *name)
{
    if (localClientNum != 0 || name == nullptr)
        return;
    const int32_t key = KeyFor(name, entityNum);
    SwitchRumbleHd_Stop(Switch_RumbleHdPlayer(), key);
    Unbind(key);
}

void CG_StopAllRumbles(int32_t localClientNum)
{
    if (localClientNum != 0)
        return;
    SwitchRumbleHd_StopAllKeyed(Switch_RumbleHdPlayer());
    for (auto &b : s_bound)
        b.key = 0;
}

int32_t CG_RumbleEvent(int32_t localClientNum, int32_t event, int32_t rumbleIndex, int32_t entityNum,
                       const float *entityOrigin)
{
    const char *name = nullptr;
    switch (event)
    {
    case EV_PLAY_RUMBLE_ON_ENT:
    case EV_PLAY_RUMBLE_ON_POS:
    case EV_PLAY_RUMBLELOOP_ON_ENT:
    case EV_PLAY_RUMBLELOOP_ON_POS:
    case EV_STOP_RUMBLE:
        name = CL_GetConfigString(localClientNum, (uint32_t)(rumbleIndex + CS_RUMBLES));
        break;
    case EV_STOP_ALL_RUMBLES:
        CG_StopAllRumbles(localClientNum);
        return 1;
    default:
        return 0;
    }

    switch (event)
    {
    case EV_PLAY_RUMBLE_ON_ENT: Play(localClientNum, name, entityNum, entityOrigin, false); break;
    case EV_PLAY_RUMBLE_ON_POS: CG_PlayRumbleOnPosition(localClientNum, name, entityOrigin); break;
    case EV_PLAY_RUMBLELOOP_ON_ENT: Play(localClientNum, name, entityNum, entityOrigin, true); break;
    case EV_PLAY_RUMBLELOOP_ON_POS: CG_PlayRumbleLoopOnPosition(localClientNum, name, entityOrigin); break;
    case EV_STOP_RUMBLE: CG_StopRumble(localClientNum, entityNum, name); break;
    }
    return 1;
}

void CG_RumbleReload(int32_t localClientNum, int32_t kind, float seconds)
{
    if (localClientNum != 0)
        return;
    SwitchRumbleHd_TriggerReload(Switch_RumbleHdPlayer(), kind, seconds);
}

void CG_RumbleFrame(int32_t localClientNum)
{
    if (localClientNum != 0)
        return;
    SwitchRumbleHdPlayer *p = Switch_RumbleHdPlayer();
    if (p == nullptr)
        return;
    SwitchRumbleHd_SetListener(p, CG_GetLocalClientGlobals(localClientNum)->refdef.vieworg);
    for (auto &b : s_bound)
    {
        if (b.key == 0)
            continue;
        float o[3];
        EntityOrigin(localClientNum, b.entityNum, o);
        SwitchRumbleHd_SetVoiceOrigin(p, b.key, o);
    }
}

#endif // __SWITCH__
#endif // KISAK_SP
