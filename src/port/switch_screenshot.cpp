// the `screenshot` / `screenshotJpeg` console commands on Switch.
//
// r_screenshot.cpp is the Win32 implementation (GDI+ JPEG encoding, front
// buffer reads through the Win32 swap chain) and stays out of the Switch
// build.  The Switch renderer already owns a real backbuffer readback --
// RB_SwapBuffers' capture ring dumps the presented frame as PNG through
// GetRenderTargetData -- so the command is served by that same path: it
// picks the next free screenshots/shotNNNN.png under fs_homepath, requests
// one capture, and the present that follows writes it.  The result is
// reported on the frame after that (RB_PollRequestedScreenshot), so the
// console message reflects what actually landed on the card.
//
// `screenshot savegame <path>` writes the 256x128 saved-game thumbnail
// <path>.svt (switch_save_thumb.h) under the game directory; the save
// writer requests the same capture for every save. `levelshot` feeds the
// Win32-only level-shot path and is reported as unsupported.
#include <universal/q_shared.h>
#include <gfx_d3d/r_screenshot.h>
#include <gfx_d3d/rb_backend.h>
#include <qcommon/cmd.h>
#include <universal/com_files.h>
#include "switch_save_writer.h"

#include <cstdio>
#include <cstring>

#ifdef __SWITCH__

static int s_lastNumber = 0;
static char s_pendingName[256] = "";
static bool s_pendingSilent = false;

void __cdecl R_ScreenshotCommand(GfxScreenshotType type)
{
    (void)type; // both spellings write PNG on Switch
    if (!strcmp(Cmd_Argv(1), "savegame"))
    {
        if (Cmd_Argc() != 3 || !*Cmd_Argv(2))
        {
            Com_Printf(8, "Usage: screenshot savegame <path>\n");
            return;
        }
        char qpath[256], ospath[260];
        Com_sprintf(qpath, sizeof(qpath), "%s.svg", Cmd_Argv(2));
        FS_BuildOSPath(fs_homepath->current.string, fs_gameDirVar->current.string, qpath, ospath);
        SwitchSaveThumb_Request(ospath);
        return;
    }
    if (!strcmp(Cmd_Argv(1), "levelshot"))
    {
        Com_Printf(8, "ScreenShot: %s is not supported on Switch\n", Cmd_Argv(1));
        return;
    }
    if (s_pendingName[0])
    {
        Com_Printf(8, "ScreenShot: previous capture still pending\n");
        return;
    }

    const bool silent = strcmp(Cmd_Argv(1), "silent") == 0;
    const bool autoNumbered = Cmd_Argc() != 2 || silent;
    char qpath[256];
    if (autoNumbered)
    {
        while (s_lastNumber <= 9999)
        {
            Com_sprintf(qpath, sizeof(qpath), "screenshots/shot%04d.png", s_lastNumber);
            if (!FS_FileExists(qpath))
                break;
            ++s_lastNumber;
        }
        if (s_lastNumber >= 9999)
        {
            Com_Printf(8, "ScreenShot: Couldn't create a file\n");
            return;
        }
    }
    else
    {
        Com_sprintf(qpath, sizeof(qpath), "screenshots/%s.png", Cmd_Argv(1));
    }

    char ospath[256];
    FS_BuildOSPath(fs_homepath->current.string, fs_gameDirVar->current.string, qpath, ospath);
    FS_CreatePath(ospath);
    Com_sprintf(s_pendingName, sizeof(s_pendingName), "%s", qpath);
    s_pendingSilent = silent;
    if (autoNumbered)
        ++s_lastNumber;
    RB_RequestScreenshot(ospath);
}

// Called once per client frame (CL_Frame) so the console message follows the
// present that wrote the file.
void Switch_ScreenshotFrame()
{
    bool ok = false;
    if (!s_pendingName[0] || !RB_PollRequestedScreenshot(&ok))
        return;
    if (ok)
    {
        if (!s_pendingSilent)
            Com_Printf(8, "Wrote %s\n", s_pendingName);
    }
    else if (!s_pendingSilent)
    {
        Com_Printf(8, "ScreenShot: Couldn't create a file\n");
    }
    s_pendingName[0] = '\0';
}

#endif // __SWITCH__
