// Proof: RetailWalkLoadZoneAssets drives the real, installed
// RetailZoneAssetLoader callbacks (Font, LocalizeEntry) from a live FS
// stream -- FS_OpenRetailFastfile/FS_ReadRetailFastfile -- rather than a
// synthetic in-memory buffer, and leaves the zone live in the registry on
// success.  Uses the exact same localize.ff/font.ff fixtures the test
// script already builds for F5's walk proof (switch_retail_walk_test.cpp),
// so the fixture bytes are independently verified against that walker's own
// byte-accounting first.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <database/database.h>
#include <database/db_retail_decode_font.h>
#include <database/db_retail_decode_fx.h>
#include <database/db_retail_decode_image.h>
#include <database/db_retail_decode_material.h>
#include <database/db_retail_decode_menulist.h>
#include <database/db_retail_decode_rawfile.h>
#include <database/db_retail_decode_small.h>
#include <database/db_retail_decode_sound.h>
#include <database/db_retail_decode_stringtable.h>
#include <database/db_retail_decode_techniqueset.h>
#include <database/db_retail_decode_ui.h>
#include <database/db_retail_decode_weapon.h>
#include <database/db_retail_decode_world.h>
#include <database/db_retail_walk.h>
#include <database/db_retail_zone.h>
#include <DynEntity/DynEntity_client.h>
#include <sound/snd_public.h>
#include <script/scr_stringlist.h>
#include <gfx_d3d/fxprimitives.h>
#include <gfx_d3d/r_bsp.h>
#include <qcommon/com_bsp.h>
#include <xanim/dobj.h>
#include <xanim/dobj_utils.h>
#include <gfx_d3d/r_dobj_skin.h>
#include <gfx_d3d/r_scene.h>
#include <port/switch_shader_prebake.h>

GfxScene scene{};
// Worker scheduling is outside this decoder/placement proof.
int R_FXNonDependentOrSpotLightPending(void *) { std::abort(); }
int R_EndFenceBusy(void *) { std::abort(); }
int R_PreSkinXSurface(const DObj_s *, XSurface *, const GfxModelSurfaceInfo *,
                      uint32_t *, GfxModelSkinnedSurface *);
// rb_light.h pulls the backend/D3D headers, which the host build does not
// have.  The light-region containment test the runtime primary-light
// assignment (R_GetPrimaryLightForModelVertex -> R_IsPointInLightRegionHull)
// performs is self-contained, so replicate just that KDOP test here for the
// Probe.
static bool RetailRegionHullContains(const GfxLightRegionHull &hull, const float *p)
{
    auto absf = [](float v) { return v < 0.0f ? -v : v; };
    if (hull.kdopHalfSize[0] <= absf(p[0] - hull.kdopMidPoint[0])) return false;
    if (hull.kdopHalfSize[1] <= absf(p[1] - hull.kdopMidPoint[1])) return false;
    if (hull.kdopHalfSize[2] <= absf(p[2] - hull.kdopMidPoint[2])) return false;
    if (hull.kdopHalfSize[3] <= absf(p[1] + p[0] - hull.kdopMidPoint[3])) return false;
    if (hull.kdopHalfSize[4] <= absf(p[0] - p[1] - hull.kdopMidPoint[4])) return false;
    if (hull.kdopHalfSize[5] <= absf(p[2] + p[0] - hull.kdopMidPoint[5])) return false;
    if (hull.kdopHalfSize[6] <= absf(p[0] - p[2] - hull.kdopMidPoint[6])) return false;
    if (hull.kdopHalfSize[7] <= absf(p[2] + p[1] - hull.kdopMidPoint[7])) return false;
    if (hull.kdopHalfSize[8] <= absf(p[1] - p[2] - hull.kdopMidPoint[8])) return false;
    for (uint32_t axisIter = 0; axisIter < hull.axisCount; ++axisIter)
    {
        const float dot = hull.axis[axisIter].dir[0] * p[0] + hull.axis[axisIter].dir[1] * p[1] +
                          hull.axis[axisIter].dir[2] * p[2];
        if (hull.axis[axisIter].halfSize <= absf(dot - hull.axis[axisIter].midPoint))
            return false;
    }
    return true;
}
#include <game/g_bsp.h>

#include <new>

// scrStringDebugGlob lives in the real SL_* TU linked for the script-string tests; the test
// snapshots its totalRefCount around the XAnim zone lifetime to prove no
// leaked script-string references survive unload.
extern scrStringDebugGlob_t *scrStringDebugGlob;

// sweep #3 (poison-on-free): ASan's own interface, declared weak so a
// non-ASan link still builds. The reader close fills every transient block
// with 0xDD before Z_Free; the demonstration below reintroduces the
// retained-reader-pointer shape (a registered pointer into a reader block) and observes the poison
// content at that pointer.
extern "C" void *__asan_region_is_poisoned(const void *beg, size_t size) __attribute__((weak));
extern "C" void __asan_unpoison_memory_region(const volatile void *addr, size_t size)
    __attribute__((weak));

// xanim.h retains this static dispatch table even though this narrow test
// never executes a collision trace.  The real engine owns both functions.
void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}

// From switch_retail_zone_stubs_test.cpp.
bool StubZoneIsLive(uint32_t zoneIndex);
uint32_t StubRegisteredCount(void);
uint32_t StubSoundAliasNameSplitCount(uint32_t *multiVariantListsOut, const char **firstSplitOut);
uint32_t StubFxVelIntervalZeroCount(uint32_t *effectsOut, uint32_t *elemsOut, const char **firstOut,
                                    int *firstElemOut);
XAssetHeader StubFindXAssetHeader(XAssetType type, const char *name);
uint32_t StubOverrideCount(void);
uint32_t StubRetiredPtrCount(void);
uint32_t StubZoneHighWater(void);
// Host mirror of Com_ShutdownWorld + its read-back: the engine shuts the
// world singleton down before zone unload (CL_ShutdownRenderer/Com_Close),
// which the whole-corpus sweep needs to unload a map whose ComWorld was live.
void StubShutdownWorld(void);
uint32_t StubWorldInUse(void);
// retained-pointer generalization: registration-time name-provenance audit (see
// switch_retail_zone_stubs_test.cpp and com_files.h's
// FS_RetailFastfilePointerIsReaderTransient).
uint32_t StubNamedRegistrationCount(void);
uint32_t StubReaderOwnedNameCount(void);
uint64_t StubNamedTypeMask(void);
void StubResetReaderOwnedNameCount(void);
// pointer-provenance generalization: the close-time audit scans every
// live zone arena (all raw pointer fields, all 33 types) against the open
// reader's transient blocks. `<X>Count` must end at zero; the negative
// control plants a pointer and requires the scan to see it.
uint32_t StubScanLiveZonesForReaderTransient(void);
uint32_t StubReaderOwnedBodyCount(void);
void StubResetReaderOwnedBodyCount(void);
uint32_t StubArenaScanZones(void);
uint64_t StubArenaScanWords(void);
uint32_t StubProofHeapRangeCount(void);
uint32_t StubProofHeapOverflow(void);
uint32_t StubProofHeapScanRanges(void);
// Host double for the production material pool (switch_retail_zone_stubs_test.cpp);
// the negative control plants a reader pointer in one of its blocks so the
// heap-range half of the sweep is proven, not just the arena half.
uint8_t *Material_Alloc(uint32_t size);
// per-type census: registration declarations per XAssetType, and a
// collect-by-type helper for the nested-owner pointer-identity audit.
uint32_t StubRegisterAttemptCount(uint32_t type);
uint32_t StubCollectRegistered(XAssetType type, XAssetHeader *out, uint32_t capacity);
void StubRegisterStockAsset(XAssetType type, XAssetHeader header);
bool StubUnregisterStockAsset(XAssetType type, XAssetHeader header);
// DB default-entry double: install the engine default material so a missing
// by-name material lookup registers the original default entry, exactly like
// DB_CreateDefaultEntry on the Switch target (see switch_retail_zone_stubs_test.cpp).
void StubInstallDefaultMaterial(Material *material);
uint32_t StubDefaultEntryCount(void);
// Index-based unload for zones whose session object the live loader owns
// internally (mirrors the real engine's index-based DB_UnloadXZone path;
// RetailZoneLoadSessionAbort covers the session-held cases below).
bool DB_RetailZoneEnd(uint32_t zoneIndex);

namespace
{
bool Check(bool condition, const char *stage)
{
    if (!condition)
        std::fprintf(stderr, "FAIL:RETAIL_BOOT_LOAD_PROOF stage=%s\n", stage);
    return condition;
}

// Converged fixture-zone helpers. Most blocks load one fixture and require the
// zone load itself succeeded, then unload it and require the zone is gone with
// no retired pointer left behind; the per-test assertions on the loaded assets
// stay at the call site. `strict` selects RetailWalkLoadZoneAssets' strict
// acceptance mode (exact typed aliases/inserts or loud failure).
bool LoadFixtureZone(const char *relativePath, RetailWalkLoadZoneResult *result,
                     const char *stage, bool strict = false)
{
    *result = {};
    return Check(RetailWalkLoadZoneAssets(relativePath, result, strict) == RETAIL_WALK_LOAD_OK,
                 stage);
}

bool UnloadFixtureZone(const RetailWalkLoadZoneResult &result)
{
    return DB_RetailZoneEnd(result.zoneIndex) && !StubZoneIsLive(result.zoneIndex) &&
           StubRetiredPtrCount() == 0;
}

// host oracle: resolve the light-grid cell a world-space lighting
// origin quantizes to, and the entry that cell's row/RLE bytes carry -- from
// the grid's own mins/maxs/rowAxis/colAxis and row data only.  This is
// deliberately independent of rb_light.cpp's R_LightGridLookup (it does not
// call it and is written from the structural layout), so its result can be
// compared with the real lookup's for the same origin.  The caller owns
// `cell` (zeroed here).
struct RetailLightGridCell
{
    bool insideGrid;
    bool rowPresent;
    bool entryPresent;
    uint32_t reason; // 0=entry, 1=outside grid, 2=no row, 3=cell outside row,
                     // 4=empty RLE block, 5=z outside block, 6=entry index OOB
    uint32_t pos[3];
    uint32_t rowIndex;
    uint32_t colIndex;
    uint32_t z;
    uint16_t colStart;
    uint16_t colCount;
    uint16_t zStart;
    uint16_t zCount;
    uint32_t firstEntry;
    uint32_t entryIndex;
    GfxLightGridEntry entry;
};

// Resolve one explicit (rowIndex, colIndex, z) cell against the row data.
// Reasons: 2=no row, 3=cell outside row bounds, 4=empty RLE block, 5=z
// outside block, 6=entry index out of range; cell.entryPresent only for a
// real entry (reason 0).
bool RetailLightGridEntryForCell(const GfxLightGrid &grid, uint32_t rowIndex,
                                 uint32_t colIndex, uint32_t z, RetailLightGridCell &cell)
{
    if (!grid.entries || !grid.rawRowData || !grid.rowDataStart || rowIndex >= 0x1000000u)
    {
        cell.reason = 2;
        return true;
    }
    const uint16_t rowStart = grid.rowDataStart[rowIndex];
    if (rowStart == 0xFFFFu || 4u * (uint32_t)rowStart + 12u > grid.rawRowDataSize)
    {
        cell.reason = 2;
        return true;
    }
    cell.rowPresent = true;
    const uint8_t *row = grid.rawRowData + 4u * (uint32_t)rowStart;
    cell.colStart = (uint16_t)(row[0] | (row[1] << 8));
    cell.colCount = (uint16_t)(row[2] | (row[3] << 8));
    cell.zStart = (uint16_t)(row[4] | (row[5] << 8));
    cell.zCount = (uint16_t)(row[6] | (row[7] << 8));
    cell.firstEntry = (uint32_t)row[8] | ((uint32_t)row[9] << 8) |
                      ((uint32_t)row[10] << 16) | ((uint32_t)row[11] << 24);
    if (colIndex >= cell.colCount || z >= cell.zCount)
    {
        cell.reason = 3;
        return true;
    }
    // The row's RLE blocks tile (colStart..colStart+colCount) x
    // (zStart..zStart+zCount) as (run, height, zBase[, zBaseHi]); entry =
    // firstEntry + colIndex*height + (z - zBase).
    const uint8_t *const rawEnd = grid.rawRowData + grid.rawRowDataSize;
    const uint8_t *rle = row + 12;
    uint32_t col = colIndex;
    uint32_t first = cell.firstEntry;
    const uint32_t zBytes = cell.zCount > 255u ? 4u : 3u;
    for (uint32_t block = 0; block < 4096u; ++block)
    {
        if (rle + 2 > rawEnd)
        {
            cell.reason = 4;
            return true;
        }
        const uint32_t run = rle[0];
        const uint32_t height = rle[1];
        if (col < run)
        {
            if (height == 0u || rle + zBytes > rawEnd)
            {
                cell.reason = 4;
                return true;
            }
            const uint32_t baseZ = rle[2] | (zBytes == 4u ? ((uint32_t)rle[3] << 8) : 0u);
            if (z < baseZ || z - baseZ >= height)
            {
                cell.reason = 5;
                return true;
            }
            const uint64_t index =
                (uint64_t)first + (uint64_t)col * height + (z - baseZ);
            if (index >= grid.entryCount)
            {
                cell.reason = 6;
                return true;
            }
            cell.entryIndex = (uint32_t)index;
            cell.entry = grid.entries[cell.entryIndex];
            cell.entryPresent = true;
            cell.reason = 0;
            return true;
        }
        col -= run;
        first += run * height;
        rle += height != 0u ? zBytes : 2u;
    }
    cell.reason = 4;
    return true;
}

bool RetailLightGridCellForOrigin(const GfxLightGrid &grid, const float *origin,
                                  RetailLightGridCell &cell)
{
    std::memset(&cell, 0, sizeof(cell));
    if (!grid.entries || !grid.rawRowData || !grid.rowDataStart || grid.rowAxis > 2 ||
        grid.colAxis > 2 || grid.rowAxis == grid.colAxis)
        return false;
    cell.pos[0] = ((int)std::floor(origin[0]) + 0x20000) >> 5;
    cell.pos[1] = ((int)std::floor(origin[1]) + 0x20000) >> 5;
    cell.pos[2] = ((int)std::floor(origin[2]) + 0x20000) >> 6;
    const uint32_t rowCount = grid.maxs[grid.rowAxis] + 1u - grid.mins[grid.rowAxis];
    if (cell.pos[grid.rowAxis] < grid.mins[grid.rowAxis] ||
        cell.pos[grid.rowAxis] - grid.mins[grid.rowAxis] >= rowCount ||
        cell.pos[grid.colAxis] < grid.mins[grid.colAxis] ||
        cell.pos[grid.colAxis] > grid.maxs[grid.colAxis] ||
        cell.pos[2] < grid.mins[2] || cell.pos[2] > grid.maxs[2])
    {
        cell.reason = 1; // outside the grid's own bounds
        return true;
    }
    cell.insideGrid = true;
    cell.rowIndex = cell.pos[grid.rowAxis] - grid.mins[grid.rowAxis];
    const uint16_t rowStart = grid.rowDataStart[cell.rowIndex];
    if (rowStart == 0xFFFFu || 4u * (uint32_t)rowStart + 12u > grid.rawRowDataSize)
    {
        cell.reason = 2; // no row for this row index: runtime samples null
        return true;
    }
    const uint8_t *row = grid.rawRowData + 4u * (uint32_t)rowStart;
    const uint16_t colStart = (uint16_t)(row[0] | (row[1] << 8));
    const uint16_t colCount = (uint16_t)(row[2] | (row[3] << 8));
    const uint16_t zStart = (uint16_t)(row[4] | (row[5] << 8));
    const uint16_t zCount = (uint16_t)(row[6] | (row[7] << 8));
    if (cell.pos[grid.colAxis] < colStart || cell.pos[grid.colAxis] - colStart >= colCount ||
        cell.pos[2] < zStart || cell.pos[2] - zStart >= zCount)
    {
        cell.reason = 3; // row exists but this cell is outside its bounds
        return true;
    }
    cell.colIndex = cell.pos[grid.colAxis] - colStart;
    cell.z = cell.pos[2] - zStart;
    return RetailLightGridEntryForCell(grid, cell.rowIndex, cell.colIndex, cell.z, cell);
}

// Resolve the runtime's eight corner cells for a lighting origin.  Corner
// order matches R_LightGridLookup's two R_GetLightGridSampleEntryQuad calls:
// the first call samples the origin's own row and fills corners 0..3 as
// (col,z), (col,z+1), (col+1,z), (col+1,z+1); the second call samples
// pos[rowAxis]+1 -- a DIFFERENT row whose own colStart/zStart bases apply --
// and fills corners 4..7 the same way.  Reading the origin row's bases for
// the second call would silently sample the wrong cells, so this mirrors the
// two-call structure explicitly.  Returns false only for a structurally
// unusable grid; otherwise each cell carries its own reason.
bool RetailLightGridQuadForOrigin(const GfxLightGrid &grid, const float *origin,
                                  RetailLightGridCell cells[8])
{
    std::memset(cells, 0, sizeof(RetailLightGridCell) * 8);
    if (!grid.entries || !grid.rawRowData || !grid.rowDataStart || grid.rowAxis > 2 ||
        grid.colAxis > 2 || grid.rowAxis == grid.colAxis)
        return false;
    const uint32_t pos[3] = {
        ((uint32_t)((int)std::floor(origin[0]) + 0x20000) >> 5),
        ((uint32_t)((int)std::floor(origin[1]) + 0x20000) >> 5),
        ((uint32_t)((int)std::floor(origin[2]) + 0x20000) >> 6)};
    for (uint32_t corner = 0; corner < 8; ++corner)
    {
        cells[corner].pos[0] = pos[0];
        cells[corner].pos[1] = pos[1];
        cells[corner].pos[2] = pos[2];
    }
    const uint32_t rowCount = grid.maxs[grid.rowAxis] + 1u - grid.mins[grid.rowAxis];
    for (uint32_t rowDelta = 0; rowDelta < 2; ++rowDelta)
    {
        const uint32_t rowPos = pos[grid.rowAxis] + rowDelta;
        uint32_t rowIndex = 0;
        const uint8_t *row = nullptr;
        if (rowPos >= grid.mins[grid.rowAxis] &&
            rowPos - grid.mins[grid.rowAxis] < rowCount)
        {
            rowIndex = rowPos - grid.mins[grid.rowAxis];
            const uint16_t rowStart = grid.rowDataStart[rowIndex];
            if (rowStart != 0xFFFFu &&
                4u * (uint32_t)rowStart + 12u <= grid.rawRowDataSize)
                row = grid.rawRowData + 4u * (uint32_t)rowStart;
        }
        if (!row)
        {
            for (uint32_t c = 0; c < 4; ++c)
                cells[rowDelta * 4 + c].reason = 2;
            continue;
        }
        const int64_t colBase = (int64_t)pos[grid.colAxis] -
                                (int64_t)(uint16_t)(row[0] | (row[1] << 8));
        const int64_t zBase = (int64_t)pos[2] - (int64_t)(uint16_t)(row[4] | (row[5] << 8));
        for (uint32_t c = 0; c < 4; ++c)
        {
            const int64_t colIndex = colBase + (int64_t)((c >> 1) & 1u);
            const int64_t z = zBase + (int64_t)(c & 1u);
            RetailLightGridCell &cell = cells[rowDelta * 4 + c];
            cell.rowIndex = rowIndex;
            if (colIndex < 0 || colIndex > 0x1000000 || z < 0 || z > 0x1000000)
            {
                cell.reason = 3;
                continue;
            }
            RetailLightGridEntryForCell(grid, rowIndex, (uint32_t)colIndex, (uint32_t)z,
                                        cell);
        }
    }
    return true;
}

// Slice A: widen every real ui.ff menu item body through the production
// RetailDecodeItemDef/RetailDecodeMenu/RetailDecodeMenuList path and tally
// the result for the OAT oracle comparison (OAT Unlinker menu-dump oracle).
// Walk-only for assets: widens into a session arena, registers nothing,
// emits no PASS itself. Needs zone/english/ui.ff staged under the root.
bool RetailUiItemsSection()
{
    auto Fail = [](const char *stage, unsigned value) {
        std::fprintf(stderr, "FAIL:UI_ITEMS_PROOF stage=%s value=%u\n", stage, value);
        return false;
    };
    FsRetailFastfileReader *fastfile = nullptr;
    if (FS_OpenRetailFastfile("zone/english/ui.ff", &fastfile) != FS_RETAIL_FF_OK || !fastfile)
    {
        std::puts("SKIP:UI_ITEMS needs staged retail ui.ff");
        return true;
    }
    static FsRetailFastfileAsset assets[35];
    FsRetailFastfileAssetList list;
    if (FS_ReadRetailFastfileAssetList(fastfile, &list, assets, 35, 0, 0) != FS_RETAIL_FF_WIRE_OK ||
        list.assetCount != 35)
        return Fail("directory", list.assetCount);
    uint32_t materials = 0, techsets = 0;
    for (uint32_t index = 0; index <= 10; ++index)
    {
        if (assets[index].header != 0xffffffffu)
            return Fail("entry_header", index);
        if (assets[index].type == 5)
        {
            FsRetailFastfileTechniqueSet techset;
            if (FS_ReadRetailFastfileTechniqueSetPrefix(fastfile, assets[index].header, &techset) !=
                FS_RETAIL_FF_WIRE_OK)
                return Fail("techset", index);
            ++techsets;
        }
        else if (assets[index].type == 4)
        {
            FsRetailFastfileMaterial material;
            static FsRetailFastfileTextureDef textureDefs[8];
            if (FS_ReadRetailFastfileMaterial(fastfile, assets[index].header, &material, textureDefs, 8) !=
                FS_RETAIL_FF_WIRE_OK)
                return Fail("material", index);
            ++materials;
        }
        else
            return Fail("entry_type", index);
    }
    if (materials != 7 || techsets != 4)
        return Fail("boot_counts", materials * 16 + techsets);
    if (assets[11].type != 20 || assets[11].header != 0xffffffffu)
        return Fail("menulist_header", assets[11].type);

    FsRetailFastfileMenuList menuList;
    static uint32_t menuRefs[56];
    if (FS_ReadRetailFastfileMenuList(fastfile, assets[11].header, &menuList, menuRefs, 56) !=
        FS_RETAIL_FF_WIRE_OK || menuList.menuCount != 56)
        return Fail("menulist", menuList.menuCount);
    for (uint32_t m = 0; m < 56; ++m)
        if (menuRefs[m] != 0xffffffffu)
            return Fail("menu_ref", m);

    // Session over a private copy of block 4: every menu/item string and
    // type-data payload the widener touches lives there (the decoder's own
    // token mask), so no other block is mapped.
    const uint32_t block4Size = FS_RetailFastfileBlockSize(fastfile, 4);
    if (!block4Size || block4Size > (1u << 24))
        return Fail("block4_size", block4Size);
    uint8_t *block4 = new (std::nothrow) uint8_t[block4Size];
    static const size_t kArenaBytes = 16u << 20;
    uint8_t *arenaBytes = new (std::nothrow) uint8_t[kArenaBytes];
    if (!block4 || !arenaBytes)
        return Fail("alloc", 0);
    if (FS_ReadRetailFastfileBlock(fastfile, 4, 0, block4, block4Size) != FS_RETAIL_FF_WIRE_OK)
        return Fail("block4_read", block4Size);
    RetailZoneLoadSession session{};
    XZoneMemory zone{};
    zone.blocks[4] = {block4, block4Size};
    session.active = true;
    session.zoneMemory = &zone;
    if (!RetailWireBlocksInit(&session.wire, &zone) ||
        !RetailNativeArenaInit(&session.arena, arenaBytes, kArenaBytes))
        return Fail("session", 0);

    static FsRetailFastfileItemKeyHandler menuHandlers[64];
    static FsRetailFastfileExpressionEntry menuEntries[1024];
    static FsRetailFastfileItemKeyHandler itemHandlers[64];
    static FsRetailFastfileExpressionEntry itemEntries[2048];
    uint32_t items = 0, widened = 0, unsupported = 0, typeskip = 0, parents = 0;
    uint32_t typeCount[23] = {};
    uint32_t stmtCount[8] = {};
    uint32_t menuStmt[3] = {};
    uint32_t entriesTotal = 0, handlersTotal = 0;
    uint32_t mainTextItems = 0, mainTextType[23] = {}, mainTextStmt[8] = {};
    uint32_t unoracledMenus = 0;
    menuDef_t *menus[56] = {};
    bool ok = true;
    for (uint32_t m = 0; ok && m < 56; ++m)
    {
        FsRetailFastfileMenu wireMenu{};
        if (FS_ReadRetailFastfileMenuPrefix(fastfile, menuRefs[m], &wireMenu, menuHandlers, 64,
                                            menuEntries, 1024) != FS_RETAIL_FF_WIRE_OK)
        {
            std::fprintf(stderr, "FAIL:UI_ITEMS_PROOF stage=menu_prefix value=%u\n", m);
            ok = false;
            break;
        }
        char menuName[64] = {};
        if (FS_ReadRetailFastfileBlock(fastfile, 4, (wireMenu.nameRef - 1) & 0x0fffffffu,
                                       reinterpret_cast<uint8_t *>(menuName), sizeof(menuName) - 1) !=
            FS_RETAIL_FF_WIRE_OK)
        {
            std::fprintf(stderr, "FAIL:UI_ITEMS_PROOF stage=menu_name value=%u\n", m);
            ok = false;
            break;
        }
        if (wireMenu.visibleExp.numEntries)
            ++menuStmt[0];
        if (wireMenu.rectXExp.numEntries)
            ++menuStmt[1];
        if (wireMenu.rectYExp.numEntries)
            ++menuStmt[2];
        const bool unoracled = menuName[0] == ',';
        if (unoracled)
            ++unoracledMenus;
        if (m == 1 && std::strcmp(menuName, "main_text") != 0)
        {
            std::fprintf(stderr, "FAIL:UI_ITEMS_PROOF stage=main_text_slot value=%u\n", m);
            ok = false;
            break;
        }
        uint32_t *itemSlots = new (std::nothrow) uint32_t[wireMenu.itemCount ? wireMenu.itemCount : 1];
        if (!itemSlots)
        {
            std::fprintf(stderr, "FAIL:UI_ITEMS_PROOF stage=item_slots value=%u\n", m);
            ok = false;
            break;
        }
        if (wireMenu.itemCount &&
            FS_ReadRetailFastfileBlock(fastfile, 4, wireMenu.itemsOffset,
                                       reinterpret_cast<uint8_t *>(itemSlots),
                                       wireMenu.itemCount * 4u) != FS_RETAIL_FF_WIRE_OK)
        {
            std::fprintf(stderr, "FAIL:UI_ITEMS_PROOF stage=items_array value=%u\n", m);
            delete[] itemSlots;
            ok = false;
            break;
        }
        itemDef_s **nativeItems = nullptr;
        if (wireMenu.itemCount)
        {
            nativeItems = static_cast<itemDef_s **>(RetailZoneLoadSessionAlloc(
                &session, sizeof(itemDef_s *) * wireMenu.itemCount, alignof(itemDef_s *)));
            if (!nativeItems)
            {
                std::fprintf(stderr, "FAIL:UI_ITEMS_PROOF stage=item_ptrs value=%u\n", m);
                delete[] itemSlots;
                ok = false;
                break;
            }
        }
        uint32_t menuTypes[23] = {}, menuStmtLocal[8] = {}, menuEntryTotal = 0, menuHandlerTotal = 0;
        for (int32_t i = 0; ok && i < wireMenu.itemCount; ++i)
        {
            FsRetailFastfileItemDef wireItem{};
            const FsRetailFastfileWireResult wireResult = FS_ReadRetailFastfileItemDef(
                fastfile, itemSlots[i], &wireItem, itemHandlers, 64, itemEntries, 2048);
            if (wireResult != FS_RETAIL_FF_WIRE_OK)
            {
                std::fprintf(stderr, "FAIL:UI_ITEMS_PROOF stage=item_wire value=%u\n", m * 1000u + i);
                ok = false;
                break;
            }
            // A set type-data ref outside the decoder's four widened forms
            // would be silently nulled below; count it instead so the
            // verifier can pin zero.
            if (wireItem.typeDataRef && wireItem.type != 6 && wireItem.type != 0xC &&
                wireItem.type != 0xD)
            {
                bool editShaped = false;
                switch (wireItem.type)
                {
                case 4: case 9: case 0x10: case 0x12: case 0xB:
                case 0xE: case 0xA: case 0: case 0x11:
                    editShaped = true;
                    break;
                default:
                    break;
                }
                if (!editShaped)
                {
                    // Matches the original Load_itemDefData_t: ownerdraw
                    // (and any other non-widened form) carries a pointer
                    // slot the loader never follows. Counted, not dropped
                    // silently, so the verifier can pin the exact case.
                    ++typeskip;
                    std::printf("UI_ITEMS_TYPESKIP menu=%u item=%d type=%d typeDataRef=0x%08x\n", m,
                                i, wireItem.type, wireItem.typeDataRef);
                }
            }
            if (wireItem.focusSoundRef)
                ++unsupported;
            // Lead: publish every item that drives the ownerdraw switch so
            // the full-map menu (ownerDraw 180/181/182-187) can be located
            // without an OAT menu dump (which this OAT build cannot produce).
            if (wireItem.ownerDraw)
            {
                std::printf("UI_ITEMS_OWNERDRAW menu=%u name='%s' item=%d type=%d ownerDraw=%d "
                            "ownerDrawFlags=%d backgroundRef=0x%08x rect=(%.1f,%.1f,%.1f,%.1f)\n",
                            m, menuName, i, wireItem.type, wireItem.ownerDraw,
                            wireItem.ownerDrawFlags, wireItem.backgroundRef, wireItem.rect[0],
                            wireItem.rect[1], wireItem.rect[2], wireItem.rect[3]);
            }
            itemDef_s *native = nullptr;
            const RetailUiDecodeResult widenResult = RetailDecodeItemDef(
                &session, &wireItem, itemHandlers, wireItem.handlerCount, itemEntries,
                wireItem.expressionEntryCount, &native, nullptr);
            if (widenResult != RETAIL_UI_DECODE_OK)
            {
                std::fprintf(stderr, "FAIL:UI_ITEMS_PROOF stage=item_widen value=%u result=%d\n",
                             m * 1000u + i, (int)widenResult);
                ok = false;
                break;
            }
            nativeItems[i] = native;
            ++items;
            ++widened;
            if (wireItem.type >= 0 && wireItem.type <= 22)
                ++typeCount[wireItem.type];
            else
                ++typeCount[22];
            if (m == 1)
            {
                ++mainTextItems;
                if (wireItem.type >= 0 && wireItem.type <= 22)
                    ++mainTextType[wireItem.type];
                for (uint32_t s = 0; s < 8; ++s)
                    if (wireItem.statements[s].numEntries)
                        ++mainTextStmt[s];
            }
            if (wireItem.type >= 0 && wireItem.type <= 22)
                ++menuTypes[wireItem.type];
            for (uint32_t s = 0; s < 8; ++s)
                if (wireItem.statements[s].numEntries)
                {
                    ++stmtCount[s];
                    ++menuStmtLocal[s];
                }
            entriesTotal += wireItem.expressionEntryCount;
            handlersTotal += wireItem.handlerCount;
            menuEntryTotal += wireItem.expressionEntryCount;
            menuHandlerTotal += wireItem.handlerCount;
        }
        delete[] itemSlots;
        if (!ok)
            break;
        menuDef_t *nativeMenu = nullptr;
        if (RetailDecodeMenu(&session, &wireMenu, menuHandlers, wireMenu.handlerCount, menuEntries,
                             wireMenu.expressionEntryCount, nativeItems, wireMenu.itemCount,
                             &nativeMenu, nullptr) != RETAIL_UI_DECODE_OK)
        {
            std::fprintf(stderr, "FAIL:UI_ITEMS_PROOF stage=menu_widen value=%u\n", m);
            ok = false;
            break;
        }
        menus[m] = nativeMenu;
        for (int32_t i = 0; i < wireMenu.itemCount; ++i)
            if (nativeMenu->items[i]->parent == nativeMenu)
                ++parents;
        if (unoracled)
        {
            std::printf("UI_ITEMS_UNORACLED index=%u name='%s' items=%u", m, menuName, wireMenu.itemCount);
            for (uint32_t t = 0; t < 23; ++t)
                if (menuTypes[t])
                    std::printf(" t%u=%u", t, menuTypes[t]);
            for (uint32_t s = 0; s < 8; ++s)
                if (menuStmtLocal[s])
                    std::printf(" s%u=%u", s, menuStmtLocal[s]);
            std::printf(" entries=%u handlers=%u\n", menuEntryTotal, menuHandlerTotal);
        }
    }
    if (ok)
    {
        FsRetailFastfileMenuList wireList{};
        wireList.nameRef = menuList.nameRef;
        wireList.menuCount = menuList.menuCount;
        MenuList *nativeList = nullptr;
        if (RetailDecodeMenuList(&session, &wireList, menus, 56, &nativeList) != RETAIL_UI_DECODE_OK ||
            !nativeList || nativeList->menuCount != 56)
        {
            std::fprintf(stderr, "FAIL:UI_ITEMS_PROOF stage=menulist_widen value=0\n");
            ok = false;
        }
    }
    std::printf("UI_ITEMS menus=56 items=%u widened=%u unsupported=%u typeskip=%u",
                items, widened, unsupported, typeskip);
    for (uint32_t t = 0; t < 23; ++t)
        std::printf(" t%u=%u", t, typeCount[t]);
    for (uint32_t s = 0; s < 8; ++s)
        std::printf(" s%u=%u", s, stmtCount[s]);
    std::printf(" mvis=%u mrx=%u mry=%u entries=%u handlers=%u parents=%u unoracled=%u",
                menuStmt[0], menuStmt[1], menuStmt[2], entriesTotal, handlersTotal, parents,
                unoracledMenus);
    std::printf(" maintext=%u mt0=%u mt1=%u mt8=%u", mainTextItems, mainTextType[0], mainTextType[1],
                mainTextType[8]);
    for (uint32_t s = 0; s < 8; ++s)
        std::printf(" mts%u=%u", s, mainTextStmt[s]);
    std::printf("\n");
    if (session.active)
        RetailZoneLoadSessionAbort(&session);
    delete[] block4;
    delete[] arenaBytes;
    FS_CloseRetailFastfile(fastfile);
    return ok;
}

// Investigation tool: full-policy load one zone and print every
// registered menu with its item count, then every item that drives an
// ownerdraw. This is the oracle-free way to locate the full-map menu
// (ownerdraw 180-187) and its background materials: OAT's Unlinker can list
// menu names but cannot dump menu bodies (its Unlinker writes no .menu
// files), and the per-zone menulists (ui/hud.txt etc.) are only reachable by
// walking the zone's own stream cursors, which this production path does.
bool RetailMenusSection(const char *zoneRel)
{
    SL_Init();
    RetailWalkLoadZoneResult result{};
    const RetailWalkLoadZoneResultCode code =
        RetailWalkLoadZoneAssets(zoneRel, &result, false, false);
    std::printf("MENUS_ZONE zone=%s code=%d assets=%u menuLists=%u failed=%u failedType=%u\n",
                zoneRel, (int)code, result.assetCount, result.registeredMenuListCount,
                result.failedOrdinal, result.failedType);
    if (code != RETAIL_WALK_LOAD_OK || result.code != RETAIL_WALK_LOAD_OK)
        return false;

    constexpr uint32_t kMaxMenuCollect = 4096;
    static XAssetHeader menus[kMaxMenuCollect];
    const uint32_t menuCount = StubCollectRegistered(ASSET_TYPE_MENU, menus, kMaxMenuCollect);
    std::printf("MENUS_TOTAL registered=%u\n", menuCount);
    uint32_t fullMapOwners = 0;
    for (uint32_t i = 0; i < menuCount; ++i)
    {
        const menuDef_t *menu = menus[i].menu;
        if (!menu || !menu->window.name)
            continue;
        uint32_t ownerdrawItems = 0;
        for (int32_t itemIndex = 0; itemIndex < menu->itemCount && menu->items; ++itemIndex)
            if (menu->items[itemIndex] && menu->items[itemIndex]->window.ownerDraw)
                ++ownerdrawItems;
        std::printf("MENUS_MENU name='%s' itemCount=%d ownerdrawItems=%u\n", menu->window.name,
                    menu->itemCount, ownerdrawItems);
        for (int32_t itemIndex = 0; itemIndex < menu->itemCount && menu->items; ++itemIndex)
        {
            const itemDef_s *item = menu->items[itemIndex];
            const int ownerDraw = item ? item->window.ownerDraw : 0;
            if (!ownerDraw)
                continue;
            const char *bgName =
                item->window.background ? item->window.background->info.name : nullptr;
            const char *action = item->action ? item->action : "";
            const char *onAccept = item->onAccept ? item->onAccept : "";
            std::printf("MENUS_OWNERDRAW menu='%s' i=%d type=%d ownerdraw=%d bg=%s "
                        "rect=%.1f,%.1f,%.1f,%.1f action='%s' onAccept='%s'\n",
                        menu->window.name, itemIndex, item->type, ownerDraw,
                        bgName ? bgName : "<null>", item->window.rect.x, item->window.rect.y,
                        item->window.rect.w, item->window.rect.h, action, onAccept);
            if (ownerDraw >= 180 && ownerDraw <= 187)
                ++fullMapOwners;
        }
    }
    std::printf("MENUS_FULLMAP_OWNERDRAW count=%u\n", fullMapOwners);
    return true;
}
} // namespace

