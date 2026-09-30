#include "db_retail_decode_xanim.h"
#include "../universal/retail_asset_trace.h"

#include "db_retail_walk.h"
#include "db_retail_wire.h"
#include "database.h"
#include "../xanim/xanim.h"
#include "../script/scr_stringlist.h"

#include <cstring>

// B3 XAnimParts widening. Every byte-count formula below mirrors
// ReadRetailXAnimPartsBody / ReadRetailXAnimPartTransBody /
// ReadRetailXAnimDeltaPartQuatBody (db_retail_walk.cpp) exactly, in the
// same stream order; the walk-only reader stays the independent oracle.
// The native target keeps wire/native sizes tracked separately per the
// standing LP64 rule: the 88-byte root is parsed from an explicit byte
// buffer, never cast.
namespace
{
constexpr uint32_t kInlineRef = 0xffffffffu;

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
    float value = 0.0f;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

// 88-byte ILP32 wire root field offsets (db_load.cpp Load_XAnimParts
// order; cross-checked against the walk reader's own slot reads).
constexpr uint32_t kRootName = 0;
constexpr uint32_t kRootDataByteCount = 4;
constexpr uint32_t kRootDataShortCount = 6;
constexpr uint32_t kRootDataIntCount = 8;
constexpr uint32_t kRootRandomDataByteCount = 10;
constexpr uint32_t kRootRandomDataIntCount = 12;
constexpr uint32_t kRootNumFrames = 14;
constexpr uint32_t kRootLoop = 16;
constexpr uint32_t kRootDelta = 17;
constexpr uint32_t kRootBoneCount = 18;
constexpr uint32_t kRootNotifyCount = 28;
constexpr uint32_t kRootAssetType = 29;
constexpr uint32_t kRootIsDefault = 30;
constexpr uint32_t kRootRandomDataShortCount = 32;
constexpr uint32_t kRootIndexCount = 36;
constexpr uint32_t kRootFramerate = 40;
constexpr uint32_t kRootFrequency = 44;
constexpr uint32_t kRootNames = 48;
constexpr uint32_t kRootDataByte = 52;
constexpr uint32_t kRootDataShort = 56;
constexpr uint32_t kRootDataInt = 60;
constexpr uint32_t kRootRandomDataShort = 64;
constexpr uint32_t kRootRandomDataByte = 68;
constexpr uint32_t kRootRandomDataInt = 72;
constexpr uint32_t kRootIndices = 76;
constexpr uint32_t kRootNotify = 80;
constexpr uint32_t kRootDeltaPart = 84;
constexpr uint32_t kRootBytes = 88;

struct XAnimDecodeContext
{
    RetailZoneLoadSession *session = nullptr;
    FsRetailFastfileReader *reader = nullptr;
    RetailXAnimDecodeResult result = RETAIL_XANIM_DECODE_OK;
    const char *assetName = "";
    // Index-width branch taken by this body (numframes >= 0x100 selects
    // the 2-byte arm everywhere: trans/deltaquat dynamic indices and the
    // top-level indices union). Recorded so the proof shows which branch
    // a fixture exercised.
    bool wideIndices = false;
    // Per-occurrence SL_* IDs interned by this body so far. The caller
    // unwinds them (SL_RemoveRefToString each) if the body fails after
    // interning anything: a failed load aborts its zone without ever
    // registering, so without this the partial references would leak past
    // the abort. On success ownership transfers to the registered asset
    // (freed by the XAnim remove handler on unload) and the list is
    // dropped. Capacity 512 covers the wire maxima (255 bones + 255
    // notifies, both u8 counts).
    uint16_t *ownedSL = nullptr;
    uint32_t ownedCount = 0;
    uint32_t ownedCap = 0;

    void Fail(RetailXAnimDecodeResult code, const char *stage)
    {
        if (result == RETAIL_XANIM_DECODE_OK)
        {
            result = code;
            Com_Printf(0, "RetailWalkLiveLoadXAnimParts: '%s' %s failed (%s)\n",
                       assetName ? assetName : "(null)", stage,
                       RetailXAnimDecodeResultName(code));
        }
    }

    bool Ok() const { return result == RETAIL_XANIM_DECODE_OK; }

