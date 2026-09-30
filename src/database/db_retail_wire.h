#pragma once

#include <cstddef>
#include <cstdint>

#include <xanim/xanim.h>

// The retail fastfile is a 32-bit wire format.  This is the only production
// cursor over its nine blocks; native consumers never see these cursors.
struct RetailWireBlocks
{
    XZoneMemory *zone;
    uint32_t cursor[9];
    uint32_t activeBlock;
    uint32_t stackIndex;
    uint32_t stack[32];
};

// Exact storage for a serialized reference.  It is deliberately distinct
// from a native pointer: field-specific code must classify it before it can
// become an offset, an insert slot, or inline data.
struct RetailPtr32
{
    uint32_t encoded;
};
static_assert(sizeof(RetailPtr32) == 4, "retail references are four-byte wire tokens");

// A retail pointer is a four-byte token, never a native pointer.  The token
// stores a one-based byte offset: subtract one before splitting the block
// nibble and low-28-bit offset.  Keep the classification separate from the
// offset so the two sentinel values can never accidentally be dereferenced.
enum RetailWireTokenKind
{
    RETAIL_WIRE_TOKEN_NULL,
    RETAIL_WIRE_TOKEN_INLINE,
    RETAIL_WIRE_TOKEN_INSERT,
    RETAIL_WIRE_TOKEN_OFFSET,
};

struct RetailWireToken
{
    uint32_t encoded;
    uint32_t block;
    uint32_t offset;
    RetailWireTokenKind kind;
};

// The XAsset directory is a serialized block-4 sequence.  These are still
// wire records; native registry ownership begins only after a family decoder
// widens a record through RetailZoneLoadSession.
struct RetailWireAssetRecord
{
    uint32_t type;
    uint32_t header;
};

enum RetailWireDirectoryResult
{
    RETAIL_WIRE_DIRECTORY_OK,
    RETAIL_WIRE_DIRECTORY_BAD_REFERENCE,
    RETAIL_WIRE_DIRECTORY_TOO_MANY_ASSETS,
    RETAIL_WIRE_DIRECTORY_TRUNCATED,
};

bool RetailWireBlocksInit(RetailWireBlocks *blocks, XZoneMemory *zone);
bool RetailWireBlocksPush(RetailWireBlocks *blocks, uint32_t block);
bool RetailWireBlocksPop(RetailWireBlocks *blocks);
bool RetailWireBlocksAlign(RetailWireBlocks *blocks, uint32_t alignment);
bool RetailWireBlocksRead(RetailWireBlocks *blocks, uint32_t block, void *out, uint32_t bytes);
uint8_t *RetailWireBlocksAlloc(RetailWireBlocks *blocks, uint32_t block, uint32_t bytes,
                               uint32_t alignment);
void RetailWireBlocksRewind(RetailWireBlocks *blocks, uint32_t block, uint32_t cursor);

// Consume the inline XAsset directory in its real serialized order.  The
// caller supplies the XAssetList scalar fields read outside the nine blocks;
// this reader owns the block-4 alignment, bytes, and cursor accounting.
RetailWireDirectoryResult RetailWireReadAssetDirectory(RetailWireBlocks *blocks,
                                                       uint32_t assetCount,
                                                       uint32_t assetsReference,
                                                       RetailWireAssetRecord *records,
                                                       uint32_t capacity);

// Decode and validate one encoded reference without changing a cursor or
// publishing partial output.  allowedBlockMask is an explicit policy (bits
// 0..8 correspond to the nine wire blocks); callers must not infer policy
// from the token itself.  span is checked with subtraction, so an overflowing
// requested extent is rejected before any source pointer is formed.
bool RetailWireTokenDecode(const RetailWireBlocks *blocks, RetailPtr32 encoded,
                           uint32_t span, uint32_t allowedBlockMask,
                           RetailWireToken *out);

// Same contract for a non-cursor block view.  The canonical bootstrap still
// receives blocks from the FS-owned reader, so it uses this adapter without
// creating a second decoder or pretending those blocks are zone cursors.
bool RetailWireTokenDecodeBlocks(const XBlock blocks[9], RetailPtr32 encoded,
                                 uint32_t span, uint32_t allowedBlockMask,
                                 RetailWireToken *out);

// Read a validated token at its encoded location.  This is intentionally
// non-cursor-consuming: token references point at already-streamed data and
// must not perturb the serialized-order walk.
bool RetailWireTokenRead(const RetailWireBlocks *blocks, RetailPtr32 encoded,
                         uint32_t span, uint32_t allowedBlockMask, void *out);

// Canonical inverse used by deterministic fixtures and bounds tests.  It
// preserves the one-based encoding, including the carry when offset is the
// last low-28-bit byte of a block.
bool RetailWireTokenEncode(uint32_t block, uint32_t offset, RetailPtr32 *encoded);

// Pseudo-block 15 is the Android DB_AddXAsset pool table, not one of the nine
// streamed blocks.  Keep its field-specific identity decode in this module so
// callers do not reintroduce raw block/mask arithmetic for pooled handles.
bool RetailWirePoolIndexDecode(RetailPtr32 encoded, uint32_t poolCapacity,
                               uint32_t *poolIndex);

// Widened pointer-bearing records live here, never in a temporary wire block.
// Chunks are stable so an allocation cannot invalidate an earlier native
// pointer.  The fixed chunk table also keeps this safe across database
// setjmp/longjmp boundaries: callers explicitly destroy it at the zone seam.
struct RetailNativeArena
{
    uint8_t *data;
    std::size_t size;
    std::size_t used;
    std::size_t peak;
};

bool RetailNativeArenaInit(RetailNativeArena *arena, void *memory, std::size_t bytes);
void *RetailNativeArenaAlloc(RetailNativeArena *arena, std::size_t bytes, std::size_t alignment);
void RetailNativeArenaDestroy(RetailNativeArena *arena);
