#pragma once

#include <cstddef>

#include "../universal/com_files.h"
#include "db_retail_zone.h"

// Widens one real Material body read live from `reader`, allocating through
// the zone's real native arena and registering through the zone's real
// asset table.  Ported from switch_sp_bootstrap.cpp's hardware-proven
// per-entry material walk (PASS:RETAIL_MENU_GFX_PROOF against the real
// retail ui.ff, materials=7), adapted to RetailZoneLoadSessionAlloc/
// RetailZoneLoadSessionRegister for the same zone-lifetime reason as
// TechniqueSet and Image.
//
// One real, deliberate scope limit relative to the bootstrap, failing
// loudly rather than guessing:
//  - Every texture-def image slot must be inline (freshly widened via
//    RetailWidenImageFromWire).  An alias slot needs the Android per-zone
//    pooled-image identity (DB_AddXAsset pseudo-block 15) to resolve back
//    to an image widened earlier in the *same* material walk, which this
//    driver does not track yet.
//  - Constants and state-bits tables are widened as raw byte copies (the
//    bootstrap's own scope: "constants: DB_G3 owns runtime shader-constant
//    binding") when present; the bootstrap's own bounded proof required
//    wireMaterial.constantCount/constantTableRef to be zero, so this keeps
//    that same requirement rather than guessing an untested shape.
//
// The technique-set binding now resolves both wire forms the bootstrap does:
// a plain wire name string through DB_FindXAssetHeader, and the comma-stub
// directory-offset convention (a reference that points back into the
// *calling zone's own* directory bytes, at a TechniqueSet-typed entry whose
// decoded name is comma-prefixed -- binding to a same-named original
// registered by an earlier-loaded zone, e.g. ui.ff's stub binding back to
// code_post_gfx.ff's real technique set).  `directoryBytes` is the calling
// zone's own `assetCount * 8`; `stubNames`/`stubCount` is the caller's
// ordinal-indexed table of decoded TechniqueSet-entry names (only entries
// whose type was ASSET_TYPE_TECHNIQUE_SET need a non-null slot).  Pass
// `directoryBytes == 0`/`stubNames == nullptr`/`stubCount == 0` when the
// caller has no directory context at all (a reference of this form then
// fails loudly instead of guessing); RetailWalkLoadZoneAssets always
// supplies real values.
struct RetailWireTechniqueCache;

bool RetailWalkLiveLoadMaterial(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                                 uint32_t headerRef, XAssetHeader *header,
                                 uint32_t directoryOffset, uint32_t directoryBytes,
                                 const char *const *stubNames, uint32_t stubCount,
                                 RetailWireTechniqueCache *cache = nullptr,
                                 bool strict = false,
                                 bool *deferredAlias = nullptr,
                                 char *deferredAliasName = nullptr,
                                 std::size_t deferredAliasNameCapacity = 0);

// Shared body of RetailWalkLiveLoadMaterial, split out so callers that already hold an
// inline-decoded FsRetailFastfileMaterial (an anonymous background material embedded
// directly in an ItemDef/menuDef_t rather than a named top-level zone asset -- see
// FsRetailFastfileMenu::backgroundWasInline) can widen and register it without a doomed
// by-name lookup through ResolveMaterialRef.
//
// `strict` is the strict acceptance mode: exact typed aliases/inserts or loud
// failure -- no RetailMaterial_Synthesize2D synthesis, no hardcoded
// per-offset identity cases, no white/$default substitution, no
// last-loaded-name guessing, and no IWI-filename fallback for unresolvable
// image names. The tolerant default preserves the menu boot path, which
// still depends on those fallbacks; acceptance proofs must pass strict.
bool RetailWidenMaterialFromWire(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                                 const FsRetailFastfileMaterial &wire,
                                 const FsRetailFastfileTextureDef *textureDefs,
                                 uint32_t directoryOffset, uint32_t directoryBytes,
                                 const char *const *stubNames, uint32_t stubCount,
                                 RetailWireTechniqueCache *cache, XAssetHeader *header,
                                 bool strict = false, bool allowNullName = false,
                                 bool allowNullTechniqueSet = false,
                                 bool *deferredAlias = nullptr,
                                 char *deferredAliasName = nullptr,
                                 std::size_t deferredAliasNameCapacity = 0);

