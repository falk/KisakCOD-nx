// Execute the SP focus, stack and key owners with synthetic menu fixtures.
// Script/game/widget boundaries are observed callbacks, not retail emulation.
#include <ui/ui_shared.h>
#include <ui/ui.h>
#include <client/client.h>
#include <platform/switch/switch_input.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <strings.h>

uiInfo_s uiInfo{};
static UiContext &dc = uiInfo.uiDC;
static menuDef_t root{}, child{}, modal{};
static itemDef_s items[6]{};
static itemDef_s *slots[6]{};
static itemDef_s childItem{}, modalItem{};
static itemDef_s *childSlots[] = {&childItem};
static itemDef_s *modalSlots[] = {&modalItem};
static bool enabled = true, visible = true;
static int actions, backs, focuses, leaves, widgetKeys, checks;
static const char *lastScript;
static bool consumeWidget;
static bool keyboardHandled;
static int keyboardCalls;
bool Switch_UI_EditTextField(UiContext *ctx, itemDef_s *item)
{
    if (ctx != &dc || item != &items[0]) std::abort();
    ++keyboardCalls;
    return keyboardHandled;
}
static bool mousePitch, padInvert, startingInvert;
static float mousePitchScale;
static int catcher, normalResponses, invertResponses;
extern uiMenuCommand_t g_currentMenuType;

ScreenPlacement scrPlaceView[1]{};
static dvar_s debugStorage{};

void MyAssertHandler(const char *, int, int, const char *fmt, ...)
{ std::fprintf(stderr, "FAIL:MENU_WALKTHROUGH assertion=%s\n", fmt); std::abort(); }
void Com_Error(errorParm_t, const char *fmt, ...)
{ std::fprintf(stderr, "FAIL:MENU_WALKTHROUGH error=%s\n", fmt); std::abort(); }
void Com_Printf(int, const char *, ...) {}
void Com_PrintWarning(int, const char *, ...) {}
void Com_PrintError(int, const char *, ...) {}
int I_stricmp(const char *a, const char *b) { return strcasecmp(a,b); }
int Dvar_GetInt(const char *) { return 0; }
bool Dvar_GetBool(const char *name)
{ if (!strcasecmp(name, "ui_mousePitch")) return mousePitch; std::abort(); }
void Dvar_SetFloatByName(const char *name, float value)
{ if (!strcasecmp(name, "m_pitch")) mousePitchScale = value; else std::abort(); }
void Dvar_SetBoolByName(const char *name, bool value)
{ if (!strcasecmp(name, "input_invertPitch")) padInvert = value; else std::abort(); }
const char *Dvar_GetString(const char *) { std::abort(); }
void Dvar_SetStringByName(const char *, char *) { std::abort(); }
const char *Dvar_GetVariantString(const char *) { return enabled ? "1" : "0"; }
bool IsExpressionTrue(int, const statement_s *) { return visible; }
int String_Parse(const char **p, char *out, int len)
{
    while (**p == ' ' || **p == '"') ++*p;
    int n = 0;
    while (**p && **p != ' ' && **p != '"')
    { if (n + 1 < len) out[n++] = **p; ++*p; }
    out[n] = 0;
    return n != 0;
}
double ScrPlace_ApplyX(const ScreenPlacement *, float x, int32_t) { return x; }
double ScrPlace_ApplyY(const ScreenPlacement *, float y, int32_t) { return y; }
void ScrPlace_ApplyRect(const ScreenPlacement *, float *, float *, float *, float *, int, int) {}
int UI_PlayLocalSoundAliasByName(uint32_t, const char *) { return 0; }
const char *Key_GetBinding(int32_t, uint32_t) { return nullptr; }
int CL_ControllerIndexFromClientNum(int n) { return n; }
void Cbuf_ExecuteBuffer(int, int, const char *) { std::abort(); }
void Cbuf_AddText(int, const char *) { std::abort(); }
void Key_SetOverstrikeMode(int, int) {}
void Key_SetCatcher(int, int value) { catcher = value; }
int Display_MouseMove(UiContext *) { return 0; }
BOOL Menu_HandleMouseMove(UiContext *, menuDef_t *) { return 0; }
void Menus_HandleOOBClick(UiContext *, menuDef_t *, int, int) { std::abort(); }
int Item_Bind_HandleKey(UiContext *, itemDef_s *, int, int) { return 0; }
void Item_TextField_BeginEdit(int, itemDef_s *) {}
bool Item_TextField_HandleKey(UiContext *, itemDef_s *, int key) { return key != 27; }
int Item_HandleKey(UiContext *, itemDef_s *, int, int down)
{ if (consumeWidget && down) { ++widgetKeys; return 1; } return 0; }
void Item_RunScript(UiContext *ctx, itemDef_s *item, char *script)
{
    if (!script) return;
    lastScript = script;
    if (!std::strcmp(script, "activate")) ++actions;
    else if (!std::strcmp(script, "focus")) ++focuses;
    else if (!std::strcmp(script, "leave")) ++leaves;
    else if (!std::strcmp(script, "open-child")) Menus_Open(ctx, &child);
    else if (!std::strcmp(script, "authored-back")) { ++backs; Menus_Close(ctx, item->parent); }
    else if (!std::strcmp(script, "open-modal")) Menus_Open(ctx, &modal);
    else if (!std::strcmp(script, "explicit-focus")) Item_SetFocus(ctx, item->parent->items[1], 0, 0);
    else if (!std::strcmp(script, "focus-first")) Menu_FocusFirstSelectableItem(ctx, item->parent);
    else if (!std::strcmp(script, "normal-response")) ++normalResponses;
    else if (!std::strcmp(script, "accept-normal"))
    { mousePitch = startingInvert; UI_Update("ui_mousePitch"); ++normalResponses; Menus_Close(ctx, item->parent); }
    else if (!std::strcmp(script, "try-invert"))
    { mousePitch = !startingInvert; UI_Update("ui_mousePitch"); ++invertResponses; Menus_Close(ctx, item->parent); }
    else if (!std::strcmp(script, "cancel-normal"))
    { Menus_Close(ctx, item->parent); ++normalResponses; }
    else { std::fprintf(stderr, "FAIL:MENU_WALKTHROUGH unknown callback=%s\n", script); std::abort(); }
}

