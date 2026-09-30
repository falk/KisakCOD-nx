#include "db_retail_decode_world.h"
#include "../universal/retail_asset_trace.h"

#include "db_retail_decode_image.h"
#include "db_retail_decode_material.h"
#include "db_retail_decode_rawfile.h"
#include "db_retail_walk.h"
#include "db_retail_wire.h"
#include "../gfx_d3d/r_bsp.h"
#include "../gfx_d3d/r_primarylights.h"
#include "../qcommon/com_bsp.h"
#include "../xanim/xanim.h"
#include "../script/scr_stringlist.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <memory>
#include <algorithm>

namespace
{
constexpr uint32_t kInlineRef = 0xffffffffu;
constexpr uint32_t kInsertRef = 0xfffffffeu;

bool DecodeFastfileToken(FsRetailFastfileReader *reader, uint32_t encoded, uint32_t span,
                         uint32_t allowedBlockMask, RetailWireToken *token)
{
    if (!reader || !token)
        return false;
    XBlock blocks[9]{};
    for (uint32_t block = 0; block < 9; ++block)
        blocks[block] = {const_cast<uint8_t *>(FS_RetailFastfileBlockData(reader, block)),
                         FS_RetailFastfileBlockSize(reader, block)};
    return RetailWireTokenDecodeBlocks(blocks, {encoded}, span, allowedBlockMask, token);
}

// Read the exact name bytes at an already-streamed block-4 offset (used
// for alias resolution: the alias names the offset, the registry binds the
// name). Printable-ASCII only, like the strict decoders -- a block-4
// back-reference into the linker's unmirrored layout lands on unwritten
// memory and must fail, never register garbage.
bool ReadBlockNameAt(FsRetailFastfileReader *reader, uint32_t offset, char *buffer,
                     uint32_t bufferSize)
{
    if (!reader || !buffer || bufferSize == 0)
        return false;
    // Exact-span read: request only the bytes actually mirrored
    // (offset..cursor), never a full buffer -- a name near the end of a
    // block made the old full-buffer read fail (or read unwritten mirror
    // bytes) even when the string itself was fully present.
    const uint32_t cursor = FS_RetailFastfileBlockCursor(reader, 4);
    if (offset >= cursor)
        return false;
    const uint32_t available = cursor - offset;
    const uint32_t readBytes = available < bufferSize - 1 ? available : bufferSize - 1;
    if (FS_ReadRetailFastfileBlock(reader, 4, offset,
                                   reinterpret_cast<uint8_t *>(buffer),
                                   readBytes) != FS_RETAIL_FF_WIRE_OK)
        return false;
    // The name must end inside the bytes read: a name longer than the buffer
    // would otherwise be truncated into another asset's identity, and one
    // running into the unwritten mirror past the cursor would leave the rest
    // of the buffer uninitialized. Both fail loudly.
    if (!std::memchr(buffer, 0, readBytes))
    {
        Com_Printf(0, "ReadBlockNameAt: name at offset=%u is unterminated or longer than %u bytes\n",
                   offset, bufferSize - 1);
        return false;
    }
    buffer[bufferSize - 1] = '\0';
    if (!buffer[0])
        return false;
    for (const char *p = buffer; *p; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c >= 0x7f)
            return false;
    }
    return true;
}

namespace
{
uint32_t WorldIndexHash(uint32_t offset, uint32_t kind, uint32_t mask)
{
    uint32_t hash = offset * 2654435761u;
    hash ^= (kind + 1u) * 2246822519u;
    hash ^= hash >> 13;
    return hash & mask;
}

// Insert/overwrite `valueIndex` for the key (offset, kind). `matches` tests a
// stored entry against that key; an overwrite keeps the newest entry for a
// repeated key, which is what the old youngest-first scans returned.
template <typename Entry, typename MatchesFn>
void WorldIndexPut(const Entry *entries, uint32_t *table, uint32_t mask, uint32_t offset,
                   uint32_t kind, uint32_t valueIndex, MatchesFn matches)
{
    uint32_t slot = WorldIndexHash(offset, kind, mask);
    for (;;)
    {
        const uint32_t stored = table[slot];
        if (stored == 0 || matches(entries[stored - 1]))
        {
            table[slot] = valueIndex + 1u;
            return;
        }
        slot = (slot + 1u) & mask;
    }
}

template <typename Entry, typename MatchesFn>
const Entry *WorldIndexFind(const Entry *entries, const uint32_t *table, uint32_t mask,
                            uint32_t offset, uint32_t kind, MatchesFn matches)
{
    uint32_t slot = WorldIndexHash(offset, kind, mask);
    for (;;)
    {
        const uint32_t stored = table[slot];
        if (stored == 0)
            return nullptr;
        const Entry &entry = entries[stored - 1];
        if (matches(entry))
            return &entry;
        slot = (slot + 1u) & mask;
    }
}

} // namespace

