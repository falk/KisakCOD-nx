// Retail console pad layout: stance tap/hold, L3 sprint-vs-breath, d-pad
// action slots, Minus tap/hold.
#include <cstdio>
#include <cstring>

#include "../../src/port/switch_pad_layout.h"

static int s_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++s_fail; } } while (0)

enum { STAND = 0, CROUCH = 1, PRONE = 2 };
struct Client
{
    int stance;
    bool stanceHeld;
    int stancePosition;
    int stanceTime;
};

static const int kHold = 300;

static void StanceFrames(Client &c, int &now, int ms)
{
    // 50 ms frames: the engine's per-usercmd CL_StanceButtonUpdate.
    for (int t = 0; t < ms; t += 50)
    {
        now += 50;
        PadStance_HoldUpdate(c, now, kHold);
    }
}

static void TestStance()
{
    Client c = {};
    int now = 1000;

    // Tap from stand: crouch, and release keeps crouch.
    PadStance_Down(c, now);
    CHECK(c.stance == CROUCH);
    StanceFrames(c, now, 100);
    PadStance_Up(c);
    CHECK(c.stance == CROUCH);

    // Tap from crouch: stand on release.
    PadStance_Down(c, now);
    CHECK(c.stance == CROUCH);
    StanceFrames(c, now, 100);
    PadStance_Up(c);
    CHECK(c.stance == STAND);

    // Hold from stand: crouch, then prone after the hold time; release keeps prone.
    PadStance_Down(c, now);
    StanceFrames(c, now, kHold - 50);
    CHECK(c.stance == CROUCH);
    StanceFrames(c, now, 100);
    CHECK(c.stance == PRONE);
    PadStance_Up(c);
    CHECK(c.stance == PRONE);

    // Tap from prone: crouch, release stays crouch (position was prone).
    PadStance_Down(c, now);
    CHECK(c.stance == CROUCH);
    StanceFrames(c, now, 100);
    PadStance_Up(c);
    CHECK(c.stance == CROUCH);

    // Hold from prone stands up (hold toggles prone off).
    c.stance = PRONE;
    PadStance_Down(c, now);
    StanceFrames(c, now, kHold + 50);
    CHECK(c.stance == STAND);
    PadStance_Up(c);
    CHECK(c.stance == STAND);

    // Hold fires once.
    Client d = {};
    PadStance_Down(d, now);
    StanceFrames(d, now, kHold + 50);
    CHECK(d.stance == PRONE && !d.stanceHeld);
    StanceFrames(d, now, 1000);
    CHECK(d.stance == PRONE);
}

static void TestL3()
{
    CHECK(PadL3Resolve(true, false).sprint && !PadL3Resolve(true, false).breath);
    CHECK(!PadL3Resolve(true, true).sprint && PadL3Resolve(true, true).breath);
    CHECK(!PadL3Resolve(false, true).sprint && !PadL3Resolve(false, true).breath);
    CHECK(!PadL3Resolve(false, false).sprint && !PadL3Resolve(false, false).breath);
}

static void TestCommands()
{
    PadCommandState st = {};
    PadCommandOut o;

    PadCommandStep(&st, SWITCH_PAD_BUTTON_UP, true, &o);
    CHECK(o.count == 1 && !std::strcmp(o.cmds[0], "+actionslot 1"));
    // Held: no repeat.
    PadCommandStep(&st, SWITCH_PAD_BUTTON_UP, true, &o);
    CHECK(o.count == 0);
    PadCommandStep(&st, 0, true, &o);
    CHECK(o.count == 1 && !std::strcmp(o.cmds[0], "-actionslot 1"));

    const uint64_t pads[4] = { SWITCH_PAD_BUTTON_UP, SWITCH_PAD_BUTTON_DOWN, SWITCH_PAD_BUTTON_LEFT,
                               SWITCH_PAD_BUTTON_RIGHT };
    const char *down[4] = { "+actionslot 1", "+actionslot 2", "+actionslot 3", "+actionslot 4" };
    for (int i = 0; i < 4; ++i)
    {
        PadCommandStep(&st, pads[i], true, &o);
        CHECK(o.count == 1 && !std::strcmp(o.cmds[0], down[i]));
        PadCommandStep(&st, 0, true, &o);
        CHECK(o.count == 1 && o.cmds[0][0] == '-');
    }

    // B press/release maps to +stance/-stance.
    PadCommandStep(&st, SWITCH_PAD_BUTTON_B, true, &o);
    CHECK(o.count == 1 && !std::strcmp(o.cmds[0], "+stance"));
    // A menu opening mid-hold still releases.
    PadCommandStep(&st, 0, false, &o);
    CHECK(o.count == 1 && !std::strcmp(o.cmds[0], "-stance"));

    // Presses outside gameplay do nothing, and no stray release follows.
    PadCommandStep(&st, SWITCH_PAD_BUTTON_B | SWITCH_PAD_BUTTON_UP, false, &o);
    CHECK(o.count == 0);

    // Two edges in one frame.
    PadCommandStep(&st, 0, true, &o);
    PadCommandStep(&st, SWITCH_PAD_BUTTON_B | SWITCH_PAD_BUTTON_LEFT, true, &o);
    CHECK(o.count == 2);
}

static void TestMinus()
{
    PadMinusState m = {};
    PadMinusOut o;

    // Tap in gameplay saves, no objectives.
    o = PadMinusStep(&m, true, true, 1000, 250);
    CHECK(!o.tapSave && !o.menuPress && !o.scoresDown);
    o = PadMinusStep(&m, false, true, 1100, 250);
    CHECK(o.tapSave && !o.scoresUp);

    // Hold shows objectives once, release hides, no save.
    o = PadMinusStep(&m, true, true, 2000, 250);
    o = PadMinusStep(&m, true, true, 2200, 250);
    CHECK(!o.scoresDown);
    o = PadMinusStep(&m, true, true, 2250, 250);
    CHECK(o.scoresDown);
    o = PadMinusStep(&m, true, true, 2400, 250);
    CHECK(!o.scoresDown);
    o = PadMinusStep(&m, false, true, 2500, 250);
    CHECK(o.scoresUp && !o.tapSave);

    // Menu press loads immediately; release saves nothing.
    o = PadMinusStep(&m, true, false, 3000, 250);
    CHECK(o.menuPress);
    o = PadMinusStep(&m, false, false, 3050, 250);
    CHECK(!o.tapSave && !o.scoresUp);

    // Pressed in a menu, released in gameplay: no save.
    o = PadMinusStep(&m, true, false, 4000, 250);
    o = PadMinusStep(&m, false, true, 4050, 250);
    CHECK(!o.tapSave);

    // Menu opens while objectives are up: hidden.
    o = PadMinusStep(&m, true, true, 5000, 250);
    o = PadMinusStep(&m, true, true, 5300, 250);
    CHECK(o.scoresDown);
    o = PadMinusStep(&m, true, false, 5400, 250);
    CHECK(o.scoresUp);
}

int main()
{
    TestStance();
    TestL3();
    TestCommands();
    TestMinus();
    if (s_fail)
        return 1;
    std::printf("PASS:PAD_LAYOUT\n");
    return 0;
}
