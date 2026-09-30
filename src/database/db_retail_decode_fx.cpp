#include "db_retail_decode_fx.h"

#include "db_retail_decode_world.h"
#include "db_retail_wire.h"
#include "database.h"

#include "../gfx_d3d/fxprimitives.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

// B6 FxEffectDef identity slice. The walk-only reader
// (ReadRetailFxEffectDefBody + ReadRetailFxElemDefBody in db_retail_walk.cpp)
// consumes the body byte-exactly and stays the independent oracle; this
// file only widens the bytes that reader leaves in the session mirror into
// native structures, then registers through the engine's existing
// Load_FxEffectDefAsset owner.
namespace
{
constexpr uint32_t kInlineRef = 0xffffffffu;
constexpr uint32_t kInsertRef = 0xfffffffeu;
constexpr uint32_t kRootBytes = 32;
constexpr uint32_t kElemBytes = 252;

// ILP32 FxElemDef offsets (native layout on the 32-bit reference, where
// Load_FxElemDef's 252-byte Load_Stream is a direct memcpy). Read from the
// serialized record the walk reader already validated.
constexpr uint32_t kWireElemPodPrefix = 180;      // flags..visStateIntervalCount
constexpr uint32_t kWireElemVelSamples = 180;
constexpr uint32_t kWireElemVisSamples = 184;
constexpr uint32_t kWireElemVisuals = 188;
constexpr uint32_t kWireElemCollMins = 192;
constexpr uint32_t kWireElemCollMaxs = 204;
constexpr uint32_t kWireElemEffectOnImpact = 216;
constexpr uint32_t kWireElemEffectOnDeath = 220;
constexpr uint32_t kWireElemEffectEmitted = 224;
constexpr uint32_t kWireElemEmitDist = 228;
constexpr uint32_t kWireElemEmitDistVariance = 236;
constexpr uint32_t kWireElemTrailDef = 244;
constexpr uint32_t kWireElemTail = 248;           // sortOrder..unused

// The native prefix up to the first pointer field must be byte-identical to
// the wire record: every preceding field is a same-size POD, so a future
// struct/layout change fails the build instead of mis-widening.
static_assert(kWireElemPodPrefix == 180, "wire prefix pinned");
static_assert(offsetof(FxElemDef, visualCount) == 177, "FxElemDef prefix drift");
static_assert(offsetof(FxElemDef, velIntervalCount) == 178, "FxElemDef prefix drift");
static_assert(offsetof(FxElemDef, visStateIntervalCount) == 179, "FxElemDef prefix drift");

uint32_t ReadLe32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void RewindTemp(RetailZoneLoadSession *session, uint32_t savedCursor0)
{
    RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
}

void WidenElemDefPrefix(FxElemDef *native, const uint8_t *wire)
{
    // Native offsets 0..179 equal the wire offsets (verified above); the
    // pointer/sample/visual/effect/trail fields stay null from the caller's
    // zero-fill and are counted as deferred nested objects.
    std::memcpy(native, wire, kWireElemPodPrefix);
    std::memcpy(reinterpret_cast<uint8_t *>(native) + offsetof(FxElemDef, collMins),
                wire + kWireElemCollMins, 12);
    std::memcpy(reinterpret_cast<uint8_t *>(native) + offsetof(FxElemDef, collMaxs),
                wire + kWireElemCollMaxs, 12);
    std::memcpy(reinterpret_cast<uint8_t *>(native) + offsetof(FxElemDef, emitDist),
                wire + kWireElemEmitDist, 8);
    std::memcpy(reinterpret_cast<uint8_t *>(native) + offsetof(FxElemDef, emitDistVariance),
                wire + kWireElemEmitDistVariance, 8);
    std::memcpy(reinterpret_cast<uint8_t *>(native) + offsetof(FxElemDef, sortOrder),
                wire + kWireElemTail, 4);
}

// B6/B7 nested FxElemDef sub-objects: the walk reader already streamed the
// velocity/vis-state sample arrays, the effect-name strings, and the trail
// verts/inds into the block-4 mirror; copy the POD spans out and bind the
// sample/trail/effect-name pointers. The visual union (material/model/mark
// slots) was live-widened by ReadRetailFxElemDefBody itself (it owns the
// worldContext and the exact stream position); this only assembles the
// already-widened pointers into the native FxElemDefVisuals shape.
//
// Load_FxEffectDefFromName's contract: an effect name string becomes the
// registered FX asset header.  Zone link order puts referenced effects
// before their users, so a miss here is a real missing asset (the registry
// answers with its default entry and prints, exactly as the original).
static const FxEffectDef *RetailResolveEffectRef(const char *name)
{
    if (!name || !name[0])
        return nullptr;
    const FxEffectDef *fx = DB_FindXAssetHeader(ASSET_TYPE_FX, name).fx;
    if (!fx)
        Com_Printf(0, "RetailResolveEffectRef: no FX asset for '%s'\n", name);
    return fx;
}

void WidenElemNested(RetailZoneLoadSession *session, FxElemDef *native,
                     const uint8_t *wire, const RetailWalkFxElemOffsets &off)
{
    const uint8_t *block4 = session->zoneMemory->blocks[4].data;
    if (off.velSamples != UINT32_MAX)
    {
        const uint32_t count = static_cast<uint32_t>(wire[178]) + 1u;
        FxElemVelStateSample *samples = static_cast<FxElemVelStateSample *>(
            RetailZoneLoadSessionAlloc(session, count * sizeof(FxElemVelStateSample),
                                       alignof(FxElemVelStateSample)));
        if (samples)
        {
            std::memcpy(samples, block4 + off.velSamples,
                        count * sizeof(FxElemVelStateSample));
            native->velSamples = samples;
        }
    }
    if (off.visSamples != UINT32_MAX)
    {
        const uint32_t count = static_cast<uint32_t>(wire[179]) + 1u;
        FxElemVisStateSample *samples = static_cast<FxElemVisStateSample *>(
            RetailZoneLoadSessionAlloc(session, count * sizeof(FxElemVisStateSample),
                                       alignof(FxElemVisStateSample)));
        if (samples)
        {
            std::memcpy(samples, block4 + off.visSamples,
                        count * sizeof(FxElemVisStateSample));
            native->visSamples = samples;
        }
    }
    // The original Load_FxEffectDefRef loads the name XString and then
    // Load_FxEffectDefFromName replaces it in place with the FX asset header
    // (the union's .handle).  The runtime only ever reads .handle
    // (FX_EffectAffectsGameplay, FX_SpawnEffect's onDeath/onImpact/emitted
    // spawns), so leaving the char* here made every effect that references
    // another effect walk a string as an FxEffectDef.
    if (off.effectOnImpact != UINT32_MAX)
        native->effectOnImpact.handle = RetailResolveEffectRef(
            reinterpret_cast<const char *>(block4 + off.effectOnImpact));
    if (off.effectOnDeath != UINT32_MAX)
        native->effectOnDeath.handle = RetailResolveEffectRef(
            reinterpret_cast<const char *>(block4 + off.effectOnDeath));
    if (off.effectEmitted != UINT32_MAX)
        native->effectEmitted.handle = RetailResolveEffectRef(
            reinterpret_cast<const char *>(block4 + off.effectEmitted));
    if (off.trailDef != UINT32_MAX)
    {
        FxTrailDef *trail = static_cast<FxTrailDef *>(
            RetailZoneLoadSessionAlloc(session, sizeof(FxTrailDef), alignof(FxTrailDef)));
        if (trail)
        {
            // Field-by-field: the wire record is ILP32 (4-byte refs) and the
            // native struct is LP64 (8-byte pointers), so a memcpy would put
            // indCount/inds at the wrong offsets.
            const uint8_t *wireTrail = block4 + off.trailDef;
            trail->scrollTimeMsec = static_cast<int>(ReadLe32(wireTrail));
            trail->repeatDist = static_cast<int>(ReadLe32(wireTrail + 4));
            trail->splitDist = static_cast<int>(ReadLe32(wireTrail + 8));
            trail->vertCount = static_cast<int>(ReadLe32(wireTrail + 12));
            trail->verts = nullptr;
            trail->indCount = static_cast<int>(ReadLe32(wireTrail + 20));
            trail->inds = nullptr;
            if (off.trailVerts != UINT32_MAX && trail->vertCount > 0)
            {
                FxTrailVertex *verts = static_cast<FxTrailVertex *>(
                    RetailZoneLoadSessionAlloc(session,
                                               static_cast<std::size_t>(trail->vertCount) *
                                                   sizeof(FxTrailVertex),
                                               alignof(FxTrailVertex)));
                if (verts)
                {
                    std::memcpy(verts, block4 + off.trailVerts,
                                static_cast<std::size_t>(trail->vertCount) *
                                    sizeof(FxTrailVertex));
                    trail->verts = verts;
                }
            }
            if (off.trailInds != UINT32_MAX && trail->indCount > 0)
            {
                uint16_t *inds = static_cast<uint16_t *>(RetailZoneLoadSessionAlloc(
                    session, static_cast<std::size_t>(trail->indCount) * sizeof(uint16_t),
                    alignof(uint16_t)));
                if (inds)
                {
                    std::memcpy(inds, block4 + off.trailInds,
                                static_cast<std::size_t>(trail->indCount) * sizeof(uint16_t));
                    trail->inds = inds;
                }
            }
            native->trailDef = trail;
        }
    }

    // B7 general fix: assemble the live-widened visual union. elemType/
    // visualCount are already correct (copied verbatim by
    // WidenElemDefPrefix, called before this). Elem types 6/7 (no visual
    // asset), 8/10 (string visuals, bound elsewhere) and a null
    // off.visual*/off.visualMarks (declared-null slot, or a pure walk-only
    // call with no worldContext) leave native->visuals zeroed, matching the
    // caller's zero-fill.
    const uint8_t elemType = native->elemType;
    const uint8_t visualCount = native->visualCount;
    if (elemType == 9)
    {
        if (off.visualMarks)
            native->visuals.markArray = off.visualMarks;
    }
    else if (elemType == 5)
    {
        if (visualCount > 1)
        {
            if (off.visualModelArray)
            {
                FxElemVisuals *array = static_cast<FxElemVisuals *>(RetailZoneLoadSessionAlloc(
                    session, static_cast<std::size_t>(visualCount) * sizeof(FxElemVisuals),
                    alignof(FxElemVisuals)));
                if (array)
                {
                    for (uint32_t i = 0; i < visualCount; ++i)
                        array[i].model = off.visualModelArray[i];
                    native->visuals.array = array;
                }
            }
        }
        else
        {
            native->visuals.instance.model = off.visualModel;
        }
    }
    else if (elemType == 10)
    {
        // Runner visuals: effect-name strings resolved to handles, the same
        // Load_FxEffectDefRef shape as the three effect refs above.
        if (visualCount > 1)
        {
            if (off.visualEffectNameArray)
            {
                FxElemVisuals *array = static_cast<FxElemVisuals *>(RetailZoneLoadSessionAlloc(
                    session, static_cast<std::size_t>(visualCount) * sizeof(FxElemVisuals),
                    alignof(FxElemVisuals)));
                if (array)
                {
                    for (uint32_t i = 0; i < visualCount; ++i)
                    {
                        array[i].effectDef.handle = nullptr;
                        if (off.visualEffectNameArray[i] != UINT32_MAX)
                            array[i].effectDef.handle = RetailResolveEffectRef(
                                reinterpret_cast<const char *>(block4 + off.visualEffectNameArray[i]));
                    }
                    native->visuals.array = array;
                }
            }
        }
        else if (off.visualEffectName != UINT32_MAX)
        {
            native->visuals.instance.effectDef.handle = RetailResolveEffectRef(
                reinterpret_cast<const char *>(block4 + off.visualEffectName));
        }
    }
    else if (elemType != 6 && elemType != 7 && elemType != 8)
    {
        if (visualCount > 1)
        {
            if (off.visualMaterialArray)
            {
                FxElemVisuals *array = static_cast<FxElemVisuals *>(RetailZoneLoadSessionAlloc(
                    session, static_cast<std::size_t>(visualCount) * sizeof(FxElemVisuals),
                    alignof(FxElemVisuals)));
                if (array)
                {
                    for (uint32_t i = 0; i < visualCount; ++i)
                        array[i].material = off.visualMaterialArray[i];
                    native->visuals.array = array;
                }
            }
        }
        else
        {
            native->visuals.instance.material = off.visualMaterial;
        }
    }
}

bool ResolveWireName(RetailZoneLoadSession *session, uint32_t nameRef,
                     uint32_t inlineStart, uint32_t inlineBytes, const char **out,
                     uint32_t *outBytes)
{
    *out = nullptr;
    *outBytes = 0;
    const uint8_t *block4 = session->zoneMemory->blocks[4].data;
    const uint32_t block4Size = session->zoneMemory->blocks[4].size;
    if (nameRef == kInlineRef)
    {
        if (inlineStart >= block4Size || inlineBytes == 0)
            return false;
        uint32_t end = inlineStart;
        while (end < block4Size && block4[end] != 0)
            ++end;
        if (end >= block4Size)
            return false;
        *out = reinterpret_cast<const char *>(block4 + inlineStart);
        *outBytes = end - inlineStart + 1u;
        return true;
    }
    RetailWireToken token{};
    RetailPtr32 encoded{};
    encoded.encoded = nameRef;
    XBlock blocks[9]{};
    for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
        blocks[blockIndex] = session->zoneMemory->blocks[blockIndex];
    if (!RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4 ||
        token.offset >= block4Size)
        return false;
    uint32_t end = token.offset;
    while (end < block4Size && block4[end] != 0)
        ++end;
    if (end >= block4Size)
        return false;
    *out = reinterpret_cast<const char *>(block4 + token.offset);
    *outBytes = end - token.offset + 1u;
    return true;
}
} // namespace

