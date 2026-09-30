#include "db_retail_walk.h"
#include "../universal/retail_asset_trace.h"

#include "db_retail_decode_clipmap.h"
#include "db_retail_decode_font.h"
#include "db_retail_decode_fx.h"
#include "db_retail_decode_image.h"
#include "db_retail_decode_material.h"
#include "db_retail_decode_menulist.h"
#include "db_retail_decode_pathdata.h"
#include "db_retail_decode_rawfile.h"
#include "db_retail_decode_small.h"
#include "db_retail_decode_sound.h"
#include "db_retail_decode_stringtable.h"
#include "db_retail_decode_techniqueset.h"
#include "db_retail_decode_world.h"
#include "db_retail_decode_weapon.h"
#include "db_retail_decode_xanim.h"
#include "db_retail_zone.h"

#include "../gfx_d3d/fxprimitives.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

// Weak default for the live-nested image hook (see db_retail_walk.h): the
// walk-only test harness links this TU without the image decoder, so the
// hook must resolve here. Production decoders override it strongly.
__attribute__((weak)) uint8_t *RetailWalkRetainLoadedSoundData(const uint8_t *root44,
                                                               const uint8_t *data,
                                                               uint32_t dataLen, uint32_t *outLen,
                                                               uint32_t *outFormat,
                                                               uint32_t *outBits)
{
    (void)root44; (void)data; (void)dataLen; (void)outLen; (void)outFormat; (void)outBits;
    return nullptr;
}

__attribute__((weak)) GfxImage *RetailWalkWidenNestedImage(RetailZoneLoadSession *session,
                                                           FsRetailFastfileReader *reader,
                                                           const FsRetailFastfileImage &wire)
{
    (void)session;
    (void)reader;
    (void)wire;
    return nullptr;
}

// The load-profile summary prints asset-type names through the real table
// (db_assetnames.cpp) in production. The host walk/boot/reload proof links
// omit that TU, so resolve here as a weak fallback like the two hooks above
// rather than breaking their link.
__attribute__((weak)) const char *__cdecl DB_GetXAssetTypeName(uint32_t type)
{
    static char fallback[16];
    std::snprintf(fallback, sizeof(fallback), "type%u", type);
    return fallback;
}

namespace
{
constexpr uint32_t kAssetCountCap = 1u << 20;
constexpr uint32_t kInlineReference = 0xffffffffu;
constexpr uint32_t kInsertReference = 0xfffffffeu;

bool IsNoStreamType(uint32_t type)
{
    switch (type)
    {
    case ASSET_TYPE_XMODELPIECES:
    case ASSET_TYPE_UI_MAP:
    case ASSET_TYPE_SNDDRIVER_GLOBALS:
    case ASSET_TYPE_AITYPE:
    case ASSET_TYPE_MPTYPE:
    case ASSET_TYPE_CHARACTER:
    case ASSET_TYPE_XMODELALIAS:
        return true;
    default:
        return false;
    }
}

} // namespace

// Engine stream-scope semantics (db_stream_load DB_PushStreamPos/
// DB_PopStreamPos, mirrored by the Android oracle): a scope entered on the
// temp block rewinds that block's cursor when it pops, while every other
// block keeps its advanced cursor.  The walk addresses blocks explicitly,
// so the scope only has to carry the temp-block cursor.
namespace
{
struct RetailWalkTempScope
{
    uint32_t savedCursor0;
};

bool RetailWalkBeginTemp(RetailZoneLoadSession *session, RetailWalkTempScope *scope)
{
    if (!session || !scope || !session->active)
        return false;
    scope->savedCursor0 = session->wire.cursor[0];
    return true;
}

void RetailWalkEndTemp(RetailZoneLoadSession *session, const RetailWalkTempScope *scope)
{
    RetailWireBlocksRewind(&session->wire, 0, scope->savedCursor0);
}

uint32_t ReadRetailLe32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint32_t ReadRetailLe16(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8);
}
} // namespace

// HandleAssetSlot semantics: see db_retail_walk.h (shared with live drivers).

bool ReadRetailMaterialBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary);
bool ReadRetailImageBody(RetailZoneLoadSession *session,
                         FsRetailFastfileReader *reader,
                         RetailWalkDirectoryRecord *record,
                         RetailWalkDirectoryResult *summary);
bool IsRetailWireAlias(const RetailZoneLoadSession *session, uint32_t reference,
                       uint32_t span);
bool ReadRetailXString(RetailZoneLoadSession *session,
                       FsRetailFastfileReader *reader, uint32_t reference,
                       uint32_t *bytes);
bool ReadRetailTechniqueSetBody(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader,
                                RetailWalkDirectoryRecord *record,
                                RetailWalkDirectoryResult *summary);
bool ReadRetailLoadedSoundBody(RetailZoneLoadSession *session,
                               FsRetailFastfileReader *reader,
                               RetailWalkDirectoryRecord *record,
                               RetailWalkDirectoryResult *summary);
bool ReadRetailFxEffectDefBody(RetailZoneLoadSession *session,
                               FsRetailFastfileReader *reader,
                               RetailWalkDirectoryRecord *record,
                               RetailWalkDirectoryResult *summary,
                               RetailWalkFxElemOffsets **elemOffsetsOut,
                               RetailWorldLoadContext *worldContext);
bool ReadRetailMenuDefBody(RetailZoneLoadSession *session,
                           FsRetailFastfileReader *reader,
                           RetailWalkDirectoryRecord *record,
                           RetailWalkDirectoryResult *summary);
bool ReadRetailMenuListBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary);
bool ReadRetailSndAliasListBody(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader,
                                RetailWalkDirectoryRecord *record,
                                RetailWalkDirectoryResult *summary,
                                uint32_t **aliasNameOffsetsOut,
                                RetailWalkSndAliasOffsets *aliasOffsetsOut);
bool ReadRetailSndAliasBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader, uint32_t offset,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary,
                            uint32_t *nameOffsets,
                            RetailWalkSndAliasOffsets *offsets);
bool ReadRetailXModelBody(RetailZoneLoadSession *session,
                          FsRetailFastfileReader *reader,
                          RetailWalkDirectoryRecord *record,
                          RetailWalkDirectoryResult *summary);
bool ReadRetailPhysPresetBody(RetailZoneLoadSession *session,
                              FsRetailFastfileReader *reader,
                              RetailWalkDirectoryRecord *record,
                              RetailWalkDirectoryResult *summary,
                              uint32_t *rootStartOut,
                              uint32_t *nameStartOut,
                              uint32_t *destNameStartOut);
bool ReadRetailMapEntsBody(RetailZoneLoadSession *session,
                           FsRetailFastfileReader *reader,
                           RetailWalkDirectoryRecord *record,
                           RetailWalkDirectoryResult *summary);
bool ReadRetailLightDefBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary);

bool ReadRetailAssetSlotBody(RetailZoneLoadSession *session,
                             FsRetailFastfileReader *reader, uint32_t reference,
                             RetailWalkNestedKind kind,
                             RetailWalkDirectoryRecord *record,
                             RetailWalkDirectoryResult *summary)
{
    if (!reference)
        return true;
    if (reference != kInlineReference && reference != kInsertReference)
        return IsRetailWireAlias(session, reference, 1);
    if (reference == kInsertReference &&
        !RetailWireBlocksAlloc(&session->wire, 4, 4, 4))
        return false;
    RetailWalkTempScope scope;
    if (!RetailWalkBeginTemp(session, &scope))
        return false;
    RetailWalkDirectoryRecord nested{};
    nested.header = kInlineReference;
    bool ok = false;
    switch (kind)
    {
    case RETAIL_WALK_NESTED_TECHNIQUE_SET:
        ok = ReadRetailTechniqueSetBody(session, reader, &nested, summary);
        break;
    case RETAIL_WALK_NESTED_MATERIAL:
        ok = ReadRetailMaterialBody(session, reader, &nested, summary);
        break;
    case RETAIL_WALK_NESTED_IMAGE:
        ok = ReadRetailImageBody(session, reader, &nested, summary);
        break;
    case RETAIL_WALK_NESTED_LOADED_SOUND:
        ok = ReadRetailLoadedSoundBody(session, reader, &nested, summary);
        break;
    case RETAIL_WALK_NESTED_SND_CURVE:
        ok = ReadRetailSndCurveBody(session, reader, &nested, summary);
        break;
    case RETAIL_WALK_NESTED_FX_EFFECT_DEF:
        ok = ReadRetailFxEffectDefBody(session, reader, &nested, summary);
        break;
    case RETAIL_WALK_NESTED_MENU_DEF:
        ok = ReadRetailMenuDefBody(session, reader, &nested, summary);
        break;
    case RETAIL_WALK_NESTED_SOUND_ALIAS_LIST:
        ok = ReadRetailSndAliasListBody(session, reader, &nested, summary);
        break;
    case RETAIL_WALK_NESTED_XMODEL:
        ok = ReadRetailXModelBody(session, reader, &nested, summary);
        break;
    case RETAIL_WALK_NESTED_PHYS_PRESET:
        ok = ReadRetailPhysPresetBody(session, reader, &nested, summary);
        break;
    case RETAIL_WALK_NESTED_MAP_ENTS:
        ok = ReadRetailMapEntsBody(session, reader, &nested, summary);
        break;
    case RETAIL_WALK_NESTED_LIGHT_DEF:
        ok = ReadRetailLightDefBody(session, reader, &nested, summary);
        break;
    }
    RetailWalkEndTemp(session, &scope);
    if (!ok)
    {
        if (summary->failedNestedKind == UINT32_MAX)
        {
            summary->failedNestedKind = static_cast<uint32_t>(kind);
            summary->failedNestedRef = reference;
        }
        return false;
    }
    record->nestedBodyBytes += nested.bodyBytes + nested.nestedBodyBytes;
    record->nestedPassCount += nested.nestedPassCount;
    if (kind == RETAIL_WALK_NESTED_IMAGE)
    {
        record->resolvedNameRef = nested.resolvedNameRef;
        record->widenedImage = nested.widenedImage;
    }
    return true;
}

static void SyncSessionToReader(RetailZoneLoadSession *session, FsRetailFastfileReader *reader, uint32_t block)
{
    if (!session || !reader || block != 4)
        return;
    const uint32_t readerCursor = FS_RetailFastfileBlockCursor(reader, block);
    const uint32_t sessionCursor = session->wire.cursor[block];
    if (sessionCursor > readerCursor)
    {
        uint8_t *readerData = FS_RetailFastfileBlockDataMutable(reader, block);
        const uint32_t blockSize = FS_RetailFastfileBlockSize(reader, block);
        if (readerData && session->zoneMemory && session->zoneMemory->blocks[block].data &&
            sessionCursor <= blockSize)
        {
            std::memcpy(readerData + readerCursor,
                        session->zoneMemory->blocks[block].data + readerCursor,
                        sessionCursor - readerCursor);
        }
        FS_RetailFastfileSetBlockCursor(reader, block, sessionCursor);
    }
}

bool RetailZoneLoadSessionSyncFromReader(RetailZoneLoadSession *session,
                                         FsRetailFastfileReader *reader,
                                         uint32_t block)
{
    if (!session || !reader || block != 4)
        return false;
    const uint32_t readerCursor = FS_RetailFastfileBlockCursor(reader, block);
    const uint32_t sessionCursor = session->wire.cursor[block];
    if (readerCursor > sessionCursor)
    {
        const uint8_t *readerData = FS_RetailFastfileBlockData(reader, block);
        if (session->zoneMemory && session->zoneMemory->blocks[block].data && readerData &&
            readerCursor <= session->zoneMemory->blocks[block].size)
        {
            std::memcpy(session->zoneMemory->blocks[block].data + sessionCursor,
                        readerData + sessionCursor,
                        readerCursor - sessionCursor);
            session->wire.cursor[block] = readerCursor;
            return true;
        }
        return false;
    }
    return true;
}

static void SyncReaderToSession(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader, uint32_t block)
{
    RetailZoneLoadSessionSyncFromReader(session, reader, block);
}

bool RetailZoneLoadSessionReadStream(RetailZoneLoadSession *session,
                                     FsRetailFastfileReader *reader, uint32_t block,
                                     uint32_t bytes, uint32_t alignment)
{
    if (!session || !session->active || !reader || block >= 9)
        return false;
    // Heal a session lag before consuming: FS-level readers
    // advance the true stream and the reader mirror without moving the
    // session wire, so the session can lag the reader. Streaming into a
    // lagging session would pack bytes below their linker offsets, and the
    // trailing auto-sync would then clobber the mirror with those packed
    // bytes and yank the reader cursor backward. Healing first keeps
    // session == reader == true structurally: reader bytes at already-
    // consumed offsets are true bytes by the same invariant, so the copy
    // lands true bytes at linker offsets. No-op whenever already glued
    // (the entire walk-only path), so exact-cursor proofs are unaffected.
    // Block-4 only, matching the existing sync scope.
    if (block == 4 && !RetailZoneLoadSessionSyncFromReader(session, reader, block))
        return false;
    const uint32_t cursor = session->wire.cursor[block];
    uint8_t *destination = RetailWireBlocksAlloc(&session->wire, block, bytes, alignment);
    if (!destination || FS_ReadRetailFastfile(reader, destination, bytes) != bytes)
    {
        RetailWireBlocksRewind(&session->wire, block, cursor);
        return false;
    }
    SyncSessionToReader(session, reader, block);
    return true;
}

namespace
{
bool ReadRetailArray(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                     uint32_t block, uint64_t count, uint32_t stride,
                     uint32_t alignment, uint32_t *start = nullptr)
{
    const uint64_t byteCount = count * stride;
    if (byteCount > UINT32_MAX ||
        !RetailZoneLoadSessionReadStream(session, reader, block,
                                         static_cast<uint32_t>(byteCount), alignment))
        return false;
    if (start)
        *start = session->wire.cursor[block] - static_cast<uint32_t>(byteCount);
    return true;
}
} // namespace

// Android LoadStream for block 1: runtime-only expansion. The span is
// zero-filled and the block cursor advances exactly like a stream read, but
// zero FS bytes are consumed -- block 1 is absent from the file. Only block
// 1 may expand; blocks 2/3 are delay-streamed (unimplemented by design, see
// RetailWalkOpenDirectory) and every other block streams through
// RetailZoneLoadSessionReadStream. Allocation failure leaves the cursor
// untouched, exactly like a short stream read.
bool RetailZoneLoadSessionExpandRuntime(RetailZoneLoadSession *session, uint32_t block,
                                        uint32_t bytes, uint32_t alignment)
{
    if (!session || !session->active || block != 1)
        return false;
    uint8_t *destination = RetailWireBlocksAlloc(&session->wire, block, bytes, alignment);
    if (!destination)
        return false;
    std::memset(destination, 0, bytes);
    return true;
}

RetailWalkDirectoryResultCode RetailWireReadAssetDirectory(
    const FsRetailFastfileAssetList *list, const FsRetailFastfileAsset *assets,
    uint32_t assetCapacity, RetailWalkDirectoryRecord *records, uint32_t capacity,
    RetailWalkDirectoryResult *result)
{
    RetailWalkDirectoryResult summary{};
    summary.code = RETAIL_WALK_BAD_ARGUMENT;
    if (!list || !result || (list->assetCount && !assets) ||
        (list->assetCount && !records) || list->assetCount > assetCapacity ||
        list->assetCount > capacity)
    {
        if (result)
            *result = summary;
        return summary.code;
    }
    summary.assetCount = list->assetCount;
    for (uint32_t ordinal = 0; ordinal < list->assetCount; ++ordinal)
    {
        const uint32_t type = assets[ordinal].type;
        if (type >= ASSET_TYPE_COUNT)
        {
            summary.recordedCount = ordinal;
            summary.code = RETAIL_WALK_UNSUPPORTED_TYPE;
            *result = summary;
            return summary.code;
        }
        records[ordinal] = {};
        records[ordinal].ordinal = ordinal;
        records[ordinal].type = type;
        records[ordinal].header = assets[ordinal].header;
        records[ordinal].state =
            IsNoStreamType(type) ? RETAIL_WALK_NO_STREAM : RETAIL_WALK_DEFERRED;
        if (records[ordinal].state == RETAIL_WALK_NO_STREAM)
            ++summary.noStreamCount;
        else
            ++summary.deferredCount;
        ++summary.recordedCount;
    }
    summary.code = RETAIL_WALK_OK;
    *result = summary;
    return summary.code;
}

bool ReadInlineRetailStringInBlock(RetailZoneLoadSession *session,
                                   FsRetailFastfileReader *reader, uint32_t block,
                                   uint32_t *bytes)
{
    constexpr uint32_t kMaxStringBytes = 4096;
    // Chunked scan: one inflate call per 64-byte chunk instead of one per
    // byte (the per-byte form re-entered inflate with a 1-byte output window
    // for every character, a measurable slice of a.ff's decode). The tail
    // past the NUL goes back to the reader as unread input so every stream
    // and caller-visible cursor still stops exactly after the terminator.
    constexpr uint32_t kChunkBytes = 64;
    if (!session || !reader || block >= 9)
        return false;
    const uint32_t nameStart = session->wire.cursor[block];
    for (;;)
    {
        const uint32_t cursor = session->wire.cursor[block];
        const uint32_t scanned = cursor - nameStart;
        if (scanned >= kMaxStringBytes)
            return false;
        uint32_t want = kMaxStringBytes - scanned;
        if (want > kChunkBytes)
            want = kChunkBytes;
        // Never request past the block or the end of the stream: the
        // byte-at-a-time form stopped exactly at the NUL, and a zone can end
        // right after the string (fixtures do). A zero-byte window means the
        // only readable bytes are ones already pushed back, so fall back to
        // the exact one-byte drain (which fails at a true end just like the
        // original loop did).
        const uint32_t blockSize = session->zoneMemory->blocks[block].size;
        const uint32_t blockRoom = blockSize > cursor ? blockSize - cursor : 0;
        const uint64_t wireBytes = FS_RetailFastfileWireBytes(reader);
        const uint32_t xfileSize = FS_RetailFastfileXFileSize(reader);
        const uint64_t streamRoom = xfileSize > wireBytes ? xfileSize - wireBytes : 0;
        uint64_t room = blockRoom;
        if (streamRoom < room)
            room = streamRoom;
        if (want > room)
            want = static_cast<uint32_t>(room);
        if (!want)
            want = 1;
        if (!RetailZoneLoadSessionReadStream(session, reader, block, want, 1))
        {
            Com_Printf(0, "STRING_DIAG fail block=%u cursor=%u size=%u scanned=%u want=%u wire=%llu xfile=%u\n",
                       block, cursor, blockSize, scanned, want,
                       static_cast<unsigned long long>(wireBytes), xfileSize);
            return false;
        }
        const uint8_t *data = session->zoneMemory->blocks[block].data;
        const uint32_t total = session->wire.cursor[block] - nameStart;
        for (uint32_t i = scanned; i < total; ++i)
        {
            if (data[nameStart + i] != 0)
                continue;
            const uint32_t end = nameStart + i + 1u;
            if (end < session->wire.cursor[block])
            {
                FS_RetailFastfileUnread(reader, data + end,
                                        session->wire.cursor[block] - end);
                RetailWireBlocksRewind(&session->wire, block, end);
                FS_RetailFastfileSetBlockCursor(reader, block, end);
            }
            if (bytes)
                *bytes = end - nameStart;
            return true;
        }
    }
}

bool ReadInlineRetailString(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader, uint32_t *bytes)
{
    return ReadInlineRetailStringInBlock(session, reader, 4, bytes);
}

// Android Load_Shader: 16-byte root {name, unknown, program, programSize,
// loadForRenderer} in the current block; the name XString follows, then the
// program bytes for any non-zero program slot (the engine allocates
// unconditionally, so the stored value carries no reference meaning).
bool ReadRetailShaderBody(RetailZoneLoadSession *session,
                          FsRetailFastfileReader *reader,
                          RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kRootBytes = 16;
    const uint32_t rootStart = (session->wire.cursor[4] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, kRootBytes, 4))
        return false;
    const uint8_t *root = session->zoneMemory->blocks[4].data + rootStart;
    const uint32_t nameRef = ReadRetailLe32(root);
    const uint32_t programRef = ReadRetailLe32(root + 8);
    const uint32_t programSize = ReadRetailLe16(root + 12);
    uint32_t nameBytes = 0;
    if (!ReadRetailXString(session, reader, nameRef, &nameBytes))
        return false;
    if (programRef)
    {
        if (!ReadRetailArray(session, reader, 4, programSize, 4, 4))
            return false;
        summary->walkedShaderProgramBytes += programSize * 4u;
    }
    ++summary->walkedShaderCount;
    return true;
}

// Android Load_MaterialTechnique: 8-byte header followed by the contiguous
// 20-byte pass array in block 4; each pass then walks its inline vertex
// declaration, both shaders, and (for any non-zero args slot) the argument
// array with literal constants; the technique name is read last.
bool ReadRetailTechniqueBody(RetailZoneLoadSession *session,
                             FsRetailFastfileReader *reader,
                             RetailWalkDirectoryRecord *record,
                             uint32_t reference,
                             RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kPassCountCap = 64;
    constexpr uint32_t kPassBytes = 20;
    constexpr uint32_t kVertexDeclarationBytes = 100;
    if (!reference)
        return true;
    if (reference == kInsertReference)
        return false;
    if (reference != kInlineReference)
        return true;

    const uint32_t techniqueStart = (session->wire.cursor[4] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, 8, 4))
        return false;
    const uint8_t *header = session->zoneMemory->blocks[4].data + techniqueStart;
    const uint32_t nameRef = ReadRetailLe32(header);
    const uint32_t passCount = ReadRetailLe16(header + 6);
    if (passCount > kPassCountCap ||
        !RetailZoneLoadSessionReadStream(session, reader, 4, passCount * kPassBytes, 4))
        return false;

    const uint32_t passStart = session->wire.cursor[4] - passCount * kPassBytes;
    const uint8_t *passes = session->zoneMemory->blocks[4].data + passStart;
    for (uint32_t pass = 0; pass < passCount; ++pass)
    {
        const uint8_t *wire = passes + pass * kPassBytes;
        const uint32_t vertexDecl = ReadRetailLe32(wire);
        const uint32_t vertexShader = ReadRetailLe32(wire + 4);
        const uint32_t pixelShader = ReadRetailLe32(wire + 8);
        const uint32_t args = ReadRetailLe32(wire + 16);
        if (vertexDecl == kInsertReference || vertexShader == kInsertReference ||
            pixelShader == kInsertReference || args == kInsertReference)
            return false;
        if (vertexDecl == kInlineReference &&
            !RetailZoneLoadSessionReadStream(session, reader, 4, kVertexDeclarationBytes, 4))
            return false;
        // Load_MaterialPass resolves each shader slot through
        // Load_MaterialVertexShader/Load_MaterialPixelShader: only the -1
        // token has an inline body; any other non-zero slot is a shared
        // shader reference that consumes no stream bytes.
        for (const uint32_t shader : {vertexShader, pixelShader})
        {
            if (shader == kInlineReference)
            {
                if (!ReadRetailShaderBody(session, reader, summary))
                    return false;
            }
            else if (shader && !IsRetailWireAlias(session, shader, 1))
            {
                return false;
            }
        }
        if (args)
        {
            const uint32_t argCount = wire[12] + wire[13] + wire[14];
            uint32_t argsStart = 0;
            if (!ReadRetailArray(session, reader, 4, argCount, 8, 4, &argsStart))
                return false;
            for (uint32_t arg = 0; arg < argCount; ++arg)
            {
                const uint8_t *argWire = session->zoneMemory->blocks[4].data +
                                         argsStart + arg * 8u;
                const uint32_t type = ReadRetailLe16(argWire);
                const uint32_t value = ReadRetailLe32(argWire + 4);
                ++summary->walkedArgumentCount;
                if ((type == 1 || type == 7) && value == kInlineReference)
                {
                    if (!RetailZoneLoadSessionReadStream(session, reader, 4, 16, 4))
                        return false;
                    ++summary->walkedArgumentLiteralCount;
                }
                else if ((type == 1 || type == 7) && value == kInsertReference)
                    return false;
            }
        }
    }
    // Keep body accounting parallel with TechniqueSet: only the fixed body
    // and pass array belong here; the following XString is block-4 metadata.
    record->nestedBodyBytes += session->wire.cursor[4] - techniqueStart;
    record->nestedPassCount += passCount;
    uint32_t nameBytes = 0;
    if (nameRef == kInlineReference && !ReadInlineRetailString(session, reader, &nameBytes))
        return false;
    if (nameRef == kInsertReference)
        return false;
    (void)nameBytes;
    return true;
}

