#pragma once

#include <cstddef>
#include <cstdint>

#include "../universal/com_files.h"

struct RetailZoneLoadSession;
struct GfxImage;
struct Material;
struct XModel;
struct FxElemMarkVisuals;
struct RetailWorldLoadContext;

enum RetailWalkDirectoryState
{
    RETAIL_WALK_DEFERRED,
    RETAIL_WALK_NO_STREAM,
    RETAIL_WALK_WALKED_DEFERRED,
};

struct RetailWalkDirectoryRecord
{
    uint32_t ordinal;
    uint32_t type;
    uint32_t header;
    RetailWalkDirectoryState state;
    uint32_t bodyBytes;
    uint32_t nameBytes;
    uint32_t nestedReferenceCount;
    uint32_t nestedBodyBytes;
    uint32_t nestedPassCount;
    // Populated by ReadRetailImageBody (and propagated through
    // ReadRetailAssetSlotBody for RETAIL_WALK_NESTED_IMAGE): the walked
    // image's name, encoded exactly like a live-read GfxImage's resolved
    // nameRef (a fresh inline name's block-4 offset reference, an existing
    // alias's own raw reference passed through unchanged, or 0 for
    // anonymous). Lets a caller that also needs the name for a live-side
    // effect (the shared-image pool write-back in ReadRetailMaterialTable)
    // avoid re-parsing the image body a second time through the live
    // reader, which would double-consume its stream bytes.
    uint32_t resolvedNameRef;
    // Live-nested widening: when the session allows it
    // (widenNestedImages, live loads only), ReadRetailImageBody widens the
    // just-walked inline image into a zone-owned registered GfxImage through
    // RetailWalkWidenNestedImage and records it here; ReadRetailMaterialTable
    // stores it into the session image pool beside the FS pool write-back so
    // later aliases bind it. Null in walk-only proofs and when widening is
    // skipped or fails soft. Propagated through ReadRetailAssetSlotBody like
    // resolvedNameRef.
    GfxImage *widenedImage;
};

enum RetailWalkDirectoryResultCode
{
    RETAIL_WALK_OK,
    RETAIL_WALK_BAD_ARGUMENT,
    RETAIL_WALK_OPEN_FAILED,
    RETAIL_WALK_CAPACITY,
    RETAIL_WALK_LIST_FAILED,
    RETAIL_WALK_SESSION_FAILED,
    RETAIL_WALK_UNSUPPORTED_TYPE,
    RETAIL_WALK_BODY_FAILED,
};

