#include "db_retail_zone.h"

#include <cstring>

namespace
{
constexpr uint32_t kInlineReference = 0xffffffffu;
constexpr uint32_t kInsertReference = 0xfffffffeu;
constexpr uint32_t kHandleSegmentSize = 256;
constexpr uint32_t kHandleSegmentCount = 128;
static_assert(ASSET_TYPE_COUNT == 33, "retail dispatcher must cover every XAssetType");

#ifdef KISAK_RETAIL_FS_PROOF_HOST
// pointer-provenance audit accounting (host proof links only).
constexpr uint32_t kRetailZoneArenaUseSlots = 256;
std::size_t g_retailZoneArenaUsed[kRetailZoneArenaUseSlots];

void RetailZoneArenaUseNote(uint32_t zoneIndex, std::size_t used)
{
    if (zoneIndex == 0 || zoneIndex >= kRetailZoneArenaUseSlots)
        return;
    if (used > g_retailZoneArenaUsed[zoneIndex])
        g_retailZoneArenaUsed[zoneIndex] = used;
}

void RetailZoneArenaUseReset(uint32_t zoneIndex)
{
    if (zoneIndex < kRetailZoneArenaUseSlots)
        g_retailZoneArenaUsed[zoneIndex] = 0;
}
#endif

bool IsNoStreamType(XAssetType type)
{
    switch (type)
    {
    case ASSET_TYPE_XMODELPIECES:
    case ASSET_TYPE_UI_MAP:
    case ASSET_TYPE_SNDDRIVER_GLOBALS:
    case ASSET_TYPE_AITYPE:
    case ASSET_TYPE_MPTYPE:
    case ASSET_TYPE_CHARACTER:
    case ASSET_TYPE_XMODELALIAS:
        return true;
    default:
        return false;
    }
}

RetailAssetHandle *GetHandle(RetailZoneLoadSession *session, uint32_t reference)
{
    if (!reference || reference > session->assetHandleCount)
        return nullptr;
    const uint32_t index = reference - 1;
    RetailAssetHandleSegment *segment = session->assetHandleSegments[index / kHandleSegmentSize];
    return segment ? &segment->entries[index % kHandleSegmentSize] : nullptr;
}

bool AddHandle(RetailZoneLoadSession *session, XAssetType type, XAssetHeader header,
               uint32_t *reference)
{
    if (session->assetHandleCount == kHandleSegmentSize * kHandleSegmentCount)
        return false;
    const uint32_t index = session->assetHandleCount;
    RetailAssetHandleSegment *&segment = session->assetHandleSegments[index / kHandleSegmentSize];
    if (!segment)
    {
        segment = static_cast<RetailAssetHandleSegment *>(
            RetailZoneLoadSessionAlloc(session, sizeof(*segment), alignof(RetailAssetHandleSegment)));
        if (!segment)
            return false;
        std::memset(segment, 0, sizeof(*segment));
    }
    segment->entries[index % kHandleSegmentSize] = {type, header};
    ++session->assetHandleCount;
    *reference = index + 1;
    return true;
}
} // namespace

#ifdef KISAK_RETAIL_FS_PROOF_HOST
std::size_t RetailZoneArenaUsedBytesForProof(uint32_t zoneIndex)
{
    if (zoneIndex == 0 || zoneIndex >= kRetailZoneArenaUseSlots)
        return 0;
    return g_retailZoneArenaUsed[zoneIndex];
}
#endif

