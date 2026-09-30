#pragma once

#include "db_retail_zone.h"
#include "../universal/com_files.h"
#include "../ui/ui_shared.h"

enum RetailUiDecodeResult
{
    RETAIL_UI_DECODE_OK,
    RETAIL_UI_DECODE_BAD_ARGUMENT,
    RETAIL_UI_DECODE_BAD_REFERENCE,
    RETAIL_UI_DECODE_UNSUPPORTED_FORM,
    RETAIL_UI_DECODE_OUT_OF_ARENA,
};

// Widen one already-walked retail ItemDef into the original SP UI object.
// This does not register an asset or create a parallel UI representation.
RetailUiDecodeResult RetailDecodeItemDef(RetailZoneLoadSession *session,
                                          const FsRetailFastfileItemDef *wire,
                                          const FsRetailFastfileItemKeyHandler *handlers,
                                          uint32_t handlerCount,
                                          const FsRetailFastfileExpressionEntry *entries,
                                          uint32_t entryCount,
                                          itemDef_s **out,
                                          Material *background = nullptr);

// Widen a walked menu and repair the parent links of its already widened
// children.  Asset-bearing fields are intentionally not guessed here: the
// caller must pass native children, while unresolved font/background/sound
// references are rejected.
RetailUiDecodeResult RetailDecodeMenu(
    RetailZoneLoadSession *session, const FsRetailFastfileMenu *wire,
    const FsRetailFastfileItemKeyHandler *handlers, uint32_t handlerCount,
    const FsRetailFastfileExpressionEntry *entries, uint32_t entryCount,
    itemDef_s *const *items, uint32_t itemCount, menuDef_t **out,
    Material *background = nullptr);

RetailUiDecodeResult RetailDecodeMenuList(
    RetailZoneLoadSession *session, const FsRetailFastfileMenuList *wire,
    menuDef_t *const *menus, uint32_t menuCount, MenuList **out);

const char *RetailUiDecodeResultName(RetailUiDecodeResult result);
