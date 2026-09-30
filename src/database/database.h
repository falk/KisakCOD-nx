#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>

#include <zlib/zlib.h>

#include <xanim/xanim.h>
#include <xanim/xmodel.h>
#include <universal/fast_critical_section.h>
#ifndef __SWITCH__
#include <win32/win_local.h>
#endif
#

extern bool g_anyFastFileLoaded;
#if defined(__SWITCH__)
extern std::atomic<bool> g_switchRetailLoadFailed;
const char *DB_SwitchRetailLoadFailure();
void DB_SwitchClearRetailLoadFailure();
#endif

enum $D93A52C218787A3ED865FD745137F4B3 : int32_t
{
    DM_MEMORY_TEMP = 0x0,
    DM_MEMORY_VIRTUAL = 0x1,
    DM_MEMORY_PHYSICAL = 0x2,
};

enum $A9FDED199653E5E1C469F04B28023DED : int32_t
{
    DB_ZONE_CODE_LOC   = 0,
    DB_ZONE_COMMON_LOC = (1 << 0),
    DB_ZONE_CODE       = (1 << 1),
    DB_ZONE_COMMON     = (1 << 2),
    DB_ZONE_GAME       = (1 << 3),
    DB_ZONE_MOD        = (1 << 4),
    DB_ZONE_LOAD       = (1 << 5),
    DB_ZONE_DEV        = (1 << 6)
};

struct StreamDelayInfo // sizeof=0x8
{
    const void *ptr;
    int32_t size;
};

struct StreamPosInfo // sizeof=0x8
{                                       // ...
    uint8_t *pos;               // ...
    uint32_t index;                 // ...
};

struct AssetList // sizeof=0xC
{                                       // ...
    int32_t assetCount;                     // ...
    int32_t maxCount;                       // ...
    XAssetHeader *assets;               // ...
};

// db_registry
void __cdecl TRACK_db_registry();
char *__cdecl DB_ReferencedFFChecksums();
char *__cdecl DB_ReferencedFFNameList();
void __cdecl Hunk_OverrideDataForFile(int32_t type, const char *name, void *data);
void __cdecl DB_GetIndexBufferAndBase(uint8_t zoneHandle, void *indices, void **ib, int32_t *baseIndex);
void __cdecl DB_GetVertexBufferAndOffset(uint8_t zoneHandle, _BYTE *verts, void **vb, int32_t *vertexOffset);
// Zone block-7/8 sizes for pre-draw bounds checks.
bool __cdecl DB_GetZoneGeometrySizes(uint8_t zoneHandle, uint32_t *vertBytes, uint32_t *indexBytes);
void __cdecl DB_EndRecoverLostDevice();
void __cdecl DB_BeginRecoverLostDevice();
void __cdecl Load_PhysPresetAsset(XAssetHeader *physPreset);
void __cdecl Mark_PhysPresetAsset(struct PhysPreset *physPreset);
void __cdecl Load_XAnimPartsAsset(XAssetHeader *parts);
void __cdecl Mark_XAnimPartsAsset(XAnimParts *parts);
void __cdecl Load_XModelAsset(XAssetHeader *model);
void __cdecl Mark_XModelAsset(XModel *model);
void __cdecl Load_MaterialAsset(XAssetHeader *material);
void __cdecl Mark_MaterialAsset(Material *material);
void __cdecl Load_MaterialTechniqueSetAsset(XAssetHeader *techniqueSet);
void __cdecl Mark_MaterialTechniqueSetAsset(MaterialTechniqueSet *techniqueSet);
void __cdecl Load_GfxImageAsset(XAssetHeader *image);
void __cdecl Mark_GfxImageAsset(GfxImage *image);
void __cdecl Load_snd_alias_list_Asset(XAssetHeader *sound);
void __cdecl Mark_snd_alias_list_Asset(snd_alias_list_t *sound);
void __cdecl Load_SndCurveAsset(XAssetHeader *sndCurve);
void __cdecl Mark_SndCurveAsset(SndCurve *sndCurve);
void __cdecl Load_LoadedSoundAsset(XAssetHeader *loadSnd);
void __cdecl Mark_LoadedSoundAsset(LoadedSound *loadSnd);
void __cdecl Load_ClipMapAsset(XAssetHeader *clipMap);
void __cdecl Mark_ClipMapAsset(clipMap_t *clipMap);
void __cdecl Load_ComWorldAsset(XAssetHeader *comWorld);
void __cdecl Mark_ComWorldAsset(ComWorld *comWorld);
void __cdecl Load_GameWorldSpAsset(XAssetHeader *gameWorldSp);
void __cdecl Mark_GameWorldSpAsset(struct GameWorldSp *gameWorldSp);
void __cdecl Load_GameWorldMpAsset(XAssetHeader *gameWorldMp);
void __cdecl Mark_GameWorldMpAsset(struct GameWorldMp *gameWorldMp);
void __cdecl Load_MapEntsAsset(XAssetHeader *mapEnts);
void __cdecl Mark_MapEntsAsset(MapEnts *mapEnts);
void __cdecl Load_GfxWorldAsset(XAssetHeader *gfxWorld);
void __cdecl Mark_GfxWorldAsset(GfxWorld *gfxWorld);
void __cdecl Load_LightDefAsset(XAssetHeader *lightDef);
void __cdecl Mark_LightDefAsset(GfxLightDef *lightDef);
void __cdecl Load_FontAsset(XAssetHeader *font);
void __cdecl Mark_FontAsset(Font_s *font);
void __cdecl Load_MenuListAsset(XAssetHeader *menuList);
void __cdecl Mark_MenuListAsset(MenuList *menuList);
void __cdecl Load_MenuAsset(XAssetHeader *menu);
void __cdecl Mark_MenuAsset(menuDef_t *menu);
void __cdecl Load_LocalizeEntryAsset(XAssetHeader *localize);
void __cdecl Mark_LocalizeEntryAsset(LocalizeEntry *localize);
void __cdecl Load_WeaponDefAsset(XAssetHeader *weapon);
void __cdecl Mark_WeaponDefAsset(WeaponDef *weapon);
void __cdecl Load_FxEffectDefAsset(XAssetHeader *fx);
void __cdecl Mark_FxEffectDefAsset(FxEffectDef *fx);
void __cdecl Load_FxEffectDefFromName(const char **name);
void __cdecl Load_FxImpactTableAsset(XAssetHeader *impactFx);
void __cdecl Mark_FxImpactTableAsset(FxImpactTable *impactFx);
void __cdecl Load_RawFileAsset(XAssetHeader *rawfile);
void __cdecl Mark_RawFileAsset(RawFile *rawfile);
void __cdecl Load_StringTableAsset(XAssetHeader *stringTable);
void __cdecl Mark_StringTableAsset(StringTable *stringTable);
void __cdecl DB_EnumXAssets_FastFile(
    XAssetType type,
    void(__cdecl *func)(XAssetHeader, void *),
    void *inData,
    bool includeOverride);
