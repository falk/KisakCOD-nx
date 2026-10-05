// Switch quick save / quick load.
//
// Pad (user's mapping): MINUS pressed while the player is being driven saves
// (`devsave <switch_save_name>`); MINUS pressed while a menu owns input -- the
// pause menu, opened with Plus -- loads (`loadgame <switch_save_name>`).  Both
// are the retail console commands; nothing here re-implements save or load.
//
// Automation: `switch_save_autoload <name>` loads that savegame out of the main
// menu instead of the fresh `spmap` autostart, and
// `switch_save_autoAfterFrames N` / `switch_save_loadAfterFrames M` save after N
// and load M frames later, counted in *controllable* frames so the timing does
// not depend on how fast the host runs.
//
// Evidence for offline tooling: the
// KISAK_QUICKSAVE line records the state a save captured (player position,
// health, level clock, script clock) and the KISAK_QUICKLOAD ... origin=ingame
// line records the same state once the loaded game is controllable again, so
// the two must agree.
#ifdef __SWITCH__

#include "switch_perf.h"
#include "switch_quicksave.h"

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <universal/assertive.h>
#include <universal/com_files.h>
#include <qcommon/com_playerprofile.h>
#include <server/server.h>
#include <game/g_main.h>
#include <script/scr_main.h>
#include <gfx_d3d/r_cinematic.h>
#include <game/savedevice.h>
#include <game/savememory.h>
#include <platform/switch/switch_input.h>
#include "switch_pad_layout.h"

#include <cstdlib>

// cl_input.cpp: true while the local player is being driven with no UI key
// catcher up, i.e. while A/B and the pause key are gameplay input rather than
// menu input.
extern "C" int Switch_ClientGameplayActive(void);

namespace
{

// The main-loop frame the fresh `spmap` autostart fires on (switch_sp_main.cpp).
constexpr int kBootTriggerFrame = 300;

const dvar_t *s_saveName;
const dvar_t *s_autoload;
const dvar_t *s_autoSaveAfterFrames;
const dvar_t *s_autoLoadAfterFrames;
const dvar_t *s_autoLoadRepeat;
const dvar_t *s_resaveAfterLoad;
const dvar_t *s_padShortcut;
const dvar_t *s_autoCmds;

int s_controllableFrames;
int s_framesSinceSave;
bool s_autoSaved;
bool s_frameLoadFired;
int s_autoLoadsDone;
bool s_loadPending;
char s_pendingLoadName[64];

const char *SaveName()
{
    const char *name = s_saveName ? s_saveName->current.string : "";
    return (name && *name) ? name : "quicksave";
}

void Fingerprint(const char *marker, const char *name, const char *origin)
{
    const gentity_s *player = &g_entities[0];
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int health = 0;
    if (player->client)
    {
        x = player->client->ps.origin[0];
        y = player->client->ps.origin[1];
        z = player->client->ps.origin[2];
        health = player->health;
    }
    // script= is scrVarPub.time, the script clock: it is saved in the script
    // stream and restored by Scr_LoadPre, so it is the one number that proves
    // the script state came back (ext_threadcount counts executing threads and
    // is back to zero once a thread parks).
    Com_Printf(0, "KISAK_%s name=%s origin=%s leveltime=%d script=%u pos=%.1f %.1f %.1f health=%d\n",
               marker, name, origin, level.time, scrVarPub.time, x, y, z, health);
}

bool GameRunning()
{
    return com_sv_running && com_sv_running->current.enabled && sv.state == SS_GAME;
}

void Refuse(const char *marker, const char *name, const char *origin, const char *reason)
{
    Com_Printf(0, "KISAK_%s name=%s origin=%s ok=0 reason=%s\n", marker, name, origin, reason);
}

void RequestSave(const char *origin)
{
    const char *name = SaveName();
    if (!GameRunning())
    {
        Refuse("QUICKSAVE", name, origin, "not-in-game");
        return;
    }
    if (R_Cinematic_IsStarted())
    {
        Refuse("QUICKSAVE", name, origin, "cinematic");
        return;
    }
    Fingerprint("QUICKSAVE", name, origin);
    Cbuf_AddText(0, va("devsave %s\n", name));
}

void RequestLoad(const char *origin)
{
    const char *name = SaveName();
    if (s_loadPending)
    {
        Refuse("QUICKLOAD", name, origin, "load-in-progress");
        return;
    }
    if (!Com_HasPlayerProfile())
    {
        // Com_BuildPlayerProfilePath traps on this, and a trap here would be a
        // fatal error for a key press; say why instead.
        Refuse("QUICKLOAD", name, origin, "no-player-profile");
        return;
    }

    char path[256];
    if (Com_BuildPlayerProfilePath(path, sizeof(path), "save/%s.svg", name) >= (int)sizeof(path))
    {
        Refuse("QUICKLOAD", name, origin, "path-too-long");
        return;
    }
    if (!SaveExists(path))
    {
        Refuse("QUICKLOAD", name, origin, va("missing-save path='%s'", path));
        return;
    }

    Fingerprint("QUICKLOAD", name, origin);
    I_strncpyz(s_pendingLoadName, name, sizeof(s_pendingLoadName));
    s_loadPending = true;
    Cbuf_AddText(0, va("loadgame %s\n", name));
}

// The retail checkpoint chain commits to memory and leaves the device write to
// the UI's `savegame_lastcommit` step (Script_WriteSave ->
// ui_saveTimeGlob.callWrite -> UI_UpdateSaveUI); on this port that step did not
// always arrive, so a mid-mission checkpoint stayed in memory and a crash or a
// reboot lost it (hw-d37.log: `G_WriteGame 'autosave\killhouse-1'` with no
// `time to write` behind it).  This issues the same command the UI would, once
// per commit.
void CheckpointTick()
{
    if (!GameRunning() || s_loadPending)
        return;
    if (!SaveMemory_IsCurrentCommittedSaveValid())
        return;
    SaveGame *committed = SaveMemory_GetLastCommittedSave();
    // isWrittenToDevice is the engine's own bookkeeping: a save that already
    // reached the card (the pad's `devsave`, a level-start save) must not be
    // written again, and a fresh commit resets it in SaveMemory_CreateHeader.
    if (!committed || SaveMemory_IsWrittenToDevice(committed))
        return;

    const SaveHeader *header = &committed->header;
    if (header->demoPlayback || SV_IsInternalSave(header->filename))
        return;
    Com_Printf(0, "KISAK_CHECKPOINT_SAVE file='%s' desc='%s'\n", header->filename,
               header->description);
    Cbuf_AddText(0, "savegame_lastcommit\n");
}

}  // namespace

