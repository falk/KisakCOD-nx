#include "db_retail_decode_material.h"
#include "../universal/retail_asset_trace.h"

#include "db_retail_decode_image.h"
#include "db_retail_decode_techniqueset.h"
#include "db_retail_wire.h"
#include "db_retail_decode_water.h"
#include "../gfx_d3d/r_image.h"
#include "../gfx_d3d/r_material.h"

#include <cstdio>
#include <cstring>

namespace
{
constexpr uint32_t kInlineRef = 0xffffffffu;
constexpr uint32_t kInsertRef = 0xfffffffeu;
constexpr uint32_t kDeferredMaterialAliasCapacity = 1024;

struct DeferredMaterialAlias
{
    Material **slot;
    // Zone that owns `slot`'s arena; DB_RetailZoneEnd purges by this so a
    // resolve after unload cannot write through a freed/reused slot.
    uint32_t zoneIndex;
    char name[64];
};

DeferredMaterialAlias g_deferredMaterialAliases[kDeferredMaterialAliasCapacity]{};
uint32_t g_deferredMaterialAliasCount = 0;

// These menu/material names are authored material aliases, not engine builtin
// constructors or image filenames.  Their owning zone may arrive later;
// routing an unresolved alias to an IWI probe or generic material default
// would hide a bad cross-zone identity and make the load path non-causal.
bool IsRequiredIwiMaterial(const char *name)
{
    if (!name)
        return false;
    if (*name == ',')
        ++name;
    while (*name == ' ' || *name == '\t')
        ++name;
    return !std::strcmp(name, "$victorybackdrop") ||
           !std::strcmp(name, "$defeatbackdrop") ||
           !std::strcmp(name, "$levelbriefing") ||
           !std::strcmp(name, "javelin_overlay_grain");
}

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
    {
        Com_Printf(0, "ReadBlockString: decode failed nameRef=0x%08x\n", nameRef);
        return false;
    }
    if (token.offset >= FS_RetailFastfileBlockCursor(reader, token.block))
    {
        Com_Printf(0, "ReadBlockString: offset=%u >= cursor=%u block=%u\n",
                   token.offset, FS_RetailFastfileBlockCursor(reader, token.block), token.block);
        return false;
    }
    // Exact-span read: the string's own NUL terminates it, so request
    // only the bytes actually mirrored (offset..cursor), never a full
    // buffer -- a name near the end of a block made the old full-buffer
    // read fail with DIRECTORY_TRUNCATED even though the string was
    // fully present.
    const uint32_t available =
        FS_RetailFastfileBlockCursor(reader, token.block) - token.offset;
    const uint32_t readBytes = available < bufferSize - 1 ? available : bufferSize - 1;
    const FsRetailFastfileWireResult readResult = FS_ReadRetailFastfileBlock(
        reader, token.block, token.offset, reinterpret_cast<uint8_t *>(buffer), readBytes);
    if (readResult != FS_RETAIL_FF_WIRE_OK)
    {
        Com_Printf(0, "ReadBlockString: block read failed offset=%u block=%u result=%d\n",
                   token.offset, token.block, (int)readResult);
        return false;
    }
    // The name must end inside the bytes read: a name longer than the buffer
    // would otherwise be truncated into another asset's identity, and one
    // running into the unwritten mirror past the cursor would leave the rest
    // of the buffer uninitialized. Both fail loudly.
    if (!std::memchr(buffer, 0, readBytes))
    {
        Com_Printf(0, "ReadBlockString: name at offset=%u is unterminated or longer than %u bytes\n",
                   token.offset, bufferSize - 1);
        return false;
    }
    buffer[bufferSize - 1] = '\0';
    if (!buffer[0])
    {
        Com_Printf(0, "ReadBlockString: empty string offset=%u block=%u\n", token.offset,
                   token.block);
        return false;
    }
    for (const char *p = buffer; *p; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c >= 0x7f)
        {
            // Escape every byte instead of printing raw bytes: buffer holds
            // non-ASCII data by construction here, and an unescaped dump can
            // corrupt tools (e.g. subprocess capture) that assume UTF-8 text.
            char hex[3 * 64 + 1];
            uint32_t hexLen = 0;
            for (uint32_t i = 0; i < bufferSize - 1 && buffer[i] && hexLen + 3 < sizeof(hex); ++i)
                hexLen += static_cast<uint32_t>(std::snprintf(hex + hexLen, sizeof(hex) - hexLen,
                                                              "%02x ", static_cast<unsigned char>(buffer[i])));
            Com_Printf(0, "ReadBlockString: non-printable byte 0x%02x at offset=%u block=%u raw_hex='%s'\n",
                       c, token.offset, token.block, hex);
            return false;
        }
    }
    return true;
}

