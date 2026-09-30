// Compile-time contract probe: plain `char` must be signed for Switch engine
// translation units.
//
// The decompiled retail sources and wire structs (usercmd_s.forwardmove /
// rightmove / upmove, playerState movementDir, the msg delta readers) are
// plain `char` and rely on the Windows/x86 signed-char baseline.  AArch64 GCC
// defaults to unsigned char, so a negative left-stick value became 128..255
// and moved the player forward/right instead of back/left.  cmake/switch.cmake
// puts -fsigned-char on every Switch target and q_shared.h carries the
// matching #error guard; ./test host compiles this file both ways and
// requires the signed build to pass and the unsigned build to fail.
#include <universal/q_shared.h>

int main()
{
    return 0;
}
