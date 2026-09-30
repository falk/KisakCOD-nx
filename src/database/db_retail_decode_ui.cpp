#include "db_retail_decode_ui.h"

#include <cstring>

namespace
{
uint32_t Le32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

float LeFloat(const uint8_t *p)
{
    uint32_t v = Le32(p);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

bool CopyWireString(RetailZoneLoadSession *session, uint32_t ref, const char **out)
{
    *out = nullptr;
    if (!ref)
        return true;
    RetailWireToken token{};
    if (!RetailWireTokenDecode(&session->wire, {ref}, 0, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET)
        return false;
    const XBlock &block = session->zoneMemory->blocks[token.block];
    const uint32_t offset = token.offset;
    if (offset >= block.size || !block.data)
        return false;
    const uint8_t *source = block.data + offset;
    const uint32_t remaining = block.size - offset;
    uint32_t length = 0;
    while (length < remaining && source[length])
        ++length;
    if (length == remaining)
        return false;
    // Re-validate the exact extent through the canonical token seam.  This
    // keeps the string copy atomic even when the source block is near its
    // end and ensures no caller can accidentally widen a truncated token.
    if (!RetailWireTokenDecode(&session->wire, {ref}, length + 1, 1u << 4, &token))
        return false;
    char *copy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, length + 1, 1));
    if (!copy)
        return false;
    std::memcpy(copy, source, length + 1);
    *out = copy;
    return true;
}

bool CopyWindow(RetailZoneLoadSession *session, const FsRetailFastfileMenu &wire,
                windowDef_t *out)
{
    std::memset(out, 0, sizeof(*out));
    if (!CopyWireString(session, wire.nameRef, &out->name) ||
        !CopyWireString(session, wire.groupRef, &out->group))
        return false;
    out->rect = {wire.rect[0], wire.rect[1], wire.rect[2], wire.rect[3],
                 wire.rectHorzAlign, wire.rectVertAlign};
    out->rectClient = {wire.rectClient[0], wire.rectClient[1], wire.rectClient[2],
                       wire.rectClient[3], wire.rectClientHorzAlign,
                       wire.rectClientVertAlign};
    out->style = wire.style;
    out->border = wire.border;
    out->ownerDraw = wire.ownerDraw;
    out->ownerDrawFlags = wire.ownerDrawFlags;
    out->borderSize = wire.borderSize;
    out->staticFlags = wire.staticFlags;
    out->dynamicFlags[0] = wire.dynamicFlags;
    out->nextTime = wire.nextTime;
    std::memcpy(out->foreColor, wire.foreColor, sizeof(out->foreColor));
    std::memcpy(out->backColor, wire.backColor, sizeof(out->backColor));
    std::memcpy(out->borderColor, wire.borderColor, sizeof(out->borderColor));
    std::memcpy(out->outlineColor, wire.outlineColor, sizeof(out->outlineColor));
    return true;
}

bool IsEditFieldType(int32_t type)
{
    switch (type)
    {
    case 4: case 9: case 0x10: case 0x12: case 0xB:
    case 0xE: case 0xA: case 0: case 0x11:
        return true;
    default:
        return false;
    }
}

RetailUiDecodeResult CopyStatement(RetailZoneLoadSession *session,
                                   const FsRetailFastfileStatement &wire,
                                   const FsRetailFastfileExpressionEntry *entries,
                                   uint32_t entryCount, uint32_t *cursor,
                                   statement_s *out)
{
    out->numEntries = static_cast<int>(wire.numEntries);
    out->entries = nullptr;
    if (!wire.numEntries)
        return RETAIL_UI_DECODE_OK;
    if (!entries || *cursor > entryCount || wire.numEntries > entryCount - *cursor)
        return RETAIL_UI_DECODE_BAD_REFERENCE;
    auto **native = static_cast<expressionEntry **>(RetailZoneLoadSessionAlloc(
        session, sizeof(expressionEntry *) * wire.numEntries, alignof(expressionEntry *)));
    if (!native)
        return RETAIL_UI_DECODE_OUT_OF_ARENA;
    for (uint32_t i = 0; i < wire.numEntries; ++i)
    {
        const auto &source = entries[*cursor + i];
        auto *entry = static_cast<expressionEntry *>(RetailZoneLoadSessionAlloc(
            session, sizeof(expressionEntry), alignof(expressionEntry)));
        if (!entry)
            return RETAIL_UI_DECODE_OUT_OF_ARENA;
        entry->type = source.type;
        if (source.type == 0)
        {
            // Operator slots index the 81-entry validOperations table; an
            // out-of-range code would ride into the evaluator unchecked.
            if (source.dataType >= static_cast<uint32_t>(NUM_OPERATORS))
                return RETAIL_UI_DECODE_UNSUPPORTED_FORM;
            entry->data.op = static_cast<operationEnum>(source.dataType);
        }
        else
        {
            entry->data.operand.dataType = static_cast<expDataType>(source.dataType);
            switch (source.dataType)
            {
            case VAL_INT:
            case VAL_FLOAT:
                entry->data.operand.internals.intVal = static_cast<int>(source.operandRef);
                break;
            case VAL_STRING:
                if (!CopyWireString(session, source.operandRef,
                                    &entry->data.operand.internals.string))
                    return RETAIL_UI_DECODE_BAD_REFERENCE;
                break;
            default:
                return RETAIL_UI_DECODE_UNSUPPORTED_FORM;
            }
        }
        native[i] = entry;
    }
    *cursor += wire.numEntries;
    out->entries = native;
    return RETAIL_UI_DECODE_OK;
}
} // namespace

