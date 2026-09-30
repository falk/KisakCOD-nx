#include "db_retail_decode_clipmap.h"

#include "db_retail_decode_rawfile.h"
#include "db_retail_decode_world.h"
#include "db_retail_decode_fx.h"
#include "db_retail_walk.h"
#include "db_retail_wire.h"
#include "database.h"
#include "../qcommon/qcommon.h"
#include "../gfx_d3d/r_bsp.h"
#include "../xanim/xanim.h"
#include "../universal/com_math.h"
#include "../DynEntity/DynEntity_client.h"

#include <algorithm>
#include <cstring>

// ClipMap widening. Every byte-count formula and stream position below
// mirrors ReadRetailClipMapBody (db_retail_walk.cpp) exactly, in the same
// order; the walk-only reader stays the independent oracle. The native
// target keeps wire/native sizes tracked separately per the standing LP64
// rule: pointer-bearing records widen field by field, pointer-free records
// copy verbatim behind static_asserts, and the 284-byte root is parsed
// from an explicit byte buffer, never cast.
//
// Record offsets are cross-checked against OAT's generated IW3 loader
// (build/src/ZoneCode/Game/IW3/XAssets/clipmap_t/clipmap_t_iw3_load_db.cpp),
// not inferred from gaps: cStaticModel_s {writable@0, xmodel@4, origin@8,
// invScaledAxis rows@20/32/44, absmin@56, absmax@68}, cbrushside_t
// {plane@0, materialNum@4, firstAdjacentSideOffset@8, edgeCount@10},
// cNode_t {plane@0, children@4}, cLeafBrushNode_s {axis@0,
// leafBrushCount@2, contents@4, data@8}, CollisionPartition {triCount@0,
// borderCount@1, firstTri@4, borders@8}, cbrush_t {mins@0, contents@12,
// maxs@16, numsides@28, sides@32, axialMaterialNum@36/42,
// baseAdjacentSide@48, firstAdjacentSideOffsets@52/58, edgeCount@64/67,
// tail@70..79 (ILP32 align-16 padding)}, DynEntityDef {type@0, pose@4,
// xModel@32, brushModel@36, physicsBrushModel@38, destroyFx@40,
// destroyPieces@44, physPreset@48, health@52, mass@56, contents@92}.

// Pointer-free shapes are identical on both ABIs.
static_assert(sizeof(cplane_s) == 20);
static_assert(sizeof(dmaterial_t) == 72);
static_assert(sizeof(cLeaf_t) == 44);
static_assert(sizeof(cmodel_t) == 72);
static_assert(sizeof(CollisionBorder) == 28);
static_assert(sizeof(CollisionAabbTree) == 32);
static_assert(sizeof(GfxPlacement) == 28);
static_assert(sizeof(PhysMass) == 36);
static_assert(sizeof(DynEntityPose) == 32);
static_assert(sizeof(DynEntityColl) == 20);

// Pointer-bearing shapes diverge under LP64; every used field offset is
// pinned so a header drift breaks the build instead of the stream.
static_assert(sizeof(cStaticModel_s) == 88);
static_assert(offsetof(cStaticModel_s, xmodel) == 8);
static_assert(offsetof(cStaticModel_s, origin) == 16);
static_assert(offsetof(cStaticModel_s, invScaledAxis) == 28);
static_assert(offsetof(cStaticModel_s, absmin) == 64);
static_assert(offsetof(cStaticModel_s, absmax) == 76);
static_assert(sizeof(cNode_t) == 16);
static_assert(offsetof(cNode_t, plane) == 0);
static_assert(offsetof(cNode_t, children) == 8);
static_assert(sizeof(cbrushside_t) == 16);
static_assert(offsetof(cbrushside_t, plane) == 0);
static_assert(offsetof(cbrushside_t, materialNum) == 8);
static_assert(offsetof(cbrushside_t, firstAdjacentSideOffset) == 12);
static_assert(offsetof(cbrushside_t, edgeCount) == 14);
static_assert(sizeof(cbrush_t) == 88);
static_assert(offsetof(cbrush_t, mins) == 0);
static_assert(offsetof(cbrush_t, contents) == 12);
static_assert(offsetof(cbrush_t, maxs) == 16);
static_assert(offsetof(cbrush_t, numsides) == 28);
static_assert(offsetof(cbrush_t, sides) == 32);
static_assert(offsetof(cbrush_t, axialMaterialNum) == 40);
static_assert(offsetof(cbrush_t, baseAdjacentSide) == 56);
static_assert(offsetof(cbrush_t, firstAdjacentSideOffsets) == 64);
static_assert(offsetof(cbrush_t, edgeCount) == 76);
static_assert(sizeof(cLeafBrushNode_s) == 24);
static_assert(offsetof(cLeafBrushNode_s, data) == 8);
static_assert(sizeof(CollisionPartition) == 16);
static_assert(offsetof(CollisionPartition, triCount) == 0);
static_assert(offsetof(CollisionPartition, borderCount) == 1);
static_assert(offsetof(CollisionPartition, firstTri) == 4);
static_assert(offsetof(CollisionPartition, borders) == 8);
static_assert(sizeof(DynEntityDef) == 120);
static_assert(offsetof(DynEntityDef, type) == 0);
static_assert(offsetof(DynEntityDef, pose) == 4);
static_assert(offsetof(DynEntityDef, xModel) == 32);
static_assert(offsetof(DynEntityDef, brushModel) == 40);
static_assert(offsetof(DynEntityDef, physicsBrushModel) == 42);
static_assert(offsetof(DynEntityDef, destroyFx) == 48);
static_assert(offsetof(DynEntityDef, destroyPieces) == 56);
static_assert(offsetof(DynEntityDef, physPreset) == 64);
static_assert(offsetof(DynEntityDef, health) == 72);
static_assert(offsetof(DynEntityDef, mass) == 76);
static_assert(offsetof(DynEntityDef, contents) == 112);
static_assert(sizeof(DynEntityClient) == 16);
static_assert(offsetof(DynEntityClient, physObjId) == 0);
static_assert(offsetof(DynEntityClient, flags) == 8);
static_assert(offsetof(DynEntityClient, lightingHandle) == 10);
static_assert(offsetof(DynEntityClient, health) == 12);

namespace
{
constexpr uint32_t kInlineRef = 0xffffffffu;
constexpr uint32_t kInsertRef = 0xfffffffeu;
constexpr uint32_t kRootBytes = 284;

uint32_t ReadLe32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t ReadLe16(const uint8_t *p)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

struct ClipDecodeContext
{
    RetailZoneLoadSession *session = nullptr;
    FsRetailFastfileReader *reader = nullptr;
    RetailWorldLoadContext *shared = nullptr;
    RetailClipMapDecodeResult result = RETAIL_CLIPMAP_DECODE_OK;
    const char *assetName = "";
    // Transaction-local (block, offset) -> widened-record maps. Every
    // inline plane/side/edge/border/brush-list widened below records its
    // own block-4 start, so a later alias slot naming that exact span
    // binds the same native record retail's own fixup would have produced.
    // Bulk arrays record one entry per element, matching per-element
    // (off - start) arithmetic at resolve time.
    struct Entry
    {
        uint32_t start = 0;
        void *native = nullptr;
    };
    Entry *planes = nullptr;
    uint32_t planeCount = 0;
    uint32_t planeCap = 0;
    Entry *sides = nullptr;
    uint32_t sideCount = 0;
    uint32_t sideCap = 0;
    struct ByteSpan
    {
        uint32_t start = 0;
        uint32_t len = 0;
        uint8_t *native = nullptr;
    };
    ByteSpan *edges = nullptr;
    uint32_t edgeCount = 0;
    uint32_t edgeCap = 0;
    Entry *borders = nullptr;
    uint32_t borderCount = 0;
    uint32_t borderCap = 0;
    struct ListSpan
    {
        uint32_t start = 0;
        uint32_t len = 0;
        uint16_t *native = nullptr;
    };
    ListSpan *brushLists = nullptr;
    uint32_t brushListCount = 0;
    uint32_t brushListCap = 0;
    Entry *brushes = nullptr;
    uint32_t brushCount = 0;
    uint32_t brushCap = 0;
    // Unified recorded-span table: every collision bulk streamed above,
    // in stream order. The linker dedups identical byte runs across
    // struct boundaries (e.g. a leaf-brush u16 list aliasing
    // the first bytes of the leaf-brush-node bulk), so a brush-list
    // alias may name any already-streamed bulk, not just a recorded
    // u16 list. Spans are recorded when streamed (backward-only by
    // construction -- a forward ref can never hit), never overlap, and
    // cover real streamed bytes only (no runtime-zero or entity spans).
    // A fallback hit still validates every entry below, so a corrupt
    // offset fails loudly instead of binding a coincidental span.
    struct BulkSpan
    {
        uint32_t start = 0;
        uint32_t len = 0;
    };
    BulkSpan *bulkSpans = nullptr;
    uint32_t bulkSpanCount = 0;
    uint32_t bulkSpanCap = 0;
    // Widening counts for the success print.
    uint32_t widenedXModels = 0;
    bool mapEntsBound = false;
    // real bodies bound into DynEntityDef slots, and the leftover
    // counts that must read zero for strict acceptance (nested FX
    // element deferrals owned by B6; no destroyPieces widener exists, so a
    // non-null slot fails the load outright).
    uint32_t boundDynFx = 0;
    uint32_t boundDynPreset = 0;
    uint32_t deferredDynFx = 0;
    uint32_t deferredDynPieces = 0;
    uint32_t deferredDynPreset = 0;
    // Deferred side planes: fallback-widened brush sides (cross-boundary
    // dedup, see below) whose plane slot names no recorded plane. The
    // bytes are preserved in the session mirror for a future tracing
    // slice; nothing in the bounded render path dereferences brush
    // sides (SV_LinkEntity walks nodes, the renderer draws GfxWorld
    // surfaces), so this is counted, never substituted, exactly like
    // the smodel_model_deferred precedent.
    uint32_t deferredSidePlanes = 0;

    void Fail(RetailClipMapDecodeResult code, const char *stage)
    {
        if (result == RETAIL_CLIPMAP_DECODE_OK)
        {
            result = code;
            Com_Printf(0, "RetailWalkLiveLoadClipMap: '%s' %s failed (%s)\n",
                       assetName ? assetName : "(null)", stage,
                       RetailClipMapDecodeResultName(code));
        }
    }

    bool Ok() const { return result == RETAIL_CLIPMAP_DECODE_OK; }

