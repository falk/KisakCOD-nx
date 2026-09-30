#include <universal/q_shared.h>
#include "database.h"
#include <game/g_bsp.h>

//int32_t marker_db_assetnames 828ddeec     db_assetnames.obj

static const char *__cdecl DB_StringTableGetName(const XAssetHeader *header);
static const char *__cdecl DB_LocalizeEntryGetName(const XAssetHeader *header);
static const char *__cdecl DB_ImageGetName(const XAssetHeader *header);
static const char *__cdecl DB_MenuListGetName(const XAssetHeader *header);
static const char *__cdecl DB_MenuGetName(const XAssetHeader *header);

const char *(__cdecl *DB_XAssetGetNameHandler[33])(const XAssetHeader *) =
{
    // KISAKTODO: these got Identical COMDAT folded into 1 function because name is usually the 1st field.
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_ImageGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    0,
    DB_StringTableGetName,
    DB_MenuListGetName,
    DB_MenuGetName,
    DB_LocalizeEntryGetName,
    DB_StringTableGetName,
    0,
    DB_StringTableGetName,
    DB_StringTableGetName,
    0,
    0,
    0,
    0,
    DB_StringTableGetName,
    DB_StringTableGetName
};

static void __cdecl DB_StringTableSetName(XAssetHeader *header, const char *name);
static void __cdecl DB_ImageSetName(XAssetHeader *header, const char *name);
static void __cdecl DB_LocalizeEntrySetName(XAssetHeader *header, const char *name);

static const char *__cdecl DB_MenuListGetName(const XAssetHeader *header)
{
    return header && header->menuList ? header->menuList->name : nullptr;
}

static const char *__cdecl DB_MenuGetName(const XAssetHeader *header)
{
    return header && header->menu ? header->menu->window.name : nullptr;
}

void(__cdecl *DB_XAssetSetNameHandler[33])(XAssetHeader *, const char *) =
{
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_ImageSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    0,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_LocalizeEntrySetName,
    DB_StringTableSetName,
    0,
    DB_StringTableSetName,
    DB_StringTableSetName,
    0,
    0,
    0,
    0,
    DB_StringTableSetName,
    DB_StringTableSetName
};

// KISAKTODO: make these non-fixed
// --- file-local forward declarations (moved out of database.h) ---
static void __cdecl DB_StringTableSetName(XAssetHeader *header, const char *name);
static const char*__cdecl DB_ImageGetName(const XAssetHeader *header);
static void __cdecl DB_ImageSetName(XAssetHeader *header, const char *name);
static const char *__cdecl DB_StringTableGetName(const XAssetHeader *header);
static const char *__cdecl DB_LocalizeEntryGetName(const XAssetHeader *header);
static void __cdecl DB_LocalizeEntrySetName(XAssetHeader *header, const char *name);

