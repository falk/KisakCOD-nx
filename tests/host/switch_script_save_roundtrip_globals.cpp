// Link-only storage for the debugger globals the save path names.
//
// scr_readwrite.cpp's Scr_AddDebuggerRefs/Scr_RemoveDebuggerRefs touch
// scrDebuggerGlob.scriptWatch, but both bodies are `if (scrVarPub.developer)`
// guarded and the round-trip harness never enables the developer flag.  The
// debugger UI itself (scr_debugger.cpp, and the UI component classes its
// globals are built from) is deliberately not linked, so the storage is
// declared as raw bytes rather than the real type: constructing the real type
// would pull in the vtables of UI_ScrollPane/Scr_ScriptList/Scr_ScriptWatch
// and their whole class hierarchy, none of which the save/load path reaches.
//
// Do not include scr_debugger.h here: its `extern scrDebuggerGlob_t` would
// disagree with the storage below.  The size covers the decompiler's 0x2B8
// plus headroom for LP64 pointer growth in the classes it contains, and
// nothing reads or writes it.  A future harness that turns the developer flag
// on must link the real debugger instead of pointing at this storage.

alignas(16) unsigned char scrDebuggerGlob[0x1000];
