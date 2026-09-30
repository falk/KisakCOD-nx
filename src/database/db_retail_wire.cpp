#include "db_retail_wire.h"

#include <cstdint>
#include <cstring>

namespace
{
constexpr uint32_t kInlineReference = 0xffffffffu;
constexpr uint32_t kInsertReference = 0xfffffffeu;
constexpr uint32_t kOffsetMask = 0x0fffffffu;

bool ValidBlock(const RetailWireBlocks *blocks, uint32_t block)
{
    return blocks != nullptr && blocks->zone != nullptr && block < 9;
}

bool ValidBlockMask(uint32_t allowedBlockMask)
{
    return (allowedBlockMask & ~((1u << 9) - 1u)) == 0;
}

bool Fits(const XBlock &storage, uint32_t cursor, uint32_t bytes)
{
    return cursor <= storage.size && bytes <= storage.size - cursor;
}

// Checked round-up: a cursor within alignment-1 of UINT32_MAX would wrap to
// a small value and resume at the start of the block, so it fails instead.
// `alignment` is a nonzero power of two (callers validate it).
bool AlignCursor(uint32_t cursor, uint32_t alignment, uint32_t *aligned)
{
    if (cursor > UINT32_MAX - (alignment - 1u))
        return false;
    *aligned = (cursor + alignment - 1u) & ~(alignment - 1u);
    return true;
}

uint32_t ReadLe32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

} // namespace

bool RetailWireBlocksInit(RetailWireBlocks *blocks, XZoneMemory *zone)
{
    if (!blocks || !zone)
        return false;
    std::memset(blocks, 0, sizeof(*blocks));
    blocks->zone = zone;
    return true;
}

bool RetailWireBlocksPush(RetailWireBlocks *blocks, uint32_t block)
{
    if (!ValidBlock(blocks, block) || blocks->stackIndex == 32)
        return false;
    blocks->stack[blocks->stackIndex++] = blocks->activeBlock;
    blocks->activeBlock = block;
    return true;
}

bool RetailWireBlocksPop(RetailWireBlocks *blocks)
{
    if (!blocks || blocks->stackIndex == 0)
        return false;
    blocks->activeBlock = blocks->stack[--blocks->stackIndex];
    return true;
}

bool RetailWireBlocksAlign(RetailWireBlocks *blocks, uint32_t alignment)
{
    if (!blocks || alignment == 0 || (alignment & (alignment - 1u)) != 0 ||
        !ValidBlock(blocks, blocks->activeBlock))
        return false;
    const uint32_t block = blocks->activeBlock;
    uint32_t aligned = 0;
    if (!AlignCursor(blocks->cursor[block], alignment, &aligned) ||
        !Fits(blocks->zone->blocks[block], aligned, 0))
        return false;
    blocks->cursor[block] = aligned;
    return true;
}

bool RetailWireBlocksRead(RetailWireBlocks *blocks, uint32_t block, void *out, uint32_t bytes)
{
    if (!ValidBlock(blocks, block) || (!out && bytes != 0) ||
        !Fits(blocks->zone->blocks[block], blocks->cursor[block], bytes))
        return false;
    if (bytes)
        std::memcpy(out, blocks->zone->blocks[block].data + blocks->cursor[block], bytes);
    blocks->cursor[block] += bytes;
    return true;
}

uint8_t *RetailWireBlocksAlloc(RetailWireBlocks *blocks, uint32_t block, uint32_t bytes,
                                uint32_t alignment)
{
    if (!ValidBlock(blocks, block) || alignment == 0 || (alignment & (alignment - 1u)) != 0)
        return nullptr;
    uint32_t aligned = 0;
    if (!AlignCursor(blocks->cursor[block], alignment, &aligned) ||
        !Fits(blocks->zone->blocks[block], aligned, bytes))
        return nullptr;
    blocks->cursor[block] = aligned + bytes;
    return blocks->zone->blocks[block].data + aligned;
}

void RetailWireBlocksRewind(RetailWireBlocks *blocks, uint32_t block, uint32_t cursor)
{
    if (ValidBlock(blocks, block) && cursor <= blocks->zone->blocks[block].size)
        blocks->cursor[block] = cursor;
}

RetailWireDirectoryResult RetailWireReadAssetDirectory(RetailWireBlocks *blocks,
                                                       uint32_t assetCount,
                                                       uint32_t assetsReference,
                                                       RetailWireAssetRecord *records,
                                                       uint32_t capacity)
{
    if (!blocks || !blocks->zone || assetsReference != kInlineReference)
        return RETAIL_WIRE_DIRECTORY_BAD_REFERENCE;
    if (assetCount > capacity || (assetCount && !records))
        return RETAIL_WIRE_DIRECTORY_TOO_MANY_ASSETS;
    if (!RetailWireBlocksPush(blocks, 4))
        return RETAIL_WIRE_DIRECTORY_TRUNCATED;

    uint32_t aligned = 0;
    const XBlock &block = blocks->zone->blocks[4];
    const uint64_t byteCount = static_cast<uint64_t>(assetCount) * 8u;
    if (!AlignCursor(blocks->cursor[4], 4, &aligned) || byteCount > UINT32_MAX ||
        (assetCount && !block.data) ||
        !Fits(block, aligned, static_cast<uint32_t>(byteCount)))
    {
        RetailWireBlocksPop(blocks);
        return RETAIL_WIRE_DIRECTORY_TRUNCATED;
    }
    if (!assetCount)
    {
        blocks->cursor[4] = aligned;
        RetailWireBlocksPop(blocks);
        return RETAIL_WIRE_DIRECTORY_OK;
    }

    // Validate the complete span before moving a cursor or publishing an
    // entry.  That prevents a short directory from leaving a partially
    // widened asset sequence indistinguishable from a complete one.
    const uint8_t *wire = block.data + aligned;
    for (uint32_t i = 0; i < assetCount; ++i)
    {
        records[i].type = ReadLe32(wire + i * 8u);
        records[i].header = ReadLe32(wire + i * 8u + 4u);
    }
    blocks->cursor[4] = aligned + static_cast<uint32_t>(byteCount);
    RetailWireBlocksPop(blocks);
    return RETAIL_WIRE_DIRECTORY_OK;
}

