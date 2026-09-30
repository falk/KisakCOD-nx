#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include "ui.h"

// Exact menu-query primitives moved out of ui_shared.cpp.  They are used by
// both the normal SP UI and the Switch input closure.
bool __cdecl Window_IsVisible(int localClientNum, const windowDef_t *w)
{
    if (!w)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 60, 0, "%s", "w");
    if (localClientNum)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 23, 0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\\n\\t%i not in [0, %i)",
            localClientNum, 1);
    return (w->dynamicFlags[localClientNum] & 4) != 0;
}

bool __cdecl Window_HasFocus(int localClientNum, const windowDef_t *w)
{
    if (!w)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 70, 0, "%s", "w");
    if (Window_IsVisible(localClientNum, w))
    {
        if (localClientNum)
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 23, 0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\\n\\t%i not in [0, %i)",
                localClientNum, 1);
        return (w->dynamicFlags[localClientNum] & 2) != 0;
    }
    if (localClientNum)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 23, 0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\\n\\t%i not in [0, %i)",
            localClientNum, 1);
    if ((w->dynamicFlags[localClientNum] & 2) != 0 && !alwaysfails)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 74, 0, "Hidden window has focus!");
    return 0;
}

menuDef_t *__cdecl Menu_GetFocused(UiContext *dc)
{
    for (int i = dc->openMenuCount - 1; i >= 0; --i)
    {
        if (Window_HasFocus(dc->localClientNum, &dc->menuStack[i]->window)
            && Window_IsVisible(dc->localClientNum, &dc->menuStack[i]->window))
            return dc->menuStack[i];
    }
    return nullptr;
}

int __cdecl Menus_AnyFullScreenVisible(UiContext *dc)
{
    for (int i = dc->openMenuCount - 1; i >= 0; --i)
    {
        if (Window_IsVisible(dc->localClientNum, &dc->menuStack[i]->window)
            && dc->menuStack[i]->fullScreen)
            return 1;
    }
    return 0;
}

int __cdecl Menu_Count(UiContext *dc)
{
    return dc->menuCount;
}