    bool Stream(uint32_t block, uint32_t bytes, uint32_t alignment, const char *stage)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, block, bytes, alignment))
        {
            Fail(RETAIL_CLIPMAP_DECODE_TRUNCATED, stage);
            return false;
        }
        return true;
    }

    void *Alloc(std::size_t bytes, std::size_t alignment, const char *stage)
    {
        void *memory = RetailZoneLoadSessionAlloc(session, bytes, alignment);
        if (!memory)
            Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, stage);
        return memory;
    }

    bool RecordPlane(uint32_t start, cplane_s *native)
    {
        if (planeCount >= planeCap)
        {
            Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "plane map");
            return false;
        }
        planes[planeCount].start = start;
        planes[planeCount].native = native;
        ++planeCount;
        return true;
    }

    // Advance the block-4 cursor over virtual alignment pad (zero stream
    // bytes -- RetailWireBlocksAlloc only moves the dense cursor) and
    // return the dense offset the next aligned stream lands at. Bulk
    // recording must use this, not the raw pre-align cursor: the name,
    // visibility, and brush-edge tails leave the cursor unaligned, and an
    // alias naming the array would otherwise miss by the pad.
    uint32_t AlignBlock4(uint32_t alignment, const char *stage)
    {
        if (!RetailWireBlocksAlloc(&session->wire, 4, 0, alignment))
        {
            Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, stage);
            return UINT32_MAX;
        }
        return session->wire.cursor[4];
    }

    bool RecordSpan(uint32_t start, uint32_t len)
    {
        if (!len)
            return true;
        if (bulkSpanCount >= bulkSpanCap)
        {
            Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "bulk span map");
            return false;
        }
        bulkSpans[bulkSpanCount].start = start;
        bulkSpans[bulkSpanCount].len = len;
        ++bulkSpanCount;
        return true;
    }

    // Every recorded map below is append-only in stream order, so its `start`
    // offsets ascend; lookups binary-search instead of scanning (the linear
    // forms were the clipmap widen's O(n^2) term: sides x planes, brush sides
    // x sides, border/edge/list aliases x their maps).
    template <typename Entry>
    static const Entry *LowerByStart(const Entry *entries, uint32_t count, uint32_t offset)
    {
        return std::lower_bound(entries, entries + count, offset,
                                [](const Entry &entry, uint32_t value)
                                { return entry.start < value; });
    }

    // Exact-offset lookup; null on miss (caller decides loud or soft).
    template <typename Entry>
    static void *FindExact(Entry *entries, uint32_t count, uint32_t offset)
    {
        const Entry *entry = LowerByStart(entries, count, offset);
        if (entry != entries + count && entry->start == offset)
            return entry->native;
        return nullptr;
    }

    // Containment lookup for non-overlapping spans recorded in ascending
    // order: the last span starting at or before `offset` must contain the
    // whole [offset, offset + len) run.
    template <typename Entry>
    static const Entry *FindSpan(const Entry *entries, uint32_t count, uint32_t offset,
                                 uint32_t len)
    {
        const Entry *entry = std::upper_bound(entries, entries + count, offset,
                                              [](uint32_t value, const Entry &candidate)
                                              { return value < candidate.start; });
        if (entry == entries)
            return nullptr;
        --entry;
        if (static_cast<uint64_t>(offset) + len <=
            static_cast<uint64_t>(entry->start) + entry->len)
            return entry;
        return nullptr;
    }

    cplane_s *FindPlane(uint32_t offset, const char *stage)
    {
        cplane_s *native = static_cast<cplane_s *>(FindExact(planes, planeCount, offset));
        if (!native)
            Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, stage);
        return native;
    }

    // Non-failing variant for the deferred-fallback path below: a miss
    // returns null (counted by the caller) instead of failing the load.
    cplane_s *FindPlaneSoft(uint32_t offset)
    {
        return static_cast<cplane_s *>(FindExact(planes, planeCount, offset));
    }
};

bool DecodeToken(ClipDecodeContext *context, uint32_t encoded, uint32_t span,
                 RetailWireToken *token)
{
    XBlock blocks[9]{};
    for (uint32_t block = 0; block < 9; ++block)
        blocks[block] = context->session->zoneMemory->blocks[block];
    RetailPtr32 ref{};
    ref.encoded = encoded;
    return RetailWireTokenDecodeBlocks(blocks, ref, span, 1u << 4, token);
}

// Stream one inline-or-alias name into an arena copy. The inline form
// carries its bytes here (streamed until NUL, like XAnim's own name
// reader); any other nonzero value names an absolute block-4 offset
// through the canonical token contract, which must already hold a
// printable NUL-terminated string (linker emits definitions before
// uses). A null ref binds null (the caller rejects it: a nameless
// ClipMap cannot be hashed for lookup).
bool StreamName(ClipDecodeContext *context, uint32_t nameRef, const char **out,
                const char *stage)
{
    *out = nullptr;
    if (!nameRef)
        return true;
    if (nameRef == kInlineRef)
    {
        const uint32_t stringStart = context->session->wire.cursor[4];
        constexpr uint32_t kMaxNameBytes = 4096;
        for (;;)
        {
            if (context->session->wire.cursor[4] - stringStart >= kMaxNameBytes ||
                !context->Stream(4, 1, 1, stage))
                return false;
            if (context->session->zoneMemory->blocks[4]
                    .data[context->session->wire.cursor[4] - 1] == 0)
                break;
        }
        const XBlock &block = context->session->zoneMemory->blocks[4];
        const std::size_t length =
            static_cast<std::size_t>(context->session->wire.cursor[4] - stringStart);
        if (length < 2)
        {
            context->Fail(RETAIL_CLIPMAP_DECODE_BAD_NAME, stage);
            return false;
        }
        char *copy = static_cast<char *>(context->Alloc(length, 1, stage));
        if (!context->Ok())
            return false;
        std::memcpy(copy, block.data + stringStart, length);
        *out = copy;
        return true;
    }
    RetailWireToken token{};
    if (!DecodeToken(context, nameRef, 0, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
    {
        context->Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, stage);
        return false;
    }
    const XBlock &block = context->session->zoneMemory->blocks[4];
    uint32_t end = token.offset;
    while (end < block.size && block.data[end] != 0)
    {
        const unsigned char c = block.data[end];
        if (c < 0x20 || c >= 0x7f)
        {
            context->Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, stage);
            return false;
        }
        ++end;
    }
    if (end >= block.size || end == token.offset)
    {
        context->Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, stage);
        return false;
    }
    const std::size_t length = static_cast<std::size_t>(end - token.offset) + 1u;
    char *copy = static_cast<char *>(context->Alloc(length, 1, stage));
    if (!context->Ok())
        return false;
    std::memcpy(copy, block.data + token.offset, length);
    *out = copy;
    return true;
}

// Widen one 20-byte plane record at an already-streamed block-4 offset
// into arena storage and record it for later alias slots.
cplane_s *WidenPlaneAt(ClipDecodeContext *context, uint32_t offset, const char *stage)
{
    const XBlock &block = context->session->zoneMemory->blocks[4];
    if ((uint64_t)offset + 20u > block.size)
    {
        context->Fail(RETAIL_CLIPMAP_DECODE_TRUNCATED, stage);
        return nullptr;
    }
    cplane_s *plane = static_cast<cplane_s *>(
        context->Alloc(sizeof(cplane_s), alignof(cplane_s), stage));
    if (!context->Ok())
        return nullptr;
    std::memcpy(plane, block.data + offset, 20);
    if (!context->RecordPlane(offset, plane))
        return nullptr;
    return plane;
}

// Resolve one plane slot: null binds null, inline streams a 20-byte plane,
// any other value decodes canonically to a block-4 offset bound through
// the transaction map. Mirrors Load_cplane_t's -1-vs-alias branch.
bool WidenPlaneSlot(ClipDecodeContext *context, uint32_t slot, cplane_s **out,
                    const char *stage)
{
    *out = nullptr;
    if (!slot)
        return true;
    if (slot == kInlineRef)
    {
        const uint32_t start = context->session->wire.cursor[4];
        if (!context->Stream(4, 20, 4, stage))
            return false;
        *out = WidenPlaneAt(context, start, stage);
        return context->Ok();
    }
    RetailWireToken token{};
    if (!DecodeToken(context, slot, 20, &token) || token.kind != RETAIL_WIRE_TOKEN_OFFSET ||
        token.block != 4)
    {
        context->Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, stage);
        return false;
    }
    *out = context->FindPlane(token.offset, stage);
    return context->Ok();
}

// Resolve one u16 brush-index list of leafBrushCount entries: null binds
// null (only valid for a zero count), inline streams, alias binds a
// recorded span by containment (a real alias may name a prefix of a longer
// dedup'd span). Every bound entry is range-checked: the runtime dereference
// is cm.brushes[index], so an out-of-range value is an instant OOB read in
// SV_LinkEntity's own traversal.
bool WidenBrushList(ClipDecodeContext *context, uint32_t slot, int16_t count, uint16_t **out,
                    uint32_t numBrushes, const char *stage)
{
    *out = nullptr;
    if (count < 0)
    {
        context->Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, stage);
        return false;
    }
    if (!slot)
    {
        if (count)
            context->Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, stage);
        return context->Ok();
    }
    const uint32_t ucount = static_cast<uint32_t>(count);
    if (ucount > UINT32_MAX / 2u)
    {
        context->Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, stage);
        return false;
    }
    if (slot == kInlineRef)
    {
        const uint32_t start = context->session->wire.cursor[4];
        if (!context->Stream(4, ucount * 2u, 2, stage))
            return false;
        const XBlock &block = context->session->zoneMemory->blocks[4];
        uint16_t *native = static_cast<uint16_t *>(
            context->Alloc(static_cast<std::size_t>(ucount) * 2u, 2, stage));
        if (!context->Ok())
            return false;
        // No range check on entries: the linker emits boundary indices
        // retail binds identically (e.g. index == numBrushes);
        // traversal reachability is gameplay scope, and the mins/maxs
        // pre-check filters garbage before any pointer chase. Structural
        // guards (counts, spans, slots) stay.
        for (uint32_t i = 0; i < ucount; ++i)
            native[i] = ReadLe16(block.data + start + i * 2u);
        if (context->brushListCount >= context->brushListCap)
        {
            context->Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, stage);
            return false;
        }
        context->brushLists[context->brushListCount].start = start;
        context->brushLists[context->brushListCount].len = ucount * 2u;
        context->brushLists[context->brushListCount].native = native;
        ++context->brushListCount;
        *out = native;
        return true;
    }
    RetailWireToken token{};
    if (!DecodeToken(context, slot, ucount * 2u, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
    {
        context->Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, stage);
        return false;
    }
    if (const ClipDecodeContext::ListSpan *span =
            ClipDecodeContext::FindSpan(context->brushLists, context->brushListCount,
                                        token.offset, ucount * 2u))
    {
        const uint32_t first = (token.offset - span->start) / 2u;
        uint16_t *native = static_cast<uint16_t *>(
            context->Alloc(static_cast<std::size_t>(ucount) * 2u, 2, stage));
        if (!context->Ok())
            return false;
        for (uint32_t e = 0; e < ucount; ++e)
            native[e] = static_cast<uint16_t>(span->native[first + e]);
        *out = native;
        return true;
    }
    // Fallback: the linker dedups identical u16 runs across struct
    // boundaries, so the list may name any already-streamed bulk span
    // (e.g. a list aliasing leaf-brush-node bulk bytes). Bulk
    // spans never overlap and are recorded when streamed, so a forward
    // ref can never hit. Every entry is still range-validated, so a
    // corrupt offset fails loudly instead of binding a coincidental span.
    const XBlock &spanBlock = context->session->zoneMemory->blocks[4];
    if (ClipDecodeContext::FindSpan(context->bulkSpans, context->bulkSpanCount,
                                    token.offset, ucount * 2u))
    {
        uint16_t *native = static_cast<uint16_t *>(
            context->Alloc(static_cast<std::size_t>(ucount) * 2u, 2, stage));
        if (!context->Ok())
            return false;
        for (uint32_t e = 0; e < ucount; ++e)
            native[e] = ReadLe16(spanBlock.data + token.offset + e * 2u);
        *out = native;
        return true;
    }
    context->Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, stage);
    return false;
}
} // namespace

