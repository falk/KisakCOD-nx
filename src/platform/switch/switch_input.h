#ifndef SWITCH_INPUT_H
#define SWITCH_INPUT_H

#include <stdint.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

enum
{
    SWITCH_INPUT_K_ENTER = 0x0D,
    SWITCH_INPUT_K_ESCAPE = 0x1B,
    SWITCH_INPUT_K_UPARROW = 0x9A,
    SWITCH_INPUT_K_DOWNARROW = 0x9B,
    SWITCH_INPUT_K_LEFTARROW = 0x9C,
    SWITCH_INPUT_K_RIGHTARROW = 0x9D
};

/* Button masks mirror libnx HidNpadButton_* so the same values can be
 * compared against padGetButtons() without a translation table. */
#define SWITCH_INPUT_BUTTON_A UINT64_C(0x0000000000000001)
#define SWITCH_INPUT_BUTTON_B UINT64_C(0x0000000000000002)
#define SWITCH_INPUT_BUTTON_X UINT64_C(0x0000000000000004)
#define SWITCH_INPUT_BUTTON_Y UINT64_C(0x0000000000000008)
#define SWITCH_INPUT_BUTTON_STICKL UINT64_C(0x0000000000000010)
#define SWITCH_INPUT_BUTTON_STICKR UINT64_C(0x0000000000000020)
#define SWITCH_INPUT_BUTTON_L UINT64_C(0x0000000000000040)
#define SWITCH_INPUT_BUTTON_R UINT64_C(0x0000000000000080)
#define SWITCH_INPUT_BUTTON_ZL UINT64_C(0x0000000000000100)
#define SWITCH_INPUT_BUTTON_ZR UINT64_C(0x0000000000000200)
#define SWITCH_INPUT_BUTTON_PLUS UINT64_C(0x0000000000000400)
#define SWITCH_INPUT_BUTTON_MINUS UINT64_C(0x0000000000000800)
#define SWITCH_INPUT_BUTTON_LEFT UINT64_C(0x0000000000001000)
#define SWITCH_INPUT_BUTTON_UP UINT64_C(0x0000000000002000)
#define SWITCH_INPUT_BUTTON_RIGHT UINT64_C(0x0000000000004000)
#define SWITCH_INPUT_BUTTON_DOWN UINT64_C(0x0000000000008000)

/* P4: the radial stick deadzone.  This is a magnitude
 * deadzone, not a per-axis one: a diagonal deflection just past the circle
 * keeps its direction instead of snapping to an axis.  The remaining
 * magnitude is rescaled to the full [0,1] range so full deflection still
 * reaches 127 units of usercmd movement. */
#define SWITCH_INPUT_STICK_DEADZONE 0.20f
/* The look (right) stick gets a smaller deadzone than movement: 0.28 made fine
 * aim impossible (nothing until 28% deflection, then a linear ramp), which is
 * the reported "hard to aim".  Keep it above the test's 0.10/0.10 diagonal
 * (magnitude 0.1414) so a resting stick still reads zero. */
#define SWITCH_INPUT_LOOK_STICK_DEADZONE 0.15f
#define SWITCH_INPUT_AXIS_COUNT 4
/* Gameplay buttons wired to existing SP usercmd buttons by CL_GamepadMove
 * (the retail console layout): ZR -> fire, ZL -> aim down sights, A -> jump,
 * B -> crouch (held), X -> use/reload, Y -> next weapon, R -> frag,
 * L -> special grenade, right stick click -> melee, left stick click ->
 * sprint.  Digital menu bindings stay in the Switch_InputTranslate path;
 * the A/B menu keys are suppressed while gameplayActive is set (see
 * SwitchInputState) so crouching does not also open the pause menu, which
 * is + (K_ESCAPE) in gameplay. */
#define SWITCH_INPUT_GAMEPLAY_BUTTON_COUNT 10

typedef void (*SwitchInputEventSink)(void *context, uint32_t timestamp, int key, int pressed);

