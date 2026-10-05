#pragma once

// Command-line `set` (kisak_diag.cfg, nxlink arguments) is a per-run
// override: config.cfg keeps the value each override replaced, so a test run
// that turns an archived setting off (e.g. sm_enable 0) cannot leave it off in
// the player's profile for every later boot. `seta` still persists, and a
// value changed during the run (menu, console) is saved as usual.
//
// The tracking is pure string logic so it runs on the host; the hooks that
// feed it Dvar values live in switch_cmdline_dvars.cpp.

#include <cstring>
#include <strings.h>

struct SwCmdlineDvar
{
    char name[64];
    char base[256];    // value before the override (profile config or default)
    char applied[256]; // displayed value right after the override was applied
    bool hasBase;
    bool hasApplied;
};

struct SwCmdlineDvarTable
{
    static constexpr int kMax = 128;
    SwCmdlineDvar entries[kMax];
    int count;
};

inline SwCmdlineDvarTable g_swCmdlineDvars;

inline void Sw_CmdlineDvarCopy(char *dst, size_t size, const char *src)
{
    std::strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

inline SwCmdlineDvar *Sw_CmdlineDvarFind(SwCmdlineDvarTable *t, const char *name, bool create)
{
    if (!name || !name[0])
        return nullptr;
    for (int i = 0; i < t->count; ++i)
    {
        if (!strcasecmp(t->entries[i].name, name))
            return &t->entries[i];
    }
    if (!create || t->count == SwCmdlineDvarTable::kMax || std::strlen(name) >= sizeof(t->entries[0].name))
        return nullptr;
    SwCmdlineDvar *e = &t->entries[t->count++];
    std::memset(e, 0, sizeof(*e));
    Sw_CmdlineDvarCopy(e->name, sizeof(e->name), name);
    return e;
}

// Before a command-line `set` is (re)applied. current is the dvar's displayed
// value, or null when it does not exist yet. The startup passes run before and
// after the profile config executes; whatever moved the value away from our
// last override since then (the config) is the value to keep.
inline void Sw_CmdlineDvarBeforeSet(SwCmdlineDvarTable *t, const char *name, const char *current)
{
    SwCmdlineDvar *e = Sw_CmdlineDvarFind(t, name, true);
    if (!e || !current)
        return;
    if (!e->hasBase || !e->hasApplied || std::strcmp(current, e->applied))
    {
        Sw_CmdlineDvarCopy(e->base, sizeof(e->base), current);
        e->hasBase = true;
    }
}

inline void Sw_CmdlineDvarAfterSet(SwCmdlineDvarTable *t, const char *name, const char *current)
{
    SwCmdlineDvar *e = Sw_CmdlineDvarFind(t, name, false);
    if (!e || !current)
        return;
    Sw_CmdlineDvarCopy(e->applied, sizeof(e->applied), current);
    e->hasApplied = true;
}

// The value config.cfg records for an archived dvar: the pre-override value
// while the override is still in effect, otherwise the current value. With no
// profile value ever seen (a new profile) the override is what gets saved.
inline const char *Sw_CmdlineDvarPersistValue(const SwCmdlineDvarTable *t, const char *name, const char *current)
{
    const SwCmdlineDvar *e = Sw_CmdlineDvarFind(const_cast<SwCmdlineDvarTable *>(t), name, false);
    if (!e || !e->hasBase || !e->hasApplied || std::strcmp(current, e->applied))
        return current;
    return e->base;
}
