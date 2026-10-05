// Host test for src/port/switch_cmdline_dvars.h: a command-line `set` of an
// archived dvar (a diagnostic run's `+set sm_enable 0`) must not become the
// profile's saved value. Replays Com_Init's order: startup pass, profile
// config exec, two more startup passes, a config write, then the next boot.

#include "src/port/switch_cmdline_dvars.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

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

// Fake dvar store: name -> displayed value; absent = not created yet.
using Store = std::map<std::string, std::string>;
using Lines = std::vector<std::pair<std::string, std::string>>;

static void StartupPass(SwCmdlineDvarTable *t, Store *dvars, const Lines &cmdline)
{
    for (const auto &kv : cmdline)
    {
        auto it = dvars->find(kv.first);
        Sw_CmdlineDvarBeforeSet(t, kv.first.c_str(), it == dvars->end() ? nullptr : it->second.c_str());
        (*dvars)[kv.first] = kv.second;
        Sw_CmdlineDvarAfterSet(t, kv.first.c_str(), (*dvars)[kv.first].c_str());
    }
}

static void ExecConfig(Store *dvars, const Store &config)
{
    for (const auto &kv : config)
        (*dvars)[kv.first] = kv.second;
}

// Writes every dvar of the store, as Dvar_WriteVariables does for archived ones.
static Store WriteConfig(const SwCmdlineDvarTable *t, const Store &dvars)
{
    Store out;
    for (const auto &kv : dvars)
        out[kv.first] = Sw_CmdlineDvarPersistValue(t, kv.first.c_str(), kv.second.c_str());
    return out;
}

// One boot: defaults registered, startup pass, profile config, passes 2 and 3,
// optional in-run changes, then the shutdown config write.
static Store Boot(const Store &defaults, const Store &profile, const Lines &cmdline, const Store &runChanges,
                  Store *liveOut = nullptr)
{
    SwCmdlineDvarTable t{};
    Store dvars;
    StartupPass(&t, &dvars, cmdline); // before registration and config
    ExecConfig(&dvars, profile);
    for (const auto &kv : defaults) // registration keeps a value already set
        dvars.emplace(kv.first, kv.second);
    StartupPass(&t, &dvars, cmdline);
    StartupPass(&t, &dvars, cmdline);
    if (liveOut)
        *liveOut = dvars;
    for (const auto &kv : runChanges)
        dvars[kv.first] = kv.second;
    return WriteConfig(&t, dvars);
}

int main()
{
    const Store defaults = {{"sm_enable", "1"}, {"r_dynres", "0"}, {"r_shadowFilter", "0"}};

    // A diagnostic run with shadows off: the run sees 0, the profile keeps 1,
    // and the next production boot renders shadows again.
    {
        const Store profile = {{"sm_enable", "1"}, {"r_dynres", "1"}, {"r_shadowFilter", "0"}};
        Store live;
        const Store saved = Boot(defaults, profile, {{"sm_enable", "0"}}, {}, &live);
        CHECK(live["sm_enable"] == "0");
        CHECK(saved.at("sm_enable") == "1");
        CHECK(saved.at("r_dynres") == "1");
        Store next;
        Boot(defaults, saved, {}, {}, &next);
        CHECK(next["sm_enable"] == "1");
    }

    // An override equal to the profile value saves that value (not the default).
    {
        const Store profile = {{"sm_enable", "1"}, {"r_dynres", "1"}, {"r_shadowFilter", "0"}};
        const Store saved = Boot(defaults, profile, {{"r_dynres", "1"}}, {});
        CHECK(saved.at("r_dynres") == "1");
    }

    // A setting changed during the run (Graphics menu) is saved even though
    // the command line overrode it.
    {
        const Store profile = {{"sm_enable", "1"}, {"r_dynres", "1"}, {"r_shadowFilter", "0"}};
        const Store saved = Boot(defaults, profile, {{"sm_enable", "0"}, {"r_shadowFilter", "1"}},
                                 {{"sm_enable", "1"}, {"r_shadowFilter", "0"}});
        CHECK(saved.at("sm_enable") == "1");
        CHECK(saved.at("r_shadowFilter") == "0");
    }

    // Case-insensitive names, as the dvar system matches them.
    {
        const Store profile = {{"sm_enable", "1"}};
        SwCmdlineDvarTable t{};
        Sw_CmdlineDvarBeforeSet(&t, "SM_ENABLE", profile.at("sm_enable").c_str());
        Sw_CmdlineDvarAfterSet(&t, "SM_ENABLE", "0");
        CHECK(!std::strcmp(Sw_CmdlineDvarPersistValue(&t, "sm_enable", "0"), "1"));
    }

    // Untracked dvars and a full table pass values through unchanged.
    {
        SwCmdlineDvarTable t{};
        CHECK(!std::strcmp(Sw_CmdlineDvarPersistValue(&t, "r_taau", "1"), "1"));
        char name[32];
        for (int i = 0; i < SwCmdlineDvarTable::kMax + 4; ++i)
        {
            std::snprintf(name, sizeof(name), "d%d", i);
            Sw_CmdlineDvarBeforeSet(&t, name, "a");
            Sw_CmdlineDvarAfterSet(&t, name, "b");
        }
        CHECK(t.count == SwCmdlineDvarTable::kMax);
        CHECK(!std::strcmp(Sw_CmdlineDvarPersistValue(&t, "d0", "b"), "a"));
        CHECK(!std::strcmp(Sw_CmdlineDvarPersistValue(&t, name, "b"), "b"));
    }

    if (g_failures)
    {
        std::printf("FAIL:SWITCH_CMDLINE_DVARS %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS:SWITCH_CMDLINE_DVARS command-line set overrides are not saved to the profile config\n");
    return 0;
}