bool ReadRetailTechniqueSetBody(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader,
                                RetailWalkDirectoryRecord *record,
                                RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 148;
    constexpr uint32_t kInlineReference = 0xffffffffu;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (record->header != kInlineReference ||
        !RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;

    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = 0;
    for (uint32_t slot = 0; slot < 34; ++slot)
        if (ReadRetailLe32(body + 12 + slot * 4u) != 0)
            ++record->nestedReferenceCount;

    const uint32_t nameRef = ReadRetailLe32(body);
    if (!nameRef)
    {
        ++summary->walkedTechniqueCount;
        record->state = RETAIL_WALK_WALKED_DEFERRED;
    }
    else if (nameRef == kInlineReference)
    {
        if (!ReadInlineRetailString(session, reader, &record->nameBytes))
            return false;
    }
    else if (nameRef == kInsertReference)
    {
        return false;
    }

    for (uint32_t slot = 0; slot < 34; ++slot)
        if (!ReadRetailTechniqueBody(session, reader, record,
                                      ReadRetailLe32(body + 12 + slot * 4u), summary))
            return false;
    ++summary->walkedTechniqueCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

bool IsRetailWireAlias(const RetailZoneLoadSession *session, uint32_t reference,
                       uint32_t span)
{
    if (!session || !reference || reference == kInlineReference ||
        reference == kInsertReference)
        return false;
    RetailWireToken token{};
    if (RetailWireTokenDecode(&session->wire, {reference}, span,
                              (1u << 9) - 1u, &token) &&
        token.kind == RETAIL_WIRE_TOKEN_OFFSET)
        return true;
    uint32_t poolIndex = 0;
    return RetailWirePoolIndexDecode({reference}, UINT32_MAX, &poolIndex);
}

// Android Load_XString only streams source bytes for the -1 inline form.
// Other non-null values are existing encoded aliases, so validate them
// through the canonical token seam without moving a cursor.
bool ReadRetailXString(RetailZoneLoadSession *session,
                       FsRetailFastfileReader *reader, uint32_t reference,
                       uint32_t *bytes)
{
    if (bytes)
        *bytes = 0;
    if (!reference)
        return true;
    if (reference == kInlineReference)
        return ReadInlineRetailString(session, reader, bytes);
    return reference != kInsertReference && IsRetailWireAlias(session, reference, 1);
}

// Resolve a non-inline XString reference to the bytes the linker already
// landed in the reader's block-4 window (Android Load_XString's
// DB_ConvertOffsetToAlias form), copying them into caller storage. The
// reference must decode to an in-window block-4 offset; the string's own NUL
// terminates it. Inline (-1), insert (-2), and declared-null (0) forms are
// the caller's responsibility. Callers that register the name must copy it
// into zone-owned memory: the reader's window is transient.
bool ReadRetailXStringAlias(FsRetailFastfileReader *reader, uint32_t reference,
                            char *buffer, uint32_t bufferSize)
{
    if (!reader || !buffer || bufferSize == 0)
        return false;
    buffer[0] = '\0';
    if (!reference || reference == kInlineReference || reference == kInsertReference)
        return false;
    XBlock blocks[9]{};
    for (uint32_t block = 0; block < 9; ++block)
        blocks[block] = {const_cast<uint8_t *>(FS_RetailFastfileBlockData(reader, block)),
                         FS_RetailFastfileBlockSize(reader, block)};
    RetailWireToken token{};
    if (!RetailWireTokenDecodeBlocks(blocks, {reference}, 1, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
        return false;
    const uint8_t *data = FS_RetailFastfileBlockData(reader, 4);
    const uint32_t mirrored = FS_RetailFastfileBlockCursor(reader, 4);
    if (token.offset >= mirrored)
        return false;
    // The empty string is a real alias target (common.ff's impact table
    // points at an empty block-4 slot): DB_ConvertOffsetToAlias keeps the
    // pointer, so "" is the registered identity, not a failed read. A
    // non-printable byte or an unterminated run is a loud failure.
    uint32_t length = 0;
    while (token.offset + length < mirrored && data[token.offset + length] != 0)
    {
        const uint8_t c = data[token.offset + length];
        if (c < 0x20 || c > 0x7e)
            return false;
        ++length;
    }
    if (token.offset + length >= mirrored || length + 1 > bufferSize)
        return false;
    std::memcpy(buffer, data + token.offset, length);
    buffer[length] = '\0';
    return true;
}

bool ReadRetailXStringInBlock(RetailZoneLoadSession *session,
                              FsRetailFastfileReader *reader, uint32_t reference,
                              uint32_t block, uint32_t *bytes)
{
    if (bytes)
        *bytes = 0;
    if (!reference)
        return true;
    if (reference == kInlineReference)
        return ReadInlineRetailStringInBlock(session, reader, block, bytes);
    return reference != kInsertReference && IsRetailWireAlias(session, reference, 1);
}

// Android Load_LocalizeEntry: root in stream block 0, then value followed by
// name through Load_XString while block 4 is active.  This F5 walker consumes
// bytes only; the LocalizeEntry decoder remains the sole owner that widens/registers LocalizeEntry.
bool ReadRetailLocalizeEntryBody(RetailZoneLoadSession *session,
                                 FsRetailFastfileReader *reader,
                                 RetailWalkDirectoryRecord *record,
                                 RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 8;
    if (!session || !reader || !record || record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t valueRef = ReadRetailLe32(body);
    const uint32_t nameRef = ReadRetailLe32(body + 4);
    uint32_t valueBytes = 0;
    uint32_t nameBytes = 0;
    if (!ReadRetailXString(session, reader, valueRef, &valueBytes) ||
        !ReadRetailXString(session, reader, nameRef, &nameBytes))
        return false;
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nameBytes = valueBytes + nameBytes;
    record->nestedReferenceCount = (valueRef != 0) + (nameRef != 0);
    ++summary->walkedLocalizeCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// GfxImage's serialized body is the 36-byte Android wire form followed by
// the XString name and, for the -1/-2 texture slot, the 16-byte loadDef plus
// resourceSize pixel bytes in the temp block.  The map type is widening
// policy owned by the G3 image slice, so this walker consumes every map
// type's bytes without constructing an image, registering an asset, or
// creating a renderer texture.
bool ReadRetailImageBody(RetailZoneLoadSession *session,
                         FsRetailFastfileReader *reader,
                         RetailWalkDirectoryRecord *record,
                         RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 36;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;

    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t textureRef = ReadRetailLe32(body + 4);
    const uint32_t nameRef = ReadRetailLe32(body + 32);

    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = textureRef ? 1 : 0;
    record->nestedBodyBytes = 0;
    record->nameBytes = 0;
    record->resolvedNameRef = 0;
    record->widenedImage = nullptr;

    if (nameRef == kInsertReference)
        return false;
    if (nameRef == kInlineReference)
    {
        // Nothing else touches block 4 between here and the string write
        // below (the 36-byte body above came from block 0), so the
        // current cursor is exactly the fresh name's own offset -- mirrors
        // ReadRetailFastfileInlineName's live-side resolved nameRef
        // encoding, so a caller can pool this name without re-parsing the
        // image body through the live reader (see the struct comment on
        // RetailWalkDirectoryRecord::resolvedNameRef).
        const uint32_t nameStart = session->wire.cursor[4];
        if (!ReadInlineRetailString(session, reader, &record->nameBytes))
            return false;
        record->resolvedNameRef = ((4u << 28) | nameStart) + 1u;
    }
    else if (nameRef)
    {
        if (!IsRetailWireAlias(session, nameRef, 1))
            return false;
        record->resolvedNameRef = nameRef;
    }

    if (!textureRef)
    {
        ++summary->walkedImageCount;
        record->state = RETAIL_WALK_WALKED_DEFERRED;
        return true;
    }
    if (textureRef != kInlineReference && textureRef != kInsertReference)
    {
        if (!IsRetailWireAlias(session, textureRef, 1))
            return false;
        record->state = RETAIL_WALK_WALKED_DEFERRED;
        return true;
    }

    // LoadGfxTextureLoad reserves the -2 insert pointer in block 4 without
    // consuming source bytes, then streams loadDef and pixels in block 0.
    if (textureRef == kInsertReference &&
        !RetailWireBlocksAlloc(&session->wire, 4, 4, 4))
        return false;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, 16, 4))
        return false;
    const uint32_t loadDefStart = session->wire.cursor[0] - 16;
    const uint8_t *loadDef = session->zoneMemory->blocks[0].data + loadDefStart;
    const uint32_t resourceSize = ReadRetailLe32(loadDef + 12);
    record->nestedBodyBytes = 16;
    if (resourceSize &&
        !RetailZoneLoadSessionReadStream(session, reader, 0, resourceSize, 1))
        return false;
    record->nestedBodyBytes += resourceSize;
    summary->walkedImagePayloadBytes += resourceSize;
    ++summary->walkedImageCount;
    // Live-nested widening: the body above
    // is fully parsed into session blocks, so complete the same
    // FsRetailFastfileImage the live FS reader would have produced and
    // widen it through the production image decoder. Never fails the walk:
    // a skipped or soft-failed widen simply leaves later aliases to the
    // existing tolerated-null path.
    if (session->widenNestedImages)
    {
        FsRetailFastfileImage wire{};
        wire.headerRef = kInlineReference;
        wire.mapType = ReadRetailLe32(body);
        wire.textureRef = textureRef;
        wire.picmip[0] = body[8];
        wire.picmip[1] = body[9];
        wire.noPicmip = body[10];
        wire.semantic = body[11];
        wire.track = body[12];
        wire.cardMemory[0] = ReadRetailLe32(body + 16);
        wire.cardMemory[1] = ReadRetailLe32(body + 20);
        wire.width = static_cast<uint16_t>(body[24] | (body[25] << 8));
        wire.height = static_cast<uint16_t>(body[26] | (body[27] << 8));
        wire.depth = static_cast<uint16_t>(body[28] | (body[29] << 8));
        wire.category = body[30];
        wire.delayLoadPixels = body[31];
        wire.nameRef = record->resolvedNameRef;
        wire.nameWasInline = (nameRef == kInlineReference) ? 1u : 0u;
        // Inline names stream into session blocks only; mirror the bytes
        // into the reader mirror at the same (dense, linker-true) offset
        // without moving any cursor, so the widener's own name read lands.
        // The offset itself decodes through the canonical token seam.
        if (wire.nameWasInline && record->nameBytes)
        {
            RetailWireToken nameToken{};
            if (RetailWireTokenDecode(&session->wire, {wire.nameRef}, 0, 1u << 4, &nameToken) &&
                nameToken.kind == RETAIL_WIRE_TOKEN_OFFSET && nameToken.block == 4 &&
                nameToken.offset + record->nameBytes <= FS_RetailFastfileBlockSize(reader, 4))
            {
                std::memcpy(FS_RetailFastfileBlockDataMutable(reader, 4) + nameToken.offset,
                            session->zoneMemory->blocks[4].data + nameToken.offset,
                            record->nameBytes);
            }
        }
        wire.haveLoadDef = 1;
        wire.loadDefLevelCount = loadDef[0];
        wire.loadDefFlags = loadDef[1];
        for (uint32_t i = 0; i < 3; ++i)
            wire.loadDefDimensions[i] =
                static_cast<uint16_t>(loadDef[2 + i * 2] | (loadDef[3 + i * 2] << 8));
        wire.loadDefFormat = ReadRetailLe32(loadDef + 8);
        wire.loadDefResourceSize = resourceSize;
        wire.pixelDataOffset = 0;
        if (resourceSize)
        {
            // Stage the session-temp pixels into the reader's persistent
            // block-0 area (the widener copies them into zone memory from
            // there). FS temp save/restore wraps only its own reads, so a
            // reservation here persists safely. On overflow, drop the pixels
            // and widen header-only; upload then falls back to IWI adopt.
            const uint32_t pixelSource = loadDefStart + 16;
            uint32_t pixelOffset = (FS_RetailFastfileBlockCursor(reader, 0) + 3u) & ~3u;
            if (pixelOffset + resourceSize <= FS_RetailFastfileBlockSize(reader, 0))
            {
                std::memcpy(FS_RetailFastfileBlockDataMutable(reader, 0) + pixelOffset,
                            session->zoneMemory->blocks[0].data + pixelSource, resourceSize);
                FS_RetailFastfileSetBlockCursor(reader, 0, pixelOffset + resourceSize);
                wire.pixelDataOffset = pixelOffset;
            }
            else
            {
                wire.loadDefResourceSize = 0;
            }
        }
        record->widenedImage = RetailWalkWidenNestedImage(session, reader, wire);
    }
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

bool ReadRetailWaterBody(RetailZoneLoadSession *session,
                         FsRetailFastfileReader *reader, bool isInsert,
                         RetailWalkDirectoryRecord *record,
                         RetailWalkDirectoryResult *summary);
// Android LoadWater order, via db_load Load_water_t: 68-byte root (H0 at
// +4 sized N*M complex floats, wTerm at +8 sized N*M floats, M at +12,
// N at +16, image at +64) for the inline form. The -2 insert form carries
// no stream bytes (db_load converts it without loading). Any other
// non-null form is an already-loaded alias.
bool ReadRetailWaterBody(RetailZoneLoadSession *session,
                         FsRetailFastfileReader *reader, bool isInsert,
                         RetailWalkDirectoryRecord *record,
                         RetailWalkDirectoryResult *summary)
{
    if (!session || !reader || !record || !summary)
        return false;
    if (isInsert)
        return true;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, 68, 4))
        return false;
    const uint32_t waterStart = session->wire.cursor[4] - 68;
    record->nestedBodyBytes += 68;
    const uint8_t *water =
        session->zoneMemory->blocks[4].data + waterStart;
    const uint32_t h0Ref = ReadRetailLe32(water + 4);
    const uint32_t wTermRef = ReadRetailLe32(water + 8);
    const uint32_t m = ReadRetailLe32(water + 12);
    const uint32_t n = ReadRetailLe32(water + 16);
    const uint32_t imageRef = ReadRetailLe32(water + 64);
    const uint32_t refs[2] = {h0Ref, wTermRef};
    const uint64_t sizes[2] = {(uint64_t)n * m * 8u, (uint64_t)n * m * 4u};
    for (uint32_t side = 0; side < 2; ++side)
    {
        if (!refs[side])
            continue;
        if (refs[side] != kInlineReference &&
            !IsRetailWireAlias(session, refs[side], 1))
            return false;
        if (refs[side] != kInlineReference)
            continue;
        if (sizes[side] > UINT32_MAX)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             (uint32_t)sizes[side], 4))
            return false;
        record->nestedBodyBytes += (uint32_t)sizes[side];
    }
    if (!ReadRetailAssetSlotBody(session, reader, imageRef,
                                 RETAIL_WALK_NESTED_IMAGE, record, summary))
        return false;
    return true;
}

bool ReadRetailMaterialTable(RetailZoneLoadSession *session,
                              FsRetailFastfileReader *reader,
                              uint32_t reference, uint32_t count,
                              uint32_t elementBytes, uint32_t alignment,
                              bool textureTable,
                              RetailWalkDirectoryRecord *record,
                              RetailWalkDirectoryResult *summary)
{
    if (!session || !reader || !record || !summary)
        return false;
    if (!reference)
        return true;
    if (reference == kInsertReference)
        return false;
    if (reference != kInlineReference)
        return IsRetailWireAlias(session, reference, 1);

    uint32_t tableStart = 0;
    if (!ReadRetailArray(session, reader, 4, count, elementBytes, alignment,
                         &tableStart))
        return false;
    const uint32_t tableBytes = count * elementBytes;
    record->nestedBodyBytes += tableBytes;

    if (!textureTable)
        return true;

    // MaterialTextureDef's union is an asset-style image/water slot. Inline
    // image bodies are consumed by the same bounded GfxImage walker as a
    // top-level image; water resolves through the water walker below.
    const uint8_t *table = session->zoneMemory->blocks[4].data + tableStart;
    for (uint32_t index = 0; index < count; ++index)
    {
        const uint8_t *def = table + index * elementBytes;
        const uint32_t nestedReference = ReadRetailLe32(def + 8);
        if (nestedReference)
            ++record->nestedReferenceCount;
        const uint32_t semantic = def[7];
        if (nestedReference == kInlineReference ||
            nestedReference == kInsertReference)
        {
            // Semantic 11 is the water_t arm of the union: a 68-byte root
            // (H0/wTerm float arrays sized by N*M plus an image) instead of
            // a GfxImage body. Image bodies count inside their own reader.
            if (semantic == 11)
            {
                if (!ReadRetailWaterBody(session, reader,
                                         nestedReference == kInsertReference,
                                         record, summary))
                    return false;
            }
            else
            {
                if (!ReadRetailAssetSlotBody(session, reader, nestedReference,
                                             RETAIL_WALK_NESTED_IMAGE, record,
                                             summary))
                    return false;
                // This slot is the shared-image pool's own declaring
                // occurrence exactly like a live-loaded material's inline
                // texture def (FS_ReadRetailFastfileMaterial,
                // com_files.cpp): a later material's texture slot can
                // alias straight back to this exact block-4 position
                // expecting a pooled image reference there (e.g. every
                // material sharing a stock/common normal map). Since this
                // asset stays walked_deferred, only the reader's pool
                // table and this one 4-byte slot get patched -- no image
                // is registered or widened, and the walk's own byte
                // accounting is unaffected (the image body was already
                // consumed by ReadRetailAssetSlotBody above; this only
                // overwrites bytes already inside the table read at the
                // top of this function). Best-effort: a full pool or an
                // out-of-range slot leaves today's behavior (a later alias
                // fails to resolve) rather than failing this walk over a
                // capacity/side-effect concern unrelated to walk-only
                // correctness.
                const uint32_t imageSlotOffset =
                    static_cast<uint32_t>((def + 8) - session->zoneMemory->blocks[4].data);
                uint32_t poolRef = 0;
                if (FS_RetailFastfileRegisterImagePoolSlot(reader, record->resolvedNameRef,
                                                           imageSlotOffset, &poolRef) ==
                    FS_RETAIL_FF_WIRE_OK)
                {
                    uint8_t *slot = session->zoneMemory->blocks[4].data + imageSlotOffset;
                    slot[0] = static_cast<uint8_t>(poolRef);
                    slot[1] = static_cast<uint8_t>(poolRef >> 8);
                    slot[2] = static_cast<uint8_t>(poolRef >> 16);
                    slot[3] = static_cast<uint8_t>(poolRef >> 24);
                    // Live loads also keep the widened image itself (see
                    // ReadRetailImageBody's hook call) in the session pool
                    // beside the FS pool write-back, so the deferred drain
                    // binds it by pool index without needing a name lookup.
                    uint32_t poolIndex = 0;
                    if (record->widenedImage &&
                        RetailWirePoolIndexDecode({poolRef}, RETAIL_FASTFILE_IMAGE_POOL_MAX,
                                                  &poolIndex))
                        session->imagePool[poolIndex] = record->widenedImage;
                }
            }
        }
        else if (nestedReference &&
                 !IsRetailWireAlias(session, nestedReference, 1))
        {
            return false;
        }
        else if (nestedReference)
        {
            // Propagate an already-declared pool reference into our own
            // slot, mirroring the live FS path's alias arm
            // (FS_ReadRetailFastfileMaterial, com_files.cpp) and the real
            // loader's in-memory DB_ConvertOffsetToAlias copy: the
            // declarer's patched bytes move into the reading slot, so a
            // later alias straight back to THIS position (e.g. a matmem
            // material sharing the image, like wc/killhouse_target3 via an
            // XModel-nested declarer) resolves at the deferred drain
            // instead of binding null and faulting the first sampled draw.
            // Forward references stay raw for the drain; only already-
            // patched targets propagate here.
            uint32_t targetRef = 0;
            const uint32_t ownOffset =
                static_cast<uint32_t>((def + 8) - session->zoneMemory->blocks[4].data);
            if (FS_RetailFastfileResolveImageAlias(reader, nestedReference, &targetRef) ==
                FS_RETAIL_FF_WIRE_OK)
            {
                if (ownOffset + 4u <= FS_RetailFastfileBlockSize(reader, 4))
                {
                    uint8_t *readerSlot =
                        FS_RetailFastfileBlockDataMutable(reader, 4) + ownOffset;
                    readerSlot[0] = static_cast<uint8_t>(targetRef);
                    readerSlot[1] = static_cast<uint8_t>(targetRef >> 8);
                    readerSlot[2] = static_cast<uint8_t>(targetRef >> 16);
                    readerSlot[3] = static_cast<uint8_t>(targetRef >> 24);
                    // Same mirror convention as the inline arm above: the
                    // session buffer is a separate copy that a later
                    // forward-only sync must never stomp back.
                    uint8_t *ownSlot = session->zoneMemory->blocks[4].data + ownOffset;
                    ownSlot[0] = readerSlot[0];
                    ownSlot[1] = readerSlot[1];
                    ownSlot[2] = readerSlot[2];
                    ownSlot[3] = readerSlot[3];
                }
            }
        }
        ++summary->walkedMaterialTextureCount;
    }
    return true;
}

bool ReadRetailMaterialBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 80;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;

    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t techniqueSetRef = ReadRetailLe32(body + 64);
    const uint32_t textureTableRef = ReadRetailLe32(body + 68);
    const uint32_t constantTableRef = ReadRetailLe32(body + 72);
    const uint32_t stateBitsTableRef = ReadRetailLe32(body + 76);
    const uint32_t textureCount = body[58];
    const uint32_t constantCount = body[59];
    const uint32_t stateBitsCount = body[60];

    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = 0;
    for (const uint32_t reference : {techniqueSetRef, textureTableRef,
                                     constantTableRef, stateBitsTableRef})
        if (reference)
            ++record->nestedReferenceCount;

    // This is the Android Load_Material order: push block 4, load the info
    // XString, then the asset-style technique set and the three independent
    // tables.  The temporary nested record keeps its accounting separate
    // from the owning Material record; neither is registered or activated.
    if (nameRef == kInsertReference)
        return false;

    if (nameRef == kInlineReference &&
        !ReadInlineRetailString(session, reader, &record->nameBytes))
        return false;
    if (nameRef && nameRef != kInlineReference &&
        !IsRetailWireAlias(session, nameRef, 1))
        return false;

    if (!ReadRetailAssetSlotBody(session, reader, techniqueSetRef,
                                 RETAIL_WALK_NESTED_TECHNIQUE_SET, record,
                                 summary))
        return false;

    if (!ReadRetailMaterialTable(session, reader, textureTableRef,
                                 textureCount, 12, 4, true, record, summary) ||
        !ReadRetailMaterialTable(session, reader, constantTableRef,
                                 constantCount, 32, 16, false, record, summary) ||
        !ReadRetailMaterialTable(session, reader, stateBitsTableRef,
                                 stateBitsCount, 8, 4, false, record, summary))
        return false;

    ++summary->walkedMaterialCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_Font: 24-byte root in block 0; fontName, the two material
// handles, and the optional Glyph array are walked while block 4 is active.
// This is deliberately byte-accounting only; the native decoder owns Font_s creation.
bool ReadRetailFontBody(RetailZoneLoadSession *session,
                        FsRetailFastfileReader *reader,
                        RetailWalkDirectoryRecord *record,
                        RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 24;
    constexpr uint32_t kGlyphBytes = 24;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t glyphCount = ReadRetailLe32(body + 8);
    const uint32_t materialRef = ReadRetailLe32(body + 12);
    const uint32_t glowMaterialRef = ReadRetailLe32(body + 16);
    const uint32_t glyphRef = ReadRetailLe32(body + 20);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (materialRef != 0) +
                                   (glowMaterialRef != 0) + (glyphRef != 0);
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;

    if (!ReadRetailAssetSlotBody(session, reader, materialRef,
                                 RETAIL_WALK_NESTED_MATERIAL, record, summary) ||
        !ReadRetailAssetSlotBody(session, reader, glowMaterialRef,
                                 RETAIL_WALK_NESTED_MATERIAL, record, summary))
        return false;

    if (glyphRef == kInlineReference)
    {
        // Glyph arrays are 4-aligned in the wire format: the engine loads
        // them via AllocLoad_FxElemVisStateSample (DB_AllocStreamPos(3)),
        // so the linker pads the stream position up to a multiple of 4
        // before the array. Reading them unaligned leaves every later
        // block-4 offset short by the skipped pad (e.g. 12 bytes
        // across code_post_gfx.ff's nine fonts), which breaks exact
        // technique/decl/shader identity matching downstream.
        if (!ReadRetailArray(session, reader, 4, glyphCount, kGlyphBytes, 4))
            return false;
        record->nestedBodyBytes += glyphCount * kGlyphBytes;
    }
    else if (glyphRef && !IsRetailWireAlias(session, glyphRef, 1))
    {
        // -2 is not a Glyph-array form in Android Load_Font; it is neither an
        // encoded pointer nor an asset handle and must fail loudly.
        return false;
    }
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_RawFile reads the fixed root from block 0, an XString name in
// block 4, then (for every non-null buffer field) exactly len + 1 raw bytes.
// The wire buffer value is a presence marker here, not an encoded alias.
bool ReadRetailRawFileBody(RetailZoneLoadSession *session,
                           FsRetailFastfileReader *reader,
                           RetailWalkDirectoryRecord *record)
{
    constexpr uint32_t kBodyBytes = 12;
    if (!session || !reader || !record || record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const int32_t length = static_cast<int32_t>(ReadRetailLe32(body + 4));
    const uint32_t bufferRef = ReadRetailLe32(body + 8);
    if (length < 0 || !ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (bufferRef != 0);
    if (bufferRef)
    {
        const uint32_t bytes = static_cast<uint32_t>(length) + 1u;
        if (!bytes || !RetailZoneLoadSessionReadStream(session, reader, 4, bytes, 1))
            return false;
        record->nestedBodyBytes = bytes;
    }
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_StringTable runs with no temp-block push, so the 16-byte
// root, its name XString, the rows*columns cell-reference table, and every
// cell string stream into the persistent block 4.
bool ReadRetailStringTableBody(RetailZoneLoadSession *session,
                               FsRetailFastfileReader *reader,
                               RetailWalkDirectoryRecord *record)
{
    constexpr uint32_t kBodyBytes = 16;
    if (!session || !reader || !record || record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[4] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[4].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t rows = ReadRetailLe32(body + 4);
    const uint32_t columns = ReadRetailLe32(body + 8);
    const uint32_t valuesRef = ReadRetailLe32(body + 12);
    record->bodyBytes = session->wire.cursor[4] - bodyStart;
    if (!ReadRetailXStringInBlock(session, reader, nameRef, 4, &record->nameBytes))
        return false;
    const uint64_t cellCount = static_cast<uint64_t>(rows) * columns;
    if (cellCount > UINT32_MAX)
        return false;
    const uint32_t count = static_cast<uint32_t>(cellCount);
    record->nestedReferenceCount = nameRef != 0;
    if (!valuesRef)
    {
        record->state = RETAIL_WALK_WALKED_DEFERRED;
        return true;
    }
    uint32_t tableStart = 0;
    if (!ReadRetailArray(session, reader, 4, count, 4, 4, &tableStart))
        return false;
    const uint8_t *table = session->zoneMemory->blocks[4].data + tableStart;
    record->nestedBodyBytes = count * 4u;
    for (uint32_t index = 0; index < count; ++index)
    {
        const uint32_t ref = ReadRetailLe32(table + index * 4u);
        uint32_t stringBytes = 0;
        if (!ReadRetailXStringInBlock(session, reader, ref, 4, &stringBytes))
            return false;
        record->nestedReferenceCount += ref != 0;
        record->nestedBodyBytes += stringBytes;
    }
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_PhysPreset: 44-byte root in the temp block; the name and
// destName XStrings at +0/+28 land in block 4.
bool ReadRetailPhysPresetBody(RetailZoneLoadSession *session,
                              FsRetailFastfileReader *reader,
                              RetailWalkDirectoryRecord *record,
                              RetailWalkDirectoryResult *summary,
                              uint32_t *rootStartOut,
                              uint32_t *nameStartOut,
                              uint32_t *destNameStartOut)
{
    constexpr uint32_t kBodyBytes = 44;
    if (!session || !reader || !record || record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (rootStartOut)
        *rootStartOut = bodyStart;
    if (nameStartOut)
        *nameStartOut = UINT32_MAX;
    if (destNameStartOut)
        *destNameStartOut = UINT32_MAX;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t destNameRef = ReadRetailLe32(body + 28);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (destNameRef != 0);
    const uint32_t nameStart = session->wire.cursor[4];
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;
    if (nameStartOut && nameRef == kInlineReference)
        *nameStartOut = nameStart;
    const uint32_t destStart = session->wire.cursor[4];
    uint32_t destBytes = 0;
    if (!ReadRetailXString(session, reader, destNameRef, &destBytes))
        return false;
    if (destNameStartOut && destNameRef == kInlineReference)
        *destNameStartOut = destStart;
    record->nameBytes += destBytes;
    ++summary->walkedPhysPresetCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_SndCurve: 72-byte root in the temp block; one filename
// XString at +0 in block 4.
bool ReadRetailSndCurveBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary,
                            uint32_t *rootStartOut,
                            uint32_t *nameStartOut)
{
    constexpr uint32_t kBodyBytes = 72;
    if (!session || !reader || !record || record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (rootStartOut)
        *rootStartOut = bodyStart;
    if (nameStartOut)
        *nameStartOut = UINT32_MAX;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = nameRef != 0;
    const uint32_t nameStart = session->wire.cursor[4];
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;
    if (nameStartOut && nameRef == kInlineReference)
        *nameStartOut = nameStart;
    ++summary->walkedSndCurveCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_GfxLightDef: 16-byte root in the temp block; name XString at
// +0 and the attenuation image as an asset-style slot at +4.
bool ReadRetailLightDefBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 16;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t imageRef = ReadRetailLe32(body + 4);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (imageRef != 0);
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;
    if (!ReadRetailAssetSlotBody(session, reader, imageRef,
                                 RETAIL_WALK_NESTED_IMAGE, record, summary))
        return false;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_LoadedSound: 44-byte root in the temp block; the name XString
// in block 4; then, inside a nested temp scope, the audio payload for the
// -1/-2 data slot (-2 first reserving its block-4 insert slot) while any
// smaller encoded value is an already-loaded alias.
bool ReadRetailLoadedSoundBody(RetailZoneLoadSession *session,
                               FsRetailFastfileReader *reader,
                               RetailWalkDirectoryRecord *record,
                               RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 44;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
    {
        Com_Printf(0, "ReadRetailLoadedSoundBody: bad header 0x%08x\n", record ? record->header : 0);
        return false;
    }
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
    {
        Com_Printf(0, "ReadRetailLoadedSoundBody: ReadStream 0 (44 bytes) failed\n");
        return false;
    }
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t dataLen = ReadRetailLe32(body + 12);
    const uint32_t dataRef = ReadRetailLe32(body + 40);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (dataRef != 0);
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
    {
        Com_Printf(0, "ReadRetailLoadedSoundBody: ReadRetailXString nameRef failed\n");
        return false;
    }

    if (dataRef && dataRef >= kInsertReference)
    {
        RetailWalkTempScope scope;
        if (dataRef == kInsertReference &&
            !RetailWireBlocksAlloc(&session->wire, 4, 4, 4))
        {
            Com_Printf(0, "ReadRetailLoadedSoundBody: WireBlocksAlloc 4 failed\n");
            return false;
        }
        if (!RetailWalkBeginTemp(session, &scope))
        {
            Com_Printf(0, "ReadRetailLoadedSoundBody: RetailWalkBeginTemp failed\n");
            return false;
        }
        const bool ok =
            RetailZoneLoadSessionReadStream(session, reader, 0, dataLen, 1);
        RetailWalkEndTemp(session, &scope);
        if (!ok)
        {
            Com_Printf(0, "ReadRetailLoadedSoundBody: ReadStream sound data (len=%u) failed cursor0=%u size0=%u\n",
                       dataLen, session->wire.cursor[0], session->zoneMemory->blocks[0].size);
            return false;
        }
        record->nestedBodyBytes += dataLen;
        ++summary->walkedLoadedSoundDataBytes;
    }
    else if (dataRef && !IsRetailWireAlias(session, dataRef, 1))
    {
        Com_Printf(0, "ReadRetailLoadedSoundBody: dataRef 0x%08x is not alias\n", dataRef);
        return false;
    }
    ++summary->walkedLoadedSoundCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_SpeakerMap: 408-byte root already resident in block 4 with
// its name XString at +4; no temp-block push in the engine.
bool ReadRetailSpeakerMapBody(RetailZoneLoadSession *session,
                              FsRetailFastfileReader *reader, uint32_t offset)
{
    const uint8_t *body = session->zoneMemory->blocks[4].data + offset;
    const uint32_t nameRef = ReadRetailLe32(body + 4);
    uint32_t nameBytes = 0;
    return ReadRetailXString(session, reader, nameRef, &nameBytes);
}

// Android Load_SoundFile: 12-byte record resident in block 4.  Type 1 holds
// an asset-style LoadedSound slot; every other type streams dir/name
// XStrings.
bool ReadRetailSoundFileBody(RetailZoneLoadSession *session,
                             FsRetailFastfileReader *reader, uint32_t offset,
                             RetailWalkDirectoryRecord *record,
                             RetailWalkDirectoryResult *summary,
                             RetailWalkSndAliasOffsets *offsets = nullptr)
{
    const XBlock &block4 = session->zoneMemory->blocks[4];
    const uint8_t *body = block4.data + offset;
    const uint32_t type = body[0];
    const uint32_t dirRef = ReadRetailLe32(body + 4);
    if (type == 1)
    {
        // The embedded LoadedSound root streams into block 0 at the next
        // 4-aligned temp base; its name follows in block 4 (after an insert
        // form's own 4-byte DB_InsertPointer reservation) and its data bytes
        // land right after the 44-byte root.
        const uint32_t loadedRoot = (session->wire.cursor[0] + 3u) & ~3u;
        const uint32_t alignedName = (session->wire.cursor[4] + 3u) & ~3u;
        const uint32_t loadedName = dirRef == kInsertReference ? alignedName + 4u
                                                               : session->wire.cursor[4];
        if (offsets && dirRef == kInsertReference)
            offsets->loadedSoundInsertSlot = alignedName;
        const bool ok = ReadRetailAssetSlotBody(session, reader, dirRef,
                                                RETAIL_WALK_NESTED_LOADED_SOUND, record,
                                                summary);
        if (ok && offsets &&
            (dirRef == kInlineReference || dirRef == kInsertReference) &&
            static_cast<uint64_t>(loadedRoot) + 44u <= session->zoneMemory->blocks[0].size)
        {
            offsets->loadedSoundRoot = loadedRoot;
            offsets->loadedSoundName = loadedName;
            // Retain the root bytes now: block 0 is the rewinding temp
            // block, so a later nested body in this list reuses the same
            // offset and the live widener would otherwise read that later
            // body's bytes (see RetailWalkSndAliasOffsets).
            uint8_t *rootCopy = static_cast<uint8_t *>(
                RetailZoneLoadSessionAlloc(session, 44, 4));
            if (!rootCopy)
                return false;
            std::memcpy(rootCopy, session->zoneMemory->blocks[0].data + loadedRoot, 44);
            offsets->loadedSoundRootBytes = rootCopy;
            // Only an inline/insert data slot actually streamed audio bytes
            // (an alias consumes none); a listed data_len with an alias slot
            // must not be copied from unrelated mirror bytes.
            const uint32_t dataRef = ReadRetailLe32(rootCopy + 40u);
            if (dataRef == kInlineReference || dataRef == kInsertReference)
            {
                offsets->loadedSoundData = loadedRoot + 44u;
                offsets->loadedSoundDataLen = ReadRetailLe32(rootCopy + 12u);
                // block 0 rewinds under later bodies, so the payload is
                // copied out now (the widener runs after the whole list).
                if (offsets->loadedSoundDataLen &&
                    static_cast<uint64_t>(offsets->loadedSoundData) + offsets->loadedSoundDataLen <=
                        session->zoneMemory->blocks[0].size)
                {
                    offsets->loadedSoundRetained = RetailWalkRetainLoadedSoundData(
                        rootCopy, session->zoneMemory->blocks[0].data + offsets->loadedSoundData,
                        offsets->loadedSoundDataLen, &offsets->loadedSoundRetainedLen,
                        &offsets->loadedSoundRetainedFormat, &offsets->loadedSoundRetainedBits);
                }
            }
        }
        return ok;
    }
    const uint32_t nameRef = ReadRetailLe32(body + 8);
    uint32_t bytes = 0;
    const uint32_t dirStart = session->wire.cursor[4];
    bool okDir = ReadRetailXString(session, reader, dirRef, &bytes);
    const uint32_t nameStart = session->wire.cursor[4];
    bool okName = ReadRetailXString(session, reader, nameRef, &bytes);
    if (okDir && okName && offsets)
    {
        if (dirRef == kInlineReference)
            offsets->soundFileDir = dirStart;
        if (nameRef == kInlineReference)
            offsets->soundFileName = nameStart;
    }
    return okDir && okName;
}

// Android Load_snd_alias_t: 92-byte record resident in block 4 with four
// Leading XStrings, the sound file, the falloff curve, and the speaker map.
// `nameOffsets` (when non-null) receives the four resolved string offsets
// for the live B2 widener: inline names land at their stream start, alias
// names at their decoded block-4 offset, null slots as UINT32_MAX.
bool ReadRetailSndAliasBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader, uint32_t offset,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary,
                            uint32_t *nameOffsets = nullptr,
                            RetailWalkSndAliasOffsets *offsets = nullptr)
{
    if (offsets)
    {
        std::memset(offsets, 0xFF, sizeof(*offsets));
        offsets->loadedSoundRootBytes = nullptr;
        offsets->curveRootBytes = nullptr;
        // Retention pointer: 0xFF here would be a live garbage pointer
        // whenever the root is inline but its payload slot is an alias (no
        // retention runs), so it must start null like the other pointers.
        offsets->loadedSoundRetained = nullptr;
    }
    const uint8_t *body = session->zoneMemory->blocks[4].data + offset;
    for (uint32_t slot = 0; slot <= 12; slot += 4)
    {
        uint32_t bytes = 0;
        const uint32_t ref = ReadRetailLe32(body + slot);
        const uint32_t stringStart = session->wire.cursor[4];
        if (!ReadRetailXString(session, reader, ref, &bytes))
        {
            Com_Printf(0, "ReadRetailSndAliasBody: string slot %u (ref=0x%08x) failed\n", slot, ref);
            return false;
        }
        record->nameBytes += bytes;
        if (ref)
            ++record->nestedReferenceCount;
        if (nameOffsets || offsets)
        {
            uint32_t resolved = UINT32_MAX;
            if (ref == kInlineReference)
                resolved = stringStart;
            else if (ref)
            {
                XBlock blocks[9]{};
                for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
                    blocks[blockIndex] = session->zoneMemory->blocks[blockIndex];
                RetailWireToken token{};
                RetailPtr32 encoded{};
                encoded.encoded = ref;
                if (RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) &&
                    token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4)
                    resolved = token.offset;
            }
            if (nameOffsets)
                nameOffsets[slot / 4u] = resolved;
            if (offsets)
                offsets->name[slot / 4u] = resolved;
        }
    }
    const uint32_t soundFileRef = ReadRetailLe32(body + 16);
    if (soundFileRef)
    {
        ++record->nestedReferenceCount;
        if (soundFileRef == kInsertReference)
        {
            Com_Printf(0, "ReadRetailSndAliasBody: soundFileRef == kInsert\n");
            return false;
        }
        if (soundFileRef == kInlineReference)
        {
            const uint32_t fileStart = (session->wire.cursor[4] + 3u) & ~3u;
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, 12, 4))
            {
                Com_Printf(0, "ReadRetailSndAliasBody: soundFile ReadStream failed\n");
                return false;
            }
            record->nestedBodyBytes += session->wire.cursor[4] - fileStart;
            if (!ReadRetailSoundFileBody(session, reader, fileStart, record,
                                         summary, offsets))
            {
                Com_Printf(0, "ReadRetailSndAliasBody: ReadRetailSoundFileBody failed\n");
                return false;
            }
            if (offsets)
                offsets->soundFile = fileStart;
        }
        else if (!IsRetailWireAlias(session, soundFileRef, 1))
        {
            Com_Printf(0, "ReadRetailSndAliasBody: soundFileRef 0x%08x not alias\n", soundFileRef);
            return false;
        }
    }
    const uint32_t curveRef = ReadRetailLe32(body + 72);
    const uint32_t curveRoot = (session->wire.cursor[0] + 3u) & ~3u;
    // Insert form reserves its 4-byte block-4 DB_InsertPointer slot before
    // streaming the name; the loader's own capture (+4) is what the live
    // widener must copy.
    const uint32_t alignedCurveName = (session->wire.cursor[4] + 3u) & ~3u;
    const uint32_t curveName = curveRef == kInsertReference ? alignedCurveName + 4u
                                                            : session->wire.cursor[4];
    if (!ReadRetailAssetSlotBody(session, reader, curveRef,
                                 RETAIL_WALK_NESTED_SND_CURVE, record, summary))
    {
        Com_Printf(0, "ReadRetailSndAliasBody: curveRef 0x%08x slot failed\n", curveRef);
        return false;
    }
    if (offsets &&
        (curveRef == kInlineReference || curveRef == kInsertReference))
    {
        offsets->curveRoot = curveRoot;
        offsets->curveName = curveName;
        // An insert form reserved this block-4 slot before streaming the
        // name; the original loader patches it with the registered curve,
        // and an alias sibling's DB_ConvertOffsetToAlias reads it back.
        if (curveRef == kInsertReference)
            offsets->curveInsertSlot = alignedCurveName;
        // Retain the 72-byte root before any later nested body reuses this
        // temp-block offset (see RetailWalkSndAliasOffsets). The slot body
        // read above already streamed it, so the span is in range.
        if (static_cast<uint64_t>(curveRoot) + 72u >
            session->zoneMemory->blocks[0].size)
            return false;
        uint8_t *curveCopy = static_cast<uint8_t *>(
            RetailZoneLoadSessionAlloc(session, 72, 4));
        if (!curveCopy)
            return false;
        std::memcpy(curveCopy, session->zoneMemory->blocks[0].data + curveRoot, 72);
        offsets->curveRootBytes = curveCopy;
    }
    const uint32_t speakerMapRef = ReadRetailLe32(body + 88);
    if (speakerMapRef)
    {
        ++record->nestedReferenceCount;
        if (speakerMapRef == kInsertReference)
        {
            Com_Printf(0, "ReadRetailSndAliasBody: speakerMapRef == kInsert\n");
            return false;
        }
        if (speakerMapRef == kInlineReference)
        {
            const uint32_t mapStart = (session->wire.cursor[4] + 3u) & ~3u;
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, 408, 4))
            {
                Com_Printf(0, "ReadRetailSndAliasBody: speakerMap ReadStream failed\n");
                return false;
            }
            record->nestedBodyBytes += 408;
            if (!ReadRetailSpeakerMapBody(session, reader, mapStart))
            {
                Com_Printf(0, "ReadRetailSndAliasBody: ReadRetailSpeakerMapBody failed\n");
                return false;
            }
            if (offsets)
            {
                offsets->speakerMap = mapStart;
                offsets->speakerMapName = mapStart + 408u;
            }
            ++summary->walkedSpeakerMapCount;
        }
        else if (!IsRetailWireAlias(session, speakerMapRef, 1))
        {
            Com_Printf(0, "ReadRetailSndAliasBody: speakerMapRef 0x%08x not alias\n", speakerMapRef);
            return false;
        }
    }
    return true;
}

// Android Load_snd_alias_list_t: 12-byte root in the temp block; name
// XString, then the inline head array of 92-byte aliases in block 4.
bool ReadRetailSndAliasListBody(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader,
                                RetailWalkDirectoryRecord *record,
                                RetailWalkDirectoryResult *summary,
                                uint32_t **aliasNameOffsetsOut,
                                RetailWalkSndAliasOffsets **aliasOffsetsOut)
{
    constexpr uint32_t kBodyBytes = 12;
    constexpr uint32_t kAliasBytes = 92;
    if (aliasNameOffsetsOut)
        *aliasNameOffsetsOut = nullptr;
    if (aliasOffsetsOut)
        *aliasOffsetsOut = nullptr;
    RetailWalkSndAliasOffsets *aliasOffsets = nullptr;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
    {
        Com_Printf(0, "ReadRetailSndAliasListBody: bad header 0x%08x\n", record ? record->header : 0);
        return false;
    }
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
    {
        Com_Printf(0, "ReadRetailSndAliasListBody: ReadStream 0 failed\n");
        return false;
    }
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t headRef = ReadRetailLe32(body + 4);
    const uint32_t aliasCount = ReadRetailLe32(body + 8);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (headRef != 0);
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
    {
        Com_Printf(0, "ReadRetailSndAliasListBody: ReadRetailXString name failed\n");
        return false;
    }
    if (!headRef)
    {
        record->state = RETAIL_WALK_WALKED_DEFERRED;
        return true;
    }
    if (headRef == kInsertReference)
    {
        Com_Printf(0, "ReadRetailSndAliasListBody: insert head is unsupported\n");
        return false;
    }
    if (headRef != kInlineReference)
    {
        bool isAlias = IsRetailWireAlias(session, headRef, 1);
        Com_Printf(0, "ReadRetailSndAliasListBody: headRef=0x%08x isAlias=%d\n", headRef, isAlias);
        return isAlias;
    }
    if ((aliasNameOffsetsOut || aliasOffsetsOut) && aliasCount > 65536u)
        return false;
    uint32_t *nameOffsets = nullptr;
    if (aliasNameOffsetsOut && aliasCount)
    {
        nameOffsets = static_cast<uint32_t *>(RetailZoneLoadSessionAlloc(
            session, static_cast<std::size_t>(aliasCount) * 16u, 4));
        if (!nameOffsets)
            return false;
        for (uint32_t i = 0; i < aliasCount * 4u; ++i)
            nameOffsets[i] = UINT32_MAX;
        *aliasNameOffsetsOut = nameOffsets;
    }
    if (aliasOffsetsOut && aliasCount)
    {
        aliasOffsets = static_cast<RetailWalkSndAliasOffsets *>(RetailZoneLoadSessionAlloc(
            session, static_cast<std::size_t>(aliasCount) * sizeof(RetailWalkSndAliasOffsets),
            4));
        if (!aliasOffsets)
            return false;
        std::memset(aliasOffsets, 0xFF,
                    static_cast<std::size_t>(aliasCount) * sizeof(RetailWalkSndAliasOffsets));
        *aliasOffsetsOut = aliasOffsets;
    }
    uint32_t headStart = 0;
    if (!ReadRetailArray(session, reader, 4, aliasCount, kAliasBytes, 4,
                         &headStart))
    {
        Com_Printf(0, "ReadRetailSndAliasListBody: alias array failed\n");
        return false;
    }
    record->nestedBodyBytes += aliasCount * kAliasBytes;
    for (uint32_t alias = 0; alias < aliasCount; ++alias)
    {
        if (!ReadRetailSndAliasBody(session, reader, headStart + alias * kAliasBytes,
                                    record, summary,
                                    nameOffsets ? nameOffsets + alias * 4u : nullptr,
                                    aliasOffsets ? aliasOffsets + alias : nullptr))
        {
            Com_Printf(0, "ReadRetailSndAliasListBody: ReadRetailSndAliasBody %u/%u failed\n", alias, aliasCount);
            return false;
        }
    }
    ++summary->walkedSoundCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_FxTrailDef: 28-byte record resident in block 4 with the
// vertex and index leaves behind their non-zero slots.
bool ReadRetailFxTrailDefBody(RetailZoneLoadSession *session,
                              FsRetailFastfileReader *reader, uint32_t offset,
                              RetailWalkDirectoryRecord *record,
                              RetailWalkFxElemOffsets *offsets = nullptr)
{
    const uint8_t *body = session->zoneMemory->blocks[4].data + offset;
    const uint32_t vertCount = ReadRetailLe32(body + 12);
    const uint32_t indCount = ReadRetailLe32(body + 20);
    const uint32_t vertsRef = ReadRetailLe32(body + 16);
    const uint32_t indsRef = ReadRetailLe32(body + 24);
    if (vertsRef)
    {
        if (vertCount > UINT32_MAX / 20u)
            return false;
        if (offsets)
            offsets->trailVerts = (session->wire.cursor[4] + 3u) & ~3u;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, vertCount * 20u, 4))
            return false;
        record->nestedBodyBytes += vertCount * 20u;
    }
    if (indsRef)
    {
        if (indCount > UINT32_MAX / 2u)
            return false;
        if (offsets)
            offsets->trailInds = session->wire.cursor[4];
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, indCount * 2u, 1))
            return false;
        record->nestedBodyBytes += indCount * 2u;
    }
    return true;
}

// Walk the four-byte FxElemDef visual union. Live callers also widen and
// record material/model slots; walk-only callers only consume their bytes.
bool ReadRetailFxElemVisualSlot(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader, uint32_t offset,
                                uint8_t elemType,
                                RetailWalkDirectoryRecord *record,
                                RetailWalkDirectoryResult *summary,
                                RetailWorldLoadContext *worldContext,
                                Material **materialOut, XModel **modelOut,
                                uint32_t *effectNameOut)
{
    const uint32_t reference = ReadRetailLe32(session->zoneMemory->blocks[4].data + offset);
    if (effectNameOut)
        *effectNameOut = UINT32_MAX;
    switch (elemType)
    {
    case 5:
        if (worldContext && worldContext->widenNestedXModel)
            return worldContext->widenNestedXModel(worldContext, reference, modelOut,
                                                   offset, nullptr);
        return ReadRetailAssetSlotBody(session, reader, reference,
                                       RETAIL_WALK_NESTED_XMODEL, record,
                                       summary);
    case 10:
    case 8:
    {
        uint32_t bytes = 0;
        if (effectNameOut && elemType == 10)
        {
            if (reference == kInlineReference)
                *effectNameOut = session->wire.cursor[4];
            else if (reference && reference != kInsertReference)
            {
                // Alias to a string the linker already landed in block 4
                // (the same name used earlier in the zone): keep its offset,
                // the decoder resolves it exactly like the inline form.
                RetailWireToken token{};
                if (RetailWireTokenDecode(&session->wire, {reference}, 0, 1u << 4, &token) &&
                    token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4)
                    *effectNameOut = token.offset;
                else
                    Com_Printf(0, "ReadRetailFxElemVisualSlot: runner visual ref 0x%x did not decode to block 4\n",
                               reference);
            }
        }
        return ReadRetailXString(session, reader, reference, &bytes);
    }
    case 6:
    case 7:
        return true;
    default:
        if (worldContext && worldContext->widenNestedMaterial)
            return worldContext->widenNestedMaterial(worldContext, reference,
                                                     materialOut, offset);
        return ReadRetailAssetSlotBody(session, reader, reference,
                                       RETAIL_WALK_NESTED_MATERIAL, record,
                                       summary);
    }
}

// Android Load_FxElemDef: 252-byte record resident in block 4; velocity and
// vis-state sample arrays, the visuals union, three effect-name strings,
// and the optional trail definition.
bool ReadRetailFxElemDefBody(RetailZoneLoadSession *session,
                             FsRetailFastfileReader *reader, uint32_t offset,
                             RetailWalkDirectoryRecord *record,
                             RetailWalkDirectoryResult *summary,
                             RetailWalkFxElemOffsets *offsets,
                             RetailWorldLoadContext *worldContext)
{
    const uint8_t *body = session->zoneMemory->blocks[4].data + offset;
    const uint32_t elemType = body[176];
    const uint32_t visualCount = body[177];
    const uint32_t velIntervalCount = body[178];
    const uint32_t visStateIntervalCount = body[179];
    if (offsets)
    {
        // Pointer fields must come up null, not the 0xff/UINT32_MAX
        // sentinel the plain offset fields below use for "nothing here".
        std::memset(offsets, 0, sizeof(*offsets));
        offsets->velSamples = UINT32_MAX;
        offsets->visSamples = UINT32_MAX;
        offsets->effectOnImpact = UINT32_MAX;
        offsets->effectOnDeath = UINT32_MAX;
        offsets->effectEmitted = UINT32_MAX;
        offsets->trailDef = UINT32_MAX;
        offsets->trailVerts = UINT32_MAX;
        offsets->trailInds = UINT32_MAX;
        offsets->visualEffectName = UINT32_MAX;
        offsets->record = offset;
    }

    if (ReadRetailLe32(body + 180))
    {
        uint32_t start = 0;
        if (!ReadRetailArray(session, reader, 4,
                             static_cast<uint64_t>(velIntervalCount) + 1u,
                             96, 4, &start))
            return false;
        if (offsets)
            offsets->velSamples = start;
        record->nestedBodyBytes += (velIntervalCount + 1u) * 96u;
    }
    if (ReadRetailLe32(body + 184))
    {
        uint32_t start = 0;
        if (!ReadRetailArray(session, reader, 4,
                             static_cast<uint64_t>(visStateIntervalCount) + 1u,
                             48, 4, &start))
            return false;
        if (offsets)
            offsets->visSamples = start;
        record->nestedBodyBytes += (visStateIntervalCount + 1u) * 48u;
    }

    const uint32_t visualsRef = ReadRetailLe32(body + 188);
    if (elemType == 9)
    {
        if (visualsRef)
        {
            uint32_t marksStart = 0;
            if (!ReadRetailArray(session, reader, 4, visualCount, 8, 4,
                                 &marksStart))
                return false;
            record->nestedBodyBytes += visualCount * 8u;
            FxElemMarkVisuals *marks = nullptr;
            if (worldContext)
            {
                marks = static_cast<FxElemMarkVisuals *>(RetailZoneLoadSessionAlloc(
                    session, static_cast<std::size_t>(visualCount) * sizeof(FxElemMarkVisuals),
                    alignof(FxElemMarkVisuals)));
                if (!marks)
                    return false;
            }
            for (uint32_t mark = 0; mark < visualCount; ++mark)
            {
                const uint32_t ref0 = ReadRetailLe32(session->zoneMemory->blocks[4].data +
                                                     marksStart + mark * 8u);
                const uint32_t ref1 = ReadRetailLe32(session->zoneMemory->blocks[4].data +
                                                     marksStart + mark * 8u + 4u);
                if (worldContext && worldContext->widenNestedMaterial)
                {
                    Material *m0 = nullptr;
                    Material *m1 = nullptr;
                    if (!worldContext->widenNestedMaterial(worldContext, ref0, &m0,
                                                           marksStart + mark * 8u) ||
                        !worldContext->widenNestedMaterial(worldContext, ref1, &m1,
                                                           marksStart + mark * 8u + 4u))
                        return false;
                    marks[mark].materials[0] = m0;
                    marks[mark].materials[1] = m1;
                }
                else if (!ReadRetailAssetSlotBody(session, reader, ref0,
                                                  RETAIL_WALK_NESTED_MATERIAL, record, summary) ||
                        !ReadRetailAssetSlotBody(session, reader, ref1,
                                                 RETAIL_WALK_NESTED_MATERIAL, record, summary))
                {
                    return false;
                }
            }
            if (offsets)
                offsets->visualMarks = marks;
        }
    }
    else if (visualCount > 1)
    {
        if (visualsRef)
        {
            if (visualCount > UINT32_MAX / 4u)
                return false;
            if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                                 visualCount * 4u, 4))
                return false;
            const uint32_t visualsStart =
                session->wire.cursor[4] - visualCount * 4u;
            record->nestedBodyBytes += visualCount * 4u;
            Material **materials = nullptr;
            XModel **models = nullptr;
            uint32_t *effectNames = nullptr;
            if (offsets && elemType == 10)
            {
                effectNames = static_cast<uint32_t *>(RetailZoneLoadSessionAlloc(
                    session, static_cast<std::size_t>(visualCount) * sizeof(uint32_t),
                    alignof(uint32_t)));
                if (!effectNames)
                    return false;
            }
            if (worldContext)
            {
                if (elemType == 5)
                {
                    models = static_cast<XModel **>(RetailZoneLoadSessionAlloc(
                        session, static_cast<std::size_t>(visualCount) * sizeof(XModel *),
                        alignof(XModel *)));
                    if (!models)
                        return false;
                }
                else if (elemType != 6 && elemType != 7 && elemType != 8 && elemType != 10)
                {
                    materials = static_cast<Material **>(RetailZoneLoadSessionAlloc(
                        session, static_cast<std::size_t>(visualCount) * sizeof(Material *),
                        alignof(Material *)));
                    if (!materials)
                        return false;
                }
            }
            for (uint32_t visual = 0; visual < visualCount; ++visual)
            {
                Material *materialOut = nullptr;
                XModel *modelOut = nullptr;
                uint32_t effectName = UINT32_MAX;
                if (!ReadRetailFxElemVisualSlot(
                        session, reader, visualsStart + visual * 4u,
                        static_cast<uint8_t>(elemType), record, summary, worldContext,
                        &materialOut, &modelOut, &effectName))
                    return false;
                if (materials)
                    materials[visual] = materialOut;
                if (models)
                    models[visual] = modelOut;
                if (effectNames)
                    effectNames[visual] = effectName;
            }
            if (offsets)
            {
                offsets->visualMaterialArray = materials;
                offsets->visualModelArray = models;
                offsets->visualEffectNameArray = effectNames;
            }
        }
    }
    else
    {
        Material *materialOut = nullptr;
        XModel *modelOut = nullptr;
        uint32_t effectName = UINT32_MAX;
        if (!ReadRetailFxElemVisualSlot(session, reader, offset + 188,
                                        static_cast<uint8_t>(elemType), record, summary,
                                        worldContext, &materialOut, &modelOut, &effectName))
            return false;
        if (offsets)
        {
            offsets->visualMaterial = materialOut;
            offsets->visualModel = modelOut;
            offsets->visualEffectName = effectName;
        }
    }

    for (uint32_t slot = 216; slot <= 224; slot += 4)
    {
        const uint32_t ref = ReadRetailLe32(body + slot);
        uint32_t *slotOut = nullptr;
        if (offsets)
            slotOut = slot == 216 ? &offsets->effectOnImpact
                                  : (slot == 220 ? &offsets->effectOnDeath
                                                 : &offsets->effectEmitted);
        if (ref == kInlineReference && slotOut)
            *slotOut = session->wire.cursor[4];
        uint32_t bytes = 0;
        if (!ReadRetailXString(session, reader, ref, &bytes))
            return false;
        record->nameBytes += bytes;
    }
    if (ReadRetailLe32(body + 244))
    {
        const uint32_t trailStart = (session->wire.cursor[4] + 3u) & ~3u;
        if (offsets)
            offsets->trailDef = trailStart;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, 28, 4))
            return false;
        record->nestedBodyBytes += session->wire.cursor[4] - trailStart;
        if (!ReadRetailFxTrailDefBody(session, reader, trailStart, record, offsets))
            return false;
    }
    return true;
}

