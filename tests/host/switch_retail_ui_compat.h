#pragma once

// Host-only compatibility prelude for the narrow UI widening verifier.  Load
// libc before renaming CoD's random() declaration, matching switch_compat.h.
#include <stdlib.h>

#define __int16 short
#define __int32 int
#define __int64 long long
#define __forceinline inline __attribute__((always_inline))
#define random switch_random
#define crandom switch_crandom

typedef unsigned char byte;
// The narrow host proofs compile shared FS code before the normal platform
// headers establish this Win32 spelling.  A repeated typedef to the same
// underlying type is valid when the vendored d3d9-headers arrive later;
// a command-line BOOL macro is not, because it corrupts those typedefs.
typedef int BOOL;
