#include "switch_settings_hud.h"

#include <cgame/cg_local.h>
#include <cgame/cg_main.h>
#include <ui/ui_shared.h>
#include <gfx_d3d/r_dynres.h>
#include <qcommon/qcommon.h>
#include <universal/q_shared.h>

static const char *HudDvarValue(void *, const char *name)
{
    const dvar_s *dvar = Dvar_FindVar(name);
    return dvar ? Dvar_DisplayableValue(dvar) : nullptr;
}

float SwSettingsHud_Draw(const ScreenPlacement *scrPlace, float y)
{
    static const float warn[4] = {1.0f, 0.55f, 0.0f, 1.0f};
    const dvar_s *mapDvar = Dvar_FindVar("mapname");
    SwHudInput in;
    in.get = HudDvarValue;
    in.ctx = nullptr;
    in.map = mapDvar ? Dvar_DisplayableValue(mapDvar) : "?";
    in.sceneWidth = (int)R_DynResSceneWidth();
    in.sceneHeight = (int)R_DynResSceneHeight();
    SwHudText t;
    SwHud_Format(in, t);

    const float x = cg_debugInfoCornerOffset->current.value + scrPlace->virtualViewableMax[0] - scrPlace->virtualViewableMin[0];
    const char *lines[3] = {t.line1, t.line2, t.hasDebug ? t.debug : nullptr};
    for (int i = 0; i < 3; ++i)
    {
        if (!lines[i])
            continue;
        const int dy = CG_DrawDevString(scrPlace, x, y, 0.75f, 0.75f, (char *)lines[i], i == 2 ? warn : colorWhite, 6,
                                        cgMedia.smallDevFont);
        y += (float)dy * 0.75f;
    }
    return y;
}
