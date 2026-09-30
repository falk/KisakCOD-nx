#pragma once

#include "../universal/com_files.h"
#include "db_retail_zone.h"

struct RetailWorldLoadContext;

// Widens one real MenuList body (and every menu it lists) read live from
// `reader`, then registers each declaring menu through its original owner
// (Load_menuDef_ptr->Load_MenuAsset, immediate in list order) and the list
// through Load_MenuListAsset -- unlike TechniqueSet/Image/Material, this
// reuses db_retail_decode_ui.cpp's already-proven
// RetailDecodeMenu/RetailDecodeMenuList (verified by
// switch_retail_ui_test.cpp's synthetic session) rather than a fresh
// reader-only decoder, since that widening logic already exists and
// duplicating it would violate this project's "one production decoder per
// asset family" rule.
//
// Non-inline entry forms follow the original loader exactly: a -2 insert
// reserves its block-4 DB_InsertPointer slot and records the registered
// menu there; any other reference is a DB_ConvertOffsetToAlias back-reference
// resolved through the zone slot ledger. No stand-in menu is ever
// synthesized. An unsupported item slot fails the load loudly.
//
// The FS-level menu readers stream into their reader mirror. After each read,
// the driver imports only the new block-4 suffix into the zone mirror used by
// RetailDecodeMenu/RetailDecodeMenuList.
//
// Scope: item bodies are widened through RetailDecodeItemDef, which itself
// only accepts the plain edit-field shape (IsEditFieldType, no background/
// focus-sound/parent set) -- the one scenario switch_retail_ui_test.cpp's
// synthetic proof already covers.  This is genuinely new production
// wiring: switch_sp_bootstrap.cpp's own hardware-proven walk stopped right
// before item bodies (item=0 "main" only).  Anything RetailDecodeItemDef
// itself does not accept (a background/scrollable/multi-value item, or an
// aliased item slot) fails loudly rather than guessing.
bool RetailWalkLiveLoadMenuList(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                                uint32_t headerRef, XAssetHeader *header,
                                uint32_t directoryOffset = 0, uint32_t directoryBytes = 0,
                                const char *const *stubNames = nullptr, uint32_t stubCount = 0,
                                RetailWorldLoadContext *worldContext = nullptr);