int32_t __cdecl DB_SizeofXAsset_RawFile_()
{
    return sizeof(RawFile);
}
int32_t __cdecl DB_SizeofXAsset_XModelPieces_()
{
    return sizeof(XModelPieces);
}
int32_t __cdecl DB_SizeofXAsset_PhysPreset_()
{
    return sizeof(PhysPreset);
}
int32_t __cdecl DB_SizeofXAsset_GameWorldSp_()
{
    return sizeof(GameWorldSp);
}
int32_t __cdecl DB_SizeofXAsset_XAnimParts_()
{
    return sizeof(XAnimParts);
}
int32_t __cdecl DB_SizeofXAsset_XModel_()
{
    return sizeof(XModel);
}
int32_t __cdecl DB_SizeofXAsset_Material_()
{
    return sizeof(Material);
}
int32_t __cdecl DB_SizeofXAsset_MaterialTechniqueSet_()
{
    return sizeof(MaterialTechniqueSet);
}
int32_t __cdecl DB_SizeofXAsset_GfxImage_()
{
    return sizeof(GfxImage);
}
int32_t __cdecl DB_SizeofXAsset_snd_alias_list_t_()
{
    return sizeof(snd_alias_list_t);
}
int32_t __cdecl DB_SizeofXAsset_SndCurve_()
{
    return sizeof(SndCurve);
}
int32_t __cdecl DB_SizeofXAsset_LoadedSound_()
{
    return sizeof(LoadedSound);
}
int32_t __cdecl DB_SizeofXAsset_clipMap_t_()
{
    return sizeof(clipMap_t);
}
int32_t __cdecl DB_SizeofXAsset_ComWorld_()
{
    return sizeof(ComWorld);
}
int32_t __cdecl DB_SizeofXAsset_MapEnts_()
{
    return sizeof(MapEnts);
}
int32_t __cdecl DB_SizeofXAsset_menuDef_t_()
{
    return sizeof(menuDef_t);
}
int32_t __cdecl DB_SizeofXAsset_MenuList_()
{
    return sizeof(MenuList);
}
int32_t __cdecl DB_SizeofXAsset_GfxLightDef_()
{
    return sizeof(GfxLightDef);
}
int32_t __cdecl DB_SizeofXAsset_StringTable_()
{
    return sizeof(StringTable);
}
int32_t __cdecl DB_SizeofXAsset_GameWorldMp_()
{
    return sizeof(GameWorldMp);
}
int32_t __cdecl DB_SizeofXAsset_GfxWorld_()
{
    return sizeof(GfxWorld);
}
int32_t __cdecl DB_SizeofXAsset_Font_s_()
{
    return sizeof(Font_s);
}
int32_t __cdecl DB_SizeofXAsset_FxImpactTable_()
{
    return sizeof(FxImpactTable);
}
int32_t __cdecl DB_SizeofXAsset_WeaponDef_()
{
    return sizeof(WeaponDef);
}
int32_t __cdecl DB_SizeofXAsset_FxEffectDef_()
{
    return sizeof(FxEffectDef);
}
int32_t __cdecl DB_SizeofXAsset_LocalizeEntry_()
{
    return sizeof(LocalizeEntry);
}
int(__cdecl *DB_GetXAssetSizeHandler[33])() =
{
    DB_SizeofXAsset_XModelPieces_,
    DB_SizeofXAsset_PhysPreset_,
    DB_SizeofXAsset_XAnimParts_,
    DB_SizeofXAsset_XModel_,
    DB_SizeofXAsset_Material_,
    DB_SizeofXAsset_MaterialTechniqueSet_,
    DB_SizeofXAsset_GfxImage_,
    DB_SizeofXAsset_snd_alias_list_t_,
    DB_SizeofXAsset_SndCurve_,
    DB_SizeofXAsset_LoadedSound_,
    DB_SizeofXAsset_clipMap_t_,
    DB_SizeofXAsset_clipMap_t_,
    DB_SizeofXAsset_ComWorld_,
    DB_SizeofXAsset_GameWorldSp_,
    DB_SizeofXAsset_GameWorldMp_,
    DB_SizeofXAsset_MapEnts_,
    DB_SizeofXAsset_GfxWorld_,
    DB_SizeofXAsset_GfxLightDef_,
    0,
    DB_SizeofXAsset_Font_s_,
    DB_SizeofXAsset_MenuList_,
    DB_SizeofXAsset_menuDef_t_,
    DB_SizeofXAsset_LocalizeEntry_,
    DB_SizeofXAsset_WeaponDef_,
    0,
    DB_SizeofXAsset_FxEffectDef_,
    DB_SizeofXAsset_FxImpactTable_,
    0,
    0,
    0,
    0,
    DB_SizeofXAsset_RawFile_,
    DB_SizeofXAsset_StringTable_,
};

void __cdecl DB_StringTableSetName(XAssetHeader *header, const char *name)
{
    header->xmodelPieces->name = name;
}

const char *__cdecl DB_ImageGetName(const XAssetHeader *header)
{
    return header->image->name;
}

void __cdecl DB_ImageSetName(XAssetHeader *header, const char *name)
{
    //header->xmodelPieces[2].pieces = name;
    //header->xmodelPieces[2].name = name;
    header->image->name = name;
}

const char *__cdecl DB_StringTableGetName(const XAssetHeader *header)
{
    return header->stringTable->name;
}

const char *__cdecl DB_LocalizeEntryGetName(const XAssetHeader *header)
{
    return header->localize->name;
}

void __cdecl DB_LocalizeEntrySetName(XAssetHeader *header, const char *name)
{
    header->localize->name = name;
}

const char *__cdecl DB_GetXAssetHeaderName(int32_t type, const XAssetHeader *header)
{
    const char *name; // [esp+0h] [ebp-4h]

    iassert(header);
    iassert(DB_XAssetGetNameHandler[type]);
    iassert(header->data);

    name = DB_XAssetGetNameHandler[type](header);

    iassert(name);
    //if (!name)
    //{
    //    MyAssertHandler(".\\database\\db_assetnames.cpp", 594, 0, "%s\n\t%s", "name", 
    //      va("Name not found for asset type %s\n", g_assetNames[type]));
    //}
    return name;
}

const char *__cdecl DB_GetXAssetName(const XAsset *asset)
{
    iassert(asset);
    return DB_GetXAssetHeaderName(asset->type, &asset->header);
}

void __cdecl DB_SetXAssetName(XAsset *asset, const char *name)
{
    if (!DB_XAssetSetNameHandler[asset->type])
        MyAssertHandler(".\\database\\db_assetnames.cpp", 608, 0, "%s", "DB_XAssetSetNameHandler[asset->type]");
    DB_XAssetSetNameHandler[asset->type](&asset->header, name);
}

int32_t __cdecl DB_GetXAssetTypeSize(int32_t type)
{
    if (!DB_GetXAssetSizeHandler[type])
        MyAssertHandler(".\\database\\db_assetnames.cpp", 615, 0, "%s", "DB_GetXAssetSizeHandler[type]");
    return DB_GetXAssetSizeHandler[type]();
}

const char *__cdecl DB_GetXAssetTypeName(uint32_t type)
{
    if (type >= ASSET_TYPE_COUNT)
        MyAssertHandler(".\\database\\db_assetnames.cpp", 621, 0, "%s", "type >= 0 && type < ASSET_TYPE_COUNT");
    return g_assetNames[type];
}