// Ported verbatim from switch_sp_bootstrap.cpp's ResolveTechniqueSetRef,
// generalized from the bootstrap's fixed kUiAssetCount/local stubNames[] to
// caller-supplied directoryBytes/stubNames/stubCount (see the declaration
// comment in db_retail_decode_material.h): a reference that decodes to an
// offset inside the calling zone's own directory bytes, aligned to a
// header-field slot (offset & 7 == 4), names *that* directory entry rather
// than a separate string -- valid only when that entry was itself a
// TechniqueSet whose decoded name is comma-prefixed (a stub), in which case
// the real target is looked up by the comma-stripped name through the real,
// global DB_FindXAssetHeader (an earlier-loaded zone's same-named original).
// Every other reference shape falls through to the plain wire-name form.
MaterialTechniqueSet *ResolveTechniqueSetRef(FsRetailFastfileReader *reader, uint32_t techRef,
                                             uint32_t directoryOffset, uint32_t directoryBytes,
                                             const char *const *stubNames, uint32_t stubCount,
                                             RetailWireTechniqueCache *cache = nullptr)
{
    RETAIL_ASSET_TRACE(0, "ResolveTechniqueSetRef: techRef=%x dirOffset=%u dirBytes=%u stubCount=%u\n",
                       techRef, directoryOffset, directoryBytes, stubCount);
    if (!techRef || techRef == kInlineRef || techRef == kInsertRef)
        return nullptr;
    RetailWireToken token{};
    if (directoryBytes && stubNames && DecodeFastfileToken(reader, techRef, 0, 1u << 4, &token) &&
        token.kind == RETAIL_WIRE_TOKEN_OFFSET)
    {
        const uint32_t offset = token.offset;
        RETAIL_ASSET_TRACE(0, "ResolveTechniqueSetRef: token.offset=%u\n", offset);
        if (offset >= directoryOffset && offset < directoryOffset + directoryBytes &&
            ((offset - directoryOffset) & 7u) == 4u)
        {
            const uint32_t entry = (offset - directoryOffset - 4u) / 8u;
            RETAIL_ASSET_TRACE(0, "ResolveTechniqueSetRef: entry=%u stub=%s\n", entry,
                               (entry < stubCount && stubNames[entry]) ? stubNames[entry] : "null");
            if (entry < stubCount && stubNames[entry] && stubNames[entry][0])
            {
                const char *look = (stubNames[entry][0] == ',') ? stubNames[entry] + 1 : stubNames[entry];
                return DB_FindXAssetHeader(ASSET_TYPE_TECHNIQUE_SET, look).techniqueSet;
            }
            return nullptr;
        }
    }
    char name[64]{};
    if (cache)
    {
        const char *known = cache->FindString(techRef);
        if (known)
        {
            std::strncpy(name, known, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        }
    }
    if (!name[0])
    {
        if (!ReadBlockString(reader, techRef, name, sizeof(name)))
        {
            Com_Printf(0, "ResolveTechniqueSetRef: ReadBlockString failed for techRef=%x\n", techRef);
            return nullptr;
        }
    }
    const char *look = (*name == ',') ? name + 1 : name;
    RETAIL_ASSET_TRACE(0, "ResolveTechniqueSetRef: looking up '%s' in DB\n", look);
    return DB_FindXAssetHeader(ASSET_TYPE_TECHNIQUE_SET, look).techniqueSet;
}
} // namespace

// process-lifetime count of material bodies manufactured by
// RetailMaterial_Synthesize2D.  The original engine has no such constructor:
// any by-name material miss goes through DB_FindXAssetHeader, which registers
// the type's default entry (DB_CreateDefaultEntry) instead of inventing a
// body.  Production walk checkpoints must observe zero synths.
static uint32_t s_materialSynthesizedCount = 0;

uint32_t RetailMaterialSynthesizedCount()
{
    return s_materialSynthesizedCount;
}

bool RetailDeferMaterialAlias(Material **slot, const char *name, uint32_t zoneIndex)
{
    if (!IsRequiredIwiMaterial(name))
        return false;
    for (uint32_t i = 0; i < g_deferredMaterialAliasCount; ++i)
    {
        DeferredMaterialAlias &entry = g_deferredMaterialAliases[i];
        if (!entry.name[0])
            continue;
        if (entry.slot == slot && !std::strcmp(entry.name, name))
            return true;
    }
    if (g_deferredMaterialAliasCount >= kDeferredMaterialAliasCapacity)
        return false;
    DeferredMaterialAlias &entry = g_deferredMaterialAliases[g_deferredMaterialAliasCount++];
    entry.slot = slot;
    entry.zoneIndex = zoneIndex;
    std::strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    return true;
}

void RetailMaterialZoneUnloaded(uint32_t zoneIndex)
{
    if (zoneIndex == 0)
        return;
    for (uint32_t i = 0; i < g_deferredMaterialAliasCount; ++i)
    {
        DeferredMaterialAlias &entry = g_deferredMaterialAliases[i];
        if (entry.name[0] && entry.zoneIndex == zoneIndex)
        {
            entry.name[0] = '\0';
            entry.slot = nullptr;
            entry.zoneIndex = 0;
        }
    }
}

uint32_t RetailMaterialDeferredAliasCount()
{
    uint32_t outstanding = 0;
    for (uint32_t i = 0; i < g_deferredMaterialAliasCount; ++i)
    {
        if (g_deferredMaterialAliases[i].name[0])
            ++outstanding;
    }
    return outstanding;
}

uint32_t RetailResolveDeferredMaterialAliases(bool failIfUnresolved)
{
    const uint32_t outstandingBefore = RetailMaterialDeferredAliasCount();
    uint32_t unresolved = 0;
    uint32_t resolved = 0;
    for (uint32_t i = 0; i < g_deferredMaterialAliasCount; ++i)
    {
        DeferredMaterialAlias &entry = g_deferredMaterialAliases[i];
        if (!entry.name[0])
            continue;
        const char *cleanName = entry.name + (entry.name[0] == ',' ? 1 : 0);
        XAssetHeader header = DB_FindXAssetHeaderNoDefault(ASSET_TYPE_MATERIAL, cleanName);
        Material *material = header.material;
        GfxImage *image = material && material->textureTable
            ? MaterialTextureImage(material->textureTable[0]) : nullptr;
        if (!image || !image->texture.basemap)
        {
            ++unresolved;
            Com_Printf(8, "RetailMaterialAlias: owner '%s' is not registered with a live image\n",
                       cleanName);
            continue;
        }
        if (entry.slot)
            *entry.slot = material;
        Com_Printf(0, "RetailMaterialAlias: resolved '%s' -> material=%p image=%p\n",
                   cleanName, material, image);
        entry.slot = nullptr;
        entry.zoneIndex = 0;
        entry.name[0] = '\0';
        ++resolved;
    }
    // (closure fact).  A pass that ends with outstanding entries
    // is legal only while an owner zone can still arrive; the movement
    // verifier requires the final count to be zero.  Print every pass that
    // had work so the run carries the evidence, resolved or not.
    if (outstandingBefore || resolved)
    {
        Com_Printf(0, "KILLHOUSE_MATERIAL_ALIAS outstanding_before=%u resolved=%u unresolved=%u outstanding=%u\n",
                   outstandingBefore, resolved, unresolved,
                   RetailMaterialDeferredAliasCount());
    }
    if (failIfUnresolved && unresolved)
        Com_Error(ERR_DROP, "Required material aliases unresolved (%u)\n", unresolved);
    return unresolved;
}

bool RetailIsNonSpMaterialAlias(const char *name)
{
    if (!name)
        return false;
    if (*name == ',')
        ++name;
    while (*name == ' ' || *name == '\t')
        ++name;
    // Comma-prefixed directory stubs whose real body exists only in a zone
    // the SP checkpoint never loads ($defeatbackdrop, $levelbriefing,
    // javelin_overlay_grain). Treat them as null-bind consumer aliases
    // instead of retaining an unresolved owner, so the zone load can still
    // report success.
    return !std::strcmp(name, "$defeatbackdrop") ||
           !std::strcmp(name, "$levelbriefing") ||
           !std::strcmp(name, "javelin_overlay_grain");
}

