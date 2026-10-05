#include "src/platform/switch/switch_input.h"

#include <stddef.h>

typedef struct
{
    uint32_t timestamp;
    int key;
    int pressed;
} InputEvent;

typedef struct
{
    InputEvent events[16];
    size_t count;
} InputEvents;

static void RecordEvent(void *context, uint32_t timestamp, int key, int pressed)
{
    InputEvents *events = (InputEvents *)context;

    if (events->count < sizeof(events->events) / sizeof(events->events[0]))
        events->events[events->count++] = (InputEvent){ timestamp, key, pressed };
}

static int CheckEvent(const InputEvents *events, size_t index, uint32_t timestamp, int key, int pressed)
{
    return index < events->count && events->events[index].timestamp == timestamp &&
        events->events[index].key == key && events->events[index].pressed == pressed;
}

int main(void)
{
    SwitchInputState state;
    InputEvents events = { 0 };

    Switch_InputInit(&state);
    Switch_InputTranslate(&state, SWITCH_INPUT_BUTTON_RIGHT | SWITCH_INPUT_BUTTON_A | SWITCH_INPUT_BUTTON_UP |
                          SWITCH_INPUT_BUTTON_LEFT | SWITCH_INPUT_BUTTON_B,
                          100, RecordEvent, &events);
    if (events.count != 5 ||
        !CheckEvent(&events, 0, 100, SWITCH_INPUT_K_UPARROW, 1) ||
        !CheckEvent(&events, 1, 100, SWITCH_INPUT_K_LEFTARROW, 1) ||
        !CheckEvent(&events, 2, 100, SWITCH_INPUT_K_RIGHTARROW, 1) ||
        !CheckEvent(&events, 3, 100, SWITCH_INPUT_K_ENTER, 1) ||
        !CheckEvent(&events, 4, 100, SWITCH_INPUT_K_ESCAPE, 1))
        return 1;

    Switch_InputTranslate(&state, SWITCH_INPUT_BUTTON_RIGHT | SWITCH_INPUT_BUTTON_A | SWITCH_INPUT_BUTTON_UP |
                          SWITCH_INPUT_BUTTON_LEFT | SWITCH_INPUT_BUTTON_B,
                          101, RecordEvent, &events);
    if (events.count != 5)
        return 1;

    Switch_InputTranslate(&state, SWITCH_INPUT_BUTTON_DOWN | SWITCH_INPUT_BUTTON_RIGHT,
                          250, RecordEvent, &events);
    if (events.count != 10 ||
        !CheckEvent(&events, 5, 250, SWITCH_INPUT_K_UPARROW, 0) ||
        !CheckEvent(&events, 6, 250, SWITCH_INPUT_K_DOWNARROW, 1) ||
        !CheckEvent(&events, 7, 250, SWITCH_INPUT_K_LEFTARROW, 0) ||
        !CheckEvent(&events, 8, 250, SWITCH_INPUT_K_ENTER, 0) ||
        !CheckEvent(&events, 9, 250, SWITCH_INPUT_K_ESCAPE, 0))
        return 1;

    Switch_InputTranslate(&state, 0, 251, RecordEvent, &events);
    if (events.count != 12 ||
        !CheckEvent(&events, 10, 251, SWITCH_INPUT_K_DOWNARROW, 0) ||
        !CheckEvent(&events, 11, 251, SWITCH_INPUT_K_RIGHTARROW, 0))
        return 1;

    {
        // P4 frame-order regression: Switch_InputFrame fills the gameplay
        // view (which stores the current button mask) before translating.
        // If the previous mask is not preserved across that fill, the edge
        // detection compares the current mask with itself and every event
        // is lost; direct Switch_InputTranslate calls alone cannot catch
        // it.  Drive the production frame body Switch_InputFrame calls.
        SwitchInputState frame;
        InputEvents frameEvents = { 0 };

        Switch_InputInit(&frame);

        Switch_InputFrameUpdate(&frame, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                SWITCH_INPUT_BUTTON_RIGHT | SWITCH_INPUT_BUTTON_A, 1,
                                300, RecordEvent, &frameEvents);
        if (frameEvents.count != 2 ||
            !CheckEvent(&frameEvents, 0, 300, SWITCH_INPUT_K_RIGHTARROW, 1) ||
            !CheckEvent(&frameEvents, 1, 300, SWITCH_INPUT_K_ENTER, 1))
            return 1;
        if (frame.buttons != (SWITCH_INPUT_BUTTON_RIGHT | SWITCH_INPUT_BUTTON_A) ||
            frame.previousButtons != frame.buttons || frame.connected != 1 ||
            !(frame.leftStick[1] > 0.0f))
            return 1;

        Switch_InputFrameUpdate(&frame, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                SWITCH_INPUT_BUTTON_RIGHT | SWITCH_INPUT_BUTTON_A, 1,
                                301, RecordEvent, &frameEvents);
        if (frameEvents.count != 2)
            return 1;

        Switch_InputFrameUpdate(&frame, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                0, 1, 302, RecordEvent, &frameEvents);
        if (frameEvents.count != 4 ||
            !CheckEvent(&frameEvents, 2, 302, SWITCH_INPUT_K_RIGHTARROW, 0) ||
            !CheckEvent(&frameEvents, 3, 302, SWITCH_INPUT_K_ENTER, 0))
            return 1;
    }

    {
        // P4: radial deadzone, saturation, axis signs, and
        // look inversion at the platform-input boundary.
        SwitchInputState gameplay;
        SwitchGameplayInput move;
        float outX;
        float outY;

        Switch_InputApplyStickDeadzone(0.0f, 0.0f, SWITCH_INPUT_STICK_DEADZONE, &outX, &outY);
        if (outX != 0.0f || outY != 0.0f)
            return 1;

        Switch_InputApplyStickDeadzone(0.10f, 0.10f, SWITCH_INPUT_STICK_DEADZONE, &outX, &outY);
        if (outX != 0.0f || outY != 0.0f)
            return 1;

        Switch_InputApplyStickDeadzone(0.0f, 0.5f, SWITCH_INPUT_STICK_DEADZONE, &outX, &outY);
        if (outX != 0.0f || !(outY > 0.0f && outY <= 1.0f))
            return 1;

        Switch_InputApplyStickDeadzone(0.3f, 0.0f, SWITCH_INPUT_STICK_DEADZONE, &outX, &outY);
        if (!(outX > 0.0f && outX <= 1.0f) || outY != 0.0f)
            return 1;

        Switch_InputApplyStickDeadzone(1.0f, 1.0f, SWITCH_INPUT_STICK_DEADZONE, &outX, &outY);
        if (outX * outX + outY * outY > 1.0001f)
            return 1;

        Switch_InputFillGameplay(&gameplay, 0.0f, 1.0f, 1.0f, 0.0f, 2.0f, -1.0f,
                                 SWITCH_INPUT_BUTTON_A | SWITCH_INPUT_BUTTON_X);
        if (gameplay.leftTrigger != 1.0f || gameplay.rightTrigger != 0.0f)
            return 1;

        Switch_InputBuildGameplay(&gameplay, 0, &move);
        if (!(move.forward > 0.0f) || move.right != 0.0f ||
            !(move.yaw > 0.0f) || move.pitch != 0.0f || move.up != 0.0f)
            return 1;
        if (move.buttons != (SWITCH_INPUT_BUTTON_A | SWITCH_INPUT_BUTTON_X))
            return 1;

        Switch_InputFillGameplay(&gameplay, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0);
        Switch_InputBuildGameplay(&gameplay, 0, &move);
        if (!(move.pitch > 0.0f))
            return 1;
        Switch_InputBuildGameplay(&gameplay, 1, &move);
        if (!(move.pitch < 0.0f))
            return 1;
    }

    Switch_InputShutdown(&state);
    {
        // Gameplay gating: while the client drives the player
        // (gameplayActive), A and B are usercmd buttons (jump/crouch) and
        // must not also arrive as ENTER/ESCAPE key events -- crouching used
        // to open the pause menu.  + stays the pause key (K_ESCAPE) in both
        // states.
        SwitchInputState gameState;
        InputEvents gameEvents;
        gameEvents.count = 0;
        Switch_InputInit(&gameState);
        gameState.gameplayActive = 1;
        Switch_InputTranslate(&gameState, SWITCH_INPUT_BUTTON_A | SWITCH_INPUT_BUTTON_B, 400,
                              RecordEvent, &gameEvents);
        if (gameEvents.count != 0)
            return 1;
        Switch_InputTranslate(&gameState, SWITCH_INPUT_BUTTON_PLUS, 401, RecordEvent, &gameEvents);
        if (gameEvents.count != 1 || !CheckEvent(&gameEvents, 0, 401, SWITCH_INPUT_K_ESCAPE, 1))
            return 1;
        Switch_InputTranslate(&gameState, 0, 402, RecordEvent, &gameEvents);
        if (gameEvents.count != 2 || !CheckEvent(&gameEvents, 1, 402, SWITCH_INPUT_K_ESCAPE, 0))
            return 1;
        // Back in a menu the same buttons translate again.
        gameState.gameplayActive = 0;
        Switch_InputTranslate(&gameState, SWITCH_INPUT_BUTTON_A, 403, RecordEvent, &gameEvents);
        if (gameEvents.count != 3 || !CheckEvent(&gameEvents, 2, 403, SWITCH_INPUT_K_ENTER, 1))
            return 1;
    }

    {
        // Hint/key-binding labels: the pad hardwires these commands, so
        // "Press [{+activate}] to pick up" must name the Switch button that
        // performs them (Y = BUTTON_USE_RELOAD), not a keyboard key.
        if (Switch_InputPadButtonForCommand("+activate") != SWITCH_INPUT_BUTTON_Y)
            return 1;
        if (Switch_InputPadButtonForCommand("+USERELOAD") != SWITCH_INPUT_BUTTON_Y)
            return 1;
        if (Switch_InputPadButtonForCommand("+attack") != SWITCH_INPUT_BUTTON_ZR)
            return 1;
        if (Switch_InputPadButtonForCommand("+frag") != SWITCH_INPUT_BUTTON_R)
            return 1;
        if (Switch_InputPadButtonForCommand("+holdbreath") != 0
            || Switch_InputPadButtonForCommand("+speed") != SWITCH_INPUT_BUTTON_ZL
            || Switch_InputPadButtonForCommand("+throw") != SWITCH_INPUT_BUTTON_R
            || Switch_InputPadButtonForCommand("+melee_breath") != 0
            || Switch_InputPadButtonForCommand(NULL) != 0)
            return 1;
        const char *name = Switch_InputPadButtonName(SWITCH_INPUT_BUTTON_Y);
        if (name == NULL || name[0] != 'Y' || name[1] != 0)
            return 1;
        if (Switch_InputPadButtonName(SWITCH_INPUT_BUTTON_Y | SWITCH_INPUT_BUTTON_X) != NULL)
            return 1;
    }

    return 0;
}