struct RetailWalkDirectoryResult
{
    RetailWalkDirectoryResultCode code;
    uint32_t directoryOffset;
    uint32_t assetCount;
    uint32_t recordedCount;
    uint32_t deferredCount;
    uint32_t noStreamCount;
    uint32_t walkedDeferredCount;
    uint32_t walkedLocalizeCount;
    uint32_t walkedTechniqueCount;
    uint32_t walkedPassCount;
    uint32_t walkedShaderCount;
    uint32_t walkedShaderProgramBytes;
    uint32_t walkedArgumentCount;
    uint32_t walkedArgumentLiteralCount;
    uint32_t walkedMaterialCount;
    uint32_t walkedMaterialTextureCount;
    uint32_t walkedImageCount;
    uint32_t walkedImagePayloadBytes;
    uint32_t walkedFontCount;
    uint32_t walkedRawFileCount;
    uint32_t walkedStringTableCount;
    uint32_t walkedPhysPresetCount;
    uint32_t walkedSndCurveCount;
    uint32_t walkedLightDefCount;
    uint32_t walkedSoundCount;
    uint32_t walkedLoadedSoundCount;
    uint32_t walkedLoadedSoundDataBytes;
    uint32_t walkedSpeakerMapCount;
    uint32_t walkedFxCount;
    uint32_t walkedImpactFxCount;
    uint32_t walkedMenuListCount;
    uint32_t walkedMenuCount;
    uint32_t walkedItemDefCount;
    uint32_t walkedXAnimCount;
    uint32_t walkedXModelCount;
    uint32_t walkedXModelSurfaceCount;
    uint32_t walkedWeaponCount;
    uint32_t walkedMapEntsCount;
    uint32_t walkedComWorldCount;
    uint32_t walkedGameWorldSpCount;
    uint32_t walkedGameWorldSpBlock1Bytes;
    uint32_t walkedClipMapCount;
    uint32_t walkedClipMapBlock1Bytes;
    uint32_t walkedGfxWorldCount;
    uint32_t walkedGfxWorldBlock1Bytes;
    // Dependency closure: assets walked inside a GfxWorld subtree,
    // accumulated as snapshot deltas (killhouse has one world; N worlds
    // accumulate). Materials/images/techsets/lightdefs are R_LoadWorld
    // activation; XModels are gate-aStaticModels exempt (static draws are gated off); cells/portals/surfaces are the DPVS visibility mechanism
    // with surfaces doubling as the draw candidate pool.
    uint32_t walkedGfxWorldMaterials;
    uint32_t walkedGfxWorldImages;
    uint32_t walkedGfxWorldTechSets;
    uint32_t walkedGfxWorldLightDefs;
    uint32_t walkedGfxWorldXModels;
    uint32_t walkedGfxWorldPasses;
    uint32_t walkedGfxCellCount;
    uint32_t walkedGfxPortalCount;
    uint32_t walkedGfxSurfaceCount;
    // M3e diagnostics: wire cursors at walk end (set on both success and
    // body failure, before teardown) and the directory ordinal that failed
    // its body walk (UINT32_MAX when no body failed).
    uint32_t endCursor[9]{};
    uint32_t failedOrdinal = UINT32_MAX;
    // Deepest nested asset walk that failed (kind + wire reference,
    // UINT32_MAX/0 when none failed). Top-level ordinal failures leave
    // these clear; nested walks set them first-failure-wins.
    uint32_t failedNestedKind = UINT32_MAX;
    uint32_t failedNestedRef = 0;
    uint32_t lastGfxPortalOffset = 0;
    uint32_t lastGfxPortalCount = 0;
    uint32_t gfxPortalCellOffsets[256]{};
    uint32_t gfxPortalCellRefCount = 0;
    uint32_t gfxTopPortalCellRefs[256]{};
    uint32_t gfxTopPortalRefCount = 0;
    uint32_t gfxTopPortalChildOffsets[256]{};
};

// Read serialized bytes from the FS stream directly into the session's
// reserved wire block. A short read rewinds that block cursor and fails.
bool RetailZoneLoadSessionReadStream(RetailZoneLoadSession *session,
                                     FsRetailFastfileReader *reader, uint32_t block,
                                     uint32_t bytes, uint32_t alignment);

// Import only the reader suffix not already present in the zone mirror.
bool RetailZoneLoadSessionSyncFromReader(RetailZoneLoadSession *session,
                                         FsRetailFastfileReader *reader,
                                         uint32_t block);

// Live-nested image widening hook: widens one just-walked
// inline image into a zone-owned registered GfxImage, or returns null when
// widening is skipped or fails soft (the walk itself never fails for this).
// Weak no-op by default so the walk-only test harness links the walk
// without the image decoder; the real decoder TU overrides it. Callers
// gate on session->widenNestedImages (live loads only).
// Hook, strong in sound/snd_driver_openal.cpp: copy an embedded
// LoadedSound payload out of the rewinding block 0 while its bytes are still
// valid, decoding IMA ADPCM (wave format 17) to 16-bit PCM.  Returns the
// sound-memory copy and the resulting format/bits/length; the weak default
// (walk-only and host links) returns null.
uint8_t *RetailWalkRetainLoadedSoundData(const uint8_t *root44, const uint8_t *data,
                                         uint32_t dataLen, uint32_t *outLen,
                                         uint32_t *outFormat, uint32_t *outBits);
GfxImage *RetailWalkWidenNestedImage(RetailZoneLoadSession *session,
                                     FsRetailFastfileReader *reader,
                                     const FsRetailFastfileImage &wire);

