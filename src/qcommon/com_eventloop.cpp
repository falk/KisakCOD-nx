#include "sys_event.h"
#include "com_error.h"

#include "../universal/assertive.h"
#include "../universal/profile.h"

#include <cstdint>

void __cdecl CL_KeyEvent(int32_t localClientNum, int32_t key, int32_t down, uint32_t time);
void __cdecl CL_CharEvent(int32_t localClientNum, int32_t key);
void __cdecl Cbuf_AddText(int localClientNum, const char *text);
void __cdecl Com_FreeEvent(char *ptr);
#ifdef KISAK_MP
void __cdecl Com_ClientPacketEvent();
void __cdecl Com_ServerPacketEvent();
#endif

void __cdecl Com_EventLoop()
{
    sysEvent_t result; // [esp+4h] [ebp-48h] BYREF
    sysEvent_t ev; // [esp+34h] [ebp-18h]

    PROF_SCOPED("Com_EventLoop");

    while (1)
    {
        ev = *Sys_GetEvent(&result);

        switch (ev.evType)
        {
        case SE_NONE:
        {
            iassert(!ev.evPtr);
#ifdef KISAK_MP
            Com_ClientPacketEvent();
            Com_ServerPacketEvent();
#endif
            goto END;
        }
        case SE_KEY:
        {
            iassert(!ev.evPtr);
            CL_KeyEvent(0, ev.evValue, ev.evValue2, ev.evTime);
            break;
        }
        case SE_CHAR:
        {
            iassert(!ev.evPtr);
            CL_CharEvent(0, ev.evValue);
            break;
        }
        case SE_CONSOLE:
        {
            iassert(ev.evPtr);
            Cbuf_AddText(0, (const char *)ev.evPtr);
            Com_FreeEvent((char *)ev.evPtr);
            Cbuf_AddText(0, "\n");
            break;
        }

        default:
            iassert(!ev.evPtr);
            Com_Error(ERR_FATAL, "Com_EventLoop: bad event type %i", ev.evType);
            break;
        }
    }

END:
    return;
}