    bool Stream(uint32_t block, uint32_t bytes, uint32_t alignment, const char *stage)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, block, bytes, alignment))
        {
            Fail(RETAIL_XANIM_DECODE_TRUNCATED, stage);
            return false;
        }
        return true;
    }

    void *Alloc(std::size_t bytes, std::size_t alignment, const char *stage)
    {
        void *memory = RetailZoneLoadSessionAlloc(session, bytes, alignment);
        if (!memory)
            Fail(RETAIL_XANIM_DECODE_OUT_OF_ARENA, stage);
        return memory;
    }
};

// Resolve one u16 wire index into this zone's script-string table to
// verbatim engine string bytes. Lazily builds the transaction-owned
// wire-index-to-block-4-offset table on first use (one linear scan of the
// table the open already streamed); inline entries record their packed
// string offset, alias entries decode canonically to block 4, null
// entries stay UINT32_MAX and fail at the caller.
bool BuildScriptTable(XAnimDecodeContext *context)
{
    RetailZoneLoadSession *session = context->session;
    if (session->scriptStringOffsets)
        return true;
    const uint32_t count = session->scriptStringCount;
    if (!count || count > 65536)
    {
        context->Fail(RETAIL_XANIM_DECODE_SCRIPT_TABLE_MISSING, "script table count");
        return false;
    }
    const XBlock &block = session->zoneMemory->blocks[4];
    if (count > block.size / 4u)
    {
        context->Fail(RETAIL_XANIM_DECODE_SCRIPT_TABLE_MISSING, "script table slots");
        return false;
    }
    uint32_t *offsets = static_cast<uint32_t *>(
        RetailZoneLoadSessionAlloc(session, static_cast<std::size_t>(count) * 4u, 4));
    if (!offsets)
    {
        context->Fail(RETAIL_XANIM_DECODE_OUT_OF_ARENA, "script table index");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i)
        offsets[i] = UINT32_MAX;
    // Inline strings pack back-to-back right after the slot table (see
    // RetailWalkOpenDirectory); alias slots name an absolute block-4
    // offset through the canonical token contract.
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
            {
                context->Fail(RETAIL_XANIM_DECODE_SCRIPT_TABLE_MISSING, "script string bytes");
                return false;
            }
            offsets[i] = pos;
            pos = end + 1;
        }
        else if (slot != 0)
        {
            // Alias slots were not validated at open (only inline strings
            // were), so an undecodable one stays UINT32_MAX here and fails
            // loudly only if an XAnim actually references it below -- never
            // for an unrelated entry.
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

const char *ZoneScriptString(XAnimDecodeContext *context, uint32_t wireIndex,
                             const char *stage)
{
    if (!BuildScriptTable(context))
        return nullptr;
    RetailZoneLoadSession *session = context->session;
    if (wireIndex >= session->scriptStringCount)
    {
        context->Fail(RETAIL_XANIM_DECODE_INDEX_OOB, stage);
        return nullptr;
    }
    const uint32_t offset = session->scriptStringOffsets[wireIndex];
    if (offset == UINT32_MAX)
    {
        context->Fail(RETAIL_XANIM_DECODE_SCRIPT_STRING_MISSING, stage);
        return nullptr;
    }
    const XBlock &block = session->zoneMemory->blocks[4];
    if (offset >= block.size)
    {
        context->Fail(RETAIL_XANIM_DECODE_SCRIPT_STRING_MISSING, stage);
        return nullptr;
    }
    return reinterpret_cast<const char *>(block.data + offset);
}

// Intern one resolved table string through the real SL_* table with
// exactly one per-occurrence reference: an existing entry takes
// SL_AddRefToString, a new one is created with user 4 (the asset user,
// matching Load_TempStringCustom) which already carries its first
// reference. The unload half is the real XAnimFree, reached through the
// XAnim remove handler on zone unload.
bool InternScriptString(XAnimDecodeContext *context, const char *str, uint16_t *out,
                        const char *stage)
{
    const uint32_t found = SL_FindString(str);
    uint32_t id = 0;
    if (found)
    {
        SL_AddRefToString(found);
        id = found;
    }
    else
    {
        id = SL_GetString(str, 4u);
        if (!id || id >= 0x10000u)
        {
            context->Fail(RETAIL_XANIM_DECODE_SCRIPT_STRING_MISSING, stage);
            return false;
        }
    }
    if (!context->ownedSL || context->ownedCount >= context->ownedCap)
    {
        SL_RemoveRefToString(id);
        context->Fail(RETAIL_XANIM_DECODE_OUT_OF_ARENA, stage);
        return false;
    }
    context->ownedSL[context->ownedCount++] = static_cast<uint16_t>(id);
    *out = static_cast<uint16_t>(id);
    return true;
}

// Stream one inline-or-alias XString (asset name): the -1 form carries
// its bytes here, any other nonzero value names an absolute block-4
// offset through the canonical token contract, and 0 binds null (which
// the caller rejects: a nameless XAnim cannot be hashed for lookup).
bool StreamXString(XAnimDecodeContext *context, uint32_t nameRef, const char **out,
                   const char *stage)
{
    *out = nullptr;
    if (!nameRef)
        return true;
    const uint32_t stringStart = context->session->wire.cursor[4];
    if (nameRef == kInlineRef)
    {
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
    }
    else
    {
        XBlock blocks[9]{};
        for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
            blocks[blockIndex] = context->session->zoneMemory->blocks[blockIndex];
        RetailWireToken token{};
        RetailPtr32 encoded{};
        encoded.encoded = nameRef;
        if (!RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) ||
            token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
        {
            context->Fail(RETAIL_XANIM_DECODE_BAD_NAME, stage);
            return false;
        }
        const XBlock &block = context->session->zoneMemory->blocks[4];
        uint32_t end = token.offset;
        while (end < block.size && block.data[end] != 0)
        {
            const unsigned char c = block.data[end];
            if (c < 0x20 || c >= 0x7f)
            {
                context->Fail(RETAIL_XANIM_DECODE_BAD_NAME, stage);
                return false;
            }
            ++end;
        }
        if (end >= block.size || end == token.offset)
        {
            context->Fail(RETAIL_XANIM_DECODE_BAD_NAME, stage);
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
    const XBlock &block = context->session->zoneMemory->blocks[4];
    const std::size_t length =
        static_cast<std::size_t>(context->session->wire.cursor[4] - stringStart);
    if (length < 2)
    {
        context->Fail(RETAIL_XANIM_DECODE_BAD_NAME, stage);
        return false;
    }
    char *copy = static_cast<char *>(context->Alloc(length, 1, stage));
    if (!context->Ok())
        return false;
    std::memcpy(copy, block.data + stringStart, length);
    *out = copy;
    return true;
}

// Widen one XAnimPartTrans body, streaming exactly what the walk reader
// consumes: 4-byte header, then (size == 0) the 12-byte frame0, else the
// 28-byte frame header, the width-selected indices, and the optional
// small/large translation frames.
//
// Native layout note (xanim_calc.cpp:1522): consumers index
// u.frames.indices._1[i] / ._2[i] as an INLINE trailing array past the
// 2-byte union, exactly as the reference bump allocator produced by
// streaming past the struct end. The head and the index tail are therefore
// one contiguous arena allocation; only the frames array itself is a
// separate pointer allocation.
bool WidenPartTrans(XAnimDecodeContext *context, XAnimPartTrans **out)
{
    *out = nullptr;
    if (!context->Stream(4, 4, 4, "trans header"))
        return false;
    const XBlock &block = context->session->zoneMemory->blocks[4];
    const uint32_t header = context->session->wire.cursor[4] - 4;
    const uint8_t *wire = block.data + header;
    const uint32_t size = ReadLe16(wire);
    const uint32_t smallTrans = wire[2];
    if (!size)
    {
        XAnimPartTrans *trans = static_cast<XAnimPartTrans *>(
            context->Alloc(sizeof(XAnimPartTrans), alignof(XAnimPartTrans), "trans"));
        if (!context->Ok())
            return false;
        std::memset(trans, 0, sizeof(*trans));
        trans->smallTrans = static_cast<unsigned __int8>(smallTrans);
        if (!context->Stream(4, 12, 1, "trans frame0"))
            return false;
        const uint8_t *frame0 =
            context->session->zoneMemory->blocks[4].data + context->session->wire.cursor[4] - 12;
        trans->u.frame0[0] = ReadLeFloat(frame0);
        trans->u.frame0[1] = ReadLeFloat(frame0 + 4);
        trans->u.frame0[2] = ReadLeFloat(frame0 + 8);
        *out = trans;
        return true;
    }
    const uint32_t count = size + 1u;
    const uint32_t indexBytes = context->wideIndices ? count * 2u : count;
    const std::size_t headBytes = sizeof(XAnimPartTrans) + indexBytes;
    XAnimPartTrans *trans = static_cast<XAnimPartTrans *>(
        context->Alloc(headBytes, alignof(XAnimPartTrans), "trans"));
    if (!context->Ok())
        return false;
    std::memset(trans, 0, headBytes);
    trans->size = static_cast<uint16_t>(size);
    trans->smallTrans = static_cast<unsigned __int8>(smallTrans);
    if (!context->Stream(4, 28, 1, "trans frames"))
        return false;
    const uint8_t *frames =
        context->session->zoneMemory->blocks[4].data + context->session->wire.cursor[4] - 28;
    XAnimPartTransFrames *native = &trans->u.frames;
    for (uint32_t i = 0; i < 3; ++i)
    {
        native->mins[i] = ReadLeFloat(frames + i * 4u);
        native->size[i] = ReadLeFloat(frames + 12u + i * 4u);
    }
    const uint32_t framesRef = ReadLe32(frames + 24);
    if (!context->Stream(4, indexBytes, context->wideIndices ? 2u : 1u, "trans indices"))
        return false;
    const uint8_t *indices =
        context->session->zoneMemory->blocks[4].data + context->session->wire.cursor[4] - indexBytes;
    std::memcpy(&native->indices, indices, indexBytes);
    if (framesRef)
    {
        if (smallTrans)
        {
            if (!context->Stream(4, count * 3u, 1, "trans small frames"))
                return false;
            const uint8_t *framesBytes = context->session->zoneMemory->blocks[4].data +
                                         context->session->wire.cursor[4] - count * 3u;
            unsigned __int8(*nativeFrames)[3] = static_cast<unsigned __int8(*)[3]>(
                context->Alloc(static_cast<std::size_t>(count) * 3u, 1, "trans small frames"));
            if (!context->Ok())
                return false;
            std::memcpy(nativeFrames, framesBytes, static_cast<std::size_t>(count) * 3u);
            native->frames._1 = nativeFrames;
        }
        else
        {
            if (!context->Stream(4, count * 6u, 4, "trans frames"))
                return false;
            const uint8_t *framesBytes = context->session->zoneMemory->blocks[4].data +
                                         context->session->wire.cursor[4] - count * 6u;
            uint16_t(*nativeFrames)[3] = static_cast<uint16_t(*)[3]>(
                context->Alloc(static_cast<std::size_t>(count) * 6u, 2, "trans frames"));
            if (!context->Ok())
                return false;
            for (uint32_t i = 0; i < count; ++i)
                for (uint32_t c = 0; c < 3; ++c)
                    nativeFrames[i][c] = ReadLe16(framesBytes + (i * 3u + c) * 2u);
            native->frames._2 = nativeFrames;
        }
    }
    *out = trans;
    return true;
}

// Widen one XAnimDeltaPartQuat body: 4-byte header, then (size == 0) the
// 4-byte frame0, else the 4-byte frames slot, the width-selected indices
// (inline trailing tail, same consumer contract as above), and the
// optional quat frames.
bool WidenDeltaPartQuat(XAnimDecodeContext *context, XAnimDeltaPartQuat **out)
{
    *out = nullptr;
    if (!context->Stream(4, 4, 4, "deltaquat header"))
        return false;
    const uint32_t header = context->session->wire.cursor[4] - 4;
    const uint32_t size =
        ReadLe16(context->session->zoneMemory->blocks[4].data + header);
    if (!size)
    {
        XAnimDeltaPartQuat *quat = static_cast<XAnimDeltaPartQuat *>(
            context->Alloc(sizeof(XAnimDeltaPartQuat), alignof(XAnimDeltaPartQuat),
                           "deltaquat"));
        if (!context->Ok())
            return false;
        std::memset(quat, 0, sizeof(*quat));
        if (!context->Stream(4, 4, 1, "deltaquat frame0"))
            return false;
        const uint8_t *frame0 =
            context->session->zoneMemory->blocks[4].data + context->session->wire.cursor[4] - 4;
        quat->u.frame0[0] = static_cast<__int16>(ReadLe16(frame0));
        quat->u.frame0[1] = static_cast<__int16>(ReadLe16(frame0 + 2));
        *out = quat;
        return true;
    }
    const uint32_t count = size + 1u;
    const uint32_t indexBytes = context->wideIndices ? count * 2u : count;
    const std::size_t headBytes = sizeof(XAnimDeltaPartQuat) + indexBytes;
    XAnimDeltaPartQuat *quat = static_cast<XAnimDeltaPartQuat *>(
        context->Alloc(headBytes, alignof(XAnimDeltaPartQuat), "deltaquat"));
    if (!context->Ok())
        return false;
    std::memset(quat, 0, headBytes);
    quat->size = static_cast<uint16_t>(size);
    if (!context->Stream(4, 4, 1, "deltaquat frames slot"))
        return false;
    const uint32_t framesRef = ReadLe32(context->session->zoneMemory->blocks[4].data +
                                        context->session->wire.cursor[4] - 4);
    if (!context->Stream(4, indexBytes, context->wideIndices ? 2u : 1u, "deltaquat indices"))
        return false;
    const uint8_t *indices =
        context->session->zoneMemory->blocks[4].data + context->session->wire.cursor[4] - indexBytes;
    XAnimDeltaPartQuatDataFrames *native = &quat->u.frames;
    std::memcpy(&native->indices, indices, indexBytes);
    if (framesRef)
    {
        if (!context->Stream(4, count * 4u, 4, "deltaquat frames"))
            return false;
        const uint8_t *framesBytes = context->session->zoneMemory->blocks[4].data +
                                     context->session->wire.cursor[4] - count * 4u;
        __int16(*nativeFrames)[2] = static_cast<__int16(*)[2]>(
            context->Alloc(static_cast<std::size_t>(count) * 4u, 2, "deltaquat frames"));
        if (!context->Ok())
            return false;
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t c = 0; c < 2; ++c)
                nativeFrames[i][c] =
                    static_cast<__int16>(ReadLe16(framesBytes + (i * 2u + c) * 2u));
        native->frames = nativeFrames;
    }
    *out = quat;
    return true;
}

// Copy one plain data array (byte/short/int width) when its slot is set.
// A nonzero element count with a null slot is a loud COUNT_MISMATCH: the
// reference loader would leave a null pointer beside a nonzero count for
// a later consumer to fault on.
bool WidenDataArray(XAnimDecodeContext *context, uint32_t slot, uint32_t count,
                    uint32_t elementSize, uint32_t alignment, void **out,
                    const char *stage)
{
    *out = nullptr;
    if (!slot)
    {
        if (count)
            context->Fail(RETAIL_XANIM_DECODE_COUNT_MISMATCH, stage);
        return context->Ok();
    }
    if (!count)
        return true;
    if (count > 16u * 1024u * 1024u / elementSize)
    {
        context->Fail(RETAIL_XANIM_DECODE_COUNT_MISMATCH, stage);
        return false;
    }
    if (!context->Stream(4, count * elementSize, alignment, stage))
        return false;
    const uint8_t *bytes = context->session->zoneMemory->blocks[4].data +
                           context->session->wire.cursor[4] - count * elementSize;
    if (elementSize == 1)
    {
        unsigned __int8 *native = static_cast<unsigned __int8 *>(
            context->Alloc(count, 1, stage));
        if (!context->Ok())
            return false;
        std::memcpy(native, bytes, count);
        *out = native;
        return true;
    }
    void *native = context->Alloc(static_cast<std::size_t>(count) * elementSize,
                                  elementSize == 2 ? 2 : 4, stage);
    if (!context->Ok())
        return false;
    if (elementSize == 2)
    {
        __int16 *shorts = static_cast<__int16 *>(native);
        for (uint32_t i = 0; i < count; ++i)
            shorts[i] = static_cast<__int16>(ReadLe16(bytes + i * 2u));
    }
    else
    {
        int *ints = static_cast<int *>(native);
        for (uint32_t i = 0; i < count; ++i)
            ints[i] = static_cast<int>(ReadLe32(bytes + i * 4u));
    }
    *out = native;
    return true;
}
} // namespace

// Native LP64 layout pins for the widened records (wire sizes stay frozen
// above as byte offsets; these guard the arena shapes instead).
static_assert(sizeof(XAnimNotifyInfo) == 8);
static_assert(sizeof(XAnimPartTrans) == 48);
static_assert(sizeof(XAnimDeltaPartQuat) == 24);
static_assert(sizeof(XAnimDeltaPart) == 16);

const char *RetailXAnimDecodeResultName(RetailXAnimDecodeResult result)
{
    static const char *const names[] = {
        "ok", "bad_argument", "bad_root", "bad_name", "count_mismatch",
        "index_oob", "script_table_missing", "script_string_missing",
        "truncated", "out_of_arena", "registration_failed",
    };
    return result >= RETAIL_XANIM_DECODE_OK && result <= RETAIL_XANIM_DECODE_REGISTRATION_FAILED ?
        names[result] : "invalid_result";
}

bool RetailWalkLiveLoadXAnimParts(RetailZoneLoadSession *session,
                                  FsRetailFastfileReader *reader,
                                  uint32_t headerRef, XAssetHeader *header)
{
    if (!session || !session->active || !reader || !header)
        return false;
    *header = XAssetHeader{};
    if (headerRef != kInlineRef)
    {
        Com_Printf(0, "RetailWalkLiveLoadXAnimParts: unsupported header 0x%08x\n", headerRef);
        return false;
    }
    XAnimDecodeContext context;
    context.session = session;
    context.reader = reader;
    // Per-body SL ownership (see the struct comment): unwound below if the
    // body fails after interning anything.
    uint16_t ownedSL[512]{};
    context.ownedSL = ownedSL;
    context.ownedCap = 512;

    // Temp-block root: consume 88 bytes, then snapshot them and rewind so
    // the next asset's root lands at the same temp base (the walk-only
    // reader's own temp-scope contract).
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!context.Stream(0, kRootBytes, 4, "root"))
        return false;
    uint8_t root[kRootBytes]{};
    std::memcpy(root, session->zoneMemory->blocks[0].data + bodyStart, kRootBytes);
    RetailWireBlocksRewind(&session->wire, 0, bodyStart);

    // Everything below may intern SL_* references; the single unwind at
    // the end releases exactly this body's partial work on any failure,
    // so a failed load aborts its zone with no leaked references.
    auto decodeBody = [&]() -> bool {
    const uint32_t numFrames = ReadLe16(root + kRootNumFrames);
    context.wideIndices = numFrames >= 0x100u;
    const uint32_t totalBones = root[kRootBoneCount + 9];
    const uint32_t notifyCount = root[kRootNotifyCount];
    const uint32_t indexCount = ReadLe32(root + kRootIndexCount);
    const uint32_t namesRef = ReadLe32(root + kRootNames);
    const uint32_t notifyRef = ReadLe32(root + kRootNotify);
    const uint32_t deltaRef = ReadLe32(root + kRootDeltaPart);
    const uint32_t indicesRef = ReadLe32(root + kRootIndices);

    const char *name = nullptr;
    if (!StreamXString(&context, ReadLe32(root + kRootName), &name, "name"))
        return false;
    if (!name || !name[0])
    {
        context.Fail(RETAIL_XANIM_DECODE_BAD_NAME, "name");
        return false;
    }
    context.assetName = name;

    // Bone-name table: u16 wire indices (fixed 2-byte width regardless of
    // the numframes branch -- only the dynamic/index arrays below are
    // width-selected) resolved through the zone script table into real
    // SL_* IDs.
    uint16_t *names = nullptr;
    const uint32_t namesBytes = totalBones * 2u;
    if (!namesRef)
    {
        if (totalBones)
        {
            context.Fail(RETAIL_XANIM_DECODE_COUNT_MISMATCH, "names");
            return false;
        }
    }
    else
    {
        if (!totalBones)
        {
            // Present slot with zero bones: consume nothing, bind null
            // (matches the reference loader's empty-array shape).
        }
        else
        {
            if (!context.Stream(4, namesBytes, 2, "names"))
                return false;
            const uint8_t *wire = session->zoneMemory->blocks[4].data +
                                  session->wire.cursor[4] - namesBytes;
            names = static_cast<uint16_t *>(
                context.Alloc(static_cast<std::size_t>(totalBones) * 2u, 2, "names"));
            if (!context.Ok())
                return false;
            for (uint32_t i = 0; i < totalBones; ++i)
            {
                const uint32_t wireIndex = ReadLe16(wire + i * 2u);
                const char *str = ZoneScriptString(&context, wireIndex, "names");
                if (!context.Ok())
                    return false;
                if (!InternScriptString(&context, str, &names[i], "names"))
                    return false;
            }
        }
    }

    // Notify array: fixed 8-byte wire records (u16 script index + float).
    XAnimNotifyInfo *notify = nullptr;
    if (!notifyRef)
    {
        if (notifyCount)
        {
            context.Fail(RETAIL_XANIM_DECODE_COUNT_MISMATCH, "notify");
            return false;
        }
    }
    else
    {
        if (!notifyCount)
        {
            // Present slot with zero notifies: consume nothing, bind null.
        }
        else
        {
            if (notifyCount > 1024u)
            {
                context.Fail(RETAIL_XANIM_DECODE_COUNT_MISMATCH, "notify");
                return false;
            }
            if (!context.Stream(4, notifyCount * 8u, 4, "notify"))
                return false;
            const uint8_t *wire = session->zoneMemory->blocks[4].data +
                                  session->wire.cursor[4] - notifyCount * 8u;
            notify = static_cast<XAnimNotifyInfo *>(
                context.Alloc(static_cast<std::size_t>(notifyCount) * sizeof(XAnimNotifyInfo),
                              alignof(XAnimNotifyInfo), "notify"));
            if (!context.Ok())
                return false;
            for (uint32_t i = 0; i < notifyCount; ++i)
            {
                const uint32_t wireIndex = ReadLe16(wire + i * 8u);
                const char *str = ZoneScriptString(&context, wireIndex, "notify");
                if (!context.Ok())
                    return false;
                if (!InternScriptString(&context, str, &notify[i].name, "notify"))
                    return false;
                notify[i].time = ReadLeFloat(wire + i * 8u + 4);
            }
        }
    }

    // Delta part: two pointer slots widened with the width branch.
    XAnimDeltaPart *deltaPart = nullptr;
    if (deltaRef)
    {
        if (!context.Stream(4, 8, 4, "delta"))
            return false;
        const uint8_t *deltaWire = session->zoneMemory->blocks[4].data +
                                   session->wire.cursor[4] - 8;
        const uint32_t transRef = ReadLe32(deltaWire);
        const uint32_t quatRef = ReadLe32(deltaWire + 4);
        deltaPart = static_cast<XAnimDeltaPart *>(
            context.Alloc(sizeof(XAnimDeltaPart), alignof(XAnimDeltaPart), "delta"));
        if (!context.Ok())
            return false;
        std::memset(deltaPart, 0, sizeof(*deltaPart));
        if (transRef && !WidenPartTrans(&context, &deltaPart->trans))
            return false;
        if (quatRef && !WidenDeltaPartQuat(&context, &deltaPart->quat))
            return false;
    }

    // Six data arrays, in reference field order.
    void *dataByte = nullptr;
    void *dataShort = nullptr;
    void *dataInt = nullptr;
    void *randomDataShort = nullptr;
    void *randomDataByte = nullptr;
    void *randomDataInt = nullptr;
    if (!WidenDataArray(&context, ReadLe32(root + kRootDataByte), ReadLe16(root + kRootDataByteCount),
                        1, 1, &dataByte, "dataByte") ||
        !WidenDataArray(&context, ReadLe32(root + kRootDataShort),
                        ReadLe16(root + kRootDataShortCount), 2, 2, &dataShort, "dataShort") ||
        !WidenDataArray(&context, ReadLe32(root + kRootDataInt), ReadLe16(root + kRootDataIntCount),
                        4, 4, &dataInt, "dataInt") ||
        !WidenDataArray(&context, ReadLe32(root + kRootRandomDataShort),
                        ReadLe32(root + kRootRandomDataShortCount), 2, 2, &randomDataShort,
                        "randomDataShort") ||
        !WidenDataArray(&context, ReadLe32(root + kRootRandomDataByte),
                        ReadLe16(root + kRootRandomDataByteCount), 1, 1, &randomDataByte,
                        "randomDataByte") ||
        !WidenDataArray(&context, ReadLe32(root + kRootRandomDataInt),
                        ReadLe16(root + kRootRandomDataIntCount), 4, 4, &randomDataInt,
                        "randomDataInt"))
        return false;

    // Top-level indices union, width-selected like every dynamic index.
    XAnimIndices indices{};
    if (!indicesRef)
    {
        if (indexCount)
        {
            context.Fail(RETAIL_XANIM_DECODE_COUNT_MISMATCH, "indices");
            return false;
        }
    }
    else if (indexCount)
    {
        const uint32_t indexBytes = context.wideIndices ? indexCount * 2u : indexCount;
        if (!context.Stream(4, indexBytes, context.wideIndices ? 2u : 1u, "indices"))
            return false;
        const uint8_t *wire = session->zoneMemory->blocks[4].data +
                              session->wire.cursor[4] - indexBytes;
        if (context.wideIndices)
        {
            uint16_t *nativeIndices = static_cast<uint16_t *>(
                context.Alloc(static_cast<std::size_t>(indexCount) * 2u, 2, "indices"));
            if (!context.Ok())
                return false;
            for (uint32_t i = 0; i < indexCount; ++i)
                nativeIndices[i] = ReadLe16(wire + i * 2u);
            indices._2 = nativeIndices;
        }
        else
        {
            unsigned __int8 *nativeIndices = static_cast<unsigned __int8 *>(
                context.Alloc(indexCount, 1, "indices"));
            if (!context.Ok())
                return false;
            std::memcpy(nativeIndices, wire, indexCount);
            indices._1 = nativeIndices;
        }
    }

    // Transaction-owned native root, filled field by field from the
    // snapshotted wire bytes. The shared registry is untouched until the
    // commit below, so any failure above rolls back by discarding this.
    XAnimParts *parts = static_cast<XAnimParts *>(
        context.Alloc(sizeof(XAnimParts), alignof(XAnimParts), "parts"));
    if (!context.Ok())
        return false;
    std::memset(parts, 0, sizeof(*parts));
    parts->name = name;
    parts->dataByteCount = ReadLe16(root + kRootDataByteCount);
    parts->dataShortCount = ReadLe16(root + kRootDataShortCount);
    parts->dataIntCount = ReadLe16(root + kRootDataIntCount);
    parts->randomDataByteCount = ReadLe16(root + kRootRandomDataByteCount);
    parts->randomDataIntCount = ReadLe16(root + kRootRandomDataIntCount);
    parts->numframes = static_cast<uint16_t>(numFrames);
    parts->bLoop = root[kRootLoop] != 0;
    parts->bDelta = root[kRootDelta] != 0;
    for (uint32_t i = 0; i < 10; ++i)
        parts->boneCount[i] = root[kRootBoneCount + i];
    parts->notifyCount = static_cast<unsigned __int8>(notifyCount);
    parts->assetType = static_cast<unsigned __int8>(root[kRootAssetType]);
    parts->isDefault = root[kRootIsDefault] != 0;
    parts->randomDataShortCount = ReadLe32(root + kRootRandomDataShortCount);
    parts->indexCount = indexCount;
    parts->framerate = ReadLeFloat(root + kRootFramerate);
    parts->frequency = ReadLeFloat(root + kRootFrequency);
    parts->names = names;
    parts->dataByte = static_cast<unsigned __int8 *>(dataByte);
    parts->dataShort = static_cast<__int16 *>(dataShort);
    parts->dataInt = static_cast<int *>(dataInt);
    parts->randomDataShort = static_cast<__int16 *>(randomDataShort);
    parts->randomDataByte = static_cast<unsigned __int8 *>(randomDataByte);
    parts->randomDataInt = static_cast<int *>(randomDataInt);
    parts->indices = indices;
    parts->notify = notify;
    parts->deltaPart = deltaPart;

    // Atomic commit through the existing real database owner. The pooled
    // header is required: callers must use it, never the transaction.
    XAssetHeader tx{};
    tx.parts = parts;
    Load_XAnimPartsAsset(&tx);
    if (!tx.parts)
    {
        context.Fail(RETAIL_XANIM_DECODE_REGISTRATION_FAILED, "register");
        return false;
    }
    RETAIL_ASSET_TRACE(0, "RetailWalkLiveLoadXAnimParts: '%s' frames=%u bones=%u notifies=%u %s ok\n",
                       name, numFrames, totalBones, notifyCount,
                       context.wideIndices ? "wide" : "narrow");
    *header = tx;
    return true;
    };
    if (!decodeBody())
    {
        for (uint32_t i = 0; i < context.ownedCount; ++i)
            SL_RemoveRefToString(context.ownedSL[i]);
        context.ownedCount = 0;
        return false;
    }
    return true;
}
