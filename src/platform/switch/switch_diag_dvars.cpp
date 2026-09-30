#include "switch_diag_dvars.h"

#include <qcommon/qcommon.h>

const dvar_t *com_hardware = nullptr;
const dvar_t *com_diagMarkers = nullptr;

void RetailKillhouseRegisterPortDvars()
{
    com_hardware = Dvar_RegisterBool(
        "com_hardware", false, DVAR_NOFLAG,
        "Physical Switch hardware run (no emulator-only workarounds)");
    // The port's verifier markers (KILLHOUSE_*, SMC_*) go out through
    // Com_Printf, which has a channel argument but no level -- nothing routes
    // through PRINT_DEVELOPER -- so they were unconditional. A single walk emits tens of thousands
    // of lines, mostly from a few per-frame sites, and every line is an
    // svcOutputDebugString: harmless in an emulator, ruinous on hardware. Default off; the automation turns it on.
    com_diagMarkers = Dvar_RegisterBool(
        "com_diagMarkers", false, DVAR_NOFLAG,
        "Emit the port's high-volume per-frame diagnostic markers");
}
