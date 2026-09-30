// Host regression harness for the REAL COD4 script parser + compiler + VM
// over a tiny synthetic .gsc, built for the host with the same high-address
// HunkUser and ASan/UBSan recipe as the yacc/parsetree script tests.
//
// Why this exists: the LP64 script-compiler defects fixed during
// level-load and movement work were only ever reproduced on the Switch
// (slow iterations).  This harness drives the shipped ScriptParse +
// ScriptCompile + VM_Execute path in-process so the same class of bug
// iterates in seconds.
//
// The specific regression it pins is SpecifyThreadPosition's handling of a
// not-yet-emitted function position.  The synthetic file `selftest.gsc`
// defines two local functions and has `main` make a FAR self-reference to a
// function defined later in the same file:
//
//     main()            { selftest::target(); }
//     target()          { record(42); }
//
// `selftest::target` is parsed as a far function (ENUM_far_function) and
// compiled while `target`'s position variable still holds its
// "not emitted yet" value.  With `pos.u.intValue = 0` on LP64 only the low
// four bytes of the pointer-width union were cleared, so EmitFunction read
// the high four bytes as a garbage code position and emitted it verbatim
// instead of recording an unresolved link.  The fix clears the full
// pos.u.codePosValue.
//
// Checks:
//   1. real ScriptParse/ScriptCompile compile the file with no CompileError;
//   2. a direct probe of SpecifyThreadPosition proves it clears the whole
//      pointer-width position union (deterministic; this is what fails when
//      the fix is reverted);
//   3. the compiled far-call operand is a real code position inside
//      [programBuffer, programBuffer + programLen) and equals `target`'s
//      position, and executing `main` through the real VM reaches `target` and
//      its stubbed builtin;
//   4. `param6(a..f)` (six formal parameters) is called through the same
//      VM_Execute entry with exactly six arguments and records all six in
//      order.  Before the VM_Execute startTop `[-(int)paramcount]` fix this
//      crashed in VM_Execute on LP64, which is the production shape behind
//      animscripts/scripted.gsc:45 init() aborting with "function called with
//      too many parameters".
//
// Check 2 is deliberately separate from check 3: the bug's damage on the full
// path needs the reused position slot to already hold a nonzero high half,
// which is true in the game after earlier scripts but not guaranteed on a
// pristine host process.  The full path therefore proves the real seam works,
// while the direct probe makes the regression assertion deterministic.
//
// Everything the compile/execute path does not legitimately need -- the
// filesystem, database, debugger UI, XAnim, Dvar, profiling -- is a loud
// stub below, never silent production behavior.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/com_error.h>
#include <qcommon/threads.h>
#include <qcommon/cmd.h>
#include <qcommon/mem_track.h>
#include <universal/q_parse.h>
#include <universal/com_math.h>
#include <universal/assertive.h>
#include <universal/critical_section.h>
#include <universal/com_memory.h>
#include <universal/profile.h>
#include <universal/physicalmemory.h>
#include <universal/com_files.h>
#include <platform/switch/switch_hunk_user.h>
#include <database/database.h>
#include <gfx_d3d/r_dvars.h>
#include <script/scr_vm.h>
#include <script/scr_variable.h>
#include <script/scr_stringlist.h>
#include <script/scr_main.h>
#include <script/scr_compiler.h>
#include <script/scr_parser.h>
#include <script/scr_parsetree.h>
#include <script/scr_debugger.h>
#include <script/scr_evaluate.h>
#include <script/scr_animtree.h>
#include <script/scr_readwrite.h>
#include <client/client.h> // SaveHeader
#include <xanim/xanim.h>
#include <xanim/xmodel.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// ---------------------------------------------------------------------------
// Deliberately excluded engine services.  None of these is on the
// parse/compile/execute path for the synthetic script; they are referenced
// only because the production script TUs mention them.  Each fails loudly
// (abort) rather than silently substituting behavior, so a future path that
// unexpectedly reaches one cannot pass as the script under test.
// ---------------------------------------------------------------------------

scrAnimPub_t scrAnimPub;
ProfileScript profileScript;
fileData_s *com_fileDataHashTable[1024];

void Com_Error(errorParm_t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP excluded Com_Error reached: ");
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
    std::abort();
}

void MyAssertHandler(const char *file, int line, int type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP assert %s:%d type=%d: ", file, line, type);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
    // A bare file:line from the decompiled tree is not enough to find the
    // caller (the ported line numbers differ from the retail ones), so print
    // the stack; resolve it with `addr2line -e <binary> -f -C`.
    void *frames[24];
    const int count = backtrace(frames, static_cast<int>(sizeof(frames) / sizeof(frames[0])));
    backtrace_symbols_fd(frames, count, 2);
    std::abort();
}

