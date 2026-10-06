// Host-test-only stand-ins for DB_RetailZoneBegin/DB_RetailZoneRegister/
// DB_RetailZoneEnd, the sole bridge from db_retail_zone.cpp into the real,
// whole-engine db_registry.cpp (which pulls in gfx_d3d/xanim/game/cgame and
// cannot reasonably be linked into a narrow ASan host test).  This exists so
// a host test can exercise the real, unmodified db_retail_zone.cpp,
// db_retail_walk.cpp, and db_retail_decode_*.cpp production code against a
// real FS-streamed fastfile, proving the stream-then-rewind live-decode
// protocol, without needing the real zone/asset registry underneath it —
// that registry behavior (override, unload, DB_AddXAsset identity) is
// already covered separately by F2's real DB_RetailZoneBegin/Register/End
// zone-lifetime proof on the canonical bootstrap.  Never built into the
// Switch target: db_registry.cpp provides the real implementations there.
#include <database/database.h>
#include <database/db_retail_zone.h>
#include <database/db_retail_decode_font.h>
#include <gfx_d3d/fxprimitives.h>
#include <database/db_retail_decode_image.h>
#include <database/db_retail_decode_material.h>
#include <database/db_retail_decode_menulist.h>
#include <database/db_retail_decode_weapon.h>
#include <database/db_retail_decode_rawfile.h>
#include <database/db_retail_decode_small.h>
#include <database/db_retail_decode_stringtable.h>
#include <script/scr_stringlist.h>
#include <universal/com_files.h>
#include <gfx_d3d/r_bsp.h>
#include <qcommon/com_bsp.h>
#include <game/g_bsp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

// Defined by switch_retail_fs_stubs.cpp (linked into the same host binaries).
void Com_Printf(int, const char *fmt, ...);

// The production decoder installs this canonical engine singleton.  The
// host registry double intentionally uses the same object identity.
ComWorld comWorld;
// The GameWorldSp decoder publishes the engine's native path-data singleton
// (pathnode_t records, links, chains, vis, tree); the host double mirrors
// g_bsp.cpp's definition so the live decoder links without the game library.
GameWorldSp gameWorldSp;

namespace
{
// Production g_zones[] is ASSET_TYPE_COUNT (33); the host double only needs
// enough slots for the boot sequence plus the synthetic lifecycle zones, and
// 16 no longer covers the GameWorldSp live-load case.  This is a test-harness
// capacity, not a production cap.
constexpr uint32_t kMaxZones = 20;
struct StubZone
{
    bool live = false;
    XZoneMemory memory{};
    void *nativeArena = nullptr;
    uint32_t nativeArenaBytes = 0;
    // Resident identity for the reload-idempotency check (mirrors the real
    // g_zones[].name): the exact filename RetailWalkLoadZoneAssets began.
    char name[64]{};
    // Load policy recorded on success (mirrors db_registry.cpp's
    // g_retailZoneBounded): only a same-policy request may skip.
    bool boundedPolicy = false;
};
StubZone g_zones[kMaxZones];

// The registration cap matches the production entry-pool ceiling (32768), so
// the harness never fails before production would and no production
// capacity has to change. Smaller caps silently dropped later registrations
// (e.g. the killhouse GfxWorld) once XAnim and FX impact-table entries were
// registered.
constexpr uint32_t kMaxRegistered = 32768;
struct StubRegisteredAsset
{
    XAssetType type;
    // Canonical header: the FIRST registration of this (type, name) pair
    // wins, exactly like the real DB_AddXAsset/DB_LinkXAssetEntry
    // (db_registry.cpp), which chains a same-name re-registration on
    // nextOverride and keeps returning the existing entry.
    XAssetHeader header;
    // Owning zone slot of the canonical header. DB_RetailZoneEnd sweeps
    // every entry tagged with the ended zone so no lookup can return a
    // pointer into freed zone memory (the retired-pointer audit below
    // proves the sweep happened).
    uint32_t zoneIndex = 0;
    // Later-zone same-name re-registration (the real nextOverride chain,
    // single-level here): promoted to canonical if the canonical owner's
    // zone unloads first. Anonymous (nameless) bodies never collide.
    XAssetHeader overrideHeader{};
    uint32_t overrideZoneIndex = 0;
};
StubRegisteredAsset g_registered[kMaxRegistered];
uint32_t g_registeredCount = 0;
// Same-name re-registrations (override-chain events), whenever they occur.
// Models the real nextOverride accounting, not a replacement count: the
// canonical header never moves while its owner is live.
uint32_t g_overrideCount = 0;
// Ownerless comma stubs that could not model the original
// DB_CreateDefaultEntry clone and kept the transient widened body instead
// (see DB_RetailZoneRegister).  Must be zero for any family whose stubs all
// have earlier owners; the lifetime/boot evidence pins the expected count.
uint32_t g_stubCommaDefaultMissingCount = 0;
// Highest zone slot ever handed out (slots are 1-based; 0 is never used).
// Lets the lifetime proof report arena high-water use against the
// fixed kMaxZones capacity.
uint32_t g_zoneHighWater = 0;
// Zone begun most recently and not yet ended (see Load_XAnimPartsAsset).
uint32_t g_stubLoadingZone = 0;

// Host double for the real DB's on-demand default-entry contract
// (db_registry.cpp DB_CreateDefaultEntry, reached from DB_FindXAssetHeader):
// when a test installs the engine default material, a by-name material miss
// registers a stock (zone 0) entry for that name exactly like the Switch
// target, so the comma-stub path can prove it delegates to the
// original default entry instead of synthesizing a body.  With no sentinel
// installed the previous null-return contract is unchanged, so unrelated
// host proofs are unaffected.
Material *g_stubDefaultMaterial = nullptr;
struct StubDefaultEntry
{
    XAssetType type;
    char name[64];
};
StubDefaultEntry g_stubDefaultEntries[64];
uint32_t g_stubDefaultEntryCount = 0;

// Host mirror of db_registry.cpp's g_defaultAssetName for the types whose
// closure comma stubs have no earlier owner (checked against OAT's ordered
// declaration lists: only image's nine MP HUD stubs, e.g.
// `,hud_javelin_lock_box`).  This is the original DB_CreateDefaultEntry
// lookup key; the default body itself is always a normally registered asset
// (code_post_gfx's generated image `$white`), never a manufactured body.
// Types whose stubs always resolve to an earlier owner return nullptr here
// and keep the loud unresolved failure if that ever changes.
const char *StubDefaultAssetName(XAssetType type)
{
    switch (type)
    {
    case ASSET_TYPE_IMAGE:
        return "$white";
    case ASSET_TYPE_MATERIAL:
        return "$default";
    case ASSET_TYPE_MENU:
        return "default_menu";
    case ASSET_TYPE_XMODEL:
        return "void";
    case ASSET_TYPE_TECHNIQUE_SET:
        return "default";
    case ASSET_TYPE_PHYSPRESET:
        return "default";
    case ASSET_TYPE_LOADED_SOUND:
        return "null.wav";
    case ASSET_TYPE_SOUND_CURVE:
        return "default";
    case ASSET_TYPE_FX:
        return "misc/missing_fx";
    default:
        return nullptr;
    }
}

// The registry's name contract, mirroring db_assetnames.cpp's
// DB_XAssetGetNameHandler table entry for entry: production reads a distinct
// pointer for image/menulist/menu/localize, the COMDAT-folded first-field
// getter (StringTable::name, offset 0) for every other name-keyed type, and
// has no handler at all for ui_map/snddriverglobals/aitype/mptype/character/
// xmodelalias.  The provenance audit below dereferences exactly what
// production's DB_GetXAssetName would, so a reader-owned name pointer that
// dangles after FS_CloseRetailFastfile cannot hide behind a host-only
// accessor that happens to read different bytes.
const char *StubAssetName(XAssetType type, XAssetHeader header)
{
    if (!header.data)
        return nullptr;
    switch (type)
    {
    case ASSET_TYPE_IMAGE:
        return header.image->name;
    case ASSET_TYPE_MENULIST:
        return header.menuList->name;
    case ASSET_TYPE_MENU:
        return header.menu->window.name;
    case ASSET_TYPE_LOCALIZE_ENTRY:
        return header.localize->name;
    case ASSET_TYPE_UI_MAP:
    case ASSET_TYPE_SNDDRIVER_GLOBALS:
    case ASSET_TYPE_AITYPE:
    case ASSET_TYPE_MPTYPE:
    case ASSET_TYPE_CHARACTER:
    case ASSET_TYPE_XMODELALIAS:
        // Production's DB_XAssetGetNameHandler[type] is null here: the
        // registry never keys these by name, so there is nothing to audit.
        return nullptr;
    default:
        // Folded DB_StringTableGetName: reads the asset's first field.
        return *reinterpret_cast<const char *const *>(header.data);
    }
}

// retained-pointer generalization counters (see DB_RetailZoneRegister below).
// The mask records which of the 33 asset types actually carried a name
// through a real registration, so the final fact line proves the audit is
// not just covering one decoder.
uint32_t g_stubNamedRegistrationCount = 0;
uint32_t g_stubReaderOwnedNameCount = 0;
uint64_t g_stubNamedTypeMask = 0; // one bit per XAssetType (0..32)
// per-type census: one counter per XAssetType, incremented for every
// DB_RetailZoneRegister call that reached a live zone with a body. Counts
// declarations (not distinct names), so an intra-run same-name override still
// counts the original loader's registration call.
uint32_t g_stubRegisterAttempts[64] = {};
// pointer-provenance generalization: every raw pointer field in
// zone-owned arena storage, not just each registration's name, is audited
// against the open reader's transient blocks. `bodyCount` must end at zero;
// `scanZones`/`scanWords` prove the sweep actually read bodies and spans all
// the live zones the walk produced.
uint32_t g_stubReaderOwnedBodyCount = 0;
uint32_t g_stubArenaScanZones = 0;
uint64_t g_stubArenaScanWords = 0;
// Registered bodies are not all arena-resident: the narrow host doubles for
// Material_Alloc and Image_Alloc allocate process-lifetime host blocks that
// hold real widened Material/MaterialTextureDef/GfxStateBits/GfxImage bodies.
// Track them so the same close-time sweep covers every raw pointer field a
// registered asset can own, not only the widened-arena subset.
struct StubProofHeapRange
{
    const uint8_t *base;
    uint32_t size;
};
constexpr uint32_t kMaxStubProofHeapRanges = 16384;
StubProofHeapRange g_stubProofHeapRanges[kMaxStubProofHeapRanges];
uint32_t g_stubProofHeapRangeCount = 0;
uint32_t g_stubProofHeapRangeOverflow = 0;
uint32_t g_stubProofHeapScanRanges = 0;

void StubTrackProofHeapRange(void *pointer, uint32_t size)
{
    if (!pointer || !size)
        return;
    if (g_stubProofHeapRangeCount >= kMaxStubProofHeapRanges)
    {
        if (!g_stubProofHeapRangeOverflow)
            std::printf("KILLHOUSE_PROVENANCE heap_range_overflow count=%u size=%u\n",
                        g_stubProofHeapRangeCount, size);
        ++g_stubProofHeapRangeOverflow;
        return;
    }
    g_stubProofHeapRanges[g_stubProofHeapRangeCount++] = {
        static_cast<const uint8_t *>(pointer), size};
}
} // namespace

// Defined at the bottom of this file; DB_RetailZoneBegin installs it as the
// FS_CloseRetailFastfile transient audit hook.
void StubReaderTransientArenaAudit(const FsRetailFastfileReader *reader);

uint32_t StubNamedRegistrationCount(void)
{
    return g_stubNamedRegistrationCount;
}

uint64_t StubNamedTypeMask(void)
{
    return g_stubNamedTypeMask;
}

uint32_t StubReaderOwnedNameCount(void)
{
    return g_stubReaderOwnedNameCount;
}

void StubResetReaderOwnedNameCount(void)
{
    g_stubReaderOwnedNameCount = 0;
}

