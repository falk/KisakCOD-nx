#ifndef SWITCH_SP_LAUNCH_H
#define SWITCH_SP_LAUNCH_H

// The Switch build ships only the single-player executable, so a request to
// relaunch as multiplayer cannot be honoured. Refuses with a printed line and
// touches nothing else: the renderer and worker threads keep running.

typedef void (*SwitchPrintLineFn)(const char *line);

// Always returns false (nothing was started).
static inline bool Switch_StartMultiplayerRefused(SwitchPrintLineFn print)
{
    if (print)
        print("startMultiplayer: not available in the Switch single-player build\n");
    return false;
}

#endif