// Reserve runtime-only block-1 memory without consuming FS stream bytes.
// The span is zero-filled and the block-1 cursor advances exactly like a
// stream read; over-size or non-block-1 requests fail with the cursor
// untouched. Only block 1 may expand: blocks 2/3 are delay-streamed and
// unimplemented by design.
bool RetailZoneLoadSessionExpandRuntime(RetailZoneLoadSession *session, uint32_t block,
                                        uint32_t bytes, uint32_t alignment);

// Consume an already-read XAssetList directory in serialized order. This only
// records deferred/no-stream entries; it never registers or activates assets.
RetailWalkDirectoryResultCode RetailWireReadAssetDirectory(
    const FsRetailFastfileAssetList *list, const FsRetailFastfileAsset *assets,
    uint32_t assetCapacity, RetailWalkDirectoryRecord *records, uint32_t capacity,
    RetailWalkDirectoryResult *result);

// Walk-only family bodies reused as stream consumers by the live GfxWorld
// driver (db_retail_decode_world.cpp): an inline XModel body inside a
// world (gate-aStaticModels deferred) and cell-graph subtrees
// must still be consumed in exact stream order. Both take throwaway
// record/summary structs when only consumption (not accounting) is needed.
bool ReadRetailXModelBody(RetailZoneLoadSession *session,
                          FsRetailFastfileReader *reader,
                          RetailWalkDirectoryRecord *record,
                          RetailWalkDirectoryResult *summary);
// Nested block-4 spans captured for the live FX widener. Offset fields use
// UINT32_MAX when no inline body was streamed. With a world context, visual
// asset slots are widened and recorded for later aliases; walk-only callers
// leave the visual pointers null.
struct RetailWalkFxElemOffsets
{
    uint32_t record;         // block-4 start of this 252-byte elem record, as streamed
    uint32_t velSamples;     // (velIntervalCount+1)*96 span
    uint32_t visSamples;     // (visStateIntervalCount+1)*48 span
    uint32_t effectOnImpact; // inline effect-name string start
    uint32_t effectOnDeath;
    uint32_t effectEmitted;
    uint32_t trailDef;       // 28-byte trail record start
    uint32_t trailVerts;     // trail vertCount*20 span
    uint32_t trailInds;      // trail indCount*2 span
    Material *visualMaterial;         // default/single visual (visualCount<=1)
    Material **visualMaterialArray;   // default/array visual, length visualCount
    FxElemMarkVisuals *visualMarks;   // elemType==9 marks, length visualCount
    XModel *visualModel;              // elemType==5 single visual
    XModel **visualModelArray;        // elemType==5 array visual, length visualCount
    // String visuals: elemType==10 (runner) effect names, which the original
    // Load_FxEffectDefRef resolves to FxEffectDef handles in place, and
    // elemType==8 (sound) alias names, kept as strings. These are the block-4
    // string starts (UINT32_MAX = none), single/array.
    uint32_t visualEffectName;
    uint32_t *visualEffectNameArray;  // length visualCount
};

// Consume one FX body. Optional outputs capture arena-owned per-element data
// and live-widen visual asset slots through the shared world context.
bool ReadRetailFxEffectDefBody(RetailZoneLoadSession *session,
                               FsRetailFastfileReader *reader,
                               RetailWalkDirectoryRecord *record,
                               RetailWalkDirectoryResult *summary,
                               RetailWalkFxElemOffsets **elemOffsetsOut = nullptr,
                               RetailWorldLoadContext *worldContext = nullptr);
