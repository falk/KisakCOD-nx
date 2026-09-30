// Bounded stream proof. The filesystem and zlib reader are production
// code; only the zone allocation/end seam is mocked so this test can inspect
// the bytes after the walker tears the session down.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <database/db_retail_walk.h>
#include <database/db_retail_zone.h>

void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}

// This walk-only binary never passes ReadRetailSndAliasListBody's
// aliasNameOffsetsOut (the live B2 widener's private table). The production
// arena allocator that path uses lives in db_retail_zone.cpp, which this
// proof deliberately does not link; fail loudly if that live path is ever
// reached instead of silently satisfying it with a null buffer.
void *RetailZoneLoadSessionAlloc(RetailZoneLoadSession *, std::size_t, std::size_t)
{
    std::fprintf(stderr, "FAIL:RETAIL_BOOT_WALK_PROOF stage=unexpected_live_arena_alloc\n");
    std::abort();
}

namespace
{
XZoneMemory g_zone;
uint8_t *g_blocks[9]{};
uint8_t g_arena[4096];
uint32_t g_beginCount;
uint32_t g_endCount;
uint32_t g_registerCount;
uint32_t g_lastCursor0;
uint32_t g_lastCursor4;

bool Check(bool value, const char *stage)
{
    if (!value)
        std::fprintf(stderr, "FAIL:RETAIL_BOOT_WALK_PROOF stage=%s\n", stage);
    return value;
}
}

bool RetailZoneLoadSessionBegin(RetailZoneLoadSession *session, const char *, int32_t,
                                const uint32_t sizes[9], std::size_t nativeArenaBytes)
{
    if (!session || !sizes || nativeArenaBytes > sizeof(g_arena))
        return false;
    std::memset(&g_zone, 0, sizeof(g_zone));
    for (uint32_t block = 0; block < 9; ++block)
    {
        std::free(g_blocks[block]);
        g_blocks[block] = nullptr;
        if (!sizes[block])
            continue;
        // Heap-sized like the zone-stub doubles so map
        // fixtures are not capped at 256 bytes; bytes stay inspectable
        // through g_zone after teardown exactly as before.
        g_blocks[block] = static_cast<uint8_t *>(std::calloc(1, sizes[block]));
        if (!g_blocks[block])
        {
            for (uint32_t done = 0; done < block; ++done)
            {
                std::free(g_blocks[done]);
                g_blocks[done] = nullptr;
            }
            return false;
        }
        g_zone.blocks[block] = {g_blocks[block], sizes[block]};
    }
    ++g_beginCount;
    std::memset(session, 0, sizeof(*session));
    session->zoneIndex = 1;
    session->zoneMemory = &g_zone;
    session->active = RetailWireBlocksInit(&session->wire, &g_zone) &&
                      RetailNativeArenaInit(&session->arena, g_arena, nativeArenaBytes);
    return session->active;
}

bool RetailZoneLoadSessionAbort(RetailZoneLoadSession *session)
{
    if (!session || !session->active || session->zoneIndex != 1)
        return false;
    ++g_endCount;
    g_lastCursor0 = session->wire.cursor[0];
    g_lastCursor4 = session->wire.cursor[4];
    RetailNativeArenaDestroy(&session->arena);
    std::memset(session, 0, sizeof(*session));
    return true;
}