void Sys_Error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP excluded Sys_Error reached: ");
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
    std::abort();
}

void Sys_OutOfMemErrorInternal(const char *filename, int line)
{
    std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP out of memory at %s:%d\n", filename, line);
    std::abort();
}

// Channel 23 is the script channel: the save path's cyclic-leak diagnostic,
// the script error reporting and Scr_PrintPrevCodePos all use it.  Channel 0
// is the terminal-error path.  A failure here has to name its object and code
// position to be actionable, so both are printed; everything else is silent.
void Com_Printf(int channel, const char *fmt, ...)
{
    if (channel != 23 && channel != 0 && channel != 24)
        return;
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stdout, "[print ch%02d] ", channel);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
}
void Com_PrintError(int channel, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stdout, "[error ch%02d] ", channel);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
}
void Com_PrintMessage(int, const char *, int) {}
void ProfLoad_Begin(const char *) {}
void ProfLoad_End() {}
void Profile_BeginScript(int) {}
void Profile_BeginScripts(uint32_t) {}
void Profile_EndScript(int) {}
void Profile_EndScripts(uint32_t) {}
ProfileScript *Profile_GetScript() { return nullptr; }
void PMem_DumpMemStats() {}
void track_PrintInfo() {}
void track_set_hunk_size(int) {}
void track_static_alloc_internal(void *, int, const char *, int) {}
void R_ReflectionProbeRegisterDvars() {}

int Sys_IsRemoteDebugClient() { return 0; }
bool Sys_IsMainThread() { return true; }
void *Sys_GetValue(int) { return nullptr; }
void Sys_EnterCriticalSection(int) {}
void Sys_LeaveCriticalSection(int) {}
int32_t CL_GetLocalClientActiveCount() { return 1; }

void Cmd_AddCommandInternal(const char *, void (*)(), cmd_function_s *) {}

int Com_sprintf(char *dest, uint32_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int written = std::vsnprintf(dest, size, fmt, ap);
    va_end(ap);
    return written;
}

void Com_Memcpy(void *dest, const void *src, size_t count) { std::memcpy(dest, src, count); }
void Com_Memset(void *dest, int val, size_t count) { std::memset(dest, val, count); }

bool I_iscsym(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
int I_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
const char *I_stristr(const char *s0, const char *substr)
{
    if (!*substr)
        return s0;
    for (; *s0; ++s0)
    {
        const char *h = s0;
        const char *n = substr;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n))
        {
            ++h;
            ++n;
        }
        if (!*n)
            return s0;
    }
    return nullptr;
}
void I_strncpyz(char *dest, const char *src, int destsize)
{
    if (destsize <= 0)
        return;
    std::strncpy(dest, src, static_cast<size_t>(destsize));
    dest[destsize - 1] = '\0';
}
float Q_rint(float in) { return static_cast<float>(static_cast<int>(in + (in < 0.0f ? -0.5f : 0.5f))); }

char *va(const char *fmt, ...)
{
    static char buffer[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    return buffer;
}

parseInfo_t *Com_Parse(const char **data_p) { (void)data_p; return nullptr; }
void Com_BeginParseSession(const char *) {}
void Com_EndParseSession() {}

void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}

// Filesystem / database: the harness never calls FS_ReadFile or
// DB_FindXAssetHeader; the source buffer is handed to ScriptParse directly.
void FS_FCloseFile(int) {}
uint32_t FS_FOpenFileByMode(char *, int *, fsMode_t) { return 0; }
uint32_t FS_FOpenFileRead(const char *, int *) { return 0; }
void FS_FreeFileList(const char **) {}
int FS_HashFileName(const char *, int) { return 0; }
int FS_LoadStack() { return 0; }
const char **FS_ListFiles(const char *, const char *, FsListBehavior_e, int *numfiles)
{
    if (numfiles)
        *numfiles = 0;
    return nullptr;
}
uint32_t FS_Read(uint8_t *, uint32_t, int) { return 0; }
XAssetHeader DB_FindXAssetHeader(XAssetType, const char *)
{
    XAssetHeader header;
    std::memset(&header, 0, sizeof(header));
    return header;
}
const dvar_s *Dvar_RegisterBool(const char *, bool value, uint16_t, const char *)
{
    static dvar_s storage;
    storage.current.enabled = value;
    return &storage;
}

