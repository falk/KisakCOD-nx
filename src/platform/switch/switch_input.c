#include "switch_input.h"

#include "switch_platform.h"
extern uint32_t Sys_Milliseconds(void);

typedef struct
{
    uint64_t button;
    int key;
    /* Menu-only: no key event while state->gameplayActive (the button is a
     * usercmd gameplay button then). */
    int menuOnly;
} SwitchInputBinding;

static const SwitchInputBinding switch_input_menu_bindings[] =
{
    { SWITCH_INPUT_BUTTON_UP, SWITCH_INPUT_K_UPARROW, 1 },
    { SWITCH_INPUT_BUTTON_DOWN, SWITCH_INPUT_K_DOWNARROW, 1 },
    { SWITCH_INPUT_BUTTON_LEFT, SWITCH_INPUT_K_LEFTARROW, 1 },
    { SWITCH_INPUT_BUTTON_RIGHT, SWITCH_INPUT_K_RIGHTARROW, 1 },
    { SWITCH_INPUT_BUTTON_A, SWITCH_INPUT_K_ENTER, 1 },
    { SWITCH_INPUT_BUTTON_B, SWITCH_INPUT_K_ESCAPE, 1 },
    /* + is the in-game pause key (the same K_ESCAPE the retail client
     * opens the ingame menu with); it stays live in menus too. */
    { SWITCH_INPUT_BUTTON_PLUS, SWITCH_INPUT_K_ESCAPE, 0 }
};

static float Switch_InputClampUnit(float value)
{
    if (value > 1.0f)
        return 1.0f;
    if (value < -1.0f)
        return -1.0f;
    return value;
}

/* Newton-Raphson square root so the platform seam stays free of libm; the
 * host seam links switch_input.c with no -lm.  Twenty fixed iterations are
 * more than enough for float and keep the result deterministic. */
static float Switch_InputSquareRoot(float value)
{
    float guess;
    float next;
    int i;

    if (value <= 0.0f)
        return 0.0f;

    guess = value;
    if (guess < 1.0f)
        guess = 1.0f;
    for (i = 0; i < 20; ++i)
    {
        next = 0.5f * (guess + value / guess);
        if (next == guess)
            break;
        guess = next;
    }
    return guess;
}

void Switch_InputApplyStickDeadzone(float x, float y, float deadzone, float *outX, float *outY)
{
    float magnitude;
    float scale;

    if (outX == NULL || outY == NULL)
        return;

    if (deadzone < 0.0f)
        deadzone = 0.0f;
    if (deadzone > 1.0f)
        deadzone = 1.0f;

    magnitude = Switch_InputSquareRoot(x * x + y * y);
    if (magnitude <= deadzone || magnitude <= 0.0f)
    {
        *outX = 0.0f;
        *outY = 0.0f;
        return;
    }

    /* Rescale the remaining magnitude to [0,1] but preserve direction. */
    scale = (magnitude - deadzone) / (1.0f - deadzone);
    if (scale > 1.0f)
        scale = 1.0f;

    *outX = Switch_InputClampUnit((x / magnitude) * scale);
    *outY = Switch_InputClampUnit((y / magnitude) * scale);
}

void Switch_InputFillGameplay(SwitchInputState *state, float leftX, float leftY,
                              float rightX, float rightY, float leftTrigger,
                              float rightTrigger, uint64_t buttons)
{
    if (state == NULL)
        return;

    state->rawLeftStick[0] = leftX;
    state->rawLeftStick[1] = leftY;
    state->rawRightStick[0] = rightX;
    state->rawRightStick[1] = rightY;
    Switch_InputApplyStickDeadzone(leftX, leftY, SWITCH_INPUT_STICK_DEADZONE,
                                   &state->leftStick[0], &state->leftStick[1]);
    Switch_InputApplyStickDeadzone(rightX, rightY, SWITCH_INPUT_LOOK_STICK_DEADZONE,
                                   &state->rightStick[0], &state->rightStick[1]);
    state->leftTrigger = leftTrigger < 0.0f ? 0.0f : (leftTrigger > 1.0f ? 1.0f : leftTrigger);
    state->rightTrigger = rightTrigger < 0.0f ? 0.0f : (rightTrigger > 1.0f ? 1.0f : rightTrigger);
    state->buttons = buttons;
}