bool RetailWireTokenDecodeBlocks(const XBlock blocks[9], RetailPtr32 encoded,
                                 uint32_t span, uint32_t allowedBlockMask,
                                 RetailWireToken *out)
{
    if (!blocks || !out || !ValidBlockMask(allowedBlockMask))
        return false;

    RetailWireToken decoded{};
    decoded.encoded = encoded.encoded;
    if (encoded.encoded == 0)
        decoded.kind = RETAIL_WIRE_TOKEN_NULL;
    else if (encoded.encoded == kInlineReference)
        decoded.kind = RETAIL_WIRE_TOKEN_INLINE;
    else if (encoded.encoded == kInsertReference)
        decoded.kind = RETAIL_WIRE_TOKEN_INSERT;
    else
    {
        const uint32_t adjusted = encoded.encoded - 1u;
        decoded.block = adjusted >> 28;
        decoded.offset = adjusted & kOffsetMask;
        if (decoded.block >= 9 || !(allowedBlockMask & (1u << decoded.block)))
            return false;
        const XBlock &storage = blocks[decoded.block];
        if (decoded.offset > storage.size || span > storage.size - decoded.offset ||
            (span != 0 && !storage.data))
            return false;
        decoded.kind = RETAIL_WIRE_TOKEN_OFFSET;
    }

    // Null and inline/insert tokens are classifications, not locations.  A
    // caller may validate their form here, but no block/span is consumed.
    *out = decoded;
    return true;
}

bool RetailWireTokenDecode(const RetailWireBlocks *blocks, RetailPtr32 encoded,
                           uint32_t span, uint32_t allowedBlockMask,
                           RetailWireToken *out)
{
    if (!blocks || !blocks->zone)
        return false;
    return RetailWireTokenDecodeBlocks(blocks->zone->blocks, encoded, span,
                                       allowedBlockMask, out);
}

bool RetailWireTokenRead(const RetailWireBlocks *blocks, RetailPtr32 encoded,
                         uint32_t span, uint32_t allowedBlockMask, void *out)
{
    if (!out && span != 0)
        return false;
    RetailWireToken token{};
    if (!RetailWireTokenDecode(blocks, encoded, span, allowedBlockMask, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET)
        return false;
    if (span)
        std::memcpy(out, blocks->zone->blocks[token.block].data + token.offset, span);
    return true;
}

bool RetailWirePoolIndexDecode(RetailPtr32 encoded, uint32_t poolCapacity,
                               uint32_t *poolIndex)
{
    if (!poolIndex || !encoded.encoded || encoded.encoded == kInlineReference ||
        encoded.encoded == kInsertReference)
        return false;
    const uint32_t adjusted = encoded.encoded - 1u;
    if ((adjusted >> 28) != 15u)
        return false;
    const uint32_t index = adjusted & kOffsetMask;
    if (index >= poolCapacity)
        return false;
    *poolIndex = index;
    return true;
}

bool RetailWireTokenEncode(uint32_t block, uint32_t offset, RetailPtr32 *encoded)
{
    if (!encoded || block >= 9 || offset > kOffsetMask)
        return false;
    const uint64_t value = (static_cast<uint64_t>(block) << 28) + offset + 1u;
    if (value > UINT32_MAX)
        return false;
    encoded->encoded = static_cast<uint32_t>(value);
    return true;
}

bool RetailNativeArenaInit(RetailNativeArena *arena, void *memory, std::size_t bytes)
{
    if (!arena || !memory || bytes == 0)
        return false;
    std::memset(arena, 0, sizeof(*arena));
    arena->data = static_cast<uint8_t *>(memory);
    arena->size = bytes;
    return true;
}

void *RetailNativeArenaAlloc(RetailNativeArena *arena, std::size_t bytes, std::size_t alignment)
{
    if (!arena || !arena->data || bytes == 0 || alignment == 0 ||
        (alignment & (alignment - 1u)) != 0)
        return nullptr;
    // Same checked round-up as the wire cursors: never wrap `used`.
    if (arena->used > SIZE_MAX - (alignment - 1u))
        return nullptr;
    const std::size_t aligned = (arena->used + alignment - 1u) & ~(alignment - 1u);
    if (aligned > arena->size || bytes > arena->size - aligned)
        return nullptr;
    arena->used = aligned + bytes;
    if (arena->used > arena->peak)
        arena->peak = arena->used;
    return arena->data + aligned;
}

void RetailNativeArenaDestroy(RetailNativeArena *arena)
{
    if (!arena)
        return;
    // The backing store is part of the zone's PMem allocation.  The registry
    // releases it after all asset entries have been removed or restored.
    std::memset(arena, 0, sizeof(*arena));
}