// Test-only lookup mirroring the real DB_FindXAssetHeader's by-name contract
// closely enough to prove RetailWalkLoadZoneAssets's registrations are
// reachable by name, without linking the real whole-engine asset registry.
XAssetHeader StubFindXAssetHeader(XAssetType type, const char *name)
{
    if (name)
    {
        for (uint32_t i = 0; i < g_stubDefaultEntryCount; ++i)
        {
            if (g_stubDefaultEntries[i].type == type &&
                std::strcmp(g_stubDefaultEntries[i].name, name) == 0)
            {
                XAssetHeader header{};
                header.material = g_stubDefaultMaterial;
                return header;
            }
        }
        for (uint32_t i = 0; i < g_registeredCount; ++i)
        {
            if (g_registered[i].type != type)
                continue;
            const char *candidate = StubAssetName(type, g_registered[i].header);
            if (candidate && std::strcmp(candidate, name) == 0)
                return g_registered[i].header;
        }
    }
    return XAssetHeader{};
}

// Forwards to the same lookup StubFindXAssetHeader exposes to tests, so
// db_retail_decode_material.cpp's real ResolveTechniqueSetRef (which calls
// the production DB_FindXAssetHeader symbol directly, exactly like
// switch_sp_bootstrap.cpp's proven code does) links and behaves correctly
// here without the real whole-engine registry.
XAssetHeader DB_FindXAssetHeader(XAssetType type, const char *name)
{
    XAssetHeader found = StubFindXAssetHeader(type, name);
    // Real-contract default-entry creation for the one case a host proof
    // needs it: a missing material while the test has installed the engine
    // default material (see g_stubDefaultMaterial).  Every other miss keeps
    // returning the empty header.
    if (!found.data && name && name[0] && g_stubDefaultMaterial &&
        type == ASSET_TYPE_MATERIAL)
    {
        if (g_stubDefaultEntryCount <
            sizeof(g_stubDefaultEntries) / sizeof(g_stubDefaultEntries[0]))
        {
            StubDefaultEntry &entry = g_stubDefaultEntries[g_stubDefaultEntryCount++];
            entry.type = type;
            std::strncpy(entry.name, name, sizeof(entry.name) - 1);
            entry.name[sizeof(entry.name) - 1] = '\0';
        }
        found.material = g_stubDefaultMaterial;
    }
    return found;
}

// The deferred-material resolver asks for a real owner without allowing the
// registry to synthesize a default; the host double never creates defaults,
// so both entry points share the same lookup.
XAssetHeader DB_FindXAssetHeaderNoDefault(XAssetType type, const char *name)
{
    return StubFindXAssetHeader(type, name);
}

bool DB_XAssetExists(XAssetType type, const char *name)
{
    return StubFindXAssetHeader(type, name).data != nullptr;
}

bool DB_IsXAssetDefault(XAssetType type, const char *name)
{
    if (!name)
        return false;
    for (uint32_t i = 0; i < g_stubDefaultEntryCount; ++i)
    {
        if (g_stubDefaultEntries[i].type == type &&
            std::strcmp(g_stubDefaultEntries[i].name, name) == 0)
            return true;
    }
    return false;
}

GfxImage *DB_RegisterImage(GfxImage *image)
{
    return image;
}

Material *DB_RegisterMaterial(Material *material)
{
    return material;
}

uint8_t *Material_Alloc(uint32_t size)
{
    void *block = std::calloc(1, size);
    // pointer-provenance: material bodies are host-heap here, not
    // arena-resident, so the close-time sweep must be told about them.
    StubTrackProofHeapRange(block, size);
    return static_cast<uint8_t *>(block);
}

void Load_BuildVertexDecl(MaterialVertexDeclaration **mtlVertDecl)
{
    (void)mtlVertDecl;
}

void Load_CreateMaterialVertexShader(GfxVertexShaderLoadDef *loadDef, MaterialVertexShader *mtlShader)
{
    (void)loadDef;
    (void)mtlShader;
}

void Load_CreateMaterialPixelShader(GfxPixelShaderLoadDef *loadDef, MaterialPixelShader *mtlShader)
{
    (void)loadDef;
    (void)mtlShader;
}

GfxImage *Image_FindExisting_LoadObj(const char *name)
{
    (void)name;
    return nullptr;
}

GfxImage *Image_Alloc(char *name, uint8_t category, uint8_t semantic, uint8_t imageTrack)
{
    (void)category;
    (void)semantic;
    (void)imageTrack;
    GfxImage *img = static_cast<GfxImage *>(std::calloc(1, sizeof(GfxImage)));
    StubTrackProofHeapRange(img, sizeof(GfxImage));
    if (img && name)
    {
        size_t len = std::strlen(name) + 1;
        char *copy = static_cast<char *>(std::calloc(1, len));
        StubTrackProofHeapRange(copy, static_cast<uint32_t>(len));
        if (copy)
        {
            std::memcpy(copy, name, len);
            img->name = copy;
        }
    }
    return img;
}

char Image_LoadFromFile(GfxImage *image)
{
    (void)image;
    return 1;
}

void Com_PrintError(int channel, const char *fmt, ...)
{
    (void)channel;
    (void)fmt;
}

bool DB_RetailZoneBegin(const char *name, int32_t flags, const uint32_t blockSizes[9],
                        uint32_t *nativeArenaBytesInOut, uint32_t *zoneIndex,
                        XZoneMemory **zoneMemory, void **nativeArenaMemory)
{
    (void)flags;
    if (!name || !name[0] || !blockSizes || !nativeArenaBytesInOut || !*nativeArenaBytesInOut ||
        !zoneIndex || !zoneMemory || !nativeArenaMemory)
        return false;
    const uint32_t nativeArenaBytes = *nativeArenaBytesInOut;
    for (uint32_t i = 1; i < kMaxZones; ++i)
    {
        if (g_zones[i].live)
            continue;
        StubZone &zone = g_zones[i];
        zone = StubZone{};
        std::strncpy(zone.name, name, sizeof(zone.name) - 1);
        zone.name[sizeof(zone.name) - 1] = '\0';
        zone.boundedPolicy = false;
        for (uint32_t block = 0; block < 9; ++block)
        {
            if (!blockSizes[block])
                continue;
            zone.memory.blocks[block].data =
                static_cast<uint8_t *>(std::calloc(1, blockSizes[block]));
            zone.memory.blocks[block].size = blockSizes[block];
            if (!zone.memory.blocks[block].data)
                return false;
        }
        zone.nativeArena = std::calloc(1, nativeArenaBytes);
        if (!zone.nativeArena)
            return false;
        zone.nativeArenaBytes = nativeArenaBytes;
        zone.live = true;
        *zoneIndex = i;
        // pointer-provenance generalization: from the first live zone
        // on, every reader close audits all live zone arenas while the
        // closing reader's blocks are still addressable.
        FS_RetailFastfileSetTransientAuditHook(&StubReaderTransientArenaAudit);
        if (i > g_zoneHighWater)
            g_zoneHighWater = i;
        *zoneMemory = &zone.memory;
        *nativeArenaMemory = zone.nativeArena;
        g_stubLoadingZone = i;
        return true;
    }
    return false;
}

// Host stand-in for the arena trim: the calloc'd arena stays allocated, but
// the zone's recorded arena size shrinks to what the load kept, so the
// provenance scan and any later allocation see the trimmed bound.
bool DB_RetailZoneTrimNativeArena(uint32_t zoneIndex, void *nativeArenaMemory,
                                  uint32_t reservedBytes, uint32_t keepBytes)
{
    if (!zoneIndex || zoneIndex >= kMaxZones || !g_zones[zoneIndex].live ||
        g_zones[zoneIndex].nativeArena != nativeArenaMemory ||
        g_zones[zoneIndex].nativeArenaBytes != reservedBytes || keepBytes > reservedBytes)
        return false;
    g_zones[zoneIndex].nativeArenaBytes = keepBytes;
    return true;
}

// Host stand-ins for db_registry.cpp's reload-idempotency pair (see
// database.h): same name+policy contract against the stub slots.  End
// needs no change: it already resets the whole StubZone, clearing name and
// policy exactly like the real retire paths.
uint32_t DB_RetailZoneFindLive(const char *name, bool bounded)
{
    if (!name || !name[0])
        return 0;
    for (uint32_t i = 1; i < kMaxZones; ++i)
    {
        if (g_zones[i].live && std::strncmp(g_zones[i].name, name, sizeof(g_zones[i].name)) == 0 &&
            g_zones[i].boundedPolicy == bounded)
            return i;
    }
    return 0;
}

void DB_RetailZoneNoteLoadPolicy(uint32_t zoneIndex, bool bounded)
{
    if (zoneIndex < kMaxZones)
        g_zones[zoneIndex].boundedPolicy = bounded;
}

// Host stand-in for db_registry.cpp's real geometry publish. The stub zones
// here have no D3D device and therefore no locked vertex/index buffers
// (StubZone::memory is value-initialized), so the real function's own guards
// would make it a no-op anyway; this keeps the host link free of the
// renderer.
void DB_RetailZoneUploadGeometryBuffers(uint32_t zoneIndex)
{
    (void)zoneIndex;
}

// Host stand-in for db_registry.cpp's bounds-guard helper. Reports the stub
// zone's own block-7/8 sizes so the audit path links without the renderer.
bool DB_GetZoneGeometrySizes(uint8_t zoneHandle, uint32_t *vertBytes, uint32_t *indexBytes)
{
    if (zoneHandle >= kMaxZones || !g_zones[zoneHandle].live)
        return false;
    if (vertBytes)
        *vertBytes = g_zones[zoneHandle].memory.blocks[7].size;
    if (indexBytes)
        *indexBytes = g_zones[zoneHandle].memory.blocks[8].size;
    return true;
}

// sweep #2 (OAT differential decode): the registry double is handed
// every freshly decoded body before the override chain can replace it, so a
// host run can observe the decoder's actual output for every registration.
// Lines are emitted only when KISAK_OAT_FIELD_TRACE is set (ordinary runs
// and ./test host output are unchanged); the independent verifier diffs them
// field by field against OAT's own dump of the same zone. FNV-1a 64 over the
// payload so the verifier can compare bytes exactly without a hash library.
uint64_t StubOatFnv1a64(const uint8_t *data, uint32_t len)
{
    uint64_t hash = 0xcbf29ce484222325ull; // FNV offset basis
    for (uint32_t i = 0; i < len; ++i)
    {
        hash ^= data[i];
        hash *= 0x100000001b3ull; // FNV prime
    }
    return hash;
}