bool RetailWalkLiveLoadFxEffectDef(RetailZoneLoadSession *session,
                                   FsRetailFastfileReader *reader,
                                   RetailWorldLoadContext *worldContext,
                                   XAssetHeader *out,
                                   RetailWalkDirectoryRecord *record,
                                   uint32_t *deferredNestedOut,
                                   uint32_t slotOffset)
{
    if (!session || !reader || !out || !session->active)
        return false;
    *out = XAssetHeader{};
    if (deferredNestedOut)
        *deferredNestedOut = 0;
    // B7 general fix: wire the function-pointer indirection walk.cpp calls
    // through to live-widen the visual union, without walk.cpp taking a
    // direct (link-time) dependency on these decode_world.cpp symbols.
    if (worldContext)
    {
        worldContext->widenNestedMaterial = &RetailWorldWidenMaterial;
        worldContext->widenNestedXModel = &RetailWorldWidenXModel;
    }

    // The body root lands in the temp block (block 0) at an aligned offset
    // exactly like the walk reader's; every exit rewinds it so later
    // block-0 consumers stay at their own placements.
    const uint32_t savedCursor0 = session->wire.cursor[0];
    const uint32_t block4Start = session->wire.cursor[4];

    RetailWalkDirectoryRecord consumed{};
    consumed.header = kInlineRef;
    RetailWalkDirectoryResult scratch{};
    RetailWalkFxElemOffsets *elemOffsets = nullptr;
    if (!ReadRetailFxEffectDefBody(session, reader, &consumed, &scratch, &elemOffsets,
                                   worldContext))
    {
        RewindTemp(session, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadFxEffectDef: body consume failed\n");
        return false;
    }

    const uint32_t bodyStart = (savedCursor0 + 3u) & ~3u;
    const uint8_t *root = session->zoneMemory->blocks[0].data + bodyStart;
    const uint8_t *block4 = session->zoneMemory->blocks[4].data;
    const uint32_t block4Size = session->zoneMemory->blocks[4].size;

    const uint32_t nameRef = ReadLe32(root);
    const uint32_t flags = ReadLe32(root + 4);
    const uint32_t totalSize = ReadLe32(root + 8);
    const uint32_t msecLoopingLife = ReadLe32(root + 12);
    const uint32_t elemDefCountLooping = ReadLe32(root + 16);
    const uint32_t elemDefCountOneShot = ReadLe32(root + 20);
    const uint32_t elemDefCountEmission = ReadLe32(root + 24);
    const uint32_t elemDefsRef = ReadLe32(root + 28);
    const uint64_t elemCount64 = static_cast<uint64_t>(elemDefCountLooping) +
                                 elemDefCountOneShot + elemDefCountEmission;

    const char *name = nullptr;
    uint32_t nameBytes = 0;
    if (!ResolveWireName(session, nameRef, block4Start, consumed.nameBytes, &name,
                         &nameBytes) ||
        !name || !name[0])
    {
        RewindTemp(session, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadFxEffectDef: unresolvable name ref=0x%08x\n", nameRef);
        return false;
    }

    FxEffectDef *fx = static_cast<FxEffectDef *>(
        RetailZoneLoadSessionAlloc(session, sizeof(FxEffectDef), alignof(FxEffectDef)));
    if (!fx)
    {
        RewindTemp(session, savedCursor0);
        return false;
    }
    std::memset(fx, 0, sizeof(*fx));
    char *nameCopy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, nameBytes, 1));
    if (!nameCopy)
    {
        RewindTemp(session, savedCursor0);
        return false;
    }
    std::memcpy(nameCopy, name, nameBytes);
    fx->name = nameCopy;
    fx->flags = static_cast<int>(flags);
    fx->totalSize = static_cast<int>(totalSize);
    fx->msecLoopingLife = static_cast<int>(msecLoopingLife);
    fx->elemDefCountLooping = static_cast<int>(elemDefCountLooping);
    fx->elemDefCountOneShot = static_cast<int>(elemDefCountOneShot);
    fx->elemDefCountEmission = static_cast<int>(elemDefCountEmission);

    uint32_t deferredNested = 0;
    if (elemCount64)
    {
        if (elemCount64 > 0x10000u || elemDefsRef != kInlineRef)
        {
            RewindTemp(session, savedCursor0);
            Com_Printf(0, "RetailWalkLiveLoadFxEffectDef: '%s' elem form unsupported "
                          "count=%llu ref=0x%08x\n",
                       name, static_cast<unsigned long long>(elemCount64), elemDefsRef);
            return false;
        }
        const uint32_t elemCount = static_cast<uint32_t>(elemCount64);
        // The walk reader streamed the elem array after the name, aligned to
        // 4 (RetailZoneLoadSessionReadStream's own alignment).
        const uint32_t afterName = block4Start + (nameRef == kInlineRef ? consumed.nameBytes : 0u);
        const uint32_t elemsStart = (afterName + 3u) & ~3u;
        if (static_cast<uint64_t>(elemsStart) + static_cast<uint64_t>(elemCount) * kElemBytes >
            block4Size)
        {
            RewindTemp(session, savedCursor0);
            Com_Printf(0, "RetailWalkLiveLoadFxEffectDef: '%s' elem span OOB\n", name);
            return false;
        }
        FxElemDef *defs = static_cast<FxElemDef *>(RetailZoneLoadSessionAlloc(
            session, static_cast<std::size_t>(elemCount) * sizeof(FxElemDef),
            alignof(FxElemDef)));
        if (!defs)
        {
            RewindTemp(session, savedCursor0);
            return false;
        }
        std::memset(defs, 0, static_cast<std::size_t>(elemCount) * sizeof(FxElemDef));
        for (uint32_t e = 0; e < elemCount; ++e)
        {
            // The walk reader records where it actually streamed each elem
            // record; the position recomputed from the name length disagreed
            // for some effects (shellejects/*: the prefix then read unrelated
            // zero bytes -- elemType 0, flags 0, velIntervalCount 0 -- while
            // velSamples attached from the reader's real offsets, tripping
            // FX_IntegrateVelocity's velIntervalCount >= 1 assert).
            uint32_t recordStart = elemsStart + e * kElemBytes;
            if (elemOffsets && elemOffsets[e].record != UINT32_MAX)
                recordStart = elemOffsets[e].record;
            if (static_cast<uint64_t>(recordStart) + kElemBytes > block4Size)
            {
                RewindTemp(session, savedCursor0);
                Com_Printf(0, "RetailWalkLiveLoadFxEffectDef: '%s' elem %u OOB\n", name, e);
                return false;
            }
            const uint8_t *wire = block4 + recordStart;
            FxElemDef *native = &defs[e];
            WidenElemDefPrefix(native, wire);
            if (elemOffsets)
                WidenElemNested(session, native, wire, elemOffsets[e]);
            // Every deferred nested pointer is one loud countable object:
            // velSamples/visSamples/visuals + three effect refs + trailDef
            // (each may be declared null on the wire, which is real).
            deferredNested += (ReadLe32(wire + kWireElemVisSamples) != 0) ? 1u : 0u;
            deferredNested += (ReadLe32(wire + kWireElemVisuals) != 0) ? 1u : 0u;
            deferredNested += (ReadLe32(wire + kWireElemEffectOnImpact) != 0) ? 1u : 0u;
            deferredNested += (ReadLe32(wire + kWireElemEffectOnDeath) != 0) ? 1u : 0u;
            deferredNested += (ReadLe32(wire + kWireElemEffectEmitted) != 0) ? 1u : 0u;
            deferredNested += (ReadLe32(wire + 244) != 0) ? 1u : 0u;
        }
        fx->elemDefs = defs;
    }
    else if (elemDefsRef && elemDefsRef != kInlineRef)
    {
        // Zero elems but a non-null array reference is irregular data; keep
        // the walk reader's own accounting (it accepted the alias form) and
        // bind null with the count loud at the caller.
        deferredNested += 1u;
    }

    XAssetHeader tx{};
    tx.fx = fx;
    Load_FxEffectDefAsset(&tx);
    if (!tx.fx)
    {
        RewindTemp(session, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadFxEffectDef: '%s' registration failed\n", name);
        return false;
    }
    if (slotOffset && worldContext &&
        !RetailWorldRecordZoneSlot(worldContext, slotOffset, ASSET_TYPE_FX, tx))
    {
        RewindTemp(session, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadFxEffectDef: '%s' slot ledger full\n", name);
        return false;
    }

    if (record)
    {
        record->bodyBytes = consumed.bodyBytes;
        record->nameBytes = consumed.nameBytes;
        record->nestedBodyBytes = consumed.nestedBodyBytes;
        record->nestedReferenceCount = consumed.nestedReferenceCount;
    }
    if (deferredNestedOut)
        *deferredNestedOut = deferredNested;
    *out = tx;
    RewindTemp(session, savedCursor0);
    return true;
}

bool RetailWalkLiveLoadFxImpactTable(RetailZoneLoadSession *session,
                                     FsRetailFastfileReader *reader,
                                     RetailWorldLoadContext *worldContext,
                                     XAssetHeader *out)
{
    constexpr uint32_t kRootBytes = 8;
    constexpr uint32_t kEntries = 12;
    constexpr uint32_t kCells = 33; // 29 nonflesh + 4 flesh
    constexpr uint32_t kTableBytes = kEntries * kCells * 4u;
    static_assert(sizeof(FxImpactEntry) == 29 * sizeof(const FxEffectDef *) +
                                           4 * sizeof(const FxEffectDef *),
                  "FxImpactEntry is the 29+4 effect pointer grid");
    if (!session || !reader || !out || !session->active)
        return false;
    *out = XAssetHeader{};

    const uint32_t savedCursor0 = session->wire.cursor[0];
    const uint32_t block4Start = session->wire.cursor[4];

    // Root: 8 bytes in the temp block; name XString follows in block 4.
    const uint32_t bodyStart = (savedCursor0 + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kRootBytes, 4))
    {
        Com_Printf(0, "RetailWalkLiveLoadFxImpactTable: root unreadable\n");
        return false;
    }
    const uint8_t *root = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadLe32(root);
    const uint32_t tableRef = ReadLe32(root + 4);

    // Android Load_FxImpactTable: stream the 8-byte root, then Load_XString
    // while block 4 is the active stream position.  An inline (-1) reference
    // streams the bytes at this position; any other non-null reference is the
    // linker's dedup alias to an earlier block-4 string and must be read at
    // the token's own offset.  Registering the name through the original
    // Load_FxImpactTableAsset requires the zone-owned copy, so an alias that
    // is not resolved here would silently register "" (code_post_gfx.ff's
    // impact table is named "default", common.ff's names an empty slot).
    char aliasName[256] = {};
    uint32_t nameBytes = 0;
    if (nameRef == kInlineRef)
    {
        if (!ReadRetailXString(session, reader, kInlineRef, &nameBytes))
        {
            Com_Printf(0, "RetailWalkLiveLoadFxImpactTable: inline name unreadable\n");
            RewindTemp(session, savedCursor0);
            return false;
        }
    }
    else if (nameRef)
    {
        if (!ReadRetailXStringAlias(reader, nameRef, aliasName, sizeof(aliasName)))
        {
            Com_Printf(0, "RetailWalkLiveLoadFxImpactTable: name alias 0x%08x did not resolve\n",
                       nameRef);
            RewindTemp(session, savedCursor0);
            return false;
        }
        nameBytes = static_cast<uint32_t>(std::strlen(aliasName)) + 1u;
    }
    else
    {
        nameBytes = 1u; // declared-null name registers as "" (the fastfile lookup key)
    }

    FxImpactTable *fx = static_cast<FxImpactTable *>(
        RetailZoneLoadSessionAlloc(session, sizeof(FxImpactTable), alignof(FxImpactTable)));
    if (!fx)
    {
        RewindTemp(session, savedCursor0);
        return false;
    }
    std::memset(fx, 0, sizeof(*fx));
    char *nameCopy = static_cast<char *>(
        RetailZoneLoadSessionAlloc(session, nameBytes, 1));
    if (!nameCopy)
    {
        RewindTemp(session, savedCursor0);
        return false;
    }
    if (nameRef == kInlineRef)
        std::memcpy(nameCopy, session->zoneMemory->blocks[4].data + block4Start, nameBytes);
    else if (nameBytes > 1)
        std::memcpy(nameCopy, aliasName, nameBytes);
    else
        nameCopy[0] = '\0';
    fx->name = nameCopy;
    // One milestone line per impact table, not dense narration: the verifier
    // pins the registered identity (code_post_gfx "default", common "") so a
    // regression to the inline-only copy (both "") fails the run.
    Com_Printf(0, "RetailWalkLiveLoadFxImpactTable: name='%s' nameRef=0x%08x code=%u\n",
               nameCopy, nameRef, nameRef == kInlineRef ? 1u : (nameRef ? 0u : 2u));

    if (tableRef == kInlineRef)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, kTableBytes, 4))
        {
            Com_Printf(0, "RetailWalkLiveLoadFxImpactTable: table unreadable\n");
            RewindTemp(session, savedCursor0);
            return false;
        }
        const uint32_t tableStart = session->wire.cursor[4] - kTableBytes;
        FxImpactEntry *table = static_cast<FxImpactEntry *>(RetailZoneLoadSessionAlloc(
            session, static_cast<std::size_t>(kEntries) * sizeof(FxImpactEntry),
            alignof(FxImpactEntry)));
        if (!table)
        {
            RewindTemp(session, savedCursor0);
            return false;
        }
        std::memset(table, 0, static_cast<std::size_t>(kEntries) * sizeof(FxImpactEntry));
        for (uint32_t entry = 0; entry < kEntries; ++entry)
        {
            for (uint32_t cell = 0; cell < kCells; ++cell)
            {
                const uint32_t index = entry * kCells + cell;
                const uint32_t slot = ReadLe32(
                    session->zoneMemory->blocks[4].data + tableStart + index * 4u);
                const FxEffectDef **dest = cell < 29 ? &table[entry].nonflesh[cell]
                                                     : &table[entry].flesh[cell - 29];
                if (!slot)
                {
                    *dest = nullptr;
                    continue;
                }
                if (!worldContext)
                {
                    *dest = nullptr;
                    continue;
                }
                XAssetHeader resolved{};
                if (slot == kInlineRef || slot == kInsertRef)
                {
                    uint32_t insertSlot = 0;
                    if (slot == kInsertRef)
                    {
                        uint8_t *reserved = RetailWireBlocksAlloc(&session->wire, 4, 4, 4);
                        if (!reserved)
                        {
                            Com_Printf(0, "RetailWalkLiveLoadFxImpactTable: insert slot failed\n");
                            RewindTemp(session, savedCursor0);
                            return false;
                        }
                        insertSlot = static_cast<uint32_t>(
                            reserved - session->zoneMemory->blocks[4].data);
                    }
                    RetailWalkDirectoryRecord fxRecord{};
                    uint32_t fxDeferred = 0;
                    if (!RetailWalkLiveLoadFxEffectDef(session, reader, worldContext,
                                                       &resolved, &fxRecord, &fxDeferred,
                                                       tableStart + index * 4u))
                    {
                        Com_Printf(0, "RetailWalkLiveLoadFxImpactTable: cell %u/%u widen failed\n",
                                   entry, cell);
                        RewindTemp(session, savedCursor0);
                        return false;
                    }
                    if (insertSlot &&
                        !RetailWorldRecordZoneSlot(worldContext, insertSlot, ASSET_TYPE_FX,
                                                   resolved))
                    {
                        Com_Printf(0, "RetailWalkLiveLoadFxImpactTable: insert ledger full\n");
                        RewindTemp(session, savedCursor0);
                        return false;
                    }
                }
                else if (!RetailWorldResolveNestedAlias(worldContext, slot, ASSET_TYPE_FX,
                                                        &resolved) ||
                         !resolved.fx)
                {
                    // B6 contract: every non-null impact cell must resolve.
                    Com_Printf(0, "RetailWalkLiveLoadFxImpactTable: cell %u/%u alias 0x%08x unresolved\n",
                               entry, cell, slot);
                    RewindTemp(session, savedCursor0);
                    return false;
                }
                *dest = resolved.fx;
            }
        }
        fx->table = table;
    }
    else if (tableRef)
    {
        // A shared/alias table is a legitimate linker form; identity is the
        // registry entry and a null table here is a real engine state.
        fx->table = nullptr;
    }

    XAssetHeader tx{};
    tx.impactFx = fx;
    Load_FxImpactTableAsset(&tx);
    if (!tx.impactFx)
    {
        Com_Printf(0, "RetailWalkLiveLoadFxImpactTable: registration failed for '%s'\n",
                   nameCopy);
        RewindTemp(session, savedCursor0);
        return false;
    }
    *out = tx;
    RewindTemp(session, savedCursor0);
    return true;
}
