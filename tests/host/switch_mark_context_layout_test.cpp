// Host proof for the MarkModelCoreContext ILP32-offset crash (hardware,
// first bullet mark on the car).
//
// R_MarkModelCoreCallback_0_ used to read the context through the decompiler's
// 4-byte-pointer offsets (markContext at +4, markDir at +12, clipPlanes at
// +16, transformNormalMatrix at +24).  MarkModelCoreContext is seven pointers,
// so on LP64 those offsets land in the wrong fields and the `+12` read returns
// a value stitched from two fields -- a mangled markDir that Vec3Dot then
// dereferenced (Data Abort at 0).  This mirrors the struct and shows the old
// offset cannot name the field while the typed access does.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
// Mirror of src/EffectsCore/fx_system.h:303 MarkModelCoreContext (field order
// is checked by the source gate).
struct MarkModelCoreContext
{
    void *markInfo;
    void *markContext;
    const float *markOrigin;
    const float *markDir;
    const float (*clipPlanes)[4];
    const float (*transformMatrix)[3];
    const float (*transformNormalMatrix)[3];
};

static_assert(sizeof(MarkModelCoreContext) == 7 * sizeof(void *),
              "MarkModelCoreContext is seven pointers");
} // namespace

#define CHECK(cond) \
    do \
    { \
        if (!(cond)) \
        { \
            printf("FAIL:MARK_CONTEXT_LAYOUT %s (line %d)\n", #cond, __LINE__); \
            return 1; \
        } \
    } while (0)

int main()
{
    const size_t p = sizeof(void *);
    CHECK(offsetof(MarkModelCoreContext, markInfo) == 0);
    CHECK(offsetof(MarkModelCoreContext, markContext) == 1 * p);
    CHECK(offsetof(MarkModelCoreContext, markOrigin) == 2 * p);
    CHECK(offsetof(MarkModelCoreContext, markDir) == 3 * p);
    CHECK(offsetof(MarkModelCoreContext, clipPlanes) == 4 * p);
    CHECK(offsetof(MarkModelCoreContext, transformMatrix) == 5 * p);
    CHECK(offsetof(MarkModelCoreContext, transformNormalMatrix) == 6 * p);

    MarkModelCoreContext context;
    memset(&context, 0, sizeof(context));
    context.markInfo = (void *)0x1111;
    context.markContext = (void *)0x2222;
    context.markOrigin = (const float *)0x3333;
    context.markDir = (const float *)0x4444;
    context.clipPlanes = (const float (*)[4])0x5555;
    context.transformMatrix = (const float (*)[3])0x6666;
    context.transformNormalMatrix = (const float (*)[3])0x7777;

    // The typed access is exact.
    CHECK(context.markDir == (const float *)0x4444);
    CHECK(context.clipPlanes == (const float (*)[4])0x5555);
    CHECK(context.transformNormalMatrix == (const float (*)[3])0x7777);

    // The old ILP32 read of markDir at +12 must not name the field on LP64.
    const float *oldMarkDir = nullptr;
    memcpy(&oldMarkDir, (const char *)&context + 12, sizeof(oldMarkDir));
    int mismatch = 0;
    if (p == 8)
    {
        CHECK(oldMarkDir != context.markDir);
        mismatch = 1;
    }
    else
    {
        // On a 4-byte-pointer ABI the old offset is the field.
        CHECK(oldMarkDir == context.markDir);
    }

    printf("PASS:MARK_CONTEXT_LAYOUT ptr=%zu markDirOffset=%zu ilp32MarkDirOffset=12 mismatch=%d\n",
           p, offsetof(MarkModelCoreContext, markDir), mismatch);
    return 0;
}