static void Check(bool ok, const char *name)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL:MENU_WALKTHROUGH stage=%s\n", name); std::exit(1); }
}
static void InitItem(itemDef_s &item, menuDef_t &menu, const char *name)
{
    item = {};
    item.parent = &menu;
    item.type = 1;
    item.text = "Entry";
    item.window.name = name;
    item.window.dynamicFlags[0] = 4;
    item.action = "activate";
    item.onFocus = "focus";
    item.leaveFocus = "leave";
}
static void Reset()
{
    dc = {}; root = {}; child = {}; modal = {};
    uiscript_debug = &debugStorage;
    g_currentMenuType = UIMENU_NONE;
    catcher = normalResponses = invertResponses = 0;
    mousePitch = padInvert = startingInvert = false; mousePitchScale = 0.022f;
    dc.cursor.x = dc.cursor.y = -100;
    root.window.name = "root"; root.items = slots; root.itemCount = 6; root.fullScreen = 1;
    root.cursorItem[0] = -1;
    for (int i = 0; i < 6; ++i) { InitItem(items[i], root, "item"); slots[i] = &items[i]; }
    child.window.name = "child"; child.items = childSlots; child.itemCount = 1;
    child.fullScreen = 1; child.cursorItem[0] = -1; InitItem(childItem, child, "child-item");
    modal.window.name = "modal"; modal.items = modalSlots; modal.itemCount = 1;
    modal.fullScreen = 1; modal.cursorItem[0] = -1; InitItem(modalItem, modal, "modal-item");
    enabled = visible = true;
    actions = backs = focuses = leaves = widgetKeys = 0; lastScript = nullptr; consumeWidget = false;
    keyboardHandled = false; keyboardCalls = 0;
    extern int g_editingField, g_waitingForKey;
    extern itemDef_s *g_editItem;
    g_editingField = g_waitingForKey = 0; g_editItem = nullptr;
}
static void Sink(void *, uint32_t, int key, int down)
{
    if (auto *menu = Menu_GetFocused(&dc)) Menu_HandleKey(&dc, menu, key, down);
}
static void Press(uint64_t button)
{
    SwitchInputState pad{};
    Switch_InputInit(&pad);
    Switch_InputTranslate(&pad, button, 100, Sink, nullptr);
    Switch_InputTranslate(&pad, 0, 110, Sink, nullptr);
}