// Android Load_FxEffectDef: 32-byte root in the temp block; name XString,
// then the elemDef array (sum of the three counts) of 252-byte records.
bool ReadRetailFxEffectDefBody(RetailZoneLoadSession *session,
                               FsRetailFastfileReader *reader,
                               RetailWalkDirectoryRecord *record,
                               RetailWalkDirectoryResult *summary,
                               RetailWalkFxElemOffsets **elemOffsetsOut,
                               RetailWorldLoadContext *worldContext)
{
    constexpr uint32_t kBodyBytes = 32;
    constexpr uint32_t kElemBytes = 252;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
    {
        Com_Printf(0, "ReadRetailFxEffectDefBody: read body failed\n");
        return false;
    }
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    uint64_t elemCount = 0;
    for (uint32_t field = 16; field <= 24; field += 4)
        elemCount += ReadRetailLe32(body + field);
    const uint32_t elemDefsRef = ReadRetailLe32(body + 28);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (elemDefsRef != 0);
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
    {
        Com_Printf(0, "ReadRetailFxEffectDefBody: read name failed\n");
        return false;
    }
    if (!elemDefsRef)
    {
        ++summary->walkedFxCount;
        record->state = RETAIL_WALK_WALKED_DEFERRED;
        return true;
    }
    if (elemDefsRef == kInsertReference ||
        elemCount > UINT32_MAX / kElemBytes)
    {
        Com_Printf(0, "ReadRetailFxEffectDefBody: invalid elemCount or insert\n");
        return false;
    }
    if (elemDefsRef != kInlineReference)
        return IsRetailWireAlias(session, elemDefsRef, 1);
    const uint64_t elemBytes = elemCount * kElemBytes;
    if (elemBytes > UINT32_MAX ||
        !RetailZoneLoadSessionReadStream(session, reader, 4,
                                         static_cast<uint32_t>(elemBytes), 4))
    {
        Com_Printf(0, "ReadRetailFxEffectDefBody: read elems failed elemBytes=%llu\n", (unsigned long long)elemBytes);
        return false;
    }
    const uint32_t elemsStart = session->wire.cursor[4] - static_cast<uint32_t>(elemBytes);
    record->nestedBodyBytes += static_cast<uint32_t>(elemBytes);
    RetailWalkFxElemOffsets *elemOffsets = nullptr;
    if (elemOffsetsOut)
    {
        if (elemCount > SIZE_MAX / sizeof(RetailWalkFxElemOffsets))
            return false;
        elemOffsets = static_cast<RetailWalkFxElemOffsets *>(RetailZoneLoadSessionAlloc(
            session, static_cast<std::size_t>(elemCount) * sizeof(RetailWalkFxElemOffsets),
            alignof(RetailWalkFxElemOffsets)));
        if (!elemOffsets)
            return false;
        std::memset(elemOffsets, 0xff,
                    static_cast<std::size_t>(elemCount) * sizeof(RetailWalkFxElemOffsets));
        *elemOffsetsOut = elemOffsets;
    }
    for (uint64_t elem = 0; elem < elemCount; ++elem)
        if (!ReadRetailFxElemDefBody(
                session, reader, elemsStart + static_cast<uint32_t>(elem) * kElemBytes,
                record, summary,
                elemOffsets ? &elemOffsets[elem] : nullptr, worldContext))
            return false;
    ++summary->walkedFxCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_FxImpactTable: 8-byte root in the temp block; name XString,
// then the 12x33 fx-handle table in block 4.
bool ReadRetailFxImpactTableBody(RetailZoneLoadSession *session,
                                 FsRetailFastfileReader *reader,
                                 RetailWalkDirectoryRecord *record,
                                 RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 8;
    constexpr uint32_t kTableBytes = 12u * 132u;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t tableRef = ReadRetailLe32(body + 4);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (tableRef != 0);
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;
    if (!tableRef)
    {
        record->state = RETAIL_WALK_WALKED_DEFERRED;
        return true;
    }
    if (tableRef == kInsertReference)
        return false;
    if (tableRef != kInlineReference)
        return IsRetailWireAlias(session, tableRef, 1);
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, kTableBytes, 4))
        return false;
    const uint32_t tableStart = session->wire.cursor[4] - kTableBytes;
    record->nestedBodyBytes += kTableBytes;
    for (uint32_t entry = 0; entry < 12u * 33u; ++entry)
        if (!ReadRetailAssetSlotBody(
                session, reader,
                ReadRetailLe32(session->zoneMemory->blocks[4].data + tableStart +
                               entry * 4u),
                RETAIL_WALK_NESTED_FX_EFFECT_DEF, record, summary))
            return false;
    ++summary->walkedImpactFxCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_XAnimPartTrans: 4-byte header; a non-zero size streams the
// 28-byte frame header, immediate indices (width keyed on numframes), and
// the optional small/large translation data.
bool ReadRetailXAnimPartTransBody(RetailZoneLoadSession *session,
                                  FsRetailFastfileReader *reader,
                                  uint32_t numFrames)
{
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, 4, 4))
        return false;
    const uint32_t header = session->wire.cursor[4] - 4;
    const uint8_t *wire = session->zoneMemory->blocks[4].data + header;
    const uint32_t size = ReadRetailLe16(wire);
    const uint32_t smallTrans = wire[2];
    if (!size)
        return RetailZoneLoadSessionReadStream(session, reader, 4, 12, 1);
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, 28, 1))
        return false;
    const uint32_t indexBytes = numFrames >= 0x100 ? (size + 1u) * 2u : (size + 1u);
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, indexBytes, 1))
        return false;
    if (ReadRetailLe32(session->zoneMemory->blocks[4].data + header + 4 + 24))
    {
        const uint32_t count = size + 1u;
        return RetailZoneLoadSessionReadStream(
            session, reader, 4, smallTrans ? count * 3u : count * 6u,
            smallTrans ? 1 : 4);
    }
    return true;
}

// Android Load_XAnimDeltaPartQuat: 4-byte header; a non-zero size streams
// the 4-byte frame header, immediate indices, and optional quat frames.
bool ReadRetailXAnimDeltaPartQuatBody(RetailZoneLoadSession *session,
                                      FsRetailFastfileReader *reader,
                                      uint32_t numFrames)
{
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, 4, 4))
        return false;
    const uint32_t header = session->wire.cursor[4] - 4;
    const uint32_t size = ReadRetailLe16(session->zoneMemory->blocks[4].data + header);
    if (!size)
        return RetailZoneLoadSessionReadStream(session, reader, 4, 4, 1);
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, 4, 1))
        return false;
    const uint32_t indexBytes = numFrames >= 0x100 ? (size + 1u) * 2u : (size + 1u);
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, indexBytes, 1))
        return false;
    if (ReadRetailLe32(session->zoneMemory->blocks[4].data + header + 4))
    {
        const uint32_t count = size + 1u;
        return RetailZoneLoadSessionReadStream(session, reader, 4, count * 4u, 4);
    }
    return true;
}

// Android Load_XAnimParts: 88-byte root in the temp block; bone-name table,
// notifies, the delta part, six data arrays, and the index union, all in
// block 4 with widths keyed on numframes.
bool ReadRetailXAnimPartsBody(RetailZoneLoadSession *session,
                              FsRetailFastfileReader *reader,
                              RetailWalkDirectoryRecord *record,
                              RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 88;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t numFrames = ReadRetailLe16(body + 14);
    const uint32_t totalBones = body[27];
    const uint32_t notifyCount = body[28];
    const uint32_t indexCount = ReadRetailLe32(body + 36);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;

    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;
    if (ReadRetailLe32(body + 48) &&
        !RetailZoneLoadSessionReadStream(session, reader, 4, totalBones * 2u, 2))
        return false;
    if (ReadRetailLe32(body + 80) &&
        !RetailZoneLoadSessionReadStream(session, reader, 4, notifyCount * 8u, 4))
        return false;
    if (ReadRetailLe32(body + 84))
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, 8, 4))
            return false;
        const uint32_t delta = session->wire.cursor[4] - 8;
        const uint8_t *deltaWire = session->zoneMemory->blocks[4].data + delta;
        if (ReadRetailLe32(deltaWire) &&
            !ReadRetailXAnimPartTransBody(session, reader, numFrames))
            return false;
        if (ReadRetailLe32(deltaWire + 4) &&
            !ReadRetailXAnimDeltaPartQuatBody(session, reader, numFrames))
            return false;
    }

    struct DataArray
    {
        uint32_t slot;
        uint32_t countOffset;
        bool countIsU32;
        uint32_t elementSize;
        uint32_t alignment;
    };
    const DataArray arrays[] = {
        {52, 4, false, 1, 1},   // dataByte
        {56, 6, false, 2, 2},   // dataShort
        {60, 8, false, 4, 4},   // dataInt
        {64, 32, true, 2, 2},   // randomDataShort
        {68, 10, false, 1, 1},  // randomDataByte
        {72, 12, false, 4, 4},  // randomDataInt
    };
    for (const DataArray &array : arrays)
    {
        if (!ReadRetailLe32(body + array.slot))
            continue;
        const uint32_t count = array.countIsU32
            ? ReadRetailLe32(body + array.countOffset)
            : ReadRetailLe16(body + array.countOffset);
        if (!ReadRetailArray(session, reader, 4, count, array.elementSize,
                             array.alignment))
            return false;
        record->nestedBodyBytes += count * array.elementSize;
    }
    if (ReadRetailLe32(body + 76))
    {
        const uint32_t indexBytes = numFrames >= 0x100 ? indexCount * 2u : indexCount;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, indexBytes,
                                             numFrames >= 0x100 ? 2 : 1))
            return false;
    }
    ++summary->walkedXAnimCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_XSurfaceCollisionTree: 40-byte record with 16-byte aligned
// nodes and 2-byte aligned leafs in block 4.
bool ReadRetailXSurfaceCollisionTreeBody(RetailZoneLoadSession *session,
                                         FsRetailFastfileReader *reader,
                                         uint32_t offset)
{
    const uint8_t *body = session->zoneMemory->blocks[4].data + offset;
    const uint32_t nodeCount = ReadRetailLe32(body + 24);
    const uint32_t leafCount = ReadRetailLe32(body + 32);
    if (ReadRetailLe32(body + 28) &&
        !ReadRetailArray(session, reader, 4, nodeCount, 16, 16))
        return false;
    if (ReadRetailLe32(body + 36) &&
        !ReadRetailArray(session, reader, 4, leafCount, 2, 2))
        return false;
    return true;
}

// Android Load_XSurface: blend info in block 4, packed vertices in block 7,
// rigid vert lists with collision trees in block 4, triangle indices in
// block 8.  The vertex/index leaves stay in their geometry blocks because
// the renderer computes GPU offsets from those CPU bases.
bool ReadRetailXSurfaceBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader, uint32_t offset,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary)
{
    const uint8_t *body = session->zoneMemory->blocks[4].data + offset;
    const uint32_t vertCount = ReadRetailLe16(body + 2);
    const uint32_t triCount = ReadRetailLe16(body + 4);
    ++summary->walkedXModelSurfaceCount;

    if (ReadRetailLe32(body + 24) == kInlineReference)
    {
        static const uint32_t kWeights[4] = {1, 3, 5, 7};
        uint32_t blendEntries = 0;
        for (uint32_t weight = 0; weight < 4; ++weight)
            blendEntries += kWeights[weight] * ReadRetailLe16(body + 16 + weight * 2u);
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             blendEntries * 2u, 2))
            return false;
    }
    if (ReadRetailLe32(body + 28) == kInlineReference &&
        !RetailZoneLoadSessionReadStream(session, reader, 7, vertCount * 32u, 16))
        return false;
    if (ReadRetailLe32(body + 36) == kInlineReference)
    {
        const uint32_t vertListCount = ReadRetailLe32(body + 32);
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             vertListCount * 12u, 4))
            return false;
        const uint32_t lists = session->wire.cursor[4] - vertListCount * 12u;
        for (uint32_t list = 0; list < vertListCount; ++list)
        {
            if (ReadRetailLe32(session->zoneMemory->blocks[4].data + lists +
                               list * 12u + 8u) != kInlineReference)
                continue;
            const uint32_t treeStart = (session->wire.cursor[4] + 3u) & ~3u;
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, 40, 4))
                return false;
            if (!ReadRetailXSurfaceCollisionTreeBody(session, reader, treeStart))
                return false;
        }
    }
    if (ReadRetailLe32(body + 12) == kInlineReference &&
        !RetailZoneLoadSessionReadStream(session, reader, 8, triCount * 6u, 16))
        return false;
    return true;
}

// Android Load_BrushWrapper: 80-byte record with sides, adjacency bytes,
// and planes in block 4.
bool ReadRetailBrushWrapperBody(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader, uint32_t offset)
{
    const uint8_t *body = session->zoneMemory->blocks[4].data + offset;
    const uint32_t numSides = ReadRetailLe32(body + 28);
    if (ReadRetailLe32(body + 32))
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, numSides * 12u, 4))
            return false;
        const uint32_t sides = session->wire.cursor[4] - numSides * 12u;
        for (uint32_t side = 0; side < numSides; ++side)
        {
            if (ReadRetailLe32(session->zoneMemory->blocks[4].data + sides +
                               side * 12u) != kInlineReference)
                continue;
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, 20, 4))
                return false;
        }
    }
    if (ReadRetailLe32(body + 48) &&
        !RetailZoneLoadSessionReadStream(session, reader, 4,
                                         ReadRetailLe32(body + 72), 1))
        return false;
    if (ReadRetailLe32(body + 76) == kInlineReference &&
        !RetailZoneLoadSessionReadStream(session, reader, 4, numSides * 20u, 4))
        return false;
    return true;
}

// Android Load_PhysGeomList: 44-byte record with the geometry array and
// nested brush wrappers in block 4.
bool ReadRetailPhysGeomListBody(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader, uint32_t offset)
{
    const uint8_t *body = session->zoneMemory->blocks[4].data + offset;
    const uint32_t count = ReadRetailLe32(body);
    if (!ReadRetailLe32(body + 4))
        return true;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, count * 68u, 4))
        return false;
    const uint32_t geoms = session->wire.cursor[4] - count * 68u;
    for (uint32_t geom = 0; geom < count; ++geom)
    {
        if (ReadRetailLe32(session->zoneMemory->blocks[4].data + geoms + geom * 68u) !=
            kInlineReference)
            continue;
        const uint32_t brushStart = (session->wire.cursor[4] + 3u) & ~3u;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, 80, 4))
            return false;
        if (!ReadRetailBrushWrapperBody(session, reader, brushStart))
            return false;
    }
    return true;
}

// Android Load_XModel: 220-byte root in the temp block; bone tables, the
// block-7/8 surface leaves, per-surface materials, collision surfaces, bone
// info, the phys preset, and the physics geometry list.
bool ReadRetailXModelBody(RetailZoneLoadSession *session,
                          FsRetailFastfileReader *reader,
                          RetailWalkDirectoryRecord *record,
                          RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 220;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t numBones = body[4];
    const uint32_t numRootBones = body[5];
    const uint32_t numSurfs = body[6];
    const uint32_t animatedBones = numBones - numRootBones;
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;

    struct BoneArray
    {
        uint32_t slot;
        uint32_t byteCount;
        uint32_t alignment;
    };
    const BoneArray boneArrays[] = {
        {8, numBones * 2u, 2},          // boneNames (script strings)
        {12, animatedBones, 1},         // parentList
        {16, animatedBones * 8u, 2},    // quats (4 shorts each)
        {20, animatedBones * 16u, 4},   // trans (4 floats each)
        {24, numBones, 1},              // partClassification
        {28, numBones * 32u, 4},        // baseMat
    };
    for (const BoneArray &array : boneArrays)
    {
        if (ReadRetailLe32(body + array.slot) != kInlineReference)
            continue;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             array.byteCount, array.alignment))
            return false;
    }

    if (ReadRetailLe32(body + 32))
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, numSurfs * 56u, 4))
            return false;
        const uint32_t surfs = session->wire.cursor[4] - numSurfs * 56u;
        for (uint32_t surf = 0; surf < numSurfs; ++surf)
            if (!ReadRetailXSurfaceBody(session, reader, surfs + surf * 56u,
                                        record, summary))
                return false;
    }
    if (ReadRetailLe32(body + 36))
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, numSurfs * 4u, 4))
            return false;
        const uint32_t materials = session->wire.cursor[4] - numSurfs * 4u;
        for (uint32_t material = 0; material < numSurfs; ++material)
            if (!ReadRetailAssetSlotBody(
                    session, reader,
                    ReadRetailLe32(session->zoneMemory->blocks[4].data + materials +
                                   material * 4u),
                    RETAIL_WALK_NESTED_MATERIAL, record, summary))
                return false;
    }
    if (ReadRetailLe32(body + 152))
    {
        const uint32_t numCollSurfs = ReadRetailLe32(body + 156);
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             numCollSurfs * 44u, 4))
            return false;
        const uint32_t collSurfs = session->wire.cursor[4] - numCollSurfs * 44u;
        for (uint32_t coll = 0; coll < numCollSurfs; ++coll)
        {
            const uint8_t *entry = session->zoneMemory->blocks[4].data + collSurfs +
                                   coll * 44u;
            if (!ReadRetailLe32(entry))
                continue;
            if (!RetailZoneLoadSessionReadStream(
                    session, reader, 4, ReadRetailLe32(entry + 4) * 48u, 4))
                return false;
        }
    }
    if (ReadRetailLe32(body + 164) &&
        !RetailZoneLoadSessionReadStream(session, reader, 4, numBones * 40u, 4))
        return false;
    if (!ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + 212),
                                 RETAIL_WALK_NESTED_PHYS_PRESET, record, summary))
        return false;
    if (ReadRetailLe32(body + 216) == kInlineReference)
    {
        const uint32_t geomsStart = (session->wire.cursor[4] + 3u) & ~3u;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, 44, 4))
            return false;
        if (!ReadRetailPhysGeomListBody(session, reader, geomsStart))
            return false;
    }
    ++summary->walkedXModelCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Android Load_XStringPtrSlot: only the -1 token allocates the inner 4-byte
// slot which is then read as an XString; any other value stays as-is.
bool ReadRetailXStringPtrSlot(RetailZoneLoadSession *session,
                              FsRetailFastfileReader *reader, uint32_t block,
                              uint32_t offset)
{
    if (ReadRetailLe32(session->zoneMemory->blocks[block].data + offset) !=
        kInlineReference)
        return true;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, 4, 4))
        return false;
    const uint32_t inner = session->wire.cursor[4] - 4;
    uint32_t bytes = 0;
    return ReadRetailXString(
        session, reader,
        ReadRetailLe32(session->zoneMemory->blocks[4].data + inner), &bytes);
}

// Android Load_WeaponDef: 2168-byte root in the temp block; the pointer
// walk is the engine's own field order (the WeaponDef field list generated
// from Load_WeaponDef).  XModel/FX/Material fields are asset-style slots,
// sound fields are XStringPtr slots, and the two knot arrays stream behind
// their inline pointers.
bool ReadRetailWeaponDefBody(RetailZoneLoadSession *session,
                             FsRetailFastfileReader *reader,
                             RetailWalkDirectoryRecord *record,
                             RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 2168;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    record->bodyBytes = session->wire.cursor[0] - bodyStart;

    const auto ReadWeaponStr = [&](uint32_t offset) {
        uint32_t bytes = 0;
        if (!ReadRetailXString(session, reader, ReadRetailLe32(body + offset),
                               &bytes))
            return false;
        record->nameBytes += bytes;
        return true;
    };
    const auto ReadWeaponStrArray = [&](uint32_t offset, uint32_t count) {
        for (uint32_t index = 0; index < count; ++index)
            if (!ReadWeaponStr(offset + index * 4u))
                return false;
        return true;
    };
    const auto ReadWeaponModelArray = [&](uint32_t offset, uint32_t count) {
        for (uint32_t index = 0; index < count; ++index)
            if (!ReadRetailAssetSlotBody(
                    session, reader, ReadRetailLe32(body + offset + index * 4u),
                    RETAIL_WALK_NESTED_XMODEL, record, summary))
                return false;
        return true;
    };
    const auto ReadWeaponSoundPtrArray = [&](uint32_t offset, uint32_t count) {
        if (ReadRetailLe32(body + offset) != kInlineReference)
            return true;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, count * 4u, 4))
            return false;
        const uint32_t array = session->wire.cursor[4] - count * 4u;
        record->nestedBodyBytes += count * 4u;
        for (uint32_t index = 0; index < count; ++index)
            if (!ReadRetailXStringPtrSlot(session, reader, 4, array + index * 4u))
                return false;
        return true;
    };
    const auto ReadWeaponKnots = [&](uint32_t offset, uint32_t countOffset) {
        if (ReadRetailLe32(body + offset) != kInlineReference)
            return true;
        const uint32_t count = ReadRetailLe32(body + countOffset);
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, count * 8u, 4))
            return false;
        record->nestedBodyBytes += count * 8u;
        return true;
    };

    if (!ReadWeaponStr(0) || !ReadWeaponStr(4) || !ReadWeaponStr(8) ||
        !ReadWeaponModelArray(12, 16) ||
        !ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + 76),
                                 RETAIL_WALK_NESTED_XMODEL, record, summary) ||
        !ReadWeaponStrArray(80, 33) || !ReadWeaponStr(212))
        return false;
    if (!ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + 332),
                                 RETAIL_WALK_NESTED_FX_EFFECT_DEF, record,
                                 summary) ||
        !ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + 336),
                                 RETAIL_WALK_NESTED_FX_EFFECT_DEF, record,
                                 summary))
        return false;
    for (uint32_t offset = 340; offset <= 516; offset += 4)
        if (!ReadRetailXStringPtrSlot(session, reader, 0, bodyStart + offset))
            return false;
    if (!ReadWeaponSoundPtrArray(520, 29))
        return false;
    for (const uint32_t offset : {524u, 528u, 532u, 536u})
        if (!ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + offset),
                                     RETAIL_WALK_NESTED_FX_EFFECT_DEF, record,
                                     summary))
            return false;
    for (const uint32_t offset : {540u, 544u})
        if (!ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + offset),
                                     RETAIL_WALK_NESTED_MATERIAL, record, summary))
            return false;
    if (!ReadWeaponModelArray(700, 16))
        return false;
    for (const uint32_t offset : {764u, 768u, 772u, 776u})
        if (!ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + offset),
                                     RETAIL_WALK_NESTED_XMODEL, record, summary))
            return false;
    for (const uint32_t offset : {780u, 788u})
        if (!ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + offset),
                                     RETAIL_WALK_NESTED_MATERIAL, record, summary))
            return false;
    if (!ReadWeaponStr(804) || !ReadWeaponStr(812) || !ReadWeaponStr(832))
        return false;
    for (const uint32_t offset : {1072u, 1076u})
        if (!ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + offset),
                                     RETAIL_WALK_NESTED_MATERIAL, record, summary))
            return false;
    for (const uint32_t offset : {1304u, 1316u})
        if (!ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + offset),
                                     RETAIL_WALK_NESTED_MATERIAL, record, summary))
            return false;
    if (!ReadWeaponStr(1340))
        return false;
    if (!ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + 1412),
                                 RETAIL_WALK_NESTED_XMODEL, record, summary))
        return false;
    for (const uint32_t offset : {1420u, 1428u, 1704u, 1732u})
        if (!ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + offset),
                                     RETAIL_WALK_NESTED_FX_EFFECT_DEF, record,
                                     summary))
            return false;
    for (const uint32_t offset : {1432u, 1436u, 1736u})
        if (!ReadRetailXStringPtrSlot(session, reader, 0, bodyStart + offset))
            return false;
    if (!ReadWeaponStr(1900) || !ReadWeaponKnots(1908, 1924) ||
        !ReadWeaponKnots(1916, 1924) || !ReadWeaponStr(1904) ||
        !ReadWeaponKnots(1912, 1928) || !ReadWeaponKnots(1920, 1928))
        return false;
    if (!ReadWeaponStr(2012) || !ReadWeaponStr(2016) || !ReadWeaponStr(2036) ||
        !ReadWeaponStr(2152) || !ReadWeaponStr(2156))
        return false;
    ++summary->walkedWeaponCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// db_load Load_MapEnts order, via Android LoadMapEnts: 12-byte root in the