// Per-alias nested wire offsets the walk reader already streamed into the
// session mirror, captured for the live B2 widener: sound file, falloff
// curve, and speaker map bodies plus the inline string starts they own.
// UINT32_MAX means the slot was null/alias (nothing streamed).
//
// `loadedSoundRootBytes`/`curveRootBytes` are arena-owned copies of the
// 44-byte LoadedSound / 72-byte SndCurve roots taken as the walk consumed
// them. They are mandatory for live widening: block 0 is the rewinding temp
// block, so a later nested body in the same list overwrites an earlier
// root's bytes at the same offset before the live widener runs (e.g.
// common.ff `helicopter1` read knotCount=1 from a following LoadedSound
// root). Null means no copy was taken; the widener then falls back to the
// recorded block-0 offset (top-level and synthetic-mirror callers, whose
// bytes are still intact at widen time).
struct RetailWalkSndAliasOffsets
{
    uint32_t name[4];         // four alias name strings (block-4 starts/offsets)
    uint32_t soundFile;       // block-4 12-byte SoundFile record
    uint32_t soundFileDir;    // block-4 inline streamed dir string
    uint32_t soundFileName;   // block-4 inline streamed name string
    uint32_t loadedSoundRoot; // block-0 44-byte LoadedSound root (type 1)
    uint32_t loadedSoundName; // block-4 inline LoadedSound name string
    uint32_t loadedSoundData; // block-0 embedded data bytes
    uint32_t loadedSoundDataLen;
    uint32_t curveRoot;       // block-0 72-byte SndCurve root
    uint32_t curveName;       // block-4 inline curve filename
    uint32_t speakerMap;      // block-4 408-byte SpeakerMap root
    uint32_t speakerMapName;  // block-4 inline speaker-map name
    // alias targets: the block-4 DB_InsertPointer slots an inline/insert
    // nested body reserved. The original loader patches each slot with the
    // registered pointer, so an alias-form sibling (DB_ConvertOffsetToAlias)
    // resolves through it. UINT32_MAX = no insert slot (inline or null form).
    uint32_t loadedSoundInsertSlot;
    uint32_t curveInsertSlot;
    const uint8_t *loadedSoundRootBytes; // arena copy of the LoadedSound root
    // the embedded PCM/ADPCM payload retained out of block 0 at walk
    // time by RetailWalkRetainLoadedSoundData (sound memory, decoded to
    // 16-bit PCM); null when no sound backend is linked (host proofs).
    uint8_t *loadedSoundRetained;
    uint32_t loadedSoundRetainedLen;
    uint32_t loadedSoundRetainedFormat;
    uint32_t loadedSoundRetainedBits;
    const uint8_t *curveRootBytes;       // arena copy of the SndCurve root
};

// B2 snd_alias_list_t body consumption (12-byte root + name + inline 92-byte
// alias records + nested sound-file/curve/speaker-map bodies), shared with
// the live sound identity widener. When `aliasNameOffsetsOut` is non-null the
// call allocates a 4-per-alias offset table (arena-owned) the live widener
// uses to point at the exact string bytes the oracle landed.
// `aliasOffsetsOut`, when non-null, receives an arena-owned per-alias
// nested-offset table (allocated by this call) the live widener reads.
bool ReadRetailSndAliasListBody(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader,
                                RetailWalkDirectoryRecord *record,
                                RetailWalkDirectoryResult *summary,
                                uint32_t **aliasNameOffsetsOut = nullptr,
                                RetailWalkSndAliasOffsets **aliasOffsetsOut = nullptr);
// B1 small roots: the optional out params expose the already-streamed body
// offsets (block-0 root and block-4 inline string starts, UINT32_MAX for
// alias/null) for the live PhysPreset/SndCurve wideners.
bool ReadRetailSndCurveBody(RetailZoneLoadSession *session,
                            FsRetailFastfileReader *reader,
                            RetailWalkDirectoryRecord *record,
                            RetailWalkDirectoryResult *summary,
                            uint32_t *rootStartOut = nullptr,
                            uint32_t *nameStartOut = nullptr);
bool ReadRetailPhysPresetBody(RetailZoneLoadSession *session,
                              FsRetailFastfileReader *reader,
                              RetailWalkDirectoryRecord *record,
                              RetailWalkDirectoryResult *summary,
                              uint32_t *rootStartOut = nullptr,
                              uint32_t *nameStartOut = nullptr,
                              uint32_t *destNameStartOut = nullptr);
