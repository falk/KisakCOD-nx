// Forced pre-include (via g++ -include) for switch_script_yacc_test.cpp's
// host build: the real libc random()/crandom() declarations need to be
// parsed and keep their real names *before* <database/database.h>'s own
// `random`/`crandom` symbols get macro-renamed (the same trick
// switch_script_parsetree_test_preamble.h uses), plus a host-compatible
// LPVOID typedef.  scr_yacc.h needs LPVOID but skips <Windows.h> under
// __SWITCH__; the normal SP build gets it from switch_compat.h (which needs
// <switch.h> and cannot compile on host), so provide the same single type
// here.  Duplicate typedefs of the identical type later (windows_base.h,
// snd_public.h) remain legal.
#include <cstdlib>
#define random switch_random
#define crandom switch_crandom
typedef void *LPVOID;
