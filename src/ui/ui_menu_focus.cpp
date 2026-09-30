#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

// Exact original focus and cursor traversal moved from ui_shared.cpp.  They
// remain the only owner of controller cursor semantics on Switch and desktop.
#include "ui_shared.h"
#include <client/client.h>

const rectDef_s *__cdecl Item_GetTextRect(int localClientNum, const itemDef_s *item)
{
    if (localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            43,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            localClientNum,
            1);
    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 44, 0, "%s", "item");
    return &item->textRect[localClientNum];
}


bool __cdecl Rect_ContainsPoint(int localClientNum, const rectDef_s *rect, float x, float y)
{
    float compareY; // [esp+8h] [ebp-24h]
    rectDef_s compareRect; // [esp+Ch] [ebp-20h] BYREF
    const ScreenPlacement *scrPlace; // [esp+24h] [ebp-8h]
    float compareX; // [esp+28h] [ebp-4h]

    if (!rect)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 492, 0, "%s", "rect");
    compareRect.x = rect->x;
    compareRect.y = rect->y;
    compareRect.w = rect->w;
    compareRect.h = rect->h;
    scrPlace = &scrPlaceView[localClientNum];
    compareX = ScrPlace_ApplyX(scrPlace, x, 4);
    compareY = ScrPlace_ApplyY(scrPlace, y, 4);
    ScrPlace_ApplyRect(
        scrPlace,
        &compareRect.x,
        &compareRect.y,
        &compareRect.w,
        &compareRect.h,
        rect->horzAlign,
        rect->vertAlign);
    return compareRect.x <= (double)compareX
        && compareX <= compareRect.x + compareRect.w
        && compareRect.y <= (double)compareY
        && compareY <= compareRect.y + compareRect.h;
}


bool __cdecl Item_IsTextField(const itemDef_s *item)
{
    bool result; // al

    switch (item->type)
    {
    case 4:
    case 9:
    case 0x10:
    case 0x11:
    case 0x12:
        result = 1;
        break;
    default:
        result = 0;
        break;
    }
    return result;
}


int __cdecl Item_SetFocus(UiContext *dc, itemDef_s *item, float x, float y)
{
    rectDef_s r; // [esp+28h] [ebp-2Ch] BYREF
    const rectDef_s *textRect; // [esp+40h] [ebp-14h]
    itemDef_s *oldFocus; // [esp+44h] [ebp-10h]
    menuDef_t *focusedMenu; // [esp+48h] [ebp-Ch]
    menuDef_t *parent; // [esp+4Ch] [ebp-8h]
    int i; // [esp+50h] [ebp-4h]

    if (!item)
    {
        MyAssertHandler(".\\ui\\ui_shared.cpp", 2183, 0, "%s", "item != NULL");
        return 0;
    }
    if ((item->window.staticFlags & 0x100000) != 0 || !Window_IsVisible(dc->localClientNum, &item->window))
        return 0;
    if (Window_HasFocus(dc->localClientNum, &item->window) && Item_IsVisible(dc->localClientNum, item))
        return 1;
    parent = item->parent;
    if (parent)
    {
        if (!Window_HasFocus(dc->localClientNum, &parent->window))
        {
            focusedMenu = Menu_GetFocused(dc);
            if (focusedMenu)
            {
                if (Rect_ContainsPoint(dc->localClientNum, &focusedMenu->window.rect, x, y)
                    && Rect_ContainsPoint(dc->localClientNum, &parent->window.rect, x, y))
                {
                    return 0;
                }
            }
        }
    }
    if ((item->dvarFlags & 3) != 0 && !Item_EnableShowViaDvar(item, 1))
        return 0;
    if (!Item_IsVisible(dc->localClientNum, item))
        return 0;
    oldFocus = Menu_ClearFocus(dc, item->parent);
    if (item->type)
    {
        Window_AddDynamicFlags(dc->localClientNum, &item->window, 2);
        if (item->onFocus)
            Item_RunScript(dc, item, (char*)item->onFocus);
    }
    else
    {
        textRect = Item_GetTextRect(dc->localClientNum, item);
        r = *textRect;
        r.y = r.y - r.h;
        r.horzAlign = textRect->horzAlign;
        r.vertAlign = textRect->vertAlign;
        if (Rect_ContainsPoint(dc->localClientNum, &r, x, y))
        {
            Window_AddDynamicFlags(dc->localClientNum, &item->window, 2);
        }
        else if (oldFocus)
        {
            Window_AddDynamicFlags(dc->localClientNum, &oldFocus->window, 2);
            if (oldFocus->onFocus)
                Item_RunScript(dc, oldFocus, (char*)oldFocus->onFocus);
        }
    }
    for (i = 0; i < parent->itemCount; ++i)
    {
        if (parent->items[i] == item && !Item_IsTextField(item))
        {
            Menu_SetCursorItem(dc->localClientNum, parent, i);
            return 1;
        }
    }
    return 1;
}


