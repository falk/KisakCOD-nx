// Regression test for LP64 script-compiler yacc pointer laundering.
//
// The generated parser in src/script/scr_yacc2.cpp passed full sval_u
// semantic values through narrow `.type` members in its argument/list
// actions (e.g. node1(yyvsp->val.type, ...) for what is really a
// pointer-bearing sval_u, not a bare Enum_t tag).  On ILP32 sval_u is 4
// bytes total so `.type` *was* the whole union and the copy happened to
// work; under LP64 sval_u is 8 bytes (it holds real sval_u*/const char*/
// scr_block_s* members) so `.type` keeps only the low 4 bytes and drops
// the high half of every AST pointer that flows through those actions.
// The fix builds full sval_u pairs (node_value_pair) plus a direct
// 2-slot script root instead.
//
// This test drives the REAL, shipped ScriptParse/yyparse (not a
// simulation of its actions) over the synthetic source
// `main(){ assert(isdefined(self)); }`, which exercises the repaired
// productions: the script root (case 1), thread/list plumbing, and the
// singleton/append argument pairs (cases 120-122) for both the outer
// `assert(...)` argument and the inner `isdefined(self)` argument.  It
// then verifies the nested AST pointers retained their upper bits.
//
// High-address precondition: HunkUser on 64-bit Linux mmaps above 4 GiB,
// so every Scr_AllocNode result here is naturally high and any `.type`
// truncation would visibly lose bits.  The test asserts this precondition
// loudly instead of silently passing on a low-address host where the bug
// would be invisible: every pointer slot checked below must read back
// above UINT32_MAX (truncation-sensitive: its low 32 alone differ), and
// the HunkUser base itself must already be high.
//
// Width discipline (learned from probing): pointer slots are checked full
// 64-bit; Enum_t tags on fresh Hunk memory are checked full 64-bit small
// (node builders write only `.type`, upper half stays zero-filled); string
// IDs and source positions are checked low-32 only, because yacc's
// short-lived `sval_u valstack` copies full 8-byte unions whose upper
// halves carry stale stack data for int-typed members -- production
// consumers read only the low 32 there, so the test does the same.
//
// Access convention: helpers take a heap allocation pointer plus a slot
// index (`parent[slot]`), uniformly.  The one stack value in play (the
// ScriptParse output root) is entered via `root.node`.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

typedef unsigned char byte;

#include <script/scr_parsetree.h>
#include <script/scr_vm.h>
#include <platform/switch/switch_hunk_user.h>
#include <script/scr_compiler.h>
#include <qcommon/com_error.h>

// Scr_AllocNode/Scr_InitAllocNode plus the real yyparse entry points under
// test never reach any of these -- they are pulled in only because the
// production translation units reference them.  Minimal link-satisfying
// stubs, not behavior under test (same set as
// switch_script_parsetree_test.cpp, plus the yacc-only needs below).
scrVmDebugPub_t scrVmDebugPub{};
scrCompilePub_t scrCompilePub{};
scrCompileGlob_t scrCompileGlob{};
void __cdecl Scr_ClearDebugExprValue(sval_u) {}
void __cdecl Scr_FreeDebugExprValue(sval_u) {}
void __cdecl G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void __cdecl CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void *Z_Malloc(int size, const char *, int) { return std::malloc(static_cast<size_t>(size)); }
void Z_Free(void *ptr, int) { std::free(ptr); }
void Com_Error(errorParm_t, const char *, ...)
{
    std::fprintf(stderr, "FAIL: parse path unexpectedly entered Com_Error\n");
    std::abort();
}

// The flex scanner's EOF path (scr_yacc2.cpp yy_get_next_buffer) takes its
// `yyrestart` loop unless a local client is active, in which case it emits
// the EOF token and yyparse terminates.  Production SP always has one
// during script load; the stub reports the same so a finite source parses
// instead of rescanning EOF forever.
int CL_GetLocalClientActiveCount() { return 1; }

// Minimal string interner standing in for the production SL system (which
// needs the full memory-tree owner this host test deliberately does not
// link).  The parser only stores the returned IDs and passes some through
// LowerCase; pointer preservation -- the property under test -- never
// depends on ID values.  IDs come out 1-based in lex order, which the
// checks below pin (`main`=1, `assert`=2, `isdefined`=3) to prove the walk
// reached the intended nodes rather than lookalikes.
static std::vector<std::string> g_strings;
uint32_t SL_GetString_(const char *str, uint32_t user, mtType_t type)
{
    (void)user; (void)type;
    g_strings.emplace_back(str ? str : "");
    return static_cast<uint32_t>(g_strings.size());
}
uint32_t SL_GetStringOfSize(const char *str, uint32_t user, uint32_t len, mtType_t type)
{
    (void)user; (void)type;
    g_strings.emplace_back(str ? std::string(str, len) : std::string());
    return static_cast<uint32_t>(g_strings.size());
}
uint32_t SL_ConvertToLowercase(uint32_t stringValue, uint32_t user, mtType_t type)
{
    (void)user; (void)type;
    return stringValue;
}

