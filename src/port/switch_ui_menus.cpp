// Port-side menu work for the SP UI (see switch_ui_menus.h): the Graphics,
// Controller, Sound and Game pages, adaptation of the retail menus, software
// keyboard entry and saved-game list details. Pure rules live in the
// switch_menu_*.h / switch_text_input.h headers so the host tests cover them.

#include <universal/q_shared.h>
#include <ui/ui.h>
#include <ui/ui_shared.h>
#include <client/client.h>
#include <qcommon/cmd.h>
#include <qcommon/com_playerprofile.h>
#include <stringed/stringed_hooks.h>
#include <universal/com_files.h>
#include <gfx_d3d/r_dynres.h>

#include <deque>
#include <string>

#include "switch_menu_pages.h"
#include "switch_menu_patch.h"
#include "switch_menu_settings.h"
#include "switch_text_input.h"
#include "switch_save_thumb.h"
#include "switch_ui_menus.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

int __cdecl ReadSaveHeader(const char *filename, SaveHeader *header);

// Defined in ui_shared_obj.cpp: serves the page text to the menu parser.
extern const char *(*UI_BuiltinMenuTextProvider)(const char *filename, int *length);

namespace
{
UiContext *s_dc;
std::deque<std::string> s_pageText;
std::deque<std::string> s_patchedText;

const dvar_t *sw_ui_preset;
const dvar_t *sw_ui_dynres;
const dvar_t *sw_status;
bool s_registered;

cmd_function_s s_cmdOpen, s_cmdSync, s_cmdDynres, s_cmdPreset, s_cmdApply, s_cmdReset, s_cmdDump, s_cmdKey;

bool OpsGet(void *, const char *name, char *out, size_t outSize)
{
    if (!Dvar_FindVar(name))
        return false;
    // The controller is chosen with the scene layout at the first frame,
    // whatever the dvar says afterwards.
    if (!strcmp(name, "r_dynres"))
    {
        snprintf(out, outSize, "%d", R_DynResControllerEnabled() ? 1 : 0);
        return true;
    }
    snprintf(out, outSize, "%s", Dvar_GetVariantString(name));
    return true;
}

bool OpsGetLatched(void *, const char *name, char *out, size_t outSize)
{
    const dvar_s *dvar = Dvar_FindVar(name);
    if (!dvar)
        return false;
    snprintf(out, outSize, "%s", Dvar_HasLatchedValue(dvar) ? Dvar_DisplayableLatchedValue(dvar) : Dvar_GetVariantString(name));
    return true;
}

void OpsSet(void *, const char *name, const char *value)
{
    if (Dvar_FindVar(name))
        Dvar_SetFromStringByName(name, value);
}

void OpsReset(void *, const char *name)
{
    if (const dvar_s *dvar = Dvar_FindVar(name))
        Dvar_Reset(dvar, DVAR_SOURCE_INTERNAL);
}

const SwMenuDvarOps *Ops()
{
    static const SwMenuDvarOps ops = {0, OpsGet, OpsGetLatched, OpsSet, OpsReset};
    return &ops;
}

void SetStatus(const char *text)
{
    if (sw_status)
        Dvar_SetString(const_cast<dvar_s *>(sw_status), const_cast<char *>(text));
}

void SetProxy(const dvar_t *dvar, int value)
{
    if (dvar)
        Dvar_SetInt(const_cast<dvar_s *>(dvar), value);
}

int LatchedDynRes()
{
    char value[32];
    return OpsGetLatched(0, "r_dynres", value, sizeof(value)) && atoi(value) != 0;
}

// Brings the page's own state back in line with the dvars.
void Refresh()
{
    SwMenu_FixDynResRange(Ops());
    SetProxy(sw_ui_dynres, LatchedDynRes());
    SetProxy(sw_ui_preset, SwMenu_MatchPreset(Ops()));
    SetStatus(SwMenu_RestartPending(Ops()) ? "Dynamic resolution changes the next time the game starts." : "");
}

void Cmd_Open()
{
    Refresh();
}

void Cmd_Sync()
{
    Refresh();
}

void Cmd_Dynres()
{
    OpsSet(0, "r_dynres", sw_ui_dynres && sw_ui_dynres->current.integer ? "1" : "0");
    Refresh();
}

void Cmd_Preset()
{
    const int preset = sw_ui_preset ? sw_ui_preset->current.integer : SW_PRESET_CUSTOM;
    SwMenu_ApplyPreset(Ops(), preset);
    Refresh();
}

void Cmd_Apply()
{
    if (!Com_HasPlayerProfile())
    {
        SetStatus("Select a profile first; settings are saved with it.");
        return;
    }
    char configFile[64];
    Com_BuildPlayerProfilePath(configFile, sizeof(configFile), "config.cfg");
    Com_WriteConfigToFile(0, configFile);
    Refresh();
    if (!SwMenu_RestartPending(Ops()))
        SetStatus("Saved.");
}

void Cmd_Reset()
{
    const char *group = Cmd_Argc() > 1 ? Cmd_Argv(1) : "";
    if (!strcmp(group, "graphics"))
        SwMenu_ResetGraphics(Ops());
    else if (!strcmp(group, "controller"))
        SwMenu_ResetDvars(Ops(), kSwControllerDvars, SW_ARRAY_COUNT(kSwControllerDvars));
    else if (!strcmp(group, "audio"))
        SwMenu_ResetDvars(Ops(), kSwSoundGameDvars, SW_ARRAY_COUNT(kSwSoundGameDvars));
    Refresh();
    if (!SwMenu_RestartPending(Ops()))
        SetStatus("Defaults restored.");
}

// Developer aid: lists a loaded menu's items with their rectangles and
// actions, to find the entries the retarget rules should catch.
void Cmd_Dump()
{
    if (!s_dc || Cmd_Argc() < 2)
        return;
    const menuDef_t *menu = Menus_FindByName(s_dc, Cmd_Argv(1));
    if (!menu)
    {
        Com_Printf(CON_CHANNEL_UI, "sw_menudump: no menu %s\n", Cmd_Argv(1));
        return;
    }
    for (int i = 0; i < menu->itemCount; ++i)
    {
        const itemDef_s *item = menu->items[i];
        Com_Printf(CON_CHANNEL_UI, "sw_menudump %s[%d] type=%d y=%.0f focus=%d name='%s' text='%s' action='%s'\n", Cmd_Argv(1), i,
                   item->type, item->window.rectClient.y, Window_HasFocus(0, &item->window) ? 1 : 0,
                   item->window.name ? item->window.name : "", item->text ? item->text : "", item->action ? item->action : "");
        if (item->type == 12 && item->dataType == 12 && item->typeData.multi)
        {
            const multiDef_s *multi = item->typeData.multi;
            Com_Printf(CON_CHANNEL_UI, "  multi dvar='%s' now='%s' count=%d strDef=%d first='%s'/'%s'\n", item->dvar,
                       Dvar_GetVariantString(item->dvar), multi->count, multi->strDef, multi->dvarList[0],
                       multi->dvarStr[0] ? multi->dvarStr[0] : "(null)");
        }
    }
}

// Developer aid: sends one key press to the UI so a run can drive the menus
// through the real key path (sw_menukey enter|esc|up|down|left|right).
void Cmd_Key()
{
    static const struct
    {
        const char *name;
        int key;
    } keys[] = {{"enter", 0x0D}, {"esc", 0x1B}, {"up", 0x9A}, {"down", 0x9B}, {"left", 0x9C}, {"right", 0x9D}};
    if (Cmd_Argc() < 2)
        return;
    for (const auto &k : keys)
    {
        if (!strcmp(Cmd_Argv(1), k.name))
        {
            CL_KeyEvent(0, k.key, 1, Sys_Milliseconds());
            CL_KeyEvent(0, k.key, 0, Sys_Milliseconds());
            return;
        }
    }
}

void RegisterOnce()
{
    if (s_registered)
        return;
    s_registered = true;
    sw_ui_preset = Dvar_RegisterInt("sw_ui_preset", SW_PRESET_CUSTOM, 0, SW_PRESET_CUSTOM, DVAR_NOFLAG,
                                    "Graphics page: selected preset (3 = custom)");
    sw_ui_dynres = Dvar_RegisterInt("sw_ui_dynres", 0, 0, 1, DVAR_NOFLAG,
                                    "Graphics page: dynamic resolution as chosen (r_dynres applies at startup)");
    sw_status = Dvar_RegisterString("sw_status", "", DVAR_NOFLAG, "Options pages: status line");
    Cmd_AddCommandInternal("sw_open", Cmd_Open, &s_cmdOpen);
    Cmd_AddCommandInternal("sw_sync", Cmd_Sync, &s_cmdSync);
    Cmd_AddCommandInternal("sw_dynres", Cmd_Dynres, &s_cmdDynres);
    Cmd_AddCommandInternal("sw_preset", Cmd_Preset, &s_cmdPreset);
    Cmd_AddCommandInternal("sw_apply", Cmd_Apply, &s_cmdApply);
    Cmd_AddCommandInternal("sw_reset", Cmd_Reset, &s_cmdReset);
    Cmd_AddCommandInternal("sw_menudump", Cmd_Dump, &s_cmdDump);
    Cmd_AddCommandInternal("sw_menukey", Cmd_Key, &s_cmdKey);
}

// The stock engine registers some of these without DVAR_ARCHIVE or only on
// first use; the pages need them to exist and to persist.
void PrepareSettings()
{
    if (!Dvar_FindVar("input_lookCurve"))
        Dvar_RegisterFloat("input_lookCurve", 2.0f, 1.0f, 4.0f, DVAR_ARCHIVE,
                           "Look-stick response exponent: 1 = linear, higher = finer control near the centre");
    for (const char *name : kSwArchiveDvars)
        if (const dvar_s *dvar = Dvar_FindVar(name))
            Dvar_AddFlags(dvar, DVAR_ARCHIVE);
}

const char *BuiltinMenuText(const char *filename, int *length)
{
    for (int i = 0; i < SW_PAGE_COUNT; ++i)
    {
        std::string name = std::string(kSwPages[i].menuName) + ".menu";
        if (name != filename)
            continue;
        if (s_pageText.size() <= (size_t)i)
            s_pageText.resize(SW_PAGE_COUNT);
        if (s_pageText[i].empty())
            s_pageText[i] = swpages::BuildPage(i);
        *length = (int)s_pageText[i].size();
        return s_pageText[i].c_str();
    }
    return 0;
}

const char *Persist(const std::string &text)
{
    s_patchedText.push_back(text);
    return s_patchedText.back().c_str();
}

void InstallPages(UiContext *dc)
{
    UI_BuiltinMenuTextProvider = BuiltinMenuText;
    for (int i = 0; i < SW_PAGE_COUNT; ++i)
    {
        if (Menus_FindByName(dc, kSwPages[i].menuName))
            continue;
        const std::string file = std::string(kSwPages[i].menuName) + ".menu";
        MenuList *list = UI_LoadMenu_LoadObj(const_cast<char *>(file.c_str()), 3);
        if (!list)
            continue;
        // UI_AddMenu insists every fast-file menu is a database asset; these
        // are parsed from the built-in text instead.
        for (int m = 0; m < list->menuCount && dc->menuCount < 640; ++m)
            dc->Menus[dc->menuCount++] = list->menus[m];
    }
}

void PatchRetailMenus(UiContext *dc)
{
    for (int m = 0; m < dc->menuCount; ++m)
    {
        menuDef_t *menu = dc->Menus[m];
        if (!menu)
            continue;
        if (menu->window.name && !I_stricmp(menu->window.name, "main_text"))
            SwPatch_HideRowByAction<menuDef_t, itemDef_s>(menu, kSwHideEntryMenu);

        if (menu->onESC)
        {
            const std::string text = SwPatch_RetargetAction(menu->onESC);
            if (!text.empty())
                menu->onESC = Persist(text);
        }
        for (int i = 0; i < menu->itemCount; ++i)
        {
            itemDef_s *item = menu->items[i];
            std::string text = SwPatch_RetargetAction(item->action);
            if (!text.empty())
                item->action = Persist(text);
            text = SwPatch_RetargetAction(item->onAccept);
            if (!text.empty())
                item->onAccept = Persist(text);
        }
    }
}
} // namespace