// Exposed (unlike ResolveTechniqueSetRef/DecodeFastfileToken/ReadBlockString
// above, which stay file-private) so RetailWalkLiveLoadLightDef
// (db_retail_walk.cpp) can resolve a non-inline attenuation image through
// the exact same comma-stub-directory/pool-alias/plain-name rules a
// material's texture-def slot already uses, instead of failing loudly on
// every non-inline form. Declared in db_retail_decode_material.h.
GfxImage *ResolveImageRef(FsRetailFastfileReader *reader, uint32_t imageRef,
                           uint32_t directoryOffset, uint32_t directoryBytes,
                           const char *const *stubNames, uint32_t stubCount,
                           bool strict)
{
    if (!imageRef || imageRef == kInlineRef || imageRef == kInsertRef)
        return nullptr;

    Com_Printf(0, "ResolveImageRef: imageRef=0x%08x cursor4=%u\n",
               imageRef, FS_RetailFastfileBlockCursor(reader, 4));

    // db_load.cpp's real, sole mechanism for a non-inline GfxImage pointer
    // (Load_GfxImagePtr -> DB_ConvertOffsetToAlias, every GfxImage* field
    // uniformly, not just a material texture-def slot): imageRef is a
    // (block,offset) pair whose raw 4 bytes already hold a pooled
    // reference once the declaring occurrence has widened. Try this before
    // any of this port's own extensions below (the comma-stub-directory
    // and plain-name paths, for forms db_load.cpp's own mechanism doesn't
    // cover, like cross-zone UI stubs).
    {
        uint32_t aliasedRef = 0;
        if (FS_RetailFastfileResolveImageAlias(reader, imageRef, &aliasedRef) ==
            FS_RETAIL_FF_WIRE_OK)
        {
            uint32_t pooledNameRef = 0;
            char imageName[64]{};
            if (FS_RetailFastfileImagePoolNameRef(reader, aliasedRef, &pooledNameRef) ==
                    FS_RETAIL_FF_WIRE_OK &&
                ReadBlockString(reader, pooledNameRef, imageName, sizeof(imageName)))
            {
                const char *look = (imageName[0] == ',') ? imageName + 1 : imageName;
                if (DB_XAssetExists(ASSET_TYPE_IMAGE, look))
                    return DB_FindXAssetHeader(ASSET_TYPE_IMAGE, look).image;
            }
        }
    }

    RetailWireToken token{};
    if (directoryBytes && stubNames &&
        DecodeFastfileToken(reader, imageRef, 0, 1u << 4, &token) &&
        token.kind == RETAIL_WIRE_TOKEN_OFFSET)
    {
        const uint32_t offset = token.offset;
        Com_Printf(0, "ResolveImageRef: token offset=%u dirOffset=%u dirBytes=%u\n",
                   offset, directoryOffset, directoryBytes);
        if (offset >= directoryOffset && offset < directoryOffset + directoryBytes &&
            ((offset - directoryOffset) & 7u) == 4u)
        {
            const uint32_t entry = (offset - directoryOffset - 4u) / 8u;
            if (entry < stubCount && stubNames[entry] && stubNames[entry][0])
            {
                const char *look = (stubNames[entry][0] == ',') ? stubNames[entry] + 1 : stubNames[entry];
                if (DB_XAssetExists(ASSET_TYPE_IMAGE, look))
                    return DB_FindXAssetHeader(ASSET_TYPE_IMAGE, look).image;
            }
        }
    }

    uint32_t pooledNameRef = 0;
    if (FS_RetailFastfileImagePoolNameRef(reader, imageRef, &pooledNameRef) == FS_RETAIL_FF_WIRE_OK)
    {
        char imageName[64]{};
        Com_Printf(0, "ResolveImageRef: pool lookup imageRef=0x%08x pooledNameRef=0x%08x\n",
                   imageRef, pooledNameRef);
        if (ReadBlockString(reader, pooledNameRef, imageName, sizeof(imageName)))
        {
            const char *look = (imageName[0] == ',') ? imageName + 1 : imageName;
            Com_Printf(0, "ResolveImageRef: pool name='%s' exists=%d\n", look,
                       (int)DB_XAssetExists(ASSET_TYPE_IMAGE, look));
            if (DB_XAssetExists(ASSET_TYPE_IMAGE, look))
                return DB_FindXAssetHeader(ASSET_TYPE_IMAGE, look).image;
        }
    }

    char name[64]{};
    if (ReadBlockString(reader, imageRef, name, sizeof(name)))
    {
        Com_Printf(0, "ResolveImageRef: ReadBlockString got '%s'\n", name);
        const char *look = (name[0] == ',') ? name + 1 : name;
        if (DB_XAssetExists(ASSET_TYPE_IMAGE, look))
            return DB_FindXAssetHeader(ASSET_TYPE_IMAGE, look).image;
    }
    else
    {
        Com_Printf(0, "ResolveImageRef: ReadBlockString failed for 0x%08x\n", imageRef);
    }

    if (!strict && DB_XAssetExists(ASSET_TYPE_IMAGE, "$white"))
    {
        Com_Printf(0, "ResolveImageRef: imageRef=0x%08x fallback to $white\n", imageRef);
        return DB_FindXAssetHeader(ASSET_TYPE_IMAGE, "$white").image;
    }

    return nullptr;
}

