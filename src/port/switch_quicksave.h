// Switch quick save / quick load: the console's own savegame commands on the
// pad, plus a boot-into-save route for automation.
//
// Retail has no manual save menu of its own: a save is the console/script
// command `devsave <name>` (sv_ccmds.cpp SV_SaveGame_f, SAVE_TYPE_CONSOLE,
// commitLevel 6: memory commit *and* device write) and a load is
// `loadgame <name>` (SV_LoadGame_f -> SV_CheckLoadGame -> SV_MapRestart or
// `map <profile>/save/<name>.svg` -> SV_SpawnServer(map, savegame=1)).
// Both are retail paths; this file only gives the pad somewhere to press them
// and prints the fingerprints the round-trip gate compares.

#ifndef KISAK_SWITCH_QUICKSAVE_H
#define KISAK_SWITCH_QUICKSAVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Registers the port dvars.  Call this *after* Com_Init: registering the
// string dvars before it (next to RetailKillhouseRegisterPortDvars) made boot
// die inside MT_Alloc with an empty script memory tree.
void Switch_QuickSaveRegisterDvars(void);

// One pad frame's raw SWITCH_INPUT_BUTTON_* mask (IN_Frame, both the physical
// and the scripted-pad path).  MINUS rising edge: save while the player drives,
// load while a menu owns input (the user's Plus+Minus).
void Switch_QuickSavePadEdge(uint64_t buttons);

// Once per main-loop frame (frameNum is the caller's Com_Frame counter): the
// checkpoint persist, the boot autoload, the frame-numbered automation triggers
// and the post-load fingerprint.  Returns 1 on the frame it queued the boot
// load, so the caller can skip the fresh `spmap` autostart.
int Switch_QuickSaveFrame(int frameNum);

// switch_autoCmds "<seconds>:<console command>,<seconds>:<command>,...": each
// command is queued once, from the main loop, when that many seconds have
// passed since boot (timed console script for unattended stress runs:
// map_restart, disconnect, spmap ...).  Logs SWITCH_AUTOCMD per command.
void Switch_AutoCmdFrame(void);

#ifdef __cplusplus
}
#endif

#endif  // KISAK_SWITCH_QUICKSAVE_H
