// Native widening proof.  It uses the production ItemDef decoder and the
// existing SP UI types; it does not build a parallel menu representation.
#include <cstdio>
#include <cstring>

#include <database/db_retail_decode_ui.h>

void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}

// The production wrapper is intentionally tiny; this narrow verifier supplies
// it without linking the database registry owner.
void *RetailZoneLoadSessionAlloc(RetailZoneLoadSession *session, std::size_t bytes,
                                 std::size_t alignment)
{
    return session && session->active ? RetailNativeArenaAlloc(&session->arena, bytes, alignment)
                                      : nullptr;
}

namespace
{
bool Check(bool value, const char *stage)
{
    if (!value)
        std::fprintf(stderr, "FAIL:RETAIL_UI_DECODE_PROOF stage=%s\n", stage);
    return value;
}

uint32_t AddString(uint8_t *block, uint32_t *cursor, const char *value)
{
    const uint32_t offset = *cursor;
    const size_t bytes = std::strlen(value) + 1;
    std::memcpy(block + offset, value, bytes);
    *cursor += static_cast<uint32_t>(bytes);
    return (4u << 28) | (offset + 1u);
}
}

int main()
{
    uint8_t block4[512]{};
    uint8_t nativeStorage[4096]{};
    XZoneMemory zone{};
    zone.blocks[4] = {block4, sizeof(block4)};
    RetailZoneLoadSession session{};
    session.active = true;
    session.zoneMemory = &zone;
    if (!Check(RetailWireBlocksInit(&session.wire, &zone), "wire") ||
        !Check(RetailNativeArenaInit(&session.arena, nativeStorage, sizeof(nativeStorage)), "arena"))
        return 1;

    uint32_t cursor = 0;
    FsRetailFastfileItemDef wire{};
    wire.type = 4;
    wire.typeDataRef = 0xffffffffu;
    wire.typeDataOffset = 128;
    wire.nameRef = AddString(block4, &cursor, "proof_item");
    wire.groupRef = AddString(block4, &cursor, "proof_group");
    wire.textRef = AddString(block4, &cursor, "proof text");
    wire.actionRef = AddString(block4, &cursor, "proof_action");
    wire.enableDvarRef = AddString(block4, &cursor, "proof_dvar");
    wire.rect[0] = 10.0f; wire.rect[1] = 20.0f; wire.rect[2] = 30.0f; wire.rect[3] = 40.0f;
    wire.style = 2; wire.typeDataRef = 0xffffffffu;
    editFieldDef_s edit{};
    edit.minVal = 1.0f; edit.maxVal = 9.0f; edit.maxChars = 24;
    std::memcpy(block4 + wire.typeDataOffset, &edit, sizeof(edit));

    FsRetailFastfileItemKeyHandler handlers[1]{};
    handlers[0].key = 13;
    handlers[0].actionRef = AddString(block4, &cursor, "proof_key");
    FsRetailFastfileExpressionEntry entries[1]{};
    wire.statements[0].numEntries = 1;
    wire.statements[0].entriesRef = 0xffffffffu;
    entries[0].type = 1;
    entries[0].dataType = VAL_STRING;
    entries[0].operandRef = AddString(block4, &cursor, "proof_expression");

    itemDef_s *item = nullptr;
    if (!Check(RetailDecodeItemDef(&session, &wire, handlers, 1, entries, 1, &item) ==
                   RETAIL_UI_DECODE_OK, "decode") ||
        !Check(item && !std::strcmp(item->window.name, "proof_item") &&
                   !std::strcmp(item->text, "proof text"), "strings") ||
        !Check(item->typeData.editField && item->typeData.editField->maxChars == 24,
                   "edit_field") ||
        !Check(item->onKey && item->onKey->key == 13 && item->onKey->next == nullptr,
                   "key_chain") ||
        !Check(item->visibleExp.numEntries == 1 && item->visibleExp.entries &&
                   !std::strcmp(item->visibleExp.entries[0]->data.operand.internals.string,
                                "proof_expression"), "expression") ||
        !Check(reinterpret_cast<uint8_t *>(item) >= nativeStorage &&
                   reinterpret_cast<uint8_t *>(item) < nativeStorage + sizeof(nativeStorage),
                   "zone_ownership"))
        return 1;

    FsRetailFastfileMenu menuWire{};
    menuWire.nameRef = AddString(block4, &cursor, "proof_menu");
    menuWire.groupRef = AddString(block4, &cursor, "proof_group");
    menuWire.fontRef = AddString(block4, &cursor, "fonts/proof");
    menuWire.onOpenRef = AddString(block4, &cursor, "open");
    menuWire.soundNameRef = AddString(block4, &cursor, "ui_menu_accept");
    menuWire.itemCount = 1;
    menuWire.fullScreen = 1;
    menuWire.rect[2] = 640.0f;
    menuWire.rect[3] = 480.0f;
    menuDef_t *nativeMenu = nullptr;
    if (!Check(RetailDecodeMenu(&session, &menuWire, nullptr, 0, nullptr, 0,
                                &item, 1, &nativeMenu) == RETAIL_UI_DECODE_OK,
                "menu_decode") ||
        !Check(nativeMenu && nativeMenu->itemCount == 1 && nativeMenu->items[0] == item,
               "menu_items") ||
        !Check(item->parent == nativeMenu && nativeMenu->fullScreen == 1 &&
                   !std::strcmp(nativeMenu->font, "fonts/proof"), "parent_repair"))
        return 1;
    FsRetailFastfileMenuList listWire{};
    listWire.nameRef = AddString(block4, &cursor, "ui/menus.txt");
    listWire.menuCount = 1;
    MenuList *nativeList = nullptr;
    menuDef_t *menus[1] = {nativeMenu};
    if (!Check(RetailDecodeMenuList(&session, &listWire, menus, 1, &nativeList) ==
                   RETAIL_UI_DECODE_OK, "menulist_decode") ||
        !Check(nativeList && nativeList->menuCount == 1 && nativeList->menus[0] == nativeMenu &&
                   !std::strcmp(nativeList->name, "ui/menus.txt"), "menulist_owner"))
        return 1;
    wire.focusSoundRef = 1;
    if (!Check(RetailDecodeItemDef(&session, &wire, handlers, 1, entries, 1, &item) ==
                   RETAIL_UI_DECODE_UNSUPPORTED_FORM, "focus_sound_rejected"))
        return 1;
    // Inline integer operands ride the wire as the ref word itself; an
    // operator slot at or past NUM_OPERATORS fails loudly instead of
    // reaching the evaluator unchecked.
    wire.focusSoundRef = 0;
    FsRetailFastfileExpressionEntry opEntries[2]{};
    opEntries[0].type = 1;
    opEntries[0].dataType = VAL_INT;
    opEntries[0].operandRef = 0x12345678u;
    opEntries[1].type = 0;
    opEntries[1].dataType = static_cast<uint32_t>(NUM_OPERATORS);
    wire.statements[0].numEntries = 1;
    itemDef_s *intItem = nullptr;
    if (!Check(RetailDecodeItemDef(&session, &wire, handlers, 1, opEntries, 1, &intItem) ==
                   RETAIL_UI_DECODE_OK &&
               intItem && intItem->visibleExp.numEntries == 1 &&
               intItem->visibleExp.entries[0]->data.operand.internals.intVal ==
                   static_cast<int>(0x12345678u),
               "int_value"))
        return 1;
    wire.statements[0].numEntries = 2;
    if (!Check(RetailDecodeItemDef(&session, &wire, handlers, 1, opEntries, 2, &intItem) ==
                   RETAIL_UI_DECODE_UNSUPPORTED_FORM, "badop_rejected"))
        return 1;
    std::puts("PASS:RETAIL_UI_DECODE_PROOF item=1 key=1 expression=1 edit=1 menu=1 menulist=1 parent=1 intop=1 badop=1");
    return 0;
}
