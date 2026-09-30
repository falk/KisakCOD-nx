#ifndef SWITCH_INPUT_LIFECYCLE_H
#define SWITCH_INPUT_LIFECYCLE_H

#include "switch_input.h"

#ifdef __cplusplus
extern "C" {
#endif

void IN_Frame(void);
void IN_ShowSystemCursor(int show);

/* Production gameplay pad state consumed by CL_GamepadMove.  The scripted
 * functions exist for the deterministic P4 usercmd proof and the P5 movement
 * proof: they replace the physical pad read for a bounded number of frames at
 * the same platform seam, so the engine path is unchanged. */
const SwitchInputState *Switch_GetInputState(void);
int Switch_InputPadActive(void);
void Switch_InputScriptPad(float leftX, float leftY, float rightX, float rightY,
                           uint64_t buttons, int frames);
void Switch_InputStopScript(void);

#ifdef __cplusplus
}
#endif

#endif