// sweep #2 (OAT differential decode), IW3 material family: the whole
// decoded Material body is the payload the renderer consumes, so a name-only
// check cannot see a wrong field offset, a mis-sized table, or a dropped
// reference.  The port's own decoded struct is printed field by field in the
// exact terms the IW3 reference structures define (IW3_Assets.h), and the
// independent verifier rebuilds every one of them from OAT's material JSON
// dump of the same zone.  State bits stay raw 32-bit words here: the port's
// decoder copies the wire bytes, so the oracle has to re-encode OAT's decoded
// structured fields back into the retail bit layout before comparing.
void StubOatMaterialTrace(uint32_t zoneIndex, const Material *material)
{
    if (!material)
        return;
    // Per-registration sequence: a body can legitimately be registered twice
    // (override chain), and the verifier has to be able to tell a duplicate
    // registration whose fields agree from a second decode that disagrees.
    static uint32_t materialSeq = 0;
    const uint32_t seq = ++materialSeq;
    const char *name = material->info.name ? material->info.name : "<null>";
    char entry[69];
    for (uint32_t i = 0; i < sizeof(material->stateBitsEntry); ++i)
        std::snprintf(entry + i * 2, sizeof(entry) - i * 2, "%02x",
                      static_cast<unsigned>(material->stateBitsEntry[i]));
    std::printf("OAT_MAT zone=%u seq=%u name='%s' gf=%u sort=%u rows=%u cols=%u "
                "stb=%u hash=%u sflags=%u cam=%u tech='%s' entry=%s "
                "texn=%u conn=%u sbn=%u\n",
                zoneIndex, seq, name, static_cast<unsigned>(material->info.gameFlags),
                static_cast<unsigned>(material->info.sortKey),
                static_cast<unsigned>(material->info.textureAtlasRowCount),
                static_cast<unsigned>(material->info.textureAtlasColumnCount),
                material->info.surfaceTypeBits, static_cast<unsigned>(material->info.hashIndex),
                static_cast<unsigned>(material->stateFlags),
                static_cast<unsigned>(material->cameraRegion),
                material->techniqueSet && material->techniqueSet->name
                    ? material->techniqueSet->name : "<null>",
                entry, static_cast<unsigned>(material->textureCount),
                static_cast<unsigned>(material->constantCount),
                static_cast<unsigned>(material->stateBitsCount));
    for (uint32_t t = 0; t < material->textureCount && material->textureTable; ++t)
    {
        const MaterialTextureDef &def = material->textureTable[t];
        const GfxImage *image = MaterialTextureImage(def);
        std::printf("OAT_MAT_TEX zone=%u seq=%u name='%s' i=%u nh=%08x ns=%d ne=%d samp=%02x "
                    "sem=%u img='%s'\n",
                    zoneIndex, seq, name, t, def.nameHash, static_cast<int>(def.nameStart),
                    static_cast<int>(def.nameEnd), static_cast<unsigned>(def.samplerState),
                    static_cast<unsigned>(def.semantic),
                    image && image->name ? image->name : "<null>");
    }
    for (uint32_t c = 0; c < material->constantCount && material->constantTable; ++c)
    {
        const MaterialConstantDef &def = material->constantTable[c];
        std::printf("OAT_MAT_CON zone=%u seq=%u name='%s' i=%u nh=%08x nm=", zoneIndex, seq,
                    name, c, def.nameHash);
        for (uint32_t b = 0; b < sizeof(def.name); ++b)
            std::printf("%02x", static_cast<unsigned>(static_cast<unsigned char>(def.name[b])));
        std::printf(" l=");
        for (uint32_t v = 0; v < 4; ++v)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &def.literal[v], sizeof(bits));
            std::printf("%s%08x", v ? "_" : "", bits);
        }
        std::printf("\n");
    }
    for (uint32_t s = 0; s < material->stateBitsCount && material->stateBitsTable; ++s)
    {
        std::printf("OAT_MAT_SB zone=%u seq=%u name='%s' i=%u w0=%08x w1=%08x\n", zoneIndex,
                    seq, name, s, material->stateBitsTable[s].loadBits[0],
                    material->stateBitsTable[s].loadBits[1]);
    }
}

// sweep #2 (OAT differential decode), IW3 font family: the HUD/menu
// text path.  The port's Font_s and Glyph are exactly OAT's IW3 reference
// structs, so every field is comparable to OAT's font JSON except the
// fontName (the dump's own file path) -- pixelHeight, both material
// bindings, the glyph count, and every glyph metric/UV.
void StubOatFontTrace(uint32_t zoneIndex, const Font_s *font)
{
    if (!font)
        return;
    const char *name = font->fontName ? font->fontName : "<null>";
    std::printf("OAT_FONT zone=%u name='%s' pixelHeight=%d glyphCount=%d mat='%s' "
                "glow='%s'\n",
                zoneIndex, name, font->pixelHeight, font->glyphCount,
                font->material && font->material->info.name
                    ? font->material->info.name : "<null>",
                font->glowMaterial && font->glowMaterial->info.name
                    ? font->glowMaterial->info.name : "<null>");
    for (int32_t g = 0; g < font->glyphCount && font->glyphs; ++g)
    {
        const Glyph &glyph = font->glyphs[g];
        uint32_t s0 = 0, t0 = 0, s1 = 0, t1 = 0;
        std::memcpy(&s0, &glyph.s0, sizeof(s0));
        std::memcpy(&t0, &glyph.t0, sizeof(t0));
        std::memcpy(&s1, &glyph.s1, sizeof(s1));
        std::memcpy(&t1, &glyph.t1, sizeof(t1));
        std::printf("OAT_FONT_GLYPH zone=%u name='%s' i=%d letter=%u x0=%d y0=%d dx=%u "
                    "pw=%u ph=%u s0=%08x t0=%08x s1=%08x t1=%08x\n",
                    zoneIndex, name, g, static_cast<unsigned>(glyph.letter),
                    static_cast<int>(glyph.x0), static_cast<int>(glyph.y0),
                    static_cast<unsigned>(glyph.dx),
                    static_cast<unsigned>(glyph.pixelWidth),
                    static_cast<unsigned>(glyph.pixelHeight), s0, t0, s1, t1);
    }
}

// sweep #2 (OAT differential decode), IW3 image family: the texture
// body every rendered surface, HUD element and lightmap samples.  The port's
// widened GfxImage keeps the real loadDef (format/flags/dims/levelCount) and,
// for inline bodies, a zone-owned copy of the pixel payload, so both the
// header and the bytes are comparable to OAT's independent DDS dump of the
// same zone.  A wire body with resourceSize 0 (pixels live in the retail
// .iwd, loaded by Image_LoadFromFile at upload time) or no payload at all
// (generated water) must stay payload-free; the verifier classifies and pins
// that shape instead of accepting a synthesized copy.
void StubOatImageTrace(uint32_t zoneIndex, const GfxImage *image)
{
    if (!image)
        return;
    const GfxImageLoadDef *def = image->texture.loadDef;
    const bool present = def && def->resourceSize > 0;
    const uint32_t resourceSize = present ? static_cast<uint32_t>(def->resourceSize) : 0;
    const uint64_t pix = present
        ? StubOatFnv1a64(def->data, resourceSize)
        : 0;
    std::printf("OAT_IMG zone=%u name='%s' mapType=%d whd=%ux%ux%u cat=%u sem=%u "
                "delay=%u haveDef=%u lvl=%u flags=%u defdims=%dx%dx%d fmt=%d "
                "res=%u data=%u pix=%016llx\n",
                zoneIndex, image->name ? image->name : "<null>",
                static_cast<int>(image->mapType), image->width, image->height, image->depth,
                static_cast<unsigned>(image->category), static_cast<unsigned>(image->semantic),
                image->delayLoadPixels ? 1u : 0u, def ? 1u : 0u,
                def ? static_cast<unsigned>(def->levelCount) : 0u,
                def ? static_cast<unsigned>(def->flags) : 0u,
                def ? def->dimensions[0] : 0, def ? def->dimensions[1] : 0,
                def ? def->dimensions[2] : 0,
                def ? static_cast<int>(def->format) : 0,
                resourceSize, present ? 1u : 0u,
                static_cast<unsigned long long>(pix));
}

// sweep #2 (OAT differential decode), IW3 XAnimParts family: the
// animation bodies the player rig, viewmodel and scripted actors play.  The
// raw wire form is a flat six-array encoding whose per-bone track layout is
// not visible in any field the port copies, so a name-only (or counts-only)
// check cannot see a decoder that streams the wrong array, the wrong index
// width, or the wrong track shape.  The port prints the root scalars it
// widened, every bone-name script string, every notify (name, time and the
// frame the IW3 linker derives), and the raw six-array/index counts; the
// independent verifier parses OAT's compiled-XAnim v17 dump of the same zone
// with OAT's own CompiledXAnimLoader encoding rules and derives the same
// counts from the expanded per-bone tracks.  A registration can legitimately
// repeat through the override chain, so every line carries a per-registration
// seq like the material trace.
// One raw flat-array line per body: the six arrays plus the index pool the
// wire decoder copied, verbatim and byte-for-byte, so the verifier's own
// FlatXAnimReader port can expand them into per-bone tracks and diff every
// component against its CompiledXAnimLoader decode of OAT's dump.  The
// arrays are the actual native buffers the runtime will consume; emitting
// them here is the only way the values can be observed on the host without
// trusting the port's interpretation of them.
void StubOatXAnimHex(const void *data, uint32_t bytes)
{
    static const char nibbles[] = "0123456789abcdef";
    char buffer[4096];
    std::size_t used = 0;
    const unsigned char *p = static_cast<const unsigned char *>(data);
    for (uint32_t i = 0; i < bytes && p; ++i)
    {
        buffer[used++] = nibbles[p[i] >> 4];
        buffer[used++] = nibbles[p[i] & 0x0f];
        if (used + 2 > sizeof(buffer))
        {
            std::fwrite(buffer, 1, used, stdout);
            used = 0;
        }
    }
    std::fwrite(buffer, 1, used, stdout);
}

void StubOatXAnimRawArray(uint32_t zoneIndex, uint32_t seq, const char *tag,
                          const void *data, uint32_t elements, uint32_t width)
{
    const uint32_t bytes = elements * width;
    std::printf("OAT_XAN_RAW zone=%u seq=%u array=%s bytes=%u hex=", zoneIndex, seq, tag,
                bytes);
    StubOatXAnimHex(data, bytes);
    std::fputc('\n', stdout);
}

