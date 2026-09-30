#pragma once

#include "../universal/com_files.h"
#include "db_retail_zone.h"

// B3 script-string table plus XAnimParts widening: field-by-field widening of the
// already-walked 88-byte XAnimParts wire root (db_retail_walk.cpp's
// ReadRetailXAnimPartsBody, proven byte-accurate against the OAT oracle)
// into transaction-owned native storage -- never a wire-root cast -- with
// bone-name and notify-name script references interned through the real
// SL_* table and registration through the existing Load_XAnimPartsAsset
// database owner.
//
// Wire contract (mirrors db_load.cpp Load_XAnimParts field order):
//   - 88-byte root in the temp block; asset name is an XString.
//   - names[boneCount[9]] is a u16 wire-index table into this zone's own
//     script-string table (XAssetList root); each XAnimNotifyInfo carries
//     the same u16 index form plus a float time.
//   - deltaPart holds trans/quat sub-bodies whose dynamic index arrays are
//     1 byte wide when numframes < 0x100 and 2 bytes wide otherwise,
//     exactly like the walk-only reader; the top-level indices union uses
//     the same width rule. The live decode reproduces the identical
//     branch and the proof exercises both.
//   - The six data arrays are plain POD copies; any nonzero element count
//     with a null slot fails loudly (COUNT_MISMATCH) rather than lending
//     a later consumer a null pointer beside a nonzero count. A present
//     slot with a zero count is tolerated as empty, matching the reference
//     loader's own shape.
// Script-string lifecycle (real SL_*, no synthesized IDs): each referenced
// table string resolves to verbatim engine bytes, then takes exactly one
// per-occurrence reference -- SL_FindString + SL_AddRefToString when the
// string already exists, SL_GetString(..., 4) when it does not -- so every
// stored u16 is a genuine script-string ID whose unload half is the real
// XAnimFree (SL_RemoveRefToString per bone/notify, xanim.cpp), reached
// through the new DB_RemoveXAssetHandler XAnim entry on zone unload. No
// leak entries survive a zone unload/override.

enum RetailXAnimDecodeResult
{
    RETAIL_XANIM_DECODE_OK,
    RETAIL_XANIM_DECODE_BAD_ARGUMENT,
    RETAIL_XANIM_DECODE_BAD_ROOT,
    RETAIL_XANIM_DECODE_BAD_NAME,
    RETAIL_XANIM_DECODE_COUNT_MISMATCH,
    RETAIL_XANIM_DECODE_INDEX_OOB,
    RETAIL_XANIM_DECODE_SCRIPT_TABLE_MISSING,
    RETAIL_XANIM_DECODE_SCRIPT_STRING_MISSING,
    RETAIL_XANIM_DECODE_TRUNCATED,
    RETAIL_XANIM_DECODE_OUT_OF_ARENA,
    RETAIL_XANIM_DECODE_REGISTRATION_FAILED,
};

const char *RetailXAnimDecodeResultName(RetailXAnimDecodeResult result);

// Live XAnimParts driver for RetailWalkLoadZoneAssets (db_retail_walk.cpp):
// consumes one inline XAnimParts body in exact walk order (88-byte temp
// root, name, bone-name table, notifies, delta part, six data arrays,
// indices), widening field by field into zone-arena transaction storage,
// interning script strings, then committing atomically through
// Load_XAnimPartsAsset and requiring the returned pooled header (the same
// transactional pattern as the world registry: pre-commit the shared registry is untouched, rollback is
// discarding the transaction). Only the inline (-1) header form is
// supported; any other form fails loudly. Runs in both bounded and full
// loads: the bounded spmap script chain reaches Scr_PrecacheAnimTrees,
// which resolves every anim through DB_FindXAssetHeader.
bool RetailWalkLiveLoadXAnimParts(RetailZoneLoadSession *session,
                                  FsRetailFastfileReader *reader,
                                  uint32_t headerRef, XAssetHeader *header);