// The script system's compile/execute path queries the fastfile flag and the
// reflection-probe dvar through the shared hunk initializer.  Point them at
// real (disabled) dvar storage so the excluded dvar subsystem is never
// dereferenced as null; no dvar registration path is exercised.
static dvar_s s_useFastFile;
static dvar_s s_reflectionProbeGenerate;
const dvar_t *useFastFile = &s_useFastFile;
const dvar_s *fs_gameDirVar = nullptr;
const dvar_t *r_reflectionProbeGenerate = &s_reflectionProbeGenerate;

// Hunk core hooks.  The compile path's temp-memory allocator (scr_block_s and
// the source buffer) lives on the production global hunk.  Thread checks pass
// (single-threaded harness) and any hunk-integrity failure is fatal, never
// silently accepted.
extern "C" void Switch_HunkCoreFatal(const char *message)
{
    std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP hunk fatal: %s\n", message);
    std::abort();
}
extern "C" void Switch_HunkCoreDrop(const char *message)
{
    std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP hunk drop: %s\n", message);
    std::abort();
}
extern "C" void Switch_HunkCoreOutOfMemory(const char *message)
{
    std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP hunk out of memory: %s\n", message);
    std::abort();
}
extern "C" int Switch_HunkCoreThreadCheck(int) { return 1; }
extern "C" void Switch_HunkCoreTrack(const char *, int, const char *, int) {}

// Debugger / profiling / anim surfaces reached only through the production
// script TUs' no-op hooks.  These must never be hit on the compile/execute
// path; abort if they are.
//
// scr_vm.cpp's Switch-specific Scr_InitSystem calls Scr_InitDebuggerBootState
// (the debugger UI itself is deliberately excluded from this harness), so the
// stub is intentionally inert.
void Scr_InitDebuggerBootState() {}
void Scr_InitDebuggerMain() {}
void Scr_InitDebugger() {}
void Scr_ShutdownDebugger() {}
void Scr_ShutdownDebuggerMain() {}
void Scr_ShutdownDebuggerSystem(int) {}
void Scr_UpdateDebugger() {}
void Scr_DebugKillThread(uint32_t, const char *) {}
void Scr_DebugTerminateThread(int) {}
void Scr_ShowConsole() {}
void Scr_AddAssignmentPos(char *) {}
bool Scr_IgnoreErrors() { return true; }
int Scr_HitBreakpoint(VariableValue *, char *, uint32_t, int) { return 0; }
int Scr_HitAssignmentBreakpoint(VariableValue *, char *, uint32_t, int) { return 0; }
void Scr_HitBuiltinBreakpoint(VariableValue *, const char *, uint32_t, int, int, uint32_t) {}
void Scr_CheckBreakonNotify(uint32_t, uint32_t, VariableValue *, char *, uint32_t) {}
void Scr_EmitAnimation(char *, uint32_t, uint32_t) {}
XAnim_s *Scr_GetAnims(uint32_t) { return nullptr; }
void Scr_LoadAnimTreeAtIndex(uint32_t, void *(*)(int), int) {}
void Scr_UsingTree(const char *, uint32_t) {}
void Scr_GetObjectField(unsigned int, unsigned int, unsigned int) {}
int Scr_SetObjectField(unsigned int, unsigned int, int) { return 0; }
bool Scr_RefToVariable(uint32_t, int)
{
    std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP excluded debugger Scr_RefToVariable reached\n");
    std::abort();
}
XAnim_s *XAnimGetAnims(const XAnimTree_s *) { return nullptr; }
char *XAnimGetAnimDebugName(const XAnim_s *, uint32_t) { return const_cast<char *>(""); }
const char *XAnimGetAnimTreeDebugName(const XAnim_s *) { return ""; }
void XAnimFreeList(XAnim_s *) {}
void XAnimFree(XAnimParts *) {}
void XModelPartsFree(XModelPartsLoad *) {}

// The save path's remaining cross-TU needs.  None of them is reached by the
// save/load round trip: ReadFromDevice/FS_Write serve the direct-to-file
// source trailer, the thread queries are memfile's stream-mode bookkeeping,
// and the two Scr_ScriptWatch members are the debugger hooks behind
// `if (scrVarPub.developer)` (never set here).  They are non-virtual members
// of the debugger's UI line component, so defining them does not emit that
// class's vtables -- which is why the real scr_debugger.cpp/ui_shared.cpp are
// not linked into this harness.
int ReadFromDevice(void *, int, void *) { return 0; }
unsigned int FS_Write(const char *, unsigned int, int) { return 0; }
bool Sys_IsRenderThread() { return false; }
bool Sys_IsDatabaseThread() { return false; }
void Scr_InitDebuggerSystem() {}
void Scr_ScriptWatch::Evaluate() {}
void Scr_ScriptWatch::UpdateBreakpoints(bool add) { (void)add; }