RetailUiDecodeResult RetailDecodeItemDef(
    RetailZoneLoadSession *session, const FsRetailFastfileItemDef *wire,
    const FsRetailFastfileItemKeyHandler *handlers, uint32_t handlerCount,
    const FsRetailFastfileExpressionEntry *entries, uint32_t entryCount,
    itemDef_s **out, Material *background)
{
    if (!session || !session->active || !wire || !out)
        return RETAIL_UI_DECODE_BAD_ARGUMENT;
    auto *item = static_cast<itemDef_s *>(RetailZoneLoadSessionAlloc(
        session, sizeof(itemDef_s), alignof(itemDef_s)));
    if (!item)
        return RETAIL_UI_DECODE_OUT_OF_ARENA;
    std::memset(item, 0, sizeof(*item));

    if (wire->focusSoundRef)
        return RETAIL_UI_DECODE_UNSUPPORTED_FORM;

    if (wire->typeDataRef)
    {
        if (IsEditFieldType(wire->type))
        {
            const uint64_t typeDataEnd = static_cast<uint64_t>(wire->typeDataOffset) + sizeof(editFieldDef_s);
            if (typeDataEnd > session->zoneMemory->blocks[4].size)
                return RETAIL_UI_DECODE_BAD_REFERENCE;

            const uint8_t *data = session->zoneMemory->blocks[4].data + wire->typeDataOffset;
            auto *edit = static_cast<editFieldDef_s *>(RetailZoneLoadSessionAlloc(
                session, sizeof(editFieldDef_s), alignof(editFieldDef_s)));
            if (!edit)
                return RETAIL_UI_DECODE_OUT_OF_ARENA;
            std::memcpy(edit, data, sizeof(*edit));
            item->typeData.editField = edit;
        }
        else if (wire->type == 6) // listBox
        {
            constexpr uint64_t kWireListBoxDefSize = 340;
            const uint64_t typeDataEnd = static_cast<uint64_t>(wire->typeDataOffset) + kWireListBoxDefSize;
            if (typeDataEnd > session->zoneMemory->blocks[4].size)
                return RETAIL_UI_DECODE_BAD_REFERENCE;

            const uint8_t *data = session->zoneMemory->blocks[4].data + wire->typeDataOffset;
            auto *list = static_cast<listBoxDef_s *>(RetailZoneLoadSessionAlloc(
                session, sizeof(listBoxDef_s), alignof(listBoxDef_s)));
            if (!list)
                return RETAIL_UI_DECODE_OUT_OF_ARENA;
            std::memset(list, 0, sizeof(*list));
            list->mousePos = static_cast<int>(Le32(data + 0));
            list->startPos[0] = static_cast<int>(Le32(data + 4));
            list->endPos[0] = static_cast<int>(Le32(data + 8));
            list->drawPadding = static_cast<int>(Le32(data + 12));
            list->elementWidth = LeFloat(data + 16);
            list->elementHeight = LeFloat(data + 20);
            list->elementStyle = static_cast<int>(Le32(data + 24));
            list->numColumns = static_cast<int>(Le32(data + 28));
            std::memcpy(list->columnInfo, data + 32, sizeof(list->columnInfo));
            const uint32_t doubleClickRef = Le32(data + 288);
            if (!CopyWireString(session, doubleClickRef, &list->doubleClick))
                return RETAIL_UI_DECODE_BAD_REFERENCE;
            list->notselectable = static_cast<int>(Le32(data + 292));
            list->noScrollBars = static_cast<int>(Le32(data + 296));
            list->usePaging = static_cast<int>(Le32(data + 300));
            for (int i = 0; i < 4; ++i)
                list->selectBorder[i] = LeFloat(data + 304 + i * 4);
            for (int i = 0; i < 4; ++i)
                list->disableColor[i] = LeFloat(data + 320 + i * 4);
            list->selectIcon = nullptr;
            item->typeData.listBox = list;
        }
        else if (wire->type == 0xC) // multiDef
        {
            constexpr uint64_t kWireMultiDefSize = 392;
            const uint64_t typeDataEnd = static_cast<uint64_t>(wire->typeDataOffset) + kWireMultiDefSize;
            if (typeDataEnd > session->zoneMemory->blocks[4].size)
                return RETAIL_UI_DECODE_BAD_REFERENCE;

            const uint8_t *data = session->zoneMemory->blocks[4].data + wire->typeDataOffset;
            auto *multi = static_cast<multiDef_s *>(RetailZoneLoadSessionAlloc(
                session, sizeof(multiDef_s), alignof(multiDef_s)));
            if (!multi)
                return RETAIL_UI_DECODE_OUT_OF_ARENA;
            std::memset(multi, 0, sizeof(*multi));
            for (uint32_t i = 0; i < 32; ++i)
            {
                const uint32_t ref = Le32(data + i * 4);
                if (!CopyWireString(session, ref, &multi->dvarList[i]))
                    return RETAIL_UI_DECODE_BAD_REFERENCE;
            }
            for (uint32_t i = 0; i < 32; ++i)
            {
                const uint32_t ref = Le32(data + 128 + i * 4);
                if (!CopyWireString(session, ref, &multi->dvarStr[i]))
                    return RETAIL_UI_DECODE_BAD_REFERENCE;
            }
            for (uint32_t i = 0; i < 32; ++i)
                multi->dvarValue[i] = LeFloat(data + 256 + i * 4);
            multi->count = static_cast<int>(Le32(data + 384));
            multi->strDef = static_cast<int>(Le32(data + 388));
            item->typeData.multi = multi;
        }
        else if (wire->type == 0xD) // enumDvarName
        {
            if (!CopyWireString(session, wire->typeDataRef, &item->typeData.enumDvarName))
                return RETAIL_UI_DECODE_BAD_REFERENCE;
        }
        else
        {
            item->typeData.data = nullptr;
        }
    }

    if (!CopyWireString(session, wire->nameRef, &item->window.name) ||
        !CopyWireString(session, wire->groupRef, &item->window.group) ||
        !CopyWireString(session, wire->textRef, &item->text) ||
        !CopyWireString(session, wire->mouseEnterTextRef, &item->mouseEnterText) ||
        !CopyWireString(session, wire->mouseExitTextRef, &item->mouseExitText) ||
        !CopyWireString(session, wire->mouseEnterRef, &item->mouseEnter) ||
        !CopyWireString(session, wire->mouseExitRef, &item->mouseExit) ||
        !CopyWireString(session, wire->actionRef, &item->action) ||
        !CopyWireString(session, wire->onAcceptRef, &item->onAccept) ||
        !CopyWireString(session, wire->onFocusRef, &item->onFocus) ||
        !CopyWireString(session, wire->leaveFocusRef, &item->leaveFocus) ||
        !CopyWireString(session, wire->dvarRef, &item->dvar) ||
        !CopyWireString(session, wire->dvarTestRef, &item->dvarTest) ||
        !CopyWireString(session, wire->enableDvarRef, &item->enableDvar))
        return RETAIL_UI_DECODE_BAD_REFERENCE;

    item->window.rect.x = wire->rect[0];
    item->window.rect.y = wire->rect[1];
    item->window.rect.w = wire->rect[2];
    item->window.rect.h = wire->rect[3];
    item->window.rect.horzAlign = wire->rectHorzAlign;
    item->window.rect.vertAlign = wire->rectVertAlign;
    item->window.rectClient.x = wire->rectClient[0];
    item->window.rectClient.y = wire->rectClient[1];
    item->window.rectClient.w = wire->rectClient[2];
    item->window.rectClient.h = wire->rectClient[3];
    item->window.rectClient.horzAlign = wire->rectClientHorzAlign;
    item->window.rectClient.vertAlign = wire->rectClientVertAlign;
    item->window.style = wire->style;
    item->window.border = wire->border;
    item->window.ownerDraw = wire->ownerDraw;
    item->window.ownerDrawFlags = wire->ownerDrawFlags;
    item->window.borderSize = wire->borderSize;
    item->window.staticFlags = wire->staticFlags;
    item->window.dynamicFlags[0] = wire->dynamicFlags;
    item->window.nextTime = wire->nextTime;
    std::memcpy(item->window.foreColor, wire->foreColor, sizeof(wire->foreColor));
    std::memcpy(item->window.backColor, wire->backColor, sizeof(wire->backColor));
    std::memcpy(item->window.borderColor, wire->borderColor, sizeof(wire->borderColor));
    std::memcpy(item->window.outlineColor, wire->outlineColor, sizeof(wire->outlineColor));
    item->window.background = background;
    item->type = wire->type;
    item->dataType = wire->dataType;
    item->alignment = wire->alignment;
    item->fontEnum = wire->fontEnum;
    item->textAlignMode = wire->textAlignMode;
    item->textalignx = wire->textalignx;
    item->textaligny = wire->textaligny;
    item->textscale = wire->textscale;
    item->textStyle = wire->textStyle;
    item->gameMsgWindowIndex = wire->gameMsgWindowIndex;
    item->gameMsgWindowMode = wire->gameMsgWindowMode;
    item->itemFlags = wire->itemFlags;
    item->dvarFlags = wire->dvarFlags;
    item->special = wire->special;
    item->cursorPos[0] = static_cast<int>(wire->cursorPos);
    item->imageTrack = wire->imageTrack;

    if (handlerCount)
    {
        auto *head = static_cast<ItemKeyHandler *>(RetailZoneLoadSessionAlloc(
            session, sizeof(ItemKeyHandler) * handlerCount, alignof(ItemKeyHandler)));
        if (!head || !handlers)
            return head ? RETAIL_UI_DECODE_BAD_REFERENCE : RETAIL_UI_DECODE_OUT_OF_ARENA;
        for (uint32_t i = 0; i < handlerCount; ++i)
        {
            head[i].key = handlers[i].key;
            if (!CopyWireString(session, handlers[i].actionRef, &head[i].action))
                return RETAIL_UI_DECODE_BAD_REFERENCE;
            head[i].next = i + 1 < handlerCount ? &head[i + 1] : nullptr;
        }
        item->onKey = head;
    }
    uint32_t cursor = 0;
    RetailUiDecodeResult result;
    statement_s *statements[] = {&item->visibleExp, &item->textExp, &item->materialExp,
        &item->rectXExp, &item->rectYExp, &item->rectWExp, &item->rectHExp,
        &item->forecolorAExp};
    for (uint32_t i = 0; i < 8; ++i)
    {
        result = CopyStatement(session, wire->statements[i], entries, entryCount,
                               &cursor, statements[i]);
        if (result != RETAIL_UI_DECODE_OK)
            return result;
    }
    if (cursor != entryCount)
        return RETAIL_UI_DECODE_BAD_REFERENCE;
    *out = item;
    return RETAIL_UI_DECODE_OK;
}

