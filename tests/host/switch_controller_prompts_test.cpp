#include <client/client.h>
#include <bgame/bg_local.h>
#include <database/database.h>
#include <platform/switch/switch_input.h>
#include <port/switch_controller_prompts.h>
#include <port/switch_pad_layout.h>
#include <qcommon/msg.h>
#include <stringed/stringed_hooks.h>
#include <stringed/stringed_ingame.h>
#include <ui/ui_shared.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static dvar_t s_fastfile{}, s_language{}, s_translate{}, s_forceEnglish{}, s_warnings{};
const dvar_t *useFastFile = &s_fastfile;

void MyAssertHandler(const char *, int, int, const char *, ...) { std::abort(); }
void Com_Error(errorParm_t, const char *, ...) { std::abort(); }
void Com_PrintWarning(int, const char *, ...) {}
void Com_Printf(int, const char *, ...) {}
char *va(const char *format, ...)
{
    static char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}
void I_strncpyz(char *dest, const char *src, int size)
{
    if (!src || size < 1) std::abort();
    std::snprintf(dest, size, "%s", src);
}
int I_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
int Com_sprintf(char *dest, uint32_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = std::vsnprintf(dest, size, format, args);
    va_end(args);
    return result;
}
void I_strncat(char *dest, int size, const char *src)
{
    const int used = static_cast<int>(std::strlen(dest));
    I_strncpyz(dest + used, src, size - used);
}
char *UI_SafeTranslateString(const char *)
{
    static char unbound[] = "Unbound";
    return unbound;
}
static const char *s_fixtureName;
static const char *s_fixtureText;
XAssetHeader DB_FindXAssetHeader(XAssetType type, const char *name)
{
    static LocalizeEntry entry;
    XAssetHeader result{};
    if (type == ASSET_TYPE_LOCALIZE_ENTRY && s_fixtureName && !std::strcmp(name, s_fixtureName))
    {
        entry.name = s_fixtureName;
        entry.value = s_fixtureText;
        result.localize = &entry;
    }
    return result;
}

static void Require(bool condition, const char *what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL:CONTROLLER_PROMPTS %s\n", what);
        std::exit(1);
    }
}