void Switch_InputBuildGameplay(const SwitchInputState *state, int invertPitch,
                               SwitchGameplayInput *out)
{
    if (out == NULL)
        return;

    out->forward = 0.0f;
    out->right = 0.0f;
    out->up = 0.0f;
    out->yaw = 0.0f;
    out->pitch = 0.0f;
    out->buttons = 0;

    if (state == NULL)
        return;

    out->forward = state->leftStick[1];   /* +y = forward */
    out->right = state->leftStick[0];     /* +x = right */
    out->yaw = state->rightStick[0];      /* +x = turn right */
    out->pitch = state->rightStick[1];    /* +y = look up */
    if (invertPitch)
        out->pitch = -out->pitch;
    out->buttons = state->buttons;
}

void Switch_InputInit(SwitchInputState *state)
{
    if (state == NULL)
        return;

    state->buttons = 0;
    state->previousButtons = 0;
    state->previousMenuButtons = 0;
    state->repeatButton = 0;
    state->repeatNextMs = 0;
    state->leftStick[0] = 0.0f;
    state->leftStick[1] = 0.0f;
    state->rightStick[0] = 0.0f;
    state->rightStick[1] = 0.0f;
    state->rawLeftStick[0] = 0.0f;
    state->rawLeftStick[1] = 0.0f;
    state->rawRightStick[0] = 0.0f;
    state->rawRightStick[1] = 0.0f;
    state->leftTrigger = 0.0f;
    state->rightTrigger = 0.0f;
    state->connected = 0;
    state->gameplayActive = 0;
#if defined(__SWITCH__)
    // Required before padInitializeDefault: without an assigned Npad ID /
    // style set, padUpdate reads no controller and every button stays 0, so
    // the menu renders but never receives input.  Matches the hardware-proven
    // bootstrap preflight (switch_sp_bootstrap.cpp), which configures one
    // standard Npad before initializing its pad.
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&state->pad);
#endif
}

void Switch_InputShutdown(SwitchInputState *state)
{
    if (state == NULL)
        return;

    state->buttons = 0;
    state->previousButtons = 0;
    state->leftStick[0] = 0.0f;
    state->leftStick[1] = 0.0f;
    state->rightStick[0] = 0.0f;
    state->rightStick[1] = 0.0f;
    state->rawLeftStick[0] = 0.0f;
    state->rawLeftStick[1] = 0.0f;
    state->rawRightStick[0] = 0.0f;
    state->rawRightStick[1] = 0.0f;
    state->leftTrigger = 0.0f;
    state->rightTrigger = 0.0f;
    state->connected = 0;
    state->gameplayActive = 0;
#if defined(__SWITCH__)
    /* PadState has no libnx destruction function. */
    state->pad = (PadState){ 0 };
#endif
}

/* d-pad bits the left stick is currently holding: the dominant axis only,
 * with engage/release hysteresis against the bits it held last frame. */
static uint64_t Switch_InputStickDirections(const SwitchInputState *state)
{
    const uint64_t dirMask = SWITCH_INPUT_BUTTON_UP | SWITCH_INPUT_BUTTON_DOWN |
                             SWITCH_INPUT_BUTTON_LEFT | SWITCH_INPUT_BUTTON_RIGHT;
    float x = state->leftStick[0];
    float y = state->leftStick[1];
    float ax = x < 0.0f ? -x : x;
    float ay = y < 0.0f ? -y : y;
    uint64_t held = 0;
    uint64_t previous = state->previousMenuButtons & dirMask;
    float major = ax > ay ? ax : ay;
    uint64_t candidate;

    if (ax > ay)
        candidate = x < 0.0f ? SWITCH_INPUT_BUTTON_LEFT : SWITCH_INPUT_BUTTON_RIGHT;
    else
        candidate = y < 0.0f ? SWITCH_INPUT_BUTTON_DOWN : SWITCH_INPUT_BUTTON_UP;

    if (major >= SWITCH_INPUT_MENU_STICK_ENGAGE ||
        (major >= SWITCH_INPUT_MENU_STICK_RELEASE && (previous & candidate) != 0))
        held = candidate;
    return held;
}