RetailUiDecodeResult RetailDecodeMenu(
    RetailZoneLoadSession *session, const FsRetailFastfileMenu *wire,
    const FsRetailFastfileItemKeyHandler *handlers, uint32_t handlerCount,
    const FsRetailFastfileExpressionEntry *entries, uint32_t entryCount,
    itemDef_s *const *items, uint32_t itemCount, menuDef_t **out,
    Material *background)
{
    if (!session || !session->active || !wire || !out)
        return RETAIL_UI_DECODE_BAD_ARGUMENT;
    *out = nullptr;
    if (wire->itemCount < 0 ||
        static_cast<uint32_t>(wire->itemCount) != itemCount ||
        (itemCount && !items))
        return RETAIL_UI_DECODE_BAD_REFERENCE;
    auto *menu = static_cast<menuDef_t *>(RetailZoneLoadSessionAlloc(
        session, sizeof(menuDef_t), alignof(menuDef_t)));
    if (!menu)
        return RETAIL_UI_DECODE_OUT_OF_ARENA;
    std::memset(menu, 0, sizeof(*menu));
    if (!CopyWindow(session, *wire, &menu->window) ||
        !CopyWireString(session, wire->fontRef, &menu->font) ||
        !CopyWireString(session, wire->onOpenRef, &menu->onOpen) ||
        !CopyWireString(session, wire->onCloseRef, &menu->onClose) ||
        !CopyWireString(session, wire->onESCRef, &menu->onESC) ||
        !CopyWireString(session, wire->allowedBindingRef, &menu->allowedBinding) ||
        !CopyWireString(session, wire->soundNameRef, &menu->soundName))
        return RETAIL_UI_DECODE_BAD_REFERENCE;
    menu->window.background = background;
    menu->fullScreen = wire->fullScreen;
    menu->itemCount = wire->itemCount;
    menu->fontIndex = wire->fontIndex;
    menu->cursorItem[0] = wire->cursorItem;
    menu->fadeCycle = wire->fadeCycle;
    menu->fadeClamp = wire->fadeClamp;
    menu->fadeAmount = wire->fadeAmount;
    menu->fadeInAmount = wire->fadeInAmount;
    menu->blurRadius = wire->blurRadius;
    menu->imageTrack = wire->imageTrack;
    std::memcpy(menu->focusColor, wire->focusColor, sizeof(menu->focusColor));
    std::memcpy(menu->disableColor, wire->disableColor, sizeof(menu->disableColor));

    if (handlerCount) {
        auto *head = static_cast<ItemKeyHandler *>(RetailZoneLoadSessionAlloc(
            session, sizeof(ItemKeyHandler) * handlerCount, alignof(ItemKeyHandler)));
        if (!head || !handlers)
            return head ? RETAIL_UI_DECODE_BAD_REFERENCE : RETAIL_UI_DECODE_OUT_OF_ARENA;
        for (uint32_t i = 0; i < handlerCount; ++i) {
            head[i].key = handlers[i].key;
            if (!CopyWireString(session, handlers[i].actionRef, &head[i].action))
                return RETAIL_UI_DECODE_BAD_REFERENCE;
            head[i].next = i + 1 < handlerCount ? &head[i + 1] : nullptr;
        }
        menu->onKey = head;
    }
    uint32_t cursor = 0;
    auto copyStatement = [&](const FsRetailFastfileStatement &statement,
                             statement_s *native) {
        return CopyStatement(session, statement, entries, entryCount, &cursor, native);
    };
    RetailUiDecodeResult result = copyStatement(wire->visibleExp, &menu->visibleExp);
    if (result != RETAIL_UI_DECODE_OK) return result;
    result = copyStatement(wire->rectXExp, &menu->rectXExp);
    if (result != RETAIL_UI_DECODE_OK) return result;
    result = copyStatement(wire->rectYExp, &menu->rectYExp);
    if (result != RETAIL_UI_DECODE_OK) return result;
    if (cursor != entryCount)
        return RETAIL_UI_DECODE_BAD_REFERENCE;

    if (itemCount) {
        menu->items = static_cast<itemDef_s **>(RetailZoneLoadSessionAlloc(
            session, sizeof(itemDef_s *) * itemCount, alignof(itemDef_s *)));
        if (!menu->items)
            return RETAIL_UI_DECODE_OUT_OF_ARENA;
        for (uint32_t i = 0; i < itemCount; ++i) {
            if (!items[i]) return RETAIL_UI_DECODE_BAD_REFERENCE;
            menu->items[i] = items[i];
            items[i]->parent = menu;
        }
    }
    *out = menu;
    return RETAIL_UI_DECODE_OK;
}

