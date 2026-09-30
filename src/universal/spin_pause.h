#pragma once

#include <stdint.h>

// Body for the retail busy-wait loops that wait on another thread's progress
// (cull-state PENDING, DObj/FX spin locks, ordered queue publishes).  Win32
// could spin there: its scheduler time-slices and boosts starved threads.
// Horizon does not -- a thread spinning on core N starves every lower-priority
// thread on core N, and every port thread shares the main thread's core at a
// lower priority.  With the renderer workers on, main spun on a scene entity
// the idle server thread had claimed on core 0 and cargoship froze.
// Call with the loop's iteration count: the first iterations are a CPU yield
// hint (cheap when the owner runs on another core), later ones sleep so a
// same-core owner gets scheduled.
#ifdef __SWITCH__
void Sys_SpinPause(uint32_t spin);
#else
inline void Sys_SpinPause(uint32_t) {}
#endif
