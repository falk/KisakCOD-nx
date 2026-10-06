#pragma once

// Save-file writes off the game thread (platform/switch/
// switch_async_file_writer.h wired to the engine's save paths).

struct SaveHeader;

// Queues the header, body and any script source / demo records for
// save/temp.svg -> players/<header filename>; the bytes are copied, so the
// caller's buffers are free on return.
void SwitchSave_Submit(const SaveHeader *header, const unsigned char *body);
// Blocks until every queued write has landed; every reader of a save file
// calls this first.
void SwitchSave_WaitForWrites(void);

// Saved-game thumbnails (switch_save_thumb.h): queue a capture of the next
// presented frame next to the save file `saveOsPath` (.svt beside the .svg),
// and build the browser's material for a listed save by name.
void SwitchSaveThumb_Request(const char *saveOsPath);
struct Material;
Material *SwitchSaveThumb_Material(const char *saveName);
