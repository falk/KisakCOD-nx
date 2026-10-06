// Host ASan/UBSan proof for script-driven rumble: the production cgame
// event path (cg_rumble.cpp), the name table and the HD mixer.  The only
// fakes are the configstring lookup, the entity/view globals and the
// vibration player handle.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "src/cgame/cg_main.h"
#include "src/cgame/cg_rumble.h"
#include "src/port/switch_rumble.h"
#include "src/port/switch_rumble_hd.h"
#include "src/port/switch_rumble_names.h"

cg_s cgArray[1];
centity_s cg_entitiesArray[1][MAX_GENTITIES];

static SwitchRumbleHdPlayer s_player;
static bool s_hdOn = true;
static char s_cs[8][64];
static int s_printCount;
static char s_lastPrint[256];

SwitchRumbleHdPlayer *Switch_RumbleHdPlayer(void) { return s_hdOn ? &s_player : nullptr; }
const char *CL_GetConfigString(int, uint32_t index)
{
    return s_cs[index - CS_RUMBLES];
}
void Com_Printf(int, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(s_lastPrint, sizeof(s_lastPrint), fmt, ap);
    va_end(ap);
    ++s_printCount;
}
void MyAssertHandler(const char *, int, int, const char *, ...)
{
    std::fprintf(stderr, "FAIL:RUMBLE_SCRIPT unexpected assert\n");
    std::abort();
}
void Com_Error(errorParm_t, const char *, ...)
{
    std::fprintf(stderr, "FAIL:RUMBLE_SCRIPT unexpected Com_Error\n");
    std::abort();
}