bool RecordNestedName(RetailWorldLoadContext *context, uint32_t nameRef, XAssetType type,
                      XAssetHeader header, uint32_t slotOffset = 0,
                      uint32_t bodyOffset = 0)
{
    if (!context || !header.data)
    {
        Com_Printf(0, "DIAG_RecordNestedName bad ctx=%d data=%d nameRef=0x%08x\n",
                   context ? 1 : 0, header.data ? 1 : 0, nameRef);
        return false;
    }
    RetailWireToken token{};
    uint32_t nameOffset = 0;
    if (nameRef)
    {
        if (!DecodeFastfileToken(context->reader, nameRef, 0, 1u << 4, &token) ||
            token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
        {
            Com_Printf(0, "DIAG_RecordNestedName decode failed nameRef=0x%08x kind=%d block=%d\n",
                       nameRef, (int)token.kind, (int)token.block);
            return false;
        }
        nameOffset = token.offset;
    }
    if (context->nestedCount >= RetailWorldLoadContext::kNestedCap)
    {
        Com_Printf(0, "DIAG_RecordNestedName cap nestedCount=%u nameRef=0x%08x\n",
                   context->nestedCount, nameRef);
        return false;
    }
    const uint32_t index = context->nestedCount;
    RetailWorldNestedName &entry = context->nested[context->nestedCount++];
    entry.bodyOffset = bodyOffset;
    entry.nameOffset = nameOffset;
    entry.slotOffset = slotOffset;
    entry.type = type;
    entry.header = header;
    if (slotOffset)
    {
        WorldIndexPut(context->nested, context->nestedSlotIndex,
                      RetailWorldLoadContext::kNestedIndexMask, slotOffset,
                      static_cast<uint32_t>(type), index,
                      [&](const RetailWorldNestedName &stored)
                      { return stored.slotOffset == slotOffset && stored.type == type; });
    }
    if (bodyOffset)
    {
        WorldIndexPut(context->nested, context->nestedBodyIndex,
                      RetailWorldLoadContext::kNestedIndexMask, bodyOffset,
                      static_cast<uint32_t>(type), index,
                      [&](const RetailWorldNestedName &stored)
                      { return stored.bodyOffset == bodyOffset && stored.type == type; });
    }
    return true;
}

// Record a widened result addressable only by its slot (a tolerated
// null bind, or a null-named body an alias can still reach by slot
// indirection). nameRef-less counterpart of RecordNestedName.
bool RecordNestedSlot(RetailWorldLoadContext *context, uint32_t slotOffset,
                      XAssetType type, XAssetHeader header)
{
    if (!context || !slotOffset || context->nestedCount >= RetailWorldLoadContext::kNestedCap)
        return false;
    const uint32_t index = context->nestedCount;
    RetailWorldNestedName &entry = context->nested[context->nestedCount++];
    entry.bodyOffset = 0;
    entry.nameOffset = 0;
    entry.slotOffset = slotOffset;
    entry.type = type;
    entry.header = header;
    WorldIndexPut(context->nested, context->nestedSlotIndex,
                  RetailWorldLoadContext::kNestedIndexMask, slotOffset,
                  static_cast<uint32_t>(type), index,
                  [&](const RetailWorldNestedName &stored)
                  { return stored.slotOffset == slotOffset && stored.type == type; });
    return true;
}

bool FindNestedSlot(RetailWorldLoadContext *context, uint32_t slotOffset,
                    XAssetType type, XAssetHeader *header)
{
    if (!context || !slotOffset || !header)
        return false;
    const RetailWorldNestedName *entry = WorldIndexFind(
        context->nested, context->nestedSlotIndex, RetailWorldLoadContext::kNestedIndexMask,
        slotOffset, static_cast<uint32_t>(type),
        [&](const RetailWorldNestedName &stored)
        { return stored.slotOffset == slotOffset && stored.type == type; });
    if (!entry)
        return false;
    *header = entry->header;
    return true;
}

bool FindNestedBody(RetailWorldLoadContext *context, uint32_t bodyOffset,
                    XAssetType type, XAssetHeader *header)
{
    if (!context || !bodyOffset || !header)
        return false;
    const RetailWorldNestedName *entry = WorldIndexFind(
        context->nested, context->nestedBodyIndex, RetailWorldLoadContext::kNestedIndexMask,
        bodyOffset, static_cast<uint32_t>(type),
        [&](const RetailWorldNestedName &stored)
        { return stored.bodyOffset == bodyOffset && stored.type == type; });
    if (!entry)
        return false;
    *header = entry->header;
    return true;
}

bool ReadBlock4Dword(FsRetailFastfileReader *reader, uint32_t offset, uint32_t *value)
{
    if (!reader || !value)
        return false;
    return FS_ReadRetailFastfileBlock(reader, 4, offset,
                                      reinterpret_cast<uint8_t *>(value), 4) ==
           FS_RETAIL_FF_WIRE_OK;
}

// Resolve one nested alias slot. Two real retail shapes exist, both exact:
//
// 1. Slot indirection (the original DB_ConvertOffsetToAlias, db_stream_
//    load.cpp): the encoded offset addresses a 4-byte slot an earlier
//    load already filled with the loaded asset. Our mirrors keep pristine
//    wire bytes, so the loaded result is recovered from the nested table
//    keyed by slot offset; a target slot still holding inline/insert wire
//    bytes means its body was widened earlier in this same stream -- a
//    table miss there is a loud failure, never a guess. A target slot
//    holding another block-4 reference chains (bounded), matching what the
//    original read out of its patched slot. A null target slot binds null.
//
// 2. Name reference: the encoded offset addresses the asset's name string
//    itself; read that exact name and bind the registered asset of the
//    slot's type through the registry.
//
// Every step is exact; any miss fails naming the slot.
bool ResolveNestedAlias(RetailWorldLoadContext *context, uint32_t slotRef, XAssetType type,
                        XAssetHeader *header)
{
    if (!context || !context->reader || !header)
        return false;
    *header = {};
    RetailWireToken token{};
    if (!DecodeFastfileToken(context->reader, slotRef, 0, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
    {
        Com_Printf(0, "RetailWorld: bad nested alias 0x%08x\n", slotRef);
        return false;
    }
    // Shape 1: slot indirection, chained through pristine wire bytes. Ends
    // with either a resolved bind (return), a loud slot-form failure
    // (return), or a fall-through when the bytes are not slot-form at all.
    {
        uint32_t offset = token.offset;
        for (int depth = 0; depth < 8; ++depth)
        {
            XAssetHeader recorded{};
            if (FindNestedBody(context, offset, type, &recorded))
            {
                *header = recorded;
                return true;
            }
            if (FindNestedSlot(context, offset, type, &recorded))
            {
                *header = recorded;
                return true;
            }
            uint32_t target = 0;
            if (!ReadBlock4Dword(context->reader, offset, &target))
                break; // out-of-block: cannot be a slot; try the name shape
            if (!target)
            {
                // The referenced slot is a declared null slot; the alias
                // binds null exactly like the original's copied value.
                return true;
            }
            if (target == kInlineRef || target == kInsertRef)
            {
                uint32_t near[4]{};
                for (uint32_t i = 0; i < 4; ++i)
                    ReadBlock4Dword(context->reader, offset + 4u + i * 4u, &near[i]);
                Com_Printf(0, "RetailWorld: nested alias 0x%08x -> slot %u holds an inline "
                              "body with no recorded widened result zone=%s count=%u "
                              "wireCursor4=%u readerCursor4=%u near=%08x %08x %08x %08x\n",
                           slotRef, offset,
                           context->session ? context->session->zoneName : "(null)",
                           context->nestedCount,
                           context->session ? context->session->wire.cursor[4] : 0u,
                           FS_RetailFastfileBlockCursor(context->reader, 4),
                           near[0], near[1], near[2], near[3]);
                return false;
            }
            RetailWireToken chained{};
            if ((target >> 28) != 4 ||
                !DecodeFastfileToken(context->reader, target, 0, 1u << 4, &chained) ||
                chained.kind != RETAIL_WIRE_TOKEN_OFFSET || chained.block != 4)
                break; // not slot-form: fall through to the name shape
            offset = chained.offset;
        }
        if (offset != token.offset)
        {
            // The chain advanced and ended on non-slot bytes. A real retail
            // shape is a name-alias chain: the final hop addresses the
            // asset's name string (observed live: weapon material slots ->
            // pointer slot -> "hud_..." bytes). Resolve that exact name
            // through the registry; anything else stays a loud miss.
            char chainName[64]{};
            if (ReadBlockNameAt(context->reader, offset, chainName, sizeof(chainName)))
            {
                const XAssetHeader found = DB_FindXAssetHeader(type, chainName);
                if (found.data)
                {
                    *header = found;
                    return true;
                }
                Com_Printf(0, "RetailWorld: nested alias 0x%08x chain names unregistered '%s'\n",
                           slotRef, chainName);
                return false;
            }
            Com_Printf(0, "RetailWorld: nested alias 0x%08x slot chain unresolved at slot %u\n",
                       slotRef, offset);
            return false;
        }
    }
    // Shape 2: name reference.
    char name[64]{};
    if (!ReadBlockNameAt(context->reader, token.offset, name, sizeof(name)))
    {
        Com_Printf(0, "RetailWorld: nested alias 0x%08x names no readable string\n", slotRef);
        return false;
    }
    const XAssetHeader found = DB_FindXAssetHeader(type, name);
    if (!found.data)
    {
        Com_Printf(0, "RetailWorld: nested alias 0x%08x names unregistered '%s'\n", slotRef, name);
        return false;
    }
    *header = found;
    return true;
}
} // namespace

bool RetailWorldResolveNestedAlias(RetailWorldLoadContext *context, uint32_t slotRef,
                                   XAssetType type, XAssetHeader *header)
{
    return ResolveNestedAlias(context, slotRef, type, header);
}

bool RetailWorldConsume(RetailWorldLoadContext *context, void *dest, uint32_t bytes,
                        uint32_t alignment)
{
    if (!context || !context->session || !context->reader)
        return false;
    if (!bytes)
        return true;
    // Dense mirror placement: every consumed byte lands in
    // the session wire blocks at its linker offset via the ordinary
    // session stream (which also auto-syncs the reader mirror forward, so
    // reader, session, and true stream positions stay glued). The previous
    // scratch-discard form advanced the true stream without moving either
    // cursor, so every later FS-level placement landed shifted and absolute
    // (block,offset) reads -- image-alias slots, nested names, pool
    // patches -- silently hit the wrong bytes (caught live: texture-table
    // bytes 12 apart resolving into unrelated bulk data, faulting the first
    // sampled draw with null images). Bulk spans were never addressable
    // before only by luck of never being referenced; killhouse's world
    // materials reference straight into them. `alignment` must match the
    // engine's own AllocLoadStreamPos for this span (4 for the
    // FxElemVisStateSample arrays, 2 for XBlendInfo ushort arrays, 1 for
    // raw-byte arrays): without it the live mirror packed spans below their
    // linker offsets, so every later block-4 slot drifted (killhouse: the
    // draw-inst base landed 4 low and 67 static-model aliases missed).
    RetailZoneLoadSession *session = context->session;
    FsRetailFastfileReader *reader = context->reader;
    if (!RetailZoneLoadSessionReadStream(session, reader, 4, bytes, alignment))
        return false;
    // Landing cursor AFTER the stream (which heals a session lag first):
    // capturing it before would memcpy stale bytes whenever the heal fired.
    const uint32_t start = session->wire.cursor[4] - bytes;
    if (dest)
        std::memcpy(dest, session->zoneMemory->blocks[4].data + start, bytes);
    context->nestedBodyBytes += bytes;
    return true;
}

bool RetailWorldStreamName(RetailWorldLoadContext *context, uint32_t nameRef,
                           uint32_t *nameOffset, const char **name)
{
    if (!context || !context->reader || !nameOffset || !name)
        return false;
    *nameOffset = 0;
    *name = nullptr;
    if (!nameRef)
        return true;
    if (nameRef != kInlineRef)
    {
        // Shared world name: absolute block-4 back-reference into the
        // linker's image. The techset alias/name resolution across this
        // same zone proves our mirror is layout-identical, so a readable
        // ASCII name here is exact, not a guess; anything else fails.
        RetailWireToken token{};
        if (!DecodeFastfileToken(context->reader, nameRef, 0, 1u << 4, &token) ||
            token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
        {
            Com_Printf(0, "RetailWorld: bad shared world name 0x%08x\n", nameRef);
            return false;
        }
        char buffer[64]{};
        if (!ReadBlockNameAt(context->reader, token.offset, buffer, sizeof(buffer)))
        {
            Com_Printf(0, "RetailWorld: shared world name 0x%08x unreadable\n", nameRef);
            return false;
        }
        const std::size_t len = std::strlen(buffer) + 1;
        char *copy = static_cast<char *>(
            RetailZoneLoadSessionAlloc(context->session, len, 1));
        if (!copy)
            return false;
        std::memcpy(copy, buffer, len);
        *nameOffset = token.offset;
        *name = copy;
        return true;
    }
    constexpr uint32_t kMaxNameBytes = 256;
    // Inline names are stream bytes. Consume them through the session
    // stream so the session wire and the FS reader mirror stay glued:
    // the old FS-direct form left the reader cursor behind the session's
    // post-reservation cursor (an insert reservation moves the session
    // only), so the next span's heal copied the name four bytes early and
    // every following side/node/plane slot desynchronized (e.g.
    // ClipMap insert fixture's side plane resolved as a bad alias).
    const uint32_t start = context->session->wire.cursor[4];
    uint8_t *block = context->session->zoneMemory->blocks[4].data;
    const uint32_t blockSize = context->session->zoneMemory->blocks[4].size;
    if (!block)
        return false;
    for (uint32_t i = 0; i < kMaxNameBytes; ++i)
    {
        if (start + i >= blockSize)
            return false;
        if (!RetailZoneLoadSessionReadStream(context->session, context->reader, 4, 1, 1))
            return false;
        const uint8_t ch = block[start + i];
        if (!ch)
        {
            context->nestedBodyBytes += i + 1;
            if (i == 0)
            {
                Com_Printf(0, "RetailWorld: empty world name\n");
                return false;
            }
            *nameOffset = start;
            *name = reinterpret_cast<const char *>(block + start);
            return true;
        }
    }
    return false;
}

bool RetailWorldRetainSpan(RetailWorldLoadContext *context, uint32_t bytes, uint32_t alignment,
                           void **out)
{
    if (!context || !context->session || !out)
        return false;
    *out = nullptr;
    if (!bytes)
        return true;
    void *copy = RetailZoneLoadSessionAlloc(context->session, bytes, alignment);
    if (!copy)
        return false;
    std::memset(copy, 0, bytes);
    if (!RetailWorldConsume(context, copy, bytes, alignment))
        return false;
    *out = copy;
    return true;
}

bool RetailWorldRuntimeBytes(RetailWorldLoadContext *context, uint32_t slotRef, uint64_t bytes,
                             uint32_t alignment)
{
    if (!context || !context->session)
        return false;
    if (!slotRef)
        return true;
    if (!bytes || bytes > UINT32_MAX)
        return false;
    if (!RetailZoneLoadSessionExpandRuntime(context->session, 1, static_cast<uint32_t>(bytes),
                                            alignment))
        return false;
    context->block1Bytes += static_cast<uint32_t>(bytes);
    return true;
}

bool RetailWorldWidenMaterial(RetailWorldLoadContext *context, uint32_t slotRef,
                              Material **out, uint32_t slotOffset)
{
    if (!context || !context->session || !context->reader || !out)
        return false;
    *out = nullptr;
    if (!slotRef)
    {
        // A declared null slot binds null; record it so a later slot
        // indirection onto this slot binds null the same way.
        if (slotOffset)
            RecordNestedSlot(context, slotOffset, ASSET_TYPE_MATERIAL, {});
        return true;
    }
    if (slotRef != kInlineRef && slotRef != kInsertRef)
    {
        XAssetHeader found{};
        if (!ResolveNestedAlias(context, slotRef, ASSET_TYPE_MATERIAL, &found) ||
            !found.material)
            return false;
        // This slot's own resolved result must be recorded too, the same
        // way the inline/insert branch below does: a sibling slot's alias
        // can point at *this* slot (materialHandles aliases are sibling
        // indirections within the same array), and without this a slot
        // that itself holds a plain alias would leave that sibling
        // reference unresolved even though the material it needs was
        // widened successfully right here.
        if (slotOffset)
            RecordNestedSlot(context, slotOffset, ASSET_TYPE_MATERIAL, found);
        *out = found.material;
        return true;
    }
    // Insert carries an inline body (HandleAssetSlot reserves the pointer
    // slot, then streams the body); the FS reader consumes both forms.
    FsRetailFastfileMaterial wire{};
    FsRetailFastfileTextureDef textureDefs[8]{};
    const uint32_t bodyOffset = FS_RetailFastfileBlockCursor(context->reader, 4);
    const FsRetailFastfileWireResult wireRes =
        FS_ReadRetailFastfileMaterial(context->reader, slotRef, &wire, textureDefs, 8);
    if (wireRes == FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL)
    {
        // Dead inline body with garbage counts: the 80-byte root and name
        // are consumed, but the tables were not. The engine eats
        // count-driven tables verbatim, so walk-consume exactly what the
        // walk-only reader consumes (same functions it uses) and only then
        // bind null loudly -- stopping here would desynchronize every
        // later structure. Truncation inside still fails (real desync).
        RetailWalkDirectoryRecord tmpRecord{};
        RetailWalkDirectoryResult tmpSummary{};
        if (!ReadRetailMaterialTable(context->session, context->reader, kInlineRef,
                                     wire.textureCount, 12, 4, true, &tmpRecord, &tmpSummary) ||
            !ReadRetailMaterialTable(context->session, context->reader, kInlineRef,
                                     wire.constantCount, 32, 16, false, &tmpRecord, &tmpSummary) ||
            !ReadRetailMaterialTable(context->session, context->reader, kInlineRef,
                                     wire.stateBitsCount, 8, 4, false, &tmpRecord, &tmpSummary))
        {
            Com_Printf(0, "RetailWorld: dead inline body unreadable\n");
            return false;
        }
        Com_Printf(0, "RetailWorld: TOLERATED null dead inline material tex=%u const=%u state=%u\n",
                   wire.textureCount, wire.constantCount, wire.stateBitsCount);
        if (slotOffset)
            RecordNestedSlot(context, slotOffset, ASSET_TYPE_MATERIAL, {});
        *out = nullptr;
        return true;
    }
    if (wireRes != FS_RETAIL_FF_WIRE_OK)
    {
        Com_Printf(0, "RetailWorld: nested material body unreadable\n");
        return false;
    }
    if (!RetailZoneLoadSessionSyncFromReader(context->session, context->reader, 4))
        return false;
    XAssetHeader widened{};
    if (!RetailWidenMaterialFromWire(context->session, context->reader, wire, textureDefs,
                                     context->directoryOffset, context->directoryBytes,
                                     context->stubNames, context->stubCount, context->techCache,
                                     &widened, true, true, true) ||
        !widened.material)
    {
        Com_Printf(0, "RetailWorld: nested material strict widen failed\n");
        return false;
    }
    // A null-named nested body widens unregistered by name (nothing can
    // address it by name), but a known slot still records the widened
    // result: later DB_ConvertOffsetToAlias-style slot indirections bind
    // through the slot, name or no name.
    if (widened.material->info.name &&
        !RecordNestedName(context, wire.nameRef, ASSET_TYPE_MATERIAL, widened, 0,
                          bodyOffset))
    {
        Com_Printf(0, "RetailWorld: nested material has no recordable name\n");
        return false;
    }
    if (slotOffset &&
        !RecordNestedSlot(context, slotOffset, ASSET_TYPE_MATERIAL, widened))
    {
        Com_Printf(0, "RetailWorld: nested material slot %u has no recordable slot entry\n",
                   slotOffset);
        return false;
    }
    ++context->widenedMaterialCount;
    *out = widened.material;
    RETAIL_ASSET_TRACE(0, "RetailWorld: material widened '%s' tex=%u\n",
                       widened.material->info.name ? widened.material->info.name : "(null)",
                       widened.material->textureCount);
    return true;
}

bool RetailWorldWidenImage(RetailWorldLoadContext *context, uint32_t slotRef, GfxImage **out,
                           uint32_t slotOffset)
{
    if (!context || !context->session || !context->reader || !out)
        return false;
    *out = nullptr;
    if (!slotRef)
        return true;
    if (slotRef != kInlineRef && slotRef != kInsertRef)
    {
        // Engine slot form (db_load.cpp's Load_GfxImagePtr ->
        // DB_ConvertOffsetToAlias, uniform for every GfxImage* field): the
        // reference is a (block,offset) pair whose raw 4 bytes the declaring
        // occurrence patched with its pooled image reference. Canonical
        // placement means those bytes sit exactly where the serialized
        // reference names them, so resolve the pool slot directly before
        // the recorded-table/name shapes.
        uint32_t poolRef = 0;
        if (FS_RetailFastfileResolveImageAlias(context->reader, slotRef, &poolRef) ==
            FS_RETAIL_FF_WIRE_OK)
        {
            uint32_t poolIndex = 0;
            if (!RetailWirePoolIndexDecode({poolRef}, RETAIL_FASTFILE_IMAGE_POOL_MAX,
                                           &poolIndex))
                return false;
            GfxImage *pooled = context->session->imagePool[poolIndex];
            if (pooled)
            {
                if (slotOffset)
                    RecordNestedSlot(context, slotOffset, ASSET_TYPE_IMAGE,
                                     XAssetHeader(pooled));
                *out = pooled;
                return true;
            }
        }
        XAssetHeader found{};
        if (!ResolveNestedAlias(context, slotRef, ASSET_TYPE_IMAGE, &found) || !found.image)
            return false;
        if (slotOffset)
            RecordNestedSlot(context, slotOffset, ASSET_TYPE_IMAGE, found);
        *out = found.image;
        return true;
    }
    // Insert carries an inline body (HandleAssetSlot reserves the pointer
    // slot, then streams the body), so it consumes exactly like inline.
    // HandleAssetSlot's -2 image insert reserves 4 bytes of block 4 before the
    // body streams (the walk reader and the engine both do this). The old
    // FS-read form skipped it, leaving every later block-4 slot 4 bytes low.
    uint32_t insertOffset = 0;
    if (slotRef == kInsertRef)
    {
        insertOffset =
            (FS_RetailFastfileBlockCursor(context->reader, 4) + 3u) & ~3u;
        FS_RetailFastfileSetBlockCursor(context->reader, 4, insertOffset + 4u);
        if (!RetailWireBlocksAlloc(&context->session->wire, 4, 4, 4))
            return false;
    }
    FsRetailFastfileImage wire{};
    if (FS_ReadRetailFastfileImage(context->reader, kInlineRef, &wire) != FS_RETAIL_FF_WIRE_OK)
    {
        Com_Printf(0, "RetailWorld: nested image body unreadable ref=0x%08x\n", slotRef);
        return false;
    }
    if (!RetailZoneLoadSessionSyncFromReader(context->session, context->reader, 4))
        return false;
    RETAIL_ASSET_TRACE(0, "RetailWorld: image wire mapType=%u textureRef=0x%08x whd=%ux%ux%u cat=%u delay=%u nameRef=0x%08x haveLoadDef=%u resSize=%u\n",
                       wire.mapType, wire.textureRef, wire.width, wire.height, wire.depth,
                       wire.category, wire.delayLoadPixels, wire.nameRef, wire.haveLoadDef,
                       wire.loadDefResourceSize);
    XAssetHeader widened{};
    if (!RetailWidenImageFromWire(context->session, context->reader, wire, &widened, nullptr,
                                  true) ||
        !widened.image)
    {
        Com_Printf(0, "RetailWorld: nested image strict widen failed ref=0x%08x\n", slotRef);
        return false;
    }
    if (!RecordNestedName(context, wire.nameRef, ASSET_TYPE_IMAGE, widened))
    {
        Com_Printf(0, "RetailWorld: nested image has no recordable name\n");
        return false;
    }
    const uint32_t poolSlot = slotOffset ? slotOffset : insertOffset;
    if (poolSlot)
    {
        uint32_t poolRef = 0;
        if (FS_RetailFastfileRegisterImagePoolSlot(context->reader, wire.nameRef,
                                                   poolSlot, &poolRef) !=
            FS_RETAIL_FF_WIRE_OK)
            return false;
        if (insertOffset && insertOffset != poolSlot &&
            FS_RetailFastfilePatchImagePoolSlot(context->reader, poolRef, insertOffset) !=
                FS_RETAIL_FF_WIRE_OK)
            return false;
        uint32_t poolIndex = 0;
        if (!RetailWirePoolIndexDecode({poolRef}, RETAIL_FASTFILE_IMAGE_POOL_MAX,
                                       &poolIndex))
            return false;
        context->session->imagePool[poolIndex] = widened.image;
    }
    ++context->widenedImageCount;
    *out = widened.image;
    RETAIL_ASSET_TRACE(0, "RetailWorld: image widened '%s' %ux%u map=%u\n",
                       widened.image->name ? widened.image->name : "(null)",
                       widened.image->width, widened.image->height, widened.image->mapType);
    return true;
}

bool RetailWorldWidenLightDef(RetailWorldLoadContext *context, uint32_t slotRef,
                              GfxLightDef **out)
{
    if (!context || !context->session || !context->reader || !out)
        return false;
    *out = nullptr;
    if (!slotRef)
        return true;
    if (slotRef != kInlineRef && slotRef != kInsertRef)
    {
        XAssetHeader found{};
        if (!ResolveNestedAlias(context, slotRef, ASSET_TYPE_LIGHT_DEF, &found) ||
            !found.lightDef)
            return false;
        *out = found.lightDef;
        return true;
    }
    // FS-level twin of the live LightDef loader: 16-byte root (name ref,
    // attenuation image slot, sampler state, lightmap lookup start), then
    // the inline name and the nested image in stream order. Insert carries
    // the same root body (HandleAssetSlot reservation is postload scope).
    uint8_t root[16]{};
    if (!RetailWorldConsume(context, root, sizeof(root)))
    {
        Com_Printf(0, "RetailWorld: nested lightdef root unreadable\n");
        return false;
    }
    Com_Printf(0, "RetailWorld: lightdef root nameRef=0x%08x imageRef=0x%08x sampler=%u lmap=%d\n",
               static_cast<uint32_t>(root[0]) | (static_cast<uint32_t>(root[1]) << 8) |
                   (static_cast<uint32_t>(root[2]) << 16) | (static_cast<uint32_t>(root[3]) << 24),
               static_cast<uint32_t>(root[4]) | (static_cast<uint32_t>(root[5]) << 8) |
                   (static_cast<uint32_t>(root[6]) << 16) | (static_cast<uint32_t>(root[7]) << 24),
               root[8],
               static_cast<int32_t>(static_cast<uint32_t>(root[12]) |
                                    (static_cast<uint32_t>(root[13]) << 8) |
                                    (static_cast<uint32_t>(root[14]) << 16) |
                                    (static_cast<uint32_t>(root[15]) << 24)));
    const uint32_t nameRef = static_cast<uint32_t>(root[0]) |
                             (static_cast<uint32_t>(root[1]) << 8) |
                             (static_cast<uint32_t>(root[2]) << 16) |
                             (static_cast<uint32_t>(root[3]) << 24);
    const uint32_t imageRef = static_cast<uint32_t>(root[4]) |
                              (static_cast<uint32_t>(root[5]) << 8) |
                              (static_cast<uint32_t>(root[6]) << 16) |
                              (static_cast<uint32_t>(root[7]) << 24);
    uint32_t nameOffset = 0;
    const char *lightName = nullptr;
    if (!RetailWorldStreamName(context, nameRef, &nameOffset, &lightName) || !lightName)
    {
        Com_Printf(0, "RetailWorld: nested lightdef name unreadable\n");
        return false;
    }
    GfxImage *attenuation = nullptr;
    if (!RetailWorldWidenImage(context, imageRef, &attenuation))
    {
        Com_Printf(0, "RetailWorld: nested lightdef attenuation unreadable\n");
        return false;
    }
    GfxLightDef *lightDef = static_cast<GfxLightDef *>(
        RetailZoneLoadSessionAlloc(context->session, sizeof(GfxLightDef), alignof(GfxLightDef)));
    if (!lightDef)
        return false;
    std::memset(lightDef, 0, sizeof(*lightDef));
    const std::size_t nameBytes = std::strlen(lightName) + 1;
    char *nameCopy = static_cast<char *>(RetailZoneLoadSessionAlloc(context->session, nameBytes, 1));
    if (!nameCopy)
        return false;
    std::memcpy(nameCopy, lightName, nameBytes);
    lightDef->name = nameCopy;
    lightDef->attenuation.samplerState = root[8];
    lightDef->attenuation.image = attenuation;
    lightDef->lmapLookupStart = static_cast<int32_t>(
        static_cast<uint32_t>(root[12]) | (static_cast<uint32_t>(root[13]) << 8) |
        (static_cast<uint32_t>(root[14]) << 16) | (static_cast<uint32_t>(root[15]) << 24));
    XAssetHeader regHeader{};
    regHeader.lightDef = lightDef;
    const XAssetHeader registered =
        RetailZoneLoadSessionRegister(context->session, ASSET_TYPE_LIGHT_DEF, regHeader);
    if (!registered.lightDef)
    {
        Com_Printf(0, "RetailWorld: nested lightdef register failed\n");
        return false;
    }
    RetailWireToken token{};
    if (DecodeFastfileToken(context->reader, nameRef, 0, 1u << 4, &token) &&
        token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4 &&
        context->nestedCount < RetailWorldLoadContext::kNestedCap)
    {
        RetailWorldNestedName &entry = context->nested[context->nestedCount++];
        entry.nameOffset = token.offset;
        entry.type = ASSET_TYPE_LIGHT_DEF;
        entry.header = registered;
    }
    ++context->widenedLightDefCount;
    *out = registered.lightDef;
    return true;
}

// camera source: the world-family MapEnts root
// widens through the same loader contract as RawFile -- a 12-byte root
// {name, entityString, numEntityChars} in block 0, the name XString in
// block 4, then exactly numEntityChars entity bytes (Load_MapEnts's
// Load_charArray has no +1: the engine's own terminator convention is
// whatever the authored span carries). The entity string is retained
// zone-lifetime so the first-frame orchestration can parse the player start.
namespace
{
uint32_t MapEntsLe32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool LoadMapEnts(RetailZoneLoadSession *session, XAssetType type, bool,
                 void *, XAssetHeader *header)
{
    if (type != ASSET_TYPE_MAP_ENTS || !header || !session)
        return false;
    uint8_t wire[12];
    if (!RetailWireBlocksRead(&session->wire, 0, wire, sizeof(wire)))
        return false;
    const uint32_t nameRef = MapEntsLe32(wire);
    const uint32_t entityRef = MapEntsLe32(wire + 4);
    const int32_t numChars = static_cast<int32_t>(MapEntsLe32(wire + 8));
    if (numChars < 0)
        return false;
    MapEnts *ents = static_cast<MapEnts *>(RetailZoneLoadSessionAlloc(
        session, sizeof(MapEnts), alignof(MapEnts)));
    if (!ents)
        return false;
    std::memset(ents, 0, sizeof(*ents));
    if (!RetailDecodeInlineOrNullString(session, nameRef, &ents->name))
        return false;
    ents->numEntityChars = numChars;
    if (entityRef)
    {
        const uint32_t bytes = static_cast<uint32_t>(numChars);
        char *entityString = static_cast<char *>(RetailZoneLoadSessionAlloc(
            session, bytes ? bytes : 1, 1));
        if (!entityString)
            return false;
        if (entityRef == kInlineRef)
        {
            if (!RetailWireBlocksRead(&session->wire, 4, entityString, bytes))
                return false;
        }
        else
        {
            // Alias: an already-streamed entity span at a block-4 offset.
            RetailWireToken token{};
            XBlock blocks[9]{};
            for (uint32_t block = 0; block < 9; ++block)
                blocks[block] = session->zoneMemory->blocks[block];
            RetailPtr32 encoded{};
            encoded.encoded = entityRef;
            if (!RetailWireTokenDecodeBlocks(blocks, encoded, 1, 1u << 4, &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4 ||
                (uint64_t)token.offset + bytes > blocks[4].size)
                return false;
            std::memcpy(entityString, blocks[4].data + token.offset, bytes);
        }
        ents->entityString = entityString;
    }
    *header = {ents};
    return true;
}
} // namespace

bool RetailZoneInstallMapEntsDecoder(RetailZoneLoadSession *session)
{
    return RetailZoneLoadSessionSetAssetLoader(session, ASSET_TYPE_MAP_ENTS,
                                               LoadMapEnts, nullptr);
}

uint32_t ComWorldLe32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Android Load_ComWorld is a 16-byte ILP32 root followed by a 68-byte
// ILP32 ComPrimaryLight array.  Native ComPrimaryLight grows because its
// defName is a host pointer, so copy fields instead of casting the wire.
// The global comWorld is the canonical engine singleton: construct a complete
// candidate first and publish it only after every authored defName resolves.
bool LoadComWorld(RetailZoneLoadSession *session, XAssetType type, bool,
                  void *, XAssetHeader *header)
{
    constexpr uint32_t kRootBytes = 16;
    constexpr uint32_t kLightBytes = 68;
    constexpr uint32_t kDefNameOffset = 64;
    if (!session || !header || type != ASSET_TYPE_COMWORLD)
        return false;
    if (comWorld.isInUse)
    {
        Com_Printf(0, "RetailComWorld: refusing replacement while '%s' is active\n",
                   comWorld.name ? comWorld.name : "(unnamed)");
        return false;
    }

    uint8_t root[kRootBytes]{};
    if (!RetailWireBlocksRead(&session->wire, 0, root, sizeof(root)))
        return false;
    const uint32_t nameRef = ComWorldLe32(root);
    const uint32_t isInUse = ComWorldLe32(root + 4);
    const uint32_t lightCount = ComWorldLe32(root + 8);
    const uint32_t lightsRef = ComWorldLe32(root + 12);
    if (lightCount > UINT32_MAX / kLightBytes || (!!lightCount != !!lightsRef))
    {
        Com_Printf(0, "RetailComWorld: invalid root count=%u lightsRef=0x%08x\n",
                   lightCount, lightsRef);
        return false;
    }

    ComWorld candidate{};
    if (!RetailDecodeInlineOrNullString(session, nameRef, &candidate.name) || !candidate.name)
    {
        Com_Printf(0, "RetailComWorld: world name unreadable ref=0x%08x\n", nameRef);
        return false;
    }
    candidate.isInUse = isInUse ? 1 : 0;
    candidate.primaryLightCount = lightCount;
    if (lightCount)
    {
        candidate.primaryLights = static_cast<ComPrimaryLight *>(
            RetailZoneLoadSessionAlloc(session, sizeof(ComPrimaryLight) * lightCount,
                                       alignof(ComPrimaryLight)));
        if (!candidate.primaryLights)
            return false;
        std::memset(candidate.primaryLights, 0, sizeof(ComPrimaryLight) * lightCount);
    }
    uint32_t *defNameRefs = nullptr;
    if (lightCount)
    {
        defNameRefs = static_cast<uint32_t *>(RetailZoneLoadSessionAlloc(
            session, sizeof(uint32_t) * lightCount, alignof(uint32_t)));
        if (!defNameRefs)
            return false;
    }

    // Load_ComPrimaryLightArray begins with an aligned Load_Stream in block
    // 4.  The preceding world-name XString is byte-aligned, so replaying an
    // authored name with a non-multiple-of-four extent must advance past its
    // stream padding before the 68-byte records are read.
    if (lightCount && !RetailWireBlocksAlloc(&session->wire, 4, 0, 4))
        return false;

    uint8_t *aliasLights = nullptr;
    if (lightsRef != kInlineRef && lightsRef != kInsertRef && lightsRef)
    {
        XBlock blocks[9]{};
        for (uint32_t block = 0; block < 9; ++block)
            blocks[block] = session->zoneMemory->blocks[block];
        RetailWireToken token{};
        if (!RetailWireTokenDecodeBlocks(blocks, {lightsRef}, lightCount * kLightBytes,
                                         1u << 4, &token) ||
            token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
        {
            Com_Printf(0, "RetailComWorld: alias array decode failed ref=0x%08x count=%u b4=%u\n",
                       lightsRef, lightCount, blocks[4].size);
            return false;
        }
        aliasLights = blocks[4].data + token.offset;
    }
    for (uint32_t i = 0; i < lightCount; ++i)
    {
        uint8_t wire[kLightBytes]{};
        if (aliasLights)
            std::memcpy(wire, aliasLights + i * kLightBytes, sizeof(wire));
        else if (!RetailWireBlocksRead(&session->wire, 4, wire, sizeof(wire)))
        {
            Com_Printf(0, "RetailComWorld: inline array read failed index=%u cursor=%u\n",
                       i, session->wire.cursor[4]);
            return false;
        }
        ComPrimaryLight &out = candidate.primaryLights[i];
        out.type = wire[0];
        out.canUseShadowMap = wire[1];
        out.exponent = wire[2];
        out.unused = wire[3];
        std::memcpy(out.color, wire + 4, sizeof(out.color));
        std::memcpy(out.dir, wire + 16, sizeof(out.dir));
        std::memcpy(out.origin, wire + 28, sizeof(out.origin));
        std::memcpy(&out.radius, wire + 40, sizeof(float) * 6);
        std::memcpy(&out.rotationLimit, wire + 56, sizeof(float) * 2);
        defNameRefs[i] = ComWorldLe32(wire + kDefNameOffset);
    }
    // Load_ComPrimaryLightArray consumes all 68-byte records first; each
    // Load_XString(defName) follows only after the whole array.  On LP64 we
    // retain those wire refs separately while widening the pointer-bearing
    // native records, then resolve in the authored order.
    //
    // Retail's ComPrimaryLight (xanim.h, sizeof 0x44) carries only the
    // defName string -- there is no pointer slot to cache a resolved
    // GfxLightDef* in, and LoadComWorld never looked one up: R_LoadPrimaryLights
    // (r_bsp_load_obj.cpp) resolves each defName via R_RegisterLightDef when
    // the zone's gfxworld loads, which is always later in decode order than
    // comworld. A same-zone comworld legitimately references a lightdef
    // declared after it (verified via OAT on cargoship.ff: comworld precedes
    // lightdef 'light_no_falloff' by two entries); eagerly requiring the
    // lightdef to already be registered here rejects that retail-valid order.
    for (uint32_t i = 0; i < lightCount; ++i)
    {
        ComPrimaryLight &out = candidate.primaryLights[i];
        if (!RetailDecodeInlineOrNullString(session, defNameRefs[i], &out.defName))
        {
            Com_Printf(0, "RetailComWorld: def name decode failed index=%u ref=0x%08x cursor=%u\n",
                       i, defNameRefs[i], session->wire.cursor[4]);
            return false;
        }
    }
    comWorld = candidate;
    *header = {&comWorld};
    return true;
}

bool RetailZoneInstallComWorldDecoder(RetailZoneLoadSession *session)
{
    return RetailZoneLoadSessionSetAssetLoader(session, ASSET_TYPE_COMWORLD,
                                               LoadComWorld, nullptr);
}

GfxWorld *RetailWorldAllocTransaction(RetailZoneLoadSession *session)
{
    if (!session || !session->active)
        return nullptr;
    GfxWorld *world = static_cast<GfxWorld *>(RetailZoneLoadSessionAlloc(
        session, sizeof(GfxWorld), alignof(GfxWorld)));
    if (world)
        std::memset(world, 0, sizeof(GfxWorld));
    return world;
}

XAssetHeader RetailWorldCommitTransaction(RetailZoneLoadSession *session, GfxWorld *transaction)
{
    XAssetHeader header{};
    if (!session || !session->active || !transaction)
        return header;
    // The atomic commit: one copy into the engine's own world singleton.
    // Everything before this point widened into zone-arena scratch, so a
    // failed load never partially mutates s_world (rollback = discard).
    std::memcpy(&s_world, transaction, sizeof(s_world));
    header.gfxWorld = &s_world;
    return RetailZoneLoadSessionRegister(session, ASSET_TYPE_GFXWORLD, header);
}

namespace
{
uint32_t g_lastMaterials = 0;
uint32_t g_lastImages = 0;
uint32_t g_lastLightDefs = 0;
uint32_t g_lastBlock1Bytes = 0;
} // namespace

void RetailWorldLastLoadStats(uint32_t *materials, uint32_t *images,
                              uint32_t *lightDefs, uint32_t *block1Bytes)
{
    if (materials)
        *materials = g_lastMaterials;
    if (images)
        *images = g_lastImages;
    if (lightDefs)
        *lightDefs = g_lastLightDefs;
    if (block1Bytes)
        *block1Bytes = g_lastBlock1Bytes;
}

namespace
{
uint32_t ReadLe32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t ReadLe16(const uint8_t *p)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

float ReadLeFloat(const uint8_t *p)
{
    const uint32_t bits = ReadLe32(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Widen one 48-byte DPVS surface record: triangle scalars and bounds copy
// verbatim (plain ints/floats, no layout question); the material slot at
// +16 is either a nested material reference or -- the common retail shape
// -- a block-4 pointer into the materialMemory pointer array (one 8-byte
// entry per matmem material, entry slotland mirror-placed by the matmem
// section above). Array entries resolve to the already-widened entry, the
// same pointer the engine's ConvertOffsetToPointer fixup produces; every
// other shape resolves exactly like every other nested material.
bool WidenGfxSurface(RetailWorldLoadContext *context, const uint8_t *record, GfxSurface *surface)
{
    if (!context || !record || !surface)
        return false;
    std::memset(surface, 0, sizeof(*surface));
    surface->tris.vertexLayerData = static_cast<int>(ReadLe32(record));
    surface->tris.firstVertex = static_cast<int>(ReadLe32(record + 4));
    surface->tris.vertexCount = ReadLe16(record + 8);
    surface->tris.triCount = ReadLe16(record + 10);
    surface->tris.baseIndex = static_cast<int>(ReadLe32(record + 12));
    Material *material = nullptr;
    const uint32_t matSlot = ReadLe32(record + 16);
    bool viaMatmem = false;
    if (context->matmem && context->matmemCount && context->matmemEngineVirt)
    {
        RetailWireToken token{};
        if (DecodeFastfileToken(context->reader, matSlot, 0, 1u << 4, &token) &&
            token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4 &&
            token.offset >= context->matmemEngineVirt &&
            token.offset - context->matmemEngineVirt <
                (uint64_t)context->matmemCount * 8u &&
            ((token.offset - context->matmemEngineVirt) & 7u) == 0u)
        {
            const uint32_t entry =
                (token.offset - context->matmemEngineVirt) / 8u;
            material = context->matmem[entry].material;
            viaMatmem = true;
        }
    }
    if (!viaMatmem && !RetailWorldWidenMaterial(context, matSlot, &material))
        return false;
    if (!material)
    {
        // a surface's material is consumer-visible (every draw of the
        // surface selects it); a dead inline body here is a load failure,
        // not a null to tolerate.
        Com_Printf(0, "RetailWorld: surface material unresolved (dead inline body) slot=0x%08x\n", matSlot);
        return false;
    }
    surface->material = material;
    surface->lightmapIndex = record[20];
    surface->reflectionProbeIndex = record[21];
    surface->primaryLightIndex = record[22];
    surface->flags = record[23];
    std::memcpy(surface->bounds, record + 24, sizeof(surface->bounds));
    return true;
}

// Widen one 76-byte GfxStaticModelDrawInst wire record (Load_GfxStaticModelDrawInst,
// db_load.cpp:6144 -- a plain 76-byte Load_Stream whose only follow-up is
// patching the model pointer at +56, so every other byte is exactly the
// disk value with no engine-side computation) into the native struct,
// which is 80 bytes on LP64 (r_gfx.h's `XModel* model` forces 8-byte
// struct alignment; LP64 layout contract). `cullDist` (+0) and `placement` (+4, GfxPackedPlacement, 0x34
// bytes) are plain floats with no pointer members, so they sit at
// identical native/wire offsets on both ABIs and widen with one bulk
// copy; the pointer at +56 is the reason every field after it shifts
// under LP64 (+64 vs wire +60 for smodelCacheIndex, etc.), so those are
// read individually at their real wire offsets rather than bulk-copied.
// Array navigation here is by plain smodelIndex, never a baked byte
// offset, so this decoder is not the GfxAabbTree::childrenOffset hazard
// class.
//
// `model`: now the real
// widened/registered XModel the caller resolved via RetailWorldWidenXModel
// from this same record's +56 slot -- never a placeholder/default when the
// wire slot is non-null; the caller passes null through only when the wire
// slot itself decoded null (a real, if unusual, retail shape).
uint32_t g_m14StaleCacheIndexCount = 0;
static_assert(offsetof(GfxStaticModelDrawInst, model) == 56,
              "wire model slot must land at the same offset the bulk placement copy assumes");
void WidenGfxStaticModelDrawInst(const uint8_t *record, XModel *model, GfxStaticModelDrawInst *out)
{
    std::memset(out, 0, sizeof(*out));
    std::memcpy(out, record, 56); // cullDist + GfxPackedPlacement: byte-identical ILP32/LP64
    out->model = model;
    out->smodelCacheIndex[0] = ReadLe16(record + 60);
    out->smodelCacheIndex[1] = ReadLe16(record + 62);
    out->smodelCacheIndex[2] = ReadLe16(record + 64);
    out->smodelCacheIndex[3] = ReadLe16(record + 66);
    out->reflectionProbeIndex = record[68];
    out->primaryLightIndex = record[69];
    out->lightingHandle = ReadLe16(record + 70);
    out->flags = record[72];
    // Temporary diagnostic (VK_ERROR_DEVICE_LOST triage): smodelCacheIndex is
    // pure renderer runtime state (r_staticmodelcache.cpp owns it), but
    // Load_GfxStaticModelDrawInst (db_load.cpp:6144) blits it straight off
    // the wire like every other field, so this port does too. If any real
    // killhouse instance ships a nonzero value, R_CacheStaticModelSurface
    // would take its "already cached" early-out on frame 1 and hand the draw
    // a leaf that SMC_Allocate never allocated (smodelIndex 0xFFFF,
    // baseVertIndex 0, index-cache range never written). Counted here, not
    // assumed either way; removed once the crash is root-caused.
    for (int lod = 0; lod < 4; ++lod)
    {
        if (out->smodelCacheIndex[lod])
            ++g_m14StaleCacheIndexCount;
    }
}
} // namespace

// ---- XModel/XSurface widening ----
//
// Wire layout cross-validated against db_retail_walk.cpp's proven-byte-exact
// walk-only readers (ReadRetailXModelBody:2160, ReadRetailXSurfaceBody:2052,
// ReadRetailXSurfaceCollisionTreeBody:2032 -- already confirmed against OAT
// for real killhouse.ff) rather than re-derived from the native struct
// layout: every offset below matches those functions' own reads exactly.
// Root is 220 bytes (Load_XModel, db_load.cpp:3156); XSurface is 56 wire /
// 80 native bytes (Load_XSurface, db_load.cpp:2025, XSurface's `uint16_t
// *triIndices`/`GfxPackedVertex *verts0`/`XRigidVertList *vertList`
// pointers force 8-byte alignment under LP64); XRigidVertList is 12 wire /
// 16 native (Load_XRigidVertList, db_load.cpp:1941).
//
// Byte-offset-vs-index LP64 review (the
// GfxAabbTree::childrenOffset precedent): every field this decoder reads is
// either (a) a plain scalar/POD value copied verbatim into a same-layout
// native field (DObjAnimMat/XModelLodInfo/XModelCollTri_s-shaped data has no
// pointer members, so ILP32 and LP64 sizes match byte-for-byte -- no
// restride is possible or needed), (b) a flat array indexed by element
// count (immune by construction: the stride multiply happens at this
// compiler's own sizeof when C++ indexes surf[i]/lodInfo[i], never a value
// pre-multiplied by the original 32-bit compiler), or (c) an asset-slot
// reference resolved through the existing alias table (name-based, never a
// raw byte offset into a native struct array). There is no relative
// sibling/child navigation field anywhere in XModel/XSurface/
// XRigidVertList/XSurfaceVertexInfo (unlike GfxAabbTree's childrenOffset),
// so none of this decoder's fields are candidates for that hazard class --
// checked explicitly per field above, not assumed absent.
//
// Scope: XModel's collSurfs/physPreset/physGeoms remain walk-consumed for
// exact cursor accounting -- required so the shared stream stays in sync
// with everything that follows -- but are deliberately left unwidened/null,
// matching the static-model *geometry* scope and the ground rule against
// activating gameplay/collision. XRigidVertList/XSurfaceCollisionTree are
// different: the renderer's rigid skinning and mark paths dereference them,
// so the skinning-critical records are widened below. XModel boneInfo is
// widened (40 float-only bytes, same layout both ABIs) because the cgame
// scene-entity bounds path (R_UpdateSceneEntBounds) dereferences it for
// every visible DObj; publishing null there null-derefs the first bounded
// entity after spawn. None of the remaining fields are read by the
// renderer's static-model draw path
// (R_SkinXSurfaceStaticVerts/R_CacheStaticModelIndices/R_SetupStaticModelPrim
// dereference only verts0/triIndices/vertCount/triCount/baseVertIndex/
// baseTriIndex/zoneHandle -- confirmed by reading r_staticmodelcache.cpp
// and r_draw_staticmodel.cpp before writing this decoder, not assumed).
// XSurfaceVertexInfo's vertsBlend is skinning-weight data for the
// character/DObj skeletal path; it is widened (a uint16 plain-pointer array)
// because R_SkinXSurfaceWeight walks it directly for every deformed surface
// once a character DObj is skinned.
namespace
{
// Widen the skinning-critical XRigidVertList records after the helper below
// resolves their block-4 spans.  The original Load_XSurface publishes these
// records; consuming and discarding them makes every rigid surface look
// deformed on LP64 and violates the original XSurface invariant.
bool WidenXRigidVertListArray(RetailWorldLoadContext *context, uint32_t ref,
                              uint32_t count, XRigidVertList **out);

bool ConsumeRootPlainArray(RetailWorldLoadContext *context, uint32_t ref, uint32_t byteCount,
                           uint32_t alignment, void **outCopy, uint32_t elementSize);

// Widens one already-bulk-read 56-byte XSurface wire record (`record`
// points into the session's block-4 bulk-read span, matching
// Load_XSurfaceArray's own bulk-then-resolve order) into the native
// (80-byte on LP64) struct, resolving verts0/triIndices into the
// zone-block-backed block 7/8 spans the renderer's static-model draw path
// dereferences directly as CPU pointers (blocks 7 and 8 are a deliberate exception: the renderer
// derives GPU offsets by subtracting their addresses from the zone's CPU
// block bases -- confirmed for static models specifically by reading
// R_SkinXSurfaceStaticVerts/R_CacheStaticModelIndices, which read
// xsurf->verts0/triIndices as plain CPU pointers, never a GPU offset).
bool WidenOneXSurface(RetailWorldLoadContext *context, const uint8_t *record, XSurface *surf)
{
    RetailZoneLoadSession *session = context->session;
    FsRetailFastfileReader *reader = context->reader;
    std::memset(surf, 0, sizeof(*surf));
    surf->tileMode = record[0];
    surf->deformed = record[1] != 0;
    const uint16_t vertCount = ReadLe16(record + 2);
    const uint16_t triCount = ReadLe16(record + 4);
    surf->vertCount = vertCount;
    surf->triCount = triCount;
    if (session->zoneIndex > UINT8_MAX)
        return false;
    surf->zoneHandle = static_cast<uint8_t>(session->zoneIndex);
    surf->baseTriIndex = ReadLe16(record + 8);
    surf->baseVertIndex = ReadLe16(record + 10);
    std::memcpy(surf->partBits, record + 40, sizeof(surf->partBits));

    // XSurfaceVertexInfo (wire +16, 12 bytes): four blend counts copy
    // verbatim; vertsBlend is a uint16 array of kWeights[] entries per count
    // that R_SkinXSurfaceWeight walks directly for every deformed surface, so
    // it must be widened (same plain-pointer-array form as verts0). A null
    // vertsBlend here is the first null deref of the character skinning path.
    for (int i = 0; i < 4; ++i)
        surf->vertInfo.vertCount[i] = static_cast<int16_t>(ReadLe16(record + 16 + i * 2));
    static const uint32_t kWeights[4] = {1, 3, 5, 7};
    uint32_t blendEntries = 0;
    for (uint32_t w = 0; w < 4; ++w)
        blendEntries += kWeights[w] * static_cast<uint16_t>(surf->vertInfo.vertCount[w]);
    void *blendCopy = nullptr;
    if (!ConsumeRootPlainArray(context, ReadLe32(record + 24), blendEntries * 2u, 2, &blendCopy, 2))
        return false;
    surf->vertInfo.vertsBlend = static_cast<uint16_t *>(blendCopy);
    if (blendEntries && !surf->vertInfo.vertsBlend)
    {
        Com_Printf(0, "RetailWorld: xmodel surface vertsBlend unresolved plain-pointer ref=0x%08x entries=%u\n",
                   ReadLe32(record + 24), blendEntries);
        return false; // skinning-critical: never silently drop blend weights
    }

    surf->verts0 = nullptr;
    if (ReadLe32(record + 28) == kInlineRef && vertCount)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 7,
                                             static_cast<uint32_t>(vertCount) * 32u, 16))
            return false;
        const uint32_t off = session->wire.cursor[7] - static_cast<uint32_t>(vertCount) * 32u;
        surf->verts0 = reinterpret_cast<GfxPackedVertex *>(session->zoneMemory->blocks[7].data + off);
    }
    else if (ReadLe32(record + 28) != 0)
    {
        // Pointer form (DB_ConvertOffsetToPointer): the value addresses an
        // already-streamed block-7 span some earlier surface's inline verts
        // filled -- surfaces sharing one vertex array. Blocks 7/8 persist
        // for the zone lifetime by design, so bind the zone-block bytes
        // directly; the span check rejects out-of-block values.
        // Decode against the zone block the pointer binds to: the FS reader
        // never streams blocks 7/8 (the walk streams them into zone memory),
        // so it keeps no mirror of them.
        RetailWireToken token{};
        if (!RetailWireTokenDecodeBlocks(session->zoneMemory->blocks, {ReadLe32(record + 28)},
                                         static_cast<uint32_t>(vertCount) * 32u, 1u << 7, &token) ||
            token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 7)
        {
            Com_Printf(0, "RetailWorld: xmodel surface verts0 unresolved plain-pointer ref=0x%08x\n",
                       ReadLe32(record + 28));
            return false; // draw-critical: never silently drop real geometry
        }
        surf->verts0 = reinterpret_cast<GfxPackedVertex *>(
            session->zoneMemory->blocks[7].data + token.offset);
    }

    const uint32_t vertListRef = ReadLe32(record + 36);
    const uint32_t wireVertListCount = ReadLe32(record + 32);
    if (wireVertListCount &&
        !WidenXRigidVertListArray(context, vertListRef, wireVertListCount,
                                  &surf->vertList))
    {
        Com_Printf(0, "RetailWorld: xmodel surface vertList widen failed ref=0x%08x count=%u\n",
                   vertListRef, (unsigned)wireVertListCount);
        return false;
    }
    // An unresolved non-inline list leaves no storage; publish the
    // pre-widening shape (count 0, null) so rigid consumers such as
    // R_SkinXSurfaceStaticVerts never index a null array. The unresolved
    // counter above keeps that deferred work visible.
    surf->vertListCount = surf->vertList ? wireVertListCount : 0;
    if (!surf->deformed && wireVertListCount == 0)
    {
        Com_Printf(0, "RetailWorld: xmodel rigid surface has no vertList\n");
        return false;
    }
    if (surf->deformed && wireVertListCount != 0)
    {
        Com_Printf(0, "RetailWorld: xmodel deformed surface has vertListCount=%u\n",
                   (unsigned)wireVertListCount);
        return false;
    }

    surf->triIndices = nullptr;
    if (ReadLe32(record + 12) == kInlineRef && triCount)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 8,
                                             static_cast<uint32_t>(triCount) * 6u, 16))
            return false;
        const uint32_t off = session->wire.cursor[8] - static_cast<uint32_t>(triCount) * 6u;
        surf->triIndices = reinterpret_cast<uint16_t *>(session->zoneMemory->blocks[8].data + off);
    }
    else if (ReadLe32(record + 12) != 0)
    {
        // Pointer form: an already-streamed block-8 span shared with an
        // earlier surface (same shape as verts0 above).
        RetailWireToken token{};
        if (!RetailWireTokenDecodeBlocks(session->zoneMemory->blocks, {ReadLe32(record + 12)},
                                         static_cast<uint32_t>(triCount) * 6u, 1u << 8, &token) ||
            token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 8)
        {
            Com_Printf(0, "RetailWorld: xmodel surface triIndices unresolved plain-pointer ref=0x%08x\n",
                       ReadLe32(record + 12));
            return false; // draw-critical: never silently drop real geometry
        }
        surf->triIndices = reinterpret_cast<uint16_t *>(
            session->zoneMemory->blocks[8].data + token.offset);
    }
    // GPU audit (a): a surface that claims tris must carry real index
    // storage and real verts, and every index must address its own verts.
    // A violation here would otherwise reach DrawIndexedPrimitive as an OOB
    // vertex fetch -> VK_ERROR_DEVICE_LOST with no validation error.
    if (triCount && !surf->triIndices)
    {
        Com_Printf(0, "RetailWorld: xmodel surface has triCount=%u but null triIndices (vert=%u baseTri=%u baseVert=%u)\n",
                   (unsigned)triCount, (unsigned)vertCount,
                   (unsigned)surf->baseTriIndex, (unsigned)surf->baseVertIndex);
        return false;
    }
    if (vertCount && !surf->verts0)
    {
        Com_Printf(0, "RetailWorld: xmodel surface has vertCount=%u but null verts0 (tri=%u baseTri=%u baseVert=%u)\n",
                   (unsigned)vertCount, (unsigned)triCount,
                   (unsigned)surf->baseTriIndex, (unsigned)surf->baseVertIndex);
        return false;
    }
    if (triCount && vertCount && surf->triIndices)
    {
        const uint32_t idxCount = 3u * static_cast<uint32_t>(triCount);
        for (uint32_t i = 0; i < idxCount; ++i)
        {
            if (surf->triIndices[i] >= vertCount)
            {
                Com_Printf(0, "RetailWorld: xmodel surface index OOB idx[%u]=%u >= vertCount=%u (tri=%u baseTri=%u baseVert=%u)\n",
                           i, (unsigned)surf->triIndices[i], (unsigned)vertCount,
                           (unsigned)triCount, (unsigned)surf->baseTriIndex,
                           (unsigned)surf->baseVertIndex);
                return false;
            }
        }
    }
    return true;
}

// Consume one root-level plain-pointer array slot (a bone array, or the
// collSurfs/boneInfo tail) exactly like ReadRetailXModelBody's own
// `boneArrays` table. The exact inline sentinel owns stream bytes; a nonzero
// value is the engine's DB_ConvertOffsetToPointer form -- an already-streamed
// block-4 span an earlier declarer filled (surfaces/instances sharing one
// bone table), so bind a private copy of those mirrored bytes. Null stays
// null. Only a ref that is not a consumed in-block-4 offset remains
// unresolved and is counted loudly.
bool ConsumeRootPlainArray(RetailWorldLoadContext *context, uint32_t ref, uint32_t byteCount,
                           uint32_t alignment, void **outCopy, uint32_t elementSize)
{
    *outCopy = nullptr;
    if (ref == kInlineRef)
    {
        if (!byteCount)
            return true;
        void *storage = RetailZoneLoadSessionAlloc(context->session, byteCount, alignment);
        if (!storage)
            return false;
        if (!RetailZoneLoadSessionReadStream(context->session, context->reader, 4, byteCount,
                                             alignment))
            return false;
        const uint32_t off = context->session->wire.cursor[4] - byteCount;
        std::memcpy(storage, context->session->zoneMemory->blocks[4].data + off, byteCount);
        context->nestedBodyBytes += byteCount;
        *outCopy = storage;
        return true;
    }
    if (ref != 0)
    {
        RetailWireToken token{};
        const uint32_t consumed = context->session->wire.cursor[4];
        if (byteCount && DecodeFastfileToken(context->reader, ref, byteCount, 1u << 4, &token) &&
            token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4 &&
            token.offset <= consumed && byteCount <= consumed - token.offset)
        {
            void *storage = RetailZoneLoadSessionAlloc(context->session, byteCount, alignment);
            if (!storage)
                return false;
            std::memcpy(storage, context->session->zoneMemory->blocks[4].data + token.offset,
                        byteCount);
            context->nestedBodyBytes += byteCount;
            *outCopy = storage;
            return true;
        }
        ++context->xmodelUnresolvedPlainPointerCount;
        Com_Printf(0, "RetailWorld: xmodel root plain-pointer array unresolved ref=0x%08x bytes=%u\n",
                   ref, byteCount);
    }
    (void)elementSize;
    return true;
}

// Resolve a plain-pointer ref that addresses an already-streamed block-4 span
// (the engine's DB_ConvertOffsetToPointer form), or null when it is not a
// consumed in-block-4 offset.
const uint8_t *ResolveWorldBlock4Span(RetailWorldLoadContext *context, uint32_t ref,
                                      uint32_t byteCount)
{
    if (!context || !ref || !byteCount)
        return nullptr;
    RetailWireToken token{};
    const uint32_t consumed = context->session->wire.cursor[4];
    if (!DecodeFastfileToken(context->reader, ref, byteCount, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4 ||
        token.offset > consumed || byteCount > consumed - token.offset)
        return nullptr;
    return context->session->zoneMemory->blocks[4].data + token.offset;
}

bool WidenXSurfaceCollisionTree(RetailWorldLoadContext *context, uint32_t ref,
                                XSurfaceCollisionTree **out)
{
    if (!context || !context->session || !context->reader || !out)
        return false;
    *out = nullptr;
    if (!ref)
        return true;

    RetailZoneLoadSession *session = context->session;
    FsRetailFastfileReader *reader = context->reader;
    uint8_t root[40]{};
    const uint8_t *rootBytes = nullptr;
    // Load_XSurface streams a tree root only for the inline form; a
    // non-inline ref addresses an already-streamed block-4 span (DB_
    // ConvertOffsetToPointer). Retail data also carries unconvertible
    // runtime pointers: consume nothing, materialize nothing, and count
    // them like the walk reader's other deferred aliases. Fabricating a
    // tree or moving the cursor would desync every later asset.
    const bool streamedRoot = (ref == kInlineRef);
    if (streamedRoot)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, sizeof(root), 4))
            return false;
        rootBytes = session->zoneMemory->blocks[4].data +
                    session->wire.cursor[4] - sizeof(root);
        context->nestedBodyBytes += sizeof(root);
    }
    else
    {
        rootBytes = ResolveWorldBlock4Span(context, ref, sizeof(root));
        if (!rootBytes)
        {
            ++context->xmodelUnresolvedPlainPointerCount;
            Com_Printf(0, "RetailWorld: xmodel collision tree unresolved plain-pointer ref=0x%08x\n",
                       ref);
            return true;
        }
    }
    std::memcpy(root, rootBytes, sizeof(root));

    XSurfaceCollisionTree *tree = static_cast<XSurfaceCollisionTree *>(
        RetailZoneLoadSessionAlloc(session, sizeof(XSurfaceCollisionTree),
                                   alignof(XSurfaceCollisionTree)));
    if (!tree)
        return false;
    std::memset(tree, 0, sizeof(*tree));
    std::memcpy(tree->trans, root + 0, sizeof(tree->trans));
    std::memcpy(tree->scale, root + 12, sizeof(tree->scale));
    tree->nodeCount = ReadLe32(root + 24);
    tree->leafCount = ReadLe32(root + 32);
    const uint32_t nodesRef = ReadLe32(root + 28);
    const uint32_t leavesRef = ReadLe32(root + 36);
    if (tree->nodeCount > UINT32_MAX / sizeof(XSurfaceCollisionNode) ||
        tree->leafCount > UINT32_MAX / sizeof(XSurfaceCollisionLeaf))
        return false;

    // Mirror ReadRetailXSurfaceCollisionTreeBody's consumption exactly: a
    // non-zero ref streams its whole array even when the count is zero (the
    // alignment read still advances block 4), nodes are 16-byte elements
    // aligned to 16, and leafs are 2-byte elements aligned to 2. The native
    // arrays are copies of the bytes just streamed. Consuming with any other
    // alignment leaves the shared block-4 cursor short of the walker's (FX
    // ord 4088 then misreads), which is the regression this mirrors
    // back to parity.
    auto copyStreamedSpan = [&](uint32_t spanRef, uint32_t count, uint32_t elementBytes,
                                uint32_t alignment, void **destination) -> bool {
        *destination = nullptr;
        if (!spanRef)
            return true;
        const uint32_t bytes = count * elementBytes;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, bytes, alignment))
            return false;
        context->nestedBodyBytes += bytes;
        if (!bytes)
            return true;
        const uint8_t *source = session->zoneMemory->blocks[4].data +
                                session->wire.cursor[4] - bytes;
        void *copy = RetailZoneLoadSessionAlloc(session, bytes, alignment);
        if (!copy)
            return false;
        std::memcpy(copy, source, bytes);
        *destination = copy;
        return true;
    };
    // A resolved root's arrays were streamed earlier, so they are copied out
    // of block 4 without consuming; an unconvertible pointer is counted and
    // leaves the arrays null (the caller then leaves the whole tree null).
    auto copyResolvedSpan = [&](const char *what, uint32_t spanRef, uint32_t count,
                                uint32_t elementBytes, uint32_t alignment,
                                void **destination) {
        *destination = nullptr;
        const uint32_t bytes = count * elementBytes;
        if (!spanRef || !bytes)
            return;
        const uint8_t *source = ResolveWorldBlock4Span(context, spanRef, bytes);
        if (!source)
        {
            ++context->xmodelUnresolvedPlainPointerCount;
            Com_Printf(0, "RetailWorld: xmodel collision %s unresolved plain-pointer "
                          "ref=0x%08x count=%u\n",
                       what, spanRef, (unsigned)count);
            return;
        }
        void *copy = RetailZoneLoadSessionAlloc(session, bytes, alignment);
        if (!copy)
            return;
        std::memcpy(copy, source, bytes);
        *destination = copy;
    };

    if (streamedRoot)
    {
        if (!copyStreamedSpan(nodesRef, tree->nodeCount, sizeof(XSurfaceCollisionNode), 16,
                              reinterpret_cast<void **>(&tree->nodes)) ||
            !copyStreamedSpan(leavesRef, tree->leafCount, sizeof(XSurfaceCollisionLeaf), 2,
                              reinterpret_cast<void **>(&tree->leafs)))
            return false;
    }
    else
    {
        copyResolvedSpan("nodes", nodesRef, tree->nodeCount, sizeof(XSurfaceCollisionNode),
                         alignof(XSurfaceCollisionNode),
                         reinterpret_cast<void **>(&tree->nodes));
        copyResolvedSpan("leafs", leavesRef, tree->leafCount, sizeof(XSurfaceCollisionLeaf),
                         alignof(XSurfaceCollisionLeaf),
                         reinterpret_cast<void **>(&tree->leafs));
        if ((tree->nodeCount && !tree->nodes) || (tree->leafCount && !tree->leafs))
        {
            *out = nullptr;
            return true;
        }
    }

    *out = tree;
    return true;
}

