// Host test for the pure parts of the port-authored options menus: preset
// tables and apply/reset/match logic (switch_menu_settings.h), the generated
// page text (switch_menu_pages.h), retail-menu patch rules
// (switch_menu_patch.h), software-keyboard text helpers
// (switch_text_input.h) and recursive profile deletion (switch_dirtree.h).

#include "src/gfx_d3d/r_dynres_controller.h"
#include "src/port/switch_dirtree.h"
#include "src/port/switch_menu_pages.h"
#include "src/port/switch_menu_patch.h"
#include "src/port/switch_menu_settings.h"
#include "src/port/switch_text_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>

static int g_failures;

#define CHECK(cond)                                                         \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

namespace
{
struct FakeStore
{
    std::map<std::string, std::string> current;
    std::map<std::string, std::string> latched;
    std::map<std::string, std::string> defaults;
};

bool FakeGet(void *ctx, const char *name, char *out, size_t size)
{
    FakeStore *s = (FakeStore *)ctx;
    auto it = s->current.find(name);
    if (it == s->current.end())
        return false;
    snprintf(out, size, "%s", it->second.c_str());
    return true;
}

bool FakeGetLatched(void *ctx, const char *name, char *out, size_t size)
{
    FakeStore *s = (FakeStore *)ctx;
    auto l = s->latched.find(name);
    if (l != s->latched.end())
    {
        snprintf(out, size, "%s", l->second.c_str());
        return true;
    }
    return FakeGet(ctx, name, out, size);
}

void FakeSet(void *ctx, const char *name, const char *value)
{
    FakeStore *s = (FakeStore *)ctx;
    if (!s->current.count(name))
        return;
    // r_dynres is latched: the value waits for the restart.
    if (!strcmp(name, "r_dynres"))
        s->latched[name] = value;
    else
        s->current[name] = value;
}

void FakeReset(void *ctx, const char *name)
{
    FakeStore *s = (FakeStore *)ctx;
    FakeSet(ctx, name, s->defaults[name].c_str());
}

SwMenuDvarOps MakeOps(FakeStore *s)
{
    return SwMenuDvarOps{s, FakeGet, FakeGetLatched, FakeSet, FakeReset};
}

void Seed(FakeStore *s, const char *const *names, int count)
{
    for (int i = 0; i < count; ++i)
        s->current[names[i]] = s->defaults[names[i]] = "0";
}

void TestPresets()
{
    FakeStore store;
    Seed(&store, kSwGraphicsDvars, SW_ARRAY_COUNT(kSwGraphicsDvars));
    SwMenuDvarOps ops = MakeOps(&store);

    CHECK(SwMenu_MatchPreset(&ops) == SW_PRESET_CUSTOM);
    for (int preset = 0; preset < SW_PRESET_COUNT; ++preset)
    {
        CHECK(SwMenu_ApplyPreset(&ops, preset) == kSwPresets[preset].count);
        CHECK(SwMenu_MatchPreset(&ops) == preset);
    }

    // Changing one dvar of a preset makes it custom; restoring it matches again.
    SwMenu_ApplyPreset(&ops, SW_PRESET_BALANCED);
    store.current["r_halfResParticles"] = "1";
    CHECK(SwMenu_MatchPreset(&ops) == SW_PRESET_CUSTOM);
    store.current["r_halfResParticles"] = "3";
    CHECK(SwMenu_MatchPreset(&ops) == SW_PRESET_BALANCED);

    CHECK(SwMenu_ApplyPreset(&ops, -1) == 0);
    CHECK(SwMenu_ApplyPreset(&ops, SW_PRESET_CUSTOM) == 0);

    // Float spellings compare numerically.
    store.current["r_dynresMin"] = "0.670000";
    CHECK(SwMenu_MatchPreset(&ops) == SW_PRESET_BALANCED);
}

void TestPresetDvarsAreEditable()
{
    // A preset may only write dvars the Graphics page can also reset.
    std::set<std::string> graphics(kSwGraphicsDvars, kSwGraphicsDvars + SW_ARRAY_COUNT(kSwGraphicsDvars));
    for (int p = 0; p < SW_PRESET_COUNT; ++p)
        for (int i = 0; i < kSwPresets[p].count; ++i)
            CHECK(graphics.count(kSwPresets[p].pairs[i].name) == 1);

    // Every preset sets the same dvars, so Match is unambiguous.
    for (int p = 1; p < SW_PRESET_COUNT; ++p)
    {
        CHECK(kSwPresets[p].count == kSwPresets[0].count);
        for (int i = 0; i < kSwPresets[p].count; ++i)
            CHECK(!strcmp(kSwPresets[p].pairs[i].name, kSwPresets[0].pairs[i].name));
    }

    // Distinct presets.
    for (int a = 0; a < SW_PRESET_COUNT; ++a)
        for (int b = a + 1; b < SW_PRESET_COUNT; ++b)
        {
            bool same = true;
            for (int i = 0; i < kSwPresets[a].count; ++i)
                same = same && !strcmp(kSwPresets[a].pairs[i].value, kSwPresets[b].pairs[i].value);
            CHECK(!same);
        }
}

void TestPresetAtomicAndReset()
{
    // A preset overwrites every graphics dvar, whatever came before.
    std::set<std::string> graphics(kSwGraphicsDvars, kSwGraphicsDvars + SW_ARRAY_COUNT(kSwGraphicsDvars));
    for (int p = 0; p < SW_PRESET_COUNT; ++p)
    {
        std::set<std::string> keys;
        for (int i = 0; i < kSwPresets[p].count; ++i)
            keys.insert(kSwPresets[p].pairs[i].name);
        CHECK(keys == graphics);
    }
    FakeStore store;
    Seed(&store, kSwGraphicsDvars, SW_ARRAY_COUNT(kSwGraphicsDvars));
    SwMenuDvarOps ops = MakeOps(&store);
    for (int a = 0; a < SW_PRESET_COUNT; ++a)
        for (int b = 0; b < SW_PRESET_COUNT; ++b)
        {
            for (const char *n : kSwGraphicsDvars)
                store.current[n] = "junk";
            SwMenu_ApplyPreset(&ops, a);
            SwMenu_ApplyPreset(&ops, b);
            CHECK(SwMenu_MatchPreset(&ops) == b);
        }

    // Any single row change off a preset reads as Custom.
    for (int p = 0; p < SW_PRESET_COUNT; ++p)
        for (int i = 0; i < kSwPresets[p].count; ++i)
        {
            SwMenu_ApplyPreset(&ops, p);
            store.latched.erase(kSwPresets[p].pairs[i].name); // r_dynres reads its latched value
            store.current[kSwPresets[p].pairs[i].name] = "0.123";
            CHECK(SwMenu_MatchPreset(&ops) == SW_PRESET_CUSTOM);
        }

    // Reset lands on Balanced.
    for (const char *n : kSwGraphicsDvars)
        store.current[n] = "junk";
    SwMenu_ResetGraphics(&ops);
    CHECK(SwMenu_MatchPreset(&ops) == SW_PRESET_BALANCED);
}

// The GPU budget follows the frame cap for every cap the page offers (and any
// other value), so a 60 fps target can never carry a 30 fps budget.
void TestBudgetFollowsCap()
{
    CHECK(dynres::BudgetForFrameCap(0) == 15.5f);
    CHECK(dynres::BudgetForFrameCap(30) == 31.0f);
    for (int p = 0; p < SW_PRESET_COUNT; ++p)
        for (int i = 0; i < kSwPresets[p].count; ++i)
            CHECK(strcmp(kSwPresets[p].pairs[i].name, "r_dynresBudgetMs") != 0);
    CHECK(dynres::BudgetForFrameCap(60) == 15.5f);
    CHECK(dynres::BudgetForFrameCap(-1) == 15.5f);
}

void TestRangeAndRestart()
{
    FakeStore store;
    Seed(&store, kSwGraphicsDvars, SW_ARRAY_COUNT(kSwGraphicsDvars));
    SwMenuDvarOps ops = MakeOps(&store);

    store.current["r_dynresMin"] = "0.85";
    store.current["r_dynresMax"] = "0.75";
    CHECK(SwMenu_FixDynResRange(&ops));
    CHECK(store.current["r_dynresMax"] == "0.85");
    CHECK(!SwMenu_FixDynResRange(&ops));

    CHECK(!SwMenu_RestartPending(&ops));
    SwMenu_ApplyPreset(&ops, SW_PRESET_BALANCED); // r_dynres 0 -> 1 is latched
    CHECK(SwMenu_RestartPending(&ops));
    store.current["r_dynres"] = "1"; // the restart happened
    store.latched.clear();
    CHECK(!SwMenu_RestartPending(&ops));
}

void TestReset()
{
    FakeStore store;
    Seed(&store, kSwControllerDvars, SW_ARRAY_COUNT(kSwControllerDvars));
    store.defaults["input_viewSensitivity"] = "1";
    store.current["input_viewSensitivity"] = "2.5";
    store.current["gyro_invertYaw"] = "1";
    SwMenuDvarOps ops = MakeOps(&store);
    CHECK(SwMenu_ResetDvars(&ops, kSwControllerDvars, SW_ARRAY_COUNT(kSwControllerDvars)) ==
          SW_ARRAY_COUNT(kSwControllerDvars));
    CHECK(store.current["input_viewSensitivity"] == "1");
    CHECK(store.current["gyro_invertYaw"] == "0");
}

// ---- generated pages -------------------------------------------------------

int Count(const std::string &text, const char *token)
{
    int n = 0;
    for (size_t at = 0; (at = text.find(token, at)) != std::string::npos; at += strlen(token))
        ++n;
    return n;
}

void TestPages()
{
    std::set<std::string> known;
    for (const char *name : kSwGraphicsDvars)
        known.insert(name);
    for (const char *name : kSwControllerDvars)
        known.insert(name);
    for (const char *name : kSwSoundGameDvars)
        known.insert(name);
    known.insert("sw_ui_preset");
    known.insert("sw_ui_dynres");

    std::set<std::string> archived(kSwArchiveDvars, kSwArchiveDvars + SW_ARRAY_COUNT(kSwArchiveDvars));
    // Dvars the stock engine registers without DVAR_ARCHIVE must be promoted.
    CHECK(archived.count("input_viewSensitivity") && archived.count("r_taau") && archived.count("r_halfResParticles") &&
          archived.count("r_shadowFilter") && archived.count("cl_yawspeed") && archived.count("cl_pitchspeed"));

    std::set<std::string> names;
    for (int i = 0; i < SW_PAGE_COUNT; ++i)
    {
        const std::string text = swpages::BuildPage(i);
        names.insert(kSwPages[i].menuName);
        CHECK(Count(text, "{") == Count(text, "}"));
        CHECK(Count(text, "(") == Count(text, ")"));
        CHECK(text.find(std::string("name \"") + kSwPages[i].menuName + "\"") != std::string::npos);
        // One tab per page.
        CHECK(Count(text, "_tab\"") == SW_PAGE_COUNT);
        CHECK(Count(text, "action {") >= kSwPages[i].rowCount);
        CHECK(text.find("onESC { close self; }") != std::string::npos);
        CHECK(text.find("name \"back\"") != std::string::npos);
        for (int r = 0; r < kSwPages[i].rowCount; ++r)
        {
            const SwRow &row = kSwPages[i].rows[r];
            if (row.kind == SW_ROW_BUTTON)
            {
                CHECK(row.action && strstr(row.action, "exec"));
                continue;
            }
            CHECK(known.count(row.dvar) == 1);
            CHECK(text.find(std::string("dvar \"") + row.dvar + "\"") != std::string::npos);
            if (row.kind != SW_ROW_YESNO)
                CHECK(row.choices && *row.choices);
            // The loader joins adjacent strings: string lists need commas.
            if (row.kind == SW_ROW_MULTI_STR)
                CHECK(!strstr(row.choices, "\" \""));
        }
        // Tabs switch pages by closing the current one first.
        for (int t = 0; t < SW_PAGE_COUNT; ++t)
            if (t != i)
                CHECK(text.find(std::string("close ") + kSwPages[i].menuName + "; open " + kSwPages[t].menuName) !=
                      std::string::npos);
        // The PC video and mouse controls are gone.
        CHECK(text.find("r_mode") == std::string::npos);
        CHECK(text.find("\"sensitivity\"") == std::string::npos);
    }
    CHECK((int)names.size() == SW_PAGE_COUNT);

    // The sharpening row only shows with the upscaler that uses it.
    const std::string graphics = swpages::BuildPage(0);
    CHECK(graphics.find("when(dvarstring(\"r_fsrMode\") == \"bilinear_rcas\");") != std::string::npos);
    CHECK(graphics.find("(restart)") != std::string::npos);
}

// ---- retail menu patch rules -----------------------------------------------

struct FakeRect
{
    float x, y, w, h;
    int horzAlign, vertAlign;
};
struct FakeWindow
{
    FakeRect rectClient;
};
struct FakeItem
{
    FakeWindow window;
    const char *action;
};
struct FakeMenu
{
    int itemCount;
    FakeItem **items;
};

void TestPatch()
{
    CHECK(SwPatch_RetargetAction("play mouse_click; open options_graphics;") == "play mouse_click; open sw_graphics;");
    CHECK(SwPatch_RetargetAction("open options_graphics_texture;") == "open sw_graphics;");
    CHECK(SwPatch_RetargetAction("play x; open options_look") == "play x; open sw_controller");
    CHECK(SwPatch_RetargetAction("close options_look; open main_options;") == "close options_look; open sw_graphics;");
    CHECK(SwPatch_RetargetAction("open options_graphicsX;").empty());
    CHECK(SwPatch_RetargetAction("reopen options_look;").empty());
    CHECK(SwPatch_RetargetAction("open resume_popmenu;").empty());
    CHECK(SwPatch_RetargetAction(0).empty());
    CHECK(SwPatch_RetargetAction("").empty());
    // The loader stores scripts as quoted tokens.
    CHECK(SwPatch_RetargetAction("\"play\" \"mouse_click\" ; \"open\" \"options_look\" ; ") ==
          "\"play\" \"mouse_click\" ; \"open\" \"sw_controller\" ; ");
    CHECK(SwPatch_RetargetAction("\"open\" \"options_graphics_texture\" ; \"open\" \"options_sound\" ;") ==
          "\"open\" \"sw_graphics\" ; \"open\" \"sw_sound\" ;");
    CHECK(SwPatch_RetargetAction("\"close\" \"options_look\" ; \"exec\" \"open options_look\" ;").empty());
    CHECK(SwPatch_OpensMenu("\"play\" \"mouse_click\" ; \"open\" \"multi_popmenu\" ;", "multi_popmenu"));
    CHECK(!SwPatch_OpensMenu("\"close\" \"multi_popmenu\" ;", "multi_popmenu"));
    CHECK(!SwPatch_OpensMenu("\"open\" \"multi_popmenu2\" ;", "multi_popmenu"));
    // Applying the rule to its own output changes nothing.
    CHECK(SwPatch_RetargetAction("open sw_graphics;").empty());

    // Rows of 5 items each at y = 308 (kept), 332 (Multiplayer), 356 (Quit).
    static FakeItem rows[16];
    FakeItem *list[16];
    int n = 0;
    for (int row = 0; row < 3; ++row)
        for (int k = 0; k < 5; ++k)
        {
            FakeItem &it = rows[n];
            it.window.rectClient = {0, 308.0f + 24.0f * row, 220, 22, 1, 1};
            it.action = (row == 1 && k == 4) ? "play mouse_click; open multi_popmenu;" : "";
            list[n++] = &it;
        }
    static FakeItem other; // another column at the same y is left alone
    other.window.rectClient = {232, 332, 300, 32, 2, 1};
    other.action = "";
    list[n++] = &other;
    FakeMenu menu{n, list};

    CHECK((SwPatch_HideRowByAction<FakeMenu, FakeItem>(&menu, kSwHideEntryMenu)) == 5);
    CHECK(menu.itemCount == 11);
    int atCredits = 0, atQuit = 0, other332 = 0;
    for (int i = 0; i < menu.itemCount; ++i)
    {
        const FakeRect &r = menu.items[i]->window.rectClient;
        if (r.horzAlign == 1 && r.y == 308.0f)
            ++atCredits;
        if (r.horzAlign == 1 && r.y == 332.0f)
            ++atQuit; // Quit moved up into the gap
        if (r.horzAlign == 2 && r.y == 332.0f)
            ++other332;
        CHECK(!(menu.items[i]->action && strstr(menu.items[i]->action, "multi_popmenu")));
    }
    CHECK(atCredits == 5 && atQuit == 5 && other332 == 1);
    // Idempotent.
    CHECK((SwPatch_HideRowByAction<FakeMenu, FakeItem>(&menu, kSwHideEntryMenu)) == 0);
    CHECK(menu.itemCount == 11);
}

// ---- text input ------------------------------------------------------------

void TestTextInput()
{
    char out[64];
    CHECK(SwitchTextInput_SanitizeFilename("Capt. Price!", out, sizeof(out), 12) == 10 && !strcmp(out, "Capt_Price"));
    CHECK(SwitchTextInput_SanitizeFilename("abcdefghijklmnop", out, sizeof(out), 12) == 12 &&
          !strcmp(out, "abcdefghijkl"));
    CHECK(SwitchTextInput_SanitizeFilename("../..\\x", out, sizeof(out), 12) == 1 && !strcmp(out, "x"));
    CHECK(SwitchTextInput_SanitizeFilename("", out, sizeof(out), 12) == 0 && !out[0]);
    CHECK(SwitchTextInput_SanitizeFilename("a-b_c", out, 3, 12) == 2 && !strcmp(out, "a-"));

    const char *taken[] = {"Player", "player2", "Falk"};
    SwitchTextInput_UniqueName("Player", taken, 3, out, sizeof(out), 12);
    CHECK(!strcmp(out, "Player3"));
    SwitchTextInput_UniqueName("Player", taken, 0, out, sizeof(out), 12);
    CHECK(!strcmp(out, "Player"));
    SwitchTextInput_UniqueName("Player", taken, 3, out, sizeof(out), 7);
    CHECK(!strcmp(out, "Player3"));
    SwitchTextInput_UniqueName("Player", taken, 3, out, sizeof(out), 6);
    CHECK(strlen(out) <= 6 && strcmp(out, "Player"));
}

// ---- directory tree removal ------------------------------------------------

bool Exists(const std::string &p)
{
    struct stat st;
    return lstat(p.c_str(), &st) == 0;
}

void WriteFile(const std::string &p)
{
    FILE *f = fopen(p.c_str(), "wb");
    if (f)
    {
        fputs("x", f);
        fclose(f);
    }
}

void TestDirTree()
{
    char tmpl[] = "/tmp/swmenu-dirtree-XXXXXX";
    char *root = mkdtemp(tmpl);
    CHECK(root != 0);
    if (!root)
        return;
    const std::string base = std::string(root) + "/profiles/falk";
    mkdir((std::string(root) + "/profiles").c_str(), 0777);
    mkdir(base.c_str(), 0777);
    mkdir((base + "/save").c_str(), 0777);
    mkdir((base + "/save/autosave").c_str(), 0777);
    WriteFile(base + "/config.cfg");
    WriteFile(base + "/save/a.svg");
    WriteFile(base + "/save/autosave/b.svg");
    mkdir((std::string(root) + "/profiles/keep").c_str(), 0777);
    WriteFile(std::string(root) + "/profiles/keep/config.cfg");
    CHECK(symlink(root, (base + "/link").c_str()) == 0); // must not be followed

    CHECK(Switch_RemoveDirTree((base + "/").c_str()) == 1);
    CHECK(!Exists(base));
    CHECK(Exists(std::string(root) + "/profiles/keep/config.cfg"));
    CHECK(Exists(root));
    CHECK(Switch_RemoveDirTree(base.c_str()) == 0); // already gone
    CHECK(Switch_RemoveDirTree("") == 0);
    CHECK(Switch_RemoveDirTree(0) == 0);
    CHECK(Switch_RemoveDirTree((std::string(root) + "/profiles/keep/config.cfg").c_str()) == 1);
    CHECK(Switch_RemoveDirTree((std::string(root) + "/profiles").c_str()) == 1);
    CHECK(Switch_RemoveDirTree(root) == 1);
}
} // namespace

int main()
{
    TestPresets();
    TestPresetDvarsAreEditable();
    TestPresetAtomicAndReset();
    TestBudgetFollowsCap();
    TestRangeAndRestart();
    TestReset();
    TestPages();
    TestPatch();
    TestTextInput();
    TestDirTree();
    if (g_failures)
    {
        fprintf(stderr, "FAIL:SWITCH_MENU_TEST %d failures\n", g_failures);
        return 1;
    }
    printf("PASS:SWITCH_MENU_TEST\n");
    return 0;
}
