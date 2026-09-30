// Host regression harness for the physics pool stride contract that the
// watermelon-melee crash broke (Atmosphere report: Data Abort in
// Pool_Alloc from Phys_CreateBodyFromState, reached by
// FX_SpawnModelPhysics).
//
// phys_ode.cpp initialized the PhysObjUserData pool with the decompiler's
// ILP32 literal 0x70 while PhysObjUserData (it holds a dxBody*) is 0x78 bytes
// on LP64.  Pool_Init builds the free list in `itemSize` steps, but every
// consumer memsets/writes sizeof(PhysObjUserData), so each item's tail
// overwrote the next item's `next` link and the chain decayed into garbage the
// moment the first physics body was created.
//
// This drives the real Pool_Init/Pool_Alloc/Pool_Free from
// src/universal/pool_allocator.cpp over a struct with the same shape (float
// blocks plus one pointer, so the native size exceeds the ILP32 literal) in
// both configurations: the corrected sizeof stride must survive a full
// alloc/fill/free/alloc cycle, and the 0x70-style stride must be caught by the
// intactness check -- which is what makes this a proof of the bug rather than
// of the fix alone.

#include <universal/q_shared.h>
#include <universal/pool_allocator.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

// Loud stubs for the engine services pool_allocator.cpp's asserts reach.
// Nothing on the alloc/free path under test is an engine service.
void MyAssertHandler(const char *file, int line, int type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "FAIL:PHYS_POOL_STRIDE assert %s:%d type=%d: ", file, line, type);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
    std::abort();
}

void Com_Error(int code, const char *fmt, ...)
{
    (void)code;
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "FAIL:PHYS_POOL_STRIDE Com_Error: ");
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
    std::abort();
}

// Same shape as PhysObjUserData: float blocks plus one pointer member, so the
// native layout is bigger than the 32-bit size the decompiler recorded.
struct ProbeUserData
{
    float translation[3];
    void *body;
    float savedPos[3];
    float savedRot[3][3];
    int sndClass;
    float friction;
    float bounce;
    int state;
    float contactCentroid[3];
    int timeLastAsleep;
    float awakeTooLongLastPos[3];
    bool hasDisplayedAwakeTooLongWarning;
    bool debugContacts;
};

static const uint32_t kItemCount = 64;
static char g_pool[sizeof(ProbeUserData) * kItemCount];
static ProbeUserData *g_seen[kItemCount];

static int g_failures = 0;

static void Fail(const char *detail)
{
    std::fprintf(stderr, "FAIL:PHYS_POOL_STRIDE %s\n", detail);
    ++g_failures;
}

// Allocates every item, writing exactly sizeof(ProbeUserData) bytes into each --
// what Phys_CreateBodyFromState does.  Returns false as soon as the free list
// yields a duplicate, an out-of-pool or misaligned pointer, or runs dry early:
// the observable shape of a corrupted chain.
static bool AllocAllAndFill(pooldata_t *pool, const char *label)
{
    const char *base = g_pool;
    uint32_t count = 0;

    for (uint32_t i = 0; i < kItemCount; ++i)
    {
        ProbeUserData *item = (ProbeUserData *)Pool_Alloc(pool);
        if (!item)
        {
            std::fprintf(stderr, "NOTE:PHYS_POOL_STRIDE %s ran dry after %u items\n", label, count);
            return false;
        }
        const char *p = (const char *)item;
        if (p < base || p >= base + sizeof(g_pool) || ((p - base) % sizeof(ProbeUserData)) != 0)
        {
            std::fprintf(stderr, "NOTE:PHYS_POOL_STRIDE %s handed out %p (outside/unaligned)\n", label,
                         (void *)item);
            return false;
        }
        for (uint32_t j = 0; j < count; ++j)
        {
            if (g_seen[j] == item)
            {
                std::fprintf(stderr, "NOTE:PHYS_POOL_STRIDE %s handed out %p twice (%u in)\n", label,
                             (void *)item, count);
                return false;
            }
        }
        g_seen[count++] = item;
        std::memset(item, 0, sizeof(ProbeUserData));
        // The pointer write is what reaches past the old 0x70 stride.
        item->body = item;
    }
    return count == kItemCount;
}

static void FreeAll(pooldata_t *pool)
{
    for (uint32_t i = 0; i < kItemCount; ++i)
    {
        if (g_seen[i])
            Pool_Free((freenode *)g_seen[i], pool);
        g_seen[i] = nullptr;
    }
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("PHYS_POOL_STRIDE sizeof(ProbeUserData)=%zu ilp32_literal=0x70\n",
                sizeof(ProbeUserData));
    if (sizeof(ProbeUserData) <= 0x70)
    {
        Fail("probe struct must be larger than the ILP32 literal for this proof to mean anything");
        std::printf("FAIL:PHYS_POOL_STRIDE failures=%d\n", g_failures);
        return 1;
    }

    // Correct configuration: stride == item size, over two full cycles.
    {
        pooldata_t pool{};
        Pool_Init(g_pool, &pool, sizeof(ProbeUserData), kItemCount);
        if (Pool_FreeCount(&pool) != kItemCount)
            Fail("sizeof stride: free count is not the full pool after init");
        if (!AllocAllAndFill(&pool, "sizeof") || pool.activeCount != (int)kItemCount)
            Fail("sizeof stride: first alloc/fill cycle did not hand out every item exactly once");
        FreeAll(&pool);
        if (Pool_FreeCount(&pool) != kItemCount || pool.activeCount != 0)
            Fail("sizeof stride: free list did not come back whole after freeing every item");
        if (!AllocAllAndFill(&pool, "sizeof-reuse") || pool.activeCount != (int)kItemCount)
            Fail("sizeof stride: second cycle did not reuse the pool intact");
        FreeAll(&pool);
        std::printf("PASS:PHYS_POOL_STRIDE_NATIVE sizeof=%zu items=%u\n", sizeof(ProbeUserData),
                    kItemCount);
    }

    // Pre-fix configuration: stride is the ILP32 literal, items are written at
    // their native size.  The intactness check must see the corruption.
    {
        pooldata_t pool{};
        std::memset(g_pool, 0, sizeof(g_pool));
        for (uint32_t i = 0; i < kItemCount; ++i)
            g_seen[i] = nullptr;
        Pool_Init(g_pool, &pool, 0x70u, kItemCount);
        if (AllocAllAndFill(&pool, "ilp32-literal"))
            Fail("ilp32 stride: corruption not detected (harness would not catch the regression)");
        else
            std::printf("PASS:PHYS_POOL_STRIDE_ILP32_CORRUPTION (old stride caught by the check)\n");
    }

    if (g_failures)
    {
        std::printf("FAIL:PHYS_POOL_STRIDE failures=%d\n", g_failures);
        return 1;
    }
    std::printf("PASS:PHYS_POOL_STRIDE\n");
    return 0;
}