// temp block (name, entity-string slot, char count), the name XString in
// block 4, then the raw entity bytes in block 4 when the entity slot is
// non-null. A non-inline entity array slot is an already-loaded alias and
// consumes no stream bytes, matching the menu-pointer-table precedent.
bool ReadRetailMapEntsBody(RetailZoneLoadSession *session,
                           FsRetailFastfileReader *reader,
                           RetailWalkDirectoryRecord *record,
                           RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 12;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t entityRef = ReadRetailLe32(body + 4);
    const uint32_t numChars = ReadRetailLe32(body + 8);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (entityRef != 0);
    // Capture span starts BEFORE the reads below advance the cursors: both
    // land bytewise in zone block 4 (no padding), so [start, start+count)
    // is the exact authored span.
    const uint32_t nameStart = session->wire.cursor[4];
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;
    uint32_t entityStart = 0;
    if (entityRef)
    {
        if (entityRef != kInlineReference &&
            !IsRetailWireAlias(session, entityRef, 1))
            return false;
        if (entityRef == kInlineReference)
        {
            entityStart = session->wire.cursor[4];
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, numChars, 1))
                return false;
            record->nestedBodyBytes += numChars;
        }
    }
    // Live-ClipMap capture (see the session fields): publish the spans for
    // the live branch to copy+register. Fails nothing by itself; the live
    // branch treats an absent capture as walk-only. The entity span must be
    // inline; the name is either the inline span or an alias token to an
    // already-streamed block-4 string (killhouse's ClipMap names its nested
    // MapEnts by absolute offset) -- resolved here against the zone blocks,
    // which only grow, with a bounded NUL scan. A missing entity string or
    // unresolvable name captures nothing.
    if (session->captureNestedMapEnts)
    {
        bool captured = entityRef == kInlineReference && entityRef != 0 && numChars != 0;
        uint32_t nameSpanStart = 0, nameSpanBytes = 0;
        if (captured)
        {
            if (nameRef == kInlineReference)
            {
                nameSpanStart = nameStart;
                nameSpanBytes = record->nameBytes;
            }
            else if (nameRef != 0)
            {
                XBlock blocks[9]{};
                for (uint32_t block = 0; block < 9; ++block)
                    blocks[block] = session->zoneMemory->blocks[block];
                RetailWireToken token{};
                if (RetailWireTokenDecodeBlocks(blocks, {nameRef}, 1, 1u << 4, &token) &&
                    token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4)
                {
                    const XBlock &nameBlock = session->zoneMemory->blocks[4];
                    uint32_t len = 0;
                    while (token.offset + len < nameBlock.size &&
                           nameBlock.data[token.offset + len] != 0 && len < 256)
                        ++len;
                    if (token.offset + len < nameBlock.size &&
                        nameBlock.data[token.offset + len] == 0)
                    {
                        nameSpanStart = token.offset;
                        nameSpanBytes = len + 1;
                    }
                    else
                    {
                        captured = false;
                    }
                }
                else
                {
                    captured = false;
                }
            }
            else
            {
                captured = false;
            }
        }
        if (captured)
        {
            session->capturedNestedMapEnts = true;
            session->capturedMapEntsNameStart = nameSpanStart;
            session->capturedMapEntsNameBytes = nameSpanBytes;
            session->capturedMapEntsEntStart = entityStart;
            session->capturedMapEntsEntLen = numChars;
        }
        else
        {
            Com_Printf(0,
                       "RetailWalk: nested mapents not captured nameRef=0x%08x entityRef=0x%08x chars=%u\n",
                       nameRef, entityRef, numChars);
        }
    }
    ++summary->walkedMapEntsCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// db_load Load_ComWorld order, via Android LoadComWorld: 16-byte root in the
// temp block (name, isInUse, primary-light count, light-array slot), the
// name XString in block 4, then count 68-byte ComPrimaryLight records in
// block 4 each carrying its LightDef-name XString at +64. The light array
// follows the same inline-or-alias rule as the entity bytes above.
bool ReadRetailComWorldBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 16;
    constexpr uint32_t kLightBytes = 68;
    constexpr uint32_t kDefNameOffset = 64;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t lightCount = ReadRetailLe32(body + 8);
    const uint32_t lightsRef = ReadRetailLe32(body + 12);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (lightsRef != 0);
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;
    if (lightsRef)
    {
        if (lightsRef != kInlineReference &&
            !IsRetailWireAlias(session, lightsRef, 1))
            return false;
        if (lightsRef == kInlineReference)
        {
            if (lightCount > UINT32_MAX / kLightBytes)
                return false;
            if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                                 lightCount * kLightBytes, 4))
                return false;
            const uint32_t lightsStart = session->wire.cursor[4] -
                                         lightCount * kLightBytes;
            record->nestedBodyBytes += lightCount * kLightBytes;
            for (uint32_t light = 0; light < lightCount; ++light)
            {
                const uint32_t defNameRef = ReadRetailLe32(
                    session->zoneMemory->blocks[4].data + lightsStart +
                    light * kLightBytes + kDefNameOffset);
                uint32_t defNameBytes = 0;
                if (!ReadRetailXString(session, reader, defNameRef, &defNameBytes))
                    return false;
                record->nestedBodyBytes += defNameBytes;
            }
        }
    }
    ++summary->walkedComWorldCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Recursive half of the GameWorldSP tree walk below: one already-streamed
// 16-byte pathnode_tree_t. An axis<0 leaf streams its u16 node list when the
// +12 slot is non-null; an interior node streams each -1-inline child and
// recurses, while any other child form is an already-loaded alias. The
// visit budget is exact -- every visited node consumed 16 stream bytes -- so
// it can neither loop forever nor false-reject valid trees.
bool ReadRetailPathNodeTreeNode(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader,
                                uint32_t nodeOffset, uint32_t *budget,
                                RetailWalkDirectoryRecord *record)
{
    if (!session || !reader || !budget || !record || !*budget)
        return false;
    --*budget;
    const uint8_t *node = session->zoneMemory->blocks[4].data + nodeOffset;
    const int32_t axis = static_cast<int32_t>(ReadRetailLe32(node));
    if (axis < 0)
    {
        const uint32_t dataRef = ReadRetailLe32(node + 12);
        if (!dataRef)
            return true;
        const uint32_t count = ReadRetailLe32(node + 8);
        if (dataRef != kInlineReference &&
            !IsRetailWireAlias(session, dataRef, 1))
            return false;
        if (dataRef != kInlineReference)
            return true;
        if (count > (UINT32_MAX / 2u))
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, count * 2u, 2))
            return false;
        record->nestedBodyBytes += count * 2u;
        return true;
    }
    for (uint32_t child = 0; child < 2; ++child)
    {
        const uint32_t childRef = ReadRetailLe32(node + 8 + child * 4u);
        if (!childRef)
            continue;
        if (childRef != kInlineReference &&
            !IsRetailWireAlias(session, childRef, 1))
            return false;
        if (childRef != kInlineReference)
            continue;
        const uint32_t childOffset = (session->wire.cursor[4] + 3u) & ~3u;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, 16, 4))
            return false;
        record->nestedBodyBytes +=
            session->wire.cursor[4] - childOffset;
        if (!ReadRetailPathNodeTreeNode(session, reader, childOffset, budget,
                                        record))
            return false;
    }
    return true;
}

// db_load Load_GameWorldSp/Load_PathData order, via Android LoadGameWorldSp:
// 44-byte root in the temp block (name + 40-byte PathData), the name XString
// in block 4, then the 128-byte node array in block 4 (each non-null Links
// slot streams its 12-byte links sized by the u16 totalLinkCount at +62;
// the five u16 script-string fields consume no stream bytes, matching
// Load_ScriptString(0)), the 16-byte base-node array in runtime block 1 via
// ExpandRuntime (zero FS bytes, tracked separately below), the two u16
// chain arrays, the vis buffer, and the 16-byte tree array with recursive
// descent. Every count is overflow-guarded before streaming.
bool ReadRetailGameWorldSpBody(RetailZoneLoadSession *session,
                               FsRetailFastfileReader *reader,
                               RetailWalkDirectoryRecord *record,
                               RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 44;
    constexpr uint32_t kNodeBytes = 128;
    constexpr uint32_t kLinkBytes = 12;
    constexpr uint32_t kBaseBytes = 16;
    constexpr uint32_t kTreeBytes = 16;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t nodeCount = ReadRetailLe32(body + 4);
    const uint32_t nodesRef = ReadRetailLe32(body + 8);
    const uint32_t baseRef = ReadRetailLe32(body + 12);
    const uint32_t chainARef = ReadRetailLe32(body + 20);
    const uint32_t chainBRef = ReadRetailLe32(body + 24);
    const uint32_t visBytes = ReadRetailLe32(body + 28);
    const uint32_t visRef = ReadRetailLe32(body + 32);
    const uint32_t treeCount = ReadRetailLe32(body + 36);
    const uint32_t treeRef = ReadRetailLe32(body + 40);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (nodesRef != 0) +
                                   (baseRef != 0) + (chainARef != 0) +
                                   (chainBRef != 0) + (visRef != 0) +
                                   (treeRef != 0);
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;
    if (nodesRef)
    {
        if (nodesRef != kInlineReference &&
            !IsRetailWireAlias(session, nodesRef, 1))
            return false;
        if (nodesRef == kInlineReference)
        {
            if (nodeCount > UINT32_MAX / kNodeBytes)
                return false;
            if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                                 nodeCount * kNodeBytes, 4))
                return false;
            const uint32_t nodesStart = session->wire.cursor[4] -
                                        nodeCount * kNodeBytes;
            record->nestedBodyBytes += nodeCount * kNodeBytes;
            for (uint32_t node = 0; node < nodeCount; ++node)
            {
                const uint8_t *nodeBody =
                    session->zoneMemory->blocks[4].data + nodesStart +
                    node * kNodeBytes;
                const uint32_t linksRef = ReadRetailLe32(nodeBody + 64);
                if (!linksRef)
                    continue;
                const uint32_t linkCount =
                    static_cast<uint32_t>(nodeBody[62]) |
                    (static_cast<uint32_t>(nodeBody[63]) << 8);
                if (linkCount > UINT32_MAX / kLinkBytes)
                    return false;
                if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                                     linkCount * kLinkBytes,
                                                     4))
                    return false;
                record->nestedBodyBytes += linkCount * kLinkBytes;
            }
        }
    }
    if (baseRef)
    {
        if (nodeCount > UINT32_MAX / kBaseBytes)
            return false;
        if (!RetailZoneLoadSessionExpandRuntime(session, 1,
                                                nodeCount * kBaseBytes, 16))
            return false;
        summary->walkedGameWorldSpBlock1Bytes += nodeCount * kBaseBytes;
    }
    const uint32_t chainRefs[2] = {chainARef, chainBRef};
    for (uint32_t chain = 0; chain < 2; ++chain)
    {
        if (!chainRefs[chain])
            continue;
        if (chainRefs[chain] != kInlineReference &&
            !IsRetailWireAlias(session, chainRefs[chain], 1))
            return false;
        if (chainRefs[chain] != kInlineReference)
            continue;
        if (nodeCount > UINT32_MAX / 2u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             nodeCount * 2u, 2))
            return false;
        record->nestedBodyBytes += nodeCount * 2u;
    }
    if (visRef)
    {
        if (visRef != kInlineReference &&
            !IsRetailWireAlias(session, visRef, 1))
            return false;
        if (visRef == kInlineReference)
        {
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, visBytes,
                                                 1))
                return false;
            record->nestedBodyBytes += visBytes;
        }
    }
    if (treeRef)
    {
        if (treeRef != kInlineReference &&
            !IsRetailWireAlias(session, treeRef, 1))
            return false;
        if (treeRef == kInlineReference)
        {
            if (treeCount > UINT32_MAX / kTreeBytes)
                return false;
            if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                                 treeCount * kTreeBytes, 4))
                return false;
            const uint32_t treesStart = session->wire.cursor[4] -
                                        treeCount * kTreeBytes;
            record->nestedBodyBytes += treeCount * kTreeBytes;
            uint32_t budget =
                treeCount +
                (session->zoneMemory->blocks[4].size -
                 session->wire.cursor[4]) /
                    kTreeBytes;
            for (uint32_t tree = 0; tree < treeCount; ++tree)
                if (!ReadRetailPathNodeTreeNode(
                        session, reader, treesStart + tree * kTreeBytes,
                        &budget, record))
                    return false;
        }
    }
    ++summary->walkedGameWorldSpCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// db_load cplane shape: a 20-byte plane streams only for the -1 slot; any
// other non-null slot is an already-loaded shared plane. Uniform rule for
// every pointer below: null skips, inline streams, anything else must
// decode as an alias and consumes no stream bytes.
bool ReadRetailCPlaneSlot(RetailZoneLoadSession *session,
                          FsRetailFastfileReader *reader, uint32_t slotBlock,
                          uint32_t slotOffset, uint32_t streamBlock,
                          uint32_t count,
                          RetailWalkDirectoryRecord *record)
{
    const uint32_t slot = ReadRetailLe32(
        session->zoneMemory->blocks[slotBlock].data + slotOffset);
    if (!slot)
        return true;
    if (slot != kInlineReference)
        return IsRetailWireAlias(session, slot, 1);
    if (count > UINT32_MAX / 20u)
        return false;
    if (!RetailZoneLoadSessionReadStream(session, reader, streamBlock,
                                         count * 20u, 4))
        return false;
    record->nestedBodyBytes += count * 20u;
    return true;
}

// db_load Load_cbrushside_t order on an already-streamed 12-byte side:
// the plane slot at the side's own offset streams its 20-byte plane.
// Callers stream the side record itself first (array bulk, or the inline
// brush-embedded side below); this helper never streams the side.
bool ReadRetailCBrushSideAt(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader, uint32_t block,
                            uint32_t sideOffset,
                            RetailWalkDirectoryRecord *record)
{
    return ReadRetailCPlaneSlot(session, reader, block, sideOffset, block,
                                1, record);
}

bool ReadRetailCBrushAt(RetailZoneLoadSession *session,
                        FsRetailFastfileReader *reader, uint32_t block,
                        uint32_t brushOffset,
                        RetailWalkDirectoryRecord *record)
{
    const uint8_t *brush =
        session->zoneMemory->blocks[block].data + brushOffset;
    const uint32_t sidesRef = ReadRetailLe32(brush + 32);
    const uint32_t edgeRef = ReadRetailLe32(brush + 48);
    if (sidesRef)
    {
        if (sidesRef != kInlineReference &&
            !IsRetailWireAlias(session, sidesRef, 1))
            return false;
        if (sidesRef == kInlineReference)
        {
            if (!RetailZoneLoadSessionReadStream(session, reader, block, 12,
                                                 4))
                return false;
            record->nestedBodyBytes += 12;
            const uint32_t sideStart = session->wire.cursor[block] - 12;
            if (!ReadRetailCBrushSideAt(session, reader, block, sideStart,
                                        record))
                return false;
        }
    }
    if (edgeRef)
    {
        if (edgeRef != kInlineReference &&
            !IsRetailWireAlias(session, edgeRef, 1))
            return false;
        if (edgeRef == kInlineReference)
        {
            if (!RetailZoneLoadSessionReadStream(session, reader, block, 1,
                                                 1))
                return false;
            record->nestedBodyBytes += 1;
        }
    }
    return true;
}

// Android LoadXModelPiecesSlot: a 12-byte root (name, count, array slot)
// for the -1 slot, then one 16-byte entry per piece each carrying a nested
// XModel asset slot. Any other non-null slot is an already-loaded alias.
bool ReadRetailXModelPiecesSlot(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader, uint32_t block,
                                uint32_t slotOffset,
                                RetailWalkDirectoryRecord *record,
                                RetailWalkDirectoryResult *summary)
{
    const uint32_t slot =
        ReadRetailLe32(session->zoneMemory->blocks[block].data + slotOffset);
    if (!slot)
        return true;
    if (slot != kInlineReference)
        return IsRetailWireAlias(session, slot, 1);
    if (!RetailZoneLoadSessionReadStream(session, reader, block, 12, 4))
        return false;
    record->nestedBodyBytes += 12;
    const uint8_t *root =
        session->zoneMemory->blocks[block].data + session->wire.cursor[block] - 12;
    uint32_t nameBytes = 0;
    if (!ReadRetailXString(session, reader, ReadRetailLe32(root), &nameBytes))
        return false;
    record->nestedBodyBytes += nameBytes;
    const uint32_t count = ReadRetailLe32(root + 4);
    const uint32_t arrayRef = ReadRetailLe32(root + 8);
    if (!arrayRef)
        return true;
    if (arrayRef != kInlineReference &&
        !IsRetailWireAlias(session, arrayRef, 1))
        return false;
    if (arrayRef != kInlineReference)
        return true;
    if (count > UINT32_MAX / 16u)
        return false;
    if (!RetailZoneLoadSessionReadStream(session, reader, block, count * 16u,
                                         4))
        return false;
    const uint32_t arrayStart = session->wire.cursor[block] - count * 16u;
    record->nestedBodyBytes += count * 16u;
    for (uint32_t piece = 0; piece < count; ++piece)
        if (!ReadRetailAssetSlotBody(
                session, reader,
                ReadRetailLe32(session->zoneMemory->blocks[block].data +
                               arrayStart + piece * 16u),
                RETAIL_WALK_NESTED_XMODEL, record, summary))
            return false;
    return true;
}

// Stream count stride-byte records into block 4 for any nonzero slot.
// Collision arrays are never shared: the engine allocates for every
// non-null pointer without a -1 check (Android AllocForSlot). Counts are
// overflow-guarded; short reads rewind and fail.
bool ReadRetailClipArray(RetailZoneLoadSession *session,
                         FsRetailFastfileReader *reader, uint32_t slot,
                         uint32_t count, uint32_t stride, uint32_t alignment,
                         RetailWalkDirectoryRecord *record)
{
    if (!slot)
        return true;
    if (!stride || count > UINT32_MAX / stride)
        return false;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, count * stride,
                                         alignment))
        return false;
    record->nestedBodyBytes += count * stride;
    return true;
}

// db_load Load_clipMap_t order, via Android LoadClipMap against the checked
// clipMap_t/DynEntityDef layouts: 284-byte root in the temp block, the name
// XString in block 4, then collision planes (20 bytes each, inline slots
// only), static models (80 bytes, nested XModel at +4), materials (72 bytes
// opaque), brush sides (12 bytes, plane follows each), brush edges (raw
// bytes), nodes (8 bytes, plane follows each), leaves (44 bytes opaque),
// leaf brushes (u16), leaf-brush nodes (20 bytes, inline u16 lists),
// leaf surfaces (u32), verts (vec3), triangle indices and walkability bits
// (u64-exact), borders (28 bytes opaque), partitions (12 bytes, inline 28
// byte border follows), aabb trees (32 bytes opaque), cmodels (72 bytes
// opaque), brushes (80 bytes, 16-aligned, sides/edge follow), visibility
// bytes (numClusters*clusterBytes), the nested MapEnts asset, the
// inline-only box brush, two dynamic-entity definition lists (96 bytes,
// nested XModel/FX/XModelPieces/PhysPreset), and six runtime block-1 lists
// via ExpandRuntime (poses, clients, collisions). ClipMapPVS has no walker:
// any encounter fails loudly rather than registering SP data.
bool ReadRetailClipMapBody(RetailZoneLoadSession *session,
                           FsRetailFastfileReader *reader,
                           RetailWalkDirectoryRecord *record,
                           RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 284;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t planeCount = ReadRetailLe32(body + 8);
    const uint32_t planesRef = ReadRetailLe32(body + 12);
    const uint32_t staticCount = ReadRetailLe32(body + 16);
    const uint32_t staticRef = ReadRetailLe32(body + 20);
    const uint32_t materialCount = ReadRetailLe32(body + 24);
    const uint32_t materialRef = ReadRetailLe32(body + 28);
    const uint32_t sideCount = ReadRetailLe32(body + 32);
    const uint32_t sideRef = ReadRetailLe32(body + 36);
    const uint32_t edgeCount = ReadRetailLe32(body + 40);
    const uint32_t edgeRef = ReadRetailLe32(body + 44);
    const uint32_t nodeCount = ReadRetailLe32(body + 48);
    const uint32_t nodeRef = ReadRetailLe32(body + 52);
    const uint32_t leafCount = ReadRetailLe32(body + 56);
    const uint32_t leafRef = ReadRetailLe32(body + 60);
    const uint32_t lbnCount = ReadRetailLe32(body + 64);
    const uint32_t lbnRef = ReadRetailLe32(body + 68);
    const uint32_t lbCount = ReadRetailLe32(body + 72);
    const uint32_t lbRef = ReadRetailLe32(body + 76);
    const uint32_t lsurfCount = ReadRetailLe32(body + 80);
    const uint32_t lsurfRef = ReadRetailLe32(body + 84);
    const uint32_t vertCount = ReadRetailLe32(body + 88);
    const uint32_t vertRef = ReadRetailLe32(body + 92);
    const uint32_t triCount = ReadRetailLe32(body + 96);
    const uint32_t triIndexRef = ReadRetailLe32(body + 100);
    const uint32_t triWalkRef = ReadRetailLe32(body + 104);
    const uint32_t borderCount = ReadRetailLe32(body + 108);
    const uint32_t borderRef = ReadRetailLe32(body + 112);
    const uint32_t partCount = ReadRetailLe32(body + 116);
    const uint32_t partRef = ReadRetailLe32(body + 120);
    const uint32_t aabbCount = ReadRetailLe32(body + 124);
    const uint32_t aabbRef = ReadRetailLe32(body + 128);
    const uint32_t cmodelCount = ReadRetailLe32(body + 132);
    const uint32_t cmodelRef = ReadRetailLe32(body + 136);
    const uint32_t brushCount =
        static_cast<uint32_t>(body[140]) |
        (static_cast<uint32_t>(body[141]) << 8);
    const uint32_t brushRef = ReadRetailLe32(body + 144);
    const uint32_t numClusters = ReadRetailLe32(body + 148);
    const uint32_t clusterBytes = ReadRetailLe32(body + 152);
    const uint32_t visRef = ReadRetailLe32(body + 156);
    const uint32_t mapEntsRef = ReadRetailLe32(body + 164);
    const uint32_t boxBrushRef = ReadRetailLe32(body + 168);
    const uint32_t dynCount0 =
        static_cast<uint32_t>(body[244]) |
        (static_cast<uint32_t>(body[245]) << 8);
    const uint32_t dynCount1 =
        static_cast<uint32_t>(body[246]) |
        (static_cast<uint32_t>(body[247]) << 8);
    const uint32_t dynDefRef[2] = {ReadRetailLe32(body + 248),
                                   ReadRetailLe32(body + 252)};
    const uint32_t dynPoseRef[2] = {ReadRetailLe32(body + 256),
                                    ReadRetailLe32(body + 260)};
    const uint32_t dynClientRef[2] = {ReadRetailLe32(body + 264),
                                      ReadRetailLe32(body + 268)};
    const uint32_t dynCollRef[2] = {ReadRetailLe32(body + 272),
                                    ReadRetailLe32(body + 276)};
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount =
        (nameRef != 0) + (planesRef != 0) + (staticRef != 0) +
        (materialRef != 0) + (sideRef != 0) + (edgeRef != 0) +
        (nodeRef != 0) + (leafRef != 0) + (lbnRef != 0) + (lbRef != 0) +
        (lsurfRef != 0) + (vertRef != 0) + (triIndexRef != 0) +
        (triWalkRef != 0) + (borderRef != 0) + (partRef != 0) +
        (aabbRef != 0) + (cmodelRef != 0) + (brushRef != 0) + (visRef != 0) +
        (mapEntsRef != 0) + (boxBrushRef != 0) + (dynDefRef[0] != 0) +
        (dynDefRef[1] != 0) + (dynPoseRef[0] != 0) + (dynPoseRef[1] != 0) +
        (dynClientRef[0] != 0) + (dynClientRef[1] != 0) +
        (dynCollRef[0] != 0) + (dynCollRef[1] != 0);
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;
    if (!ReadRetailCPlaneSlot(session, reader, 0, bodyStart + 12, 4,
                              planeCount, record))
        return false;
    if (staticRef)
    {
        if (staticCount > UINT32_MAX / 80u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             staticCount * 80u, 4))
            return false;
        const uint32_t staticStart =
            session->wire.cursor[4] - staticCount * 80u;
        record->nestedBodyBytes += staticCount * 80u;
        for (uint32_t model = 0; model < staticCount; ++model)
            if (!ReadRetailAssetSlotBody(
                    session, reader,
                    ReadRetailLe32(session->zoneMemory->blocks[4].data +
                                   staticStart + model * 80u + 4),
                    RETAIL_WALK_NESTED_XMODEL, record, summary))
                return false;
    }
    if (!ReadRetailClipArray(session, reader, materialRef, materialCount, 72,
                             4, record))
        return false;
    if (sideRef)
    {
        if (sideCount > UINT32_MAX / 12u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             sideCount * 12u, 4))
            return false;
        const uint32_t sideStart =
            session->wire.cursor[4] - sideCount * 12u;
        record->nestedBodyBytes += sideCount * 12u;
        for (uint32_t side = 0; side < sideCount; ++side)
            if (!ReadRetailCBrushSideAt(session, reader, 4,
                                        sideStart + side * 12u, record))
                return false;
    }
    if (!ReadRetailClipArray(session, reader, edgeRef, edgeCount, 1, 1,
                             record))
        return false;
    if (nodeRef)
    {
        if (nodeCount > UINT32_MAX / 8u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             nodeCount * 8u, 4))
            return false;
        const uint32_t nodeStart =
            session->wire.cursor[4] - nodeCount * 8u;
        record->nestedBodyBytes += nodeCount * 8u;
        for (uint32_t node = 0; node < nodeCount; ++node)
            if (!ReadRetailCPlaneSlot(session, reader, 4, nodeStart + node * 8u,
                                      4, 1, record))
                return false;
    }
    if (!ReadRetailClipArray(session, reader, leafRef, leafCount, 44, 4,
                             record) ||
        !ReadRetailClipArray(session, reader, lbRef, lbCount, 2, 2, record))
        return false;
    if (lbnRef)
    {
        if (lbnCount > UINT32_MAX / 20u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             lbnCount * 20u, 4))
            return false;
        const uint32_t lbnStart =
            session->wire.cursor[4] - lbnCount * 20u;
        record->nestedBodyBytes += lbnCount * 20u;
        for (uint32_t lbn = 0; lbn < lbnCount; ++lbn)
        {
            const uint8_t *node =
                session->zoneMemory->blocks[4].data + lbnStart + lbn * 20u;
            const int16_t brushCount =
                static_cast<int16_t>(static_cast<uint32_t>(node[2]) |
                                     (static_cast<uint32_t>(node[3]) << 8));
            if (brushCount <= 0)
                continue;
            const uint32_t brushesRef = ReadRetailLe32(node + 8);
            if (!brushesRef)
                continue;
            if (brushesRef != kInlineReference &&
                !IsRetailWireAlias(session, brushesRef, 1))
                return false;
            if (brushesRef != kInlineReference)
                continue;
            if (!RetailZoneLoadSessionReadStream(
                    session, reader, 4,
                    static_cast<uint32_t>(brushCount) * 2u, 2))
                return false;
            record->nestedBodyBytes +=
                static_cast<uint32_t>(brushCount) * 2u;
        }
    }
    if (!ReadRetailClipArray(session, reader, lsurfRef, lsurfCount, 4, 4,
                             record) ||
        !ReadRetailClipArray(session, reader, vertRef, vertCount, 12, 4,
                             record))
        return false;
    if (triIndexRef)
    {
        const uint64_t triIndices = 2u * (3u * (uint64_t)triCount);
        if (triIndices > UINT32_MAX)
            return false;
        if (!RetailZoneLoadSessionReadStream(
                session, reader, 4, (uint32_t)triIndices, 2))
            return false;
        record->nestedBodyBytes += (uint32_t)triIndices;
    }
    if (triWalkRef)
    {
        const uint64_t walkable =
            4u * (((3u * (uint64_t)triCount + 31u) >> 5));
        if (walkable > UINT32_MAX)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             (uint32_t)walkable, 1))
            return false;
        record->nestedBodyBytes += (uint32_t)walkable;
    }
    if (!ReadRetailClipArray(session, reader, borderRef, borderCount, 28, 4,
                             record))
        return false;
    if (partRef)
    {
        if (partCount > UINT32_MAX / 12u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             partCount * 12u, 4))
            return false;
        const uint32_t partStart =
            session->wire.cursor[4] - partCount * 12u;
        record->nestedBodyBytes += partCount * 12u;
        for (uint32_t part = 0; part < partCount; ++part)
        {
            const uint32_t borderSlot = ReadRetailLe32(
                session->zoneMemory->blocks[4].data + partStart + part * 12u +
                8);
            if (!borderSlot)
                continue;
            if (borderSlot != kInlineReference &&
                !IsRetailWireAlias(session, borderSlot, 1))
                return false;
            if (borderSlot != kInlineReference)
                continue;
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, 28, 4))
                return false;
            record->nestedBodyBytes += 28;
        }
    }
    if (!ReadRetailClipArray(session, reader, aabbRef, aabbCount, 32, 4,
                             record) ||
        !ReadRetailClipArray(session, reader, cmodelRef, cmodelCount, 72, 4,
                             record))
        return false;
    if (brushRef)
    {
        if (brushCount > UINT32_MAX / 80u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             brushCount * 80u, 16))
            return false;
        const uint32_t brushStart =
            session->wire.cursor[4] - brushCount * 80u;
        record->nestedBodyBytes += brushCount * 80u;
        for (uint32_t brush = 0; brush < brushCount; ++brush)
            if (!ReadRetailCBrushAt(session, reader, 4,
                                    brushStart + brush * 80u, record))
                return false;
    }
    if (visRef)
    {
        const uint64_t visBytes = (uint64_t)numClusters * clusterBytes;
        if (visBytes > UINT32_MAX)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             (uint32_t)visBytes, 1))
            return false;
        record->nestedBodyBytes += (uint32_t)visBytes;
    }
    if (!ReadRetailAssetSlotBody(session, reader, mapEntsRef,
                                 RETAIL_WALK_NESTED_MAP_ENTS, record, summary))
        return false;
    if (boxBrushRef)
    {
        if (boxBrushRef != kInlineReference &&
            !IsRetailWireAlias(session, boxBrushRef, 1))
            return false;
        if (boxBrushRef == kInlineReference)
        {
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, 80, 16))
                return false;
            const uint32_t boxStart = session->wire.cursor[4] - 80;
            record->nestedBodyBytes += 80;
            if (!ReadRetailCBrushAt(session, reader, 4, boxStart, record))
                return false;
        }
    }
    const uint32_t dynCounts[2] = {dynCount0, dynCount1};
    for (uint32_t list = 0; list < 2; ++list)
    {
        if (!dynDefRef[list])
            continue;
        if (dynCounts[list] > UINT32_MAX / 96u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             dynCounts[list] * 96u, 4))
            return false;
        const uint32_t defStart =
            session->wire.cursor[4] - dynCounts[list] * 96u;
        record->nestedBodyBytes += dynCounts[list] * 96u;
        for (uint32_t def = 0; def < dynCounts[list]; ++def)
        {
            const uint8_t *defBody =
                session->zoneMemory->blocks[4].data + defStart + def * 96u;
            if (!ReadRetailAssetSlotBody(session, reader,
                                         ReadRetailLe32(defBody + 32),
                                         RETAIL_WALK_NESTED_XMODEL, record,
                                         summary) ||
                !ReadRetailAssetSlotBody(session, reader,
                                         ReadRetailLe32(defBody + 40),
                                         RETAIL_WALK_NESTED_FX_EFFECT_DEF,
                                         record, summary) ||
                !ReadRetailXModelPiecesSlot(session, reader, 4,
                                            defStart + def * 96u + 44, record,
                                            summary) ||
                !ReadRetailAssetSlotBody(session, reader,
                                         ReadRetailLe32(defBody + 48),
                                         RETAIL_WALK_NESTED_PHYS_PRESET,
                                         record, summary))
                return false;
        }
    }
    struct ClipRuntimeList
    {
        uint32_t slot;
        uint32_t stride;
        uint32_t list;
    };
    static const ClipRuntimeList kRuntimeLists[] = {
        {256, 32, 0}, {260, 32, 1}, {264, 12, 0},
        {268, 12, 1}, {272, 20, 0}, {276, 20, 1},
    };
    for (uint32_t list = 0; list < 6; ++list)
    {
        const uint32_t slot =
            ReadRetailLe32(body + kRuntimeLists[list].slot);
        if (!slot)
            continue;
        const uint32_t count = dynCounts[kRuntimeLists[list].list];
        if (count > UINT32_MAX / kRuntimeLists[list].stride)
            return false;
        if (!RetailZoneLoadSessionExpandRuntime(
                session, 1, count * kRuntimeLists[list].stride, 4))
            return false;
        summary->walkedClipMapBlock1Bytes +=
            count * kRuntimeLists[list].stride;
    }
    ++summary->walkedClipMapCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// db_load Load_GfxAabbTree order on an already-streamed 44-byte tree: the
// u16-indexed smodel list streams inline only for the -1 slot (count from
// the u16 at +34); any other non-null slot is an already-loaded alias.
bool ReadRetailGfxAabbTreeAt(RetailZoneLoadSession *session,
                             FsRetailFastfileReader *reader, uint32_t block,
                             uint32_t treeOffset,
                             RetailWalkDirectoryRecord *record,
                             RetailGfxCellTrace *trace)
{
    const uint8_t *tree =
        session->zoneMemory->blocks[block].data + treeOffset;
    const uint32_t indexesRef = ReadRetailLe32(tree + 36);
    if (!indexesRef)
        return true;
    if (indexesRef != kInlineReference &&
        !IsRetailWireAlias(session, indexesRef, 1))
        return false;
    if (indexesRef != kInlineReference)
    {
        const uint32_t count =
            static_cast<uint32_t>(tree[34]) |
            (static_cast<uint32_t>(tree[35]) << 8);
        if (trace)
            trace->Record(RetailGfxCellTrace::TREE_LIST_ALIAS, 0, count,
                          indexesRef);
        return true;
    }
    const uint32_t count =
        static_cast<uint32_t>(tree[34]) |
        (static_cast<uint32_t>(tree[35]) << 8);
    if (count > UINT32_MAX / 2u)
        return false;
    if (!RetailZoneLoadSessionReadStream(session, reader, block, count * 2u,
                                         2))
        return false;
    const uint32_t listStart = session->wire.cursor[block] - count * 2u;
    if (trace)
        trace->Record(RetailGfxCellTrace::TREE_LIST, listStart, count, 0);
    record->nestedBodyBytes += count * 2u;
    return true;
}

