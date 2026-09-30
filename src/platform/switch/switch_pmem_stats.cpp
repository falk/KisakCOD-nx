// PMem_DumpMemStats lives outside switch_pmem.cpp on purpose: it is the one
// PMem_* function that calls into the wider engine (Com_Printf/ConvertToMB),
// and switch_pmem.cpp's host/Switch pmem test harness compiles that file
// standalone with no -I./src include path.  Keeping this dependency in its
// own translation unit, added only to the normal SP target's Switch
// sources, avoids adding a qcommon dependency to that narrow proof build.
// Otherwise a verbatim port of the original engine's stats dump
// (physicalmemory.cpp): portable already, since it only reads g_mem and
// prints through Com_Printf.
#include "switch_pmem.h"

#ifdef __SWITCH__
#include <qcommon/mem_track.h>
#include <qcommon/qcommon.h>

void __cdecl PMem_DumpMemStats()
{
    uint32_t i;
    uint32_t top;
    uint32_t bottom;

    for (i = 0; i < g_mem.prim[1].allocListCount; ++i)
    {
        if (i == g_mem.prim[1].allocListCount - 1)
            bottom = g_mem.prim[1].pos;
        else
            bottom = g_mem.prim[1].allocList[i + 1].pos;
        double sizeMb = ConvertToMB(g_mem.prim[1].allocList[i].pos - bottom);
        Com_Printf(16, "%-18.18s %5.1f\n", g_mem.prim[1].allocList[i].name, sizeMb);
    }
    double freeMb = ConvertToMB(PMem_GetFreeAmount());
    Com_Printf(16, "free physical      %5.1f\n", freeMb);
    top = g_mem.prim[0].pos;
    for (int32_t j = (int32_t)g_mem.prim[0].allocListCount - 1; j >= 0; --j)
    {
        double sizeMb = ConvertToMB(top - g_mem.prim[0].allocList[j].pos);
        Com_Printf(16, "%-18.18s %5.1f\n", g_mem.prim[0].allocList[j].name, sizeMb);
        top = g_mem.prim[0].allocList[j].pos;
    }
    Com_Printf(16, "------------------------\n");
}
#endif