itemDef_s *__cdecl Menu_ClearFocus(UiContext *dc, menuDef_t *menu)
{
    itemDef_s *ret; // [esp+Ch] [ebp-8h]
    int i; // [esp+10h] [ebp-4h]

    if (!menu)
        return 0;
    ret = 0;
    for (i = 0; i < menu->itemCount; ++i)
    {
        if (Window_HasFocus(dc->localClientNum, &menu->items[i]->window))
        {
            if (ret)
                MyAssertHandler(".\\ui\\ui_shared.cpp", 471, 0, "%s", "ret == NULL");
            ret = menu->items[i];
            Window_RemoveDynamicFlags(dc->localClientNum, &ret->window, 2);
            if (menu->items[i]->leaveFocus)
                Item_RunScript(dc, menu->items[i], (char*)menu->items[i]->leaveFocus);
        }
    }
    return ret;
}


itemDef_s *__cdecl Menu_SetPrevCursorItem(UiContext *dc, menuDef_t *menu)
{
    int v3; // [esp+Ch] [ebp-40h]
    int v4; // [esp+14h] [ebp-38h]
    int v5; // [esp+1Ch] [ebp-30h]
    int v6; // [esp+24h] [ebp-28h]
    int v7; // [esp+2Ch] [ebp-20h]
    int v8; // [esp+34h] [ebp-18h]
    int v9; // [esp+3Ch] [ebp-10h]
    int localClientNum; // [esp+40h] [ebp-Ch]
    int oldCursor; // [esp+44h] [ebp-8h]
    int wrapped; // [esp+48h] [ebp-4h]

    wrapped = 0;
    localClientNum = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            localClientNum,
            1);
    oldCursor = menu->cursorItem[localClientNum];
    v9 = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            v9,
            1);
    if (menu->cursorItem[v9] < 0)
    {
        Menu_SetCursorItem(dc->localClientNum, menu, menu->itemCount - 1);
        wrapped = 1;
    }
    do
    {
        v8 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v8,
                1);
        if (menu->cursorItem[v8] <= -1)
            goto LABEL_27;
        v7 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v7,
                1);
        Menu_SetCursorItem(dc->localClientNum, menu, menu->cursorItem[v7] - 1);
        v6 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v6,
                1);
        if (menu->cursorItem[v6] < 0 && !wrapped)
        {
            wrapped = 1;
            Menu_SetCursorItem(dc->localClientNum, menu, menu->itemCount - 1);
        }
        v5 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v5,
                1);
        if (menu->cursorItem[v5] < 0)
        {
        LABEL_27:
            Menu_SetCursorItem(dc->localClientNum, menu, oldCursor);
            return 0;
        }
        v4 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v4,
                1);
    } while (!Item_SetFocus(dc, menu->items[menu->cursorItem[v4]], dc->cursor.x, dc->cursor.y));
    v3 = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            v3,
            1);
    return menu->items[menu->cursorItem[v3]];
}

itemDef_s *__cdecl Menu_SetNextCursorItem(UiContext *dc, menuDef_t *menu)
{
    int v3; // [esp+Ch] [ebp-38h]
    int v4; // [esp+14h] [ebp-30h]
    int v5; // [esp+1Ch] [ebp-28h]
    int v6; // [esp+24h] [ebp-20h]
    int v7; // [esp+2Ch] [ebp-18h]
    int v8; // [esp+34h] [ebp-10h]
    int localClientNum; // [esp+38h] [ebp-Ch]
    int oldCursor; // [esp+3Ch] [ebp-8h]
    int wrapped; // [esp+40h] [ebp-4h]

    wrapped = 0;
    localClientNum = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            localClientNum,
            1);
    oldCursor = menu->cursorItem[localClientNum];
    v8 = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            v8,
            1);
    if (menu->cursorItem[v8] == -1)
    {
        Menu_SetCursorItem(dc->localClientNum, menu, 0);
        wrapped = 1;
    }
    do
    {
        v7 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v7,
                1);
        if (menu->cursorItem[v7] >= menu->itemCount)
        {
            Menu_SetCursorItem(dc->localClientNum, menu, oldCursor);
            return 0;
        }
        v6 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v6,
                1);
        Menu_SetCursorItem(dc->localClientNum, menu, menu->cursorItem[v6] + 1);
        v5 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v5,
                1);
        if (menu->cursorItem[v5] >= menu->itemCount)
        {
            if (wrapped)
                return menu->items[oldCursor];
            wrapped = 1;
            Menu_SetCursorItem(dc->localClientNum, menu, 0);
        }
        v4 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v4,
                1);
    } while (!Item_SetFocus(dc, menu->items[menu->cursorItem[v4]], dc->cursor.x, dc->cursor.y));
    v3 = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            v3,
            1);
    return menu->items[menu->cursorItem[v3]];
}