bool ReadRetailGfxPortalAt(RetailZoneLoadSession *session,
                           FsRetailFastfileReader *reader,
                           uint32_t portalOffset, uint32_t *budget,
                           RetailWalkDirectoryRecord *record,
                           RetailWalkDirectoryResult *summary,
                           RetailGfxCellTrace *trace)
{
    if (!session || !reader || !budget || !record || !summary)
        return false;
    ++summary->walkedGfxPortalCount;
    const uint8_t *portal =
        session->zoneMemory->blocks[4].data + portalOffset;
    const uint32_t cellRef = ReadRetailLe32(portal + 32);
    const uint32_t vertsRef = ReadRetailLe32(portal + 36);
    const uint32_t vertexCount = portal[40];
    if (cellRef)
    {
        if (cellRef != kInlineReference &&
            !IsRetailWireAlias(session, cellRef, 1))
            return false;
        if (cellRef == kInlineReference)
        {
            if (!*budget)
                return false;
            --*budget;
            const uint32_t cellStart =
                (session->wire.cursor[4] + 3u) & ~3u;
            if (summary->lastGfxPortalCount && portalOffset >= summary->lastGfxPortalOffset)
            {
                const uint32_t index = (portalOffset - summary->lastGfxPortalOffset) / 68u;
                if (index < 256)
                    summary->gfxTopPortalChildOffsets[index] = cellStart;
            }
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, 56, 4))
                return false;
            if (trace)
                trace->Record(RetailGfxCellTrace::PORTAL_CELL_INLINE, cellStart,
                              1, 0);
            record->nestedBodyBytes += session->wire.cursor[4] - cellStart;
            if (!ReadRetailGfxCellAt(session, reader, cellStart, budget,
                                     record, summary, trace))
                return false;
        }
        else if (trace)
        {
            trace->Record(RetailGfxCellTrace::PORTAL_CELL_ALIAS, 0, 1, cellRef);
        }
    }
    if (!vertsRef)
        return true;
    if (vertsRef != kInlineReference &&
        !IsRetailWireAlias(session, vertsRef, 1))
        return false;
    if (vertsRef != kInlineReference)
        return true;
    if ((uint64_t)vertexCount * 12u > UINT32_MAX)
        return false;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                         vertexCount * 12u, 4))
        return false;
    {
        const uint32_t vertsStart = session->wire.cursor[4] - vertexCount * 12u;
        if (trace)
            trace->Record(RetailGfxCellTrace::PORTAL_VERTS, vertsStart, vertexCount, 0);
    }
    record->nestedBodyBytes += vertexCount * 12u;
    return true;
}

// db_load Load_GfxCell order on an already-streamed 56-byte cell, extended
// beyond Android's cell walker with its cull-group and reflection-probe
// arrays (present in db_load, absent in the oracle): the aabb-tree array
// (44 bytes each), the portal array (68 bytes each, recursing through
// inline portal cells against the shared budget), the int cull-group
// array, and the byte reflection-probe array. An optional trace
// records every placement for the live widener (see db_retail_walk.h).
bool ReadRetailGfxCellAt(RetailZoneLoadSession *session,
                         FsRetailFastfileReader *reader, uint32_t cellOffset,
                         uint32_t *budget,
                         RetailWalkDirectoryRecord *record,
                         RetailWalkDirectoryResult *summary,
                         RetailGfxCellTrace *trace)
{
    if (!session || !reader || !budget || !record || !summary)
        return false;
    const uint8_t *cell =
        session->zoneMemory->blocks[4].data + cellOffset;
    ++summary->walkedGfxCellCount;
    if (trace)
        trace->Record(RetailGfxCellTrace::CELL, cellOffset, 1, 0);
    const uint32_t aabbCount = ReadRetailLe32(cell + 24);
    const uint32_t aabbRef = ReadRetailLe32(cell + 28);
    const uint32_t portalCount = ReadRetailLe32(cell + 32);
    const uint32_t portalRef = ReadRetailLe32(cell + 36);
    const uint32_t cullCount = ReadRetailLe32(cell + 40);
    const uint32_t cullRef = ReadRetailLe32(cell + 44);
    const uint32_t probeCount = cell[48];
    const uint32_t probeRef = ReadRetailLe32(cell + 52);
    if (aabbRef)
    {
        if (aabbCount > UINT32_MAX / 44u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             aabbCount * 44u, 4))
            return false;
        const uint32_t treesStart =
            session->wire.cursor[4] - aabbCount * 44u;
        if (trace)
            trace->Record(RetailGfxCellTrace::TREE_ARRAY, treesStart,
                          aabbCount, 0);
        record->nestedBodyBytes += aabbCount * 44u;
        for (uint32_t tree = 0; tree < aabbCount; ++tree)
            if (!ReadRetailGfxAabbTreeAt(session, reader, 4,
                                         treesStart + tree * 44u, record,
                                         trace))
                return false;
    }
    if (portalRef)
    {
        if (portalCount > UINT32_MAX / 68u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             portalCount * 68u, 4))
            return false;
        const uint32_t portalsStart =
            session->wire.cursor[4] - portalCount * 68u;
        if (trace)
            trace->Record(RetailGfxCellTrace::PORTAL_ARRAY, portalsStart,
                          portalCount, 0);
        summary->lastGfxPortalOffset = portalsStart;
        summary->lastGfxPortalCount = portalCount;
        record->nestedBodyBytes += portalCount * 68u;
        for (uint32_t portal = 0; portal < portalCount; ++portal)
        {
            const uint8_t *portalWire = session->zoneMemory->blocks[4].data +
                                        portalsStart + portal * 68u;
            if (portal < 256)
            {
                summary->gfxTopPortalCellRefs[portal] = ReadRetailLe32(portalWire + 32);
                summary->gfxTopPortalRefCount = portal + 1;
            }
            if (summary->gfxPortalCellRefCount < 256)
                summary->gfxPortalCellOffsets[summary->gfxPortalCellRefCount++] =
                    ReadRetailLe32(portalWire + 32);
            if (!ReadRetailGfxPortalAt(session, reader,
                                       portalsStart + portal * 68u, budget,
                                       record, summary, trace))
                return false;
        }
    }
    if (cullRef)
    {
        if (cullCount > UINT32_MAX / 4u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             cullCount * 4u, 4))
            return false;
        const uint32_t cullStart = session->wire.cursor[4] - cullCount * 4u;
        if (trace)
            trace->Record(RetailGfxCellTrace::CULL_INTS, cullStart, cullCount, 0);
        record->nestedBodyBytes += cullCount * 4u;
    }
    if (probeRef)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, probeCount,
                                             1))
            return false;
        const uint32_t probeStart = session->wire.cursor[4] - probeCount;
        if (trace)
            trace->Record(RetailGfxCellTrace::PROBE_BYTES, probeStart, probeCount, 0);
        record->nestedBodyBytes += probeCount;
    }
    return true;
}

// Reserve runtime block-1 bytes for engine-computed sizes (cell masks,
// shadow visibilities, DPVS visibility sets). Zero FS bytes are consumed;
// oversized requests fail loudly with the cursor untouched.
bool ReadRetailGfxRuntimeBytes(RetailZoneLoadSession *session, uint32_t slot,
                               uint64_t bytes, uint32_t alignment,
                               RetailWalkDirectoryResult *summary)
{
    if (!slot)
        return true;
    if (!summary || bytes > UINT32_MAX)
        return false;
    if (!RetailZoneLoadSessionExpandRuntime(session, 1, (uint32_t)bytes,
                                            alignment))
        return false;
    summary->walkedGfxWorldBlock1Bytes += (uint32_t)bytes;
    return true;
}

// db_load Load_GfxWorld order, via Android LoadGfxWorld against the checked
// r_bsp.h layouts: 732-byte root in the temp block, both name strings in
// block 4, then indices, sky surfaces, the sky image asset, the inline-only
// sun light (64 bytes plus its LightDef asset), reflection probes (16
// bytes each plus image assets) with block-1 probe textures, the DPVS
// planes/nodes/cell-mask (inline-only planes; u16 nodes; block-1 mask),
// the cell array (56 bytes each with aabb trees, portals with recursive
// inline cells against an exact visit budget, cull groups, and probe
// bytes -- the last two per db_load, beyond the Android oracle), lightmap
// pairs (two image assets each), the in-root light grid (row/entries/color
// spans), block-1 lightmap textures, brush models, material memory (one
// material asset each), vertex and layer data, sun materials, the outdoor
// image, block-1 runtime masks, shadow geometry and light regions with
// their index/hull spans, and the DPVS static/dynamic visibility sets.
// Every nested Material/Image/LightDef/XModel reference resolves through
// the typed asset-slot handler without registering anything.
bool ReadRetailGfxWorldBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 732;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t baseMaterials = summary->walkedMaterialCount;
    const uint32_t baseImages = summary->walkedImageCount;
    const uint32_t baseTechSets = summary->walkedTechniqueCount;
    const uint32_t baseLightDefs = summary->walkedLightDefCount;
    const uint32_t baseXModels = summary->walkedXModelCount;
    const uint32_t basePasses = summary->walkedPassCount;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t baseNameRef = ReadRetailLe32(body + 4);
    const uint32_t planeCount = ReadRetailLe32(body + 8);
    const uint32_t nodeCount = ReadRetailLe32(body + 12);
    const uint32_t indexCount = ReadRetailLe32(body + 16);
    const uint32_t indicesRef = ReadRetailLe32(body + 20);
    const uint32_t surfaceCount = ReadRetailLe32(body + 24);
    const uint32_t skySurfCount = ReadRetailLe32(body + 32);
    const uint32_t skyStartSurfsRef = ReadRetailLe32(body + 36);
    const uint32_t skyImageRef = ReadRetailLe32(body + 40);
    const uint32_t vertexCount = ReadRetailLe32(body + 48);
    const uint32_t verticesRef = ReadRetailLe32(body + 52);
    const uint32_t vldSize = ReadRetailLe32(body + 60);
    const uint32_t vldRef = ReadRetailLe32(body + 64);
    const uint32_t sunLightRef = ReadRetailLe32(body + 200);
    const uint32_t sunPrimaryLightIndex = ReadRetailLe32(body + 216);
    const uint32_t primaryLightCount = ReadRetailLe32(body + 220);
    const uint32_t cullGroupCount = ReadRetailLe32(body + 224);
    const uint32_t probeCount = ReadRetailLe32(body + 228);
    const uint32_t probesRef = ReadRetailLe32(body + 232);
    const uint32_t probeTexturesRef = ReadRetailLe32(body + 236);
    const uint32_t cellCount = ReadRetailLe32(body + 240);
    const uint32_t dpvsPlanesRef = ReadRetailLe32(body + 244);
    const uint32_t dpvsNodesRef = ReadRetailLe32(body + 248);
    const uint32_t sceneEntCellBitsRef = ReadRetailLe32(body + 252);
    const uint32_t cellsRef = ReadRetailLe32(body + 260);
    const uint32_t lightmapCount = ReadRetailLe32(body + 264);
    const uint32_t lightmapsRef = ReadRetailLe32(body + 268);
    const uint32_t gridRowAxis = ReadRetailLe32(body + 292);
    const uint32_t gridRowDataRef = ReadRetailLe32(body + 300);
    const uint32_t gridRawBytes = ReadRetailLe32(body + 304);
    const uint32_t gridRawRef = ReadRetailLe32(body + 308);
    const uint32_t gridEntryCount = ReadRetailLe32(body + 312);
    const uint32_t gridEntriesRef = ReadRetailLe32(body + 316);
    const uint32_t gridColorCount = ReadRetailLe32(body + 320);
    const uint32_t gridColorsRef = ReadRetailLe32(body + 324);
    const uint32_t lightmapPrimaryRef = ReadRetailLe32(body + 328);
    const uint32_t lightmapSecondaryRef = ReadRetailLe32(body + 332);
    const uint32_t modelCount = ReadRetailLe32(body + 336);
    const uint32_t modelsRef = ReadRetailLe32(body + 340);
    const uint32_t materialMemoryCount = ReadRetailLe32(body + 372);
    const uint32_t materialMemoryRef = ReadRetailLe32(body + 376);
    const uint32_t spriteMaterialRef = ReadRetailLe32(body + 384);
    const uint32_t flareMaterialRef = ReadRetailLe32(body + 388);
    const uint32_t outdoorImageRef = ReadRetailLe32(body + 540);
    const uint32_t cellCasterBitsRef = ReadRetailLe32(body + 544);
    const uint32_t sceneDynModelRef = ReadRetailLe32(body + 548);
    const uint32_t sceneDynBrushRef = ReadRetailLe32(body + 552);
    const uint32_t primaryLightEntityShadowVisRef =
        ReadRetailLe32(body + 556);
    const uint32_t dynEntShadowVisRef[2] = {ReadRetailLe32(body + 560),
                                            ReadRetailLe32(body + 564)};
    const uint32_t nonSunRef = ReadRetailLe32(body + 568);
    const uint32_t shadowGeomRef = ReadRetailLe32(body + 572);
    const uint32_t lightRegionRef = ReadRetailLe32(body + 576);
    const uint32_t dpvsSmodelCount = ReadRetailLe32(body + 580);
    const uint32_t dpvsStaticSurfaceCount = ReadRetailLe32(body + 584);
    const uint32_t dpvsNoDecalCount = ReadRetailLe32(body + 588);
    const uint32_t dpvsSmodelVisData = ReadRetailLe32(body + 616);
    const uint32_t dpvsSurfaceVisData = ReadRetailLe32(body + 620);
    const uint32_t dpvsVisRef[7] = {
        ReadRetailLe32(body + 624), ReadRetailLe32(body + 628),
        ReadRetailLe32(body + 632), ReadRetailLe32(body + 636),
        ReadRetailLe32(body + 640), ReadRetailLe32(body + 644),
        ReadRetailLe32(body + 648)};
    const uint32_t dpvsSortedRef = ReadRetailLe32(body + 652);
    const uint32_t dpvsInstsRef = ReadRetailLe32(body + 656);
    const uint32_t dpvsSurfacesRef = ReadRetailLe32(body + 660);
    const uint32_t dpvsCullGroupsRef = ReadRetailLe32(body + 664);
    const uint32_t dpvsDrawInstsRef = ReadRetailLe32(body + 668);
    const uint32_t dpvsSurfMatsRef = ReadRetailLe32(body + 672);
    const uint32_t dpvsCastsShadowRef = ReadRetailLe32(body + 676);
    const uint32_t dpvsWordCount[2] = {ReadRetailLe32(body + 684),
                                       ReadRetailLe32(body + 688)};
    const uint32_t dynEntClientCount[2] = {ReadRetailLe32(body + 692),
                                           ReadRetailLe32(body + 696)};
    const uint32_t dpvsCellBitsRef[2] = {ReadRetailLe32(body + 700),
                                         ReadRetailLe32(body + 704)};
    const uint32_t dpvsVisDataRef[2][3] = {{ReadRetailLe32(body + 708),
                                            ReadRetailLe32(body + 712),
                                            ReadRetailLe32(body + 716)},
                                           {ReadRetailLe32(body + 720),
                                            ReadRetailLe32(body + 724),
                                            ReadRetailLe32(body + 728)}};
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount =
        (nameRef != 0) + (baseNameRef != 0) + (indicesRef != 0) +
        (skyStartSurfsRef != 0) + (skyImageRef != 0) + (verticesRef != 0) +
        (vldRef != 0) + (sunLightRef != 0) + (probesRef != 0) +
        (probeTexturesRef != 0) + (dpvsPlanesRef != 0) + (dpvsNodesRef != 0) +
        (sceneEntCellBitsRef != 0) + (cellsRef != 0) + (lightmapsRef != 0) +
        (gridRowDataRef != 0) + (gridRawRef != 0) + (gridEntriesRef != 0) +
        (gridColorsRef != 0) + (lightmapPrimaryRef != 0) +
        (lightmapSecondaryRef != 0) + (modelsRef != 0) +
        (materialMemoryRef != 0) + (spriteMaterialRef != 0) +
        (flareMaterialRef != 0) + (outdoorImageRef != 0) +
        (cellCasterBitsRef != 0) + (sceneDynModelRef != 0) +
        (sceneDynBrushRef != 0) + (primaryLightEntityShadowVisRef != 0) +
        (dynEntShadowVisRef[0] != 0) + (dynEntShadowVisRef[1] != 0) +
        (nonSunRef != 0) + (shadowGeomRef != 0) + (lightRegionRef != 0) +
        (dpvsSortedRef != 0) + (dpvsInstsRef != 0) + (dpvsSurfacesRef != 0) +
        (dpvsCullGroupsRef != 0) + (dpvsDrawInstsRef != 0) +
        (dpvsSurfMatsRef != 0) + (dpvsCastsShadowRef != 0) +
        (dpvsCellBitsRef[0] != 0) + (dpvsCellBitsRef[1] != 0) +
        (dpvsVisDataRef[0][0] != 0) + (dpvsVisDataRef[0][1] != 0) +
        (dpvsVisDataRef[0][2] != 0) + (dpvsVisDataRef[1][0] != 0) +
        (dpvsVisDataRef[1][1] != 0) + (dpvsVisDataRef[1][2] != 0);
    for (uint32_t vis = 0; vis < 7; ++vis)
        record->nestedReferenceCount += (dpvsVisRef[vis] != 0);
    uint32_t nameBytes = 0;
    uint32_t baseNameBytes = 0;
    if (!ReadRetailXString(session, reader, nameRef, &nameBytes) ||
        !ReadRetailXString(session, reader, baseNameRef, &baseNameBytes))
        return false;
    record->nameBytes = nameBytes + baseNameBytes;
    if (indicesRef)
    {
        if (indexCount > UINT32_MAX / 2u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             indexCount * 2u, 2))
            return false;
        record->nestedBodyBytes += indexCount * 2u;
    }
    if (skyStartSurfsRef)
    {
        if (skySurfCount > UINT32_MAX / 4u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             skySurfCount * 4u, 4))
            return false;
        record->nestedBodyBytes += skySurfCount * 4u;
    }
    if (!ReadRetailAssetSlotBody(session, reader, skyImageRef,
                                 RETAIL_WALK_NESTED_IMAGE, record, summary))
        return false;
    if (sunLightRef)
    {
        if (sunLightRef != kInlineReference &&
            !IsRetailWireAlias(session, sunLightRef, 1))
            return false;
        if (sunLightRef == kInlineReference)
        {
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, 64, 4))
                return false;
            const uint32_t lightStart = session->wire.cursor[4] - 64;
            record->nestedBodyBytes += 64;
            if (!ReadRetailAssetSlotBody(
                    session, reader,
                    ReadRetailLe32(session->zoneMemory->blocks[4].data +
                                   lightStart + 60),
                    RETAIL_WALK_NESTED_LIGHT_DEF, record, summary))
                return false;
        }
    }
    if (probesRef)
    {
        if (probeCount > UINT32_MAX / 16u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             probeCount * 16u, 4))
            return false;
        const uint32_t probesStart =
            session->wire.cursor[4] - probeCount * 16u;
        record->nestedBodyBytes += probeCount * 16u;
        for (uint32_t probe = 0; probe < probeCount; ++probe)
            if (!ReadRetailAssetSlotBody(
                    session, reader,
                    ReadRetailLe32(session->zoneMemory->blocks[4].data +
                                   probesStart + probe * 16u + 12),
                    RETAIL_WALK_NESTED_IMAGE, record, summary))
                return false;
    }
    if (probeTexturesRef)
    {
        if (probeCount > UINT32_MAX / 4u)
            return false;
        if (!ReadRetailGfxRuntimeBytes(session, probeTexturesRef,
                                       (uint64_t)probeCount * 4u, 4, summary))
            return false;
    }
    if (!ReadRetailCPlaneSlot(session, reader, 0, bodyStart + 244, 4,
                              planeCount, record))
        return false;
    if (dpvsNodesRef)
    {
        if (nodeCount > UINT32_MAX / 2u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             nodeCount * 2u, 2))
            return false;
        record->nestedBodyBytes += nodeCount * 2u;
    }
    if (!ReadRetailGfxRuntimeBytes(session, sceneEntCellBitsRef,
                                   (uint64_t)cellCount << 10, 4, summary))
        return false;
    if (cellsRef)
    {
        if (cellCount > UINT32_MAX / 56u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             cellCount * 56u, 4))
            return false;
        const uint32_t cellsStart =
            session->wire.cursor[4] - cellCount * 56u;
        record->nestedBodyBytes += cellCount * 56u;
        uint32_t budget =
            (session->zoneMemory->blocks[4].size -
             session->wire.cursor[4]) /
            56u;
        for (uint32_t cell = 0; cell < cellCount; ++cell)
            if (!ReadRetailGfxCellAt(session, reader, cellsStart + cell * 56u,
                                     &budget, record, summary))
                return false;
    }
    if (lightmapsRef)
    {
        if (lightmapCount > UINT32_MAX / 8u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             lightmapCount * 8u, 4))
            return false;
        const uint32_t mapsStart =
            session->wire.cursor[4] - lightmapCount * 8u;
        record->nestedBodyBytes += lightmapCount * 8u;
        for (uint32_t map = 0; map < lightmapCount; ++map)
        {
            const uint8_t *entry =
                session->zoneMemory->blocks[4].data + mapsStart + map * 8u;
            if (!ReadRetailAssetSlotBody(session, reader,
                                         ReadRetailLe32(entry),
                                         RETAIL_WALK_NESTED_IMAGE, record,
                                         summary) ||
                !ReadRetailAssetSlotBody(session, reader,
                                         ReadRetailLe32(entry + 4),
                                         RETAIL_WALK_NESTED_IMAGE, record,
                                         summary))
                return false;
        }
    }
    if (gridRowDataRef)
    {
        const uint32_t rowAxis = gridRowAxis;
        if (rowAxis > 2)
            return false;
        const uint8_t *grid = session->zoneMemory->blocks[0].data +
                              bodyStart + 272;
        const uint32_t lo =
            static_cast<uint32_t>(grid[8 + rowAxis * 2]) |
            (static_cast<uint32_t>(grid[9 + rowAxis * 2]) << 8);
        const uint32_t hi =
            static_cast<uint32_t>(grid[14 + rowAxis * 2]) |
            (static_cast<uint32_t>(grid[15 + rowAxis * 2]) << 8);
        const uint32_t span = hi - lo + 1u;
        if (span > UINT32_MAX / 2u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, span * 2u,
                                             2))
            return false;
        record->nestedBodyBytes += span * 2u;
    }
    if (gridRawRef)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, gridRawBytes,
                                             1))
            return false;
        record->nestedBodyBytes += gridRawBytes;
    }
    if (gridEntriesRef)
    {
        if (gridEntryCount > UINT32_MAX / 4u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             gridEntryCount * 4u, 4))
            return false;
        record->nestedBodyBytes += gridEntryCount * 4u;
    }
    if (gridColorsRef)
    {
        if (gridColorCount > UINT32_MAX / 168u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             gridColorCount * 168u, 4))
            return false;
        record->nestedBodyBytes += gridColorCount * 168u;
    }
    if (!ReadRetailGfxRuntimeBytes(session, lightmapPrimaryRef,
                                   (uint64_t)lightmapCount * 4u, 4, summary) ||
        !ReadRetailGfxRuntimeBytes(session, lightmapSecondaryRef,
                                   (uint64_t)lightmapCount * 4u, 4, summary))
        return false;
    if (modelsRef)
    {
        if (modelCount > UINT32_MAX / 56u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             modelCount * 56u, 4))
            return false;
        record->nestedBodyBytes += modelCount * 56u;
    }
    if (materialMemoryRef)
    {
        if (materialMemoryCount > UINT32_MAX / 8u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             materialMemoryCount * 8u, 4))
            return false;
        const uint32_t memoryStart =
            session->wire.cursor[4] - materialMemoryCount * 8u;
        record->nestedBodyBytes += materialMemoryCount * 8u;
        for (uint32_t entry = 0; entry < materialMemoryCount; ++entry)
            if (!ReadRetailAssetSlotBody(
                    session, reader,
                    ReadRetailLe32(session->zoneMemory->blocks[4].data +
                                   memoryStart + entry * 8u),
                    RETAIL_WALK_NESTED_MATERIAL, record, summary))
                return false;
    }
    if (verticesRef)
    {
        if (vertexCount > UINT32_MAX / 44u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             vertexCount * 44u, 4))
            return false;
        record->nestedBodyBytes += vertexCount * 44u;
    }
    if (vldRef)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, vldSize, 1))
            return false;
        record->nestedBodyBytes += vldSize;
    }
    if (!ReadRetailAssetSlotBody(session, reader, spriteMaterialRef,
                                 RETAIL_WALK_NESTED_MATERIAL, record,
                                 summary) ||
        !ReadRetailAssetSlotBody(session, reader, flareMaterialRef,
                                 RETAIL_WALK_NESTED_MATERIAL, record,
                                 summary) ||
        !ReadRetailAssetSlotBody(session, reader, outdoorImageRef,
                                 RETAIL_WALK_NESTED_IMAGE, record, summary))
        return false;
    const uint64_t nonSunLight =
        ((uint64_t)primaryLightCount + 0x100000000ull -
         (uint64_t)sunPrimaryLightIndex - 1u) &
        0xffffffffu;
    if (!ReadRetailGfxRuntimeBytes(
            session, cellCasterBitsRef,
            4u * ((uint64_t)cellCount * (((uint64_t)cellCount + 31u) >> 5)), 4,
            summary) ||
        !ReadRetailGfxRuntimeBytes(session, sceneDynModelRef,
                                   6u * dynEntClientCount[0], 4, summary) ||
        !ReadRetailGfxRuntimeBytes(session, sceneDynBrushRef,
                                   4u * dynEntClientCount[1], 4, summary) ||
        !ReadRetailGfxRuntimeBytes(session, primaryLightEntityShadowVisRef,
                                   4u * (nonSunLight << 12), 4, summary) ||
        !ReadRetailGfxRuntimeBytes(session, dynEntShadowVisRef[0],
                                   4u * (uint64_t)dynEntClientCount[0] *
                                       nonSunLight,
                                   4, summary) ||
        !ReadRetailGfxRuntimeBytes(session, dynEntShadowVisRef[1],
                                   4u * (uint64_t)dynEntClientCount[1] *
                                       nonSunLight,
                                   4, summary) ||
        !ReadRetailGfxRuntimeBytes(session, nonSunRef, dynEntClientCount[0],
                                   1, summary))
        return false;
    if (shadowGeomRef)
    {
        if (primaryLightCount > UINT32_MAX / 12u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             primaryLightCount * 12u, 4))
            return false;
        const uint32_t geomsStart =
            session->wire.cursor[4] - primaryLightCount * 12u;
        record->nestedBodyBytes += primaryLightCount * 12u;
        for (uint32_t geom = 0; geom < primaryLightCount; ++geom)
        {
            const uint8_t *entry =
                session->zoneMemory->blocks[4].data + geomsStart + geom * 12u;
            const uint32_t count0 =
                static_cast<uint32_t>(entry[0]) |
                (static_cast<uint32_t>(entry[1]) << 8);
            const uint32_t ref0 = ReadRetailLe32(entry + 4);
            const uint32_t count1 =
                static_cast<uint32_t>(entry[2]) |
                (static_cast<uint32_t>(entry[3]) << 8);
            const uint32_t ref1 = ReadRetailLe32(entry + 8);
            const uint32_t refs[2] = {ref0, ref1};
            const uint32_t counts[2] = {count0, count1};
            for (uint32_t side = 0; side < 2; ++side)
            {
                if (!refs[side])
                    continue;
                if (counts[side] > UINT32_MAX / 2u)
                    return false;
                if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                                     counts[side] * 2u, 2))
                    return false;
                record->nestedBodyBytes += counts[side] * 2u;
            }
        }
    }
    if (lightRegionRef)
    {
        if (primaryLightCount > UINT32_MAX / 8u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             primaryLightCount * 8u, 4))
            return false;
        const uint32_t regionsStart =
            session->wire.cursor[4] - primaryLightCount * 8u;
        record->nestedBodyBytes += primaryLightCount * 8u;
        for (uint32_t region = 0; region < primaryLightCount; ++region)
        {
            const uint8_t *entry =
                session->zoneMemory->blocks[4].data + regionsStart +
                region * 8u;
            const uint32_t hullsRef = ReadRetailLe32(entry + 4);
            if (!hullsRef)
                continue;
            const uint32_t hullCount = ReadRetailLe32(entry);
            if (hullCount > UINT32_MAX / 80u)
                return false;
            if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                                 hullCount * 80u, 4))
                return false;
            const uint32_t hullsStart = session->wire.cursor[4] -
                                        hullCount * 80u;
            record->nestedBodyBytes += hullCount * 80u;
            for (uint32_t hull = 0; hull < hullCount; ++hull)
            {
                const uint8_t *hullBody =
                    session->zoneMemory->blocks[4].data + hullsStart +
                    hull * 80u;
                const uint32_t axisRef = ReadRetailLe32(hullBody + 76);
                if (!axisRef)
                    continue;
                const uint32_t axisCount = ReadRetailLe32(hullBody + 72);
                if (axisCount > UINT32_MAX / 20u)
                    return false;
                if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                                     axisCount * 20u, 4))
                    return false;
                record->nestedBodyBytes += axisCount * 20u;
            }
        }
    }
    const uint32_t dpvsVisBytes[7] = {
        dpvsSmodelCount, dpvsSmodelCount, dpvsSmodelCount,
        dpvsStaticSurfaceCount, dpvsStaticSurfaceCount,
        dpvsStaticSurfaceCount, 8u * dpvsSmodelVisData};
    for (uint32_t vis = 0; vis < 7; ++vis)
    {
        const uint32_t alignment = (vis == 6) ? 128u : 1u;
        if (!ReadRetailGfxRuntimeBytes(session, dpvsVisRef[vis],
                                       dpvsVisBytes[vis], alignment, summary))
            return false;
    }
    if (dpvsSortedRef)
    {
        const uint64_t sorted =
            (uint64_t)dpvsNoDecalCount + dpvsStaticSurfaceCount;
        if (sorted > UINT32_MAX / 2u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             (uint32_t)sorted * 2u, 2))
            return false;
        record->nestedBodyBytes += (uint32_t)sorted * 2u;
    }
    if (dpvsInstsRef)
    {
        if (dpvsSmodelCount > UINT32_MAX / 28u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             dpvsSmodelCount * 28u, 4))
            return false;
        record->nestedBodyBytes += dpvsSmodelCount * 28u;
    }
    if (dpvsSurfacesRef)
    {
        if (surfaceCount > UINT32_MAX / 48u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             surfaceCount * 48u, 4))
            return false;
        const uint32_t surfacesStart =
            session->wire.cursor[4] - surfaceCount * 48u;
        record->nestedBodyBytes += surfaceCount * 48u;
        for (uint32_t surface = 0; surface < surfaceCount; ++surface)
        {
            ++summary->walkedGfxSurfaceCount;
            const uint32_t matSlot = ReadRetailLe32(session->zoneMemory->blocks[4].data +
                                                    surfacesStart + surface * 48u + 16);
            if (!ReadRetailAssetSlotBody(
                    session, reader, matSlot,
                    RETAIL_WALK_NESTED_MATERIAL, record, summary))
                return false;
        }
    }
    if (dpvsCullGroupsRef)
    {
        if (cullGroupCount > UINT32_MAX / 32u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             cullGroupCount * 32u, 4))
            return false;
        record->nestedBodyBytes += cullGroupCount * 32u;
    }
    if (dpvsDrawInstsRef)
    {
        if (dpvsSmodelCount > UINT32_MAX / 76u)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4,
                                             dpvsSmodelCount * 76u, 4))
            return false;
        const uint32_t instsStart =
            session->wire.cursor[4] - dpvsSmodelCount * 76u;
        record->nestedBodyBytes += dpvsSmodelCount * 76u;
        for (uint32_t inst = 0; inst < dpvsSmodelCount; ++inst)
            if (!ReadRetailAssetSlotBody(
                    session, reader,
                    ReadRetailLe32(session->zoneMemory->blocks[4].data +
                                   instsStart + inst * 76u + 56),
                    RETAIL_WALK_NESTED_XMODEL, record, summary))
                return false;
    }
    if (!ReadRetailGfxRuntimeBytes(session, dpvsSurfMatsRef,
                                   8u * dpvsStaticSurfaceCount, 4, summary) ||
        !ReadRetailGfxRuntimeBytes(session, dpvsCastsShadowRef,
                                   4u * dpvsSurfaceVisData, 128, summary))
        return false;
    for (uint32_t list = 0; list < 2; ++list)
        if (!ReadRetailGfxRuntimeBytes(
                session, dpvsCellBitsRef[list],
                4u * (uint64_t)cellCount * dpvsWordCount[list], 4, summary))
            return false;
    // dynEntVisData in engine order [0][0], [1][0], [0][1], [1][1],
    // [0][2], [1][2].
    for (uint32_t pass = 0; pass < 3; ++pass)
        for (uint32_t list = 0; list < 2; ++list)
            if (!ReadRetailGfxRuntimeBytes(
                    session, dpvsVisDataRef[list][pass],
                    32u * dpvsWordCount[list], 16, summary))
                return false;
    summary->walkedGfxWorldMaterials +=
        summary->walkedMaterialCount - baseMaterials;
    summary->walkedGfxWorldImages += summary->walkedImageCount - baseImages;
    summary->walkedGfxWorldTechSets +=
        summary->walkedTechniqueCount - baseTechSets;
    summary->walkedGfxWorldLightDefs +=
        summary->walkedLightDefCount - baseLightDefs;
    summary->walkedGfxWorldXModels += summary->walkedXModelCount - baseXModels;
    summary->walkedGfxWorldPasses += summary->walkedPassCount - basePasses;
    ++summary->walkedGfxWorldCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Dispatch one inline asset body inside the caller's temp-block scope.
