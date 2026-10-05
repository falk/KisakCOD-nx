#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

// Controller and desktop keys share the SP menu dispatch owner.
#include "ui_shared.h"

#include <client/client.h>
#include <qcommon/cmd.h>
#ifdef __SWITCH__
#include <port/switch_ui_menus.h>
#endif

extern int g_waitingForKey;
extern int g_editingField;
extern itemDef_s *g_editItem;
extern int g_debugMode;
void __cdecl Menus_HandleOOBClick(UiContext *dc, menuDef_t *menu, int key, int down);

static bool Menu_HasInteractiveParent(UiContext *dc, menuDef_t *menu)
{
    bool below = false;
    for (int i = dc->openMenuCount - 1; i >= 0; --i)
    {
        menuDef_t *parent = dc->menuStack[i];
        if (parent == menu) { below = true; continue; }
        // A background-only menu cannot receive control when Back closes a page.
        if (below && Window_IsVisible(dc->localClientNum, &parent->window))
            for (int j = 0; j < parent->itemCount; ++j)
            {
                const itemDef_s *candidate = parent->items[j];
                if (!(candidate->window.staticFlags & 0x100000)
                    && (candidate->type || candidate->action || candidate->onKey))
                    return true;
            }
    }
    return false;
}

itemDef_s *g_bindItem;
int inHandleKey;
void __cdecl Menu_HandleKey(UiContext *dc, menuDef_t *menu, int key, int down)
{
    int v4; // eax
    const rectDef_s *v5; // eax
    float x; // [esp+0h] [ebp-1A4h]
    float y; // [esp+4h] [ebp-1A0h]
    itemDef_s it; // [esp+1Ch] [ebp-188h] BYREF
    itemDef_s *item; // [esp+194h] [ebp-10h]
    int inHandler; // [esp+198h] [ebp-Ch]
    int i; // [esp+19Ch] [ebp-8h]
    const char *binding; // [esp+1A0h] [ebp-4h]

    item = 0;
    inHandler = 1;
    if (g_waitingForKey && down)
    {
        Item_Bind_HandleKey(dc, g_bindItem, key, down);
        inHandler = 0;
        return;
    }
    if (!g_editingField || !down)
        goto LABEL_13;
    if (!Item_TextField_HandleKey(dc, g_editItem, key))
    {
        g_editingField = 0;
        g_editItem = 0;
        inHandler = 0;
        return;
    }
    if (key == 200 || key == 201 || key == 202)
    {
        g_editingField = 0;
        g_editItem = 0;
        Display_MouseMove(dc);
    LABEL_13:
        if (menu)
        {
            if (down
                && (key & 0x400) == 0
                && menu->allowedBinding
                && (binding = Key_GetBinding(dc->localClientNum, key)) != 0
                && !I_stricmp(binding, menu->allowedBinding))
            {
                v4 = CL_ControllerIndexFromClientNum(dc->localClientNum);
                Cbuf_ExecuteBuffer(dc->localClientNum, v4, (char *)binding);
            }
            else if (!down
                || (menu->window.staticFlags & 0x1000000) != 0
                || menu->fullScreen
                || Rect_ContainsPoint(dc->localClientNum, &menu->window.rect, dc->cursor.x, dc->cursor.y)
                || inHandleKey
                || key != 200 && key != 201 && key != 202)
            {
                for (i = 0; i < menu->itemCount; ++i)
                {
                    if (Item_IsVisible(dc->localClientNum, menu->items[i])
                        && ((menu->items[i]->dvarFlags & 3) == 0
                            || Item_EnableShowViaDvar(menu->items[i], 1)))
                    {
                        if (Window_HasFocus(dc->localClientNum, &menu->items[i]->window))
                            item = menu->items[i];
                    }
                }
                if (key != 205 && key != 206 || item && item->type == 6)
                {
                    if (item && Item_HandleKey(dc, item, key, down))
                    {
                        Item_Action(dc, item);
                        inHandler = 0;
                    }
                    else if (down)
                    {
                        if (key <= 0 || key > 255 || !Menu_CheckOnKey(dc, menu, key))
                        {
                            switch (key)
                            {
                            case 9:
                            case 155:
                            case 157:
                            case 189:
                            case 205:
                                Menu_SetNextCursorItem(dc, menu);
                                break;
                            case 13:
                            case 191:
                            case 202:
                                if (item)
                                {
                                    if (Item_IsTextField(item))
                                    {
#ifdef __SWITCH__
                                        if (Switch_UI_EditTextField(dc, item))
                                            break;
#endif
                                        item->cursorPos[dc->localClientNum] = 0;
                                        g_editingField = 1;
                                        g_editItem = item;
                                        Key_SetOverstrikeMode(dc->localClientNum, 1);
                                    }
                                    else
                                    {
                                        Item_Action(dc, item);
                                    }
                                }
                                break;
                            case 27:
                                if (!g_waitingForKey && menu->onESC)
                                {
                                    it.parent = menu;
                                    Item_RunScript(dc, &it, (char*)menu->onESC);
                                }
                                else if (!g_waitingForKey && Menu_HasInteractiveParent(dc, menu))
                                {
                                    // Missing Escape scripts return to an interactive
                                    // parent; the root stays above its backdrop.
                                    Menus_Close(dc, menu);
                                }
                                break;
                            case 154:
                            case 156:
                            case 183:
                            case 206:
                                Menu_SetPrevCursorItem(dc, menu);
                                break;
                            case 177:
                                if (Dvar_GetInt("developer"))
                                    g_debugMode ^= 1u;
                                break;
                            case 178:
                                if (Dvar_GetInt("developer"))
                                    Cbuf_AddText(dc->localClientNum, "screenshot\n");
                                break;
                            case 200:
                            case 201:
                                if (item)
                                {
                                    if (item->type)
                                    {
                                        if (Rect_ContainsPoint(dc->localClientNum, &item->window.rect, dc->cursor.x, dc->cursor.y))
                                        {
                                            if (Item_IsTextField(item))
                                                Item_TextField_BeginEdit(dc->localClientNum, item);
                                            else
                                                Item_Action(dc, item);
                                        }
                                    }
                                    else
                                    {
                                        y = dc->cursor.y;
                                        x = dc->cursor.x;
                                        v5 = Item_CorrectedTextRect(dc->localClientNum, item);
                                        if (Rect_ContainsPoint(dc->localClientNum, v5, x, y))
                                            Item_Action(dc, item);
                                    }
                                }
                                break;
                            default:
                                return;
                            }
                        }
                    }
                    else
                    {
                        inHandler = 0;
                    }
                }
            }
            else
            {
                inHandleKey = 1;
                Menus_HandleOOBClick(dc, menu, key, down);
                inHandleKey = 0;
                inHandler = 0;
            }
        }
        else
        {
            inHandler = 0;
        }
    }
}
