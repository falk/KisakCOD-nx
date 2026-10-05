#ifndef SWITCH_PAD_LAYOUT_H
#define SWITCH_PAD_LAYOUT_H

// Pure, host-testable pieces of the retail console pad layout: the stance
// tap/hold machine, the L3 sprint-vs-breath decision, d-pad action slots and
// the Minus tap/hold split.  No engine dependency; cl_input.cpp and
// switch_quicksave.cpp feed it the frame's buttons and run what it returns.

#include <stdint.h>

#define SWITCH_PAD_BUTTON_STICKL UINT64_C(0x0000000000000010)
#define SWITCH_PAD_BUTTON_B      UINT64_C(0x0000000000000002)
#define SWITCH_PAD_BUTTON_LEFT   UINT64_C(0x0000000000001000)
#define SWITCH_PAD_BUTTON_UP     UINT64_C(0x0000000000002000)
#define SWITCH_PAD_BUTTON_RIGHT  UINT64_C(0x0000000000004000)
#define SWITCH_PAD_BUTTON_DOWN   UINT64_C(0x0000000000008000)

// Stance machine shared with the engine's +stance/-stance handlers.  C is
// clientActive_t or any struct with the same four fields; stance values are
// 0 stand, 1 crouch, 2 prone.
template <class C>
inline void PadStance_Down(C &c, int now)
{
    c.stanceHeld = 1;
    c.stancePosition = c.stance;
    c.stanceTime = now;
    if (c.stance != 1)
        c.stance = static_cast<decltype(c.stance)>(1);
}

template <class C>
inline void PadStance_Up(C &c)
{
    if (c.stanceHeld && c.stancePosition == 1)
        c.stance = static_cast<decltype(c.stance)>(0);
    c.stanceHeld = 0;
}

template <class C>
inline void PadStance_HoldUpdate(C &c, int now, int holdTime)
{
    if (c.stanceHeld && now - c.stanceTime >= holdTime)
    {
        c.stance = static_cast<decltype(c.stance)>(c.stancePosition == 2 ? 0 : 2);
        c.stanceHeld = 0;
    }
}

// L3 is the retail +breath_sprint: scoped (ADS) it steadies the aim, otherwise
// it sprints.  Holding both would sprint-cancel the ADS.
struct PadL3Decision
{
    bool sprint;
    bool breath;
};

inline PadL3Decision PadL3Resolve(bool l3Held, bool ads)
{
    PadL3Decision d;
    d.sprint = l3Held && !ads;
    d.breath = l3Held && ads;
    return d;
}

struct PadThrowDecision
{
    bool frag;
    bool throwSelected;
};

// Selected detonator grenades use the throw bit, not the offhand-frag bit.
// ZL keeps the retail aim/throw combination; R can also throw the selected C4.
inline PadThrowDecision PadThrowResolve(bool zlHeld, bool rHeld, bool selectedDetonator)
{
    return {rHeld && !selectedDetonator, selectedDetonator && (zlHeld || rHeld)};
}

// Edge-triggered console commands: B is +stance/-stance, the d-pad is
// +/-actionslot 1..4 (up, down, left, right).  Releases are always reported so
// a hold that began in gameplay cannot stay latched when a menu opens.
struct PadCommandOut
{
    int count;
    const char *cmds[12];
};

struct PadCommandState
{
    uint64_t prev;
};

inline void PadCommandStep(PadCommandState *st, uint64_t buttons, bool gameplay, PadCommandOut *out)
{
    static const struct
    {
        uint64_t button;
        const char *down;
        const char *up;
    } map[] = {
        { SWITCH_PAD_BUTTON_B, "+stance", "-stance" },
        { SWITCH_PAD_BUTTON_UP, "+actionslot 1", "-actionslot 1" },
        { SWITCH_PAD_BUTTON_DOWN, "+actionslot 2", "-actionslot 2" },
        { SWITCH_PAD_BUTTON_LEFT, "+actionslot 3", "-actionslot 3" },
        { SWITCH_PAD_BUTTON_RIGHT, "+actionslot 4", "-actionslot 4" },
    };
    const uint64_t pressed = buttons & ~st->prev;
    const uint64_t released = ~buttons & st->prev;
    st->prev = buttons;
    out->count = 0;
    for (const auto &m : map)
    {
        if (released & m.button)
            out->cmds[out->count++] = m.up;
        else if ((pressed & m.button) && gameplay)
            out->cmds[out->count++] = m.down;
    }
}

// Minus: a tap saves (load while a menu owns input), a hold shows the
// objectives (+scores).
struct PadMinusState
{
    bool down;
    bool fromGameplay;
    bool scoresShown;
    uint32_t downMs;
};

struct PadMinusOut
{
    bool scoresDown;
    bool scoresUp;
    bool tapSave;
    bool menuPress;
};

inline PadMinusOut PadMinusStep(PadMinusState *st, bool minusDown, bool gameplay, uint32_t nowMs,
                                uint32_t holdMs)
{
    PadMinusOut o = {};
    if (minusDown && !st->down)
    {
        st->down = true;
        st->fromGameplay = gameplay;
        st->downMs = nowMs;
        o.menuPress = !gameplay;
    }
    else if (!minusDown && st->down)
    {
        st->down = false;
        if (st->scoresShown)
        {
            st->scoresShown = false;
            o.scoresUp = true;
        }
        else
        {
            o.tapSave = st->fromGameplay && gameplay;
        }
    }
    else if (st->down)
    {
        if (st->scoresShown && !gameplay)
        {
            st->scoresShown = false;
            o.scoresUp = true;
        }
        else if (!st->scoresShown && st->fromGameplay && gameplay && nowMs - st->downMs >= holdMs)
        {
            st->scoresShown = true;
            o.scoresDown = true;
        }
    }
    return o;
}

#endif
