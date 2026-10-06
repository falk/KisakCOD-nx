#ifndef SWITCH_RUMBLE_NAMES_H
#define SWITCH_RUMBLE_NAMES_H

// Port-owned map from the rumble names SP scripts pass to precacheRumble /
// playRumble* to HD effects.  PC retail zones carry no rumble curves, only
// the names, so these curves are authored here.  Names compare
// case-insensitively; unknown names fall back to an effect picked by a
// substring pattern.

#include "switch_rumble_hd.h"

#include <stddef.h>
#include <string.h>

typedef struct SwitchRumbleNameInfo
{
    int32_t effect;
    float scale;
    // Falloff radius (world units) when the rumble comes from something
    // other than the local player; 0 = no falloff.
    float radius;
    int32_t category;
    int32_t known;
} SwitchRumbleNameInfo;

typedef struct SwitchRumbleNameEntry
{
    const char *name;
    int32_t effect;
    float scale;
    float radius;
    int32_t category;
} SwitchRumbleNameEntry;

static const SwitchRumbleNameEntry kSwitchRumbleNames[] = {
    {"artillery_rumble", SWITCH_RUMBLE_HD_SCRIPT_BLAST, 0.90f, 3000.0f, SWITCH_RUMBLE_CAT_BLAST},
    {"crash_heli_rumble", SWITCH_RUMBLE_HD_SCRIPT_ROTOR, 1.00f, 1800.0f, SWITCH_RUMBLE_CAT_NONE},
    {"crash_heli_rumble_rest", SWITCH_RUMBLE_HD_SCRIPT_ROTOR, 0.55f, 1800.0f, SWITCH_RUMBLE_CAT_NONE},
    {"damage_heavy", SWITCH_RUMBLE_HD_SCRIPT_DAMAGE_HEAVY, 1.00f, 0.0f, SWITCH_RUMBLE_CAT_DAMAGE},
    {"damage_light", SWITCH_RUMBLE_HD_SCRIPT_DAMAGE_LIGHT, 1.00f, 0.0f, SWITCH_RUMBLE_CAT_DAMAGE},
    {"grenade_rumble", SWITCH_RUMBLE_HD_SCRIPT_BLAST, 0.70f, 900.0f, SWITCH_RUMBLE_CAT_BLAST},
    {"jeepride_bridgesink", SWITCH_RUMBLE_HD_SCRIPT_BLAST, 1.00f, 3500.0f, SWITCH_RUMBLE_CAT_NONE},
    {"jeepride_cliffblow", SWITCH_RUMBLE_HD_SCRIPT_BLAST, 1.00f, 3500.0f, SWITCH_RUMBLE_CAT_NONE},
    {"jeepride_pillarblow", SWITCH_RUMBLE_HD_SCRIPT_BLAST, 1.00f, 3500.0f, SWITCH_RUMBLE_CAT_NONE},
    {"mig_rumble", SWITCH_RUMBLE_HD_SCRIPT_ROTOR, 0.80f, 3000.0f, SWITCH_RUMBLE_CAT_NONE},
    {"minigun_rumble", SWITCH_RUMBLE_HD_SCRIPT_MINIGUN, 1.00f, 0.0f, SWITCH_RUMBLE_CAT_NONE},
    {"stinger_lock_rumble", SWITCH_RUMBLE_HD_SCRIPT_LOCK, 1.00f, 0.0f, SWITCH_RUMBLE_CAT_NONE},
    {"tank_rumble", SWITCH_RUMBLE_HD_SCRIPT_TANK, 1.00f, 1400.0f, SWITCH_RUMBLE_CAT_NONE},
};

static inline int32_t SwitchRumbleNames_Equal(const char *a, const char *b)
{
    for (;; ++a, ++b)
    {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        if (ca == 0)
            return 1;
    }
}

static inline int32_t SwitchRumbleNames_Contains(const char *name, const char *needle)
{
    const size_t n = strlen(needle);
    for (; *name; ++name)
    {
        size_t i;
        for (i = 0; i < n; ++i)
        {
            char c = name[i];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != needle[i])
                break;
        }
        if (i == n)
            return 1;
    }
    return 0;
}

// Fills `out` for `name`; returns out->known (0 = pattern fallback).
static inline int32_t SwitchRumbleNames_Lookup(const char *name, SwitchRumbleNameInfo *out)
{
    size_t i;
    if (out == NULL)
        return 0;
    out->known = 0;
    out->scale = 1.0f;
    out->radius = 0.0f;
    out->category = SWITCH_RUMBLE_CAT_NONE;
    out->effect = SWITCH_RUMBLE_HD_SCRIPT_GENERIC;
    if (name == NULL || *name == 0)
        return 0;
    for (i = 0; i < sizeof(kSwitchRumbleNames) / sizeof(kSwitchRumbleNames[0]); ++i)
    {
        if (SwitchRumbleNames_Equal(name, kSwitchRumbleNames[i].name))
        {
            out->effect = kSwitchRumbleNames[i].effect;
            out->scale = kSwitchRumbleNames[i].scale;
            out->radius = kSwitchRumbleNames[i].radius;
            out->category = kSwitchRumbleNames[i].category;
            out->known = 1;
            return 1;
        }
    }
    if (SwitchRumbleNames_Contains(name, "tank") || SwitchRumbleNames_Contains(name, "engine") ||
        SwitchRumbleNames_Contains(name, "vehicle"))
    {
        out->effect = SWITCH_RUMBLE_HD_SCRIPT_TANK;
        out->radius = 1400.0f;
    }
    else if (SwitchRumbleNames_Contains(name, "heli") || SwitchRumbleNames_Contains(name, "rotor") ||
             SwitchRumbleNames_Contains(name, "jet") || SwitchRumbleNames_Contains(name, "mig"))
    {
        out->effect = SWITCH_RUMBLE_HD_SCRIPT_ROTOR;
        out->radius = 2000.0f;
    }
    else if (SwitchRumbleNames_Contains(name, "light"))
    {
        out->effect = SWITCH_RUMBLE_HD_SCRIPT_DAMAGE_LIGHT;
        out->category = SWITCH_RUMBLE_CAT_DAMAGE;
    }
    else if (SwitchRumbleNames_Contains(name, "damage") || SwitchRumbleNames_Contains(name, "heavy"))
    {
        out->effect = SWITCH_RUMBLE_HD_SCRIPT_DAMAGE_HEAVY;
        out->category = SWITCH_RUMBLE_CAT_DAMAGE;
    }
    else if (SwitchRumbleNames_Contains(name, "blow") || SwitchRumbleNames_Contains(name, "explo") ||
             SwitchRumbleNames_Contains(name, "grenade") || SwitchRumbleNames_Contains(name, "artillery") ||
             SwitchRumbleNames_Contains(name, "crash") || SwitchRumbleNames_Contains(name, "sink"))
    {
        out->effect = SWITCH_RUMBLE_HD_SCRIPT_BLAST;
        out->radius = 2500.0f;
    }
    else if (SwitchRumbleNames_Contains(name, "gun") || SwitchRumbleNames_Contains(name, "fire"))
    {
        out->effect = SWITCH_RUMBLE_HD_SCRIPT_MINIGUN;
    }
    else if (SwitchRumbleNames_Contains(name, "lock"))
    {
        out->effect = SWITCH_RUMBLE_HD_SCRIPT_LOCK;
    }
    return 0;
}

#endif