bool WidenXRigidVertListArray(RetailWorldLoadContext *context, uint32_t ref,
                              uint32_t count, XRigidVertList **out)
{
    if (!context || !context->session || !context->reader || !out)
        return false;
    *out = nullptr;
    if (!count)
        return true;
    if (count > UINT32_MAX / sizeof(XRigidVertList) || count > UINT32_MAX / 12u)
        return false;

    RetailZoneLoadSession *session = context->session;
    FsRetailFastfileReader *reader = context->reader;
    const uint8_t *raw = nullptr;
    bool streamed = false;
    if (ref == kInlineRef)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, count * 12u, 4))
            return false;
        raw = session->zoneMemory->blocks[4].data + session->wire.cursor[4] - count * 12u;
        context->nestedBodyBytes += count * 12u;
        streamed = true;
    }
    else if (ref != 0)
    {
        // DB_ConvertOffsetToPointer form: Load_XSurface only streams the
        // inline list (ReadRetailXSurfaceBody consumes nothing here). Resolve
        // the already-streamed block-4 span when the value is a real
        // converted offset; retail data also carries forward references (the
        // original loader's DB_ConvertOffsetToPointer just takes the address;
        // the bytes arrive later), which are not yet consumed. Keep those
        // unwidened and counted, never fabricated, and never consume: any
        // stream movement here would desync every later asset in the zone.
        raw = ResolveWorldBlock4Span(context, ref, count * 12u);
        if (!raw)
        {
            ++context->xmodelUnresolvedPlainPointerCount;
            Com_Printf(0, "RetailWorld: xmodel surface vertList unresolved plain-pointer ref=0x%08x count=%u\n",
                       ref, (unsigned)count);
            return true;
        }
    }
    else
    {
        // Non-inline null ref: Load_XSurface consumes nothing.
        return true;
    }

    XRigidVertList *lists = static_cast<XRigidVertList *>(
        RetailZoneLoadSessionAlloc(session, count * sizeof(XRigidVertList),
                                   alignof(XRigidVertList)));
    if (!lists)
        return false;
    std::memset(lists, 0, count * sizeof(XRigidVertList));
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t *entry = raw + i * 12u;
        lists[i].boneOffset = ReadLe16(entry + 0);
        lists[i].vertCount = ReadLe16(entry + 2);
        lists[i].triOffset = ReadLe16(entry + 4);
        lists[i].triCount = ReadLe16(entry + 6);
        // Load_XSurface reaches a list's collision tree only inside the
        // inline array branch (Load_XRigidVertListArray); an array resolved
        // from an already-streamed span publishes its scalar fields but its
        // tree refs are the walk reader's never-resolved pointer class, so
        // nothing is widened or consumed for them.
        if (streamed &&
            !WidenXSurfaceCollisionTree(context, ReadLe32(entry + 8),
                                        &lists[i].collisionTree))
            return false;
    }
    *out = lists;
    return true;
}

// XModelCollSurf_s wire layout (44 bytes, ILP32): collTris ref, numCollTris,
// mins[3], maxs[3], boneIdx, contents, surfFlags. Copy the scalar fields into
// the native record (48 bytes on LP64, with a real 8-byte collTris pointer).
void FillCollSurf(const uint8_t *entry, XModelCollSurf_s &out)
{
    out.numCollTris = static_cast<int>(ReadLe32(entry + 4));
    std::memcpy(out.mins, entry + 8, sizeof(out.mins));
    std::memcpy(out.maxs, entry + 20, sizeof(out.maxs));
    out.boneIdx = static_cast<int>(ReadLe32(entry + 32));
    out.contents = static_cast<int>(ReadLe32(entry + 36));
    out.surfFlags = static_cast<int>(ReadLe32(entry + 40));
}

// XModelCollTri_s is 48 bytes on every ABI (floats only), so the triangle
// array is a plain copy from the mirror.
XModelCollTri_s *CopyCollTris(RetailWorldLoadContext *context, const uint8_t *src,
                              uint32_t triCount)
{
    if (!src || !triCount)
        return nullptr;
    XModelCollTri_s *out = static_cast<XModelCollTri_s *>(RetailZoneLoadSessionAlloc(
        context->session, static_cast<std::size_t>(triCount) * sizeof(XModelCollTri_s),
        alignof(XModelCollTri_s)));
    if (!out)
        return nullptr;
    std::memcpy(out, src, static_cast<std::size_t>(triCount) * sizeof(XModelCollTri_s));
    return out;
}

// The XModel boneNames array is not POD name data: each u16 is an index into
// the zone's script-string table, which the engine remaps to a runtime
// scr_string_t through Load_ScriptStringArray. Rebuild that table (shared on
// the session with the XAnim/Weapon decoders) and intern each bone name the
// same way; the zone stays resident for the process, so the reference is not
// tracked for unload.
bool EnsureWorldScriptTable(RetailZoneLoadSession *session)
{
    if (session->scriptStringOffsets)
        return true;
    const uint32_t count = session->scriptStringCount;
    const XBlock &block = session->zoneMemory->blocks[4];
    if (!count || count > 65536 || count > block.size / 4u)
        return false;
    uint32_t *offsets = static_cast<uint32_t *>(
        RetailZoneLoadSessionAlloc(session, static_cast<std::size_t>(count) * 4u, 4));
    if (!offsets)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        offsets[i] = UINT32_MAX;
    uint32_t pos = count * 4u;
    XBlock blocks[9]{};
    for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
        blocks[blockIndex] = session->zoneMemory->blocks[blockIndex];
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t slot = ReadLe32(block.data + i * 4u);
        if (slot == kInlineRef)
        {
            uint32_t end = pos;
            while (end < block.size && block.data[end] != 0)
                ++end;
            if (end >= block.size)
                return false;
            offsets[i] = pos;
            pos = end + 1;
        }
        else if (slot != 0)
        {
            RetailWireToken token{};
            RetailPtr32 encoded{};
            encoded.encoded = slot;
            if (RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) &&
                token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4 &&
                token.offset < block.size)
            {
                uint32_t end = token.offset;
                bool printable = true;
                while (end < block.size && block.data[end] != 0)
                {
                    const unsigned char c = block.data[end];
                    if (c < 0x20 || c >= 0x7f)
                    {
                        printable = false;
                        break;
                    }
                    ++end;
                }
                if (printable && end < block.size)
                    offsets[i] = token.offset;
            }
        }
    }
    session->scriptStringOffsets = offsets;
    return true;
}

const char *WorldScriptString(RetailZoneLoadSession *session, uint32_t wireIndex)
{
    if (!EnsureWorldScriptTable(session))
        return nullptr;
    if (wireIndex >= session->scriptStringCount)
        return nullptr;
    const uint32_t offset = session->scriptStringOffsets[wireIndex];
    if (offset == UINT32_MAX)
        return nullptr;
    return reinterpret_cast<const char *>(session->zoneMemory->blocks[4].data + offset);
}

uint16_t WorldInternScriptString(const char *str)
{
    if (!str || !str[0])
        return 0;
    const uint32_t found = SL_FindString(str);
    if (found)
    {
        SL_AddRefToString(found);
        return static_cast<uint16_t>(found < 0x10000u ? found : 0);
    }
    const uint32_t id = SL_GetString(str, 4u);
    return static_cast<uint16_t>(id < 0x10000u ? id : 0);
}

void RemapXModelBoneNames(RetailWorldLoadContext *context, uint16_t *boneNames, uint32_t numBones)
{
    if (!boneNames || !numBones)
        return;
    for (uint32_t i = 0; i < numBones; ++i)
    {
        const char *name = WorldScriptString(context->session, boneNames[i]);
        boneNames[i] = name ? WorldInternScriptString(name) : 0;
    }
}

// Up-front record-coherence gate: a widened record with count>0 and a null
// pointer is exactly the state the runtime dereferences later
// (XModelGetStaticBounds over collSurfs, DObj bone lookups over boneNames).
// Fail the load here, naming the asset, instead of surviving to a guest-boot
// crash. The host boot test exercises the same widener, so a new field gap
// fails there in seconds.
bool ValidateWidenedXModel(const XModel *model)
{
    bool ok = true;
    const char *name = (model->name && model->name[0]) ? model->name : "<unnamed>";
    if (model->numBones && !model->boneNames)
    {
        Com_Printf(0, "RetailWorld: xmodel '%s' numBones=%u with null boneNames\n", name,
                   (unsigned)model->numBones);
        ok = false;
    }
    if (model->numBones && !model->boneInfo)
    {
        Com_Printf(0, "RetailWorld: xmodel '%s' numBones=%u with null boneInfo\n", name,
                   (unsigned)model->numBones);
        ok = false;
    }
    if (model->numCollSurfs && !model->collSurfs)
    {
        Com_Printf(0, "RetailWorld: xmodel '%s' numCollSurfs=%d with null collSurfs\n", name,
                   model->numCollSurfs);
        ok = false;
    }
    else if (model->collSurfs)
    {
        for (int i = 0; i < model->numCollSurfs; ++i)
        {
            const XModelCollSurf_s &cs = model->collSurfs[i];
            if (cs.numCollTris && !cs.collTris)
            {
                Com_Printf(0, "RetailWorld: xmodel '%s' collSurf %d numCollTris=%d with null collTris\n",
                           name, i, cs.numCollTris);
                ok = false;
            }
        }
    }
    return ok;
}

} // namespace