void StubOatXAnimTrace(uint32_t zoneIndex, const XAnimParts *parts)
{
    if (!parts)
        return;
    static uint32_t xanimSeq = 0;
    const uint32_t seq = ++xanimSeq;
    const char *name = parts->name ? parts->name : "<null>";
    uint32_t framerateBits = 0, frequencyBits = 0;
    std::memcpy(&framerateBits, &parts->framerate, sizeof(framerateBits));
    std::memcpy(&frequencyBits, &parts->frequency, sizeof(frequencyBits));
    const XAnimDeltaPart *delta = parts->deltaPart;
    const uint32_t deltaQuatPresent = delta && delta->quat ? 1u : 0u;
    const uint32_t deltaQuatSize = deltaQuatPresent ? delta->quat->size : 0u;
    const uint32_t deltaTransPresent = delta && delta->trans ? 1u : 0u;
    const uint32_t deltaTransSize = deltaTransPresent ? delta->trans->size : 0u;
    const uint32_t deltaTransSmall = deltaTransPresent ? delta->trans->smallTrans : 0u;
    std::printf("OAT_XAN zone=%u seq=%u name='%s' frames=%u loop=%u delta=%u assetType=%u "
                "framerate=%08x freq=%08x btotal=%u bcounts=%u,%u,%u,%u,%u,%u,%u,%u,%u "
                "notifyCount=%u isDefault=%u arrays=%u,%u,%u,%u,%u,%u,%u "
                "deltaq=%u,%u deltat=%u,%u,%u\n",
                zoneIndex, seq, name, static_cast<unsigned>(parts->numframes),
                parts->bLoop ? 1u : 0u, parts->bDelta ? 1u : 0u,
                static_cast<unsigned>(parts->assetType), framerateBits, frequencyBits,
                static_cast<unsigned>(parts->boneCount[9]),
                static_cast<unsigned>(parts->boneCount[0]),
                static_cast<unsigned>(parts->boneCount[1]),
                static_cast<unsigned>(parts->boneCount[2]),
                static_cast<unsigned>(parts->boneCount[3]),
                static_cast<unsigned>(parts->boneCount[4]),
                static_cast<unsigned>(parts->boneCount[5]),
                static_cast<unsigned>(parts->boneCount[6]),
                static_cast<unsigned>(parts->boneCount[7]),
                static_cast<unsigned>(parts->boneCount[8]),
                static_cast<unsigned>(parts->notifyCount), parts->isDefault ? 1u : 0u,
                static_cast<unsigned>(parts->dataByteCount),
                static_cast<unsigned>(parts->dataShortCount),
                static_cast<unsigned>(parts->dataIntCount),
                static_cast<unsigned>(parts->randomDataShortCount),
                static_cast<unsigned>(parts->randomDataByteCount),
                static_cast<unsigned>(parts->randomDataIntCount),
                static_cast<unsigned>(parts->indexCount), deltaQuatPresent,
                static_cast<unsigned>(deltaQuatSize), deltaTransPresent,
                static_cast<unsigned>(deltaTransSize),
                static_cast<unsigned>(deltaTransSmall));
    for (uint32_t i = 0; i < parts->boneCount[9] && parts->names; ++i)
    {
        const char *bone = SL_ConvertToString(parts->names[i]);
        std::printf("OAT_XAN_BONE zone=%u seq=%u i=%u name='%s'\n", zoneIndex, seq, i,
                    bone ? bone : "<null>");
    }
    for (uint32_t i = 0; i < parts->notifyCount && parts->notify; ++i)
    {
        const char *notify = SL_ConvertToString(parts->notify[i].name);
        uint32_t timeBits = 0;
        std::memcpy(&timeBits, &parts->notify[i].time, sizeof(timeBits));
        const uint32_t frame = parts->numframes > 0
            ? static_cast<uint32_t>(std::lround(parts->notify[i].time *
                                                static_cast<float>(parts->numframes)))
            : 0u;
        std::printf("OAT_XAN_NOTIFY zone=%u seq=%u i=%u name='%s' time=%08x frame=%u\n",
                    zoneIndex, seq, i, notify ? notify : "<null>", timeBits, frame);
    }
    StubOatXAnimRawArray(zoneIndex, seq, "db", parts->dataByte, parts->dataByteCount, 1);
    StubOatXAnimRawArray(zoneIndex, seq, "ds", parts->dataShort, parts->dataShortCount, 2);
    StubOatXAnimRawArray(zoneIndex, seq, "di", parts->dataInt, parts->dataIntCount, 4);
    StubOatXAnimRawArray(zoneIndex, seq, "rds", parts->randomDataShort,
                         parts->randomDataShortCount, 2);
    StubOatXAnimRawArray(zoneIndex, seq, "rdb", parts->randomDataByte,
                         parts->randomDataByteCount, 1);
    StubOatXAnimRawArray(zoneIndex, seq, "rdi", parts->randomDataInt,
                         parts->randomDataIntCount, 4);
    if (parts->numframes >= 0x100u)
        StubOatXAnimRawArray(zoneIndex, seq, "ind", parts->indices._2,
                             parts->indexCount, 2);
    else
        StubOatXAnimRawArray(zoneIndex, seq, "ind", parts->indices._1,
                             parts->indexCount, 1);
    // Delta part: separate buffers, same verification treatment.  The quat
    // delta body is the 2-component IW3 form (frame0 when size is zero);
    // the trans delta body is the same small/full/constant shape as a bone
    // trans.  Index width follows the body's numframes branch.
    if (parts->deltaPart && parts->deltaPart->quat)
    {
        const XAnimDeltaPartQuat *quat = parts->deltaPart->quat;
        const uint32_t count = quat->size ? quat->size + 1u : 0u;
        std::printf("OAT_XAN_DQ zone=%u seq=%u size=%u ind=", zoneIndex, seq,
                    static_cast<unsigned>(quat->size));
        if (count)
        {
            const void *indices = parts->numframes >= 0x100u
                ? static_cast<const void *>(quat->u.frames.indices._2)
                : static_cast<const void *>(quat->u.frames.indices._1);
            StubOatXAnimHex(indices, count * (parts->numframes >= 0x100u ? 2u : 1u));
            std::printf(" frames=");
            StubOatXAnimHex(quat->u.frames.frames, count * 4u);
        }
        else
        {
            std::printf(" frames=");
            StubOatXAnimHex(quat->u.frame0, 4u);
        }
        std::fputc('\n', stdout);
    }
    if (parts->deltaPart && parts->deltaPart->trans)
    {
        const XAnimPartTrans *trans = parts->deltaPart->trans;
        const uint32_t count = trans->size ? trans->size + 1u : 0u;
        std::printf("OAT_XAN_DT zone=%u seq=%u size=%u small=%u ind=", zoneIndex, seq,
                    static_cast<unsigned>(trans->size),
                    static_cast<unsigned>(trans->smallTrans));
        if (count)
        {
            const void *indices = parts->numframes >= 0x100u
                ? static_cast<const void *>(trans->u.frames.indices._2)
                : static_cast<const void *>(trans->u.frames.indices._1);
            StubOatXAnimHex(indices, count * (parts->numframes >= 0x100u ? 2u : 1u));
            std::printf(" mins=");
            StubOatXAnimHex(trans->u.frames.mins, 12u);
            std::printf(" size=");
            StubOatXAnimHex(trans->u.frames.size, 12u);
            std::printf(" frames=");
            if (trans->smallTrans)
                StubOatXAnimHex(trans->u.frames.frames._1, count * 3u);
            else
                StubOatXAnimHex(trans->u.frames.frames._2, count * 6u);
        }
        else
        {
            std::printf(" const=");
            StubOatXAnimHex(trans->u.frame0, 12u);
        }
        std::fputc('\n', stdout);
    }
}

// sweep #2 (OAT differential decode), IW3 menu family: every menu and
// ItemDef body the UI consumes.  OAT's IW3 MenuWriter re-serializes its own
// binary decode as the legacy .menu text form, one file per menu, covering
// every scalar window/menu/item field plus the type-specific listBox/
// editField/multi/enum bodies and the material/sound bindings.  The port's
// own decoded structs (ui_shared.h) are the same IW3 reference layout, so
// this trace prints every field the writer can express: floats as raw IEEE
// bit patterns, strings as hex bytes, and the raw static/dynamic flag words
// so the verifier can check the exact bit the writer's keyword implies
// without trusting this decoder's derivation.  Scripts are printed raw (the
// verifier tokenizes both sides with the writer's own lexer rules); statement
// slots print their entry count so the verifier can check that each
// expression the writer emits is actually attached to the right slot.
void StubOatMenuHex(const char *value)
{
    if (!value)
        return;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p)
        std::printf("%02x", static_cast<unsigned>(*p));
}

uint32_t StubOatFloatBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

uint32_t StubOatMenuColor(const float (&color)[4])
{
    for (uint32_t i = 0; i < 4; ++i)
        std::printf("%s%08x", i ? "," : "", StubOatFloatBits(color[i]));
    return 4;
}

void StubOatMenuRectValues(const rectDef_s &rect)
{
    std::printf("%08x,%08x,%08x,%08x,%d,%d",
                StubOatFloatBits(rect.x), StubOatFloatBits(rect.y),
                StubOatFloatBits(rect.w), StubOatFloatBits(rect.h),
                rect.horzAlign, rect.vertAlign);
}

void StubOatMenuRect(const rectDef_s &rect)
{
    std::printf("rect=");
    StubOatMenuRectValues(rect);
}

void StubOatMenuItemTrace(uint32_t zoneIndex, uint32_t menuSeq, const char *menuName,
                          uint32_t itemIndex, const itemDef_s *item)
{
    if (!item)
        return;
    std::printf("OAT_MENUIT zone=%u seq=%u menu=", zoneIndex, menuSeq);
    StubOatMenuHex(menuName);
    std::printf(" i=%u name=", itemIndex);
    StubOatMenuHex(item->window.name);
    std::printf(" text=");
    StubOatMenuHex(item->text);
    std::printf(" group=");
    StubOatMenuHex(item->window.group);
    std::printf(" ");
    StubOatMenuRect(item->window.rectClient);
    std::printf(" rawrect=");
    StubOatMenuRectValues(item->window.rect);
    std::printf(" style=%d type=%d border=%d borderSize=%08x static=%u dynamic=%u "
                "ownerdraw=%d odflags=%u align=%d textalign=%d textalignx=%08x "
                "textaligny=%08x textscale=%08x textstyle=%d textfont=%d "
                "gamemsgindex=%d gamemsgmode=%d backcolor=",
                item->window.style, item->type, item->window.border,
                StubOatFloatBits(item->window.borderSize),
                static_cast<unsigned>(item->window.staticFlags),
                static_cast<unsigned>(item->window.dynamicFlags[0]),
                item->window.ownerDraw, static_cast<unsigned>(item->window.ownerDrawFlags),
                item->alignment, item->textAlignMode,
                StubOatFloatBits(item->textalignx), StubOatFloatBits(item->textaligny),
                StubOatFloatBits(item->textscale), item->textStyle, item->fontEnum,
                item->gameMsgWindowIndex, item->gameMsgWindowMode);
    StubOatMenuColor(item->window.backColor);
    std::printf(" forecolor=");
    StubOatMenuColor(item->window.foreColor);
    std::printf(" bordercolor=");
    StubOatMenuColor(item->window.borderColor);
    std::printf(" outlinecolor=");
    StubOatMenuColor(item->window.outlineColor);
    std::printf(" background=");
    StubOatMenuHex(item->window.background && item->window.background->info.name
                       ? item->window.background->info.name : nullptr);
    std::printf(" focussound=");
    StubOatMenuHex(item->focusSound ? item->focusSound->aliasName : nullptr);
    std::printf(" dvar=");
    StubOatMenuHex(item->dvar);
    std::printf(" dvartest=");
    StubOatMenuHex(item->dvarTest);
    std::printf(" enabledvar=");
    StubOatMenuHex(item->enableDvar);
    std::printf(" dvarflags=%d special=%08x onkey=%u vexp=%d texp=%d matexp=%d "
                "rxexp=%d ryexp=%d rwexp=%d rhexp=%d faexp=%d itemflags=%u "
                "onfocus=",
                item->dvarFlags, StubOatFloatBits(item->special),
                [item]() -> uint32_t {
                    uint32_t count = 0;
                    for (const ItemKeyHandler *h = item->onKey; h; h = h->next)
                        ++count;
                    return count;
                }(),
                item->visibleExp.numEntries, item->textExp.numEntries,
                item->materialExp.numEntries, item->rectXExp.numEntries,
                item->rectYExp.numEntries, item->rectWExp.numEntries,
                item->rectHExp.numEntries, item->forecolorAExp.numEntries,
                static_cast<unsigned>(item->itemFlags));
    StubOatMenuHex(item->onFocus);
    std::printf(" leavefocus=");
    StubOatMenuHex(item->leaveFocus);
    std::printf(" mouseenter=");
    StubOatMenuHex(item->mouseEnter);
    std::printf(" mouseexit=");
    StubOatMenuHex(item->mouseExit);
    std::printf(" mouseentertext=");
    StubOatMenuHex(item->mouseEnterText);
    std::printf(" mouseexittext=");
    StubOatMenuHex(item->mouseExitText);
    std::printf(" action=");
    StubOatMenuHex(item->action);
    std::printf(" accept=");
    StubOatMenuHex(item->onAccept);
    std::printf("\n");

    if (item->onKey)
    {
        uint32_t handlerIndex = 0;
        for (const ItemKeyHandler *h = item->onKey; h; h = h->next, ++handlerIndex)
        {
            std::printf("OAT_MENUITKEY zone=%u seq=%u menu=", zoneIndex, menuSeq);
            StubOatMenuHex(menuName);
            std::printf(" i=%u k=%u key=%d action=", itemIndex, handlerIndex, h->key);
            StubOatMenuHex(h->action);
            std::printf("\n");
        }
    }
    if (item->typeData.data)
    {
        if (item->type == 6 && item->typeData.listBox)
        {
            const listBoxDef_s *lb = item->typeData.listBox;
            std::printf("OAT_MENUILB zone=%u seq=%u menu=", zoneIndex, menuSeq);
            StubOatMenuHex(menuName);
            std::printf(" i=%u mousepos=%d start=%d end=%d pad=%d ew=%08x eh=%08x "
                        "estyle=%d ncols=%d doubleclick=",
                        itemIndex, lb->mousePos, lb->startPos[0], lb->endPos[0],
                        lb->drawPadding, StubOatFloatBits(lb->elementWidth),
                        StubOatFloatBits(lb->elementHeight), lb->elementStyle,
                        lb->numColumns);
            StubOatMenuHex(lb->doubleClick);
            std::printf(" notselectable=%d noscrollbars=%d usepaging=%d selectborder=",
                        lb->notselectable, lb->noScrollBars, lb->usePaging);
            StubOatMenuColor(lb->selectBorder);
            std::printf(" disablecolor=");
            StubOatMenuColor(lb->disableColor);
            std::printf(" selecticon=");
            StubOatMenuHex(lb->selectIcon && lb->selectIcon->info.name
                               ? lb->selectIcon->info.name : nullptr);
            for (uint32_t c = 0; c < 16; ++c)
                std::printf(" col%u=%d,%d,%d,%d", c, lb->columnInfo[c].pos,
                            lb->columnInfo[c].width, lb->columnInfo[c].maxChars,
                            lb->columnInfo[c].alignment);
            std::printf("\n");
        }
        else if ((item->type == 4 || item->type == 9 || item->type == 0x10 ||
                  item->type == 0x12 || item->type == 0xB || item->type == 0xE ||
                  item->type == 0xA || item->type == 0 || item->type == 0x11) &&
                 item->typeData.editField)
        {
            const editFieldDef_s *ef = item->typeData.editField;
            std::printf("OAT_MENUIEF zone=%u seq=%u menu=", zoneIndex, menuSeq);
            StubOatMenuHex(menuName);
            std::printf(" i=%u min=%08x max=%08x def=%08x range=%08x maxchars=%d "
                        "gotonext=%d paintchars=%d paintoffset=%d\n",
                        itemIndex, StubOatFloatBits(ef->minVal),
                        StubOatFloatBits(ef->maxVal), StubOatFloatBits(ef->defVal),
                        StubOatFloatBits(ef->range), ef->maxChars,
                        ef->maxCharsGotoNext, ef->maxPaintChars, ef->paintOffset);
        }
        else if (item->type == 0xC && item->typeData.multi)
        {
            const multiDef_s *multi = item->typeData.multi;
            std::printf("OAT_MENUIMD zone=%u seq=%u menu=", zoneIndex, menuSeq);
            StubOatMenuHex(menuName);
            std::printf(" i=%u count=%d strdef=%d", itemIndex, multi->count,
                        multi->strDef);
            for (int32_t e = 0; e < multi->count && e < 32; ++e)
            {
                std::printf(" dv%u=", static_cast<unsigned>(e));
                StubOatMenuHex(multi->dvarList[e]);
                std::printf(" ds%u=", static_cast<unsigned>(e));
                StubOatMenuHex(multi->dvarStr[e]);
                std::printf(" df%u=%08x", static_cast<unsigned>(e),
                            StubOatFloatBits(multi->dvarValue[e]));
            }
            std::printf("\n");
        }
        else if (item->type == 0xD)
        {
            std::printf("OAT_MENUIED zone=%u seq=%u menu=", zoneIndex, menuSeq);
            StubOatMenuHex(menuName);
            std::printf(" i=%u enumdvar=", itemIndex);
            StubOatMenuHex(item->typeData.enumDvarName);
            std::printf("\n");
        }
    }
}