bool RetailZoneLoadSessionBegin(RetailZoneLoadSession *session, const char *name,
                                int32_t flags, const uint32_t blockSizes[9],
                                std::size_t nativeArenaBytes)
{
    if (!session || !name || !name[0] || !blockSizes || nativeArenaBytes == 0)
        return false;
    std::memset(session, 0, sizeof(*session));
    std::strncpy(session->zoneName, name, sizeof(session->zoneName) - 1);
    session->zoneName[sizeof(session->zoneName) - 1] = '\0';
    void *nativeArenaMemory = nullptr;
    uint32_t reservedArenaBytes = static_cast<uint32_t>(nativeArenaBytes);
    if (nativeArenaBytes > UINT32_MAX ||
        !DB_RetailZoneBegin(name, flags, blockSizes, &reservedArenaBytes,
                            &session->zoneIndex, &session->zoneMemory, &nativeArenaMemory))
        return false;
    nativeArenaBytes = reservedArenaBytes;
    if (!RetailWireBlocksInit(&session->wire, session->zoneMemory) ||
        !RetailNativeArenaInit(&session->arena, nativeArenaMemory, nativeArenaBytes))
    {
        RetailZoneLoadSessionAbort(session);
        return false;
    }
#ifdef KISAK_RETAIL_FS_PROOF_HOST
    // This zone slot may be reused by the stub registry: start its proof
    // high-water at zero so the pointer-provenance scan reads only this
    // session's arena bytes.
    RetailZoneArenaUseReset(session->zoneIndex);
#endif
    session->active = true;
    return true;
}

XAssetHeader RetailZoneLoadSessionRegister(RetailZoneLoadSession *session, XAssetType type,
                                           XAssetHeader header)
{
    XAssetHeader empty{};
    if (!session || !session->active || !header.data)
        return empty;
    return DB_RetailZoneRegister(type, header, session->zoneIndex);
}

bool RetailZoneLoadSessionRegisterMenuGraph(RetailZoneLoadSession *session,
                                             MenuList *list,
                                             menuDef_t *const *menus,
                                             uint32_t menuCount,
                                             MenuList **out)
{
    if (!session || !session->active || !list || !out || list->menuCount < 0 ||
        static_cast<uint32_t>(list->menuCount) != menuCount ||
        (menuCount && (!menus || !list->menus)))
        return false;

    // Register children first.  DB_AddXAsset may return an older canonical
    // header when an override is not active, so retain every returned menu
    // rather than assuming the widened pointer survived registration.
    for (uint32_t i = 0; i < menuCount; ++i)
    {
        if (!menus[i] || !menus[i]->window.name)
            return false;
        XAssetHeader registered = RetailZoneLoadSessionRegister(
            session, ASSET_TYPE_MENU, {menus[i]});
        if (!registered.menu)
            return false;
        list->menus[i] = registered.menu;
        for (int32_t item = 0; item < registered.menu->itemCount; ++item)
        {
            if (!registered.menu->items || !registered.menu->items[item])
                return false;
            registered.menu->items[item]->parent = registered.menu;
        }
    }

    if (!list->name)
        return false;
    XAssetHeader registered = RetailZoneLoadSessionRegister(
        session, ASSET_TYPE_MENULIST, {list});
    if (!registered.menuList)
        return false;
    *out = registered.menuList;
    return true;
}

void *RetailZoneLoadSessionAlloc(RetailZoneLoadSession *session, std::size_t bytes,
                                  std::size_t alignment)
{
    if (!session || !session->active)
        return nullptr;
    void *result = RetailNativeArenaAlloc(&session->arena, bytes, alignment);
#ifdef KISAK_RETAIL_FS_PROOF_HOST
    // pointer-provenance audit: record how far this zone's arena has
    // been written so the close-time scan covers every live widened record.
    RetailZoneArenaUseNote(session->zoneIndex, session->arena.used);
#endif
    return result;
}

void RetailZoneLoadSessionReleaseHandles(RetailZoneLoadSession *session)
{
    if (!session)
        return;
    for (uint32_t i = 0; i < kHandleSegmentCount; ++i)
    {
        if (!session->assetHandleSegments[i])
            continue;
        std::memset(session->assetHandleSegments[i], 0,
                    sizeof(*session->assetHandleSegments[i]));
        session->assetHandleSegments[i] = nullptr;
    }
    session->assetHandleCount = 0;
}

bool RetailZoneLoadSessionTrackResource(RetailZoneLoadSession *session, void *ptr,
                                         void (*release)(void *))
{
    if (!session || !session->active || !ptr || !release || session->resourceCount == 64)
        return false;
    session->resources[session->resourceCount++] = {ptr, release};
    return true;
}

