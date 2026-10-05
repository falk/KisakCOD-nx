#ifndef SWITCH_MENU_SETTINGS_H
#define SWITCH_MENU_SETTINGS_H

// Preset tables and apply/reset/match logic for the port-authored options
// pages. Pure: dvar access goes through SwMenuDvarOps so the host test drives
// it with a fake store; switch_ui_menus.cpp binds the real dvar system.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SwMenuDvarOps
{
    void *ctx;
    // Current value as text; false when the dvar does not exist.
    bool (*get)(void *ctx, const char *name, char *out, size_t outSize);
    // Value a latched dvar will take at the next start (the current value
    // when nothing is pending).
    bool (*getLatched)(void *ctx, const char *name, char *out, size_t outSize);
    void (*set)(void *ctx, const char *name, const char *value);
    void (*reset)(void *ctx, const char *name);
};

struct SwMenuPair
{
    const char *name;
    const char *value;
};

struct SwMenuPreset
{
    const char *label;
    const SwMenuPair *pairs;
    int count;
};

// Every preset sets the same keys so nothing from a previous preset survives.
// r_dynres is latched: presets request it and the page says a restart is
// needed. com_maxfps 0 is the uncapped 60 Hz vsync pace; the dynres GPU budget
// follows the cap in the controller, so it is not a preset value. Quality pins the
// scale at native so TAAU runs as plain TAA (it resolves at the dynres site).
static const SwMenuPair kSwQualityPairs[] = {
    {"com_maxfps", "30"},
    {"r_dynres", "1"},
    {"r_dynresMin", "1"},
    {"r_dynresMax", "1"},
    {"r_taau", "1"},
    {"r_fsrMode", "sgsr"},
    {"r_fsrSharpness", "0.2"},
    {"sm_enable", "1"},
    {"r_halfResParticles", "0"},
    {"r_shadowFilter", "0"},
};
static const SwMenuPair kSwBalancedPairs[] = {
    {"com_maxfps", "0"},
    {"r_dynres", "1"},
    {"r_dynresMin", "0.67"},
    {"r_dynresMax", "1"},
    {"r_taau", "1"},
    {"r_fsrMode", "sgsr"},
    {"r_fsrSharpness", "0.2"},
    {"sm_enable", "1"},
    {"r_halfResParticles", "3"},
    {"r_shadowFilter", "0"},
};
static const SwMenuPair kSwPerformancePairs[] = {
    {"com_maxfps", "0"},
    {"r_dynres", "1"},
    {"r_dynresMin", "0.5"},
    {"r_dynresMax", "1"},
    {"r_taau", "1"},
    {"r_fsrMode", "sgsr"},
    {"r_fsrSharpness", "0.2"},
    {"sm_enable", "1"},
    {"r_halfResParticles", "1"},
    {"r_shadowFilter", "1"},
};

enum
{
    SW_PRESET_QUALITY = 0,
    SW_PRESET_BALANCED = 1,
    SW_PRESET_PERFORMANCE = 2,
    SW_PRESET_COUNT = 3,
    SW_PRESET_CUSTOM = SW_PRESET_COUNT,
};

#define SW_ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const SwMenuPreset kSwPresets[SW_PRESET_COUNT] = {
    {"Quality", kSwQualityPairs, SW_ARRAY_COUNT(kSwQualityPairs)},
    {"Balanced", kSwBalancedPairs, SW_ARRAY_COUNT(kSwBalancedPairs)},
    {"Performance", kSwPerformancePairs, SW_ARRAY_COUNT(kSwPerformancePairs)},
};

// Every dvar the Graphics page edits; "Reset to defaults" restores these.
static const char *const kSwGraphicsDvars[] = {
    "com_maxfps", "r_dynres", "r_dynresMin", "r_dynresMax", "r_taau",
    "r_fsrMode", "r_fsrSharpness", "r_halfResParticles", "sm_enable", "r_shadowFilter",
};

static const char *const kSwControllerDvars[] = {
    "input_viewSensitivity", "input_lookCurve", "input_invertPitch", "cl_yawspeed", "cl_pitchspeed",
    "gyro_enable", "gyro_sensitivity", "gyro_invertYaw", "gyro_invertPitch",
    "rumble_enable", "rumble_intensity", "rumble_hd",
};

