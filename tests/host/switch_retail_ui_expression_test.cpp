// LP64 UI expression proof.  Drives the production UI expression
// compiler/evaluator (Statement_AddStringOperand + EvaluateExpression from
// src/ui/ui_shared_obj.cpp / ui_expressions.cpp / ui_expressions_logicfunctions.cpp)
// with every operand allocated above 4 GiB.  If any pointer in the
// expressionEntry/Operand path were truncated to 32 bits -- the exact class
// of bug (fixed earlier) -- the string equality/concat operands would
// misread or ASan would fault.  The visual/menu consumer is out of scope
// here; this proves the evaluator half only.
#include <universal/q_shared.h>
#include <ui/ui_shared.h>
#include <stringed/stringed_hooks.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>

void __cdecl Statement_AddStringOperand(statement_s *statement, char *str);
void __cdecl Statement_AddIntOperand(statement_s *statement, int val);
void __cdecl Statement_AddFloatOperand(statement_s *statement, float val);
void __cdecl Statement_AddOperator(statement_s *statement, operationEnum op);
Operand *__cdecl EvaluateExpression(int localClientNum, const statement_s *statement,
                                    Operand *result);
char *__cdecl GetSourceString(Operand operand);
operandInternalDataUnion __cdecl GetSourceInt(Operand *source);
char __cdecl parse_expression_internal(int handle, statement_s *statement, int maxEntries);
void __cdecl GetDvarStringValue(Operand *source, Operand *result);
void __cdecl GetDvarIntValue(Operand *source, Operand *result);
void __cdecl GetDvarBoolValue(Operand *source, Operand *result);
void __cdecl GetDvarFloatValue(Operand *source, Operand *result);
void __cdecl GetLocalVarIntValue(int localClientNum, Operand *source, Operand *result);
void __cdecl GetLocalVarBoolValue(int localClientNum, Operand *source, Operand *result);
void __cdecl GetLocalVarFloatValue(int localClientNum, Operand *source, Operand *result);
void __cdecl GetLocalVarStringValue(int localClientNum, Operand *source, Operand *result,
                                    char *stringBuf, uint32_t size);
void __cdecl LocalizeString(OperandList *list, Operand *operandResult);