bool RetailWorldWidenXModel(RetailWorldLoadContext *context, uint32_t slotRef, XModel **out,
                            uint32_t slotOffset, uint32_t *insertSlotOffsetOut)
{
    if (!context || !context->session || !context->reader || !out)
        return false;
    *out = nullptr;
    if (insertSlotOffsetOut)
        *insertSlotOffsetOut = 0;
    if (!slotRef)
    {
        // A declared null slot binds null; record it so a later slot
        // indirection onto this slot binds null the same way.
        if (slotOffset)
            RecordNestedSlot(context, slotOffset, ASSET_TYPE_XMODEL, {});
        return true;
    }
    if (slotRef != kInlineRef && slotRef != kInsertRef)
    {
        XAssetHeader found{};
        if (!ResolveNestedAlias(context, slotRef, ASSET_TYPE_XMODEL, &found))
        {
            Com_Printf(0, "RetailWorld: xmodel slot alias 0x%08x resolve failed\n", slotRef);
            return false;
        }
        if (!found.model)
        {
            // The referenced slot is null on the wire: the instance binds
            // null exactly like the original's copied value. The draw-side
            // null-model guard owns that state (as recorded, never synthesized).
            // Name the exact target slot for the first few null binds so a
            // real unrecorded insert slot can be told from a declared null
            // without flooding a 12k-static-model zone.
            static uint32_t s_nullBindLog = 0;
            if (s_nullBindLog < 8)
            {
                ++s_nullBindLog;
                RetailWireToken token{};
                if (DecodeFastfileToken(context->reader, slotRef, 0, 1u << 4, &token))
                    Com_Printf(0, "RetailWorld: xmodel slot alias 0x%08x binds null target=%u "
                                  "recorded=%u\n",
                               slotRef, token.offset, context->nestedCount);
                else
                    Com_Printf(0, "RetailWorld: xmodel slot alias 0x%08x binds null "
                                  "(undecodable target)\n",
                               slotRef);
            }
            // Record this slot's own null bind too (matching the declared-
            // null branch above): a sibling slot could still alias onto
            // *this* slot, and without recording it that sibling lookup
            // would hit an unrecorded-slot failure instead of the real,
            // legitimate null.
            if (slotOffset)
                RecordNestedSlot(context, slotOffset, ASSET_TYPE_XMODEL, {});
            *out = nullptr;
            return true;
        }
        // This slot's own resolved result must be recorded too -- a
        // sibling slot's alias can point at *this* slot (materialHandles-
        // style sibling indirection), and without this a slot that itself
        // holds a plain alias would leave that sibling reference
        // unresolved even though the model it needs was widened
        // successfully right here.
        if (slotOffset)
            RecordNestedSlot(context, slotOffset, ASSET_TYPE_XMODEL, found);
        *out = found.model;
        return true;
    }
    RetailZoneLoadSession *session = context->session;
    FsRetailFastfileReader *reader = context->reader;

    // HandleAssetSlot's -2 form (DB_InsertPointer): reserve the 4-byte block-4
    // pointer slot before the body streams, then write the widened result
    // there. In our pristine-wire mirror that reservation is the bookkeeping
    // later DB_ConvertOffsetToAlias aliases resolve through, so it must be
    // recorded, not merely advanced past. (The top-level driver defers
    // registration to the session dispatcher; it re-records this slot with
    // the registered header through insertSlotOffsetOut.)
    uint32_t insertSlotOffset = 0;
    if (slotRef == kInsertRef)
    {
        uint8_t *insertSlot = RetailWireBlocksAlloc(&session->wire, 4, 4, 4);
        if (!insertSlot)
        {
            Com_Printf(0, "RetailWorld: xmodel insert slot reservation failed\n");
            return false;
        }
        insertSlotOffset = static_cast<uint32_t>(
            insertSlot - session->zoneMemory->blocks[4].data);
    }

    uint8_t root[220]{};
    // XModel roots are Load_Stream'd into the temp block (block 0), for both
    // top-level directory bodies and nested world draw-instance slots.  Land
    // and rewind the root so the field decoder reads the exact bytes just
    // streamed; following arrays/assets continue in block 4.
    const uint32_t rootStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, sizeof(root), 4))
    {
        Com_Printf(0, "RetailWorld: nested xmodel root unreadable\n");
        return false;
    }
    RetailWireBlocksRewind(&session->wire, 0, rootStart);
    if (!RetailWireBlocksRead(&session->wire, 0, root, sizeof(root)))
    {
        Com_Printf(0, "RetailWorld: nested xmodel root unreadable\n");
        return false;
    }
    const uint32_t nameRef = ReadLe32(root + 0);
    const uint32_t numBones = root[4];
    const uint32_t numRootBones = root[5];
    const uint32_t numSurfs = root[6];
    const uint32_t lodRampType = root[7];
    const uint32_t animatedBones = numBones - numRootBones;

    XModel *model = static_cast<XModel *>(
        RetailZoneLoadSessionAlloc(session, sizeof(XModel), alignof(XModel)));
    if (!model)
        return false;
    std::memset(model, 0, sizeof(*model));
    model->numBones = static_cast<uint8_t>(numBones);
    model->numRootBones = static_cast<uint8_t>(numRootBones);
    model->numsurfs = static_cast<uint8_t>(numSurfs);
    model->lodRampType = static_cast<uint8_t>(lodRampType);

    uint32_t nameOffset = 0;
    const char *name = nullptr;
    if (!RetailWorldStreamName(context, nameRef, &nameOffset, &name))
    {
        Com_Printf(0, "RetailWorld: nested xmodel name unreadable\n");
        return false;
    }
    if (name)
    {
        const std::size_t nameBytes = std::strlen(name) + 1;
        char *nameCopy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, nameBytes, 1));
        if (!nameCopy)
            return false;
        std::memcpy(nameCopy, name, nameBytes);
        model->name = nameCopy;
    }
    // Six bone arrays "XModel and nested
    // collision"): all POD, identical element size on both ABIs (no
    // restride possible), consumed only when exactly inline-declared.
    void *copy = nullptr;
    if (!ConsumeRootPlainArray(context, ReadLe32(root + 8), numBones * 2u, 2, &copy, 2))
        return false;
    model->boneNames = static_cast<uint16_t *>(copy);
    RemapXModelBoneNames(context, model->boneNames, numBones);
    if (!ConsumeRootPlainArray(context, ReadLe32(root + 12), animatedBones, 1, &copy, 1))
        return false;
    model->parentList = static_cast<uint8_t *>(copy);
    if (!ConsumeRootPlainArray(context, ReadLe32(root + 16), animatedBones * 8u, 2, &copy, 2))
        return false;
    model->quats = static_cast<int16_t *>(copy);
    if (!ConsumeRootPlainArray(context, ReadLe32(root + 20), animatedBones * 16u, 4, &copy, 4))
        return false;
    model->trans = static_cast<float *>(copy);
    if (!ConsumeRootPlainArray(context, ReadLe32(root + 24), numBones, 1, &copy, 1))
        return false;
    model->partClassification = static_cast<uint8_t *>(copy);
    if (!ConsumeRootPlainArray(context, ReadLe32(root + 28), numBones * 32u, 4, &copy, 4))
        return false;
    model->baseMat = static_cast<DObjAnimMat *>(copy);

    // lodInfo[4] (wire +40, 112 bytes total, 28 bytes/entry): plain POD,
    // identical size both ABIs -- bulk-read then per-element member copy
    // (lets the compiler place native offsets; only the wire side needs
    // manual byte math).
    // lodInfo[4] is inline in the already-consumed 220-byte root at +40;
    // do not consume a second 112-byte span (that would desynchronize every
    // following XSurface/material body).
    const uint8_t *lodRaw = root + 40;
    for (int lod = 0; lod < 4; ++lod)
    {
        const uint8_t *entry = lodRaw + lod * 28;
        XModelLodInfo &info = model->lodInfo[lod];
        info.dist = ReadLeFloat(entry + 0);
        info.numsurfs = ReadLe16(entry + 4);
        info.surfIndex = ReadLe16(entry + 6);
        for (int p = 0; p < 4; ++p)
            info.partBits[p] = static_cast<int>(ReadLe32(entry + 8 + p * 4));
        info.lod = entry[24];
        info.smcIndexPlusOne = entry[25];
        info.smcAllocBits = entry[26];
        info.unused = entry[27];
    }
    // GPU audit (a2): lod/smc fields feed the smodel-cache allocator
    // directly (R_CacheStaticModelSurface -> SMC_Allocate). Out-of-range
    // values corrupt the cache freelist (CPU heap OOB) and later fault the
    // GPU with no validation error, so fail loudly here instead.
    for (int lod = 0; lod < 4; ++lod)
    {
        const XModelLodInfo &info = model->lodInfo[lod];
        if (info.numsurfs &&
            (uint32_t)info.surfIndex + (uint32_t)info.numsurfs > numSurfs)
        {
            Com_Printf(0, "RetailWorld: xmodel '%s' lod %d surf range OOB (surfIndex=%u numsurfs=%u modelSurfs=%u)\n",
                       model->name ? model->name : "(null)", lod,
                       (unsigned)info.surfIndex, (unsigned)info.numsurfs,
                       (unsigned)numSurfs);
            return false;
        }
        if (info.smcIndexPlusOne)
        {
            if (info.smcAllocBits < 4 || info.smcAllocBits > 9)
            {
                Com_Printf(0, "RetailWorld: xmodel '%s' lod %d smcAllocBits OOB (%u, want 4..9, smcIndexPlusOne=%u)\n",
                           model->name ? model->name : "(null)", lod,
                           (unsigned)info.smcAllocBits, (unsigned)info.smcIndexPlusOne);
                return false;
            }
            if ((uint32_t)info.smcIndexPlusOne - 1u >= 4u)
            {
                Com_Printf(0, "RetailWorld: xmodel '%s' lod %d smcIndex OOB (plusOne=%u, want 1..4)\n",
                           model->name ? model->name : "(null)", lod,
                           (unsigned)info.smcIndexPlusOne);
                return false;
            }
        }
    }

    // numsurfs XSurfaces (wire 56 -> native 80 bytes) and numsurfs Material
    // slots: both bulk-then-resolve, matching Load_XSurfaceArray/
    // Load_MaterialHandleArray's own order (an inline nested body for
    // element i must not eat element i+1's still-unread fixed record).
    const uint32_t surfsRef = ReadLe32(root + 32);
    if (surfsRef != 0 && surfsRef != kInlineRef)
    {
        Com_Printf(0, "RetailWorld: xmodel surfs unsupported ref=0x%08x (must be inline)\n", surfsRef);
        return false;
    }
    if (surfsRef == kInlineRef && numSurfs)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, numSurfs * 56u, 4))
        {
            Com_Printf(0, "RetailWorld: nested xmodel surfs unreadable\n");
            return false;
        }
        const uint32_t rawStart = session->wire.cursor[4] - numSurfs * 56u;
        uint8_t *rawSurfs = session->zoneMemory->blocks[4].data + rawStart;
        context->nestedBodyBytes += numSurfs * 56u;
        XSurface *surfs = static_cast<XSurface *>(
            RetailZoneLoadSessionAlloc(session, numSurfs * sizeof(XSurface), alignof(XSurface)));
        if (!surfs)
            return false;
        for (uint32_t s = 0; s < numSurfs; ++s)
        {
            if (!WidenOneXSurface(context, rawSurfs + s * 56u, &surfs[s]))
            {
                Com_Printf(0, "RetailWorld: nested xmodel surf %u/%u widen failed\n", s, numSurfs);
                return false;
            }
        }
        model->surfs = surfs;
    }

    const uint32_t materialHandlesRef = ReadLe32(root + 36);
    if (materialHandlesRef != 0 && materialHandlesRef != kInlineRef)
    {
        Com_Printf(0, "RetailWorld: xmodel materialHandles unsupported ref=0x%08x (must be inline)\n",
                   materialHandlesRef);
        return false;
    }
    if (materialHandlesRef == kInlineRef && numSurfs)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, numSurfs * 4u, 4))
        {
            Com_Printf(0, "RetailWorld: nested xmodel material slots unreadable\n");
            return false;
        }
        const uint32_t rawStart = session->wire.cursor[4] - numSurfs * 4u;
        uint8_t *rawMats = session->zoneMemory->blocks[4].data + rawStart;
        context->nestedBodyBytes += numSurfs * 4u;
        Material **handles = static_cast<Material **>(
            RetailZoneLoadSessionAlloc(session, numSurfs * sizeof(Material *), alignof(Material *)));
        if (!handles)
            return false;
        for (uint32_t s = 0; s < numSurfs; ++s)
        {
            // The slot's own block-4 offset: materialHandles aliases are
            // DB_ConvertOffsetToAlias slot indirections onto sibling slots
            // of this same array (surfaces sharing materials), so every
            // widened slot must be recorded for later resolution.
            if (!RetailWorldWidenMaterial(context, ReadLe32(rawMats + s * 4u), &handles[s],
                                          rawStart + s * 4u))
            {
                Com_Printf(0, "RetailWorld: nested xmodel material %u/%u widen failed\n", s, numSurfs);
                return false;
            }
            if (!handles[s])
            {
                // an xmodel's material handle is consumer-visible; a
                // dead inline body cannot be bound as null.
                Com_Printf(0, "RetailWorld: nested xmodel material %u/%u unresolved (dead inline body)\n", s, numSurfs);
                return false;
            }
        }
        model->materialHandles = handles;
    }

    // Root tail: collision/physics data. Consume the 44-byte wire entries
    // exactly (matching ReadRetailXModelBody) and widen them into native
    // XModelCollSurf_s records (48 bytes on LP64, with a real collTris
    // pointer). A count>0 with a null collSurfs/collTris is what
    // XModelGetStaticBounds/G_GetModelBounds dereference, so never publish
    // that state: unresolved collision data drops the count to 0.
    const uint32_t collSurfsRef = ReadLe32(root + 152);
    const uint32_t numCollSurfs = ReadLe32(root + 156);
    model->numCollSurfs = static_cast<int>(numCollSurfs);
    model->contents = static_cast<int>(ReadLe32(root + 160));

    XModelCollSurf_s *nativeCollSurfs = nullptr;
    if (numCollSurfs)
    {
        nativeCollSurfs = static_cast<XModelCollSurf_s *>(RetailZoneLoadSessionAlloc(
            session, static_cast<std::size_t>(numCollSurfs) * sizeof(XModelCollSurf_s),
            alignof(XModelCollSurf_s)));
        if (!nativeCollSurfs)
            return false;
        std::memset(nativeCollSurfs, 0,
                    static_cast<std::size_t>(numCollSurfs) * sizeof(XModelCollSurf_s));
    }
    model->collSurfs = nativeCollSurfs;

    if (collSurfsRef == kInlineRef)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, numCollSurfs * 44u, 4))
        {
            Com_Printf(0, "RetailWorld: nested xmodel collSurfs unreadable\n");
            return false;
        }
        const uint32_t base = session->wire.cursor[4] - numCollSurfs * 44u;
        context->nestedBodyBytes += numCollSurfs * 44u;
        for (uint32_t c = 0; c < numCollSurfs; ++c)
        {
            const uint8_t *entry = session->zoneMemory->blocks[4].data + base + c * 44u;
            XModelCollSurf_s &cs = nativeCollSurfs[c];
            FillCollSurf(entry, cs);
            const uint32_t trisRef = ReadLe32(entry);
            if (!trisRef)
                continue;
            const uint32_t triCount = static_cast<uint32_t>(cs.numCollTris);
            if (!RetailZoneLoadSessionReadStream(session, reader, 4, triCount * 48u, 4))
            {
                Com_Printf(0, "RetailWorld: nested xmodel collSurf triangles unreadable\n");
                return false;
            }
            const uint32_t triBase = session->wire.cursor[4] - triCount * 48u;
            context->nestedBodyBytes += triCount * 48u;
            if (trisRef == kInlineRef)
                cs.collTris = CopyCollTris(context,
                    session->zoneMemory->blocks[4].data + triBase, triCount);
            else
                cs.collTris = CopyCollTris(context,
                    ResolveWorldBlock4Span(context, trisRef, triCount * 48u), triCount);
        }
    }
    else if (collSurfsRef != 0)
    {
        const uint8_t *wireBase =
            ResolveWorldBlock4Span(context, collSurfsRef, numCollSurfs * 44u);
        if (!wireBase)
        {
            ++context->xmodelUnresolvedPlainPointerCount;
            Com_Printf(0, "RetailWorld: xmodel collSurfs unresolved plain-pointer ref=0x%08x\n",
                       collSurfsRef);
            model->collSurfs = nullptr;
            model->numCollSurfs = 0;
        }
        else
        {
            for (uint32_t c = 0; c < numCollSurfs; ++c)
            {
                const uint8_t *entry = wireBase + c * 44u;
                XModelCollSurf_s &cs = nativeCollSurfs[c];
                FillCollSurf(entry, cs);
                const uint32_t trisRef = ReadLe32(entry);
                if (trisRef)
                    cs.collTris = CopyCollTris(context,
                        ResolveWorldBlock4Span(context, trisRef,
                                               static_cast<uint32_t>(cs.numCollTris) * 48u),
                        static_cast<uint32_t>(cs.numCollTris));
            }
        }
    }

    model->radius = ReadLeFloat(root + 168);
    std::memcpy(model->mins, root + 172, sizeof(model->mins));
    std::memcpy(model->maxs, root + 184, sizeof(model->maxs));
    model->numLods = static_cast<int16_t>(ReadLe16(root + 196));
    model->collLod = static_cast<int16_t>(ReadLe16(root + 198));
    // GPU audit (a3): numLods sizes every lod loop the renderer runs
    // (XModelGetLodForDist, R_GetStaticModelId). Values outside 0..4 would
    // overrun lodInfo[4] on the draw path.
    if (model->numLods < 0 || model->numLods > 4)
    {
        Com_Printf(0, "RetailWorld: xmodel '%s' numLods OOB (%d, want 0..4)\n",
                   model->name ? model->name : "(null)", (int)model->numLods);
        return false;
    }

    const uint32_t boneInfoRef = ReadLe32(root + 164);
    if (!ConsumeRootPlainArray(context, boneInfoRef, numBones * 40u, 4, &copy, 40))
        return false;
    // XBoneInfo is 40 float-only bytes (bounds[2][3]+offset[3]+radiusSquared)
    // on both ABIs, so the consumed retail array is already native layout.
    // Publishing null here made every model with bones return null
    // boneInfoArray entries to R_UpdateSceneEntBounds, which null-derefs
    // v35->bounds[0] on the first bounded scene entity after spawn.
    model->boneInfo = static_cast<XBoneInfo *>(copy);

    // physPreset is the real Load_PhysPresetPtr slot. The nested body
    // is widened and registered through Load_PhysPresetAsset (and the -2
    // insert form's reserved block-4 slot recorded) instead of being
    // consumed and nulled; a non-null slot that cannot be resolved fails
    // loudly. physGeoms stays walk-consumed (collision-only, no widener).
    PhysPreset *physPreset = nullptr;
    const uint32_t physPresetRef = ReadLe32(root + 212);
    if (!RetailWorldWidenPhysPreset(context, physPresetRef, &physPreset, 0, nullptr))
    {
        Com_Printf(0, "RetailWorld: nested xmodel physPreset slot 0x%08x failed\n",
                   physPresetRef);
        return false;
    }

    const uint32_t physGeomsRef = ReadLe32(root + 216);
    if (physGeomsRef == kInlineRef)
    {
        // The 44-byte PhysGeomList root, its geom array, and any inline
        // brush wrappers are collision-only data. Consume them exactly like
        // the walk-only reader (ReadRetailPhysGeomListBody) so the shared
        // stream stays byte-exact; never widen or activate them here.
        const uint32_t geomsStart = (session->wire.cursor[4] + 3u) & ~3u;
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, 44, 4))
        {
            Com_Printf(0, "RetailWorld: nested xmodel physGeoms root unreadable\n");
            return false;
        }
        context->nestedBodyBytes += 44u;
        if (!ReadRetailPhysGeomListBody(session, reader, geomsStart))
        {
            Com_Printf(0, "RetailWorld: nested xmodel physGeoms body unreadable\n");
            return false;
        }
    }
    else if (physGeomsRef != 0 && physGeomsRef != kInsertRef)
    {
        ++context->xmodelUnresolvedPlainPointerCount;
        Com_Printf(0, "RetailWorld: xmodel physGeoms unresolved ref=0x%08x\n", physGeomsRef);
    }

    model->physPreset = physPreset;
    model->physGeoms = nullptr;

    if (!ValidateWidenedXModel(model))
        return false;

    XAssetHeader registered{};
    registered.model = model;
    if (!context->deferXModelRegistration)
    {
        registered = RetailZoneLoadSessionRegister(session, ASSET_TYPE_XMODEL, registered);
        if (!registered.model)
        {
            Com_Printf(0, "RetailWorld: nested xmodel register failed\n");
            return false;
        }
    }
    uint32_t recordNameRef = nameRef;
    if (model->name && nameRef == kInlineRef)
    {
        RetailPtr32 encodedName{};
        if (!RetailWireTokenEncode(4, nameOffset, &encodedName))
            return false;
        recordNameRef = encodedName.encoded;
    }
    if (model->name && !RecordNestedName(context, recordNameRef, ASSET_TYPE_XMODEL, registered))
    {
        Com_Printf(0, "RetailWorld: nested xmodel has no recordable name\n");
        return false;
    }
    if (slotOffset &&
        !RecordNestedSlot(context, slotOffset, ASSET_TYPE_XMODEL, registered))
    {
        Com_Printf(0, "RetailWorld: nested xmodel slot %u has no recordable slot entry\n",
                   slotOffset);
        return false;
    }
    if (insertSlotOffset)
    {
        if (insertSlotOffsetOut)
            *insertSlotOffsetOut = insertSlotOffset;
        // The top-level driver handles registration itself and re-records the
        // reserved slot with the registered header; nested callers register
        // here and record now.
        if (!context->deferXModelRegistration &&
            !RecordNestedSlot(context, insertSlotOffset, ASSET_TYPE_XMODEL, registered))
        {
            Com_Printf(0, "RetailWorld: nested xmodel insert slot %u has no recordable entry\n",
                       insertSlotOffset);
            return false;
        }
    }
    ++context->widenedXModelCount;
    *out = registered.model;
    RETAIL_ASSET_TRACE(0, "RetailWorld: xmodel widened '%s' numsurfs=%u numBones=%u numLods=%d\n",
                       model->name ? model->name : "(null)", numSurfs, numBones,
                       static_cast<int>(model->numLods));
    return true;
}

bool RetailWalkLiveLoadXModel(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                              uint32_t header, XAssetHeader *out,
                              RetailWorldLoadContext *context,
                              uint32_t *insertSlotOffsetOut)
{
    if (!out || !context || !session || !reader)
        return false;
    // Top-level registration belongs to the session dispatcher; toggled per
    // call, which is safe because widening is synchronous. Restored to the
    // world-path default (false) here so a later world load on this shared
    // context starts from the known state.
    context->deferXModelRegistration = true;
    XModel *model = nullptr;
    const bool ok = RetailWorldWidenXModel(context, header, &model, 0, insertSlotOffsetOut);
    context->deferXModelRegistration = false;
    if (!ok || !model)
        return false;
    out->model = model;
    return true;
}

bool RetailWorldRecordZoneSlot(RetailWorldLoadContext *context, uint32_t slotOffset,
                               XAssetType type, XAssetHeader header)
{
    return RecordNestedSlot(context, slotOffset, type, header);
}

bool RetailWorldRecordDeferredSlotName(RetailWorldLoadContext *context, uint32_t slotOffset,
                                       XAssetType type, const char *name, bool bindNull)
{
    if (!context || !slotOffset || !name || !name[0] ||
        context->deferredSlotCount >= RetailWorldLoadContext::kDeferredSlotCap)
        return false;
    RetailWorldLoadContext::RetailWorldDeferredSlot &entry =
        context->deferredSlots[context->deferredSlotCount++];
    entry.slotOffset = slotOffset;
    entry.type = type;
    entry.bindNull = bindNull;
    std::strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    return true;
}

bool RetailWorldFindDeferredSlotName(RetailWorldLoadContext *context, uint32_t slotRef,
                                     XAssetType type, char *name, uint32_t nameCapacity,
                                     bool *bindNull)
{
    if (!context || !context->reader || !slotRef || !name || nameCapacity == 0)
        return false;
    name[0] = '\0';
    if (bindNull)
        *bindNull = false;
    RetailWireToken token{};
    if (!DecodeFastfileToken(context->reader, slotRef, 0, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
        return false;
    for (uint32_t i = context->deferredSlotCount; i > 0; --i)
    {
        const RetailWorldLoadContext::RetailWorldDeferredSlot &entry =
            context->deferredSlots[i - 1];
        if (entry.slotOffset == token.offset && entry.type == type)
        {
            std::strncpy(name, entry.name, nameCapacity - 1);
            name[nameCapacity - 1] = '\0';
            if (bindNull)
                *bindNull = entry.bindNull;
            return true;
        }
    }
    return false;
}

bool RetailWorldRecordBody(RetailWorldLoadContext *context, uint32_t bodyOffset,
                           XAssetType type, XAssetHeader header)
{
    if (!context)
        return false;
    // A zero cursor or an anonymous/null header has no stable body identity
    // that an alias can address. It is therefore not a ledger failure; any
    // later alias still fails through the normal strict resolver.
    if (!bodyOffset || !header.data)
        return true;
    if (context->nestedCount >= RetailWorldLoadContext::kNestedCap)
        return false;
    const uint32_t index = context->nestedCount;
    RetailWorldNestedName &entry = context->nested[context->nestedCount++];
    entry.bodyOffset = bodyOffset;
    entry.nameOffset = 0;
    entry.slotOffset = 0;
    entry.type = type;
    entry.header = header;
    WorldIndexPut(context->nested, context->nestedBodyIndex,
                  RetailWorldLoadContext::kNestedIndexMask, bodyOffset,
                  static_cast<uint32_t>(type), index,
                  [&](const RetailWorldNestedName &stored)
                  { return stored.bodyOffset == bodyOffset && stored.type == type; });
    return true;
}

bool RetailWorldRecordSoundBody(RetailWorldLoadContext *context, uint32_t block4Offset,
                                uint8_t kind, void *object)
{
    if (!context || !block4Offset || !object)
        return false;
    if (context->soundBodyCount >= RetailWorldLoadContext::kSoundBodyCap)
        return false;
    const uint32_t index = context->soundBodyCount;
    RetailWorldLoadContext::RetailSoundBody &entry =
        context->soundBodies[context->soundBodyCount++];
    entry.block4Offset = block4Offset;
    entry.object = object;
    entry.kind = kind;
    WorldIndexPut(context->soundBodies, context->soundBodyIndex,
                  RetailWorldLoadContext::kSoundBodyIndexMask, block4Offset,
                  static_cast<uint32_t>(kind), index,
                  [&](const RetailWorldLoadContext::RetailSoundBody &stored)
                  { return stored.block4Offset == block4Offset && stored.kind == kind; });
    return true;
}

void *RetailWorldFindSoundBody(RetailWorldLoadContext *context, uint32_t block4Offset,
                               uint8_t kind)
{
    if (!context || !block4Offset)
        return nullptr;
    // Newest entry wins on a repeated key, matching the old youngest-first
    // scan (a same-offset re-widen is the newest object; an earlier alias
    // already bound the earlier one).
    const RetailWorldLoadContext::RetailSoundBody *entry = WorldIndexFind(
        context->soundBodies, context->soundBodyIndex,
        RetailWorldLoadContext::kSoundBodyIndexMask, block4Offset,
        static_cast<uint32_t>(kind),
        [&](const RetailWorldLoadContext::RetailSoundBody &stored)
        { return stored.block4Offset == block4Offset && stored.kind == kind; });
    return entry ? entry->object : nullptr;
}

// Shared by the top-level and nested PhysPreset wideners: a body already
// consumed by ReadRetailPhysPresetBody (rootStart/nameStart/destStart name
// its exact block-0/block-4 bytes) is widened field by field into the
// native PhysPreset (all scalars same-size, pointer fields resolved/copied)
// and registered through the engine's Load_PhysPresetAsset owner. The
// callers own the temp-cursor push/pop semantics.
bool WidenConsumedPhysPreset(RetailZoneLoadSession *session, uint32_t rootStart,
                             uint32_t nameStart, uint32_t destStart, XAssetHeader *out)
{
    constexpr uint32_t kInlineRef = 0xffffffffu;
    if (!session || !out)
        return false;
    *out = XAssetHeader{};
    const uint8_t *root = session->zoneMemory->blocks[0].data + rootStart;
    const uint8_t *block4 = session->zoneMemory->blocks[4].data;
    const uint32_t block4Size = session->zoneMemory->blocks[4].size;
    auto copyString = [&](uint32_t ref, uint32_t inlineStart) -> char *
    {
        if (!ref)
            return nullptr;
        uint32_t off = inlineStart;
        if (ref != kInlineRef)
        {
            RetailWireToken token{};
            RetailPtr32 encoded{};
            encoded.encoded = ref;
            XBlock blocks[9]{};
            for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
                blocks[blockIndex] = session->zoneMemory->blocks[blockIndex];
            if (!RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4 ||
                token.offset >= block4Size)
                return nullptr;
            off = token.offset;
        }
        if (off == UINT32_MAX || off >= block4Size)
            return nullptr;
        uint32_t end = off;
        while (end < block4Size && block4[end] != 0)
            ++end;
        if (end >= block4Size)
            return nullptr;
        const uint32_t bytes = end - off + 1u;
        char *copy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, bytes, 1));
        if (!copy)
            return nullptr;
        std::memcpy(copy, block4 + off, bytes);
        return copy;
    };
    PhysPreset *preset = static_cast<PhysPreset *>(
        RetailZoneLoadSessionAlloc(session, sizeof(PhysPreset), alignof(PhysPreset)));
    if (!preset)
        return false;
    std::memset(preset, 0, sizeof(*preset));
    preset->name = copyString(ReadLe32(root), nameStart);
    preset->sndAliasPrefix = copyString(ReadLe32(root + 28), destStart);
    preset->type = static_cast<int>(ReadLe32(root + 4));
    preset->mass = ReadLeFloat(root + 8);
    preset->bounce = ReadLeFloat(root + 12);
    preset->friction = ReadLeFloat(root + 16);
    preset->bulletForceScale = ReadLeFloat(root + 20);
    preset->explosiveForceScale = ReadLeFloat(root + 24);
    preset->piecesSpreadFraction = ReadLeFloat(root + 32);
    preset->piecesUpwardVelocity = ReadLeFloat(root + 36);
    preset->tempDefaultToCylinder = root[40] != 0;
    XAssetHeader tx{};
    tx.physPreset = preset;
    Load_PhysPresetAsset(&tx);
    if (!tx.physPreset)
    {
        Com_Printf(0, "WidenConsumedPhysPreset: registration failed for '%s'\n",
                   preset->name ? preset->name : "(null)");
        return false;
    }
    *out = tx;
    return true;
}

// B1 PhysPreset small root: the walk reader consumes the 44-byte root and
// its two XStrings byte-exactly; widen the same bytes into the native
// PhysPreset (all scalars same-size, pointer fields resolved/copied) and
// register through the engine's Load_PhysPresetAsset owner.
bool RetailWalkLiveLoadPhysPreset(RetailZoneLoadSession *session,
                                  FsRetailFastfileReader *reader, XAssetHeader *out)
{
    constexpr uint32_t kInlineRef = 0xffffffffu;
    if (!session || !reader || !out || !session->active)
        return false;
    *out = XAssetHeader{};
    const uint32_t savedCursor0 = session->wire.cursor[0];
    RetailWalkDirectoryRecord consumed{};
    consumed.header = kInlineRef;
    RetailWalkDirectoryResult scratch{};
    uint32_t rootStart = 0;
    uint32_t nameStart = UINT32_MAX;
    uint32_t destStart = UINT32_MAX;
    if (!ReadRetailPhysPresetBody(session, reader, &consumed, &scratch, &rootStart,
                                  &nameStart, &destStart))
    {
        RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadPhysPreset: body consume failed\n");
        return false;
    }
    XAssetHeader tx{};
    if (!WidenConsumedPhysPreset(session, rootStart, nameStart, destStart, &tx))
    {
        RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
        return false;
    }
    *out = tx;
    RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
    return true;
}