void Switch_InputTranslate(SwitchInputState *state, uint64_t buttons, uint32_t timestamp,
                           SwitchInputEventSink sink, void *context)
{
    size_t index;
    uint64_t menuButtons;
    const uint64_t dirMask = SWITCH_INPUT_BUTTON_UP | SWITCH_INPUT_BUTTON_DOWN |
                             SWITCH_INPUT_BUTTON_LEFT | SWITCH_INPUT_BUTTON_RIGHT;

    if (state == NULL)
        return;

    menuButtons = buttons;
    if (!state->gameplayActive)
        menuButtons |= Switch_InputStickDirections(state);

    if (sink != NULL)
    {
        for (index = 0; index < sizeof(switch_input_menu_bindings) / sizeof(switch_input_menu_bindings[0]); ++index)
        {
            const SwitchInputBinding *binding = &switch_input_menu_bindings[index];
            int was_pressed = (state->previousMenuButtons & binding->button) != 0;
            int is_pressed = (menuButtons & binding->button) != 0;

            if (binding->menuOnly && state->gameplayActive)
                continue;
            if (was_pressed != is_pressed)
            {
                sink(context, timestamp, binding->key, is_pressed);
                if (is_pressed && (binding->button & dirMask) != 0)
                {
                    state->repeatButton = binding->button;
                    state->repeatNextMs = timestamp + SWITCH_INPUT_MENU_REPEAT_DELAY_MS;
                }
            }
        }

        if (state->repeatButton != 0)
        {
            if (state->gameplayActive || (menuButtons & state->repeatButton) == 0)
            {
                state->repeatButton = 0;
            }
            else if ((int32_t)(timestamp - state->repeatNextMs) >= 0)
            {
                for (index = 0; index < sizeof(switch_input_menu_bindings) / sizeof(switch_input_menu_bindings[0]); ++index)
                {
                    if (switch_input_menu_bindings[index].button == state->repeatButton)
                    {
                        sink(context, timestamp, switch_input_menu_bindings[index].key, 1);
                        break;
                    }
                }
                state->repeatNextMs = timestamp + SWITCH_INPUT_MENU_REPEAT_INTERVAL_MS;
            }
        }
    }
    else
    {
        state->repeatButton = 0;
    }
    state->buttons = buttons;
    state->previousButtons = buttons;
    state->previousMenuButtons = menuButtons;
}

void Switch_InputFrameUpdate(SwitchInputState *state, float leftX, float leftY,
                             float rightX, float rightY, float leftTrigger,
                             float rightTrigger, uint64_t buttons, int connected,
                             uint32_t timestamp, SwitchInputEventSink sink,
                             void *context)
{
    if (state == NULL)
        return;

    /* The previous mask must stay intact until the transition compare;
     * FillGameplay writes the current gameplay mask into state->buttons,
     * so translate against previousButtons and commit both. */
    Switch_InputFillGameplay(state, leftX, leftY, rightX, rightY,
                             leftTrigger, rightTrigger, buttons);
    state->connected = connected;
    Switch_InputTranslate(state, buttons, timestamp, sink, context);
}

void Switch_InputFrame(SwitchInputState *state, SwitchInputEventSink sink, void *context)
{
#if defined(__SWITCH__)
    HidAnalogStickState left;
    HidAnalogStickState right;
    uint64_t buttons;

    if (state == NULL)
        return;

    padUpdate(&state->pad);
    buttons = padGetButtons(&state->pad);
    left = padGetStickPos(&state->pad, 0);
    right = padGetStickPos(&state->pad, 1);

    Switch_InputFrameUpdate(state,
                            (float)left.x / 32767.0f, (float)left.y / 32767.0f,
                            (float)right.x / 32767.0f, (float)right.y / 32767.0f,
                            (buttons & SWITCH_INPUT_BUTTON_ZL) ? 1.0f : 0.0f,
                            (buttons & SWITCH_INPUT_BUTTON_ZR) ? 1.0f : 0.0f,
                            buttons,
                            padIsConnected(&state->pad) ? 1 : 0,
                            Sys_Milliseconds(), sink, context);
#else
    (void)state;
    (void)sink;
    (void)context;
#endif
}

/* Keep in step with the gameplay block of CL_SwitchPadMove (cl_input.cpp):
 * only commands whose effect that button really reproduces are listed
 * (e.g. +melee_breath is not: R stick sets only the melee bit). */