// PhysGeomList consumption (root already streamed; `offset` = the array's
// block-4 landing site): geom array plus any inline BrushWrapper bodies.
bool ReadRetailPhysGeomListBody(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader, uint32_t offset);
// HandleAssetSlot semantics (Android Load_*Ptr): a zero slot is empty, -1/-2
// stream the nested body in its own temp scope (-2 first reserving the 4-byte
// block-4 insert slot), and any other value is an already-loaded alias that
// consumes no stream bytes. Shared with live drivers (B7 WeaponDef) so
// inline nested bodies of deferred assets are consumed exactly.
enum RetailWalkNestedKind
{
    RETAIL_WALK_NESTED_TECHNIQUE_SET,
    RETAIL_WALK_NESTED_MATERIAL,
    RETAIL_WALK_NESTED_IMAGE,
    RETAIL_WALK_NESTED_LOADED_SOUND,
    RETAIL_WALK_NESTED_SND_CURVE,
    RETAIL_WALK_NESTED_FX_EFFECT_DEF,
    RETAIL_WALK_NESTED_MENU_DEF,
    RETAIL_WALK_NESTED_SOUND_ALIAS_LIST,
    RETAIL_WALK_NESTED_XMODEL,
    RETAIL_WALK_NESTED_PHYS_PRESET,
    RETAIL_WALK_NESTED_MAP_ENTS,
    RETAIL_WALK_NESTED_LIGHT_DEF,
};

bool ReadRetailAssetSlotBody(RetailZoneLoadSession *session,
                             FsRetailFastfileReader *reader, uint32_t reference,
                             RetailWalkNestedKind kind,
                             RetailWalkDirectoryRecord *record,
                             RetailWalkDirectoryResult *summary);
// Stream one inline-or-alias XString (0 = null, -1 = inline bytes here, any
// other value = block-4 offset to already-landed bytes); `bytes` receives
// the streamed length. Shared by the walk-only reader and live decoders.
bool ReadRetailXString(RetailZoneLoadSession *session,
                       FsRetailFastfileReader *reader, uint32_t reference,
                       uint32_t *bytes);
// Resolve a non-inline XString reference to the linker-deduplicated string in
// the reader's block-4 window and copy it into `buffer` (NUL-terminated).
// This is Android Load_XString's DB_ConvertOffsetToAlias content path: the
// alias target is an already-streamed string, not bytes at this body's
// stream position, which is why an inline-only copy silently registers "".
// An empty alias target is real (the pointer names an empty string slot) and
// resolves to "". Fails loudly on a non-window/non-offset reference, a
// non-printable byte, an unterminated run, or an out-of-range offset.
// Callers that register the returned name must keep the zone-owned copy they
// made here, never the reader's transient window pointer.
bool ReadRetailXStringAlias(FsRetailFastfileReader *reader, uint32_t reference,
                            char *buffer, uint32_t bufferSize);
bool ReadRetailXStringPtrSlot(RetailZoneLoadSession *session,
                              FsRetailFastfileReader *reader, uint32_t block,
                              uint32_t offset);
bool ReadRetailMaterialTable(RetailZoneLoadSession *session,
                             FsRetailFastfileReader *reader,
                             uint32_t reference, uint32_t count,
                             uint32_t elementBytes, uint32_t alignment,
                             bool textureTable,
                             RetailWalkDirectoryRecord *record,
                             RetailWalkDirectoryResult *summary);
bool ReadRetailCPlaneSlot(RetailZoneLoadSession *session,
                          FsRetailFastfileReader *reader, uint32_t slotBlock,
                          uint32_t slotOffset, uint32_t streamBlock,
                          uint32_t count,
                          RetailWalkDirectoryRecord *record);