bool RetailZoneLoadSessionSetAssetLoader(RetailZoneLoadSession *session, XAssetType type,
                                         RetailZoneAssetLoader loader, void *context)
{
    if (!session || !session->active || type < 0 || type >= ASSET_TYPE_COUNT || IsNoStreamType(type))
        return false;
    session->assetLoaders[type] = loader;
    session->assetLoaderContexts[type] = context;
    return true;
}

RetailZoneAssetResult RetailZoneLoadSessionDispatchAsset(RetailZoneLoadSession *session,
                                                         XAssetType expectedType,
                                                         uint32_t reference,
                                                         XAssetHeader *header,
                                                         uint32_t *handleReference)
{
    if (!session || !session->active || !header || !handleReference || expectedType < 0 ||
        expectedType >= ASSET_TYPE_COUNT)
        return RETAIL_ZONE_ASSET_BAD_TYPE;
    *header = {};
    *handleReference = 0;
    if (IsNoStreamType(expectedType))
        return RETAIL_ZONE_ASSET_OK;
    if (!reference)
        return RETAIL_ZONE_ASSET_OK;
    if (reference != kInlineReference && reference != kInsertReference)
    {
        RetailAssetHandle *alias = GetHandle(session, reference);
        if (!alias)
            return RETAIL_ZONE_ASSET_BAD_REFERENCE;
        if (alias->type != expectedType)
            return RETAIL_ZONE_ASSET_TYPE_MISMATCH;
        *header = alias->header;
        *handleReference = reference;
        return RETAIL_ZONE_ASSET_OK;
    }
    RetailZoneAssetLoader loader = session->assetLoaders[expectedType];
    if (!loader)
        return RETAIL_ZONE_ASSET_UNSUPPORTED;
    XAssetHeader loaded{};
    if (!loader(session, expectedType, reference == kInsertReference,
                session->assetLoaderContexts[expectedType], &loaded) || !loaded.data)
        return RETAIL_ZONE_ASSET_UNSUPPORTED;
    const XAssetHeader registered = RetailZoneLoadSessionRegister(session, expectedType, loaded);
    if (!registered.data)
        return RETAIL_ZONE_ASSET_REGISTRATION_FAILED;
    if (!AddHandle(session, expectedType, registered, handleReference))
        return RETAIL_ZONE_ASSET_REGISTRATION_FAILED;
    *header = registered;
    return RETAIL_ZONE_ASSET_OK;
}

const char *RetailZoneAssetResultName(RetailZoneAssetResult result)
{
    static const char *const names[] = {"ok", "bad_type", "unsupported", "bad_reference",
                                        "type_mismatch", "registration_failed"};
    return result >= RETAIL_ZONE_ASSET_OK && result <= RETAIL_ZONE_ASSET_REGISTRATION_FAILED ?
        names[result] : "invalid_result";
}

bool RetailZoneLoadSessionAbort(RetailZoneLoadSession *session)
{
    if (!session)
        return false;
    for (uint32_t i = session->resourceCount; i != 0; --i)
    {
        RetailZoneResource &resource = session->resources[i - 1];
        if (resource.ptr && resource.release)
            resource.release(resource.ptr);
        resource = {};
    }
    session->resourceCount = 0;
    if (session->zoneIndex && !DB_RetailZoneEnd(session->zoneIndex))
        return false;
    RetailNativeArenaDestroy(&session->arena);
    std::memset(session, 0, sizeof(*session));
    return true;
}

void RetailZoneLoadSessionPublishGeometry(RetailZoneLoadSession *session)
{
    if (!session || !session->zoneIndex)
        return;
    DB_RetailZoneUploadGeometryBuffers(session->zoneIndex);
}

bool RetailZoneLoadSessionIsEmpty(const RetailZoneLoadSession *session)
{
    return session != nullptr && !session->active && session->resourceCount == 0 &&
        session->zoneIndex == 0 && session->zoneMemory == nullptr && session->arena.data == nullptr;
}