void Switch_QuickSaveRegisterDvars(void)
{
    s_saveName = Dvar_RegisterString("switch_save_name", "quicksave", DVAR_NOFLAG,
                                     "Savegame name used by the pad quicksave/quickload shortcut");
    s_autoload = Dvar_RegisterString("switch_save_autoload", "", DVAR_NOFLAG,
                                     "Load this savegame out of the main menu instead of starting a fresh map");
    s_autoSaveAfterFrames = Dvar_RegisterInt(
        "switch_save_autoAfterFrames", 0, 0, INT_MAX, DVAR_NOFLAG,
        "Save automatically once the player has been controllable for this many frames (0: off)");
    s_autoLoadAfterFrames = Dvar_RegisterInt(
        "switch_save_loadAfterFrames", 0, 0, INT_MAX, DVAR_NOFLAG,
        "Load again this many frames after that automatic save (0: off)");
    s_autoLoadRepeat = Dvar_RegisterInt(
        "switch_save_loadRepeat", 1, 1, 1000, DVAR_NOFLAG,
        "Number of automatic loads of that save, each loadAfterFrames after the previous load completed");
    s_resaveAfterLoad = Dvar_RegisterBool(
        "switch_save_resaveAfterLoad", false, DVAR_NOFLAG,
        "With loadRepeat: save again autoAfterFrames after each load and load that new save "
        "(the death -> checkpoint reload chain)");
    s_padShortcut = Dvar_RegisterBool("switch_save_pad", true, DVAR_NOFLAG,
                                      "Minus saves, Plus+Minus loads");
    s_autoCmds = Dvar_RegisterString(
        "switch_autoCmds", "", DVAR_NOFLAG,
        "Timed console script: '<seconds>:<command>,<seconds>:<command>,...' each queued once at that many "
        "seconds after boot (unattended stress: map_restart, disconnect, spmap)");
}

// ---- switch_autoCmds -------------------------------------------------------

namespace
{
constexpr int kMaxAutoCmds = 16;
struct AutoCmd
{
    int atMs;
    char text[96];
    bool fired;
};
AutoCmd s_autoCmdList[kMaxAutoCmds];
int s_autoCmdCount = -1; // -1: not parsed yet

void ParseAutoCmds(const char *spec)
{
    s_autoCmdCount = 0;
    const char *p = spec;
    while (*p && s_autoCmdCount < kMaxAutoCmds)
    {
        while (*p == ',' || *p == ' ')
            ++p;
        if (!*p)
            break;
        char *end = nullptr;
        const double seconds = std::strtod(p, &end);
        if (end == p || *end != ':')
        {
            Com_PrintError(0, "switch_autoCmds: expected '<seconds>:<command>' at '%s'\n", p);
            break;
        }
        p = end + 1;
        AutoCmd &cmd = s_autoCmdList[s_autoCmdCount];
        size_t n = 0;
        while (*p && *p != ',' && n + 1 < sizeof(cmd.text))
            cmd.text[n++] = *p++;
        while (*p && *p != ',')
            ++p; // an over-long command is truncated, not merged with the next
        cmd.text[n] = '\0';
        cmd.atMs = (int)(seconds * 1000.0);
        cmd.fired = false;
        if (n)
            ++s_autoCmdCount;
    }
    if (s_autoCmdCount)
        Com_Printf(0, "SWITCH_AUTOCMD parsed=%d\n", s_autoCmdCount);
}
} // namespace