// ---------------------------------------------------------------------------
// Minimal builtin table (the same contract switch_script_compile_test.cpp
// uses): ScriptCompile resolves a call through the real Scr_GetFunction and
// stores the returned function's index in scrCompilePub.func_table, so the
// only observable side effect the test needs is a builtin that records a value.
// ---------------------------------------------------------------------------

static int g_recorded = -1;
static int g_recordCount = 0;

static void RecordBuiltin()
{
    g_recorded = Scr_GetInt(0);
    ++g_recordCount;
}

void (*Scr_GetFunction(const char **pName, int *type))()
{
    if (std::strcmp(*pName, "record") == 0)
    {
        *type = 0;
        return RecordBuiltin;
    }
    return nullptr;
}

void (*Scr_GetMethod(const char **pName, int *type))(scr_entref_t)
{
    (void)pName;
    *type = 0;
    return nullptr;
}

// ---------------------------------------------------------------------------
// The script under test.  `main` stores two values on the level object and
// starts `worker`, which parks in waittill("go") and records their sum when
// the notify arrives.  `fire` delivers that notify.  A round trip that
// restores the object graph, the notify list membership and the parked
// thread's stack therefore ends with record(33); a graph that came back
// without the thread (or without its values) does not.
// ---------------------------------------------------------------------------

static char g_plus[] = "+";
static const char *g_source =
    "main()\n"
    "{\n"
    "    self.values[0] = 11;\n"
    "    self.values[1] = 22;\n"
    "    self.nested[0][0] = 1;\n"
    "    self.nested[1][0] = 2;\n"
    "    self.nested[2][\"k\"] = 3;\n"
    "    self.selfref = self;\n"
    "    self.values[2] = self;\n"
    "    thread worker();\n"
    "}\n"
    "\n"
    "worker()\n"
    "{\n"
    "    self waittill(\"go\");\n"
    "    record(self.values[0] + self.values[1]);\n"
    "}\n"
    "\n"
    "fire()\n"
    "{\n"
    "    record(-100);\n"
    "    self notify(\"go\");\n"
    "}\n";

static int g_failures = 0;
static int g_mainHandle = 0;
static int g_fireHandle = 0;

static void Fail(const char *detail)
{
    std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP %s\n", detail);
    ++g_failures;
}

void ScriptParse(sval_u *parseData, unsigned char user);

// Scr_LoadScriptInternal's in-memory setup without FS_ReadFile: the lexer sees
// the production byte protocol (a one-character in_ptr drain followed by the
// real source), exactly as switch_script_compile_test.cpp does it.
static void LoadAndCompile()
{
    scrCompilePub.far_function_count = 0;
    Scr_InitAllocNode();

    const uint32_t name = Scr_CreateCanonicalFilename("savetest");
    const uint32_t scriptId = GetNewVariable(scrCompilePub.loadedscripts, name);

    const int sourceLen = static_cast<int>(std::strlen(g_source));
    char *sourceBuf = reinterpret_cast<char *>(Hunk_AllocateTempMemoryHigh(sourceLen + 1, "scr_save_roundtrip source"));
    std::memcpy(sourceBuf, g_source, static_cast<size_t>(sourceLen) + 1);

    char extFilename[64];
    Com_sprintf(extFilename, sizeof(extFilename), "%s.gsc", SL_ConvertToString(name));
    Scr_AddSourceBufferInternal(extFilename, TempMalloc(0), sourceBuf, sourceLen, 1, true);

    scrCompilePub.in_ptr = g_plus;
    scrCompilePub.parseBuf = sourceBuf;

    sval_u parseData;
    ScriptParse(&parseData, 0);

    const uint32_t fileVar = GetVariable(scrCompilePub.scripts, name);
    const uint32_t fileId = GetObject(fileVar);

    PrecacheEntry entries[MAX_PRECACHE_ENTRIES];
    ScriptCompile(parseData, fileId, scriptId, &entries[0], 0);

    g_mainHandle = Scr_GetFunctionHandle("savetest", "main");
    g_fireHandle = Scr_GetFunctionHandle("savetest", "fire");
    if (!g_mainHandle || !g_fireHandle)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP missing handles main=%d fire=%d\n",
                     g_mainHandle, g_fireHandle);
        ++g_failures;
    }
}

