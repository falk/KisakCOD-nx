// `switch_introDiag N`: per rendered cgame frame after a load, what the hud
// holds against the snapshot times, to see whether a frame is drawn before the
// level script's overlay reached the client.
#include <universal/q_shared.h>
#include <cgame/cg_local.h>
#include <cgame/cg_public.h>
#include <cgame/cg_main.h>
#include <bgame/bg_local.h>
#include <qcommon/qcommon.h>
#include <limits.h>

#include <client/client.h>
#include <gfx_d3d/r_cinematic.h>

#include "switch_perf.h"

extern "C" int Switch_ClientGameplayActive(void);

extern "C" void Switch_IntroDiag(int localClientNum)
{
    static const dvar_t *dvar = Dvar_RegisterInt("switch_introDiag", 0, 0, 100000, DVAR_NOFLAG,
        "Log the hud overlays of the first N rendered frames after a load");
    static int s_frame;
    static int s_lastTime = INT_MAX;

    if (!Switch_ClientGameplayActive() || CL_SkipRendering() || !CG_GetLocalClientGlobals(localClientNum)->nextSnap)
    {
        s_frame = 0;
        return;
    }
    cg_s *cg = CG_GetLocalClientGlobals(localClientNum);
    if (cg->time < s_lastTime)
        s_frame = 0;
    s_lastTime = cg->time;
    if (dvar->current.integer <= 0 || s_frame++ >= dvar->current.integer)
        return;

    hudelem_s *elems[1025];
    const int count = GetSortedHudElems(localClientNum, elems);
    char line[512];
    int len = Com_sprintf(line, sizeof(line), "INTRO_FRAME %d cgtime=%d snap=%d next=%d cin=%d/%d huds=%d", s_frame - 1, cg->time,
        cg->snap ? cg->snap->serverTime : -1, cg->nextSnap ? cg->nextSnap->serverTime : -1, R_Cinematic_IsStarted() ? 1 : 0, R_Cinematic_IsPending() ? 1 : 0,
        count);
    for (int i = 0; i < count && i < 8 && len < (int)sizeof(line) - 64; ++i)
    {
        hudelem_color_t c;
        BG_LerpHudColors(elems[i], cg->time, &c);
        len += Com_sprintf(line + len, sizeof(line) - len, " [t%d %dx%d a%d]", (int)elems[i]->type, elems[i]->width,
            elems[i]->height, (int)c.a);
    }
    Com_Printf(CON_CHANNEL_CLIENT, "%s\n", line);
}
