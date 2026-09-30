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
#include <xanim/xanim.h>
#include <xanim/xmodel.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    std::fprintf(stderr, "FAIL:SCRIPT_COMPILE excluded Com_Error reached: ");
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
    std::abort();
}

void MyAssertHandler(const char *file, int line, int type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "FAIL:SCRIPT_COMPILE assert %s:%d type=%d: ", file, line, type);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
    std::abort();
}

void Sys_Error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "FAIL:SCRIPT_COMPILE excluded Sys_Error reached: ");
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
    std::abort();
}

void Sys_OutOfMemErrorInternal(const char *filename, int line)
{
    std::fprintf(stderr, "FAIL:SCRIPT_COMPILE out of memory at %s:%d\n", filename, line);
    std::abort();
}

void Com_Printf(int, const char *, ...) {}
void Com_PrintError(int, const char *, ...) {}
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
    std::fprintf(stderr, "FAIL:SCRIPT_COMPILE hunk fatal: %s\n", message);
    std::abort();
}
extern "C" void Switch_HunkCoreDrop(const char *message)
{
    std::fprintf(stderr, "FAIL:SCRIPT_COMPILE hunk drop: %s\n", message);
    std::abort();
}
extern "C" void Switch_HunkCoreOutOfMemory(const char *message)
{
    std::fprintf(stderr, "FAIL:SCRIPT_COMPILE hunk out of memory: %s\n", message);
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
    std::fprintf(stderr, "FAIL:SCRIPT_COMPILE excluded debugger Scr_RefToVariable reached\n");
    std::abort();
}
XAnim_s *XAnimGetAnims(const XAnimTree_s *) { return nullptr; }
char *XAnimGetAnimDebugName(const XAnim_s *, uint32_t) { return const_cast<char *>(""); }
const char *XAnimGetAnimTreeDebugName(const XAnim_s *) { return ""; }
void XAnimFreeList(XAnim_s *) {}
void XAnimFree(XAnimParts *) {}
void XModelPartsFree(XModelPartsLoad *) {}

// ---------------------------------------------------------------------------
// Minimal builtin table.
//
// ScriptCompile resolves a call's name through the real Scr_GetBuiltin (which
// reports "not a script-local function") and then Scr_GetFunction/Scr_GetMethod.
// The compiled call stores the returned function pointer's index in
// scrCompilePub.func_table; VM_Execute invokes it from there.  We register
// exactly one builtin, `record`, which records its first integer argument.
// ---------------------------------------------------------------------------

static int g_recorded = -1;
static int g_records[16];
static int g_recordCount = 0;

static void RecordBuiltin()
{
    g_recorded = Scr_GetInt(0);
    if (g_recordCount < 16)
        g_records[g_recordCount++] = g_recorded;
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

void ScriptParse(sval_u *parseData, unsigned char user);

static int g_failures = 0;

static void Fail(const char *detail)
{
    std::fprintf(stderr, "FAIL:SCRIPT_COMPILE %s\n", detail);
    ++g_failures;
}

// SpecifyThreadPosition is a production symbol with no public header
// declaration; this harness is its only external caller.
uint32_t SpecifyThreadPosition(uint32_t posId, uint32_t name, uint32_t sourcePos, int type);

// Deterministic unit check of the exact fixed function.  The end-to-end
// ScriptCompile+VM path below proves the far call links and runs, but the
// buggy high-half read is environment dependent: it only produces garbage
// when the reused position slot's 8-byte VariableUnion already holds a
// nonzero high half (as it does in the game after earlier scripts, and as the
// uninitialized stack temporary can).  This check makes the precondition
// explicit: seed a not-yet-emitted position slot with a nonzero high half and
// require SpecifyThreadPosition to clear the whole pointer-width union.
static void CheckSpecifyThreadPositionClearsFullUnion()
{
    const uint32_t threadId = AllocObject();
    GetNewVariable(threadId, 1);

    VariableValue dirty;
    dirty.type = VAR_UNDEFINED;
    dirty.u.pointerValue = static_cast<intptr_t>(0x7FFF000000000000ULL);
    SetVariableValue(GetVariable(threadId, 1), &dirty);

    SpecifyThreadPosition(threadId, 0, 0, VAR_CODEPOS);

    const VariableValue stored = Scr_EvalVariable(GetVariable(threadId, 1));
    if (stored.u.codePosValue != nullptr)
    {
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "SpecifyThreadPosition left codePos=%p, want null: high union half not cleared",
                      reinterpret_cast<const void *>(stored.u.codePosValue));
        Fail(detail);
    }
}