// The engine's own VM bring-up order (qcommon/common.cpp, then G_InitGame):
// string list and hunk once at process start, then per map: variables, VM,
// settings, the game system, the game variable and the script load context.
static void BootVm()
{
    Scr_InitVariables();
    Scr_Init();
    Scr_Settings(0, 0, 0);
    Scr_InitSystem(1);
    Scr_AllocGameVariable();
    // GScr_PostLoadScripts (g_scr_main.cpp): the four class entity arrays are
    // roots of the save graph.  Without them Scr_SavePost writes id 0 for each
    // class and Scr_LoadPre calls RemoveRefToObject(0), which indexes the
    // free-list head entry and corrupts the object allocator (found the hard
    // way: the loaded graph then could not allocate a single further object).
    for (int classnum = 0; classnum < 4; ++classnum)
        Scr_SetClassMap(classnum);
    Scr_SetLoading(1);
    Scr_BeginLoadScripts();
}

// Scr_SaveShutdown's cyclic-leak check reports a bare error; name the leaking
// objects first so a failure here is actionable.  Same predicate it uses:
// an externally referenced object that never made it into saveIdMap.
static int ReportLeakedObjects()
{
    if (!scrVarDebugPub)
        return 0;
    int leaked = 0;
    for (int id = 1; id <= 0x7FFF; ++id)
    {
        const VariableValueInternal *entry = &scrVarGlob.variableList[id + 1];
        if ((entry->w.type & 0x60) == 0)
            continue;
        if (scrVarPub.saveIdMap[id] || (entry->w.type & 0x1F) == 0x15)
            continue;
        if (leaked < 8)
        {
            std::printf(
                "SCRIPT_SAVE_ROUNDTRIP_LEAK id=%d type=0x%x usage='%s'\n",
                id, entry->w.type,
                scrVarDebugPub->varUsage[id + 1] ? scrVarDebugPub->varUsage[id + 1] : "(none)");
        }
        ++leaked;
    }
    return leaked;
}

// ---------------------------------------------------------------------------
// Two processes, one file: the parent builds a live script graph and writes the
// save stream to disk; the child is a fresh process that boots its own VM,
// compiles the same script (so code positions resolve), loads the stream back
// and proves the parked thread resumes.  That is the game's own shape -- a save
// file is what a map spawn reads -- and it keeps this proof away from a
// same-process VM teardown, which the engine only performs on a map change.
// ---------------------------------------------------------------------------

// Length of a variable range's free list, refusing to loop: a *cycle* in that
// list is how the load once died with "exceeded maximum number of script
// variables" (Scr_LoadPre calls RemoveRefToObject(0) when the VM has no class
// entity arrays, which indexes the free-list head entry), so a repeated or
// out-of-range offset is reported instead of counted.
static uint32_t CountFreeList(uint32_t begin)
{
    static unsigned char seen[0x20000];
    std::memset(seen, 0, sizeof(seen));
    uint32_t length = 0;
    for (uint32_t index = scrVarGlob.variableList[begin].u.next; index; ++length)
    {
        if (index >= 0x20000)
        {
            std::printf("SCRIPT_SAVE_ROUNDTRIP_FREELIST begin=0x%x offset-out-of-range=%u after=%u\n",
                        begin, index, length);
            return length;
        }
        if (seen[index])
        {
            std::printf("SCRIPT_SAVE_ROUNDTRIP_FREELIST begin=0x%x cycle-at=%u after=%u\n",
                        begin, index, length);
            return length;
        }
        seen[index] = 1;
        const VariableValueInternal *entry = &scrVarGlob.variableList[index + begin];
        index = entry->u.next;
    }
    return length;
}

