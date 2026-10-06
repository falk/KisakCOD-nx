#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include <stdint.h>

// Script-driven rumble (playRumbleOnEntity / playRumbleLoopOnEntity /
// playRumbleOnPosition / playRumbleLoopOnPosition / stopRumble /
// stopAllRumbles).  The rumble name arrives as a configstring; the curve
// comes from the port's own name table.
void CG_PlayRumbleOnClient(int32_t localClientNum, const char *name);
void CG_PlayRumbleOnEntity(int32_t localClientNum, const char *name, int32_t entityNum);
void CG_PlayRumbleOnPosition(int32_t localClientNum, const char *name, const float *origin);
void CG_PlayRumbleLoopOnEntity(int32_t localClientNum, const char *name, int32_t entityNum);
void CG_PlayRumbleLoopOnPosition(int32_t localClientNum, const char *name, const float *origin);
void CG_StopRumble(int32_t localClientNum, int32_t entityNum, const char *name);
void CG_StopAllRumbles(int32_t localClientNum);

// Entity-event entry point: handles EV_PLAY_RUMBLE_* / EV_STOP_RUMBLE /
// EV_STOP_ALL_RUMBLES (rumbleIndex is the event parm, i.e. the configstring
// offset past CS_RUMBLES).  Returns 1 when the event was a rumble event.
int32_t CG_RumbleEvent(int32_t localClientNum, int32_t event, int32_t rumbleIndex, int32_t entityNum,
                       const float *entityOrigin);

// Reload stages for the local player (kind: SwitchRumbleHdReloadKind,
// seconds: the weapon's reload duration).
void CG_RumbleReload(int32_t localClientNum, int32_t kind, float seconds);

// Once per frame: points the falloff listener at the view and moves
// entity-bound rumbles to their entity.
void CG_RumbleFrame(int32_t localClientNum);