bool __cdecl DB_IsMinimumFastFileLoaded();
XAssetHeader __cdecl DB_FindXAssetHeader(XAssetType type, const char *name);
// Unlike DB_FindXAssetHeader, this is safe for a negative lookup: the latter
// may synthesize a default for missing asset types such as Font.
bool __cdecl DB_XAssetExists(XAssetType type, const char *name);
void __cdecl DB_Update();
void __cdecl DB_SetInitializing(bool inUse);
bool __cdecl DB_IsXAssetDefault(XAssetType type, const char *name);
int32_t __cdecl DB_GetAllXAssetOfType_FastFile(XAssetType type, XAssetHeader *assets, int32_t maxCount);
void __cdecl DB_UpdateDebugZone();
void __cdecl DB_SyncXAssets();
void __cdecl DB_LoadXAssets(XZoneInfo *zoneInfo, uint32_t zoneCount, int32_t sync);
// Initializes the existing asset pools and registry exactly once.  This is a
// narrow entry point for widened (LP64) asset loaders; it does not load a
// zone or touch renderer resources.
void __cdecl DB_InitializeRegistry();
// Retail SP zone lifetime.  These helpers are the sole bridge from the
// widened reader to the real XZone/asset registry.  A load receives a
// nonzero zone slot and must end it explicitly on success or abort.
// *nativeArenaBytes is the arena size wanted on entry and the size reserved
// on return: the reservation is clamped to the free pool, so a zone that
// needs more fails loudly with out_of_arena instead of aborting in PMem.
bool DB_RetailZoneBegin(const char *name, int32_t flags, const uint32_t blockSizes[9],
                        uint32_t *nativeArenaBytes, uint32_t *zoneIndex,
                        XZoneMemory **zoneMemory, void **nativeArenaMemory);
// After a successful load: return the unused tail of the zone's native arena
// (reserved - keepBytes) to the pool. Only possible while the arena is still
// the pool's last allocation; returns false (nothing changed) otherwise.
bool DB_RetailZoneTrimNativeArena(uint32_t zoneIndex, void *nativeArenaMemory,
                                  uint32_t reservedBytes, uint32_t keepBytes);