// Every family walker mirrors its Android Load_* order and consumes exactly
// the serialized bytes of that body; unsupported families fail loudly.
bool RetailWalkDispatchAssetBody(RetailZoneLoadSession *session,
                                 FsRetailFastfileReader *reader, uint32_t type,
                                 RetailWalkDirectoryRecord *record,
                                 RetailWalkDirectoryResult *summary)
{
    switch (type)
    {
    case ASSET_TYPE_TECHNIQUE_SET:
        return ReadRetailTechniqueSetBody(session, reader, record, summary);
    case ASSET_TYPE_MATERIAL:
        return ReadRetailMaterialBody(session, reader, record, summary);
    case ASSET_TYPE_IMAGE:
        return ReadRetailImageBody(session, reader, record, summary);
    case ASSET_TYPE_LOCALIZE_ENTRY:
        return ReadRetailLocalizeEntryBody(session, reader, record, summary);
    case ASSET_TYPE_FONT:
        return ReadRetailFontBody(session, reader, record, summary);
    case ASSET_TYPE_RAWFILE:
        return ReadRetailRawFileBody(session, reader, record);
    case ASSET_TYPE_PHYSPRESET:
        return ReadRetailPhysPresetBody(session, reader, record, summary);
    case ASSET_TYPE_SOUND_CURVE:
        return ReadRetailSndCurveBody(session, reader, record, summary);
    case ASSET_TYPE_LIGHT_DEF:
        return ReadRetailLightDefBody(session, reader, record, summary);
    case ASSET_TYPE_SOUND:
        return ReadRetailSndAliasListBody(session, reader, record, summary);
    case ASSET_TYPE_LOADED_SOUND:
        return ReadRetailLoadedSoundBody(session, reader, record, summary);
    case ASSET_TYPE_FX:
        return ReadRetailFxEffectDefBody(session, reader, record, summary);
    case ASSET_TYPE_IMPACT_FX:
        return ReadRetailFxImpactTableBody(session, reader, record, summary);
    case ASSET_TYPE_MENULIST:
        return ReadRetailMenuListBody(session, reader, record, summary);
    case ASSET_TYPE_XANIMPARTS:
        return ReadRetailXAnimPartsBody(session, reader, record, summary);
    case ASSET_TYPE_XMODEL:
        return ReadRetailXModelBody(session, reader, record, summary);
    case ASSET_TYPE_WEAPON:
        return ReadRetailWeaponDefBody(session, reader, record, summary);
    case ASSET_TYPE_MAP_ENTS:
        return ReadRetailMapEntsBody(session, reader, record, summary);
    case ASSET_TYPE_COMWORLD:
        return ReadRetailComWorldBody(session, reader, record, summary);
    case ASSET_TYPE_GAMEWORLD_SP:
        return ReadRetailGameWorldSpBody(session, reader, record, summary);
    case ASSET_TYPE_CLIPMAP:
        return ReadRetailClipMapBody(session, reader, record, summary);
    case ASSET_TYPE_GFXWORLD:
        return ReadRetailGfxWorldBody(session, reader, record, summary);
    case ASSET_TYPE_STRINGTABLE:
        return ReadRetailStringTableBody(session, reader, record);
    default:
        return false;
    }
}

// Android Load_Statement: numEntries at +0; a non-zero entries slot streams
// the pointer table in block 4, then one 12-byte expressionEntry per non-zero
// slot whose type != 0 and dataType == VAL_STRING carries an inline string.
// the native ItemDef decoder remains the widening owner of these records.
bool ReadRetailStatementBody(RetailZoneLoadSession *session,
                             FsRetailFastfileReader *reader, uint32_t block,
                             uint32_t offset)
{
    const uint8_t *statement = session->zoneMemory->blocks[block].data + offset;
    const uint32_t numEntries = ReadRetailLe32(statement);
    if (!ReadRetailLe32(statement + 4) || !numEntries)
        return true;
    if (numEntries > UINT32_MAX / 4u)
        return false;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, numEntries * 4u, 4))
        return false;
    const uint32_t entriesStart = session->wire.cursor[4] - numEntries * 4u;
    for (uint32_t index = 0; index < numEntries; ++index)
    {
        const uint32_t slot = ReadRetailLe32(session->zoneMemory->blocks[4].data +
                                             entriesStart + index * 4u);
        if (!slot)
            continue;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, 12, 4))
            return false;
        const uint32_t entryStart = session->wire.cursor[4] - 12;
        const uint8_t *entry = session->zoneMemory->blocks[4].data + entryStart;
        if (ReadRetailLe32(entry) != 0 && ReadRetailLe32(entry + 4) == 2u)
        {
            uint32_t bytes = 0;
            if (!ReadRetailXString(session, reader, ReadRetailLe32(entry + 8),
                                   &bytes))
                return false;
        }
    }
    return true;
}

// Android Load_ItemKeyHandler: 12-byte {key, action, next} records linked
// through their next slot, all resident in block 4.
bool ReadRetailKeyHandlerChain(RetailZoneLoadSession *session,
                               FsRetailFastfileReader *reader, uint32_t block,
                               uint32_t offset)
{
    uint32_t guard = 0;
    for (;;)
    {
        const uint32_t slot =
            ReadRetailLe32(session->zoneMemory->blocks[block].data + offset);
        if (!slot)
            return true;
        if (++guard > 4096)
            return false;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, 12, 4))
            return false;
        const uint32_t handlerStart = session->wire.cursor[4] - 12;
        const uint8_t *handler = session->zoneMemory->blocks[4].data + handlerStart;
        uint32_t bytes = 0;
        if (!ReadRetailXString(session, reader, ReadRetailLe32(handler + 4),
                               &bytes))
            return false;
        block = 4;
        offset = handlerStart + 8;
    }
}

// Android Load_windowDef_t: the two leading XStrings plus the background
// material asset slot at +152.
bool ReadRetailWindowDefBody(RetailZoneLoadSession *session,
                             FsRetailFastfileReader *reader, uint32_t block,
                             uint32_t offset,
                             RetailWalkDirectoryRecord *record,
                             RetailWalkDirectoryResult *summary)
{
    uint32_t bytes = 0;
    if (!ReadRetailXString(session, reader,
                           ReadRetailLe32(session->zoneMemory->blocks[block].data +
                                          offset),
                           &bytes) ||
        !ReadRetailXString(session, reader,
                           ReadRetailLe32(session->zoneMemory->blocks[block].data +
                                          offset + 52),
                           &bytes))
        return false;
    record->nameBytes += bytes;
    return ReadRetailAssetSlotBody(
        session, reader,
        ReadRetailLe32(session->zoneMemory->blocks[block].data + offset + 152),
        RETAIL_WALK_NESTED_MATERIAL, record, summary);
}

// Android Load_itemDef_t: 372-byte record resident in block 4 (the menu's
// item table allocates it there); the type-specific data dispatch keys on
// the item type at +180 with its union slot at +300.
bool ReadRetailItemDefBody(RetailZoneLoadSession *session,
                           FsRetailFastfileReader *reader, uint32_t offset,
                           RetailWalkDirectoryRecord *record,
                           RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 372;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, kBodyBytes, 4))
        return false;
    ++summary->walkedItemDefCount;
    record->nestedBodyBytes += kBodyBytes;
    const uint8_t *body = session->zoneMemory->blocks[4].data + offset;
    if (!ReadRetailWindowDefBody(session, reader, 4, offset, record, summary))
        return false;
    for (uint32_t slot : {224u, 236u, 240u, 244u, 248u, 252u, 256u, 260u, 264u,
                          268u, 272u})
    {
        uint32_t bytes = 0;
        if (!ReadRetailXString(session, reader, ReadRetailLe32(body + slot),
                               &bytes))
            return false;
        record->nameBytes += bytes;
    }
    if (!ReadRetailKeyHandlerChain(session, reader, 4, offset + 276))
        return false;
    {
        uint32_t bytes = 0;
        if (!ReadRetailXString(session, reader, ReadRetailLe32(body + 280),
                               &bytes))
            return false;
    }
    // The focus sound is a full snd_alias_list_t asset slot.
    if (!ReadRetailAssetSlotBody(session, reader, ReadRetailLe32(body + 288),
                                 RETAIL_WALK_NESTED_SOUND_ALIAS_LIST, record,
                                 summary))
        return false;

    const uint32_t itemType = ReadRetailLe32(body + 180);
    if (itemType == 0xD)
    {
        // The enum-dvar name is an XString read unconditionally; a zero
        // slot streams nothing.
        uint32_t bytes = 0;
        if (!ReadRetailXString(session, reader, ReadRetailLe32(body + 300),
                               &bytes))
            return false;
    }
    else if (ReadRetailLe32(body + 300))
    {
        // AllocForSlot semantics: any non-zero type slot streams the type
        // data inline.  Unknown item types have no serialized type data.
        switch (itemType)
        {
        case 6: // listBox
        {
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, 340, 4))
                return false;
            record->nestedBodyBytes += 340;
            const uint32_t listBox = session->wire.cursor[4] - 340;
            const uint8_t *listBody = session->zoneMemory->blocks[4].data + listBox;
            uint32_t bytes = 0;
            if (!ReadRetailXString(session, reader,
                                   ReadRetailLe32(listBody + 288), &bytes))
                return false;
            if (!ReadRetailAssetSlotBody(
                    session, reader, ReadRetailLe32(listBody + 336),
                    RETAIL_WALK_NESTED_MATERIAL, record, summary))
                return false;
            break;
        }
        case 0: case 4: case 9: case 0xA: case 0xB: case 0xE:
        case 0x10: case 0x11: case 0x12: // editField
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, 32, 4))
                return false;
            record->nestedBodyBytes += 32;
            break;
        case 0xC: // multiDef
        {
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, 392, 4))
                return false;
            record->nestedBodyBytes += 392;
            const uint32_t multi = session->wire.cursor[4] - 392;
            const uint8_t *multiBody = session->zoneMemory->blocks[4].data + multi;
            for (uint32_t slot = 0; slot < 32; ++slot)
            {
                uint32_t bytes = 0;
                if (!ReadRetailXString(session, reader,
                                       ReadRetailLe32(multiBody + slot * 4u),
                                       &bytes))
                    return false;
            }
            for (uint32_t slot = 0; slot < 32; ++slot)
            {
                uint32_t bytes = 0;
                if (!ReadRetailXString(session, reader,
                                       ReadRetailLe32(multiBody + 128 + slot * 4u),
                                       &bytes))
                    return false;
            }
            break;
        }
        default:
            break;
        }
    }

    for (uint32_t stmt : {308u, 316u, 324u, 332u, 340u, 348u, 356u, 364u})
        if (!ReadRetailStatementBody(session, reader, 4, offset + stmt))
            return false;
    return true;
}

// Android Load_menuDef_t: 284-byte root in the temp block; window, event
// strings, key handlers, expressions, and the block-4 item table whose
// entries are asset-style itemDef slots.
bool ReadRetailMenuDefBody(RetailZoneLoadSession *session,
                           FsRetailFastfileReader *reader,
                           RetailWalkDirectoryRecord *record,
                           RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 284;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    ++summary->walkedMenuCount;
    const uint8_t *body = session->zoneMemory->blocks[0].data +
                          (session->wire.cursor[0] - kBodyBytes);
    const uint32_t bodyOffset = session->wire.cursor[0] - kBodyBytes;
    record->bodyBytes += kBodyBytes;
    if (!ReadRetailWindowDefBody(session, reader, 0, bodyOffset, record, summary))
        return false;
    for (uint32_t slot : {156u, 196u, 200u, 204u})
    {
        uint32_t bytes = 0;
        if (!ReadRetailXString(session, reader, ReadRetailLe32(body + slot),
                               &bytes))
            return false;
        record->nameBytes += bytes;
    }
    if (!ReadRetailKeyHandlerChain(session, reader, 0, bodyOffset + 208))
        return false;
    if (!ReadRetailStatementBody(session, reader, 0, bodyOffset + 212))
        return false;
    for (uint32_t slot : {220u, 224u})
    {
        uint32_t bytes = 0;
        if (!ReadRetailXString(session, reader, ReadRetailLe32(body + slot),
                               &bytes))
            return false;
        record->nameBytes += bytes;
    }
    if (!ReadRetailStatementBody(session, reader, 0, bodyOffset + 264) ||
        !ReadRetailStatementBody(session, reader, 0, bodyOffset + 272))
        return false;

    const uint32_t itemCount = ReadRetailLe32(body + 164);
    const uint32_t itemsRef = ReadRetailLe32(body + 280);
    if (!itemsRef)
        return true;
    if (itemsRef == kInsertReference ||
        itemCount > UINT32_MAX / 4u)
        return false;
    if (itemsRef != kInlineReference)
        return IsRetailWireAlias(session, itemsRef, 1);
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, itemCount * 4u, 4))
        return false;
    const uint32_t itemsStart = session->wire.cursor[4] - itemCount * 4u;
    record->nestedBodyBytes += itemCount * 4u;
    for (uint32_t item = 0; item < itemCount; ++item)
    {
        const uint32_t slot = ReadRetailLe32(session->zoneMemory->blocks[4].data +
                                             itemsStart + item * 4u);
        if (!slot)
            continue;
        // AllocForSlot semantics: any non-zero item slot streams the itemDef
        // inline.  The itemDef root itself is block-4 data (AllocForSlot
        // inside the menu's pushed block-4 scope), so its bytes persist.
        const uint32_t itemOffset = (session->wire.cursor[4] + 3u) & ~3u;
        if (!ReadRetailItemDefBody(session, reader, itemOffset, record, summary))
            return false;
    }
    return true;
}

// Android Load_MenuList: 12-byte root in the temp block; name XString, then
// the block-4 menu pointer table with one asset-style menuDef slot per entry.
bool ReadRetailMenuListBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary)
{
    constexpr uint32_t kBodyBytes = 12;
    if (!session || !reader || !record || !summary ||
        record->header != kInlineReference)
        return false;
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t menuCount = ReadRetailLe32(body + 4);
    const uint32_t menusRef = ReadRetailLe32(body + 8);
    record->bodyBytes = session->wire.cursor[0] - bodyStart;
    record->nestedReferenceCount = (nameRef != 0) + (menusRef != 0);
    if (!ReadRetailXString(session, reader, nameRef, &record->nameBytes))
        return false;
    if (!menusRef)
    {
        record->state = RETAIL_WALK_WALKED_DEFERRED;
        return true;
    }
    if (menusRef == kInsertReference ||
        menuCount > UINT32_MAX / 4u)
        return false;
    if (menusRef != kInlineReference)
        return IsRetailWireAlias(session, menusRef, 1);
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, menuCount * 4u, 4))
        return false;
    const uint32_t menusStart = session->wire.cursor[4] - menuCount * 4u;
    record->nestedBodyBytes += menuCount * 4u;
    for (uint32_t menu = 0; menu < menuCount; ++menu)
        if (!ReadRetailAssetSlotBody(
                session, reader,
                ReadRetailLe32(session->zoneMemory->blocks[4].data + menusStart +
                               menu * 4u),
                RETAIL_WALK_NESTED_MENU_DEF, record, summary))
            return false;
    ++summary->walkedMenuListCount;
    record->state = RETAIL_WALK_WALKED_DEFERRED;
    return true;
}

// Shared prefix of every real directory-driving entrypoint below: open the
// FS-owned fastfile, begin a real (uncollapsed) retail zone session sized
// from its own nine block sizes, and record its XAsset directory in
// serialized order.  On success the session is left ACTIVE and the reader
// OPEN — the caller now owns both and decides whether to walk-only (and
// tear down) or to also register real assets and keep the zone live.  On
// any failure this already tears everything down itself.
RetailWalkDirectoryResultCode RetailWalkOpenDirectory(
    const char *filename, RetailZoneLoadSession *session,
    FsRetailFastfileReader **outReader, FsRetailFastfileAsset **outAssets,
    RetailWalkDirectoryRecord *records, uint32_t capacity,
    RetailWalkDirectoryResult *result,
    std::size_t nativeArenaBytes)
{
    RetailWalkDirectoryResult summary{};
    summary.code = RETAIL_WALK_BAD_ARGUMENT;
    *outReader = nullptr;
    *outAssets = nullptr;
    if (!filename || !filename[0] || !session || !result || (capacity && !records))
    {
        if (result)
            *result = summary;
        return summary.code;
    }

    FsRetailFastfileReader *reader = nullptr;
    const FsRetailFastfileResult openResult = FS_OpenRetailFastfile(filename, &reader);
    Com_Printf(0, "RetailWalkOpenDirectory: FS_OpenRetailFastfile('%s') returned %d reader=%p\n",
               filename, openResult, reader);
    if (openResult != FS_RETAIL_FF_OK || !reader)
    {
        summary.code = RETAIL_WALK_OPEN_FAILED;
        *result = summary;
        return summary.code;
    }

    uint32_t blockSizes[9]{};
    for (uint32_t block = 0; block < 9; ++block)
        blockSizes[block] = FS_RetailFastfileBlockSize(reader, block);

    // Block 1 is runtime-only expansion (zero-filled, never streamed):
    // Android's LoadStream memsets the span and advances the cursor without
    // consuming file bytes.  Blocks 2/3 are delay-streamed after the walk;
    // that ownership does not exist here yet, so a zone that actually uses
    // them must fail loudly rather than silently desynchronize the FS
    // stream.  Map zones declare block 2/3 as zero, so this accepts them
    // while keeping any future nonzero delayed block a hard error.
    if (blockSizes[2] || blockSizes[3])
    {
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_SESSION_FAILED;
        *result = summary;
        return summary.code;
    }

    if (!RetailZoneLoadSessionBegin(session, filename, 0, blockSizes, nativeArenaBytes))
    {
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_SESSION_FAILED;
        *result = summary;
        return summary.code;
    }

    // XAssetList itself is the stream root and sits outside the nine zone
    // blocks. Only its pointed-to tables belong in block 4.
    uint8_t root[16]{};
    if (FS_ReadRetailFastfile(reader, root, sizeof(root)) != sizeof(root))
    {
        RetailZoneLoadSessionAbort(session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_LIST_FAILED;
        *result = summary;
        return summary.code;
    }
    FsRetailFastfileAssetList list{};
    list.scriptStringCount = ReadRetailLe32(root);
    list.scriptStringsRef = ReadRetailLe32(root + 4);
    list.assetCount = ReadRetailLe32(root + 8);
    list.assetsRef = ReadRetailLe32(root + 12);
    if (list.scriptStringCount > kAssetCountCap || list.assetCount > kAssetCountCap ||
        (list.scriptStringCount && list.scriptStringsRef != kInlineReference) ||
        (list.assetCount && list.assetsRef != kInlineReference))
    {
        RetailZoneLoadSessionAbort(session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_LIST_FAILED;
        *result = summary;
        return summary.code;
    }
    summary.assetCount = list.assetCount;
    // B3 script-string table: the XAnim widener resolves u16 bone/notify
    // wire indices through this zone's own table (count + block-4 bytes
    // streamed below), then interns through the real SL_* table.
    session->scriptStringCount = list.scriptStringCount;
    session->scriptStringOffsets = nullptr;

    // The block-4 script-string pointer table precedes its inline strings.
    if (list.scriptStringCount)
    {
        const uint32_t scriptBytes = list.scriptStringCount * 4u;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, scriptBytes, 4))
        {
            RetailZoneLoadSessionAbort(session);
            FS_CloseRetailFastfile(reader);
            summary.code = RETAIL_WALK_LIST_FAILED;
            *result = summary;
            return summary.code;
        }
        const uint8_t *scriptTable = session->zoneMemory->blocks[4].data;
        for (uint32_t i = 0; i < list.scriptStringCount; ++i)
        {
            if (ReadRetailLe32(scriptTable + i * 4u) != kInlineReference)
                continue;
            if (!ReadInlineRetailString(session, reader, nullptr))
            {
                RetailZoneLoadSessionAbort(session);
                FS_CloseRetailFastfile(reader);
                summary.code = RETAIL_WALK_LIST_FAILED;
                *result = summary;
                return summary.code;
            }
        }
    }

    if (list.assetCount > capacity)
    {
        RetailZoneLoadSessionAbort(session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_CAPACITY;
        *result = summary;
        return summary.code;
    }

    FsRetailFastfileAsset *assets = list.assetCount ?
        new (std::nothrow) FsRetailFastfileAsset[list.assetCount] : nullptr;
    if (list.assetCount && !assets)
    {
        RetailZoneLoadSessionAbort(session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_CAPACITY;
        *result = summary;
        return summary.code;
    }
    const uint32_t directoryBytes = list.assetCount * 8u;
    uint32_t savedDirOffset = 0;
    if (directoryBytes)
    {
        savedDirOffset = (session->wire.cursor[4] + 3u) & ~3u;
        summary.directoryOffset = savedDirOffset;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, directoryBytes, 4))
        {
            delete[] assets;
            RetailZoneLoadSessionAbort(session);
            FS_CloseRetailFastfile(reader);
            summary.code = RETAIL_WALK_LIST_FAILED;
            *result = summary;
            return summary.code;
        }
        // Block 4 also contains the script-string table and any inline
        // string bodies.  Decode the directory from the exact span just
        // streamed, never from the start of the zone block.
        const uint8_t *directory = session->zoneMemory->blocks[4].data + savedDirOffset;
        for (uint32_t ordinal = 0; ordinal < list.assetCount; ++ordinal)
        {
            assets[ordinal].type = ReadRetailLe32(directory + ordinal * 8u);
            assets[ordinal].header = ReadRetailLe32(directory + ordinal * 8u + 4u);
        }
    }

    if (RetailWireReadAssetDirectory(&list, assets, list.assetCount, records, capacity, &summary) !=
            RETAIL_WALK_OK)
    {
        delete[] assets;
        RetailZoneLoadSessionAbort(session);
        FS_CloseRetailFastfile(reader);
        if (summary.code == RETAIL_WALK_BAD_ARGUMENT)
            summary.code = RETAIL_WALK_LIST_FAILED;
        *result = summary;
        return summary.code;
    }

    *outReader = reader;
    *outAssets = assets;
    summary.code = RETAIL_WALK_OK;
    summary.directoryOffset = savedDirOffset;
    *result = summary;
    return summary.code;
}

RetailWalkDirectoryResultCode RetailWalkFastfileDirectory(
    const char *filename, RetailWalkDirectoryRecord *records, uint32_t capacity,
    RetailWalkDirectoryResult *result)
{
    RetailWalkDirectoryResult summary{};
    summary.code = RETAIL_WALK_BAD_ARGUMENT;
    if (!result)
        return summary.code;

    RetailZoneLoadSession session{};
    FsRetailFastfileReader *reader = nullptr;
    FsRetailFastfileAsset *assets = nullptr;
    const RetailWalkDirectoryResultCode openCode =
        RetailWalkOpenDirectory(filename, &session, &reader, &assets, records, capacity, &summary);
    if (openCode != RETAIL_WALK_OK)
    {
        *result = summary;
        return openCode;
    }
    const uint32_t assetCount = summary.assetCount;

    // Asset bodies follow the directory in serialized order, mirroring the
    // engine's LoadAssetHeader stream contract: StringTable keeps the block-4
    // stream position without a temp push and has no insert form, every other
    // inline/insert body streams inside its own rewinding temp-block scope
    // (with the insert header first reserving its 4-byte block-4 slot), and a
    // non-sentinel header is an already-loaded alias with no body here.  Any
    // inline body this dispatcher cannot consume is a hard error: returning
    // success would leave the FS stream desynchronized and falsely claim a
    // complete zone walk.
    uint32_t ordinal = 0;
    for (; ordinal < assetCount; ++ordinal)
    {
        const uint32_t type = records[ordinal].type;
        const uint32_t header = records[ordinal].header;
        if (records[ordinal].state == RETAIL_WALK_NO_STREAM)
            continue;
        if (type == ASSET_TYPE_STRINGTABLE)
        {
            if (!header)
                continue;
            // Load_XAssetHeader (Android DB_ConvertOffsetToAlias): any header
            // other than the sentinel forms is an already-loaded asset from
            // elsewhere in the zone graph.  The engine never fails resolving
            // it, and this bounded walker has no visibility into the target,
            // so it stays classified deferred without consuming stream bytes.
            if (header != kInlineReference)
                continue;
            if (!ReadRetailStringTableBody(&session, reader, &records[ordinal]))
                break;
            ++summary.walkedStringTableCount;
            ++summary.walkedDeferredCount;
            --summary.deferredCount;
            continue;
        }
        if (!header)
            continue;
        if (header != kInlineReference && header != kInsertReference)
            continue;
        if (header == kInsertReference &&
            !RetailWireBlocksAlloc(&session.wire, 4, 4, 4))
            break;
        RetailWalkTempScope scope;
        if (!RetailWalkBeginTemp(&session, &scope))
            break;
        const bool ok = RetailWalkDispatchAssetBody(&session, reader, type,
                                                    &records[ordinal], &summary);
        RetailWalkEndTemp(&session, &scope);
        if (!ok)
            break;
        switch (type)
        {
        // TechniqueSet bodies count inside their reader (named and empty
        // alike); a switch bump here would count every named body twice.
        // Passes still fold here: they accumulate per nested technique,
        // not per set.
        case ASSET_TYPE_TECHNIQUE_SET:
            summary.walkedPassCount += records[ordinal].nestedPassCount;
            break;
        case ASSET_TYPE_FONT:
            ++summary.walkedFontCount;
            break;
        case ASSET_TYPE_RAWFILE:
            ++summary.walkedRawFileCount;
            break;
        // PhysPreset, SndCurve, Material, Image, and MenuList bodies count
        // inside their readers (top-level and nested alike); a switch bump
        // here would count every top-level body twice and break exact OAT
        // reconciliation. Font, RawFile, and LightDef have no nested
        // bodies, so they keep counting here.
        case ASSET_TYPE_PHYSPRESET:
        case ASSET_TYPE_SOUND_CURVE:
        case ASSET_TYPE_MATERIAL:
        case ASSET_TYPE_IMAGE:
        case ASSET_TYPE_MENULIST:
            break;
        case ASSET_TYPE_LIGHT_DEF:
            ++summary.walkedLightDefCount;
            break;
        // XAnimParts, XModel, and Weapon bodies count inside their readers
        // (top-level and nested alike); a switch bump here would count every
        // top-level body twice and break exact OAT reconciliation.
        case ASSET_TYPE_XANIMPARTS:
        case ASSET_TYPE_XMODEL:
        case ASSET_TYPE_WEAPON:
            break;
        default:
            break;
        }
        ++summary.walkedDeferredCount;
        --summary.deferredCount;
    }
    if (ordinal < assetCount)
    {
        for (uint32_t block = 0; block < 9; ++block)
            summary.endCursor[block] = session.wire.cursor[block];
        summary.failedOrdinal = ordinal;
        delete[] assets;
        RetailZoneLoadSessionAbort(&session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_BODY_FAILED;
        *result = summary;
        return summary.code;
    }

    delete[] assets;
    for (uint32_t block = 0; block < 9; ++block)
        summary.endCursor[block] = session.wire.cursor[block];
    RetailZoneLoadSessionAbort(&session);
    FS_CloseRetailFastfile(reader);
    summary.code = RETAIL_WALK_OK;
    *result = summary;
    return summary.code;
}

namespace
{
// Stream + rewind a fixed-size root struct in block 0, matching the exact
// contract every Read*Body function above already established: the FS
// stream is advanced by `bytes` at the (4-byte-aligned) current cursor, then
// the in-memory write cursor is rewound back to the start of that span so
// the installed RetailZoneAssetLoader's own RetailWireBlocksRead call
// consumes the very bytes just streamed instead of the next, unrelated span.
bool RetailWalkLiveStreamRootBlock0(RetailZoneLoadSession *session,
                                    FsRetailFastfileReader *reader, uint32_t bytes,
                                    uint32_t *bodyStart)
{
    *bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, bytes, 4))
        return false;
    RetailWireBlocksRewind(&session->wire, 0, *bodyStart);
    return true;
}

struct RetailLiveXModelLoaderContext
{
    FsRetailFastfileReader *reader = nullptr;
    uint32_t directoryOffset = 0;
    uint32_t directoryBytes = 0;
    const char *const *stubNames = nullptr;
    uint32_t stubCount = 0;
    RetailWireTechniqueCache *techCache = nullptr;
    // Zone-scoped world-load context (heap-owned here): its widened-slot
    // table must outlive any single XModel because material/model slots
    // alias sibling slots across the whole zone (raw DB_ConvertOffsetToAlias
    // indirection), and the GfxWorld's own draw-inst model slots alias the
    // top-level XModels widened through this same context.
    std::unique_ptr<RetailWorldLoadContext> world;
    // Set by the XModel loader when the body was an insert form: the block-4
    // DB_InsertPointer slot it reserved. The top-level driver records that
    // slot with the session-registered header once dispatch returns (the
    // loader itself cannot know the canonical registered pointer).
    uint32_t lastInsertSlotOffset = 0;
};

bool RetailLiveLoadXModelAsset(RetailZoneLoadSession *session, XAssetType type,
                               bool insert, void *opaque, XAssetHeader *out)
{
    if (type != ASSET_TYPE_XMODEL || !session || !opaque || !out)
        return false;
    auto *context = static_cast<RetailLiveXModelLoaderContext *>(opaque);
    if (!context->reader || !context->world)
        return false;
    // The widened model's block-0 root (and its walk-consumed physPreset/
    // physGeom roots) are temp-block placements exactly like the walk-only
    // reader's: block 0's cursor must rewind after each body or every later
    // block-0 consumer (LoadedSound data, XAnim tables) exhausts the block.
    RetailWalkTempScope scope;
    if (!RetailWalkBeginTemp(session, &scope))
        return false;
    context->lastInsertSlotOffset = 0;
    const bool ok = RetailWalkLiveLoadXModel(session, context->reader,
                                             insert ? kInsertReference : kInlineReference,
                                             out, context->world.get(),
                                             &context->lastInsertSlotOffset);
    RetailWalkEndTemp(session, &scope);
    return ok;
}

// Android Load_LocalizeEntry order: root in block 0 (value ref then name
// ref), then value's XString followed by name's XString in block 4.  Only
// the fully-inline shape is wired up today; anything else is out of scope
// for this live loader (RetailDecodeLocalizeEntry itself only accepts the
// inline form regardless).
bool RetailWalkLiveLoadLocalizeEntry(RetailZoneLoadSession *session,
                                     FsRetailFastfileReader *reader,
                                     XAssetHeader *header, uint32_t *handle)
{
    uint32_t bodyStart = 0;
    if (!RetailWalkLiveStreamRootBlock0(session, reader, 8, &bodyStart))
    {
        Com_Printf(0, "RetailWalkLiveLoadLocalizeEntry: RootBlock0 failed\n");
        return false;
    }
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t valueRef = ReadRetailLe32(body);
    const uint32_t nameRef = ReadRetailLe32(body + 4);
    if (!valueRef && !nameRef)
    {
        Com_Printf(0, "RetailWalkLiveLoadLocalizeEntry: empty entry\n");
        return false;
    }
    const uint32_t stringStart = session->wire.cursor[4];
    if (valueRef == kInlineReference)
    {
        if (!ReadInlineRetailString(session, reader, nullptr))
        {
            Com_Printf(0, "RetailWalkLiveLoadLocalizeEntry: StreamInlineString value failed\n");
            return false;
        }
    }
    if (nameRef == kInlineReference)
    {
        if (!ReadInlineRetailString(session, reader, nullptr))
        {
            Com_Printf(0, "RetailWalkLiveLoadLocalizeEntry: StreamInlineString name failed\n");
            return false;
        }
    }
    RetailWireBlocksRewind(&session->wire, 4, stringStart);
    const RetailZoneAssetResult res = RetailZoneLoadSessionDispatchAsset(
        session, ASSET_TYPE_LOCALIZE_ENTRY, kInlineReference, header, handle);
    RetailWireBlocksRewind(&session->wire, 0, bodyStart);
    if (res != RETAIL_ZONE_ASSET_OK)
    {
        Com_Printf(0, "RetailWalkLiveLoadLocalizeEntry: dispatch failed res=%d\n", res);
        return false;
    }
    return true;
}

// Android Load_MapEnts order: 12-byte root {name, entityString,
// numEntityChars} in block 0, then the name XString followed by exactly
// numEntityChars entity bytes in block 4 (an alias entity slot consumes
// no stream bytes; the loader resolves it against the session blocks).
// Bounded mode keeps this live: the camera source is
// the MapEnts player-start entity.
bool RetailWalkLiveLoadMapEnts(RetailZoneLoadSession *session,
                               FsRetailFastfileReader *reader,
                               XAssetHeader *header, uint32_t *handle)
{
    uint32_t bodyStart = 0;
    if (!RetailWalkLiveStreamRootBlock0(session, reader, 12, &bodyStart))
    {
        Com_Printf(0, "RetailWalkLiveLoadMapEnts: RootBlock0 failed\n");
        return false;
    }
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t entityRef = ReadRetailLe32(body + 4);
    const uint32_t numChars = ReadRetailLe32(body + 8);
    const uint32_t spanStart = session->wire.cursor[4];
    if (nameRef == kInlineReference &&
        !ReadInlineRetailString(session, reader, nullptr))
    {
        Com_Printf(0, "RetailWalkLiveLoadMapEnts: StreamInlineString failed\n");
        return false;
    }
    if (entityRef == kInlineReference)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, numChars, 1))
        {
            Com_Printf(0, "RetailWalkLiveLoadMapEnts: entity span unreadable\n");
            return false;
        }
    }
    RetailWireBlocksRewind(&session->wire, 4, spanStart);
    const RetailZoneAssetResult res = RetailZoneLoadSessionDispatchAsset(
        session, ASSET_TYPE_MAP_ENTS, kInlineReference, header, handle);
    RetailWireBlocksRewind(&session->wire, 0, bodyStart);
    if (res != RETAIL_ZONE_ASSET_OK)
    {
        Com_Printf(0, "RetailWalkLiveLoadMapEnts: dispatch failed res=%d\n", res);
        return false;
    }
    return true;
}

// Live counterpart to ReadRetailComWorldBody. Stream the wire shape first,
// then replay it from zone-owned blocks through the native decoder.
bool RetailWalkLiveLoadComWorld(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader,
                                XAssetHeader *header, uint32_t *handle)
{
    constexpr uint32_t kRootBytes = 16;
    constexpr uint32_t kLightBytes = 68;
    constexpr uint32_t kDefNameOffset = 64;
    uint32_t rootStart = 0;
    if (!RetailWalkLiveStreamRootBlock0(session, reader, kRootBytes, &rootStart))
    {
        Com_Printf(0, "RetailComWorld: root stream failed\n");
        return false;
    }
    const uint8_t *root = session->zoneMemory->blocks[0].data + rootStart;
    const uint32_t nameRef = ReadRetailLe32(root);
    const uint32_t count = ReadRetailLe32(root + 8);
    const uint32_t lightsRef = ReadRetailLe32(root + 12);
    if (count > UINT32_MAX / kLightBytes || (!!count != !!lightsRef))
    {
        Com_Printf(0, "RetailComWorld: invalid preload root count=%u ref=0x%08x\n", count, lightsRef);
        return false;
    }
    const uint32_t block4Start = session->wire.cursor[4];
    if (nameRef == kInlineReference && !ReadInlineRetailString(session, reader, nullptr))
    {
        Com_Printf(0, "RetailComWorld: preload world name failed\n");
        return false;
    }
    if (lightsRef == kInlineReference || lightsRef == kInsertReference)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, count * kLightBytes, 4))
        {
            Com_Printf(0, "RetailComWorld: preload light array failed count=%u\n", count);
            return false;
        }
        const uint8_t *lights = session->zoneMemory->blocks[4].data +
                                session->wire.cursor[4] - count * kLightBytes;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (ReadRetailLe32(lights + i * kLightBytes + kDefNameOffset) == kInlineReference &&
                !ReadInlineRetailString(session, reader, nullptr))
            {
                Com_Printf(0, "RetailComWorld: preload def name failed index=%u\n", i);
                return false;
            }
        }
    }
    else if (lightsRef)
    {
        XBlock blocks[9]{};
        for (uint32_t block = 0; block < 9; ++block)
            blocks[block] = session->zoneMemory->blocks[block];
        RetailWireToken token{};
        if (!RetailWireTokenDecodeBlocks(blocks, {lightsRef}, count * kLightBytes,
                                         1u << 4, &token) ||
            token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
        {
            Com_Printf(0, "RetailComWorld: preload alias array invalid ref=0x%08x\n", lightsRef);
            return false;
        }
    }
    RetailWireBlocksRewind(&session->wire, 4, block4Start);
    const RetailZoneAssetResult res = RetailZoneLoadSessionDispatchAsset(
        session, ASSET_TYPE_COMWORLD, kInlineReference, header, handle);
    RetailWireBlocksRewind(&session->wire, 0, rootStart);
    if (res != RETAIL_ZONE_ASSET_OK)
        Com_Printf(0, "RetailComWorld: dispatch failed res=%s\n", RetailZoneAssetResultName(res));
    return res == RETAIL_ZONE_ASSET_OK;
}