void StubOatMenuTrace(uint32_t zoneIndex, const menuDef_t *menu)
{
    if (!menu)
        return;
    static uint32_t menuSeq = 0;
    const uint32_t seq = ++menuSeq;
    const char *name = menu->window.name;
    std::printf("OAT_MENU zone=%u seq=%u name=", zoneIndex, seq);
    StubOatMenuHex(name);
    std::printf(" fullscreen=%d static=%u dynamic=%u ", menu->fullScreen != 0 ? 1 : 0,
                static_cast<unsigned>(menu->window.staticFlags),
                static_cast<unsigned>(menu->window.dynamicFlags[0]));
    StubOatMenuRect(menu->window.rect);
    std::printf(" style=%d border=%d borderSize=%08x ownerdraw=%d odflags=%u "
                "fadecycle=%d fadeclamp=%08x fadeamount=%08x fadeinamount=%08x "
                "blurworld=%08x soundloop=",
                menu->window.style, menu->window.border,
                StubOatFloatBits(menu->window.borderSize), menu->window.ownerDraw,
                static_cast<unsigned>(menu->window.ownerDrawFlags), menu->fadeCycle,
                StubOatFloatBits(menu->fadeClamp), StubOatFloatBits(menu->fadeAmount),
                StubOatFloatBits(menu->fadeInAmount), StubOatFloatBits(menu->blurRadius));
    StubOatMenuHex(menu->soundName);
    std::printf(" allowedbinding=");
    StubOatMenuHex(menu->allowedBinding);
    std::printf(" backcolor=");
    StubOatMenuColor(menu->window.backColor);
    std::printf(" forecolor=");
    StubOatMenuColor(menu->window.foreColor);
    std::printf(" bordercolor=");
    StubOatMenuColor(menu->window.borderColor);
    std::printf(" outlinecolor=");
    StubOatMenuColor(menu->window.outlineColor);
    std::printf(" focuscolor=");
    StubOatMenuColor(menu->focusColor);
    std::printf(" disablecolor=");
    StubOatMenuColor(menu->disableColor);
    std::printf(" background=");
    StubOatMenuHex(menu->window.background && menu->window.background->info.name
                       ? menu->window.background->info.name : nullptr);
    std::printf(" onopen=");
    StubOatMenuHex(menu->onOpen);
    std::printf(" onclose=");
    StubOatMenuHex(menu->onClose);
    std::printf(" onesc=");
    StubOatMenuHex(menu->onESC);
    std::printf(" onkey=%u vexp=%d rxexp=%d ryexp=%d itemcount=%d ptr=%p",
                [menu]() -> uint32_t {
                    uint32_t count = 0;
                    for (const ItemKeyHandler *h = menu->onKey; h; h = h->next)
                        ++count;
                    return count;
                }(),
                menu->visibleExp.numEntries, menu->rectXExp.numEntries,
                menu->rectYExp.numEntries, menu->itemCount,
                static_cast<const void *>(menu));
    std::printf("\n");
    if (menu->onKey)
    {
        uint32_t handlerIndex = 0;
        for (const ItemKeyHandler *h = menu->onKey; h; h = h->next, ++handlerIndex)
        {
            std::printf("OAT_MENUKEY zone=%u seq=%u k=%u key=%d action=", zoneIndex, seq,
                        handlerIndex, h->key);
            StubOatMenuHex(h->action);
            std::printf("\n");
        }
    }
    for (int32_t i = 0; i < menu->itemCount; ++i)
        StubOatMenuItemTrace(zoneIndex, seq, name, static_cast<uint32_t>(i),
                             menu->items ? menu->items[i] : nullptr);
}

void StubOatFieldTrace(XAssetType type, uint32_t zoneIndex, XAssetHeader header)
{
    static const bool enabled = std::getenv("KISAK_OAT_FIELD_TRACE") != nullptr;
    if (!enabled || !header.data)
        return;
    switch (type)
    {
    case ASSET_TYPE_XANIMPARTS:
        StubOatXAnimTrace(zoneIndex, header.parts);
        break;
    case ASSET_TYPE_MENU:
        StubOatMenuTrace(zoneIndex, header.menu);
        break;
    case ASSET_TYPE_MATERIAL:
        StubOatMaterialTrace(zoneIndex, header.material);
        break;
    case ASSET_TYPE_FONT:
        StubOatFontTrace(zoneIndex, header.font);
        break;
    case ASSET_TYPE_IMAGE:
        StubOatImageTrace(zoneIndex, header.image);
        break;
    case ASSET_TYPE_LOADED_SOUND:
    {
        // sweep #2 (OAT differential decode), IW3 loaded-sound family:
        // OAT dumps the PCM body as a canonical RIFF/WAVE file, so the
        // format/channels/rate/bits/data-length the port read out of the
        // 44-byte wire header are all directly comparable.  The payload
        // pointer is emitted too: the port's own B2 decision keeps it null
        // (zone-arena exhaustion), which the verifier
        // pins rather than pretending the bytes were copied.
        const LoadedSound *sound = header.loadSnd;
        if (!sound)
            return;
        std::printf("OAT_SND zone=%u name='%s' format=%d data_len=%u rate=%u bits=%d "
                    "channels=%d samples=%u block=%u data=%u\n",
                    zoneIndex, sound->name ? sound->name : "<null>",
                    sound->sound.info.format, sound->sound.info.data_len,
                    sound->sound.info.rate, sound->sound.info.bits,
                    sound->sound.info.channels, sound->sound.info.samples,
                    sound->sound.info.block_size, sound->sound.data ? 1u : 0u);
        break;
    }
    case ASSET_TYPE_MENULIST:
    {
        // sweep #2 (OAT differential decode), IW3 menulist family:
        // OAT's lnDumperIW3 writes the list as `loadMenu { "<path>" }` lines
        // for referenced menus plus embedded menuDef blocks in list order,
        // where <path> is `<list parent>/<menu name without comma>.menu`.
        // The port prints the registered list name, its menuCount, and every
        // entry's (name, pointer) pair so the verifier can both rebuild the
        // ordered path sequence and require each pointer to be the canonical
        // registered owner body, never the transient comma placeholder.
        const MenuList *list = header.menuList;
        if (!list)
            return;
        std::printf("OAT_MENULIST zone=%u name=", zoneIndex);
        StubOatMenuHex(list->name);
        std::printf(" count=%d", list->menuCount);
        for (int32_t i = 0; i < list->menuCount; ++i)
        {
            const menuDef_t *menu = list->menus ? list->menus[i] : nullptr;
            std::printf(" m%d=", i);
            StubOatMenuHex(menu && menu->window.name ? menu->window.name : nullptr);
            std::printf(",%p", static_cast<const void *>(menu));
        }
        std::printf("\n");
        break;
    }
    case ASSET_TYPE_SOUND_CURVE:
    {
        // sweep #2 (OAT differential decode), IW3 soundcurve family:
        // OAT's SndCurveDumper writes "SNDCURVE\\n\\n<knots>\\n<x> <y>" per
        // .vfcurve file (4-decimal precision is the writer's own limit), so
        // knotCount and the knot pairs are directly comparable.  The wire
        // filename XString is the original loader's registered name (OAT's
        // LoaderSoundCurveIW3 stores Dup(assetName) too), so the verifier
        // compares it against the dump file stem.  Floats print as raw IEEE
        // bits so the verifier compares exactly, then applies at most the
        // writer's 5e-5 rounding margin when expanding OAT's decimal text.
        const SndCurve *curve = header.sndCurve;
        if (!curve)
            return;
        std::printf("OAT_SNDCURVE zone=%u name=", zoneIndex);
        StubOatMenuHex(curve->filename);
        std::printf(" knots=%d", curve->knotCount);
        for (uint32_t i = 0; i < 8; ++i)
            std::printf(" k%u=%08x,%08x", i, StubOatFloatBits(curve->knots[i][0]),
                        StubOatFloatBits(curve->knots[i][1]));
        std::printf("\n");
        break;
    }
    case ASSET_TYPE_STRINGTABLE:
    {
        // sweep #2: the cell values are the payload. OAT writes the
        // same row-major cells as CSV, so both sides digest one canonical
        // stream (rows, cols, then per cell a 32-bit length and the bytes),
        // which pins the orientation, the count and every value at once.
        const StringTable *table = header.stringTable;
        if (!table)
            return;
        const uint32_t rows = table->rowCount < 0 ? 0u : static_cast<uint32_t>(table->rowCount);
        const uint32_t cols = table->columnCount < 0 ? 0u : static_cast<uint32_t>(table->columnCount);
        const uint32_t cells = rows * cols;
        uint64_t digest = 0xcbf29ce484222325ull;
        if (cells && !table->values)
        {
            std::printf("OAT_FIELD zone=%u type=32 name='%s' rows=%d cols=%d "
                        "cells=%u stnull=1 sdigest=0000000000000000\n",
                        zoneIndex, table->name ? table->name : "<null>",
                        table->rowCount, table->columnCount, cells);
            break;
        }
        auto mix = [&digest](const void *data, uint32_t length)
        {
            const uint8_t *bytes = static_cast<const uint8_t *>(data);
            for (uint32_t i = 0; i < length; ++i)
            {
                digest ^= bytes[i];
                digest *= 0x100000001b3ull;
            }
        };
        auto mixU32 = [&mix](uint32_t value)
        {
            uint8_t bytes[4] = {static_cast<uint8_t>(value & 0xff),
                                static_cast<uint8_t>((value >> 8) & 0xff),
                                static_cast<uint8_t>((value >> 16) & 0xff),
                                static_cast<uint8_t>((value >> 24) & 0xff)};
            mix(bytes, 4);
        };
        mixU32(rows);
        mixU32(cols);
        for (uint32_t i = 0; i < cells; ++i)
        {
            const char *cell = table->values[i] ? table->values[i] : "";
            const uint32_t length = static_cast<uint32_t>(std::strlen(cell));
            mixU32(length);
            mix(cell, length);
        }
        std::printf("OAT_FIELD zone=%u type=32 name='%s' rows=%d cols=%d "
                    "cells=%u stnull=0 sdigest=%016llx\n",
                    zoneIndex, table->name ? table->name : "<null>",
                    table->rowCount, table->columnCount, cells,
                    static_cast<unsigned long long>(digest));
        break;
    }
    case ASSET_TYPE_MAP_ENTS:
    {
        // sweep #2: the entity string is the spawn/camera source the
        // first-frame orchestration parses. OAT's own dumper writes
        // max(numEntityChars - 1, 0) bytes (the authored span's trailing
        // terminator byte is dropped by the .ents form), so the diff compares
        // exactly that span and records the terminator convention as an
        // intentional divergence rather than filtering it.
        const MapEnts *ents = header.mapEnts;
        if (!ents)
            return;
        const uint32_t elen = ents->numEntityChars > 0
            ? static_cast<uint32_t>(ents->numEntityChars) - 1u : 0u;
        const uint64_t edigest = ents->entityString
            ? StubOatFnv1a64(reinterpret_cast<const uint8_t *>(ents->entityString), elen)
            : (elen == 0 ? StubOatFnv1a64(nullptr, 0) : 0);
        std::printf("OAT_FIELD zone=%u type=15 name='%s' nchars=%d elen=%u "
                    "enull=%u edigest=%016llx\n",
                    zoneIndex, ents->name ? ents->name : "<null>", ents->numEntityChars,
                    elen, ents->entityString ? 0u : 1u,
                    static_cast<unsigned long long>(edigest));
        break;
    }
    case ASSET_TYPE_LOCALIZE_ENTRY:
    {
        // sweep #2: the localize value is the whole payload the HUD and
        // menu text consume. The decoder resolves both slots through either
        // the inline block-4 form or a block-offset alias (CopyInlineString),
        // so a wrong alias resolution changes the string, not just a count.
        const LocalizeEntry *entry = header.localize;
        if (!entry)
            return;
        const uint32_t vlen = entry->value
            ? static_cast<uint32_t>(std::strlen(entry->value)) : 0;
        const uint64_t vdigest = entry->value
            ? StubOatFnv1a64(reinterpret_cast<const uint8_t *>(entry->value), vlen)
            : StubOatFnv1a64(nullptr, 0);
        std::printf("OAT_FIELD zone=%u type=22 name='%s' vlen=%u vnull=%u vfnv=%016llx\n",
                    zoneIndex, entry->name ? entry->name : "<null>", vlen,
                    entry->value ? 0u : 1u, static_cast<unsigned long long>(vdigest));
        break;
    }
    case ASSET_TYPE_RAWFILE:
    {
        const RawFile *file = header.rawfile;
        if (!file)
            return;
        const uint32_t len = file->len < 0 ? 0xffffffffu : static_cast<uint32_t>(file->len);
        // Android Load_RawFile allocates len + 1 bytes (the streamed
        // terminator included) only when the buffer slot is nonzero; OAT
        // dumps exactly len payload bytes, so a missing buffer is a real
        // divergence the verifier must see, never a silently equal digest.
        const uint64_t digest = file->buffer
            ? StubOatFnv1a64(reinterpret_cast<const uint8_t *>(file->buffer), len)
            : (len == 0 ? StubOatFnv1a64(nullptr, 0) : 0);
        std::printf("OAT_FIELD zone=%u type=31 name='%s' len=%d buf=%u fnv=%016llx\n",
                    zoneIndex, file->name ? file->name : "<null>", file->len,
                    file->buffer ? 1u : 0u, static_cast<unsigned long long>(digest));
        break;
    }
    default:
        break;
    }
}