XAssetHeader DB_RetailZoneRegister(XAssetType type, XAssetHeader header, uint32_t zoneIndex);
bool DB_RetailZoneEnd(uint32_t zoneIndex);
// Retail reload idempotency (PMem retention fix): retail zones begin with
// flags=0, so no freeFlags unload ever retires them -- every R_Init reload
// of an already-resident graphics zone used to leak a full zone's PMem
// (blocks + native arena) until `PMem_Alloc: Need more bytes of ram`
// aborted the run.  A zone's bytes are deterministic from its file, so
// reloading a resident zone is pure waste; RetailWalkLoadZoneAssets skips
// it when a live zone with the same name AND the same load policy is
// found.  Policy matters because bounded (first-BSP-frame) loads register
// only the world closure while full loads register everything: serving a
// bounded-partial zone to a full request would drop RawFiles the caller
// needs.  Returns the live slot, or 0 when no usable resident exists.
uint32_t DB_RetailZoneFindLive(const char *name, bool bounded);
// Records which policy a successfully loaded retail zone used.  Called by
// RetailWalkLoadZoneAssets on success only; cleared whenever the slot is
// (re)initialized or retired so a reused slot never misdescribes its zone.
void DB_RetailZoneNoteLoadPolicy(uint32_t zoneIndex, bool bounded);
// Retires the retail zone that owns the resident CLIPMAP asset so the next
// CM_LoadMap re-decodes it.  Com_Restart zeroes `cm` (CM_Shutdown) but keeps
// retail zone memory; the reload-idempotency skip would otherwise hand back
// the stale zeroed clipmap.  Call from Com_Restart after CM_Shutdown.
void DB_RetailZoneRetireClipMapZone();
// Publishes the zone's block-7 (vertex) and block-8 (index) bytes to the D3D
// geometry buffers DB_AllocXZoneMemory created for this zone, then unlocks
// them.  The normal DB_LoadXFileInternal path does this incrementally while
// streaming (DB_SetStreamIndex -> DB_CloneStreamData into
// XZoneMemory::lockedVertexData/lockedIndexData) and unlocks once in
// DB_FinishGeometryBlocks; the retail walker streams block 7/8 straight into
// blocks[7]/blocks[8].data instead, so the same publish happens once here at
// the end of a successful load.  Without it the buffers
// DB_GetVertexBufferAndOffset/DB_GetIndexBufferAndBase hand the static-model
// draw path stay uninitialized and still mapped.
void __cdecl DB_RetailZoneUploadGeometryBuffers(uint32_t zoneIndex);
// Registers a widened runtime technique set in the existing database.  Shader
// remapping/upload is intentionally left to the renderer integration seam.
MaterialTechniqueSet *__cdecl DB_RegisterMaterialTechniqueSet(MaterialTechniqueSet *techniqueSet);
// Registers a widened runtime material in the existing database.  Technique
// set and image dependencies must already be registered; renderer resources
// are owned by the later renderer seam.
Material *__cdecl DB_RegisterMaterial(Material *material);
// Registers a runtime-loaded GfxImage in the existing database after the
// engine image path created its D3D texture.
GfxImage *__cdecl DB_RegisterImage(GfxImage *image);
XAssetHeader DB_FindXAssetHeaderNoDefault(XAssetType type, const char *name);
void __cdecl DB_InitThread();
void __cdecl DB_ReleaseXAssets();
void __cdecl DB_ShutdownXAssets();
void __cdecl DB_ReplaceModel(const char *original, const char *replacement);
void __cdecl DB_Cleanup();

void __cdecl DB_EnumXAssets_FastFile(
    XAssetType type,
    void(__cdecl* func)(XAssetHeader, void*),
    void* inData,
    bool includeOverride);

int32_t __cdecl DB_GetAllXAssetOfType_FastFile(XAssetType type, XAssetHeader* assets, int32_t maxCount);
int32_t __cdecl DB_GetAllXAssetOfType(XAssetType type, XAssetHeader* assets, int32_t maxCount);

struct fileData_s;
void __cdecl DB_EnumXAssets(
    XAssetType type,
    void(__cdecl* func)(XAssetHeader, void*),
    void* inData,
    bool includeOverride);

void __cdecl DB_EnumXAssetsFor(
    fileData_s* fileData,
    int32_t fileDataType,
    void(* func)(void*, void*),
    void* inData);

int32_t __cdecl DB_FileSize(const char *zoneName, int32_t isMod);
bool __cdecl DB_ModFileExists();

void __cdecl Load_GetCurrentZoneHandle(uint8_t *handle);


