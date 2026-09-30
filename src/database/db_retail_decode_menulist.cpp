#include "db_retail_decode_menulist.h"

#include "db_retail_decode_material.h"
#include "db_retail_decode_ui.h"
#include "db_retail_decode_world.h"
#include "db_retail_walk.h"
#include "db_retail_wire.h"
#include "../qcommon/qcommon.h"

#include <cstring>
#include <cstddef>

namespace
{
constexpr uint32_t kInlineRef = 0xffffffffu;
constexpr uint32_t kInsertRef = 0xfffffffeu;
constexpr uint32_t kMenuCapacity = 256;
constexpr uint32_t kItemCapacity = 1024;
constexpr uint32_t kHandlerCapacity = 32;
constexpr uint32_t kEntryCapacity = 256;

uint32_t ReadLe32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// An inline background (embedded directly in the ItemDef/menuDef_t wire body rather than a
// named top-level zone Material) has no name a later by-name lookup could ever find -- widen
// it directly from the already-decoded wire body instead of asking ResolveMaterialRef to find
// an asset that was never registered under that name.
//
// A non-inline background ref is exactly what the original Load_MaterialHandle
// handles: an alias into the block-4 slot some earlier Load_MaterialAsset
// patched with the registered Material pointer.  The port's equivalent of that
// patch is the zone slot ledger, so the ref resolves through
// RetailWorldResolveNestedAlias -- the same seam the font path's materialRef
// and the world materials use.  A miss fails loudly: this is not a name to
// synthesize or default-substitute.  A ref that resolves to a declared null
// slot binds null exactly like the original's copied value.
bool ResolveBackgroundMaterial(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                               RetailWorldLoadContext *worldContext,
                               bool wasInline, const FsRetailFastfileMaterial &inlineMaterial,
                               const FsRetailFastfileTextureDef *inlineTextures,
                               uint32_t backgroundRef,
                               uint32_t directoryOffset, uint32_t directoryBytes,
                               const char *const *stubNames, uint32_t stubCount,
                               Material **out, char *deferredName,
                               std::size_t deferredNameCapacity)
{
    if (!out)
        return false;
    *out = nullptr;
    if (deferredName && deferredNameCapacity)
        deferredName[0] = '\0';
    if (wasInline)
    {
        XAssetHeader header{};
        bool deferredAlias = false;
        const bool widened = RetailWidenMaterialFromWire(
            session, reader, inlineMaterial, inlineTextures, directoryOffset, directoryBytes,
            stubNames, stubCount, nullptr, &header, false, false, false,
            &deferredAlias, deferredName, deferredNameCapacity);
        if (widened && header.material)
        {
            *out = header.material;
            return true;
        }
        if (widened)
        {
            // An inline comma-stub body whose stripped owner is outside the
            // loaded closure: production DB_FindXAssetHeader would return
            // the type's default entry (a registry default body this port
            // deliberately never manufactures).  The consumer binds
            // null; a real replacement body would be a fabricated asset.
            Com_Printf(8, "ResolveBackgroundMaterial: inline comma-stub background outside "
                          "the closure binds null\n");
            return true;
        }
        if (deferredAlias && deferredName && deferredName[0])
        {
            if (RetailIsNonSpMaterialAlias(deferredName))
            {
                // Owner outside the loaded closure: bind null rather than
                // retaining a deferral that can never resolve.
                deferredName[0] = '\0';
                return true;
            }
            return true;
        }
        Com_Printf(0, "ResolveBackgroundMaterial: RetailWidenMaterialFromWire failed for inline background "
                      "nameRef=0x%08x techRef=0x%08x texCount=%u\n",
                   inlineMaterial.nameRef, inlineMaterial.techniqueSetRef,
                   static_cast<unsigned>(inlineMaterial.textureCount));
        return false;
    }
    if (!backgroundRef)
        return true;
    if (worldContext)
    {
        XAssetHeader resolved{};
        if (RetailWorldResolveNestedAlias(worldContext, backgroundRef, ASSET_TYPE_MATERIAL, &resolved))
        {
            // A declared-null slot binds null exactly like the original's
            // copied value, so success with a null material is valid.
            *out = resolved.material;
            return true;
        }
        char pendingName[64]{};
        bool pendingBindNull = false;
        if (RetailWorldFindDeferredSlotName(worldContext, backgroundRef, ASSET_TYPE_MATERIAL,
                                            pendingName, sizeof(pendingName), &pendingBindNull))
        {
            if (pendingBindNull)
            {
                // The stub's owner is outside the loaded closure: bind null
                // (production would return the type default entry, a body
                // this port deliberately never manufactures).
                return true;
            }
            // The ref addresses a comma-stub material's directory slot whose
            // owner zone has not loaded yet: defer to that owner exactly like
            // the stub's own registration does.
            if (deferredName && deferredNameCapacity)
            {
                std::strncpy(deferredName, pendingName, deferredNameCapacity - 1);
                deferredName[deferredNameCapacity - 1] = '\0';
            }
            return true;
        }
    }
    // Leftover forms: the exact directory/stub/name rules in strict mode
    // (no synthesis, no hardcoded identity, no white/$default
    // substitution).  A required IWI alias whose owner zone has not loaded
    // yet comes back as a deferred name; the caller records the slot and
    // patch-fills it when the owner registers.
    Material *material = ResolveMaterialRef(reader, backgroundRef, directoryOffset, directoryBytes,
                                            stubNames, stubCount, true, deferredName,
                                            deferredNameCapacity);
    if (material)
    {
        *out = material;
        return true;
    }
    if (deferredName && deferredName[0])
    {
        if (RetailIsNonSpMaterialAlias(deferredName))
        {
            // Name-form consumer ref to a stub whose owner is outside the
            // loaded closure: bind null, exactly like the directory-slot
            // form above.
            deferredName[0] = '\0';
            return true;
        }
        return true;
    }
    Com_Printf(0, "ResolveBackgroundMaterial: alias ref=0x%08x unresolved in the zone slot "
                  "ledger\n", backgroundRef);
    return false;
}

bool LoadOneItem(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                 uint32_t itemHeaderRef, itemDef_s **out,
                 uint32_t directoryOffset, uint32_t directoryBytes,
                 const char *const *stubNames, uint32_t stubCount,
                 RetailWorldLoadContext *worldContext)
{
    if (!session || !session->active || !reader || !out)
        return false;
    *out = nullptr;
    if (!itemHeaderRef)
        return true;
    if (itemHeaderRef != kInlineRef)
        return false;

    const uint32_t itemBodyOffset =
        (FS_RetailFastfileBlockCursor(reader, 4) + 3u) & ~3u;
    FsRetailFastfileItemDef wireItem;
    FsRetailFastfileItemKeyHandler handlers[kHandlerCapacity];
    FsRetailFastfileExpressionEntry entries[kEntryCapacity];
    const FsRetailFastfileWireResult readRes =
        FS_ReadRetailFastfileItemDef(reader, itemHeaderRef, &wireItem, handlers, kHandlerCapacity,
                                     entries, kEntryCapacity);
    if (readRes != FS_RETAIL_FF_WIRE_OK)
    {
        Com_Printf(0, "LoadOneItem: FS_ReadRetailFastfileItemDef failed res=%d\n", readRes);
        return false;
    }
    if (!RetailZoneLoadSessionSyncFromReader(session, reader, 4))
        return false;

    char deferredName[64]{};
    Material *bg = nullptr;
    if (!ResolveBackgroundMaterial(session, reader, worldContext,
                                   wireItem.backgroundWasInline,
                                   wireItem.backgroundInlineMaterial, wireItem.backgroundInlineTextures,
                                   wireItem.backgroundRef, directoryOffset, directoryBytes,
                                   stubNames, stubCount, &bg, deferredName, sizeof(deferredName)))
    {
        Com_Printf(0, "LoadOneItem: background ref=0x%08x unresolved (inline=%d)\n",
                   wireItem.backgroundRef, wireItem.backgroundWasInline ? 1 : 0);
        return false;
    }
    if (wireItem.backgroundWasInline && bg && worldContext &&
        !RetailWorldRecordZoneSlot(worldContext, itemBodyOffset + 152u,
                                   ASSET_TYPE_MATERIAL, XAssetHeader{bg}))
    {
        Com_Printf(0, "LoadOneItem: background material slot ledger full at %u\n",
                   itemBodyOffset + 152u);
        return false;
    }

    const RetailUiDecodeResult decodeRes =
        RetailDecodeItemDef(session, &wireItem, handlers, wireItem.handlerCount, entries,
                            wireItem.expressionEntryCount, out, bg);
    if (decodeRes != RETAIL_UI_DECODE_OK)
    {
        Com_Printf(0, "LoadOneItem: RetailDecodeItemDef failed res=%d type=%d typeDataRef=0x%x bgRef=0x%x focusSoundRef=0x%x parentRef=0x%x\n",
                   decodeRes, wireItem.type, wireItem.typeDataRef, wireItem.backgroundRef,
                   wireItem.focusSoundRef, wireItem.parentRef);
        return false;
    }
    if (!bg && deferredName[0] && *out &&
        !RetailDeferMaterialAlias(&(*out)->window.background, deferredName, session->zoneIndex))
        return false;
    return true;
}
} // namespace