// The object allocator's own cycle: allocate, release, allocate again and
// require the parent free list to stay acyclic and long.  A save/load round
// trip walks this path hard (Scr_LoadPre allocates one object per saved id),
// so a broken allocator here fails long before the stream is at fault.
static int AllocatorCycleProbe()
{
    const int kCount = 64;
    uint32_t ids[kCount];
    for (int i = 0; i < kCount; ++i)
        ids[i] = AllocObject();
    for (int i = 0; i < kCount; ++i)
        RemoveRefToObject(ids[i]);

    const uint32_t afterFree = CountFreeList(VARIABLELIST_PARENT_BEGIN);
    uint32_t again[kCount];
    for (int i = 0; i < kCount; ++i)
        again[i] = AllocObject();
    const uint32_t afterRealloc = CountFreeList(VARIABLELIST_PARENT_BEGIN);
    for (int i = 0; i < kCount; ++i)
        RemoveRefToObject(again[i]);

    const uint32_t finalFree = CountFreeList(VARIABLELIST_PARENT_BEGIN);
    std::printf("SCRIPT_SAVE_ROUNDTRIP_ALLOC_CYCLE alloc=%d free=%u realloc=%u final=%u\n",
                kCount, afterFree, afterRealloc, finalFree);
    if (afterFree < VARIABLELIST_PARENT_SIZE - 2 * kCount
        || afterRealloc < VARIABLELIST_PARENT_SIZE - 2 * kCount
        || finalFree < VARIABLELIST_PARENT_SIZE - 2 * kCount)
        return 1;
    std::printf("PASS:SCRIPT_SAVE_ROUNDTRIP_ALLOC_CYCLE\n");
    return 0;
}

static int SaveSide(const char *selfPath)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    SL_Init();
    Com_InitHunkMemory();
    BootVm();
    if (reinterpret_cast<uintptr_t>(scrVarPub.programBuffer) <= UINT32_MAX)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP program buffer not high (%p)\n",
                     reinterpret_cast<const void *>(scrVarPub.programBuffer));
        return 1;
    }

    if (AllocatorCycleProbe() != 0)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP allocator cycle probe\n");
        return 1;
    }

    LoadAndCompile();
    // Scr_LoadScripts' own tail: the compile-time objects (loadedscripts,
    // scripts, builtin tables) are freed here, so they are not part of the
    // save graph -- without it the save's cyclic-leak check flags them.
    Scr_EndLoadScripts();
    if (g_failures)
        return 1;

    // main() stores the values and starts worker(), which parks in waittill.
    // A parked thread is not "executing", so scrVarPub.ext_threadcount is back
    // to zero here; the parked thread's existence is proved by the child's
    // notify, which only a live waittill can answer.
    const uint16_t mainThread = Scr_ExecThread(g_mainHandle, 0);
    Scr_FreeThread(mainThread);
    if (g_recorded != -1)
    {
        std::fprintf(stderr,
                     "FAIL:SCRIPT_SAVE_ROUNDTRIP the worker ran to completion before the save "
                     "(recorded=%d)\n",
                     g_recorded);
        return 1;
    }

    static uint8_t saveBuffer[65536];
    MemoryFile writer;
    MemFile_InitForWriting(&writer, static_cast<int>(sizeof(saveBuffer)), saveBuffer, true, false);

    scrVarPub.time = 12345;
    Scr_SavePre(1);
    Scr_SavePost(&writer);
    MemFile_StartSegment(&writer, -1);
    const int payloadSize = writer.bufferSize;
    const int savedObjectCount = scrVarPub.savecount;
    const int leakedObjects = ReportLeakedObjects();
    Scr_SaveShutdown(true);
    if (leakedObjects)
    {
        std::fprintf(stderr,
                     "FAIL:SCRIPT_SAVE_ROUNDTRIP save graph leaked %d object(s) (see the LEAK lines)\n",
                     leakedObjects);
        return 1;
    }
    if (payloadSize <= 0 || writer.memoryOverflow)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP save produced no payload size=%d overflow=%d\n",
                     payloadSize, writer.memoryOverflow ? 1 : 0);
        return 1;
    }
    if (savedObjectCount <= 0)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP save graph is empty\n");
        return 1;
    }

    char path[256];
    std::snprintf(path, sizeof(path), "%s", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    const size_t base = std::strlen(path);
    std::snprintf(path + base, sizeof(path) - base, "/kisak-script-save-roundtrip-%ld.bin", (long)getpid());

    FILE *file = std::fopen(path, "wb");
    if (!file || std::fwrite(saveBuffer, 1, static_cast<size_t>(payloadSize), file) != static_cast<size_t>(payloadSize))
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP cannot write the save stream to '%s'\n", path);
        return 1;
    }
    std::fclose(file);

    std::printf("PASS:SCRIPT_SAVE_ROUNDTRIP_SAVE payload=%d objects=%d path=%s\n",
                payloadSize, savedObjectCount, path);

    // Fresh process, fresh VM: the load half runs where a map spawn would.
    const pid_t child = fork();
    if (child == 0)
    {
        execl(selfPath, selfPath, "--load", path, static_cast<char *>(nullptr));
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP cannot exec the load side\n");
        _exit(127);
    }
    if (child < 0)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP fork failed\n");
        return 1;
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP waitpid failed\n");
        return 1;
    }
    if (std::getenv("KISAK_SCRIPT_SAVE_ROUNDTRIP_KEEP"))
        std::printf("SCRIPT_SAVE_ROUNDTRIP_KEPT path=%s\n", path);
    else
        std::remove(path);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP load side exited status=%d\n", status);
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Real savegame (LoadSavegameSide): the same sequence G_LoadMainState runs, but
// the object graph comes out of a file the engine's own writer produced instead
// of a synthetic one.  The engine's MemoryFile decodes the segment chain, so
// the file bytes are handed over unchanged; what this pins is that a real
// several-thousand-object graph loads, that its removal walk frees what it
// allocated, and that the load-time leak check does not index anything outside
// its own tables -- the three things a full run dies inside, without
// saying which.
// ---------------------------------------------------------------------------
static bool ReadWholeFile(const char *path, std::vector<unsigned char> &out)
{
    FILE *file = std::fopen(path, "rb");
    if (!file)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0)
    {
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t got = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    return got == out.size();
}