// Any CompileError on the fixed synthetic source is a test failure.
// Record loudly; the post-parse check turns it into FAIL: instead of a
// confusing downstream crash.
static bool g_compileFailed = false;
void CompileError(uint32_t sourcePos, const char *msg, ...)
{
    g_compileFailed = true;
    std::fprintf(stderr, "FAIL: unexpected CompileError pos=%u msg=%s\n", sourcePos, msg);
}

// Byte-identical input protocol to production Scr_LoadScriptInternal: the
// scanner first drains a one-character "+" (in_ptr), then the real source
// (parseBuf).  The replicated Scr_ScanFile body below matches
// src/script/scr_main.cpp so the lexer sees exactly what it sees live.
static char g_plus[] = "+";
static const char *g_source = "main(){ assert(isdefined(self)); }\n";

int Scr_ScanFile(unsigned char *buf, int max_size)
{
    char c = 42;
    int n = 0;
    for (n = 0; n < max_size; ++n)
    {
        c = *scrCompilePub.in_ptr++;
        if (!c || c == 10)
            break;
        buf[n] = static_cast<unsigned char>(c);
    }
    if (c == 10)
    {
        buf[n++] = static_cast<unsigned char>(c);
    }
    else if (!c)
    {
        if (scrCompilePub.parseBuf)
        {
            scrCompilePub.in_ptr = scrCompilePub.parseBuf;
            scrCompilePub.parseBuf = 0;
        }
        else
        {
            --scrCompilePub.in_ptr;
        }
    }
    return n;
}

void ScriptParse(sval_u *parseData, unsigned char user);

static int g_failures = 0;

static void Fail(const char *ctx, const char *detail)
{
    std::fprintf(stderr, "FAIL:YACC_PTR_LP64 %s: %s\n", ctx, detail);
    ++g_failures;
}

static bool InHunk(const void *p)
{
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a >= reinterpret_cast<uintptr_t>(g_allocNodeUser->buf) &&
           a < reinterpret_cast<uintptr_t>(g_allocNodeUser->pos);
}

// Pointer slot parent[slot]: must be a real high in-hunk node pointer
// whose upper bits matter (low32 alone differ).  With the old `.type`
// laundering this slot would read back low (high half zero-filled fresh
// Hunk), failing here.
static sval_u *CheckChild(const sval_u *parent, int slot, const char *ctx)
{
    if (slot < 0 || slot >= 9)
    {
        Fail(ctx, "slot index out of node range");
        return nullptr;
    }
    void *p = parent[slot].node;
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (p == nullptr)
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "slot[%d] unexpectedly null", slot);
        Fail(ctx, detail);
        return nullptr;
    }
    if (a <= UINT32_MAX)
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "slot[%d]=%p lost upper bits (truncated?)", slot, p);
        Fail(ctx, detail);
        return nullptr;
    }
    if (!InHunk(p))
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "slot[%d]=%p outside parse arenas", slot, p);
        Fail(ctx, detail);
        return nullptr;
    }
    if (static_cast<uint32_t>(a) == a)
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "slot[%d]=%p insensitive (upper bits zero?)", slot, p);
        Fail(ctx, detail);
        return nullptr;
    }
    return static_cast<sval_u *>(p);
}

// Tag slot on fresh Hunk memory: builders write only `.type`, upper stays
// zero, so the full 8-byte slot must equal the small tag exactly.
static void CheckTag(const sval_u *node, int expected, const char *ctx)
{
    uintptr_t raw = 0;
    std::memcpy(&raw, &node[0], sizeof(raw));
    if (raw != static_cast<uintptr_t>(expected))
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "tag 0x%llx, want %d", static_cast<unsigned long long>(raw), expected);
        Fail(ctx, detail);
    }
}

// Int slot (string ID / source position): low 32 only; upper may be stale
// valstack garbage, which production also ignores.
static uint32_t Low32(const sval_u *node, int slot)
{
    uint32_t v = 0;
    std::memcpy(&v, &node[slot], 4);
    return v;
}

