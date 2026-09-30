#include "db_retail_decode_image.h"

#include "db_retail_walk.h"
#include "db_retail_wire.h"

#include "../gfx_d3d/r_image.h"

#include <cstring>


namespace
{
bool DecodeFastfileToken(FsRetailFastfileReader *reader, uint32_t encoded, uint32_t span,
                         uint32_t allowedBlockMask, RetailWireToken *token)
{
    if (!reader || !token)
        return false;
    XBlock blocks[9]{};
    for (uint32_t block = 0; block < 9; ++block)
        blocks[block] = {const_cast<uint8_t *>(FS_RetailFastfileBlockData(reader, block)),
                         FS_RetailFastfileBlockSize(reader, block)};
    return RetailWireTokenDecodeBlocks(blocks, {encoded}, span, allowedBlockMask, token);
}

bool ReadBlockString(FsRetailFastfileReader *reader, uint32_t nameRef, char *buffer,
                     uint32_t bufferSize)
{
    RetailWireToken token{};
    if (!nameRef || bufferSize == 0 ||
        !DecodeFastfileToken(reader, nameRef, 0, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET)
        return false;
    // Exact-span read: request only the bytes actually mirrored
    // (offset..cursor), never a full buffer -- a name near the end of a
    // block made the old full-buffer read fail even though the string was
    // fully present.
    if (token.offset >= FS_RetailFastfileBlockCursor(reader, token.block))
        return false;
    const uint32_t available =
        FS_RetailFastfileBlockCursor(reader, token.block) - token.offset;
    const uint32_t readBytes = available < bufferSize - 1 ? available : bufferSize - 1;
    if (FS_ReadRetailFastfileBlock(reader, token.block, token.offset,
                                   reinterpret_cast<uint8_t *>(buffer),
                                   readBytes) != FS_RETAIL_FF_WIRE_OK)
        return false;
    buffer[bufferSize - 1] = '\0';
    return true;
}

// True when images/<name>.iwi exists and its header declares exactly the
// dimensions this wire image's loadDef declares.  Used to accept or reject a
// fallback name (see the header comment on RetailWidenImageFromWire): the
// on-disk file is the same one Image_LoadFromFile would open, so a matching
// header means the guessed name really does name this image's pixels.
bool IwiHeaderMatches(const char *name, uint16_t width, uint16_t height)
{
    if (!name || !name[0] || !width || !height)
        return false;
    char filepath[80];
    if (snprintf(filepath, sizeof(filepath), "images/%s.iwi", name) < 0)
        return false;
    int fileHandle = 0;
    const int fileSize = static_cast<int>(FS_FOpenFileRead(filepath, &fileHandle));
    if (fileSize < static_cast<int>(sizeof(GfxImageFileHeader)) || !fileHandle)
    {
        if (fileHandle)
            FS_FCloseFile(fileHandle);
        return false;
    }
    GfxImageFileHeader fileHeader{};
    const bool read = FS_Read(reinterpret_cast<uint8_t *>(&fileHeader), sizeof(fileHeader),
                              fileHandle) == sizeof(fileHeader);
    FS_FCloseFile(fileHandle);
    if (!read || fileHeader.tag[0] != 'I' || fileHeader.tag[1] != 'W' || fileHeader.tag[2] != 'i')
        return false;
    return static_cast<uint16_t>(fileHeader.dimensions[0]) == width &&
           static_cast<uint16_t>(fileHeader.dimensions[1]) == height;
}
} // namespace

bool RetailWidenImageFromWire(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                              const FsRetailFastfileImage &wire, XAssetHeader *header,
                              const char *fallbackName, bool strict)
{
    if (!session || !session->active || !reader || !header)
        return false;
    *header = {};

    // Strict acceptance mode widens every map type exactly,
    // including cube/volume forms: widening (header + loadDef bytes into
    // zone memory) is exact registry data regardless of type, while
    // texture UPLOAD stays gated at the device seam (G3) until the
    // per-type upload semantics exist. Only the meaningless NONE/INVALID
    // forms fail here -- a silent 2D downgrade would corrupt sampling.
    if (strict && wire.mapType != MAPTYPE_2D && wire.mapType != MAPTYPE_3D &&
        wire.mapType != MAPTYPE_CUBE)
    {
        Com_Printf(0, "RetailWidenImageFromWire: strict mapType=%u not 2D/3D/cube\n",
                   wire.mapType);
        return false;
    }

    char name[64];
    if (!ReadBlockString(reader, wire.nameRef, name, sizeof(name)))
        name[0] = '\0';
    if (!name[0] && !strict && !wire.nameWasInline && fallbackName && fallbackName[0] &&
        IwiHeaderMatches(fallbackName, wire.loadDefDimensions[0], wire.loadDefDimensions[1]))
    {
        // The serialized name is a block-4 reference our block-4 mirror cannot
        // resolve; the owning material's name names the same pixels on disk.
        std::strncpy(name, fallbackName, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        static int s_recoverLog = 0;
        if (s_recoverLog < 40)
        {
            ++s_recoverLog;
            char rbuf[160];
            snprintf(rbuf, sizeof(rbuf),
                     "IMGNAME-RECOVER[%d]: nameRef=0x%08x -> '%s' (%ux%u)\n", s_recoverLog,
                     wire.nameRef, name, wire.loadDefDimensions[0], wire.loadDefDimensions[1]);
        }
    }

    GfxImage *image = static_cast<GfxImage *>(
        RetailZoneLoadSessionAlloc(session, sizeof(GfxImage), alignof(GfxImage)));
    const std::size_t nameBytes = std::strlen(name) + 1;
    char *nameCopy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, nameBytes, 1));
    if (!image || !nameCopy)
    {
        Com_Printf(0, "RetailWidenImageFromWire: zone alloc failed name='%s' image=%p nameCopy=%p\n",
                   name, (void *)image, (void *)nameCopy);
        return false;
    }
    std::memset(image, 0, sizeof(*image));
    std::memcpy(nameCopy, name, nameBytes);
    image->mapType = static_cast<MapType>(wire.mapType);
    image->picmip.platform[0] = wire.picmip[0];
    image->picmip.platform[1] = wire.picmip[1];
    image->noPicmip = wire.noPicmip;
    image->semantic = wire.semantic;
    image->track = wire.track;
    image->cardMemory.platform[0] = static_cast<int>(wire.cardMemory[0]);
    image->cardMemory.platform[1] = static_cast<int>(wire.cardMemory[1]);
    image->width = wire.width;
    image->height = wire.height;
    image->depth = wire.depth;
    image->category = wire.category;
    image->delayLoadPixels = wire.delayLoadPixels;
    image->name = nameCopy;

