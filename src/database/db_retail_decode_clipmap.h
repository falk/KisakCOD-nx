#pragma once

#include "../universal/com_files.h"
#include "db_retail_zone.h"

// Native ClipMap registry:
// field-by-field widening of the already-walked 284-byte ClipMap wire root
// (db_retail_walk.cpp's ReadRetailClipMapBody, proven byte-accurate against
// the OAT oracle through M3c/M3e) into transaction-owned native storage --
// never a wire-root cast -- committed atomically into the engine's own
// collision singleton (cm, owned by db_registry.cpp's DB_XAssetPool table,
// the existing ClipMap database owner, never a parallel database).
//
// Wire contract (mirrors db_load.cpp Load_clipMap_t field order; every
// record offset below cross-checked against OAT's generated IW3 loader,
// build/src/ZoneCode/Game/IW3/XAssets/clipmap_t/clipmap_t_iw3_load_db.cpp,
// not inferred from gaps):
//   - 284-byte root in the temp block: name@0, isInUse@4, planeCount@8,
//     planes@12, numStaticModels@16, staticModelList@20, numMaterials@24,
//     materials@28, numBrushSides@32, brushsides@36, numBrushEdges@40,
//     brushEdges@44, numNodes@48, nodes@52, numLeafs@56, leafs@60,
//     leafbrushNodesCount@64, leafbrushNodes@68, numLeafBrushes@72,
//     leafbrushes@76, numLeafSurfaces@80, leafsurfaces@84, vertCount@88,
//     verts@92, triCount@96, triIndices@100, triEdgeIsWalkable@104,
//     borderCount@108, borders@112, partitionCount@116, partitions@120,
//     aabbTreeCount@124, aabbTrees@128, numSubModels@132, cmodels@136,
//     numBrushes u16@140, brushes@144, numClusters@148, clusterBytes@152,
//     visibility@156, vised@160, mapEnts@164, box_brush@168, box_model
//     (embedded cmodel_t, 72 bytes, no pointers)@172, dynEntCount[2]@244,
//     dynEntDefList[2]@248/252, dynEntPoseList[2]@256/260 (runtime block 1),
//     dynEntClientList[2]@264/268 (runtime), dynEntCollList[2]@272/276
//     (runtime), checksum@280.
//   - Top-level collision arrays stream inline for any nonzero slot and
//     bind null for a null slot (Android AllocForSlot: no -1 check at this
//     level, matching ReadRetailClipArray). A null slot beside a nonzero
//     count fails loudly (COUNT_MISMATCH): it would hand SV_SetBrushModel /
//     SV_LinkEntity a null pointer beside a nonzero count. A zero count
//     binds null for any slot form without consuming bytes (vacuous: zero
//     elements are never dereferenced). The one exception is a declared
//     (nonzero-count) planes array with a non-inline slot: the linker's
//     cross-asset dedup, sharing the already-widened GfxWorld planes bulk
//     that loads two ordinals earlier in the same zone, so no planes bytes
//     exist in the stream here. The decoder shares the world planes when
//     the count matches exactly, records every element at the alias
//     offsets for later plane slots, and unit-checks each plane on the
//     way in; anything else fails loudly. The M3e
//     real-killhouse walk (cursors=exact oat=1) proves every other
//     declared array slot there is inline, so the remaining strictness
//     cannot regress the map.
//   - Pointer-bearing records widen field by field, never memcpy'd whole:
//     cStaticModel_s (88 native vs 80 wire: XModel slot at +4 resolved
//     through the shared zone XModel widener, floats copied), cNode_t
//     (16 vs 8: plane slot resolved, children copied + range-checked),
//     cbrushside_t (16 vs 12: plane slot resolved, scalars copied),
//     cbrush_t (88 vs 80: sides/edge slots resolved, scalars copied, the
//     10-byte ILP32 align tail is padding and stays zeroed), and
//     CollisionPartition (16 vs 12: borders slot resolved, scalars copied).
//     Pointer-free records (cplane_s, dmaterial_t, cLeaf_t, leafbrushes,
//     leafsurfaces, verts, triIndices, walkability bits, CollisionBorder,
//     CollisionAabbTree, cmodel_t, box_model, DynEntityPose, DynEntityColl,
//     visibility bytes, checksum) copy verbatim with wire/native sizes
//     pinned by static_assert.
//   - DynEntityDef (native layout pinned by offsetof asserts; wire 96:
//     type@0, pose@4, xModel@32, brushModel@36, physicsBrushModel@38,
//     destroyFx@40, destroyPieces@44, physPreset@48, health@52, mass@56,
//     contents@92): the XModel slot widens through the shared zone
//     widener; destroyFx resolves through the real FxEffectDef loader and
//     physPreset through the nested PhysPreset widener, each
//     recording its declaring field slot so sibling aliases bind the same
//     registered object. A non-null destroyPieces slot fails loudly (no
//     live XModelPieces widener exists; killhouse declares none).
//     DynEnt pose/client/coll lists are
//     runtime block-1 expansion (ExpandRuntime, zero stream bytes,
//     matching the walk): poses/colls copy from the expanded zeros,
//     clients widen 12->16 from them -- the honest runtime-initial state.
//   - Alias (non-null, non-inline) element slots decode canonically to a
//     block-4 offset and bind an already-widened record through a
//     transaction-local span map (planes, brush sides, brush edges,
//     partition borders, brushes for the box alias). Leaf-brush u16 lists
//     resolve by containment across every recorded collision bulk span:
//     the linker dedups identical runs across struct boundaries, so a
//     list may name any already-streamed span, never a future one.
//     Entries bind verbatim like retail (boundary indices included);
//     a miss fails loudly (BAD_ALIAS), never binds a guess.
//   - Node children are range-checked at widen time (positive < numNodes,
//     negative -1-child < numLeafs -- exactly CM_BoxLeafnums' own indexing)
//     and leafBrushNode is 0-or-in-range: an out-of-range value there is
//     an instant OOB read in SV_LinkEntity's own traversal, so this is a
//     crash-catching boundary, not hardening. Leaf-brush u16 entries are
//     bound verbatim (see above).
//   - The nested MapEnts leaf widens and registers through the session
//     table here (inline entity bytes streamed, inline-or-alias name),
//     replacing the TEMP walk-only capture bridge: cm.mapEnts points at
//     the registered asset, which is what the preflight bridge and
//     G_ParseSpawnVars already consume by name.
// Live ClipMap driver for RetailWalkLoadZoneAssets (db_retail_walk.cpp):
// consumes one inline ClipMap body in exact db_load order, widening into a
// zone-arena transaction and committing once (one memcpy into cm, then
// session registration whose returned pooled header must equal &cm --
// pre-commit cm is untouched, rollback is discarding the transaction).
// Only the inline (-1) directory-header form is supported; any other form
// fails loudly. Runs in both bounded and full policy (the bounded spmap
// spawn path needs cm.cmodels/planes/nodes/leafs live). No renderer
// resources are created.

enum RetailClipMapDecodeResult
{
    RETAIL_CLIPMAP_DECODE_OK,
    RETAIL_CLIPMAP_DECODE_BAD_ARGUMENT,
    RETAIL_CLIPMAP_DECODE_BAD_HEADER,
    RETAIL_CLIPMAP_DECODE_BAD_ROOT,
    RETAIL_CLIPMAP_DECODE_BAD_NAME,
    RETAIL_CLIPMAP_DECODE_COUNT_MISMATCH,
    RETAIL_CLIPMAP_DECODE_BAD_ALIAS,
    RETAIL_CLIPMAP_DECODE_UNSUPPORTED_ASSET,
    RETAIL_CLIPMAP_DECODE_TRUNCATED,
    RETAIL_CLIPMAP_DECODE_OUT_OF_ARENA,
    RETAIL_CLIPMAP_DECODE_REGISTRATION_FAILED,
};

const char *RetailClipMapDecodeResultName(RetailClipMapDecodeResult result);

struct RetailWorldLoadContext;

bool RetailWalkLiveLoadClipMap(RetailZoneLoadSession *session,
                               FsRetailFastfileReader *reader,
                               uint32_t headerRef, XAssetHeader *header,
                               RetailWorldLoadContext *sharedCtx);