// GameWorldSp node tree: the wire streams each tree record's inline u16 leaf
// list in slot order (db_load's Load_pathnode_tree_tArray, mirrored by the
// walk-only reader's ReadRetailPathNodeTreeNode), so a live decode that walks
// the tree depth-first runs its ReserveBlock4 calls in a different order and
// hands every leaf another leaf's node list.  Nothing fails loudly -- the tree
// still covers every node exactly once -- but Path_NodesInCylinder then prunes
// to leaves holding no node near the query, Path_NearestNode returns null and
// every actor reports "couldn't find path to goal" (killhouse: Gaz never
// moves, and gaz_in_idle_position never gets set, so his later
// gaz_animation() calls all early-return).  Two executable consequences of a
// correct decode, both checked on the real map's tree: every interior record's
// plane separates the nodes of its two children, and a descent from any node's
// own position reaches a leaf that holds it.
struct PathTreeCensus
{
    int nodes = 0;
    int leaves = 0;
    int interior = 0;
    int splitViolations = 0;
    int splitChecks = 0;
    int selfReach = 0;
};

static void PathTreeGather(const PathData &p, const pathnode_tree_t *t, std::vector<int> &out)
{
    if (!t)
        return;
    if (t->axis < 0)
    {
        for (int i = 0; i < t->u.s.nodeCount; ++i)
        {
            const unsigned idx = t->u.s.nodes ? t->u.s.nodes[i] : 0xffffu;
            if (idx < p.nodeCount)
                out.push_back(static_cast<int>(idx));
        }
        return;
    }
    PathTreeGather(p, t->u.child[0], out);
    PathTreeGather(p, t->u.child[1], out);
}

static void PathTreeStructures(const PathData &p, const pathnode_tree_t *t,
                               PathTreeCensus &census)
{
    if (!t)
        return;
    if (t->axis < 0)
    {
        ++census.leaves;
        for (int i = 0; i < t->u.s.nodeCount; ++i)
        {
            const unsigned idx = t->u.s.nodes ? t->u.s.nodes[i] : 0xffffu;
            if (idx < p.nodeCount)
                ++census.nodes;
        }
        return;
    }
    ++census.interior;
    if (t->axis <= 2)
    {
        std::vector<int> low, high;
        PathTreeGather(p, t->u.child[0], low);
        PathTreeGather(p, t->u.child[1], high);
        if (!low.empty() && !high.empty())
        {
            ++census.splitChecks;
            float lowMax = p.nodes[low[0]].constant.vOrigin[t->axis];
            float highMin = p.nodes[high[0]].constant.vOrigin[t->axis];
            for (int idx : low)
                lowMax = p.nodes[idx].constant.vOrigin[t->axis] > lowMax
                             ? p.nodes[idx].constant.vOrigin[t->axis]
                             : lowMax;
            for (int idx : high)
                highMin = p.nodes[idx].constant.vOrigin[t->axis] < highMin
                              ? p.nodes[idx].constant.vOrigin[t->axis]
                              : highMin;
            if (!(lowMax <= t->dist && t->dist <= highMin))
                ++census.splitViolations;
        }
    }
    PathTreeStructures(p, t->u.child[0], census);
    PathTreeStructures(p, t->u.child[1], census);
}

// The engine's Path_NodesInCylinder_r descent, verbatim (including the
// two-sided visit when the query is within maxDist of a split plane).
static void PathTreeCollectCylinder(const pathnode_tree_t *t, const float *origin, float maxDist,
                                    std::vector<int> &out)
{
    if (!t)
        return;
    if (t->axis < 0)
    {
        for (int i = 0; i < t->u.s.nodeCount; ++i)
            out.push_back(static_cast<int>(t->u.s.nodes ? t->u.s.nodes[i] : 0xffffu));
        return;
    }
    const float dist = origin[t->axis] - t->dist;
    if (maxDist < dist)
    {
        PathTreeCollectCylinder(t->u.child[1], origin, maxDist, out);
        return;
    }
    if (-maxDist <= dist)
    {
        PathTreeCollectCylinder(t->u.child[0], origin, maxDist, out);
        PathTreeCollectCylinder(t->u.child[1], origin, maxDist, out);
        return;
    }
    PathTreeCollectCylinder(t->u.child[0], origin, maxDist, out);
}

static void PathTreeSelfReach(const PathData &p, PathTreeCensus &census)
{
    std::vector<int> reached;
    for (uint32_t i = 0; i < p.nodeCount; ++i)
    {
        reached.clear();
        PathTreeCollectCylinder(p.nodeTree, p.nodes[i].constant.vOrigin, 192.0f, reached);
        for (int idx : reached)
        {
            if (idx == static_cast<int>(i))
            {
                ++census.selfReach;
                break;
            }
        }
    }
}

static bool ExportCharacterSkinInputs()
{
    const char *dir = std::getenv("KISAK_CHARACTER_SKIN_EXPORT_DIR");
    if (!dir) return true;
    if (!useFastFile)
        useFastFile = Dvar_RegisterBool("useFastFile", true, DVAR_NOFLAG, "Use fastfile assets");
    static_assert(sizeof(DObjSkelMat) == 64 && sizeof(GfxPackedVertex) == 32);
    const char *names[] = {"head_sp_sas_woodland_hugh", "head_sp_sas_woodland_zied",
        "head_sp_sas_woodland_peter", "head_sp_sas_woodland_mac", "head_sp_sas_woodland_todd",
        "body_sp_sas_woodland_support_a", "body_sp_sas_woodland_assault_a",
        "body_sp_sas_woodland_assault_b", "body_complete_sp_sas_ct_price_maskup",
        "body_complete_sp_sas_woodland_gaz"};
    for (const char *name : names)
    {
        const XModel *model = StubFindXAssetHeader(ASSET_TYPE_XMODEL, name).model;
        if (!model) continue;
        auto invalid = [&](uint32_t surface, const char *reason) {
            std::printf("CHARACTER_SKIN_EXPORT_FAIL model=%s surface=%u bones=%u surfaces=%u reason=%s\n",
                        name, surface, model->numBones, model->numsurfs, reason);
            return false;
        };
        if (!model->numBones || model->numBones > 128 || !model->surfs || !model->numsurfs)
            return invalid(0, "model_counts_or_surfaces");
        if (model->numLods < 1 || model->numLods > 4 || !model->materialHandles)
            return invalid(0, "lod_count_or_materials");
        for (int lod = 0; lod < model->numLods; ++lod)
        {
            const XModelLodInfo &info = model->lodInfo[lod];
            if (!std::isfinite(info.dist) || info.dist < 0 ||
                uint32_t(info.surfIndex) + info.numsurfs > model->numsurfs)
                return invalid(lod, "lod_range_or_distance");
            XSurface *selected = nullptr;
            if (XModelGetSurfaces(model, &selected, lod) != info.numsurfs ||
                selected != model->surfs + info.surfIndex ||
                XModelGetSkins(model, lod) != model->materialHandles + info.surfIndex)
                return invalid(lod, "lod_surface_material_selection");
            std::printf("CHARACTER_LOD model=%s lod=%d distance=%.9g first=%u surfaces=%u\n",
                        name, lod, info.dist, info.surfIndex, info.numsurfs);
            for (uint32_t s = info.surfIndex; s < uint32_t(info.surfIndex) + info.numsurfs; ++s)
            {
                const Material *material = model->materialHandles[s];
                if (!material) return invalid(s, "lod_material_null");
                std::printf("CHARACTER_LOD_SURFACE model=%s lod=%d surface=%u material=%s vertices=%u rigid=%u\n",
                            name, lod, s, material->info.name, model->surfs[s].vertCount,
                            model->surfs[s].vertListCount);
            }
            // An isolated threshold advances at equality; zero is terminal.
            if (info.dist == 0)
            {
                if (XModelGetLodForDist(model, 1000000.0f) != lod)
                    return invalid(lod, "lod_zero_terminal");
            }
            else if ((lod == 0 || (model->lodInfo[lod - 1].dist > 0 &&
                       model->lodInfo[lod - 1].dist < info.dist)) &&
                     (lod + 1 == model->numLods || model->lodInfo[lod + 1].dist == 0 ||
                       model->lodInfo[lod + 1].dist > info.dist))
            {
                const int after = lod + 1 == model->numLods ? -1 : lod + 1;
                if (XModelGetLodForDist(model, std::nextafter(info.dist, -INFINITY)) != lod ||
                    XModelGetLodForDist(model, info.dist) != after ||
                    XModelGetLodForDist(model, std::nextafter(info.dist, INFINITY)) != after)
                    return invalid(lod, "lod_threshold_boundary");
            }
        }
        std::vector<DObjSkelMat> bones(model->numBones);
        for (uint32_t b = 0; b < model->numBones; ++b)
        {
            bones[b].axis[0][0] = bones[b].axis[1][1] = bones[b].axis[2][2] = 1.0f;
            bones[b].origin[0] = float(b); bones[b].origin[1] = 2.0f * b;
            bones[b].origin[2] = -float(b);
            bones[b].origin[3] = 1.0f;
        }
        for (uint32_t s = 0; s < model->numsurfs; ++s)
        {
            const XSurface &surf = model->surfs[s];
            if (!surf.deformed && surf.vertListCount == 1)
            {
                if (!model->baseMat || !useFastFile->current.enabled)
                    return invalid(s, "rigid_pose_input");
                for (uint32_t boneOffset : {0u, 52u})
                {
                    if (boneOffset + model->numBones > 128) continue;
                    std::vector<DObjAnimMat> pose(boneOffset + model->numBones);
                    std::memcpy(pose.data() + boneOffset, model->baseMat,
                                model->numBones * sizeof(DObjAnimMat));
                    DObj_s object{};
                    object.skel.mat = pose.data();
                    GfxModelSurfaceInfo info{};
                    info.boneIndex = boneOffset;
                    info.boneCount = model->numBones;
                    info.baseMat = model->baseMat;
                    GfxModelRigidSurface output{};
                    uint32_t vertices = 0;
                    if (R_PreSkinXSurface(&object, &model->surfs[s], &info, &vertices, &output.surf) !=
                            sizeof(output) || output.surf.skinnedCachedOffset != -2 || vertices)
                        return invalid(s, "rigid_record_selection");
                    for (int axis = 0; axis < 3; ++axis)
                        if (std::fabs(output.placement.base.origin[axis]) > 0.002f ||
                            std::fabs(output.placement.base.quat[axis]) > 0.0001f)
                            return invalid(s, "rigid_bind_pose_transform");
                    if (std::fabs(std::fabs(output.placement.base.quat[3]) - 1.0f) > 0.0001f)
                        return invalid(s, "rigid_bind_pose_rotation");
                }
                std::printf("CHARACTER_RIGID_BIND model=%s surface=%u bone=%u\n",
                            name, s, surf.vertList->boneOffset / 64u);
            }
            uint32_t header[10] = {0x534b494e, model->numBones, surf.vertCount,
                                  uint32_t(surf.deformed), surf.vertListCount, 0, 0, 0, 0, 0};
            if (!surf.vertCount || !surf.verts0 || surf.vertListCount > surf.vertCount ||
                (surf.vertListCount && !surf.vertList)) return invalid(s, "surface_counts_or_data");
            uint32_t weighted = 0;
            for (uint32_t w = 0; w < 4; ++w)
            {
                if (surf.vertInfo.vertCount[w] < 0) return invalid(s, "negative_blend_count");
                header[6 + w] = surf.vertInfo.vertCount[w];
                weighted += header[6 + w];
                header[5] += header[6 + w] * (2 * w + 1);
            }
            if (surf.deformed && (weighted != surf.vertCount || !surf.vertInfo.vertsBlend))
                return invalid(s, "deformed_blend_count_or_data");
            if (!surf.deformed && weighted) return invalid(s, "rigid_has_blends");
            uint32_t blend = 0;
            for (uint32_t w = 0; w < 4; ++w)
                for (uint32_t v = 0; v < header[6 + w]; ++v)
                {
                    for (uint32_t b = 0; b <= w; ++b)
                    {
                        const uint32_t offset = surf.vertInfo.vertsBlend[blend + (b ? 2 * b - 1 : 0)];
                        if (offset % 64 || offset >= model->numBones * 64u)
                            return invalid(s, "blend_bone_offset");
                    }
                    blend += 2 * w + 1;
                }
            std::vector<uint16_t> rigid;
            uint32_t rigidVerts = 0;
            for (uint32_t r = 0; r < surf.vertListCount; ++r)
            {
                const XRigidVertList &part = surf.vertList[r];
                if (part.boneOffset % 64 || part.boneOffset >= model->numBones * 64u)
                    return invalid(s, "rigid_bone_offset");
                rigidVerts += part.vertCount;
                rigid.push_back(part.boneOffset); rigid.push_back(part.vertCount);
            }
            if (!surf.deformed && rigidVerts != surf.vertCount) return invalid(s, "rigid_vertex_count");
            char path[4096];
            const int len = std::snprintf(path, sizeof(path), "%s/%s_%u.skin", dir, name, s);
            if (len < 0 || size_t(len) >= sizeof(path)) return invalid(s, "path_length");
            FILE *out = std::fopen(path, "wb");
            if (!out) return invalid(s, "open_output");
            auto write = [&](const void *p, size_t bytes) { return !bytes || std::fwrite(p, 1, bytes, out) == bytes; };
            const bool ok = write(header, sizeof(header)) && write(bones.data(), bones.size() * 64) &&
                write(surf.verts0, surf.vertCount * 32u) &&
                write(surf.vertInfo.vertsBlend, header[5] * sizeof(uint16_t)) &&
                write(rigid.data(), rigid.size() * sizeof(uint16_t));
            const bool closed = std::fclose(out) == 0;
            if (!ok || !closed) { std::remove(path); return invalid(s, "write_output"); }
            std::printf("CHARACTER_SKIN_EXPORT model=%s surface=%u bones=%u vertices=%u deformed=%u rigid=%u blends=%u\n",
                        name, s, model->numBones, surf.vertCount, surf.deformed, surf.vertListCount, header[5]);
        }
    }
    const char *bodies[] = {"body_sp_sas_woodland_support_a",
        "body_sp_sas_woodland_assault_a", "body_sp_sas_woodland_assault_b"};
    for (const char *bodyName : bodies)
        for (int h = 0; h < 5; ++h)
        {
            XModel *models[2] = {StubFindXAssetHeader(ASSET_TYPE_XMODEL, bodyName).model,
                                StubFindXAssetHeader(ASSET_TYPE_XMODEL, names[h]).model};
            if (!models[0] || !models[1]) continue;
            if (uint32_t(models[0]->numBones) + models[1]->numBones > 128)
                return false;
            DObj_s object{};
            object.models = models;
            object.numModels = 2;
            int cases = 0;
            for (int b = -1; b < models[0]->numLods; ++b)
                for (int head = -1; head < models[1]->numLods; ++head)
                {
                    int8_t lods[2] = {int8_t(b), int8_t(head)};
                    int bits[4]{};
                    uint32_t expected[4]{};
                    uint32_t offset = 0, count = 0;
                    for (int m = 0; m < 2; ++m)
                    {
                        if (lods[m] >= 0)
                        {
                            const XModelLodInfo &info = models[m]->lodInfo[lods[m]];
                            count += info.numsurfs;
                            for (uint32_t bone = 0; bone < 128 && bone + offset < 128; ++bone)
                                if (uint32_t(info.partBits[bone >> 5]) & (0x80000000u >> (bone & 31)))
                                    expected[(bone + offset) >> 5] |= 0x80000000u >> ((bone + offset) & 31);
                        }
                        offset += models[m]->numBones;
                    }
                    if (DObjGetSurfaces(&object, bits, lods) != count ||
                        std::memcmp(bits, expected, sizeof(bits)))
                    {
                        std::printf("CHARACTER_LOD_MASK_FAIL body=%s head=%s lods=%d,%d\n",
                                    bodyName, names[h], b, head);
                        return false;
                    }
                    ++cases;
                }
            std::printf("CHARACTER_LOD_MASK body=%s head=%s boneOffset=%u cases=%d\n",
                        bodyName, names[h], models[0]->numBones, cases);
        }
    return true;
}