    if (!name[0])
    {
        static int s_emptyNameLog = 0;
        if (s_emptyNameLog < 40)
        {
            ++s_emptyNameLog;
            char ebuf[256];
            snprintf(ebuf, sizeof(ebuf),
                     "EMPTYNAME-IMG[%d]: mapType=%u wh=%ux%u cat=%u delay=%u haveLoadDef=%u "
                     "loadDefDims=%ux%ux%u fmt=%u resSize=%u textureRef=0x%08x nameRef=0x%08x "
                     "nameWasInline=%u fallback='%s'\n",
                     s_emptyNameLog, wire.mapType, wire.width, wire.height, wire.category,
                     wire.delayLoadPixels, wire.haveLoadDef, wire.loadDefDimensions[0],
                     wire.loadDefDimensions[1], wire.loadDefDimensions[2], wire.loadDefFormat,
                     wire.loadDefResourceSize, wire.textureRef, wire.nameRef, wire.nameWasInline,
                     fallbackName ? fallbackName : "");
        }
    }

    if (wire.haveLoadDef)
    {
        // GfxImageLoadDef::data is a trailing flexible array (see
        // OFFSET_TO_GfxImageLoadDef_DATA in r_gfx.h), not a pointer: the
        // allocation must be widened past the 4-byte placeholder to hold the
        // real payload before Load_Texture (r_image.cpp) can upload it.
        const std::size_t extraDataBytes =
            wire.loadDefResourceSize > 4 ? (wire.loadDefResourceSize - 4) : 0;
        const std::size_t loadDefBytes = sizeof(GfxImageLoadDef) + extraDataBytes;
        GfxImageLoadDef *loadDef = static_cast<GfxImageLoadDef *>(
            RetailZoneLoadSessionAlloc(session, loadDefBytes, alignof(GfxImageLoadDef)));
        if (!loadDef)
        {
            Com_Printf(0, "RetailWidenImageFromWire: loadDef alloc failed name='%s' bytes=%zu\n",
                       name, loadDefBytes);
            return false;
        }
        std::memset(loadDef, 0, loadDefBytes);
        loadDef->levelCount = wire.loadDefLevelCount;
        loadDef->flags = wire.loadDefFlags;
        for (uint32_t i = 0; i < 3; ++i)
            loadDef->dimensions[i] = static_cast<int16_t>(wire.loadDefDimensions[i]);
        loadDef->format = static_cast<_D3DFORMAT>(wire.loadDefFormat);
        loadDef->resourceSize = static_cast<int>(wire.loadDefResourceSize);
        if (wire.loadDefResourceSize)
        {
            // FS_ReadRetailFastfileImage already streamed these bytes off
            // the wire into block 0 at pixelDataOffset; copy them into
            // loadDef's trailing data so the renderer uploads real pixels
            // instead of R_MaterializeImage creating an empty texture (the
            // previously open G3 gap for inline, no-on-disk-file images).
            const uint8_t *blockData = FS_RetailFastfileBlockData(reader, 0);
            const uint32_t blockSize = FS_RetailFastfileBlockSize(reader, 0);
            if (!blockData || wire.pixelDataOffset + wire.loadDefResourceSize > blockSize)
            {
                Com_Printf(0, "RetailWidenImageFromWire: pixel bounds fail name='%s' off=%u size=%u blockSize=%u\n",
                           name, wire.pixelDataOffset, wire.loadDefResourceSize, blockSize);
                return false;
            }
            std::memcpy(loadDef->data, blockData + wire.pixelDataOffset, wire.loadDefResourceSize);
            // Whole-payload scan for the baked lightmaps: the 4 KB probes at
            // upload time showed lightmap0_primary's first chunk all zero.
            // An all-zero payload (or a tiny average) means the embedded DXT
            // copy is the dark-frame cause, not the sampler/light setup.
            if (!strncmp(name, "*lightmap", 9))
            {
                const uint8_t *p = loadDef->data;
                const uint32_t bytes = wire.loadDefResourceSize;
                uint64_t sum = 0;
                uint32_t nonzero = 0;
                uint8_t maxByte = 0;
                uint32_t firstNonzero = bytes;
                for (uint32_t i = 0; i < bytes; ++i)
                {
                    const uint8_t b = p[i];
                    sum += b;
                    if (b)
                    {
                        ++nonzero;
                        if (firstNonzero == bytes)
                            firstNonzero = i;
                        if (b > maxByte)
                            maxByte = b;
                    }
                }
                
            }
        }
        image->texture.loadDef = loadDef;
    }