// Nested PhysPreset: the exact port equivalent of Load_PhysPresetPtr
// (db_load.cpp:2953) for pointer fields inside widened assets (XModel@212,
// DynEntityDef@48). Null binds null; an alias resolves through the zone
// ledger/registry like every other nested slot; the -2 insert form reserves
// its block-4 DB_InsertPointer slot before the body streams, widens and
// registers the body, and records the reserved slot (plus the declaring
// field slot when the caller has one) so later DB_ConvertOffsetToAlias
// references bind the registered preset instead of a pristine zero slot.
bool RetailWorldWidenPhysPreset(RetailWorldLoadContext *context, uint32_t slotRef,
                                PhysPreset **out, uint32_t slotOffset,
                                uint32_t *insertSlotOffsetOut)
{
    if (!context || !context->session || !context->reader || !out)
        return false;
    *out = nullptr;
    if (insertSlotOffsetOut)
        *insertSlotOffsetOut = 0;
    RetailZoneLoadSession *session = context->session;
    FsRetailFastfileReader *reader = context->reader;
    if (!slotRef)
    {
        // A declared null slot binds null; record it so a later slot
        // indirection onto this slot binds null the same way.
        if (slotOffset)
            RecordNestedSlot(context, slotOffset, ASSET_TYPE_PHYSPRESET, {});
        return true;
    }
    if (slotRef != kInlineRef && slotRef != kInsertRef)
    {
        XAssetHeader found{};
        if (!ResolveNestedAlias(context, slotRef, ASSET_TYPE_PHYSPRESET, &found))
        {
            Com_Printf(0, "RetailWorld: physpreset slot alias 0x%08x resolve failed\n",
                       slotRef);
            return false;
        }
        if (slotOffset &&
            !RecordNestedSlot(context, slotOffset, ASSET_TYPE_PHYSPRESET, found))
            return false;
        *out = found.physPreset;
        return true;
    }
    // HandleAssetSlot's -2 form (DB_InsertPointer): reserve the 4-byte
    // block-4 pointer slot before the body streams, then record the widened
    // result there. That reservation is exactly the address later
    // DB_ConvertOffsetToAlias aliases read, so it must carry the registered
    // asset, never stay zero.
    uint32_t insertSlotOffset = 0;
    if (slotRef == kInsertRef)
    {
        uint8_t *insertSlot = RetailWireBlocksAlloc(&session->wire, 4, 4, 4);
        if (!insertSlot)
        {
            Com_Printf(0, "RetailWorld: physpreset insert slot reservation failed\n");
            return false;
        }
        insertSlotOffset = static_cast<uint32_t>(
            insertSlot - session->zoneMemory->blocks[4].data);
    }
    const uint32_t savedCursor0 = session->wire.cursor[0];
    RetailWalkDirectoryRecord consumed{};
    consumed.header = kInlineRef;
    RetailWalkDirectoryResult scratch{};
    uint32_t rootStart = 0;
    uint32_t nameStart = UINT32_MAX;
    uint32_t destStart = UINT32_MAX;
    if (!ReadRetailPhysPresetBody(session, reader, &consumed, &scratch, &rootStart,
                                  &nameStart, &destStart))
    {
        Com_Printf(0, "RetailWorld: nested physpreset body unreadable\n");
        return false;
    }
    const uint32_t nameRef =
        ReadLe32(session->zoneMemory->blocks[0].data + rootStart);
    XAssetHeader widened{};
    if (!WidenConsumedPhysPreset(session, rootStart, nameStart, destStart, &widened))
    {
        Com_Printf(0, "RetailWorld: nested physpreset widen/register failed\n");
        return false;
    }
    // The original Load_PhysPresetPtr widens the nested body inside its own
    // pushed temp position and pops back, so the enclosing structure's own
    // block-0 staging stays where it was. Block-4 name bytes stay: they are
    // the asset's addressable name.
    RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
    if (widened.physPreset->name)
    {
        uint32_t recordNameRef = nameRef;
        if (nameRef == kInlineRef)
        {
            RetailPtr32 encodedName{};
            if (!RetailWireTokenEncode(4, nameStart, &encodedName))
                return false;
            recordNameRef = encodedName.encoded;
        }
        if (!RecordNestedName(context, recordNameRef, ASSET_TYPE_PHYSPRESET, widened, 0,
                              rootStart))
        {
            Com_Printf(0, "RetailWorld: nested physpreset has no recordable name\n");
            return false;
        }
    }
    if (slotOffset &&
        !RecordNestedSlot(context, slotOffset, ASSET_TYPE_PHYSPRESET, widened))
        return false;
    if (insertSlotOffset)
    {
        if (!RecordNestedSlot(context, insertSlotOffset, ASSET_TYPE_PHYSPRESET, widened))
            return false;
        if (insertSlotOffsetOut)
            *insertSlotOffsetOut = insertSlotOffset;
    }
    ++context->widenedPhysPresetCount;
    *out = widened.physPreset;
    RETAIL_ASSET_TRACE(0, "RetailWorld: physpreset widened '%s'\n",
                       widened.physPreset->name ? widened.physPreset->name : "(null)");
    return true;
}

// GPU audit (a4): host + live census over the widened draw-inst models.
// Walks world->dpvs.smodelDrawInsts (the exact pointers the static-model
// draw funnels dereference) and re-validates every lod/surface range the
// widener already enforced at widen time, plus the cache-slot fit
// (baseVert+vertCount <= 1<<allocBits) that only exists once the whole
// model is widened. Returns false loudly on the first OOB (with model/lod/surf
// identity) instead of letting it reach DrawIndexedPrimitive as
// VK_ERROR_DEVICE_LOST.
bool RetailWorldAuditStaticModelsForGpu(const GfxWorld *world)
{
    if (!world)
    {
        
        return false;
    }
    const uint32_t smodelCount = world->dpvs.smodelCount;
    const GfxStaticModelDrawInst *insts = world->dpvs.smodelDrawInsts;
    if (!smodelCount)
    {
        
        return true;
    }
    if (!insts)
    {
        
        return false;
    }
    uint32_t withModel = 0, cachedLods = 0, rigidLods = 0, totalSurfs = 0;
    uint32_t maxVert = 0, maxTri = 0, bad = 0;
    // staticissue.md census: do not stop at the first minor bounds
    // excursion. Count every vertex outside its model's nominal local
    // bounds, the worst such excursion, non-finite coordinates, and the
    // most extreme coordinate seen -- so a genuinely corrupt decode is
    // distinguishable from loose LOD/bounds padding.
    uint32_t nonFiniteVerts = 0, outsideVerts = 0, outsideReported = 0;
    float maxOutside = 0.0f, extremeCoord = 0.0f;
    const XModel *extremeModel = nullptr;
    // staticissue.md test 5: transform each model's local bounds by its own
    // placement and compare with the serialized per-instance world bounds
    // (GfxStaticModelInst). A large disagreement identifies a draw-instance
    // association or placement-layout error before rendering.
    const GfxStaticModelInst *instBounds =
        world->dpvs.smodelInsts ? world->dpvs.smodelInsts : nullptr;
    uint32_t placementMismatch = 0, placementReported = 0;
    float maxPlacementDelta = 0.0f;
    const XModel *worstPlacementModel = nullptr;
    for (uint32_t i = 0; i < smodelCount && !bad; ++i)
    {
        const XModel *model = insts[i].model;
        if (!model)
            continue;
        ++withModel;
        const int numLods = model->numLods;
        // staticissue.md test 5 (independent of the lod loop): transform the
        // model's local AABB by this instance's placement and compare with
        // the serialized per-instance world bounds.
        if (instBounds && model->mins[0] <= model->maxs[0])
        {
            const GfxPackedPlacement &p = insts[i].placement;
            float worldMins[3] = { 1.0e30f, 1.0e30f, 1.0e30f };
            float worldMaxs[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
            for (uint32_t corner = 0; corner < 8; ++corner)
            {
                const float local[3] = {
                    (corner & 1u) ? model->maxs[0] : model->mins[0],
                    (corner & 2u) ? model->maxs[1] : model->mins[1],
                    (corner & 4u) ? model->maxs[2] : model->mins[2],
                };
                for (uint32_t axis = 0; axis < 3; ++axis)
                {
                    const float world = p.origin[axis] +
                        p.scale * (p.axis[0][axis] * local[0] +
                                   p.axis[1][axis] * local[1] +
                                   p.axis[2][axis] * local[2]);
                    worldMins[axis] = std::min(worldMins[axis], world);
                    worldMaxs[axis] = std::max(worldMaxs[axis], world);
                }
            }
            float delta = 0.0f;
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                delta = std::max(delta, worldMins[axis] - instBounds[i].mins[axis]);
                delta = std::max(delta, instBounds[i].mins[axis] - worldMins[axis]);
                delta = std::max(delta, worldMaxs[axis] - instBounds[i].maxs[axis]);
                delta = std::max(delta, instBounds[i].maxs[axis] - worldMaxs[axis]);
            }
            if (delta > 1.0f)
            {
                ++placementMismatch;
                if (delta > maxPlacementDelta)
                {
                    maxPlacementDelta = delta;
                    worstPlacementModel = model;
                }
                if (placementReported < 12u)
                {
                    ++placementReported;
                    
                }
            }
        }
        if (numLods < 0 || numLods > 4)
        {
            
            bad = 1;
            break;
        }
        for (int lod = 0; lod < numLods && lod < 4 && !bad; ++lod)
        {
            const XModelLodInfo &info = model->lodInfo[lod];
            if (info.smcIndexPlusOne)
                ++cachedLods;
            else
                ++rigidLods;
            if (info.smcIndexPlusOne &&
                (info.smcAllocBits < 4 || info.smcAllocBits > 9 ||
                 (uint32_t)info.smcIndexPlusOne - 1u >= 4u))
            {
                
                bad = 1;
                break;
            }
            if (info.numsurfs &&
                (uint32_t)info.surfIndex + (uint32_t)info.numsurfs > (uint32_t)model->numsurfs)
            {
                
                bad = 1;
                break;
            }
            if (!model->surfs && info.numsurfs)
            {
                
                bad = 1;
                break;
            }
            const uint32_t allocVerts = info.smcIndexPlusOne ? (1u << info.smcAllocBits) : 0u;
            for (uint32_t s = 0; s < info.numsurfs && !bad; ++s)
            {
                const XSurface &surf = model->surfs[(uint32_t)info.surfIndex + s];
                ++totalSurfs;
                if (surf.vertCount > maxVert)
                    maxVert = surf.vertCount;
                if (surf.triCount > maxTri)
                    maxTri = surf.triCount;
                if (surf.triCount && !surf.triIndices)
                {
                    
                    bad = 1;
                    break;
                }
                if (surf.vertCount && !surf.verts0)
                {
                    
                    bad = 1;
                    break;
                }
                if (surf.triIndices && surf.vertCount)
                {
                    for (uint32_t v = 0; v < surf.vertCount; ++v)
                    {
                        const GfxPackedVertex &vertex = surf.verts0[v];
                        float outside = 0.0f;
                        for (uint32_t axis = 0; axis < 3; ++axis)
                        {
                            const float coord = vertex.xyz[axis];
                            if (!(coord == coord) ||
                                coord < -3.0e38f || coord > 3.0e38f)
                            {
                                ++nonFiniteVerts;
                            }
                            const float magnitude = coord < 0.0f ? -coord : coord;
                            if (magnitude > extremeCoord)
                            {
                                extremeCoord = magnitude;
                                extremeModel = model;
                            }
                            if (coord < model->mins[axis])
                                outside = std::max(outside, model->mins[axis] - coord);
                            else if (coord > model->maxs[axis])
                                outside = std::max(outside, coord - model->maxs[axis]);
                        }
                        if (outside > 0.0f)
                        {
                            ++outsideVerts;
                            if (outside > maxOutside && outsideReported < 40u)
                            {
                                ++outsideReported;
                                maxOutside = outside;
                                
                            }
                        }
                    }
                    const uint32_t idxCount = 3u * (uint32_t)surf.triCount;
                    for (uint32_t k = 0; k < idxCount; ++k)
                    {
                        if (surf.triIndices[k] >= surf.vertCount)
                        {
                            
                            bad = 1;
                            break;
                        }
                    }
                }
                if (info.smcIndexPlusOne &&
                    (uint32_t)surf.baseVertIndex + (uint32_t)surf.vertCount > allocVerts)
                {
                    
                    bad = 1;
                    break;
                }
            }
        }
    }
    // staticissue wall-texture census: report the colorMap/texture images of
    // a few real BSP wall materials as decoded (host side, before any device
    // materialize): name, dims, delayed-load flag, embedded-pixel payload
    // size. If the wall colorMaps carry no pixel payload (or are delayed with
    // no activation seam), the walls render black even though their surfaces,
    // state bits, and lightmaps are all correct.
    {
        static const char *const kWallMats[] = {
            "wc/com_plastic_wall", "wc/icbm_greymetal", "wc/me_drywall"
        };
        uint32_t logged = 0;
        for (uint32_t s = 0; s < (uint32_t)world->surfaceCount && logged < 12u; ++s)
        {
            const Material *m = world->dpvs.surfaces[s].material;
            if (!m || !m->info.name || !m->textureTable)
                continue;
            bool match = false;
            for (const char *want : kWallMats)
            {
                const std::size_t n = std::strlen(want);
                if (!std::strncmp(m->info.name, want, n))
                {
                    match = true;
                    break;
                }
            }
            if (!match)
                continue;
            ++logged;
            if (m->techniqueSet)
            {
                
                for (int ti = 0; ti < 34; ++ti)
                {
                    const MaterialTechnique *tech = m->techniqueSet->techniques[ti];
                    if (!tech)
                        continue;
                    const MaterialPass *pass = tech->passCount ? &tech->passArray[0] : nullptr;
                    
                }
            }
            for (uint32_t t = 0; t < m->textureCount; ++t)
            {
                const GfxImage *img = MaterialTextureImage(m->textureTable[t]);
                
            }
        }
    }
    
    return bad == 0;
}

// cell-graph widener: replays a walk-recorded placement trace (see
// RetailGfxCellTrace, db_retail_walk.h) to widen native GfxCell/GfxAabbTree/
// GfxPortal objects from the cell-graph bytes the walk recursion already
// streamed into the session's zone block 4. Every consumed trace entry is
// checked against the record the widener is currently looking at, so any
// walk/widen divergence fails loudly instead of widening wrong bytes.
// Alias-form references (portal cells, smodel-index lists) resolve through
// block-4 offset maps populated as placements widen, matching
// DB_ConvertOffsetToPointer's block-4 keying exactly: the live one-pass
// reader places every streamed byte at its original engine offset, so a
// wire alias and this pass's own trace name one canonical coordinate
// space with no translation.
namespace
{
struct RetailCellGraphWidener
{
    RetailZoneLoadSession *session = nullptr;
    RetailGfxCellTrace *trace = nullptr;
    // DecodeFastfileToken bounds alias decoding against the FS reader's
    // own block sizes; the driver sets this before widening.
    FsRetailFastfileReader *contextReader = nullptr;
    uint32_t nextEntry = 0;
    bool ok = true;
    // Canonical block-4 offset -> native object maps for alias resolution.
    struct MapEntry
    {
        uint32_t offset;
        uint32_t bytes;
        void *ptr;
    };
    MapEntry cellMap[8192]{};
    uint32_t cellMapCount = 0;
    MapEntry listMap[65536]{};
    uint32_t listMapCount = 0;
    // Deferred PORTAL_CELL_ALIAS bindings. An alias names a cell by its
    // canonical block-4 record offset, and the target may not be widened
    // yet when the alias is read (a portal graph is not a tree). Both are
    // handled by deferring: after every cell in the trace is widened, each
    // alias is bound to the already-widened cell recorded at exactly that
    // offset. Binding to the existing object (rather than widening a
    // second copy) is mandatory, not an optimization: r_dpvs.cpp derives a
    // cell index as "cell - rgp.world->cells" and indexes dpvsGlob.cellBits
    // with it.
    struct CellAliasFixup
    {
        GfxCell **slot;
        uint32_t offset;
    };
    CellAliasFixup cellFixups[8192]{};
    uint32_t cellFixupCount = 0;
    uint32_t aliasBound = 0;
    uint32_t aliasBoundInArray = 0;

    void Fail(const char *what, uint32_t detail)
    {
        if (ok)
            Com_Printf(0, "RetailWorld: cell graph widen failed at %s detail=0x%x entry=%u/%u\n",
                       what, detail, nextEntry, trace ? trace->count : 0);
        ok = false;
    }

    const RetailGfxCellTrace::Entry *Take(uint32_t kind)
    {
        if (!ok)
            return nullptr;
        if (!trace || nextEntry >= trace->count)
        {
            Fail("trace exhausted", kind);
            return nullptr;
        }
        const RetailGfxCellTrace::Entry *entry = &trace->entries[nextEntry];
        if (entry->kind != kind)
        {
            Fail("trace kind mismatch (expected/actual)", (kind << 16) | entry->kind);
            return nullptr;
        }
        ++nextEntry;
        return entry;
    }

    bool DecodeAliasOffset(uint32_t ref, uint32_t *outOffset)
    {
        if (!ok)
            return false;
        RetailWireToken token{};
        if (!DecodeFastfileToken(contextReader, ref, 0, 1u << 4, &token) ||
            token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
            return false;
        *outOffset = token.offset;
        return true;
    }

    // Keyed by canonical block-4 offset (the space wire aliases name), so
    // a TREE_LIST_ALIAS repeated across trees shares one native array.
    void *FindList(uint32_t offset, uint32_t bytes)
    {
        for (uint32_t i = 0; i < listMapCount; ++i)
        {
            const uint64_t end = static_cast<uint64_t>(offset) + bytes;
            const uint64_t mapEnd = static_cast<uint64_t>(listMap[i].offset) +
                                    listMap[i].bytes;
            if (offset >= listMap[i].offset && end <= mapEnd)
                return static_cast<uint8_t *>(listMap[i].ptr) +
                       (offset - listMap[i].offset);
        }
        return nullptr;
    }

    void RecordCell(uint32_t offset, void *ptr)
    {
        if (cellMapCount >= sizeof(cellMap) / sizeof(cellMap[0]))
        {
            Fail("cell map full", cellMapCount);
            return;
        }
        cellMap[cellMapCount].offset = offset;
        cellMap[cellMapCount].bytes = 56;
        cellMap[cellMapCount].ptr = ptr;
        ++cellMapCount;
    }

    void *FindCell(uint32_t offset)
    {
        for (uint32_t i = 0; i < cellMapCount; ++i)
            if (cellMap[i].offset == offset)
                return cellMap[i].ptr;
        return nullptr;
    }

    void RecordList(uint32_t offset, uint32_t bytes, void *ptr)
    {
        if (listMapCount >= sizeof(listMap) / sizeof(listMap[0]))
        {
            Fail("list map full", listMapCount);
            return;
        }
        listMap[listMapCount].offset = offset;
        listMap[listMapCount].bytes = bytes;
        listMap[listMapCount].ptr = ptr;
        ++listMapCount;
    }

    const uint8_t *Block4(uint32_t offset, uint32_t span)
    {
        if (!ok || !session || !session->zoneMemory)
        {
            ok = false;
            return nullptr;
        }
        const XBlock &block = session->zoneMemory->blocks[4];
        if ((uint64_t)offset + span > block.size)
        {
            ok = false;
            return nullptr;
        }
        return block.data + offset;
    }

    // Widen the aabb-tree array of the cell whose 56-byte record is at
    // recordOffset. The walk streams the whole array first, then each
    // tree's inline smodel-index list in tree order, so the widener
    // consumes the trace in exactly that order.
    void WidenTrees(const uint8_t *cellRecord, GfxCell *dest)
    {
        const uint32_t aabbCount = ReadLe32(cellRecord + 24);
        const uint32_t aabbRef = ReadLe32(cellRecord + 28);
        if (!aabbRef)
            return;
        const RetailGfxCellTrace::Entry *array = Take(RetailGfxCellTrace::TREE_ARRAY);
        if (!ok || array->count != aabbCount)
        {
            Fail("tree array", array ? array->offset : 0);
            return;
        }
        const uint8_t *wire = Block4(array->offset, aabbCount * 44u);
        GfxAabbTree *trees = nullptr;
        if (aabbCount)
        {
            trees = static_cast<GfxAabbTree *>(RetailZoneLoadSessionAlloc(
                session, aabbCount * sizeof(GfxAabbTree), alignof(GfxAabbTree)));
            if (!trees)
            {
                ok = false;
                return;
            }
        }
        for (uint32_t tree = 0; tree < aabbCount; ++tree)
        {
            const uint8_t *record = wire + tree * 44u;
            GfxAabbTree &out = trees[tree];
            std::memset(&out, 0, sizeof(out));
            std::memcpy(out.mins, record, sizeof(out.mins));
            std::memcpy(out.maxs, record + 12, sizeof(out.maxs));
            out.childCount = ReadLe16(record + 24);
            out.surfaceCount = ReadLe16(record + 26);
            out.startSurfIndex = ReadLe16(record + 28);
            out.surfaceCountNoDecal = ReadLe16(record + 30);
            out.startSurfIndexNoDecal = ReadLe16(record + 32);
            out.smodelIndexCount = ReadLe16(record + 34);
            const uint32_t indexesRef = ReadLe32(record + 36);
            // childrenOffset is a *byte* delta from this node to the first of
            // its children inside the same array, and every runtime consumer
            // applies it directly to the native pointer:
            // `(char *)tree + tree->childrenOffset` in
            // R_AddAabbTreeSurfacesInFrustum_r (r_dpvs_static.cpp:61) and at
            // four sites in r_marks.cpp. The linker computed that delta
            // against the *serialized* 44-byte GfxAabbTree; the native record
            // is wider under LP64 (the uint16_t* smodelIndexes forces 8-byte
            // alignment, sizeof == 56), so the wire value must be restrided or
            // every descent below a tree's root lands mid-record on an
            // unrelated node -- silently, because the bogus index still falls
            // inside the array. In real killhouse.ff all 6228
            // aabb-tree records across the three cells carry a nonzero
            // multiple of 44 whose implied child span is in range and always
            // after the parent. Fail loudly rather than guess if that ever
            // stops holding; never fabricate a substitute offset.
            const int32_t wireChildrenOffset = static_cast<int32_t>(ReadLe32(record + 40));
            if (wireChildrenOffset <= 0 || wireChildrenOffset % 44 != 0)
            {
                Fail("aabb children offset stride", static_cast<uint32_t>(wireChildrenOffset));
                return;
            }
            const int64_t childDelta = wireChildrenOffset / 44;
            const int64_t firstChild = static_cast<int64_t>(tree) + childDelta;
            if (out.childCount &&
                (firstChild < 0 ||
                 firstChild + static_cast<int64_t>(out.childCount) > static_cast<int64_t>(aabbCount)))
            {
                Fail("aabb children span", static_cast<uint32_t>(firstChild));
                return;
            }
            out.childrenOffset =
                static_cast<int>(childDelta * static_cast<int64_t>(sizeof(GfxAabbTree)));
            if (!indexesRef)
                continue;
            if (indexesRef == kInlineRef)
            {
                const RetailGfxCellTrace::Entry *list =
                    Take(RetailGfxCellTrace::TREE_LIST);
                if (!ok || list->count != out.smodelIndexCount)
                {
                    Fail("tree list", list ? list->offset : 0);
                    return;
                }
                const uint8_t *listWire = Block4(list->offset, list->count * 2u);
                if (!ok)
                    return;
                uint16_t *native = static_cast<uint16_t *>(RetailZoneLoadSessionAlloc(
                    session, list->count * 2u, alignof(uint16_t)));
                if (!native)
                {
                    ok = false;
                    return;
                }
                std::memcpy(native, listWire, list->count * 2u);
                out.smodelIndexes = native;
                RecordList(list->offset, list->count * 2u, native);
            }
            else
            {
                const RetailGfxCellTrace::Entry *alias =
                    Take(RetailGfxCellTrace::TREE_LIST_ALIAS);
                if (!ok || alias->count != out.smodelIndexCount)
                {
                    Fail("tree list alias", alias ? alias->ref : 0);
                    return;
                }
                uint32_t offset = 0;
                if (!DecodeAliasOffset(alias->ref, &offset))
                {
                    Fail("tree list alias ref", alias->ref);
                    return;
                }
                void *native = FindList(offset, alias->count * 2u);
                if (!native)
                {
                    Fail("unresolved tree list alias", offset);
                    return;
                }
                out.smodelIndexes = static_cast<uint16_t *>(native);
            }
        }
        dest->aabbTreeCount = static_cast<int>(aabbCount);
        dest->aabbTree = trees;
    }

    // Widen one cell's contents (the caller allocates the GfxCell). The
    // record itself is read from the session block at recordOffset; nested
    // placements come from the trace in walk order (trees, then portals,
    // then cull groups, then reflection probes).
    void WidenCellContents(uint32_t recordOffset, GfxCell *dest)
    {
        const uint8_t *record = Block4(recordOffset, 56);
        if (!ok)
            return;
        const RetailGfxCellTrace::Entry *cellEntry =
            Take(RetailGfxCellTrace::CELL);
        if (!ok || cellEntry->offset != recordOffset)
        {
            Fail("cell record", recordOffset);
            return;
        }
        std::memset(dest, 0, sizeof(*dest));
        std::memcpy(dest->mins, record, sizeof(dest->mins));
        std::memcpy(dest->maxs, record + 12, sizeof(dest->maxs));
        RecordCell(recordOffset, dest);
        WidenTrees(record, dest);
        if (!ok)
            return;
        const uint32_t portalCount = ReadLe32(record + 32);
        const uint32_t portalRef = ReadLe32(record + 36);
        if (portalRef)
        {
            const RetailGfxCellTrace::Entry *array =
                Take(RetailGfxCellTrace::PORTAL_ARRAY);
            if (!ok || array->count != portalCount)
            {
                Fail("portal array", array ? array->offset : 0);
                return;
            }
            const uint8_t *wire = Block4(array->offset, portalCount * 68u);
            if (!ok)
                return;
            GfxPortal *portals = nullptr;
            if (portalCount)
            {
                portals = static_cast<GfxPortal *>(RetailZoneLoadSessionAlloc(
                    session, portalCount * sizeof(GfxPortal), alignof(GfxPortal)));
                if (!portals)
                {
                    ok = false;
                    return;
                }
            }
            for (uint32_t portal = 0; portal < portalCount; ++portal)
            {
                const uint8_t *wirePortal = wire + portal * 68u;
                GfxPortal &out = portals[portal];
                // writable state is runtime-only (queued/hull scratch):
                // zeroed, never taken from the wire.
                std::memset(&out, 0, sizeof(out));
                std::memcpy(&out.plane, wirePortal + 12, sizeof(DpvsPlane));
                const uint32_t cellRef = ReadLe32(wirePortal + 32);
                const uint32_t vertsRef = ReadLe32(wirePortal + 36);
                out.vertexCount = wirePortal[40];
                std::memcpy(out.hullAxis, wirePortal + 44, sizeof(out.hullAxis));
                if (cellRef == kInlineRef)
                {
                    const RetailGfxCellTrace::Entry *inner =
                        Take(RetailGfxCellTrace::PORTAL_CELL_INLINE);
                    if (!ok)
                    {
                        Fail("portal cell inline", 0);
                        return;
                    }
                    GfxCell *innerCell = static_cast<GfxCell *>(RetailZoneLoadSessionAlloc(
                        session, sizeof(GfxCell), alignof(GfxCell)));
                    if (!innerCell)
                    {
                        ok = false;
                        return;
                    }
                    WidenCellContents(inner->offset, innerCell);
                    if (!ok)
                        return;
                    out.cell = innerCell;
                }
                else if (cellRef)
                {
                    const RetailGfxCellTrace::Entry *alias =
                        Take(RetailGfxCellTrace::PORTAL_CELL_ALIAS);
                    if (!ok)
                    {
                        Fail("portal cell alias", 0);
                        return;
                    }
                    uint32_t offset = 0;
                    if (!DecodeAliasOffset(alias->ref, &offset))
                    {
                        Fail("portal cell alias ref", alias->ref);
                        return;
                    }
                    // Bound after every cell is widened -- the target is
                    // routinely a sibling top-level cell this pass has not
                    // reached yet (portal graphs are not trees). See
                    // cellFixups / ResolveCellAliasFixups.
                    if (cellFixupCount >= sizeof(cellFixups) / sizeof(cellFixups[0]))
                    {
                        Fail("portal cell alias fixup table full", cellFixupCount);
                        return;
                    }
                    cellFixups[cellFixupCount].slot = &out.cell;
                    cellFixups[cellFixupCount].offset = offset;
                    ++cellFixupCount;
                    out.cell = nullptr;
                }
                if (vertsRef)
                {
                    if (vertsRef != kInlineRef)
                    {
                        // Load_GfxPortal has no alias form for vertices
                        // (any nonzero slot allocates and streams); a
                        // non-inline reference here is a defect.
                        Fail("portal verts alias form", vertsRef);
                        return;
                    }
                    const RetailGfxCellTrace::Entry *verts =
                        Take(RetailGfxCellTrace::PORTAL_VERTS);
                    if (!ok || verts->count != out.vertexCount)
                    {
                        Fail("portal verts", verts ? verts->offset : 0);
                        return;
                    }
                    const uint8_t *vertsWire = Block4(verts->offset,
                                                      verts->count * 12u);
                    if (!ok)
                        return;
                    float(*native)[3] = static_cast<float(*)[3]>(
                        RetailZoneLoadSessionAlloc(session, verts->count * 12u, 4));
                    if (!native)
                    {
                        ok = false;
                        return;
                    }
                    std::memcpy(native, vertsWire, verts->count * 12u);
                    out.vertices = native;
                }
            }
            dest->portalCount = static_cast<int>(portalCount);
            dest->portals = portals;
        }
        const uint32_t cullCount = ReadLe32(record + 40);
        const uint32_t cullRef = ReadLe32(record + 44);
        if (cullRef)
        {
            const RetailGfxCellTrace::Entry *culls =
                Take(RetailGfxCellTrace::CULL_INTS);
            if (!ok || culls->count != cullCount)
            {
                Fail("cull ints", culls ? culls->offset : 0);
                return;
            }
            const uint8_t *wire = Block4(culls->offset, culls->count * 4u);
            if (!ok)
                return;
            int *native = static_cast<int *>(RetailZoneLoadSessionAlloc(
                session, culls->count * 4u, alignof(int)));
            if (!native)
            {
                ok = false;
                return;
            }
            std::memcpy(native, wire, culls->count * 4u);
            dest->cullGroupCount = static_cast<int>(culls->count);
            dest->cullGroups = native;
        }
        const uint32_t probeCount = record[48];
        const uint32_t probeRef = ReadLe32(record + 52);
        if (probeRef)
        {
            const RetailGfxCellTrace::Entry *probes =
                Take(RetailGfxCellTrace::PROBE_BYTES);
            if (!ok || probes->count != probeCount)
            {
                Fail("probe bytes", probes ? probes->offset : 0);
                return;
            }
            const uint8_t *wire = Block4(probes->offset, probes->count);
            if (!ok)
                return;
            uint8_t *native = static_cast<uint8_t *>(
                RetailZoneLoadSessionAlloc(session, probes->count, 1));
            if (!native)
            {
                ok = false;
                return;
            }
            std::memcpy(native, wire, probes->count);
            dest->reflectionProbeCount = static_cast<uint8_t>(probes->count);
            dest->reflectionProbes = native;
        }
    }

    bool ResolveCellAliasFixups(const GfxCell *cellsBase, uint32_t cellCount)
    {
        for (uint32_t i = 0; i < cellFixupCount && ok; ++i)
        {
            GfxCell *match = static_cast<GfxCell *>(FindCell(cellFixups[i].offset));
            if (!match)
            {
                Fail("portal cell alias target", cellFixups[i].offset);
                return false;
            }
            *cellFixups[i].slot = match;
            ++aliasBound;
            // r_dpvs.cpp turns a visited cell back into an index with
            // "cell - rgp.world->cells" and indexes dpvsGlob.cellBits
            // with it, so a portal reaching a cell outside that array
            // would corrupt the bit set. Counted, not assumed: killhouse
            // binds all 94 of its aliases inside the array.
            if (cellsBase && match >= cellsBase && match < cellsBase + cellCount)
                ++aliasBoundInArray;
        }
        return ok;
    }
};
} // namespace

// Original call chain: R_LoadWorld -> R_InitShadowGeometryArrays
// (r_bsp_load_obj.cpp) walks every shadow-casting surface per primary light,
// allocates each light's sortedSurfIndex span and static-model smodelIndex
// span, then fills them. The live retail loader allocated only the zeroed
// per-light headers and relied on R_AddShadowSurfaceToPrimaryLight's null
// guard, so every primary light kept surfaceCount/smodelCount 0 and the spot
// shadow maps rendered with no casters (e.g. 87 spot-shadow ent commands
// but zero executed spot-shadow draw surfs). This restores the same two-pass
// construction, with zone-session storage instead of Hunk_Alloc.
static bool RetailWalkBuildShadowGeometry(RetailZoneLoadSession *session)
{
    if (!session)
        return false;
    // A world with no primary lights has no per-light shadow geometry in the
    // original loader either (the header array is only allocated for
    // primaryLightCount entries).
    if (s_world.primaryLightCount == 0)
        return true;
    if (!s_world.shadowGeom || !s_world.lightRegion || !s_world.models ||
        !s_world.dpvs.surfaces)
        return false;

    R_ForEachShadowCastingSurfaceOnEachLight(R_IncrementShadowGeometryCount);
    s_world.shadowGeom[0].surfaceCount = 0;
    if (s_world.sunPrimaryLightIndex < s_world.primaryLightCount)
        s_world.shadowGeom[s_world.sunPrimaryLightIndex].surfaceCount = 0;
    for (uint32_t light = 0; light < s_world.primaryLightCount; ++light)
    {
        GfxShadowGeometry *geom = &s_world.shadowGeom[light];
        if (geom->surfaceCount)
        {
            geom->sortedSurfIndex = static_cast<uint16_t *>(RetailZoneLoadSessionAlloc(
                session, 2u * geom->surfaceCount, alignof(uint16_t)));
            if (!geom->sortedSurfIndex)
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: shadow geometry light=%u surfaces=%u oom\n",
                           light, geom->surfaceCount);
                return false;
            }
            geom->surfaceCount = 0;
        }
    }
    R_ForEachShadowCastingSurfaceOnEachLight(R_AddShadowSurfaceToPrimaryLight);

    // R_AllocStaticModels increments smodelCount during the fastfile load
    // (r_bsp_load_obj.cpp); the live loader never ran that pass, so count and
    // fill the per-light instance spans here with the same bounds contract.
    for (uint32_t smodelIndex = 0; smodelIndex < s_world.dpvs.smodelCount; ++smodelIndex)
    {
        const uint32_t light = s_world.dpvs.smodelDrawInsts[smodelIndex].primaryLightIndex;
        if (light >= s_world.primaryLightCount)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: smodel %u light %u out of range %u\n",
                       smodelIndex, light, s_world.primaryLightCount);
            return false;
        }
        ++s_world.shadowGeom[light].smodelCount;
    }
    for (uint32_t light = 0; light < s_world.primaryLightCount; ++light)
    {
        GfxShadowGeometry *geom = &s_world.shadowGeom[light];
        if (geom->smodelCount)
        {
            geom->smodelIndex = static_cast<uint16_t *>(RetailZoneLoadSessionAlloc(
                session, 2u * geom->smodelCount, alignof(uint16_t)));
            if (!geom->smodelIndex)
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: shadow smodels light=%u count=%u oom\n",
                           light, geom->smodelCount);
                return false;
            }
            geom->smodelCount = 0;
        }
    }
    for (uint32_t smodelIndex = 0; smodelIndex < s_world.dpvs.smodelCount; ++smodelIndex)
    {
        GfxShadowGeometry *geom =
            &s_world.shadowGeom[s_world.dpvs.smodelDrawInsts[smodelIndex].primaryLightIndex];
        if (geom->smodelIndex)
            geom->smodelIndex[geom->smodelCount++] = static_cast<uint16_t>(smodelIndex);
    }

    uint64_t shadowSurfaces = 0;
    uint64_t shadowSModels = 0;
    uint32_t shadowLights = 0;
    for (uint32_t light = 0; light < s_world.primaryLightCount; ++light)
    {
        const GfxShadowGeometry *geom = &s_world.shadowGeom[light];
        shadowSurfaces += geom->surfaceCount;
        shadowSModels += geom->smodelCount;
        if (geom->surfaceCount || geom->smodelCount)
            ++shadowLights;
    }
    Com_Printf(0,
               "KILLHOUSE_SHADOW_GEOM lights=%u primary=%u surfaces=%llu smodels=%llu sun=%u\n",
               shadowLights, s_world.primaryLightCount,
               (unsigned long long)shadowSurfaces, (unsigned long long)shadowSModels,
               s_world.sunPrimaryLightIndex);
    return true;
}

