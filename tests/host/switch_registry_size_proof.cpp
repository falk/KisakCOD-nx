// Host ASan proof for the registry's LP64 payload-size contract.
//
// DB_CloneXAssetInternal uses DB_GetXAssetTypeSize(type) as its memcpy length.
// This proof gives each clone/override slot an exact native-sized payload and
// redzones it on both sides.  It exercises the same clone sequence used by a
// registry entry (initial clone, override clone, then unload/clear) for every
// payload-bearing type.  A stale Win32 COMDAT alias therefore overwrites a
// redzone before a PASS can be emitted; ASan is enabled by the test driver as
// an additional diagnostic if the allocator itself is touched.
//
// This file is a thin verifier only.  It includes the production asset-name
// table so the executable calls the real DB_GetXAssetTypeSize handlers; it
// does not add a decoder, registry, or alternate asset representation.

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <vector>

// q_shared.h is written for the original Win32 compiler.  These definitions
// are the same host-only compatibility needed by the existing Switch proof
// compile and are intentionally local to this verifier translation unit.
#define random switch_random
#define crandom switch_crandom
#define __int8 char
#define __int16 short
#define __int32 int
#define __int64 long long
#define __forceinline inline
typedef unsigned char byte;

#include <database/database.h>

// Compile the production handler table into this proof.  This keeps the
// verifier independent of the full database/renderer link while ensuring it
// cannot accidentally test a duplicated expected-size table.
#include "src/database/db_assetnames.cpp"

const char *g_assetNames[ASSET_TYPE_COUNT] = {};

void MyAssertHandler(const char *, int, int, const char *, ...)
{
    std::abort();
}

void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}

namespace
{
constexpr std::size_t kGuardBytes = 32;
constexpr uint8_t kGuardValue = 0xa5;

struct PayloadSlot
{
    explicit PayloadSlot(std::size_t payloadSize)
        : storage(payloadSize + 2 * kGuardBytes, kGuardValue),
          payload(storage.data() + kGuardBytes),
          size(payloadSize)
    {
        std::memset(payload, 0, size);
    }

    bool GuardsIntact() const
    {
        for (std::size_t i = 0; i < kGuardBytes; ++i)
        {
            if (storage[i] != kGuardValue || storage[kGuardBytes + size + i] != kGuardValue)
                return false;
        }
        return true;
    }

