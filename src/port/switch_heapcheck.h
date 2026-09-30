#pragma once

// Diagnostic heap checker; linked only with KISAK_SWITCH_HEAP_CHECK=ON
// (see switch_heapcheck.cpp).  Starts the thread that scans redzones and
// prints FAIL:HEAPCHK_* findings.
void HeapCheck_Start();
