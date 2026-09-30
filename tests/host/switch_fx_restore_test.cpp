// Host ASan/UBSan proof for the FX savegame round trip on LP64.
//
// FX_Save stores the FxSystem record with the saving process's absolute
// visStateBufferRead/Write pointers plus a 32-bit "system address" token.
// The retail FX_Restore relocated both pointers by `system - token`, which
// on LP64 with objects above 4 GiB adds the whole high half of the address:
// on hardware (image above 4 GiB) this corrupted visStateBufferWrite on
// every G_LoadGame and the render worker's FX_ToggleVisBlockerFrame
// faulted.  Some emulators load the NRO below 4 GiB, so only a host proof with
// high addresses sees it.
//
// This runs the real FX_Save/FX_Restore (fx_archive.cpp) through a byte
// buffer, restoring (a) into the same objects and (b) into a second pair at
// a different address (a later boot), for both double-buffer phases, and
// requires the restored pointers to name the live visState[0]/[1] in the
// saved order.  A corrupt record (pointers outside the saved buffers) must
// be refused with Com_Error, not accepted.

#include <csetjmp>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "src/EffectsCore/fx_system.h"
#include "src/physics/phys_local.h"

namespace
{
std::vector<uint8_t> g_bytes;
size_t g_readPos;
FxSystem *g_system;
FxSystemBuffers *g_buffers;
bool g_expectError;
bool g_sawError;
jmp_buf g_errorJump;
} // namespace

void Com_Error(errorParm_t, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (!g_expectError)
    {
        std::fprintf(stderr, "FAIL:FX_RESTORE_LP64 stage=unexpected-com-error text=%s\n", text);
        std::exit(1);
    }
    g_sawError = true;
    std::longjmp(g_errorJump, 1);
}

void Com_Printf(int, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
}

void MyAssertHandler(const char *file, int line, int, const char *fmt, ...)
{
    std::fprintf(stderr, "FAIL:FX_RESTORE_LP64 stage=assert file=%s line=%d fmt=%s\n", file, line, fmt);
    std::exit(1);
}

void MemFile_WriteData(MemoryFile *, int byteCount, const void *p)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(p);
    g_bytes.insert(g_bytes.end(), bytes, bytes + byteCount);
}

void MemFile_WriteCString(MemoryFile *memFile, const char *string)
{
    MemFile_WriteData(memFile, (int)std::strlen(string) + 1, string);
}

void MemFile_ReadData(MemoryFile *, int byteCount, uint8_t *p)
{
    if (g_readPos + (size_t)byteCount > g_bytes.size())
    {
        std::fprintf(stderr, "FAIL:FX_RESTORE_LP64 stage=read-past-end\n");
        std::exit(1);
    }
    std::memcpy(p, g_bytes.data() + g_readPos, (size_t)byteCount);
    g_readPos += (size_t)byteCount;
}

const char *MemFile_ReadCString(MemoryFile *)
{
    const char *s = reinterpret_cast<const char *>(g_bytes.data() + g_readPos);
    g_readPos += std::strlen(s) + 1;
    return s;
}

FxSystem *FX_GetSystem(int32_t) { return g_system; }
FxSystemBuffers *FX_GetSystemBuffers(int32_t) { return g_buffers; }

// Same body as fx_system.cpp's (which drags in the whole FX runtime).
void FX_LinkSystemBuffers(FxSystem *system, FxSystemBuffers *systemBuffers)
{
    system->elems = systemBuffers->elems;
    system->effects = systemBuffers->effects;
    system->trails = systemBuffers->trails;
    system->trailElems = systemBuffers->trailElems;
    system->visState = systemBuffers->visState;
    system->deferredElems = systemBuffers->deferredElems;
}

// No effect definitions and no active effects: the record is the FxSystem
// and FxSystemBuffers blobs plus the token.
static dvar_t s_useFastFile = [] {
    dvar_t d{};
    d.current.enabled = true;
    return d;
}();
const dvar_t *useFastFile = &s_useFastFile;
int MemFile_GetUsedSize(MemoryFile *) { return (int)g_bytes.size(); }
char *va(const char *format, ...)
{
    static char text[256];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    return text;
}
void DB_EnumXAssets(XAssetType, void(__cdecl *)(XAssetHeader, void *), void *, bool) {}
void FX_ForEachEffectDef(void(__cdecl *)(const FxEffectDef *, void *), void *) {}
const FxEffectDef *FX_Register(const char *) { return nullptr; }
dxBody *Phys_ObjLoad(PhysWorld, MemoryFile *) { return nullptr; }
void Phys_ObjSetCollisionFromXModel(const XModel *, PhysWorld, dxBody *) {}
void Phys_ObjSave(dxBody *, MemoryFile *) {}
const float fx_randomTable[507]{};