// Android Load_Font order: 24-byte root (name, pixelHeight, glyphCount,
// material, glowMaterial, glyphs) in block 0, then the name XString and the
// glyph leaf array in block 4.  RetailDecodeFont resolves the two material
// slots through the normal recursive dispatcher itself (a null slot is a
// valid zero token).
//
// The two material slots are Load_MaterialHandle fields (db_load.cpp:2643):
// null binds null, inline (-1) widens the nested body, insert (-2) first
// reserves its 4-byte block-4 DB_InsertPointer slot and then writes the
// published material pointer into it (`*inserted = *varMaterialHandle`), and
// every other form is DB_ConvertOffsetToAlias -- the 4 bytes at that block-4
// slot, which is exactly the pointer the insert owner wrote there.
//
// code_post_gfx.ff relies on all three: smallDevFont declares fonts/devfonts
// inline (insert), consoleFont declares fonts/gamefonts_pc inline (insert),
// and the other seven fonts alias those two slots by offset.  The port's
// pristine block-4 mirror can never hold the loader's in-memory write, so the
// insert reservation is recorded in the zone-scoped widened-slot ledger
// (RetailWorldRecordZoneSlot, the port equivalent of *inserted = ...) and an
// offset alias resolves through RetailWorldResolveNestedAlias (the port
// equivalent of DB_ConvertOffsetToAlias).  A non-null slot that resolves to
// nothing fails the zone load loudly; there is no hardcoded font-material
// substitution.
bool RetailWalkLiveLoadFont(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                             XAssetHeader *header, uint32_t *handle, uint32_t directoryOffset,
                             uint32_t directoryBytes, const char *const *stubNames, uint32_t stubCount,
                             RetailWireTechniqueCache *cache, RetailWorldLoadContext *worldContext)
{
    uint32_t bodyStart = 0;
    if (!RetailWalkLiveStreamRootBlock0(session, reader, 24, &bodyStart))
        return false;
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t pixelHeight = ReadRetailLe32(body + 4);
    const uint32_t glyphCount = ReadRetailLe32(body + 8);
    const uint32_t materialRef = ReadRetailLe32(body + 12);
    const uint32_t glowMaterialRef = ReadRetailLe32(body + 16);
    const uint32_t glyphRef = ReadRetailLe32(body + 20);
    if (nameRef != kInlineReference || glyphCount > UINT32_MAX / 24u)
        return false;

    if (!materialRef && !glowMaterialRef)
    {
        const uint32_t blockStart = session->wire.cursor[4];
        if (!ReadInlineRetailString(session, reader, nullptr))
            return false;
        if (glyphCount)
        {
            if (glyphRef != kInlineReference ||
                !RetailZoneLoadSessionReadStream(session, reader, 4, glyphCount * 24u, 1))
                return false;
        }
        else if (glyphRef)
        {
            return false;
        }
        RetailWireBlocksRewind(&session->wire, 4, blockStart);
        const RetailZoneAssetResult res = RetailZoneLoadSessionDispatchAsset(session, ASSET_TYPE_FONT, kInlineReference,
                                                                             header, handle);
        RetailWireBlocksRewind(&session->wire, 0, bodyStart);
        return res == RETAIL_ZONE_ASSET_OK;
    }

    // Retail font carrying embedded materials (code_post_gfx.ff / ui.ff)
    const uint32_t nameStart = session->wire.cursor[4];
    if (!ReadInlineRetailString(session, reader, nullptr))
        return false;
    const char *fontName = reinterpret_cast<const char *>(session->zoneMemory->blocks[4].data + nameStart);

    // Font-material activation: the three Load_MaterialHandle forms
    // are implemented exactly; see the function comment above.  The insert
    // reservation is recorded in the zone slot ledger so later fonts aliasing
    // that slot bind the same registered Material the original's
    // *inserted = *varMaterialHandle wrote there.
    Material *fontMaterial = nullptr;
    if (materialRef == kInlineReference || materialRef == kInsertReference)
    {
        uint32_t slotOffset = 0;
        if (materialRef == kInsertReference)
        {
            uint8_t *slot = RetailWireBlocksAlloc(&session->wire, 4, 4, 4);
            if (!slot)
                return false;
            slotOffset = static_cast<uint32_t>(slot - session->zoneMemory->blocks[4].data);
        }
        SyncSessionToReader(session, reader, 4);
        XAssetHeader matHeader{};
        if (!RetailWalkLiveLoadMaterial(session, reader, kInlineReference, &matHeader,
                                        directoryOffset, directoryBytes,
                                        stubNames, stubCount, cache))
            return false;
        SyncReaderToSession(session, reader, 4);
        fontMaterial = matHeader.material;
        if (slotOffset && (!RetailWorldRecordZoneSlot(worldContext, slotOffset,
                                                      ASSET_TYPE_MATERIAL, matHeader)))
        {
            Com_Printf(0, "KILLHOUSE_FONT_MATERIAL_UNRESOLVED font=%s insert_slot=%u\n",
                       fontName ? fontName : "(null)", slotOffset);
            return false;
        }
    }
    else if (materialRef)
    {
        XAssetHeader aliasHeader{};
        if (!RetailWorldResolveNestedAlias(worldContext, materialRef, ASSET_TYPE_MATERIAL,
                                           &aliasHeader) ||
            !aliasHeader.material)
        {
            Com_Printf(0, "KILLHOUSE_FONT_MATERIAL_UNRESOLVED font=%s ref=0x%08x\n",
                       fontName ? fontName : "(null)", materialRef);
            return false;
        }
        fontMaterial = aliasHeader.material;
    }

    Material *fontGlowMaterial = nullptr;
    if (glowMaterialRef == kInlineReference || glowMaterialRef == kInsertReference)
    {
        uint32_t slotOffset = 0;
        if (glowMaterialRef == kInsertReference)
        {
            uint8_t *slot = RetailWireBlocksAlloc(&session->wire, 4, 4, 4);
            if (!slot)
                return false;
            slotOffset = static_cast<uint32_t>(slot - session->zoneMemory->blocks[4].data);
        }
        SyncSessionToReader(session, reader, 4);
        XAssetHeader glowHeader{};
        if (!RetailWalkLiveLoadMaterial(session, reader, kInlineReference, &glowHeader,
                                        directoryOffset, directoryBytes,
                                        stubNames, stubCount, cache))
            return false;
        SyncReaderToSession(session, reader, 4);
        fontGlowMaterial = glowHeader.material;
        if (slotOffset && (!RetailWorldRecordZoneSlot(worldContext, slotOffset,
                                                      ASSET_TYPE_MATERIAL, glowHeader)))
        {
            Com_Printf(0, "KILLHOUSE_FONT_MATERIAL_UNRESOLVED font=%s glow_insert_slot=%u\n",
                       fontName ? fontName : "(null)", slotOffset);
            return false;
        }
    }
    else if (glowMaterialRef)
    {
        XAssetHeader aliasHeader{};
        if (!RetailWorldResolveNestedAlias(worldContext, glowMaterialRef, ASSET_TYPE_MATERIAL,
                                           &aliasHeader) ||
            !aliasHeader.material)
        {
            Com_Printf(0, "KILLHOUSE_FONT_MATERIAL_UNRESOLVED font=%s glow_ref=0x%08x\n",
                       fontName ? fontName : "(null)", glowMaterialRef);
            return false;
        }
        fontGlowMaterial = aliasHeader.material;
    }

    Glyph *glyphs = nullptr;
    if (glyphCount)
    {
        if (glyphRef != kInlineReference)
            return false;
        // Same 4-alignment as the walk-only reader above (engine
        // AllocLoad_FxElemVisStateSample): the glyph pointer must be the
        // aligned destination, not the pre-alignment cursor.
        const uint32_t glyphStart = (session->wire.cursor[4] + 3u) & ~3u;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, glyphCount * 24u, 4))
            return false;
        glyphs = reinterpret_cast<Glyph *>(session->zoneMemory->blocks[4].data + glyphStart);
        SyncSessionToReader(session, reader, 4);
    }
    else if (glyphRef)
    {
        return false;
    }

    Font_s *font = static_cast<Font_s *>(RetailZoneLoadSessionAlloc(session, sizeof(Font_s), alignof(Font_s)));
    if (!font)
        return false;
    std::memset(font, 0, sizeof(*font));
    font->fontName = fontName;
    font->pixelHeight = static_cast<int>(pixelHeight);
    font->glyphCount = static_cast<int>(glyphCount);
    font->glyphs = glyphs;
    font->material = fontMaterial;
    font->glowMaterial = fontGlowMaterial;

    Com_Printf(0, "RetailWalkLiveLoadFont: '%s' mat='%s' glow='%s' glyphs=%d pxHeight=%d\n",
               fontName ? fontName : "(null)",
               (fontMaterial && fontMaterial->info.name) ? fontMaterial->info.name : "(null)",
               (fontGlowMaterial && fontGlowMaterial->info.name) ? fontGlowMaterial->info.name : "(null)",
               font->glyphCount, font->pixelHeight);

    const XAssetHeader registered = RetailZoneLoadSessionRegister(session, ASSET_TYPE_FONT, {font});
    if (!registered.font)
        return false;
    *header = registered;
    *handle = 0;
    RetailWireBlocksRewind(&session->wire, 0, bodyStart);
    return true;
}

// Android Load_RawFile order: 12-byte root in block 0, then the name
// XString (absent when nameRef is 0, matching RetailDecodeRawFile's own
// InlineOrNullString), then -- for any nonzero buffer slot, a presence
// marker rather than an encoded alias -- exactly len + 1 raw bytes.
static bool RetailWalkLiveLoadRawFile(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                                      XAssetHeader *header, uint32_t *handle)
{
    uint32_t bodyStart = 0;
    if (!RetailWalkLiveStreamRootBlock0(session, reader, 12, &bodyStart))
    {
        return false;
    }
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const int32_t length = static_cast<int32_t>(ReadRetailLe32(body + 4));
    const uint32_t bufferRef = ReadRetailLe32(body + 8);
    if (length < 0)
    {
        return false;
    }
    const uint32_t blockStart = session->wire.cursor[4];
    if (nameRef == kInlineReference && !ReadInlineRetailString(session, reader, nullptr))
    {
        return false;
    }
    if (bufferRef)
    {
        const uint32_t bytes = static_cast<uint32_t>(length) + 1u;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, bytes, 1))
        {
            return false;
        }
    }
    RetailWireBlocksRewind(&session->wire, 4, blockStart);
    const RetailZoneAssetResult res = RetailZoneLoadSessionDispatchAsset(session, ASSET_TYPE_RAWFILE, kInlineReference,
                                                                         header, handle);
    RetailWireBlocksRewind(&session->wire, 0, bodyStart);
    return res == RETAIL_ZONE_ASSET_OK;
}

// Resolve a LightDef's own alias-form name reference (any non-zero,
// non-inline nameRef -- the -1 inline form streams its own bytes and never
// reaches here) to its already-declared block-4 string, matching every
// other alias-name resolution in this codebase (db_retail_decode_material.
// cpp's ReadBlockString, db_retail_decode_world.cpp's ResolveNestedAlias):
// decode canonically to a block-4 offset, read that exact printable-ASCII
// string, fail loudly on any miss. Previously this fell straight through
// to a null name for any non-inline form -- harmless in the host-only test
// harness's narrow stub registry, but a real null-pointer crash in the
// production DB_LinkXAssetEntry -> DB_StringTableGetName generic name
// getter (GfxLightDef::name is its first field, the same "name is usually
// the first field" COMDAT-folded getter every mostly-name-first asset type
// shares) -- found only by this milestone's first-ever live-device run of
// this exact code path against real killhouse.ff (its sun LightDef).
// Returns a pointer directly into reader's own persistent block-4 buffer
// (same convention the inline-name case already uses --
// session->zoneMemory->blocks[4].data + nameStart -- a live pointer, not a
// copy, valid for the zone's whole lifetime), or null on any decode/
// validation failure.
static const char *ReadRetailLightDefAliasName(FsRetailFastfileReader *reader, uint32_t nameRef)
{
    if (!reader || !nameRef)
        return nullptr;
    XBlock blocks[9]{};
    for (uint32_t block = 0; block < 9; ++block)
        blocks[block] = {const_cast<uint8_t *>(FS_RetailFastfileBlockData(reader, block)),
                         FS_RetailFastfileBlockSize(reader, block)};
    RetailWireToken token{};
    if (!RetailWireTokenDecodeBlocks(blocks, {nameRef}, 0, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
    {
        Com_Printf(0, "ReadRetailLightDefAliasName: decode failed nameRef=0x%08x\n", nameRef);
        return nullptr;
    }
    const uint8_t *data = FS_RetailFastfileBlockData(reader, 4);
    const uint32_t size = FS_RetailFastfileBlockSize(reader, 4);
    uint32_t len = 0;
    while (token.offset + len < size && data[token.offset + len] != 0)
    {
        const uint8_t c = data[token.offset + len];
        if (c < 0x20 || c > 0x7e)
        {
            Com_Printf(0, "ReadRetailLightDefAliasName: unreadable name at offset=%u\n", token.offset);
            return nullptr;
        }
        ++len;
    }
    if (token.offset + len >= size)
    {
        Com_Printf(0, "ReadRetailLightDefAliasName: unterminated name at offset=%u\n", token.offset);
        return nullptr;
    }
    return reinterpret_cast<const char *>(data + token.offset);
}

// Android Load_GfxLightDef: 16-byte root in block 0 (name ref at +0, attenuation image slot ref at +4,
// samplerState at +8, lmapLookupStart at +12). Live loader streams root, inline name, and nested
// image slot, then registers GfxLightDef into the zone session.
static bool RetailWalkLiveLoadLightDef(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                                      XAssetHeader *header, uint32_t *handle,
                                      uint32_t directoryOffset, uint32_t directoryBytes,
                                      const char *const *stubNames, uint32_t stubCount)
{
    constexpr uint32_t kBodyBytes = 16;
    uint32_t bodyStart = 0;
    if (!RetailWalkLiveStreamRootBlock0(session, reader, kBodyBytes, &bodyStart))
    {
        return false;
    }
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t imageRef = ReadRetailLe32(body + 4);
    const uint8_t samplerState = body[8];
    const int32_t lmapLookupStart = static_cast<int32_t>(ReadRetailLe32(body + 12));
    Com_Printf(0, "RetailWalkLiveLoadLightDef: root nameRef=0x%08x imageRef=0x%08x samplerState=%u "
                  "lmapLookupStart=%d bodyStart=%u raw='%02x %02x %02x %02x %02x %02x %02x %02x "
                  "%02x %02x %02x %02x %02x %02x %02x %02x'\n",
               nameRef, imageRef, samplerState, lmapLookupStart, bodyStart,
               body[0], body[1], body[2], body[3], body[4], body[5], body[6], body[7],
               body[8], body[9], body[10], body[11], body[12], body[13], body[14], body[15]);

    const uint32_t nameStart = session->wire.cursor[4];
    if (nameRef == kInlineReference && !ReadInlineRetailString(session, reader, nullptr))
    {
        return false;
    }
    const char *lightName = nullptr;
    if (nameRef == kInlineReference)
    {
        lightName = reinterpret_cast<const char *>(session->zoneMemory->blocks[4].data + nameStart);
    }
    else if (nameRef && nameRef != kInsertReference)
    {
        SyncSessionToReader(session, reader, 4);
        const char *aliasName = ReadRetailLightDefAliasName(reader, nameRef);
        if (!aliasName)
        {
            Com_Printf(0, "RetailWalkLiveLoadLightDef: alias nameRef=0x%08x unresolved\n", nameRef);
            return false;
        }
        // retained-pointer lifetime: the alias bytes live in the reader's block-4 mirror,
        // which FS_CloseRetailFastfile Z_Free's when this zone load returns
        // while the registered asset stays live. DB_GetXAssetName keys the
        // registry off lightDef->name, so the name must be owned for the
        // zone's lifetime like every other widened string (the original
        // loader's own name pointer stays inside zone memory). Killhouse's
        // light_point_linear carries exactly this alias form; keeping the raw
        // reader pointer made the runtime lookup miss (dangling name, then a
        // default-entry fallback after an 11.6 s wait).
        const std::size_t aliasBytes = std::strlen(aliasName) + 1;
        char *nameCopy = static_cast<char *>(
            RetailZoneLoadSessionAlloc(session, aliasBytes, 1));
        if (!nameCopy)
            return false;
        std::memcpy(nameCopy, aliasName, aliasBytes);
        lightName = nameCopy;
    }

    // Attenuation image: widen the real nested body, never a placeholder.
    // An insert slot (Load_GfxImagePtr's -2 form, db_load.cpp) reserves its
    // own 4-byte DB_InsertPointer slot in block 4 *before* the inline body
    // is read -- the same thing a material texture-def's own -2 slot does
    // (FS_ReadRetailFastfileMaterial, com_files.cpp) -- so a later
    // DB_ConvertOffsetToAlias reference elsewhere in the zone has
    // somewhere to find the resolved pool ref. Skipping this for a
    // shared/common attenuation image (declared insert exactly once, then
    // aliased by every other lightdef needing the same falloff texture)
    // both leaves that alias permanently unresolvable and desyncs this
    // reader's own block-4 cursor by 4 bytes relative to what the file's
    // own linker accounted for downstream.
    GfxImage *attenuation = nullptr;
    if (imageRef == kInlineReference || imageRef == kInsertReference)
    {
        // The image body is read directly through reader (FS_ReadRetailFastfileImage,
        // RetailWidenImageFromWire) rather than through session->wire, exactly like
        // every other branch in this dispatcher that touches reader directly (compare
        // the TechniqueSet/Image/Material/GfxWorld branches in RetailWalkLoadZoneAssets,
        // which all bracket their reader-based work with SyncSessionToReader/
        // SyncReaderToSession). Without the sync-in here, reader's true stream
        // position can be behind session's (e.g. this lightdef's own name was just
        // streamed through session), causing the image body to be read from the
        // wrong file position. Without the sync-out, reader's true consumption of
        // the image body never reaches session->wire.cursor[4] or
        // session->zoneMemory->blocks[4]'s copy of that byte range, so the very next
        // field or ordinal that trusts session's cursor reads/writes at a stale
        // offset -- this was the root cause of a 16-19 byte block-4 desync that
        // surfaced many ordinals later as truncated material names and unresolvable
        // TechniqueSet shared names.
        SyncSessionToReader(session, reader, 4);
        uint32_t defInsertOffset = 0;
        if (imageRef == kInsertReference &&
            FS_RetailFastfileReserveBlock4Slot(reader, &defInsertOffset) != FS_RETAIL_FF_WIRE_OK)
        {
            Com_Printf(0, "RetailWalkLiveLoadLightDef: insert slot reservation failed\n");
            return false;
        }
        FsRetailFastfileImage wireImage{};
        if (FS_ReadRetailFastfileImage(reader, kInlineReference, &wireImage) !=
            FS_RETAIL_FF_WIRE_OK)
        {
            Com_Printf(0, "RetailWalkLiveLoadLightDef: nested image body unreadable\n");
            return false;
        }
        XAssetHeader imageHeader{};
        if (!RetailWidenImageFromWire(session, reader, wireImage, &imageHeader, nullptr) ||
            !imageHeader.image)
        {
            Com_Printf(0, "RetailWalkLiveLoadLightDef: nested image widen failed\n");
            return false;
        }
        attenuation = imageHeader.image;
        if (defInsertOffset)
        {
            uint32_t poolRef = 0;
            if (FS_RetailFastfileRegisterImagePoolSlot(reader, wireImage.nameRef, defInsertOffset,
                                                       &poolRef) != FS_RETAIL_FF_WIRE_OK)
            {
                Com_Printf(0, "RetailWalkLiveLoadLightDef: insert slot pool registration failed\n");
                return false;
            }
        }
        SyncReaderToSession(session, reader, 4);
    }
    else if (imageRef)
    {
        // Same comma-stub-directory/pool-alias/plain-name resolution a
        // material's own texture-def slot already uses (RetailWidenMaterialFromWire,
        // db_retail_decode_material.cpp) -- a shared attenuation image
        // (e.g. a stock/common falloff texture) is declared inline exactly
        // once and aliased everywhere else, same as GfxWorld materials'
        // shared normal maps. ResolveImageRef consumes no stream bytes (pure
        // alias/pool lookup), but it still reads through reader's directory/pool
        // state, so keep reader caught up with any pending session-side advance
        // first for consistency with the inline/insert branch above.
        SyncSessionToReader(session, reader, 4);
        attenuation = ResolveImageRef(reader, imageRef, directoryOffset, directoryBytes,
                                      stubNames, stubCount, true);
        if (!attenuation)
        {
            Com_Printf(0, "RetailWalkLiveLoadLightDef: non-inline attenuation imageRef=0x%08x unresolved\n",
                       imageRef);
            return false;
        }
    }

    GfxLightDef *lightDef = static_cast<GfxLightDef *>(
        RetailZoneLoadSessionAlloc(session, sizeof(GfxLightDef), alignof(GfxLightDef)));
    if (!lightDef)
    {
        return false;
    }
    std::memset(lightDef, 0, sizeof(*lightDef));
    lightDef->name = lightName;
    lightDef->attenuation.samplerState = samplerState;
    lightDef->attenuation.image = attenuation;
    lightDef->lmapLookupStart = lmapLookupStart;

    XAssetHeader regHeader;
    regHeader.lightDef = lightDef;
    const XAssetHeader registered = RetailZoneLoadSessionRegister(session, ASSET_TYPE_LIGHT_DEF, regHeader);
    if (!registered.lightDef)
    {
        return false;
    }

    *header = registered;
    *handle = 0;
    RetailWireBlocksRewind(&session->wire, 0, bodyStart);
    return true;
}

// Android Load_StringTable order: unlike every other type here, the 16-byte
// root {name, columnCount, rowCount, values} lives in block 4, not block 0 --
// there is no separate temp-block root.  A nonzero values slot streams a
// contiguous rowCount * columnCount pointer table, then each inline (-1)
// cell owns one XString immediately after the table, in cell order; a zero
// cell owns nothing, and anything else is an unsupported alias this walker
// does not resolve (it consumes no stream bytes either way, matching
// ReadRetailXStringInBlock, so the FS stream stays synced even when the
// later RetailDecodeStringTable replay rejects it).
bool RetailWalkLiveLoadStringTable(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                                    XAssetHeader *header, uint32_t *handle)
{
    const uint32_t blockStart = (session->wire.cursor[4] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, 16, 4))
    {
        return false;
    }
    const uint8_t *body = session->zoneMemory->blocks[4].data + blockStart;
    const uint32_t nameRef = ReadRetailLe32(body);
    const uint32_t columnCount = ReadRetailLe32(body + 4);
    const uint32_t rowCount = ReadRetailLe32(body + 8);
    const uint32_t valuesRef = ReadRetailLe32(body + 12);
    if (nameRef != 0 && nameRef != kInlineReference)
        return false;
    if (nameRef == kInlineReference && !ReadInlineRetailString(session, reader, nullptr))
        return false;
    if (columnCount && rowCount > UINT32_MAX / columnCount)
        return false;
    const uint32_t count = rowCount * columnCount;
    if (valuesRef)
    {
        if (count > UINT32_MAX / 4u ||
            !RetailZoneLoadSessionReadStream(session, reader, 4, count * 4u, 4))
            return false;
        const uint32_t tableStart = session->wire.cursor[4] - count * 4u;
        const uint8_t *table = session->zoneMemory->blocks[4].data + tableStart;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (ReadRetailLe32(table + i * 4u) != kInlineReference)
                continue;
            if (!ReadInlineRetailString(session, reader, nullptr))
                return false;
        }
    }
    RetailWireBlocksRewind(&session->wire, 4, blockStart);
    const RetailZoneAssetResult res = RetailZoneLoadSessionDispatchAsset(session, ASSET_TYPE_STRINGTABLE, kInlineReference,
                                                                         header, handle);
    return res == RETAIL_ZONE_ASSET_OK;
}

// Live GameWorldSp (path data): the walk-only reader owns the wire order; run
// it first so the FS reader lands exactly at the asset end, rewind the
// session cursors, and let the native decoder replay the same bytes into the
// engine's gameWorldSp singleton.  Without this the map's authored pathnode
// entities never enter gameWorldSp.path and G_UpdateTrackExtraNodes reports
// every one of them as an extra node, which trips the map-load error summary.
bool RetailWalkLiveLoadGameWorldSp(RetailZoneLoadSession *session,
                                   FsRetailFastfileReader *reader,
                                   RetailWalkDirectoryRecord *record,
                                   XAssetHeader *header, uint32_t *handle)
{
    if (!session || !reader || !record || record->header != kInlineReference)
        return false;
    const uint32_t rootStart = session->wire.cursor[0];
    const uint32_t block4Start = session->wire.cursor[4];
    RetailWalkDirectoryRecord walkRecord = *record;
    RetailWalkDirectoryResult walkSummary{};
    if (!ReadRetailGameWorldSpBody(session, reader, &walkRecord, &walkSummary))
    {
        Com_Printf(0, "RetailGameWorldSp: walk preload failed b0=%u b4=%u header=0x%08x\n",
                   rootStart, block4Start, record->header);
        return false;
    }
    RetailWireBlocksRewind(&session->wire, 4, block4Start);
    RetailWireBlocksRewind(&session->wire, 0, rootStart);
    const RetailZoneAssetResult res = RetailZoneLoadSessionDispatchAsset(
        session, ASSET_TYPE_GAMEWORLD_SP, kInlineReference, header, handle);
    RetailWireBlocksRewind(&session->wire, 0, rootStart);
    if (res != RETAIL_ZONE_ASSET_OK)
        Com_Printf(0, "RetailGameWorldSp: dispatch failed res=%s\n",
                   RetailZoneAssetResultName(res));
    return res == RETAIL_ZONE_ASSET_OK;
}
} // namespace