Material *RetailMaterial_Synthesize2D(const char *name, RetailZoneLoadSession *session)
{
    if (!name || !name[0])
        return nullptr;

    const char *cleanName = (name[0] == ',') ? name + 1 : name;
    while (*cleanName == ' ' || *cleanName == '\t')
        cleanName++;
    if (!cleanName[0])
        return nullptr;

    if (DB_XAssetExists(ASSET_TYPE_MATERIAL, cleanName) && !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, cleanName))
        return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, cleanName).material;

    // Past the registered-original check this call is manufacturing a body
    // the original engine would not have: count the attempt so a checkpoint
    // can prove a production walk took no synthesis path.
    ++s_materialSynthesizedCount;

    // 1. Find or load image
    GfxImage *image = nullptr;
    if (DB_XAssetExists(ASSET_TYPE_IMAGE, cleanName) && !DB_IsXAssetDefault(ASSET_TYPE_IMAGE, cleanName))
    {
        image = DB_FindXAssetHeader(ASSET_TYPE_IMAGE, cleanName).image;
    }
    if (!image || !image->texture.basemap)
    {
        image = Image_FindExisting_LoadObj(cleanName);
    }
    if (!image || !image->texture.basemap)
    {
        image = Image_Alloc(const_cast<char *>(cleanName), 3u, 3, 0);
        if (image)
        {
            if (!Image_LoadFromFile(image) || !image->texture.basemap)
            {
                Com_PrintError(8, "RetailMaterial_Synthesize2D: failed to load IWI image for '%s'\n", cleanName);
                return nullptr;
            }
            if (session && session->active)
                RetailZoneLoadSessionRegister(session, ASSET_TYPE_IMAGE, {image});
            else
                DB_RegisterImage(image);
        }
    }
    if (!image || !image->texture.basemap)
        return nullptr;

    // Reference material to borrow 2D techniqueSet, drawSurf, and blend state
    Material *refMat = nullptr;
    if (DB_XAssetExists(ASSET_TYPE_MATERIAL, "button_highlight_end") &&
        !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "button_highlight_end"))
    {
        refMat = DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, "button_highlight_end").material;
    }
    else if (DB_XAssetExists(ASSET_TYPE_MATERIAL, "white") &&
             !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "white"))
    {
        refMat = DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, "white").material;
    }

    MaterialTechniqueSet *ts = refMat ? refMat->techniqueSet : nullptr;
    if (!ts)
    {
        if (DB_XAssetExists(ASSET_TYPE_TECHNIQUE_SET, "2d"))
            ts = DB_FindXAssetHeader(ASSET_TYPE_TECHNIQUE_SET, "2d").techniqueSet;
        else if (DB_XAssetExists(ASSET_TYPE_TECHNIQUE_SET, "sm2/2d"))
            ts = DB_FindXAssetHeader(ASSET_TYPE_TECHNIQUE_SET, "sm2/2d").techniqueSet;
    }

    const uint8_t stateCount = (refMat && refMat->stateBitsCount > 0) ? refMat->stateBitsCount : 1;
    const std::size_t stateBytes = stateCount * sizeof(GfxStateBits);
    const std::size_t nameBytes = std::strlen(cleanName) + 1;

    Material *material = nullptr;
    char *nameCopy = nullptr;
    MaterialTextureDef *textureTable = nullptr;
    GfxStateBits *stateBits = nullptr;

    if (session && session->active)
    {
        material = static_cast<Material *>(
            RetailZoneLoadSessionAlloc(session, sizeof(Material), alignof(Material)));
        nameCopy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, nameBytes, 1));
        textureTable = static_cast<MaterialTextureDef *>(
            RetailZoneLoadSessionAlloc(session, sizeof(MaterialTextureDef), alignof(MaterialTextureDef)));
        stateBits = static_cast<GfxStateBits *>(
            RetailZoneLoadSessionAlloc(session, stateBytes, alignof(GfxStateBits)));
    }
    else
    {
        material = reinterpret_cast<Material *>(Material_Alloc(sizeof(Material)));
        nameCopy = reinterpret_cast<char *>(Material_Alloc(nameBytes));
        textureTable = reinterpret_cast<MaterialTextureDef *>(Material_Alloc(sizeof(MaterialTextureDef)));
        stateBits = reinterpret_cast<GfxStateBits *>(Material_Alloc(stateBytes));
    }

    if (!material || !nameCopy || !textureTable || !stateBits)
        return nullptr;

    std::memset(material, 0, sizeof(*material));
    std::memcpy(nameCopy, cleanName, nameBytes);
    std::memset(textureTable, 0, sizeof(MaterialTextureDef));
    std::memset(stateBits, 0, stateBytes);

    textureTable[0].nameHash = 0xa0ab1041; // "colorMap"
    textureTable[0].samplerState =
        (refMat && refMat->textureCount > 0) ? refMat->textureTable[0].samplerState : 0x62;
    textureTable[0].semantic = 3;
    textureTable[0].u.image = image;

    material->info.name = nameCopy;
    material->info.sortKey = refMat ? refMat->info.sortKey : 43;
    if (refMat)
        material->info.drawSurf = refMat->info.drawSurf;
    else
        material->info.drawSurf.packed = 0x0101;
    material->textureCount = 1;
    material->stateBitsCount = stateCount;
    material->techniqueSet = ts;
    material->textureTable = textureTable;
    material->stateBitsTable = stateBits;

    if (refMat)
    {
        std::memcpy(material->stateBitsEntry, refMat->stateBitsEntry, sizeof(material->stateBitsEntry));
        material->stateFlags = refMat->stateFlags;
        material->cameraRegion = refMat->cameraRegion;
        if (refMat->stateBitsTable && refMat->stateBitsCount > 0)
            std::memcpy(stateBits, refMat->stateBitsTable, stateBytes);
    }

    XAssetHeader registered{};
    if (session && session->active)
        registered = RetailZoneLoadSessionRegister(session, ASSET_TYPE_MATERIAL, {material});
    else
        registered.material = DB_RegisterMaterial(material);

    Com_Printf(0, "RetailMaterial_Synthesize2D: synthesized '%s' -> %p (image=%p basemap=%p)\n",
               cleanName, registered.material, image, image ? image->texture.basemap : nullptr);
    return registered.material;
}