namespace
{
struct Pair
{
    FxSystem *system;
    FxSystemBuffers *buffers;
};

Pair MakePair()
{
    Pair pair{};
    pair.system = static_cast<FxSystem *>(std::calloc(1, sizeof(FxSystem)));
    pair.buffers = static_cast<FxSystemBuffers *>(std::calloc(1, sizeof(FxSystemBuffers)));
    FX_LinkSystemBuffers(pair.system, pair.buffers);
    pair.system->isInitialized = 1;
    return pair;
}

bool High(const void *p) { return reinterpret_cast<uintptr_t>(p) > UINT32_MAX; }

int Fail(const char *stage, int phase)
{
    std::fprintf(stderr, "FAIL:FX_RESTORE_LP64 stage=%s phase=%d\n", stage, phase);
    return 1;
}

int RoundTrip(Pair saver, Pair loader, int phase, bool corrupt)
{
    g_bytes.clear();
    g_readPos = 0;
    saver.system->visStateBufferRead = saver.system->visState + phase;
    saver.system->visStateBufferWrite = saver.system->visState + (phase ^ 1);
    saver.system->isArchiving = 0;
    g_system = saver.system;
    g_buffers = saver.buffers;
    MemoryFile memFile{};
    FX_Save(0, &memFile);
    if (corrupt)
    {
        // Point the saved write buffer one FxVisState past visState[1].
        FxSystem record;
        size_t at = 1; // FX_SaveEffectDefTable's terminating ""
        std::memcpy(&record, g_bytes.data() + at, sizeof(record));
        record.visStateBufferWrite = record.visState + 2;
        std::memcpy(g_bytes.data() + at, &record, sizeof(record));
    }

    std::memset(loader.system, 0x5a, sizeof(FxSystem));
    g_system = loader.system;
    g_buffers = loader.buffers;
    g_expectError = corrupt;
    g_sawError = false;
    if (setjmp(g_errorJump) == 0)
        FX_Restore(0, &memFile);
    g_expectError = false;
    if (corrupt)
        return g_sawError ? 0 : Fail("corrupt-record-accepted", phase);

    FxSystem *s = loader.system;
    if (s->visState != loader.buffers->visState)
        return Fail("visstate-not-linked", phase);
    if (s->visStateBufferRead != loader.buffers->visState + phase)
    {
        std::fprintf(stderr, "read=%p expected=%p\n", (const void *)s->visStateBufferRead,
                     (const void *)(loader.buffers->visState + phase));
        return Fail("read-buffer", phase);
    }
    if (s->visStateBufferWrite != loader.buffers->visState + (phase ^ 1))
    {
        std::fprintf(stderr, "write=%p expected=%p\n", (void *)s->visStateBufferWrite,
                     (void *)(loader.buffers->visState + (phase ^ 1)));
        return Fail("write-buffer", phase);
    }
    if (s->isArchiving)
        return Fail("archiving-left-set", phase);
    // What the render worker's FX_ToggleVisBlockerFrame does next.
    s->visStateBufferWrite->blockerCount = 0;
    return 0;
}
} // namespace

int main()
{
#if UINTPTR_MAX == UINT32_MAX
    std::fprintf(stderr, "FAIL:FX_RESTORE_LP64 stage=host-not-lp64\n");
    return 1;
#else
    Pair a = MakePair();
    Pair b = MakePair();
    if (!High(a.system) || !High(a.buffers) || !High(b.system) || !High(b.buffers))
    {
        std::fprintf(stderr, "FAIL:FX_RESTORE_LP64 stage=fixture-below-4g %p %p\n", (void *)a.system,
                     (void *)a.buffers);
        return 1;
    }
    for (int phase = 0; phase < 2; ++phase)
    {
        if (RoundTrip(a, a, phase, false) || RoundTrip(a, b, phase, false) || RoundTrip(b, a, phase, false))
            return 1;
    }
    if (RoundTrip(a, b, 0, true))
        return 1;
    std::printf("PASS:FX_RESTORE_LP64 roundtrips=6 corrupt_refused=1 system=%p\n", (void *)a.system);
    return 0;
#endif
}
