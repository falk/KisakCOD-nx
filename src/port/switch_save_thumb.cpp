// Saved-game thumbnails on Switch: capture at save time and the material the
// save browser draws. Pixel format and file layout: switch_save_thumb.h.
//
// Capture: SwitchSaveThumb_Request asks the renderer for the next presented
// frame (RB_SwapBuffers does the readback and writes <save>.svt).
// Display: Material_RegisterRawImage -> SwitchSaveThumb_Material loads the
// file into one reusable image ("$savegameshot"), wrapped by a copy of the
// "unknownsave" material so it draws like the placeholder it replaces.
#ifdef __SWITCH__
#include <universal/q_shared.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/rb_backend.h>
#include <qcommon/qcommon.h>
#include <qcommon/com_playerprofile.h>
#include <universal/com_files.h>

#include "switch_save_thumb.h"
#include "switch_save_writer.h"

#include <cstring>

namespace
{
Material *s_material;
GfxImage *s_image;
char s_loadedKey[128];
}

void SwitchSaveThumb_Request(const char *saveOsPath)
{
    char thumbPath[512];
    if (!SaveThumb_PathForSave(saveOsPath, thumbPath, sizeof(thumbPath)))
    {
        Com_PrintError(CON_CHANNEL_FILES, "SAVE_THUMB path too long: %s\n", saveOsPath);
        return;
    }
    // The save writer creates the folders on its own thread, after this runs.
    FS_CreatePath(thumbPath);
    RB_RequestSaveThumbnail(thumbPath);
}

// `saveName` is the save file's name without extension, as the browser lists
// it. Returns null when the thumbnail is missing or unreadable (the caller
// falls back to the placeholder).
Material *SwitchSaveThumb_Material(const char *saveName)
{
    char qpath[128], thumbQpath[160];
    if (!saveName || !*saveName)
        return nullptr;
    if (Com_BuildPlayerProfilePath(qpath, sizeof(qpath), "save/%s.svg", saveName) < 0 ||
        !SaveThumb_PathForSave(qpath, thumbQpath, sizeof(thumbQpath)))
        return nullptr;

    SwitchSave_WaitForWrites();
    void *file = nullptr;
    const int size = FS_ReadFile(thumbQpath, &file);
    static uint8_t bgra[kSaveThumbBgraBytes];
    const bool decoded = size > 0 && file && SaveThumb_Decode(static_cast<const uint8_t *>(file), (size_t)size, bgra);
    if (file)
        FS_FreeFile(static_cast<char *>(file));
    if (!decoded)
    {
        Com_PrintWarning(CON_CHANNEL_UI, "SAVE_THUMB unreadable: %s\n", thumbQpath);
        return nullptr;
    }

    // Both objects live on the hunk; the pair is reusable only while the
    // material is still the one registered under its name.
    uint16_t hashIndex[3];
    bool exists = false;
    Material_GetHashIndex("$savegameshot", hashIndex, &exists);
    if (!exists || !s_material || rg.materialHashTable[hashIndex[0]] != s_material || !s_image)
    {
        Material *placeholder = Material_RegisterHandle("unknownsave", IMAGE_TRACK_UI);
        if (!placeholder || !placeholder->textureTable || placeholder->textureCount < 1)
            return nullptr;
        char imageName[] = "$savegameshot";
        s_image = Image_Alloc(imageName, IMG_CATEGORY_RAW, TS_2D, IMAGE_TRACK_UI);
        s_material = Material_Duplicate(placeholder, imageName);
        if (!s_image || !s_material || !s_material->textureTable)
            return nullptr;
        s_material->textureTable[0].u.image = s_image;
        s_loadedKey[0] = 0;
    }
    else
    {
        Image_Release(s_image); // the texture is rebuilt for the new pixels below
    }
    Image_Generate2D(s_image, bgra, kSaveThumbWidth, kSaveThumbHeight, D3DFMT_A8R8G8B8);
    I_strncpyz(s_loadedKey, saveName, sizeof(s_loadedKey));
    Com_Printf(CON_CHANNEL_UI, "SAVE_THUMB loaded %s\n", thumbQpath);
    return s_material;
}
#endif