int main(int argc, char **argv)
{
    if (argc == 2)
    {
        Reset();
        if (!std::strcmp(argv[1], "backdrop"))
        {
            root.itemCount = 0; Menus_Open(&dc, &root); Menus_Open(&dc, &child);
            Press(SWITCH_INPUT_BUTTON_B);
            Check(Menu_GetFocused(&dc) == &child, "B-must-not-leave-only-backdrop");
        }
        else if (!std::strcmp(argv[1], "disabled"))
        {
            Menus_Open(&dc, &root); Item_SetFocus(&dc, &items[0], 0, 0);
            items[0].dvarFlags = 1; items[0].dvarTest = "enabled"; items[0].enableDvar = "1";
            enabled = false; Press(SWITCH_INPUT_BUTTON_A);
            Check(actions == 0, "A-must-not-activate-disabled-focus");
        }
        else if (!std::strcmp(argv[1], "unselected"))
        {
            root.window.dynamicFlags[0] = 6;
            Menu_SetNextCursorItem(&dc, &root);
            Check(root.cursorItem[0] == 0, "first-down-must-select-item-zero");
        }
        else if (!std::strcmp(argv[1], "all-hidden"))
        {
            root.window.dynamicFlags[0] = 6;
            for (auto &item : items) item.window.dynamicFlags[0] = 0;
            Check(Menu_SetNextCursorItem(&dc, &root) == nullptr, "all-hidden-from-unselected-is-safe");
        }
        else std::exit(2);
        std::printf("PASS:MENU_WALKTHROUGH case=%s\n", argv[1]);
        return 0;
    }
    for (int handled = 0; handled < 2; ++handled)
    {
        Reset();
        items[0].type = 4;
        Menus_Open(&dc, &root);
        Item_SetFocus(&dc, &items[0], 0, 0);
        keyboardHandled = handled;
        Press(SWITCH_INPUT_BUTTON_A);
        extern int g_editingField;
        extern itemDef_s *g_editItem;
        Check(keyboardCalls == 1, "text-field-invokes-keyboard-once");
        Check(bool(g_editingField) == !handled, "keyboard-result-controls-inline-edit");
        Check(handled || g_editItem == &items[0], "keyboard-fallback-retains-edit-item");
    }
    struct RemovedOption { const char *menu; const char *dvar; };
    const RemovedOption removed[] = {
        {"options_graphics", "ui_r_aasamples"}, {"options_graphics", "r_gamma"},
        {"options_graphics", "ui_r_vsync"}, {"options_graphics", "ai_corpsecount"},
        {"options_sound", "ui_snd_enableEq"}, {"options_sound", "ui_snd_khz"},
        {"options_sound", "ui_outputConfig"}, {"options_look", "sensitivity"}
    };
    for (const auto &option : removed)
    {
        Reset();
        itemDef_s rowItems[9]{};
        itemDef_s *rowSlots[9]{};
        for (int i = 0; i < 9; ++i)
        {
            rowItems[i].parent = &root;
            rowItems[i].type = 1;
            rowItems[i].text = "option";
            rowItems[i].window.dynamicFlags[0] = 4;
            rowItems[i].window.rectClient = {-74, 98, 175, 20, 2, 1};
            rowItems[i].window.rect = rowItems[i].window.rectClient;
            rowSlots[i] = &rowItems[i];
        }
        for (int i = 0; i < 5; ++i) rowItems[i].window.staticFlags = 0x100000;
        rowItems[5].dvar = option.dvar;
        rowItems[5].window.rectClient.w = 300;
        rowItems[6].dvar = "snd_volume";
        rowItems[6].action = "count-action";
        rowItems[6].window.rectClient.y = rowItems[6].window.rect.y = 120;
        rowItems[7].window.name = "back";
        rowItems[7].window.rectClient = {-120, 464, 160, 32, 0, 0};
        rowItems[8].window.staticFlags = 0x100000;
        rowItems[8].window.rectClient.x = -300; // Side-tab decoration shares Y, not the row.
        root.window.name = option.menu;
        root.items = rowSlots; root.itemCount = 9; root.cursorItem[0] = 6;
        UI_RemoveUnsupportedSwitchOptions(&root);
        Check(root.itemCount == 3 && root.items[0] == &rowItems[6], "unsupported-option-entire-row-removed");
        Check(root.cursorItem[0] == 0, "removal-remaps-retained-cursor");
        Check(rowItems[6].window.rectClient.y == 98 && rowItems[7].window.rectClient.y == 464,
              "removal-closes-row-gap-preserves-footer");
        Check(root.items[2] == &rowItems[8] && root.items[3] == nullptr,
              "removal-preserves-unrelated-decoration-clears-tail");
        UI_RemoveUnsupportedSwitchOptions(&root);
        Check(root.itemCount == 3 && rowItems[6].window.rectClient.y == 98, "removal-idempotent");
        Menus_Open(&dc, &root);
        Check(Window_HasFocus(0, &rowItems[6].window), "filtered-menu-focuses-live-control");
        Press(SWITCH_INPUT_BUTTON_DOWN);
        Check(Window_HasFocus(0, &rowItems[7].window), "filtered-menu-down-reaches-back");
        root.onESC = "authored-back";
        Press(SWITCH_INPUT_BUTTON_B);
        Check(backs == 1 && dc.openMenuCount == 0, "filtered-menu-authored-back-closes");
    }
    Reset(); root.window.name = "options_sound";
    items[0].window.name = "apply"; items[1].window.name = "apply2";
    root.cursorItem[0] = 0;
    UI_RemoveUnsupportedSwitchOptions(&root);
    Check(root.itemCount == 4 && root.cursorItem[0] == -1, "sound-removes-obsolete-apply-and-selection");
    Reset(); root.window.name = "other-menu"; items[0].dvar = "r_gamma";
    UI_RemoveUnsupportedSwitchOptions(&root);
    Check(root.itemCount == 6 && root.items[0] == &items[0], "other-menus-preserved");

    Reset();
    items[0].window.staticFlags = 0x100000; // background decoration
    items[2].window.dynamicFlags[0] = 0; // hidden duplicate
    items[3].dvarFlags = 1; items[3].dvarTest = "enabled"; items[3].enableDvar = "1";
    enabled = false;
    Menus_Open(&dc, &root);
    Check(Window_HasFocus(0, &items[1].window), "fresh-open-selects-first-eligible");
    Press(SWITCH_INPUT_BUTTON_A); Check(actions == 1, "A-activates-current-item-once");
    Press(SWITCH_INPUT_BUTTON_DOWN); Check(Window_HasFocus(0, &items[4].window), "down-skips-hidden-disabled");
    Press(SWITCH_INPUT_BUTTON_DOWN); Check(Window_HasFocus(0, &items[5].window), "down-next");
    Press(SWITCH_INPUT_BUTTON_DOWN); Check(Window_HasFocus(0, &items[1].window), "down-wrap");
    Press(SWITCH_INPUT_BUTTON_UP); Check(Window_HasFocus(0, &items[5].window), "up-wrap");
    Press(SWITCH_INPUT_BUTTON_UP); Check(Window_HasFocus(0, &items[4].window), "up-next");
    Press(SWITCH_INPUT_BUTTON_UP); Check(Window_HasFocus(0, &items[1].window), "up-skips-hidden-disabled");
    Check(focuses == 7 && leaves == 6, "focus-callbacks-once-per-transition");
    items[1].action = "open-child";
    Press(SWITCH_INPUT_BUTTON_A);
    Check(Menu_GetFocused(&dc) == &child && dc.openMenuCount == 2, "A-opens-child");
    Check(Window_HasFocus(0, &childItem.window), "child-initial-focus");
    Press(SWITCH_INPUT_BUTTON_B);
    Check(Menu_GetFocused(&dc) == &root && dc.openMenuCount == 1, "B-fallback-restores-parent");
    Check(Window_HasFocus(0, &items[1].window), "parent-item-retained");
    Menus_Open(&dc, &child); child.onESC = "authored-back";
    Press(SWITCH_INPUT_BUTTON_B); Check(backs == 1 && Menu_GetFocused(&dc) == &root, "B-authored-handler");
    Press(SWITCH_INPUT_BUTTON_B); Check(dc.openMenuCount == 1, "root-B-does-not-strand-UI");
    Menus_Open(&dc, &child); Menus_Open(&dc, &modal);
    Press(SWITCH_INPUT_BUTTON_PLUS); Check(Menu_GetFocused(&dc) == &child, "plus-cancels-top-modal");
    Press(SWITCH_INPUT_BUTTON_B); Check(Menu_GetFocused(&dc) == &root, "nested-back-to-root");
    Menus_Open(&dc, &child); Menus_Open(&dc, &child);
    Check(dc.openMenuCount == 2, "duplicate-open-does-not-grow-stack");

    Reset(); root.onOpen = "open-child"; Menus_Open(&dc, &root);
    Check(Menu_GetFocused(&dc) == &child, "onOpen-child-keeps-menu-focus");
    Check(!(root.window.dynamicFlags[0] & 2), "parent-fallback-cannot-steal-child-focus");
    child.onClose = "open-modal"; Press(SWITCH_INPUT_BUTTON_B);
    Check(Menu_GetFocused(&dc) == &modal && dc.openMenuCount == 2, "onClose-open-keeps-new-modal-focus");

    Reset(); Menus_Open(&dc, &root); Menus_Open(&dc, &child);
    items[0].dvarFlags = 1; items[0].dvarTest = "enabled"; items[0].enableDvar = "1";
    enabled = false; Press(SWITCH_INPUT_BUTTON_B);
    Check(Window_HasFocus(0, &items[1].window), "Back-repairs-parent-disabled-selection");

    Reset(); root.window.dynamicFlags[0] = 6;
    Check(Menu_SetNextCursorItem(&dc, &root) == &items[0], "unselected-down-starts-first");
    Menu_ClearFocus(&dc, &root); root.cursorItem[0] = -1;
    Check(Menu_SetPrevCursorItem(&dc, &root) == &items[5], "unselected-up-starts-last");

    Reset(); Menus_Open(&dc, &root); items[1].type = 0;
    Press(SWITCH_INPUT_BUTTON_DOWN);
    Check(root.cursorItem[0] == 2 && Window_HasFocus(0, &items[2].window), "rejected-text-cannot-desynchronize-cursor-and-focus");

    Reset(); root.itemCount = 0; root.window.name = "main";
    child.window.name = "main_text"; Menus_Open(&dc, &root); Menus_Open(&dc, &child);
    Press(SWITCH_INPUT_BUTTON_B);
    Check(Menu_GetFocused(&dc) == &child && dc.openMenuCount == 2, "root-B-keeps-controls-above-backdrop");

    Reset(); root.onOpen = "explicit-focus"; Menus_Open(&dc, &root);
    Check(Window_HasFocus(0, &items[1].window), "authored-initial-focus-preserved");
    items[1].dvarFlags = 1; items[1].dvarTest = "enabled"; items[1].enableDvar = "1";
    enabled = false;
    Check(!Item_SetFocus(&dc, &items[1], 0, 0), "stale-disabled-focus-rejected");
    Press(SWITCH_INPUT_BUTTON_A); Check(actions == 0, "disabled-focused-item-cannot-activate");
    Press(SWITCH_INPUT_BUTTON_DOWN); Check(Window_HasFocus(0, &items[2].window), "recover-from-disabled-focus");
    items[2].visibleExp.numEntries = 1; visible = false;
    Press(SWITCH_INPUT_BUTTON_A); Check(actions == 0, "hidden-focused-item-cannot-activate");

    Reset(); items[0].text = ""; items[1].text = ""; Menus_Open(&dc, &root);
    Check(Window_HasFocus(0, &items[2].window), "controller-skips-blank-popup-mouse-hitboxes");
    Press(SWITCH_INPUT_BUTTON_UP);
    Check(Window_HasFocus(0, &items[5].window), "controller-wrap-skips-mouse-hitboxes");
    dc.isCursorVisible = 1;
    Check(Item_SetFocus(&dc, &items[0], 0, 0), "mouse-hitboxes-remain-mouse-focusable");
    dc.isCursorVisible = 0; items[0].materialExp.numEntries = 1;
    Check(Item_SetFocus(&dc, &items[0], 0, 0), "image-only-button-remains-controller-focusable");
    items[1].textExp.numEntries = 1;
    Check(Item_SetFocus(&dc, &items[1], 0, 0), "expression-labeled-button-remains-focusable");

    Reset(); Menus_Open(&dc, &root);
    items[0].window.name = items[1].window.name = "newgame_regular";
    itemDef_s scriptItem{}; scriptItem.parent = &root;
    const char *focusArgs = "newgame_regular";
    Script_SetFocus(&dc, &scriptItem, &focusArgs);
    Check(Window_HasFocus(0, &items[0].window), "duplicate-name-focus-uses-first-match");
    items[0].dvarFlags = items[1].dvarFlags = 16;
    items[0].dvarTest = items[1].dvarTest = "g_gameskill";
    items[0].enableDvar = "2"; items[1].enableDvar = "1";
    focusArgs = "g_gameskill";
    Script_SetFocusByDvar(&dc, &scriptItem, &focusArgs);
    Check(Window_HasFocus(0, &items[1].window), "skill-dvar-disambiguates-duplicate-names");

    Reset(); Menus_Open(&dc, &root);
    ItemKeyHandler handler{}; handler.key = 13; handler.action = "open-child"; root.onKey = &handler;
    Press(SWITCH_INPUT_BUTTON_A); Check(Menu_GetFocused(&dc) == &child && actions == 0, "authored-onKey-precedes-action");
    Reset(); Menus_Open(&dc, &root); consumeWidget = true;
    Press(SWITCH_INPUT_BUTTON_DOWN);
    Check(widgetKeys == 1 && root.cursorItem[0] == 0, "widget-consumes-arrow-before-navigation");
    Reset(); items[0].type = 4; Menus_Open(&dc, &root);
    Press(SWITCH_INPUT_BUTTON_A);
    extern int g_editingField;
    Check(g_editingField && actions == 0, "A-enters-text-edit");
    Press(SWITCH_INPUT_BUTTON_B); Check(!g_editingField && dc.openMenuCount == 1, "first-B-cancels-edit-only");

    for (int inverted = 0; inverted <= 1; ++inverted)
    {
        Reset(); startingInvert = mousePitch = padInvert = inverted;
        root.window.name = "invert_axis_pc"; root.itemCount = 4;
        items[0].text = items[1].text = "";
        items[2].text = "No"; items[3].text = "Yes";
        items[2].action = "accept-normal"; items[3].action = "try-invert";
        root.onOpen = "focus-first"; root.onClose = "normal-response"; root.onESC = "cancel-normal";
        dc.Menus[0] = &root; dc.menuCount = 1; dc.isCursorVisible = 1;
        Check(UI_PopupScriptMenu("invert_axis_pc", true) == 1, "killhouse-script-popup-opens");
        Check(!dc.isCursorVisible && catcher == 16, "killhouse-popup-takes-controller-mode-and-catcher");
        Check(Window_HasFocus(0, &items[2].window), "killhouse-popup-initial-focus-is-No");
        Press(SWITCH_INPUT_BUTTON_DOWN); Check(Window_HasFocus(0, &items[3].window), "killhouse-down-focuses-Yes");
        Press(SWITCH_INPUT_BUTTON_UP); Check(Window_HasFocus(0, &items[2].window), "killhouse-up-focuses-No");
        Press(SWITCH_INPUT_BUTTON_A);
        Check(padInvert == bool(inverted) && dc.openMenuCount == 0 && normalResponses > 0, "killhouse-No-retains-stick-inversion-and-responds");
        Check(UI_PopupScriptMenu("invert_axis_pc", true) == 1, "killhouse-second-popup-opens");
        Press(SWITCH_INPUT_BUTTON_DOWN); Press(SWITCH_INPUT_BUTTON_A);
        Check(padInvert != bool(inverted) && invertResponses == 1 && dc.openMenuCount == 0, "killhouse-Yes-changes-stick-inversion-and-responds");
        Check(mousePitchScale == (mousePitch ? -0.022f : 0.022f), "killhouse-mouse-and-stick-setting-agree");
        SwitchInputState pad{}; SwitchGameplayInput gameplay{};
        Switch_InputInit(&pad);
        Switch_InputFillGameplay(&pad, 0, 0, 0, 0.75f, 0, 0, 0);
        Switch_InputBuildGameplay(&pad, padInvert, &gameplay);
        Check((gameplay.pitch < 0) == padInvert, "killhouse-choice-changes-production-stick-pitch-sign");
        UI_PopupScriptMenu("invert_axis_pc", true); Press(SWITCH_INPUT_BUTTON_B);
        Check(dc.openMenuCount == 0 && normalResponses > 0, "killhouse-B-closes-and-sends-normal-response");
    }

    Reset(); for (auto &item : items) item.window.dynamicFlags[0] = 0;
    Menus_Open(&dc, &root);
    Check(Menu_FocusFirstSelectableItem(&dc, &root) == nullptr, "all-hidden-has-no-focus");
    Press(SWITCH_INPUT_BUTTON_DOWN); Press(SWITCH_INPUT_BUTTON_UP); Press(SWITCH_INPUT_BUTTON_A);
    Check(actions == 0, "all-hidden-buttons-safe");
    Reset(); root.itemCount = 0; Menus_Open(&dc, &root);
    Press(SWITCH_INPUT_BUTTON_DOWN); Press(SWITCH_INPUT_BUTTON_UP);
    Check(root.cursorItem[0] == -1, "empty-menu-cursor-stays-unselected");
    std::printf("PASS:MENU_WALKTHROUGH checks=%d production=pad,focus,key,stack synthetic=menus,scripts,widgets,dvars\n", checks);
}
