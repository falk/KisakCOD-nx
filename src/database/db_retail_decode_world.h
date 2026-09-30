#pragma once

#include "../universal/com_files.h"
#include "db_retail_zone.h"

// Native GfxWorld registry: widen the
// registry-facing GfxWorld shape from a live FS stream and commit it
// atomically into the engine's own world singleton (s_world, owned by
// db_registry.cpp's DB_XAssetPool table -- the existing GfxWorld database
// owner, never a parallel database or a second s_world).
//
// Field coverage contract -- widened here, all other GfxWorld fields stay
// zeroed and are owned by later slices:
//   WIDENED: name, baseName, indexCount + indices (arena copy), surfaceCount,
//     skySurfCount + skyStartSurfs (arena copy), skyImage (null-or-bound),
//     vertexCount + vd.vertices/vld bytes (arena copies, raw 44-byte
//     records -- field layout verified by the preflight), sunLight
//     (64-byte record memcpy plus the exactly-bound nested LightDef),
//     sunPrimaryLightIndex, primaryLightCount, reflectionProbeCount +
//     probes (origin + null-or-bound reflectionImage -- every probe's
//     16-byte record is bulk-read before any probe's image is resolved,
//     matching Load_GfxReflectionProbeArray's single bulk Load_Stream
//     ahead of its per-element pointer pass), cellCount + cells array
//     (zeroed records -- cell-graph widening is separate), lightmapCount +
//     lightmaps (null-or-bound primary/secondary images, same bulk-then-
//     resolve order as probes), modelCount + models array (verbatim
//     56-byte POD copy -- GfxBrushModel carries only bounds floats and
//     surface-range uint16s, no pointers, so the ILP32 wire record already
//     matches the native layout), materialMemoryCount + materialMemory
//     (null-or-bound material, same bulk-then-resolve order as probes),
//     sunflare sprite/flare bindings, outdoorImage (null or bound),
//     dpvs.staticSurfaceCount + dpvs.surfaces (widened GfxSurfaces, same
//     bulk-then-resolve order as probes), dpvs.surfaceMaterials +
//     dpvs.surfaceCastsSunShadow (zeroed sort workspace sized by
//     models[0].surfaceCount for R_SortWorldSurfaces), shadowGeom header
//     array (zeroed, one entry per primary light; per-light index spans
//     stay null, guarded by R_AddShadowSurfaceToPrimaryLight),
//     dpvsPlanes (cellCount + planes + nodes + zeroed sceneEntCellBits),
//     cells (full native cell graph: aabb trees with inline/alias
//     smodel-index lists, portals with recursive inline cells, alias cell
//     references, vertices, hull axes, cull-group int lists, reflection-
//     probe byte lists), cellBitsCount, cellCasterBits, sceneDynModel/
//     sceneDynBrush, dpvsDyn counts + dynEntCellBits/dynEntVisData, the
//     six dpvs vis scratch arrays + lodData, dpvs.smodelCount +
//     smodelInsts, dpvs.sortedSurfIndex, dpvs.cullGroups + cullGroupCount,
//     and the root-serialized dpvs static draw ranges (lit/decal/emissive,
//     staticSurfaceCountNoDecal, smodelVisDataCount/surfaceVisDataCount).
//   DEFERRED (zeroed, fail loudly at their consumer, never synthesized):
//     sunParse/sunColorFromBsp (entity data),
//     lightGrid/lightmap textures/probe textures (device textures;
//     texture-handle arrays are allocated), shadowVis/nonSun/lightRegion
//     payload (sceneDyn headers are allocated), smodelDrawInsts
//     (XModel-bearing, gate-aStaticModels deferred), streamInfo (empty).
//
// Nested dependencies resolve exactly or fail loudly, following the strict
// strict acceptance mode: an inline body widens through the existing
// strict decoders and registers through the session table; a null slot
// binds null (asserted, never defaulted); an alias decodes canonically to
// a block-4 name offset, reads that exact name, and binds the registered
// asset of the slot's type -- miss on any step fails naming the slot.
// Cross-body block-0 aliases (rewinding-temp records with no stable stream
// identity) are unsupported by design until pooled identity exists; the
// host fixture keeps nested aliases name-resolvable and the walk-only reader
// independently agrees on every byte first (dual-reader proof below).
// No renderer resources are created before a live D3D9 device: widening
// stops at CPU data and registered headers (G3/R own upload).