static int LoadSavegameSide(const char *path, bool shutdown)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::vector<unsigned char> bytes;
    if (!ReadWholeFile(path, bytes))
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP_SAVEGAME cannot read '%s'\n", path);
        return 1;
    }
    if (bytes.size() < sizeof(SaveHeader))
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP_SAVEGAME '%s' is shorter than SaveHeader\n", path);
        return 1;
    }
    SaveHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.saveVersion != 287 || header.bodySize <= 0
        || header.bodySize > 1572864
        || bytes.size() < sizeof(SaveHeader) + static_cast<size_t>(header.bodySize))
    {
        std::fprintf(stderr,
                     "FAIL:SCRIPT_SAVE_ROUNDTRIP_SAVEGAME '%s' is not loadable: version=%d bodySize=%d bytes=%zu\n",
                     path, header.saveVersion, header.bodySize, bytes.size());
        return 1;
    }

    SL_Init();
    Com_InitHunkMemory();
    BootVm();
    LoadAndCompile();
    Scr_EndLoadScripts();
    if (g_failures)
        return 1;

    // G_LoadMainState runs Scr_ShutdownSystem before Scr_LoadPre; the class
    // entity arrays survive it, which is what Scr_LoadPre replaces.
    Scr_ShutdownSystem(1, 1);

    // The load-side diagnostics are what this mode exists to show.
    scrLoadDiagEnabled = true;

    // The real save's code positions are offsets into the retail program, which
    // this proof does not have (the round trip above compiles its own script for
    // exactly that reason).  Nothing in the load's teardown dereferences them, so
    // give the VM an opcode buffer large enough to contain every offset and let
    // the membership check pass instead of tripping the proof's asserts; the
    // release build compiles that check out.
    static std::vector<char> opcodeSpace(16u << 20);
    scrVarPub.programBuffer = opcodeSpace.data();
    scrCompilePub.programLen = static_cast<uint32_t>(opcodeSpace.size());

    MemoryFile reader;
    MemFile_InitForReading(&reader, header.bodySize, bytes.data() + sizeof(SaveHeader), false);
    MemFile_MoveToSegment(&reader, 4);
    std::printf("SCRIPT_SAVE_ROUNDTRIP_SAVEGAME stage=segment4 file=%s map='%s'\n", path,
                std::string(header.mapName, strnlen(header.mapName, sizeof(header.mapName))).c_str());

    Scr_LoadPre(1, &reader);
    const int loadedObjects = scrVarPub.savecount;
    std::printf("SCRIPT_SAVE_ROUNDTRIP_SAVEGAME stage=loadpre objects=%d time=%u level=%u anim=%u timeArray=%u\n",
                loadedObjects, scrVarPub.time, scrVarPub.levelId, scrVarPub.animId,
                scrVarPub.timeArrayId);
    if (loadedObjects <= 0 || !scrVarPub.levelId || !scrVarPub.timeArrayId)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP_SAVEGAME the script segment did not load\n");
        return 1;
    }

    if (shutdown)
    {
        Scr_LoadShutdown();
        std::printf("SCRIPT_SAVE_ROUNDTRIP_SAVEGAME stage=loadshutdown objects=%d values=%d refs=%d\n",
                    scrVarPub.numScriptObjects, scrVarPub.numScriptValues,
                    scrVarPub.totalObjectRefCount);
    }

    std::printf("PASS:SCRIPT_SAVE_ROUNDTRIP_SAVEGAME objects=%d shutdown=%d asserts=0\n",
                loadedObjects, shutdown ? 1 : 0);
    return 0;
}