typedef struct
{
    /* Current frame's gameplay button mask (Switch_InputFillGameplay). */
    uint64_t buttons;
    /* Last mask committed by Switch_InputTranslate.  The original engine
     * keeps its previous mask in a separate s_wmv.oldButtonState and queues
     * SE_KEY only on the XOR (win_input.cpp IN_MouseEvent), because the
     * current mask is also the gameplay state; edge detection must compare
     * against this field rather than `buttons`, which the frame's gameplay
     * fill overwrites before translation. */
    uint64_t previousButtons;
    /* Normalized, deadzoned stick axes in [-1,1]: [0] = x, [1] = y. */
    float leftStick[2];
    float rightStick[2];
    /* Same frame's normalized axes before the deadzone/rescale, kept only
     * for diagnostics (e.g. CL_GamepadMove's INPUT_DIAG line) so a hardware
     * calibration/orientation problem can be told apart from a bug in the
     * deadzone or gameplay mapping. */
    float rawLeftStick[2];
    float rawRightStick[2];
    /* Normalized analog triggers in [0,1]. */
    float leftTrigger;
    float rightTrigger;
    int connected;
    /* Set by the client while the local player is being driven (no UI key
     * catcher): the face buttons then belong to the usercmd, and the
     * menu-only key translations (A -> ENTER, B -> ESCAPE) are withheld. */
    int gameplayActive;
#if defined(__SWITCH__)
    PadState pad;
#endif
} SwitchInputState;

/* Pure, host-testable gameplay view of a pad frame.  forward/right/up are
 * the usercmd movement directions; yaw/pitch are the view-angle deltas
 * (pitch already carries the invert-look sign). */
typedef struct
{
    float forward;
    float right;
    float up;
    float yaw;
    float pitch;
    uint64_t buttons;
} SwitchGameplayInput;

#ifdef __cplusplus
extern "C" {
#endif
void Switch_InputInit(SwitchInputState *state);
void Switch_InputShutdown(SwitchInputState *state);
void Switch_InputTranslate(SwitchInputState *state, uint64_t buttons, uint32_t timestamp,
                            SwitchInputEventSink sink, void *context);
void Switch_InputFrame(SwitchInputState *state, SwitchInputEventSink sink, void *context);
/* Pad-independent body of one input frame: apply this sample's sticks,
 * triggers, buttons, and connection state, then translate the button
 * transitions against the previous translated mask.  Switch_InputFrame
 * reads the physical pad and forwards here; the host seam drives this
 * directly so the production frame order is covered without libnx. */
void Switch_InputFrameUpdate(SwitchInputState *state, float leftX, float leftY,
                             float rightX, float rightY, float leftTrigger,
                             float rightTrigger, uint64_t buttons, int connected,
                             uint32_t timestamp, SwitchInputEventSink sink,
                             void *context);

void Switch_InputApplyStickDeadzone(float x, float y, float deadzone, float *outX, float *outY);
void Switch_InputFillGameplay(SwitchInputState *state, float leftX, float leftY,
                              float rightX, float rightY, float leftTrigger,
                              float rightTrigger, uint64_t buttons);
void Switch_InputBuildGameplay(const SwitchInputState *state, int invertPitch,
                               SwitchGameplayInput *out);

/* Gameplay commands the pad performs without a key binding (CL_SwitchPadMove
 * in cl_input.cpp sets their usercmd bits or runs them straight from the
 * button).  The binding table never names these buttons, so every "Press
 * [{+usereload}] to ..." string would otherwise show the keyboard default
 * (F, R, ...) or KEY_UNBOUND.  Returns the SWITCH_INPUT_BUTTON_* bit that
 * performs COMMAND (case-insensitive), or 0 when the pad has no fixed button
 * for it and the binding table stays authoritative. */
uint64_t Switch_InputPadButtonForCommand(const char *command);
/* Printable label of one SWITCH_INPUT_BUTTON_* bit ("Y", "ZR", "R Stick"),
 * or NULL for an unknown/multi-bit value. */
const char *Switch_InputPadButtonName(uint64_t button);
#ifdef __cplusplus
}
#endif

#endif