namespace
{
bool Check(bool value, const char *stage)
{
    if (!value)
        std::fprintf(stderr, "FAIL:M4_UI_EXPRESSION stage=%s\n", stage);
    return value;
}

uint8_t *g_highArena = nullptr;
size_t g_highUsed = 0;
constexpr size_t kHighArenaBytes = 8u << 20;

void HighArenaInit()
{
    void *arena = mmap(reinterpret_cast<void *>(0x200000000ULL), kHighArenaBytes,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (arena == MAP_FAILED)
        arena = mmap(nullptr, kHighArenaBytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_highArena = static_cast<uint8_t *>(arena == MAP_FAILED ? nullptr : arena);
}
} // namespace

// Minimal engine surface the evaluator compiles against.  Only the exercised
// paths resolve: the many other operations the shared translation units
// reference stay unresolved (link with --unresolved-symbols=ignore-all) and
// are never called, and the proof recipe weakens production's
// PC_ReadTokenHandle in the linked object so the test file's scripted
// token source feeds the parser error path.
static char s_print_error_capture[1024];
void *Z_Malloc(int size, const char *, int)
{
    const size_t aligned = (static_cast<size_t>(size) + 15u) & ~static_cast<size_t>(15);
    void *p = g_highArena + g_highUsed;
    g_highUsed += aligned;
    return p;
}
void MyAssertHandler(const char *, int, int, const char *, ...)
{
    std::fprintf(stderr, "M4_UI_EXPRESSION assert\n");
    std::abort();
}
void Com_Error(errorParm_t, const char *fmt, ...)
{
    std::fprintf(stderr, "Com_Error: %s\n", fmt);
    std::abort();
}
void Com_PrintWarning(int, const char *, ...) {}
void Com_PrintError(int, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_print_error_capture, sizeof(s_print_error_capture), fmt, ap);
    va_end(ap);
}
void Com_Printf(int, const char *, ...) {}
char *va(const char *fmt, ...)
{
    static char buffer[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    return buffer;
}
int Com_sprintf(char *dest, uint32_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int written = vsnprintf(dest, size, fmt, ap);
    va_end(ap);
    return written;
}
void I_strncpyz(char *dest, const char *src, int size)
{
    if (size > 0)
    {
        std::strncpy(dest, src, static_cast<size_t>(size));
        dest[size - 1] = '\0';
    }
}
int I_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
// Canned tick: 90000 % 60000 == 30000, so the retail scroll expression
// below must evaluate to exactly 427.0.
uint32_t Sys_Milliseconds(void) { return 90000u; }
bool Dvar_GetBool(const char *) { return false; }
static char *s_loc_high_value = nullptr;
char *SEH_LocalizeTextMessage(const char *key, const char *, msgLocErrType_t)
{
    if (s_loc_high_value && key && !std::strcmp(key, "PROOF_KEY"))
        return s_loc_high_value;
    static char empty[1] = {};
    return empty;
}
static dvar_s s_uiscript_debug_storage{};
const dvar_t *uiscript_debug = &s_uiscript_debug_storage;
static dvar_s s_use_fast_file_storage{};
const dvar_t *useFastFile = &s_use_fast_file_storage;

// B0 doubles: one stub string dvar served from the high arena, a
// localizer echoing a high-address value, a capturing Com_PrintError, and
// a scripted token source driving parse_expression_internal down its
// `operand (` error path with a high-address string operand.
static dvar_s s_proof_string_dvar{};
static dvar_s s_proof_empty_dvar{};
static int s_token_call = 0;
static char *s_token_high_marker = nullptr;

const dvar_s *Dvar_FindVar(const char *name)
{
    if (!std::strcmp(name, "proof_dvar"))
        return &s_proof_string_dvar;
    if (!std::strcmp(name, "fs_game"))
        return &s_proof_empty_dvar;
    return nullptr;
}
const char *Dvar_GetVariantString(const char *name)
{
    if (!std::strcmp(name, "proof_int"))
        return "42";
    if (!std::strcmp(name, "proof_bool"))
        return "1";
    if (!std::strcmp(name, "proof_float"))
        return "2.5";
    return "";
}

// Local-var owner double: one BOOL/INT/FLOAT/STRING name served from a
// static context; production only reads through these accessors.
static UILocalVarContext s_local_ctx{};
UILocalVarContext *UI_GetLocalVarsContext(int) { return &s_local_ctx; }
UILocalVarContext *UILocalVar_Find(UILocalVarContext *, const char *name)
{
    if (!std::strcmp(name, "proof_local"))
        return &s_local_ctx;
    return nullptr;
}
bool UILocalVar_GetBool(const UILocalVar *) { return true; }
UILocalVar_u UILocalVar_GetInt(const UILocalVar *) { return UILocalVar_u(42); }
double UILocalVar_GetFloat(const UILocalVar *) { return 0.5; }
char *UILocalVar_GetString(const UILocalVar *, char *stringBuf, uint32_t size)
{
    if (size)
    {
        std::strncpy(stringBuf, "local_high", static_cast<size_t>(size));
        stringBuf[size - 1] = '\0';
    }
    return stringBuf;
}

// Token source for the proof link: the harness weakens production's
// PC_ReadTokenHandle in the linked object (objcopy --weaken-symbol, see
// the proof recipe in ./test), so this strong definition feeds one
// high-address string operand, one left paren to trip the error path,
// then EOF.
int PC_ReadTokenHandle(int, pc_token_s *token)
{
    std::memset(token, 0, sizeof(*token));
    if (s_token_call == 0)
    {
        token->type = 1;
        std::strncpy(token->string, s_token_high_marker, sizeof(token->string) - 1);
    }
    else if (s_token_call == 1)
    {
        token->type = 0;
        std::strncpy(token->string, "(", sizeof(token->string) - 1);
    }
    else
    {
        return 0;
    }
    ++s_token_call;
    return 1;
}

int main()
{
    HighArenaInit();
    if (!Check(g_highArena != nullptr, "arena") ||
        !Check(reinterpret_cast<uintptr_t>(g_highArena) > 0xffffffffULL, "high_arena"))
        return 1;

    // High-address string equality: both operands are heap entries above 4 GiB.
    static expressionEntry *equalEntries[8];
    statement_s equalStatement{};
    equalStatement.entries = equalEntries;
    Statement_AddStringOperand(&equalStatement, const_cast<char *>("main_text"));
    Statement_AddStringOperand(&equalStatement, const_cast<char *>("main_text"));
    Statement_AddOperator(&equalStatement, OP_EQUALS);
    Operand equalResult{};
    const bool equalOk = EvaluateExpression(0, &equalStatement, &equalResult) != nullptr &&
                         GetSourceInt(&equalResult).intVal == 1;

    // High-address string concatenation through OP_ADD.
    static expressionEntry *concatEntries[8];
    statement_s concatStatement{};
    concatStatement.entries = concatEntries;
    Statement_AddStringOperand(&concatStatement, const_cast<char *>("kitchen"));
    Statement_AddStringOperand(&concatStatement, const_cast<char *>("_sink"));
    Statement_AddOperator(&concatStatement, OP_ADD);
    Operand concatResult{};
    const bool concatOk = EvaluateExpression(0, &concatStatement, &concatResult) != nullptr &&
                          std::strcmp(GetSourceString(concatResult), "kitchen_sink") == 0;

    const bool entriesHigh = reinterpret_cast<uintptr_t>(equalEntries[0]) > 0xffffffffULL &&
                             reinterpret_cast<uintptr_t>(concatEntries[1]) > 0xffffffffULL;

    // B0a: the parser's `operand (` error path must carry a high-address
    // string operand through the widened union intact (was: truncated
    // through intVal; ASan faults on the truncated %s under the old code).
    char *highMarker = static_cast<char *>(Z_Malloc(64, __FILE__, __LINE__));
    std::strcpy(highMarker, "unionsafe_high_marker");
    s_token_high_marker = highMarker;
    s_token_call = 0;
    s_print_error_capture[0] = '\0';
    static expressionEntry *parseEntries[8];
    statement_s parseStatement{};
    parseStatement.entries = parseEntries;
    const bool parseOk = parse_expression_internal(0, &parseStatement, 8) == 1;
    const bool unionSafe =
        parseOk && reinterpret_cast<uintptr_t>(highMarker) > 0xffffffffULL &&
        std::strstr(s_print_error_capture, "unionsafe_high_marker") != nullptr;

    // B0b: dvarstring() with a high-address name and a high-address value.
    char *dvarName = static_cast<char *>(Z_Malloc(32, __FILE__, __LINE__));
    std::strcpy(dvarName, "proof_dvar");
    char *dvarValue = static_cast<char *>(Z_Malloc(64, __FILE__, __LINE__));
    std::strcpy(dvarValue, "proof_value_high");
    s_proof_string_dvar.type = 7;
    s_proof_string_dvar.name = dvarName;
    s_proof_string_dvar.current.string = dvarValue;
    Operand dvarSource{};
    dvarSource.dataType = VAL_STRING;
    dvarSource.internals.string = dvarName;
    Operand dvarResult{};
    GetDvarStringValue(&dvarSource, &dvarResult);
    const bool dvarStringOk =
        dvarResult.dataType == VAL_STRING &&
        reinterpret_cast<uintptr_t>(dvarName) > 0xffffffffULL &&
        reinterpret_cast<uintptr_t>(dvarValue) > 0xffffffffULL &&
        !std::strcmp(dvarResult.internals.string, "proof_value_high");

    // B0c: locstring() with a high-address key resolving to a high value.
    char *locValue = static_cast<char *>(Z_Malloc(64, __FILE__, __LINE__));
    std::strcpy(locValue, "proof_loc_high");
    s_loc_high_value = locValue;
    char *locKey = static_cast<char *>(Z_Malloc(32, __FILE__, __LINE__));
    std::strcpy(locKey, "@PROOF_KEY");
    OperandList locList{};
    locList.operands[0].dataType = VAL_STRING;
    locList.operands[0].internals.string = locKey;
    locList.operandCount = 1;
    Operand locResult{};
    LocalizeString(&locList, &locResult);
    const bool locStringOk =
        locResult.dataType == VAL_STRING &&
        reinterpret_cast<uintptr_t>(locKey) > 0xffffffffULL &&
        locResult.internals.string == locValue;

    // B1a: integer arithmetic through the real evaluator (6 * 7).
    static expressionEntry *mulEntries[8];
    statement_s mulStatement{};
    mulStatement.entries = mulEntries;
    Statement_AddIntOperand(&mulStatement, 6);
    Statement_AddIntOperand(&mulStatement, 7);
    Statement_AddOperator(&mulStatement, OP_MULTIPLY);
    Operand mulResult{};
    const bool intOk = EvaluateExpression(0, &mulStatement, &mulResult) != nullptr &&
                       GetSourceInt(&mulResult).intVal == 42;

    // B1b: float arithmetic (0.5 + 0.25 == 0.75, exact in binary).
    static expressionEntry *addEntries[8];
    statement_s addStatement{};
    addStatement.entries = addEntries;
    Statement_AddFloatOperand(&addStatement, 0.5f);
    Statement_AddFloatOperand(&addStatement, 0.25f);
    Statement_AddOperator(&addStatement, OP_ADD);
    Operand addResult{};
    const bool floatOk = EvaluateExpression(0, &addStatement, &addResult) != nullptr &&
                         addResult.dataType == VAL_FLOAT && addResult.internals.floatVal == 0.75f;

    // B1c: dvar int/bool/float through the variant-string path.
    char *intName = static_cast<char *>(Z_Malloc(32, __FILE__, __LINE__));
    std::strcpy(intName, "proof_int");
    Operand intSource{};
    intSource.dataType = VAL_STRING;
    intSource.internals.string = intName;
    Operand intDvarResult{};
    GetDvarIntValue(&intSource, &intDvarResult);
    char *boolName = static_cast<char *>(Z_Malloc(32, __FILE__, __LINE__));
    std::strcpy(boolName, "proof_bool");
    Operand boolSource{};
    boolSource.dataType = VAL_STRING;
    boolSource.internals.string = boolName;
    Operand boolDvarResult{};
    GetDvarBoolValue(&boolSource, &boolDvarResult);
    char *floatName = static_cast<char *>(Z_Malloc(32, __FILE__, __LINE__));
    std::strcpy(floatName, "proof_float");
    Operand floatSource{};
    floatSource.dataType = VAL_STRING;
    floatSource.internals.string = floatName;
    Operand floatDvarResult{};
    GetDvarFloatValue(&floatSource, &floatDvarResult);
    const bool dvarValsOk =
        intDvarResult.dataType == VAL_INT && intDvarResult.internals.intVal == 42 &&
        boolDvarResult.dataType == VAL_INT && boolDvarResult.internals.intVal == 1 &&
        floatDvarResult.dataType == VAL_FLOAT && floatDvarResult.internals.floatVal == 2.5f;

    // B1d: localvar int/bool/float/string through the stub owner.
    char *localName = static_cast<char *>(Z_Malloc(32, __FILE__, __LINE__));
    std::strcpy(localName, "proof_local");
    Operand localSource{};
    localSource.dataType = VAL_STRING;
    localSource.internals.string = localName;
    Operand localIntResult{}, localBoolResult{}, localFloatResult{}, localStringResult{};
    GetLocalVarIntValue(0, &localSource, &localIntResult);
    GetLocalVarBoolValue(0, &localSource, &localBoolResult);
    GetLocalVarFloatValue(0, &localSource, &localFloatResult);
    char localBuf[64];
    GetLocalVarStringValue(0, &localSource, &localStringResult, localBuf, sizeof(localBuf));
    const bool localVarsOk =
        localIntResult.dataType == VAL_INT && localIntResult.internals.intVal == 42 &&
        localBoolResult.dataType == VAL_INT && localBoolResult.internals.intVal == 1 &&
        localFloatResult.dataType == VAL_FLOAT && localFloatResult.internals.floatVal == 0.5f &&
        localStringResult.dataType == VAL_STRING &&
        !std::strcmp(localStringResult.internals.string, "local_high");

    // B2a: the real main_text visibility pattern, dvarstring("fs_game") == ""
    // (empty here, so the item shows -- matching retail boot with no mod).
    // Entries are infix, exactly as the game parser and OAT's IW3 menu
    // converter lay them out: function-op, arguments, RIGHTPAREN, then the
    // enclosing infix operator and its right operand.
    s_proof_empty_dvar.type = 7;
    s_proof_empty_dvar.name = "fs_game";
    s_proof_empty_dvar.current.string = "";
    static expressionEntry *visEntries[8];
    statement_s visStatement{};
    visStatement.entries = visEntries;
    Statement_AddOperator(&visStatement, OP_DVARSTRING);
    Statement_AddStringOperand(&visStatement, const_cast<char *>("fs_game"));
    Statement_AddOperator(&visStatement, OP_RIGHTPAREN);
    Statement_AddOperator(&visStatement, OP_EQUALS);
    Statement_AddStringOperand(&visStatement, const_cast<char *>(""));
    Operand visResult{};
    const bool dvarCmpOk = EvaluateExpression(0, &visStatement, &visResult) != nullptr &&
                           GetSourceInt(&visResult).intVal == 1;

    // B2b: the real main_text scroll sub-expression
    // ((float(milliseconds() % 60000) / 60000) * (854)) at tick 90000:
    // 90000 % 60000 = 30000, 30000.0 / 60000 = 0.5, 0.5 * 854 = 427.0.
    // Entry order is the retail binary's, not source token order: a binary
    // operator is emitted after its left operand and before its right operand
    // (OAT MenuWriterIW3's OP_LEFTPAREN/function walk over the real ui.ff
    // main_text rectX entries round-trips exactly this layout), and literal
    // right operands keep their source grouping parens.  Function calls carry
    // no LEFTPAREN: the parser and OAT's writer both synthesize the call's
    // paren, so the entries only close them with RIGHTPAREN.
    static expressionEntry *scrollEntries[24];
    statement_s scrollStatement{};
    scrollStatement.entries = scrollEntries;
    Statement_AddOperator(&scrollStatement, OP_LEFTPAREN);
    Statement_AddOperator(&scrollStatement, OP_LEFTPAREN);
    Statement_AddOperator(&scrollStatement, OP_TOFLOAT);
    Statement_AddOperator(&scrollStatement, OP_MILLISECONDS);
    Statement_AddOperator(&scrollStatement, OP_RIGHTPAREN);
    Statement_AddOperator(&scrollStatement, OP_MODULUS);
    Statement_AddIntOperand(&scrollStatement, 60000);
    Statement_AddOperator(&scrollStatement, OP_RIGHTPAREN);
    Statement_AddOperator(&scrollStatement, OP_DIVIDE);
    Statement_AddIntOperand(&scrollStatement, 60000);
    Statement_AddOperator(&scrollStatement, OP_RIGHTPAREN);
    Statement_AddOperator(&scrollStatement, OP_MULTIPLY);
    Statement_AddOperator(&scrollStatement, OP_LEFTPAREN);
    Statement_AddIntOperand(&scrollStatement, 854);
    Statement_AddOperator(&scrollStatement, OP_RIGHTPAREN);
    Statement_AddOperator(&scrollStatement, OP_RIGHTPAREN);
    Operand scrollResult{};
    Operand *scrollRet = EvaluateExpression(0, &scrollStatement, &scrollResult);
    const bool msArithOk = scrollRet != nullptr &&
                           scrollResult.dataType == VAL_FLOAT &&
                           scrollResult.internals.floatVal == 427.0f;

    // B2c: bare localvarint truthiness, the dominant visible-when form.
    static expressionEntry *lvEntries[8];
    statement_s lvStatement{};
    lvStatement.entries = lvEntries;
    Statement_AddOperator(&lvStatement, OP_LOCALVARINT);
    Statement_AddStringOperand(&lvStatement, localName);
    Statement_AddOperator(&lvStatement, OP_RIGHTPAREN);
    Operand lvResult{};
    const bool localTruthyOk = EvaluateExpression(0, &lvStatement, &lvResult) != nullptr &&
                               GetSourceInt(&lvResult).intVal == 42;

    std::printf("M4_UI_EXPRESSION arena=%p eq=%d concat=%d entries_high=%d result='%s' "
                "unionsafe=%d dvarstring=%d locstring=%d tokencalls=%d capture='%s' "
                "int=%d float=%d dvarvals=%d localvars=%d dvarcmp=%d msarith=%d localtruthy=%d\n",
                static_cast<void *>(g_highArena), equalOk ? 1 : 0, concatOk ? 1 : 0,
                entriesHigh ? 1 : 0, concatOk ? GetSourceString(concatResult) : "(none)",
                unionSafe ? 1 : 0, dvarStringOk ? 1 : 0, locStringOk ? 1 : 0,
                s_token_call, s_print_error_capture,
                intOk ? 1 : 0, floatOk ? 1 : 0, dvarValsOk ? 1 : 0, localVarsOk ? 1 : 0,
                dvarCmpOk ? 1 : 0, msArithOk ? 1 : 0, localTruthyOk ? 1 : 0);
    if (!Check(equalOk, "string_equal") || !Check(concatOk, "string_concat") ||
        !Check(entriesHigh, "expression_entries_high") || !Check(unionSafe, "union_safe") ||
        !Check(dvarStringOk, "dvarstring_high") || !Check(locStringOk, "locstring_high") ||
        !Check(intOk, "int_arithmetic") || !Check(floatOk, "float_arithmetic") ||
        !Check(dvarValsOk, "dvar_values") || !Check(localVarsOk, "localvar_values") ||
        !Check(dvarCmpOk, "dvarstring_compare") || !Check(msArithOk, "milliseconds_arithmetic") ||
        !Check(localTruthyOk, "localvar_truthy"))
        return 1;
    std::puts("PASS:M4_UI_EXPRESSION string=high_address equal=1 concat=1 entries=high "
              "unionsafe=1 dvarstring=1 locstring=1 int=1 float=1 dvarvals=1 localvars=1 "
              "dvarcmp=1 msarith=1 localtruthy=1 stringtable=deferred tablelookup=deferred");
    return 0;
}
