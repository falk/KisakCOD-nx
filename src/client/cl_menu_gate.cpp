#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

// Exact original menu-path client gate moved from cl_main.cpp: the render-skip
// flag the UI key path consults and the controller-index mapping used by menu
// input and UI expressions.  They remain the only owner of these semantics on
// Switch and desktop.
#include <universal/q_shared.h>
#include "client.h"

int __cdecl CL_ControllerIndexFromClientNum(int clientIndex)
{
    if (clientIndex)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\client\\cl_main.cpp",
            230,
            0,
            "clientIndex doesn't index STATIC_MAX_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            clientIndex,
            1);
    return cl_controller_in_use;
}

static bool cl_skipRendering;
void __cdecl CL_SetSkipRendering(bool skip)
{
    cl_skipRendering = skip;
}

bool __cdecl CL_SkipRendering()
{
    return cl_skipRendering;
}
