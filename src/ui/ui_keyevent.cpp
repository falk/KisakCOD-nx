#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include "ui.h"

#include <client/client.h>

// Kept as the original UI owner, but split out of ui_main.cpp so the input
// seam can gain its Switch dependency closure without pulling unrelated UI
// startup, savegame, profile, and cgame entrypoints into that closure.
void __cdecl UI_KeyEvent(int localClientNum, int key, int down)
{
    menuDef_t *Focused;

    if (Menu_Count(&uiInfo.uiDC))
    {
        Focused = Menu_GetFocused(&uiInfo.uiDC);
        if (!Focused)
            goto LABEL_10;
        if (key != 2 || !down || Menus_AnyFullScreenVisible(&uiInfo.uiDC) || Focused->onESC)
            Menu_HandleKey(&uiInfo.uiDC, Focused, key, down);
        else
            Menus_CloseAll(&uiInfo.uiDC);
        if (!Menu_GetFocused(&uiInfo.uiDC))
        {
        LABEL_10:
            Key_RemoveCatcher(localClientNum, -17);
            Key_ClearStates(localClientNum);
            if (!CL_SkipRendering())
                Dvar_SetIntByName("cl_paused", 0);
        }
    }
}
