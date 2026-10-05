// A text field with maxChars and maxCharsGotoNext that fills up asks
// Menu_SetNextCursorItem for the next item. With no other focusable item in
// the menu that returns null, and Item_TextField_HandleKey must not touch it.
// Runs the real Item_TextField_HandleKey (ui_shared.cpp), cursor helpers
// (ui_utils.cpp) and Menu_SetNextCursorItem (ui_menu_focus.cpp) under ASan.
#include <ui/ui_shared.h>
#include <ui/ui.h>
#include <client/client.h>
#include <universal/q_parse.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <strings.h>

uiInfo_s uiInfo{};
static UiContext &dc = uiInfo.uiDC;
ScreenPlacement scrPlaceView[1]{};
static char s_dvarValue[1024];
static int s_dvarSets, s_scripts, s_checks, s_failures;

void MyAssertHandler(const char *, int, int, const char *fmt, ...)
{ std::fprintf(stderr, "FAIL:TEXTFIELD_NEXT assertion=%s\n", fmt); std::abort(); }
void Com_Error(errorParm_t, const char *fmt, ...)
{ std::fprintf(stderr, "FAIL:TEXTFIELD_NEXT error=%s\n", fmt); std::abort(); }
void Com_Printf(int, const char *, ...) {}
void Com_PrintWarning(int, const char *, ...) {}
void Com_PrintError(int, const char *, ...) {}
const char *Dvar_GetVariantString(const char *) { return s_dvarValue; }
void Dvar_SetFromStringByName(const char *, const char *value)
{
    ++s_dvarSets;
    std::snprintf(s_dvarValue, sizeof(s_dvarValue), "%s", value);
}
int Key_GetOverstrikeMode(int) { return 0; }
bool IsExpressionTrue(int, const statement_s *) { return true; }
void Item_RunScript(UiContext *, itemDef_s *, char *) { ++s_scripts; }
bool Dvar_GetBool(const char *) { return false; }
void Key_SetOverstrikeMode(int, int) {}
char Com_GetDecimalDelimiter() { return '.'; }
parseInfo_t *Com_ParseOnLine(const char **) { std::abort(); }
const char *SEH_StringEd_GetString(const char *s) { return s; }
bool I_isdigit(int c) { return c >= '0' && c <= '9'; }
bool I_isforfilename(int c) { return c > ' '; }
int I_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
void I_strncpyz(char *dst, const char *src, int len) { std::snprintf(dst, len, "%s", src); }
double ScrPlace_ApplyX(const ScreenPlacement *, float x, int32_t) { return x; }
double ScrPlace_ApplyY(const ScreenPlacement *, float y, int32_t) { return y; }
void ScrPlace_ApplyRect(const ScreenPlacement *, float *, float *, float *, float *, int, int) {}
static dvar_s s_debugStorage{};
const dvar_t *uiscript_debug = &s_debugStorage;

static void Check(bool ok, const char *name)
{
    ++s_checks;
    std::printf("%s %s\n", ok ? "ok" : "FAIL:TEXTFIELD_NEXT", name);
    s_failures += !ok;
}

int main()
{
    // One menu holding only the edit field: there is no next cursor item.
    static menuDef_t menu{};
    static itemDef_s field{};
    static itemDef_s *slots[] = {&field};
    static editFieldDef_s edit{};
    menu.window.name = "solo";
    menu.items = slots;
    menu.itemCount = 1;
    menu.cursorItem[0] = 0;
    field.parent = &menu;
    field.type = 4; // ITEM_TYPE_EDITFIELD
    field.dvar = "ui_textfield";
    field.window.name = "field";
    field.typeData.editField = &edit;
    edit.maxChars = 1;
    edit.maxCharsGotoNext = 1;

    extern itemDef_s *g_editItem;
    g_editItem = &field;
    s_dvarValue[0] = 0;
    Check(Menu_SetNextCursorItem(&dc, &menu) == nullptr, "precondition: no next cursor item");
    menu.cursorItem[0] = 0;

    const bool handled = Item_TextField_HandleKey(&dc, &field, 'a' | K_CHAR_FLAG);
    Check(handled, "filling key handled");
    Check(s_dvarSets == 1 && !std::strcmp(s_dvarValue, "a"), "character stored");
    Check(field.cursorPos[0] == 1, "cursor advanced to maxChars");
    Check(g_editItem == &field, "edit item kept when there is no next item");
    Check(menu.cursorItem[0] == 0, "menu cursor restored");

    std::printf("textfield next: %s (%d checks, %d failures)\n", s_failures ? "FAIL" : "PASS", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