Material *ResolveMaterialRef(FsRetailFastfileReader *reader, uint32_t matRef,
                             uint32_t directoryOffset, uint32_t directoryBytes,
                             const char *const *stubNames, uint32_t stubCount,
                             bool strict, char *deferredName,
                             std::size_t deferredNameCapacity)
{
    if (deferredName && deferredNameCapacity)
        deferredName[0] = '\0';
    auto defer = [&](const char *name) {
        if (deferredName && deferredNameCapacity && name)
        {
            std::strncpy(deferredName, name, deferredNameCapacity - 1);
            deferredName[deferredNameCapacity - 1] = '\0';
        }
    };
    if (!matRef || matRef == kInlineRef || matRef == kInsertRef)
        return nullptr;

    // Hardcoded per-offset identity cases plus 2D synthesis: the tolerant
    // menu path's answer for ui.ff background refs whose real bodies the
    // old ItemDef walk used to discard. Strict acceptance mode excludes
    // all of it: a reference must resolve through the exact directory,
    // slot, or name rules below, or fail.
    if (!strict)
    {
    if (matRef == 0x4000262d)
    {
        if (DB_XAssetExists(ASSET_TYPE_MATERIAL, "button_highlight_end") &&
            !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "button_highlight_end"))
            return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, "button_highlight_end").material;
    }
    else if (matRef == 0x40002809)
    {
        if (DB_XAssetExists(ASSET_TYPE_MATERIAL, "gradient_fadein") &&
            !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "gradient_fadein"))
            return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, "gradient_fadein").material;
        Material *synth = RetailMaterial_Synthesize2D("gradient_fadein", nullptr);
        if (synth)
            return synth;
    }
    else if (matRef == 0x400007d9)
    {
        if (DB_XAssetExists(ASSET_TYPE_MATERIAL, "animbg_fogscroll") &&
            !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "animbg_fogscroll"))
            return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, "animbg_fogscroll").material;
        Material *synth = RetailMaterial_Synthesize2D("animbg_fogscroll", nullptr);
        if (synth)
            return synth;
    }
    else if (matRef == 0x400009e1)
    {
        if (DB_XAssetExists(ASSET_TYPE_MATERIAL, "animbg_fogscrollthin") &&
            !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "animbg_fogscrollthin"))
            return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, "animbg_fogscrollthin").material;
        Material *synth = RetailMaterial_Synthesize2D("animbg_fogscrollthin", nullptr);
        if (synth)
            return synth;
    }
    else if (matRef == 0x40000d51)
    {
        if (DB_XAssetExists(ASSET_TYPE_MATERIAL, "animbg_front") &&
            !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "animbg_front"))
            return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, "animbg_front").material;
        Material *synth = RetailMaterial_Synthesize2D("animbg_front", nullptr);
        if (synth)
            return synth;
    }
    else if (matRef == 0x400010c9 || matRef == 0x400012d1)
    {
        if (DB_XAssetExists(ASSET_TYPE_MATERIAL, "animbg_front2") &&
            !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "animbg_front2"))
            return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, "animbg_front2").material;
        Material *synth = RetailMaterial_Synthesize2D("animbg_front2", nullptr);
        if (synth)
            return synth;
    }
    }

    RetailWireToken token{};
    const bool hasToken = DecodeFastfileToken(reader, matRef, 0, 1u << 4, &token);
    if (hasToken && directoryBytes && stubNames &&
        token.kind == RETAIL_WIRE_TOKEN_OFFSET)
    {
        const uint32_t offset = token.offset;
        if (offset >= directoryOffset && offset < directoryOffset + directoryBytes &&
            ((offset - directoryOffset) & 7u) == 4u)
        {
            const uint32_t entry = (offset - directoryOffset - 4u) / 8u;
            if (entry < stubCount && stubNames[entry] && stubNames[entry][0])
            {
                const char *rawName = stubNames[entry];
                const char *look = (rawName[0] == ',') ? rawName + 1 : rawName;
                if (DB_XAssetExists(ASSET_TYPE_MATERIAL, look) && !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, look))
                    return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, look).material;
                // comma-prefixed stub -> original DB_LinkXAssetEntry
                // stub semantics (existing owner or registered default entry),
                // never a synthesized body.
                if (rawName[0] == ',')
                    return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, look).material;
                if (!strict && !IsRequiredIwiMaterial(look))
                {
                    Material *synth = RetailMaterial_Synthesize2D(look, nullptr);
                    if (synth)
                        return synth;
                }
                else if (IsRequiredIwiMaterial(look))
                    defer(look);
            }
        }
    }

    // Check if matRef points to a 4-byte slot containing a nameRef or pointer
    if (hasToken && token.kind == RETAIL_WIRE_TOKEN_OFFSET &&
        token.offset + 4u <= FS_RetailFastfileBlockSize(reader, token.block))
    {
        const uint8_t *slotPtr = FS_RetailFastfileBlockData(reader, token.block) + token.offset;
        const uint32_t slotVal = static_cast<uint32_t>(slotPtr[0]) |
                                 (static_cast<uint32_t>(slotPtr[1]) << 8) |
                                 (static_cast<uint32_t>(slotPtr[2]) << 16) |
                                 (static_cast<uint32_t>(slotPtr[3]) << 24);
        char slotName[64]{};
        if (slotVal && ReadBlockString(reader, slotVal, slotName, sizeof(slotName)))
        {
            const char *look = (slotName[0] == ',') ? slotName + 1 : slotName;
            if (DB_XAssetExists(ASSET_TYPE_MATERIAL, look) && !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, look))
                return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, look).material;
            if (slotName[0] == ',')
                return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, look).material;
            if (!strict && !IsRequiredIwiMaterial(look))
            {
                Material *synth = RetailMaterial_Synthesize2D(look, nullptr);
                if (synth)
                    return synth;
            }
            else if (IsRequiredIwiMaterial(look))
                defer(look);
        }
    }

    char matName[64]{};
    if (ReadBlockString(reader, matRef, matName, sizeof(matName)))
    {
        const char *look = (matName[0] == ',') ? matName + 1 : matName;
        if (DB_XAssetExists(ASSET_TYPE_MATERIAL, look) && !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, look))
            return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, look).material;
        if (matName[0] == ',')
            return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, look).material;
        if (!strict && !IsRequiredIwiMaterial(look))
        {
            Material *synth = RetailMaterial_Synthesize2D(look, nullptr);
            if (synth)
                return synth;
        }
        else if (IsRequiredIwiMaterial(look))
            defer(look);
    }

    if (IsRequiredIwiMaterial(matName))
        defer(matName);
    else if (!strict)
    {
        if (DB_XAssetExists(ASSET_TYPE_MATERIAL, "white") && !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, "white"))
            return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, "white").material;
        if (DB_XAssetExists(ASSET_TYPE_MATERIAL, "$default"))
            return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, "$default").material;
    }
    return nullptr;
}