struct GfxWorld;
struct GfxSurface;
struct GfxLight;
struct GfxImage;
struct GfxLightDef;
struct Material;
struct MaterialMemory;
struct RetailWireTechniqueCache;
struct XModel;

struct RetailWorldNestedName
{
    // Block-4 offset of the inline body start, when the body was streamed in
    // block 4. Some retail DB_ConvertOffsetToAlias references target the
    // body itself rather than its name or pointer slot.
    uint32_t bodyOffset = 0;
    // Block-4 offset of a widened named body's name string, with the
    // registered header it resolved to. Populated for every named nested
    // body in stream order; aliases match by exact offset.
    uint32_t nameOffset = 0;
    // Block-4 offset of the slot whose inline/insert body produced this
    // header (0 = unknown slot). Retail aliases most often name an asset's
    // string, but the original DB_ConvertOffsetToAlias is a slot
    // indirection -- the encoded offset addresses a 4-byte slot an earlier
    // load already filled with the loaded asset. Our mirrors keep pristine
    // wire bytes, so the loaded result is recovered from this table keyed
    // by slot offset instead of a patched slot value.
    uint32_t slotOffset = 0;
    XAssetType type = ASSET_TYPE_XMODELPIECES;
    XAssetHeader header{};
};

struct RetailWorldLoadContext
{
    RetailZoneLoadSession *session = nullptr;
    FsRetailFastfileReader *reader = nullptr;
    // Function-pointer indirection, not a direct call, so
    // db_retail_walk.cpp can live-widen a nested FX visual-union slot
    // (material/model) without creating a link-time dependency on this
    // translation unit. The narrow walk-only host link (test's
    // retail_walk_host_check: db_retail_walk.cpp + db_retail_wire.cpp only,
    // no decode_*.cpp) must keep building with zero references to
    // RetailWorldWidenMaterial/RetailWorldWidenXModel; a direct call from
    // walk.cpp would pull those symbols in even though that link never
    // constructs a context that uses them. Null unless a live FX loader
    // (RetailWalkLiveLoadFxEffectDef) wires it up; walk.cpp falls back to
    // walk-only consumption (no widen, no record) when null, exactly like
    // having no worldContext at all.
    bool (*widenNestedMaterial)(RetailWorldLoadContext *, uint32_t, Material **,
                                uint32_t) = nullptr;
    bool (*widenNestedXModel)(RetailWorldLoadContext *, uint32_t, XModel **, uint32_t,
                              uint32_t *) = nullptr;
    // The zone's ordinal-indexed TechniqueSet stub table (same one Material
    // and MenuList entries resolve their comma-stub directory-offset
    // technique-set/image references through -- see db_retail_walk.cpp's
    // stubNames comment): a world-nested Material can carry the identical
    // reference shape, so it must resolve through this same table rather
    // than a null/zero one.
    uint32_t directoryOffset = 0;
    uint32_t directoryBytes = 0;
    const char *const *stubNames = nullptr;
    uint32_t stubCount = 0;
    RetailWireTechniqueCache *techCache = nullptr;
    // The session dispatcher owns typed-handle registration for top-level
    // XModels; nested world models register directly through this widener.
    bool deferXModelRegistration = false;
    // Byte counts consumed, mirroring the walk-only reader's accounting so
    // a divergence fails fast instead of desynchronizing the stream.
    uint32_t nestedBodyBytes = 0;
    uint32_t block1Bytes = 0;
    // Resolved nested dependencies, in stream order.
    uint32_t widenedMaterialCount = 0;
    uint32_t widenedImageCount = 0;
    uint32_t widenedLightDefCount = 0;
    uint32_t widenedXModelCount = 0;
    // Measured, never-silent count of a plain-pointer XModel sub-field
    // (bone arrays, blend info, rigid vert lists) decoding to a nonzero
    // value that is neither null nor the inline sentinel -- the one shape
    // RetailWorldWidenXModel does not resolve (would need a
    // DB_ConvertOffsetToPointer-style shared-array bind). None of these
    // fields are read by the static-model geometry draw path, so this
    // counter exists purely for visibility, not to gate the milestone.
    uint32_t xmodelUnresolvedPlainPointerCount = 0;
    // Nested PhysPreset bodies widened and registered through the real
    // Load_PhysPresetAsset owner (XModel@212 / DynEntityDef@48). Any
    // non-null nested slot either lands here or fails the load loudly, so
    // no silent null/deferred PhysPreset remains.
    uint32_t widenedPhysPresetCount = 0;
    // Widened materialMemory entries, in stream order. Surface material
    // slots address array entries by engine-image offset; the entry index
    // maps through matmemEngineVirt below. Populated by the matmem section
    // before any surface widens.
    uint32_t matmemCount = 0;
    MaterialMemory *matmem = nullptr;
    // Linker-visible block-4 offset where the live materialMemory array landed.
    uint32_t matmemEngineVirt = 0;
    // Zone-scoped widened-slot/name ledger. The full production load records
    // one (and for named materials two) entry per registered XModel/FX/Sound/
    // Material directory asset in addition to world-nested bodies -- real
    // zones (common.ff: 6502 assets) overflow the original 2048 cap, so the
    // capacity is sized for the largest retail zone.
    static constexpr uint32_t kNestedCap = 1u << 17;
    RetailWorldNestedName nested[kNestedCap]{};
    uint32_t nestedCount = 0;
    // Append-only open-addressing indexes over `nested` (0 = empty, value =
    // entry index + 1; key = offset + type, newest entry wins on a duplicate
    // key, matching the old youngest-first scan). Alias resolution used to
    // scan the whole ledger backwards per reference -- with nestedCount
    // reaching ~1e5 on the real world that was the gfx_map decoder's O(n^2)
    // term. Tables are 2x the entry cap so probing always has empty slots.
    static constexpr uint32_t kNestedIndexSize = 1u << 18;
    static constexpr uint32_t kNestedIndexMask = kNestedIndexSize - 1u;
    uint32_t nestedSlotIndex[kNestedIndexSize]{};
    uint32_t nestedBodyIndex[kNestedIndexSize]{};

