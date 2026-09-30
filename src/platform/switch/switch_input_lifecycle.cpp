#include "switch_input_lifecycle.h"

#include "switch_input.h"
#include <port/switch_quicksave.h>
#include "switch_sys_event.h"
#include <port/switch_gyro.h>
#include <port/switch_rumble.h>
#include <qcommon/sys_event.h>

namespace
{
SwitchInputState s_input;
bool s_input_initialized;

// Scripted pad replacement used by the deterministic usercmd/movement proofs.
// While s_scriptedFrames is positive, IN_Frame skips the physical pad read and
// Switch_GetInputState reports this state instead, so no synthetic menu key
// events leak into the event queue.
SwitchInputState s_scripted_input;
int s_scripted_frames;

void QueueKeyEvent(void *, uint32_t timestamp, int key, int pressed)
{
    Sys_QueEvent(timestamp, SE_KEY, key, pressed, 0, nullptr);
}
}

// cl_input.cpp: true while the local player is being driven with no UI key
// catcher up, i.e. while A/B are usercmd buttons rather than menu keys.
extern "C" int Switch_ClientGameplayActive(void);

extern "C" void IN_Frame(void)
{
    if (!s_input_initialized)
    {
        Switch_InputInit(&s_input);
        s_input_initialized = true;
    }
    s_input.gameplayActive = Switch_ClientGameplayActive();
    if (s_scripted_frames > 0)
    {
        --s_scripted_frames;
        // The scripted pad still drives the quicksave shortcut, so a proof can
        // press MINUS exactly where a player would.
        Switch_QuickSavePadEdge(s_scripted_input.buttons);
        return;
    }
    Switch_InputFrame(&s_input, QueueKeyEvent, nullptr);
    // Menu state included: the shortcut must fire while the pause menu owns
    // input too (that is the load half), and CL_GamepadMove is skipped there.
    Switch_QuickSavePadEdge(s_input.buttons);
    // Gyro sample + rumble mix/gate/send, once per engine frame regardless
    // of menu/pause/gameplay state (both read their own gates internally:
    // gyro's view-angle application is gated in CL_GamepadMove, rumble's
    // vibration send is gated inside Switch_RumbleFrame).
    Switch_GyroFrame();
    Switch_RumbleFrame();
}

extern "C" void IN_ShowSystemCursor(int show)
{
    // Switch has controller navigation and no desktop system cursor.
    (void)show;
}

extern "C" const SwitchInputState *Switch_GetInputState(void)
{
    return s_scripted_frames > 0 ? &s_scripted_input : &s_input;
}

extern "C" int Switch_InputPadActive(void)
{
    return s_scripted_frames > 0 || s_input.connected;
}

extern "C" void Switch_InputScriptPad(float leftX, float leftY, float rightX, float rightY,
                                      uint64_t buttons, int frames)
{
    Switch_InputFillGameplay(&s_scripted_input, leftX, leftY, rightX, rightY,
                             0.0f, 0.0f, buttons);
    s_scripted_input.connected = 1;
    s_scripted_frames = frames > 0 ? frames : 0;
}

extern "C" void Switch_InputStopScript(void)
{
    s_scripted_frames = 0;
}

// No desktop window manager on Horizon: there is exactly one foreground
// "window" (this application) and no OS mouse capture to activate/release.
extern "C" void IN_ActivateMouse(int force)
{
    (void)force;
}

extern "C" void IN_SetForegroundWindow()
{
}

extern "C" bool IN_IsForegroundWindow()
{
    return true;
}