bool RetailWidenMaterialFromWire(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                                 const FsRetailFastfileMaterial &wire,
                                 const FsRetailFastfileTextureDef *textureDefs,
                                 uint32_t directoryOffset, uint32_t directoryBytes,
                                 const char *const *stubNames, uint32_t stubCount,
                                 RetailWireTechniqueCache *cache, XAssetHeader *header,
                                 bool strict, bool allowNullName, bool allowNullTechniqueSet,
                                 bool *deferredAlias, char *deferredAliasName,
                                 std::size_t deferredAliasNameCapacity)
{
    if (!session || !session->active || !reader || !header)
        return false;
    *header = {};
    if (deferredAlias)
        *deferredAlias = false;
    if (deferredAliasName && deferredAliasNameCapacity)
        deferredAliasName[0] = '\0';

    if (wire.textureCount > 8)
    {
        Com_Printf(0, "RetailWidenMaterialFromWire: textureCount=%u > 8\n", wire.textureCount);
        return false;
    }

    char name[64]{};
    if (wire.nameWasInline)
    {
        ReadBlockString(reader, wire.nameRef, name, sizeof(name));
    }
    else
    {
        if (cache)
        {
            const char *known = cache->FindString(wire.nameRef);
            if (known)
            {
                std::strncpy(name, known, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';
            }
        }
        if (!name[0])
        {
            ReadBlockString(reader, wire.nameRef, name, sizeof(name));
            if (std::strncmp(name, "ps_", 3) == 0 || std::strncmp(name, "vs_", 3) == 0)
            {
                name[0] = '\0';
            }
        }
    }

    MaterialTechniqueSet *techniqueSet = nullptr;
    if (wire.techniqueSetRef)
    {
        techniqueSet = ResolveTechniqueSetRef(
            reader, wire.techniqueSetRef, directoryOffset, directoryBytes, stubNames, stubCount, cache);
        // A null techniqueSet here is engine-garbage-equivalent (the real
        // loader binds whatever ConvertOffsetToAlias reads, valid or not).
        // Nested-only tolerance: a top-level material must still fail, but
        // a world-nested spare keeps its null and fails loudly only if the
        // renderer ever selects it.
        if ((!techniqueSet || !techniqueSet->name) && !allowNullTechniqueSet)
            return false;
        if (!techniqueSet || !techniqueSet->name)
        {
            Com_Printf(0, "RetailWalkLiveLoadMaterial: null techniqueSet tolerated in material '%s' techRef=0x%08x\n",
                       name, wire.techniqueSetRef);
            techniqueSet = nullptr;
        }
    }

    if (!name[0] && techniqueSet && techniqueSet->name)
    {
        std::strncpy(name, techniqueSet->name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        if (cache)
            cache->RecordString(wire.nameRef, name);
    }
    if (!name[0] && !allowNullName)
    {
        Com_Printf(0, "RetailWalkLiveLoadMaterial: failed to resolve name\n");
        return false;
    }
    RETAIL_ASSET_TRACE(0, "RetailWalkLiveLoadMaterial: name='%s'\n", name);

    const char *cleanName = (name[0] == ',') ? name + 1 : name;
    if (name[0] == ',')
    {
        if (DB_XAssetExists(ASSET_TYPE_MATERIAL, cleanName) && !DB_IsXAssetDefault(ASSET_TYPE_MATERIAL, cleanName))
        {
            *header = DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, cleanName);
            return true;
        }

        // Strict acceptance mode resolves a comma stub only to an already
        // registered original: no synthesis, and never the generic default
        // entry (returning a placeholder under the requested name is silent
        // substitution, the exact failure the menu-texture work diagnosed).
        if (!strict)
        {
            if (IsRequiredIwiMaterial(cleanName))
            {
                // A comma-prefixed material is an external alias, not an
                // image filename.  Its owning zone can arrive later (the
                // code_post_gfx alias for $victorybackdrop is fulfilled by
                // killhouse.ff), so retain the unresolved slot without
                // manufacturing a material/default or probing images/<alias>.iwi.
                Com_Printf(8,
                           "RetailWalkLiveLoadMaterial: required material alias '%s' deferred\n",
                           cleanName);
                if (deferredAlias)
                    *deferredAlias = true;
                if (deferredAliasName && deferredAliasNameCapacity)
                {
                    std::strncpy(deferredAliasName, cleanName, deferredAliasNameCapacity - 1);
                    deferredAliasName[deferredAliasNameCapacity - 1] = '\0';
                }
                return false;
            }
            // original DB_LinkXAssetEntry's stub branch.  A
            // comma-prefixed directory name with no registered owner is not a
            // body: it resolves to the type's default entry, which
            // DB_FindXAssetHeader creates and registers under the stripped
            // name (DB_CreateDefaultEntry).  The old RetailMaterial_Synthesize2D
            // call here loaded a loose images/<name>.iwi and manufactured a
            // 512x512 material that retail Killhouse never has (gasmask_overlay
            // is only owned by the airplane/cargoship campaign zones), which
            // made the registry and frame evidence non-causal.
            *header = DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, cleanName);
            return true;
        }
        Com_Printf(0, "RetailWalkLiveLoadMaterial: strict comma stub '%s' has no registered original\n",
                   cleanName);
        return false;
    }

    MaterialTextureDef *textureTable = nullptr;
    if (wire.textureCount)
    {
        textureTable = static_cast<MaterialTextureDef *>(RetailZoneLoadSessionAlloc(
            session, wire.textureCount * sizeof(MaterialTextureDef), alignof(MaterialTextureDef)));
        if (!textureTable)
            return false;
        std::memset(textureTable, 0, wire.textureCount * sizeof(MaterialTextureDef));
    }
    for (uint32_t t = 0; t < wire.textureCount; ++t)
    {
        const FsRetailFastfileTextureDef &wireDef = textureDefs[t];
        GfxImage *image = nullptr;
        // Anonymous per-material inline textures (imageWasInline, or an alias
        // whose ref is pool-tagged -- see RETAIL_FASTFILE_POOL_BLOCK in
        // com_files.h) are identified by their pool slot, not by name: many
        // legitimately carry an empty name, and routing them through the
        // named DB_AddXAsset registry (like every other, genuinely-named
        // asset correctly does) makes every empty-named one collide into
        // whichever was registered last. Resolve/populate session->imagePool
        // directly instead, bypassing the name registry for these.
        const uint32_t poolIndex = (wireDef.imageRef - 1u) & 0x0fffffffu;
        const bool isPoolRef = (wireDef.imageRef >> 28) == 15u && poolIndex < RETAIL_FASTFILE_IMAGE_POOL_MAX;
        if (wireDef.imageWasInline)
        {
            XAssetHeader imageHeader{};
            // A single-texture material's one image is its colorMap, and CoD4's
            // UI assets name that image exactly like the material.  Offered only
            // as a fallback for a name our block-4 mirror cannot resolve, and
            // only accepted there against the real images/<name>.iwi header --
            // see RetailWidenImageFromWire's contract. Strict acceptance mode
            // never guesses: an unresolvable inline name fails instead.
            const char *imageNameFallback = (!strict && wire.textureCount == 1) ? cleanName : nullptr;
            if (!RetailWidenImageFromWire(session, reader, wireDef.inlineImage, &imageHeader,
                                          imageNameFallback))
                return false;
            image = imageHeader.image;
            if (isPoolRef && image)
                session->imagePool[poolIndex] = image;
        }
        else if (isPoolRef && session->imagePool[poolIndex])
        {
            image = session->imagePool[poolIndex];
        }
        else if (strict && !wireDef.imageRef)
        {
            // Empty texture slot under strict mode: no stream bytes belong
            // to it in any form, so there is nothing to defer or resolve.
            // (Non-strict keeps routing nulls through ResolveImageRef's
            // $white fallback exactly as before.)
            image = nullptr;
        }
        else
        {
            image = ResolveImageRef(reader, wireDef.imageRef, directoryOffset, directoryBytes,
                                    stubNames, stubCount, strict);
            if (!image)
            {
                // Unresolvable now, maybe resolvable later: the slot may
                // address block-4 bytes the stream has not produced yet (a
                // pool slot of a later declarer, or a later inline image
                // -- e.g. a GfxWorld materialMemory material naming a
                // top-level material's texture-def slot). The real loader
                // resolves offsets order-independently, so record the slot
                // and keep widening (consumption must stay
                // validation-independent); the post-directory pass retries
                // every recording once the mirror and pool are whole, and
                // only references outside the streamed zone still fail
                // there. Non-strict keeps its immediate $white fallback
                // (no recording).
                if (!strict || !cache)
                {
                    Com_Printf(0, "RetailWalkLiveLoadMaterial: failed to resolve image for imageRef=0x%08x in material '%s'\n",
                               wireDef.imageRef, name);
                    return false;
                }
                if (cache->deferredImageCount >= 2048)
                {
                    Com_Printf(0, "RetailWalkLiveLoadMaterial: deferred image table full in material '%s'\n",
                               name);
                    return false;
                }
                RetailWireTechniqueCache::DeferredImageSlot &deferred =
                    cache->deferredImages[cache->deferredImageCount++];
                deferred.table = textureTable;
                deferred.texIndex = t;
                deferred.reference = wireDef.imageRef;
                std::strncpy(deferred.materialName, name, sizeof(deferred.materialName) - 1);
                deferred.materialName[sizeof(deferred.materialName) - 1] = '\0';
            }
        }
        textureTable[t].nameHash = wireDef.nameHash;
        textureTable[t].nameStart = wireDef.nameStart;
        textureTable[t].nameEnd = wireDef.nameEnd;
        textureTable[t].samplerState = wireDef.samplerState;
        textureTable[t].semantic = wireDef.semantic;
        if (wireDef.semantic == 11 && wireDef.water.reference)
        {
            if (!wireDef.imageRef)
            {
                Com_Printf(0, "RetailWidenMaterialFromWire: water has no image in '%s'\n", name);
                return false;
            }
            water_t *water = nullptr;
            if (cache)
                for (uint32_t i = 0; i < cache->waterCount; ++i)
                    if (cache->waters[i].reference == wireDef.water.reference)
                        water = cache->waters[i].water;
            if (!water)
            {
                const FsRetailFastfileWater &body = wireDef.water;
                if (!body.m || body.m != body.n || body.m > 64 ||
                    (body.m & (body.m - 1u)))
                {
                    Com_Printf(0, "RetailWidenMaterialFromWire: invalid water dimensions %ux%u in '%s'\n",
                               body.m, body.n, name);
                    return false;
                }
                water = static_cast<water_t *>(RetailZoneLoadSessionAlloc(
                    session, sizeof(water_t), alignof(water_t)));
                if (!water)
                    return false;
                std::memset(water, 0, sizeof(*water));
                water->H0 = static_cast<complex_s *>(RetailZoneLoadSessionAlloc(
                    session, body.m * body.n * sizeof(complex_s), alignof(complex_s)));
                water->wTerm = static_cast<float *>(RetailZoneLoadSessionAlloc(
                    session, body.m * body.n * sizeof(float), alignof(float)));
                XZoneMemory mirror{};
                for (uint32_t b = 0; b < 9; ++b)
                    mirror.blocks[b] = {const_cast<uint8_t *>(FS_RetailFastfileBlockData(reader, b)),
                                        FS_RetailFastfileBlockSize(reader, b)};
                RetailWireBlocks blocks{};
                if (!RetailWireBlocksInit(&blocks, &mirror) ||
                    !RetailWidenWaterFieldsFromWire(body, blocks, water, image))
                {
                    Com_Printf(0, "RetailWidenMaterialFromWire: invalid water body/arrays in '%s'\n", name);
                    return false;
                }
                if (cache)
                {
                    if (cache->waterCount >= 256)
                        return false;
                    cache->waters[cache->waterCount++] = {body.reference, water};
                }
            }
            textureTable[t].u.water = water;
        }
        else if (wireDef.semantic == 11 && wireDef.imageRef)
        {
            Com_Printf(0, "RetailWidenMaterialFromWire: missing water root in '%s'\n", name);
            return false;
        }
        else
        {
            textureTable[t].u.image = image;
        }
    }

    MaterialConstantDef *constantTable = nullptr;
    if (wire.constantCount)
    {
        if (wire.constantTableRef == kInlineRef)
            return false;
        const std::size_t constantBytes = wire.constantCount * sizeof(MaterialConstantDef);
        constantTable = static_cast<MaterialConstantDef *>(
            RetailZoneLoadSessionAlloc(session, constantBytes, alignof(MaterialConstantDef)));
        if (!constantTable)
            return false;
        std::memset(constantTable, 0, constantBytes);
        if (FS_ReadRetailFastfileBlock(reader, 4, wire.constantTableOffset,
                                       reinterpret_cast<uint8_t *>(constantTable),
                                       constantBytes) != FS_RETAIL_FF_WIRE_OK)
            return false;
    }

    GfxStateBits *stateBits = nullptr;
    if (wire.stateBitsCount)
    {
        if (wire.stateBitsTableRef == kInlineRef)
            return false;
        const std::size_t stateBytes = wire.stateBitsCount * sizeof(GfxStateBits);
        stateBits = static_cast<GfxStateBits *>(
            RetailZoneLoadSessionAlloc(session, stateBytes, alignof(GfxStateBits)));
        if (!stateBits)
            return false;
        std::memset(stateBits, 0, stateBytes);
        if (FS_ReadRetailFastfileBlock(reader, 4, wire.stateBitsOffset,
                                       reinterpret_cast<uint8_t *>(stateBits),
                                       stateBytes) != FS_RETAIL_FF_WIRE_OK)
            return false;
    }

    Material *material = static_cast<Material *>(
        RetailZoneLoadSessionAlloc(session, sizeof(Material), alignof(Material)));
    const std::size_t nameBytes = std::strlen(cleanName) + 1;
    char *nameCopy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, nameBytes, 1));
    if (!material || !nameCopy)
        return false;
    std::memset(material, 0, sizeof(*material));
    if (name[0])
    {
        std::memcpy(nameCopy, cleanName, nameBytes);
        material->info.name = nameCopy;
    }
    else
    {
        // Null-named nested body (engine-tolerated under allowNullName):
        // nothing can address it by name, so it stays unregistered below.
        material->info.name = nullptr;
    }
    material->info.gameFlags = wire.gameFlags;
    material->info.sortKey = wire.sortKey;
    material->info.textureAtlasRowCount = wire.textureAtlasRowCount;
    material->info.textureAtlasColumnCount = wire.textureAtlasColumnCount;
    material->info.drawSurf.packed = wire.drawSurf;
    material->info.surfaceTypeBits = wire.surfaceTypeBits;
    material->info.hashIndex = wire.hashIndex;
    std::memcpy(material->stateBitsEntry, wire.stateBitsEntry, sizeof(material->stateBitsEntry));
    material->textureCount = wire.textureCount;
    material->constantCount = wire.constantCount;
    material->stateBitsCount = wire.stateBitsCount;
    material->stateFlags = wire.stateFlags;
    material->cameraRegion = wire.cameraRegion;
    material->techniqueSet = techniqueSet;
    material->textureTable = textureTable;
    material->constantTable = constantTable;
    material->stateBitsTable = stateBits;

    if (!name[0])
    {
        *header = {material};
        return true;
    }
    const XAssetHeader registered = RetailZoneLoadSessionRegister(session, ASSET_TYPE_MATERIAL, {material});
    if (!registered.material)
        return false;
    *header = registered;
    return true;
}