// cell-graph placement trace: records where the walk-only cell
// recursion placed every cell-graph span in zone block 4, in exact
// recursion order, so the live widener (db_retail_decode_world.cpp) can
// widen native GfxCell/GfxAabbTree/GfxPortal objects from recorded
// offsets instead of re-deriving the walk's cursor arithmetic. A null
// trace disables recording and changes no walk behavior; every entry the
// widener consumes is self-checked against the record it expects, so a
// walk/widen divergence fails loudly instead of widening wrong bytes.
struct RetailGfxCellTrace
{
    enum Kind : uint32_t
    {
        CELL = 0,             // 56-byte GfxCell record at offset
        TREE_ARRAY = 1,       // aabbCount 44-byte GfxAabbTree records at offset
        TREE_LIST = 2,        // count u16 smodelIndexes streamed at offset
        TREE_LIST_ALIAS = 3,  // alias ref names an earlier TREE_LIST offset
        PORTAL_ARRAY = 4,     // portalCount 68-byte GfxPortal records at offset
        PORTAL_CELL_INLINE = 5, // recursive inline cell record at offset
        PORTAL_CELL_ALIAS = 6,  // alias ref names an earlier CELL offset
        PORTAL_VERTS = 7,     // vertexCount 12-byte vertices at offset
        CULL_INTS = 8,        // cullGroupCount ints at offset
        PROBE_BYTES = 9       // reflectionProbeCount bytes at offset
    };
    struct Entry
    {
        uint32_t kind;
        uint32_t offset; // block-4 byte offset (0 for aliases)
        uint32_t count;  // element count (0 for aliases)
        uint32_t ref;    // raw wire reference (aliases only)
    };
    Entry entries[65536]{};
    uint32_t count = 0;
    bool overflowed = false;

    void Record(uint32_t kind, uint32_t offset, uint32_t count, uint32_t ref)
    {
        if (this->count == sizeof(entries) / sizeof(entries[0]))
        {
            overflowed = true;
            return;
        }
        entries[this->count].kind = kind;
        entries[this->count].offset = offset;
        entries[this->count].count = count;
        entries[this->count].ref = ref;
        ++this->count;
    }
};

bool ReadRetailGfxCellAt(RetailZoneLoadSession *session,
                         FsRetailFastfileReader *reader, uint32_t cellOffset,
                         uint32_t *budget,
                         RetailWalkDirectoryRecord *record,
                         RetailWalkDirectoryResult *summary,
                         RetailGfxCellTrace *trace = nullptr);

// Opens an FS-owned retail fastfile, creates a real retail zone session
// from its nine block sizes, and records its XAsset directory in serialized
// order without walking bodies. On success the session is left ACTIVE and
// the reader OPEN: the caller owns both, inspects what it needs, then tears
// down with RetailZoneLoadSessionAbort plus FS_CloseRetailFastfile (and
// frees the returned asset array). On any failure this already tears
// everything down itself.
RetailWalkDirectoryResultCode RetailWalkOpenDirectory(
    const char *filename, RetailZoneLoadSession *session,
    FsRetailFastfileReader **outReader, FsRetailFastfileAsset **outAssets,
    RetailWalkDirectoryRecord *records, uint32_t capacity,
    RetailWalkDirectoryResult *result, std::size_t nativeArenaBytes = 4096);

// Opens an FS-owned retail fastfile, creates and tears down a real retail
// zone session from its nine block sizes, and records its XAsset directory in
// serialized order.  Asset bodies follow the engine's LoadAssetHeader stream
// semantics: every inline/insert body streams into the rewinding temp block
// (block 0) inside its own scope, nested tables/names/programs land in the
// persistent block 4, insert headers reserve their block-4 slot, and
// StringTable roots stay in block 4 without a temp push.  Every other inline
// body fails loudly until its Android-derived family walker is added. This
// entrypoint never registers or activates an asset.
RetailWalkDirectoryResultCode RetailWalkFastfileDirectory(
    const char *filename, RetailWalkDirectoryRecord *records, uint32_t capacity,
    RetailWalkDirectoryResult *result);

enum RetailWalkLoadZoneResultCode
{
    RETAIL_WALK_LOAD_OK,
    RETAIL_WALK_LOAD_BAD_ARGUMENT,
    RETAIL_WALK_LOAD_OPEN_FAILED,
    RETAIL_WALK_LOAD_SESSION_FAILED,
    RETAIL_WALK_LOAD_LIST_FAILED,
    RETAIL_WALK_LOAD_CAPACITY,
    RETAIL_WALK_LOAD_BODY_FAILED,
    RETAIL_WALK_LOAD_UNSUPPORTED_ASSET,
};