int main(int argc, char **argv)
{
    // zone-batch and deko-shader-dump take a variable number of zones.
    const bool variadicZones = argc >= 4 && (!std::strcmp(argv[2], "zone-batch") ||
                                         !std::strcmp(argv[2], "deko-shader-dump") ||
                                         !std::strcmp(argv[2], "deko-variant-passes"));
    if (!variadicZones && argc != 2 && argc != 3 && argc != 4)
    {
        std::fprintf(stderr,
                     "usage: %s <fixture-root> [killhouse|menus <zoneRel>|"
                     "zone-load <zoneRel>|zone-batch <closureCsv> <zoneRel>...]\n",
                     argv[0]);
        return 1;
    }
    FS_InitRetailSource(argv[1]);

    // Slice A standalone mode: widen every real ui.ff menu item (see
    // RetailUiItemsSection). Other modes run the full sequence below.
    if (argc == 3 && !std::strcmp(argv[2], "ui-items"))
        return RetailUiItemsSection() ? 0 : 1;
    if (argc == 4 && !std::strcmp(argv[2], "menus"))
        return RetailMenusSection(argv[3]) ? 0 : 1;

    // Dump every unique vertex/pixel shader program of one zone's technique
    // sets, keyed by the deko3d renderer's cache hash (FNV-1a 64 over the
    // bytecode). Files go to KISAK_DEKO_EXTRACT_DIR, outside the repository:
    // they are user game data and feed the shader-translation corpus test.
    if (argc >= 4 && !std::strcmp(argv[2], "deko-shader-dump"))
    {
        const char *extractDir = std::getenv("KISAK_DEKO_EXTRACT_DIR");
        if (!extractDir)
        {
            std::fprintf(stderr, "DEKO_SHADER_DUMP_FAIL KISAK_DEKO_EXTRACT_DIR unset\n");
            return 1;
        }
        SL_Init();
        static GfxImage identityNormalMap{};
        identityNormalMap.mapType = MAPTYPE_2D;
        identityNormalMap.width = identityNormalMap.height = identityNormalMap.depth = 1;
        identityNormalMap.name = "$identitynormalmap";
        StubRegisterStockAsset(ASSET_TYPE_IMAGE, XAssetHeader(&identityNormalMap));
        // Zones load in argument order so later zones resolve references
        // into earlier ones (code_post_gfx, common, then the level).
        std::vector<uint32_t> zoneIndices;
        for (int z = 3; z < argc; ++z)
        {
            RetailWalkLoadZoneResult result{};
            const RetailWalkLoadZoneResultCode code = RetailWalkLoadZoneAssets(argv[z], &result);
            if (code != RETAIL_WALK_LOAD_OK)
            {
                std::fprintf(stderr, "DEKO_SHADER_DUMP_LOAD_FAIL zone=%s code=%d ordinal=%u type=%u\n",
                             argv[z], (int)code, result.failedOrdinal, result.failedType);
                return 1;
            }
            zoneIndices.push_back(result.zoneIndex);
        }
        const uint32_t count = StubCollectRegistered(ASSET_TYPE_TECHNIQUE_SET, nullptr, 0);
        std::vector<XAssetHeader> techsets(count);
        StubCollectRegistered(ASSET_TYPE_TECHNIQUE_SET, techsets.data(), count);
        uint32_t written = 0, seen = 0;
        bool ok = true;
        const auto dump = [&](const char *ext, const char *name, const void *program,
                              uint32_t words) {
            if (!program || !words)
                return;
            ++seen;
            uint64_t hash = 0xcbf29ce484222325ull;
            const uint8_t *bytes = static_cast<const uint8_t *>(program);
            for (uint32_t i = 0; i < words * 4u; ++i)
                hash = (hash ^ bytes[i]) * 0x100000001b3ull;
            char path[512];
            if (std::snprintf(path, sizeof(path), "%s/%016llx.%s", extractDir,
                              (unsigned long long)hash, ext) >= (int)sizeof(path))
            {
                ok = false;
                return;
            }
            if (FILE *existing = std::fopen(path, "rb"))
            {
                std::fclose(existing);
                return;
            }
            FILE *file = std::fopen(path, "wb");
            if (!file)
            {
                ok = false;
                return;
            }
            ok = std::fwrite(program, 4, words, file) == words && ok;
            ok = std::fclose(file) == 0 && ok;
            ++written;
            std::printf("DEKO_SHADER %s %016llx %s words=%u\n", ext,
                        (unsigned long long)hash, name ? name : "", words);
        };
        for (const XAssetHeader &header : techsets)
        {
            const MaterialTechniqueSet *set = header.techniqueSet;
            if (!set)
                continue;
            for (int technique = 0; technique < TECHNIQUE_COUNT; ++technique)
            {
                const MaterialTechnique *t = set->techniques[technique];
                if (!t)
                    continue;
                for (uint32_t p = 0; p < t->passCount; ++p)
                {
                    const MaterialPass &pass = t->passArray[p];
                    if (pass.vertexShader)
                        dump("vs", pass.vertexShader->name,
                             pass.vertexShader->prog.loadDef.program,
                             pass.vertexShader->prog.loadDef.programSize);
                    if (pass.pixelShader)
                        dump("ps", pass.pixelShader->name,
                             pass.pixelShader->prog.loadDef.program,
                             pass.pixelShader->prog.loadDef.programSize);
                }
            }
        }
        std::printf("DEKO_SHADER_DUMP zone=%s techsets=%u programs=%u new=%u ok=%u\n",
                    argv[argc - 1], count, seen, written, ok ? 1u : 0u);
        for (auto it = zoneIndices.rbegin(); it != zoneIndices.rend(); ++it)
            ok = DB_RetailZoneEnd(*it) && ok;
        return ok ? 0 : 1;
    }

    // Shader variant prebake census: every material pass of the zones (in
    // argument order: code_post_gfx, ui, common, then the level) exactly as the
    // zone-load prebake describes it (switch_shader_prebake.h), one
    // DEKO_VARIANT_PASS line each, shaders keyed by the deko3d renderer's
    // bytecode hash. Programs the corpus lacks are written to
    // KISAK_DEKO_EXTRACT_DIR (user game data, outside the repository).
    if (argc >= 4 && !std::strcmp(argv[2], "deko-variant-passes"))
    {
        const char *extractDir = std::getenv("KISAK_DEKO_EXTRACT_DIR");
        if (!extractDir)
        {
            std::fprintf(stderr, "DEKO_VARIANT_PASSES_FAIL KISAK_DEKO_EXTRACT_DIR unset\n");
            return 1;
        }
        SL_Init();
        static GfxImage identityNormalMap{};
        identityNormalMap.mapType = MAPTYPE_2D;
        identityNormalMap.width = identityNormalMap.height = identityNormalMap.depth = 1;
        identityNormalMap.name = "$identitynormalmap";
        StubRegisterStockAsset(ASSET_TYPE_IMAGE, XAssetHeader(&identityNormalMap));
        std::vector<uint32_t> zoneIndices;
        for (int z = 3; z < argc; ++z)
        {
            RetailWalkLoadZoneResult result{};
            const RetailWalkLoadZoneResultCode code = RetailWalkLoadZoneAssets(argv[z], &result);
            if (code != RETAIL_WALK_LOAD_OK)
            {
                std::fprintf(stderr, "DEKO_VARIANT_PASSES_LOAD_FAIL zone=%s code=%d ordinal=%u type=%u\n",
                             argv[z], (int)code, result.failedOrdinal, result.failedType);
                return 1;
            }
            zoneIndices.push_back(result.zoneIndex);
        }
        bool ok = true;
        const auto programHash = [&](const char *ext, const void *program, uint32_t words) -> uint64_t {
            if (!program || !words)
                return 0;
            uint64_t hash = 0xcbf29ce484222325ull;
            const uint8_t *bytes = static_cast<const uint8_t *>(program);
            for (uint32_t i = 0; i < words * 4u; ++i)
                hash = (hash ^ bytes[i]) * 0x100000001b3ull;
            char path[512];
            if (std::snprintf(path, sizeof(path), "%s/%016llx.%s", extractDir, (unsigned long long)hash, ext) >=
                (int)sizeof(path))
            {
                ok = false;
                return hash;
            }
            if (FILE *existing = std::fopen(path, "rb"))
            {
                std::fclose(existing);
                return hash;
            }
            FILE *file = std::fopen(path, "wb");
            if (!file)
            {
                ok = false;
                return hash;
            }
            ok = std::fwrite(program, 4, words, file) == words && ok;
            ok = std::fclose(file) == 0 && ok;
            return hash;
        };
        // The runtime feature remap (Material_RuntimeRemapTarget) for the
        // Switch configuration: shader model 3, every feature dvar on,
        // hardware shadow maps. Only the shadow token then changes: the
        // "sm"/"hsm" feature maps to "hsm" (Material_RemapTechniqueSetName's
        // tokenizer: '_' separates, a digit-to-letter boundary splits).
        const uint32_t techsetCount = StubCollectRegistered(ASSET_TYPE_TECHNIQUE_SET, nullptr, 0);
        std::vector<XAssetHeader> techsets(techsetCount);
        StubCollectRegistered(ASSET_TYPE_TECHNIQUE_SET, techsets.data(), techsetCount);
        const auto remapTarget = [&](const MaterialTechniqueSet *set) -> const MaterialTechniqueSet * {
            if (!set || !set->name)
                return set;
            std::string name, token;
            const char *parse = set->name;
            if (!std::strncmp(parse, "sm2/", 4))
                parse += 4;
            const auto flush = [&](bool underscore) {
                if (token.empty())
                    return;
                if (underscore && !name.empty())
                    name += '_';
                name += token == "sm" ? "hsm" : token;
                token.clear();
            };
            bool underscore = false;
            for (; *parse; ++parse)
            {
                if (*parse == '_')
                {
                    flush(underscore);
                    underscore = true;
                    continue;
                }
                if (!token.empty() && std::isdigit((unsigned char)token.back()) &&
                    !std::isdigit((unsigned char)*parse))
                {
                    flush(underscore);
                    underscore = false;
                }
                token += *parse;
            }
            flush(underscore);
            if (name == set->name)
                return set;
            for (const XAssetHeader &t : techsets)
                if (t.techniqueSet && t.techniqueSet->name && name == t.techniqueSet->name)
                    return t.techniqueSet;
            return set;
        };
        const uint32_t count = StubCollectRegistered(ASSET_TYPE_MATERIAL, nullptr, 0);
        std::vector<XAssetHeader> materials(count);
        StubCollectRegistered(ASSET_TYPE_MATERIAL, materials.data(), count);
        uint32_t passes = 0, remapped = 0;
        for (const XAssetHeader &header : materials)
        {
            const Material *material = header.material;
            if (!material || !material->techniqueSet)
                continue;
            const MaterialTechniqueSet *target = remapTarget(material->techniqueSet);
            remapped += target != material->techniqueSet;
            const MaterialTechniqueSet *sets[2] = {material->techniqueSet,
                                                   target != material->techniqueSet ? target : nullptr};
            for (const MaterialTechniqueSet *set : sets)
                R_ForEachPrebakePass(material, set, true, true, [&](const PrebakePassInputs &in) {
                const MaterialPass &pass = *in.pass;
                const uint64_t vs = pass.vertexShader ? programHash("vs", pass.vertexShader->prog.loadDef.program,
                                                                    pass.vertexShader->prog.loadDef.programSize)
                                                      : 0;
                const uint64_t ps = pass.pixelShader ? programHash("ps", pass.pixelShader->prog.loadDef.program,
                                                                   pass.pixelShader->prog.loadDef.programSize)
                                                     : 0;
                if (!vs && !ps)
                    return;
                char regs[16 * 4 + 1] = "";
                int at = 0;
                for (uint32_t r = 0; r < in.instanceRegCount; ++r)
                    at += std::snprintf(regs + at, sizeof(regs) - (size_t)at, r ? ",%u" : "%u",
                                        (unsigned)in.instanceRegs[r]);
                std::printf("DEKO_VARIANT_PASS vs=%016llx ps=%016llx depth=0x%x atest=%u inst=%u:%s\n",
                            (unsigned long long)vs, (unsigned long long)ps, in.depthSamplerMask,
                            in.alphaTest ? 1u : 0u, in.instanceRegCount, regs);
                ++passes;
            });
        }
        std::printf("DEKO_VARIANT_PASSES zone=%s materials=%u remapped=%u passes=%u ok=%u\n", argv[argc - 1], count,
                    remapped, passes, ok ? 1u : 0u);
        for (auto it = zoneIndices.rbegin(); it != zoneIndices.rend(); ++it)
            ok = DB_RetailZoneEnd(*it) && ok;
        return ok ? 0 : 1;
    }

    // Inspect a loaded retail pass through the production zone loader. The
    // output contains metadata only; shader bytes stay in the user's data.
    if (argc == 4 && !std::strcmp(argv[2], "deko-material-list"))
    {
        const char *extractDir = std::getenv("KISAK_DEKO_EXTRACT_DIR");
        const char *extractMaterial = std::getenv("KISAK_DEKO_MATERIAL");
        SL_Init();
        static GfxImage identityNormalMap{};
        identityNormalMap.mapType = MAPTYPE_2D;
        identityNormalMap.width = identityNormalMap.height = identityNormalMap.depth = 1;
        identityNormalMap.name = "$identitynormalmap";
        StubRegisterStockAsset(ASSET_TYPE_IMAGE, XAssetHeader(&identityNormalMap));
        RetailWalkLoadZoneResult result{};
        const RetailWalkLoadZoneResultCode code = RetailWalkLoadZoneAssets(argv[3], &result);
        if (code != RETAIL_WALK_LOAD_OK)
        {
            std::fprintf(stderr, "DEKO_MATERIAL_LOAD_FAIL zone=%s code=%d ordinal=%u type=%u\n",
                         argv[3], (int)code, result.failedOrdinal, result.failedType);
            return 1;
        }
        const uint32_t count = StubCollectRegistered(ASSET_TYPE_MATERIAL, nullptr, 0);
        std::vector<XAssetHeader> materials(count);
        StubCollectRegistered(ASSET_TYPE_MATERIAL, materials.data(), count);
        uint32_t candidates = 0;
        for (const XAssetHeader &header : materials)
        {
            const Material *material = header.material;
            if (!material || !material->textureCount || !material->textureTable ||
                !material->techniqueSet)
                continue;
            for (int technique = 0; technique < TECHNIQUE_COUNT; ++technique)
            {
                const MaterialTechnique *t = material->techniqueSet->techniques[technique];
                if (!t || !t->passCount)
                    continue;
                const MaterialPass &pass = t->passArray[0];
                if (!pass.vertexShader || !pass.pixelShader ||
                    !pass.vertexShader->prog.loadDef.program ||
                    !pass.pixelShader->prog.loadDef.program)
                    continue;
                const GfxImage *image = MaterialTextureImage(material->textureTable[0]);
                if (!image)
                    continue;
                std::printf("DEKO_CANDIDATE material=%s technique=%d pass=%s vs=%s vsWords=%u ps=%s psWords=%u image=%s dims=%ux%u fmt=%u bytes=%d\n",
                            material->info.name ? material->info.name : "",
                            technique, t->name ? t->name : "",
                            pass.vertexShader->name ? pass.vertexShader->name : "",
                            pass.vertexShader->prog.loadDef.programSize,
                            pass.pixelShader->name ? pass.pixelShader->name : "",
                            pass.pixelShader->prog.loadDef.programSize,
                            image->name ? image->name : "", image->width, image->height,
                            image->texture.loadDef ? (unsigned)image->texture.loadDef->format : 0u,
                            image->texture.loadDef ? image->texture.loadDef->resourceSize : 0);
                if (extractDir && extractMaterial && material->info.name &&
                    !std::strcmp(material->info.name, extractMaterial))
                {
                    const auto writeShader = [extractDir](const char *filename,
                        const void *program, uint16_t words) -> bool {
                        char path[512];
                        if (std::snprintf(path, sizeof(path), "%s/%s", extractDir, filename) >=
                            (int)sizeof(path))
                            return false;
                        FILE *file = std::fopen(path, "wb");
                        if (!file)
                            return false;
                        const bool ok = std::fwrite(program, sizeof(uint32_t), words, file) == words;
                        return std::fclose(file) == 0 && ok;
                    };
                    const bool extracted =
                        writeShader("vertex.dxso", pass.vertexShader->prog.loadDef.program,
                                    pass.vertexShader->prog.loadDef.programSize) &&
                        writeShader("pixel.dxso", pass.pixelShader->prog.loadDef.program,
                                    pass.pixelShader->prog.loadDef.programSize);
                    bool textureExtracted = false;
                    if (image->texture.loadDef && image->texture.loadDef->resourceSize == 4 &&
                        image->texture.loadDef->format == D3DFMT_A8R8G8B8 &&
                        image->width == 1 && image->height == 1)
                    {
                        char path[512];
                        if (std::snprintf(path, sizeof(path), "%s/texture.bgra", extractDir) <
                            (int)sizeof(path))
                        {
                            FILE *file = std::fopen(path, "wb");
                            if (file)
                            {
                                textureExtracted = std::fwrite(image->texture.loadDef->data,
                                    1, 4, file) == 4;
                                textureExtracted = std::fclose(file) == 0 && textureExtracted;
                            }
                        }
                    }
                    std::printf("DEKO_EXTRACT material=%s ok=%u\n", extractMaterial,
                                extracted ? 1u : 0u);
                    std::printf("DEKO_TEXTURE_EXTRACT material=%s ok=%u\n", extractMaterial,
                                textureExtracted ? 1u : 0u);
                    if (!extracted || !textureExtracted)
                        return 1;
                }
                ++candidates;
                break;
            }
        }
        std::printf("DEKO_CANDIDATE_COUNT zone=%s materials=%u candidates=%u\n",
                    argv[3], count, candidates);
        return DB_RetailZoneEnd(result.zoneIndex) && candidates ? 0 : 1;
    }

    // sweep #1 (whole-corpus zone sweep): one zone through the
    // production full-policy loader in a fresh process, then the engine's
    // index-based zone end. Prints CORPUS_ZONE/CORPUS_UNLOAD facts; emits no PASS itself and exits
    // nonzero on any load failure, live-zone residue, or nonzero retired
    // pointer count. The stock identity-normal-map image stands in for the
    // R_InitImages side effect a real boot has already produced before any
    // zone loads, matching the bounded section below.
    if (argc == 4 && !std::strcmp(argv[2], "zone-load"))
    {
        // Production calls SL_Init once at boot via Scr_Init long before any
        // zone loads (same precondition as the menus mode below).
        SL_Init();
        static GfxImage identityNormalMap{};
        identityNormalMap.mapType = MAPTYPE_2D;
        identityNormalMap.width = 1;
        identityNormalMap.height = 1;
        identityNormalMap.depth = 1;
        identityNormalMap.name = "$identitynormalmap";
        StubRegisterStockAsset(ASSET_TYPE_IMAGE, XAssetHeader(&identityNormalMap));

        const uint32_t registeredBefore = StubRegisteredCount();
        RetailWalkLoadZoneResult result{};
        const RetailWalkLoadZoneResultCode code =
            RetailWalkLoadZoneAssets(argv[3], &result);
        const uint32_t registered = StubRegisteredCount() - registeredBefore;
        std::printf("CORPUS_ZONE ff=%s code=%d assets=%u registered=%u walked=%u "
                    "failedOrdinal=%u failedType=%u live=%u\n",
                    argv[3], (int)code, result.assetCount, registered,
                    result.walkedOnlyCount, result.failedOrdinal, result.failedType,
                    code == RETAIL_WALK_LOAD_OK && StubZoneIsLive(result.zoneIndex) ? 1u : 0u);
        if (code != RETAIL_WALK_LOAD_OK)
            return 1;
        const bool ended = DB_RetailZoneEnd(result.zoneIndex);
        const uint32_t retired = StubRetiredPtrCount();
        std::printf("CORPUS_UNLOAD ff=%s ended=%u live=%u retired=%u world=%u\n",
                    argv[3], ended ? 1u : 0u,
                    StubZoneIsLive(result.zoneIndex) ? 1u : 0u, retired,
                    StubWorldInUse());
        return ended && !StubZoneIsLive(result.zoneIndex) && retired == 0 ? 0 : 1;
    }

    // sweep #1 batch form: load the shared boot closure once (argv[3],
    // comma-separated), then load+unload each target zone in turn (argv[4..]),
    // then unload the closure in reverse. This matches the real lifecycle --
    // code_post_gfx -> ui -> common stay live while each map loads -- and
    // keeps each map's own load/unload inside one ASan process, which the
    // single-zone mode cannot do for zones whose material aliases resolve
    // through the earlier boot zones. Prints CORPUS_ZONE/CORPUS_UNLOAD for
    // every load and unload; emits no PASS itself.
    if (argc >= 4 && !std::strcmp(argv[2], "zone-batch"))
    {
        SL_Init();
        static GfxImage identityNormalMap{};
        identityNormalMap.mapType = MAPTYPE_2D;
        identityNormalMap.width = 1;
        identityNormalMap.height = 1;
        identityNormalMap.depth = 1;
        identityNormalMap.name = "$identitynormalmap";
        StubRegisterStockAsset(ASSET_TYPE_IMAGE, XAssetHeader(&identityNormalMap));

        constexpr uint32_t kMaxLive = 256;
        uint32_t liveIndex[kMaxLive]{};
        const char *liveRel[kMaxLive]{};
        uint32_t liveCount = 0;
        uint32_t failures = 0;

        auto loadZone = [&](const char *rel) -> bool
        {
            const uint32_t registeredBefore = StubRegisteredCount();
            RetailWalkLoadZoneResult result{};
            const RetailWalkLoadZoneResultCode code =
                RetailWalkLoadZoneAssets(rel, &result);
            const uint32_t registered = StubRegisteredCount() - registeredBefore;
            std::printf("CORPUS_ZONE ff=%s code=%d assets=%u registered=%u walked=%u "
                        "failedOrdinal=%u failedType=%u live=%u zoneIndex=%u\n",
                        rel, (int)code, result.assetCount, registered,
                        result.walkedOnlyCount, result.failedOrdinal, result.failedType,
                        code == RETAIL_WALK_LOAD_OK && StubZoneIsLive(result.zoneIndex) ? 1u : 0u,
                        result.zoneIndex);
            if (code != RETAIL_WALK_LOAD_OK)
                return false;
            if (!ExportCharacterSkinInputs()) return false;
            {
                uint32_t fxEffects = 0, fxElems = 0;
                const char *firstBad = nullptr;
                int firstBadElem = -1;
                const uint32_t badVel =
                    StubFxVelIntervalZeroCount(&fxEffects, &fxElems, &firstBad, &firstBadElem);
                std::printf("CORPUS_FX_VEL ff=%s effects=%u elems=%u vel_samples_zero_intervals=%u first=%s elem=%d\n",
                            rel, fxEffects, fxElems, badVel, firstBad ? firstBad : "-", firstBadElem);
            }
            if (liveCount >= kMaxLive)
            {
                std::printf("CORPUS_OVERFLOW ff=%s\n", rel);
                return false;
            }
            liveIndex[liveCount] = result.zoneIndex;
            liveRel[liveCount] = rel;
            ++liveCount;
            return true;
        };
        auto unloadTop = [&](const char *rel) -> void
        {
            if (liveCount == 0)
                return;
            --liveCount;
            const uint32_t index = liveIndex[liveCount];
            const bool ended = DB_RetailZoneEnd(index);
            const uint32_t retired = StubRetiredPtrCount();
            std::printf("CORPUS_UNLOAD ff=%s ended=%u live=%u retired=%u world=%u\n", rel,
                        ended ? 1u : 0u, StubZoneIsLive(index) ? 1u : 0u, retired,
                        StubWorldInUse());
            if (!ended || StubZoneIsLive(index) || retired != 0)
                ++failures;
        };

        char closure[4096];
        std::strncpy(closure, argv[3], sizeof(closure) - 1);
        closure[sizeof(closure) - 1] = '\0';
        bool closureOk = true;
        for (char *part = closure; part && *part && closureOk;)
        {
            char *comma = std::strchr(part, ',');
            if (comma)
                *comma = '\0';
            closureOk = loadZone(part);
            part = comma ? comma + 1 : nullptr;
        }
        if (!closureOk)
            return 1;
        for (int i = 4; i < argc; ++i)
        {
            if (!loadZone(argv[i]))
            {
                ++failures;
                continue;
            }
            unloadTop(liveRel[liveCount - 1]);
        }
        while (liveCount > 0)
            unloadTop(liveRel[liveCount - 1]);
        return failures == 0 ? 0 : 1;
    }

    // The render thread consumes this status while the database thread is
    // walking a live fastfile.  Prove the real reader publishes an active
    // zero state, advances it as decompressed bytes are consumed, and clears
    // it after close; a synthetic status setter would not cover this seam.
    {
        // The retail loadbar is sized per load: DB_ResetZoneSize clears it
        // (FS_ResetRetailLoadProgress), so nothing is reported until a walk
        // opens the next zone.
        FS_ResetRetailLoadProgress();
        if (!Check(!FS_GetRetailLoadProgress(nullptr, nullptr, nullptr), "loading_progress_reset_unsized"))
            return 1;
        FsRetailFastfileReader *progressReader = nullptr;
        if (!Check(FS_OpenRetailFastfile("zone/english/xmodel_live.ff", &progressReader) ==
                       FS_RETAIL_FF_OK && progressReader,
                   "loading_progress_open"))
            return 1;
        char label[64] = {};
        uint32_t percent = 0;
        if (!Check(FS_GetRetailLoadStatus(label, sizeof(label), &percent) &&
                       !std::strcmp(label, "xmodel_live.ff") && percent == 0,
                   "loading_progress_initial"))
        {
            FS_CloseRetailFastfile(progressReader);
            return 1;
        }
        uint8_t probe[4096] = {};
        bool advanced = false;
        uint32_t lastPercent = 0;
        for (uint32_t attempt = 0; attempt < 32 && !advanced; ++attempt)
        {
            if (!FS_ReadRetailFastfile(progressReader, probe, sizeof(probe)))
                break;
            if (!Check(FS_GetRetailLoadStatus(nullptr, 0, &percent),
                       "loading_progress_active"))
            {
                FS_CloseRetailFastfile(progressReader);
                return 1;
            }
            lastPercent = percent;
            advanced = percent > 0;
        }
        if (!Check(advanced, "loading_progress_advanced"))
        {
            FS_CloseRetailFastfile(progressReader);
            return 1;
        }
        FS_CloseRetailFastfile(progressReader);
        if (!Check(!FS_GetRetailLoadStatus(nullptr, 0, &percent),
                   "loading_progress_closed"))
            return 1;
        (void)lastPercent;
        // The retail briefing loadbar (Window_Paint style 6 -> UI_DrawLoadBar
        // -> UI_LoadBarProgress_FastFile -> DB_GetLoadedFraction) divides the
        // compressed bytes read by the zone file's size (+ external bytes).
        // Those counters are sized at open and held after close, so the bar
        // keeps its fill for the rest of the mission load; only the next
        // DB_ResetZoneSize (FS_ResetRetailLoadProgress) clears them.
        uint64_t heldRead = 0, heldFile = 0;
        uint32_t heldExternal = 0;
        if (!Check(FS_GetRetailLoadProgress(&heldRead, &heldFile, &heldExternal) && heldFile > 12 &&
                       heldRead > 12 && heldRead <= heldFile,
                   "loading_progress_fraction_held"))
            return 1;
        FS_ResetRetailLoadProgress();
        if (!Check(!FS_GetRetailLoadProgress(nullptr, nullptr, nullptr), "loading_progress_reset_after_close"))
            return 1;
        std::printf("PASS:RETAIL_LOADING_PROGRESS active=1 advanced=1 closed=1 fraction_held=1 reset=1 "
                    "read=%llu file=%llu external=%u\n",
                    (unsigned long long)heldRead, (unsigned long long)heldFile, heldExternal);
    }

    // Owns the first real SL_* user in this host process: intern the
    // script-string table once up front (production calls SL_Init at boot
    // via Scr_Init long before any zone loads).
    SL_Init();

    // Wiring only: real FS/dispatcher/widener with a synthetic triangle.
    // This proves native registration/geometry bytes, never GPU activation.
    {
        RetailWalkDirectoryRecord records[1]{};
        RetailWalkDirectoryResult walk{};
        if (!Check(RetailWalkFastfileDirectory("zone/english/xmodel_live.ff", records, 1,
                                               &walk) == RETAIL_WALK_OK &&
                       walk.walkedXModelCount == 1 && walk.walkedXModelSurfaceCount == 1,
                   "b4_xmodel_walk"))
            return 1;
        RetailWalkLoadZoneResult loaded{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/xmodel_live.ff", &loaded) ==
                       RETAIL_WALK_LOAD_OK, "b4_xmodel_load"))
            return 1;
        XModel *model = StubFindXAssetHeader(ASSET_TYPE_XMODEL, "xmodel/wire").model;
        if (!Check(model && model->numsurfs == 1 && model->numLods == 1 &&
                       model->lodInfo[0].dist == 128.0f && model->lodInfo[0].numsurfs == 1 &&
                       model->surfs && model->surfs[0].vertCount == 3 &&
                       model->surfs[0].triCount == 1 &&
                       !model->surfs[0].deformed &&
                       model->surfs[0].vertListCount == 2 &&
                       model->surfs[0].vertList &&
                       model->surfs[0].vertList[0].vertCount == 3 &&
                       model->surfs[0].vertList[0].triCount == 1 &&
                       model->surfs[0].vertList[0].collisionTree &&
                       model->surfs[0].vertList[1].collisionTree == nullptr &&
                       model->surfs[0].zoneHandle == loaded.zoneIndex &&
                       model->surfs[0].verts0 && model->surfs[0].triIndices &&
                       model->surfs[0].triIndices[2] == 2 && model->materialHandles &&
                       model->materialHandles[0] ==
                           StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "mat/xmodel").material,
                   "b4_xmodel_native_geometry"))
            return 1;
        // The inline collision tree's node/leaf arrays must be materialized
        // with their real contents. If the widener consumed them with the
        // wrong alignment, the following material slot/name would be
        // misread first, so the cursor-parity check below is the primary
        // witness and this one pins the materialized payload.
        const XSurfaceCollisionTree *tree = model->surfs[0].vertList[0].collisionTree;
        if (!Check(tree->nodeCount == 1 && tree->leafCount == 1 &&
                       tree->nodes && tree->leafs &&
                       tree->trans[0] == 1.0f && tree->trans[2] == 3.0f &&
                       tree->scale[0] == 4.0f && tree->scale[2] == 6.0f &&
                       tree->nodes[0].aabb.mins[0] == 1 && tree->nodes[0].aabb.maxs[2] == 6 &&
                       tree->nodes[0].childBeginIndex == 7 && tree->nodes[0].childCount == 8 &&
                       tree->leafs[0].triangleBeginIndex == 9,
                   "b4_xmodel_collision_tree"))
            return 1;
        // Same guard the bounce-array seam uses: Load_XSurface's walk-only
        // reader and the live widener must end every block at the same byte.
        bool cursorParity = true;
        for (int block = 0; block < 9; ++block)
            cursorParity = cursorParity && walk.endCursor[block] == loaded.endCursor[block];
        if (!Check(cursorParity, "b4_xmodel_cursor_parity"))
        {
            std::printf("FAIL:B4_XMODEL_CURSOR_PARITY walk=%u,%u,%u,%u,%u,%u,%u,%u,%u "
                        "live=%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                        walk.endCursor[0], walk.endCursor[1], walk.endCursor[2],
                        walk.endCursor[3], walk.endCursor[4], walk.endCursor[5],
                        walk.endCursor[6], walk.endCursor[7], walk.endCursor[8],
                        loaded.endCursor[0], loaded.endCursor[1], loaded.endCursor[2],
                        loaded.endCursor[3], loaded.endCursor[4], loaded.endCursor[5],
                        loaded.endCursor[6], loaded.endCursor[7], loaded.endCursor[8]);
            return 1;
        }
        const uint8_t *vertices = reinterpret_cast<const uint8_t *>(model->surfs[0].verts0);
        for (uint32_t i = 0; i < 96; ++i)
            if (!Check(vertices[i] == i, "b4_xmodel_vertex_bytes"))
                return 1;
        if (!Check(UnloadFixtureZone(loaded) &&
                       !StubFindXAssetHeader(ASSET_TYPE_XMODEL, "xmodel/wire").data &&
                       !StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "mat/xmodel").data,
                   "b4_xmodel_unload"))
            return 1;
        std::printf("OK:B4_XMODEL_WIRING synthetic=1 surfaces=1 vertices=3 indices=3 "
                    "rigid_lists=2 collision_nodes=1 collision_leafs=1 cursor_parity=1 "
                    "gpu=unproven\n");
    }

    // XAnimParts widening plus real SL_* script-string interning. Both
    // index-width branches (numframes 255 narrow, 256 wide) register live
    // through Load_XAnimPartsAsset with byte-exact native fields; the
    // truncated, out-of-range-index, and count-mismatch fixtures fail
    // loudly; unloading both happy zones frees every interned reference
    // (leak check against the real SL_* table, not a fake).
    {
        const uint32_t slBefore = scrStringDebugGlob ? scrStringDebugGlob->totalRefCount : 0;
        RetailWalkLoadZoneResult narrowResult{};
        if (!LoadFixtureZone("zone/english/xanim_narrow.ff", &narrowResult, "b3_narrow_load") ||
            !Check(narrowResult.assetCount == 1 && narrowResult.registeredXAnimCount == 1 &&
                       narrowResult.walkedOnlyCount == 0,
                   "b3_narrow_counts"))
            return 1;
        XAnimParts *narrow =
            StubFindXAssetHeader(ASSET_TYPE_XANIMPARTS, "anim/narrow").parts;
        if (!Check(narrow && narrow->numframes == 255 && narrow->boneCount[9] == 2 &&
                       narrow->notifyCount == 1 && narrow->indexCount == 4 &&
                       narrow->framerate == 30.0f && narrow->bLoop && narrow->bDelta &&
                       narrow->names && narrow->notify && narrow->deltaPart &&
                       narrow->dataByte && narrow->dataShort && narrow->dataInt &&
                       narrow->randomDataShort && narrow->randomDataByte &&
                       narrow->randomDataInt && narrow->indices._1,
                   "b3_narrow_native") ||
            !Check(!std::strcmp(SL_ConvertToString(narrow->names[0]), "b3_nrw_spine") &&
                       !std::strcmp(SL_ConvertToString(narrow->names[1]), "b3_nrw_head") &&
                       !std::strcmp(SL_ConvertToString(narrow->notify[0].name),
                                    "b3_nrw_fire") &&
                       narrow->notify[0].time == 0.5f,
                   "b3_narrow_strings") ||
            !Check(narrow->indices._1[0] == 0 && narrow->indices._1[1] == 1 &&
                       narrow->indices._1[2] == 2 && narrow->indices._1[3] == 3,
                   "b3_narrow_indices") ||
            !Check(narrow->dataByte[0] == 10 && narrow->dataByte[2] == 30 &&
                       narrow->dataShort[1] == 2000 && narrow->dataInt[0] == 123456 &&
                       narrow->randomDataShort[0] == 77 && narrow->randomDataByte[1] == 9 &&
                       narrow->randomDataInt[0] == 654321,
                   "b3_narrow_arrays") ||
            !Check(narrow->deltaPart->trans && narrow->deltaPart->trans->size == 1 &&
                       narrow->deltaPart->trans->smallTrans &&
                       narrow->deltaPart->trans->u.frames.frames._1[0][0] == 1 &&
                       narrow->deltaPart->trans->u.frames.frames._1[1][2] == 6 &&
                       narrow->deltaPart->trans->u.frames.indices._1[0] == 0 &&
                       narrow->deltaPart->trans->u.frames.indices._1[1] == 1 &&
                       narrow->deltaPart->quat && narrow->deltaPart->quat->size == 1 &&
                       narrow->deltaPart->quat->u.frames.frames[0][0] == 100 &&
                       narrow->deltaPart->quat->u.frames.frames[1][1] == 400 &&
                       narrow->deltaPart->quat->u.frames.indices._1[0] == 0 &&
                       narrow->deltaPart->quat->u.frames.indices._1[1] == 1,
                   "b3_narrow_delta"))
            return 1;
        RetailWalkLoadZoneResult wideResult{};
        if (!LoadFixtureZone("zone/english/xanim_wide.ff", &wideResult, "b3_wide_load") ||
            !Check(wideResult.assetCount == 1 && wideResult.registeredXAnimCount == 1 &&
                       wideResult.walkedOnlyCount == 0,
                   "b3_wide_counts"))
            return 1;
        XAnimParts *wide = StubFindXAssetHeader(ASSET_TYPE_XANIMPARTS, "anim/wide").parts;
        if (!Check(wide && wide->numframes == 256 && wide->boneCount[9] == 2 &&
                       wide->notifyCount == 1 && wide->indices._2 &&
                       wide->indices._2[0] == 0 && wide->indices._2[1] == 1 &&
                       wide->indices._2[2] == 200 && wide->indices._2[3] == 255,
                   "b3_wide_native") ||
            !Check(!std::strcmp(SL_ConvertToString(wide->names[0]), "b3_wde_spine") &&
                       !std::strcmp(SL_ConvertToString(wide->names[1]), "b3_wde_head") &&
                       !std::strcmp(SL_ConvertToString(wide->notify[0].name),
                                    "b3_wde_fire"),
                   "b3_wide_strings") ||
            !Check(wide->deltaPart->trans->u.frames.indices._2[0] == 0 &&
                       wide->deltaPart->trans->u.frames.indices._2[1] == 255 &&
                       wide->deltaPart->quat->u.frames.indices._2[0] == 0 &&
                       wide->deltaPart->quat->u.frames.indices._2[1] == 255,
                   "b3_wide_indices"))
            return 1;
        RetailWalkLoadZoneResult truncResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/xanim_trunc.ff", &truncResult) !=
                       RETAIL_WALK_LOAD_OK,
                   "b3_trunc_fails"))
            return 1;
        RetailWalkLoadZoneResult badIndexResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/xanim_badindex.ff", &badIndexResult) !=
                       RETAIL_WALK_LOAD_OK,
                   "b3_badindex_fails"))
            return 1;
        RetailWalkLoadZoneResult mismatchResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/xanim_mismatch.ff", &mismatchResult) !=
                       RETAIL_WALK_LOAD_OK,
                   "b3_mismatch_fails"))
            return 1;
        if (!Check(UnloadFixtureZone(narrowResult) && UnloadFixtureZone(wideResult) &&
                       !StubFindXAssetHeader(ASSET_TYPE_XANIMPARTS, "anim/narrow").data &&
                       !StubFindXAssetHeader(ASSET_TYPE_XANIMPARTS, "anim/wide").data,
                   "b3_unload") ||
            !Check(!SL_FindString("b3_nrw_spine") && !SL_FindString("b3_nrw_head") &&
                       !SL_FindString("b3_nrw_fire") && !SL_FindString("b3_wde_spine") &&
                       !SL_FindString("b3_wde_head") && !SL_FindString("b3_wde_fire") &&
                       (!scrStringDebugGlob ||
                        scrStringDebugGlob->totalRefCount == slBefore),
                   "b3_no_leaked_sl_refs"))
            return 1;
        std::printf("PASS:B3_XANIM_WIDEN xanims=2 registered=2 script_strings=6 "
                    "index_width_branches=2 leaked_sl_refs=0 defaults=0 placeholders=0\n");
    }

    // WeaponDef widening plus real SL_* interning for hideTags and
    // notetrack maps. The empty sentinel and the knots fixture register
    // live through Load_WeaponDefAsset with byte-exact native fields;
    // unresolvable XModel/sound fixtures defer with a loud audit line
    // (F5 contract) while truncation still aborts; the walk-only reader
    // agrees on the happy fixture first, and unloading all three zones
    // frees every interned reference (leak check against the real SL_*
    // table, not a fake).
    {
        const uint32_t slBefore = scrStringDebugGlob ? scrStringDebugGlob->totalRefCount : 0;
        RetailWalkDirectoryRecord weaponWalk[2]{};
        RetailWalkDirectoryResult weaponWalkResult{};
        if (!Check(RetailWalkFastfileDirectory("zone/english/weapon_empty.ff", weaponWalk, 2,
                                               &weaponWalkResult) == RETAIL_WALK_OK &&
                       weaponWalkResult.walkedWeaponCount == 2,
                   "b7_weapon_walk"))
            return 1;
        RetailWalkLoadZoneResult emptyResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/weapon_empty.ff", &emptyResult) ==
                       RETAIL_WALK_LOAD_OK,
                   "b7_empty_load") ||
            !Check(emptyResult.assetCount == 2 && emptyResult.registeredWeaponCount == 2 &&
                       emptyResult.walkedOnlyCount == 0,
                   "b7_empty_counts") ||
            !Check(StubZoneIsLive(emptyResult.zoneIndex), "b7_empty_zone_kept_live"))
            return 1;
        WeaponDef *empty = StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/empty").weapon;
        if (!Check(empty && !std::strcmp(empty->szInternalName, "wpn/empty") &&
                       !std::strcmp(empty->szDisplayName, "WPN Empty") &&
                       empty->damage == 30 && empty->iClipSize == 10 &&
                       empty->iMaxAmmo == 300 && empty->fAdsZoomFov == 55.0f &&
                       empty->accuracy == 0.75f && empty->aiSpread == 0.5f &&
                       empty->playerSpread == 0.25f &&
                       empty->parallelBounce[0] == 1.5f &&
                       empty->perpendicularBounce[0] == 2.5f &&
                       empty->locationDamageMultipliers[0] == 1.0f &&
                       empty->locationDamageMultipliers[1] == 0.5f &&
                       empty->accuracyGraphKnots[0] == nullptr &&
                       empty->accuracyGraphKnotCount[0] == 0 &&
                       empty->gunXModel[0] == nullptr && empty->handXModel == nullptr &&
                       empty->worldModel[0] == nullptr && empty->fireSound == nullptr &&
                       empty->hudIcon == nullptr && empty->killIcon == nullptr &&
                       empty->hudIconRatio == WEAPON_ICON_RATIO_2TO1 &&
                       empty->ammoCounterIconRatio == WEAPON_ICON_RATIO_4TO1 &&
                       empty->ammoCounterClip == AMMO_COUNTER_CLIP_MAGAZINE &&
                       empty->iStartAmmo == 30 &&
                       !std::strcmp(empty->szAmmoName, "ammo_empty") &&
                       !std::strcmp(empty->szOverlayName, "overlay") &&
                       empty->accuracyGraphName[0] &&
                       empty->accuracyGraphName[0][0] == 0 &&
                       empty->accuracyGraphName[1] == nullptr,
                   "b7_empty_native") ||
            !Check(!std::strcmp(SL_ConvertToString(empty->hideTags[0]), "tag_hide") &&
                       !std::strcmp(SL_ConvertToString(empty->hideTags[1]), "note_key") &&
                       !std::strcmp(SL_ConvertToString(empty->hideTags[2]), "note_val") &&
                       !std::strcmp(SL_ConvertToString(empty->notetrackSoundMapKeys[1]),
                                     "note_key") &&
                       !std::strcmp(SL_ConvertToString(empty->notetrackSoundMapValues[1]),
                                     "note_val"),
                   "b7_empty_strings"))
            return 1;
        WeaponDef *knots = StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/knots").weapon;
        if (!Check(knots && knots->damage == 45 && knots->iClipSize == 20 &&
                       knots->accuracyGraphKnotCount[0] == 2 &&
                       knots->accuracyGraphKnotCount[1] == 1 &&
                       knots->accuracyGraphKnots[0] && knots->accuracyGraphKnots[1] &&
                       knots->accuracyGraphKnots[0][0][0] == 1.0f &&
                       knots->accuracyGraphKnots[0][0][1] == 2.0f &&
                       knots->accuracyGraphKnots[0][1][0] == 3.0f &&
                       knots->accuracyGraphKnots[0][1][1] == 4.0f &&
                       knots->accuracyGraphKnots[1][0][0] == 5.0f &&
                       knots->accuracyGraphKnots[1][0][1] == 6.0f &&
                       knots->originalAccuracyGraphKnots[0] == nullptr &&
                       !std::strcmp(knots->accuracyGraphName[0], "acc0") &&
                       !std::strcmp(knots->accuracyGraphName[1], "acc1"),
                   "b7_knots_native"))
            return 1;
        // Unresolvable references defer to walked accounting (F5 contract):
        // the zone stays live and successful with nothing registered, and
        // the audit line names the exact slot. Truncation still aborts.
        RetailWalkLoadZoneResult badModelResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/weapon_badmodel.ff", &badModelResult) ==
                       RETAIL_WALK_LOAD_OK,
                   "b7_badmodel_defers") ||
            !Check(badModelResult.assetCount == 1 &&
                       badModelResult.registeredWeaponCount == 0 &&
                       badModelResult.walkedOnlyCount == 1,
                   "b7_badmodel_counts") ||
            !Check(StubZoneIsLive(badModelResult.zoneIndex) &&
                       !StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/badmodel").data,
                   "b7_badmodel_unregistered"))
            return 1;
        RetailWalkLoadZoneResult badSoundResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/weapon_badsound.ff", &badSoundResult) ==
                       RETAIL_WALK_LOAD_OK,
                   "b7_badsound_defers") ||
            !Check(badSoundResult.assetCount == 1 &&
                       badSoundResult.registeredWeaponCount == 0 &&
                       badSoundResult.walkedOnlyCount == 1,
                   "b7_badsound_counts") ||
            !Check(StubZoneIsLive(badSoundResult.zoneIndex) &&
                       !StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/badsound").data,
                   "b7_badsound_unregistered"))
            return 1;
        // deferred-sound contract: a resolvable sound name that no
        // loaded zone declares stays visible as an outstanding deferred
        // entry -- never bound to the engine default (`null`) entry. The
        // body registers and the entry is dropped by the owning zone's
        // unload sweep; the walk verifier rejects any entry still outstanding
        // after the final zone load.
        RetailWalkLoadZoneResult missingSoundResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/weapon_sound_missing.ff",
                                            &missingSoundResult) == RETAIL_WALK_LOAD_OK,
                   "b7_sound_missing_defers") ||
            !Check(missingSoundResult.assetCount == 1 &&
                       missingSoundResult.registeredWeaponCount == 1 &&
                       missingSoundResult.walkedOnlyCount == 0,
                   "b7_sound_missing_counts") ||
            !Check(StubZoneIsLive(missingSoundResult.zoneIndex) &&
                       RetailWeaponDeferredSoundCount() == 1 &&
                       StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/missing").data,
                   "b7_sound_missing_recorded"))
            return 1;
        // The unload sweep must drop the outstanding entry before the zone's
        // arena is freed, so no later resolve pass can write through the
        // dangling native slot.
        if (!Check(DB_RetailZoneEnd(missingSoundResult.zoneIndex) &&
                       RetailWeaponDeferredSoundCount() == 0 &&
                       !StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/missing").data,
                   "b7_sound_missing_unload"))
            return 1;
        // forward-reference fixture: the weapon is declared before the
        // snd_alias_list_t it names (the real killhouse/common shape). The
        // post-zone pass must bind the real list -- not the default entry,
        // not a deferred body.
        RetailWalkLoadZoneResult forwardResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/weapon_forward_sound.ff",
                                            &forwardResult) == RETAIL_WALK_LOAD_OK,
                   "b7_forward_load") ||
            !Check(forwardResult.assetCount == 2 &&
                       forwardResult.registeredWeaponCount == 1 &&
                       forwardResult.registeredSoundCount == 1 &&
                       forwardResult.walkedOnlyCount == 0,
                   "b7_forward_counts"))
            return 1;
        const XAssetHeader forwardSound =
            StubFindXAssetHeader(ASSET_TYPE_SOUND, "snd/forward");
        const WeaponDef *forwardWeapon =
            StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/forward").weapon;
        std::printf("B7_FORWARD_SOUND weapon='%s' sound='%s' fire=%p list=%p bound=%u\n",
                    forwardWeapon && forwardWeapon->szInternalName
                        ? forwardWeapon->szInternalName : "(null)",
                    forwardSound.sound && forwardSound.sound->aliasName
                        ? forwardSound.sound->aliasName : "(null)",
                    (const void *)(forwardWeapon ? forwardWeapon->fireSound : nullptr),
                    (const void *)forwardSound.sound,
                    forwardWeapon && forwardSound.sound &&
                            forwardWeapon->fireSound == forwardSound.sound
                        ? 1u : 0u);
        if (!Check(forwardSound.sound && forwardWeapon &&
                       forwardWeapon->fireSound == forwardSound.sound,
                   "b7_forward_bound") ||
            !Check(StubZoneIsLive(forwardResult.zoneIndex), "b7_forward_live"))
            return 1;
        // End this fixture's zone now (peak-slot parity with the other 
        // fixtures) and prove the resolved binding's owner unloads cleanly.
        if (!Check(UnloadFixtureZone(forwardResult) &&
                       !StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/forward").data &&
                       !StubFindXAssetHeader(ASSET_TYPE_SOUND, "snd/forward").data,
                   "b7_forward_unload"))
            return 1;
        // Bounce-array cursor seam (killhouse ord-890 regression): the
        // live decoder must consume the inline 29-slot bounce array from the
        // exact post-stream base the walk-only reader uses. A pre-stream base
        // read the pointer array before the 4-byte stream alignment, left the
        // live cursor behind the walk, and misread the nested reticle
        // material that follows (ord 890 m203_m4) as an inline technique set.
        // The endCursor parity below fails loudly if that divergence returns.
        RetailWalkDirectoryRecord bounceWalk[1]{};
        RetailWalkDirectoryResult bounceWalkResult{};
        const RetailWalkDirectoryResultCode bounceWalkCode =
            RetailWalkFastfileDirectory("zone/english/weapon_bounce.ff", bounceWalk, 1,
                                        &bounceWalkResult);
        if (!Check(bounceWalkCode == RETAIL_WALK_OK &&
                       bounceWalkResult.walkedWeaponCount == 1,
                   "b7_bounce_walk"))
            return 1;
        RetailWalkLoadZoneResult bounceResult{};
        const RetailWalkLoadZoneResultCode bounceCode =
            RetailWalkLoadZoneAssets("zone/english/weapon_bounce.ff", &bounceResult);
        std::printf("B7_BOUNCE code=%d assets=%u registered=%u walkedOnly=%u failed=%u\n",
                    (int)bounceCode, bounceResult.assetCount,
                    bounceResult.registeredWeaponCount, bounceResult.walkedOnlyCount,
                    bounceResult.failedOrdinal);
        if (!Check(bounceCode == RETAIL_WALK_LOAD_OK, "b7_bounce_defers") ||
            !Check(bounceResult.assetCount == 1 && bounceResult.registeredWeaponCount == 1 &&
                       bounceResult.walkedOnlyCount == 0,
                   "b7_bounce_counts") ||
            !Check(bounceResult.endCursor[0] == bounceWalkResult.endCursor[0] &&
                       bounceResult.endCursor[4] == bounceWalkResult.endCursor[4],
                   "b7_bounce_cursor_parity") ||
            !Check(StubZoneIsLive(bounceResult.zoneIndex) &&
                       StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/bounce").weapon,
                   "b7_bounce_registered"))
            return 1;
        RetailWalkLoadZoneResult truncResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/weapon_trunc.ff", &truncResult) !=
                       RETAIL_WALK_LOAD_OK,
                   "b7_trunc_fails"))
            return 1;
        if (!Check(UnloadFixtureZone(emptyResult) &&
                       DB_RetailZoneEnd(badModelResult.zoneIndex) &&
                       DB_RetailZoneEnd(badSoundResult.zoneIndex) &&
                       DB_RetailZoneEnd(bounceResult.zoneIndex) &&
                       !StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/empty").data &&
                       !StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/knots").data &&
                        !StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/badmodel").data &&
                        !StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/badsound").data &&
                        !StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/bounce").data &&
                        !StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/forward").data &&
                        !StubFindXAssetHeader(ASSET_TYPE_WEAPON, "wpn/missing").data &&
                        !StubFindXAssetHeader(ASSET_TYPE_SOUND, "snd/forward").data,
                   "b7_unload") ||
            !Check(!SL_FindString("tag_hide") && !SL_FindString("note_key") &&
                       !SL_FindString("note_val") && !SL_FindString("tag0") &&
                       (!scrStringDebugGlob ||
                        scrStringDebugGlob->totalRefCount == slBefore),
                   "b7_no_leaked_sl_refs"))
            return 1;
        std::printf("PASS:B7_WEAPON_WIDEN weapons=2 registered=2 script_strings=3 "
                    "knot_pairs=3 deferred=2 failed=1 leaked_sl_refs=0 defaults=0 placeholders=0 "
                    "bounce_parity=1\n");

        // Synthetic pointer-class proof for the shared (alias-form) bounce
        // array: build the pristine mirror layout the retail declarer leaves
        // (29 entry slots followed by the -1 entries' inner slot/name bytes)
        // and resolve it through the production resolver. Covers inline-name,
        // alias-to-pointer-slot, direct-name-offset, and null entries, plus a
        // loud miss for an alias pointing outside the block.
        {
            snd_alias_list_t stockLists[3]{};
            char stockNames[3][16] = {"bounce_zero", "bounce_one", "bounce_inline"};
            const uint32_t boBlocks[9]{};
            uint32_t boZone = 0;
            XZoneMemory *boMemory = nullptr;
            void *boArena = nullptr;
            uint32_t boArenaBytes = 4096;
            if (!Check(DB_RetailZoneBegin("b7_bounce_stock", 0, boBlocks, &boArenaBytes, &boZone,
                                          &boMemory, &boArena),
                       "b7_bounce_stock_zone"))
                return 1;
            for (uint32_t i = 0; i < 3; ++i)
            {
                stockLists[i].aliasName = stockNames[i];
                XAssetHeader registered =
                    DB_RetailZoneRegister(ASSET_TYPE_SOUND, {&stockLists[i]}, boZone);
                if (!Check(registered.sound == &stockLists[i], "b7_bounce_stock_register"))
                    return 1;
            }
            auto aliasRef = [](uint32_t off) { return ((4u << 28) | off) + 1u; };
            uint8_t block4[1024]{};
            auto w32 = [&](uint32_t off, uint32_t value)
            {
                block4[off] = static_cast<uint8_t>(value);
                block4[off + 1] = static_cast<uint8_t>(value >> 8);
                block4[off + 2] = static_cast<uint8_t>(value >> 16);
                block4[off + 3] = static_cast<uint8_t>(value >> 24);
            };
            std::memcpy(block4 + 8, "bounce_zero\0", 12);
            std::memcpy(block4 + 32, "bounce_one\0", 11);
            std::memcpy(block4 + 256, "bounce_unused\0", 14);
            w32(48, aliasRef(8)); // pointer slot -> "bounce_zero"
            w32(64, 0xffffffffu); // entry 0: inline name follows the array
            w32(68, aliasRef(48)); // entry 1: alias -> pointer slot
            w32(72, aliasRef(32)); // entry 2: direct name-offset entry
            w32(180, 0xffffffffu); // entry 0 inner slot -> inline bytes
            std::memcpy(block4 + 184, "bounce_inline\0", 14);

            XZoneMemory mirror{};
            mirror.blocks[4] = {block4, sizeof(block4)};
            RetailZoneLoadSession session{};
            session.active = true;
            session.zoneMemory = &mirror;
            void *bounce[29]{};
            uint32_t failedIndex = 0;
            const char *failedName = nullptr;
            const bool resolved = RetailWeaponResolveBounceArray(
                &session, aliasRef(64), bounce, &failedIndex, &failedName);
            if (!Check(resolved && failedIndex == UINT32_MAX && failedName == nullptr &&
                           bounce[0] == &stockLists[2] && bounce[1] == &stockLists[0] &&
                           bounce[2] == &stockLists[1] && bounce[3] == nullptr,
                       "b7_bounce_alias_resolve"))
                return 1;
            // Negative: a non-null entry aliasing outside the block must fail
            // loudly and report the exact entry, never bind a silent null.
            w32(76, aliasRef(4096));
            void *badBounce[29]{};
            const bool bad = RetailWeaponResolveBounceArray(
                &session, aliasRef(64), badBounce, &failedIndex, &failedName);
            std::printf("B7_BOUNCE_ALIAS resolved=%d failed=%d index=%u\n",
                        resolved ? 1 : 0, bad ? 1 : 0, failedIndex);
            if (!Check(!bad && failedIndex == 3, "b7_bounce_alias_negative"))
                return 1;
            if (!Check(DB_RetailZoneEnd(boZone) &&
                           StubFindXAssetHeader(ASSET_TYPE_SOUND, "bounce_zero").sound == nullptr &&
                           StubRetiredPtrCount() == 0,
                       "b7_bounce_alias_unload"))
                return 1;
            std::puts("PASS:B7_BOUNCE_ALIAS inline=1 slot_alias=1 direct=1 null=1 miss=loud");
        }
    }

    // Nested sound graph proof: the walk reader's captured
    // offsets for streamed sound files, embedded LoadedSound data, falloff
    // curves, and speaker maps widen into native objects with byte-exact
    // fields -- identity/ownership only, no playback or streaming claim.
    {
        uint8_t block0[512]{};
        uint8_t block4[1024]{};
        auto w32 = [](uint8_t *p, uint32_t v)
        {
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
            p[2] = static_cast<uint8_t>(v >> 16);
            p[3] = static_cast<uint8_t>(v >> 24);
        };
        // block0: embedded LoadedSound root at 0 (data at 64) and a SndCurve
        // root at 128 (one knot pair 2.5/3.5).
        w32(block0 + 4, 1);      // info.format
        w32(block0 + 12, 4);     // info.data_len
        w32(block0 + 16, 44100); // info.rate
        w32(block0 + 20, 16);    // info.bits
        w32(block0 + 24, 2);     // info.channels
        w32(block0 + 28, 100);   // info.samples
        w32(block0 + 32, 512);   // info.block_size
        block0[64] = 1;
        block0[65] = 2;
        block0[66] = 3;
        block0[67] = 4;
        w32(block0 + 128, 0xffffffffu); // curve filename inline
        w32(block0 + 132, 1);           // knotCount
        const float knot0[2] = {2.5f, 3.5f};
        std::memcpy(block0 + 136, knot0, sizeof(knot0));
        // block4: a streamed SoundFile record at 0 ("snd/" + "a.wav"), an
        // embedded SoundFile record at 128, and a SpeakerMap root at 256
        // with its inline name after the 408-byte record.
        block4[1] = 1; // exists: the linker's byte (1 on every shipped entry)
        w32(block4 + 4, 0xffffffffu);
        w32(block4 + 8, 0xffffffffu);
        std::memcpy(block4 + 12, "snd/", 5);
        std::memcpy(block4 + 17, "a.wav", 6);
        block4[128] = 1;
        block4[129] = 1; // exists
        w32(block4 + 132, 0xffffffffu);
        block4[256] = 1;
        w32(block4 + 260, 0xffffffffu);
        w32(block4 + 264, 3); // channelMaps[0][0].speakerCount
        std::memcpy(block4 + 664, "spk", 4);
        std::memcpy(block4 + 32, "curve/x", 8);

        static uint8_t b2Arena[65536];
        XZoneMemory mirror{};
        mirror.blocks[0] = {block0, sizeof(block0)};
        mirror.blocks[4] = {block4, sizeof(block4)};
        RetailZoneLoadSession session{};
        session.active = true;
        session.zoneMemory = &mirror;
        if (!Check(RetailNativeArenaInit(&session.arena, b2Arena, sizeof(b2Arena)),
                   "b2_arena_init"))
            return 1;

        RetailWalkSndAliasOffsets offsets{};
        std::memset(&offsets, 0xFF, sizeof(offsets));
        offsets.loadedSoundRootBytes = nullptr;
        offsets.curveRootBytes = nullptr;
        offsets.loadedSoundRetained = nullptr;
        offsets.soundFile = 0;
        offsets.soundFileDir = 12;
        offsets.soundFileName = 17;
        offsets.curveRoot = 128;
        offsets.curveName = 32;
        offsets.speakerMap = 256;
        offsets.speakerMapName = 664;
        snd_alias_t alias{};
        if (!Check(RetailSoundWidenAliasGraphs(&session, offsets, &alias),
                   "b2_streamed_graph") ||
            !Check(alias.soundFile && alias.soundFile->type == 0 &&
                       alias.soundFile->exists &&
                       !std::strcmp(alias.soundFile->u.streamSnd.filename.info.raw.dir, "snd/") &&
                       !std::strcmp(alias.soundFile->u.streamSnd.filename.info.raw.name, "a.wav"),
                   "b2_streamed_fields") ||
            !Check(alias.volumeFalloffCurve && alias.volumeFalloffCurve->knotCount == 1 &&
                       !std::strcmp(alias.volumeFalloffCurve->filename, "curve/x") &&
                       alias.volumeFalloffCurve->knots[0][0] == 2.5f &&
                       alias.volumeFalloffCurve->knots[0][1] == 3.5f,
                   "b2_curve_fields") ||
            !Check(alias.speakerMap && alias.speakerMap->isDefault &&
                       !std::strcmp(alias.speakerMap->name, "spk") &&
                       alias.speakerMap->channelMaps[0][0].speakerCount == 3,
                   "b2_speaker_fields"))
            return 1;

        RetailWalkSndAliasOffsets embedded{};
        std::memset(&embedded, 0xFF, sizeof(embedded));
        embedded.loadedSoundRootBytes = nullptr;
        embedded.curveRootBytes = nullptr;
        embedded.loadedSoundRetained = nullptr;
        embedded.soundFile = 128;
        embedded.loadedSoundRoot = 0;
        embedded.loadedSoundName = 17;
        embedded.loadedSoundData = 64;
        embedded.loadedSoundDataLen = 4;
        snd_alias_t embeddedAlias{};
        if (!Check(RetailSoundWidenAliasGraphs(&session, embedded, &embeddedAlias),
                   "b2_embedded_graph") ||
            !Check(embeddedAlias.soundFile && embeddedAlias.soundFile->type == 1 &&
                       embeddedAlias.soundFile->u.loadSnd &&
                       embeddedAlias.soundFile->u.loadSnd->sound.info.rate == 44100 &&
                       embeddedAlias.soundFile->u.loadSnd->sound.info.channels == 2 &&
                       embeddedAlias.soundFile->u.loadSnd->sound.info.data_len == 4 &&
                       embeddedAlias.soundFile->u.loadSnd->sound.data == nullptr &&
                       embeddedAlias.soundFile->u.loadSnd->sound.info.data_ptr == nullptr,
                   "b2_embedded_fields"))
            return 1;

        // `exists` is the zone's byte, not asserted: a record carrying 0
        // widens to a missing file (Com_GetSoundFileName callers skip it).
        block4[1] = 0;
        snd_alias_t missingAlias{};
        if (!Check(RetailSoundWidenAliasGraphs(&session, offsets, &missingAlias),
                   "b2_missing_graph") ||
            !Check(missingAlias.soundFile && !missingAlias.soundFile->exists, "b2_missing_exists"))
            return 1;
        block4[1] = 1;
        std::puts("PASS:B2_SOUND_GRAPH streamed=1 embedded=1 curve=1 speaker_map=1 payload_deferred=1 no_playback=1 exists_byte=1");
    }

    // ImpactFx proof: the empty-name fastfile table registers
    // and every non-null cell resolves -- the inline cell widens live, the
    // alias cell binds it through the zone nested table, and declared-null
    // cells stay null (the missing-effect sentinel path is the engine's
    // defaultEffect, not a synthesized pointer here). Identity only: no
    // spawn/draw activation is claimed.
    {
        RetailWalkDirectoryRecord impactWalkRecords[1]{};
        RetailWalkDirectoryResult impactWalk{};
        if (!Check(RetailWalkFastfileDirectory("zone/english/impactfx_live.ff",
                                               impactWalkRecords, 1, &impactWalk) ==
                       RETAIL_WALK_OK &&
                       impactWalk.walkedImpactFxCount == 1,
                   "b6_impact_walk"))
            return 1;
        RetailWalkLoadZoneResult impactResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/impactfx_live.ff",
                                            &impactResult) == RETAIL_WALK_LOAD_OK &&
                       impactResult.registeredImpactFxCount == 1,
                   "b6_impact_load"))
            return 1;
        const XAssetHeader impact = StubFindXAssetHeader(ASSET_TYPE_IMPACT_FX, "");
        const bool impactOk =
            impact.impactFx && impact.impactFx->table &&
            impact.impactFx->table[0].nonflesh[0] &&
            !std::strcmp(impact.impactFx->table[0].nonflesh[0]->name, "fx/e0") &&
            impact.impactFx->table[1].nonflesh[1] == impact.impactFx->table[0].nonflesh[0] &&
            impact.impactFx->table[11].flesh[3] == nullptr;
        std::printf("B6_IMPACT_FX table=%p cell0=%p alias=%p null=%p\n",
                    static_cast<const void *>(impact.impactFx ? impact.impactFx->table : nullptr),
                    static_cast<const void *>(impact.impactFx && impact.impactFx->table
                                                  ? impact.impactFx->table[0].nonflesh[0]
                                                  : nullptr),
                    static_cast<const void *>(impact.impactFx && impact.impactFx->table
                                                  ? impact.impactFx->table[1].nonflesh[1]
                                                  : nullptr),
                    static_cast<const void *>(impact.impactFx && impact.impactFx->table
                                                  ? impact.impactFx->table[11].flesh[3]
                                                  : nullptr));
        if (!Check(impactOk, "b6_impact_cells"))
            return 1;
        if (!Check(UnloadFixtureZone(impactResult) &&
                       !StubFindXAssetHeader(ASSET_TYPE_IMPACT_FX, "").data,
                   "b6_impact_unload"))
            return 1;
        std::puts("PASS:B6_IMPACT_FX table=12x33 inline=1 alias=bound null=1 sentinel=engine_default no_spawn=1");
    }

    // Nested FxElemDef sub-objects: the live widener copies the
    // velocity/vis-state sample arrays and the trail verts/inds out of the
    // block-4 mirror the walk reader filled, and binds the inline effect name.
    {
        RetailWalkLoadZoneResult nestedResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/fx_nested_live.ff",
                                            &nestedResult) == RETAIL_WALK_LOAD_OK &&
                       nestedResult.registeredFxCount == 2,
                   "b6_nested_load"))
            return 1;
        const XAssetHeader nested = StubFindXAssetHeader(ASSET_TYPE_FX, "fx/test");
        const FxEffectDef *fx = nested.fx;
        const FxElemDef *elem =
            (fx && fx->elemDefs && fx->elemDefCountOneShot == 1) ? &fx->elemDefs[0] : nullptr;
        // The effect refs are FxEffectDef handles (Load_FxEffectDefFromName
        // replaces the name in place with the registered header): the
        // impact ref must be the 'fx/impact' effect declared first in the
        // zone, and the unset death ref stays null.
        const FxEffectDef *impact = StubFindXAssetHeader(ASSET_TYPE_FX, "fx/impact").fx;
        const bool nestedOk =
            elem && elem->velSamples != nullptr && elem->visSamples != nullptr &&
            impact != nullptr && elem->effectOnImpact.handle == impact &&
            elem->effectOnDeath.handle == nullptr &&
            elem->trailDef != nullptr && elem->trailDef->verts != nullptr &&
            elem->trailDef->inds != nullptr && elem->trailDef->vertCount == 2 &&
            elem->trailDef->indCount == 3;
        std::printf("B6_FX_NESTED fx=%p elem=%p vel=%p vis=%p effect='%s' trail=%p verts=%p inds=%p\n",
                    static_cast<const void *>(fx), static_cast<const void *>(elem),
                    elem ? static_cast<const void *>(elem->velSamples) : nullptr,
                    elem ? static_cast<const void *>(elem->visSamples) : nullptr,
                    elem && elem->effectOnImpact.handle && elem->effectOnImpact.handle == impact
                        ? elem->effectOnImpact.handle->name : "(null)",
                    elem ? static_cast<const void *>(elem->trailDef) : nullptr,
                    elem && elem->trailDef ? static_cast<const void *>(elem->trailDef->verts)
                                           : nullptr,
                    elem && elem->trailDef ? static_cast<const void *>(elem->trailDef->inds)
                                           : nullptr);
        if (!Check(nestedOk, "b6_nested_objects"))
            return 1;
        if (!Check(DB_RetailZoneEnd(nestedResult.zoneIndex),
                   "b6_nested_unload"))
            return 1;
        std::puts("PASS:B6_FX_NESTED vel=bound vis=bound effect=handle trail=verts+inds deferred_visuals=1");
    }

    // General fix: the FX visual union widens live and records each
    // material/model slot at its own block-4 offset, so a later slot-alias
    // onto that offset resolves instead of failing as an unrecorded inline
    // body (the fraggrenade shape). One FX, two material-visual elems: elem 0
    // is an inline material body, elem 1 aliases elem 0's recorded visual
    // slot. Walk-only and live must agree on the end cursors, and both elems
    // must bind the same widened Material.
    {
        RetailWalkDirectoryRecord visualWalk[1]{};
        RetailWalkDirectoryResult visualWalkResult{};
        if (!Check(RetailWalkFastfileDirectory("zone/english/fx_visual_live.ff",
                                               visualWalk, 1, &visualWalkResult) ==
                       RETAIL_WALK_OK &&
                       visualWalkResult.walkedFxCount == 1,
                   "b7_fx_visual_walk"))
            return 1;
        RetailWalkLoadZoneResult visualResult{};
        const RetailWalkLoadZoneResultCode visualCode =
            RetailWalkLoadZoneAssets("zone/english/fx_visual_live.ff", &visualResult);
        std::printf("B7_FX_VISUAL code=%d assets=%u registered=%u "
                    "endCursor0=%u/%u endCursor4=%u/%u\n",
                    (int)visualCode, visualResult.assetCount,
                    visualResult.registeredFxCount, visualResult.endCursor[0],
                    visualWalkResult.endCursor[0], visualResult.endCursor[4],
                    visualWalkResult.endCursor[4]);
        if (!Check(visualCode == RETAIL_WALK_LOAD_OK && visualResult.assetCount == 1 &&
                       visualResult.registeredFxCount == 1,
                   "b7_fx_visual_load"))
            return 1;
        if (!Check(visualResult.endCursor[0] == visualWalkResult.endCursor[0] &&
                       visualResult.endCursor[4] == visualWalkResult.endCursor[4],
                   "b7_fx_visual_cursor_parity"))
            return 1;
        const XAssetHeader visual = StubFindXAssetHeader(ASSET_TYPE_FX, "fx/vis");
        const FxEffectDef *fx = visual.fx;
        const Material *m0 =
            fx && fx->elemDefs ? fx->elemDefs[0].visuals.instance.material : nullptr;
        const Material *m1 =
            fx && fx->elemDefs ? fx->elemDefs[1].visuals.instance.material : nullptr;
        const FxTrailDef *trail = fx && fx->elemDefs ? fx->elemDefs[0].trailDef : nullptr;
        const bool visualOk = fx && fx->elemDefs && fx->elemDefCountOneShot == 2 &&
                              m0 != nullptr && m0 == m1 && m0->info.name &&
                              !std::strcmp(m0->info.name, "vis/mat") &&
                              m0->textureCount == 1 && m0->constantCount == 2 &&
                              m0->stateBitsCount == 2 && m0->textureTable &&
                              m0->constantTable && m0->stateBitsTable && trail &&
                              trail->vertCount == 2 && trail->verts &&
                              trail->indCount == 3 && trail->inds;
        std::printf("B7_FX_VISUAL fx=%p m0=%p m1=%p name='%s'\n",
                    static_cast<const void *>(fx), static_cast<const void *>(m0),
                    static_cast<const void *>(m1),
                    m0 && m0->info.name ? m0->info.name : "(null)");
        if (!Check(visualOk, "b7_fx_visual_alias_binds"))
            return 1;
        if (!Check(UnloadFixtureZone(visualResult) &&
                       !StubFindXAssetHeader(ASSET_TYPE_FX, "fx/vis").data,
                   "b7_fx_visual_unload"))
            return 1;
        std::puts("PASS:B7_FX_VISUAL material=live-recorded tables=bound "
                  "trail=bound slot_alias=bound cursor_parity=1 unload=clean");
    }

    // Small roots: PhysPreset and SndCurve register through
    // their engine owners with byte-exact fields, and unload leaves no
    // retired registry pointer (canary intact).
    {
        RetailWalkDirectoryRecord smallWalkRecords[2]{};
        RetailWalkDirectoryResult smallWalk{};
        if (!Check(RetailWalkFastfileDirectory("zone/english/small_live.ff", smallWalkRecords, 2,
                                               &smallWalk) == RETAIL_WALK_OK &&
                       smallWalk.walkedPhysPresetCount == 1 &&
                       smallWalk.walkedSndCurveCount == 1,
                   "b1_walk"))
            return 1;
        RetailWalkLoadZoneResult smallResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/small_live.ff", &smallResult) ==
                       RETAIL_WALK_LOAD_OK &&
                       smallResult.registeredPhysPresetCount == 1 &&
                       smallResult.registeredSndCurveCount == 1,
                   "b1_load"))
            return 1;
        const XAssetHeader phys = StubFindXAssetHeader(ASSET_TYPE_PHYSPRESET, "pp/test");
        const XAssetHeader curve = StubFindXAssetHeader(ASSET_TYPE_SOUND_CURVE, "curve/test");
        const bool b1Ok =
            phys.physPreset && phys.physPreset->name &&
            !std::strcmp(phys.physPreset->name, "pp/test") && phys.physPreset->type == 2 &&
            phys.physPreset->mass == 1.5f && phys.physPreset->bounce == 0.6f &&
            phys.physPreset->friction == 0.7f && phys.physPreset->bulletForceScale == 1.1f &&
            phys.physPreset->explosiveForceScale == 1.2f &&
            phys.physPreset->sndAliasPrefix &&
            !std::strcmp(phys.physPreset->sndAliasPrefix, "snd/") &&
            phys.physPreset->piecesSpreadFraction == 0.25f &&
            phys.physPreset->piecesUpwardVelocity == 3.0f &&
            phys.physPreset->tempDefaultToCylinder &&
            curve.sndCurve && curve.sndCurve->filename &&
            !std::strcmp(curve.sndCurve->filename, "curve/test") &&
            curve.sndCurve->knotCount == 2 && curve.sndCurve->knots[0][0] == 1.0f &&
            curve.sndCurve->knots[0][1] == 2.0f && curve.sndCurve->knots[1][0] == 3.0f &&
            curve.sndCurve->knots[1][1] == 4.0f;
        std::printf("B1_SMALL_ROOTS phys=%p curve=%p\n",
                    static_cast<const void *>(phys.physPreset),
                    static_cast<const void *>(curve.sndCurve));
        if (!Check(b1Ok, "b1_fields"))
            return 1;
        if (!Check(UnloadFixtureZone(smallResult) &&
                       !StubFindXAssetHeader(ASSET_TYPE_PHYSPRESET, "pp/test").data &&
                       !StubFindXAssetHeader(ASSET_TYPE_SOUND_CURVE, "curve/test").data,
                   "b1_unload"))
            return 1;
        std::puts("PASS:B1_SMALL_ROOTS physpreset=1 sndcurve=1 fields=exact unload=clean canaries=ok");
    }

    // LocalizeEntry: real, live-streamed registration.
    RetailWalkLoadZoneResult localizeResult{};
    if (!LoadFixtureZone("zone/english/localize.ff", &localizeResult, "localize_load") ||
        !Check(localizeResult.assetCount == 1 && localizeResult.registeredLocalizeCount == 1 &&
                   localizeResult.registeredFontCount == 0 && localizeResult.walkedOnlyCount == 0,
               "localize_counts") ||
        !Check(StubZoneIsLive(localizeResult.zoneIndex), "localize_zone_kept_live"))
        return 1;
    const XAssetHeader localizeHeader = StubFindXAssetHeader(ASSET_TYPE_LOCALIZE_ENTRY, "key");
    if (!Check(localizeHeader.localize != nullptr, "localize_findable") ||
        !Check(!std::strcmp(localizeHeader.localize->name, "key") &&
                   !std::strcmp(localizeHeader.localize->value, "value"),
               "localize_value"))
        return 1;

    // Font: real, live-streamed registration (null material/glowMaterial
    // refs in this fixture, so no dependency on an installed Material
    // decoder -- see db_retail_walk.cpp's RetailWalkLiveLoadFont comment).
    RetailWalkLoadZoneResult fontResult{};
    if (!LoadFixtureZone("zone/english/font.ff", &fontResult, "font_load") ||
        !Check(fontResult.assetCount == 1 && fontResult.registeredFontCount == 1 &&
                   fontResult.registeredLocalizeCount == 0 && fontResult.walkedOnlyCount == 0,
               "font_counts") ||
        !Check(StubZoneIsLive(fontResult.zoneIndex), "font_zone_kept_live"))
        return 1;
    const XAssetHeader fontHeader = StubFindXAssetHeader(ASSET_TYPE_FONT, "f");
    if (!Check(fontHeader.font != nullptr, "font_findable") ||
        !Check(fontHeader.font->glyphCount == 2 && fontHeader.font->material == nullptr &&
                   fontHeader.font->glowMaterial == nullptr && fontHeader.font->glyphs != nullptr,
               "font_value"))
        return 1;

    // Load_MaterialHandle pointer lookup (db_load.cpp:2643): the insert
    // form reserves a block-4 DB_InsertPointer slot and writes the published
    // material pointer into it; an offset form is DB_ConvertOffsetToAlias,
    // i.e. the value the insert owner wrote.  Font "decl" declares the
    // material inline (-2), font "alias" points at the reserved slot by
    // offset.  The port must bind the *same registered Material* for both,
    // never a by-name/directory/default substitute, and a non-null alias the
    // ledger cannot resolve must fail the load loudly.
    {
        RetailWalkLoadZoneResult aliasResult{};
        const RetailWalkLoadZoneResultCode aliasCode =
            RetailWalkLoadZoneAssets("zone/english/font_alias_live.ff", &aliasResult);
        const XAssetHeader declFont = StubFindXAssetHeader(ASSET_TYPE_FONT, "decl");
        const XAssetHeader aliasFont = StubFindXAssetHeader(ASSET_TYPE_FONT, "alias");
        const XAssetHeader declMaterial = StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "matdecl");
        if (!Check(aliasCode == RETAIL_WALK_LOAD_OK && aliasResult.code == RETAIL_WALK_LOAD_OK,
                   "font_alias_load") ||
            !Check(aliasResult.assetCount == 2 && aliasResult.registeredFontCount == 2,
                   "font_alias_counts") ||
            !Check(declFont.font && aliasFont.font && declMaterial.material, "font_alias_found") ||
            !Check(declFont.font->material != nullptr &&
                       declFont.font->material == aliasFont.font->material &&
                       declFont.font->material == declMaterial.material,
                   "font_alias_identity") ||
            !Check(aliasFont.font->material->info.name &&
                       !std::strcmp(aliasFont.font->material->info.name, "matdecl"),
                   "font_alias_name"))
            return 1;
        std::puts("KILLHOUSE_GRAPHICS d15_font_material decl=insert alias=offset identity=1 fallback=0");
    }
    {
        RetailWalkLoadZoneResult dangleResult{};
        if (!Check(RetailWalkLoadZoneAssets("zone/english/font_alias_dangle.ff", &dangleResult) !=
                       RETAIL_WALK_LOAD_OK,
                   "font_alias_dangle_fails"))
            return 1;
        std::puts("KILLHOUSE_GRAPHICS d15_font_material_dangle rejected=1");
    }

    // RawFile: real, live-streamed registration.
    RetailWalkLoadZoneResult rawfileResult{};
    if (!LoadFixtureZone("zone/english/rawfile_walk.ff", &rawfileResult, "rawfile_load") ||
        !Check(rawfileResult.assetCount == 1 && rawfileResult.registeredRawFileCount == 1 &&
                   rawfileResult.walkedOnlyCount == 0,
               "rawfile_counts"))
        return 1;
    const XAssetHeader rawfileHeader = StubFindXAssetHeader(ASSET_TYPE_RAWFILE, "raw");
    if (!Check(rawfileHeader.rawfile != nullptr, "rawfile_findable") ||
        !Check(rawfileHeader.rawfile->len == 5 &&
                   !std::strcmp(rawfileHeader.rawfile->buffer, "hello"),
               "rawfile_value"))
        return 1;

    // StringTable: real, live-streamed registration.  Uses the same
    // "presence marker, not -1" convention as RawFile's buffer slot for its
    // values table (valuesRef == 1 in this fixture).
    RetailWalkLoadZoneResult stringTableResult{};
    if (!LoadFixtureZone("zone/english/stringtable.ff", &stringTableResult, "stringtable_load") ||
        !Check(stringTableResult.assetCount == 1 &&
                   stringTableResult.registeredStringTableCount == 1 &&
                   stringTableResult.walkedOnlyCount == 0,
               "stringtable_counts"))
        return 1;
    const XAssetHeader stringTableHeader = StubFindXAssetHeader(ASSET_TYPE_STRINGTABLE, "t");
    if (!Check(stringTableHeader.stringTable != nullptr, "stringtable_findable") ||
        !Check(stringTableHeader.stringTable->columnCount == 1 &&
                   stringTableHeader.stringTable->rowCount == 2 &&
                   !std::strcmp(stringTableHeader.stringTable->values[0], "a") &&
                   !std::strcmp(stringTableHeader.stringTable->values[1], "b"),
               "stringtable_value"))
        return 1;

    // GameWorldSp path data: the singleton body must widen into the engine's
    // native gameWorldSp (nodes/links/base nodes/chains/vis/tree) so a map's
    // authored pathnode entities match the compiled nodeCount instead of
    // being reported as extras.  The fixture carries two nodes (node 0
    // with one inline link), one inline u16 chain array, a 3-byte vis buffer,
    // and a leaf plus an interior tree whose children cover all child forms.
    RetailWalkLoadZoneResult pathResult{};
    if (!LoadFixtureZone("zone/english/gameworldsp.ff", &pathResult, "pathdata_load") ||
        !Check(pathResult.assetCount == 1 && pathResult.registeredGameWorldSpCount == 1 &&
                   pathResult.walkedOnlyCount == 0 && StubZoneIsLive(pathResult.zoneIndex),
               "pathdata_counts"))
        return 1;
    const XAssetHeader pathHeader = StubFindXAssetHeader(ASSET_TYPE_GAMEWORLD_SP, "gw");
    if (!Check(pathHeader.gameWorldSp == &gameWorldSp, "pathdata_findable") ||
        !Check(gameWorldSp.name && !std::strcmp(gameWorldSp.name, "gw"), "pathdata_name") ||
        !Check(gameWorldSp.path.nodeCount == 2 && gameWorldSp.path.nodes != nullptr &&
                   gameWorldSp.path.basenodes != nullptr,
               "pathdata_nodes") ||
        !Check(gameWorldSp.path.nodes[0].constant.type == NODE_BADNODE &&
                   gameWorldSp.path.nodes[0].constant.totalLinkCount == 1 &&
                   gameWorldSp.path.nodes[0].constant.Links != nullptr &&
                   gameWorldSp.path.nodes[1].constant.totalLinkCount == 0 &&
                   gameWorldSp.path.nodes[1].constant.Links == nullptr,
               "pathdata_links") ||
        !Check(gameWorldSp.path.chainNodeCount == 2 &&
                   gameWorldSp.path.chainNodeForNode != nullptr &&
                   gameWorldSp.path.chainNodeForNode[0] == 1 &&
                   gameWorldSp.path.chainNodeForNode[1] == 0 &&
                   gameWorldSp.path.nodeForChainNode == nullptr,
               "pathdata_chains") ||
        !Check(gameWorldSp.path.visBytes == 3 && gameWorldSp.path.pathVis != nullptr &&
                   gameWorldSp.path.pathVis[0] == 1 && gameWorldSp.path.pathVis[1] == 2 &&
                   gameWorldSp.path.pathVis[2] == 3,
               "pathdata_vis"))
        return 1;
    if (!Check(gameWorldSp.path.nodeTreeCount == 2 && gameWorldSp.path.nodeTree != nullptr,
               "pathdata_tree_count") ||
        !Check(gameWorldSp.path.nodeTree[0].axis == -1 &&
                   gameWorldSp.path.nodeTree[0].u.s.nodeCount == 2 &&
                   gameWorldSp.path.nodeTree[0].u.s.nodes != nullptr &&
                   gameWorldSp.path.nodeTree[0].u.s.nodes[0] == 5 &&
                   gameWorldSp.path.nodeTree[0].u.s.nodes[1] == 6,
               "pathdata_tree_leaf") ||
        !Check(gameWorldSp.path.nodeTree[1].axis == 0 &&
                   gameWorldSp.path.nodeTree[1].u.child[0] != nullptr &&
                   gameWorldSp.path.nodeTree[1].u.child[1] != nullptr &&
                   gameWorldSp.path.nodeTree[1].u.child[0]->axis == -1 &&
                   gameWorldSp.path.nodeTree[1].u.child[0]->u.s.nodeCount == 1 &&
                   gameWorldSp.path.nodeTree[1].u.child[0]->u.s.nodes[0] == 7 &&
                   gameWorldSp.path.nodeTree[1].u.child[1]->u.s.nodeCount == 0 &&
                   gameWorldSp.path.nodeTree[1].u.child[1]->u.s.nodes == nullptr,
               "pathdata_tree_children"))
        return 1;

    // TechniqueSet: real, live-streamed registration (ported from
    // switch_sp_bootstrap.cpp's hardware-proven LoadTechniqueSet, adapted to
    // the zone's real arena/registration instead of Hunk_Alloc/
    // DB_RegisterMaterialTechniqueSet).  One inline technique with an
    // all-zero (no vertex decl, no shaders, no args) pass.
    RetailWalkLoadZoneResult techsetResult{};
    if (!LoadFixtureZone("zone/english/techset_live.ff", &techsetResult, "techset_load") ||
        !Check(techsetResult.assetCount == 1 && techsetResult.registeredTechniqueSetCount == 1 &&
                   techsetResult.walkedOnlyCount == 0,
               "techset_counts"))
        return 1;
    const XAssetHeader techsetHeader = StubFindXAssetHeader(ASSET_TYPE_TECHNIQUE_SET, "ts/live");
    if (!Check(techsetHeader.techniqueSet != nullptr, "techset_findable") ||
        !Check(techsetHeader.techniqueSet->worldVertFormat == 2 &&
                   techsetHeader.techniqueSet->techniques[0] != nullptr &&
                   techsetHeader.techniqueSet->techniques[0]->passCount == 1 &&
                   techsetHeader.techniqueSet->techniques[1] == nullptr,
               "techset_value"))
        return 1;

    // Image: real, live-streamed registration (ported from
    // switch_sp_bootstrap.cpp's hardware-proven WidenImage), including the
    // GfxImageLoadDef shape with pixel upload deliberately left null.
    RetailWalkLoadZoneResult imageResult{};
    if (!LoadFixtureZone("zone/english/image_live.ff", &imageResult, "image_load") ||
        !Check(imageResult.assetCount == 1 && imageResult.registeredImageCount == 1 &&
                   imageResult.walkedOnlyCount == 0,
               "image_counts"))
        return 1;
    const XAssetHeader imageHeader = StubFindXAssetHeader(ASSET_TYPE_IMAGE, "img/live");
    if (!Check(imageHeader.image != nullptr, "image_findable") ||
        !Check(imageHeader.image->width == 64 && imageHeader.image->height == 64 &&
                   imageHeader.image->texture.loadDef != nullptr &&
                   imageHeader.image->texture.loadDef->levelCount == 1,
               "image_value"))
        return 1;

    // Material: real, live-streamed registration (ported from
    // switch_sp_bootstrap.cpp's hardware-proven per-entry material walk).
    // One zone carrying a TechniqueSet followed by a Material bound to it
    // by a real absolute-offset name token, no textures.
    RetailWalkLoadZoneResult materialResult{};
    if (!LoadFixtureZone("zone/english/material_live.ff", &materialResult, "material_load") ||
        !Check(materialResult.assetCount == 2 && materialResult.registeredTechniqueSetCount == 1 &&
                   materialResult.registeredMaterialCount == 1 &&
                   materialResult.walkedOnlyCount == 0,
               "material_counts"))
        return 1;
    const XAssetHeader materialHeader = StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "mat/live");
    if (!Check(materialHeader.material != nullptr, "material_findable") ||
        !Check(materialHeader.material->techniqueSet != nullptr &&
                   !std::strcmp(materialHeader.material->techniqueSet->name, "ts/live") &&
                   materialHeader.material->textureCount == 0,
               "material_value"))
        return 1;

    // Material comma-stub directory-offset binding: a *second* zone whose
    // own directory entry 0 is an empty comma-stub TechniqueSet (",ts/live")
    // and whose entry 1 is a Material whose techniqueSetRef names entry 0's
    // own directory-slot offset rather than a separate name string --
    // switch_sp_bootstrap.cpp's real ui.ff convention for binding a stub
    // back to an original registered by an earlier-loaded zone (here,
    // "ts/live" from the techset_load zone above).  Proves
    // RetailWalkLiveLoadMaterial's directoryBytes/stubNames plumbing, not
    // just the plain-name-token path material_load above already proves.
    RetailWalkLoadZoneResult stubResult{};
    if (!LoadFixtureZone("zone/english/material_stub_live.ff", &stubResult, "material_stub_load") ||
        !Check(stubResult.assetCount == 2 && stubResult.registeredTechniqueSetCount == 1 &&
                   stubResult.registeredMaterialCount == 1 && stubResult.walkedOnlyCount == 0,
               "material_stub_counts"))
        return 1;
    const XAssetHeader stubMaterialHeader = StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "mat/stub");
    if (!Check(stubMaterialHeader.material != nullptr, "material_stub_findable") ||
        // The resolved technique set must be the *original* "ts/live" from
        // the earlier zone by identity, not merely by name; the comma stub's
        // widened body is never registered separately (db_registry.cpp
        // DB_LinkXAssetEntry's stub branch returns the existing owner before
        // the registration/override bookkeeping), so a `,ts/live` registry
        // entry would be exactly the host-double artifact the generic stub
        // branch removed.
        !Check(stubMaterialHeader.material->techniqueSet == techsetHeader.techniqueSet,
               "material_stub_binds_original") ||
        !Check(StubFindXAssetHeader(ASSET_TYPE_TECHNIQUE_SET, ",ts/live").techniqueSet == nullptr,
               "material_stub_not_registered_separately"))
        return 1;

    // MenuList/Menu: real, live-streamed registration (ported from
    // switch_sp_bootstrap.cpp's hardware-proven ui-menulist walk, via
    // db_retail_decode_ui.cpp's already-proven RetailDecodeMenu/MenuList
    // widening bridged from the FS reader's own block buffers).  One
    // MenuList with one Menu, matching the real retail "main" container
    // menu's shape (itemCount == 0).
    RetailWalkLoadZoneResult menuListResult{};
    if (!LoadFixtureZone("zone/english/menulist_live.ff", &menuListResult, "menulist_load") ||
        !Check(menuListResult.assetCount == 1 && menuListResult.registeredMenuListCount == 1 &&
                   menuListResult.walkedOnlyCount == 0,
               "menulist_counts"))
        return 1;
    const XAssetHeader menuListHeader = StubFindXAssetHeader(ASSET_TYPE_MENULIST, "list");
    if (!Check(menuListHeader.menuList != nullptr, "menulist_findable") ||
        !Check(menuListHeader.menuList->menuCount == 1 && menuListHeader.menuList->menus[0] &&
                   !std::strcmp(menuListHeader.menuList->menus[0]->window.name, "main") &&
                   menuListHeader.menuList->menus[0]->itemCount == 1,
               "menulist_value") ||
        !Check(menuListHeader.menuList->menus[0]->items[0] != nullptr &&
                   !std::strcmp(menuListHeader.menuList->menus[0]->items[0]->window.name, "item0") &&
                   menuListHeader.menuList->menus[0]->items[0]->parent ==
                       menuListHeader.menuList->menus[0] &&
                   menuListHeader.menuList->menus[0]->items[0]->typeData.editField != nullptr,
               "menulist_item_value"))
        return 1;

    std::puts("PASS:RETAIL_BOOT_LOAD_PROOF localize=1 font=1 rawfile=1 stringtable=1 "
              "techniqueset=1 image=1 material=1 menulist=1 zones_kept_live=8");

    // Block semantics, synthetic half (always runs): runtime-only block 1
    // expansion against block1.ff. The fixture stream carries exactly the
    // 16-byte list plus the 8-byte directory, so reaching the directory and
    // then failing a trailing stream read proves expansion consumed zero FS
    // bytes. Nonzero delay blocks fail the open loudly by design.
    {
        RetailZoneLoadSession session{};
        FsRetailFastfileReader *reader = nullptr;
        FsRetailFastfileAsset *assets = nullptr;
        RetailWalkDirectoryRecord records[1]{};
        RetailWalkDirectoryResult openResult{};
        if (!Check(RetailWalkOpenDirectory("zone/english/block1.ff", &session, &reader, &assets,
                                           records, 1, &openResult) == RETAIL_WALK_OK &&
                       openResult.code == RETAIL_WALK_OK &&
                       openResult.assetCount == 1 && openResult.recordedCount == 1 &&
                       records[0].type == ASSET_TYPE_XMODELPIECES &&
                       records[0].header == 0 &&
                       records[0].state == RETAIL_WALK_NO_STREAM,
                   "m2_block1_open"))
            return 1;
        const uint32_t cursor0 = session.wire.cursor[0];
        const uint32_t cursor4 = session.wire.cursor[4];
        bool ok = session.zoneMemory->blocks[1].size == 64 &&
                  RetailZoneLoadSessionExpandRuntime(&session, 1, 1, 1) &&
                  session.wire.cursor[1] == 1 &&
                  RetailZoneLoadSessionExpandRuntime(&session, 1, 1, 4) &&
                  session.wire.cursor[1] == 5 &&
                  RetailZoneLoadSessionExpandRuntime(&session, 1, 59, 1) &&
                  session.wire.cursor[1] == 64;
        for (uint32_t i = 0; ok && i < 64; ++i)
            ok = session.zoneMemory->blocks[1].data[i] == 0;
        ok = ok && session.wire.cursor[0] == cursor0 && session.wire.cursor[4] == cursor4 &&
             !RetailZoneLoadSessionExpandRuntime(&session, 1, 1, 1) &&
             session.wire.cursor[1] == 64 &&
             !RetailZoneLoadSessionExpandRuntime(&session, 0, 1, 1) &&
             !RetailZoneLoadSessionExpandRuntime(&session, 2, 1, 1) &&
             !RetailZoneLoadSessionExpandRuntime(&session, 9, 1, 1) &&
             session.wire.cursor[1] == 64 &&
             !RetailZoneLoadSessionReadStream(&session, reader, 4, 1000000, 1) &&
             session.wire.cursor[4] == cursor4;
        const uint32_t zoneIndex = session.zoneIndex;
        ok = ok && RetailZoneLoadSessionAbort(&session) && !StubZoneIsLive(zoneIndex);
        delete[] assets;
        if (reader)
            FS_CloseRetailFastfile(reader);
        if (!Check(ok, "m2_block1_semantics"))
            return 1;
    }
    {
        RetailZoneLoadSession session{};
        FsRetailFastfileReader *reader = nullptr;
        FsRetailFastfileAsset *assets = nullptr;
        RetailWalkDirectoryRecord records[1]{};
        RetailWalkDirectoryResult result2{};
        RetailWalkDirectoryResult result3{};
        if (!Check(RetailWalkOpenDirectory("zone/english/delayed2.ff", &session, &reader, &assets,
                                           records, 1, &result2) == RETAIL_WALK_SESSION_FAILED &&
                       result2.code == RETAIL_WALK_SESSION_FAILED && reader == nullptr &&
                       assets == nullptr && !session.active && session.zoneIndex == 0 &&
                       RetailWalkOpenDirectory("zone/english/delayed3.ff", &session, &reader,
                                               &assets, records, 1, &result3) ==
                           RETAIL_WALK_SESSION_FAILED &&
                       result3.code == RETAIL_WALK_SESSION_FAILED && reader == nullptr &&
                       assets == nullptr && !session.active && session.zoneIndex == 0,
                   "m2_delayed_reject"))
            return 1;
    }

    // Mandatory graphics loaders: exact
    // TechniqueSet alias identity, world-shaped Material variants, 2D Image
    // variants, and real LightDef attenuation, all in strict acceptance
    // mode (exact typed aliases/inserts or loud failure; no synthesis, no
    // hardcoded identities, no white/$default substitution, no name
    // guessing). Fixture-only, so it runs in every context: every zone it
    // keeps live is aborted before returning, and the stub registry is
    // identical on exit (proven below, not trusted).
    uint32_t registeredHigh = StubRegisteredCount();
    uint32_t m6TechSets = 0, m6Materials = 0, m6Images = 0;
    uint32_t m6Shaders = 0, m6Decls = 0, m6Textures = 0;
    const uint32_t m6RegEntry = StubRegisteredCount();
    {
        // Exact alias: slot 1 must BE slot 0's technique (pointer
        // identity, the retail ord-1222 shape), slot 2 a distinct second
        // technique; the full pass (decl, named vs/ps, arg) widened.
        RetailWalkLoadZoneResult aliasResult{};
        if (!LoadFixtureZone("zone/english/techset_alias_live.ff", &aliasResult, "m6_alias_load") ||
            !Check(aliasResult.assetCount == 1 && aliasResult.registeredTechniqueSetCount == 1 &&
                        aliasResult.walkedOnlyCount == 0,
                    "m6_alias_counts"))
            return 1;
        const XAssetHeader aliasHeader = StubFindXAssetHeader(ASSET_TYPE_TECHNIQUE_SET, "ts/alias");
        bool ok = aliasHeader.techniqueSet != nullptr &&
                  !std::strcmp(aliasHeader.techniqueSet->name, "ts/alias") &&
                  aliasHeader.techniqueSet->techniques[0] != nullptr &&
                  aliasHeader.techniqueSet->techniques[1] == aliasHeader.techniqueSet->techniques[0] &&
                  aliasHeader.techniqueSet->techniques[2] != nullptr &&
                  aliasHeader.techniqueSet->techniques[2] != aliasHeader.techniqueSet->techniques[0] &&
                  !std::strcmp(aliasHeader.techniqueSet->techniques[0]->name, "techA") &&
                  !std::strcmp(aliasHeader.techniqueSet->techniques[2]->name, "techB");
        const MaterialPass &aliasPass = aliasHeader.techniqueSet->techniques[0]->passArray[0];
        ok = ok && aliasPass.vertexDecl != nullptr && aliasPass.vertexShader != nullptr &&
             aliasPass.pixelShader != nullptr &&
             aliasPass.vertexShader->name != nullptr &&
             !std::strcmp(aliasPass.vertexShader->name, "vs/a") &&
             aliasPass.pixelShader->name != nullptr &&
             !std::strcmp(aliasPass.pixelShader->name, "ps/a") &&
             aliasPass.vertexShader->prog.loadDef.program != nullptr &&
             aliasPass.pixelShader->prog.loadDef.program != nullptr &&
             aliasPass.stableArgCount == 1 && aliasPass.args != nullptr &&
             aliasPass.args[0].type == 4 &&
             static_cast<uint32_t>(aliasPass.args[0].u.codeSampler) == 7u;
        if (!Check(ok, "m6_alias_identity"))
            return 1;
        ++m6TechSets;
        m6Shaders += 2;
        m6Decls += 1;
        std::printf("KILLHOUSE_GRAPHICS alias techsets=1 shaders=2 declarations=1 args=1\n");
        if (StubRegisteredCount() > registeredHigh)
            registeredHigh = StubRegisteredCount();
        ok = UnloadFixtureZone(aliasResult) &&
             StubFindXAssetHeader(ASSET_TYPE_TECHNIQUE_SET, "ts/alias").data == nullptr &&
             StubRetiredPtrCount() == 0;
        if (!Check(ok, "m6_alias_unload"))
            return 1;
    }
    // reproduce the two shader states a drawn pass can never use
    // -- a program the device never created, and a variant cooked for the
    // other renderer -- against the predicate Material_GetTechnique uses at
    // draw time.  The runtime guard turns either into MyAssertHandler; this
    // is the executable negative for it (no D3D device needed).
    {
        MaterialPixelShader ps{};
        MaterialVertexShader vs{};
        MaterialPass pass{};
        pass.pixelShader = &ps;
        pass.vertexShader = &vs;
        ps.prog.loadDef.loadForRenderer = 1;
        vs.prog.loadDef.loadForRenderer = 1;
        ps.prog.ps = reinterpret_cast<IDirect3DPixelShader9 *>(&pass);
        vs.prog.vs = reinterpret_cast<IDirect3DVertexShader9 *>(&pass);
        bool ok = MaterialPassShadersDrawable(&pass, 1);
        if (!Check(ok, "d5_match_drawable"))
            return 1;
        ps.prog.ps = nullptr;
        ok = !MaterialPassShadersDrawable(&pass, 1);
        if (!Check(ok, "d5_null_program_rejected"))
            return 1;
        ps.prog.ps = reinterpret_cast<IDirect3DPixelShader9 *>(&pass);
        vs.prog.vs = nullptr;
        ok = !MaterialPassShadersDrawable(&pass, 1);
        if (!Check(ok, "d5_null_vs_rejected"))
            return 1;
        vs.prog.vs = reinterpret_cast<IDirect3DVertexShader9 *>(&pass);
        ps.prog.loadDef.loadForRenderer = 0;
        ok = !MaterialPassShadersDrawable(&pass, 1);
        if (!Check(ok, "d5_ps_renderer_mismatch_rejected"))
            return 1;
        ps.prog.loadDef.loadForRenderer = 1;
        vs.prog.loadDef.loadForRenderer = 0;
        ok = !MaterialPassShadersDrawable(&pass, 1);
        if (!Check(ok, "d5_vs_renderer_mismatch_rejected"))
            return 1;
        pass.pixelShader = nullptr;
        ok = !MaterialPassShadersDrawable(&pass, 1);
        if (!Check(ok, "d5_null_shader_rejected"))
            return 1;
        std::printf("KILLHOUSE_GRAPHICS d5_pass_predicate match=ok "
                    "null_program=rejected null_vs=rejected "
                    "ps_mismatch=rejected vs_mismatch=rejected null_shader=rejected\n");
    }
    {
        // World-shaped Material, strict: absolute techset binding, two
        // real inline images, one constant and state bits; every identity
        // asserted, nothing substituted (the stub table holds no white,
        // $default, or IWI files, so any substitution path would fail
        // rather than hide here).
        RetailWalkLoadZoneResult worldResult{};
        const RetailWalkLoadZoneResultCode worldCode = RetailWalkLoadZoneAssets(
            "zone/english/material_world_live.ff", &worldResult, true);
        if (!Check(worldCode == RETAIL_WALK_LOAD_OK && worldResult.code == RETAIL_WALK_LOAD_OK,
                    "m6_world_load") ||
            !Check(worldResult.assetCount == 2 && worldResult.registeredTechniqueSetCount == 1 &&
                        worldResult.registeredMaterialCount == 1 &&
                        worldResult.walkedOnlyCount == 0,
                    "m6_world_counts"))
            return 1;
        const XAssetHeader worldTsHeader =
            StubFindXAssetHeader(ASSET_TYPE_TECHNIQUE_SET, "ts/world");
        const XAssetHeader worldMatHeader =
            StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "mat/world");
        if (!Check(worldTsHeader.techniqueSet != nullptr && worldMatHeader.material != nullptr &&
                        !std::strcmp(worldMatHeader.material->info.name, "mat/world"),
                    "m6_world_found") ||
            !Check(worldMatHeader.material->techniqueSet == worldTsHeader.techniqueSet,
                    "m6_world_techset") ||
            !Check(worldMatHeader.material->textureCount == 2 &&
                        worldMatHeader.material->constantCount == 1 &&
                        worldMatHeader.material->stateBitsCount == 1,
                    "m6_world_counts2"))
            return 1;
        const MaterialTextureDef *texTable = worldMatHeader.material->textureTable;
        if (!Check(texTable != nullptr && texTable[0].u.image != nullptr &&
                        texTable[1].u.image != nullptr,
                    "m6_world_texbound") ||
            !Check(!std::strcmp(texTable[0].u.image->name, "img/w0") &&
                        texTable[0].u.image->width == 32 && texTable[0].u.image->height == 32 &&
                        !std::strcmp(texTable[1].u.image->name, "img/w1") &&
                        texTable[1].u.image->width == 16 && texTable[1].u.image->height == 16,
                    "m6_world_texnames"))
            return 1;
        if (!Check(worldMatHeader.material->constantTable != nullptr &&
                        worldMatHeader.material->constantTable[0].nameHash == 0x12345678u &&
                        worldMatHeader.material->constantTable[0].literal[0] == 1.0f,
                    "m6_world_const") ||
            !Check(worldMatHeader.material->stateBitsTable != nullptr &&
                        worldMatHeader.material->stateBitsTable[0].loadBits[0] == 0x11111111u &&
                        worldMatHeader.material->stateBitsTable[0].loadBits[1] == 0x22222222u,
                    "m6_world_state"))
            return 1;
        ++m6TechSets;
        ++m6Materials;
        m6Images += 2;
        m6Textures += 2;
        std::printf("KILLHOUSE_GRAPHICS world techsets=1 materials=1 images=2 textures=2\n");
        if (StubRegisteredCount() > registeredHigh)
            registeredHigh = StubRegisteredCount();
        const bool worldUnloaded =
            UnloadFixtureZone(worldResult) &&
            StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "mat/world").data == nullptr &&
            StubRetiredPtrCount() == 0;
        if (!Check(worldUnloaded, "m6_world_unload"))
            return 1;
    }
    {
        // LightDef real attenuation: the nested inline image widens for
        // real and binds -- never the old fake 1x1 "$white".
        RetailWalkLoadZoneResult ldResult{};
        if (!LoadFixtureZone("zone/english/lightdef_live.ff", &ldResult, "m6_lightdef_load") ||
            !Check(ldResult.assetCount == 1 && ldResult.registeredLightDefCount == 1 &&
                        ldResult.walkedOnlyCount == 0,
                    "m6_lightdef_counts"))
            return 1;
        const XAssetHeader ldHeader = StubFindXAssetHeader(ASSET_TYPE_LIGHT_DEF, "ld/light");
        bool ok = ldHeader.lightDef != nullptr &&
                  ldHeader.lightDef->name != nullptr &&
                  !std::strcmp(ldHeader.lightDef->name, "ld/light") &&
                  ldHeader.lightDef->attenuation.samplerState == 7 &&
                  ldHeader.lightDef->lmapLookupStart == 3 &&
                  ldHeader.lightDef->attenuation.image != nullptr &&
                  !std::strcmp(ldHeader.lightDef->attenuation.image->name, "img/atten") &&
                  std::strcmp(ldHeader.lightDef->attenuation.image->name, "$white") != 0 &&
                  ldHeader.lightDef->attenuation.image->width == 8 &&
                  ldHeader.lightDef->attenuation.image->height == 8;
        if (!Check(ok, "m6_lightdef_attenuation"))
            return 1;
        ++m6Images;
        std::printf("KILLHOUSE_GRAPHICS lightdef lightdefs=1 images=1 attenuation=real\n");
        if (StubRegisteredCount() > registeredHigh)
            registeredHigh = StubRegisteredCount();
        ok = UnloadFixtureZone(ldResult) &&
             StubFindXAssetHeader(ASSET_TYPE_LIGHT_DEF, "ld/light").data == nullptr &&
             StubRetiredPtrCount() == 0;
        if (!Check(ok, "m6_lightdef_unload"))
            return 1;
    }
    {
        // LightDef alias-name lifetime: the LightDef references a name
        // string the zone's earlier PhysPreset declared inline (the exact
        // killhouse.ff light_point_linear shape: nameRef = raw block-4
        // offset). DB_GetXAssetName keys the registry off lightDef->name, so
        // the loader must own a copy for the zone's lifetime -- the reader's
        // block-4 mirror is freed with the reader at the end of the load.
        // Pre-fix this lookup reads freed reader memory (ASan use-after-free).
        RetailWalkLoadZoneResult aliasResult{};
        const RetailWalkLoadZoneResultCode aliasCode =
            RetailWalkLoadZoneAssets("zone/english/lightdef_alias_live.ff", &aliasResult);
        if (!Check(aliasCode == RETAIL_WALK_LOAD_OK && aliasResult.code == RETAIL_WALK_LOAD_OK &&
                       aliasResult.assetCount == 2 &&
                       aliasResult.registeredPhysPresetCount == 1 &&
                       aliasResult.registeredLightDefCount == 1 &&
                       aliasResult.walkedOnlyCount == 0,
                   "d30_alias_load"))
            return 1;
        const XAssetHeader aliasLd =
            StubFindXAssetHeader(ASSET_TYPE_LIGHT_DEF, "ld/alias");
        const XAssetHeader aliasPreset =
            StubFindXAssetHeader(ASSET_TYPE_PHYSPRESET, "ld/alias");
        const bool aliasOk =
            aliasLd.lightDef != nullptr && aliasLd.lightDef->name != nullptr &&
            !std::strcmp(aliasLd.lightDef->name, "ld/alias") &&
            aliasLd.lightDef->attenuation.samplerState == 9 &&
            aliasLd.lightDef->lmapLookupStart == 5 &&
            aliasLd.lightDef->attenuation.image == nullptr &&
            aliasPreset.physPreset != nullptr;
        std::printf("D30_LIGHTDEF_ALIAS_NAME registered=1 bound=%d preset=%d name_owned=%d\n",
                    aliasLd.lightDef != nullptr ? 1 : 0, aliasPreset.physPreset ? 1 : 0,
                    (aliasLd.lightDef && aliasLd.lightDef->name &&
                     !std::strcmp(aliasLd.lightDef->name, "ld/alias"))
                        ? 1
                        : 0);
        if (!Check(aliasOk, "d30_alias_name_owned"))
            return 1;
        const bool aliasUnloaded =
            UnloadFixtureZone(aliasResult) &&
            StubFindXAssetHeader(ASSET_TYPE_LIGHT_DEF, "ld/alias").data == nullptr &&
            StubFindXAssetHeader(ASSET_TYPE_PHYSPRESET, "ld/alias").data == nullptr &&
            StubRetiredPtrCount() == 0;
        if (!Check(aliasUnloaded, "d30_alias_unload"))
            return 1;
        std::puts("PASS:D30_LIGHTDEF_ALIAS_LIFETIME name=zone-owned lookup_after_close=1 unload=clean");
    }
    {
        // Negatives: every unsupported form fails loudly instead of
        // substituting. A dangling technique alias; a strict material
        // whose texture names nothing registered; a strict cube-map image
        // (no cube upload semantics exist); a strict comma stub with no
        // registered original. The cube case additionally proves the flag
        // is what decides: tolerant mode still widens it (zone aborted
        // right after, like every graphics-loader zone).
        RetailWalkLoadZoneResult dangResult{};
        const RetailWalkLoadZoneResultCode dangCode = RetailWalkLoadZoneAssets(
            "zone/english/techset_dangling_live.ff", &dangResult);
        if (!Check(dangCode != RETAIL_WALK_LOAD_OK, "m6_negative_dangle"))
            return 1;
        RetailWalkLoadZoneResult missResult{};
        const RetailWalkLoadZoneResultCode missCode = RetailWalkLoadZoneAssets(
            "zone/english/material_missing_live.ff", &missResult, true);
        if (!Check(missCode != RETAIL_WALK_LOAD_OK, "m6_negative_missing"))
            return 1;
        RetailWalkLoadZoneResult cubeStrictResult{};
        const RetailWalkLoadZoneResultCode cubeStrictCode = RetailWalkLoadZoneAssets(
            "zone/english/image_cube_live.ff", &cubeStrictResult, true);
        // Cube maps widen exactly in strict mode too (registry data, not
        // upload): map type preserved, never downgraded to 2D. Upload
        // itself stays gated at the device seam.
        bool ok = cubeStrictCode == RETAIL_WALK_LOAD_OK &&
                  cubeStrictResult.code == RETAIL_WALK_LOAD_OK &&
                  cubeStrictResult.registeredImageCount == 1;
        const XAssetHeader cubeStrictHeader =
            StubFindXAssetHeader(ASSET_TYPE_IMAGE, "img/cube");
        ok = ok && cubeStrictHeader.image != nullptr &&
             cubeStrictHeader.image->mapType == 5 &&
             cubeStrictHeader.image->width == 16 && cubeStrictHeader.image->height == 16;
        if (!Check(ok, "m6_cube_strict"))
            return 1;
        ok = UnloadFixtureZone(cubeStrictResult) &&
             StubFindXAssetHeader(ASSET_TYPE_IMAGE, "img/cube").data == nullptr &&
             StubRetiredPtrCount() == 0;
        if (!Check(ok, "m6_cube_strict_unload"))
            return 1;
        std::printf("KILLHOUSE_GRAPHICS cube mapType=5 strict=ok\n");
        RetailWalkLoadZoneResult cubeResult{};
        const RetailWalkLoadZoneResultCode cubeCode = RetailWalkLoadZoneAssets(
            "zone/english/image_cube_live.ff", &cubeResult);
        ok = cubeCode == RETAIL_WALK_LOAD_OK && cubeResult.code == RETAIL_WALK_LOAD_OK &&
             cubeResult.registeredImageCount == 1;
        const XAssetHeader cubeHeader = StubFindXAssetHeader(ASSET_TYPE_IMAGE, "img/cube");
        ok = ok && cubeHeader.image != nullptr && cubeHeader.image->mapType == 5;
        if (!Check(ok, "m6_cube_tolerant"))
            return 1;
        ok = UnloadFixtureZone(cubeResult) &&
             StubRetiredPtrCount() == 0;
        if (!Check(ok, "m6_cube_unload"))
            return 1;
        RetailWalkLoadZoneResult commaResult{};
        const RetailWalkLoadZoneResultCode commaCode = RetailWalkLoadZoneAssets(
            "zone/english/material_comma_miss_live.ff", &commaResult, true);
        if (!Check(commaCode != RETAIL_WALK_LOAD_OK, "m6_negative_comma"))
            return 1;
        // tolerant comma-stub contract.  The original
        // DB_LinkXAssetEntry stub branch resolves a comma name with no owner
        // to the type's default entry (DB_CreateDefaultEntry via
        // DB_FindXAssetHeader); it never manufactures a body.  With the
        // engine default material installed, the host DB double now mirrors
        // that on-demand registration, so this asserts the exact restored
        // side effect: the stub resolves to the default material and the
        // RetailMaterial_Synthesize2D fallback is not entered.
        {
            Material defaultMaterial{};
            defaultMaterial.info.name = const_cast<char *>("$default");
            const uint32_t synthesizedBefore = RetailMaterialSynthesizedCount();
            StubInstallDefaultMaterial(&defaultMaterial);
            RetailWalkLoadZoneResult commaTolerantResult{};
            const RetailWalkLoadZoneResultCode commaTolerantCode =
                RetailWalkLoadZoneAssets("zone/english/material_comma_miss_live.ff",
                                         &commaTolerantResult);
            const XAssetHeader commaTolerant =
                DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, "ts/nope");
            const bool commaTolerantOk =
                commaTolerantCode == RETAIL_WALK_LOAD_OK &&
                commaTolerantResult.code == RETAIL_WALK_LOAD_OK &&
                RetailMaterialSynthesizedCount() == synthesizedBefore &&
                StubDefaultEntryCount() == 1 &&
                commaTolerant.material == &defaultMaterial &&
                StubFindXAssetHeader(ASSET_TYPE_MATERIAL, ",ts/nope").data == nullptr;
            StubInstallDefaultMaterial(nullptr);
            if (!Check(commaTolerantOk, "m6_comma_tolerant_default"))
                return 1;
            const bool commaTolerantUnload =
                UnloadFixtureZone(commaTolerantResult) &&
                StubRetiredPtrCount() == 0;
            if (!Check(commaTolerantUnload, "m6_comma_tolerant_unload"))
                return 1;
            std::printf("KILLHOUSE_GRAPHICS comma-tolerated default=real body=0 synth=0\n");
        }
        std::printf("KILLHOUSE_GRAPHICS negative dangle=fail missing=fail comma=fail\n");
    }
    {
        // Leaves the stub registry exactly as it found it: every kept-
        // live zone above was aborted, every failure path tore itself down.
        const bool ok = StubRegisteredCount() == m6RegEntry && StubRetiredPtrCount() == 0;
        std::printf("KILLHOUSE_GRAPHICS summary materials=%u techsets=%u images=%u shaders=%u declarations=%u textures=%u defaults=0 placeholders=0 unload=clean retired=%u\n",
                    m6Materials, m6TechSets, m6Images, m6Shaders, m6Decls, m6Textures,
                    StubRetiredPtrCount());
        if (!Check(ok, "m6_registry_restored"))
            return 1;
    }
    {
        // a comma-material alias whose only real owner lives outside the
        // SP checkpoint closure must not be retained as a required deferred
        // alias.  $levelbriefing's owner is mp_killhouse_load.ff (OAT:
        // material $levelbriefing + image loadscreen_mp_killhouse); SP
        // killhouse.ff carries only the `,$levelbriefing` directory stub.
        // Before this fix the walk deferred it forever and the zone load
        // still reported success.
        const uint32_t deferredBefore = RetailMaterialDeferredAliasCount();
        RetailWalkLoadZoneResult nonSpResult{};
        if (!LoadFixtureZone("zone/english/material_non_sp_alias_live.ff", &nonSpResult,
                             "d18_non_sp_alias_load"))
            return 1;
        const bool nonSpOk =
            RetailMaterialDeferredAliasCount() == deferredBefore &&
            StubFindXAssetHeader(ASSET_TYPE_MATERIAL, ",$levelbriefing").data == nullptr;
        const bool nonSpUnload = UnloadFixtureZone(nonSpResult);
        if (!Check(nonSpOk, "d18_non_sp_alias_skipped") ||
            !Check(nonSpUnload, "d18_non_sp_alias_unload"))
            return 1;
        std::printf("KILLHOUSE_GRAPHICS d18_non_sp_alias skip=1 deferred=0\n");

        // Measurement + resolution: an owner that arrives later patches
        // the consumer slot and clears the ledger; until then the outstanding
        // count is the partial-load signal the walk checkpoint
        // requires to be zero.
        Material owner{};
        owner.info.name = const_cast<char *>("$victorybackdrop");
        GfxImage ownerImage{};
        ownerImage.texture.loadDef = reinterpret_cast<GfxImageLoadDef *>(1);
        MaterialTextureDef ownerDef{};
        ownerDef.semantic = 0;
        ownerDef.u.image = &ownerImage;
        owner.textureTable = &ownerDef;
        Material *slot = nullptr;
        XAssetHeader ownerHeader{};
        ownerHeader.material = &owner;
        StubRegisterStockAsset(ASSET_TYPE_MATERIAL, ownerHeader);
        const bool deferred = RetailDeferMaterialAlias(&slot, ",$victorybackdrop", 0);
        const uint32_t outstanding = RetailMaterialDeferredAliasCount();
        const uint32_t unresolved = RetailResolveDeferredMaterialAliases(false);
        const bool resolveOk =
            deferred && outstanding == 1 && unresolved == 0 &&
            slot == &owner && RetailMaterialDeferredAliasCount() == 0;
        const bool ownerRemoved = StubUnregisterStockAsset(ASSET_TYPE_MATERIAL, ownerHeader);
        if (!Check(resolveOk, "d18_required_alias_resolved") ||
            !Check(ownerRemoved, "d18_alias_owner_cleanup"))
            return 1;
        std::printf("KILLHOUSE_GRAPHICS d18_required_alias deferred=1 resolved=1 outstanding=0\n");
    }
    {
        // Host proof: the native decoder must publish the canonical
        // engine singleton only after the authored definition name resolves.
        GfxLightDef def{};
        def.name = "l0";
        StubRegisterStockAsset(ASSET_TYPE_LIGHT_DEF, {&def});
        RetailWalkLoadZoneResult comResult{};
        const RetailWalkLoadZoneResultCode comCode =
            RetailWalkLoadZoneAssets("zone/english/comworld.ff", &comResult);
        const XAssetHeader comHeader = StubFindXAssetHeader(ASSET_TYPE_COMWORLD, "com");
        const bool ok = comCode == RETAIL_WALK_LOAD_OK &&
                        comResult.registeredComWorldCount == 1 &&
                        comHeader.comWorld == &comWorld && comWorld.name &&
                        !std::strcmp(comWorld.name, "com") && !comWorld.isInUse &&
                        comWorld.primaryLightCount == 2 && comWorld.primaryLights &&
                        comWorld.primaryLights[0].defName &&
                        !std::strcmp(comWorld.primaryLights[0].defName, "l0") &&
                        comWorld.primaryLights[1].defName == nullptr;
        if (!Check(ok, "m9_comworld_singleton") ||
            !Check(DB_RetailZoneEnd(comResult.zoneIndex) &&
                       StubFindXAssetHeader(ASSET_TYPE_COMWORLD, "com").data == nullptr &&
                       StubUnregisterStockAsset(ASSET_TYPE_LIGHT_DEF, {&def}),
                   "m9_comworld_unload"))
            return 1;
        std::puts("PASS:KILLHOUSE_COMWORLD_ACTIVATION lights=2 defs=1 singleton=canonical");
        comWorld = ComWorld{};
    }

    // Native ClipMap registry: walk-first dual agreement
    // over clipmap_live.ff (the walk-only reader must consume the same
    // stream the live driver widens), then the live load widening the
    // collision graph into a transaction committed atomically into cm.
    // Rollback runs the box-alias dangle variant (the singleton stays
    // bit-identical, partial MapEnts swept); unload ends the zone and
    // clears cm. Fixture-only, like the GfxWorld fixture.
    //
    // This retires PASS:CLIPMAP_NESTED_MAPENTS: the nested MapEnts leaf
    // now registers inside the real ClipMap driver (asserted below), not
    // through the removed capture bridge.
    {
        RetailWalkDirectoryRecord walkRecords[1]{};
        RetailWalkDirectoryResult walkResult{};
        const uint32_t walkRegBefore = StubRegisteredCount();
        const RetailWalkDirectoryResultCode walkCode = RetailWalkFastfileDirectory(
            "zone/english/clipmap_live.ff", walkRecords, 1, &walkResult);
        const uint32_t walkRegAfter = StubRegisteredCount();
        std::printf("KILLHOUSE_CLIPMAP walk code=%d assets=%u walked=%u clipmaps=%u mapents=%u failed=%u regSame=%u b1=%u b4=%u\n",
                    (int)walkCode, walkResult.assetCount, walkResult.walkedDeferredCount,
                    walkResult.walkedClipMapCount, walkResult.walkedMapEntsCount,
                    walkResult.failedOrdinal, walkRegBefore == walkRegAfter ? 1u : 0u,
                    walkResult.endCursor[1], walkResult.endCursor[4]);
        if (!Check(walkCode == RETAIL_WALK_OK && walkResult.code == RETAIL_WALK_OK,
                    "s3_walk_ok") ||
            !Check(walkResult.assetCount == 1 && walkResult.walkedDeferredCount == 1 &&
                        walkResult.walkedClipMapCount == 1 &&
                        walkResult.walkedMapEntsCount == 1 &&
                        walkResult.walkedClipMapBlock1Bytes == 0 &&
                        walkResult.failedOrdinal == UINT32_MAX && walkRegBefore == walkRegAfter,
                    "s3_walk_counts") ||
            !Check(walkRecords[0].type == ASSET_TYPE_CLIPMAP &&
                        walkRecords[0].header == 0xffffffffu &&
                        walkRecords[0].state == RETAIL_WALK_WALKED_DEFERRED &&
                        walkRecords[0].bodyBytes == 284 && walkRecords[0].nameBytes == 7 &&
                        walkRecords[0].nestedReferenceCount == 13 &&
                        walkRecords[0].nestedBodyBytes == 507,
                    "s3_walk_body") ||
            !Check(walkResult.endCursor[0] == 0 && walkResult.endCursor[4] == 528,
                    "s3_walk_cursors"))
            // Dense-base note: session block-4
            // coordinates start after the mirrored 8-byte directory entry
            // (base 8); the 16-byte list-root prefix is consumed by the FS
            // reader without mirroring. The live load below resolves both
            // absolute aliases (lbn list at 220, MapEnts name at 8) in
            // those same session coordinates, so walk and live agree on
            // positions as well as bytes -- any layout drift moves this
            // cursor and fails here first.
            return 1;
    }
    {
        const uint32_t regsBeforeClip = StubRegisteredCount();
        RetailWalkLoadZoneResult clipResult{};
        if (!LoadFixtureZone("zone/english/clipmap_live.ff", &clipResult, "s3_live_load") ||
            !Check(clipResult.assetCount == 1 && clipResult.registeredClipMapCount == 1 &&
                        clipResult.walkedOnlyCount == 0 &&
                        StubRegisteredCount() - regsBeforeClip == 2,
                    "s3_live_counts"))
            return 1;
        const XAssetHeader clipHeader = StubFindXAssetHeader(ASSET_TYPE_CLIPMAP, "cmlive");
        if (!Check(clipHeader.clipMap != nullptr && clipHeader.clipMap == &cm,
                    "s3_live_singleton"))
            return 1;
        // Structural identity the real collision consumers need:
        // SV_SetBrushModel reads cmodels bounds, SV_LinkEntity walks
        // planes/nodes/leafs, CM_PointContents follows leafBrushNodes.
        if (!Check(!std::strcmp(cm.name, "cmlive") && cm.planeCount == 1 && cm.planes &&
                        cm.numStaticModels == 1 && cm.staticModelList &&
                        cm.staticModelList[0].xmodel == nullptr &&
                        cm.staticModelList[0].origin[0] == 100.0f,
                    "s3_live_collision_static") ||
            !Check(cm.numNodes == 1 && cm.nodes && cm.nodes[0].children[0] == -1 &&
                        cm.nodes[0].children[1] == -1 && cm.numLeafs == 1 && cm.leafs &&
                        cm.leafs[0].leafBrushNode == 0,
                    "s3_live_collision_graph") ||
            !Check(cm.numSubModels == 1 && cm.cmodels && cm.cmodels[0].maxs[0] == 16.0f &&
                        cm.numBrushes == 1 && cm.brushes && cm.brushes[0].numsides == 1 &&
                        cm.brushes[0].sides && cm.brushes[0].sides[0].materialNum == 3 &&
                        cm.box_brush && cm.box_brush->sides == nullptr,
                    "s3_live_collision_brush") ||
            !Check(cm.numClusters == 1 && cm.clusterBytes == 1 && cm.visibility &&
                        cm.visibility[0] == 0xAB && cm.checksum == 0x12345678u,
                    "s3_live_collision_vis"))
            return 1;
        const XAssetHeader mapentsHeader = StubFindXAssetHeader(ASSET_TYPE_MAP_ENTS, "cmlive");
        if (!Check(mapentsHeader.mapEnts != nullptr && cm.mapEnts == mapentsHeader.mapEnts &&
                        mapentsHeader.mapEnts->numEntityChars == 4 &&
                        !std::strcmp(mapentsHeader.mapEnts->entityString, "ent"),
                    "s3_live_mapents"))
            return 1;
        std::printf("KILLHOUSE_CLIPMAP registry cm=cm planes=1 nodes=1 leafs=1 brushes=1 cmodels=1\n");
        if (StubRegisteredCount() > registeredHigh)
            registeredHigh = StubRegisteredCount();
        // Rollback: the box-alias dangle variant walks clean first (dual
        // agreement, so the failure below is semantic, not structural),
        // then must fail after the nested MapEnts registered, sweep it,
        // and leave cm bit-identical (canary).
        RetailWalkDirectoryRecord dangleWalkRecords[1]{};
        RetailWalkDirectoryResult dangleWalkResult{};
        const RetailWalkDirectoryResultCode dangleWalkCode = RetailWalkFastfileDirectory(
            "zone/english/clipmap_dangle_live.ff", dangleWalkRecords, 1, &dangleWalkResult);
        if (!Check(dangleWalkCode == RETAIL_WALK_OK && dangleWalkResult.code == RETAIL_WALK_OK &&
                        dangleWalkResult.assetCount == 1 &&
                        dangleWalkResult.walkedDeferredCount == 1 &&
                        dangleWalkResult.failedOrdinal == UINT32_MAX,
                    "s3_dangle_walk"))
            return 1;
        uint8_t clipCanary[sizeof(cm)];
        std::memcpy(clipCanary, &cm, sizeof(cm));
        const uint32_t regsBeforeDangle = StubRegisteredCount();
        RetailWalkLoadZoneResult dangleResult{};
        const RetailWalkLoadZoneResultCode dangleCode =
            RetailWalkLoadZoneAssets("zone/english/clipmap_dangle_live.ff", &dangleResult);
        // The dangle MapEnts registers under the same (type, name) as the
        // still-live happy load, so the stub chains it as an override;
        // the abort must sweep exactly that override and restore the
        // happy canonical (same pointer), not drop the entry.
        bool ok = dangleCode != RETAIL_WALK_LOAD_OK &&
                  dangleResult.failedOrdinal == 0 &&
                  dangleResult.failedType == ASSET_TYPE_CLIPMAP &&
                  std::memcmp(clipCanary, &cm, sizeof(cm)) == 0 &&
                  StubRegisteredCount() == regsBeforeDangle &&
                  StubFindXAssetHeader(ASSET_TYPE_MAP_ENTS, "cmlive").mapEnts ==
                      mapentsHeader.mapEnts &&
                  StubRetiredPtrCount() == 0;
        std::printf("KILLHOUSE_CLIPMAP rollback fail=1 canary=%u swept=%u retired=%u\n",
                    std::memcmp(clipCanary, &cm, sizeof(cm)) == 0 ? 1u : 0u,
                    StubRegisteredCount() == regsBeforeDangle ? 1u : 0u,
                    StubRetiredPtrCount());
        if (!Check(ok, "s3_rollback_clean"))
            return 1;
        // Unload: end the live zone, clear cm (its arrays point into
        // freed zone memory), and prove nothing resolves anymore.
        ok = UnloadFixtureZone(clipResult);
        cm = clipMap_t{};
        ok = ok && StubFindXAssetHeader(ASSET_TYPE_CLIPMAP, "cmlive").data == nullptr &&
             StubFindXAssetHeader(ASSET_TYPE_MAP_ENTS, "cmlive").data == nullptr &&
             StubRetiredPtrCount() == 0;
        static const uint8_t zeroClip[sizeof(cm)]{};
        ok = ok && std::memcmp(&cm, zeroClip, sizeof(cm)) == 0;
        std::printf("KILLHOUSE_CLIPMAP unload dead=1 cleared=1 retired=%u\n",
                    StubRetiredPtrCount());
        if (!Check(ok, "s3_unload_clean"))
            return 1;
        std::puts("PASS:KILLHOUSE_CLIPMAP_REGISTRY cm=cm planes=1 nodes=1 leafs=1 brushes=1 cmodels=1 defaults=0 rollback=clean unload=clean");
    }
    {
        // Negatives: truncated body, brush/plane count mismatch,
        // unresolvable alias, and node-child escape. Each must fail loudly,
        // never substitute a default.
        RetailWalkLoadZoneResult truncResult{};
        const RetailWalkLoadZoneResultCode truncCode =
            RetailWalkLoadZoneAssets("zone/english/clipmap_trunc_live.ff", &truncResult);
        RetailWalkLoadZoneResult countResult{};
        const RetailWalkLoadZoneResultCode countCode =
            RetailWalkLoadZoneAssets("zone/english/clipmap_count_live.ff", &countResult);
        RetailWalkLoadZoneResult aliasResult{};
        const RetailWalkLoadZoneResultCode aliasCode =
            RetailWalkLoadZoneAssets("zone/english/clipmap_alias_live.ff", &aliasResult);
        RetailWalkLoadZoneResult boundsResult{};
        const RetailWalkLoadZoneResultCode boundsCode =
            RetailWalkLoadZoneAssets("zone/english/clipmap_bounds_live.ff", &boundsResult);
        const bool ok = truncCode != RETAIL_WALK_LOAD_OK &&
                        countCode != RETAIL_WALK_LOAD_OK &&
                        countResult.failedOrdinal == 0 &&
                        countResult.failedType == ASSET_TYPE_CLIPMAP &&
                        aliasCode != RETAIL_WALK_LOAD_OK &&
                        aliasResult.failedOrdinal == 0 &&
                        aliasResult.failedType == ASSET_TYPE_CLIPMAP &&
                        boundsCode != RETAIL_WALK_LOAD_OK &&
                        boundsResult.failedOrdinal == 0 &&
                        boundsResult.failedType == ASSET_TYPE_CLIPMAP &&
                        StubRetiredPtrCount() == 0;
        std::printf("KILLHOUSE_CLIPMAP negative trunc=%d count=%d alias=%d bounds=%d retired=%u\n",
                    truncCode != RETAIL_WALK_LOAD_OK ? 1 : 0,
                    countCode != RETAIL_WALK_LOAD_OK ? 1 : 0,
                    aliasCode != RETAIL_WALK_LOAD_OK ? 1 : 0,
                    boundsCode != RETAIL_WALK_LOAD_OK ? 1 : 0, StubRetiredPtrCount());
        if (!Check(ok, "s3_negative"))
            return 1;
    }
    {
        // XModel insert-slot bookkeeping (the production CM_LinkWorld
        // null-XModel crash): an insert-form XModel static model reserves a
        // block-4 DB_InsertPointer slot; a sibling static model aliasing
        // that reserved slot must bind the same widened/registered XModel.
        // Pre-fix the alias read a pristine zero slot and bound null.
        RetailWalkDirectoryRecord insertRecords[1]{};
        RetailWalkDirectoryResult insertWalk{};
        const RetailWalkDirectoryResultCode insertWalkCode = RetailWalkFastfileDirectory(
            "zone/english/clipmap_insert_live.ff", insertRecords, 1, &insertWalk);
        std::printf("B4_INSERT_WALK code=%d assets=%u walked=%u clipmaps=%u failed=%u b0=%u b4=%u\n",
                    (int)insertWalkCode, insertWalk.assetCount, insertWalk.walkedDeferredCount,
                    insertWalk.walkedClipMapCount, insertWalk.failedOrdinal,
                    insertWalk.endCursor[0], insertWalk.endCursor[4]);
        if (!Check(insertWalkCode == RETAIL_WALK_OK && insertWalk.code == RETAIL_WALK_OK &&
                       insertWalk.assetCount == 1 && insertWalk.walkedClipMapCount == 1 &&
                       insertWalk.failedOrdinal == UINT32_MAX,
                   "b4_insert_walk"))
            return 1;
        RetailWalkLoadZoneResult insertResult{};
        const RetailWalkLoadZoneResultCode insertCode =
            RetailWalkLoadZoneAssets("zone/english/clipmap_insert_live.ff", &insertResult);
        if (!Check(insertCode == RETAIL_WALK_LOAD_OK &&
                       insertResult.registeredClipMapCount == 1,
                   "b4_insert_load"))
            return 1;
        XModel *insertModel =
            StubFindXAssetHeader(ASSET_TYPE_XMODEL, "xmodel/insert").model;
        const bool insertBound =
            cm.numStaticModels == 2 && cm.staticModelList &&
            cm.staticModelList[0].xmodel != nullptr &&
            cm.staticModelList[1].xmodel == cm.staticModelList[0].xmodel &&
            cm.staticModelList[0].xmodel == insertModel;
        std::printf("B4_INSERT_SLOT models=%d bound=%d registered=%d\n",
                    (int)cm.numStaticModels, insertBound ? 1 : 0, insertModel ? 1 : 0);
        if (!Check(insertBound, "b4_insert_bound"))
            return 1;
        const bool insertUnloaded =
            UnloadFixtureZone(insertResult) &&
            !StubFindXAssetHeader(ASSET_TYPE_XMODEL, "xmodel/insert").data &&
            StubRetiredPtrCount() == 0;
        cm = clipMap_t{};
        if (!Check(insertUnloaded, "b4_insert_unload"))
            return 1;
        std::puts("PASS:B4_XMODEL_INSERT_SLOT models=2 insert=1 alias=bound registered=1 unload=clean");
    }
    {
        // Nested PhysPreset (the port's Load_PhysPresetPtr): the insert
        // XModel's slot@212 is a -2 PhysPreset form (body + name follow),
        // and a DynEntityDef physPreset slot aliases the reserved block-4
        // pointer slot. Both must bind the same registered PhysPreset; a
        // consume-and-null decoder leaves both null and counts defpreset=1.
        RetailWalkDirectoryRecord dpWalkRecords[1]{};
        RetailWalkDirectoryResult dpWalk{};
        const RetailWalkDirectoryResultCode dpWalkCode = RetailWalkFastfileDirectory(
            "zone/english/clipmap_dynpreset_live.ff", dpWalkRecords, 1, &dpWalk);
        std::printf("D19_DYNPRESET_WALK code=%d assets=%u walked=%u clipmaps=%u failed=%u b0=%u b4=%u\n",
                    (int)dpWalkCode, dpWalk.assetCount, dpWalk.walkedDeferredCount,
                    dpWalk.walkedClipMapCount, dpWalk.failedOrdinal,
                    dpWalk.endCursor[0], dpWalk.endCursor[4]);
        if (!Check(dpWalkCode == RETAIL_WALK_OK && dpWalk.code == RETAIL_WALK_OK &&
                       dpWalk.assetCount == 1 && dpWalk.walkedClipMapCount == 1 &&
                       dpWalk.failedOrdinal == UINT32_MAX,
                   "d19_dynpreset_walk"))
            return 1;
        RetailWalkLoadZoneResult dpResult{};
        const RetailWalkLoadZoneResultCode dpCode =
            RetailWalkLoadZoneAssets("zone/english/clipmap_dynpreset_live.ff", &dpResult);
        if (!Check(dpCode == RETAIL_WALK_LOAD_OK && dpResult.registeredClipMapCount == 1,
                   "d19_dynpreset_load"))
            return 1;
        XModel *dpModel = StubFindXAssetHeader(ASSET_TYPE_XMODEL, "xmodel/insert").model;
        PhysPreset *dpPreset =
            StubFindXAssetHeader(ASSET_TYPE_PHYSPRESET, "physpreset/insert").physPreset;
        const bool dpBound =
            cm.numStaticModels == 2 && cm.staticModelList && dpModel && dpPreset &&
            cm.staticModelList[0].xmodel == dpModel &&
            cm.staticModelList[1].xmodel == dpModel &&
            dpModel->physPreset == dpPreset &&
            cm.dynEntCount[0] == 1 && cm.dynEntDefList[0] &&
            cm.dynEntDefList[0][0].physPreset == dpPreset;
        std::printf("D19_DYNPRESET_BOUND model=%d preset=%d xmodel_field=1 dyn_field=%d\n",
                    dpModel ? 1 : 0, dpPreset ? 1 : 0,
                    (cm.dynEntDefList[0] &&
                     cm.dynEntDefList[0][0].physPreset == dpPreset)
                        ? 1
                        : 0);
        if (!Check(dpBound, "d19_dynpreset_bound"))
            return 1;
        const bool dpUnloaded =
            UnloadFixtureZone(dpResult) &&
            !StubFindXAssetHeader(ASSET_TYPE_PHYSPRESET, "physpreset/insert").data &&
            !StubFindXAssetHeader(ASSET_TYPE_XMODEL, "xmodel/insert").data &&
            StubRetiredPtrCount() == 0;
        cm = clipMap_t{};
        if (!Check(dpUnloaded, "d19_dynpreset_unload"))
            return 1;
        std::puts("PASS:D19_DYN_PHYSPRESET_RESTORED xmodel=1 dyn=1 shared=1 deferred=0 unload=clean");
    }

    // Native GfxWorld registry: walk-first dual
    // agreement over gfxworld_live.ff (the walk-only reader must consume
    // the same stream the live driver widens), then the live load
    // widening the registry-facing shape into a transaction committed
    // atomically into s_world. Rollback runs the dangling variant (the
    // singleton stays bit-identical, partial registrations swept); unload
    // ends the zone and clears the singleton. Fixture-only, like the graphics-loader fixture.
    uint32_t m7Surfaces = 0, m7Vertices = 0, m7Indices = 0, m7Cells = 0, m7Deps = 0;
    {
        RetailWalkDirectoryRecord walkRecords[1]{};
        RetailWalkDirectoryResult walkResult{};
        const uint32_t walkRegBefore = StubRegisteredCount();
        const RetailWalkDirectoryResultCode walkCode = RetailWalkFastfileDirectory(
            "zone/english/gfxworld_live.ff", walkRecords, 1, &walkResult);
        const uint32_t walkRegAfter = StubRegisteredCount();
        std::printf("KILLHOUSE_WORLD walk code=%d assets=%u walked=%u materials=%u lightdefs=%u images=%u gfxworlds=%u failed=%u regSame=%u b1=%u b4=%u\n",
                    (int)walkCode, walkResult.assetCount, walkResult.walkedDeferredCount,
                    walkResult.walkedMaterialCount, walkResult.walkedLightDefCount,
                    walkResult.walkedImageCount, walkResult.walkedGfxWorldCount,
                    walkResult.failedOrdinal, walkRegBefore == walkRegAfter ? 1u : 0u,
                    walkResult.endCursor[1], walkResult.endCursor[4]);
        if (!Check(walkCode == RETAIL_WALK_OK && walkResult.code == RETAIL_WALK_OK,
                    "m7_walk_ok") ||
            // Nested lightdefs never bump the walk counter (only top-level
            // bodies count, matching the killhouse OAT reconciliation where
            // walked == direct == 1): the sun's nested lightdef walks its
            // bytes silently here and registers for real in the live load
            // below. Nested materials/images do count (top-level and nested
            // together, per the accounting contract).
            !Check(walkResult.assetCount == 1 && walkResult.walkedDeferredCount == 1 &&
                        walkResult.walkedMaterialCount == 2 &&
                        walkResult.walkedLightDefCount == 0 &&
                        walkResult.walkedImageCount == 1 && walkResult.walkedGfxWorldCount == 1 &&
                        walkResult.failedOrdinal == UINT32_MAX &&
                        walkRegBefore == walkRegAfter,
                    "m7_walk_counts"))
            return 1;
    }
    {
        // The live loop counts only top-level dispatches, so the world's
        // nested registrations are counted as a stub-table delta: sprite,
        // dpvsmat, sun lightdef, outdoor image, and the world itself (the
        // flare alias registers nothing). Resolution itself is proven by
        // the identity asserts below, not by counting.
        const uint32_t regsBeforeWorld = StubRegisteredCount();
        RetailWalkLoadZoneResult worldResult{};
        if (!LoadFixtureZone("zone/english/gfxworld_live.ff", &worldResult, "m7_world_load") ||
            !Check(worldResult.assetCount == 1 && worldResult.registeredGfxWorldCount == 1 &&
                        worldResult.registeredMaterialCount == 0 &&
                        worldResult.registeredLightDefCount == 0 &&
                        worldResult.registeredImageCount == 0 &&
                        worldResult.walkedOnlyCount == 0 &&
                        StubRegisteredCount() - regsBeforeWorld == 5,
                    "m7_world_counts"))
            return 1;
        const XAssetHeader worldHeader = StubFindXAssetHeader(ASSET_TYPE_GFXWORLD, "gfxlive");
        if (!Check(worldHeader.gfxWorld != nullptr && worldHeader.gfxWorld == &s_world,
                    "m7_world_singleton"))
            return 1;
        const GfxWorld *world = worldHeader.gfxWorld;
        if (!Check(!std::strcmp(world->name, "gfxlive") && !std::strcmp(world->baseName, "base") &&
                        world->surfaceCount == 1 && world->vertexCount == 2 &&
                        world->indexCount == 3 &&
                        // 16 * ((cellCount + 127) >> 7) with cellCount == 1.
                        world->cellBitsCount == 16 &&
                        world->lightmapCount == 1 && world->reflectionProbeCount == 1 &&
                        world->modelCount == 1 && world->materialMemoryCount == 1 &&
                        world->mins[0] == -1024.0f && world->mins[1] == -512.0f &&
                        world->mins[2] == -128.0f && world->maxs[0] == 2048.0f &&
                        world->maxs[1] == 1024.0f && world->maxs[2] == 512.0f &&
                        world->checksum == 0x4b484d31u,
                    "m7_world_scalars") ||
            !Check(world->indices != nullptr && world->indices[0] == 0 &&
                        world->indices[1] == 1 && world->indices[2] == 2 &&
                        world->vd.vertices != nullptr && world->vld.data != nullptr &&
                        world->skyStartSurfs != nullptr,
                    "m7_world_spans"))
            return 1;
        const XAssetHeader spriteHeader =
            StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "sprite");
        const XAssetHeader dpvsHeader =
            StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "dpvsmat");
        const XAssetHeader sunLdHeader =
            StubFindXAssetHeader(ASSET_TYPE_LIGHT_DEF, "ldsun");
        // The sky image declared through the -2 insert form after the
        // discard-decoded world spans, and the outdoor slot's alias onto
        // that insert slot's canonical offset, must both bind the one
        // registered image -- no translation, no pool fallback.
        const XAssetHeader skyHeader = StubFindXAssetHeader(ASSET_TYPE_IMAGE, "img/sky");
        if (!Check(spriteHeader.material != nullptr && dpvsHeader.material != nullptr &&
                         sunLdHeader.lightDef != nullptr && skyHeader.image != nullptr,
                    "m7_world_deps_found") ||
            !Check(world->sun.spriteMaterial == spriteHeader.material &&
                        world->sun.flareMaterial == spriteHeader.material &&
                        world->sun.hasValidData,
                    "m7_world_sunflare") ||
            !Check(world->sunLight != nullptr && world->sunLight->def == sunLdHeader.lightDef &&
                        world->outdoorImage == skyHeader.image,
                    "m7_world_sun_outdoor") ||
            !Check(world->skyImage == skyHeader.image,
                    "m7_world_sky_insert_alias") ||
            !Check(world->outdoorLookupMatrix[0][0] == 1.0f / 3072.0f &&
                        world->outdoorLookupMatrix[1][1] == 1.0f / 1536.0f &&
                        world->outdoorLookupMatrix[2][2] == 1.0f / 640.0f &&
                        world->outdoorLookupMatrix[3][0] == 1.0f / 3 &&
                        world->outdoorLookupMatrix[3][1] == 1.0f / 3 &&
                        world->outdoorLookupMatrix[3][2] == 0.2f &&
                        world->outdoorLookupMatrix[3][3] == 1.0f &&
                        world->outdoorLookupMatrix[0][1] == 0.0f,
                    "m7_world_outdoor_lookup_matrix") ||
            !Check(world->dpvs.surfaces != nullptr &&
                        world->dpvs.surfaces[0].material == dpvsHeader.material &&
                        world->dpvs.staticSurfaceCount == 1 &&
                        world->lightmaps != nullptr &&
                        world->lightmaps[0].primary == nullptr &&
                        world->lightmaps[0].secondary == nullptr,
                    "m7_world_surfaces"))
            return 1;
        m7Surfaces = 1;
        m7Vertices = 2;
        m7Indices = 3;
        m7Cells = 1;
        m7Deps = 5;
        std::printf("KILLHOUSE_WORLD registry world=s_world surfaces=1 vertices=2 indices=3 cells=1 dependencies=5\n");
        if (StubRegisteredCount() > registeredHigh)
            registeredHigh = StubRegisteredCount();
        // Rollback: the dangling variant walks clean first (dual
        // agreement, so the failure below is semantic, not structural),
        // then must fail, sweep its partial sprite registration, and leave
        // s_world bit-identical (canary).
        RetailWalkDirectoryRecord dangleWalkRecords[1]{};
        RetailWalkDirectoryResult dangleWalkResult{};
        const RetailWalkDirectoryResultCode dangleWalkCode = RetailWalkFastfileDirectory(
            "zone/english/gfxworld_dangle_live.ff", dangleWalkRecords, 1, &dangleWalkResult);
        std::printf("KILLHOUSE_WORLD dangle_walk code=%d assets=%u walked=%u materials=%u failed=%u\n",
                    (int)dangleWalkCode, dangleWalkResult.assetCount,
                    dangleWalkResult.walkedDeferredCount, dangleWalkResult.walkedMaterialCount,
                    dangleWalkResult.failedOrdinal);
        if (!Check(dangleWalkCode == RETAIL_WALK_OK && dangleWalkResult.code == RETAIL_WALK_OK &&
                        dangleWalkResult.assetCount == 1 &&
                        dangleWalkResult.walkedDeferredCount == 1 &&
                        dangleWalkResult.walkedMaterialCount == 1 &&
                        dangleWalkResult.failedOrdinal == UINT32_MAX,
                    "m7_dangle_walk"))
            return 1;
        uint8_t worldCanary[sizeof(s_world)];
        std::memcpy(worldCanary, &s_world, sizeof(s_world));
        const uint32_t regsBeforeDangle = StubRegisteredCount();
        RetailWalkLoadZoneResult dangleResult{};
        const RetailWalkLoadZoneResultCode dangleCode =
            RetailWalkLoadZoneAssets("zone/english/gfxworld_dangle_live.ff", &dangleResult);
        bool ok = dangleCode != RETAIL_WALK_LOAD_OK &&
                  dangleResult.failedOrdinal == 0 &&
                  dangleResult.failedType == ASSET_TYPE_GFXWORLD &&
                  std::memcmp(worldCanary, &s_world, sizeof(s_world)) == 0 &&
                  StubRegisteredCount() == regsBeforeDangle && StubRetiredPtrCount() == 0 &&
                  StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "sprited").data == nullptr;
        std::printf("KILLHOUSE_WORLD rollback fail=1 canary=%u swept=%u retired=%u\n",
                    std::memcmp(worldCanary, &s_world, sizeof(s_world)) == 0 ? 1u : 0u,
                    StubRegisteredCount() == regsBeforeDangle ? 1u : 0u,
                    StubRetiredPtrCount());
        if (!Check(ok, "m7_rollback_clean"))
            return 1;
        // Unload: end the world zone, clear the singleton (its arrays point
        // into freed zone memory), and prove nothing resolves anymore.
        ok = UnloadFixtureZone(worldResult);
        std::memset(&s_world, 0, sizeof(s_world));
        ok = ok && StubFindXAssetHeader(ASSET_TYPE_GFXWORLD, "gfxlive").data == nullptr &&
             StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "sprite").data == nullptr &&
             StubRetiredPtrCount() == 0;
        static const uint8_t zeroWorld[sizeof(s_world)]{};
        ok = ok && std::memcmp(&s_world, zeroWorld, sizeof(s_world)) == 0;
        std::printf("KILLHOUSE_WORLD unload dead=1 cleared=1 retired=%u\n", StubRetiredPtrCount());
        if (!Check(ok, "m7_unload_clean"))
            return 1;
        std::printf("KILLHOUSE_WORLD summary surfaces=%u vertices=%u indices=%u cells=%u dependencies=%u defaults=0 rollback=clean unload=clean\n",
                    m7Surfaces, m7Vertices, m7Indices, m7Cells, m7Deps);
    }

    // consumer-visible unresolved nested fields fail the world load;
    // the sunflare's never-selected slots load with the flare disabled.
    // Four fixtures share gfxworld_live.ff's layout with one slot swapped
    // for the linker's dead-inline form.
    {
        struct D35Case
        {
            const char *path;
            bool expectLoad;
            const char *what;
        };
        static const D35Case cases[] = {
            {"zone/english/gfxworld_d35_control_live.ff", true, "d35_control"},
            {"zone/english/gfxworld_d35_deadflare_live.ff", true, "d35_deadflare"},
            {"zone/english/gfxworld_d35_deadoutdoor_live.ff", false, "d35_deadoutdoor"},
            {"zone/english/gfxworld_d35_deadsurfmat_live.ff", false, "d35_deadsurfmat"},
        };
        uint32_t loaded = 0;
        uint32_t rejected = 0;
        for (const D35Case &c : cases)
        {
            const uint32_t regsBefore = StubRegisteredCount();
            RetailWalkLoadZoneResult result{};
            const RetailWalkLoadZoneResultCode code = RetailWalkLoadZoneAssets(c.path, &result);
            const bool didLoad = code == RETAIL_WALK_LOAD_OK;
            std::printf("KILLHOUSE_D35 fixture=%s load=%d expected=%d failedType=%d sprite=%d flare=%d valid=%d outdoor=%d\n",
                        c.what, didLoad ? 1 : 0, c.expectLoad ? 1 : 0, (int)result.failedType,
                        s_world.sun.spriteMaterial ? 1 : 0, s_world.sun.flareMaterial ? 1 : 0,
                        s_world.sun.hasValidData ? 1 : 0, s_world.outdoorImage ? 1 : 0);
            if (!Check(didLoad == c.expectLoad, c.what))
                return 1;
            if (didLoad)
            {
                const bool flareDead = std::strcmp(c.what, "d35_deadflare") == 0;
                const bool flareOk = flareDead
                    ? (!s_world.sun.hasValidData && !s_world.sun.spriteMaterial &&
                       !s_world.sun.flareMaterial)
                    : (s_world.sun.hasValidData && s_world.sun.spriteMaterial &&
                       s_world.sun.flareMaterial);
                if (!Check(flareOk && s_world.outdoorImage != nullptr &&
                               s_world.dpvs.surfaces && s_world.dpvs.surfaces[0].material,
                           flareDead ? "d35_deadflare_disabled" : "d35_control_valid"))
                    return 1;
                if (!Check(DB_RetailZoneEnd(result.zoneIndex), "d35_unload"))
                    return 1;
                std::memset(&s_world, 0, sizeof(s_world));
                ++loaded;
            }
            else
            {
                // Rejected loads sweep their partial registrations.
                if (!Check(result.failedType == ASSET_TYPE_GFXWORLD &&
                               StubRegisteredCount() == regsBefore,
                           "d35_reject_swept"))
                    return 1;
                std::memset(&s_world, 0, sizeof(s_world));
                ++rejected;
            }
        }
        std::printf("PASS:D35_DECODE_STRICTNESS_FIXTURES loaded=%u rejected=%u flare_disabled=1\n",
                    loaded, rejected);
    }

    // Killhouse half: reopen the real map zone and reach its asset
    // directory without walking bodies. Block 1 is proven as runtime
    // expansion at retail scale, blocks 2/3 as zero-delayed, and the session
    // rolls back clean. The caller links zone/english/killhouse.ff into the
    // fixture root (argv[2] present); without it this half SKIPs.
    if (argc < 3)
    {
        std::puts("SKIP:KILLHOUSE_BLOCK_SEMANTICS needs staged retail killhouse.ff");
        return 0;
    }
    {
        constexpr uint32_t kKillhouseAssets = 1684;
        constexpr uint32_t kKillhouseBlock1 = 522928;
        RetailWalkDirectoryRecord *records =
            new (std::nothrow) RetailWalkDirectoryRecord[kKillhouseAssets];
        RetailZoneLoadSession session{};
        FsRetailFastfileReader *reader = nullptr;
        FsRetailFastfileAsset *assets = nullptr;
        RetailWalkDirectoryResult openResult{};
        if (!Check(records != nullptr, "m2_killhouse_records") ||
            !Check(RetailWalkOpenDirectory("zone/english/killhouse.ff", &session, &reader,
                                           &assets, records, kKillhouseAssets,
                                           &openResult) == RETAIL_WALK_OK &&
                       openResult.code == RETAIL_WALK_OK,
                   "m2_killhouse_open"))
        {
            delete[] records;
            return 1;
        }
        const uint32_t cursor0 = session.wire.cursor[0];
        const uint32_t cursor4 = session.wire.cursor[4];
        bool ok = openResult.assetCount == kKillhouseAssets &&
                  openResult.recordedCount == kKillhouseAssets &&
                  session.zoneMemory->blocks[1].size == kKillhouseBlock1 &&
                  session.zoneMemory->blocks[2].size == 0 &&
                  session.zoneMemory->blocks[3].size == 0 && session.wire.cursor[1] == 0 &&
                  RetailZoneLoadSessionExpandRuntime(&session, 1, kKillhouseBlock1, 16) &&
                  session.wire.cursor[1] == kKillhouseBlock1;
        for (uint32_t i = 0; ok && i < kKillhouseBlock1; ++i)
            ok = session.zoneMemory->blocks[1].data[i] == 0;
        ok = ok && session.wire.cursor[0] == cursor0 && session.wire.cursor[4] == cursor4;
        const uint32_t zoneIndex = session.zoneIndex;
        ok = ok && RetailZoneLoadSessionAbort(&session) && !StubZoneIsLive(zoneIndex);
        delete[] records;
        delete[] assets;
        if (reader)
            FS_CloseRetailFastfile(reader);
        if (!Check(ok, "m2_killhouse_semantics"))
            return 1;
    }
    std::puts("PASS:KILLHOUSE_BLOCK_SEMANTICS block1=522928 block2=0 block3=0 "
              "runtime_alloc=exact delayed=exact rollback=clean");

    // Killhouse map-zone accounting (walk-only): replay every B-family
    // and world walker in exact directory order over the real map zone and
    // report the totals. Registers nothing: the stub registry
    // count before and after must match.
    {
        constexpr uint32_t kKillhouseAssets = 1684;
        RetailWalkDirectoryRecord *records =
            new (std::nothrow) RetailWalkDirectoryRecord[kKillhouseAssets];
        RetailWalkDirectoryResult walkResult{};
        const uint32_t regBefore = StubRegisteredCount();
        const RetailWalkDirectoryResultCode walkCode =
            RetailWalkFastfileDirectory("zone/english/killhouse.ff", records,
                                        kKillhouseAssets, &walkResult);
        const uint32_t regAfter = StubRegisteredCount();
        std::printf("KILLHOUSE_ZONE_WALK code=%d/%d assets=%u recorded=%u deferred=%u noStream=%u walkedDeferred=%u failedOrdinal=%u failedType=%u failedNestedKind=%u failedNestedRef=0x%08x registeredBefore=%u registeredAfter=%u\n",
                    (int)walkCode, (int)walkResult.code, walkResult.assetCount,
                    walkResult.recordedCount, walkResult.deferredCount,
                    walkResult.noStreamCount, walkResult.walkedDeferredCount,
                    walkResult.failedOrdinal,
                    walkResult.failedOrdinal < kKillhouseAssets ?
                        records[walkResult.failedOrdinal].type : 99u,
                    walkResult.failedNestedKind, walkResult.failedNestedRef,
                    regBefore, regAfter);
        std::printf("KILLHOUSE_ZONE_WALK family techsets=%u passes=%u materials=%u images=%u imagePayload=%u sounds=%u loadedSounds=%u loadedData=%u fx=%u weapons=%u localizes=%u rawfiles=%u stringtables=%u xanims=%u xmodels=%u xsurfaces=%u fonts=%u physpresets=%u sndcurves=%u lightdefs=%u menus=%u menulists=%u items=%u mapents=%u comworlds=%u gameworlds=%u gameworldb1=%u clipmaps=%u clipmapb1=%u gfxworlds=%u gfxworldb1=%u\n",
                    walkResult.walkedTechniqueCount, walkResult.walkedPassCount,
                    walkResult.walkedMaterialCount, walkResult.walkedImageCount,
                    walkResult.walkedImagePayloadBytes, walkResult.walkedSoundCount,
                    walkResult.walkedLoadedSoundCount, walkResult.walkedLoadedSoundDataBytes,
                    walkResult.walkedFxCount, walkResult.walkedWeaponCount,
                    walkResult.walkedLocalizeCount, walkResult.walkedRawFileCount,
                    walkResult.walkedStringTableCount, walkResult.walkedXAnimCount,
                    walkResult.walkedXModelCount, walkResult.walkedXModelSurfaceCount,
                    walkResult.walkedFontCount, walkResult.walkedPhysPresetCount,
                    walkResult.walkedSndCurveCount, walkResult.walkedLightDefCount,
                    walkResult.walkedMenuCount, walkResult.walkedMenuListCount,
                    walkResult.walkedItemDefCount, walkResult.walkedMapEntsCount,
                    walkResult.walkedComWorldCount, walkResult.walkedGameWorldSpCount,
                    walkResult.walkedGameWorldSpBlock1Bytes, walkResult.walkedClipMapCount,
                    walkResult.walkedClipMapBlock1Bytes, walkResult.walkedGfxWorldCount,
                    walkResult.walkedGfxWorldBlock1Bytes);
        std::printf("KILLHOUSE_ZONE_WALK closure worldMats=%u worldImages=%u worldTechSets=%u worldLightDefs=%u worldXModels=%u worldPasses=%u cells=%u portals=%u surfaces=%u\n",
                    walkResult.walkedGfxWorldMaterials, walkResult.walkedGfxWorldImages,
                    walkResult.walkedGfxWorldTechSets, walkResult.walkedGfxWorldLightDefs,
                    walkResult.walkedGfxWorldXModels, walkResult.walkedGfxWorldPasses,
                    walkResult.walkedGfxCellCount, walkResult.walkedGfxPortalCount,
                    walkResult.walkedGfxSurfaceCount);
        std::printf("KILLHOUSE_ZONE_WALK cursors b0=%u b1=%u b2=%u b3=%u b4=%u b5=%u b6=%u b7=%u b8=%u\n",
                    walkResult.endCursor[0], walkResult.endCursor[1],
                    walkResult.endCursor[2], walkResult.endCursor[3],
                    walkResult.endCursor[4], walkResult.endCursor[5],
                    walkResult.endCursor[6], walkResult.endCursor[7],
                    walkResult.endCursor[8]);
        delete[] records;
        if (!Check(walkCode == RETAIL_WALK_OK && walkResult.code == RETAIL_WALK_OK,
                   "m3e_killhouse_walk") ||
            !Check(walkResult.assetCount == kKillhouseAssets &&
                        walkResult.recordedCount == kKillhouseAssets &&
                        walkResult.walkedDeferredCount == kKillhouseAssets &&
                        walkResult.noStreamCount == 0 &&
                        walkResult.failedOrdinal == UINT32_MAX &&
                        walkResult.failedNestedKind == UINT32_MAX &&
                        regBefore == regAfter,
                    "m3e_killhouse_accounting"))
            return 1;
    }

    // Map-zone registry and lifetime: synthetic override/rollback
    // mechanics first (fixture-only, always runs), then the real SP
    // boot-zone sequence code_post_gfx -> ui -> common -> killhouse opened
    // in order with retained-zone, walk, and unload accounting. Only the
    // stub registry stands in for db_registry.cpp here (see the stubs
    // file); every session open/walk/abort call below is production code.
    // registeredHigh keeps flowing from the section above (same process,
    // same stub table); the water line reports the combined peak.
    uint32_t m5ReloadZone = 0;
    uint32_t m5OverrideDelta = 0;
    {
        // Override: load localize_override.ff (same entry name "key", new
        // value) through the real live loader.  This models the real
        // cross-zone override scenario (a later zone shadowing an earlier
        // zone's same-named asset, e.g. a mod zone over base): the FIRST
        // header must stay canonical (the real DB_LinkXAssetEntry chains a
        // same-name re-registration on nextOverride and keeps returning the
        // existing entry), the re-registration must be counted as an
        // override, and both zones must stay live (an override never tears
        // down either owner).  Unload promotion is proven at the end of
        // this section.  Deliberately NOT a reload of localize.ff itself:
        // identical-file reloads are now a PMem-saving no-op skip (see
        // DB_RetailZoneFindLive -- R_Init/spmap redundant reloads of
        // resident graphics zones), which is the correct production
        // behavior there but cannot prove override mechanics; the skip is
        // covered separately by switch_retail_zone_reload_test.cpp.
        const uint32_t overridesBefore = StubOverrideCount();
        RetailWalkLoadZoneResult relocalizeResult{};
        const RetailWalkLoadZoneResultCode relocalizeCode =
            RetailWalkLoadZoneAssets("zone/english/localize_override.ff", &relocalizeResult);
        if (!Check(relocalizeCode == RETAIL_WALK_LOAD_OK &&
                        relocalizeResult.code == RETAIL_WALK_LOAD_OK,
                    "m5_reload_load") ||
            !Check(!relocalizeResult.alreadyLoaded, "m5_reload_not_skipped") ||
            !Check(relocalizeResult.assetCount == 1 &&
                        relocalizeResult.registeredLocalizeCount == 1 &&
                        relocalizeResult.walkedOnlyCount == 0,
                    "m5_reload_counts") ||
            !Check(StubZoneIsLive(relocalizeResult.zoneIndex) &&
                        StubZoneIsLive(localizeResult.zoneIndex) &&
                        relocalizeResult.zoneIndex != localizeResult.zoneIndex,
                    "m5_reload_zones_live"))
            return 1;
        m5ReloadZone = relocalizeResult.zoneIndex;
        m5OverrideDelta = StubOverrideCount() - overridesBefore;
        std::printf("KILLHOUSE_LIFETIME reload idx=%u high=%u\n",
                    m5ReloadZone, StubZoneHighWater());
        const XAssetHeader reloadedKeyHeader =
            StubFindXAssetHeader(ASSET_TYPE_LOCALIZE_ENTRY, "key");
        if (!Check(StubOverrideCount() == overridesBefore + 1, "m5_reload_counted") ||
            !Check(reloadedKeyHeader.localize != nullptr &&
                        reloadedKeyHeader.localize == localizeHeader.localize,
                    "m5_reload_first_wins") ||
            !Check(!std::strcmp(reloadedKeyHeader.localize->name, "key") &&
                        !std::strcmp(reloadedKeyHeader.localize->value, "value"),
                    "m5_reload_value"))
            return 1;
        if (StubRegisteredCount() > registeredHigh)
            registeredHigh = StubRegisteredCount();
    }
    {
        // Rollback: open a real session, register a named asset in it
        // through the production session wrapper, then abort and prove the
        // zone is dead, the name no longer resolves, and no retired
        // pointer into the freed zone remains reachable.
        RetailZoneLoadSession session{};
        FsRetailFastfileReader *reader = nullptr;
        FsRetailFastfileAsset *assets = nullptr;
        RetailWalkDirectoryRecord records[1]{};
        RetailWalkDirectoryResult openResult{};
        if (!Check(RetailWalkOpenDirectory("zone/english/block1.ff", &session, &reader, &assets,
                                            records, 1, &openResult) == RETAIL_WALK_OK &&
                        openResult.code == RETAIL_WALK_OK && session.active &&
                        session.zoneIndex != 0,
                    "m5_rollback_open"))
            return 1;
        static LocalizeEntry rollbackEntry{"gone", "m5/rollback"};
        XAssetHeader rollbackHeader{};
        rollbackHeader.localize = &rollbackEntry;
        const XAssetHeader registered =
            RetailZoneLoadSessionRegister(&session, ASSET_TYPE_LOCALIZE_ENTRY, rollbackHeader);
        bool ok = registered.localize == &rollbackEntry &&
                  StubFindXAssetHeader(ASSET_TYPE_LOCALIZE_ENTRY, "m5/rollback").localize ==
                      &rollbackEntry;
        if (StubRegisteredCount() > registeredHigh)
            registeredHigh = StubRegisteredCount();
        const uint32_t zoneIndex = session.zoneIndex;
        ok = ok && RetailZoneLoadSessionAbort(&session) && !session.active &&
             !StubZoneIsLive(zoneIndex) &&
             StubFindXAssetHeader(ASSET_TYPE_LOCALIZE_ENTRY, "m5/rollback").data == nullptr &&
             StubRetiredPtrCount() == 0;
        delete[] assets;
        if (reader)
            FS_CloseRetailFastfile(reader);
        if (!Check(ok, "m5_rollback_swept"))
            return 1;
        std::printf("KILLHOUSE_LIFETIME synthetic overrides=%u rollback=clean high=%u\n",
                    m5OverrideDelta, StubZoneHighWater());
        if (StubRegisteredCount() > registeredHigh)
            registeredHigh = StubRegisteredCount();
    }
    {
        // retained-pointer negative control: the zero-violation fact
        // line at the end of this binary is only meaningful if the audit can
        // actually see a reader-owned name. Plant a LocalizeEntry whose name
        // points into the still-open reader's transient block -- exactly the
        // pre-fix RetailWalkLiveLoadLightDef alias shape -- register it
        // through the production session entrypoint, and require the audit
        // to flag it. Abort sweeps the planted registration while the reader
        // is still open, so nothing dangling survives this block; the
        // counter is then reset so the final line measures only real asset
        // registrations.
        RetailZoneLoadSession session{};
        FsRetailFastfileReader *reader = nullptr;
        FsRetailFastfileAsset *assets = nullptr;
        RetailWalkDirectoryRecord records[1]{};
        RetailWalkDirectoryResult openResult{};
        bool ok = RetailWalkOpenDirectory("zone/english/block1.ff", &session, &reader, &assets,
                                          records, 1, &openResult) == RETAIL_WALK_OK &&
                  openResult.code == RETAIL_WALK_OK && session.active &&
                  reader != nullptr;
        uint32_t blockIndex = 9;
        if (ok)
        {
            for (blockIndex = 0; blockIndex < 9; ++blockIndex)
            {
                if (FS_RetailFastfileBlockData(reader, blockIndex) &&
                    FS_RetailFastfileBlockSize(reader, blockIndex))
                    break;
            }
            ok = blockIndex < 9;
        }
        const uint32_t before = StubReaderOwnedNameCount();
        LocalizeEntry planted{};
        planted.value = "provenance";
        planted.name = ok ? reinterpret_cast<const char *>(
                                FS_RetailFastfileBlockData(reader, blockIndex))
                          : nullptr;
        if (ok)
            ok = RetailZoneLoadSessionRegister(&session, ASSET_TYPE_LOCALIZE_ENTRY,
                                               XAssetHeader(&planted))
                     .localize != nullptr;
        const uint32_t detected = StubReaderOwnedNameCount() - before;
        std::printf("KILLHOUSE_LIFETIME provenance_negative block=%u detected=%u\n",
                    blockIndex, detected);
        ok = ok && detected == 1;
        // pointer-provenance generalization negative control: plant an
        // arena-resident raw pointer into the still-open reader's transient
        // storage -- the retained-pointer shape as a plain pointer field, not a name -- and
        // require the generic arena scan to see it. This proves the sweep
        // catches any pointer field of any asset family, not just registration
        // names, before the session is aborted.
        uint32_t bodyDetected = 0;
        if (ok)
        {
            // One plant in the widened-record arena (where real zone bodies
            // live) and one in the host material pool the no-session
            // synthesize path uses, so both swept storage kinds are proven.
            void **plantedSlot = static_cast<void **>(
                RetailZoneLoadSessionAlloc(&session, sizeof(void *), alignof(void *)));
            void **plantedHeap = reinterpret_cast<void **>(Material_Alloc(sizeof(void *)));
            if (plantedSlot && plantedHeap)
            {
                const uint8_t *readerBlock =
                    const_cast<uint8_t *>(FS_RetailFastfileBlockData(reader, blockIndex));
                *plantedSlot = const_cast<uint8_t *>(readerBlock);
                *plantedHeap = const_cast<uint8_t *>(readerBlock);
                StubResetReaderOwnedBodyCount();
                StubScanLiveZonesForReaderTransient();
                bodyDetected = StubReaderOwnedBodyCount();
            }
        }
        std::printf("KILLHOUSE_LIFETIME provenance_body_negative block=%u detected=%u\n",
                    blockIndex, bodyDetected);
        ok = ok && bodyDetected >= 2;
        const char *plantedName = planted.name;
        const uint8_t *plantedBlock = reinterpret_cast<const uint8_t *>(plantedName);
        const uint32_t plantedBlockSize =
            ok && blockIndex < 9 ? FS_RetailFastfileBlockSize(reader, blockIndex) : 0;
        if (session.active)
            RetailZoneLoadSessionAbort(&session);
        delete[] assets;
        uint32_t poisonBlocksBefore = 0;
        uint64_t poisonBytesBefore = 0;
        uint32_t poisonFailuresBefore = 0;
        FS_RetailFastfilePoisonStats(&poisonBlocksBefore, &poisonBytesBefore,
                                     &poisonFailuresBefore);
        if (reader)
            FS_CloseRetailFastfile(reader);
        uint32_t poisonBlocksAfter = 0;
        uint64_t poisonBytesAfter = 0;
        uint32_t poisonFailuresAfter = 0;
        FS_RetailFastfilePoisonStats(&poisonBlocksAfter, &poisonBytesAfter,
                                     &poisonFailuresAfter);
        // sweep #3 done-when: the close poisons every non-empty
        // transient block (host counters verify 0xDD at start/middle/end),
        // and a deliberately reintroduced retained-reader pointer into that
        // storage reads recognizable 0xDD at its use point instead of a
        // delayed freed-heap stall. ASan catches the read itself; the
        // Probed bytes are unpoisoned so the content is what the check sees
        // (a verification channel in the proof link, not a production
        // suppression). ASan writes its quarantine links into the first
        // bytes of a freed allocation, so the observed byte is the block's
        // midpoint -- the same freed reader storage the retained
        // pointer addressed.
        uint8_t observedStart = 0;
        uint8_t observedMid = 0;
        bool asanCaught = false;
        if (plantedName && plantedBlockSize)
        {
            const uint8_t *probeMid = plantedBlock + plantedBlockSize / 2u;
            if (__asan_region_is_poisoned)
                asanCaught = __asan_region_is_poisoned(plantedName, 1) != nullptr;
            if (__asan_unpoison_memory_region)
            {
                __asan_unpoison_memory_region(plantedName, 1);
                __asan_unpoison_memory_region(probeMid, 1);
            }
            observedStart = plantedBlock[0];
            observedMid = probeMid[0];
        }
        const uint32_t poisonBlocks = poisonBlocksAfter - poisonBlocksBefore;
        const uint32_t poisonFailures = poisonFailuresAfter - poisonFailuresBefore;
        std::printf("KILLHOUSE_LIFETIME poison blocks=%u bytes=%llu failures=%u "
                    "start=0x%02x mid=0x%02x asan_caught=%d\n",
                    poisonBlocks,
                    static_cast<unsigned long long>(poisonBytesAfter - poisonBytesBefore),
                    poisonFailures, observedStart, observedMid, asanCaught ? 1 : 0);
        ok = ok && poisonBlocks > 0 && poisonFailures == 0 && observedMid == 0xDD;
        if (!Check(ok, "m5_provenance_negative"))
            return 1;
        StubResetReaderOwnedNameCount();
        // Same for the arena pointer sweep: the final fact line and the
        // lifetime verifier measure only the real zone loads that follow.
        StubResetReaderOwnedBodyCount();
    }

    // Real-zone half: the four SP boot zones in load order. The caller
    // links zone/english/{code_post_gfx,ui,common,killhouse}.ff into the
    // fixture root; without them this half SKIPs. The argc gate alone is
    // not enough: earlier killhouse-only runs (blocks/zonewalk checks)
    // also pass argv[2] while the fixture still holds the 66-byte
    // synthetic code_post_gfx.ff, so probe the real directory first. (A
    // wrong-but-plausible file cannot slip through: the lifetime verifier
    // pins SHA-256 before invoking this binary.)
    if (argc < 3)
    {
        std::puts("SKIP:KILLHOUSE_ZONE_LIFETIME needs staged retail boot zones");
        return 0;
    }
    {
        RetailZoneLoadSession probeSession{};
        FsRetailFastfileReader *probeReader = nullptr;
        FsRetailFastfileAsset *probeAssets = nullptr;
        RetailWalkDirectoryRecord *probeRecords =
            new (std::nothrow) RetailWalkDirectoryRecord[1639];
        RetailWalkDirectoryResult probeResult{};
        const bool staged =
            probeRecords != nullptr &&
            RetailWalkOpenDirectory("zone/english/code_post_gfx.ff", &probeSession,
                                    &probeReader, &probeAssets, probeRecords, 1639,
                                    &probeResult) == RETAIL_WALK_OK &&
            probeResult.code == RETAIL_WALK_OK && probeResult.assetCount == 1639;
        delete[] probeRecords;
        delete[] probeAssets;
        if (probeReader)
            FS_CloseRetailFastfile(probeReader);
        if (probeSession.active)
            RetailZoneLoadSessionAbort(&probeSession);
        if (!staged)
        {
            std::puts("SKIP:KILLHOUSE_ZONE_LIFETIME needs staged retail boot zones");
            return 0;
        }
    }
    {
        static const char *kM5Rels[4] = {
            "zone/english/code_post_gfx.ff",
            "zone/english/ui.ff",
            "zone/english/common.ff",
            "zone/english/killhouse.ff",
        };
        static const uint32_t kM5Assets[4] = {1639, 35, 6502, 1684};
        RetailZoneLoadSession sessions[4]{};
        FsRetailFastfileReader *readers[4]{nullptr, nullptr, nullptr, nullptr};
        FsRetailFastfileAsset *assetArrays[4]{nullptr, nullptr, nullptr, nullptr};
        RetailWalkDirectoryRecord *recordArrays[4]{nullptr, nullptr, nullptr, nullptr};
        uint32_t zoneBlockSizes[4][9]{};
        const uint8_t *zoneBlockData[4][9]{};
        uint32_t aliasTotal = 0, insertTotal = 0;
        uint32_t prevIndex = 0;
        bool ok = true;
        for (uint32_t z = 0; ok && z < 4; ++z)
        {
            recordArrays[z] = new (std::nothrow) RetailWalkDirectoryRecord[kM5Assets[z]];
            RetailWalkDirectoryResult openResult{};
            ok = recordArrays[z] != nullptr &&
                 RetailWalkOpenDirectory(kM5Rels[z], &sessions[z], &readers[z], &assetArrays[z],
                                          recordArrays[z], kM5Assets[z],
                                          &openResult) == RETAIL_WALK_OK &&
                 openResult.code == RETAIL_WALK_OK && sessions[z].active &&
                 openResult.assetCount == kM5Assets[z] &&
                 openResult.recordedCount == kM5Assets[z] &&
                 sessions[z].zoneIndex != 0 && sessions[z].zoneIndex != prevIndex &&
                 (z == 0 || sessions[z].zoneIndex > prevIndex);
            if (!ok)
                break;
            prevIndex = sessions[z].zoneIndex;
            uint32_t aliases = 0, inserts = 0;
            uint32_t aliasOrd = UINT32_MAX, aliasType = 99u;
            for (uint32_t i = 0; i < kM5Assets[z]; ++i)
            {
                const uint32_t header = recordArrays[z][i].header;
                if (header == 0xfffffffeu)
                    ++inserts;
                else if (header != 0u && header != 0xffffffffu)
                {
                    if (aliases == 0)
                    {
                        aliasOrd = recordArrays[z][i].ordinal;
                        aliasType = recordArrays[z][i].type;
                    }
                    ++aliases;
                }
            }
            aliasTotal += aliases;
            insertTotal += inserts;
            for (uint32_t b = 0; b < 9; ++b)
            {
                zoneBlockSizes[z][b] = sessions[z].zoneMemory->blocks[b].size;
                zoneBlockData[z][b] = sessions[z].zoneMemory->blocks[b].data;
            }
            // Retained-zone invariant: opening this zone must not have
            // moved or resized any earlier zone's blocks or data.
            for (uint32_t p = 0; ok && p < z; ++p)
            {
                for (uint32_t b = 0; b < 9; ++b)
                {
                    ok = sessions[p].zoneMemory->blocks[b].size == zoneBlockSizes[p][b] &&
                         sessions[p].zoneMemory->blocks[b].data == zoneBlockData[p][b] &&
                         StubZoneIsLive(sessions[p].zoneIndex);
                }
            }
            std::printf("KILLHOUSE_LIFETIME zone idx=%u ff=%s assets=%u aliases=%u inserts=%u aliasOrd=%u aliasType=%u\n",
                        sessions[z].zoneIndex, kM5Rels[z], kM5Assets[z], aliases, inserts,
                        aliasOrd, aliasType);
            if (!ok && !Check(false, "m5_zone_retained"))
                break;
        }
        if (!Check(ok, "m5_zone_sequence"))
            return 1;
        std::printf("KILLHOUSE_LIFETIME retained ok=1 zones=4\n");
        // Walk-only accounting for the three boot zones (killhouse itself
        // was just walked by the section above in this same process).
        // Must register nothing: the stub count is identical across each.
        for (uint32_t z = 0; z < 3; ++z)
        {
            RetailWalkDirectoryRecord *records =
                new (std::nothrow) RetailWalkDirectoryRecord[kM5Assets[z]];
            RetailWalkDirectoryResult walkResult{};
            const uint32_t regBefore = StubRegisteredCount();
            const RetailWalkDirectoryResultCode walkCode =
                RetailWalkFastfileDirectory(kM5Rels[z], records, kM5Assets[z], &walkResult);
            const uint32_t regAfter = StubRegisteredCount();
            std::printf("KILLHOUSE_LIFETIME walk ff=%s code=%d assets=%u walked=%u noStream=%u failed=%u regSame=%u b1=%u b4=%u b7=%u b8=%u\n",
                        kM5Rels[z], (int)walkCode, walkResult.assetCount,
                        walkResult.walkedDeferredCount, walkResult.noStreamCount,
                        walkResult.failedOrdinal,
                        regBefore == regAfter ? 1u : 0u, walkResult.endCursor[1],
                        walkResult.endCursor[4], walkResult.endCursor[7],
                        walkResult.endCursor[8]);
            delete[] records;
            if (!Check(walkCode == RETAIL_WALK_OK && walkResult.code == RETAIL_WALK_OK,
                        "m5_boot_walk") ||
                !Check(walkResult.assetCount == kM5Assets[z] &&
                            walkResult.walkedDeferredCount + walkResult.noStreamCount ==
                                kM5Assets[z] &&
                        walkResult.failedOrdinal == UINT32_MAX && regBefore == regAfter,
                        "m5_boot_accounting"))
                return 1;
            if (StubRegisteredCount() > registeredHigh)
                registeredHigh = StubRegisteredCount();
        }
        // Unload youngest-first (children before parents): every session
        // must die, no retired pointer may remain, and unrelated zones
        // (the overridden "key") must keep resolving.
        uint32_t dead = 0;
        for (int32_t z = 3; z >= 0; --z)
        {
            const uint32_t zoneIndex = sessions[z].zoneIndex;
            ok = RetailZoneLoadSessionAbort(&sessions[z]) && !sessions[z].active &&
                 !StubZoneIsLive(zoneIndex);
            delete[] recordArrays[z];
            delete[] assetArrays[z];
            if (readers[z])
                FS_CloseRetailFastfile(readers[z]);
            if (!ok)
                break;
            ++dead;
        }
        const XAssetHeader keyAfter =
            StubFindXAssetHeader(ASSET_TYPE_LOCALIZE_ENTRY, "key");
        ok = ok && dead == 4 && StubRetiredPtrCount() == 0 && keyAfter.data != nullptr &&
             keyAfter.localize == localizeHeader.localize;
        std::printf("KILLHOUSE_LIFETIME unload dead=%u retired=%u unrelatedLive=%u\n",
                    dead, StubRetiredPtrCount(), keyAfter.data != nullptr ? 1u : 0u);
        if (!Check(ok, "m5_unload_clean"))
            return 1;
        // Override promotion: ending the first "key" owner must promote the
        // still-live override to canonical (the real nextOverride unload
        // path), not drop the name or leave a retired pointer behind.
        ok = UnloadFixtureZone(localizeResult) &&
             StubZoneIsLive(m5ReloadZone);
        const XAssetHeader keyPromoted =
            StubFindXAssetHeader(ASSET_TYPE_LOCALIZE_ENTRY, "key");
        ok = ok && keyPromoted.localize != nullptr &&
             keyPromoted.localize != localizeHeader.localize &&
             StubRetiredPtrCount() == 0;
        std::printf("KILLHOUSE_LIFETIME promote ok=%u retired=%u\n",
                    ok ? 1u : 0u, StubRetiredPtrCount());
        std::printf("KILLHOUSE_LIFETIME water zoneHigh=%u registeredHigh=%u aliasesTotal=%u insertsTotal=%u\n",
                    StubZoneHighWater(), registeredHigh, aliasTotal, insertTotal);
        if (!Check(ok, "m5_promote_canonical"))
            return 1;
    }
    // Bounded first-frame zone load: the real
    // killhouse.ff through RetailWalkLoadZoneAssets in bounded mode --
    // world-closure families live (techset/material/image/lightdef/
    // gfxworld), everything else walks byte-accurately and stays
    // unregistered (deferred set). Prints KILLHOUSE_BOUNDED facts for
    // the preflight verifier; emits no PASS itself. Needs only
    // killhouse.ff staged (any argv[2] run with it linked); otherwise
    // SKIPs. Runs last: it registers ~900 assets and commits the real
    // world over the fixture world.
    {
        RetailZoneLoadSession probeSession{};
        FsRetailFastfileReader *probeReader = nullptr;
        FsRetailFastfileAsset *probeAssets = nullptr;
        RetailWalkDirectoryRecord *probeRecords =
            new (std::nothrow) RetailWalkDirectoryRecord[1684];
        RetailWalkDirectoryResult probeResult{};
        const bool staged =
            probeRecords != nullptr &&
            RetailWalkOpenDirectory("zone/english/killhouse.ff", &probeSession,
                                    &probeReader, &probeAssets, probeRecords, 1684,
                                    &probeResult) == RETAIL_WALK_OK &&
            probeResult.code == RETAIL_WALK_OK && probeResult.assetCount == 1684;
        delete[] probeRecords;
        delete[] probeAssets;
        if (probeReader)
            FS_CloseRetailFastfile(probeReader);
        if (probeSession.active)
            RetailZoneLoadSessionAbort(&probeSession);
        if (argc < 3 || !staged)
        {
            std::puts("SKIP:KILLHOUSE_BOUNDED needs staged retail killhouse.ff");
            return 0;
        }
    }
    {
        // R_InitImages (gfx_d3d/r_image.cpp) always creates and registers
        // this exact stock identity-normal-map image before any SP zone
        // loads in the real boot sequence (Image_LoadIdentityNormalMap ->
        // Image_LoadSolid(0x80,0x80,0xFF,0x80), a 1x1 D3DFMT_A8R8G8B8
        // texture) -- countless real materials reference it by name for a
        // "no real normal map" default (143 of killhouse's own, per the
        // OAT oracle dump). This narrow
        // database-only test never runs R_InitImages (no live D3D9
        // device), so seed the same header a real boot would already have
        // produced by this point -- CPU-side data only, matching every
        // other image this test widens (no renderer resource created
        // before a live device).
        static GfxImage identityNormalMap{};
        identityNormalMap.mapType = MAPTYPE_2D;
        identityNormalMap.width = 1;
        identityNormalMap.height = 1;
        identityNormalMap.depth = 1;
        identityNormalMap.name = "$identitynormalmap";
        StubRegisterStockAsset(ASSET_TYPE_IMAGE, XAssetHeader(&identityNormalMap));

        // Real killhouse materials carry comma-stub TechniqueSet/Material/
        // Image references whose actual definition lives in an
        // earlier-loaded zone (this file's own comment on
        // ResolveTechniqueSetRef; confirmed against real data --
        // the "wc_l_sm_b0c0n0s0" finding, which the OAT
        // oracle confirms lives in common.ff specifically, not
        // code_post_gfx.ff). The real boot order is code_post_gfx -> ui ->
        // common -> killhouse (above); bounded-live-load all three
        // here too, the same way the real-zone half opens all four in order for
        // its lifetime proof, so those cross-zone stubs have something
        // real to resolve against. Each is independently gated on being
        // staged alongside killhouse.ff (the "lifetime" fixture symlinks
        // all four; the plain "killhouse" fixture does not) -- otherwise
        // that step is skipped and killhouse's own bounded load runs
        // exactly as before, unregistered cross-zone stubs and all. A
        // zone's own bounded load failing (its own unrelated deferred-
        // asset gaps) does not block trying the next one or killhouse
        // itself -- best-effort, matching this section's existing
        // tolerance for partial cross-zone data.
        // per-type census snapshot: every LoadedSound/SndCurve/Menu
        // declaration reached through the four full-policy zone loads below
        // must call its original registration owner. Snapshot before the
        // sequence (earlier fixture tests register sounds/menus too).
        const uint32_t menuAttemptsBefore = StubRegisterAttemptCount(ASSET_TYPE_MENU);
        const uint32_t loadedSoundAttemptsBefore =
            StubRegisterAttemptCount(ASSET_TYPE_LOADED_SOUND);
        const uint32_t sndCurveAttemptsBefore =
            StubRegisterAttemptCount(ASSET_TYPE_SOUND_CURVE);
        // broadened census: the same four full-policy zone loads are
        // the "broader traversal" the evidence asks for -- every
        // reachable asset family, not the scripted walk window.  Snapshot
        // all 33 type counters so the table can be diffed against the
        // independent OAT Unlinker --list totals for these zones.
        uint32_t typeAttemptsBefore[ASSET_TYPE_COUNT];
        for (uint32_t t = 0; t < ASSET_TYPE_COUNT; ++t)
            typeAttemptsBefore[t] = StubRegisterAttemptCount(t);

        auto boundedLoadEarlierZone = [](const char *rel, uint32_t assetCount,
                                         RetailWalkLoadZoneResult *resultOut) -> bool
        {
            RetailZoneLoadSession probe{};
            FsRetailFastfileReader *probeReader = nullptr;
            FsRetailFastfileAsset *probeAssets = nullptr;
            RetailWalkDirectoryRecord *probeRecords =
                new (std::nothrow) RetailWalkDirectoryRecord[assetCount];
            RetailWalkDirectoryResult probeResult{};
            const bool staged =
                probeRecords != nullptr &&
                RetailWalkOpenDirectory(rel, &probe, &probeReader, &probeAssets, probeRecords,
                                        assetCount, &probeResult) == RETAIL_WALK_OK &&
                probeResult.code == RETAIL_WALK_OK && probeResult.assetCount == assetCount;
            delete[] probeRecords;
            delete[] probeAssets;
            if (probeReader)
                FS_CloseRetailFastfile(probeReader);
            if (probe.active)
                RetailZoneLoadSessionAbort(&probe);
            if (!staged)
                return false;
            const uint32_t menuAttemptsZoneBefore =
                StubRegisterAttemptCount(ASSET_TYPE_MENU);
            RetailWalkLoadZoneResult result{};
            const RetailWalkLoadZoneResultCode code =
                RetailWalkLoadZoneAssets(rel, &result, false, false);
            std::printf("KILLHOUSE_BOUNDED %s code=%d assets=%u techsets=%u materials=%u "
                        "images=%u lightdefs=%u sounds=%u soundsDeferred=%u "
                        "sndNameB0=%u sndNameOther=%u sndNamePool=%u sndNameUnknown=%u "
                        "sndFileAlias=%u sndCurveAlias=%u sndMapAlias=%u sndHeadAlias=%u "
                        "nestedLSounds=%u nestedSCurves=%u menuOwners=%u weapons=%u failed=%u failedType=%u\n",
                        rel, (int)code, result.assetCount, result.registeredTechniqueSetCount,
                        result.registeredMaterialCount, result.registeredImageCount,
                        result.registeredLightDefCount, result.registeredSoundCount,
                        result.soundDeferredNestedCount,
                        result.soundDeferredNameBlock0Count,
                        result.soundDeferredNameOtherBlockCount,
                        result.soundDeferredNamePoolCount,
                        result.soundDeferredNameUnclassifiedCount,
                        result.soundDeferredSoundFileAliasCount,
                        result.soundDeferredCurveAliasCount,
                        result.soundDeferredSpeakerMapAliasCount,
                        result.soundDeferredHeadAliasCount,
                        result.registeredNestedLoadedSoundCount,
                        result.registeredNestedSndCurveCount,
                        StubRegisterAttemptCount(ASSET_TYPE_MENU) - menuAttemptsZoneBefore,
                        result.registeredWeaponCount,
                        result.failedOrdinal, result.failedType);
            if (resultOut)
                *resultOut = result;
            return code == RETAIL_WALK_LOAD_OK && result.code == RETAIL_WALK_LOAD_OK;
        };
        RetailWalkLoadZoneResult cpGfxResult{};
        RetailWalkLoadZoneResult uiResult{};
        RetailWalkLoadZoneResult commonResult{};
        const bool cpGfxOk =
            boundedLoadEarlierZone("zone/english/code_post_gfx.ff", 1639, &cpGfxResult);
        const bool uiOk = boundedLoadEarlierZone("zone/english/ui.ff", 35, &uiResult);
        const bool commonOk =
            boundedLoadEarlierZone("zone/english/common.ff", 6502, &commonResult);
        std::printf("KILLHOUSE_BOUNDED lookup weap_pickup=%d grenade_pickup=%d frag=%d smoke=%d flash=%d\n",
                    StubFindXAssetHeader(ASSET_TYPE_SOUND, "weap_pickup").sound ? 1 : 0,
                    StubFindXAssetHeader(ASSET_TYPE_SOUND, "grenade_pickup").sound ? 1 : 0,
                    StubFindXAssetHeader(ASSET_TYPE_WEAPON, "fraggrenade").weapon ? 1 : 0,
                    StubFindXAssetHeader(ASSET_TYPE_WEAPON, "smoke_grenade_american").weapon ? 1 : 0,
                    StubFindXAssetHeader(ASSET_TYPE_WEAPON, "flash_grenade").weapon ? 1 : 0);

        const uint32_t regsBeforeBounded = StubRegisteredCount();
        RetailWalkLoadZoneResult boundedResult{};
        const RetailWalkLoadZoneResultCode boundedCode =
            RetailWalkLoadZoneAssets("zone/english/killhouse.ff", &boundedResult, false, false);
        std::printf("KILLHOUSE_BOUNDED code=%d assets=%u techsets=%u materials=%u images=%u lightdefs=%u gfxworlds=%u walkedOnly=%u failed=%u failedType=%u nullTechNames=%u nullShaderNames=%u deferred=%u resolved=%u subDeferred=%u subResolved=%u regDelta=%u sounds=%u soundsDeferred=%u sndNameB0=%u sndNameOther=%u sndNamePool=%u sndNameUnknown=%u sndFileAlias=%u sndCurveAlias=%u sndMapAlias=%u sndHeadAlias=%u nestedLSounds=%u nestedSCurves=%u\n",
                    (int)boundedCode, boundedResult.assetCount,
                    boundedResult.registeredTechniqueSetCount,
                    boundedResult.registeredMaterialCount, boundedResult.registeredImageCount,
                    boundedResult.registeredLightDefCount, boundedResult.registeredGfxWorldCount,
                    boundedResult.walkedOnlyCount, boundedResult.failedOrdinal,
                    boundedResult.failedType, boundedResult.nullTechniqueNameCount,
                    boundedResult.nullShaderNameCount, boundedResult.deferredTechniqueAliasCount,
                    boundedResult.resolvedTechniqueAliasCount,
                    boundedResult.deferredSubAliasCount, boundedResult.resolvedSubAliasCount,
                    StubRegisteredCount() - regsBeforeBounded,
                    boundedResult.registeredSoundCount,
                    boundedResult.soundDeferredNestedCount,
                    boundedResult.soundDeferredNameBlock0Count,
                    boundedResult.soundDeferredNameOtherBlockCount,
                    boundedResult.soundDeferredNamePoolCount,
                    boundedResult.soundDeferredNameUnclassifiedCount,
                    boundedResult.soundDeferredSoundFileAliasCount,
                    boundedResult.soundDeferredCurveAliasCount,
                    boundedResult.soundDeferredSpeakerMapAliasCount,
                    boundedResult.soundDeferredHeadAliasCount,
                    boundedResult.registeredNestedLoadedSoundCount,
                    boundedResult.registeredNestedSndCurveCount);
        if (!Check(boundedCode == RETAIL_WALK_LOAD_OK &&
                        boundedResult.code == RETAIL_WALK_LOAD_OK,
                    "m9a_bounded_load") ||
            !Check(boundedResult.assetCount == 1684 &&
                        boundedResult.registeredGfxWorldCount == 1 &&
                        boundedResult.registeredClipMapCount == 1 &&
                        boundedResult.failedOrdinal == UINT32_MAX,
                    "m9a_bounded_counts"))
            return 1;

        // Variants of one alias share a single aliasName pointer, as retail's
        // offset back-references do; per-variant copies restarted every
        // multi-variant loop sound each frame (emt_ac_metal_rattle re-opened
        // and decoded its WAV per frame on the main thread).
        {
            uint32_t multiVariantLists = 0;
            const char *firstSplit = nullptr;
            const uint32_t splitLists = StubSoundAliasNameSplitCount(&multiVariantLists, &firstSplit);
            std::printf("KILLHOUSE_SOUND_ALIAS_IDENTITY multi_variant_lists=%u split=%u first_split=%s\n",
                        multiVariantLists, splitLists, firstSplit ? firstSplit : "-");
            if (!Check(multiVariantLists > 0 && splitLists == 0, "sound_alias_name_identity"))
                return 1;
        }

        // Every FX element with velocity samples has >= 1 interval
        // (fx_convert.cpp builds velIntervalCount = states - 1 and asserts it).
        // The decoder used to recompute the elem array start from the name
        // length; for shellejects/* that landed 24-28 bytes off, decoding an
        // all-zero prefix under real velSamples and tripping
        // FX_IntegrateVelocity's assert (fx_update.cpp:764) when firing.
        {
            uint32_t fxEffects = 0, fxElems = 0;
            const char *firstBad = nullptr;
            int firstBadElem = -1;
            const uint32_t badVel =
                StubFxVelIntervalZeroCount(&fxEffects, &fxElems, &firstBad, &firstBadElem);
            std::printf("KILLHOUSE_FX_VEL_INTERVALS effects=%u elems=%u bad=%u first=%s elem=%d\n",
                        fxEffects, fxElems, badVel, firstBad ? firstBad : "-", firstBadElem);
            if (!Check(fxEffects > 0 && badVel == 0, "fx_vel_interval_decode"))
                return 1;
        }

        // Nav-graph tree on the real map (see PathTreeCensus): the decoded
        // GameWorldSp must be a usable spatial KD tree over its own nodes, or
        // every actor's path search fails with "couldn't find path to goal".
        {
            PathTreeCensus census{};
            PathTreeStructures(gameWorldSp.path, gameWorldSp.path.nodeTree, census);
            PathTreeSelfReach(gameWorldSp.path, census);
            std::printf("KILLHOUSE_PATH_TREE nodes=%d leaves=%d interior=%d splitChecks=%d "
                        "splitViolations=%d selfReach=%d/%u\n",
                        census.nodes, census.leaves, census.interior, census.splitChecks,
                        census.splitViolations, census.selfReach, gameWorldSp.path.nodeCount);
            if (!Check(census.nodes == static_cast<int>(gameWorldSp.path.nodeCount) &&
                           census.leaves > 0 && census.interior > 0 &&
                           census.splitChecks == census.interior &&
                           census.splitViolations == 0 &&
                           census.selfReach == census.nodes,
                       "killhouse_path_tree"))
                return 1;
            std::printf("PASS:KILLHOUSE_PATH_TREE nodes=%d splits=%d selfReach=%d\n", census.nodes,
                        census.splitChecks, census.selfReach);
        }

        // per-type census (the four full-policy zone loads above, not
        // one scripted walk window): the original loader's registration
        // owners must be called for every nested declaration the OAT oracle
        // counts for these zones -- Load_MenuAsset for menus nested in
        // menulists (14+56+71+7=148), Load_LoadedSoundAsset for embedded
        // LoadedSounds (9+0+763+132=904), and Load_SndCurveAsset for
        // falloff curves (1 direct + 1+0+7+6 nested = 15). A missing call
        // leaves the nested object unregistered.
        if (cpGfxOk && uiOk && commonOk)
        {
            const uint32_t menuAttempts =
                StubRegisterAttemptCount(ASSET_TYPE_MENU) - menuAttemptsBefore;
            const uint32_t loadedSoundAttempts =
                StubRegisterAttemptCount(ASSET_TYPE_LOADED_SOUND) - loadedSoundAttemptsBefore;
            const uint32_t sndCurveAttempts =
                StubRegisterAttemptCount(ASSET_TYPE_SOUND_CURVE) - sndCurveAttemptsBefore;
            std::printf("KILLHOUSE_TYPE_CENSUS menu=%u loadedSound=%u sndCurve=%u\n",
                        menuAttempts, loadedSoundAttempts, sndCurveAttempts);
            std::printf("KILLHOUSE_TYPE_CENSUS_ALL");
            for (uint32_t t = 0; t < ASSET_TYPE_COUNT; ++t)
                std::printf(" t%u=%u", t,
                            StubRegisterAttemptCount(t) - typeAttemptsBefore[t]);
            std::printf("\n");
            if (!Check(menuAttempts == 148 && loadedSoundAttempts == 904 &&
                           sndCurveAttempts == 15,
                       "p6b1_type_census"))
                return 1;

            // COMPASS_TYPE_FULL renders solid white: host-side
            // falsification of the suspected cause, plus the census that
            // locates the real full-map draw inputs.
            //
            // 1. compass_map_killhouse -- the material the killhouse script
            //    itself names (decompressed killhouse.ff:
            //    _compass::setupMiniMap("compass_map_killhouse")) and the one
            //    both the radar (ownerdraw 159) and the full map (181) draw
            //    -- decodes with stateFlags=0 and techset '2d', so
            //    R_AddCmdDrawStretchPic's default-material substitution
            //    (fogable technique or stateFlags&0x10), which would make the
            //    plain full-map path white while the rotated radar path
            //    stayed textured, cannot fire. Checked below.
            // 2. No menu registered by the four-zone traversal carries an
            //    ownerdraw in the full-map family 180..187, and the only
            //    full-map-named SP menu (common.ff ui/hud.txt 'overheadmap')
            //    decodes with 0 items, so COMPASS_TYPE_FULL is not reachable
            //    through the decoded SP menu set. Printed below.
            // 3. The white screen with green text in the capture ring
            //    (already rejected as nonblank by a screenshot freshness
            //    check) is the mission-briefing screen:
            //    'briefing' item 0 is ownerdraw 277
            //    (R_Cinematic_DrawStretchPic_Letterboxed) with a full-screen
            //    'white' background, and the Switch cinematic stub is a
            //    no-op, so the white background is what remains. That is the
            //    Bink boundary, not a material decode gap. Checked below.
            const Material *compassMap =
                StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "compass_map_killhouse").material;
            bool compassMapDrawable = false;
            if (compassMap)
            {
                const MaterialTechniqueSet *techSet = compassMap->techniqueSet;
                const char *techName = techSet && techSet->name ? techSet->name : "<null>";
                const bool usesDepthBuffer = (compassMap->stateFlags & 0x10) != 0;
                const bool litBegin = techSet && techSet->techniques[7] != nullptr;
                const bool emissive = techSet && techSet->techniques[5] != nullptr;
                const bool nameHas2d = std::strstr(techName, "2d") != nullptr;
                const bool fogable = !nameHas2d && (litBegin || emissive);
                compassMapDrawable = !fogable && !usesDepthBuffer;
                std::printf("KILLHOUSE_COMPASS_MATERIAL name='%s' stateFlags=0x%02x techset='%s' "
                            "litBegin=%d emissive=%d nameHas2d=%d usesDepthBuffer=%d "
                            "drawStretchPicSubstitutes=%d\n",
                            compassMap->info.name ? compassMap->info.name : "<null>",
                            compassMap->stateFlags, techName, litBegin ? 1 : 0, emissive ? 1 : 0,
                            nameHas2d ? 1 : 0, usesDepthBuffer ? 1 : 0,
                            compassMapDrawable ? 0 : 1);
            }
            else
            {
                std::printf("KILLHOUSE_COMPASS_MATERIAL absent\n");
            }
            if (!Check(compassMap && compassMapDrawable, "d29_compass_material_drawable"))
                return 1;

            constexpr uint32_t kMaxMenuCollect = 4096;
            static XAssetHeader allMenus[kMaxMenuCollect];
            const uint32_t menuCountRegistered =
                StubCollectRegistered(ASSET_TYPE_MENU, allMenus, kMaxMenuCollect);
            std::printf("KILLHOUSE_MENU_CENSUS registered=%u\n", menuCountRegistered);
            uint32_t fullMapOwnerdraws = 0;
            int32_t overheadMapItems = -1;
            for (uint32_t i = 0; i < menuCountRegistered; ++i)
            {
                const menuDef_t *menu = allMenus[i].menu;
                if (!menu || !menu->window.name)
                    continue;
                if (std::strcmp(menu->window.name, "overheadmap") == 0)
                    overheadMapItems = menu->itemCount;
                bool anyOwnerdraw = menu->window.ownerDraw != 0;
                for (int32_t itemIndex = 0; itemIndex < menu->itemCount && menu->items; ++itemIndex)
                    if (menu->items[itemIndex] && menu->items[itemIndex]->window.ownerDraw)
                        anyOwnerdraw = true;
                if (!anyOwnerdraw)
                    continue;
                std::printf("KILLHOUSE_MENU_OWNERDRAW menu='%s' itemCount=%d menuOwner=%d\n",
                            menu->window.name, menu->itemCount, menu->window.ownerDraw);
                for (int32_t itemIndex = 0; itemIndex < menu->itemCount && menu->items; ++itemIndex)
                {
                    const itemDef_s *item = menu->items[itemIndex];
                    const int ownerDraw = item ? item->window.ownerDraw : 0;
                    if (!ownerDraw)
                        continue;
                    const char *bgName =
                        item->window.background ? item->window.background->info.name : nullptr;
                    std::printf("KILLHOUSE_MENU_OWNERDRAW_ITEM menu='%s' i=%d type=%d ownerdraw=%d "
                                "bg=%s rect=%.1f,%.1f,%.1f,%.1f\n",
                                menu->window.name, itemIndex, item->type, ownerDraw,
                                bgName ? bgName : "<null>", item->window.rect.x,
                                item->window.rect.y, item->window.rect.w, item->window.rect.h);
                    if (ownerDraw >= 180 && ownerDraw <= 187)
                        ++fullMapOwnerdraws;
                }
            }
            std::printf("KILLHOUSE_FULLMAP_OWNERDRAW count=%u overheadmap_items=%d\n",
                        fullMapOwnerdraws, overheadMapItems);

            // OAT ground truth for the fullscreen briefing item
            // (code_post_gfx.ff ui/briefing.menu): ownerdraw 277, rect
            // -107 0 854 480, `background "cinematic"`.  OAT's material dump
            // has code_post_gfx `cinematic` as a real body (techniqueSet
            // 'cinematic', zero textures), and the engine's Menu_Setup binds
            // the declared name through Material_RegisterHandle, so the port
            // must bind that zone body.  The old bg=white pin was the
            // pre-sweep decoder misresolving the same slot; the Switch
            // cinematic ownerdraw 277 remains the documented no-op, so
            // this backdrop still proves the slot is a real registered body
            // and never a substituted/default one.
            const menuDef_t *briefing = StubFindXAssetHeader(ASSET_TYPE_MENU, "briefing").menu;
            bool briefingShape = false;
            if (briefing && briefing->items && briefing->itemCount > 0 && briefing->items[0])
            {
                const itemDef_s *backgroundItem = briefing->items[0];
                const Material *bgMaterial = backgroundItem->window.background;
                const char *bgName = bgMaterial ? bgMaterial->info.name : nullptr;
                const char *bgTechSet = bgMaterial && bgMaterial->techniqueSet
                    ? bgMaterial->techniqueSet->name : nullptr;
                briefingShape = backgroundItem->window.ownerDraw == 277 && bgName &&
                    std::strcmp(bgName, "cinematic") == 0 && bgTechSet &&
                    std::strcmp(bgTechSet, "cinematic") == 0 &&
                    !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "cinematic") &&
                    backgroundItem->window.rect.w >= 854.0f &&
                    backgroundItem->window.rect.h >= 480.0f;
                std::printf("KILLHOUSE_BRIEFING_BACKDROP ownerdraw=%d bg=%s techset=%s "
                            "rect=%.1f,%.1f,%.1f,%.1f\n",
                            backgroundItem->window.ownerDraw, bgName ? bgName : "<null>",
                            bgTechSet ? bgTechSet : "<null>",
                            backgroundItem->window.rect.x, backgroundItem->window.rect.y,
                            backgroundItem->window.rect.w, backgroundItem->window.rect.h);
            }
            if (!Check(briefingShape, "d29_briefing_stub_backdrop"))
                return 1;

            // the white-with-green walk-ring frame
            // is not a missing black backdrop -- the retail killhouse spawn
            // sequence never creates one. Decompressed killhouse.ff /
            // _introscreen.gsc: _introscreen::main()'s killhouse case calls
            // introscreen_delay, which returns through flying_intro()
            // (flying_levels["killhouse"] = true). That branch feeds
            // KILLHOUSE_INTROSCREEN_LINE_1/3/4/5 through
            // introscreen_feed_lines -> _CornerLineThread (x=20, bottom
            // align, font 'objective', color (0.8,1.0,0.8)) -- the exact
            // green lower-left lines in the capture -- and its
            // introscreen_generic_black_fade_in call is commented out (as is
            // every killhouse_introscreen call site). The only fullscreen
            // fade the killhouse scripts run is
            // introscreen_generic_white_fade_in(.1, 0.30) in inside_start():
            // a fullscreen foreground 'white' HUD element. That element must
            // resolve to the real 1x1 zone body (and the briefing backdrop
            // above to the real code_post_gfx `cinematic` body), so a
            // correctly drawn scripted white frame is never a default or
            // synthesized material.
            const Material *whiteFade =
                StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "white").material;
            const Material *blackFade =
                StubFindXAssetHeader(ASSET_TYPE_MATERIAL, "black").material;
            const GfxImage *whiteFadeImage =
                (whiteFade && whiteFade->textureCount > 0 && whiteFade->textureTable)
                    ? MaterialTextureImage(whiteFade->textureTable[0]) : nullptr;
            const GfxImage *blackFadeImage =
                (blackFade && blackFade->textureCount > 0 && blackFade->textureTable)
                    ? MaterialTextureImage(blackFade->textureTable[0]) : nullptr;
            const bool whiteFadeReal =
                whiteFade && whiteFade->info.name && whiteFadeImage &&
                std::strcmp(whiteFade->info.name, "white") == 0 &&
                std::strcmp(whiteFadeImage->name, "$white") == 0 &&
                whiteFadeImage->width == 1 && whiteFadeImage->height == 1 &&
                !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "white");
            const bool blackFadeReal =
                blackFade && blackFade->info.name && blackFadeImage &&
                std::strcmp(blackFade->info.name, "black") == 0 &&
                std::strcmp(blackFadeImage->name, "$black") == 0 &&
                blackFadeImage->width == 1 && blackFadeImage->height == 1 &&
                !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "black");
            std::printf("KILLHOUSE_INTRO_FADE white=%d whiteImage='%s' black=%d "
                        "blackImage='%s' synth=%u\n",
                        whiteFadeReal ? 1 : 0,
                        whiteFadeImage ? whiteFadeImage->name : "<null>",
                        blackFadeReal ? 1 : 0,
                        blackFadeImage ? blackFadeImage->name : "<null>",
                        RetailMaterialSynthesizedCount());
            if (!Check(whiteFadeReal && blackFadeReal, "d29_intro_fade_materials"))
                return 1;
            // broader traversal: the four full-policy SP zone loads
            // (code_post_gfx/ui/common/killhouse) above are the non-scripted
            // traversal the evidence asks for; no material body may be
            // manufactured there (the original loader would have registered
            // or loudly failed). Offline tooling also requires
            // this line's synth=0, so the zero is gate-enforced, not just
            // printed.
            if (!Check(RetailMaterialSynthesizedCount() == 0,
                       "p6b1_broader_traversal_no_synth"))
                return 1;

            // alias closure: Load_snd_alias_t binds a non-inline
            // soundFile/speakerMap through DB_ConvertOffsetToPointer (the
            // block-4 struct an earlier load resident) and a falloff
            // curve/embedded LoadedSound through DB_ConvertOffsetToAlias
            // (the 4-byte pointer field the owning load wrote). Every one of
            // those references must have bound the earlier object in all
            // four zones -- the decoder fails the zone load loudly on a
            // miss, so a successful load plus all-zero classes here is the
            // restored side effect, not a tolerated null.
            auto soundRefsClosed = [](const RetailWalkLoadZoneResult &r) -> bool
            {
                return r.soundDeferredNestedCount == 0 &&
                       r.soundDeferredNameBlock0Count == 0 &&
                       r.soundDeferredNameOtherBlockCount == 0 &&
                       r.soundDeferredNamePoolCount == 0 &&
                       r.soundDeferredNameUnclassifiedCount == 0 &&
                       r.soundDeferredSoundFileAliasCount == 0 &&
                       r.soundDeferredCurveAliasCount == 0 &&
                       r.soundDeferredSpeakerMapAliasCount == 0 &&
                       r.soundDeferredHeadAliasCount == 0;
            };
            std::printf("KILLHOUSE_SOUND_ALIAS_CLOSURE cpGfx=%u ui=%u common=%u killhouse=%u\n",
                        cpGfxResult.soundDeferredNestedCount,
                        uiResult.soundDeferredNestedCount,
                        commonResult.soundDeferredNestedCount,
                        boundedResult.soundDeferredNestedCount);
            if (!Check(soundRefsClosed(cpGfxResult) && soundRefsClosed(uiResult) &&
                           soundRefsClosed(commonResult) && soundRefsClosed(boundedResult),
                       "p6b1_sound_alias_closure"))
                return 1;

            // Concrete alias identity on real data: code_post_gfx's
            // match_countdown_tick aliases the curve field of the earlier
            // enter_prestige list (its own curve is inline). The original
            // DB_ConvertOffsetToAlias copies that field, so both lists must
            // hold the one registered SndCurve -- not two re-parsed copies.
            const snd_alias_list_t *ownerList =
                StubFindXAssetHeader(ASSET_TYPE_SOUND, "enter_prestige").sound;
            const snd_alias_list_t *aliasList =
                StubFindXAssetHeader(ASSET_TYPE_SOUND, "match_countdown_tick").sound;
            if (!Check(ownerList && ownerList->head && ownerList->count > 0 &&
                           aliasList && aliasList->head && aliasList->count > 0 &&
                           ownerList->head[0].volumeFalloffCurve != nullptr &&
                           ownerList->head[0].volumeFalloffCurve ==
                               aliasList->head[0].volumeFalloffCurve,
                       "p6b1_sound_curve_alias_identity"))
                return 1;

            // Pointer-identity half of the census: every widened alias
            // graph node must bind the header the original owner registered
            // (the Load_LoadedSoundAsset/Load_SndCurveAsset return value),
            // not the unregistered arena object the widener built. The
            // lookup below is the registry's own by-name contract.
            constexpr uint32_t kMaxSoundCollect = 16384;
            static XAssetHeader soundLists[kMaxSoundCollect];
            const uint32_t listCount = StubCollectRegistered(
                ASSET_TYPE_SOUND, soundLists, kMaxSoundCollect);
            uint32_t boundLoadedSounds = 0;
            uint32_t boundSndCurves = 0;
            uint32_t badBindings = 0;
            uint32_t unregisteredNested = 0;
            // Consumer contract: Com_GetGraphList/DevGui_AddGraph asserts
            // every registered SndCurve has knotCount >= 2 (devgui.cpp:496).
            // A stale temp-block root copy read here shows up as a bad curve
            // count long before a guest boot reaches devgui.
            uint32_t badCurveKnots = 0;
            for (uint32_t i = 0; i < listCount; ++i)
            {
                const snd_alias_list_t *list = soundLists[i].sound;
                if (!list || !list->head || list->count <= 0)
                    continue;
                for (int32_t a = 0; a < list->count; ++a)
                {
                    const snd_alias_t *alias = &list->head[a];
                    if (alias->soundFile && alias->soundFile->type == 1 &&
                        alias->soundFile->u.loadSnd)
                    {
                        const LoadedSound *ls = alias->soundFile->u.loadSnd;
                        const XAssetHeader reg =
                            StubFindXAssetHeader(ASSET_TYPE_LOADED_SOUND,
                                                 ls->name ? ls->name : "");
                        if (!reg.loadSnd)
                            ++unregisteredNested;
                        else if (reg.loadSnd != ls)
                            ++badBindings;
                        else
                            ++boundLoadedSounds;
                    }
                    if (alias->volumeFalloffCurve)
                    {
                        const SndCurve *curve = alias->volumeFalloffCurve;
                        const XAssetHeader reg =
                            StubFindXAssetHeader(ASSET_TYPE_SOUND_CURVE,
                                                 curve->filename ? curve->filename : "");
                        if (!reg.sndCurve)
                            ++unregisteredNested;
                        else if (reg.sndCurve != curve)
                            ++badBindings;
                        else
                            ++boundSndCurves;
                        // Production's DB_LinkXAssetEntry strips the leading
                        // comma and rebinds a stub curve to the earlier zone's
                        // canonical owner; the host double registers it as its
                        // own entry, so only check the real bodies' devgui
                        // contract here (DevGui_AddGraph requires >=2 knots).
                        if ((!curve->filename || curve->filename[0] != ',') &&
                            curve->knotCount < 2)
                            ++badCurveKnots;
                    }
                }
            }
            std::printf("KILLHOUSE_NESTED_BIND loadedSounds=%u sndCurves=%u "
                        "bad=%u unregistered=%u badCurveKnots=%u lists=%u\n",
                        boundLoadedSounds, boundSndCurves, badBindings,
                        unregisteredNested, badCurveKnots, listCount);
            if (!Check(badBindings == 0 && unregisteredNested == 0 &&
                           badCurveKnots == 0 &&
                           boundLoadedSounds > 0 && boundSndCurves > 0,
                       "p6b1_nested_bind"))
                return 1;
        }
        uint32_t worldMats = 0, worldImages = 0, worldLightDefs = 0, worldBlock1 = 0;
        RetailWorldLastLoadStats(&worldMats, &worldImages, &worldLightDefs, &worldBlock1);
        // The wire carries both a name and a base name; the registered one
        // is whichever the decoder widened into world->name (the walk
        // reads "killhouse"). R_LoadWorld looks up "maps/<map>.d3dbsp",
        // so report both spellings and take whichever registered.
        XAssetHeader boundedHeader = StubFindXAssetHeader(ASSET_TYPE_GFXWORLD, "killhouse");
        const char *boundedName = "killhouse";
        if (!boundedHeader.gfxWorld)
        {
            boundedHeader = StubFindXAssetHeader(ASSET_TYPE_GFXWORLD, "maps/killhouse.d3dbsp");
            boundedName = "maps/killhouse.d3dbsp";
        }
        std::printf("KILLHOUSE_BOUNDED registeredName='%s'\n", boundedName);
        if (!Check(boundedHeader.gfxWorld != nullptr && boundedHeader.gfxWorld == &s_world,
                    "m9a_world_singleton"))
            return 1;
        // Live proof on real killhouse.ff: the ClipMap committed into
        // cm with the collision graph SV_SetBrushModel/SV_LinkEntity read
        // (cmodels, nodes, leafs, planes) and the nested MapEnts the
        // script probe parses. Counts are reported, not pinned: the
        // fixture proof above pins exact widening semantics.
        XAssetHeader boundedClipHeader =
            StubFindXAssetHeader(ASSET_TYPE_CLIPMAP, "maps/killhouse.d3dbsp");
        if (!boundedClipHeader.clipMap)
            boundedClipHeader = StubFindXAssetHeader(ASSET_TYPE_CLIPMAP, "killhouse");
        std::printf("KILLHOUSE_BOUNDED clipmap found=%d cmodels=%u nodes=%u leafs=%u planes=%d brushes=%u mapents=%d\n",
                    boundedClipHeader.clipMap != nullptr, cm.numSubModels, cm.numNodes,
                    cm.numLeafs, cm.planeCount, (unsigned)cm.numBrushes,
                    cm.mapEnts != nullptr ? 1 : 0);
        if (!Check(boundedClipHeader.clipMap != nullptr && boundedClipHeader.clipMap == &cm &&
                        cm.cmodels && cm.numSubModels > 0 && cm.nodes && cm.numNodes > 0 &&
                        cm.leafs && cm.numLeafs > 0 && cm.planes && cm.planeCount > 0 &&
                        cm.mapEnts && cm.mapEnts->entityString &&
                        cm.mapEnts->numEntityChars > 0,
                    "m9a_clipmap_singleton"))
            return 1;
        const GfxWorld *world = boundedHeader.gfxWorld;
        const bool visibilityGraphOwned =
            world->dpvsPlanes.cellCount == 0 ||
            (world->dpvsPlanes.planes && world->dpvsPlanes.nodes && world->cells);
        std::printf("KILLHOUSE_BOUNDED visibility planes=%p nodes=%p cells=%p owned=%d\n",
                    (const void *)world->dpvsPlanes.planes,
                    (const void *)world->dpvsPlanes.nodes,
                    (const void *)world->cells, visibilityGraphOwned ? 1 : 0);
        if (!Check(visibilityGraphOwned, "m9a_visibility_graph_owned"))
            return 1;
        std::printf("KILLHOUSE_BOUNDED world name='%s' base='%s' surfaces=%d vertices=%u indices=%d cells=%d lightmaps=%d probes=%d models=%d sunPrimary=%u skyImage=%s skySamplerState=%u outdoorImage=%s sunDef=%s\n",
                    world->name ? world->name : "(null)",
                    world->baseName ? world->baseName : "(null)",
                    world->dpvs.staticSurfaceCount, world->vertexCount, world->indexCount,
                    world->dpvsPlanes.cellCount, world->lightmapCount,
                    world->reflectionProbeCount, world->modelCount,
                    world->sunPrimaryLightIndex,
                    world->skyImage ? world->skyImage->name : "(null)",
                    (unsigned)world->skySamplerState,
                    world->outdoorImage ? world->outdoorImage->name : "(null)",
                    world->sunLight && world->sunLight->def && world->sunLight->def->name ?
                        world->sunLight->def->name : "(null)");
        // Scoping: RB_SetBspImages (src/gfx_d3d/rb_backend.cpp)
        // asserts !skyImage || (skySamplerState & SAMPLER_FILTER_MASK). Confirm the
        // real wire byte (offset 44, verified against OAT's IW3 ZoneCode loader)
        // actually clears that assert for killhouse's real non-null skyImage,
        // rather than trusting the decode silently.
        if (!Check(!world->skyImage || (world->skySamplerState & 7) != 0,
                    "m8_sky_sampler_state"))
            return 1;
        std::printf("KILLHOUSE_BOUNDED subtree mats=%u images=%u lightdefs=%u block1=%u\n",
                    worldMats, worldImages, worldLightDefs, worldBlock1);
        // Scoping: R_LoadWorld's own reflectionProbeIndex/
        // lightmapIndex loops write world->reflectionProbeTextures[i].basemap /
        // world->lightmapPrimaryTextures[i].basemap / ...SecondaryTextures[i].basemap
        // unconditionally for every real probe/lightmap -- a null array here is a
        // guaranteed null-pointer write the moment R_LoadWorld runs against a
        // real-shaped world. Exercise exactly that write pattern (no live device
        // needed -- GfxTexture is a 4-byte handle union, not a resource) to prove
        // the arrays are real and correctly sized, not just present.
        bool textureArraysOk = true;
        for (uint32_t probe = 0; probe < world->reflectionProbeCount && textureArraysOk; ++probe)
        {
            if (!world->reflectionProbeTextures)
                textureArraysOk = false;
            else
                world->reflectionProbeTextures[probe].basemap =
                    world->reflectionProbes[probe].reflectionImage
                        ? world->reflectionProbes[probe].reflectionImage->texture.basemap
                        : nullptr;
        }
        for (int map = 0; map < world->lightmapCount && textureArraysOk; ++map)
        {
            if (!world->lightmapPrimaryTextures || !world->lightmapSecondaryTextures)
                textureArraysOk = false;
            else
            {
                world->lightmapPrimaryTextures[map].basemap =
                    world->lightmaps[map].primary ? world->lightmaps[map].primary->texture.basemap
                                                   : nullptr;
                world->lightmapSecondaryTextures[map].basemap =
                    world->lightmaps[map].secondary ? world->lightmaps[map].secondary->texture.basemap
                                                     : nullptr;
            }
        }
        std::printf("KILLHOUSE_BOUNDED m8_texture_arrays probes=%u lightmaps=%d ok=%u\n",
                    world->reflectionProbeCount, world->lightmapCount, textureArraysOk ? 1u : 0u);
        if (!Check(textureArraysOk, "m8_world_texture_arrays"))
            return 1;
        // Gating: R_SortWorldSurfaces (r_drawsurf.cpp)
        // runs unconditionally from R_BeginFrame once a world is active --
        // the first live Com_Frame after activation faulted inside its
        // memset because these arrays were still deferred. Exercise exactly
        // its write pattern (plain CPU writes, no device needed) to prove
        // the arrays are real and correctly sized, not just present.
        bool sortArraysOk = true;
        const uint32_t sortCount =
            world->models ? (uint32_t)world->models->surfaceCount : 0;
        if (sortCount)
        {
            if (!world->dpvs.surfaceMaterials || !world->dpvs.surfaceCastsSunShadow)
                sortArraysOk = false;
            else
            {
                std::memset(world->dpvs.surfaceCastsSunShadow, 0,
                            4 * ((sortCount - 1) >> 5) + 4);
                for (uint32_t surf = 0; surf < sortCount && sortArraysOk; ++surf)
                {
                    const Material *surfMat = world->dpvs.surfaces
                                                  ? world->dpvs.surfaces[surf].material
                                                  : nullptr;
                    if (!surfMat)
                        sortArraysOk = false;
                    else
                        world->dpvs.surfaceMaterials[surf] = surfMat->info.drawSurf;
                }
            }
        }
        if (world->primaryLightCount && !world->shadowGeom)
            sortArraysOk = false;
        else if (world->shadowGeom)
        {
            for (uint32_t light = 0; light < world->primaryLightCount; ++light)
                world->shadowGeom[light].surfaceCount = 0;
        }
        // Host mirror of the R_SetPrimaryLightShadowSurfaces gate
        // (r_drawsurf.cpp): count the customIndex surfaces the live gate
        // will skip in first-BSP-frame mode, so the live "skipped %u" line
        // has a deterministic expected value to check against.
        uint32_t shadowSkipped = 0;
        for (uint32_t surf = 0; surf < sortCount; ++surf)
        {
            if (world->dpvs.surfaceMaterials[surf].fields.customIndex != 0)
                ++shadowSkipped;
        }
        std::printf("KILLHOUSE_BOUNDED m8_sort_arrays surfaces=%u geoms=%u shadow_skipped=%u ok=%u\n",
                    sortCount, world->primaryLightCount, shadowSkipped,
                    sortArraysOk ? 1u : 0u);
        if (!Check(sortArraysOk, "m8_world_sort_arrays"))
            return 1;
        // GPU audit (a): census over the real killhouse draw-inst models
        // widened above (cached vs rigid, index ranges, cache-slot fit).
        // Turns a GPU-side VK_ERROR_DEVICE_LOST into a loud host FAIL with
        // model/lod/surf identity instead of blind on-device iteration.
        if (!Check(RetailWorldAuditStaticModelsForGpu(world), "m14_static_audit"))
            return 1;
        // host trace: the runtime model-lighting consumer indexes
        // world->lightGrid.colors for every static model whose lighting it
        // computes (R_CalcModelLighting -> R_GetLightingAtPoint ->
        // RB_PatchModelLighting's LockBox upload). Prove the decoded CPU
        // arrays for real killhouse are present and carry non-zero light
        // values, and that a model from the reported-underlit family
        // (shelf/locker/crate) is in the draw-instance list; the guest
        // verifier then measures the values that actually reach the volume
        // texture for that model in the production walk.
        {
            const GfxLightGrid &grid = world->lightGrid;
            uint32_t gridNonzeroColors = 0;
            uint32_t gridMaxChannel = 0;
            uint32_t gridDarkColors = 0;
            uint32_t gridMidColors = 0;
            uint32_t gridBrightColors = 0;
            uint64_t gridMaxChannelSum = 0;
            for (uint32_t c = 0; c < grid.colorCount && grid.colors; ++c)
            {
                const GfxLightGridColors &colors = grid.colors[c];
                bool nonzero = false;
                uint8_t colorMax = 0;
                for (uint32_t sample = 0; sample < 56; ++sample)
                {
                    for (uint32_t channel = 0; channel < 3; ++channel)
                    {
                        const uint8_t value = colors.rgb[sample][channel];
                        if (value)
                            nonzero = true;
                        if (value > colorMax)
                            colorMax = value;
                    }
                }
                if (nonzero)
                    ++gridNonzeroColors;
                if (colorMax > gridMaxChannel)
                    gridMaxChannel = colorMax;
                gridMaxChannelSum += colorMax;
                if (colorMax < 0x40)
                    ++gridDarkColors;
                else if (colorMax < 0x80)
                    ++gridMidColors;
                else
                    ++gridBrightColors;
            }
            uint32_t namedModels = 0;
            uint32_t namedRuntimeHandles = 0;
            // Probe: print every named prop (not just the first four), and
            // next to its wire-decoded primaryLightIndex the light-region
            // assignment the loadobj path's R_LoadMiscModel performs
            // (R_GetPrimaryLightForModel + the sun-grid fallback).  A prop
            // whose wire primary is 0 but whose light-region hull assignment is
            // a real non-sun light is the candidate for the missing call the
            // fastfile path never makes.
            for (uint32_t i = 0; i < world->dpvs.smodelCount && namedModels < 64 &&
                                world->dpvs.smodelDrawInsts; ++i)
            {
                const GfxStaticModelDrawInst &inst = world->dpvs.smodelDrawInsts[i];
                const XModel *model = inst.model;
                if (!model || !model->name)
                    continue;
                if (!std::strstr(model->name, "shelf") &&
                    !std::strstr(model->name, "locker") &&
                    !std::strstr(model->name, "crate"))
                    continue;
                ++namedModels;
                if (!inst.lightingHandle)
                    ++namedRuntimeHandles;
                const GfxStaticModelInst *instLight =
                    world->dpvs.smodelInsts ? &world->dpvs.smodelInsts[i] : nullptr;
                // The runtime lighting origin R_SetStaticModelLighting uses is
                // the average of the serialized instance bounds, so print it
                // with the grid cell it quantizes to (R_LightGridLookup's own
                // pos math) -- a wrong bounds decode shows up here first.
                float lightOrg[3] = {0, 0, 0};
                uint32_t lightPos[3] = {0, 0, 0};
                if (instLight)
                {
                    for (uint32_t axis = 0; axis < 3; ++axis)
                        lightOrg[axis] = 0.5f * (instLight->mins[axis] + instLight->maxs[axis]);
                    lightPos[0] = ((int)std::floor(lightOrg[0]) + 0x20000) >> 5;
                    lightPos[1] = ((int)std::floor(lightOrg[1]) + 0x20000) >> 5;
                    lightPos[2] = ((int)std::floor(lightOrg[2]) + 0x20000) >> 6;
                }
                uint32_t regionPrimary = 0;
                uint32_t regionHullCount = 0;
                char regionHits[192];
                regionHits[0] = 0;
                if (instLight && world->lightRegion && world->primaryLightCount)
                {
                    for (uint32_t light = 0; light < world->primaryLightCount; ++light)
                    {
                        regionHullCount += world->lightRegion[light].hullCount;
                        if (!comWorld.isInUse || !comWorld.primaryLights)
                            continue;
                        const float *lightOrigin = comWorld.primaryLights[light].origin;
                        const float rel[3] = {inst.placement.origin[0] - lightOrigin[0],
                                              inst.placement.origin[1] - lightOrigin[1],
                                              inst.placement.origin[2] - lightOrigin[2]};
                        for (uint32_t hull = 0; hull < world->lightRegion[light].hullCount;
                             ++hull)
                        {
                            if (!RetailRegionHullContains(
                                    world->lightRegion[light].hulls[hull], rel))
                                continue;
                            if (!regionPrimary)
                                regionPrimary = light;
                            if (std::strlen(regionHits) < sizeof(regionHits) - 12)
                                std::snprintf(regionHits + std::strlen(regionHits),
                                              sizeof(regionHits) - std::strlen(regionHits),
                                              "%s%u:%u", regionHits[0] ? "," : "", light, hull);
                        }
                    }
                }
                std::printf("KILLHOUSE_MODELLIGHT_MODEL name='%s' smodel=%u wire_handle=%u "
                            "primary=%u region_primary=%u region_hulls=%u region_hits=%s "
                            "ground=%08x "
                            "org=(%.1f,%.1f,%.1f) "
                            "mins=(%.1f,%.1f,%.1f) maxs=(%.1f,%.1f,%.1f) "
                            "light_org=(%.1f,%.1f,%.1f) pos=(%u,%u,%u)\n",
                            model->name, i, (unsigned)inst.lightingHandle,
                            (unsigned)inst.primaryLightIndex,
                            regionPrimary, regionHullCount, regionHits,
                            instLight ? instLight->groundLighting.packed : 0u,
                            inst.placement.origin[0], inst.placement.origin[1],
                            inst.placement.origin[2],
                            instLight ? instLight->mins[0] : 0.0f,
                            instLight ? instLight->mins[1] : 0.0f,
                            instLight ? instLight->mins[2] : 0.0f,
                            instLight ? instLight->maxs[0] : 0.0f,
                            instLight ? instLight->maxs[1] : 0.0f,
                            instLight ? instLight->maxs[2] : 0.0f,
                            lightOrg[0], lightOrg[1], lightOrg[2],
                            lightPos[0], lightPos[1], lightPos[2]);
            }
            const uint32_t gridMeanChannel = grid.colorCount
                ? (uint32_t)(gridMaxChannelSum / grid.colorCount) : 0u;
            // Which light-grid *entries* the model lighting consumer samples:
            // bucket every entry's colorsIndex by the brightness of the color
            // it points at, and count entries on the dark color the guest's
            // named shelf rows resolve to.
            uint32_t entryDark = 0, entryMid = 0, entryBright = 0, entryColor322 = 0;
            for (uint32_t e = 0; e < grid.entryCount && grid.entries && grid.colors; ++e)
            {
                const uint16_t colorsIndex = grid.entries[e].colorsIndex;
                if (colorsIndex == 322)
                    ++entryColor322;
                if (colorsIndex >= grid.colorCount)
                    continue;
                uint8_t colorMax = 0;
                const GfxLightGridColors &colors = grid.colors[colorsIndex];
                for (uint32_t sample = 0; sample < 56; ++sample)
                {
                    for (uint32_t channel = 0; channel < 3; ++channel)
                    {
                        if (colors.rgb[sample][channel] > colorMax)
                            colorMax = colors.rgb[sample][channel];
                    }
                }
                if (colorMax < 0x40)
                    ++entryDark;
                else if (colorMax < 0x80)
                    ++entryMid;
                else
                    ++entryBright;
            }
            std::printf("KILLHOUSE_MODELLIGHT_ENTRIES entries=%u dark=%u mid=%u bright=%u "
                        "color322=%u\n",
                        grid.entryCount, entryDark, entryMid, entryBright, entryColor322);
            std::printf("KILLHOUSE_MODELLIGHT_GRID colors=%u nonzero_colors=%u max_channel=%u "
                        "mean_max_channel=%u dark=%u mid=%u bright=%u entries=%u raw=%u "
                        "named=%u runtime_handles=%u\n",
                        grid.colorCount, gridNonzeroColors, gridMaxChannel, gridMeanChannel,
                        gridDarkColors, gridMidColors, gridBrightColors, grid.entryCount,
                        grid.rawRowDataSize, namedModels, namedRuntimeHandles);
            if (!Check(grid.colors && grid.colorCount > 1 && gridNonzeroColors > 0 &&
                           gridMaxChannel > 0 && grid.entries && grid.entryCount > 0 &&
                           grid.rawRowData && grid.rawRowDataSize > 0 && namedModels > 0 &&
                           namedRuntimeHandles > 0,
                       "p6b5_light_grid_values"))
                return 1;
        }
        // host oracle: for every named shelf/locker/crate
        // instance, resolve the cell its world-space lighting origin
        // quantizes to, and the entry/colours/primary that cell's own row
        // and RLE bytes carry, using only the grid's mins/maxs/rowAxis/
        // colAxis and row data (RetailLightGridCellForOrigin above), so a
        // run of the real R_LightGridLookup on the same origin can be
        // compared by entry index and coloursIndex.  `sun=` prints the
        // grid/world primary-light facts.  Host-side only.
        {
            const GfxLightGrid &grid = world->lightGrid;
            uint32_t namedRows = 0;
            uint32_t resolved = 0;
            uint32_t lastEntry = 0;
            uint32_t lastColors = 0;
            uint32_t lastPrimary = 0;
            for (uint32_t i = 0; i < world->dpvs.smodelCount && namedRows < 32 &&
                                world->dpvs.smodelDrawInsts && world->dpvs.smodelInsts;
                 ++i)
            {
                const GfxStaticModelDrawInst &inst = world->dpvs.smodelDrawInsts[i];
                const XModel *model = inst.model;
                if (!model || !model->name)
                    continue;
                if (!std::strstr(model->name, "shelf") && !std::strstr(model->name, "locker") &&
                    !std::strstr(model->name, "crate"))
                    continue;
                const GfxStaticModelInst &instLight = world->dpvs.smodelInsts[i];
                float lightOrg[3];
                for (uint32_t axis = 0; axis < 3; ++axis)
                    lightOrg[axis] = 0.5f * (instLight.mins[axis] + instLight.maxs[axis]);
                RetailLightGridCell cell;
                RetailLightGridCellForOrigin(grid, lightOrg, cell);
                RetailLightGridCell quad[8];
                RetailLightGridQuadForOrigin(grid, lightOrg, quad);
                ++namedRows;
                if (cell.entryPresent)
                {
                    ++resolved;
                    lastEntry = cell.entryIndex;
                    lastColors = cell.entry.colorsIndex;
                    lastPrimary = cell.entry.primaryLightIndex;
                }
                char quadText[8 * 32];
                quadText[0] = '\0';
                for (uint32_t corner = 0; corner < 8; ++corner)
                {
                    char one[48];
                    const RetailLightGridCell &q = quad[corner];
                    std::snprintf(one, sizeof(one), "%s%u/%u/%u/%u/%u", corner ? "," : "",
                                  q.entryPresent ? q.entryIndex : 0xFFFFFFFFu,
                                  q.entryPresent ? q.entry.colorsIndex : 0u,
                                  q.entryPresent ? q.entry.primaryLightIndex : 0u,
                                  q.entryPresent ? q.entry.needsTrace : 0u, q.reason);
                    std::strncat(quadText, one, sizeof(quadText) - std::strlen(quadText) - 1);
                }
                std::printf(
                    "KILLHOUSE_LIGHTGRID_HOST name='%s' smodel=%u entry0=%s entry=%u colors=%u "
                    "primary=%u needs_trace=%u reason=%u inside=%u row=%u cell=(%u,%u,%u) "
                    "colIndex=%u z=%u colStart=%u colCount=%u zStart=%u zCount=%u first=%u "
                    "row_count=%u sun=%u world_sun=%u prim=%u quad=%s\n",
                    model->name, i, cell.entryPresent ? "present" : "none",
                    cell.entryPresent ? cell.entryIndex : 0xFFFFFFFFu,
                    cell.entryPresent ? cell.entry.colorsIndex : 0u,
                    cell.entryPresent ? cell.entry.primaryLightIndex : 0u,
                    cell.entryPresent ? cell.entry.needsTrace : 0u, cell.reason,
                    cell.insideGrid ? 1u : 0u, cell.rowIndex, cell.pos[0], cell.pos[1], cell.pos[2],
                    cell.colIndex, cell.z, cell.colStart, cell.colCount, cell.zStart,
                    cell.zCount, cell.firstEntry,
                    grid.maxs[grid.rowAxis] + 1u - grid.mins[grid.rowAxis],
                    grid.sunPrimaryLightIndex, world->sunPrimaryLightIndex,
                    world->primaryLightCount, quadText);
            }
            std::printf("KILLHOUSE_LIGHTGRID_HOST_SUM named=%u resolved=%u last_entry=%u "
                        "last_colors=%u last_primary=%u raw=%u entries=%u colors=%u\n",
                        namedRows, resolved, lastEntry, lastColors, lastPrimary,
                        grid.rawRowDataSize, grid.entryCount, grid.colorCount);
            if (!Check(namedRows > 0 && resolved > 0, "p6b10_lightgrid_cell_resolves"))
                return 1;
        }
        // BSP-composition census: distinct materials bound to the real
        // BSP surfaces above. The live BSP ledger sees mc/mtl_* materials
        // next to wc/* ones; this decides on host whether those are genuine
        // brush surfaces sharing model materials or misrouted static
        // geometry reaching the BSP funnel. Pointer identity matches the
        // ledger's own dedup exactly. Covers [0, surfaceCount): the live
        // renderer (R_DrawTrianglesLit) indexes dpvs.surfaces by surfaceCount,
        // not staticSurfaceCount, so truncating here would hide a tail.
        {
            // Index what the renderer indexes: [0, surfaceCount), the exact
            // bound R_DrawTrianglesLit asserts against (the widened array is
            // sized by surfaceCount, db_retail_decode_world.cpp).
            const uint32_t bspSurfCount = static_cast<uint32_t>(world->surfaceCount);
            std::printf("KILLHOUSE_BSP_MATS staticCount=%u surfaceCount=%u models=%s modelSurfs=%u\n",
                        world->dpvs.staticSurfaceCount, world->surfaceCount,
                        world->models ? "yes" : "no",
                        world->models ? (uint32_t)world->models->surfaceCount : 0u);
            const Material *seen[512]{};
            uint32_t seenCount = 0;
            uint32_t overflow = 0;
            uint32_t nullMat = 0;
            bool ok = world->dpvs.surfaces != nullptr;
            for (uint32_t s = 0; ok && s < bspSurfCount; ++s)
            {
                const Material *mat = world->dpvs.surfaces[s].material;
                if (!mat)
                {
                    ++nullMat;
                    continue;
                }
                bool known = false;
                for (uint32_t k = 0; k < seenCount; ++k)
                {
                    if (seen[k] == mat)
                    {
                        known = true;
                        break;
                    }
                }
                if (known)
                    continue;
                if (seenCount < 512)
                    seen[seenCount++] = mat;
                else
                    ++overflow;
            }
            std::printf("KILLHOUSE_BSP_MATS surfs=%u distinct=%u overflow=%u null=%u\n",
                        bspSurfCount, seenCount, overflow, nullMat);
            for (uint32_t k = 0; k < seenCount; ++k)
            {
                std::printf("KILLHOUSE_BSP_MAT name='%s'\n",
                            seen[k]->info.name ? seen[k]->info.name : "(null)");
            }
            if (!Check(ok && overflow == 0 && nullMat == 0, "m14_bsp_mats"))
                return 1;
        }
        if (StubRegisteredCount() > registeredHigh)
            registeredHigh = StubRegisteredCount();
        std::printf("KILLHOUSE_BOUNDED water zoneHigh=%u registeredHigh=%u\n",
                    StubZoneHighWater(), registeredHigh);
    }
    // retained-pointer generalization: the accumulated registration-time name
    // provenance over every asset registered by this whole run (fixtures +
    // the bounded four-zone boot loads). `named` counts registrations the
    // production name getter keys by name, `types` how many distinct asset
    // types carried one, and `readerOwned` must be zero, i.e. no registered
    // asset retained a pointer into the reader's transient storage (the
    // pre-fix retained-pointer shape). The lifetime verifier consumes this line and
    // re-derives its own PASS.
    uint32_t namedTypes = 0;
    for (uint64_t mask = StubNamedTypeMask(); mask; mask &= mask - 1)
        ++namedTypes;
    std::printf("KILLHOUSE_LIFETIME provenance named=%u types=%u readerOwned=%u\n",
                StubNamedRegistrationCount(), namedTypes, StubReaderOwnedNameCount());
    // pointer-provenance generalization: the same run's close-time
    // arena sweep. `zones`/`words` are the real scan volume (live zone arenas
    // at every reader close), `readerOwned` counts any raw pointer field found
    // inside the open readers' transient storage -- the retained-pointer shape generalized
    // from names to every pointer field of all 33 asset types. Must be zero;
    // the negative control above proves the scan detects a planted pointer.
    std::printf("KILLHOUSE_LIFETIME provenance_body zones=%u ranges=%u/%u words=%llu "
                "readerOwned=%u overflow=%u\n",
                StubArenaScanZones(), StubProofHeapScanRanges(),
                StubProofHeapRangeCount(),
                static_cast<unsigned long long>(StubArenaScanWords()),
                StubReaderOwnedBodyCount(), StubProofHeapOverflow());

    // A deferred material alias must be owned by
    // the zone whose arena holds its slot, so DB_RetailZoneEnd drops it before
    // that arena is released. Placed after the process-wide high-water lines so
    // its extra fixture zone cannot perturb their pinned counts.
    {
        RetailWalkLoadZoneResult ownerZone{};
        if (!LoadFixtureZone("zone/english/material_live.ff", &ownerZone, "matzone_load"))
            return 1;
        Material owner{};
        owner.info.name = const_cast<char *>("$victorybackdrop");
        GfxImage ownerImage{};
        ownerImage.texture.loadDef = reinterpret_cast<GfxImageLoadDef *>(1);
        MaterialTextureDef ownerDef{};
        ownerDef.u.image = &ownerImage;
        owner.textureTable = &ownerDef;
        XAssetHeader ownerHeader{};
        ownerHeader.material = &owner;
        StubRegisterStockAsset(ASSET_TYPE_MATERIAL, ownerHeader);

        const uint32_t before = RetailMaterialDeferredAliasCount();
        Material *slot = nullptr;
        const bool purged =
            RetailDeferMaterialAlias(&slot, ",$victorybackdrop", ownerZone.zoneIndex) &&
            RetailMaterialDeferredAliasCount() == before + 1 &&
            DB_RetailZoneEnd(ownerZone.zoneIndex) &&
            RetailMaterialDeferredAliasCount() == before;
        // The owner body is registered, so a resolve without the purge would
        // patch the slot; it must stay untouched.
        const bool notPatched = RetailResolveDeferredMaterialAliases(false) == 0 && slot == nullptr;
        const bool ownerRemoved = StubUnregisterStockAsset(ASSET_TYPE_MATERIAL, ownerHeader);
        if (!Check(purged, "matzone_purged") || !Check(notPatched, "matzone_not_patched") ||
            !Check(ownerRemoved, "matzone_owner_cleanup"))
            return 1;
        std::printf("KILLHOUSE_GRAPHICS material_zone_purge owned=1 purged=1 stale_write=0\n");
    }

    // The empty-technique fallback now needs the
    // block-4 write high-water-mark proof (provably unwritten filler). The
    // negative side is the repointed techset_dangling_live.ff in the
    // graphics-loader block.
    {
        RetailWalkLoadZoneResult fillerResult{};
        if (!LoadFixtureZone("zone/english/techset_filler_live.ff", &fillerResult,
                             "techcache_filler_load"))
            return 1;
        const XAssetHeader filler = StubFindXAssetHeader(ASSET_TYPE_TECHNIQUE_SET, "ts/filler");
        const bool fillerOk = fillerResult.registeredTechniqueSetCount == 1 &&
            filler.techniqueSet != nullptr && filler.techniqueSet->techniques[0] == nullptr &&
            filler.techniqueSet->techniques[1] != nullptr &&
            filler.techniqueSet->techniques[1]->passCount == 0 &&
            !std::strcmp(filler.techniqueSet->techniques[1]->name, "$empty");
        const bool fillerUnload = UnloadFixtureZone(fillerResult) &&
            StubFindXAssetHeader(ASSET_TYPE_TECHNIQUE_SET, "ts/filler").data == nullptr;
        if (!Check(fillerOk, "techcache_filler_empty") ||
            !Check(fillerUnload, "techcache_filler_unload"))
            return 1;
        std::printf("KILLHOUSE_GRAPHICS techcache_filler unwritten_offset=empty\n");
    }
    return 0;
}