XAssetHeader DB_RetailZoneRegister(XAssetType type, XAssetHeader header, uint32_t zoneIndex)
{
    if (zoneIndex == 0 || zoneIndex >= kMaxZones || !g_zones[zoneIndex].live || !header.data)
        return XAssetHeader{};
    if (static_cast<uint32_t>(type) < 64)
        ++g_stubRegisterAttempts[static_cast<uint32_t>(type)];
    // Override chain, not replacement: the real DB_LinkXAssetEntry keeps
    // returning the FIRST (existing) entry on a same-name collision and
    // links the newcomer on nextOverride for later unload promotion.
    // Anonymous (nameless) bodies never collide.
    const char *name = StubAssetName(type, header);
    // body-registration parity: the original DB_LinkXAssetEntry treats
    // a name whose first byte is ',' as a stub asset -- it strips the comma,
    // finds the already-registered owner, and returns that entry without
    // registering the widened body; with no owner it calls
    // DB_CreateDefaultEntry(type, stripped) and returns the engine default
    // body (db_registry.cpp isStubAsset branch, non-override path).  The
    // port's production path (DB_RetailZoneRegister -> DB_AddXAsset) already
    // does this on the Switch target; the host double must model the same
    // branch or every host proof would see the transient empty comma body as
    // a real registration.  Measured closures comma registrations:
    // techniqueset (100), loadedsound (38), image (28), fx (20), menu (17),
    // xmodel (12), soundcurve (5), physpreset (5); material's comma stubs
    // are consumed by the material walk's deferred-slot ledger before this
    // call.  OAT's ordered declaration lists show every one of those stubs
    // (except image's nine MP HUD names) has an owner in an earlier zone or
    // earlier in the same zone, so owner resolution is the whole branch for
    // them; the no-owner image stubs take the mirrored default-body half.
    const bool commaStub = name && name[0] == ',';
    const char *matchName = commaStub ? name + 1 : name;
    // Diagnostic for the per-type coverage sweep: name every
    // registration attempt so the host walk can be diffed against OAT's
    // per-type declaration list. Env-gated so ordinary test output is
    // unchanged.
    static const bool traceRegisters = std::getenv("KISAK_ZONE_REGISTER_TRACE") != nullptr;
    if (traceRegisters)
        std::fprintf(stderr, "ZONE_REGISTER zone=%u t=%u name=%s\n", zoneIndex,
                     static_cast<unsigned>(type), name ? name : "<anon>");
    StubOatFieldTrace(type, zoneIndex, header);
    if (name)
    {
        // retained-pointer generalization: every registration's name pointer must
        // be zone-owned, never reader-transient (com_files.h's
        // FS_RetailFastfileBlockData contract).  A hit is the exact shape
        // that made light_point_linear's registered name dangle once
        // FS_CloseRetailFastfile ran while the zone stayed live: the reader
        // is still open here, so this catches it at the earliest point it
        // exists.  The lifetime verifier requires the count to end at zero.
        ++g_stubNamedRegistrationCount;
        if (static_cast<uint32_t>(type) < 64)
            g_stubNamedTypeMask |= 1ull << static_cast<uint32_t>(type);
        if (FS_RetailFastfilePointerIsReaderTransient(name))
        {
            ++g_stubReaderOwnedNameCount;
            if (g_stubReaderOwnedNameCount <= 16)
            {
                std::printf("KILLHOUSE_PROVENANCE reader_owned_name type=%u zone=%u name='%.48s'\n",
                            static_cast<unsigned>(type), zoneIndex, name);
                // Flush immediately: this is the loud diagnostic for a
                // dangling-name bug, and a later ASan use-after-free would
                // otherwise discard it with the stdio buffer.
                std::fflush(stdout);
            }
        }
        for (uint32_t i = 0; i < g_registeredCount; ++i)
        {
            if (g_registered[i].type != type)
                continue;
            const char *candidate = StubAssetName(type, g_registered[i].header);
            if (candidate && std::strcmp(candidate, matchName) == 0)
            {
                if (commaStub)
                {
                    // Original DB_LinkXAssetEntry's stub branch: a
                    // comma-prefixed name resolves to the already-registered
                    // stripped owner and returns that entry; the widened
                    // comma body is never registered.  Provenance matters:
                    // the menulist slot has to point at the LIVE canonical
                    // menu, not the transient empty placeholder (the retail
                    // corpus stores all 17 closure comma menus with zero
                    // items and a same-named full owner in an earlier zone).
                    if (type == ASSET_TYPE_MENU)
                    {
                        std::printf("KILLHOUSE_MENU_STUB zone=%u name='%s' resolved_zone=%u "
                                    "resolved_name='%s' ptr=%p\n",
                                    zoneIndex, name, g_registered[i].zoneIndex, candidate,
                                    static_cast<const void *>(g_registered[i].header.menu));
                    }
                    std::printf("KILLHOUSE_COMMA_STUB type=%u zone=%u name='%s' resolved_zone=%u "
                                "resolved_name='%s' ptr=%p\n",
                                static_cast<unsigned>(type), zoneIndex, name,
                                g_registered[i].zoneIndex, candidate,
                                static_cast<const void *>(g_registered[i].header.data));
                    std::fflush(stdout);
                    // DB_LinkXAssetEntry's stub branch returns before the
                    // override bookkeeping: no nextOverride is linked.
                    return g_registered[i].header;
                }
                g_registered[i].overrideHeader = header;
                g_registered[i].overrideZoneIndex = zoneIndex;
                ++g_overrideCount;
                return g_registered[i].header;
            }
        }
    }
    if (commaStub)
    {
        // No registered owner.  The original DB_LinkXAssetEntry calls
        // DB_CreateDefaultEntry(type, stripped): a clone of the type's engine
        // default body keyed under the stripped name (`$white` for image).
        // Modeling that clone faithfully needs a per-type body copy plus the
        // DB_SetXAssetName handler, which this double cannot link; the
        // fall-through below therefore keeps the previous behavior (register
        // the widened transient comma body) for these ownerless stubs, and
        // this loud counter/diagnostic records the remaining fidelity gap so
        // it is never mistaken for verified original behavior.  Measured:
        // image's nine MP HUD stubs only (no killhouse-reachable consumer);
        // every other closure stub family has an earlier owner.  Menus keep
        // their loud unresolved failure because their verifier depends on it
        // and all 17 have owners.
        if (type == ASSET_TYPE_MENU)
        {
            std::printf("KILLHOUSE_MENU_STUB_UNRESOLVED zone=%u name='%s'\n",
                        zoneIndex, name);
            std::fflush(stdout);
            return XAssetHeader{};
        }
        ++g_stubCommaDefaultMissingCount;
        std::printf("KILLHOUSE_COMMA_STUB default_missing type=%u zone=%u name='%s' "
                    "engine_default='%s'\n",
                    static_cast<unsigned>(type), zoneIndex, name,
                    StubDefaultAssetName(type) ? StubDefaultAssetName(type) : "<none>");
        std::fflush(stdout);
    }
    if (g_registeredCount >= kMaxRegistered)
    {
        // Loud, never a silent drop: production DB_AllocXAssetEntry DROPs
        // on pool exhaustion, and a silently unrecorded registration
        // returns a healthy-looking header that no later lookup can find
        // (this hid a full table behind a passing load).
        Com_Printf(0, "StubRegister: table full (%u), cannot register type=%d\n",
                   g_registeredCount, (int)type);
        return XAssetHeader{};
    }
    g_registered[g_registeredCount++] = {type, header, zoneIndex};
    return header;
}

// Removes a non-zone stock asset installed by a narrow host fixture. The
// production registry owns these through normal zone unload; this helper
// keeps the host double from retaining a pointer to a fixture stack object.
bool StubUnregisterStockAsset(XAssetType type, XAssetHeader header)
{
    for (uint32_t i = 0; i < g_registeredCount; ++i)
    {
        if (g_registered[i].type != type || g_registered[i].zoneIndex != 0 ||
            g_registered[i].header.data != header.data)
            continue;
        g_registered[i] = g_registered[g_registeredCount - 1];
        --g_registeredCount;
        return true;
    }
    return false;
}

// Host stand-in for the production DB_RemoveXAssetHandler XAnim entry
// (db_registry.cpp, B3): the real handler calls XAnimFree, which releases
// one SL_* reference per bone/notify occurrence. This mirrors that exact
// logic so host zone unload proves the same mark/free balance against the
// real SL_* table linked below; the interning side is never stubbed.
void StubFreeXAnimPartsSL(XAssetHeader header)
{
    if (!header.parts)
        return;
    XAnimParts *parts = header.parts;
    if (parts->names)
    {
        for (uint32_t i = 0; i < parts->boneCount[9]; ++i)
            SL_RemoveRefToString(parts->names[i]);
    }
    if (parts->notify)
    {
        for (uint32_t i = 0; i < parts->notifyCount; ++i)
            SL_RemoveRefToString(parts->notify[i].name);
    }
}