// host-only differential trace. The OAT light-grid oracle
// (ObjWriting/Game/IW3/GfxWorld/LightGridDumperIW3) serializes the same
// decoded GfxLightGrid from OAT's own GfxWorld load;
// offline tooling compares scalars and every array byte
// between the two independent decodes. Gated by KISAK_OAT_FIELD_TRACE, the
// same gate every other OAT_FIELD family uses, so a normal/guest run never
// executes it. The appended +1 zeroed default entry R_LoadLightGridColors
// allocates past the wire count is reported separately as extraZero and is
// deliberately not part of the wire array comparison.
static bool RetailOatLightGridTraceEnabled()
{
    static const bool enabled = std::getenv("KISAK_OAT_FIELD_TRACE") != nullptr;
    return enabled;
}

static void RetailOatTraceLightGrid(const GfxWorld *world)
{
    if (!world || !RetailOatLightGridTraceEnabled())
        return;
    const GfxLightGrid &grid = world->lightGrid;
    if (grid.colorCount == 0 || !grid.colors)
        return;
    // colorCount already includes the +1 zeroed default entry; the wire grid
    // OAT loads has the wire count only.
    const uint32_t wireColorCount = grid.colorCount - 1u;
    const uint8_t *extra = reinterpret_cast<const uint8_t *>(&grid.colors[wireColorCount]);
    uint8_t extraZero = 1;
    for (uint32_t i = 0; i < sizeof(GfxLightGridColors); ++i)
    {
        if (extra[i])
        {
            extraZero = 0;
            break;
        }
    }
    uint32_t rowCount = 0;
    if (grid.rowAxis < 3 && grid.rowDataStart &&
        grid.maxs[grid.rowAxis] >= grid.mins[grid.rowAxis])
        rowCount = static_cast<uint32_t>(grid.maxs[grid.rowAxis] - grid.mins[grid.rowAxis] + 1u);
    Com_Printf(0,
               "OAT_LIGHTGRID name='%s' hasRegions=%u sun=%u mins=%u,%u,%u maxs=%u,%u,%u "
               "rowAxis=%u colAxis=%u rows=%u rawSize=%u entries=%u colors=%u extraZero=%u\n",
               world->name ? world->name : "", grid.hasLightRegions ? 1u : 0u,
               grid.sunPrimaryLightIndex, grid.mins[0], grid.mins[1], grid.mins[2],
               grid.maxs[0], grid.maxs[1], grid.maxs[2], grid.rowAxis, grid.colAxis,
               rowCount, grid.rawRowDataSize, grid.entryCount, wireColorCount,
               static_cast<unsigned>(extraZero));
    // Com_Printf's own buffer is 4100 bytes, so each array is emitted in
    // bounded chunks; the verifier reassembles by (name, tag) in off order.
    const auto traceArray = [&](const char *tag, const void *data, uint32_t size)
    {
        if (!data || size == 0)
            return;
        static const char digits[] = "0123456789abcdef";
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        constexpr uint32_t kChunkBytes = 1984;
        for (uint32_t off = 0; off < size; off += kChunkBytes)
        {
            const uint32_t len = size - off < kChunkBytes ? size - off : kChunkBytes;
            char line[64 + 768 + kChunkBytes * 2 + 4];
            int n = std::snprintf(line, sizeof(line),
                                  "OAT_LIGHTGRID_%s name='%s' off=%u size=%u hex=", tag,
                                  world->name ? world->name : "", off, len);
            for (uint32_t i = 0; i < len; ++i)
            {
                line[n++] = digits[bytes[off + i] >> 4];
                line[n++] = digits[bytes[off + i] & 0xF];
            }
            line[n] = '\0';
            Com_Printf(0, "%s\n", line);
        }
    };
    traceArray("ROW", grid.rowDataStart, rowCount * 2u);
    traceArray("RAW", grid.rawRowData, grid.rawRowDataSize);
    traceArray("ENTRY", grid.entries, grid.entryCount * sizeof(GfxLightGridEntry));
    traceArray("COLOR", grid.colors, wireColorCount * sizeof(GfxLightGridColors));
}