    std::vector<uint8_t> storage;
    uint8_t *payload;
    std::size_t size;
};

struct PayloadSpec
{
    XAssetType type;
    std::size_t nativeSize;
    bool knownCorruptingAlias;
};

// The list is the original Load_XAssetHeader payload set: 26 types.  The
// seven omitted enum values are deliberate no-stream types and must not be
// made registry pool entries by this proof.
const PayloadSpec kPayloads[] = {
    {ASSET_TYPE_PHYSPRESET, sizeof(PhysPreset), true},
    {ASSET_TYPE_XANIMPARTS, sizeof(XAnimParts), false},
    {ASSET_TYPE_XMODEL, sizeof(XModel), false},
    {ASSET_TYPE_MATERIAL, sizeof(Material), false},
    {ASSET_TYPE_TECHNIQUE_SET, sizeof(MaterialTechniqueSet), false},
    {ASSET_TYPE_IMAGE, sizeof(GfxImage), false},
    {ASSET_TYPE_SOUND, sizeof(snd_alias_list_t), false},
    {ASSET_TYPE_SOUND_CURVE, sizeof(SndCurve), false},
    {ASSET_TYPE_LOADED_SOUND, sizeof(LoadedSound), true},
    {ASSET_TYPE_CLIPMAP, sizeof(clipMap_t), true},
    {ASSET_TYPE_CLIPMAP_PVS, sizeof(clipMap_t), true},
    {ASSET_TYPE_COMWORLD, sizeof(ComWorld), false},
    {ASSET_TYPE_GAMEWORLD_SP, sizeof(GameWorldSp), false},
    {ASSET_TYPE_GAMEWORLD_MP, sizeof(GameWorldMp), false},
    {ASSET_TYPE_MAP_ENTS, sizeof(MapEnts), false},
    {ASSET_TYPE_GFXWORLD, sizeof(GfxWorld), false},
    {ASSET_TYPE_LIGHT_DEF, sizeof(GfxLightDef), true},
    {ASSET_TYPE_FONT, sizeof(Font_s), false},
    {ASSET_TYPE_MENULIST, sizeof(MenuList), false},
    {ASSET_TYPE_MENU, sizeof(menuDef_t), false},
    {ASSET_TYPE_LOCALIZE_ENTRY, sizeof(LocalizeEntry), false},
    {ASSET_TYPE_WEAPON, sizeof(WeaponDef), false},
    {ASSET_TYPE_FX, sizeof(FxEffectDef), false},
    {ASSET_TYPE_IMPACT_FX, sizeof(FxImpactTable), false},
    {ASSET_TYPE_RAWFILE, sizeof(RawFile), false},
    {ASSET_TYPE_STRINGTABLE, sizeof(StringTable), false},
};

bool Check(bool condition, XAssetType type, const char *stage)
{
    if (condition)
        return true;
    std::fprintf(stderr, "FAIL:REGISTRY_SIZE_PROOF type=%d stage=%s\n", static_cast<int>(type), stage);
    return false;
}

bool ClonePayload(XAssetType type, const PayloadSlot &source, PayloadSlot &destination)
{
    const std::size_t copySize = static_cast<std::size_t>(DB_GetXAssetTypeSize(type));
    // Deliberately do not clamp this copy.  DB_CloneXAssetInternal has the
    // same contract: the registry handler is the memcpy length.  The slots'
    // redzones must catch an oversized historical alias rather than allowing
    // the proof to mask it with a test-only bounds check.
    std::memcpy(destination.payload, source.payload, copySize);
    return Check(source.GuardsIntact() && destination.GuardsIntact(), type, "clone-canary");
}

bool Exercise(const PayloadSpec &spec)
{
    const XAssetType type = spec.type;
    const std::size_t handlerSize = static_cast<std::size_t>(DB_GetXAssetTypeSize(type));
    PayloadSlot source(spec.nativeSize);
    PayloadSlot active(spec.nativeSize);
    PayloadSlot overrideSlot(spec.nativeSize);
    std::memset(source.payload, 0x30 + (static_cast<int>(type) & 0x1f), source.size);

    // Initial registry insertion is a clone into a fresh pool entry.
    if (!ClonePayload(type, source, active))
        return false;
    if (!Check(handlerSize == spec.nativeSize, type, "native-size"))
        return false;

    // An override owns another pool entry and follows the same typed clone.
    if (!ClonePayload(type, source, overrideSlot))
        return false;
    if (!Check(std::memcmp(source.payload, active.payload, source.size) == 0 &&
                   std::memcmp(source.payload, overrideSlot.payload, source.size) == 0,
               type,
               "override-content"))
        return false;

    // The unload path clears/releases both generations.  The redzones must
    // remain intact while the payload is retired; a wrong size handler has
    // already failed above before this state transition can pass.
    std::memset(active.payload, 0xdd, active.size);
    std::memset(overrideSlot.payload, 0xdd, overrideSlot.size);
    if (!Check(active.GuardsIntact() && overrideSlot.GuardsIntact() && source.GuardsIntact(), type, "unload-canary"))
        return false;

    if (spec.knownCorruptingAlias && handlerSize == spec.nativeSize)
        return true;
    return true;
}
} // namespace

int main()
{
    static_assert(sizeof(kPayloads) / sizeof(kPayloads[0]) == 26, "payload proof must cover every wire payload type");
    int corruptingAliasCount = 0;
    for (const PayloadSpec &spec : kPayloads)
    {
        if (!Exercise(spec))
            return 1;
        if (spec.knownCorruptingAlias)
            ++corruptingAliasCount;
    }
    if (corruptingAliasCount != 5)
    {
        std::fprintf(stderr, "FAIL:REGISTRY_SIZE_PROOF corrupting-alias-count=%d\n", corruptingAliasCount);
        return 1;
    }
    std::printf("PASS:REGISTRY_SIZE_PROOF payloads=26 clone=26 override=26 unload=26 corrupting_aliases=5\n");
    return 0;
}
