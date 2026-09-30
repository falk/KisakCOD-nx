// Regression test for the Scr_AllocNode LP64 bug: Scr_AllocNode(size) used to
// allocate `4 * size` bytes, correct only when sizeof(sval_u) == 4 (the
// ILP32 reference ABI). Under LP64 sval_u is 8 bytes (it holds real
// pointer members: sval_u*, const char*, scr_block_s*), so every parse
// node was allocated at half its real size out of Scr_AllocNode's bump
// allocator -- the next allocation silently overwrote the tail of the
// previous one. This is exactly the pattern that turned
// linked_list_end's NULL list terminator into a self-referential pointer
// and hung the GSC compiler forever on every real script.
//
// This test drives the real, shipped Scr_AllocNode/Scr_InitAllocNode
// against the real HunkUser allocator (not a simulation stand-in): it
// allocates a sequence of differently-sized nodes, fully writes every
// byte of each one, and asserts that no earlier node's bytes were ever
// altered by a later allocation -- which is exactly the corruption this
// bug produced and would remain silent under an allocator that merely
// avoids crashing.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

// q_shared.h is written for the original Win32 compiler. The same
// host-only compatibility macros switch_registry_size_proof.cpp uses for
// pulling in <database/database.h> (which scr_parsetree.h drags in
// transitively) on a host compiler are passed on the command line here
// instead of defined in this file, since scr_parsetree.cpp (compiled and
// linked as its own translation unit below, to test the real function,
// not a copy of it) needs the identical macros too.
typedef unsigned char byte;

#include <script/scr_parsetree.h>
#include <script/scr_vm.h>
#include <platform/switch/switch_hunk_user.h>
#include <qcommon/com_error.h>

// Scr_AllocNode/Scr_InitAllocNode/Scr_ShutdownAllocNode (the only real
// scr_parsetree.cpp entry points this test exercises) never reach any of
// these -- they're pulled in only because they're referenced elsewhere in
// the same translation unit (debugger-support and combat/physics paths
// this test never calls). Minimal link-satisfying stubs, not behavior
// under test.
scrVmDebugPub_t scrVmDebugPub{};
void __cdecl Scr_ClearDebugExprValue(sval_u) {}
void __cdecl Scr_FreeDebugExprValue(sval_u) {}
void __cdecl G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void __cdecl CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void *Z_Malloc(int size, const char *, int) { return std::malloc(static_cast<size_t>(size)); }
void Z_Free(void *ptr, int) { std::free(ptr); }

// scr_parsetree.cpp's new fail-loud guards call Com_Error; the tested
// entry points must never reach it (the test drives the success path), so
// this is a link-satisfying abort, not behavior under test.
void Com_Error(errorParm_t, const char *, ...)
{
    std::fprintf(stderr, "FAIL: Scr_AllocNode path unexpectedly entered Com_Error\n");
    std::abort();
}

int main()
{
    if (sizeof(sval_u) < sizeof(void *))
    {
        std::fprintf(stderr,
                      "FAIL: sizeof(sval_u)=%zu is narrower than a pointer (%zu) on this "
                      "build -- sval_u holds real pointer members and must never be\n",
                      sizeof(sval_u), sizeof(void *));
        return 1;
    }

    Scr_InitAllocNode();

    // Mirrors real grammar-action node sizes (node0/node1/.../node8 exist
    // in scr_parsetree.cpp up to 9 slots; exercise a representative
    // spread, largest first isn't required -- corruption showed up
    // between *any* two adjacent allocations).
    const int counts[] = {1, 2, 3, 5, 7, 9, 2, 4, 1, 6};
    const int kNodeCount = sizeof(counts) / sizeof(counts[0]);
    sval_u *nodes[kNodeCount];

    for (int i = 0; i < kNodeCount; ++i)
    {
        nodes[i] = Scr_AllocNode(counts[i]);
        if (!nodes[i])
        {
            std::fprintf(stderr, "FAIL: Scr_AllocNode(%d) returned null at index %d\n",
                         counts[i], i);
            return 1;
        }
        // Fill every requested slot with a distinct, recognizable pattern
        // (including the pointer-typed member, the one the original bug
        // silently truncated/overwrote) so a later corruption is visible.
        for (int slot = 0; slot < counts[i]; ++slot)
        {
            nodes[i][slot].node = reinterpret_cast<sval_u *>(
                static_cast<uintptr_t>(0x1000 + i) << 20 | static_cast<uintptr_t>(slot));
        }
    }

    int failures = 0;
    for (int i = 0; i < kNodeCount; ++i)
    {
        for (int slot = 0; slot < counts[i]; ++slot)
        {
            const uintptr_t expected =
                (static_cast<uintptr_t>(0x1000 + i) << 20) | static_cast<uintptr_t>(slot);
            const uintptr_t actual = reinterpret_cast<uintptr_t>(nodes[i][slot].node);
            if (actual != expected)
            {
                std::fprintf(stderr,
                             "FAIL: node %d slot %d corrupted: expected 0x%zx got 0x%zx "
                             "(a later Scr_AllocNode call overwrote this allocation's tail -- "
                             "the exact failure mode of the sizeof(sval_u) LP64 bug)\n",
                             i, slot, expected, actual);
                ++failures;
            }
        }
    }

    Scr_ShutdownAllocNode();

    if (failures)
    {
        std::fprintf(stderr, "FAIL:SCR_ALLOC_NODE_LP64 failures=%d\n", failures);
        return 1;
    }

    std::printf("PASS:SCR_ALLOC_NODE_LP64 sizeof_sval_u=%zu nodes=%d\n",
                sizeof(sval_u), kNodeCount);
    return 0;
}
