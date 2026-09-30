#pragma once

#include "../universal/com_files.h"
#include "db_retail_zone.h"
#include "db_retail_walk.h"

struct RetailWorldLoadContext;

// B6 FxEffectDef identity slice: widen one inline FxEffectDef
// body field by field into a transaction-owned native FxEffectDef and
// register it through the existing Load_FxEffectDefAsset database owner.
//
// Wire contract (mirrors db_retail_walk.cpp's ReadRetailFxEffectDefBody,
// which stays the independent byte oracle): a 32-byte root in the temp
// block holds the name XString ref, flags, totalSize, msecLoopingLife, the
// three elem counts, and the elemDefs array ref; the name XString and the
// elemDefs array (sum of the three counts, 252-byte ILP32 records) follow in
// block 4, with each elem's nested sample/visual/effect/trail bodies
// consumed by the walk reader exactly as the reference Load_FxElemDef does.
//
// Widened now (real, never synthesized): name, every root scalar, and each
// FxElemDef's POD prefix (flags..atlas + elemType/visualCount/velocity/
// visibility counts, whose ILP32 and native offsets agree -- verified
// offsetof guards below), collMins/collMaxs, emitDist/emitDistVariance, and
// the sortOrder/lightingFrac/item-clip tail. Raw FxElemDef[].velSamples,
// visSamples, visuals and effectOnImpact/Death/Emitted/trailDef are
// consumed byte-exactly but explicitly left null (drawn/spawned activation
// is a later B6 continuation): the registered effect is a real identity and
// scalar graph, and every deferred nested object is counted so a consumer
// that reaches one fails loudly rather than dereferencing null silently.
//
// `worldContext` may be null (walk-only callers). `slotOffset` records the
// widened result in the zone-scoped nested table when nonzero, so later
// DB_ConvertOffsetToAlias-style references (weapon viewFlash/worldFlash
// slots) resolve to the registered asset exactly like the world decoder's
// own nested references.
bool RetailWalkLiveLoadFxEffectDef(RetailZoneLoadSession *session,
                                   FsRetailFastfileReader *reader,
                                   RetailWorldLoadContext *worldContext,
                                   XAssetHeader *out,
                                   RetailWalkDirectoryRecord *record,
                                   uint32_t *deferredNestedOut,
                                   uint32_t slotOffset);

// B6 ImpactFX identity: widen one inline FxImpactTable body --
// the 8-byte root (name XString, table ref) and the 12x33 FxEffectDefHandle
// grid -- resolving every non-null cell through the zone's nested table /
// live FX widener and registering through Load_FxImpactTableAsset. The
// retail fastfile registers this asset under its (usually empty) wire name,
// which is exactly the name CG_RegisterImpactEffects_FastFile looks up.
// A null cell stays null (the engine's missing-effect sentinel path is the
// separately registered fx_load defaultEffect, e.g. misc/missing_fx). This
// is identity/ownership only: no spawn or draw activation is claimed.
bool RetailWalkLiveLoadFxImpactTable(RetailZoneLoadSession *session,
                                     FsRetailFastfileReader *reader,
                                     RetailWorldLoadContext *worldContext,
                                     XAssetHeader *out);
