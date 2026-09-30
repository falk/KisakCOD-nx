#pragma once

#include "../universal/com_files.h"
#include "db_retail_zone.h"
#include "db_retail_walk.h"

// XAssetHeader/RetailZoneLoadSession come from the includes above (same
// shape as db_retail_decode_xanim.h); WeaponDef itself is only named by
// RetailWeaponFree below and defined where used.
struct WeaponDef;
struct RetailWorldLoadContext;

// B7 WeaponDef widening: source-derived 98-step walk of the
// already-walked 2168-byte wire root (db_retail_walk.cpp's
// ReadRetailWeaponDefBody, proven byte-accurate) into transaction-owned
// native storage -- never a wire-root cast -- with registration through
// the existing Load_WeaponDefAsset database owner.
//
// Wire contract (mirrors db_load.cpp Load_WeaponDef field order; every
// offset below reconciles against the walk reader's own slot reads and the
// native WeaponDef field order in xanim.h -- the three agree to the byte,
// summing to exactly 2168):
//   - 2168-byte root in the temp block, snapshotted and rewound like B3.
//   - 16 plain XStrings plus one 33-entry animation-name array (block 4).
//   - Three inline u16 script-ID arrays (hideTags[8] @216, notetrack keys
//     [16] @232, values[16] @264): already inside the root, no stream
//     bytes; each element resolves through the zone script table into a
//     real SL_* ID exactly like B3 (empty resolves to 0, never interned).
//   - 38 XModel slots (two 16-arrays @12/@700 plus six singles), 10 FX
//     slots, 8 Material slots: null binds null; anything else fails loudly
//     naming weapon/slot/offset -- XModel/FX have no live owner before
//     B4/B6 and weapon-embedded materials have none either.
//   - 48 sound-name slots (45 @340..516 plus 3 singles) plus one 29-entry
//     bounce array @520: the reference resolves every one through
//     ASSET_TYPE_SOUND (B2 territory), so null binds null and anything
//     else fails loudly. No inline sound bytes are consumed on failure:
//     the zone aborts, so accounting past the failure is irrelevant.
//   - Four accuracy-graph knot arrays (vec2 pairs, counts shared per pair
//     at @1924/@1928): null binds null, inline (-1) streams count*8 bytes
//     into the arena, shared (alias) fails loudly.
//   - All remaining root bytes are 4-byte scalars or plain float/u16 runs,
//     widened by explicit runs (native side via offsetof, never hand
//     LP64 numbers); the whole native record is zeroed first, matching the
//     reference (which never streams originalAccuracyGraphKnotCount).
//
// Operation inventory (98, one per reference Load_* step): 16 plain
// XStrings + 1 string array + 3 script arrays + 8 XModel ops (2 arrays +
// 6 singles) + 10 FX + 8 Material + 49 sound ops (48 singles + 1 bounce
// array) + 4 knot ops = 98. kRetailWeaponOpCount asserts this; the
// walk-only reader stays the independent oracle (live and walk consume
// block-4 bytes in identical order, enforced by the proof's cursor check).
// Script-string lifecycle mirrors B3: per-occurrence SL refs unwound on
// failure, owned by the registered asset after commit, released by
// RetailWeaponFree through the WEAPON remove-handler row on unload.

enum RetailWeaponDecodeResult
{
    RETAIL_WEAPON_DECODE_OK,
    RETAIL_WEAPON_DECODE_BAD_ARGUMENT,
    RETAIL_WEAPON_DECODE_BAD_ROOT,
    RETAIL_WEAPON_DECODE_BAD_NAME,
    RETAIL_WEAPON_DECODE_COUNT_MISMATCH,
    RETAIL_WEAPON_DECODE_INDEX_OOB,
    RETAIL_WEAPON_DECODE_SCRIPT_TABLE_MISSING,
    RETAIL_WEAPON_DECODE_SCRIPT_STRING_MISSING,
    RETAIL_WEAPON_DECODE_UNRESOLVED_REFERENCE,
    RETAIL_WEAPON_DECODE_TRUNCATED,
    RETAIL_WEAPON_DECODE_OUT_OF_ARENA,
    RETAIL_WEAPON_DECODE_REGISTRATION_FAILED,
};

const char *RetailWeaponDecodeResultName(RetailWeaponDecodeResult result);

