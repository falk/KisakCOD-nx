#ifndef SWITCH_UI_MENUS_H
#define SWITCH_UI_MENUS_H

// Port hooks into the SP menu system: the options pages, retail-menu
// adaptations, software-keyboard text entry and saved-game list details.

struct UiContext;
struct itemDef_s;
struct SavegameInfo;

// Called after a menu list is added to `dc`: installs the port-authored pages
// and adapts the retail menus. Safe to call repeatedly.
void Switch_UI_OnMenusAdded(UiContext *dc);

// Description panel text for the selected saved game (map, date, time).
void Switch_UI_FormatSaveInfo(const SavegameInfo *info, char *out, int outSize);

// A / Enter on a text field: shows the software keyboard and stores the
// result. False leaves the engine's own edit mode in charge.
bool Switch_UI_EditTextField(UiContext *dc, itemDef_s *item);

// A text field is being focused for editing (script or click): queue the
// software keyboard for the next UI frame instead, as the field cannot be
// typed into. False when the field is not a dvar-backed one.
bool Switch_UI_QueueTextEdit(itemDef_s *item);

// Runs the queued keyboard request; called once per UI frame.
void Switch_UI_Frame();

// Reads the save header behind `saveName` (no extension) into the list row.
void Switch_UI_FillSaveInfo(SavegameInfo *info, const char *saveName);

#endif