    // menu-family parity: a comma-stub material asset has no widened
    // body to record in the nested ledger, but the original directory slot
    // still becomes the identity a later consumer alias points at (the slot
    // the original's *inserted/DB_AddXAsset patch would fill).  When the
    // material decoder defers such a stub until its owning zone arrives, the
    // directory slot and the stripped owner name are recorded here so a
    // consumer alias (ItemDef/menuDef background) resolves to the same
    // deferred name instead of failing or substituting a default body.
    struct RetailWorldDeferredSlot
    {
        uint32_t slotOffset = 0;
        XAssetType type = ASSET_TYPE_MATERIAL;
        // Non-SP stub (owner outside the loaded closure): the original's
        // DB_FindXAssetHeader would return the type default entry; this port
        // binds the consumer null instead of deferring forever or
        // manufacturing a default body.
        bool bindNull = false;
        char name[64]{};
    };
    static constexpr uint32_t kDeferredSlotCap = 256;
    RetailWorldDeferredSlot deferredSlots[kDeferredSlotCap]{};
    uint32_t deferredSlotCount = 0;

    // sound-body ledger: SoundFile and SpeakerMap are not registered
    // assets, but an alias-form snd_alias_t slot resolves through
    // DB_ConvertOffsetToPointer to the *pointer of the first load* at the
    // same block-4 wire offset. Recording each widened object under its wire
    // body offset recreates that identity across lists/aliases without a
    // second object or a re-parse.
    enum RetailSoundBodyKind : uint8_t
    {
        RETAIL_SOUND_BODY_FILE = 0,
        RETAIL_SOUND_BODY_SPEAKER_MAP = 1,
    };
    struct RetailSoundBody
    {
        uint32_t block4Offset = 0;
        void *object = nullptr;
        uint8_t kind = RETAIL_SOUND_BODY_FILE;
    };
    // Same capacity class as the nested ledger (largest retail zone).
    static constexpr uint32_t kSoundBodyCap = 1u << 17;
    RetailSoundBody soundBodies[kSoundBodyCap]{};
    uint32_t soundBodyCount = 0;
    // Offset+kind index over `soundBodies` (same open-addressing scheme as
    // the nested ledger above).
    static constexpr uint32_t kSoundBodyIndexSize = 1u << 18;
    static constexpr uint32_t kSoundBodyIndexMask = kSoundBodyIndexSize - 1u;
    uint32_t soundBodyIndex[kSoundBodyIndexSize]{};
};

