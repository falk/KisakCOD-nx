// Forced pre-include (via g++ -include) for switch_script_parsetree_test.cpp's
// host build: the real libc random()/crandom() declarations need to be
// parsed and keep their real names *before* <database/database.h>'s own
// `random`/`crandom` symbols get macro-renamed for the original Win32
// compiler compat (the same trick switch_registry_size_proof.cpp gets via
// plain #include order -- applied here via -include instead, since
// scr_parsetree.cpp is production source and can't gain test-only host
// macros itself, but still needs the identical renaming when compiled for
// this host test).
#include <cstdlib>
#define random switch_random
#define crandom switch_crandom