bool RetailWalkLiveLoadMaterial(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader, uint32_t headerRef, XAssetHeader *header,
                                uint32_t directoryOffset, uint32_t directoryBytes,
                                const char *const *stubNames, uint32_t stubCount,
                                RetailWireTechniqueCache *cache, bool strict,
                                bool *deferredAlias, char *deferredAliasName,
                                std::size_t deferredAliasNameCapacity)
{
    RETAIL_ASSET_TRACE(0, "RetailWalkLiveLoadMaterial: start headerRef=%x\n", headerRef);
    if (!session || !session->active || !reader || !header)
        return false;
    *header = {};

    FsRetailFastfileMaterial wire{};
    FsRetailFastfileTextureDef textureDefs[8]{};
    const FsRetailFastfileWireResult wireRes = FS_ReadRetailFastfileMaterial(reader, headerRef, &wire, textureDefs, 8);
    RETAIL_ASSET_TRACE(0, "RetailWalkLiveLoadMaterial: FS_ReadRetailFastfileMaterial returned %d\n", wireRes);
    if (wireRes != FS_RETAIL_FF_WIRE_OK)
        return false;

    return RetailWidenMaterialFromWire(session, reader, wire, textureDefs, directoryOffset, directoryBytes,
                                       stubNames, stubCount, cache, header, strict, false, false,
                                       deferredAlias, deferredAliasName, deferredAliasNameCapacity);
}

