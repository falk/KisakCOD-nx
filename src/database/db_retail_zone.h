#pragma once

#include <cstddef>
#include <cstdint>

#include "database.h"
#include "db_retail_wire.h"
#include "../universal/com_files.h"

struct RetailZoneResource
{
    void *ptr;
    void (*release)(void *);
};

// Android's pseudo-block 15 is a typed table of registered headers, not a
// second asset registry.  References are one-based so zero remains null.
struct RetailAssetHandle
{
    XAssetType type;
    XAssetHeader header;
};

struct RetailAssetHandleSegment
{
    RetailAssetHandle entries[256];
};

struct RetailZoneLoadSession;
typedef bool (*RetailZoneAssetLoader)(RetailZoneLoadSession *session, XAssetType type,
                                      bool insert, void *context, XAssetHeader *header);

enum RetailZoneAssetResult
{
    RETAIL_ZONE_ASSET_OK,
    RETAIL_ZONE_ASSET_BAD_TYPE,
    RETAIL_ZONE_ASSET_UNSUPPORTED,
    RETAIL_ZONE_ASSET_BAD_REFERENCE,
    RETAIL_ZONE_ASSET_TYPE_MISMATCH,
    RETAIL_ZONE_ASSET_REGISTRATION_FAILED,
};

struct RetailZoneLoadSession
{
    uint32_t zoneIndex;
    // The filename this session was begun with (e.g.
    // "zone/english/killhouse.ff"): cross-phase key. The walk-only reader
    // and the live widener run in separate sessions; engine-image offsets
    // the walk records (matmem array site below) are only meaningful to a
    // live load of this same file.
    char zoneName[64];
    XZoneMemory *zoneMemory;
    RetailWireBlocks wire;
    RetailNativeArena arena;
    RetailZoneResource resources[64];
    uint32_t resourceCount;
    RetailAssetHandleSegment *assetHandleSegments[128];
    uint32_t assetHandleCount;
    RetailZoneAssetLoader assetLoaders[ASSET_TYPE_COUNT];
    void *assetLoaderContexts[ASSET_TYPE_COUNT];
    // Mirrors the wire reader's own imagePoolNameRefs[]/RETAIL_FASTFILE_POOL_BLOCK
    // indexing (see com_files.h), but holds the actual widened GfxImage* for
    // each pool slot instead of just its (often empty) nameRef. Anonymous
    // inline material textures are identified by this pool index, not by
    // name -- routing them through the named DB_AddXAsset registry instead
    // (as every other asset correctly does) makes every empty-named entry
    // collide into one shared, wrong slot. Sized to the reader pool so any
    // pooled slot the zone declares stays session-addressable. See
    // db_retail_decode_material.cpp.
    GfxImage *imagePool[RETAIL_FASTFILE_IMAGE_POOL_MAX];
    bool active;
    // Live loads widen+register walked-deferred inline images (XModel-nested
    // textures and the like) so later aliases bind real assets instead of
    // null; walk-only proofs must observe zero registrations and leave this
    // false. Set only by the live zone-load entrypoint, never by the
    // walk-only directory/census readers.
    bool widenNestedImages;
    // Capture slot for the ClipMap-nested MapEnts (script-chain entity
    // support): the bounded spmap path reaches the real game script chain,
    // which parses spawn vars, but bounded mode never runs CM_LoadMap so
    // cm.mapEnts stays NULL. The map's MapEnts lives nested inside its
    // ClipMap body (never a direct root), so when the request flag is set,
    // the nested-MapEnts walk branch publishes the parsed inline name +
    // entity spans here instead of dropping them; the live ClipMap branch
    // then copies them into zone-lifetime arena storage and registers the
    // native asset. Integer spans only, so the walk side needs no
    // allocator: walk-only link closures (which gc out the live loader)
    // keep linking. Cleared by session begin (memset); the live branch
    // consumes immediately after its own ordinal's walk, before any later
    // ordinal can append past the spans (blocks only grow, never move).
    bool captureNestedMapEnts;
    bool capturedNestedMapEnts;
    uint32_t capturedMapEntsNameStart;
    uint32_t capturedMapEntsNameBytes;
    uint32_t capturedMapEntsEntStart;
    uint32_t capturedMapEntsEntLen;
    // Zone script-string table (XAssetList root, B3): the count from the
    // list root plus a lazily-built arena-owned wire-index-to-block-4-offset
    // table (UINT32_MAX = null entry). Populated by RetailWalkOpenDirectory's
    // count and by the XAnim decoder on first use; transaction-owned, freed
    // with the zone arena. Lets the XAnim widener resolve u16 bone/notify
    // wire indices to verbatim engine string bytes for real SL_* interning
    // without depending on a previous session or process-global state.
    uint32_t scriptStringCount;
    uint32_t *scriptStringOffsets;
};