void Switch_UI_OnMenusAdded(UiContext *dc)
{
    s_dc = dc;
    RegisterOnce();
    PrepareSettings();
    InstallPages(dc);
    PatchRetailMenus(dc);
}

void Switch_UI_FillSaveInfo(SavegameInfo *info, const char *saveName)
{
    char path[64];
    SaveHeader header;
    Com_BuildPlayerProfilePath(path, sizeof(path), "save/%s.svg", saveName);
    if (!ReadSaveHeader(path, &header))
        return;

    // Level-start saves all share one description, so the map leads the name.
    if (header.description[0])
    {
        const char *description = UI_SafeTranslateString(header.description);
        info->savegameName = String_Alloc(header.mapName[0] ? va("%s - %s", header.mapName, description) : description);
    }
    if (header.mapName[0])
        info->mapName = String_Alloc(header.mapName);

    // The thumbnail written at save time (switch_save_thumb.h); the draw call
    // asks the renderer for it by the save's name.
    char thumbPath[96];
    if (SaveThumb_PathForSave(path, thumbPath, sizeof(thumbPath)) && FS_FileExists(thumbPath))
        info->imageName = String_Alloc(saveName);

    info->tm = header.time;
    if (header.time.tm_year > 0)
    {
        info->date = String_Alloc(va("%02d/%02d/%04d", header.time.tm_mon + 1, header.time.tm_mday, 1900 + header.time.tm_year));
        info->time = String_Alloc(va("%02d:%02d", header.time.tm_hour, header.time.tm_min));
    }
}