bool RetailWireImageCacheResolveDeferred(RetailZoneLoadSession *session,
                                         RetailWireTechniqueCache *cache,
                                         FsRetailFastfileReader *reader,
                                         uint32_t directoryOffset, uint32_t directoryBytes,
                                         const char *const *stubNames, uint32_t stubCount)
{
    if (!cache || !reader)
        return false;
    bool missing = false;
    for (uint32_t i = 0; i < cache->deferredImageCount; ++i)
    {
        RetailWireTechniqueCache::DeferredImageSlot &entry = cache->deferredImages[i];
        if (!entry.table || !entry.reference)
            continue;
        // Pool-shaped references bypass name resolution: the declarer's
        // inline widen already session-bound the image (see the isPoolRef
        // arm of the texture loop above), so bind it directly. This is the
        // same session-pool lookup the directory loop uses, just retried
        // once the pool is whole.
        const uint32_t poolIndex = (entry.reference - 1u) & 0x0fffffffu;
        if (session && (entry.reference >> 28) == 15u && poolIndex < RETAIL_FASTFILE_IMAGE_POOL_MAX &&
            session->imagePool[poolIndex])
        {
            MaterialTextureDef &texture = entry.table[entry.texIndex];
            if (texture.semantic == 11)
                texture.u.water->image = session->imagePool[poolIndex];
            else
                texture.u.image = session->imagePool[poolIndex];
            ++cache->deferredImageResolvedCount;
            continue;
        }
        GfxImage *image = ResolveImageRef(reader, entry.reference, directoryOffset,
                                           directoryBytes, stubNames, stubCount, true);
        if (!image)
        {
            RetailWireToken token{};
            uint32_t target = 0;
            if (DecodeFastfileToken(reader, entry.reference, 0, 1u << 4, &token) &&
                token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4)
                target = token.offset;
            Com_Printf(0, "RetailWireImageCacheResolveDeferred: material '%s' tex %u ref 0x%08x (block-4 offset %u) still missing\n",
                       entry.materialName, entry.texIndex, entry.reference, target);
            missing = true;
            continue;
        }
        MaterialTextureDef &texture = entry.table[entry.texIndex];
        if (texture.semantic == 11)
            texture.u.water->image = image;
        else
            texture.u.image = image;
        ++cache->deferredImageResolvedCount;
    }
    return !missing;
}