RetailWalkLoadZoneResultCode RetailWalkLoadZoneAssets(const char *filename,
                                                       RetailWalkLoadZoneResult *result,
                                                       bool strict,
                                                       bool bounded)
{
    RetailWalkLoadZoneResult summary{};
    const int walkStartMs = static_cast<int>(Sys_Milliseconds());
    if (!result)
        return RETAIL_WALK_LOAD_BAD_ARGUMENT;
    *result = summary;
    if (!filename || !filename[0])
    {
        summary.code = RETAIL_WALK_LOAD_BAD_ARGUMENT;
        *result = summary;
        return summary.code;
    }

    // Reload idempotency (PMem retention fix): retail zones begin with
    // flags=0, so no freeFlags unload ever retires them.  Without this
    // check every R_Init reload of an already-resident graphics zone leaks
    // another full zone's PMem (blocks + 96MB native arena) until the run
    // dies in `PMem_Alloc: Need more bytes of ram`.  Zone bytes are
    // deterministic from the file, and the policy check keeps a
    // bounded-partial zone from ever satisfying a full request, so a hit
    // is a pure no-op: same slot, same registrations, zero new bytes.
    if (const uint32_t liveIndex = DB_RetailZoneFindLive(filename, bounded))
    {
        summary.code = RETAIL_WALK_LOAD_OK;
        summary.zoneIndex = liveIndex;
        summary.alreadyLoaded = true;
        *result = summary;
        Com_Printf(0, "RetailWalkLoadZoneAssets: already loaded %s (zone %u), skipping reload\n",
                   filename, liveIndex);
        return summary.code;
    }

    constexpr uint32_t kCapacity = 1u << 16;

    auto records = std::unique_ptr<RetailWalkDirectoryRecord[]>(
        new (std::nothrow) RetailWalkDirectoryRecord[kCapacity]);
    if (!records)
    {
        summary.code = RETAIL_WALK_LOAD_CAPACITY;
        *result = summary;
        return summary.code;
    }
    RetailWalkDirectoryResult directoryResult{};
    RetailZoneLoadSession session{};
    FsRetailFastfileReader *reader = nullptr;
    FsRetailFastfileAsset *assets = nullptr;
    // Reservation, not a budget: the arena is the zone's last PMem
    // allocation and is trimmed to what the zone used once it has loaded
    // (below), so the unused tail returns to the pool before the next zone.
    // Sized above the largest SP map's use; the old fixed 96 MiB
    // stopped some SP maps with out_of_arena.
    // DB_RetailZoneBegin clamps the reservation to the free pool.
    constexpr std::size_t kRetailZoneNativeArenaBytes = 256u * 1024u * 1024u;
    const RetailWalkDirectoryResultCode openCode = RetailWalkOpenDirectory(
        filename, &session, &reader, &assets, records.get(), kCapacity, &directoryResult,
        kRetailZoneNativeArenaBytes);
    const int openEndMs = static_cast<int>(Sys_Milliseconds());
    summary.assetCount = directoryResult.assetCount;
    Com_Printf(0, "RetailWalkLoadZoneAssets: openCode=%d, assetCount=%u\n", openCode, summary.assetCount);
    if (openCode != RETAIL_WALK_OK)
    {
        switch (openCode)
        {
        case RETAIL_WALK_OPEN_FAILED:   summary.code = RETAIL_WALK_LOAD_OPEN_FAILED; break;
        case RETAIL_WALK_SESSION_FAILED:summary.code = RETAIL_WALK_LOAD_SESSION_FAILED; break;
        case RETAIL_WALK_CAPACITY:      summary.code = RETAIL_WALK_LOAD_CAPACITY; break;
        default:                        summary.code = RETAIL_WALK_LOAD_LIST_FAILED; break;
        }
        *result = summary;
        return summary.code;
    }
    summary.zoneIndex = session.zoneIndex;
    // Live loads keep registrations resident (the zone stays live on
    // success, swept transactionally on abort), so walked-deferred inline
    // images may widen+register for later aliases to bind. Walk-only
    // readers never set this and must observe zero registrations.
    session.widenNestedImages = true;
    if (!RetailZoneInstallFontDecoder(&session) || !RetailZoneInstallLocalizeEntryDecoder(&session) ||
        !RetailZoneInstallRawFileDecoder(&session) || !RetailZoneInstallStringTableDecoder(&session) ||
        !RetailZoneInstallMapEntsDecoder(&session) ||
        !RetailZoneInstallComWorldDecoder(&session) ||
        !RetailZoneInstallGameWorldSpDecoder(&session))
    {
        delete[] assets;
        RetailZoneLoadSessionAbort(&session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_LOAD_SESSION_FAILED;
        *result = summary;
        return summary.code;
    }

    // Ordinal-indexed name table for TechniqueSet-typed directory entries,
    // consumed by Material's comma-stub directory-offset technique-set
    // binding (see db_retail_decode_material.h/.cpp's RetailWalkLiveLoadMaterial):
    // a Material's techniqueSetRef can point back at *this same zone's own*
    // directory bytes rather than a separate name string, naming the
    // TechniqueSet entry at that slot.  Only TechniqueSet entries ever get a
    // non-null row; every other slot stays nullptr and safely fails that
    // lookup rather than reading uninitialized data.
    const uint32_t directoryBytes = summary.assetCount * 8u;
    auto stubStorage = summary.assetCount
        ? std::unique_ptr<char[]>(new (std::nothrow) char[summary.assetCount * 64])
        : nullptr;
    auto stubNames = summary.assetCount
        ? std::unique_ptr<const char *[]>(new (std::nothrow) const char *[summary.assetCount]())
        : nullptr;
    if (summary.assetCount && (!stubStorage || !stubNames))
    {
        delete[] assets;
        RetailZoneLoadSessionAbort(&session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_LOAD_CAPACITY;
        *result = summary;
        return summary.code;
    }

    std::unique_ptr<RetailWireTechniqueCache> techCache(new (std::nothrow) RetailWireTechniqueCache());
    RetailLiveXModelLoaderContext xmodelLoaderContext{};
    xmodelLoaderContext.reader = reader;
    xmodelLoaderContext.directoryOffset = directoryResult.directoryOffset;
    xmodelLoaderContext.directoryBytes = directoryBytes;
    xmodelLoaderContext.stubNames = stubNames.get();
    xmodelLoaderContext.stubCount = summary.assetCount;
    xmodelLoaderContext.techCache = techCache.get();
    xmodelLoaderContext.world.reset(new (std::nothrow) RetailWorldLoadContext{});
    if (!xmodelLoaderContext.world)
    {
        delete[] assets;
        RetailZoneLoadSessionAbort(&session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_LOAD_SESSION_FAILED;
        *result = summary;
        return summary.code;
    }
    xmodelLoaderContext.world->session = &session;
    xmodelLoaderContext.world->reader = reader;
    xmodelLoaderContext.world->directoryOffset = directoryResult.directoryOffset;
    xmodelLoaderContext.world->directoryBytes = directoryBytes;
    xmodelLoaderContext.world->stubNames = stubNames.get();
    xmodelLoaderContext.world->stubCount = summary.assetCount;
    xmodelLoaderContext.world->techCache = techCache.get();
    if (!RetailZoneLoadSessionSetAssetLoader(&session, ASSET_TYPE_XMODEL,
                                             RetailLiveLoadXModelAsset,
                                             &xmodelLoaderContext))
    {
        delete[] assets;
        RetailZoneLoadSessionAbort(&session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_LOAD_SESSION_FAILED;
        *result = summary;
        return summary.code;
    }

    // Per-type decode profile (one Sys_Milliseconds pair per streamed
    // directory entry, plus a compact summary at the end of the zone). The
    // arrays are indexed by the real directory type; a type the wire reader
    // rejected can never reach here, so no guard is needed.
    uint64_t typeDecodeMs[ASSET_TYPE_COUNT] = {};
    uint32_t typeDecodeCount[ASSET_TYPE_COUNT] = {};
    struct RetailWalkAssetTypeTimer
    {
        int32_t startMs;
        uint64_t *accum;
        uint32_t *count;
        RetailWalkAssetTypeTimer(uint32_t type, uint64_t *a, uint32_t *c)
            : startMs(static_cast<int32_t>(Sys_Milliseconds())), accum(a), count(c) {}
        ~RetailWalkAssetTypeTimer()
        {
            *accum += static_cast<uint64_t>(
                static_cast<int32_t>(Sys_Milliseconds()) - startMs);
            ++*count;
        }
    };

    uint32_t ordinal = 0;
    const int loopStartMs = static_cast<int>(Sys_Milliseconds());
    for (; ordinal < summary.assetCount; ++ordinal)
    {
        const uint32_t type = records[ordinal].type;
        const uint32_t header = records[ordinal].header;
        if (records[ordinal].state == RETAIL_WALK_NO_STREAM)
            continue;
        RetailWalkAssetTypeTimer typeTimer(type, &typeDecodeMs[type], &typeDecodeCount[type]);
        // Bounded first-frame mode walks the StringTable like every other
        // non-closure family instead of registering it.
        if (!bounded && type == ASSET_TYPE_STRINGTABLE)
        {
            if (!header)
                continue;
            // StringTable has no insert form (Android Load_XAssetHeader);
            // any non-inline header is an already-loaded alias with no body.
            if (header != kInlineReference)
                continue;
            SyncSessionToReader(&session, reader, 4);
            XAssetHeader registered{};
            uint32_t handle = 0;
            if (!RetailWalkLiveLoadStringTable(&session, reader, &registered, &handle))
                break;
            SyncReaderToSession(&session, reader, 4);
            ++summary.registeredStringTableCount;
            continue;
        }
        if (!header)
            continue;
        if (header != kInlineReference && header != kInsertReference)
            continue;

        // Bounded mode keeps the world-closure LightDef live (the sun
        // binds it) and the MapEnts live (the camera source parses its
        // entity string), but walks every other small-root family instead
        // of registering assets the bounded frame never activates --
        // except RawFile, which stays live in both policies: the bounded
        // spmap path reaches the real game script chain
        // (GScr_LoadScriptsAndAnims reads every .gsc exclusively through
        // RawFile assets), so deferring script bytes would starve the very
        // scripts the probe executes.  Same reader either way
        // (RetailWalkLiveLoadRawFile), so stream positions are unchanged;
        // only the registered/walked classification moves.  M9a's host
        // loads are full-policy and assert no bounded counts, M3e is
        // walk-only, and the draw ledger never counts registrations.
        if (header == kInlineReference &&
            ((!bounded && (type == ASSET_TYPE_LOCALIZE_ENTRY || type == ASSET_TYPE_FONT)) ||
             type == ASSET_TYPE_RAWFILE || type == ASSET_TYPE_LIGHT_DEF ||
             type == ASSET_TYPE_MAP_ENTS || type == ASSET_TYPE_COMWORLD ||
             type == ASSET_TYPE_GAMEWORLD_SP))
        {
            // A live root begins by consuming block 0, then captures its
            // block-4 start for replay through the native decoder.  The
            // reader is the forward-only authority and may already be ahead
            // after a nested reader-owned body; heal the session *before*
            // that capture.  Delaying this until the first block-4 read
            // leaves the saved replay cursor stale and decodes the prior
            // asset's bytes as this root (observed on Killhouse ComWorld).
            SyncReaderToSession(&session, reader, 4);
            XAssetHeader registered{};
            uint32_t handle = 0;
            bool ok;
            if (type == ASSET_TYPE_LOCALIZE_ENTRY)
                ok = RetailWalkLiveLoadLocalizeEntry(&session, reader, &registered, &handle);
            else if (type == ASSET_TYPE_FONT)
                ok = RetailWalkLiveLoadFont(&session, reader, &registered, &handle,
                                            directoryResult.directoryOffset, directoryBytes,
                                            stubNames.get(), summary.assetCount, techCache.get(),
                                            xmodelLoaderContext.world.get());
            else if (type == ASSET_TYPE_RAWFILE)
                ok = RetailWalkLiveLoadRawFile(&session, reader, &registered, &handle);
            else if (type == ASSET_TYPE_MAP_ENTS)
                ok = RetailWalkLiveLoadMapEnts(&session, reader, &registered, &handle);
            else if (type == ASSET_TYPE_COMWORLD)
                ok = RetailWalkLiveLoadComWorld(&session, reader, &registered, &handle);
            else if (type == ASSET_TYPE_GAMEWORLD_SP)
                ok = RetailWalkLiveLoadGameWorldSp(&session, reader, &records[ordinal],
                                                   &registered, &handle);
            else
                ok = RetailWalkLiveLoadLightDef(&session, reader, &registered, &handle,
                                                directoryResult.directoryOffset, directoryBytes,
                                                stubNames.get(), summary.assetCount);
            if (!ok)
                break;
            if (type == ASSET_TYPE_LOCALIZE_ENTRY)
                ++summary.registeredLocalizeCount;
            else if (type == ASSET_TYPE_FONT)
                ++summary.registeredFontCount;
            else if (type == ASSET_TYPE_RAWFILE)
                ++summary.registeredRawFileCount;
            else if (type == ASSET_TYPE_MAP_ENTS)
                ++summary.registeredMapEntsCount;
            else if (type == ASSET_TYPE_COMWORLD)
                ++summary.registeredComWorldCount;
            else if (type == ASSET_TYPE_GAMEWORLD_SP)
                ++summary.registeredGameWorldSpCount;
            else
                ++summary.registeredLightDefCount;
            continue;
        }

        // TechniqueSet's wire references are Disk32-style absolute-offset
        // tokens resolved directly against the FS reader's own per-block
        // buffers (see db_retail_decode_techniqueset.h), not the sequential
        // inline-or-nothing sentinel the block above handles -- it neither
        // needs nor uses session->wire for this asset family.
        if (header == kInlineReference && type == ASSET_TYPE_TECHNIQUE_SET)
        {
            SyncSessionToReader(&session, reader, 4);
            XAssetHeader registered{};
            if (!RetailWalkLiveLoadTechniqueSet(&session, reader, header, &registered, techCache.get()))
                break;
            SyncReaderToSession(&session, reader, 4);
            ++summary.registeredTechniqueSetCount;
            // Record this entry's own registered name for Material's
            // directory-offset technique-set binding above, whether or not
            // it happens to be a comma-stub -- ResolveTechniqueSetRef itself
            // checks for the leading comma before using a row.
            if (registered.techniqueSet && registered.techniqueSet->name)
            {
                char *row = stubStorage.get() + static_cast<std::size_t>(ordinal) * 64;
                std::snprintf(row, 64, "%s", registered.techniqueSet->name);
                stubNames[ordinal] = row;
            }
            continue;
        }

        // Same reader-resolved token addressing as TechniqueSet above.
        // Strict acceptance mode threads through to the image widen:
        // unmeasured map types and unresolvable names fail instead of
        // widening tolerantly.
        if (header == kInlineReference && type == ASSET_TYPE_IMAGE)
        {
            SyncSessionToReader(&session, reader, 4);
            const uint32_t imageCursorBefore = FS_RetailFastfileBlockCursor(reader, 4);
            XAssetHeader registered{};
            if (!RetailWalkLiveLoadImage(&session, reader, header, &registered, strict))
                break;
            SyncReaderToSession(&session, reader, 4);
            const uint32_t imageCursorAfter = FS_RetailFastfileBlockCursor(reader, 4);
            if (registered.image && registered.image->name && registered.image->name[0])
                RETAIL_ASSET_TRACE(0, "RetailWalk: top-level image ord=%u name='%s' b4=[%u,%u)\n",
                                   ordinal, registered.image->name, imageCursorBefore, imageCursorAfter);
            ++summary.registeredImageCount;
            if (registered.image && registered.image->name && stubStorage && stubNames)
            {
                char *row = stubStorage.get() + static_cast<std::size_t>(ordinal) * 64;
                std::snprintf(row, 64, "%s", registered.image->name);
                stubNames[ordinal] = row;
            }
            continue;
        }

        // Same reader-resolved token addressing as TechniqueSet/Image above.
        // Strict mode reaches the material widen: no synthesis, no
        // hardcoded identities, no white/$default substitution.
        if (header == kInlineReference && type == ASSET_TYPE_MATERIAL)
        {
            SyncSessionToReader(&session, reader, 4);
            const uint32_t bodyOffset = FS_RetailFastfileBlockCursor(reader, 4);
            XAssetHeader registered{};
            bool deferredAlias = false;
            char deferredAliasName[64]{};
            const bool materialLoaded = RetailWalkLiveLoadMaterial(
                &session, reader, header, &registered,
                directoryResult.directoryOffset, directoryBytes,
                stubNames.get(), summary.assetCount,
                techCache.get(), strict, &deferredAlias,
                deferredAliasName, sizeof(deferredAliasName));
            SyncReaderToSession(&session, reader, 4);
            if (!materialLoaded && deferredAlias)
            {
                // A comma-prefixed required material is a directory alias,
                // not an asset body.  Do not count an empty XAssetHeader as a
                // successful material registration; the owner zone will
                // register the real body and deferred consumers resolve it.
                // A stub whose owner lives outside the loaded closure
                // (RetailIsNonSpMaterialAlias) records the same slot with a
                // null-bind marker instead: the original's
                // DB_FindXAssetHeader would return the type default entry,
                // and this port binds consumers null rather than
                // manufacturing that default body or leaving a deferral that
                // can never resolve.
                const bool nonSpAlias = RetailIsNonSpMaterialAlias(deferredAliasName);
                if (!nonSpAlias)
                {
                    if (!RetailDeferMaterialAlias(nullptr, deferredAliasName,
                                                  session.zoneIndex))
                        break;
                }
                else
                {
                    Com_Printf(8,
                               "RetailWalkLoadZoneAssets: non-SP material alias '%s' ignored (directory-only stub)\n",
                               deferredAliasName);
                }
                // The comma-stub directory slot is the identity a later
                // consumer alias (ItemDef/menuDef background) points at; the
                // original's DB_AddXAsset patch would have filled it through
                // DB_FindXAssetHeader(cleanName). Record it so that consumer
                // resolves the same way (defer to the owner, or bind null for
                // a non-SP alias) instead of failing or substituting a
                // synthesized body.
                if (!RetailWorldRecordDeferredSlotName(
                        xmodelLoaderContext.world.get(),
                        directoryResult.directoryOffset + ordinal * 8u + 4u,
                        ASSET_TYPE_MATERIAL, deferredAliasName, nonSpAlias))
                {
                    Com_Printf(0, "RetailWalkLoadZoneAssets: Material ord=%u deferred "
                                  "slot ledger full\n", ordinal);
                    break;
                }
                continue;
            }
            if (!materialLoaded)
                break;
            ++summary.registeredMaterialCount;
            // Record the directory header slot so a weapon/BSP slot alias to
            // this material resolves through the zone-scoped nested table.
            if (!RetailWorldRecordZoneSlot(xmodelLoaderContext.world.get(),
                                           directoryResult.directoryOffset + ordinal * 8u + 4u,
                                           ASSET_TYPE_MATERIAL, registered))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: Material ord=%u slot ledger full\n",
                           ordinal);
                break;
            }
            if (!RetailWorldRecordBody(xmodelLoaderContext.world.get(), bodyOffset,
                                       ASSET_TYPE_MATERIAL, registered))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: Material ord=%u body ledger full\n",
                           ordinal);
                break;
            }
            if (registered.material && registered.material->info.name && stubStorage && stubNames)
            {
                char *row = stubStorage.get() + static_cast<std::size_t>(ordinal) * 64;
                std::snprintf(row, 64, "%s", registered.material->info.name);
                stubNames[ordinal] = row;
            }
            continue;
        }

        // XModel roots are temp-block (block 0) bodies.  Dispatch through the
        // real session loader so inline/insert identities enter the pooled
        // typed-handle table; aliases remain ordinary directory references.
        if ((header == kInlineReference || header == kInsertReference) &&
            type == ASSET_TYPE_XMODEL)
        {
            SyncSessionToReader(&session, reader, 4);
            XAssetHeader registered{};
            uint32_t handle = 0;
            xmodelLoaderContext.lastInsertSlotOffset = 0;
            const RetailZoneAssetResult loadResult = RetailZoneLoadSessionDispatchAsset(
                &session, ASSET_TYPE_XMODEL, header, &registered, &handle);
            if (loadResult != RETAIL_ZONE_ASSET_OK || !registered.model)
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: XModel ord=%u failed: %s\n",
                           ordinal, RetailZoneAssetResultName(loadResult));
                break;
            }
            SyncReaderToSession(&session, reader, 4);
            ++summary.registeredXModelCount;
            // Record the directory entry's own header slot (8-byte records,
            // header ref at +4) so a later draw-inst slot can alias this
            // model by raw DB_ConvertOffsetToAlias indirection onto it.
            if (!RetailWorldRecordZoneSlot(xmodelLoaderContext.world.get(),
                                           directoryResult.directoryOffset + ordinal * 8u + 4u,
                                           ASSET_TYPE_XMODEL, registered))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: XModel ord=%u slot ledger full\n",
                           ordinal);
                break;
            }
            // The loader reserved the insert form's DB_InsertPointer slot
            // before streaming the body; record it now with the canonical
            // registered header (an alias onto that slot must bind exactly
            // what the original wrote there).
            if (xmodelLoaderContext.lastInsertSlotOffset &&
                !RetailWorldRecordZoneSlot(xmodelLoaderContext.world.get(),
                                           xmodelLoaderContext.lastInsertSlotOffset,
                                           ASSET_TYPE_XMODEL, registered))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: XModel ord=%u insert slot ledger full\n",
                           ordinal);
                break;
            }
            if (registered.model->name && stubStorage && stubNames)
            {
                char *row = stubStorage.get() + static_cast<std::size_t>(ordinal) * 64;
                std::snprintf(row, 64, "%s", registered.model->name);
                stubNames[ordinal] = row;
            }
            continue;
        }

        // B6 FxEffectDef identity: inline and insert roots widen
        // through the real decoder and register through Load_FxEffectDefAsset,
        // then the directory header slot (and an insert's reserved
        // DB_InsertPointer slot) is recorded so weapon/effect alias references
        // (Load_FxEffectDefHandle's DB_ConvertOffsetToAlias) resolve to the
        // registered effect exactly like XModel draw-inst slots do.
        if ((header == kInlineReference || header == kInsertReference) &&
            type == ASSET_TYPE_FX)
        {
            SyncSessionToReader(&session, reader, 4);
            uint32_t insertSlot = 0;
            if (header == kInsertReference)
            {
                uint8_t *slot = RetailWireBlocksAlloc(&session.wire, 4, 4, 4);
                if (!slot)
                    break;
                insertSlot = static_cast<uint32_t>(slot - session.zoneMemory->blocks[4].data);
            }
            XAssetHeader registered{};
            uint32_t fxDeferred = 0;
            if (!RetailWalkLiveLoadFxEffectDef(&session, reader, xmodelLoaderContext.world.get(),
                                               &registered, &records[ordinal], &fxDeferred,
                                               insertSlot))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: FX ord=%u failed\n", ordinal);
                break;
            }
            SyncReaderToSession(&session, reader, 4);
            ++summary.registeredFxCount;
            summary.fxDeferredNestedCount += fxDeferred;
            if (!RetailWorldRecordZoneSlot(xmodelLoaderContext.world.get(),
                                           directoryResult.directoryOffset + ordinal * 8u + 4u,
                                           ASSET_TYPE_FX, registered))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: FX ord=%u slot ledger full\n",
                           ordinal);
                break;
            }
            continue;
        }

        // B6 ImpactFx identity: inline and insert roots widen
        // through the real decoder -- every non-null cell of the 12x33 effect
        // grid resolves through the zone nested table / live FX widener --
        // and register through Load_FxImpactTableAsset under the wire name
        // CG_RegisterImpactEffects_FastFile looks up.
        if ((header == kInlineReference || header == kInsertReference) &&
            type == ASSET_TYPE_IMPACT_FX)
        {
            SyncSessionToReader(&session, reader, 4);
            uint32_t insertSlot = 0;
            if (header == kInsertReference)
            {
                uint8_t *slot = RetailWireBlocksAlloc(&session.wire, 4, 4, 4);
                if (!slot)
                    break;
                insertSlot = static_cast<uint32_t>(slot - session.zoneMemory->blocks[4].data);
            }
            XAssetHeader registered{};
            if (!RetailWalkLiveLoadFxImpactTable(&session, reader,
                                                 xmodelLoaderContext.world.get(), &registered))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: ImpactFx ord=%u failed\n", ordinal);
                break;
            }
            SyncReaderToSession(&session, reader, 4);
            ++summary.registeredImpactFxCount;
            if (!RetailWorldRecordZoneSlot(xmodelLoaderContext.world.get(),
                                           directoryResult.directoryOffset + ordinal * 8u + 4u,
                                           ASSET_TYPE_IMPACT_FX, registered))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: ImpactFx ord=%u slot ledger full\n",
                           ordinal);
                break;
            }
            if (insertSlot &&
                !RetailWorldRecordZoneSlot(xmodelLoaderContext.world.get(), insertSlot,
                                           ASSET_TYPE_IMPACT_FX, registered))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: ImpactFx ord=%u insert ledger full\n",
                           ordinal);
                break;
            }
            continue;
        }

        // B2 snd_alias_list_t identity: inline and insert roots
        // widen through the real decoder and register through
        // Load_snd_alias_list_Asset, then the directory header slot (and an
        // insert's reserved DB_InsertPointer slot) is recorded so weapon sound
        // references resolve to the registered list.
        if ((header == kInlineReference || header == kInsertReference) &&
            type == ASSET_TYPE_SOUND)
        {
            SyncSessionToReader(&session, reader, 4);
            uint32_t insertSlot = 0;
            if (header == kInsertReference)
            {
                uint8_t *slot = RetailWireBlocksAlloc(&session.wire, 4, 4, 4);
                if (!slot)
                    break;
                insertSlot = static_cast<uint32_t>(slot - session.zoneMemory->blocks[4].data);
            }
            XAssetHeader registered{};
            uint32_t soundDeferred = 0;
            uint32_t nestedLoadedSounds = 0;
            uint32_t nestedSndCurves = 0;
            RetailSoundDeferredBreakdown soundBreakdown{};
            if (!RetailWalkLiveLoadSndAliasList(&session, reader,
                                                xmodelLoaderContext.world.get(),
                                                &registered, &records[ordinal],
                                                &soundDeferred, insertSlot,
                                                &nestedLoadedSounds, &nestedSndCurves,
                                                &soundBreakdown))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: Sound ord=%u failed\n", ordinal);
                break;
            }
            SyncReaderToSession(&session, reader, 4);
            ++summary.registeredSoundCount;
            summary.soundDeferredNestedCount += soundDeferred;
            summary.soundDeferredNameBlock0Count += soundBreakdown.nameBlock0;
            summary.soundDeferredNameOtherBlockCount += soundBreakdown.nameOtherBlock;
            summary.soundDeferredNamePoolCount += soundBreakdown.namePool;
            summary.soundDeferredNameUnclassifiedCount += soundBreakdown.nameUnclassified;
            summary.soundDeferredSoundFileAliasCount += soundBreakdown.soundFileAlias;
            summary.soundDeferredCurveAliasCount += soundBreakdown.curveAlias;
            summary.soundDeferredSpeakerMapAliasCount += soundBreakdown.speakerMapAlias;
            summary.soundDeferredHeadAliasCount += soundBreakdown.headAlias;
            summary.registeredNestedLoadedSoundCount += nestedLoadedSounds;
            summary.registeredNestedSndCurveCount += nestedSndCurves;
            if (!RetailWorldRecordZoneSlot(xmodelLoaderContext.world.get(),
                                           directoryResult.directoryOffset + ordinal * 8u + 4u,
                                           ASSET_TYPE_SOUND, registered))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: Sound ord=%u slot ledger full\n",
                           ordinal);
                break;
            }
            continue;
        }

        // B3 XAnimParts widening: inline bodies widen through
        // the real decoder and register through Load_XAnimPartsAsset, in
        // both bounded and full loads -- the bounded spmap script chain
        // reaches Scr_PrecacheAnimTrees, which resolves every anim through
        // DB_FindXAssetHeader, so walking XAnim deferred would keep killing
        // the run at the generic_talker_allies DROP.
        if (header == kInlineReference && type == ASSET_TYPE_XANIMPARTS)
        {
            SyncSessionToReader(&session, reader, 4);
            XAssetHeader registered{};
            if (!RetailWalkLiveLoadXAnimParts(&session, reader, header, &registered))
                break;
            SyncReaderToSession(&session, reader, 4);
            ++summary.registeredXAnimCount;
            continue;
        }

        // B7 WeaponDef widening: inline bodies widen through
        // the real decoder and register through Load_WeaponDefAsset, in
        // both bounded and full loads -- G_RegisterWeapon resolves every
        // weapon through DB_FindXAssetHeader during server init on any
        // spmap path, so walking weapons deferred kept killing the run at
        // G_SetupWeaponDef's defaultweapon lookup. References whose owner
        // lives in a later slice (XModel/FX/Sound assets, script-table
        // gaps) mark the body walked-deferred with a loud audit line
        // instead of aborting the zone (F5 contract): the fused pass
        // already accounted every byte, so the zone continues and
        // consumers of the missing asset still fail loudly by name.
        if (header == kInlineReference && type == ASSET_TYPE_WEAPON)
        {
            SyncSessionToReader(&session, reader, 4);
            XAssetHeader registered{};
            const RetailWeaponDecodeResult weaponResult =
                RetailWalkLiveLoadWeaponDef(&session, reader, xmodelLoaderContext.world.get(),
                                            header, &registered, &records[ordinal],
                                            &directoryResult);
            if (weaponResult == RETAIL_WEAPON_DECODE_OK)
            {
                SyncReaderToSession(&session, reader, 4);
                ++summary.registeredWeaponCount;
                continue;
            }
            if (weaponResult == RETAIL_WEAPON_DECODE_UNRESOLVED_REFERENCE ||
                weaponResult == RETAIL_WEAPON_DECODE_SCRIPT_STRING_MISSING ||
                weaponResult == RETAIL_WEAPON_DECODE_INDEX_OOB ||
                weaponResult == RETAIL_WEAPON_DECODE_BAD_NAME)
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: weapon deferred ord=%u reason=%s\n",
                           ordinal, RetailWeaponDecodeResultName(weaponResult));
                records[ordinal].state = RETAIL_WALK_WALKED_DEFERRED;
                ++summary.walkedOnlyCount;
                continue;
            }
            break;
        }

        // MenuList bridges reader's block 4 into session->zoneMemory (see
        // db_retail_decode_menulist.h) rather than resolving purely against
        // reader like the three asset types above. Bounded mode walks it
        // instead: menu registration is not part of the first frame.
        if (!bounded && header == kInlineReference && type == ASSET_TYPE_MENULIST)
        {
            SyncSessionToReader(&session, reader, 4);
            XAssetHeader registered{};
            if (!RetailWalkLiveLoadMenuList(&session, reader, header, &registered,
                                            directoryResult.directoryOffset, directoryBytes,
                                            stubNames.get(), summary.assetCount,
                                            xmodelLoaderContext.world.get()))
                break;
            SyncReaderToSession(&session, reader, 4);
            ++summary.registeredMenuListCount;
            continue;
        }

        // GfxWorld widens the registry-facing world shape from the live
        // stream into a transaction and commits it into s_world (see
        // db_retail_decode_world.h). Reader-addressed like TechniqueSet/
        // Material/Image above; the world root itself streams through the
        // session temp block like every other inline body.
        if (header == kInlineReference && type == ASSET_TYPE_GFXWORLD)
        {
            SyncSessionToReader(&session, reader, 4);
            XAssetHeader registered{};
            uint32_t handle = 0;
            if (!RetailWalkLiveLoadGfxWorld(&session, reader, &registered, &handle,
                                            directoryResult.directoryOffset, directoryBytes,
                                            stubNames.get(), summary.assetCount, techCache.get(),
                                            xmodelLoaderContext.world.get()))
                break;
            SyncReaderToSession(&session, reader, 4);
            ++summary.registeredGfxWorldCount;
            continue;
        }

        // Native ClipMap registry: the map's collision body
        // widens field by field into the engine's own cm singleton through
        // RetailWalkLiveLoadClipMap (db_retail_decode_clipmap.{h,cpp}),
        // committing atomically with the returned pooled header required
        // to equal &cm. The nested MapEnts leaf widens and registers inside
        // that same driver (inline entity bytes, inline-or-alias name),
        // retiring the former walk-only capture bridge: cm.mapEnts points
        // at the registered asset, which the script probe and
        // G_ParseSpawnVars consume by name. Both policies: entity scripts
        // and SV_SetBrushModel/SV_LinkEntity need cm live on any spmap
        // path. SV_LinkEntity's consumers (CM_BoxLeafnums/CM_LinkEntity)
        // read the widened planes/nodes/leafs/cmodels; XModel-bearing
        // static-model slots widen through the shared zone XModel context.
        if (header == kInlineReference && type == ASSET_TYPE_CLIPMAP)
        {
            SyncSessionToReader(&session, reader, 4);
            XAssetHeader registered{};
            if (!RetailWalkLiveLoadClipMap(&session, reader, header, &registered,
                                           xmodelLoaderContext.world.get()))
                break;
            SyncReaderToSession(&session, reader, 4);
            ++summary.registeredClipMapCount;
            continue;
        }

        // B1 small roots: PhysPreset and SndCurve widen through
        // their real decoders and register through their engine owners, both
        // for inline and insert headers, with the declared directory slot
        // (and an insert's reserved slot) recorded for later aliases.
        if ((header == kInlineReference || header == kInsertReference) &&
            (type == ASSET_TYPE_PHYSPRESET || type == ASSET_TYPE_SOUND_CURVE))
        {
            SyncSessionToReader(&session, reader, 4);
            uint32_t insertSlot = 0;
            if (header == kInsertReference)
            {
                uint8_t *slot = RetailWireBlocksAlloc(&session.wire, 4, 4, 4);
                if (!slot)
                    break;
                insertSlot = static_cast<uint32_t>(slot - session.zoneMemory->blocks[4].data);
            }
            XAssetHeader registered{};
            const bool smallOk = type == ASSET_TYPE_PHYSPRESET
                                     ? RetailWalkLiveLoadPhysPreset(&session, reader, &registered)
                                     : RetailWalkLiveLoadSndCurve(&session, reader, &registered);
            if (!smallOk)
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: type=%u ord=%u failed\n", type, ordinal);
                break;
            }
            SyncReaderToSession(&session, reader, 4);
            if (type == ASSET_TYPE_PHYSPRESET)
                ++summary.registeredPhysPresetCount;
            else
                ++summary.registeredSndCurveCount;
            if (!RetailWorldRecordZoneSlot(xmodelLoaderContext.world.get(),
                                           directoryResult.directoryOffset + ordinal * 8u + 4u,
                                           static_cast<XAssetType>(type), registered))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: type=%u ord=%u slot ledger full\n",
                           type, ordinal);
                break;
            }
            if (insertSlot &&
                !RetailWorldRecordZoneSlot(xmodelLoaderContext.world.get(), insertSlot,
                                           static_cast<XAssetType>(type), registered))
            {
                Com_Printf(0, "RetailWalkLoadZoneAssets: type=%u ord=%u insert ledger full\n",
                           type, ordinal);
                break;
            }
            continue;
        }

        SyncSessionToReader(&session, reader, 4);
        if (header == kInsertReference &&
            !RetailWireBlocksAlloc(&session.wire, 4, 4, 4))
            break;
        SyncSessionToReader(&session, reader, 4);
        RetailWalkTempScope scope;
        if (!RetailWalkBeginTemp(&session, &scope))
            break;
        const bool ok = RetailWalkDispatchAssetBody(&session, reader, type,
                                                    &records[ordinal], &directoryResult);
        RetailWalkEndTemp(&session, &scope);
        if (!ok)
            break;
        ++summary.walkedOnlyCount;
    }
    const int loopEndMs = static_cast<int>(Sys_Milliseconds());
    if (ordinal < summary.assetCount)
    {
        Com_Printf(0, "RetailWalkLoadZoneAssets: FAILED at ord=%u/%u type=%u header=0x%08x state=%d\n",
                   ordinal, summary.assetCount, records[ordinal].type, records[ordinal].header, records[ordinal].state);
        Com_Printf(0, "RetailWalkLoadZoneAssets: arena used=%zu peak=%zu size=%zu\n",
                   session.arena.used, session.arena.peak, session.arena.size);
        summary.failedOrdinal = ordinal;
        summary.failedType = records[ordinal].type;
        delete[] assets;
        RetailZoneLoadSessionAbort(&session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_LOAD_UNSUPPORTED_ASSET;
        *result = summary;
        return summary.code;
    }

    // Success: the zone stays live in the real registry (DB_RetailZoneBegin's
    // slot) for the rest of the process, exactly like a normal
    // DB_TryLoadXFileInternal load.  Only the bounded loading scaffolding
    // (reader, directory scratch, alias handle table) is torn down here.
    Com_Printf(0, "RetailWalkLoadZoneAssets: SUCCESS loaded all %u assets for %s\n", summary.assetCount, filename);
    Com_Printf(0, "RetailWalkLoadZoneAssets: arena used=%zu peak=%zu size=%zu\n",
               session.arena.used, session.arena.peak, session.arena.size);
    // Forward texture-slot image bindings first (materials name images whose
    // declarer widens later in the same zone -- e.g. a GfxWorld-embedded
    // material aliasing a top-level material's texture-def slot); the
    // mirror and the image pool are whole now. Technique aliases last;
    // neither pass depends on the other.
    if (techCache && techCache->deferredImageCount > 0 &&
        !RetailWireImageCacheResolveDeferred(&session, techCache.get(), reader,
                                             directoryResult.directoryOffset, directoryBytes,
                                             stubNames.get(), summary.assetCount))
    {
        Com_Printf(0, "RetailWalkLoadZoneAssets: deferred image aliases unresolved\n");
        delete[] assets;
        RetailZoneLoadSessionAbort(&session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_LOAD_UNSUPPORTED_ASSET;
        *result = summary;
        return summary.code;
    }
    if (techCache && techCache->deferredCount > 0 &&
        !RetailWireTechniqueCacheResolveDeferred(techCache.get(), reader))
    {
        Com_Printf(0, "RetailWalkLoadZoneAssets: deferred technique aliases unresolved\n");
        delete[] assets;
        RetailZoneLoadSessionAbort(&session);
        FS_CloseRetailFastfile(reader);
        summary.code = RETAIL_WALK_LOAD_UNSUPPORTED_ASSET;
        *result = summary;
        return summary.code;
    }
    // Every asset in this zone has now streamed whatever block-7 (vertex)
    // and block-8 (index) bytes it owns into the zone's CPU blocks. Publish
    // them to the zone's D3D geometry buffers and unlock those buffers, the
    // same thing DB_LoadXFileInternal does via DB_CloneStreamData +
    // DB_FinishGeometryBlocks. DB_GetVertexBufferAndOffset /
    // DB_GetIndexBufferAndBase (db_registry.cpp) hand exactly these buffers
    // to the static-model draw path, which derives its stream offset and
    // base index by subtracting blocks[7]/blocks[8].data from the XSurface's
    // own CPU pointers -- so the GPU-side bytes have to match the CPU block
    // contents exactly, and the buffer must not still be mapped at draw time.
    RetailZoneLoadSessionPublishGeometry(&session);
    // A core zone can carry comma aliases before the map-specific owner is
    // loaded. Re-run the real registry lookup after each successful zone so
    // consumers are patched to the owner material as soon as its body and
    // image are registered; unresolved owners stay visible, never default.
    RetailResolveDeferredMaterialAliases(false);
    // a weapon body can name a snd_alias_list_t that streams later in
    // this zone (killhouse) or in a zone that loads later (common -> killhouse
    // turret sounds). Those slots were recorded instead of binding the engine
    // default; every zone loaded so far has now registered its sounds, so
    // re-run the real lookup and bind the real lists. Outstanding entries stay
    // visible (never defaulted) and the P6 verifier requires zero after the
    // final zone load.
    RetailResolveDeferredWeaponSounds();
    // Measured closure fact: a full-policy load that still walked bodies
    // instead of registering them, or that still holds an unresolved
    // comma-material alias, is a partial zone load.  `walked_only` is the
    // loader's own classification count; the P6 acceptance verifier requires
    // both fields to be zero for the full killhouse zone.
    Com_Printf(0, "KILLHOUSE_ZONE_CLOSURE ff=%s assets=%u walked_only=%u alias_outstanding=%u "
                  "nested_loadedsounds=%u nested_sndcurves=%u sounds_deferred=%u "
                  "snd_name=%u/%u/%u/%u snd_alias=%u/%u/%u/%u\n",
               filename, summary.assetCount, summary.walkedOnlyCount,
               RetailMaterialDeferredAliasCount(),
               summary.registeredNestedLoadedSoundCount,
               summary.registeredNestedSndCurveCount,
               summary.soundDeferredNestedCount,
               summary.soundDeferredNameBlock0Count,
               summary.soundDeferredNameOtherBlockCount,
               summary.soundDeferredNamePoolCount,
               summary.soundDeferredNameUnclassifiedCount,
               summary.soundDeferredSoundFileAliasCount,
               summary.soundDeferredCurveAliasCount,
               summary.soundDeferredSpeakerMapAliasCount,
               summary.soundDeferredHeadAliasCount);
    {
        // Load-phase decode profile: where the walk's wall time went. The
        // wire bytes make reader restarts visible (a total far above the
        // xfile size means the same zone stream was inflated more than once).
        Com_Printf(0, "KILLHOUSE_LOAD_DECODE zone=%s open_ms=%d loop_ms=%d post_ms=%d wire_bytes=%llu xfile_bytes=%u\n",
                   filename, openEndMs - walkStartMs, loopEndMs - loopStartMs,
                   static_cast<int>(Sys_Milliseconds()) - loopEndMs,
                   static_cast<unsigned long long>(FS_RetailFastfileWireBytes(reader)),
                   FS_RetailFastfileXFileSize(reader));
        uint32_t order[ASSET_TYPE_COUNT];
        uint32_t orderCount = 0;
        for (uint32_t t = 0; t < ASSET_TYPE_COUNT; ++t)
        {
            if (typeDecodeCount[t])
                order[orderCount++] = t;
        }
        for (uint32_t i = 1; i < orderCount; ++i)
        {
            const uint32_t key = order[i];
            uint32_t j = i;
            while (j > 0 && typeDecodeMs[order[j - 1]] < typeDecodeMs[key])
            {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = key;
        }
        for (uint32_t i = 0; i < orderCount; ++i)
        {
            const uint32_t t = order[i];
            Com_Printf(0, "KILLHOUSE_LOAD_DECODE_TYPE zone=%s type=%s count=%u ms=%llu\n",
                       filename, DB_GetXAssetTypeName(t), typeDecodeCount[t],
                       static_cast<unsigned long long>(typeDecodeMs[t]));
        }
    }
    for (uint32_t block = 0; block < 9; ++block)
        summary.endCursor[block] = session.wire.cursor[block];
    delete[] assets;
    // The alias handle table is in-walk scaffolding: an entry can hold another
    // zone's canonical body (an override), and that owner may unload while
    // this zone stays live, freeing the arena the entry points into.  Zero it
    // before the reader closes so no live zone arena retains a cross-zone
    // pointer that a later heap reuse can alias into a reader's transient
    // block (the close-time provenance scan flags exactly that shape).
    RetailZoneLoadSessionReleaseHandles(&session);
    FS_CloseRetailFastfile(reader);
    // Every arena allocation of this zone has happened: return the unused
    // tail to PMem and pin the arena at what was kept, so a stray later
    // allocation fails loudly instead of overlapping the next zone.
    {
        const std::size_t reserved = session.arena.size;
        const std::size_t keep = (session.arena.used + 15u) & ~static_cast<std::size_t>(15u);
        const bool trimmed = keep <= reserved &&
            DB_RetailZoneTrimNativeArena(session.zoneIndex, session.arena.data,
                                         static_cast<uint32_t>(reserved),
                                         static_cast<uint32_t>(keep));
        if (trimmed)
            session.arena.size = keep;
        Com_Printf(0, "RetailWalkLoadZoneAssets: native arena %s reserved=%zu kept=%zu\n",
                   trimmed ? "trimmed" : "NOT trimmed (not the last PMem allocation)",
                   reserved, trimmed ? keep : reserved);
    }
    summary.code = RETAIL_WALK_LOAD_OK;
    // Record the policy this resident zone was loaded under so a later
    // same-file request reloads instead of skipping when the policies
    // differ (bounded-partial must never satisfy a full request).
    DB_RetailZoneNoteLoadPolicy(session.zoneIndex, bounded);
    summary.nullTechniqueNameCount = techCache ? techCache->nullTechniqueNames : 0;
    summary.nullShaderNameCount = techCache ? techCache->nullShaderNames : 0;
    summary.deferredTechniqueAliasCount = techCache ? techCache->deferredCount : 0;
    summary.resolvedTechniqueAliasCount = techCache ? techCache->deferredResolvedCount : 0;
    summary.deferredSubAliasCount = techCache ? techCache->deferredSubCount : 0;
    summary.resolvedSubAliasCount = techCache ? techCache->deferredSubResolvedCount : 0;
    *result = summary;
    return summary.code;
}