// db_assetnames
const char *__cdecl DB_GetXAssetHeaderName(int32_t type, const XAssetHeader *header);
const char *__cdecl DB_GetXAssetName(const XAsset *asset);
void __cdecl DB_SetXAssetName(XAsset *asset, const char *name);
int32_t __cdecl DB_GetXAssetTypeSize(int32_t type);
const char *__cdecl DB_GetXAssetTypeName(uint32_t type);


// db_auth
int32_t __cdecl DB_AuthLoad_InflateInit(z_stream_s *stream, bool isSecure);
void __cdecl DB_AuthLoad_InflateEnd(z_stream_s *stream);
uint32_t __cdecl DB_AuthLoad_Inflate(z_stream_s *stream, int32_t flush);


// db_file_load
void __cdecl DB_LoadedExternalData(int32_t size);
double __cdecl DB_GetLoadedFraction();
void __cdecl DB_LoadXFileData(uint8_t *pos, uint32_t size);
void __stdcall DB_FileReadCompletion(
    uint32_t dwErrorCode,
    uint32_t dwNumberOfBytesTransfered,
    _OVERLAPPED *lpOverlapped);
void __cdecl DB_LoadXFileInternal();
void __cdecl DB_ResetZoneSize(int32_t trackLoadProgress);
void __cdecl DB_LoadXFile(
    const char *path,
    void *f,
    const char *filename,
    XZoneMemory *zoneMem,
    void(__cdecl *interrupt)(),
    uint8_t *buf,
    int32_t allocType);

// db_memory
void __cdecl DB_RecoverGeometryBuffers(XZoneMemory *zoneMem);
void __cdecl DB_ReleaseGeometryBuffers(XZoneMemory *zoneMem);
void __cdecl DB_AllocXZoneMemory(
    uint32_t *blockSize,
    const char *filename,
    XZoneMemory *zoneMem,
    uint32_t allocType);


// db_stream
void __cdecl DB_InitStreams(XZoneMemory *zoneMem);
void __cdecl DB_PushStreamPos(uint32_t index);
void __cdecl DB_PopStreamPos();
uint8_t *__cdecl DB_GetStreamPos();
uint8_t *__cdecl DB_AllocStreamPos(int32_t alignment);
void __cdecl DB_IncStreamPos(int32_t size);
const void **__cdecl DB_InsertPointer();

// db_stream_load
void __cdecl Load_Stream(bool atStreamStart, uint8_t *ptr, int32_t size);
void __cdecl Load_DelayStream();
void __cdecl DB_ConvertOffsetToAlias(uint32_t *data);
void __cdecl DB_ConvertOffsetToPointer(uint32_t *data);
void __cdecl Load_XStringCustom(char **str);
void __cdecl Load_TempStringCustom(char **str);

// db_file_load: the original zone-load tail (DB_LoadXFileInternal runs it
// right after Load_DelayStream) -- sweeps every registered image asset and
// loads the pixels of anything still flagged delayLoadPixels through
// R_DelayLoadImage -> Image_LoadFromFile (images/<name>.iwi via FS/IWD).
void __cdecl DB_LoadDelayedImages();

// db_stringtable_load
void __cdecl Load_ScriptStringCustom(uint16_t *var);
void __cdecl Mark_ScriptStringCustom(uint16_t *var);


// db_load
void __cdecl Load_ScriptStringList(bool atStreamStart);
XAsset *__cdecl AllocLoad_FxElemVisStateSample();
void __cdecl Load_XAsset(bool atStreamStart);
void __cdecl Mark_XAsset();
void __cdecl DB_LoadDObjs();

extern const char *g_assetNames[ASSET_TYPE_COUNT];

extern XAssetEntry *g_copyInfo[0x800];
extern uint32_t g_copyInfoCount;

extern volatile uint32_t g_loadingAssets;

extern XAssetList *varXAssetList;

extern struct fileData_s *com_fileDataHashTable[1024];

extern uint32_t volatile g_mainThreadBlocked;

extern uint32_t g_streamDelayIndex;
extern XBlock *g_streamBlocks;
extern uint8_t *g_streamPosArray[9];
extern StreamDelayInfo g_streamDelayArray[4096];
extern uint32_t g_streamPosIndex;
extern StreamPosInfo g_streamPosStack[64];
extern XZoneMemory *g_streamZoneMem;
extern uint8_t *g_streamPos;
extern uint32_t g_streamPosStackIndex;

extern XAsset *varXAsset;

extern FastCriticalSection db_hashCritSect;

// Changes whenever a by-name registry lookup may return something else
// (db_registry.cpp); 0 while reorder logging needs every lookup. For
// NamePtrCache memos of DB_FindXAssetHeader results.
uint32_t DB_AssetLookupGeneration();

extern ScriptStringList *varScriptStringList;