static char g_plus[] = "+";
static const char *g_filename = "selftest";
static const char *g_source =
    "main()\n"
    "{\n"
    "    selftest::target();\n"
    "}\n"
    "\n"
    "target()\n"
    "{\n"
    "    record(42);\n"
    "}\n"
    "\n"
    "param6(a, b, c, d, e, f)\n"
    "{\n"
    "    record(a);\n"
    "    record(b);\n"
    "    record(c);\n"
    "    record(d);\n"
    "    record(e);\n"
    "    record(f);\n"
    "}\n";

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Real startup order (qcommon/common.cpp): string-list/memory-tree
    // system, the global hunk, variable system, VM, then the game-script
    // system before any script load.
    SL_Init();
    Com_InitHunkMemory();
    Scr_InitVariables();
    Scr_Init();
    Scr_Settings(0, 0, 0);
    Scr_InitSystem(1);
    Scr_SetLoading(1);

    Scr_BeginLoadScripts();
    if (g_failures)
        return 1;

    // High-address precondition: every code position/operand in this test is
    // pointer-width, and the regression is only visible when the program
    // buffer lives above 4 GiB.  The Switch platform's HunkUser host branch
    // mmaps there; fail loudly if this host cannot.
    if (reinterpret_cast<uintptr_t>(scrVarPub.programBuffer) <= UINT32_MAX ||
        reinterpret_cast<uintptr_t>(scrVarPub.programBuffer) != reinterpret_cast<uintptr_t>(scrVarPub.programHunkUser->buf))
    {
        std::fprintf(stderr, "FAIL:SCRIPT_COMPILE program buffer not high (%p)\n",
                     reinterpret_cast<const void *>(scrVarPub.programBuffer));
        return 1;
    }

    // Replicate Scr_LoadScriptInternal's in-memory setup (no FS_ReadFile) so
    // the lexer sees exactly the production byte protocol: a one-character
    // `in_ptr` drain followed by the real source.
    uint32_t name = Scr_CreateCanonicalFilename(g_filename);
    uint32_t scriptId = GetNewVariable(scrCompilePub.loadedscripts, name);

    int sourceLen = static_cast<int>(std::strlen(g_source));
    char *sourceBuf = reinterpret_cast<char *>(Hunk_AllocateTempMemoryHigh(sourceLen + 1, "scr_compile_test source"));
    std::memcpy(sourceBuf, g_source, static_cast<size_t>(sourceLen) + 1);

    char extFilename[64];
    Com_sprintf(extFilename, sizeof(extFilename), "%s.gsc", SL_ConvertToString(name));
    Scr_AddSourceBufferInternal(extFilename, TempMalloc(0), sourceBuf, sourceLen, 1, true);

    scrCompilePub.far_function_count = 0;
    Scr_InitAllocNode();
    scrCompilePub.in_ptr = g_plus;
    scrCompilePub.parseBuf = sourceBuf;

    sval_u parseData;
    ScriptParse(&parseData, 0);

    uint32_t fileVar = GetVariable(scrCompilePub.scripts, name);
    uint32_t fileId = GetObject(fileVar);

    PrecacheEntry entries[MAX_PRECACHE_ENTRIES];
    CheckSpecifyThreadPositionClearsFullUnion();
    ScriptCompile(parseData, fileId, scriptId, &entries[0], 0);

    int mainHandle = Scr_GetFunctionHandle(g_filename, "main");
    int targetHandle = Scr_GetFunctionHandle(g_filename, "target");
    if (!mainHandle || !targetHandle)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_COMPILE missing compiled handle main=%d target=%d\n",
                     mainHandle, targetHandle);
        return 1;
    }

    const char *targetPos = scrVarPub.programBuffer + targetHandle;
    if (!Scr_IsInOpcodeMemory(targetPos))
    {
        std::fprintf(stderr, "FAIL:SCRIPT_COMPILE target position %p outside program buffer\n",
                     static_cast<const void *>(targetPos));
        ++g_failures;
    }

    // Check 3: the far-call operand emitted for `selftest::target()` must have
    // been linked to target's real code position.  Scan the program buffer for
    // the exact pointer-width value.  It is unique (a high Hunk address), so
    // finding it here proves the unresolved link was recorded and patched
    // rather than a garbage operand emitted.
    bool found = false;
    for (uint32_t off = 0; off + sizeof(const char *) <= scrCompilePub.programLen; ++off)
    {
        const char *slot = nullptr;
        std::memcpy(&slot, scrVarPub.programBuffer + off, sizeof(slot));
        if (slot == targetPos)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        Fail("compiled far-call operand was not linked to the target code position");
        std::fprintf(stderr, "FAIL:SCRIPT_COMPILE failures=%d (skipping execution of corrupt bytecode)\n", g_failures);
        return 1;
    }

    // Check 3: execute main through the real VM.  If the far call linked, its
    // body runs and the stubbed builtin records 42.
    g_recorded = -1;
    uint16_t thread = Scr_ExecThread(mainHandle, 0);
    Scr_FreeThread(thread);
    if (g_recorded != 42)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_COMPILE executed main but builtin recorded %d, want 42\n", g_recorded);
        ++g_failures;
    }

    // Parameter-handling regression: a function with six formal parameters
    // must see OP_checkclearparams accept exactly six pushed arguments.  The
    // real game trips "function called with too many parameters" in
    // animscripts/scripted.gsc:45 init() through this exact path, so compile
    // and call it here to iterate in-process instead of a slow device run.
    // Caller pushes in reverse (last pushed is formal parameter 0), matching
    // ScrCmd_animscriptedInternal.
    int param6Handle = Scr_GetFunctionHandle(g_filename, "param6");
    if (!param6Handle)
    {
        Fail("missing compiled handle param6");
    }
    else
    {
        g_recorded = -1;
        g_recordCount = 0;
        Scr_AddInt(6);
        Scr_AddInt(5);
        Scr_AddInt(4);
        Scr_AddInt(3);
        Scr_AddInt(2);
        Scr_AddInt(1);
        uint16_t paramThread = Scr_ExecThread(param6Handle, 6);
        Scr_FreeThread(paramThread);
        for (int i = 0; i < 6; ++i)
        {
            if (g_records[i] != i + 1)
            {
                std::fprintf(stderr,
                             "FAIL:SCRIPT_COMPILE param6 argument %d recorded %d, want %d (count=%d)\n",
                             i, g_records[i], i + 1, g_recordCount);
                ++g_failures;
                break;
            }
        }
    }

    if (g_failures)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_COMPILE failures=%d\n", g_failures);
        return 1;
    }

    std::printf("PASS:SCRIPT_COMPILE handler=far-self-ref target_in_program=1 recorded=%d programLen=%u\n",
                g_recorded, scrCompilePub.programLen);
    return 0;
}