// Record/find one widened SoundFile/SpeakerMap by its block-4 body offset.
// The original Load_snd_alias_t alias branch (DB_ConvertOffsetToPointer)
// binds the already-loaded struct at exactly that offset, so the lookup is
// an exact-offset identity map, never a re-parse or a synthesized object.
bool RetailWorldRecordSoundBody(RetailWorldLoadContext *context, uint32_t block4Offset,
                                uint8_t kind, void *object);
void *RetailWorldFindSoundBody(RetailWorldLoadContext *context, uint32_t block4Offset,
                               uint8_t kind);

// Consume `bytes` stream bytes into `dest`, or advance past them when
// `dest` is null. Every byte lands densely in the session wire blocks (and,
// through the session stream's own sync, in the reader mirror) at its linker
// offset: reader, session, and true stream positions stay glued, so absolute
// (block,offset) reads and pool patches address true bytes. Discarding
// without landing (the old scratch form) shifted every later mirror
// placement and silently broke cross-references into bulk spans.
bool RetailWorldConsume(RetailWorldLoadContext *context, void *dest, uint32_t bytes,
                        uint32_t alignment = 1u);

// Stream one inline name into the reader mirror, returning its block-4
// offset. A null nameRef binds null without consuming bytes.
bool RetailWorldStreamName(RetailWorldLoadContext *context, uint32_t nameRef,
                           uint32_t *nameOffset, const char **name);

// Copy a retained span (indices, vertices, layer data, sky starts) into
// zone-arena memory owned for the zone's lifetime.
bool RetailWorldRetainSpan(RetailWorldLoadContext *context, uint32_t bytes,
                           uint32_t alignment, void **out);

// Reserve runtime-only block-1 bytes (mirrors the walk-only reader's
// block-1 accounting exactly).
bool RetailWorldRuntimeBytes(RetailWorldLoadContext *context, uint32_t slotRef,
                             uint64_t bytes, uint32_t alignment);

// Widen one nested slot of the given type: null binds null, inline widens
// through the strict decoder and registers, alias resolves by exact name
// offset through the registry. Returns false loudly on any other form.
// `slotOffset` (block-4 offset of the slot itself, 0 = unknown) records the
// widened result for later DB_ConvertOffsetToAlias-style slot indirection.
bool RetailWorldWidenMaterial(RetailWorldLoadContext *context, uint32_t slotRef,
                              Material **out, uint32_t slotOffset = 0);
bool RetailWorldWidenImage(RetailWorldLoadContext *context, uint32_t slotRef,
                           GfxImage **out, uint32_t slotOffset = 0);
bool RetailWorldWidenLightDef(RetailWorldLoadContext *context, uint32_t slotRef,
                              GfxLightDef **out);