bool RetailWalkLiveLoadMenuList(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                                uint32_t headerRef, XAssetHeader *header,
                                uint32_t directoryOffset, uint32_t directoryBytes,
                                const char *const *stubNames, uint32_t stubCount,
                                RetailWorldLoadContext *worldContext)
{
    Com_Printf(0, "RetailWalkLiveLoadMenuList: start headerRef=0x%x\n", headerRef);
    if (!session || !session->active || !reader || !header)
    {
        Com_Printf(0, "RetailWalkLiveLoadMenuList: bad args session=%p reader=%p header=%p\n",
                   session, reader, header);
        return false;
    }
    *header = {};

    FsRetailFastfileMenuList wireList;
    uint32_t menuRefs[kMenuCapacity];
    const FsRetailFastfileWireResult listRes =
        FS_ReadRetailFastfileMenuList(reader, headerRef, &wireList, menuRefs, kMenuCapacity);
    if (listRes != FS_RETAIL_FF_WIRE_OK)
    {
        Com_Printf(0, "RetailWalkLiveLoadMenuList: FS_ReadRetailFastfileMenuList failed res=%d\n", listRes);
        return false;
    }
    Com_Printf(0, "RetailWalkLiveLoadMenuList: nameRef=0x%x menuCount=%u menusRef=0x%x\n",
               wireList.nameRef, wireList.menuCount, wireList.menusRef);
    if (wireList.menuCount > kMenuCapacity)
    {
        Com_Printf(0, "RetailWalkLiveLoadMenuList: menuCount %u > cap %u\n", wireList.menuCount, kMenuCapacity);
        return false;
    }

    menuDef_t *menus[kMenuCapacity];
    for (uint32_t i = 0; i < wireList.menuCount; ++i)
    {
        if (!menuRefs[i])
        {
            menus[i] = nullptr;
            continue;
        }
        if (menuRefs[i] != kInlineRef && menuRefs[i] != kInsertRef)
        {
            // Original Load_menuDef_ptr's alias form (DB_ConvertOffsetToAlias):
            // the block-4 slot holds the registered pointer the declaring -2
            // entry wrote. Resolve it through the zone slot ledger; a
            // synthesized stand-in menu would register an asset the retail
            // zones never declared.
            XAssetHeader resolved{};
            if (!worldContext ||
                !RetailWorldResolveNestedAlias(worldContext, menuRefs[i], ASSET_TYPE_MENU,
                                               &resolved) ||
                !resolved.menu)
            {
                Com_Printf(0, "RetailWalkLiveLoadMenuList: menu[%u] alias 0x%08x unresolved\n",
                           i, menuRefs[i]);
                return false;
            }
            menus[i] = resolved.menu;
            continue;
        }
        // Original DB_InsertPointer: reserve the 4-byte block-4 slot this
        // declaring menu's registered pointer is written into, so later
        // aliases (this list or a later zone) resolve it.
        uint32_t insertSlot = 0;
        if (menuRefs[i] == kInsertRef)
        {
            uint8_t *slot = RetailWireBlocksAlloc(&session->wire, 4, 4, 4);
            if (!slot)
            {
                Com_Printf(0, "RetailWalkLiveLoadMenuList: menu[%u] insert slot alloc failed\n", i);
                return false;
            }
            insertSlot = static_cast<uint32_t>(slot - session->zoneMemory->blocks[4].data);
        }

        FsRetailFastfileMenu wireMenu;
        FsRetailFastfileItemKeyHandler handlers[kHandlerCapacity];
        FsRetailFastfileExpressionEntry entries[kEntryCapacity];
        const FsRetailFastfileWireResult prefixRes =
            FS_ReadRetailFastfileMenuPrefix(reader, kInlineRef, &wireMenu, handlers,
                                            kHandlerCapacity, entries, kEntryCapacity);
        if (prefixRes != FS_RETAIL_FF_WIRE_OK)
        {
            Com_Printf(0, "RetailWalkLiveLoadMenuList: FS_ReadRetailFastfileMenuPrefix failed res=%d\n", prefixRes);
            return false;
        }
        if (wireMenu.itemCount < 0 || static_cast<uint32_t>(wireMenu.itemCount) > kItemCapacity)
        {
            Com_Printf(0, "RetailWalkLiveLoadMenuList: bad itemCount %d\n", wireMenu.itemCount);
            return false;
        }

        itemDef_s *items[kItemCapacity];
        if (wireMenu.itemCount)
        {
            const uint8_t *itemRefArray = FS_RetailFastfileBlockData(reader, 4);
            if (!itemRefArray)
            {
                Com_Printf(0, "RetailWalkLiveLoadMenuList: itemRefArray is null\n");
                return false;
            }
            for (int32_t item = 0; item < wireMenu.itemCount; ++item)
            {
                const uint32_t itemHeaderRef =
                    ReadLe32(itemRefArray + wireMenu.itemsOffset + static_cast<uint32_t>(item) * 4u);
                if (!itemHeaderRef)
                {
                    items[item] = nullptr;
                    continue;
                }
                if (itemHeaderRef != kInlineRef)
                {
                    // The original Load_itemDef_ptrArray only has the
                    // allocate-and-stream-inline form (no alias/insert
                    // branch): any non-zero slot is a body. This port does
                    // not implement that shape, and a zeroed stand-in item
                    // would silently drop the body; fail loudly instead.
                    Com_Printf(0, "RetailWalkLiveLoadMenuList: menu[%u] item[%d] non-inline 0x%08x unsupported\n",
                               i, item, itemHeaderRef);
                    return false;
                }
                if (!LoadOneItem(session, reader, itemHeaderRef, &items[item],
                                 directoryOffset, directoryBytes, stubNames, stubCount,
                                 worldContext))
                {
                    Com_Printf(0, "RetailWalkLiveLoadMenuList: menu[%u] LoadOneItem(%d) failed\n", i, item);
                    return false;
                }
            }
        }

        if (!RetailZoneLoadSessionSyncFromReader(session, reader, 4))
            return false;

        char deferredName[64]{};
        Material *menuBg = nullptr;
        if (!ResolveBackgroundMaterial(session, reader, worldContext,
                                       wireMenu.backgroundWasInline,
                                       wireMenu.backgroundInlineMaterial, wireMenu.backgroundInlineTextures,
                                       wireMenu.backgroundRef, directoryOffset, directoryBytes,
                                       stubNames, stubCount, &menuBg, deferredName,
                                       sizeof(deferredName)))
        {
            Com_Printf(0, "RetailWalkLiveLoadMenuList: menu[%u] background ref=0x%08x unresolved (inline=%d)\n",
                       i, wireMenu.backgroundRef, wireMenu.backgroundWasInline ? 1 : 0);
            return false;
        }

        menuDef_t *nativeMenu = nullptr;
        const RetailUiDecodeResult menuDecRes =
            RetailDecodeMenu(session, &wireMenu, handlers, wireMenu.handlerCount, entries,
                             wireMenu.expressionEntryCount, wireMenu.itemCount ? items : nullptr,
                             static_cast<uint32_t>(wireMenu.itemCount), &nativeMenu, menuBg);
        if (menuDecRes != RETAIL_UI_DECODE_OK)
        {
            Com_Printf(0, "RetailWalkLiveLoadMenuList: menu[%u] RetailDecodeMenu failed res=%d\n", i, menuDecRes);
            return false;
        }
        if (!menuBg && deferredName[0] && nativeMenu &&
            !RetailDeferMaterialAlias(&nativeMenu->window.background, deferredName,
                                      session->zoneIndex))
            return false;
        if (!nativeMenu || !nativeMenu->window.name)
        {
            // Registering a synthetic unnamed menu would manufacture an
            // asset retail never declared; the original Load_MenuAsset keys
            // the registry by exactly this name.
            Com_Printf(0, "RetailWalkLiveLoadMenuList: menu[%u] has no name; refusing registration\n", i);
            return false;
        }

        // Register each declaring menu immediately, in list order, exactly
        // like Load_menuDef_ptr->Load_MenuAsset: later entries in this list
        // (and later zones) alias-form reference what this call registered.
        XAssetHeader registered{};
        registered.menu = nativeMenu;
        registered = RetailZoneLoadSessionRegister(session, ASSET_TYPE_MENU, registered);
        if (!registered.menu)
        {
            Com_Printf(0, "RetailWalkLiveLoadMenuList: menu[%u] registration failed\n", i);
            return false;
        }
        for (int32_t item = 0; item < registered.menu->itemCount; ++item)
        {
            if (!registered.menu->items || !registered.menu->items[item])
            {
                Com_Printf(0, "RetailWalkLiveLoadMenuList: menu[%u] item[%d] missing after registration\n",
                           i, item);
                return false;
            }
            registered.menu->items[item]->parent = registered.menu;
        }
        // Record the slot the original loader patches with this registered
        // menu pointer, so alias-form entries (this list or a later zone)
        // resolve exactly like DB_ConvertOffsetToAlias:
        //   - inline (-1): the linker array slot itself (Load_Stream reads
        //     the array into block-4 memory and Load_menuDef_ptr replaces
        //     the element with the allocated pointer);
        //   - insert (-2): the DB_InsertPointer slot reserved above.
        const uint32_t slotOffset =
            menuRefs[i] == kInsertRef ? insertSlot : wireList.menusOffset + i * 4u;
        if (slotOffset &&
            (!worldContext ||
             !RetailWorldRecordZoneSlot(worldContext, slotOffset, ASSET_TYPE_MENU, registered)))
        {
            Com_Printf(0, "RetailWalkLiveLoadMenuList: menu[%u] slot %u record failed\n",
                       i, slotOffset);
            return false;
        }
        menus[i] = registered.menu;
    }

    if (!RetailZoneLoadSessionSyncFromReader(session, reader, 4))
        return false;
    MenuList *widenedList = nullptr;
    const RetailUiDecodeResult listDecRes =
        RetailDecodeMenuList(session, &wireList, menus, wireList.menuCount, &widenedList);
    if (listDecRes != RETAIL_UI_DECODE_OK)
    {
        Com_Printf(0, "RetailWalkLiveLoadMenuList: RetailDecodeMenuList failed res=%d\n", listDecRes);
        return false;
    }

    XAssetHeader registeredList{};
    registeredList.menuList = widenedList;
    registeredList = RetailZoneLoadSessionRegister(session, ASSET_TYPE_MENULIST, registeredList);
    if (!registeredList.menuList)
    {
        Com_Printf(0, "RetailWalkLiveLoadMenuList: list registration failed\n");
        return false;
    }
    *header = registeredList;
    Com_Printf(0, "RetailWalkLiveLoadMenuList: SUCCESS name='%s'\n",
               registeredList.menuList->name ? registeredList.menuList->name : "<null>");
    return true;
}
