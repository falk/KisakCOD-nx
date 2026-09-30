// F4 executable proof: one canonical four-byte retail reference contract.
// The fixture deliberately exercises a block-boundary carry, exact span
// validation, explicit block policy, and the no-publish/no-cursor mutation
// guarantees required by the native widening seam.
#include <cstdio>
#include <cstring>

#include <database/db_retail_wire.h>

// xanim.h carries the engine's static trace dispatch table even though this
// proof only exercises the wire cursor.
void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}

namespace
{
bool Check(bool condition, const char *stage)
{
    if (!condition)
        std::fprintf(stderr, "FAIL:RETAIL_WIRE_TOKEN_PROOF stage=%s\n", stage);
    return condition;
}
}

int main()
{
    uint8_t block0[16] = {};
    uint8_t block3[8] = {};
    uint8_t block4[32] = {};
    std::memcpy(block4 + 4, "wire", 5);
    XZoneMemory zone{};
    zone.blocks[0] = {block0, sizeof(block0)};
    zone.blocks[3] = {block3, sizeof(block3)};
    zone.blocks[4] = {block4, sizeof(block4)};

    RetailWireBlocks blocks{};
    if (!Check(RetailWireBlocksInit(&blocks, &zone), "init"))
        return 1;
    blocks.cursor[4] = 9;

    RetailWireToken token{0xdeadbeefu, 99, 99, RETAIL_WIRE_TOKEN_OFFSET};
    if (!Check(RetailWireTokenDecode(&blocks, {0}, 0, 1u << 4, &token) &&
                   token.kind == RETAIL_WIRE_TOKEN_NULL && token.encoded == 0,
               "null") ||
        !Check(RetailWireTokenDecode(&blocks, {0xffffffffu}, 32, 0, &token) &&
                   token.kind == RETAIL_WIRE_TOKEN_INLINE,
               "inline") ||
        !Check(RetailWireTokenDecode(&blocks, {0xfffffffeu}, 32, 0, &token) &&
                   token.kind == RETAIL_WIRE_TOKEN_INSERT,
               "special"))
        return 1;

    RetailPtr32 encoded{};
    if (!Check(RetailWireTokenEncode(4, 0x0fffffffu, &encoded) && encoded.encoded == 0x50000000u,
               "encode_carry") ||
        !Check((zone.blocks[4].size = 0x10000000u,
                RetailWireTokenDecode(&blocks, encoded, 0, 1u << 4, &token)) &&
                   token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4 &&
                   token.offset == 0x0fffffffu,
               "carry"))
        return 1;

    zone.blocks[4].size = sizeof(block4);

    if (!Check(RetailWireTokenEncode(4, 4, &encoded) &&
                   RetailWireTokenDecode(&blocks, encoded, 5, 1u << 4, &token) &&
                   token.block == 4 && token.offset == 4,
               "span") ||
        !Check(RetailWireTokenRead(&blocks, encoded, 5, 1u << 4, block0) &&
                   !std::memcmp(block0, "wire\0", 5) && blocks.cursor[4] == 9,
               "sync"))
        return 1;

    // XAssetList itself sits outside the nine stream blocks, but its inline
    // directory is block-4 wire data.  Prove this reader owns the alignment
    // and exact cursor advance before any native decoder can consume headers.
    blocks.cursor[4] = 1;
    block4[4] = 3;  block4[5] = 0; block4[6] = 0; block4[7] = 0;
    block4[8] = 0xff; block4[9] = 0xff; block4[10] = 0xff; block4[11] = 0xff;
    block4[12] = 20; block4[13] = 0; block4[14] = 0; block4[15] = 0;
    block4[16] = 0xfe; block4[17] = 0xff; block4[18] = 0xff; block4[19] = 0xff;
    RetailWireAssetRecord directory[2] = {};
    if (!Check(RetailWireReadAssetDirectory(&blocks, 2, 0xffffffffu, directory, 2) ==
                   RETAIL_WIRE_DIRECTORY_OK && blocks.cursor[4] == 20 &&
                   directory[0].type == 3 && directory[0].header == 0xffffffffu &&
                   directory[1].type == 20 && directory[1].header == 0xfffffffeu,
               "directory") ||
        !Check(RetailWireReadAssetDirectory(&blocks, 3, 0xffffffffu, directory, 2) ==
                   RETAIL_WIRE_DIRECTORY_TOO_MANY_ASSETS && blocks.cursor[4] == 20,
               "directory_atomic"))
        return 1;

    RetailWireToken before = {0x12345678u, 7, 8, RETAIL_WIRE_TOKEN_INSERT};
    token = before;
    if (!Check(!RetailWireTokenDecode(&blocks, {0x30000001u}, 1, 1u << 4, &token) &&
                   !std::memcmp(&token, &before, sizeof(token)),
               "wrong_block"))
        return 1;
    token = before;
    if (!Check(!RetailWireTokenDecode(&blocks, {0x40000020u}, 2, 1u << 4, &token) &&
                   !std::memcmp(&token, &before, sizeof(token)),
               "overflow"))
        return 1;
    if (!Check(!RetailWireTokenDecode(&blocks, {0x40000001u}, 1, 1u << 9, &token),
               "policy"))
        return 1;
    if (!Check(!RetailWireTokenRead(&blocks, {0xffffffffu}, 1, 1u << 4, block0), "special_read_rejected"))
        return 1;

    // Alignment near UINT32_MAX must fail, not wrap the cursor back to 0 and
    // resume at the start of the block (the unchecked round-up did). The
    // fake block size lets the aligned-to-zero result "fit" if it wrapped.
    zone.blocks[4].size = UINT32_MAX;
    blocks.activeBlock = 4;
    blocks.cursor[4] = UINT32_MAX - 1u;
    if (!Check(!RetailWireBlocksAlign(&blocks, 4) && blocks.cursor[4] == UINT32_MAX - 1u,
               "align_wrap") ||
        !Check(RetailWireBlocksAlloc(&blocks, 4, 0, 16) == nullptr &&
                   blocks.cursor[4] == UINT32_MAX - 1u,
               "alloc_align_wrap") ||
        !Check(RetailWireReadAssetDirectory(&blocks, 0, 0xffffffffu, directory, 2) ==
                   RETAIL_WIRE_DIRECTORY_TRUNCATED && blocks.cursor[4] == UINT32_MAX - 1u,
               "directory_align_wrap"))
        return 1;
    zone.blocks[4].size = sizeof(block4);
    uint8_t arenaMemory[64];
    RetailNativeArena arena{};
    if (!Check(RetailNativeArenaInit(&arena, arenaMemory, sizeof(arenaMemory)), "arena_init"))
        return 1;
    arena.used = SIZE_MAX - 2u;
    if (!Check(RetailNativeArenaAlloc(&arena, 1, 8) == nullptr && arena.used == SIZE_MAX - 2u,
               "arena_align_wrap"))
        return 1;

    std::puts("PASS:RETAIL_WIRE_TOKEN_PROOF null=1 inline=1 special=1 carry=1 span=1 wrong_block=1 overflow=1 atomic=1 sync=1 directory=1 align_wrap=1");
    return 0;
}