void Switch_AutoCmdFrame(void)
{
    Switch_IntroDiag(0);
    if (!s_autoCmds)
        return;
    if (s_autoCmdCount < 0)
        ParseAutoCmds(s_autoCmds->current.string);
    if (!s_autoCmdCount)
        return;
    const int now = Sys_Milliseconds();
    for (int i = 0; i < s_autoCmdCount; ++i)
    {
        AutoCmd &cmd = s_autoCmdList[i];
        if (cmd.fired || now < cmd.atMs)
            continue;
        cmd.fired = true;
        Com_Printf(0, "SWITCH_AUTOCMD t=%.1fs cmd='%s'\n", now / 1000.0, cmd.text);
        Cbuf_AddText(0, va("%s\n", cmd.text));
    }
}

void Switch_QuickSavePadEdge(uint64_t buttons)
{
    static PadMinusState minus;
    // Hold long enough that a quick tap never flashes the objectives.
    const PadMinusOut out = PadMinusStep(&minus, (buttons & SWITCH_INPUT_BUTTON_MINUS) != 0,
                                         Switch_ClientGameplayActive() != 0,
                                         (uint32_t)Sys_Milliseconds(), 250);
    if (out.scoresDown)
        Cbuf_AddText(0, "+scores\n");
    if (out.scoresUp)
        Cbuf_AddText(0, "-scores\n");
    if (!(out.tapSave || out.menuPress) || !s_padShortcut || !s_padShortcut->current.enabled)
        return;

    if (out.tapSave)
        RequestSave("pad");
    else
        RequestLoad("pad");
}

int Switch_QuickSaveFrame(int frameNum)
{
    if (!s_saveName)
        return 0;

    const bool controllable = Switch_ClientGameplayActive() && GameRunning();
    s_controllableFrames = controllable ? s_controllableFrames + 1 : 0;

    // A load is done when the player is controllable again; the state printed
    // here is what must match the KISAK_QUICKSAVE line.
    if (s_loadPending && controllable)
    {
        Fingerprint("QUICKLOAD", s_pendingLoadName, "ingame");
        s_loadPending = false;
        // switch_save_loadRepeat N: load the same save again, loadAfterFrames
        // controllable frames after each completed load, N loads in total
        // (repeated G_LoadGame stress).
        if (s_frameLoadFired && s_autoLoadRepeat)
        {
            if (++s_autoLoadsDone < s_autoLoadRepeat->current.integer)
            {
                s_frameLoadFired = false;
                s_framesSinceSave = 0;
                // Save state captured *after* a load differs from the first
                // save (e.g. cgame restarts the ambient on the other track
                // pair), which is what a checkpoint reload after death sees.
                if (s_resaveAfterLoad && s_resaveAfterLoad->current.enabled)
                {
                    s_autoSaved = false;
                    s_controllableFrames = 0;
                }
            }
            else if (s_autoLoadRepeat->current.integer > 1)
            {
                Com_Printf(0, "KISAK_QUICKLOAD_REPEAT_DONE loads=%d\n", s_autoLoadsDone);
            }
        }
    }

    // Deterministic automation: save after N frames of controllable play (so it
    // never depends on how fast the host runs), then load N frames later.
    if (s_autoSaveAfterFrames && s_autoSaveAfterFrames->current.integer > 0 && !s_autoSaved
        && s_controllableFrames >= s_autoSaveAfterFrames->current.integer)
    {
        s_autoSaved = true;
        s_framesSinceSave = 0;
        RequestSave("auto");
    }
    if (s_autoSaved && s_autoLoadAfterFrames && s_autoLoadAfterFrames->current.integer > 0
        && !s_frameLoadFired)
    {
        ++s_framesSinceSave;
        if (s_framesSinceSave >= s_autoLoadAfterFrames->current.integer)
        {
            s_frameLoadFired = true;
            RequestLoad("auto");
        }
    }

    CheckpointTick();

    // One-shot boot autoload, on the same frame as the fresh `spmap` autostart.
    if (frameNum == kBootTriggerFrame && s_autoload && *s_autoload->current.string)
    {
        RequestLoad("boot");
        return s_loadPending ? 1 : 0;
    }
    return 0;
}

#endif  // __SWITCH__
