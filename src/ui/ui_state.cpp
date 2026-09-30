#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include "ui.h"

// Runtime UI state is independent of UI startup.  Keeping it separate lets
// the input/menu closure use the same state object without dragging in the
// save/profile/cgame startup implementation from ui_main.cpp.
uiInfo_s uiInfo;
sharedUiInfo_t sharedUiInfo;
SaveTimeGlob ui_saveTimeGlob;