// Live GfxWorld driver: consumes one inline GfxWorld body in exact db_load
// order (the walk-only reader's order), widening the registry-facing
// subset into a zone-arena transaction and committing once. Structural
// spans the registry does not need are consumed-and-discarded; every span
// formula mirrors the walk-only reader's overflow-guarded arithmetic so a
// divergence desynchronizes loudly at the next nested body instead of
// silently. Anything outside the proven fixture shapes fails loudly.
bool RetailWalkLiveLoadGfxWorld(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                                XAssetHeader *header, uint32_t *handle,
                                uint32_t directoryOffset, uint32_t directoryBytes,
                                const char *const *stubNames, uint32_t stubCount,
                                RetailWireTechniqueCache *techCache,
                                RetailWorldLoadContext *sharedContext)
{
    constexpr uint32_t kBodyBytes = 732;
    if (!session || !session->active || !reader || !header || !handle)
        return false;
    *header = {};
    *handle = 0;

    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!RetailZoneLoadSessionReadStream(session, reader, 0, kBodyBytes, 4))
    {
        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: root unreadable\n");
        return false;
    }
    const uint8_t *body = session->zoneMemory->blocks[0].data + bodyStart;
    const uint32_t nameRef = ReadLe32(body);
    const uint32_t baseNameRef = ReadLe32(body + 4);
    const uint32_t planeCount = ReadLe32(body + 8);
    const uint32_t nodeCount = ReadLe32(body + 12);
    const uint32_t indexCount = ReadLe32(body + 16);
    const uint32_t indicesRef = ReadLe32(body + 20);
    const uint32_t surfaceCount = ReadLe32(body + 24);
    const uint32_t skySurfCount = ReadLe32(body + 32);
    const uint32_t skyStartSurfsRef = ReadLe32(body + 36);
    const uint32_t skyImageRef = ReadLe32(body + 40);
    // Confirmed against OAT's own IW3 ZoneCode loader (gfxworld_iw3_load_db.cpp:
    // "fillAccessor.Fill(varGfxWorld->skySamplerState, 44);"), not inferred from
    // the gap between skyImageRef and vertexCount alone -- both this reader and
    // the walk-only one already correctly treat this one byte (plus 3 native
    // padding bytes) as unconsumed body space, so no stream-cursor change is
    // needed, only extracting the value RB_SetBspImages requires.
    const uint8_t skySamplerState = body[44];
    const uint32_t vertexCount = ReadLe32(body + 48);
    const uint32_t verticesRef = ReadLe32(body + 52);
    const uint32_t vldSize = ReadLe32(body + 60);
    const uint32_t vldRef = ReadLe32(body + 64);
    const uint32_t sunLightRef = ReadLe32(body + 200);
    const uint32_t sunPrimaryLightIndex = ReadLe32(body + 216);
    const uint32_t primaryLightCount = ReadLe32(body + 220);
    const uint32_t cullGroupCount = ReadLe32(body + 224);
    const uint32_t probeCount = ReadLe32(body + 228);
    const uint32_t probesRef = ReadLe32(body + 232);
    const uint32_t probeTexturesRef = ReadLe32(body + 236);
    const uint32_t cellCount = ReadLe32(body + 240);
    const uint32_t dpvsPlanesRef = ReadLe32(body + 244);
    const uint32_t dpvsNodesRef = ReadLe32(body + 248);
    const uint32_t sceneEntCellBitsRef = ReadLe32(body + 252);
    const uint32_t cellsRef = ReadLe32(body + 260);
    const uint32_t lightmapCount = ReadLe32(body + 264);
    const uint32_t lightmapsRef = ReadLe32(body + 268);
    const uint32_t gridRowAxis = ReadLe32(body + 292);
    const uint32_t gridRowDataRef = ReadLe32(body + 300);
    const uint32_t gridRawBytes = ReadLe32(body + 304);
    const uint32_t gridRawRef = ReadLe32(body + 308);
    const uint32_t gridEntryCount = ReadLe32(body + 312);
    const uint32_t gridEntriesRef = ReadLe32(body + 316);
    const uint32_t gridColorCount = ReadLe32(body + 320);
    const uint32_t gridColorsRef = ReadLe32(body + 324);
    const uint32_t lightmapPrimaryRef = ReadLe32(body + 328);
    const uint32_t lightmapSecondaryRef = ReadLe32(body + 332);
    const uint32_t modelCount = ReadLe32(body + 336);
    const uint32_t modelsRef = ReadLe32(body + 340);
    // The root's world-space bounds and checksum follow models at their
    // serialized ILP32 offsets.  R_LoadWorld consumes the checksum, and
    // first-frame camera validation must use the retail world extent rather
    // than the zero-filled transaction default.
    const uint32_t materialMemoryCount = ReadLe32(body + 372);
    const uint32_t materialMemoryRef = ReadLe32(body + 376);
    const uint32_t spriteMaterialRef = ReadLe32(body + 384);
    const uint32_t flareMaterialRef = ReadLe32(body + 388);
    const uint32_t outdoorImageRef = ReadLe32(body + 540);
    const uint32_t cellCasterBitsRef = ReadLe32(body + 544);
    const uint32_t sceneDynModelRef = ReadLe32(body + 548);
    const uint32_t sceneDynBrushRef = ReadLe32(body + 552);
    const uint32_t primaryLightEntityShadowVisRef = ReadLe32(body + 556);
    const uint32_t dynEntShadowVisRef[2] = {ReadLe32(body + 560), ReadLe32(body + 564)};
    const uint32_t nonSunRef = ReadLe32(body + 568);
    const uint32_t shadowGeomRef = ReadLe32(body + 572);
    const uint32_t lightRegionRef = ReadLe32(body + 576);
    const uint32_t dpvsSmodelCount = ReadLe32(body + 580);
    const uint32_t dpvsStaticSurfaceCount = ReadLe32(body + 584);
    const uint32_t dpvsNoDecalCount = ReadLe32(body + 588);
    const uint32_t dpvsSmodelVisData = ReadLe32(body + 616);
    const uint32_t dpvsSurfaceVisData = ReadLe32(body + 620);
    const uint32_t dpvsVisRef[7] = {
        ReadLe32(body + 624), ReadLe32(body + 628), ReadLe32(body + 632),
        ReadLe32(body + 636), ReadLe32(body + 640), ReadLe32(body + 644),
        ReadLe32(body + 648)};
    const uint32_t dpvsSortedRef = ReadLe32(body + 652);
    const uint32_t dpvsInstsRef = ReadLe32(body + 656);
    const uint32_t dpvsSurfacesRef = ReadLe32(body + 660);
    const uint32_t dpvsCullGroupsRef = ReadLe32(body + 664);
    const uint32_t dpvsDrawInstsRef = ReadLe32(body + 668);
    const uint32_t dpvsSurfMatsRef = ReadLe32(body + 672);
    const uint32_t dpvsCastsShadowRef = ReadLe32(body + 676);
    const uint32_t dpvsWordCount[2] = {ReadLe32(body + 684), ReadLe32(body + 688)};
    const uint32_t dynEntClientCount[2] = {ReadLe32(body + 692), ReadLe32(body + 696)};
    const uint32_t dpvsCellBitsRef[2] = {ReadLe32(body + 700), ReadLe32(body + 704)};
    const uint32_t dpvsVisDataRef[2][3] = {{ReadLe32(body + 708), ReadLe32(body + 712),
                                            ReadLe32(body + 716)},
                                           {ReadLe32(body + 720), ReadLe32(body + 724),
                                            ReadLe32(body + 728)}};

    GfxWorld *world = RetailWorldAllocTransaction(session);
    // The context carries the (large) widened-slot table, so the
    // no-shared-caller path heap-allocates rather than spending ~100KB of
    // DB-thread stack. A caller-provided zone-scoped context carries that
    // table across every asset in the zone (top-level XModels widen into
    // it, so world draw-inst slots can alias their slots by raw
    // indirection); the world-path default for model registration is false.
    std::unique_ptr<RetailWorldLoadContext> heapContext;
    if (!sharedContext)
    {
        heapContext.reset(new (std::nothrow) RetailWorldLoadContext{});
        if (!heapContext)
            return false;
    }
    RetailWorldLoadContext *const contextPtr = sharedContext ? sharedContext : heapContext.get();
    RetailWorldLoadContext &context = *contextPtr;
    context.deferXModelRegistration = false;
    context.session = session;
    context.reader = reader;
    context.directoryOffset = directoryOffset;
    context.directoryBytes = directoryBytes;
    context.stubNames = stubNames;
    context.stubCount = stubCount;
    context.techCache = techCache;
    if (!world)
        return false;
    std::memcpy(world->mins, body + 344, sizeof(world->mins));
    std::memcpy(world->maxs, body + 356, sizeof(world->maxs));
    world->checksum = ReadLe32(body + 368);
    // The linker serializes the weather lookup that maps world space onto the
    // baked $outdoor height map (inline 512x512 L8 payload at body + 540)
    // right after sunflare_t (380 + 0x60).  Left zero, every world point
    // samples the same texel and precipitation culls itself everywhere.
    // Field order cross-checked against OAT's IW3 GfxWorld
    // (outdoorLookupMatrix immediately precedes outdoorImage).
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            const float v = ReadLeFloat(body + 476 + 4 * (4 * row + col));
            if (!std::isfinite(v))
            {
                Com_Printf(0, "RetailWorld: outdoor lookup matrix [%d][%d] is not finite\n", row, col);
                return false;
            }
            world->outdoorLookupMatrix[row][col] = v;
        }
    }
    Com_Printf(0, "RetailWorld: outdoor lookup scale=%g,%g,%g translate=%g,%g,%g w=%g\n",
               world->outdoorLookupMatrix[0][0], world->outdoorLookupMatrix[1][1],
               world->outdoorLookupMatrix[2][2], world->outdoorLookupMatrix[3][0],
               world->outdoorLookupMatrix[3][1], world->outdoorLookupMatrix[3][2],
               world->outdoorLookupMatrix[3][3]);
    Com_Printf(0, "RetailWorld: root planes=%u nodes=%u indices=%u surfaces=%u sky=%u verts=%u vld=%u sun=%u sunPrimary=%u primaryLights=%u probes=%u cells=%u lightmaps=%u models=%u matmem=%u skyImg=0x%x sunRef=0x%x outdoor=0x%x staticSurfs=%u\n",
               planeCount, nodeCount, indexCount, surfaceCount, skySurfCount, vertexCount,
               vldSize, sunLightRef != 0, sunPrimaryLightIndex, primaryLightCount, probeCount,
               cellCount, lightmapCount, modelCount, materialMemoryCount, skyImageRef,
               sunLightRef, outdoorImageRef, dpvsStaticSurfaceCount);
    g_lastMaterials = 0;
    g_lastImages = 0;
    g_lastLightDefs = 0;
    g_lastBlock1Bytes = 0;

    uint32_t nameOffset = 0;
    const char *worldName = nullptr;
    uint32_t baseOffset = 0;
    const char *baseName = nullptr;
    if (!RetailWorldStreamName(&context, nameRef, &nameOffset, &worldName) || !worldName ||
        !RetailWorldStreamName(&context, baseNameRef, &baseOffset, &baseName) || !baseName)
    {
        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: world names unreadable\n");
        return false;
    }
    char *nameCopy = static_cast<char *>(RetailZoneLoadSessionAlloc(
        session, std::strlen(worldName) + 1, 1));
    char *baseCopy = static_cast<char *>(RetailZoneLoadSessionAlloc(
        session, std::strlen(baseName) + 1, 1));
    if (!nameCopy || !baseCopy)
        return false;
    std::memcpy(nameCopy, worldName, std::strlen(worldName) + 1);
    std::memcpy(baseCopy, baseName, std::strlen(baseName) + 1);
    world->name = nameCopy;
    world->baseName = baseCopy;

    // GfxWorld::sunParse -- the map compiler's already-parsed worldspawn sun
    // block, serialized inline in the root record at ILP32 offsets 72..199
    // (0x80 bytes, immediately before the sunLight pointer this reader
    // already takes from offset 200).  The fastfile world path has no text
    // parser (R_ParseSunLight/R_LoadSunSettings belong to the legacy
    // Com_GetBspLump loader), so this struct is the *only* source of sun and
    // ambient values on the retail path: R_LoadWorld feeds it to
    // R_CopyParseParamsToDvars, then R_UpdateLightsFromDvars ->
    // R_InterpretSunLightParseParamsIntoLights -> R_SetUpSunLight, the sole
    // producer of rgp.world->sunLight->color and thus of
    // CONST_SRC_CODE_SUN_DIFFUSE for every lit BSP surface.  A zero-filled
    // sunParse therefore renders the whole world with no sun term at all.
    // Widened field by field; every member is a scalar/char array with
    // identical ILP32 and LP64 placement, so the offsets below are the
    // serialized ones.
    static_assert(sizeof(SunLightParseParams) == 0x80,
                  "GfxWorld::sunParse occupies serialized body[72..199]");
    {
        const uint8_t *sunParse = body + 72;
        std::memcpy(world->sunParse.name, sunParse, sizeof(world->sunParse.name));
        world->sunParse.name[sizeof(world->sunParse.name) - 1] = '\0';
        world->sunParse.ambientScale = ReadLeFloat(sunParse + 64);
        for (uint32_t channel = 0; channel < 3; ++channel)
            world->sunParse.ambientColor[channel] = ReadLeFloat(sunParse + 68 + 4 * channel);
        world->sunParse.diffuseFraction = ReadLeFloat(sunParse + 80);
        world->sunParse.sunLight = ReadLeFloat(sunParse + 84);
        for (uint32_t channel = 0; channel < 3; ++channel)
            world->sunParse.sunColor[channel] = ReadLeFloat(sunParse + 88 + 4 * channel);
        for (uint32_t channel = 0; channel < 3; ++channel)
            world->sunParse.diffuseColor[channel] = ReadLeFloat(sunParse + 100 + 4 * channel);
        world->sunParse.diffuseColorHasBeenSet = sunParse[112] != 0;
        for (uint32_t axis = 0; axis < 3; ++axis)
            world->sunParse.angles[axis] = ReadLeFloat(sunParse + 116 + 4 * axis);
        Com_Printf(0,
                   "RetailWorld: sunParse name='%s' ambient=%.4f ambientColor=%.3f/%.3f/%.3f "
                   "diffuseFraction=%.4f sunLight=%.4f sunColor=%.3f/%.3f/%.3f "
                   "diffuseColor=%.3f/%.3f/%.3f diffuseSet=%u angles=%.3f/%.3f/%.3f\n",
                   world->sunParse.name, world->sunParse.ambientScale,
                   world->sunParse.ambientColor[0], world->sunParse.ambientColor[1],
                   world->sunParse.ambientColor[2], world->sunParse.diffuseFraction,
                   world->sunParse.sunLight, world->sunParse.sunColor[0],
                   world->sunParse.sunColor[1], world->sunParse.sunColor[2],
                   world->sunParse.diffuseColor[0], world->sunParse.diffuseColor[1],
                   world->sunParse.diffuseColor[2],
                   world->sunParse.diffuseColorHasBeenSet ? 1u : 0u,
                   world->sunParse.angles[0], world->sunParse.angles[1],
                   world->sunParse.angles[2]);
    }

    world->indexCount = static_cast<int>(indexCount);
    if (indicesRef)
    {
        if (indexCount > UINT32_MAX / 2u)
            return false;
        void *indices = nullptr;
        if (!RetailWorldRetainSpan(&context, indexCount * 2u, 2, &indices))
            return false;
        world->indices = static_cast<uint16_t *>(indices);
    }
    world->surfaceCount = static_cast<int>(surfaceCount);
    if (skyStartSurfsRef)
    {
        if (skySurfCount > UINT32_MAX / 4u)
            return false;
        void *starts = nullptr;
        if (!RetailWorldRetainSpan(&context, skySurfCount * 4u, 4, &starts))
            return false;
        world->skyStartSurfs = static_cast<int *>(starts);
    }
    world->skySurfCount = static_cast<int>(skySurfCount);
    Com_Printf(0, "RetailWorld: slot skyImage ref=0x%08x\n", skyImageRef);
    if (!RetailWorldWidenImage(&context, skyImageRef, &world->skyImage))
        return false;
    world->skySamplerState = skySamplerState;
    if (sunLightRef)
    {
        if (sunLightRef != kInlineRef)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: non-inline sun 0x%08x\n", sunLightRef);
            return false;
        }
        uint8_t sunRecord[64]{};
        if (!RetailWorldConsume(&context, sunRecord, sizeof(sunRecord), 4))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: sun record unreadable\n");
            return false;
        }
        Com_Printf(0, "RetailWorld: sun lightdef slot=0x%08x\n", ReadLe32(sunRecord + 60));
        GfxLight *sun = static_cast<GfxLight *>(RetailZoneLoadSessionAlloc(
            session, sizeof(GfxLight), alignof(GfxLight)));
        if (!sun)
            return false;
        std::memset(sun, 0, sizeof(*sun));
        // The 64-byte wire record is the ILP32 GfxLight image: every field
        // before the trailing def pointer is pointer-free scalars with
        // identical LP64 layout, so the first 60 bytes copy verbatim and
        // only def resolves separately. A null sun-def slot stays null
        // (killhouse carries none; the direct lightdef serves elsewhere):
        // R_ paths that need it fail loudly at preflight with
        // full context, never here with none.
        std::memcpy(sun, sunRecord, 60);
        GfxLightDef *sunDef = nullptr;
        if (!RetailWorldWidenLightDef(
                &context,
                ReadLe32(sunRecord + 60),
                &sunDef))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: sun lightdef unresolved\n");
            return false;
        }
        sun->def = sunDef;
        world->sunLight = sun;
    }
    world->sunPrimaryLightIndex = sunPrimaryLightIndex;
    world->primaryLightCount = primaryLightCount;
    if (probesRef)
    {
        if (probeCount > UINT32_MAX / 16u)
            return false;
        GfxReflectionProbe *probes = static_cast<GfxReflectionProbe *>(
            RetailZoneLoadSessionAlloc(session, probeCount * sizeof(GfxReflectionProbe),
                                       alignof(GfxReflectionProbe)));
        uint8_t *rawProbes = probeCount ? static_cast<uint8_t *>(RetailZoneLoadSessionAlloc(
                                              session, probeCount * 16u, 4))
                                        : nullptr;
        if ((!probes || !rawProbes) && probeCount)
            return false;
        // Load_GfxReflectionProbeArray bulk-reads every fixed 16-byte record
        // (Load_Stream(16*count)) before any element's image pointer is
        // resolved: an inline image on probe[i] must not eat probe[i+1]'s
        // still-unread record bytes, so consume the whole array first and
        // only then widen each slot.
        if (!RetailWorldConsume(&context, rawProbes, probeCount * 16u, 4))
            return false;
        const uint32_t probeSlotsStart =
            context.session->wire.cursor[4] - probeCount * 16u;
        for (uint32_t probe = 0; probe < probeCount; ++probe)
        {
            std::memset(&probes[probe], 0, sizeof(probes[probe]));
            std::memcpy(probes[probe].origin, rawProbes + probe * 16u,
                        sizeof(probes[probe].origin));
        }
        for (uint32_t probe = 0; probe < probeCount; ++probe)
        {
            const uint32_t ref = ReadLe32(rawProbes + probe * 16u + 12);
            Com_Printf(0, "RetailWorld: slot probe%u ref=0x%08x\n", probe, ref);
            if (!RetailWorldWidenImage(&context, ref, &probes[probe].reflectionImage,
                                       probeSlotsStart + probe * 16u + 12u))
                return false;
        }
        world->reflectionProbes = probes;
    }
    world->reflectionProbeCount = probeCount;
    if (!RetailWorldRuntimeBytes(&context, probeTexturesRef, (uint64_t)probeCount * 4u, 4))
        return false;
    // Device-texture array R_LoadWorld itself writes into unconditionally
    // (src/gfx_d3d/r_bsp.cpp's reflectionProbeIndex loop) for every probe
    // -- must exist and be sized before that call, or it is a null-pointer
    // write. RetailWorldRuntimeBytes above only mirrors the walk-only
    // reader's block-1 byte accounting (a runtime-only, discarded region);
    // it allocates no addressable storage, so this needs its own real,
    // zone-lifetime arena span. Each GfxTexture is a zero-initialized
    // null basemap until device-texture upload wiring exists -- safe
    // for R_LoadWorld's own copy (a handle copy, not a dereference), not
    // yet a real reflection texture.
    if (probeCount)
    {
        if (probeCount > UINT32_MAX / sizeof(GfxTexture))
            return false;
        GfxTexture *probeTextures = static_cast<GfxTexture *>(RetailZoneLoadSessionAlloc(
            session, probeCount * sizeof(GfxTexture), alignof(GfxTexture)));
        if (!probeTextures)
            return false;
        std::memset(probeTextures, 0, probeCount * sizeof(GfxTexture));
        world->reflectionProbeTextures = probeTextures;
    }
    // DPVS planes/nodes: R_CellForPoint walks the node tree against
    // the plane array every rendered frame, so both widen for real now.
    // cplane_s is a pointer-free 20-byte record (normal/dist/type/signbits/
    // pad), identical to the wire layout, so the array is a verbatim copy;
    // nodes are a plain u16 array. The plane slot stays inline-only (the
    // walk-first validator already proved alias validity; only the inline
    // form reaches here with bytes to own).
    static_assert(sizeof(cplane_s) == 20, "cplane_s must match the 20-byte wire record");
    if (dpvsPlanesRef)
    {
        if (dpvsPlanesRef != kInlineRef)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: non-inline planes 0x%08x\n",
                       dpvsPlanesRef);
            return false;
        }
        if (planeCount > UINT32_MAX / sizeof(cplane_s))
            return false;
        cplane_s *planes = static_cast<cplane_s *>(RetailZoneLoadSessionAlloc(
            session, planeCount * sizeof(cplane_s), alignof(cplane_s)));
        if (!planes && planeCount)
            return false;
        if (!RetailWorldConsume(&context, planes, planeCount * 20u, 4))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: dpvs planes unreadable\n");
            return false;
        }
        world->dpvsPlanes.planes = planes;
    }
    if (dpvsNodesRef)
    {
        if (nodeCount > UINT32_MAX / 2u)
            return false;
        uint16_t *nodes = static_cast<uint16_t *>(RetailZoneLoadSessionAlloc(
            session, nodeCount * 2u, alignof(uint16_t)));
        if (!nodes && nodeCount)
            return false;
        if (!RetailWorldConsume(&context, nodes, nodeCount * 2u, 2))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: dpvs nodes unreadable\n");
            return false;
        }
        world->dpvsPlanes.nodes = nodes;
    }
    world->dpvsPlanes.cellCount = static_cast<int>(cellCount);
    world->planeCount = static_cast<int>(planeCount);
    world->nodeCount = static_cast<int>(nodeCount);
    if (cellCount && !world->dpvsPlanes.nodes)
    {
        // R_CellForPoint dereferences nodes[0] before any bounds check;
        // a zero-node world with cells is a defect, not a supported shape.
        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: cellCount=%u without nodes\n",
                   cellCount);
        return false;
    }
    if (!RetailWorldRuntimeBytes(&context, sceneEntCellBitsRef, (uint64_t)cellCount << 10, 4))
    {
        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: scene entity cell bits unexpandable\n");
        return false;
    }
    // R_ClearDpvsScene/R_FilterEntitiesIntoCells touch sceneEntCellBits
    // per cell (r_dpvs.cpp's 4*(entCount>>5) memset per 128-u32 cell row),
    // so the runtime blob gets real zone-lifetime storage sized exactly
    // like the walk's block-1 accounting (cellCount << 10 bytes).
    if (cellCount)
    {
        uint32_t *sceneEntCellBits = static_cast<uint32_t *>(RetailZoneLoadSessionAlloc(
            session, static_cast<size_t>(cellCount) << 10, alignof(uint32_t)));
        if (!sceneEntCellBits)
            return false;
        std::memset(sceneEntCellBits, 0, static_cast<size_t>(cellCount) << 10);
        world->dpvsPlanes.sceneEntCellBits = sceneEntCellBits;
    }
    if (cellsRef)
    {
        if (cellCount > UINT32_MAX / 56u)
            return false;
        // Cell-graph internals recurse exactly like the walk-only reader
        // (reuse, not a second implementation); a placement trace records
        // where every nested span landed in the session block so the
        // widener can build the native graph from those exact offsets.
        //
        // The reused walker streams through the ordinary session stream,
        // which lands dense bytes and auto-syncs the reader mirror forward
        // at every step: no cursor restore follows (restoring backward
        // would relag the reader and shift every later absolute
        // placement). The session cursor may stay advanced (each family
        // reads what it wrote, and no later Sync can fire backward).
        if (!RetailZoneLoadSessionReadStream(session, reader, 4, cellCount * 56u, 4))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: cells unreadable\n");
            return false;
        }
        const uint32_t cellsStart = session->wire.cursor[4] - cellCount * 56u;
        context.nestedBodyBytes += cellCount * 56u;
        uint32_t budget = (session->zoneMemory->blocks[4].size - session->wire.cursor[4]) / 56u;
        // Heap-allocated from the zone arena, not a stack local: at 65536
        // entries (see RetailGfxCellTrace, db_retail_walk.h) this struct is
        // over 1 MiB, far past a safe stack frame on the real Switch target.
        RetailGfxCellTrace *tracePtr = static_cast<RetailGfxCellTrace *>(
            RetailZoneLoadSessionAlloc(session, sizeof(RetailGfxCellTrace),
                                       alignof(RetailGfxCellTrace)));
        if (!tracePtr)
            return false;
        std::memset(tracePtr, 0, sizeof(*tracePtr));
        RetailGfxCellTrace &trace = *tracePtr;
        GfxCell *cells = static_cast<GfxCell *>(RetailZoneLoadSessionAlloc(
            session, cellCount * sizeof(GfxCell), alignof(GfxCell)));
        if (!cells && cellCount)
            return false;
        for (uint32_t cell = 0; cell < cellCount; ++cell)
        {
            RetailWalkDirectoryRecord tmpRecord{};
            tmpRecord.header = kInlineRef;
            RetailWalkDirectoryResult tmpSummary{};
            if (!ReadRetailGfxCellAt(session, reader, cellsStart + cell * 56u, &budget,
                                      &tmpRecord, &tmpSummary, &trace))
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: cell %u unreadable\n", cell);
                return false;
            }
        }
        if (trace.overflowed)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: cell trace overflowed (%u entries)\n",
                       trace.count);
            return false;
        }
        // Heap-allocated like the trace above: cellMap/listMap alone are
        // over 1 MiB at their real-data-sized capacities, again far past a
        // safe stack frame on the real Switch target.
        RetailCellGraphWidener *widenerPtr = static_cast<RetailCellGraphWidener *>(
            RetailZoneLoadSessionAlloc(session, sizeof(RetailCellGraphWidener),
                                       alignof(RetailCellGraphWidener)));
        if (!widenerPtr)
            return false;
        std::memset(widenerPtr, 0, sizeof(*widenerPtr));
        RetailCellGraphWidener &widener = *widenerPtr;
        widener.ok = true;
        widener.session = session;
        widener.trace = &trace;
        widener.contextReader = reader;
        for (uint32_t cell = 0; cell < cellCount; ++cell)
            widener.WidenCellContents(cellsStart + cell * 56u, &cells[cell]);
        if (widener.ok)
            widener.ResolveCellAliasFixups(cells, cellCount);
        Com_Printf(0,
                   "RetailWalkLiveLoadGfxWorld: cell graph widened=%u/%u entries cells=%u "
                   "portalAliases=%u/%u inCellArray=%u lists=%u ok=%d\n",
                   widener.nextEntry, trace.count, widener.cellMapCount, widener.aliasBound,
                   widener.cellFixupCount, widener.aliasBoundInArray, widener.listMapCount,
                   widener.ok ? 1 : 0);
        if (!widener.ok || widener.nextEntry != trace.count)
            return false;
        world->cells = cells;
    }
    // Mirrors R_LoadWorldInternal's own cellBitsCount (r_bsp_load_obj.cpp):
    // the per-cell ent-bit row count the scene-entity machinery uses.
    world->cellBitsCount = 16 * ((cellCount + 127) >> 7);
    if (lightmapsRef)
    {
        if (lightmapCount > UINT32_MAX / 8u)
            return false;
        GfxLightmapArray *maps = static_cast<GfxLightmapArray *>(
            RetailZoneLoadSessionAlloc(session, lightmapCount * sizeof(GfxLightmapArray),
                                       alignof(GfxLightmapArray)));
        uint8_t *rawMaps = lightmapCount ? static_cast<uint8_t *>(RetailZoneLoadSessionAlloc(
                                                session, lightmapCount * 8u, 4))
                                         : nullptr;
        if ((!maps || !rawMaps) && lightmapCount)
            return false;
        // Load_GfxLightmapArrayArray bulk-reads every fixed 8-byte record
        // before resolving either image pointer, same ordering rule as
        // reflection probes above.
        if (!RetailWorldConsume(&context, rawMaps, lightmapCount * 8u, 4))
            return false;
        const uint32_t lightmapSlotsStart =
            context.session->wire.cursor[4] - lightmapCount * 8u;
        for (uint32_t map = 0; map < lightmapCount; ++map)
            std::memset(&maps[map], 0, sizeof(maps[map]));
        for (uint32_t map = 0; map < lightmapCount; ++map)
        {
            const uint32_t primaryRef = ReadLe32(rawMaps + map * 8u);
            const uint32_t secondaryRef = ReadLe32(rawMaps + map * 8u + 4);
            Com_Printf(0, "RetailWorld: slot lightmap%u refs=0x%08x,0x%08x\n", map, primaryRef,
                       secondaryRef);
            if (!RetailWorldWidenImage(&context, primaryRef, &maps[map].primary,
                                       lightmapSlotsStart + map * 8u) ||
                !RetailWorldWidenImage(&context, secondaryRef, &maps[map].secondary,
                                       lightmapSlotsStart + map * 8u + 4u))
                return false;
        }
        world->lightmaps = maps;
    }
    world->lightmapCount = static_cast<int>(lightmapCount);
    // Light grid CPU data (the static-model
    // ground-lighting path -- R_AllocStaticModelLighting / RB_PatchModel
    // Lighting -- indexes world->lightGrid.colors by sampled grid index, so
    // a static-model draw needs the grid's CPU arrays resident. Device
    // textures are not handled here; this is plain data retention, matching
    // Load_GfxLightGrid's exact span order: rows, raw bytes, entries,
    // colors. The PC loader (R_LoadLightGridColors) also appends one zeroed
    // default entry past the wire count, which grid clamping may address.
    world->lightGrid.hasLightRegions = body[272];
    world->lightGrid.sunPrimaryLightIndex = ReadLe32(body + 276);
    for (int i = 0; i < 3; ++i)
    {
        world->lightGrid.mins[i] =
            static_cast<uint16_t>(body[280 + i * 2] | (body[281 + i * 2] << 8));
        world->lightGrid.maxs[i] =
            static_cast<uint16_t>(body[286 + i * 2] | (body[287 + i * 2] << 8));
    }
    const uint32_t gridColAxis = ReadLe32(body + 296);
    world->lightGrid.rowAxis = gridRowAxis;
    world->lightGrid.colAxis = gridColAxis;
    world->lightGrid.rawRowDataSize = gridRawBytes;
    world->lightGrid.entryCount = gridEntryCount;
    world->lightGrid.colorCount = gridColorCount;
    if (gridRowDataRef)
    {
        if (gridRowAxis > 2)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: bad grid row axis %u\n", gridRowAxis);
            return false;
        }
        const uint32_t lo = static_cast<uint32_t>(body[272 + 8 + gridRowAxis * 2]) |
                            (static_cast<uint32_t>(body[272 + 9 + gridRowAxis * 2]) << 8);
        const uint32_t hi = static_cast<uint32_t>(body[272 + 14 + gridRowAxis * 2]) |
                            (static_cast<uint32_t>(body[272 + 15 + gridRowAxis * 2]) << 8);
        const uint32_t span = hi - lo + 1u;
        void *rows = nullptr;
        if (span > UINT32_MAX / 2u ||
            !RetailWorldRetainSpan(&context, span * 2u, 2, &rows))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: grid rows unreadable\n");
            return false;
        }
        world->lightGrid.rowDataStart = static_cast<uint16_t *>(rows);
    }
    if (gridRawRef)
    {
        void *raw = nullptr;
        if (!RetailWorldRetainSpan(&context, gridRawBytes, 1, &raw))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: grid raw unreadable\n");
            return false;
        }
        world->lightGrid.rawRowData = static_cast<uint8_t *>(raw);
    }
    if (gridEntriesRef)
    {
        void *entries = nullptr;
        if (gridEntryCount > UINT32_MAX / 4u ||
            !RetailWorldRetainSpan(&context, gridEntryCount * 4u, 4, &entries))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: grid entries unreadable\n");
            return false;
        }
        world->lightGrid.entries = static_cast<GfxLightGridEntry *>(entries);
    }
    if (gridColorsRef)
    {
        // One extra zeroed default entry past the wire count, mirroring
        // R_LoadLightGridColors's colorCount+1 allocation. The default
        // entry is allocated, NOT streamed -- only the wire's own
        // colorCount entries are consumed (an extra stream consume here
        // desynchronized every following world span, caught live).
        GfxLightGridColors *colors = static_cast<GfxLightGridColors *>(
            RetailZoneLoadSessionAlloc(session, (gridColorCount + 1u) * 168u,
                                       alignof(GfxLightGridColors)));
        if (!colors || gridColorCount > UINT32_MAX / 168u - 1u)
            return false;
        std::memset(colors, 0, (gridColorCount + 1u) * 168u);
        if (!RetailWorldConsume(&context, colors, gridColorCount * 168u, 4))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: grid colors unreadable\n");
            return false;
        }
        world->lightGrid.colors = colors;
        world->lightGrid.colorCount = static_cast<uint32_t>(gridColorCount + 1u);
    }
    RetailOatTraceLightGrid(world);
    Com_Printf(0,
               "RetailWorld: lightGrid widened hasRegions=%u sunIdx=%u mins=(%u,%u,%u) "
               "maxs=(%u,%u,%u) rowAxis=%u colAxis=%u rows=%p raw=%u@%p entries=%u@%p "
               "colors=%u@%p\n",
               world->lightGrid.hasLightRegions, world->lightGrid.sunPrimaryLightIndex,
               world->lightGrid.mins[0], world->lightGrid.mins[1], world->lightGrid.mins[2],
               world->lightGrid.maxs[0], world->lightGrid.maxs[1], world->lightGrid.maxs[2],
               world->lightGrid.rowAxis, world->lightGrid.colAxis,
               (const void *)world->lightGrid.rowDataStart, world->lightGrid.rawRowDataSize,
               (const void *)world->lightGrid.rawRowData, world->lightGrid.entryCount,
               (const void *)world->lightGrid.entries, world->lightGrid.colorCount,
               (const void *)world->lightGrid.colors);
    if (!RetailWorldRuntimeBytes(&context, lightmapPrimaryRef, (uint64_t)lightmapCount * 4u, 4) ||
        !RetailWorldRuntimeBytes(&context, lightmapSecondaryRef, (uint64_t)lightmapCount * 4u, 4))
    {
        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: lightmap textures unexpandable\n");
        return false;
    }
    // Same reasoning as reflectionProbeTextures above: R_LoadWorld's own
    // lightmapIndex loop writes both arrays unconditionally for every
    // lightmap, so both need real, zone-lifetime, zeroed storage before
    // that call -- the RetailWorldRuntimeBytes calls above only mirror
    // walk-only's discarded block-1 byte accounting.
    if (lightmapCount)
    {
        if (lightmapCount > UINT32_MAX / sizeof(GfxTexture))
            return false;
        GfxTexture *primaryTextures = static_cast<GfxTexture *>(RetailZoneLoadSessionAlloc(
            session, lightmapCount * sizeof(GfxTexture), alignof(GfxTexture)));
        GfxTexture *secondaryTextures = static_cast<GfxTexture *>(RetailZoneLoadSessionAlloc(
            session, lightmapCount * sizeof(GfxTexture), alignof(GfxTexture)));
        if (!primaryTextures || !secondaryTextures)
            return false;
        std::memset(primaryTextures, 0, lightmapCount * sizeof(GfxTexture));
        std::memset(secondaryTextures, 0, lightmapCount * sizeof(GfxTexture));
        world->lightmapPrimaryTextures = primaryTextures;
        world->lightmapSecondaryTextures = secondaryTextures;
    }
    if (modelsRef)
    {
        if (modelCount > UINT32_MAX / 56u)
            return false;
        GfxBrushModel *models = static_cast<GfxBrushModel *>(
            RetailZoneLoadSessionAlloc(session, modelCount * sizeof(GfxBrushModel),
                                       alignof(GfxBrushModel)));
        if (!models && modelCount)
            return false;
        static_assert(sizeof(GfxBrushModel) == 56,
                      "GfxBrushModel must match the 56-byte wire record exactly");
        for (uint32_t model = 0; model < modelCount; ++model)
        {
            // GfxBrushModel (Load_GfxBrushModelArray: a plain 56-byte
            // Load_Stream with no per-element follow-up call) holds only
            // bounds floats and surface-range uint16s -- no pointers -- so
            // the ILP32 wire record already matches the native layout
            // exactly and widens with a verbatim copy; a real world's
            // models are never all-zero.
            uint8_t record[56]{};
            if (!RetailWorldConsume(&context, record, sizeof(record), 4))
                return false;
            std::memcpy(&models[model], record, sizeof(record));
        }
        world->models = models;
    }
    world->modelCount = static_cast<int>(modelCount);
    if (materialMemoryRef)
    {
        if (materialMemoryCount > UINT32_MAX / 8u)
            return false;
        MaterialMemory *memory = static_cast<MaterialMemory *>(
            RetailZoneLoadSessionAlloc(session, materialMemoryCount * sizeof(MaterialMemory),
                                       alignof(MaterialMemory)));
        uint8_t *rawMemory =
            materialMemoryCount ? static_cast<uint8_t *>(RetailZoneLoadSessionAlloc(
                                      session, materialMemoryCount * 8u, 4))
                                : nullptr;
        if ((!memory || !rawMemory) && materialMemoryCount)
            return false;
        // Load_MaterialMemoryArray bulk-reads every fixed 8-byte record before
        // resolving its material slots. RetailWorldConsume densely mirrors the
        // span, so this live cursor is also its linker-visible block-4 address.
        if (!RetailWorldConsume(&context, rawMemory, materialMemoryCount * 8u, 4))
            return false;
        const uint32_t arrayVirt = session->wire.cursor[4] - materialMemoryCount * 8u;
        for (uint32_t entry = 0; entry < materialMemoryCount; ++entry)
        {
            std::memset(&memory[entry], 0, sizeof(memory[entry]));
            if (!RetailWorldWidenMaterial(&context, ReadLe32(rawMemory + entry * 8u),
                                          &memory[entry].material,
                                          arrayVirt + entry * 8u))
                return false;
        }
        context.matmemCount = materialMemoryCount;
        context.matmem = memory;
        context.matmemEngineVirt = materialMemoryCount ? arrayVirt : 0;
        world->materialMemory = memory;
    }
    world->materialMemoryCount = static_cast<int>(materialMemoryCount);
    world->vertexCount = vertexCount;
    if (verticesRef)
    {
        if (vertexCount > UINT32_MAX / 44u)
            return false;
        void *verts = nullptr;
        if (!RetailWorldRetainSpan(&context, vertexCount * 44u, 4, &verts))
            return false;
        world->vd.vertices = static_cast<GfxWorldVertex *>(verts);
    }
    if (vldRef)
    {
        void *vld = nullptr;
        if (!RetailWorldRetainSpan(&context, vldSize, 1, &vld))
            return false;
        world->vertexLayerDataSize = vldSize;
        world->vld.data = static_cast<uint8_t *>(vld);
    }
    Material *spriteMaterial = nullptr;
    Material *flareMaterial = nullptr;
    // Unused sunflare slots ship as inline garbage (unresolvable counts
    // and names -- the linker leaves dead entries in fill memory instead
    // of nulling them, same class as the zone's empty inline bodies and
    // dangling texture slots; SP drives its sun through dvars
    // (R_SetSunFromDvars), not these). Consumption stays exact either
    // way: valid counts stream tables and nested bodies field-for-field
    // before any validation fails, and oversize counts take the
    // walk-consume remainder path inside RetailWorldWidenMaterial -- so
    // binding null here on widen failure cannot desynchronize anything
    // downstream. Alias slots keep strict failure -- a dangling sunflare
    // alias is a genuine defect, and the m7 rollback proof pins exactly
    // that (gfxworld_dangle_live.ff must still fail).
    if ((spriteMaterialRef == kInlineRef || spriteMaterialRef == kInsertRef) &&
        !RetailWorldWidenMaterial(&context, spriteMaterialRef, &spriteMaterial))
    {
        Com_Printf(0, "RetailWorld: TOLERATED null sunflare spriteMaterial ref=0x%08x\n",
                   spriteMaterialRef);
        spriteMaterial = nullptr;
    }
    else if (spriteMaterialRef != kInlineRef && spriteMaterialRef != kInsertRef &&
             !RetailWorldWidenMaterial(&context, spriteMaterialRef, &spriteMaterial))
    {
        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: sunflare spriteMaterial unresolved\n");
        return false;
    }
    if ((flareMaterialRef == kInlineRef || flareMaterialRef == kInsertRef) &&
        !RetailWorldWidenMaterial(&context, flareMaterialRef, &flareMaterial))
    {
        Com_Printf(0, "RetailWorld: TOLERATED null sunflare flareMaterial ref=0x%08x\n",
                   flareMaterialRef);
        flareMaterial = nullptr;
    }
    else if (flareMaterialRef != kInlineRef && flareMaterialRef != kInsertRef &&
             !RetailWorldWidenMaterial(&context, flareMaterialRef, &flareMaterial))
    {
        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: sunflare flareMaterial unresolved\n");
        return false;
    }
    world->sun.spriteMaterial = spriteMaterial;
    world->sun.flareMaterial = flareMaterial;
    // the flare is only valid when both materials resolved.  A dead
    // inline slot leaves hasValidData false so RB_DrawSun never selects a
    // null material; the tolerance line above still reports it.
    world->sun.hasValidData = spriteMaterial != nullptr && flareMaterial != nullptr;
    if (!world->sun.hasValidData)
        Com_Printf(0, "RetailWorld: sunflare disabled (unresolved sprite=%d flare=%d)\n",
                   spriteMaterial ? 1 : 0, flareMaterial ? 1 : 0);
    Com_Printf(0, "RetailWorld: slot outdoor ref=0x%08x\n", outdoorImageRef);
    // the outdoor image is consumer-visible -- RB_SetupGfxCmdBufInput
    // binds it as the TEXTURE_SRC_CODE_OUTDOOR code image every frame and a
    // null there is a fatal sampler bind on the first lit surface.  Every
    // real zone ships it (killhouse: `$outdoor`), so an unresolved slot of
    // either form fails the load instead of binding null.
    if (!RetailWorldWidenImage(&context, outdoorImageRef, &world->outdoorImage) ||
        !world->outdoorImage)
    {
        Com_Printf(0, "RetailWorld: outdoor image unresolved ref=0x%08x\n", outdoorImageRef);
        return false;
    }
    const uint64_t nonSunLight =
        ((uint64_t)primaryLightCount + 0x100000000ull - (uint64_t)sunPrimaryLightIndex - 1u) &
        0xffffffffu;
    if (!RetailWorldRuntimeBytes(&context, cellCasterBitsRef,
                                 4u * ((uint64_t)cellCount * (((uint64_t)cellCount + 31u) >> 5)),
                                 4) ||
        !RetailWorldRuntimeBytes(&context, sceneDynModelRef, 6u * dynEntClientCount[0], 4) ||
        !RetailWorldRuntimeBytes(&context, sceneDynBrushRef, 4u * dynEntClientCount[1], 4) ||
        !RetailWorldRuntimeBytes(&context, primaryLightEntityShadowVisRef,
                                 4u * (nonSunLight << 12), 4) ||
        !RetailWorldRuntimeBytes(&context, dynEntShadowVisRef[0],
                                 4u * (uint64_t)dynEntClientCount[0] * nonSunLight, 4) ||
        !RetailWorldRuntimeBytes(&context, dynEntShadowVisRef[1],
                                 4u * (uint64_t)dynEntClientCount[1] * nonSunLight, 4) ||
        !RetailWorldRuntimeBytes(&context, nonSunRef, dynEntClientCount[0], 1))
    {
        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: runtime blobs unexpandable\n");
        return false;
    }
    // Runtime blobs with real writers/readers get zone-lifetime zeroed
    // storage sized exactly like the walk's block-1 accounting above:
    // R_GenerateShadowMapCasterCells memsets cellCasterBits for every
    // cell (unconditionally from R_LoadWorld), and R_ClearDpvsScene
    // memsets sceneDynModel/sceneDynBrush each frame.
    if (cellCount)
    {
        const uint64_t casterBytes =
            4ull * cellCount * (((uint64_t)cellCount + 31u) >> 5);
        if (!cellCasterBitsRef || casterBytes > SIZE_MAX)
            return false;
        uint32_t *casters = static_cast<uint32_t *>(RetailZoneLoadSessionAlloc(
            session, static_cast<size_t>(casterBytes), alignof(uint32_t)));
        if (!casters)
            return false;
        std::memset(casters, 0, static_cast<size_t>(casterBytes));
        world->cellCasterBits = casters;
    }
    // Runtime primary-light entity shadow bits. R_LinkBoxEntityToPrimaryLights
    // and R_LinkSphereEntityToPrimaryLights run for every linked entity (the
    // script_brushmodel props are the first live users in the bounded
    // frame) and write this array; the model-lighting path reads it. The PC
    // loader allocates it in R_AllocPrimaryLightBuffers, which the retail
    // decode path never ran, so the pointer stayed null and the first linked
    // brush faulted at address 0x0. Same size/shape as the PC allocator
    // (4 bytes per 32 lights-entity bits, 4096 bits budget per non-sun
    // light), zeroed for the zone's lifetime.
    if (primaryLightCount > sunPrimaryLightIndex + 1u)
    {
        const uint32_t relevantLights = primaryLightCount - (sunPrimaryLightIndex + 1u);
        const uint64_t shadowVisBytes =
            4ull * (((static_cast<uint64_t>(relevantLights) << 12) + 31ull) >> 5);
        if (shadowVisBytes > SIZE_MAX)
            return false;
        uint32_t *shadowVis = static_cast<uint32_t *>(RetailZoneLoadSessionAlloc(
            session, static_cast<size_t>(shadowVisBytes), alignof(uint32_t)));
        if (!shadowVis)
            return false;
        std::memset(shadowVis, 0, static_cast<size_t>(shadowVisBytes));
        world->primaryLightEntityShadowVis = shadowVis;
    }
    // Runtime dynamic-entity primary-light buffers. R_AllocPrimaryLightBuffers
    // allocates these on the PC path; the retail decode path validated the
    // disk refs above but never published native storage, so the first
    // R_LinkDynEntToPrimaryLights(DYNENT_DRAW_MODEL) wrote
    // nonSunPrimaryLightForModelDynEnt[dynEntId] through a null pointer and
    // faulted at address 0x0. Same sizes/shape as the PC allocator, zeroed for
    // the zone's lifetime.
    // R_LinkDynEntToPrimaryLights writes nonSunPrimaryLightForModelDynEnt for
    // every model dynent even when the world has no non-sun light (it then
    // stores index 0), and R_AllocPrimaryLightBuffers sizes it from the
    // dynent count alone. Gating it on non-sun lights left it null on
    // airplane (2 primary lights, sun = 1) and faulted at map start.
    if (dynEntClientCount[0])
    {
        uint8_t *nonSun = static_cast<uint8_t *>(
            RetailZoneLoadSessionAlloc(session, dynEntClientCount[0], alignof(uint8_t)));
        if (!nonSun)
            return false;
        std::memset(nonSun, 0, dynEntClientCount[0]);
        world->nonSunPrimaryLightForModelDynEnt = nonSun;
    }
    if (primaryLightCount > sunPrimaryLightIndex + 1u)
    {
        const uint32_t relevantLights = primaryLightCount - (sunPrimaryLightIndex + 1u);
        for (uint32_t list = 0; list < 2; ++list)
        {
            if (!dynEntClientCount[list])
                continue;
            const uint64_t bits = static_cast<uint64_t>(relevantLights) * dynEntClientCount[list];
            const uint64_t bytes = 4ull * ((bits + 31ull) >> 5);
            if (bytes > SIZE_MAX)
                return false;
            uint32_t *vis = static_cast<uint32_t *>(RetailZoneLoadSessionAlloc(
                session, static_cast<size_t>(bytes), alignof(uint32_t)));
            if (!vis)
                return false;
            std::memset(vis, 0, static_cast<size_t>(bytes));
            world->primaryLightDynEntShadowVis[list] = vis;
        }
    }
    if (dynEntClientCount[0])
    {
        if (!sceneDynModelRef)
            return false;
        uint8_t *sceneDynModel = static_cast<uint8_t *>(
            RetailZoneLoadSessionAlloc(session, 6u * dynEntClientCount[0], 4));
        if (!sceneDynModel)
            return false;
        std::memset(sceneDynModel, 0, 6u * dynEntClientCount[0]);
        world->sceneDynModel = reinterpret_cast<GfxSceneDynModel *>(sceneDynModel);
    }
    if (dynEntClientCount[1])
    {
        if (!sceneDynBrushRef)
            return false;
        uint8_t *sceneDynBrush = static_cast<uint8_t *>(
            RetailZoneLoadSessionAlloc(session, 4u * dynEntClientCount[1], 4));
        if (!sceneDynBrush)
            return false;
        std::memset(sceneDynBrush, 0, 4u * dynEntClientCount[1]);
        world->sceneDynBrush = reinterpret_cast<GfxSceneDynBrush *>(sceneDynBrush);
    }
    if (shadowGeomRef)
    {
        if (primaryLightCount > UINT32_MAX / 12u)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: shadow geom count %u overflows\n",
                       primaryLightCount);
            return false;
        }
        // Load_GfxShadowGeometryArray bulk-reads every fixed 12-byte record
        // before any element's index span: an inline span on light[i] must
        // not eat light[i+1]'s still-unread record bytes, so consume the
        // whole array first and only then stream each span. The per-light
        // interleaved form misframes the stream (killhouse ord 772: spans
        // eaten out of the entries region, cascading into the hull-count
        // failure).
        uint8_t *rawGeoms = primaryLightCount ? static_cast<uint8_t *>(RetailZoneLoadSessionAlloc(
                                                 session, primaryLightCount * 12u, 4))
                                              : nullptr;
        if (!rawGeoms && primaryLightCount)
            return false;
        if (!RetailWorldConsume(&context, rawGeoms, primaryLightCount * 12u, 4))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: shadow geom unreadable\n");
            return false;
        }
        for (uint32_t geom = 0; geom < primaryLightCount; ++geom)
        {
            // Per-light shadow-index spans, mirrored from the walk-only
            // reader: the caster CELLS the renderer builds come from the
            // cell graph, but every byte here is still accounted.
            const uint8_t *entry = rawGeoms + geom * 12u;
            const uint32_t counts[2] = {
                static_cast<uint32_t>(entry[0]) | (static_cast<uint32_t>(entry[1]) << 8),
                static_cast<uint32_t>(entry[2]) | (static_cast<uint32_t>(entry[3]) << 8)};
            const uint32_t refs[2] = {ReadLe32(entry + 4), ReadLe32(entry + 8)};
            for (uint32_t side = 0; side < 2; ++side)
            {
                if (!refs[side])
                    continue;
                if (counts[side] > UINT32_MAX / 2u ||
                    !RetailWorldConsume(&context, nullptr, counts[side] * 2u, 2))
                {
                    Com_Printf(0, "RetailWalkLiveLoadGfxWorld: shadow indices unreadable\n");
                    return false;
                }
            }
        }
    }
    // Native light-region data (GfxLightRegion/GfxLightRegionHull/Axis).
    // R_LinkBoxEntityToPrimaryLights, R_GetNonSunPrimaryLightForBox and
    // R_GetNonSunPrimaryLightForSphere index rgp.world->lightRegion for every
    // non-sun light unconditionally; the PC loader allocates the array in
    // R_LoadLightRegions, but the retail decode path only streamed past these
    // records. Publish zeroed storage for every primary light first, then
    // widen the real hull/axis records below (hullCount==0 is the same shape
    // an authored hull-less light has).
    if (primaryLightCount)
    {
        if (primaryLightCount > SIZE_MAX / sizeof(GfxLightRegion))
            return false;
        GfxLightRegion *regions = static_cast<GfxLightRegion *>(RetailZoneLoadSessionAlloc(
            session, primaryLightCount * sizeof(GfxLightRegion), alignof(GfxLightRegion)));
        if (!regions)
            return false;
        std::memset(regions, 0, primaryLightCount * sizeof(GfxLightRegion));
        world->lightRegion = regions;
    }
    if (lightRegionRef)
    {
        if (primaryLightCount > UINT32_MAX / 8u)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: light region count %u overflows\n",
                       primaryLightCount);
            return false;
        }
        // Same bulk-first order as the walk-only reader (and the shadow
        // geometry above): every fixed 8-byte region record streams before
        // any region's hull span, so consume the whole array first and only
        // then stream each hull span.
        uint8_t *rawRegions = primaryLightCount ? static_cast<uint8_t *>(RetailZoneLoadSessionAlloc(
                                                  session, primaryLightCount * 8u, 4))
                                               : nullptr;
        if (!rawRegions && primaryLightCount)
            return false;
        if (!RetailWorldConsume(&context, rawRegions, primaryLightCount * 8u, 4))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: light region unreadable\n");
            return false;
        }
        for (uint32_t region = 0; region < primaryLightCount; ++region)
        {
            const uint8_t *entry = rawRegions + region * 8u;
            const uint32_t hullsRef = ReadLe32(entry + 4);
            if (!hullsRef)
                continue;
            const uint32_t hullCount = ReadLe32(entry);
            if (hullCount > UINT32_MAX / 80u ||
                hullCount > SIZE_MAX / sizeof(GfxLightRegionHull))
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: hull count %u overflows\n", hullCount);
                return false;
            }
            GfxLightRegionHull *nativeHulls = nullptr;
            if (hullCount)
            {
                nativeHulls = static_cast<GfxLightRegionHull *>(RetailZoneLoadSessionAlloc(
                    session, hullCount * sizeof(GfxLightRegionHull), alignof(GfxLightRegionHull)));
                if (!nativeHulls)
                    return false;
                std::memset(nativeHulls, 0, hullCount * sizeof(GfxLightRegionHull));
            }
            world->lightRegion[region].hullCount = hullCount;
            world->lightRegion[region].hulls = nativeHulls;
            // Same bulk-first order as the walk-only reader: every fixed
            // 80-byte hull record streams before any hull's axis span, so
            // consume all hulls first and only then stream each axis span.
            // The per-hull interleaved form eats axis bytes as hull records
            // (killhouse ord 772 region 2: hull 1's 40-byte axis misframed
            // hull 2, cascading into the hull-axis failure).
            uint8_t *rawHulls = hullCount ? static_cast<uint8_t *>(RetailZoneLoadSessionAlloc(
                                               session, hullCount * 80u, 4))
                                          : nullptr;
            if (!rawHulls && hullCount)
                return false;
            if (!RetailWorldConsume(&context, rawHulls, hullCount * 80u, 4))
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: hull unreadable\n");
                return false;
            }
            for (uint32_t hull = 0; hull < hullCount; ++hull)
            {
                const uint8_t *hullBody = rawHulls + hull * 80u;
                GfxLightRegionHull *nativeHull = &nativeHulls[hull];
                std::memcpy(nativeHull->kdopMidPoint, hullBody, sizeof(nativeHull->kdopMidPoint));
                std::memcpy(nativeHull->kdopHalfSize, hullBody + 36, sizeof(nativeHull->kdopHalfSize));
                const uint32_t axisRef = ReadLe32(hullBody + 76);
                const uint32_t axisCount = ReadLe32(hullBody + 72);
                if (axisCount > UINT32_MAX / 20u)
                {
                    Com_Printf(0, "RetailWalkLiveLoadGfxWorld: hull axis count %u overflows\n",
                               axisCount);
                    return false;
                }
                if (!axisRef)
                {
                    // No axis span on the wire (the walk tolerates this):
                    // publish the hull hull-less rather than a dangling
                    // pointer; the culler then treats the hull as covering
                    // the whole light region.
                    nativeHull->axisCount = 0;
                    nativeHull->axis = nullptr;
                    continue;
                }
                if (axisCount)
                {
                    GfxLightRegionAxis *axes = static_cast<GfxLightRegionAxis *>(
                        RetailZoneLoadSessionAlloc(session, axisCount * sizeof(GfxLightRegionAxis),
                                                   alignof(GfxLightRegionAxis)));
                    if (!axes || !RetailWorldConsume(&context, axes, axisCount * 20u, 4))
                    {
                        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: hull axis unreadable\n");
                        return false;
                    }
                    nativeHull->axisCount = axisCount;
                    nativeHull->axis = axes;
                }
                else
                {
                    nativeHull->axisCount = 0;
                    nativeHull->axis = nullptr;
                }
            }
        }
    }
    const uint32_t dpvsVisBytes[7] = {
        dpvsSmodelCount, dpvsSmodelCount, dpvsSmodelCount,
        dpvsStaticSurfaceCount, dpvsStaticSurfaceCount,
        dpvsStaticSurfaceCount, 8u * dpvsSmodelVisData};
    for (uint32_t vis = 0; vis < 7; ++vis)
    {
        // Block-1 visibility blobs: R_ClearDpvsScene memsets the six vis
        // arrays every frame and the smodel LOD table is smodel-scratch,
        // so each gets real zone-lifetime zeroed storage sized exactly
        // like the walk's block-1 accounting. The 128-byte alignment on
        // the last one mirrors the walk-only reader exactly.
        if (!RetailWorldRuntimeBytes(&context, dpvsVisRef[vis], dpvsVisBytes[vis],
                                     (vis == 6) ? 128u : 1u))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: dpvs vis unexpandable\n");
            return false;
        }
        const uint32_t bytes = dpvsVisBytes[vis];
        if (!dpvsVisRef[vis] && bytes)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: dpvs vis[%u] absent but %u bytes needed\n",
                       vis, bytes);
            return false;
        }
        if (!bytes)
            continue;
        void *storage = RetailZoneLoadSessionAlloc(session, bytes, (vis == 6) ? 128u : alignof(uint8_t));
        if (!storage)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: dpvs vis[%u] arena oom\n", vis);
            return false;
        }
        std::memset(storage, 0, bytes);
        if (vis < 3)
            world->dpvs.smodelVisData[vis] = static_cast<uint8_t *>(storage);
        else if (vis < 6)
            world->dpvs.surfaceVisData[vis - 3] = static_cast<uint8_t *>(storage);
        else
            world->dpvs.lodData = static_cast<uint32_t *>(storage);
    }
    world->dpvs.smodelVisDataCount = dpvsSmodelVisData;
    world->dpvs.surfaceVisDataCount = dpvsSurfaceVisData;
    world->dpvs.staticSurfaceCountNoDecal = dpvsNoDecalCount;
    // Draw ranges serialized in the root's inline GfxWorldDpvsStatic
    // (offsets 592-612 mirror r_gfx.h's lit/decal/emissive fields); the
    // backend add-stage iterates exactly these spans.
    world->dpvs.litSurfsBegin = ReadLe32(body + 592);
    world->dpvs.litSurfsEnd = ReadLe32(body + 596);
    world->dpvs.decalSurfsBegin = ReadLe32(body + 600);
    world->dpvs.decalSurfsEnd = ReadLe32(body + 604);
    world->dpvs.emissiveSurfsBegin = ReadLe32(body + 608);
    world->dpvs.emissiveSurfsEnd = ReadLe32(body + 612);
    if (dpvsSortedRef)
    {
        const uint64_t sorted = (uint64_t)dpvsNoDecalCount + dpvsStaticSurfaceCount;
        if (sorted > UINT32_MAX / 2u)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: sorted indices overflow\n");
            return false;
        }
        // R_AddAabbTreeSurfacesInFrustum resolves every visible surface
        // through this sorted index (leaf trees carry sorted ranges), so
        // the u16 array widens for real instead of being discarded.
        uint16_t *sortedIndex = static_cast<uint16_t *>(RetailZoneLoadSessionAlloc(
            session, static_cast<size_t>(sorted) * 2u, alignof(uint16_t)));
        if (!sortedIndex && sorted)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: sorted index arena oom\n");
            return false;
        }
        if (!RetailWorldConsume(&context, sortedIndex, static_cast<uint32_t>(sorted) * 2u, 2))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: sorted indices unreadable\n");
            return false;
        }
        world->dpvs.sortedSurfIndex = sortedIndex;
    }
    if (dpvsInstsRef)
    {
        // GfxStaticModelInst (Load_GfxStaticModelInstArray, 28 bytes, no
        // XModel pointer -- that field only exists on the separate 76-byte
        // GfxStaticModelDrawInst below) is pointer-free POD (bounds floats
        // + groundLighting color), so it widens verbatim; the R_ClearDpvs
        // smodel vis memsets and the gated smodel cull read it by index.
        static_assert(sizeof(GfxStaticModelInst) == 28,
                      "GfxStaticModelInst must match the 28-byte wire record");
        if (dpvsSmodelCount > UINT32_MAX / 28u)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: smodel inst overflow\n");
            return false;
        }
        GfxStaticModelInst *insts = static_cast<GfxStaticModelInst *>(
            RetailZoneLoadSessionAlloc(session, dpvsSmodelCount * sizeof(GfxStaticModelInst),
                                       alignof(GfxStaticModelInst)));
        if (!insts && dpvsSmodelCount)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: smodel inst arena oom\n");
            return false;
        }
        if (!RetailWorldConsume(&context, insts, dpvsSmodelCount * 28u, 4))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: smodel inst unreadable\n");
            return false;
        }
        world->dpvs.smodelInsts = insts;
        world->dpvs.smodelCount = dpvsSmodelCount;
    }
    if (dpvsSurfacesRef)
    {
        if (surfaceCount > UINT32_MAX / 48u)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: surfaceCount %u overflows\n", surfaceCount);
            return false;
        }
        GfxSurface *surfaces = static_cast<GfxSurface *>(
            RetailZoneLoadSessionAlloc(session, surfaceCount * sizeof(GfxSurface),
                                       alignof(GfxSurface)));
        uint8_t *rawSurfaces = surfaceCount ? static_cast<uint8_t *>(RetailZoneLoadSessionAlloc(
                                                  session, surfaceCount * 48u, 4))
                                            : nullptr;
        if ((!surfaces || !rawSurfaces) && surfaceCount)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: surface arena out of memory count=%u\n",
                       surfaceCount);
            return false;
        }
        // Load_GfxSurfaceArray bulk-reads every fixed 48-byte record before
        // resolving any element's material pointer, same ordering rule as
        // reflection probes above.
        if (!RetailWorldConsume(&context, rawSurfaces, surfaceCount * 48u, 4))
            return false;
        for (uint32_t surface = 0; surface < surfaceCount; ++surface)
        {
            if (!WidenGfxSurface(&context, rawSurfaces + surface * 48u, &surfaces[surface]))
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: surface %u/%u material slotRef=0x%08x unwidenable\n",
                           surface, surfaceCount,
                           ReadLe32(rawSurfaces + surface * 48u + 16));
                return false;
            }
        }
        world->dpvs.surfaces = surfaces;
    }
    world->dpvs.staticSurfaceCount = dpvsStaticSurfaceCount;
    // R_SortWorldSurfaces (r_drawsurf.cpp) runs unconditionally from
    // R_BeginFrame once a world is active and writes three arrays the old
    // coverage left deferred: dpvs.surfaceMaterials (one GfxDrawSurf per
    // models[0] surface), dpvs.surfaceCastsSunShadow (the exact
    // 4*((count-1)>>5)+4 memset span the sort performs), and the fixed
    // world->shadowGeom header array (one entry per primary light, reset by
    // R_SetPrimaryLightShadowSurfaces; per-light sortedSurfIndex spans stay
    // null, which R_AddShadowSurfaceToPrimaryLight already guards). All are
    // zone-lifetime zeroed handle storage, no renderer resources -- the same
    // contract as the probe/lightmap texture arrays. Live-proven: without
    // these the first Com_Frame after activation faults inside the sort's
    // memset (killhouse: 8492 surfaces, span 1064).
    {
        const uint32_t sortSurfCount = world->models ? world->models->surfaceCount : 0;
        if (sortSurfCount)
        {
            if (!world->dpvs.surfaces || sortSurfCount > surfaceCount)
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: sort range %u exceeds %u widened surfaces\n",
                           sortSurfCount, surfaceCount);
                return false;
            }
            if (sortSurfCount > UINT32_MAX / sizeof(GfxDrawSurf))
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: sort surface count %u overflows\n",
                           sortSurfCount);
                return false;
            }
            const uint64_t shadowSpan =
                4ull * (((uint64_t)sortSurfCount - 1u) >> 5) + 4u;
            if (shadowSpan > SIZE_MAX)
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: shadow span overflows\n");
                return false;
            }
            GfxDrawSurf *sortMaterials = static_cast<GfxDrawSurf *>(RetailZoneLoadSessionAlloc(
                session, sortSurfCount * sizeof(GfxDrawSurf), alignof(GfxDrawSurf)));
            uint32_t *sunShadowBits = static_cast<uint32_t *>(RetailZoneLoadSessionAlloc(
                session, static_cast<size_t>(shadowSpan), alignof(uint32_t)));
            if (!sortMaterials || !sunShadowBits)
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: sort arrays out of memory count=%u\n",
                           sortSurfCount);
                return false;
            }
            std::memset(sortMaterials, 0, sortSurfCount * sizeof(GfxDrawSurf));
            std::memset(sunShadowBits, 0, static_cast<size_t>(shadowSpan));
            world->dpvs.surfaceMaterials = sortMaterials;
            world->dpvs.surfaceCastsSunShadow = sunShadowBits;
        }
        if (primaryLightCount)
        {
            if (primaryLightCount > UINT32_MAX / sizeof(GfxShadowGeometry))
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: shadow geom headers %u overflow\n",
                           primaryLightCount);
                return false;
            }
            GfxShadowGeometry *geoms = static_cast<GfxShadowGeometry *>(RetailZoneLoadSessionAlloc(
                session, primaryLightCount * sizeof(GfxShadowGeometry),
                alignof(GfxShadowGeometry)));
            if (!geoms)
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: shadow geom headers out of memory\n");
                return false;
            }
            std::memset(geoms, 0, primaryLightCount * sizeof(GfxShadowGeometry));
            world->shadowGeom = geoms;
        }
    }
    if (dpvsCullGroupsRef)
    {
        // GfxCullGroup (Load_GfxCullGroupArray: a plain 32-byte Load_Stream
        // with no per-element follow-up) is pointer-free POD (bounds floats
        // + surface-count/index ints), so it widens verbatim; per-cell
        // cull-group int lists index directly into it.
        static_assert(sizeof(GfxCullGroup) == 32,
                      "GfxCullGroup must match the 32-byte wire record");
        if (cullGroupCount > UINT32_MAX / 32u)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: cull group overflow\n");
            return false;
        }
        GfxCullGroup *groups = static_cast<GfxCullGroup *>(
            RetailZoneLoadSessionAlloc(session, cullGroupCount * sizeof(GfxCullGroup),
                                       alignof(GfxCullGroup)));
        if (!groups && cullGroupCount)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: cull group arena oom\n");
            return false;
        }
        if (!RetailWorldConsume(&context, groups, cullGroupCount * 32u, 4))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: cull groups unreadable\n");
            return false;
        }
        world->dpvs.cullGroups = groups;
        world->cullGroupCount = static_cast<int>(cullGroupCount);
    }
    if (dpvsDrawInstsRef)
    {
        if (dpvsSmodelCount > UINT32_MAX / 76u)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: draw inst count %u overflows\n",
                       dpvsSmodelCount);
            return false;
        }
        // Load_GfxStaticModelDrawInstArray bulk-reads every fixed 76-byte
        // record before resolving any element's XModel pointer, same
        // ordering rule as reflection probes above.
        uint8_t *rawInsts = dpvsSmodelCount ? static_cast<uint8_t *>(RetailZoneLoadSessionAlloc(
                                                  session, dpvsSmodelCount * 76u, 4))
                                            : nullptr;
        if (!rawInsts && dpvsSmodelCount)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: draw inst arena out of memory count=%u\n",
                       dpvsSmodelCount);
            return false;
        }
        if (!RetailWorldConsume(&context, rawInsts, dpvsSmodelCount * 76u, 4))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: draw inst unreadable\n");
            return false;
        }
        // Block-4 offset the bulk span landed at (captured after the
        // stream, which heals any session lag first).
        const uint32_t rawInstsOffset = session->wire.cursor[4] - dpvsSmodelCount * 76u;
        // Widen every instance's real placement/bounds/index data into the
        // native (LP64: 80-byte) array, resolving each model through
        // the same typed nested widener used by the rest of the world.
        static_assert(sizeof(GfxPackedPlacement) == 0x34,
                      "GfxPackedPlacement must match the serialized placement block");
        GfxStaticModelDrawInst *drawInsts = nullptr;
        if (dpvsSmodelCount)
        {
            if (dpvsSmodelCount > UINT32_MAX / sizeof(GfxStaticModelDrawInst))
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: draw inst native array count %u overflows\n",
                           dpvsSmodelCount);
                return false;
            }
            drawInsts = static_cast<GfxStaticModelDrawInst *>(RetailZoneLoadSessionAlloc(
                session, dpvsSmodelCount * sizeof(GfxStaticModelDrawInst),
                alignof(GfxStaticModelDrawInst)));
            if (!drawInsts)
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: draw inst native arena oom count=%u\n",
                           dpvsSmodelCount);
                return false;
            }
        }
        for (uint32_t inst = 0; inst < dpvsSmodelCount; ++inst)
        {
            const uint32_t recordOffset = rawInstsOffset + inst * 76u;
            const uint8_t *record = rawInsts + inst * 76u;
            XModel *model = nullptr;
            // The +56 model slot's own block-4 offset is recorded so later
            // DB_ConvertOffsetToAlias-style model slot indirections resolve.
            if (!RetailWorldWidenXModel(&context, ReadLe32(record + 56), &model,
                                        recordOffset + 56u))
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: draw inst %u XModel resolve failed\n",
                           inst);
                return false;
            }
            WidenGfxStaticModelDrawInst(record, model, &drawInsts[inst]);
        }
        world->dpvs.smodelDrawInsts = drawInsts;
        if (dpvsSmodelCount)
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: smodel draw insts widened count=%u "
                          "model=resolved stale_cache_index=%u\n",
                       dpvsSmodelCount, g_m14StaleCacheIndexCount);
        }
    }
    if (!RetailWorldRuntimeBytes(&context, dpvsSurfMatsRef, 8u * dpvsStaticSurfaceCount, 4) ||
        !RetailWorldRuntimeBytes(&context, dpvsCastsShadowRef, 4u * dpvsSurfaceVisData, 128))
    {
        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: surf materials unexpandable\n");
        return false;
    }
    for (uint32_t list = 0; list < 2; ++list)
    {
        const uint64_t cellBitsBytes = 4u * (uint64_t)cellCount * dpvsWordCount[list];
        if (!RetailWorldRuntimeBytes(&context, dpvsCellBitsRef[list],
                                     cellBitsBytes, 4))
        {
            Com_Printf(0, "RetailWalkLiveLoadGfxWorld: cell bits unexpandable\n");
            return false;
        }
        // DynEntCl_InitFilter memsets dynEntCellBits per frame for
        // cellCount*wordCount u32s; the counts themselves are the
        // engine's dpvsDyn bookkeeping the frontend reads everywhere.
        world->dpvsDyn.dynEntClientWordCount[list] = dpvsWordCount[list];
        world->dpvsDyn.dynEntClientCount[list] = dynEntClientCount[list];
        if (dpvsWordCount[list] && cellCount)
        {
            if (!dpvsCellBitsRef[list] || cellBitsBytes > SIZE_MAX)
                return false;
            uint32_t *cellBits = static_cast<uint32_t *>(RetailZoneLoadSessionAlloc(
                session, static_cast<size_t>(cellBitsBytes), alignof(uint32_t)));
            if (!cellBits)
                return false;
            std::memset(cellBits, 0, static_cast<size_t>(cellBitsBytes));
            world->dpvsDyn.dynEntCellBits[list] = cellBits;
        }
    }
    for (uint32_t pass = 0; pass < 3; ++pass)
    {
        for (uint32_t list = 0; list < 2; ++list)
        {
            const uint64_t visBytes = 32u * dpvsWordCount[list];
            if (!RetailWorldRuntimeBytes(&context, dpvsVisDataRef[list][pass],
                                         visBytes, 16))
            {
                Com_Printf(0, "RetailWalkLiveLoadGfxWorld: vis data unexpandable\n");
                return false;
            }
            // R_ClearDpvsScene memsets dynEntVisData[list][pass] for
            // dynEntClientCount[list] bytes every frame.
            if (dpvsWordCount[list])
            {
                if (!dpvsVisDataRef[list][pass] || visBytes > SIZE_MAX)
                    return false;
                uint8_t *visData = static_cast<uint8_t *>(RetailZoneLoadSessionAlloc(
                    session, static_cast<size_t>(visBytes), 16));
                if (!visData)
                    return false;
                std::memset(visData, 0, static_cast<size_t>(visBytes));
                world->dpvsDyn.dynEntVisData[list][pass] = visData;
            }
        }
    }
    RetailWireBlocksRewind(&session->wire, 0, bodyStart);
    g_lastMaterials = context.widenedMaterialCount;
    g_lastImages = context.widenedImageCount;
    g_lastLightDefs = context.widenedLightDefCount;
    g_lastBlock1Bytes = context.block1Bytes;
    *header = RetailWorldCommitTransaction(session, world);
    if (!header->gfxWorld || header->gfxWorld != &s_world)
    {
        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: commit did not return s_world\n");
        return false;
    }
    // R_InitShadowGeometryArrays runs after R_LoadWorld's own commit, so the
    // callbacks see the same committed world the original path builds them on.
    if (!RetailWalkBuildShadowGeometry(session))
    {
        Com_Printf(0, "RetailWalkLiveLoadGfxWorld: shadow geometry construction failed\n");
        return false;
    }
    return true;
}