static const char *const kSwSoundGameDvars[] = {"snd_volume", "cg_blood", "cg_subtitles", "cg_drawCrosshair"};

// Settings the stock engine registers without DVAR_ARCHIVE; the pages need
// them to survive a restart.
static const char *const kSwArchiveDvars[] = {
    "input_viewSensitivity", "input_invertPitch", "cl_yawspeed", "cl_pitchspeed",
    "r_taau", "r_halfResParticles", "r_shadowFilter",
};

static inline bool SwMenu_ValuesEqual(const char *a, const char *b)
{
    if (!strcmp(a, b))
        return true;
    char *endA = 0;
    char *endB = 0;
    double x = strtod(a, &endA);
    double y = strtod(b, &endB);
    if (endA == a || endB == b || *endA || *endB)
        return false;
    return fabs(x - y) < 1e-4;
}

static inline bool SwMenu_IsRangeDvar(const char *name)
{
    return !strcmp(name, "r_dynresMin") || !strcmp(name, "r_dynresMax");
}

// Sets every dvar of the preset. Returns the number of dvars written.
static inline int SwMenu_ApplyPreset(const SwMenuDvarOps *ops, int preset)
{
    if (preset < 0 || preset >= SW_PRESET_COUNT)
        return 0;
    const SwMenuPreset &p = kSwPresets[preset];
    for (int i = 0; i < p.count; ++i)
        ops->set(ops->ctx, p.pairs[i].name, p.pairs[i].value);
    return p.count;
}

static inline bool SwMenu_ReadSetting(const SwMenuDvarOps *ops, const char *name, char *out, size_t outSize)
{
    // The latched value is what the player chose; the current one is what
    // runs until the restart.
    return ops->getLatched(ops->ctx, name, out, outSize);
}

// Index of the preset whose every dvar matches, else SW_PRESET_CUSTOM.
static inline int SwMenu_MatchPreset(const SwMenuDvarOps *ops)
{
    for (int preset = 0; preset < SW_PRESET_COUNT; ++preset)
    {
        const SwMenuPreset &p = kSwPresets[preset];
        bool all = true;
        for (int i = 0; i < p.count && all; ++i)
        {
            char value[64];
            all = SwMenu_ReadSetting(ops, p.pairs[i].name, value, sizeof(value)) &&
                SwMenu_ValuesEqual(value, p.pairs[i].value);
        }
        if (all)
            return preset;
    }
    return SW_PRESET_CUSTOM;
}

// The dynres range must stay ordered; the page raises max to meet min.
static inline bool SwMenu_FixDynResRange(const SwMenuDvarOps *ops)
{
    char lo[64];
    char hi[64];
    if (!ops->get(ops->ctx, "r_dynresMin", lo, sizeof(lo)) || !ops->get(ops->ctx, "r_dynresMax", hi, sizeof(hi)))
        return false;
    if (atof(lo) <= atof(hi) + 1e-6)
        return false;
    ops->set(ops->ctx, "r_dynresMax", lo);
    return true;
}

// True when a latched setting differs from what is running.
static inline bool SwMenu_RestartPending(const SwMenuDvarOps *ops)
{
    char now[64];
    char later[64];
    if (!ops->get(ops->ctx, "r_dynres", now, sizeof(now)) || !ops->getLatched(ops->ctx, "r_dynres", later, sizeof(later)))
        return false;
    return !SwMenu_ValuesEqual(now, later);
}

static inline int SwMenu_ResetDvars(const SwMenuDvarOps *ops, const char *const *names, int count)
{
    for (int i = 0; i < count; ++i)
        ops->reset(ops->ctx, names[i]);
    return count;
}

// Graphics defaults are the Balanced preset (the engine's plain defaults
// differ for particles), after resetting every page dvar.
static inline int SwMenu_ResetGraphics(const SwMenuDvarOps *ops)
{
    SwMenu_ResetDvars(ops, kSwGraphicsDvars, SW_ARRAY_COUNT(kSwGraphicsDvars));
    SwMenu_ApplyPreset(ops, SW_PRESET_BALANCED);
    return SW_ARRAY_COUNT(kSwGraphicsDvars);
}

#endif