// Host stand-in for the production DB_RemoveXAssetHandler WEAPON entry
// (db_registry.cpp, B7): the real handler calls RetailWeaponFree, which
// releases one SL_* reference per hideTags/notetrack occurrence. Mirrors
// that exact logic so host zone unload proves the same mark/free balance.
void StubFreeWeaponSL(XAssetHeader header)
{
    if (!header.weapon)
        return;
    WeaponDef *weapon = header.weapon;
    for (uint32_t i = 0; i < 8; ++i)
    {
        if (weapon->hideTags[i])
            SL_RemoveRefToString(weapon->hideTags[i]);
    }
    for (uint32_t i = 0; i < 16; ++i)
    {
        if (weapon->notetrackSoundMapKeys[i])
            SL_RemoveRefToString(weapon->notetrackSoundMapKeys[i]);
        if (weapon->notetrackSoundMapValues[i])
            SL_RemoveRefToString(weapon->notetrackSoundMapValues[i]);
    }
}
// Host stand-in for db_registry.cpp's Load_XAnimPartsAsset: the production
// owner registers through DB_AddXAsset (attributed to g_zoneIndex) and
// updates the header to the pooled/canonical entry. The stub registry
// needs the zone explicitly, so attribute to g_stubLoadingZone (the zone
// begun most recently and not yet ended -- loads are strictly sequential
// here, making that unambiguous during any decoder commit, exactly when
// this runs).
void Load_XAnimPartsAsset(XAssetHeader *parts)
{
    if (!parts)
        return;
    XAssetHeader empty{};
    if (!g_stubLoadingZone)
    {
        *parts = empty;
        return;
    }
    *parts = DB_RetailZoneRegister(ASSET_TYPE_XANIMPARTS, *parts, g_stubLoadingZone);
}

// Host stand-in for db_registry.cpp's Load_WeaponDefAsset: same
// zone-attributed registration contract, ASSET_TYPE_WEAPON.
void Load_WeaponDefAsset(XAssetHeader *weapon)
{
    if (!weapon)
        return;
    XAssetHeader empty{};
    if (!g_stubLoadingZone)
    {
        *weapon = empty;
        return;
    }
    *weapon = DB_RetailZoneRegister(ASSET_TYPE_WEAPON, *weapon, g_stubLoadingZone);
}

// Host stand-ins for db_registry.cpp's B2/B6 owners: same zone-attributed
// registration contract. Neither type interns SL_* references, so unload
// needs no special sweep.
void Load_snd_alias_list_Asset(XAssetHeader *sound)
{
    if (!sound)
        return;
    XAssetHeader empty{};
    if (!g_stubLoadingZone)
    {
        *sound = empty;
        return;
    }
    *sound = DB_RetailZoneRegister(ASSET_TYPE_SOUND, *sound, g_stubLoadingZone);
}

void Load_FxEffectDefAsset(XAssetHeader *fx)
{
    if (!fx)
        return;
    XAssetHeader empty{};
    if (!g_stubLoadingZone)
    {
        *fx = empty;
        return;
    }
    *fx = DB_RetailZoneRegister(ASSET_TYPE_FX, *fx, g_stubLoadingZone);
}

void Load_FxImpactTableAsset(XAssetHeader *impactFx)
{
    if (!impactFx)
        return;
    XAssetHeader empty{};
    if (!g_stubLoadingZone)
    {
        *impactFx = empty;
        return;
    }
    *impactFx = DB_RetailZoneRegister(ASSET_TYPE_IMPACT_FX, *impactFx, g_stubLoadingZone);
}

void Load_PhysPresetAsset(XAssetHeader *physPreset)
{
    if (!physPreset)
        return;
    XAssetHeader empty{};
    if (!g_stubLoadingZone)
    {
        *physPreset = empty;
        return;
    }
    *physPreset = DB_RetailZoneRegister(ASSET_TYPE_PHYSPRESET, *physPreset, g_stubLoadingZone);
}

void Load_SndCurveAsset(XAssetHeader *sndCurve)
{
    if (!sndCurve)
        return;
    XAssetHeader empty{};
    if (!g_stubLoadingZone)
    {
        *sndCurve = empty;
        return;
    }
    *sndCurve = DB_RetailZoneRegister(ASSET_TYPE_SOUND_CURVE, *sndCurve, g_stubLoadingZone);
}

// Single-threaded host has no database contention; the real SL_* table
// still takes these locks internally, so stub them as no-ops here rather
// than linking the Switch critical-section owner into every host binary.
void Sys_EnterCriticalSection(int) {}
void Sys_LeaveCriticalSection(int) {}

// Defined at the bottom of this file; DB_RetailZoneEnd models the production
// per-type remove handlers and needs it before that definition.
void StubShutdownWorld(void);

bool DB_RetailZoneEnd(uint32_t zoneIndex)
{
    if (zoneIndex == 0 || zoneIndex >= kMaxZones || !g_zones[zoneIndex].live)
        return false;
    // sweep #1: production DB_UnloadXZone runs the per-type remove
    // handler table (db_registry.cpp:3422) before freeing zone memory; for
    // ASSET_TYPE_COMWORLD that is DB_RemoveComWorld, which takes the world
    // singleton out of use with its owning zone. Mirror it before the
    // registration sweep below so a later zone's LoadComWorld cannot print a
    // name pointing into this zone's freed arena (the ASan heap-use-after-free
    // the whole-corpus sweep hit on a failed zone load's abort path).
    for (uint32_t i = 0; i < g_registeredCount; ++i)
    {
        if (g_registered[i].zoneIndex == zoneIndex &&
            g_registered[i].type == ASSET_TYPE_COMWORLD)
        {
            StubShutdownWorld();
            break;
        }
    }
    // drop deferred weapon sound slots owned by this zone before its
    // arena is freed, so no later resolve pass can write through a dangling
    // native slot pointer. Deferred comma-material aliases carry the same
    // owner-zone slot lifetime and are purged the same way (mirrors
    // production DB_RetailZoneEnd).
    RetailWeaponZoneUnloaded(zoneIndex);
    RetailMaterialZoneUnloaded(zoneIndex);
    if (zoneIndex == g_stubLoadingZone)
        g_stubLoadingZone = 0;
    StubZone &zone = g_zones[zoneIndex];
    // Sweep registrations (releasing per-asset SL_* references, e.g.
    // XAnimParts bone/notify strings) BEFORE freeing the zone's blocks and
    // arena: the headers point into that memory, matching the production
    // DB_UnloadXZone-before-DB_UnloadXZoneMemory order. Freeing first would
    // leave the SL release reading freed arena memory.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_registeredCount; ++i)
    {
        if (g_registered[i].zoneIndex == zoneIndex)
        {
            const uint32_t oZone = g_registered[i].overrideZoneIndex;
            if (g_registered[i].overrideHeader.data != nullptr && oZone != 0 &&
                oZone < kMaxZones && g_zones[oZone].live)
            {
                // The retired canonical's own SL_* occurrences go with it;
                // the promoted override keeps exactly its own.
                if (g_registered[i].type == ASSET_TYPE_XANIMPARTS)
                    StubFreeXAnimPartsSL(g_registered[i].header);
                else if (g_registered[i].type == ASSET_TYPE_WEAPON)
                    StubFreeWeaponSL(g_registered[i].header);
                g_registered[i].header = g_registered[i].overrideHeader;
                g_registered[i].zoneIndex = oZone;
                g_registered[i].overrideHeader = XAssetHeader{};
                g_registered[i].overrideZoneIndex = 0;
                g_registered[kept++] = g_registered[i];
            }
            else if (g_registered[i].type == ASSET_TYPE_XANIMPARTS)
            {
                StubFreeXAnimPartsSL(g_registered[i].header);
            }
            else if (g_registered[i].type == ASSET_TYPE_WEAPON)
            {
                StubFreeWeaponSL(g_registered[i].header);
            }
            continue;
        }
        if (g_registered[i].overrideZoneIndex == zoneIndex)
        {
            if (g_registered[i].type == ASSET_TYPE_XANIMPARTS)
                StubFreeXAnimPartsSL(g_registered[i].overrideHeader);
            else if (g_registered[i].type == ASSET_TYPE_WEAPON)
                StubFreeWeaponSL(g_registered[i].overrideHeader);
            g_registered[i].overrideHeader = XAssetHeader{};
            g_registered[i].overrideZoneIndex = 0;
        }
        g_registered[kept++] = g_registered[i];
    }
    g_registeredCount = kept;
    // Untrack every registration owned by the ended zone, matching the real
    // DB_RetailZoneEnd contract: after unload, no lookup may resolve into
    // freed zone memory. A swept canonical entry with a still-live
    // override is promoted (the real nextOverride promotion); otherwise the
    // entry is dropped. The retired-pointer audit below fails if this sweep
    // ever stops covering an entry.
    for (auto &block : zone.memory.blocks)
        std::free(block.data);
    std::free(zone.nativeArena);
    zone = StubZone{};
    return true;
}

// Host-only stand-in for the engine's own collision singleton (cm,
// owned by db_registry.cpp's DB_XAssetPool table on Switch, which this
// narrow host test cannot link). The commit path writes through the
// same global the Switch build's decoder writes through; unload clears it
// so no pointer into freed zone memory survives.
clipMap_t cm{};

// Host-only stand-in for the engine's own world singleton (s_world,
// owned by db_registry.cpp's DB_XAssetPool table on Switch, which this
// narrow host test cannot link). The commit path writes through the
// same global the Switch build's decoder writes through; unload clears it
// so no pointer into freed zone memory survives.
GfxWorld s_world{};

// Host doubles for the original shadow-caster per-light walk and its two
// callbacks (r_bsp_load_obj.cpp). The narrow database host proof has no live
// ComWorld/light-region hulls, so the walk is a no-op and the live loader's
// RetailWalkBuildShadowGeometry sees zero counts, exactly like a world with no
// shadow casters. The Switch build links the real implementation.
void __cdecl R_ForEachShadowCastingSurfaceOnEachLight(
    void(__cdecl *Callback)(GfxWorld *, uint32_t, uint32_t))
{
    (void)Callback;
}

void __cdecl R_IncrementShadowGeometryCount(GfxWorld *world, uint32_t primaryLightIndex, uint32_t idk)
{
    (void)world;
    (void)primaryLightIndex;
    (void)idk;
}

void __cdecl R_AddShadowSurfaceToPrimaryLight(
    GfxWorld *world, uint32_t primaryLightIndex, uint32_t sortedSurfIndex)
{
    (void)world;
    (void)primaryLightIndex;
    (void)sortedSurfIndex;
}

// Test-only introspection: did the zone survive without DB_RetailZoneEnd
// being called on it?  RetailWalkLoadZoneAssets deliberately skips
// RetailZoneLoadSessionAbort (which calls DB_RetailZoneEnd) on success, so
// this is how the test proves that behavior instead of just trusting it.
bool StubZoneIsLive(uint32_t zoneIndex)
{
    return zoneIndex != 0 && zoneIndex < kMaxZones && g_zones[zoneIndex].live;
}

uint32_t StubRegisteredCount(void)
{
    return g_registeredCount;
}

// FX elements that carry velocity samples but decode velIntervalCount == 0.
// fx_convert.cpp builds velIntervalCount = velStateCount - 1 and asserts it is
// non-zero, so every retail element with samples has at least one interval;
// FX_IntegrateVelocity asserts the same at runtime (fx_update.cpp:764).
uint32_t StubFxVelIntervalZeroCount(uint32_t *effectsOut, uint32_t *elemsOut, const char **firstOut,
                                    int *firstElemOut)
{
    uint32_t effects = 0, elems = 0, bad = 0;
    if (firstOut)
        *firstOut = nullptr;
    for (uint32_t i = 0; i < g_registeredCount; ++i)
    {
        if (g_registered[i].type != ASSET_TYPE_FX || !g_registered[i].header.fx)
            continue;
        const FxEffectDef *def = g_registered[i].header.fx;
        ++effects;
        const int count = def->elemDefCountLooping + def->elemDefCountOneShot + def->elemDefCountEmission;
        for (int e = 0; e < count && def->elemDefs; ++e)
        {
            ++elems;
            const FxElemDef *elem = &def->elemDefs[e];
            if (elem->velSamples && elem->velIntervalCount == 0)
            {
                std::printf("CORPUS_FX_VEL_BAD fx=%s elem=%d elemType=%u flags=0x%08x visualCount=%u "
                            "velIntervals=%u visIntervals=%u physicsModel=%d\n",
                            def->name, e, (unsigned)elem->elemType, (unsigned)elem->flags,
                            (unsigned)elem->visualCount, (unsigned)elem->velIntervalCount,
                            (unsigned)elem->visStateIntervalCount,
                            elem->elemType == 5 && (elem->flags & 0x8000000) != 0);
                if (!bad && firstOut)
                {
                    *firstOut = def->name;
                    if (firstElemOut)
                        *firstElemOut = e;
                }
                ++bad;
            }
        }
    }
    if (effectsOut)
        *effectsOut = effects;
    if (elemsOut)
        *elemsOut = elems;
    return bad;
}

