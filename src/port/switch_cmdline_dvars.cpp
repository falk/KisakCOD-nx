// Engine glue for switch_cmdline_dvars.h: command-line `set` overrides do not
// leak into the profile's config.cfg.

#include "switch_cmdline_dvars.h"

#include <qcommon/cmd.h>
#include <qcommon/qcommon.h>

void Switch_CmdlineDvarSet()
{
    const char *name = Cmd_Argv(1);
    const dvar_s *before = Dvar_FindVar(name);
    Sw_CmdlineDvarBeforeSet(&g_swCmdlineDvars, name, before ? Dvar_DisplayableLatchedValue(before) : nullptr);
    Dvar_Set_f();
    const dvar_s *after = Dvar_FindVar(name);
    if (after)
        Sw_CmdlineDvarAfterSet(&g_swCmdlineDvars, name, Dvar_DisplayableLatchedValue(after));
}

const char *Switch_CmdlineDvarPersistValue(const dvar_s *dvar, const char *current)
{
    return Sw_CmdlineDvarPersistValue(&g_swCmdlineDvars, dvar->name, current);
}