// Operation inventory (98, one per reference Load_* step): 16 plain
// XStrings + 1 string array + 3 script arrays + 8 XModel ops (2 arrays +
// 6 singles) + 10 FX + 8 Material + 49 sound ops (48 singles + 1 bounce
// array) + 4 knot ops = 98. The grouped run table lives in the .cpp (one
// entry per contiguous same-kind run, in reference order -- including the
// one inversion, accuracyGraphName[1] @1904 streaming after the first knot
// pair); two constexpr asserts pin its expansion to 98 steps and its wire
// offsets to in-range/unique, and the walk-only reader stays the
// independent oracle (live and walk consume block-4 bytes in identical
// order, enforced by the proof's cursor check).

// Resolve one shared (alias-form) 29-entry WeaponDef bounce pointer array
// from the zone mirror: the retail smoke/flash_grenade shape. `aliasRef` is
// the weapon root's bounce slot (a block-4 token naming the array a declarer
// streamed inline earlier); `bounce` receives the bound snd_alias_list_t*
// per entry (null entries stay null). Returns false with *unresolvedIndex
// naming the first non-null entry this resolver could not bind. Exposed for
// the B7 synthetic pointer-class proof.
bool RetailWeaponResolveBounceArray(RetailZoneLoadSession *session, uint32_t aliasRef,
                                    void **bounce, uint32_t *unresolvedIndex,
                                    const char **unresolvedName,
                                    bool deferUnresolved = false);

// Live WeaponDef driver for RetailWalkLoadZoneAssets (db_retail_walk.cpp):
// consumes one inline WeaponDef body in exact walk order, widening field
// by field into zone-arena transaction storage, then committing atomically
// through Load_WeaponDefAsset and requiring the returned pooled header.
// Only the inline (-1) header form is supported; any other form fails
// loudly. Runs in both bounded and full loads: G_RegisterWeapon resolves
// every weapon through DB_FindXAssetHeader during server init on any spmap
// path, so walking weapons deferred would keep killing the run at
// G_SetupWeaponDef's defaultweapon lookup.
//
// Tri-state contract: OK registers; DEFER-class results
// (UNRESOLVED_REFERENCE, SCRIPT_STRING_MISSING, INDEX_OOB, BAD_NAME --
// references the live load cannot resolve: an unregistered sound asset, a
// script-table gap, a nameless body) mark the body walked-deferred with a
// loud audit line instead of aborting the zone (F5 contract); anything
// else is zone-fatal corruption the walk reader could not account either.
// XModel/FX/Material aliases resolve through `worldContext`'s zone-scoped
// nested table and the registry exactly like the world decoder's own
// nested references (RetailWorldResolveNestedAlias), so a weapon whose
// dependency was widened earlier in the same zone registers live; only a
// genuine miss defers. `worldContext` may be null (walk-only/test
// callers), in which case every alias defers as before.
// Accounting and widening share one walk-ordered forward pass (the reader
// is zlib forward-only: nothing is ever re-read or rewound). A deferred
// body is fully accounted -- inline nested bodies consumed through the
// walk helpers -- but never committed, so consumers of the missing asset
// still fail loudly by name.
RetailWeaponDecodeResult RetailWalkLiveLoadWeaponDef(RetailZoneLoadSession *session,
                                                     FsRetailFastfileReader *reader,
                                                     RetailWorldLoadContext *worldContext,
                                                     uint32_t headerRef, XAssetHeader *header,
                                                     RetailWalkDirectoryRecord *record,
                                                     RetailWalkDirectoryResult *summary);

// deferred weapon sound references (see the .cpp comment): a weapon
// body streams before the snd_alias_list_t it names, and resolving it must
// wait until the owner zone has streamed. RetailDeferWeaponSound records
// (slot, copied name, owning zone) when the name has no live entry yet;
// RetailResolveDeferredWeaponSounds runs after every successful zone load
// and binds each now-live list, returning the outstanding count (never
// creating the engine default). RetailWeaponZoneUnloaded drops entries whose
// owning zone is torn down so no pass can write into freed zone storage.
bool RetailDeferWeaponSound(void **slot, const char *name, uint32_t zoneIndex);
uint32_t RetailResolveDeferredWeaponSounds();
uint32_t RetailWeaponDeferredSoundCount();
uint32_t RetailWeaponDeferredSoundOverflow();
void RetailWeaponZoneUnloaded(uint32_t zoneIndex);

// Unload half for interned hideTags/notetrack script strings, installed in
// the WEAPON row of DB_RemoveXAssetHandler (db_registry.cpp). Releases the
// 8 + 16 + 16 IDs exactly like XAnimFree releases bones/notifies.
void RetailWeaponFree(WeaponDef *weapon);
