// Sys_SnapVector was win_shared.cpp-owned (excluded on Switch); its body is
// already portable (SnapFloat is a plain banker's-rounding inline in
// qcommon.h), so this is a straight port, not a stub.  A separate .cpp
// (rather than switch_platform.c) because SnapFloat needs C++ and qcommon.h.
#ifdef __SWITCH__
#include <qcommon/qcommon.h>

extern "C" void __cdecl Sys_SnapVector(float *v)
{
    v[0] = SnapFloat(v[0]);
    v[1] = SnapFloat(v[1]);
    v[2] = SnapFloat(v[2]);
}
#endif