// Widen one nested/top-level XModel slot:
// null binds null, alias resolves by exact name offset through the
// registry (DB_FindXAssetHeader), inline/insert widens the full 220-byte
// root -- six bone arrays, numsurfs XSurfaces (block 7/8 geometry stays
// zone-block-backed per the docs' block table), and numsurfs Material
// slots resolved through RetailWorldWidenMaterial -- and registers through
// the session table. Collision/physics sub-objects (XRigidVertList/
// XSurfaceCollisionTree is widened because rigid skinning and mark consumers
// dereference it; XModel collSurfs/physPreset/physGeoms remain walk-consumed
// for exact cursor accounting but deliberately unwidened/null. This call
// never activates gameplay/collision, matching the static-model geometry
// scope of this milestone.
// The -2 insert form reserves its 4-byte block-4 DB_InsertPointer slot before
// the body and records the widened result there (the HandleAssetSlot shape
// later DB_ConvertOffsetToAlias aliases resolve through). When
// `insertSlotOffsetOut` is non-null it receives that reserved offset so a
// deferred-registration caller can record it with the registered header.
bool RetailWorldWidenXModel(RetailWorldLoadContext *context, uint32_t slotRef,
                            XModel **out, uint32_t slotOffset = 0,
                            uint32_t *insertSlotOffsetOut = nullptr);

// Live XModel driver for RetailWalkLoadZoneAssets (db_retail_walk.cpp):
// widens one top-level inline ASSET_TYPE_XMODEL directory entry through
// RetailWorldWidenXModel and registers it, so later same-zone alias
// references (including GfxWorld's own dpvsDrawInstsRef per-instance model
// slot) resolve through the real registry instead of staying deferred.
// `context` is the zone-scoped load context (one per zone load, owned by
// the caller): XModel material slots alias sibling slots across the whole
// zone, not just within one model, so the widened-slot table must outlive
// any single model. The context's deferXModelRegistration is set true for
// the duration of this call (the session dispatcher owns top-level
// registration); the field is toggled per call, which is safe because
// widening is synchronous.
bool RetailWalkLiveLoadXModel(RetailZoneLoadSession *session,
                              FsRetailFastfileReader *reader, uint32_t header,
                              XAssetHeader *out, RetailWorldLoadContext *context,
                              uint32_t *insertSlotOffsetOut = nullptr);

// Record one zone-scoped widened-slot entry from outside the world decoder
// (top-level directory slots, whose registered results later draw-inst
// slots alias by raw DB_ConvertOffsetToAlias indirection).
bool RetailWorldRecordZoneSlot(RetailWorldLoadContext *context, uint32_t slotOffset,
                               XAssetType type, XAssetHeader header);
// Record a top-level inline body start for aliases that point directly at
// that body's wire bytes rather than at its directory slot or name.
bool RetailWorldRecordBody(RetailWorldLoadContext *context, uint32_t bodyOffset,
                           XAssetType type, XAssetHeader header);

// menu-family parity: record a deferred comma-stub directory slot and
// its stripped owner name (no widened body exists yet). A consumer alias
// encoded against `slotOffset` resolves to that name so its owner slot can
// be patch-filled when the owning zone registers, exactly like the
// material-stub deferral itself.
bool RetailWorldRecordDeferredSlotName(RetailWorldLoadContext *context, uint32_t slotOffset,
                                       XAssetType type, const char *name, bool bindNull);
// Look up the deferred name for one encoded consumer alias ref. Returns
// false when the ref names no deferred slot of this type; on true, bindNull
// selects between a null bind (no closure owner exists) and a deferred
// name the consumer slot is patch-filled with once the owner registers.
bool RetailWorldFindDeferredSlotName(RetailWorldLoadContext *context, uint32_t slotRef,
                                     XAssetType type, char *name, uint32_t nameCapacity,
                                     bool *bindNull);