static int s_failures;
#define CHECK(c)                                                                      \
    do                                                                                \
    {                                                                                 \
        if (!(c))                                                                     \
        {                                                                             \
            std::fprintf(stderr, "FAIL:RUMBLE_SCRIPT line %d: %s\n", __LINE__, #c);   \
            ++s_failures;                                                             \
        }                                                                             \
    } while (0)

static float Peak(float seconds, float step, SwitchRumbleHdOut *last = nullptr)
{
    // Highest per-motor band amplitude seen while advancing `seconds`.
    float peak = 0.0f;
    SwitchRumbleHdOut out;
    for (float t = 0.0f; t < seconds; t += step)
    {
        SwitchRumbleHd_Advance(&s_player, step, &out);
        for (float a : {out.left.ampLow, out.left.ampHigh})
            if (a > peak)
                peak = a;
    }
    if (last)
        *last = out;
    return peak;
}

static int ActiveVoices()
{
    int n = 0;
    for (auto &v : s_player.voices)
        n += v.active;
    return n;
}

static void Fresh()
{
    SwitchRumbleHd_Init(&s_player);
    std::memset(s_cs, 0, sizeof(s_cs));
    s_hdOn = true;
    std::memset(cgArray, 0, sizeof(cgArray));
    std::memset(cg_entitiesArray, 0, sizeof(cg_entitiesArray));
    cgArray[0].predictedPlayerState.clientNum = 0;
}

static void SetName(int idx, const char *n) { std::strncpy(s_cs[idx], n, 63); }

int main()
{
    // ---- name table ----
    {
        SwitchRumbleNameInfo i;
        CHECK(SwitchRumbleNames_Lookup("TANK_RUMBLE", &i) && i.effect == SWITCH_RUMBLE_HD_SCRIPT_TANK && i.known);
        CHECK(SwitchRumbleNames_Lookup("damage_light", &i) && i.category == SWITCH_RUMBLE_CAT_DAMAGE);
        CHECK(SwitchRumbleNames_Lookup("artillery_rumble", &i) && i.radius > 0.0f);
        CHECK(!SwitchRumbleNames_Lookup("future_tank_thing", &i) && i.effect == SWITCH_RUMBLE_HD_SCRIPT_TANK);
        CHECK(!SwitchRumbleNames_Lookup("big_explosion", &i) && i.effect == SWITCH_RUMBLE_HD_SCRIPT_BLAST);
        CHECK(!SwitchRumbleNames_Lookup("zzz", &i) && i.effect == SWITCH_RUMBLE_HD_SCRIPT_GENERIC);
        CHECK(!SwitchRumbleNames_Lookup("", &i) && i.effect == SWITCH_RUMBLE_HD_SCRIPT_GENERIC);
        // Every retail script name has an authored (known) entry.
        const char *retail[] = {"artillery_rumble", "crash_heli_rumble", "crash_heli_rumble_rest", "damage_heavy",
                                "damage_light", "grenade_rumble", "jeepride_bridgesink", "jeepride_cliffblow",
                                "jeepride_pillarblow", "minigun_rumble", "stinger_lock_rumble", "tank_rumble",
                                "mig_rumble"};
        for (const char *n : retail)
            CHECK(SwitchRumbleNames_Lookup(n, &i));
    }

    // ---- play on the local player: full strength, one-shot ends ----
    {
        Fresh();
        SetName(1, "damage_heavy");
        CHECK(CG_RumbleEvent(0, EV_PLAY_RUMBLE_ON_ENT, 1, 0, cg_entitiesArray[0][0].pose.origin) == 1);
        CHECK(s_player.started[SWITCH_RUMBLE_SRC_SCRIPT] == 1 && ActiveVoices() == 1);
        const float p = Peak(0.1f, 0.01f);
        CHECK(p > 0.5f);
        Peak(0.6f, 0.01f);
        CHECK(ActiveVoices() == 0);
        CHECK(CG_RumbleEvent(0, EV_SOUND_ALIAS, 0, 0, nullptr) == 0);
    }

    // ---- positional falloff ----
    {
        float at[3] = {0, 0, 0}, mid[3] = {1500, 0, 0}, far[3] = {3500, 0, 0};
        Fresh();
        SetName(2, "artillery_rumble");
        SwitchRumbleHdOut o0, o1, o2;
        CG_RumbleEvent(0, EV_PLAY_RUMBLE_ON_POS, 2, 1, at);
        SwitchRumbleHd_Advance(&s_player, 0.08f, &o0);
        Fresh();
        SetName(2, "artillery_rumble");
        CG_RumbleEvent(0, EV_PLAY_RUMBLE_ON_POS, 2, 1, mid);
        SwitchRumbleHd_Advance(&s_player, 0.08f, &o1);
        Fresh();
        SetName(2, "artillery_rumble");
        CG_RumbleEvent(0, EV_PLAY_RUMBLE_ON_POS, 2, 1, far);
        SwitchRumbleHd_Advance(&s_player, 0.08f, &o2);
        CHECK(o0.left.ampLow > 0.5f);
        // half the radius -> smoothstep(0.5) = 0.5 of the unattenuated level
        CHECK(o1.left.ampLow > o0.left.ampLow * 0.45f && o1.left.ampLow < o0.left.ampLow * 0.55f);
        CHECK(o2.left.ampLow == 0.0f && o2.left.ampHigh == 0.0f);
    }

    // ---- loop on another entity: keeps playing, follows the entity, stops ----
    {
        Fresh();
        SetName(3, "tank_rumble");
        cg_entitiesArray[0][5].pose.origin[0] = 700.0f;
        cgArray[0].refdef.vieworg[0] = 0.0f;
        CG_RumbleFrame(0);
        CG_RumbleEvent(0, EV_PLAY_RUMBLELOOP_ON_ENT, 3, 5, cg_entitiesArray[0][5].pose.origin);
        const float near1 = Peak(2.0f, 0.016f);
        CHECK(near1 > 0.05f && ActiveVoices() == 1);
        // Loops across several cycles: still sounding after 2 s.
        CHECK(Peak(0.4f, 0.016f) > 0.05f);
        // Entity drives away beyond the radius: silent but alive.
        cg_entitiesArray[0][5].pose.origin[0] = 5000.0f;
        CG_RumbleFrame(0);
        CHECK(Peak(0.5f, 0.016f) == 0.0f && ActiveVoices() == 1);
        // Comes back.
        cg_entitiesArray[0][5].pose.origin[0] = 300.0f;
        CG_RumbleFrame(0);
        CHECK(Peak(0.5f, 0.016f) > near1);
        // Stopping another entity's rumble leaves it; the right one ends it.
        CG_RumbleEvent(0, EV_STOP_RUMBLE, 3, 6, nullptr);
        CHECK(ActiveVoices() == 1);
        CG_RumbleEvent(0, EV_STOP_RUMBLE, 3, 5, nullptr);
        CHECK(ActiveVoices() == 0 && Peak(0.2f, 0.016f) == 0.0f);
    }

    // ---- stop all: script voices go, port one-shots stay ----
    {
        Fresh();
        SetName(1, "tank_rumble");
        SetName(2, "minigun_rumble");
        CG_RumbleEvent(0, EV_PLAY_RUMBLELOOP_ON_ENT, 1, 0, nullptr);
        CG_RumbleEvent(0, EV_PLAY_RUMBLELOOP_ON_ENT, 2, 0, nullptr);
        SwitchRumbleHd_Trigger(&s_player, SWITCH_RUMBLE_HD_MELEE, 1.0f, 0.0f);
        CHECK(ActiveVoices() == 3);
        CG_RumbleEvent(0, EV_STOP_ALL_RUMBLES, 0, 0, nullptr);
        CHECK(ActiveVoices() == 1 && s_player.voices[2].effect == SWITCH_RUMBLE_HD_MELEE);
    }

    // ---- mixing with port effects: one event is one effect ----
    {
        Fresh();
        SetName(1, "damage_heavy");
        SwitchRumbleHd_TriggerDamage(&s_player, 50, 0.0f); // port damage
        Peak(0.1f, 0.01f);
        CG_RumbleEvent(0, EV_PLAY_RUMBLE_ON_ENT, 1, 0, nullptr); // same event via script
        CHECK(s_player.started[SWITCH_RUMBLE_SRC_DAMAGE] == 1 && s_player.started[SWITCH_RUMBLE_SRC_SCRIPT] == 0 &&
              s_player.deduped == 1);
        Peak(0.5f, 0.01f); // window passes
        CG_RumbleEvent(0, EV_PLAY_RUMBLE_ON_ENT, 1, 0, nullptr);
        CHECK(s_player.started[SWITCH_RUMBLE_SRC_SCRIPT] == 1);

        // Reverse order: script first, then the port's own damage rumble.
        Fresh();
        SetName(1, "damage_light");
        CG_RumbleEvent(0, EV_PLAY_RUMBLE_ON_ENT, 1, 0, nullptr);
        Peak(0.1f, 0.01f);
        SwitchRumbleHd_TriggerDamage(&s_player, 30, 0.2f);
        CHECK(s_player.started[SWITCH_RUMBLE_SRC_SCRIPT] == 1 && s_player.started[SWITCH_RUMBLE_SRC_DAMAGE] == 0 &&
              s_player.deduped == 1);

        // A different category is never merged; a script blast and a port
        // explosion are, a loop is never dropped.
        Fresh();
        SetName(1, "grenade_rumble");
        SetName(2, "damage_heavy");
        SwitchRumbleHd_TriggerExplosion(&s_player, 100.0f, 400.0f);
        CG_RumbleEvent(0, EV_PLAY_RUMBLE_ON_ENT, 1, 0, nullptr);
        CHECK(s_player.deduped == 1);
        CG_RumbleEvent(0, EV_PLAY_RUMBLE_ON_ENT, 2, 0, nullptr); // damage vs explosion: different
        CHECK(s_player.started[SWITCH_RUMBLE_SRC_SCRIPT] == 1 && s_player.deduped == 1);
        CG_RumbleEvent(0, EV_PLAY_RUMBLELOOP_ON_ENT, 2, 0, nullptr);
        CHECK(s_player.started[SWITCH_RUMBLE_SRC_SCRIPT] == 2);

        // Summed output stays under the mix cap.
        SwitchRumbleHdOut o;
        SwitchRumbleHd_Advance(&s_player, 0.05f, &o);
        CHECK(o.left.ampLow <= SWITCH_RUMBLE_HD_MIX_CAP + 1e-6f && o.left.ampHigh <= SWITCH_RUMBLE_HD_MIX_CAP + 1e-6f);
    }

    // ---- unknown names: generic effect, reported once ----
    {
        Fresh();
        SetName(1, "mystery_rumble");
        s_printCount = 0;
        CG_RumbleEvent(0, EV_PLAY_RUMBLE_ON_ENT, 1, 0, nullptr);
        Peak(1.0f, 0.02f);
        CG_RumbleEvent(0, EV_PLAY_RUMBLE_ON_ENT, 1, 0, nullptr);
        CHECK(s_printCount == 1 && std::strstr(s_lastPrint, "mystery_rumble"));
        CHECK(s_player.started[SWITCH_RUMBLE_SRC_SCRIPT] == 2);
    }

    // ---- rumble_hd off: nothing plays, nothing crashes ----
    {
        Fresh();
        s_hdOn = false;
        SetName(1, "tank_rumble");
        CG_RumbleEvent(0, EV_PLAY_RUMBLELOOP_ON_ENT, 1, 0, nullptr);
        CG_RumbleFrame(0);
        CG_RumbleReload(0, SWITCH_RUMBLE_RELOAD_NORMAL, 2.0f);
        CG_StopAllRumbles(0);
        CHECK(ActiveVoices() == 0 && s_player.started[SWITCH_RUMBLE_SRC_SCRIPT] == 0);
        s_hdOn = true;
    }

    // ---- reload stages: delayed, subtle, high band ----
    {
        Fresh();
        CG_RumbleReload(0, SWITCH_RUMBLE_RELOAD_NORMAL, 2.0f);
        CHECK(s_player.started[SWITCH_RUMBLE_SRC_RELOAD] == 2);
        CHECK(Peak(0.55f, 0.01f) == 0.0f);      // before mag out (0.6 s)
        const float out = Peak(0.15f, 0.005f);   // mag out
        CHECK(out > 0.05f && out < 0.3f);
        SwitchRumbleHdOut o;
        // Mag out is pure high band.
        Fresh();
        CG_RumbleReload(0, SWITCH_RUMBLE_RELOAD_NORMAL, 2.0f);
        Peak(0.61f, 0.01f);
        SwitchRumbleHd_Advance(&s_player, 0.03f, &o);
        CHECK(o.left.ampLow <= 0.06f && o.left.ampHigh > 0.08f);
        // Whole reload (normal) never exceeds 0.35 per band and ends silent.
        Fresh();
        CG_RumbleReload(0, SWITCH_RUMBLE_RELOAD_NORMAL, 2.0f);
        CHECK(Peak(2.0f, 0.005f) < 0.35f && ActiveVoices() == 0);
        // From empty adds the chamber stage; start/end/rechamber are single ticks.
        Fresh();
        CG_RumbleReload(0, SWITCH_RUMBLE_RELOAD_EMPTY, 2.0f);
        CHECK(s_player.started[SWITCH_RUMBLE_SRC_RELOAD] == 3);
        Fresh();
        CG_RumbleReload(0, SWITCH_RUMBLE_RELOAD_START, 1.0f);
        CG_RumbleReload(0, SWITCH_RUMBLE_RELOAD_END, 1.0f);
        CG_RumbleReload(0, SWITCH_RUMBLE_RELOAD_RECHAMBER, 1.0f);
        CHECK(s_player.started[SWITCH_RUMBLE_SRC_RELOAD] == 3);
        // Non-local clients never rumble.
        Fresh();
        CG_RumbleReload(1, SWITCH_RUMBLE_RELOAD_NORMAL, 2.0f);
        CHECK(s_player.started[SWITCH_RUMBLE_SRC_RELOAD] == 0);
    }

    // ---- counter line ----
    {
        Fresh();
        SwitchRumbleHd_TriggerWeaponFire(&s_player, 4, 0);
        SwitchRumbleHd_Trigger(&s_player, SWITCH_RUMBLE_HD_LAND, 1.0f, 0.0f);
        SwitchRumbleHd_Trigger(&s_player, SWITCH_RUMBLE_HD_MELEE, 1.0f, 0.0f);
        SwitchRumbleHd_TriggerExplosion(&s_player, 10.0f, 100.0f);
        char line[256];
        SwitchRumbleHd_FormatCounters(&s_player, line, sizeof(line));
        CHECK(std::strcmp(line, "SWITCH_RUMBLE counters weapon=1 damage=0 explosion=1 land=1 melee=1 script=0 reload=0 deduped=0\n") == 0);
    }

    if (s_failures)
        return 1;
    std::printf("PASS:RUMBLE_SCRIPT names=%zu voices=%d\n", sizeof(kSwitchRumbleNames) / sizeof(kSwitchRumbleNames[0]),
                SWITCH_RUMBLE_HD_MAX_VOICES);
    return 0;
}
