#pragma once

#ifndef __cdecl
#define __cdecl
#endif
#ifndef QDECL
#define QDECL __cdecl
#endif
#if defined(__GNUC__) && !defined(__int32)
#define __int32 int
#endif

enum errorParm_t : __int32
{                                       // ...
    ERR_FATAL = 0x0,
    ERR_DROP = 0x1,
    ERR_SERVERDISCONNECT = 0x2,
    ERR_DISCONNECT = 0x3,
    ERR_SCRIPT = 0x4,
    ERR_SCRIPT_DROP = 0x5,
    ERR_LOCALIZATION = 0x6,
    ERR_MAPLOADERRORSUMMARY = 0x7,
};

void QDECL Com_Error(errorParm_t code, const char* fmt, ...);