struct RetailWalkLoadZoneResult
{
    RetailWalkLoadZoneResultCode code;
    uint32_t assetCount;
    uint32_t registeredFontCount;
    uint32_t registeredLocalizeCount;
    uint32_t registeredRawFileCount;
    uint32_t registeredStringTableCount;
    uint32_t registeredTechniqueSetCount;
    uint32_t registeredImageCount;
    uint32_t registeredMaterialCount;
    uint32_t registeredMenuListCount;
    uint32_t registeredLightDefCount;
    uint32_t registeredComWorldCount;
    // Native GameWorldSp path data (pathnodes/links/chains/vis/tree) is
    // authored in the zone's singleton body, not the entity string; the
    // live decoder publishes the engine's gameWorldSp so G_SpawnPathnode*
    // matches the compiled nodeCount instead of reporting extras.
    uint32_t registeredGameWorldSpCount;
    uint32_t registeredGfxWorldCount;
    uint32_t registeredMapEntsCount;
    uint32_t registeredXModelCount;
    // B6 FxEffectDef identity: live-registered inline FX roots plus the
    // nested elem objects (samples/visuals/effect refs/trail) that are
    // consumed byte-exactly but explicitly deferred past identity.
    uint32_t registeredFxCount;
    uint32_t fxDeferredNestedCount;
    // B6 ImpactFx: live-registered inline ImpactFx tables (every non-null
    // cell resolved through the zone nested table / FX widener).
    uint32_t registeredImpactFxCount;
    // B2 snd_alias_list_t identity: live-registered inline sound roots plus
    // the nested alias sub-objects (sound files/falloff curves/speaker maps)
    // consumed byte-exactly but explicitly deferred past identity.
    uint32_t registeredSoundCount;
    uint32_t soundDeferredNestedCount;
    // sound deferral classification: the total above split by the
    // original side effect each still-unbound reference needs (see
    // RetailSoundDeferredBreakdown). A complete zone must read zero for every
    // class, not only for the total.
    uint32_t soundDeferredNameBlock0Count;
    uint32_t soundDeferredNameOtherBlockCount;
    uint32_t soundDeferredNamePoolCount;
    uint32_t soundDeferredNameUnclassifiedCount;
    uint32_t soundDeferredSoundFileAliasCount;
    uint32_t soundDeferredCurveAliasCount;
    uint32_t soundDeferredSpeakerMapAliasCount;
    uint32_t soundDeferredHeadAliasCount;
    // B1 small roots: live-registered inline PhysPreset and SndCurve assets.
    uint32_t registeredPhysPresetCount;
    uint32_t registeredSndCurveCount;
    // B1 nested sound owners: every inline/insert LoadedSound/SndCurve
    // declared inside a live-widened snd_alias_list_t is registered through
    // its original owner (Load_LoadedSoundAsset/Load_SndCurveAsset) and the
    // registered pointer replaces the widened one in the alias graph. These
    // counts must equal the walk's nested-loadedSound/nested-SndCurve
    // census (OAT deltas) for a complete zone, not just the walk window.
    uint32_t registeredNestedLoadedSoundCount;
    uint32_t registeredNestedSndCurveCount;
    // Native ClipMap registry: live-widened inline bodies committed
    // into the engine's own cm singleton (both bounded and full loads --
    // the bounded spmap spawn path needs cm live).
    uint32_t registeredClipMapCount;
    // B3 XAnimParts widening: live-registered inline bodies (both bounded
    // and full loads -- the bounded script chain precaches anims).
    uint32_t registeredXAnimCount;
    // B7 WeaponDef widening: live-registered inline bodies (both bounded
    // and full loads -- server init registers weapons on any spmap path).
    uint32_t registeredWeaponCount;
    // Techniques widened with a null name (retail linker-pooled technique
    // names unresolvable in our block-4 mirror; counted, never guessed --
    // the preflight fails loudly if one reaches a drawn technique).
    uint32_t nullTechniqueNameCount;
    // Vertex/pixel shaders widened with a null name (same pooled-string
    // class; upload consumes bytecode, never names).
    uint32_t nullShaderNameCount;
    // Technique-slot aliases deferred past their use point (forward
    // references) and how many the post-pass resolved.
    uint32_t deferredTechniqueAliasCount;
    uint32_t resolvedTechniqueAliasCount;
    // Deferred vertex-declaration / shader aliases and resolutions.
    uint32_t deferredSubAliasCount;
    uint32_t resolvedSubAliasCount;
    uint32_t walkedOnlyCount;
    // Wire cursors at live-load end (set on RETAIL_WALK_LOAD_OK, mirroring
    // RetailWalkDirectoryResult::endCursor for the walk-only reader). The
    // live and walk-only passes must consume identical bytes; comparing
    // these is the executable parity proof for the B7 bounce-array seam
    // (ord-890 regression: a live-only pre-stream inner base left block 4
    // 36 bytes behind the walk, misreading the following material).
    uint32_t endCursor[9]{};
    // Valid when code == RETAIL_WALK_LOAD_UNSUPPORTED_ASSET or
    // RETAIL_WALK_LOAD_BODY_FAILED: which directory entry the loader stopped
    // on, so a caller can report exactly what production support is missing
    // instead of an opaque failure. UINT32_MAX (matching
    // RetailWalkDirectoryResult::failedOrdinal's sentinel) on any success
    // path -- nothing stopped it.
    uint32_t failedOrdinal = UINT32_MAX;
    uint32_t failedType;
    uint32_t zoneIndex;
    // True when the load was skipped because an identical-policy live zone
    // with the same name already exists (reload idempotency -- see
    // DB_RetailZoneFindLive).  No new PMem was consumed and no asset was
    // (re)registered; zoneIndex names the resident slot.  False on every
    // fresh load, including the first load of a file.
    bool alreadyLoaded = false;
};