int main(int argc, char **argv)
{
    s_fastfile.current.enabled = true;
    s_translate.current.enabled = true;
    s_warnings.current.enabled = true;
    loc_language = &s_language;
    loc_translate = &s_translate;
    loc_forceEnglish = &s_forceEnglish;
    loc_warnings = &s_warnings;
    loc_warningsAsErrors = &s_warnings;
    if (argc == 3 && !std::strcmp(argv[1], "--binding"))
    {
        char output[256];
        const int count = GetKeyBindingLocalizedString(0, argv[2], output, true);
        std::puts(output);
        return count ? 0 : 2;
    }
    if (argc == 4 && !std::strcmp(argv[1], "--localize"))
    {
        s_fixtureName = argv[2];
        s_fixtureText = argv[3];
        std::puts(SE_GetString(s_fixtureName));
        return 0;
    }

    // A copied PC profile must not determine the console's hint or the
    // script's choice between separate melee and scoped-breath actions.
    playerKeys[0].keys['f'].binding = "+activate";
    playerKeys[0].keys['r'].binding = "+reload";
    playerKeys[0].keys[160].binding = "+holdbreath";
    playerKeys[0].keys[161].binding = "+melee_breath";
    playerKeys[0].keys['w'].binding = "+forward";
    char keys[2][128] = {};
    char label[256] = {};
    Require(CL_GetKeyBinding(0, "+activate", keys) == 1 && !std::strcmp(keys[0], "Y")
            && keys[1][0] == 0, "pickup uses Y despite F profile");
    Require(GetKeyBindingLocalizedString(0, "+reload", label, false) == 1
            && !std::strcmp(label, "Y"), "reload uses Y despite R profile");
    Require(!Key_IsCommandBound(0, "+holdbreath") && !Key_IsCommandBound(0, "+melee_breath"),
            "keyboard-only aliases cannot select controller variants");
    Require(GetKeyBindingLocalizedString(0, "+holdbreath", label, false) == 0,
            "steady hint falls through the keyboard command");
    Require(GetKeyBindingLocalizedString(0, "+breath_sprint", label, false) == 1
            && !std::strcmp(label, "L Stick"), "scoped steady uses left stick click");
    Require(Key_IsCommandBound(0, "+stance") && !Key_IsCommandBound(0, "goprone"),
            "stance variant selects the executed hold command");
    Require(GetKeyBindingLocalizedString(0, "+forward", label, false) == 1
            && !std::strcmp(label, "L Stick"), "movement uses analog stick despite W profile");
    Require(GetKeyBindingLocalizedString(0, "+speed", label, false) == 1
            && !std::strcmp(label, "ZL"), "aiming uses ZL");
    Require(!Switch_InputPadBindingName("+binoculars"), "unsupported legacy action has no invented button");

    WeaponDef weapon{};
    bg_lastParsedWeaponIndex = 1;
    bg_weaponDefs[1] = &weapon;
    weapon.weapType = WEAPTYPE_GRENADE;
    weapon.hasDetonator = true;
    Require(PM_GetWeaponFireButton(1) == BUTTON_THROW, "retail C4 requires the throw bit");
    const PadThrowDecision c4 = PadThrowResolve(true, false, PM_GetWeaponFireButton(1) == BUTTON_THROW);
    Require(c4.throwSelected && !c4.frag, "advertised C4 control drives its required fire button");
    weapon.hasDetonator = false;
    Require(PM_GetWeaponFireButton(1) == BUTTON_ATTACK, "normal grenades retain attack fire button");
    weapon.weapType = WEAPTYPE_BULLET;
    weapon.hasDetonator = true;
    Require(PM_GetWeaponFireButton(1) == BUTTON_ATTACK, "only detonator grenades change throw routing");
    bg_weaponDefs[1] = nullptr;
    for (int detonator = 0; detonator < 2; ++detonator)
        for (int zl = 0; zl < 2; ++zl)
            for (int r = 0; r < 2; ++r)
            {
                const PadThrowDecision throwing = PadThrowResolve(zl, r, detonator);
                Require(throwing.frag == (r && !detonator), "normal R throws offhand frag");
                Require(throwing.throwSelected == (detonator && (zl || r)),
                        "detonator grenade throws on either advertised control");
            }

    // Drive the real localizer and argument insertion, not a copied prompt
    // formatter. Conversion slots must survive the controller wording.
    char message[] = "SCRIPT_PLATFORM_HINT_MOVEFORWARD\x14L Stick";
    const char *translated = SEH_LocalizeTextMessage(message, "controller test", LOCMSG_SAFE);
    Require(translated && !std::strcmp(translated, "Move ^3L Stick^7 forward."),
            "movement localization consumes script argument");
    const uint64_t buttons[] = {SWITCH_INPUT_BUTTON_A, SWITCH_INPUT_BUTTON_B,
        SWITCH_INPUT_BUTTON_X, SWITCH_INPUT_BUTTON_Y, SWITCH_INPUT_BUTTON_STICKL,
        SWITCH_INPUT_BUTTON_STICKR, SWITCH_INPUT_BUTTON_L, SWITCH_INPUT_BUTTON_R,
        SWITCH_INPUT_BUTTON_ZL, SWITCH_INPUT_BUTTON_ZR, SWITCH_INPUT_BUTTON_PLUS,
        SWITCH_INPUT_BUTTON_MINUS, SWITCH_INPUT_BUTTON_UP, SWITCH_INPUT_BUTTON_DOWN,
        SWITCH_INPUT_BUTTON_LEFT, SWITCH_INPUT_BUTTON_RIGHT};
    for (uint64_t button : buttons)
    {
        const char *name = Switch_InputPadButtonName(button);
        const char *localized = SEH_LocalizeTextMessage(name, "controller label", LOCMSG_SAFE);
        Require(localized && !std::strcmp(localized, name), "every script control label resolves");
    }
    Require(!Switch_ControllerControlLabel("UNRELATED_TOKEN"), "no arbitrary token passthrough");
    s_translate.current.enabled = false;
    Require(!std::strcmp(SEH_LocalizeTextMessage("L Stick", "untranslated control", LOCMSG_SAFE), "L Stick"),
            "translation disabled handles the aliased token");
    s_translate.current.enabled = true;
    Require(!std::strcmp(SE_GetString("PLATFORM_DYK_MSG36"),
                         "With C4 selected, press ^3ZR^7 to detonate placed charges."),
            "loading tip uses the supported detonator control");
    Require(!std::strcmp(SE_GetString("SCOUTSNIPER_HINT_PRONE_DOUBLE"), "Hold ^3B^7 to go prone."),
            "prone fixes double tap and misspelled command");
    Require(!std::strcmp(SE_GetString("KILLHOUSE_HINT_BREATH_MELEE"),
                         "Hold ^3L Stick^7 while scoped to steady your aim."), "breath context uses L stick");
    Require(!std::strcmp(SE_GetString("KILLHOUSE_HINT_MELEE_BREATH"),
                         "Click ^3R Stick^7 to melee."), "melee context uses R stick");
    Require(!Switch_ControllerPrompt("KILLHOUSE_HINT_PRONE_UNRELATED", 0), "no broad reference rewriting");
    s_fixtureName = "SCRIPT_PLATFORM_HINT_MOVEFORWARD";
    s_fixtureText = "localized fixture";
    s_language.current.integer = 1;
    Require(!std::strcmp(SE_GetString(s_fixtureName), s_fixtureText), "preserve other languages");
    s_forceEnglish.current.enabled = true;
    Require(!std::strcmp(SE_GetString(s_fixtureName), "Move ^3&&1^7 forward."), "forced English uses controls");
    std::puts("PASS:CONTROLLER_PROMPTS binding=production localization=production profile=pc gesture=controller");
}