    if (!image->name[0])
    {
        // Anonymous per-material inline images are identified by their pool
        // slot (see RetailWidenMaterialFromWire's imagePool), never by name.
        // Routing them through the named DB_AddXAsset registry -- correct for
        // every genuinely-named image -- makes every empty-named one collide
        // into whichever was registered last (DB_LinkXAssetEntry dedupes by
        // (name,type) hash, and "" hashes the same for all of them).
        header->image = image;
        return true;
    }
    const XAssetHeader registered = RetailZoneLoadSessionRegister(session, ASSET_TYPE_IMAGE, {image});
    if (!registered.image)
    {
        Com_Printf(0, "RetailWidenImageFromWire: register failed name='%s'\n", name);
        return false;
    }
    *header = registered;
    return true;
}

bool RetailWalkLiveLoadImage(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                             uint32_t headerRef, XAssetHeader *header,
                             bool strict)
{
    if (!session || !reader || !header)
        return false;
    FsRetailFastfileImage wire;
    if (FS_ReadRetailFastfileImage(reader, headerRef, &wire) != FS_RETAIL_FF_WIRE_OK)
        return false;
    if (!RetailWidenImageFromWire(session, reader, wire, header, nullptr, strict))
        return false;
    // A top-level image asset is always named; an empty name here means the
    // wire name was unresolvable (the anonymous pool-image path inside the
    // widener is only valid for per-material inline textures, never for a
    // directory asset). Strict mode fails instead of leaving it anonymous.
    if (strict && header->image && !header->image->name[0])
    {
        Com_Printf(0, "RetailWalkLiveLoadImage: strict top-level image has no name\n");
        *header = {};
        return false;
    }
    return true;
}

// Strong override of the walk's weak default (see db_retail_walk.h):
// widens one just-walked inline image (XModel-nested textures and the
// like) through the production decoder so later aliases bind it by pool
// index or name instead of null. Tolerant by design: a nested image must
// never fail its parent walk (the deferred drain and the draw proof judge
// coverage, not the walker), and pixel upload stays gated at the device
// seam exactly like every other widened image. Registration reuses the
// widener's own named/anonymous contract, so empty-named pool images never
// collide in the named registry.
GfxImage *RetailWalkWidenNestedImage(RetailZoneLoadSession *session,
                                     FsRetailFastfileReader *reader,
                                     const FsRetailFastfileImage &wire)
{
    if (!session || !session->widenNestedImages)
        return nullptr;
    XAssetHeader out{};
    if (!RetailWidenImageFromWire(session, reader, wire, &out, nullptr, false) || !out.image)
        return nullptr;
    return out.image;
}
