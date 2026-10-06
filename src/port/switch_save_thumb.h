#ifndef SWITCH_SAVE_THUMB_H
#define SWITCH_SAVE_THUMB_H

// Saved-game thumbnails: the presented frame boxed down to 256x128 and stored
// next to the save as <name>.svt (an 8-byte header, then 3 bytes per pixel in
// B, G, R order: 96 KiB). Power-of-two so the renderer can upload it as an
// ordinary 2D image. Pure (no engine or libnx calls), so the host test covers
// the exact bytes the capture writes and the menu reads.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum
{
    kSaveThumbWidth = 256,
    kSaveThumbHeight = 128,
    kSaveThumbHeaderBytes = 8,
    kSaveThumbFileBytes = kSaveThumbHeaderBytes + kSaveThumbWidth * kSaveThumbHeight * 3,
    kSaveThumbBgraBytes = kSaveThumbWidth * kSaveThumbHeight * 4
};

// Box-filters a width x height frame (4 bytes per pixel, B G R x, `pitch`
// bytes per row) into out[kSaveThumbBgraBytes] as B G R 255. False for an
// empty frame.
static inline bool SaveThumb_FromFrame(const uint8_t *src, uint32_t width, uint32_t height, uint32_t pitch, uint8_t *out)
{
    if (!src || !out || !width || !height || pitch < width * 4)
        return false;
    for (uint32_t y = 0; y < (uint32_t)kSaveThumbHeight; ++y)
    {
        const uint32_t y0 = (uint32_t)((uint64_t)y * height / kSaveThumbHeight);
        uint32_t y1 = (uint32_t)((uint64_t)(y + 1) * height / kSaveThumbHeight);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (uint32_t x = 0; x < (uint32_t)kSaveThumbWidth; ++x)
        {
            const uint32_t x0 = (uint32_t)((uint64_t)x * width / kSaveThumbWidth);
            uint32_t x1 = (uint32_t)((uint64_t)(x + 1) * width / kSaveThumbWidth);
            if (x1 <= x0)
                x1 = x0 + 1;
            uint32_t sum[3] = {0, 0, 0};
            for (uint32_t sy = y0; sy < y1; ++sy)
            {
                const uint8_t *row = src + (size_t)sy * pitch;
                for (uint32_t sx = x0; sx < x1; ++sx)
                    for (int c = 0; c < 3; ++c)
                        sum[c] += row[sx * 4 + c];
            }
            const uint32_t count = (y1 - y0) * (x1 - x0);
            uint8_t *dst = out + ((size_t)y * kSaveThumbWidth + x) * 4;
            for (int c = 0; c < 3; ++c)
                dst[c] = (uint8_t)((sum[c] + count / 2) / count);
            dst[3] = 255;
        }
    }
    return true;
}

// bgra[kSaveThumbBgraBytes] -> file[kSaveThumbFileBytes].
static inline void SaveThumb_Encode(const uint8_t *bgra, uint8_t *file)
{
    memcpy(file, "SVT1", 4);
    file[4] = (uint8_t)(kSaveThumbWidth & 255);
    file[5] = (uint8_t)(kSaveThumbWidth >> 8);
    file[6] = (uint8_t)(kSaveThumbHeight & 255);
    file[7] = (uint8_t)(kSaveThumbHeight >> 8);
    uint8_t *dst = file + kSaveThumbHeaderBytes;
    for (int i = 0; i < kSaveThumbWidth * kSaveThumbHeight; ++i, dst += 3)
        memcpy(dst, bgra + (size_t)i * 4, 3);
}

// file -> bgra[kSaveThumbBgraBytes] (alpha 255). Rejects any file that is not
// exactly the format above: wrong size, magic or dimensions.
static inline bool SaveThumb_Decode(const uint8_t *file, size_t size, uint8_t *bgra)
{
    if (!file || !bgra || size != (size_t)kSaveThumbFileBytes || memcmp(file, "SVT1", 4))
        return false;
    if ((file[4] | (file[5] << 8)) != kSaveThumbWidth || (file[6] | (file[7] << 8)) != kSaveThumbHeight)
        return false;
    const uint8_t *src = file + kSaveThumbHeaderBytes;
    for (int i = 0; i < kSaveThumbWidth * kSaveThumbHeight; ++i, src += 3)
    {
        memcpy(bgra + (size_t)i * 4, src, 3);
        bgra[(size_t)i * 4 + 3] = 255;
    }
    return true;
}

// "profiles/p/save/a.svg" -> "profiles/p/save/a.svt" (any other extension is
// replaced the same way; none is appended). False when `out` is too small.
static inline bool SaveThumb_PathForSave(const char *savePath, char *out, size_t outSize)
{
    if (!savePath || !out)
        return false;
    size_t length = strlen(savePath);
    const char *dot = strrchr(savePath, '.');
    const char *slash = strrchr(savePath, '/');
    const char *back = strrchr(savePath, '\\');
    if (dot && (dot < slash || dot < back))
        dot = 0;
    if (dot)
        length = (size_t)(dot - savePath);
    if (length + 5 > outSize)
        return false;
    memcpy(out, savePath, length);
    memcpy(out + length, ".svt", 5);
    return true;
}

// Whole-file write (temp name, then rename: a reader never sees half a file).
// Horizon's rename refuses an existing target (POSIX would replace it), so a
// failed rename removes the old thumbnail and renames once more.
static inline bool SaveThumb_WriteFile(const char *path, const uint8_t *bgra,
                                       int (*renameFn)(const char *, const char *) = rename)
{
    char temp[512];
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp))
        return false;
    static uint8_t file[kSaveThumbFileBytes];
    SaveThumb_Encode(bgra, file);
    FILE *f = fopen(temp, "wb");
    if (!f)
        return false;
    const bool ok = fwrite(file, 1, sizeof(file), f) == sizeof(file);
    if (fclose(f) != 0 || !ok)
    {
        remove(temp);
        return false;
    }
    if (renameFn(temp, path) != 0 && (remove(path), renameFn(temp, path) != 0))
    {
        remove(temp);
        return false;
    }
    return true;
}

#endif