static const struct
{
    const char *command;
    uint64_t button;
} s_switchPadCommands[] = {
    { "+attack", SWITCH_INPUT_BUTTON_ZR },
    { "+speed", SWITCH_INPUT_BUTTON_ZL },
    { "+speed_throw", SWITCH_INPUT_BUTTON_ZL },
    { "+toggleads_throw", SWITCH_INPUT_BUTTON_ZL },
    { "toggleads", SWITCH_INPUT_BUTTON_ZL },
    { "+gostand", SWITCH_INPUT_BUTTON_A },
    { "+moveup", SWITCH_INPUT_BUTTON_A },
    { "+stance", SWITCH_INPUT_BUTTON_B },
    { "gocrouch", SWITCH_INPUT_BUTTON_B },
    { "togglecrouch", SWITCH_INPUT_BUTTON_B },
    { "+movedown", SWITCH_INPUT_BUTTON_B },
    /* Y sets BUTTON_USE_RELOAD: use when a use target/cursor hint is up,
     * reload otherwise -- so it is the pickup/activate and reload button. */
    { "+usereload", SWITCH_INPUT_BUTTON_Y },
    { "+activate", SWITCH_INPUT_BUTTON_Y },
    { "+reload", SWITCH_INPUT_BUTTON_Y },
    { "weapnext", SWITCH_INPUT_BUTTON_X },
    { "+frag", SWITCH_INPUT_BUTTON_R },
    { "+throw", SWITCH_INPUT_BUTTON_R },
    { "+smoke", SWITCH_INPUT_BUTTON_L },
    { "+melee", SWITCH_INPUT_BUTTON_STICKR },
    { "+sprint", SWITCH_INPUT_BUTTON_STICKL },
    { "+breath_sprint", SWITCH_INPUT_BUTTON_STICKL },
    { "+actionslot 1", SWITCH_INPUT_BUTTON_UP },
    { "+actionslot 2", SWITCH_INPUT_BUTTON_DOWN },
    { "+actionslot 3", SWITCH_INPUT_BUTTON_LEFT },
    { "+actionslot 4", SWITCH_INPUT_BUTTON_RIGHT },
    { "+scores", SWITCH_INPUT_BUTTON_MINUS },
    { "togglemenu", SWITCH_INPUT_BUTTON_PLUS },
};

static int Switch_InputCommandEqual(const char *a, const char *b)
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

uint64_t Switch_InputPadButtonForCommand(const char *command)
{
    size_t i;

    if (command == NULL)
        return 0;
    for (i = 0; i < sizeof(s_switchPadCommands) / sizeof(s_switchPadCommands[0]); ++i)
    {
        if (Switch_InputCommandEqual(command, s_switchPadCommands[i].command))
            return s_switchPadCommands[i].button;
    }
    return 0;
}

const char *Switch_InputPadButtonName(uint64_t button)
{
    switch (button)
    {
    case SWITCH_INPUT_BUTTON_A: return "A";
    case SWITCH_INPUT_BUTTON_B: return "B";
    case SWITCH_INPUT_BUTTON_X: return "X";
    case SWITCH_INPUT_BUTTON_Y: return "Y";
    case SWITCH_INPUT_BUTTON_STICKL: return "L Stick";
    case SWITCH_INPUT_BUTTON_STICKR: return "R Stick";
    case SWITCH_INPUT_BUTTON_L: return "L";
    case SWITCH_INPUT_BUTTON_R: return "R";
    case SWITCH_INPUT_BUTTON_ZL: return "ZL";
    case SWITCH_INPUT_BUTTON_ZR: return "ZR";
    case SWITCH_INPUT_BUTTON_PLUS: return "+";
    case SWITCH_INPUT_BUTTON_MINUS: return "-";
    case SWITCH_INPUT_BUTTON_LEFT: return "Left";
    case SWITCH_INPUT_BUTTON_UP: return "Up";
    case SWITCH_INPUT_BUTTON_RIGHT: return "Right";
    case SWITCH_INPUT_BUTTON_DOWN: return "Down";
    default: return NULL;
    }
}

const char *Switch_InputPadBindingName(const char *command)
{
    if (command == NULL)
        return NULL;
    /* Movement is an axis, not a digital button or a keyboard assignment. */
    if (Switch_InputCommandEqual(command, "+forward")
        || Switch_InputCommandEqual(command, "+back")
        || Switch_InputCommandEqual(command, "+moveleft")
        || Switch_InputCommandEqual(command, "+moveright"))
        return "L Stick";
    if (Switch_InputCommandEqual(command, "+left")
        || Switch_InputCommandEqual(command, "+right")
        || Switch_InputCommandEqual(command, "+lookup")
        || Switch_InputCommandEqual(command, "+lookdown"))
        return "R Stick";
    return Switch_InputPadButtonName(Switch_InputPadButtonForCommand(command));
}