static void CheckSmall(uint32_t v, uint32_t bound, const char *ctx, const char *what)
{
    if (v >= bound)
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "%s=%u out of small range (want <%u)", what, v, bound);
        Fail(ctx, detail);
    }
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (sizeof(sval_u) < sizeof(void *))
    {
        std::fprintf(stderr, "FAIL: sizeof(sval_u)=%zu narrower than pointer\n", sizeof(sval_u));
        return 1;
    }

    Scr_InitAllocNode();
    if (reinterpret_cast<uintptr_t>(g_allocNodeUser) <= UINT32_MAX ||
        reinterpret_cast<uintptr_t>(g_allocNodeUser->buf) <= UINT32_MAX)
    {
        std::fprintf(stderr, "FAIL:YACC_PTR_LP64 HunkUser not high (%p/%p); host cannot verify upper bits\n",
                     static_cast<void *>(g_allocNodeUser),
                     static_cast<void *>(g_allocNodeUser->buf));
        return 1;
    }

    scrCompilePub.in_ptr = g_plus;
    scrCompilePub.parseBuf = g_source;
    scrCompilePub.far_function_count = 0;
    sval_u root{};
    ScriptParse(&root, 0);
    if (g_compileFailed)
    {
        std::fprintf(stderr, "FAIL:YACC_PTR_LP64 CompileError fired on fixed source\n");
        return 1;
    }
    if (!root.node || !InHunk(root.node))
    {
        std::fprintf(stderr, "FAIL:YACC_PTR_LP64 root missing\n");
        return 1;
    }

    // Case 1 (script : include_list thread_list): untagged 2-slot root,
    // both payloads full sval_u.  Old code kept only low32 of slot 0.
    {
        uintptr_t r0 = 0, r1 = 0;
        std::memcpy(&r0, &root.node[0], 8);
        std::memcpy(&r1, &root.node[1], 8);
        if (r0 <= UINT32_MAX || !InHunk(root.node[0].node))
            Fail("script root slot[0] (include_list)", "not a high in-hunk pointer");
        if (r1 <= UINT32_MAX || !InHunk(root.node[1].node))
            Fail("script root slot[1] (thread_list)", "not a high in-hunk pointer");
        if (static_cast<uint32_t>(r0) == r0 || static_cast<uint32_t>(r1) == r1)
            Fail("script root", "insensitive upper bits");
    }

    // Thread list: header {head,tail} (2, untagged); elements {payload,next}
    // (2, untagged); exactly the sentinel-NOP plus one `main` thread, NULL
    // terminated (also re-pins the AllocNode sizing fix end to end).
    sval_u *threadNode = nullptr;
    {
        sval_u *header = CheckChild(root.node, 1, "thread_list header");
        if (header)
        {
            sval_u *head = CheckChild(header, 0, "thread_list head");
            sval_u *tail = CheckChild(header, 1, "thread_list tail");
            if (head && tail)
            {
                int elems = 0;
                int threads = 0;
                sval_u *last = nullptr;
                for (sval_u *e = head; e && elems < 16; ++elems)
                {
                    if (!InHunk(e))
                    {
                        Fail("thread_list elem", "outside hunk");
                        break;
                    }
                    sval_u *pay = e[0].node;
                    if (!pay || !InHunk(pay))
                    {
                        Fail("thread_list elem payload", "not high in-hunk");
                        break;
                    }
                    if (reinterpret_cast<uintptr_t>(pay) <= UINT32_MAX)
                    {
                        Fail("thread_list elem payload", "lost upper bits");
                        break;
                    }
                    uint32_t tag = 0;
                    std::memcpy(&tag, &pay[0], 4);
                    uintptr_t full = 0;
                    std::memcpy(&full, &pay[0], 8);
                    if (full <= UINT32_MAX && tag == 0x44)
                    {
                        ++threads;
                        threadNode = pay;
                    }
                    last = e;
                    if (e[1].node == nullptr)
                    {
                        ++elems;
                        break;
                    }
                    if (reinterpret_cast<uintptr_t>(e[1].node) <= UINT32_MAX || !InHunk(e[1].node))
                    {
                        Fail("thread_list elem next", "not high-or-null");
                        break;
                    }
                    e = e[1].node;
                }
                if (elems != 2)
                {
                    char d[96];
                    std::snprintf(d, sizeof(d), "want 2 elems (sentinel+main), got %d", elems);
                    Fail("thread_list shape", d);
                }
                if (last != tail)
                    Fail("thread_list tail", "tail is not the last element");
                if (threads != 1 || !threadNode)
                    Fail("thread_list", "want exactly one ENUM_thread payload");
            }
        }
    }

    // ENUM_thread (node6, 7 slots): [1]=`main` (ID 1), [2]/[3]=high
    // subtrees (params/body), [5]=small sourcePos.
    sval_u *bodyFromThread = nullptr;
    if (threadNode)
    {
        CheckTag(threadNode, 0x44, "thread tag");
        if (Low32(threadNode, 1) != 1)
            Fail("thread name", "want string ID 1 (`main`)");
        sval_u *c2 = CheckChild(threadNode, 2, "thread child[2]");
        sval_u *c3 = CheckChild(threadNode, 3, "thread child[3]");
        CheckSmall(Low32(threadNode, 5), 128, "thread srcPos", "srcPos");
        // Body side is the child leading to the assert statement; the other
        // side is the empty formal-parameter plumbing.  Probed shape puts
        // the body under child[3].
        bodyFromThread = c3;
        (void)c2;
    }

    // Body -> ... -> call_expression_statement (25) -> assert call (23).
    // Pair nodes on this path (thread-body prepends) are untagged 2-slots
    // whose [0] must stay high.
    sval_u *assertCall = nullptr;
    if (bodyFromThread)
    {
        uintptr_t b0 = 0, b1 = 0;
        std::memcpy(&b0, &bodyFromThread[0], 8);
        std::memcpy(&b1, &bodyFromThread[1], 8);
        if (b0 <= UINT32_MAX || !InHunk(bodyFromThread[0].node))
            Fail("thread body pair[0]", "not high");
        if (b1 <= UINT32_MAX || !InHunk(bodyFromThread[1].node))
            Fail("thread body pair[1]", "not high");
        // Probed shape: body[1]=P2, P2[0]=stmt(25).
        sval_u *p2 = bodyFromThread[1].node;
        sval_u *stmt = nullptr;
        if (p2 && InHunk(p2))
        {
            sval_u *cand = p2[0].node;
            if (cand && InHunk(cand))
            {
                uint32_t t0 = 0;
                uintptr_t f0 = 0;
                std::memcpy(&t0, &cand[0], 4);
                std::memcpy(&f0, &cand[0], 8);
                if (f0 <= UINT32_MAX && t0 == 25)
                    stmt = cand;
            }
            if (!stmt)
            {
                for (int s = 0; s < 2 && !stmt; ++s)
                {
                    sval_u *ch = p2[s].node;
                    if (ch && InHunk(ch))
                    {
                        uint32_t ct = 0;
                        uintptr_t cf = 0;
                        std::memcpy(&ct, &ch[0], 4);
                        std::memcpy(&cf, &ch[0], 8);
                        if (cf <= UINT32_MAX && ct == 25)
                            stmt = ch;
                    }
                }
            }
        }
        if (!stmt)
            Fail("statement", "no ENUM_call_expression_statement (25) under thread body");
        else
        {
            CheckTag(stmt, 25, "call_expression_statement");
            assertCall = CheckChild(stmt, 1, "assert call");
            if (assertCall)
                CheckTag(assertCall, 0x17, "assert ENUM_call");
        }
    }

    // assert call (node3, 4 slots): [1]=func chain (->ID 2), [2]=args (high),
    // [3]=small sourcePos.  Then args -> pair{primExpr,srcPos} (fixed case
    // 120 singleton) -> primExpr(6) -> callExpr(19) -> isdefined call(23).
    sval_u *isdefCall = nullptr;
    if (assertCall)
    {
        sval_u *func = CheckChild(assertCall, 1, "assert func");
        sval_u *args = CheckChild(assertCall, 2, "assert args");
        CheckSmall(Low32(assertCall, 3), 128, "assert srcPos", "srcPos");
        // Func chain must resolve to `assert' (ID 2) through high nodes.
        bool foundAssert = false;
        for (sval_u *f = func; f && InHunk(f);)
        {
            bool advanced = false;
            for (int s = 1; s < 4; ++s)
            {
                uintptr_t raw = 0;
                std::memcpy(&raw, &f[s], 8);
                if (raw > 0 && raw <= 16)
                {
                    if (raw == 2)
                        foundAssert = true;
                }
                if (raw > UINT32_MAX && InHunk(reinterpret_cast<void *>(raw)))
                {
                    if (!advanced)
                    {
                        f = reinterpret_cast<sval_u *>(raw);
                        advanced = true;
                    }
                }
                else if (s > 0 && raw > UINT32_MAX)
                {
                    // Int slots (IDs/positions) can carry stale valstack
                    // upper halves, which production reads low32-only and
                    // ignores; tolerate small low32s but flag large ones
                    // (a truncated hunk pointer would land here).
                    uint32_t lo = 0;
                    std::memcpy(&lo, &f[s], 4);
                    if (lo >= 1024)
                    {
                        char d[128];
                        std::snprintf(d, sizeof(d), "func slot[%d] suspicious raw=0x%llx", s,
                                      static_cast<unsigned long long>(raw));
                        Fail("assert func", d);
                    }
                }
            }
            if (!advanced)
                break;
            if (foundAssert)
                break;
        }
        if (!foundAssert)
            Fail("assert func", "ID 2 (`assert`) not reachable through high nodes");
        // Args: header -> head elem -> pair{primExpr,srcPos} (fixed site).
        if (args)
        {
            sval_u *head = CheckChild(args, 0, "assert args head");
            if (head)
            {
                sval_u *pair = CheckChild(head, 0, "assert arg pair (case 120)");
                if (pair)
                {
                    sval_u *prim = CheckChild(pair, 0, "assert arg value (primExpr)");
                    CheckSmall(Low32(pair, 1), 128, "assert arg pos", "srcPos");
                    if (prim)
                    {
                        CheckTag(prim, 6, "primExpr tag");
                        sval_u *ce = CheckChild(prim, 1, "primExpr inner (callExpr)");
                        if (ce)
                        {
                            CheckTag(ce, 0x13, "callExpr tag");
                            isdefCall = CheckChild(ce, 1, "isdefined call");
                            if (isdefCall)
                                CheckTag(isdefCall, 0x17, "isdefined ENUM_call");
                        }
                    }
                }
            }
        }
    }

    // isdefined call: [1]=func (->ID 3), [2]=args (->pair{primExpr,srcPos}
    // fixed case), [3]=small srcPos.  Inner pair value must stay high down
    // to `self' (tag 32).
    if (isdefCall)
    {
        sval_u *func = CheckChild(isdefCall, 1, "isdefined func");
        sval_u *args = CheckChild(isdefCall, 2, "isdefined args");
        CheckSmall(Low32(isdefCall, 3), 128, "isdefined srcPos", "srcPos");
        bool foundIsdef = false;
        for (sval_u *f = func; f && InHunk(f);)
        {
            bool advanced = false;
            for (int s = 1; s < 4; ++s)
            {
                uintptr_t raw = 0;
                std::memcpy(&raw, &f[s], 8);
                if (raw > 0 && raw <= 16)
                {
                    if (raw == 3)
                        foundIsdef = true;
                }
                if (raw > UINT32_MAX && InHunk(reinterpret_cast<void *>(raw)))
                {
                    if (!advanced)
                    {
                        f = reinterpret_cast<sval_u *>(raw);
                        advanced = true;
                    }
                }
                else if (s > 0 && raw > UINT32_MAX)
                {
                    // Same stale-upper tolerance as the assert func walk.
                    uint32_t lo = 0;
                    std::memcpy(&lo, &f[s], 4);
                    if (lo >= 1024)
                    {
                        char d[128];
                        std::snprintf(d, sizeof(d), "func slot[%d] suspicious raw=0x%llx", s,
                                      static_cast<unsigned long long>(raw));
                        Fail("isdefined func", d);
                    }
                }
            }
            if (!advanced)
                break;
            if (foundIsdef)
                break;
        }
        if (!foundIsdef)
            Fail("isdefined func", "ID 3 (`isdefined`) not reachable through high nodes");
        if (args)
        {
            sval_u *head = CheckChild(args, 0, "isdefined args head");
            if (head)
            {
                sval_u *pair = CheckChild(head, 0, "isdefined arg pair (case 120)");
                if (pair)
                {
                    sval_u *prim = CheckChild(pair, 0, "isdefined arg value (primExpr)");
                    CheckSmall(Low32(pair, 1), 128, "isdefined arg pos", "srcPos");
                    if (prim)
                    {
                        CheckTag(prim, 6, "inner primExpr tag");
                        sval_u *inner = CheckChild(prim, 1, "inner expr (`self`)");
                        if (inner)
                            CheckTag(inner, 0x20, "self tag");
                    }
                }
            }
        }
    }
    else if (!g_failures)
    {
        Fail("isdefined call", "unreachable");
    }

    Scr_ShutdownAllocNode();
    if (g_failures || g_compileFailed)
    {
        std::fprintf(stderr, "FAIL:SCRIPT_YACC_PTR_LP64 failures=%d compileFailed=%d\n",
                     g_failures, g_compileFailed ? 1 : 0);
        return 1;
    }
    std::printf("PASS:SCRIPT_YACC_PTR_LP64 root=high thread=main assert=isdefined(self)\n");
    return 0;
}