namespace
{
itemDef_s *s_pendingEdit;
}

bool Switch_UI_QueueTextEdit(itemDef_s *item)
{
#ifdef __SWITCH__
    if (!item || !item->dvar)
        return false;
    s_pendingEdit = item;
    return true;
#else
    (void)item;
    return false;
#endif
}

void Switch_UI_Frame()
{
    if (!s_pendingEdit)
        return;
    itemDef_s *item = s_pendingEdit;
    s_pendingEdit = 0;
    if (s_dc)
        Switch_UI_EditTextField(s_dc, item);
}

void Switch_UI_FormatSaveInfo(const SavegameInfo *info, char *out, int outSize)
{
    out[0] = 0;
    if (info->savegameName)
        I_strncat(out, outSize, info->savegameName);
    if (info->mapName)
    {
        I_strncat(out, outSize, "\n");
        I_strncat(out, outSize, info->mapName);
    }
    if (info->date && info->time)
        I_strncat(out, outSize, va("\n%s %s", info->date, info->time));
}

bool Switch_UI_EditTextField(UiContext *dc, itemDef_s *item)
{
#ifdef __SWITCH__
    if (!item->dvar)
        return false;
    const editFieldDef_s *edit = Item_GetEditFieldDef(item);
    const int maxChars = edit && edit->maxChars > 0 && edit->maxChars < 32 ? edit->maxChars : 31;
    const bool profileName = !I_stricmp(item->dvar, "ui_playerProfileNameNew");

    char typed[128] = "";
    bool shown = false;
    bool unavailable = false;
    SwkbdConfig kbd;
    if (R_SUCCEEDED(swkbdCreate(&kbd, 0)))
    {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetHeaderText(&kbd, profileName ? "Profile name" : "Enter text");
        swkbdConfigSetInitialText(&kbd, Dvar_GetVariantString(item->dvar));
        swkbdConfigSetStringLenMax(&kbd, maxChars);
        swkbdConfigSetStringLenMin(&kbd, 1);
        const u64 start = armGetSystemTick();
        const Result rc = swkbdShow(&kbd, typed, sizeof(typed));
        shown = R_SUCCEEDED(rc);
        const u64 elapsedMs = armTicksToNs(armGetSystemTick() - start) / 1000000ull;
        // A result in a few milliseconds is a refusal to open, not the player
        // cancelling.
        unavailable = !shown && elapsedMs < 300;
        Com_Printf(CON_CHANNEL_UI, "Software keyboard: result 0x%x after %llu ms\n", (unsigned)rc, (unsigned long long)elapsedMs);
        swkbdClose(&kbd);
    }
    else
    {
        unavailable = true;
        Com_Printf(CON_CHANNEL_UI, "Software keyboard: swkbdCreate failed\n");
    }

    char value[64] = "";
    if (shown)
    {
        if (profileName)
            SwitchTextInput_SanitizeFilename(typed, value, sizeof(value), maxChars);
        else
            snprintf(value, sizeof(value), "%.*s", maxChars, typed);
    }
    else if (unavailable && profileName)
    {
        int count = 0;
        const char **existing = FS_ListFiles("profiles", "/", FS_LIST_ALL, &count);
        SwitchTextInput_UniqueName("Player", existing, count, value, sizeof(value), maxChars);
        FS_FreeFileList(existing);
        Com_Printf(CON_CHANNEL_UI, "Software keyboard unavailable; using profile name %s\n", value);
    }
    if (!value[0])
        return true;

    Dvar_SetFromStringByName(item->dvar, value);
    if (item->onAccept)
        Item_RunScript(dc, item, const_cast<char *>(item->onAccept));
    return true;
#else
    (void)dc;
    (void)item;
    return false;
#endif
}