// One FX_STRING_VISUAL line per sound (elemType 8) and runner (elemType 10)
// visual of every FX body owned by zoneIndex (canonical or override entry):
// the sound alias name, or the runner's resolved effect name. A missing
// string prints "(null)". Returns the number of sound visuals whose name is
// null; FX_SpawnSound asserts on exactly that.
static void DumpFxStringVisuals(const char *ff, const FxEffectDef *def, uint32_t *soundOut,
                                uint32_t *soundNullOut, uint32_t *runnerOut)
{
    const int count = def->elemDefCountLooping + def->elemDefCountOneShot + def->elemDefCountEmission;
    for (int e = 0; e < count && def->elemDefs; ++e)
    {
        const FxElemDef *elem = &def->elemDefs[e];
        if (elem->elemType != 8 && elem->elemType != 10)
            continue;
        for (int v = 0; v < elem->visualCount; ++v)
        {
            FxElemVisuals visual{};
            if (elem->visualCount == 1)
                visual = elem->visuals.instance;
            else if (elem->visuals.array)
                visual = elem->visuals.array[v];
            const char *name = nullptr;
            if (elem->elemType == 8)
            {
                name = visual.soundName;
                ++*soundOut;
                *soundNullOut += name == nullptr;
            }
            else
            {
                name = visual.effectDef.handle ? visual.effectDef.handle->name : nullptr;
                ++*runnerOut;
            }
            std::printf("FX_STRING_VISUAL ff=%s fx=%s elem=%d type=%u visual=%d name=%s\n", ff,
                        def->name, e, (unsigned)elem->elemType, v, name ? name : "(null)");
        }
    }
}

uint32_t StubFxStringVisualDump(uint32_t zoneIndex, const char *ff, uint32_t *soundOut,
                                uint32_t *runnerOut)
{
    uint32_t sound = 0, soundNull = 0, runner = 0;
    for (uint32_t i = 0; i < g_registeredCount; ++i)
    {
        if (g_registered[i].type != ASSET_TYPE_FX)
            continue;
        if (g_registered[i].zoneIndex == zoneIndex && g_registered[i].header.fx)
            DumpFxStringVisuals(ff, g_registered[i].header.fx, &sound, &soundNull, &runner);
        if (g_registered[i].overrideZoneIndex == zoneIndex && g_registered[i].overrideHeader.fx)
            DumpFxStringVisuals(ff, g_registered[i].overrideHeader.fx, &sound, &soundNull, &runner);
    }
    if (soundOut)
        *soundOut = sound;
    if (runnerOut)
        *runnerOut = runner;
    return soundNull;
}

// Registered multi-variant sound alias lists, and how many of them give their
// same-named variants distinct aliasName pointers.  The engine matches a
// playing loop by pointer (SND_ContinueLoopingSound), so a split list restarts
// its loop whenever CL_PickSoundAlias picks another variant.
uint32_t StubSoundAliasNameSplitCount(uint32_t *multiVariantListsOut, const char **firstSplitOut)
{
    uint32_t lists = 0;
    uint32_t split = 0;
    if (firstSplitOut)
        *firstSplitOut = nullptr;
    for (uint32_t i = 0; i < g_registeredCount; ++i)
    {
        if (g_registered[i].type != ASSET_TYPE_SOUND || !g_registered[i].header.sound)
            continue;
        const snd_alias_list_t *list = g_registered[i].header.sound;
        if (list->count < 2 || !list->head)
            continue;
        ++lists;
        for (int v = 1; v < list->count; ++v)
        {
            const char *a = list->head[0].aliasName;
            const char *b = list->head[v].aliasName;
            if (a && b && a != b && std::strcmp(a, b) == 0)
            {
                ++split;
                if (firstSplitOut && !*firstSplitOut)
                    *firstSplitOut = a;
                break;
            }
        }
    }
    if (multiVariantListsOut)
        *multiVariantListsOut = lists;
    return split;
}

// per-type census: registration declarations per XAssetType (see
// g_stubRegisterAttempts). A loader that walks a nested body instead of
// calling its original owner shows up here as a zero/missing declaration.
uint32_t StubRegisterAttemptCount(uint32_t type)
{
    return type < 64 ? g_stubRegisterAttempts[type] : 0;
}

// Copy the registered headers of one type into the caller's array for
// pointer-identity audits (: every widened nested LoadedSound/SndCurve
// must be the header the alias graph binds, not an unregistered arena
// object).
uint32_t StubCollectRegistered(XAssetType type, XAssetHeader *out, uint32_t capacity)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_registeredCount; ++i)
    {
        if (g_registered[i].type != type)
            continue;
        if (out && count < capacity)
            out[count] = g_registered[i].header;
        ++count;
    }
    return count;
}

// Test-only stand-in for the handful of engine-lifetime stock assets the
// real boot sequence creates once, before any SP zone loads (R_InitImages'
// "$identitynormalmap" and similar -- Image_LoadIdentityNormalMap et al,
// gfx_d3d/r_image.cpp), which this narrow database-only test never runs
// (no live D3D9 device: the loader creates no renderer resources
// before one exists). Registers with zoneIndex 0 -- the
// same "not owned by any tracked zone" sentinel StubZoneIsLive already
// treats as never-live -- so DB_RetailZoneEnd's per-zone sweep can never
// retire it, exactly matching a real stock asset's process-lifetime, not
// per-zone, ownership.
void StubRegisterStockAsset(XAssetType type, XAssetHeader header)
{
    if (g_registeredCount >= kMaxRegistered)
        return;
    g_registered[g_registeredCount].type = type;
    g_registered[g_registeredCount].header = header;
    g_registered[g_registeredCount].zoneIndex = 0;
    ++g_registeredCount;
}

// Installs the engine default material for the DB_FindXAssetHeader
// default-entry double above; passing nullptr clears the emulation and every
// entry it created, so the rest of the host proof sees the stock contract.
void StubInstallDefaultMaterial(Material *material)
{
    g_stubDefaultMaterial = material;
    if (!material)
        g_stubDefaultEntryCount = 0;
}

uint32_t StubDefaultEntryCount(void)
{
    return g_stubDefaultEntryCount;
}

uint32_t StubOverrideCount(void)
{
    return g_overrideCount;
}

// Retired-pointer audit: registrations still in the table whose owning zone
// is no longer live. DB_RetailZoneEnd's sweep must keep this at zero; any
// nonzero value means a lookup could return a pointer into freed zone
// memory (a use-after-free across unload). Zone 0 is StubRegisterStockAsset's
// permanent, ownerless bucket (process-lifetime engine stock), not a retired
// owner, so it is deliberately outside this audit.
uint32_t StubRetiredPtrCount(void)
{
    uint32_t retired = 0;
    for (uint32_t i = 0; i < g_registeredCount; ++i)
    {
        const uint32_t owner = g_registered[i].zoneIndex;
        if (owner == 0)
            continue;
        if (owner >= kMaxZones || !g_zones[owner].live)
            ++retired;
    }
    return retired;
}

// Highest zone slot handed out so far (1-based; slot 0 is never used).
// Lets the lifetime proof report arena high-water use.
uint32_t StubZoneHighWater(void)
{
    return g_zoneHighWater;
}

// Mirrors the engine's shutdown order: CL_ShutdownRenderer/Com_Close call
// Com_ShutdownWorld (com_bsp_load_obj.cpp:459) before zone unload runs
// DB_RemoveXAssetHandler[ASSET_TYPE_COMWORLD] (db_registry.cpp:3422 ->
// DB_RemoveComWorld). Without this, a host zone end leaves comWorld.isInUse
// naming freed zone memory, and the next zone's LoadComWorld prints the
// dangling name -- an ASan heap-use-after-free the whole-corpus sweep hit.
// Kept in the host double because the real Com_ShutdownWorld lives in
// com_bsp_load_obj.cpp, which this narrow link does not include.
void StubShutdownWorld(void)
{
    comWorld.isInUse = 0;
    comWorld.name = nullptr;
}

uint32_t StubWorldInUse(void)
{
    return comWorld.isInUse ? 1u : 0u;
}

// pointer-provenance generalization: scan every live zone's
// zone-owned arena for raw pointer-sized values that fall inside any
// currently-tracked reader's transient blocks. A hit means the registered
// asset graph retained reader-owned storage that dangles once
// FS_CloseRetailFastfile frees it -- the retained-pointer shape generalized from each
// registration's name to every pointer field of every asset family. The
// close-time hook below runs this while the closing reader is still tracked;
// a negative control calls it directly with a planted pointer.
namespace
{
// One 8-byte-aligned word sweep of a proof-owned range. `tag`/`tagId` name
// the range in detail lines (zone slot or host-heap range index).
uint32_t StubScanProofRange(const uint8_t *base, uint32_t bytes, const char *tag,
                            uint32_t tagId)
{
    uint32_t hits = 0;
    if (!base || bytes < sizeof(uintptr_t))
        return 0;
    for (uint32_t off = 0; off + sizeof(uintptr_t) <= bytes; off += alignof(uintptr_t))
    {
        uintptr_t value = 0;
        std::memcpy(&value, base + off, sizeof(value));
        if (value == 0 ||
            !FS_RetailFastfilePointerIsReaderTransient(reinterpret_cast<const void *>(value)))
            continue;
        ++g_stubReaderOwnedBodyCount;
        ++hits;
        if (g_stubReaderOwnedBodyCount <= 16)
        {
            std::printf("KILLHOUSE_PROVENANCE reader_owned_body %s=%u off=%u value=%p bytes=%u\n",
                        tag, tagId, off, reinterpret_cast<void *>(value), bytes);
            std::fflush(stdout);
        }
    }
    return hits;
}
} // namespace

uint32_t StubScanLiveZonesForReaderTransient(void)
{
    uint32_t hits = 0;
    for (uint32_t z = 1; z < kMaxZones; ++z)
    {
        const StubZone &zone = g_zones[z];
        if (!zone.live || !zone.nativeArena || zone.nativeArenaBytes < sizeof(uintptr_t))
            continue;
        uint32_t used = static_cast<uint32_t>(RetailZoneArenaUsedBytesForProof(z));
        if (used > zone.nativeArenaBytes)
            used = zone.nativeArenaBytes;
        ++g_stubArenaScanZones;
        g_stubArenaScanWords += used / sizeof(uintptr_t);
        hits += StubScanProofRange(static_cast<const uint8_t *>(zone.nativeArena), used,
                                   "zone", z);
    }
    // Registered bodies that live in the host doubles' process-lifetime
    // Material_Alloc/Image_Alloc blocks (see g_stubProofHeapRanges).
    for (uint32_t r = 0; r < g_stubProofHeapRangeCount; ++r)
    {
        const StubProofHeapRange &range = g_stubProofHeapRanges[r];
        ++g_stubProofHeapScanRanges;
        g_stubArenaScanWords += range.size / sizeof(uintptr_t);
        hits += StubScanProofRange(range.base, range.size, "heap", r);
    }
    return hits;
}

void StubReaderTransientArenaAudit(const FsRetailFastfileReader *reader)
{
    (void)reader;
    StubScanLiveZonesForReaderTransient();
}

uint32_t StubReaderOwnedBodyCount(void)
{
    return g_stubReaderOwnedBodyCount;
}

void StubResetReaderOwnedBodyCount(void)
{
    g_stubReaderOwnedBodyCount = 0;
}

uint32_t StubArenaScanZones(void)
{
    return g_stubArenaScanZones;
}

uint64_t StubArenaScanWords(void)
{
    return g_stubArenaScanWords;
}

uint32_t StubProofHeapRangeCount(void)
{
    return g_stubProofHeapRangeCount;
}

uint32_t StubProofHeapOverflow(void)
{
    return g_stubProofHeapRangeOverflow;
}

uint32_t StubProofHeapScanRanges(void)
{
    return g_stubProofHeapScanRanges;
}