int main(int argc, char **argv)
{
    // Retain the xanim static-dispatch owners pulled by the production
    // database headers when this proof links with --gc-sections.
    volatile auto keepCgTrace = &CG_TraceCapsule;
    volatile auto keepGTrace = &G_TraceCapsule;
    (void)keepCgTrace;
    (void)keepGTrace;
    if (!Check(argc == 2, "argument"))
        return 1;
    FS_InitRetailSource(argv[1]);
    RetailWalkDirectoryRecord records[2]{};
    RetailWalkDirectoryResult result{};
    const RetailWalkDirectoryResultCode code = RetailWalkFastfileDirectory(
        "zone/english/code_post_gfx.ff", records, 2, &result);
    if (!Check(code == RETAIL_WALK_OK && result.code == RETAIL_WALK_OK,
               "walk") ||
        !Check(result.assetCount == 2 && result.recordedCount == 2 &&
                   result.deferredCount == 2 && result.noStreamCount == 0,
               "counts") ||
        !Check(records[0].ordinal == 0 && records[0].type == 4u &&
                   records[0].header == 0x11111111u &&
                   records[0].state == RETAIL_WALK_DEFERRED,
               "first") ||
        !Check(records[1].ordinal == 1 && records[1].type == 6u &&
                   records[1].header == 0x22222222u &&
                   records[1].state == RETAIL_WALK_DEFERRED,
               "second") ||
        !Check(g_beginCount == 1 && g_endCount == 1 && g_registerCount == 0,
               "lifecycle") ||
        !Check(g_zone.blocks[0].data[0] == 0 &&
                   !std::memcmp(g_zone.blocks[4].data + 8, "one\0two\0", 8),
               "script_strings") ||
        !Check(g_zone.blocks[4].data[16] == 4u &&
                   g_zone.blocks[4].data[24] == 6u,
               "aligned_directory"))
        return 1;

    RetailWalkDirectoryRecord techniqueRecord{};
    RetailWalkDirectoryResult techniqueResult{};
    const RetailWalkDirectoryResultCode techniqueCode = RetailWalkFastfileDirectory(
        "zone/english/technique.ff", &techniqueRecord, 1, &techniqueResult);
    if (!Check(techniqueCode == RETAIL_WALK_OK && techniqueResult.code == RETAIL_WALK_OK,
               "technique_walk") ||
        !Check(techniqueResult.assetCount == 1 && techniqueResult.recordedCount == 1 &&
                   techniqueResult.walkedDeferredCount == 1 &&
                   techniqueResult.deferredCount == 0,
               "technique_counts") ||
        !Check(techniqueRecord.state == RETAIL_WALK_WALKED_DEFERRED,
               "technique_state") ||
        !Check(techniqueRecord.bodyBytes == 148 && techniqueRecord.nameBytes == 6,
               "technique_root_body") ||
        !Check(techniqueRecord.nestedReferenceCount == 2,
               "technique_references") ||
        !Check(techniqueRecord.nestedBodyBytes == 28,
               "technique_nested_bytes") ||
        !Check(techniqueRecord.nestedPassCount == 1,
               "technique_nested_passes") ||
        !Check(g_beginCount == 2 && g_endCount == 2 && g_registerCount == 0 &&
                   g_zone.blocks[0].data[0] == 0xff &&
                   g_zone.blocks[0].data[12] == 0xff &&
                   !std::memcmp(g_zone.blocks[4].data + 8, "ts/2d\0", 6) &&
                   g_zone.blocks[4].data[16] == 0xff &&
                   g_zone.blocks[4].data[22] == 1 &&
                   !std::memcmp(g_zone.blocks[4].data + 44, "pass/0\0", 7),
               "technique_stream"))
        return 1;
    RetailWalkDirectoryRecord materialRecord{};
    RetailWalkDirectoryResult materialResult{};
    const RetailWalkDirectoryResultCode materialCode = RetailWalkFastfileDirectory(
        "zone/english/material.ff", &materialRecord, 1, &materialResult);
    if (!Check(materialCode == RETAIL_WALK_OK && materialResult.code == RETAIL_WALK_OK,
               "material_walk") ||
        !Check(materialResult.assetCount == 1 && materialResult.recordedCount == 1 &&
                   materialResult.walkedDeferredCount == 1 &&
                   materialResult.walkedMaterialCount == 1 &&
                   materialResult.walkedMaterialTextureCount == 1 &&
                   materialResult.walkedImageCount == 1 &&
                   materialResult.walkedImagePayloadBytes == 4 &&
                   materialResult.deferredCount == 0,
               "material_counts") ||
        !Check(materialRecord.type == 4u && materialRecord.header == 0xffffffffu &&
                   materialRecord.state == RETAIL_WALK_WALKED_DEFERRED &&
                   materialRecord.bodyBytes == 80 && materialRecord.nameBytes == 4 &&
                   materialRecord.nestedReferenceCount == 4 &&
                   materialRecord.nestedBodyBytes == 108,
               "material_state") ||
        !Check(g_beginCount == 3 && g_endCount == 3 && g_registerCount == 0 &&
                   g_zone.blocks[0].data[0] == 0xff &&
                   g_zone.blocks[0].data[80] == 3u &&
                   g_zone.blocks[0].data[84] == 0xff &&
                   g_zone.blocks[0].data[116] == 1u &&
                   g_zone.blocks[0].data[132] == 0xc0 &&
                   !std::memcmp(g_zone.blocks[4].data + 8, "mat\0", 4) &&
                   g_zone.blocks[4].data[12] == 0x78 &&
                   !std::memcmp(g_zone.blocks[4].data + 24, "image\0", 6) &&
                   g_zone.blocks[4].data[32] == 0x00 &&
                   g_zone.blocks[4].data[64] == 0xa0,
               "material_stream"))
        return 1;
    RetailWalkDirectoryRecord unsupportedRecord{};
    RetailWalkDirectoryResult unsupportedResult{};
    const RetailWalkDirectoryResultCode unsupportedCode = RetailWalkFastfileDirectory(
        "zone/english/unsupported_inline.ff", &unsupportedRecord, 1, &unsupportedResult);
    if (!Check(unsupportedCode == RETAIL_WALK_BODY_FAILED &&
                   unsupportedResult.code == RETAIL_WALK_BODY_FAILED,
               "unsupported_inline_fails") ||
        !Check(g_beginCount == 4 && g_endCount == 4 && g_registerCount == 0,
               "unsupported_lifecycle"))
        return 1;
    RetailWalkDirectoryRecord unsupportedImageRecord{};
    RetailWalkDirectoryResult unsupportedImageResult{};
    const RetailWalkDirectoryResultCode unsupportedImageCode = RetailWalkFastfileDirectory(
        "zone/english/unsupported_image.ff", &unsupportedImageRecord, 1,
        &unsupportedImageResult);
    if (!Check(unsupportedImageCode == RETAIL_WALK_BODY_FAILED &&
                   unsupportedImageResult.code == RETAIL_WALK_BODY_FAILED,
               "unsupported_image_variant_fails") ||
        !Check(g_beginCount == 5 && g_endCount == 5 && g_registerCount == 0,
               "unsupported_image_lifecycle"))
        return 1;
    RetailWalkDirectoryRecord localizeRecord{};
    RetailWalkDirectoryResult localizeResult{};
    const RetailWalkDirectoryResultCode localizeCode = RetailWalkFastfileDirectory(
        "zone/english/localize.ff", &localizeRecord, 1, &localizeResult);
    if (!Check(localizeCode == RETAIL_WALK_OK && localizeResult.code == RETAIL_WALK_OK,
               "localize_walk") ||
        !Check(localizeResult.assetCount == 1 && localizeResult.recordedCount == 1 &&
                   localizeResult.walkedDeferredCount == 1 && localizeResult.deferredCount == 0,
               "localize_counts") ||
        !Check(localizeRecord.type == ASSET_TYPE_LOCALIZE_ENTRY &&
                   localizeRecord.state == RETAIL_WALK_WALKED_DEFERRED &&
                   localizeRecord.bodyBytes == 8 && localizeRecord.nameBytes == 10 &&
                   localizeRecord.nestedReferenceCount == 2,
               "localize_body") ||
        !Check(g_beginCount == 6 && g_endCount == 6 && g_registerCount == 0 &&
                   g_zone.blocks[0].data[0] == 0xff &&
                   !std::memcmp(g_zone.blocks[4].data + 8, "value\0key\0", 10),
               "localize_stream"))
        return 1;
    RetailWalkDirectoryRecord fontRecord{};
    RetailWalkDirectoryResult fontResult{};
    const RetailWalkDirectoryResultCode fontCode = RetailWalkFastfileDirectory(
        "zone/english/font.ff", &fontRecord, 1, &fontResult);
    if (!Check(fontCode == RETAIL_WALK_OK && fontResult.code == RETAIL_WALK_OK,
               "font_walk") ||
        !Check(fontResult.assetCount == 1 && fontResult.recordedCount == 1 &&
                   fontResult.walkedFontCount == 1 && fontResult.walkedDeferredCount == 1 &&
                   fontResult.deferredCount == 0,
               "font_counts") ||
        !Check(fontRecord.type == ASSET_TYPE_FONT &&
                   fontRecord.state == RETAIL_WALK_WALKED_DEFERRED &&
                   fontRecord.bodyBytes == 24 && fontRecord.nameBytes == 2 &&
                   fontRecord.nestedReferenceCount == 2 && fontRecord.nestedBodyBytes == 48,
               "font_body") ||
        !Check(g_beginCount == 7 && g_endCount == 7 && g_registerCount == 0 &&
                   g_zone.blocks[0].data[0] == 0xff && g_zone.blocks[0].data[8] == 2 &&
                   !std::memcmp(g_zone.blocks[4].data + 8, "f\0", 2) &&
                   g_zone.blocks[4].data[10] == 0 && g_zone.blocks[4].data[59] == 47,
               "font_stream"))
        return 1;
    RetailWalkDirectoryRecord rawFileRecord{};
    RetailWalkDirectoryResult rawFileResult{};
    const RetailWalkDirectoryResultCode rawFileCode = RetailWalkFastfileDirectory(
        "zone/english/rawfile_walk.ff", &rawFileRecord, 1, &rawFileResult);
    if (!Check(rawFileCode == RETAIL_WALK_OK && rawFileResult.code == RETAIL_WALK_OK,
               "rawfile_walk") ||
        !Check(rawFileResult.assetCount == 1 && rawFileResult.recordedCount == 1 &&
                   rawFileResult.walkedRawFileCount == 1 && rawFileResult.walkedDeferredCount == 1 &&
                   rawFileResult.deferredCount == 0,
               "rawfile_counts") ||
        !Check(rawFileRecord.type == ASSET_TYPE_RAWFILE &&
                   rawFileRecord.state == RETAIL_WALK_WALKED_DEFERRED &&
                   rawFileRecord.bodyBytes == 12 && rawFileRecord.nameBytes == 4 &&
                   rawFileRecord.nestedReferenceCount == 2 && rawFileRecord.nestedBodyBytes == 6,
               "rawfile_body") ||
        !Check(g_beginCount == 8 && g_endCount == 8 && g_registerCount == 0 &&
                   g_zone.blocks[0].data[0] == 0xff && g_zone.blocks[0].data[4] == 5 &&
                   !std::memcmp(g_zone.blocks[4].data + 8, "raw\0hello\0", 10),
               "rawfile_stream"))
        return 1;
    RetailWalkDirectoryRecord stringTableRecord{};
    RetailWalkDirectoryResult stringTableResult{};
    const RetailWalkDirectoryResultCode stringTableCode = RetailWalkFastfileDirectory(
        "zone/english/stringtable.ff", &stringTableRecord, 1, &stringTableResult);
    if (!Check(stringTableCode == RETAIL_WALK_OK && stringTableResult.code == RETAIL_WALK_OK,
               "stringtable_walk") ||
        !Check(stringTableResult.walkedStringTableCount == 1 &&
                   stringTableResult.walkedDeferredCount == 1 && stringTableResult.deferredCount == 0,
               "stringtable_counts") ||
        !Check(stringTableRecord.bodyBytes == 16 && stringTableRecord.nameBytes == 2 &&
                   stringTableRecord.nestedReferenceCount == 3 && stringTableRecord.nestedBodyBytes == 12,
               "stringtable_body") ||
        !Check(g_beginCount == 9 && g_endCount == 9 && g_registerCount == 0 &&
                   !std::memcmp(g_zone.blocks[4].data + 24, "t\0", 2) &&
                   g_zone.blocks[4].data[28] == 0xff &&
                   !std::memcmp(g_zone.blocks[4].data + 36, "a\0b\0", 4),
               "stringtable_stream"))
        return 1;
    std::puts("PASS:RETAIL_BOOT_WALK_PROOF zones=9 assets=10 cursors=15 techniques=1 passes=1 materials=1 texture_defs=1 images=1 localize=1 fonts=1 rawfiles=1 stringtables=1 payload=4 oat=0 deferred=audited");
    // M3a small worlds: MapEnts root/name/entity bytes and ComWorld
    // root/name/primary-light array plus LightDef-name strings, each ending
    // walked-deferred. Cursor finals are captured by the stub above.
    RetailWalkDirectoryRecord mapentsRecord{};
    RetailWalkDirectoryResult mapentsResult{};
    const RetailWalkDirectoryResultCode mapentsCode = RetailWalkFastfileDirectory(
        "zone/english/mapents.ff", &mapentsRecord, 1, &mapentsResult);
    if (!Check(mapentsCode == RETAIL_WALK_OK && mapentsResult.code == RETAIL_WALK_OK,
               "mapents_walk") ||
        !Check(mapentsResult.assetCount == 1 && mapentsResult.recordedCount == 1 &&
                   mapentsResult.walkedDeferredCount == 1 &&
                   mapentsResult.walkedMapEntsCount == 1 &&
                   mapentsResult.deferredCount == 0,
               "mapents_counts") ||
        !Check(mapentsRecord.type == ASSET_TYPE_MAP_ENTS &&
                   mapentsRecord.header == 0xffffffffu &&
                   mapentsRecord.state == RETAIL_WALK_WALKED_DEFERRED &&
                   mapentsRecord.bodyBytes == 12 && mapentsRecord.nameBytes == 4 &&
                   mapentsRecord.nestedReferenceCount == 2 &&
                   mapentsRecord.nestedBodyBytes == 9,
               "mapents_body") ||
        !Check(g_beginCount == 10 && g_endCount == 10 && g_registerCount == 0 &&
                   g_zone.blocks[4].data[0] == 0xfu &&
                   !std::memcmp(g_zone.blocks[4].data + 8, "map\0entities\0", 13),
               "mapents_stream"))
        return 1;
    const uint32_t mapentsCursor0 = g_lastCursor0;
    const uint32_t mapentsCursor4 = g_lastCursor4;
    RetailWalkDirectoryRecord comworldRecord{};
    RetailWalkDirectoryResult comworldResult{};
    const RetailWalkDirectoryResultCode comworldCode = RetailWalkFastfileDirectory(
        "zone/english/comworld.ff", &comworldRecord, 1, &comworldResult);
    if (!Check(comworldCode == RETAIL_WALK_OK && comworldResult.code == RETAIL_WALK_OK,
               "comworld_walk") ||
        !Check(comworldResult.assetCount == 1 && comworldResult.recordedCount == 1 &&
                   comworldResult.walkedDeferredCount == 1 &&
                   comworldResult.walkedComWorldCount == 1 &&
                   comworldResult.deferredCount == 0,
               "comworld_counts") ||
        !Check(comworldRecord.type == ASSET_TYPE_COMWORLD &&
                   comworldRecord.header == 0xffffffffu &&
                   comworldRecord.state == RETAIL_WALK_WALKED_DEFERRED &&
                   comworldRecord.bodyBytes == 16 && comworldRecord.nameBytes == 4 &&
                   comworldRecord.nestedReferenceCount == 2 &&
                   comworldRecord.nestedBodyBytes == 139,
               "comworld_body") ||
        !Check(g_beginCount == 11 && g_endCount == 11 && g_registerCount == 0 &&
                   !std::memcmp(g_zone.blocks[4].data + 8, "com\0", 4) &&
                   g_zone.blocks[4].data[12 + 64] == 0xff &&
                   g_zone.blocks[4].data[12 + 68 + 64] == 0x0 &&
                   !std::memcmp(g_zone.blocks[4].data + 12 + 136, "l0\0", 3),
               "comworld_stream"))
        return 1;
    // Block 0 is a rewinding temp scope (RetailWalkBeginTemp/EndTemp), so
    // its final cursor is back at zero; block 4 is persistent and holds the
    // exact 21/151-byte prefixes. bodyBytes above already pins the 12/16
    // temp bytes each walk consumed before the rewind.
    if (!Check(mapentsCursor0 == 0 && mapentsCursor4 == 21 &&
                   g_lastCursor0 == 0 && g_lastCursor4 == 151,
               "small_world_cursors"))
        return 1;
    std::puts("PASS:KILLHOUSE_SMALL_WORLD_WALK mapents=1 comworld=1 cursors=b4=21/151 b0temp=rewound");
    // M3b GameWorldSP/PathData: node array with per-node links, block-1
    // base nodes (zero stream bytes), one chain array, vis buffer, and a
    // leaf tree. The five u16 script-string fields consume no stream bytes.
    RetailWalkDirectoryRecord gameworldRecord{};
    RetailWalkDirectoryResult gameworldResult{};
    const RetailWalkDirectoryResultCode gameworldCode = RetailWalkFastfileDirectory(
        "zone/english/gameworldsp.ff", &gameworldRecord, 1, &gameworldResult);
    if (!Check(gameworldCode == RETAIL_WALK_OK && gameworldResult.code == RETAIL_WALK_OK,
               "gameworld_walk") ||
        !Check(gameworldResult.assetCount == 1 && gameworldResult.recordedCount == 1 &&
                   gameworldResult.walkedDeferredCount == 1 &&
                   gameworldResult.walkedGameWorldSpCount == 1 &&
                   gameworldResult.walkedGameWorldSpBlock1Bytes == 32 &&
                   gameworldResult.deferredCount == 0,
               "gameworld_counts") ||
        !Check(gameworldRecord.type == ASSET_TYPE_GAMEWORLD_SP &&
                   gameworldRecord.header == 0xffffffffu &&
                   gameworldRecord.state == RETAIL_WALK_WALKED_DEFERRED &&
                   gameworldRecord.bodyBytes == 44 && gameworldRecord.nameBytes == 3 &&
                   gameworldRecord.nestedReferenceCount == 6 &&
                   gameworldRecord.nestedBodyBytes == 345,
               "gameworld_body") ||
        !Check(g_beginCount == 12 && g_endCount == 12 && g_registerCount == 0 &&
                   !std::memcmp(g_zone.blocks[4].data + 8, "gw\0", 3) &&
                   g_zone.blocks[4].data[12 + 64] == 0xff &&
                   g_zone.blocks[4].data[12 + 128 + 64] == 0x0 &&
                   !std::memcmp(g_zone.blocks[4].data + 268, "\0\0\0\0\0\0\0\0\0\0\0\0", 12) &&
                   !std::memcmp(g_zone.blocks[4].data + 280, "\x01\0\0\0", 4) &&
                   !std::memcmp(g_zone.blocks[4].data + 284, "\x01\x02\x03", 3) &&
                   !std::memcmp(g_zone.blocks[4].data + 288, "\xff\xff\xff\xff", 4) &&
                   g_zone.blocks[4].data[288 + 8] == 0x2 &&
                   !std::memcmp(g_zone.blocks[4].data + 320, "\x05\0\x06\0", 4) &&
                   !std::memcmp(g_zone.blocks[4].data + 304, "\0\0\0\0", 4) &&
                   !std::memcmp(g_zone.blocks[4].data + 324, "\xff\xff\xff\xff", 4) &&
                   !std::memcmp(g_zone.blocks[4].data + 340, "\x07\0", 2) &&
                   !std::memcmp(g_zone.blocks[4].data + 344, "\xff\xff\xff\xff", 4) &&
                   g_zone.blocks[4].data[344 + 8] == 0x0 &&
                   g_zone.blocks[4].data[344 + 12] == 0x0,
               "gameworld_stream"))
        return 1;
    if (!Check(g_lastCursor0 == 0 && g_lastCursor4 == 360,
               "gameworld_cursors"))
        return 1;
    std::puts("PASS:KILLHOUSE_GAMEWORLD_WALK roots=1 nodes=2 block1=32 cursors=b4=360 b0temp=rewound");
    // M3c ClipMap: planes, static models (null + alias XModel slots),
    // materials, brush sides/edges/nodes with planes, leaves, leaf brushes,
    // leaf-brush nodes with brush lists, surfaces, verts, triangles,
    // borders, partitions with borders, aabb trees, cmodels, brushes with
    // sides, visibility, a nested inline MapEnts, an inline box brush, one
    // dynamic-entity definition with null asset slots, and two block-1
    // runtime lists. The nested MapEnts reuses the M3a reader.
    RetailWalkDirectoryRecord clipmapRecord{};
    RetailWalkDirectoryResult clipmapResult{};
    const RetailWalkDirectoryResultCode clipmapCode = RetailWalkFastfileDirectory(
        "zone/english/clipmap.ff", &clipmapRecord, 1, &clipmapResult);
    if (!Check(clipmapCode == RETAIL_WALK_OK && clipmapResult.code == RETAIL_WALK_OK,
               "clipmap_walk") ||
        !Check(clipmapResult.assetCount == 1 && clipmapResult.recordedCount == 1 &&
                   clipmapResult.walkedDeferredCount == 1 &&
                   clipmapResult.walkedClipMapCount == 1 &&
                   clipmapResult.walkedClipMapBlock1Bytes == 52 &&
                   clipmapResult.walkedMapEntsCount == 1 &&
                   clipmapResult.deferredCount == 0,
               "clipmap_counts") ||
        !Check(clipmapRecord.type == ASSET_TYPE_CLIPMAP &&
                   clipmapRecord.header == 0xffffffffu &&
                   clipmapRecord.state == RETAIL_WALK_WALKED_DEFERRED &&
                   clipmapRecord.bodyBytes == 284 && clipmapRecord.nameBytes == 5 &&
                   clipmapRecord.nestedReferenceCount == 25 &&
                   clipmapRecord.nestedBodyBytes == 894,
               "clipmap_body") ||
        !Check(g_beginCount == 13 && g_endCount == 13 && g_registerCount == 0 &&
                   !std::memcmp(g_zone.blocks[4].data + 8, "clip\0", 5) &&
                   g_zone.blocks[4].data[16] == 0x0,
               "clipmap_s_name") ||
        !Check(g_zone.blocks[4].data[120] == 0x1 &&
                   g_zone.blocks[4].data[123] == 0x40,
               "clipmap_s_static") ||
        !Check(g_zone.blocks[4].data[268] == 0xff &&
                   !std::memcmp(g_zone.blocks[4].data + 300, "\xAA\xBB\xCC", 3) &&
                   g_zone.blocks[4].data[304] == 0xff,
               "clipmap_s_sides") ||
        !Check(!std::memcmp(g_zone.blocks[4].data + 376, "\x01\0\x02\0", 4) &&
                   g_zone.blocks[4].data[382] == 0x1 &&
                   g_zone.blocks[4].data[388] == 0xff &&
                   !std::memcmp(g_zone.blocks[4].data + 400, "\x07\0", 2),
               "clipmap_s_leaf") ||
        !Check(g_zone.blocks[4].data[468] == 0xff,
               "clipmap_s_part") ||
        !Check(g_zone.blocks[4].data[640] == 0xff &&
                   g_zone.blocks[4].data[656] == 0x0 &&
                   g_zone.blocks[4].data[688] == 0xff,
               "clipmap_s_brush") ||
        !Check(!std::memcmp(g_zone.blocks[4].data + 726, "cm\0ent\0\0", 9) &&
                   g_zone.blocks[4].data[736] == 0x0 &&
                   g_zone.blocks[4].data[816] == 0x0 &&
                   g_zone.blocks[1].size == 52 &&
                   g_zone.blocks[1].data[0] == 0x0 &&
                   g_zone.blocks[1].data[51] == 0x0,
               "clipmap_s_tail"))
        return 1;
    if (!Check(g_lastCursor0 == 0 && g_lastCursor4 == 912,
               "clipmap_cursors"))
        return 1;
    std::puts("PASS:KILLHOUSE_CLIPMAP_WALK roots=1 dependencies=25 block1=52 cursors=b4=912 b0temp=rewound");
    // M3d GfxWorld: names, indices, sky, sun light + LightDef, probes,
    // cells with aabb trees/portals, lightmaps, models, materials, verts,
    // sun materials (inline + alias), shadow/light regions, and one DPVS
    // surface with a material.
    RetailWalkDirectoryRecord gfxworldRecord{};
    RetailWalkDirectoryResult gfxworldResult{};
    const RetailWalkDirectoryResultCode gfxworldCode = RetailWalkFastfileDirectory(
        "zone/english/gfxworld.ff", &gfxworldRecord, 1, &gfxworldResult);
    if (!Check(gfxworldCode == RETAIL_WALK_OK && gfxworldResult.code == RETAIL_WALK_OK,
               "gfxworld_walk") ||
        !Check(gfxworldResult.assetCount == 1 && gfxworldResult.recordedCount == 1 &&
                   gfxworldResult.walkedDeferredCount == 1 &&
                   gfxworldResult.walkedGfxWorldCount == 1 &&
                   gfxworldResult.walkedGfxWorldBlock1Bytes == 12 &&
                   gfxworldResult.deferredCount == 0,
               "gfxworld_counts") ||
        !Check(gfxworldRecord.type == ASSET_TYPE_GFXWORLD &&
                   gfxworldRecord.header == 0xffffffffu &&
                   gfxworldRecord.state == RETAIL_WALK_WALKED_DEFERRED &&
                   gfxworldRecord.bodyBytes == 732 && gfxworldRecord.nameBytes == 9 &&
                   gfxworldRecord.nestedReferenceCount == 20 &&
                   gfxworldRecord.nestedBodyBytes == 850,
               "gfxworld_body") ||
        !Check(g_beginCount == 14 && g_endCount == 14 && g_registerCount == 0 &&
                   !std::memcmp(g_zone.blocks[4].data + 8, "gfx\0base\0", 9) &&
                   !std::memcmp(g_zone.blocks[4].data + 18, "\0\0\x01\0\x02\0", 6),
               "gfxworld_s_names") ||
        !Check(g_zone.blocks[4].data[88] == 0xff,
               "gfxworld_s_sun") ||
        !Check(g_zone.blocks[4].data[136] == 0xff &&
                   g_zone.blocks[4].data[132] == 0x1 &&
                   g_zone.blocks[4].data[144] == 0xff &&
                   g_zone.blocks[4].data[140] == 0x1,
               "gfxworld_s_cell") ||
        !Check(g_zone.blocks[4].data[200] == 0xff &&
                   g_zone.blocks[4].data[198] == 0x2,
               "gfxworld_s_tree") ||
        !Check(g_zone.blocks[4].data[244] == 0xff &&
                   g_zone.blocks[4].data[252] == 0x2,
               "gfxworld_s_portal") ||
        !Check(g_zone.blocks[4].data[540] == 0xff &&
                   g_zone.blocks[4].data[536] == 0x1,
               "gfxworld_s_region") ||
        !Check(g_zone.blocks[4].data[620] == 0xff &&
                   g_zone.blocks[4].data[616] == 0x1,
               "gfxworld_s_hull") ||
        !Check(g_zone.blocks[4].data[660] == 0xff &&
                   g_zone.blocks[1].size == 12 &&
                   g_zone.blocks[1].data[0] == 0x0 &&
                   g_zone.blocks[1].data[11] == 0x0,
               "gfxworld_s_tail"))
        return 1;
    if (!Check(g_lastCursor0 == 0 && g_lastCursor4 == 692,
               "gfxworld_cursors"))
        return 1;
    std::puts("PASS:KILLHOUSE_GFXWORLD_WALK roots=1 surfaces=1 vertices=2 indices=3 dependencies=20 cursors=b4=692 b0temp=rewound");
    return 0;
}