bool RetailZoneLoadSessionBegin(RetailZoneLoadSession *session, const char *name,
                                int32_t flags, const uint32_t blockSizes[9],
                                std::size_t nativeArenaBytes);
// Publishes this session's finished block-7/8 bytes to the zone's D3D
// geometry buffers (see DB_RetailZoneUploadGeometryBuffers in database.h).
// Called once, on the success path of a live zone load, after every asset in
// the zone has streamed its vertices/indices into those blocks.
void RetailZoneLoadSessionPublishGeometry(RetailZoneLoadSession *session);
XAssetHeader RetailZoneLoadSessionRegister(RetailZoneLoadSession *session, XAssetType type,
                                           XAssetHeader header);
// Register a widened MenuList and its menus through the ordinary XAsset
// registry.  The returned list/menu headers are canonical (and may therefore
// belong to an override), and every item parent is repaired to that canonical
// menu before the caller can expose the graph to UI consumers.
bool RetailZoneLoadSessionRegisterMenuGraph(RetailZoneLoadSession *session,
                                             MenuList *list,
                                             menuDef_t *const *menus,
                                             uint32_t menuCount,
                                             MenuList **out);
void *RetailZoneLoadSessionAlloc(RetailZoneLoadSession *session, std::size_t bytes,
                                  std::size_t alignment);
#ifdef KISAK_RETAIL_FS_PROOF_HOST
// pointer-provenance audit: greatest arena high-water observed for a
// live session's zone slot (0 when none). The host registry double scans only
// the zone-owned bytes that can hold live pointer fields, so its reader-owned
// pointer sweep reads real bodies instead of a zeroed 96 MB arena tail.
std::size_t RetailZoneArenaUsedBytesForProof(uint32_t zoneIndex);
#endif
bool RetailZoneLoadSessionTrackResource(RetailZoneLoadSession *session, void *ptr,
                                          void (*release)(void *));
bool RetailZoneLoadSessionSetAssetLoader(RetailZoneLoadSession *session, XAssetType type,
                                         RetailZoneAssetLoader loader, void *context);
RetailZoneAssetResult RetailZoneLoadSessionDispatchAsset(RetailZoneLoadSession *session,
                                                         XAssetType expectedType,
                                                         uint32_t reference,
                                                         XAssetHeader *header,
                                                         uint32_t *handleReference);
// Zeroes the session's alias handle table (Android pseudo-block 15).  The
// table resolves in-file references while the walk runs; afterward it is dead
// scaffolding whose header entries may point at another zone's canonical body
// (an override) that can unload and have its arena reused.  The successful
// walk path must call this before it returns so no live zone keeps a stale
// cross-zone pointer in its arena.
void RetailZoneLoadSessionReleaseHandles(RetailZoneLoadSession *session);
const char *RetailZoneAssetResultName(RetailZoneAssetResult result);
bool RetailZoneLoadSessionAbort(RetailZoneLoadSession *session);
bool RetailZoneLoadSessionIsEmpty(const RetailZoneLoadSession *session);