// B1 PhysPreset small root: consumes one inline PhysPreset body (44-byte
// root + name/sndAliasPrefix XStrings), widens it field by field into the
// native PhysPreset, and registers through Load_PhysPresetAsset. The walk
// reader stays the independent byte oracle.
bool RetailWalkLiveLoadPhysPreset(RetailZoneLoadSession *session,
                                  FsRetailFastfileReader *reader,
                                  XAssetHeader *out);

// Nested PhysPreset widener (the port's Load_PhysPresetPtr): null binds
// null, an alias resolves through the zone nested table/registry, and the
// inline/-2 forms consume the 44-byte body + XStrings, widen into a native
// PhysPreset, register through Load_PhysPresetAsset, and record the
// declaring `slotOffset` and any -2 reserved insert slot so later
// DB_ConvertOffsetToAlias aliases bind the registered preset. A non-null
// slot that cannot be resolved fails loudly (never a silent null).
bool RetailWorldWidenPhysPreset(RetailWorldLoadContext *context, uint32_t slotRef,
                                PhysPreset **out, uint32_t slotOffset = 0,
                                uint32_t *insertSlotOffsetOut = nullptr);

// Resolve one block-4 asset alias exactly the way the world decoder's own
// nested references do (slot indirection through the zone-scoped widened
// table, then name reference through the registry); see ResolveNestedAlias
// in db_retail_decode_world.cpp. Exposed for the B7 weapon decoder, whose
// XModel/FX/Material slots use the identical wire shape and previously
// deferred every alias even when the target was already live in the zone.
bool RetailWorldResolveNestedAlias(RetailWorldLoadContext *context, uint32_t slotRef,
                                   XAssetType type, XAssetHeader *header);

// GPU audit (a4): census over widened draw-inst models, host + live.
// See db_retail_decode_world.cpp for contract.
bool RetailWorldAuditStaticModelsForGpu(const GfxWorld *world);

// Allocate the zeroed transaction world in zone-arena memory. The caller
// populates it field by field, then commits once.
GfxWorld *RetailWorldAllocTransaction(RetailZoneLoadSession *session);

// Atomically publish a fully-widened transaction into the engine's own
// world singleton and register it through the session table, returning
// the pooled header (which must equal the singleton). Pre-commit the
// singleton is untouched (rollback = discard the transaction); post-commit
// it is bit-identical to the transaction.
XAssetHeader RetailWorldCommitTransaction(RetailZoneLoadSession *session,
                                          GfxWorld *transaction);

// Live GfxWorld driver for RetailWalkLoadZoneAssets (db_retail_walk.cpp):
// consumes one inline GfxWorld body in exact db_load order, widening the
// registry-facing subset above and committing once. Structural spans the
// registry does not need are consumed-and-discarded with the walk-only
// reader's own span formulas; anything outside the proven shapes fails
// loudly. No renderer resources are created.
bool RetailWalkLiveLoadGfxWorld(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader,
                                XAssetHeader *header, uint32_t *handle,
                                uint32_t directoryOffset = 0, uint32_t directoryBytes = 0,
                                const char *const *stubNames = nullptr, uint32_t stubCount = 0,
                                RetailWireTechniqueCache *techCache = nullptr,
                                RetailWorldLoadContext *sharedContext = nullptr);

// Nested widening counts from the most recent live GfxWorld load (reset
// at each driver entry): materials/images/lightdefs widened inside the
// world subtree, and block-1 bytes expanded for its runtime arrays. Read
// immediately after the load, before any other world load.
void RetailWorldLastLoadStats(uint32_t *materials, uint32_t *images,
                              uint32_t *lightDefs, uint32_t *block1Bytes);

// Installs the MapEnts loader in the shared dispatcher (world family;
// see the LoadMapEnts implementation in db_retail_decode_world.cpp).
bool RetailZoneInstallMapEntsDecoder(RetailZoneLoadSession *session);
// Installs the sole native ComWorld widener.  It is deliberately enrolled
// only by the live SP map-zone path; directory walks remain decode-free.
bool RetailZoneInstallComWorldDecoder(RetailZoneLoadSession *session);