// The menu-reachable production link earlier stages do not yet provide: opens a
// real retail fastfile, begins a real (non-torn-down) retail zone session,
// and for every directory entry either
//   - registers it for real through DB_AddXAsset (today: ASSET_TYPE_FONT,
//     ASSET_TYPE_LOCALIZE_ENTRY, ASSET_TYPE_RAWFILE, and
//     ASSET_TYPE_STRINGTABLE, the only types with an installed
//     RetailZoneAssetLoader), or
//   - walks it byte-accurately via the same family walker F5 already proved,
//     to stay correctly positioned in the stream for whatever follows, or
//   - stops loudly and reports the exact ordinal/type it cannot yet widen.
// On RETAIL_WALK_LOAD_OK the zone is left live in the real zone registry
// (DB_RetailZoneBegin's slot) for the rest of the process, exactly like a
// normal DB_TryLoadXFileInternal load; on any other result the zone (if one
// was begun) has already been torn down via RetailZoneLoadSessionAbort.
//
// `strict` is the strict acceptance mode, threaded to the Material/Image live
// loaders: exact typed aliases/inserts or loud failure (no synthesis, no
// hardcoded identities, no white/$default substitution, no IWI-filename
// guessing, 2D-only images). The tolerant default preserves the menu boot
// path. TechniqueSet needs no flag: its alias resolution is exact-only in
// both modes.
//
// `bounded` is the first-frame mode: only the world-closure families
// (TechniqueSet, Material, Image, LightDef, GfxWorld) plus the script
// chain's RawFile bytes load live; every other type walks byte-accurately
// via the same family walker instead of failing (or, for the
// menu/small-root families, instead of registering assets the bounded
// frame never activates). Walked-deferred roots stay unregistered by
// construction. The tolerant default preserves existing full-load
// behavior.
RetailWalkLoadZoneResultCode RetailWalkLoadZoneAssets(const char *filename,
                                                        RetailWalkLoadZoneResult *result,
                                                        bool strict = false,
                                                        bool bounded = false);