static int LoadSide(const char *path)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    static uint8_t saveBuffer[65536];
    FILE *file = std::fopen(path, "rb");
    if (!file)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP load side cannot open '%s'\n", path);
        return 1;
    }
    const size_t payloadSize = std::fread(saveBuffer, 1, sizeof(saveBuffer), file);
    std::fclose(file);
    if (payloadSize == 0 || payloadSize >= sizeof(saveBuffer))
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP load side read %zu bytes\n", payloadSize);
        return 1;
    }

    SL_Init();
    Com_InitHunkMemory();
    BootVm();
    LoadAndCompile();
    Scr_EndLoadScripts();
    if (g_failures)
        return 1;

    // G_LoadMainState: the fresh VM's graph is torn down again and the saved
    // graph is read into what is left -- the state Scr_LoadPre asserts on.
    std::printf("SCRIPT_SAVE_ROUNDTRIP_LOAD_STAGE compile=done\n");
    Scr_ShutdownSystem(1, 1);
    std::printf("SCRIPT_SAVE_ROUNDTRIP_LOAD_STAGE teardown=done\n");

    MemoryFile reader;
    MemFile_InitForReading(&reader, static_cast<int>(payloadSize), saveBuffer, false);
    Scr_LoadPre(1, &reader);
    std::printf("SCRIPT_SAVE_ROUNDTRIP_LOAD_STAGE loadpre=done\n");
    Scr_LoadShutdown();
    std::printf("SCRIPT_SAVE_ROUNDTRIP_LOAD_STAGE loadshutdown=done\n");

    const int loadedObjects = scrVarPub.savecount;
    const uint32_t loadedTime = scrVarPub.time;
    int failures = 0;
    if (loadedTime != 12345)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP script clock changed saved=12345 loaded=%u\n",
                     loadedTime);
        ++failures;
    }
    if (!scrVarPub.levelId || !scrVarPub.timeArrayId)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_SAVE_ROUNDTRIP level/time roots did not come back\n");
        ++failures;
    }

    // The loaded thread must still run: fire()'s notify only reaches a live
    // waittill, and the value it records is the one main() stored before the
    // save, so both the graph and the parked stack had to survive.
    g_recorded = -1;
    const uint16_t fireThread = Scr_ExecThread(g_fireHandle, 0);
    Scr_FreeThread(fireThread);
    // VM_Notify does not resume a waittill thread in place: it re-queues it in
    // the time array for scrVarPub.time, and the thread runs when the engine's
    // per-frame pump reaches that entry.  Scr_IncTime/Scr_RunCurrentThreads is
    // that pump (same pair the game runs each frame), so the loaded thread has
    // to become runnable here exactly as it would after a real load.
    for (int frame = 0; frame < 4 && g_recorded != 33; ++frame)
    {
        Scr_IncTime();
        Scr_RunCurrentThreads();
    }
    if (g_recorded != 33)
    {
        // Name what a stalled resume is usually missing: VM_Notify resolves the
        // notify list as the level object's child under the reserved field name
        // 0x18000 and the watcher inside it, so report both on failure.
        const uint32_t notifyListId = FindVariable(scrVarPub.levelId, 0x18000u);
        const uint32_t goString = SL_FindString("go");
        std::fprintf(stderr,
                     "FAIL:SCRIPT_SAVE_ROUNDTRIP loaded thread did not resume; recorded=%d want=33 "
                     "objects=%d level=%u notifyList=%u size=%d goStr=%u\n",
                     g_recorded, loadedObjects, scrVarPub.levelId, notifyListId,
                     notifyListId ? GetArraySize(FindObject(notifyListId)) : -1, goString);
        ++failures;
    }
    if (failures)
        return 1;

    std::printf("PASS:SCRIPT_SAVE_ROUNDTRIP objects=%d payload=%zu recorded=%d scriptTime=%u asserts=0\n",
                loadedObjects, payloadSize, g_recorded, loadedTime);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "--load") == 0)
        return LoadSide(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "--load-savegame") == 0)
        return LoadSavegameSide(argv[2], argc < 4 || std::strcmp(argv[3], "--no-shutdown") != 0);
    return SaveSide(argv[0]);
}