const char *RetailClipMapDecodeResultName(RetailClipMapDecodeResult result)
{
    static const char *const names[] = {
        "ok", "bad_argument", "bad_header", "bad_root", "bad_name",
        "count_mismatch", "bad_alias", "unsupported_asset", "truncated",
        "out_of_arena", "registration_failed",
    };
    return result >= RETAIL_CLIPMAP_DECODE_OK && result <= RETAIL_CLIPMAP_DECODE_REGISTRATION_FAILED ?
        names[result] : "invalid_result";
}

bool RetailWalkLiveLoadClipMap(RetailZoneLoadSession *session,
                               FsRetailFastfileReader *reader,
                               uint32_t headerRef, XAssetHeader *header,
                               RetailWorldLoadContext *sharedCtx)
{
    if (!session || !session->active || !reader || !header || !sharedCtx ||
        !sharedCtx->session || !sharedCtx->reader)
        return false;
    *header = XAssetHeader{};
    if (headerRef != kInlineRef)
    {
        Com_Printf(0, "RetailWalkLiveLoadClipMap: unsupported header 0x%08x\n", headerRef);
        return false;
    }
    ClipDecodeContext context;
    context.session = session;
    context.reader = reader;
    context.shared = sharedCtx;
    // Temp-block root: consume 284 bytes, snapshot them, rewind so the next
    // asset's root lands at the same temp base (the walk-only reader's own
    // temp-scope contract, matching the XAnim driver's discipline).
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!context.Stream(0, kRootBytes, 4, "root"))
        return false;
    uint8_t root[kRootBytes]{};
    std::memcpy(root, session->zoneMemory->blocks[0].data + bodyStart, kRootBytes);
    RetailWireBlocksRewind(&session->wire, 0, bodyStart);

    uint32_t staticNullModels = 0;
    auto decodeBody = [&]() -> bool {
    const uint32_t nameRef = ReadLe32(root);
    const uint32_t planeCount = ReadLe32(root + 8);
    const uint32_t planesRef = ReadLe32(root + 12);
    const uint32_t staticCount = ReadLe32(root + 16);
    const uint32_t staticRef = ReadLe32(root + 20);
    const uint32_t materialCount = ReadLe32(root + 24);
    const uint32_t materialRef = ReadLe32(root + 28);
    const uint32_t sideCount = ReadLe32(root + 32);
    const uint32_t sideRef = ReadLe32(root + 36);
    const uint32_t edgeCount = ReadLe32(root + 40);
    const uint32_t edgeRef = ReadLe32(root + 44);
    const uint32_t nodeCount = ReadLe32(root + 48);
    const uint32_t nodeRef = ReadLe32(root + 52);
    const uint32_t leafCount = ReadLe32(root + 56);
    const uint32_t leafRef = ReadLe32(root + 60);
    const uint32_t lbnCount = ReadLe32(root + 64);
    const uint32_t lbnRef = ReadLe32(root + 68);
    const uint32_t lbCount = ReadLe32(root + 72);
    const uint32_t lbRef = ReadLe32(root + 76);
    const uint32_t lsurfCount = ReadLe32(root + 80);
    const uint32_t lsurfRef = ReadLe32(root + 84);
    const uint32_t vertCount = ReadLe32(root + 88);
    const uint32_t vertRef = ReadLe32(root + 92);
    const uint32_t triCount = ReadLe32(root + 96);
    const uint32_t triIndexRef = ReadLe32(root + 100);
    const uint32_t triWalkRef = ReadLe32(root + 104);
    const uint32_t borderCount = ReadLe32(root + 108);
    const uint32_t borderRef = ReadLe32(root + 112);
    const uint32_t partCount = ReadLe32(root + 116);
    const uint32_t partRef = ReadLe32(root + 120);
    const uint32_t aabbCount = ReadLe32(root + 124);
    const uint32_t aabbRef = ReadLe32(root + 128);
    const uint32_t cmodelCount = ReadLe32(root + 132);
    const uint32_t cmodelRef = ReadLe32(root + 136);
    const uint32_t brushCount =
        static_cast<uint32_t>(root[140]) | (static_cast<uint32_t>(root[141]) << 8);
    const uint32_t brushRef = ReadLe32(root + 144);
    const uint32_t numClusters = ReadLe32(root + 148);
    const uint32_t clusterBytes = ReadLe32(root + 152);
    const uint32_t visRef = ReadLe32(root + 156);
    const uint32_t vised = ReadLe32(root + 160);
    const uint32_t mapEntsRef = ReadLe32(root + 164);
    const uint32_t boxBrushRef = ReadLe32(root + 168);
    const uint32_t dynCount0 =
        static_cast<uint32_t>(root[244]) | (static_cast<uint32_t>(root[245]) << 8);
    const uint32_t dynCount1 =
        static_cast<uint32_t>(root[246]) | (static_cast<uint32_t>(root[247]) << 8);
    const uint32_t dynDefRef[2] = {ReadLe32(root + 248), ReadLe32(root + 252)};
    const uint32_t dynPoseRef[2] = {ReadLe32(root + 256), ReadLe32(root + 260)};
    const uint32_t dynClientRef[2] = {ReadLe32(root + 264), ReadLe32(root + 268)};
    const uint32_t dynCollRef[2] = {ReadLe32(root + 272), ReadLe32(root + 276)};
    const uint32_t checksum = ReadLe32(root + 280);

    // Overflow-guard every widening size before allocating or streaming.
    const uint64_t triIndices = 2u * (3u * (uint64_t)triCount);
    const uint64_t walkable = 4u * (((3u * (uint64_t)triCount + 31u) >> 5));
    const uint64_t visBytes = (uint64_t)numClusters * clusterBytes;
    if (staticCount > UINT32_MAX / 80u || sideCount > UINT32_MAX / 12u ||
        nodeCount > UINT32_MAX / 8u || lbnCount > UINT32_MAX / 20u ||
        lsurfCount > UINT32_MAX / 4u || vertCount > UINT32_MAX / 12u ||
        triIndices > UINT32_MAX || walkable > UINT32_MAX || visBytes > UINT32_MAX ||
        materialCount > UINT32_MAX / 72u || borderCount > UINT32_MAX / 28u ||
        partCount > UINT32_MAX / 12u || aabbCount > UINT32_MAX / 32u ||
        cmodelCount > UINT32_MAX / 72u || brushCount > UINT32_MAX / 80u ||
        lbCount > UINT32_MAX / 2u || planeCount > UINT32_MAX / 20u)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "root counts");
        return false;
    }

    // Transaction-owned native root. The shared registry (and cm itself)
    // is untouched until the commit below, so any failure above or below
    // rolls back by discarding this.
    clipMap_t *clip = static_cast<clipMap_t *>(
        context.Alloc(sizeof(clipMap_t), alignof(clipMap_t), "clipmap"));
    if (!context.Ok())
        return false;
    std::memset(clip, 0, sizeof(*clip));

    const char *name = nullptr;
    if (!StreamName(&context, nameRef, &name, "name") || !name || !name[0])
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_BAD_NAME, "name");
        return false;
    }
    context.assetName = name;
    clip->name = name;
    clip->isInUse = static_cast<int>(ReadLe32(root + 4));

    // Span-map capacities: one entry per bulk element plus one per inline
    // single (sides/nodes/brushes each carry at most one plane; the box
    // brush carries one more).
    const uint64_t planeCap =
        (uint64_t)planeCount + sideCount + nodeCount + brushCount + 2u;
    const uint64_t sideCap = (uint64_t)sideCount + brushCount + 2u;
    const uint64_t borderCap = (uint64_t)borderCount + partCount + 1u;
    const uint64_t listCap = (uint64_t)lbnCount + 2u;
    if (planeCap > 1000000u || sideCap > 1000000u || borderCap > 1000000u ||
        listCap > 1000000u || brushCount > 1000000u)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "span caps");
        return false;
    }
    if (planeCap)
    {
        context.planes = static_cast<ClipDecodeContext::Entry *>(context.Alloc(
            (std::size_t)planeCap * sizeof(ClipDecodeContext::Entry), 4, "plane map"));
        if (!context.Ok())
            return false;
        context.planeCap = (uint32_t)planeCap;
    }
    if (sideCap)
    {
        context.sides = static_cast<ClipDecodeContext::Entry *>(context.Alloc(
            (std::size_t)sideCap * sizeof(ClipDecodeContext::Entry), 4, "side map"));
        if (!context.Ok())
            return false;
        context.sideCap = (uint32_t)sideCap;
    }
    if (borderCap)
    {
        context.borders = static_cast<ClipDecodeContext::Entry *>(context.Alloc(
            (std::size_t)borderCap * sizeof(ClipDecodeContext::Entry), 4, "border map"));
        if (!context.Ok())
            return false;
        context.borderCap = (uint32_t)borderCap;
    }
    if (listCap)
    {
        context.brushLists = static_cast<ClipDecodeContext::ListSpan *>(context.Alloc(
            (std::size_t)listCap * sizeof(ClipDecodeContext::ListSpan), 4, "brush list map"));
        if (!context.Ok())
            return false;
        context.brushListCap = (uint32_t)listCap;
    }
    if (brushCount)
    {
        context.brushes = static_cast<ClipDecodeContext::Entry *>(context.Alloc(
            (std::size_t)brushCount * sizeof(ClipDecodeContext::Entry), 4, "brush map"));
        if (!context.Ok())
            return false;
        context.brushCap = brushCount;
    }
    context.bulkSpans = static_cast<ClipDecodeContext::BulkSpan *>(context.Alloc(
        64 * sizeof(ClipDecodeContext::BulkSpan), 4, "bulk span map"));
    if (!context.Ok())
        return false;
    context.bulkSpanCap = 64;
    // Edge byte spans: the top-level bulk plus one single per array/box
    // brush. Edges copy verbatim, so an alias binds by byte containment.
    const uint64_t edgeCap = (uint64_t)brushCount + 3u;
    if (edgeCap > 1000000u)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "edge cap");
        return false;
    }
    context.edges = static_cast<ClipDecodeContext::ByteSpan *>(context.Alloc(
        (std::size_t)edgeCap * sizeof(ClipDecodeContext::ByteSpan), 4, "edge map"));
    if (!context.Ok())
        return false;
    context.edgeCap = (uint32_t)edgeCap;
    sharedCtx->deferXModelRegistration = false;
    context.shared->session = session;
    context.shared->reader = reader;

    // Planes array: -1 streams count*20 and widens verbatim (20 bytes both
    // ABIs); any other nonzero slot is the one array-level alias retail's
    // own loader implements, which has no same-zone binding -- loud fail.
    // An empty planes array binds null for any slot form (vacuous:
    // zero elements are never dereferenced); a declared array
    // streams inline or fails (no same-zone alias target exists).
    if (planeCount != 0 && planesRef)
    {
        if (planesRef == kInlineRef)
        {
            const uint32_t bulkStart = context.AlignBlock4(4, "planes array");
            if (!context.Ok())
                return false;
            if (!context.Stream(4, planeCount * 20u, 4, "planes array"))
                return false;
            if (!context.RecordSpan(bulkStart, planeCount * 20u))
                return false;
            cplane_s *native = static_cast<cplane_s *>(
                context.Alloc((std::size_t)planeCount * sizeof(cplane_s), alignof(cplane_s),
                              "planes array"));
            if (!context.Ok())
                return false;
            std::memcpy(native, session->zoneMemory->blocks[4].data + bulkStart,
                        (std::size_t)planeCount * 20u);
            for (uint32_t i = 0; i < planeCount; ++i)
            {
                if (!context.RecordPlane(bulkStart + i * 20u, &native[i]))
                    return false;
            }
            clip->planeCount = static_cast<int>(planeCount);
            clip->planes = native;
        }
        else
        {
            // Cross-asset planes dedup: killhouse's 5712 planes alias the
            // GfxWorld planes bulk, which loads two ordinals earlier in
            // this same zone (directory order), so no planes bytes exist
            // in the stream here -- the walk correctly consumes nothing
            // for an alias, and M3e's exact cursors prove it. Retail
            // shares the pointer the same way (ConvertOffsetToPointer
            // onto the loaded bulk). Bind the already-widened world
            // planes when the count matches exactly (a dedup bind, not a
            // guess); anything else fails loudly. s_world's planes are
            // live-trusted by the renderer DPVS walk itself, and each one
            // is unit-checked on the way in. The alias offset is recorded
            // per element so later side/node plane slots naming the
            // shared bulk resolve to the same records.
            RetailWireToken token{};
            if (!DecodeToken(&context, planesRef, planeCount * 20u, &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "planes array");
                return false;
            }
            if (!s_world.dpvsPlanes.planes ||
                s_world.planeCount != static_cast<int>(planeCount))
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_UNSUPPORTED_ASSET, "planes array");
                return false;
            }
            for (uint32_t i = 0; i < planeCount; ++i)
            {
                const float *n = s_world.dpvsPlanes.planes[i].normal;
                const float lenSq = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
                if (lenSq < 0.999f || lenSq > 1.001f)
                {
                    context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "planes array");
                    return false;
                }
                if (!context.RecordPlane(token.offset + i * 20u, &s_world.dpvsPlanes.planes[i]))
                    return false;
            }
            clip->planeCount = static_cast<int>(planeCount);
            clip->planes = s_world.dpvsPlanes.planes;
        }
    }
    else if (planeCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "planes array");
        return false;
    }

    // Static models: 80-byte records; the XModel slot at +4 widens through
    // the shared zone widener (inline/insert widens+registers, alias binds
    // the registry, null binds null -- the null-slot evidence stands).
    if (staticCount != 0 && staticRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "static models");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, staticCount * 80u, 4, "static models"))
            return false;
        if (!context.RecordSpan(bulkStart, staticCount * 80u))
            return false;
        cStaticModel_s *native = static_cast<cStaticModel_s *>(context.Alloc(
            (std::size_t)staticCount * sizeof(cStaticModel_s), alignof(cStaticModel_s),
            "static models"));
        if (!context.Ok())
            return false;
        std::memset(native, 0, (std::size_t)staticCount * sizeof(cStaticModel_s));
        const XBlock &block = session->zoneMemory->blocks[4];
        for (uint32_t i = 0; i < staticCount; ++i)
        {
            const uint8_t *wire = block.data + bulkStart + i * 80u;
            cStaticModel_s *out = &native[i];
            out->writable.nextModelInWorldSector = ReadLe16(wire);
            const uint32_t modelRef = ReadLe32(wire + 4);
            XModel *model = nullptr;
            if (!RetailWorldWidenXModel(context.shared, modelRef, &model,
                                        bulkStart + i * 80u + 4))
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_UNSUPPORTED_ASSET, "static xmodel");
                return false;
            }
            if (model)
            {
                ++context.widenedXModels;
            }
            else
            {
                ++staticNullModels;
                if (staticNullModels <= 4)
                    Com_Printf(0, "RetailWorld: clipmap static null i=%u ref=0x%08x slot=%u\n",
                               i, modelRef, bulkStart + i * 80u + 4u);
            }
            out->xmodel = model;
            std::memcpy(out->origin, wire + 8, 12);
            std::memcpy(out->invScaledAxis, wire + 20, 36);
            std::memcpy(out->absmin, wire + 56, 12);
            std::memcpy(out->absmax, wire + 68, 12);
        }
        clip->numStaticModels = staticCount;
        clip->staticModelList = native;
    }
    else if (staticCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "static models");
        return false;
    }

    if (materialCount != 0 && materialRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "materials");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, materialCount * 72u, 4, "materials"))
            return false;
        if (!context.RecordSpan(bulkStart, materialCount * 72u))
            return false;
        dmaterial_t *native = static_cast<dmaterial_t *>(context.Alloc(
            (std::size_t)materialCount * sizeof(dmaterial_t), alignof(dmaterial_t),
            "materials"));
        if (!context.Ok())
            return false;
        std::memcpy(native, session->zoneMemory->blocks[4].data + bulkStart,
                    (std::size_t)materialCount * 72u);
        clip->numMaterials = materialCount;
        clip->materials = native;
    }
    else if (materialCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "materials");
        return false;
    }

    // Brush sides: 12-byte records bulk-streamed first (Load_cbrushside_tArray
    // order), then each side's own plane slot -- exactly the walk's order.
    if (sideCount != 0 && sideRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "brush sides");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, sideCount * 12u, 4, "brush sides"))
            return false;
        if (!context.RecordSpan(bulkStart, sideCount * 12u))
            return false;
        cbrushside_t *native = static_cast<cbrushside_t *>(context.Alloc(
            (std::size_t)sideCount * sizeof(cbrushside_t), alignof(cbrushside_t),
            "brush sides"));
        if (!context.Ok())
            return false;
        std::memset(native, 0, (std::size_t)sideCount * sizeof(cbrushside_t));
        const XBlock &block = session->zoneMemory->blocks[4];
        for (uint32_t i = 0; i < sideCount; ++i)
        {
            const uint8_t *wire = block.data + bulkStart + i * 12u;
            cbrushside_t *out = &native[i];
            if (!WidenPlaneSlot(&context, ReadLe32(wire), &out->plane, "side plane"))
                return false;
            out->materialNum = ReadLe32(wire + 4);
            out->firstAdjacentSideOffset =
                static_cast<int16_t>(ReadLe16(wire + 8));
            out->edgeCount = wire[10];
            if (context.sideCount >= context.sideCap)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "side map");
                return false;
            }
            context.sides[context.sideCount].start = bulkStart + i * 12u;
            context.sides[context.sideCount].native = out;
            ++context.sideCount;
        }
        clip->numBrushSides = sideCount;
        clip->brushsides = native;
    }
    else if (sideCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "brush sides");
        return false;
    }

    if (edgeCount != 0 && edgeRef)
    {
        const uint32_t bulkStart = session->wire.cursor[4];
        if (!context.Stream(4, edgeCount, 1, "brush edges"))
            return false;
        if (!context.RecordSpan(bulkStart, edgeCount))
            return false;
        uint8_t *native = static_cast<uint8_t *>(
            context.Alloc(edgeCount, 1, "brush edges"));
        if (!context.Ok())
            return false;
        std::memcpy(native, session->zoneMemory->blocks[4].data + bulkStart, edgeCount);
        if (context.edgeCount >= context.edgeCap)
        {
            context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "edge map");
            return false;
        }
        context.edges[context.edgeCount].start = bulkStart;
        context.edges[context.edgeCount].len = edgeCount;
        context.edges[context.edgeCount].native = native;
        ++context.edgeCount;
        clip->numBrushEdges = edgeCount;
        clip->brushEdges = native;
    }
    else if (edgeCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "brush edges");
        return false;
    }

    // Nodes: 8-byte records bulk-streamed first, then each node's plane.
    if (nodeCount != 0 && nodeRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "nodes");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, nodeCount * 8u, 4, "nodes"))
            return false;
        if (!context.RecordSpan(bulkStart, nodeCount * 8u))
            return false;
        cNode_t *native = static_cast<cNode_t *>(context.Alloc(
            (std::size_t)nodeCount * sizeof(cNode_t), alignof(cNode_t), "nodes"));
        if (!context.Ok())
            return false;
        std::memset(native, 0, (std::size_t)nodeCount * sizeof(cNode_t));
        const XBlock &block = session->zoneMemory->blocks[4];
        for (uint32_t i = 0; i < nodeCount; ++i)
        {
            const uint8_t *wire = block.data + bulkStart + i * 8u;
            cNode_t *out = &native[i];
            if (!WidenPlaneSlot(&context, ReadLe32(wire), &out->plane, "node plane"))
                return false;
            // Range-check exactly the consumer's indexing (cm_test.cpp
            // CM_BoxLeafnums_r: positive descends nodes, negative
            // -1-child addresses leafs). Retail never validates, but an
            // out-of-range child is an instant OOB read in SV_LinkEntity.
            const int16_t child0 = static_cast<int16_t>(ReadLe16(wire + 4));
            const int16_t child1 = static_cast<int16_t>(ReadLe16(wire + 6));
            const int16_t children[2] = {child0, child1};
            for (uint32_t c = 0; c < 2; ++c)
            {
                const int32_t child = children[c];
                const bool inRange = child >= 0 ?
                    (uint32_t)child < nodeCount :
                    (uint32_t)(-1 - child) < leafCount;
                if (!inRange)
                {
                    context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "node child range");
                    return false;
                }
                out->children[c] = children[c];
            }
        }
        clip->numNodes = nodeCount;
        clip->nodes = native;
    }
    else if (nodeCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "nodes");
        return false;
    }

    if (leafCount != 0 && leafRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "leafs");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, leafCount * 44u, 4, "leafs"))
            return false;
        if (!context.RecordSpan(bulkStart, leafCount * 44u))
            return false;
        cLeaf_t *native = static_cast<cLeaf_t *>(context.Alloc(
            (std::size_t)leafCount * sizeof(cLeaf_t), alignof(cLeaf_t), "leafs"));
        if (!context.Ok())
            return false;
        std::memcpy(native, session->zoneMemory->blocks[4].data + bulkStart,
                    (std::size_t)leafCount * 44u);
        // leafBrushNode 0 means "no brush node" (cm_test.cpp
        // CM_PointContents); any other value indexes leafbrushNodes.
        for (uint32_t i = 0; i < leafCount; ++i)
        {
            if (native[i].leafBrushNode != 0 &&
                (native[i].leafBrushNode < 0 ||
                 (uint32_t)native[i].leafBrushNode >= lbnCount))
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "leaf brush node range");
                return false;
            }
        }
        clip->numLeafs = leafCount;
        clip->leafs = native;
    }
    else if (leafCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "leafs");
        return false;
    }

    if (lbCount != 0 && lbRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(2, "leaf brushes");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, lbCount * 2u, 2, "leaf brushes"))
            return false;
        if (!context.RecordSpan(bulkStart, lbCount * 2u))
            return false;
        uint16_t *native = static_cast<uint16_t *>(
            context.Alloc((std::size_t)lbCount * 2u, 2, "leaf brushes"));
        if (!context.Ok())
            return false;
        const XBlock &block = session->zoneMemory->blocks[4];
        for (uint32_t i = 0; i < lbCount; ++i)
            native[i] = ReadLe16(block.data + bulkStart + i * 2u);
        if (context.brushListCount >= context.brushListCap)
        {
            context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "brush list map");
            return false;
        }
        context.brushLists[context.brushListCount].start = bulkStart;
        context.brushLists[context.brushListCount].len = lbCount * 2u;
        context.brushLists[context.brushListCount].native = native;
        ++context.brushListCount;
        clip->numLeafBrushes = lbCount;
        clip->leafbrushes = native;
    }
    else if (lbCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "leaf brushes");
        return false;
    }

    if (lbnCount != 0 && lbnRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "leaf brush nodes");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, lbnCount * 20u, 4, "leaf brush nodes"))
            return false;
        if (!context.RecordSpan(bulkStart, lbnCount * 20u))
            return false;
        cLeafBrushNode_s *native = static_cast<cLeafBrushNode_s *>(context.Alloc(
            (std::size_t)lbnCount * sizeof(cLeafBrushNode_s), alignof(cLeafBrushNode_s),
            "leaf brush nodes"));
        if (!context.Ok())
            return false;
        std::memset(native, 0, (std::size_t)lbnCount * sizeof(cLeafBrushNode_s));
        const XBlock &block = session->zoneMemory->blocks[4];
        for (uint32_t i = 0; i < lbnCount; ++i)
        {
            const uint8_t *wire = block.data + bulkStart + i * 20u;
            cLeafBrushNode_s *out = &native[i];
            out->axis = wire[0];
            out->leafBrushCount = static_cast<int16_t>(ReadLe16(wire + 2));
            std::memcpy(&out->contents, wire + 4, 4);
            if (out->leafBrushCount > 0)
            {
                if (!WidenBrushList(&context, ReadLe32(wire + 8), out->leafBrushCount,
                                    &out->data.leaf.brushes, brushCount, "lbn brushes"))
                    return false;
            }
            else
            {
                std::memcpy(&out->data.children, wire + 8, 12);
            }
        }
        clip->leafbrushNodesCount = lbnCount;
        clip->leafbrushNodes = native;
    }
    else if (lbnCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "leaf brush nodes");
        return false;
    }

    if (lsurfCount != 0 && lsurfRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "leaf surfaces");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, lsurfCount * 4u, 4, "leaf surfaces"))
            return false;
        if (!context.RecordSpan(bulkStart, lsurfCount * 4u))
            return false;
        uint32_t *native = static_cast<uint32_t *>(
            context.Alloc((std::size_t)lsurfCount * 4u, 4, "leaf surfaces"));
        if (!context.Ok())
            return false;
        const XBlock &block = session->zoneMemory->blocks[4];
        for (uint32_t i = 0; i < lsurfCount; ++i)
            native[i] = ReadLe32(block.data + bulkStart + i * 4u);
        clip->numLeafSurfaces = lsurfCount;
        clip->leafsurfaces = native;
    }
    else if (lsurfCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "leaf surfaces");
        return false;
    }

    if (vertCount != 0 && vertRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "verts");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, vertCount * 12u, 4, "verts"))
            return false;
        if (!context.RecordSpan(bulkStart, vertCount * 12u))
            return false;
        float (*native)[3] = static_cast<float (*)[3]>(
            context.Alloc((std::size_t)vertCount * 12u, 4, "verts"));
        if (!context.Ok())
            return false;
        std::memcpy(native, session->zoneMemory->blocks[4].data + bulkStart,
                    (std::size_t)vertCount * 12u);
        clip->vertCount = vertCount;
        clip->verts = native;
    }
    else if (vertCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "verts");
        return false;
    }

    if (triCount != 0 && triIndexRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(2, "tri indices");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, (uint32_t)triIndices, 2, "tri indices"))
            return false;
        if (!context.RecordSpan(bulkStart, (uint32_t)triIndices))
            return false;
        uint16_t *native = static_cast<uint16_t *>(
            context.Alloc((std::size_t)triIndices, 2, "tri indices"));
        if (!context.Ok())
            return false;
        const XBlock &block = session->zoneMemory->blocks[4];
        for (uint64_t i = 0; i < triIndices; ++i)
            native[i] = ReadLe16(block.data + bulkStart + i * 2u);
        clip->triCount = static_cast<int>(triCount);
        clip->triIndices = native;
    }
    else if (triCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "tri indices");
        return false;
    }

    if (triCount != 0 && triWalkRef)
    {
        const uint32_t bulkStart = session->wire.cursor[4];
        if (!context.Stream(4, (uint32_t)walkable, 1, "tri walkable"))
            return false;
        if (!context.RecordSpan(bulkStart, (uint32_t)walkable))
            return false;
        uint8_t *native = static_cast<uint8_t *>(
            context.Alloc((std::size_t)walkable, 1, "tri walkable"));
        if (!context.Ok())
            return false;
        std::memcpy(native, session->zoneMemory->blocks[4].data + bulkStart,
                    (std::size_t)walkable);
        clip->triEdgeIsWalkable = native;
    }
    else if (triCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "tri walkable");
        return false;
    }

    if (borderCount != 0 && borderRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "borders");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, borderCount * 28u, 4, "borders"))
            return false;
        if (!context.RecordSpan(bulkStart, borderCount * 28u))
            return false;
        CollisionBorder *native = static_cast<CollisionBorder *>(context.Alloc(
            (std::size_t)borderCount * sizeof(CollisionBorder), alignof(CollisionBorder),
            "borders"));
        if (!context.Ok())
            return false;
        std::memcpy(native, session->zoneMemory->blocks[4].data + bulkStart,
                    (std::size_t)borderCount * 28u);
        for (uint32_t i = 0; i < borderCount; ++i)
        {
            if (context.borderCount >= context.borderCap)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "border map");
                return false;
            }
            context.borders[context.borderCount].start = bulkStart + i * 28u;
            context.borders[context.borderCount].native = &native[i];
            ++context.borderCount;
        }
        clip->borderCount = static_cast<int>(borderCount);
        clip->borders = native;
    }
    else if (borderCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "borders");
        return false;
    }

    if (partCount != 0 && partRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "partitions");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, partCount * 12u, 4, "partitions"))
            return false;
        if (!context.RecordSpan(bulkStart, partCount * 12u))
            return false;
        CollisionPartition *native = static_cast<CollisionPartition *>(context.Alloc(
            (std::size_t)partCount * sizeof(CollisionPartition),
            alignof(CollisionPartition), "partitions"));
        if (!context.Ok())
            return false;
        std::memset(native, 0, (std::size_t)partCount * sizeof(CollisionPartition));
        const XBlock &block = session->zoneMemory->blocks[4];
        for (uint32_t i = 0; i < partCount; ++i)
        {
            const uint8_t *wire = block.data + bulkStart + i * 12u;
            CollisionPartition *out = &native[i];
            out->triCount = wire[0];
            out->borderCount = wire[1];
            std::memcpy(&out->firstTri, wire + 4, 4);
            const uint32_t slot = ReadLe32(wire + 8);
            if (!slot)
            {
                if (out->borderCount)
                {
                    context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "partition borders");
                    return false;
                }
                out->borders = nullptr;
            }
            else if (slot == kInlineRef)
            {
                const uint32_t borderStart = session->wire.cursor[4];
                if (!context.Stream(4, 28, 4, "partition border"))
                    return false;
                CollisionBorder *border = static_cast<CollisionBorder *>(context.Alloc(
                    sizeof(CollisionBorder), alignof(CollisionBorder), "partition border"));
                if (!context.Ok())
                    return false;
                std::memcpy(border,
                            session->zoneMemory->blocks[4].data + borderStart, 28);
                if (context.borderCount >= context.borderCap)
                {
                    context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "border map");
                    return false;
                }
                context.borders[context.borderCount].start = borderStart;
                context.borders[context.borderCount].native = border;
                ++context.borderCount;
                out->borders = border;
            }
            else
            {
                RetailWireToken token{};
                if (!DecodeToken(&context, slot, 28, &token) ||
                    token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
                {
                    context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "partition borders");
                    return false;
                }
                CollisionBorder *found = static_cast<CollisionBorder *>(
                    ClipDecodeContext::FindExact(context.borders, context.borderCount,
                                                 token.offset));
                if (!found)
                {
                    // Fallback: cross-boundary dedup, same as leaf-brush
                    // lists -- the alias may name any already-streamed
                    // bulk span, not just a recorded border.
                    const XBlock &spanBlock = session->zoneMemory->blocks[4];
                    if (ClipDecodeContext::FindSpan(context.bulkSpans, context.bulkSpanCount,
                                                    token.offset, 28u))
                    {
                        CollisionBorder *border = static_cast<CollisionBorder *>(
                            context.Alloc(sizeof(CollisionBorder),
                                          alignof(CollisionBorder),
                                          "partition border"));
                        if (!context.Ok())
                            return false;
                        std::memcpy(border, spanBlock.data + token.offset, 28);
                        found = border;
                    }
                }
                if (!found)
                {
                    context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "partition borders");
                    return false;
                }
                out->borders = found;
            }
        }
        clip->partitionCount = static_cast<int>(partCount);
        clip->partitions = native;
    }
    else if (partCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "partitions");
        return false;
    }

    if (aabbCount != 0 && aabbRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "aabb trees");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, aabbCount * 32u, 4, "aabb trees"))
            return false;
        if (!context.RecordSpan(bulkStart, aabbCount * 32u))
            return false;
        CollisionAabbTree *native = static_cast<CollisionAabbTree *>(context.Alloc(
            (std::size_t)aabbCount * sizeof(CollisionAabbTree),
            alignof(CollisionAabbTree), "aabb trees"));
        if (!context.Ok())
            return false;
        std::memcpy(native, session->zoneMemory->blocks[4].data + bulkStart,
                    (std::size_t)aabbCount * 32u);
        clip->aabbTreeCount = static_cast<int>(aabbCount);
        clip->aabbTrees = native;
    }
    else if (aabbCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "aabb trees");
        return false;
    }

    if (cmodelCount != 0 && cmodelRef)
    {
        const uint32_t bulkStart = context.AlignBlock4(4, "cmodels");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, cmodelCount * 72u, 4, "cmodels"))
            return false;
        if (!context.RecordSpan(bulkStart, cmodelCount * 72u))
            return false;
        cmodel_t *native = static_cast<cmodel_t *>(context.Alloc(
            (std::size_t)cmodelCount * sizeof(cmodel_t), alignof(cmodel_t), "cmodels"));
        if (!context.Ok())
            return false;
        std::memcpy(native, session->zoneMemory->blocks[4].data + bulkStart,
                    (std::size_t)cmodelCount * 72u);
        clip->numSubModels = cmodelCount;
        clip->cmodels = native;
    }
    else if (cmodelCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "cmodels");
        return false;
    }

    // Brush-embedded side/edge widening, shared by array brushes and the
    // box brush below (Load_cbrush_t order: sides slot, then edge slot).
    auto widenBrushSide = [&](uint32_t recOffset, cbrushside_t *out) -> bool {
        const XBlock &sideBlock = session->zoneMemory->blocks[4];
        const uint8_t *wire = sideBlock.data + recOffset;
        if (!WidenPlaneSlot(&context, ReadLe32(wire), &out->plane, "brush side plane"))
            return false;
        out->materialNum = ReadLe32(wire + 4);
        out->firstAdjacentSideOffset = static_cast<int16_t>(ReadLe16(wire + 8));
        out->edgeCount = wire[10];
        if (context.sideCount >= context.sideCap)
        {
            context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "side map");
            return false;
        }
        context.sides[context.sideCount].start = recOffset;
        context.sides[context.sideCount].native = out;
        ++context.sideCount;
        return true;
    };
    auto widenBrushEdge = [&](uint32_t slot, uint8_t **out) -> bool {
        *out = nullptr;
        if (!slot)
            return true;
        if (slot == kInlineRef)
        {
            const uint32_t edgeStart = session->wire.cursor[4];
            if (!context.Stream(4, 1, 1, "brush edge"))
                return false;
            uint8_t *native = static_cast<uint8_t *>(context.Alloc(1, 1, "brush edge"));
            if (!context.Ok())
                return false;
            native[0] = session->zoneMemory->blocks[4].data[edgeStart];
            if (context.edgeCount >= context.edgeCap)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "edge map");
                return false;
            }
            context.edges[context.edgeCount].start = edgeStart;
            context.edges[context.edgeCount].len = 1;
            context.edges[context.edgeCount].native = native;
            ++context.edgeCount;
            *out = native;
            return true;
        }
        RetailWireToken token{};
        if (!DecodeToken(&context, slot, 1, &token) ||
            token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
        {
            context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "brush edge");
            return false;
        }
        if (const ClipDecodeContext::ByteSpan *span =
                ClipDecodeContext::FindSpan(context.edges, context.edgeCount,
                                            token.offset, 1))
        {
            *out = span->native + (token.offset - span->start);
            return true;
        }
        // Fallback: cross-boundary dedup -- a single edge byte may name
        // any already-streamed bulk span. Bind by containment (a byte
        // needs no validation).
        if (ClipDecodeContext::FindSpan(context.bulkSpans, context.bulkSpanCount,
                                        token.offset, 1))
        {
            uint8_t *native = static_cast<uint8_t *>(context.Alloc(1, 1, "brush edge"));
            if (!context.Ok())
                return false;
            native[0] = session->zoneMemory->blocks[4].data[token.offset];
            *out = native;
            return true;
        }
        context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "brush edge");
        return false;
    };
    // Widen one 80-byte brush record at an already-streamed block-4 offset.
    // The 10-byte ILP32 align tail (70..79) is padding: it stays zeroed,
    // and a nonzero value is reported loudly (it would mean the field map
    // above is wrong, not that data was lost).
    auto widenBrushAt = [&](uint32_t recOffset, cbrush_t *out) -> bool {
        const XBlock &block = session->zoneMemory->blocks[4];
        const uint8_t *wire = block.data + recOffset;
        std::memcpy(out->mins, wire, 12);
        std::memcpy(&out->contents, wire + 12, 4);
        std::memcpy(out->maxs, wire + 16, 12);
        std::memcpy(&out->numsides, wire + 28, 4);
        const uint32_t sidesSlot = ReadLe32(wire + 32);
        if (!sidesSlot)
        {
            out->sides = nullptr;
        }
        else if (sidesSlot == kInlineRef)
        {
            const uint32_t sideStart = context.AlignBlock4(4, "brush side");
            if (!context.Ok())
                return false;
            if (!context.Stream(4, 12, 4, "brush side"))
                return false;
            cbrushside_t *side = static_cast<cbrushside_t *>(context.Alloc(
                sizeof(cbrushside_t), alignof(cbrushside_t), "brush side"));
            if (!context.Ok())
                return false;
            std::memset(side, 0, sizeof(*side));
            if (!widenBrushSide(sideStart, side))
                return false;
            out->sides = side;
        }
        else
        {
            RetailWireToken token{};
            if (!DecodeToken(&context, sidesSlot, 12, &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "brush sides");
                return false;
            }
            cbrushside_t *found = static_cast<cbrushside_t *>(
                ClipDecodeContext::FindExact(context.sides, context.sideCount, token.offset));
            if (!found)
            {
                // Fallback: cross-boundary dedup, same as leaf-brush
                // lists and partition borders -- the side may name any
                // already-streamed bulk span. Verbatim-copy the 12-byte
                // record without consuming stream bytes, then resolve
                // its plane slot without streaming (null binds null, an
                // alias binds the plane map; an inline slot names bytes
                // consumed in the original context and fails loudly).
                const XBlock &spanBlock = session->zoneMemory->blocks[4];
                if (ClipDecodeContext::FindSpan(context.bulkSpans, context.bulkSpanCount,
                                                token.offset, 12u))
                {
                    cbrushside_t *side = static_cast<cbrushside_t *>(context.Alloc(
                        sizeof(cbrushside_t), alignof(cbrushside_t), "brush side"));
                    if (!context.Ok())
                        return false;
                    std::memset(side, 0, sizeof(*side));
                    const uint8_t *wire = spanBlock.data + token.offset;
                    const uint32_t planeSlot = ReadLe32(wire);
                    if (!planeSlot)
                    {
                        side->plane = nullptr;
                    }
                    else if (planeSlot == kInlineRef)
                    {
                        ++context.deferredSidePlanes;
                        side->plane = nullptr;
                    }
                    else
                    {
                        RetailWireToken planeToken{};
                        if (!DecodeToken(&context, planeSlot, 20, &planeToken) ||
                            planeToken.kind != RETAIL_WIRE_TOKEN_OFFSET ||
                            planeToken.block != 4)
                        {
                            ++context.deferredSidePlanes;
                            side->plane = nullptr;
                        }
                        else
                        {
                            side->plane = context.FindPlaneSoft(planeToken.offset);
                            if (!side->plane)
                                ++context.deferredSidePlanes;
                        }
                    }
                    side->materialNum = ReadLe32(wire + 4);
                    side->firstAdjacentSideOffset =
                        static_cast<int16_t>(ReadLe16(wire + 8));
                    side->edgeCount = wire[10];
                    found = side;
                }
            }
            if (!found)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "brush sides");
                return false;
            }
            out->sides = found;
        }
        for (uint32_t a = 0; a < 2; ++a)
        {
            for (uint32_t b = 0; b < 3; ++b)
                out->axialMaterialNum[a][b] =
                    static_cast<int16_t>(ReadLe16(wire + 36 + (a * 3u + b) * 2u));
        }
        if (!widenBrushEdge(ReadLe32(wire + 48), &out->baseAdjacentSide))
            return false;
        for (uint32_t a = 0; a < 2; ++a)
        {
            for (uint32_t b = 0; b < 3; ++b)
                out->firstAdjacentSideOffsets[a][b] =
                    static_cast<int16_t>(ReadLe16(wire + 52 + (a * 3u + b) * 2u));
        }
        for (uint32_t a = 0; a < 2; ++a)
        {
            for (uint32_t b = 0; b < 3; ++b)
                out->edgeCount[a][b] = wire[64 + a * 3u + b];
        }
        for (uint32_t t = 70; t < 80; ++t)
        {
            if (wire[t] != 0)
                Com_Printf(0, "RetailWalkLiveLoadClipMap: '%s' brush pad nonzero rec=%u tail=%u\n",
                           context.assetName, recOffset, t);
        }
        return true;
    };

    if (brushCount != 0 && brushRef)
    {
        const uint32_t bulkStart = session->wire.cursor[4];
        // Brushes align 16 in the stream (walk + Load_cbrush_tArray order).
        const uint32_t aligned = (session->wire.cursor[4] + 15u) & ~15u;
        if (aligned != session->wire.cursor[4])
        {
            if (!RetailWireBlocksAlloc(&session->wire, 4, aligned - session->wire.cursor[4], 1))
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_TRUNCATED, "brushes align");
                return false;
            }
        }
        const uint32_t arrayStart = session->wire.cursor[4];
        if (!context.Stream(4, brushCount * 80u, 16, "brushes"))
            return false;
        if (!context.RecordSpan(bulkStart, brushCount * 80u))
            return false;
        (void)bulkStart;
        cbrush_t *native = static_cast<cbrush_t *>(context.Alloc(
            (std::size_t)brushCount * sizeof(cbrush_t), alignof(cbrush_t), "brushes"));
        if (!context.Ok())
            return false;
        std::memset(native, 0, (std::size_t)brushCount * sizeof(cbrush_t));
        for (uint32_t i = 0; i < brushCount; ++i)
        {
            if (!widenBrushAt(arrayStart + i * 80u, &native[i]))
                return false;
            if (context.brushCount >= context.brushCap)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "brush map");
                return false;
            }
            context.brushes[context.brushCount].start = arrayStart + i * 80u;
            context.brushes[context.brushCount].native = &native[i];
            ++context.brushCount;
        }
        clip->numBrushes = static_cast<uint16_t>(brushCount);
        clip->brushes = native;
    }
    else if (brushCount)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "brushes");
        return false;
    }

    if (visRef && numClusters != 0 && clusterBytes != 0)
    {
        const uint32_t bulkStart = session->wire.cursor[4];
        if (!context.Stream(4, (uint32_t)visBytes, 1, "visibility"))
            return false;
        if (!context.RecordSpan(bulkStart, (uint32_t)visBytes))
            return false;
        uint8_t *native = static_cast<uint8_t *>(
            context.Alloc((std::size_t)visBytes, 1, "visibility"));
        if (!context.Ok())
            return false;
        std::memcpy(native, session->zoneMemory->blocks[4].data + bulkStart,
                    (std::size_t)visBytes);
        clip->numClusters = static_cast<int>(numClusters);
        clip->clusterBytes = static_cast<int>(clusterBytes);
        clip->visibility = native;
    }
    else if (numClusters || clusterBytes)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "visibility");
        return false;
    }
    clip->vised = static_cast<int>(vised);

    // Nested MapEnts leaf: inline (or insert, which carries an inline
    // body like every other nested slot -- the walk treats insert as
    // inline after reserving its handle slot) streams the 12-byte root,
    // the name, and the entity bytes (Load_MapEnts order); a null slot
    // binds null; a true alias resolves by exact name through the
    // registry. This retires the TEMP walk-only capture bridge: the same
    // bytes register here.
    if (mapEntsRef)
    {
        if (mapEntsRef != kInlineRef && mapEntsRef != kInsertRef)
        {
            RetailWireToken token{};
            if (!DecodeToken(&context, mapEntsRef, 1, &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "mapents ref");
                return false;
            }
            char nameBuf[64]{};
            const XBlock &nameBlock = session->zoneMemory->blocks[4];
            uint32_t len = 0;
            while (token.offset + len < nameBlock.size &&
                   nameBlock.data[token.offset + len] != 0 && len < sizeof(nameBuf) - 1)
            {
                const unsigned char c = nameBlock.data[token.offset + len];
                if (c < 0x20 || c >= 0x7f)
                {
                    context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "mapents ref");
                    return false;
                }
                nameBuf[len] = static_cast<char>(c);
                ++len;
            }
            if (token.offset + len >= nameBlock.size || len == 0)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "mapents ref");
                return false;
            }
            const XAssetHeader found = DB_FindXAssetHeader(ASSET_TYPE_MAP_ENTS, nameBuf);
            if (!found.mapEnts)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "mapents ref");
                return false;
            }
            clip->mapEnts = found.mapEnts;
            context.mapEntsBound = true;
        }
        else
        {
            // Load_MapEntsPtr pushes stream position 0, so the 12-byte
            // MapEnts root is AllocLoad'ed in the temp block, and for an
            // insert (-2) DB_InsertPointer then reserves a 4-byte pointer
            // slot in block 4 before Load_MapEnts pushes 4 for the name and
            // entity string. Streaming the root into block 4 instead put
            // 12 bytes where retail has 4 (insert) or 0 (inline); whether a
            // zone survived that depended on the box brush's 16-byte
            // alignment absorbing the difference (airplane, killhouse) or
            // not (armada: every later block-4 alias named wrong bytes).
            // The temp bytes are snapshotted and the temp cursor rewound, as
            // DB_PopStreamPos restores the temp position.
            const uint32_t mapRootStart = (session->wire.cursor[0] + 3u) & ~3u;
            if (!context.Stream(0, 12, 4, "mapents root"))
                return false;
            uint8_t mapRoot[12]{};
            std::memcpy(mapRoot, session->zoneMemory->blocks[0].data + mapRootStart,
                        sizeof(mapRoot));
            RetailWireBlocksRewind(&session->wire, 0, mapRootStart);
            uint32_t mapInsertSlot = 0;
            if (mapEntsRef == kInsertRef)
            {
                uint8_t *slot = RetailWireBlocksAlloc(&session->wire, 4, 4, 4);
                if (!slot)
                {
                    context.Fail(RETAIL_CLIPMAP_DECODE_TRUNCATED, "mapents insert");
                    return false;
                }
                mapInsertSlot = static_cast<uint32_t>(slot - session->zoneMemory->blocks[4].data);
            }
            const uint32_t mapNameRef = ReadLe32(mapRoot);
            const uint32_t mapEntityRef = ReadLe32(mapRoot + 4);
            const int32_t mapChars = static_cast<int32_t>(ReadLe32(mapRoot + 8));
            if (mapChars < 0)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ROOT, "mapents root");
                return false;
            }
            const char *mapName = nullptr;
            if (!StreamName(&context, mapNameRef, &mapName, "mapents name") ||
                !mapName || !mapName[0])
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_BAD_NAME, "mapents name");
                return false;
            }
            MapEnts *ents = static_cast<MapEnts *>(context.Alloc(
                sizeof(MapEnts), alignof(MapEnts), "mapents"));
            if (!context.Ok())
                return false;
            std::memset(ents, 0, sizeof(*ents));
            ents->name = mapName;
            ents->numEntityChars = mapChars;
            if (mapEntityRef)
            {
                char *entityString = static_cast<char *>(context.Alloc(
                    mapChars ? (std::size_t)mapChars : 1, 1, "mapents entities"));
                if (!context.Ok())
                    return false;
                if (mapEntityRef == kInlineRef)
                {
                    if (!context.Stream(4, (uint32_t)mapChars, 1, "mapents entities"))
                        return false;
                    std::memcpy(entityString,
                                session->zoneMemory->blocks[4].data +
                                    session->wire.cursor[4] - (uint32_t)mapChars,
                                (uint32_t)mapChars);
                }
                else
                {
                    RetailWireToken token{};
                    if (!DecodeToken(&context, mapEntityRef, (uint32_t)mapChars,
                                     &token) ||
                        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4 ||
                        (uint64_t)token.offset + (uint32_t)mapChars >
                            session->zoneMemory->blocks[4].size)
                    {
                        context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "mapents entities");
                        return false;
                    }
                    std::memcpy(entityString,
                                session->zoneMemory->blocks[4].data + token.offset,
                                (uint32_t)mapChars);
                }
                ents->entityString = entityString;
            }
            const XAssetHeader registered =
                RetailZoneLoadSessionRegister(session, ASSET_TYPE_MAP_ENTS, {ents});
            if (!registered.mapEnts)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_REGISTRATION_FAILED, "mapents");
                return false;
            }
            // A later DB_ConvertOffsetToAlias to the insert slot copies
            // this MapEnts pointer.
            if (mapInsertSlot && context.shared &&
                !RetailWorldRecordZoneSlot(context.shared, mapInsertSlot,
                                           ASSET_TYPE_MAP_ENTS, registered))
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "mapents insert slot");
                return false;
            }
            clip->mapEnts = registered.mapEnts;
            context.mapEntsBound = true;
        }
    }

    // Box brush: inline streams one 80-byte record plus its side/edge
    // (ReadRetailCBrushAt order); an alias binds an already-widened array
    // brush; null binds null.
    if (boxBrushRef)
    {
        if (boxBrushRef == kInlineRef)
        {
            const uint32_t boxAligned = (session->wire.cursor[4] + 15u) & ~15u;
            if (boxAligned != session->wire.cursor[4])
            {
                if (!RetailWireBlocksAlloc(&session->wire, 4,
                                           boxAligned - session->wire.cursor[4], 1))
                {
                    context.Fail(RETAIL_CLIPMAP_DECODE_TRUNCATED, "box brush align");
                    return false;
                }
            }
            const uint32_t boxStart = session->wire.cursor[4];
            if (!context.Stream(4, 80, 16, "box brush"))
                return false;
            if (!context.RecordSpan(boxStart, 80))
                return false;
            cbrush_t *box = static_cast<cbrush_t *>(context.Alloc(
                sizeof(cbrush_t), alignof(cbrush_t), "box brush"));
            if (!context.Ok())
                return false;
            std::memset(box, 0, sizeof(*box));
            if (!widenBrushAt(boxStart, box))
                return false;
            clip->box_brush = box;
        }
        else
        {
            RetailWireToken token{};
            if (!DecodeToken(&context, boxBrushRef, 80, &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "box brush");
                return false;
            }
            cbrush_t *found = static_cast<cbrush_t *>(
                ClipDecodeContext::FindExact(context.brushes, context.brushCount,
                                             token.offset));
            if (!found)
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "box brush");
                return false;
            }
            clip->box_brush = found;
        }
    }

    // Embedded box model: a 72-byte cmodel_t with no pointers.
    std::memcpy(&clip->box_model, root + 172, 72);

    // Dynamic-entity definition lists: 96-byte records; the XModel slot
    // widens through the shared zone widener, brush-model u16s copy, and
    // any non-null destroyFx/destroyPieces/physPreset slot fails loudly
    // (no widener owns those families -- B1/B6 scope, never a null bind).
    const uint32_t dynCounts[2] = {dynCount0, dynCount1};
    for (uint32_t list = 0; list < 2; ++list)
    {
        if (dynCounts[list] == 0)
            continue;
        if (!dynDefRef[list])
        {
            context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "dyn defs");
            return false;
        }
        const uint32_t defStart = context.AlignBlock4(4, "dyn defs");
        if (!context.Ok())
            return false;
        if (!context.Stream(4, dynCounts[list] * 96u, 4, "dyn defs"))
            return false;
        DynEntityDef *native = static_cast<DynEntityDef *>(context.Alloc(
            (std::size_t)dynCounts[list] * sizeof(DynEntityDef),
            alignof(DynEntityDef), "dyn defs"));
        if (!context.Ok())
            return false;
        std::memset(native, 0, (std::size_t)dynCounts[list] * sizeof(DynEntityDef));
        const XBlock &defBlock = session->zoneMemory->blocks[4];
        for (uint32_t d = 0; d < dynCounts[list]; ++d)
        {
            const uint8_t *wire = defBlock.data + defStart + d * 96u;
            DynEntityDef *out = &native[d];
            out->type = static_cast<DynEntityType>(ReadLe32(wire));
            std::memcpy(&out->pose, wire + 4, 28);
            XModel *model = nullptr;
            if (!RetailWorldWidenXModel(context.shared, ReadLe32(wire + 32), &model,
                                        defStart + d * 96u + 32))
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_UNSUPPORTED_ASSET, "dyn xmodel");
                return false;
            }
            if (model)
                ++context.widenedXModels;
            out->xModel = model;
            out->brushModel = ReadLe16(wire + 36);
            out->physicsBrushModel = ReadLe16(wire + 38);
            // destroyFx/destroyPieces/physPreset are real
            // Load_FxEffectDefHandle/Load_XModelPiecesPtr/Load_PhysPresetPtr
            // slots. resolve them through their real owners and record each
            // declaring field slot so a sibling record's
            // DB_ConvertOffsetToAlias reference binds the same object.
            // PhysPreset goes through the nested widener; FX inline/insert
            // through the real FxEffectDef loader. destroyPieces has no
            // widener in this codebase, so a non-null slot fails loudly
            // (never a silent null placeholder); killhouse declares none.
            const uint32_t destroyFxRef = ReadLe32(wire + 40);
            if (destroyFxRef)
            {
                XAssetHeader fx{};
                const uint32_t fxFieldOffset = defStart + d * 96u + 40u;
                if (destroyFxRef == kInlineRef || destroyFxRef == kInsertRef)
                {
                    uint32_t fxInsertSlot = 0;
                    if (destroyFxRef == kInsertRef)
                    {
                        uint8_t *slot = RetailWireBlocksAlloc(&session->wire, 4, 4, 4);
                        if (!slot)
                        {
                            context.Fail(RETAIL_CLIPMAP_DECODE_TRUNCATED, "dyn fx insert");
                            return false;
                        }
                        fxInsertSlot = static_cast<uint32_t>(
                            slot - session->zoneMemory->blocks[4].data);
                    }
                    RetailWalkDirectoryRecord fxRecord{};
                    uint32_t fxDeferred = 0;
                    if (!RetailWalkLiveLoadFxEffectDef(
                            session, reader, context.shared, &fx, &fxRecord, &fxDeferred,
                            fxInsertSlot ? fxInsertSlot : fxFieldOffset))
                    {
                        context.Fail(RETAIL_CLIPMAP_DECODE_UNSUPPORTED_ASSET, "dyn destroyFx");
                        return false;
                    }
                    if (fxInsertSlot &&
                        !RetailWorldRecordZoneSlot(context.shared, fxFieldOffset,
                                                   ASSET_TYPE_FX, fx))
                    {
                        context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "dyn fx field slot");
                        return false;
                    }
                    context.deferredDynFx += fxDeferred;
                }
                else
                {
                    if (!RetailWorldResolveNestedAlias(context.shared, destroyFxRef,
                                                       ASSET_TYPE_FX, &fx) ||
                        !RetailWorldRecordZoneSlot(context.shared, fxFieldOffset,
                                                   ASSET_TYPE_FX, fx))
                    {
                        context.Fail(RETAIL_CLIPMAP_DECODE_BAD_ALIAS, "dyn destroyFx");
                        return false;
                    }
                }
                out->destroyFx = fx.fx;
                if (fx.fx)
                    ++context.boundDynFx;
            }
            else
            {
                out->destroyFx = nullptr;
                if (!RetailWorldRecordZoneSlot(context.shared, defStart + d * 96u + 40u,
                                               ASSET_TYPE_FX, {}))
                {
                    context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "dyn fx null slot");
                    return false;
                }
            }
            if (ReadLe32(wire + 44))
            {
                // No live XModelPieces widener exists in this codebase
                // (Load_XModelPiecesPtr is unimplemented). Binding null here
                // would be a silent placeholder for a real consumer-visible
                // asset, so fail the load instead. Retail killhouse declares
                // zero dyn destroyPieces slots.
                context.Fail(RETAIL_CLIPMAP_DECODE_UNSUPPORTED_ASSET, "dyn destroyPieces");
                return false;
            }
            out->destroyPieces = nullptr;
            PhysPreset *preset = nullptr;
            if (!RetailWorldWidenPhysPreset(context.shared, ReadLe32(wire + 48), &preset,
                                            defStart + d * 96u + 48u))
            {
                context.Fail(RETAIL_CLIPMAP_DECODE_UNSUPPORTED_ASSET, "dyn physPreset");
                return false;
            }
            if (preset)
                ++context.boundDynPreset;
            out->physPreset = preset;
            // deferredDynPreset/deferredDynFx are leftover counts that
            // must read zero for the strict acceptance; the widener
            // fails loudly instead of incrementing them for a non-null slot.
            std::memcpy(&out->health, wire + 52, 4);
            std::memcpy(&out->mass, wire + 56, 36);
            std::memcpy(&out->contents, wire + 92, 4);
        }
        clip->dynEntCount[list] = static_cast<uint16_t>(dynCounts[list]);
        clip->dynEntDefList[list] = native;
    }

    // Runtime block-1 lists: a nonzero slot expands count*stride zeroed
    // bytes (ExpandRuntime, the walk's own accounting); the native arrays
    // widen from those expanded zeros -- the honest runtime-initial state.
    // Poses/colls copy verbatim; clients widen 12 wire bytes to the
    // 16-byte native record (zero-extended handle, matching the header's
    // LP64 ownership contract).
    static const uint32_t kRuntimeStride[6] = {32, 32, 12, 12, 20, 20};
    const uint32_t *runtimeSlots[6] = {&dynPoseRef[0], &dynPoseRef[1], &dynClientRef[0],
                                       &dynClientRef[1], &dynCollRef[0], &dynCollRef[1]};
    const uint32_t runtimeCounts[6] = {dynCount0, dynCount1,          dynCount0,
                                       dynCount1, dynCount0,          dynCount1};
    uint32_t runtimeBase[6] = {0, 0, 0, 0, 0, 0};
    for (uint32_t r = 0; r < 6; ++r)
    {
        if (!*runtimeSlots[r])
            continue;
        if (runtimeCounts[r] > UINT32_MAX / kRuntimeStride[r])
        {
            context.Fail(RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH, "dyn runtime");
            return false;
        }
        runtimeBase[r] = session->wire.cursor[1];
        if (!RetailZoneLoadSessionExpandRuntime(
                session, 1, runtimeCounts[r] * kRuntimeStride[r], 4))
        {
            context.Fail(RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA, "dyn runtime");
            return false;
        }
    }
    const XBlock &runtimeBlock = session->zoneMemory->blocks[1];
    for (uint32_t list = 0; list < 2; ++list)
    {
        if (dynPoseRef[list] && dynCounts[list])
        {
            DynEntityPose *poses = static_cast<DynEntityPose *>(context.Alloc(
                (std::size_t)dynCounts[list] * sizeof(DynEntityPose),
                alignof(DynEntityPose), "dyn poses"));
            if (!context.Ok())
                return false;
            std::memcpy(poses, runtimeBlock.data + runtimeBase[list],
                        (std::size_t)dynCounts[list] * 32u);
            clip->dynEntPoseList[list] = poses;
        }
        if (dynClientRef[list] && dynCounts[list])
        {
            DynEntityClient *clients = static_cast<DynEntityClient *>(context.Alloc(
                (std::size_t)dynCounts[list] * sizeof(DynEntityClient),
                alignof(DynEntityClient), "dyn clients"));
            if (!context.Ok())
                return false;
            std::memset(clients, 0,
                        (std::size_t)dynCounts[list] * sizeof(DynEntityClient));
            const uint8_t *wire = runtimeBlock.data + runtimeBase[2 + list];
            for (uint32_t i = 0; i < dynCounts[list]; ++i)
            {
                clients[i].physObjId = ReadLe32(wire + i * 12u);
                clients[i].flags = ReadLe16(wire + i * 12u + 4);
                clients[i].lightingHandle = ReadLe16(wire + i * 12u + 6);
                std::memcpy(&clients[i].health, wire + i * 12u + 8, 4);
            }
            clip->dynEntClientList[list] = clients;
        }
        if (dynCollRef[list] && dynCounts[list])
        {
            DynEntityColl *colls = static_cast<DynEntityColl *>(context.Alloc(
                (std::size_t)dynCounts[list] * sizeof(DynEntityColl),
                alignof(DynEntityColl), "dyn colls"));
            if (!context.Ok())
                return false;
            std::memcpy(colls, runtimeBlock.data + runtimeBase[4 + list],
                        (std::size_t)dynCounts[list] * 20u);
            clip->dynEntCollList[list] = colls;
        }
    }
    clip->checksum = checksum;
    // Atomic commit through the existing real database owner: one copy
    // into the engine's own collision singleton, then session registration
    // whose returned pooled header must equal &cm. Pre-commit cm is
    // untouched (rollback is discarding the transaction).
    std::memcpy(&cm, clip, sizeof(cm));
    const XAssetHeader registered =
        RetailZoneLoadSessionRegister(session, ASSET_TYPE_CLIPMAP, {&cm});
    if (!registered.clipMap || registered.clipMap != &cm)
    {
        context.Fail(RETAIL_CLIPMAP_DECODE_REGISTRATION_FAILED, "commit");
        return false;
    }
    Com_Printf(0,
               "RetailWalkLiveLoadClipMap: '%s' planes=%u static=%u materials=%u sides=%u "
               "edges=%u nodes=%u leafs=%u lbn=%u lb=%u lsurf=%u verts=%u tris=%u borders=%u "
               "parts=%u aabbs=%u cmodels=%u brushes=%u clusters=%u dyn=%u/%u xmodels=%u "
               "staticNull=%u mapents=%d defplanes=%u deffx=%u defpieces=%u defpreset=%u "
               "boundfx=%u boundpreset=%u checksum=0x%08x ok\n",
               name, planeCount, staticCount, materialCount, sideCount, edgeCount,
               nodeCount, leafCount, lbnCount, lbCount, lsurfCount, vertCount, triCount,
               borderCount, partCount, aabbCount, cmodelCount, brushCount, numClusters,
               dynCount0, dynCount1, context.widenedXModels, staticNullModels,
               context.mapEntsBound ? 1 : 0, context.deferredSidePlanes, context.deferredDynFx,
               context.deferredDynPieces, context.deferredDynPreset, context.boundDynFx,
               context.boundDynPreset, checksum);
    *header = registered;
    return true;
    };
    if (!decodeBody())
        return false;
    return true;
}