RetailUiDecodeResult RetailDecodeMenuList(
    RetailZoneLoadSession *session, const FsRetailFastfileMenuList *wire,
    menuDef_t *const *menus, uint32_t menuCount, MenuList **out)
{
    if (!session || !session->active || !wire || !out)
        return RETAIL_UI_DECODE_BAD_ARGUMENT;
    *out = nullptr;
    if (wire->menuCount != menuCount || (menuCount && !menus))
        return RETAIL_UI_DECODE_BAD_REFERENCE;
    auto *list = static_cast<MenuList *>(RetailZoneLoadSessionAlloc(
        session, sizeof(MenuList), alignof(MenuList)));
    if (!list)
        return RETAIL_UI_DECODE_OUT_OF_ARENA;
    std::memset(list, 0, sizeof(*list));
    if (!CopyWireString(session, wire->nameRef, &list->name))
        return RETAIL_UI_DECODE_BAD_REFERENCE;
    list->menuCount = static_cast<int>(menuCount);
    if (menuCount) {
        list->menus = static_cast<menuDef_t **>(RetailZoneLoadSessionAlloc(
            session, sizeof(menuDef_t *) * menuCount, alignof(menuDef_t *)));
        if (!list->menus) return RETAIL_UI_DECODE_OUT_OF_ARENA;
        for (uint32_t i = 0; i < menuCount; ++i) {
            if (!menus[i]) return RETAIL_UI_DECODE_BAD_REFERENCE;
            list->menus[i] = menus[i];
        }
    }
    *out = list;
    return RETAIL_UI_DECODE_OK;
}

const char *RetailUiDecodeResultName(RetailUiDecodeResult result)
{
    static const char *const names[] = {"ok", "bad_argument", "bad_reference",
        "unsupported_form", "out_of_arena"};
    return result >= RETAIL_UI_DECODE_OK && result <= RETAIL_UI_DECODE_OUT_OF_ARENA ?
        names[result] : "invalid_result";
}