Material *ResolveMaterialRef(FsRetailFastfileReader *reader, uint32_t matRef,
                             uint32_t directoryOffset, uint32_t directoryBytes,
                                 const char *const *stubNames, uint32_t stubCount,
                                 bool strict = false,
                                 char *deferredName = nullptr,
                                 std::size_t deferredNameCapacity = 0);

// Resolves a non-inline, non-insert GfxImage reference by its comma-stub
// directory-offset entry, its shared image-pool slot, or (least strict) its
// plain block-4 name -- the same rules a material's own texture-def slot
// already uses (RetailWidenMaterialFromWire above). Exposed so any other
// live loader whose wire form uses this identical reference shape (e.g.
// LightDef's attenuation image, RetailWalkLiveLoadLightDef in
// db_retail_walk.cpp) can resolve it instead of failing loudly on every
// non-inline form.
GfxImage *ResolveImageRef(FsRetailFastfileReader *reader, uint32_t imageRef,
                          uint32_t directoryOffset, uint32_t directoryBytes,
                          const char *const *stubNames, uint32_t stubCount,
                          bool strict = false);

// Retries every texture-slot image binding deferred during the directory
// loop (forward pool/image references whose declarer widens later in the
// same zone). Runs after the loop, when the block-4 mirror and the image
// pool are whole; anything still missing then is genuinely dangling and
// fails loudly naming the material, slot, reference, and target offset.
bool RetailWireImageCacheResolveDeferred(RetailZoneLoadSession *session,
                                         RetailWireTechniqueCache *cache,
                                         FsRetailFastfileReader *reader,
                                         uint32_t directoryOffset, uint32_t directoryBytes,
                                         const char *const *stubNames, uint32_t stubCount);

// Records a consumer slot whose comma-material alias arrived before its
// owning zone. The slot is patched only after a real registered Material is
// available; unresolved owners remain visible to the production verifier.
// `zoneIndex` is the zone that owns `slot` (the arena its pointer lives in);
// it lets DB_RetailZoneEnd drop the entry before that arena is released, so a
// later resolve pass can never write through a dangling slot. 0 means
// unowned (test callers) and is never purged by zone index.
bool RetailDeferMaterialAlias(Material **slot, const char *name, uint32_t zoneIndex);
uint32_t RetailResolveDeferredMaterialAliases(bool failIfUnresolved = false);
// Drops every deferred material alias owned by `zoneIndex`, mirroring
// RetailWeaponZoneUnloaded. Called from DB_RetailZoneEnd before the zone's
// memory (and therefore every recorded slot) is released.
void RetailMaterialZoneUnloaded(uint32_t zoneIndex);
// Count of deferred material aliases not yet patched by a real registered
// owner. Must be zero for a successful zone load: an alias whose owner zone
// never loads cannot pass as a successful partial zone load.
uint32_t RetailMaterialDeferredAliasCount();
// `$defeatbackdrop` is emitted as a comma stub by every SP map, but OAT's
// all-zone oracle has no owning material.  `$levelbriefing`'s only owner is
// `mp_killhouse_load.ff` (material + loadscreen_mp_killhouse image); the SP
// zone set carries only the killhouse.ff directory stub.  The SP Killhouse
// route never loads a defeat/briefing-screen consumer; keep these
// directory-only dependencies out of the required material closure while
// still rejecting them if a consumer asks for the identity through
// ResolveMaterialRef.
bool RetailIsNonSpMaterialAlias(const char *name);

Material *RetailMaterial_Synthesize2D(const char *name, RetailZoneLoadSession *session = nullptr);

// process-lifetime count of RetailMaterial_Synthesize2D body
// manufacture attempts (including ones that later fail).  The original
// engine has no synthesis; a production walk checkpoint must observe zero
// for the material closure it consumes.  Declared here so the strict
// graphics host proof can assert the count directly.
uint32_t RetailMaterialSynthesizedCount();
