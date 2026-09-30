#include <universal/q_shared.h>
#include "r_setstate_d3d.h"
#include "r_init.h"
#include <deko9/deko9_native.h>


bool __cdecl RB_IsGpuFinished()
{
    // deko9 native frame pacing: the GPU-sync fence is a frame id
    // (R_InsertGpuFence), not dx.flushGpuQuery's event query.
    if (!Deko9_FrameDone(dx.device, dx.gpuSyncFrame))
        return 0;
    --dx.flushGpuQueryCount;
    return 1;
}

bool __cdecl RB_IsGpuFenceFinished()
{
    if (dx.flushGpuQueryIssued)
    {
    	// KISAKGPUFENCE: Comment asserts out for now. Sometimes goes off when alt-tabbing.
        //if (dx.flushGpuQueryCount != 1)
        //    MyAssertHandler(".\\r_setstate_d3d.cpp", 48, 0, "%s", "dx.flushGpuQueryCount == 1");
        if (RB_IsGpuFinished())
        {
            dx.flushGpuQueryIssued = 0;
            // KISAKGPUFENCE: Comment asserts out for now. Sometimes goes off when alt-tabbing.
            //if (dx.flushGpuQueryCount)
            //    MyAssertHandler(".\\r_setstate_d3d.cpp", 54, 0, "%s", "!dx.flushGpuQueryCount");
            return 1;
        }
        else
        {
            return 0;
        }
    }
    else
    {
    	// KISAKGPUFENCE: Comment asserts out for now. Sometimes goes off when alt-tabbing.
        //if (dx.flushGpuQueryCount)
        //    MyAssertHandler(".\\r_setstate_d3d.cpp", 44, 0, "%s", "!dx.flushGpuQueryCount");
        return 1;
    }
}