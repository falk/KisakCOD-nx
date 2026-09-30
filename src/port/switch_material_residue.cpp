// Five Material_* functions that r_material_load_obj.cpp used to provide
// alongside its excluded D3DX text-compiler fallback (see the exclusion
// note in scripts/sp/CMakeLists.txt), but which real production code calls
// unconditionally, not only from that dead IsFastFileLoad()==false path:
// Material_GetInfo (EffectsCore/fx_convert.cpp), Material_Sort
// (r_bsp_load_obj.cpp/r_rendercmds.cpp/cl_demo.cpp), and Material_Duplicate
// (Material_MakeDefault's real "material not found" fallback, reachable any
// time an authored menu/material name fails to resolve).
#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_init.h>
#include <database/database.h>

#ifdef __SWITCH__
#include <cstring>

// Verbatim port: no D3DX/Windows dependency. Uses Material_FromHandle (as the
// reference did) so a null/empty-name handle resolves to rgp.defaultMaterial
// instead of dereferencing a bad pointer into a bogus MaterialInfo -- the
// same silent-wrong-material class the Material_Sort stub caused. The
// reference resolver is hardened here too (r_material.cpp:905) precisely for
// this.
void __cdecl Material_GetInfo(Material *handle, MaterialInfo *matInfo)
{
    iassert(handle);
    iassert(matInfo);
    *matInfo = Material_FromHandle(handle)->info;
}

// Material_Compare (the real draw-order sort key comparator) lives in the
// excluded file and pulls in the full technique/pass comparison machinery
// this port has not carried over.  Sort order is a draw-call-batching
// optimization, not a correctness requirement: leaving materials in
// registration order still renders correctly, just without that batching.
//
// What is NOT optional is Material_SortInternal's second half: it re-stamps
// each material's info.drawSurf (primarySortKey, prepass, customIndex, and
// above all materialSortedIndex = its index in rgp.sortedMaterials). R_SetMaterial
// (r_draw_material.cpp) resolves the bound material as
// rgp.sortedMaterials[drawSurf.fields.materialSortedIndex], and r_drawsurf /
// r_scene assert rgp.sortedMaterials[mat->info.drawSurf.fields.materialSortedIndex]
// == mat everywhere. A fastfile stores only the link-time/placeholder index,
// so skipping this left nearly every BSP surface binding the wrong material:
// wrong techniqueSet (Material_GetTechnique returns null -> surface silently
// skipped, looking like a missing/transparent wall), wrong stateBitsEntry
// (blend/cull/alpha), and wrong textures. The old stub did exactly that.
uint32_t MakeMaterialPrimarySortKey(const Material *material)
{
    iassert(material);
    return material->info.sortKey;
}

uint32_t MakeMaterialPrepassSortKey(const Material *material)
{
    const MaterialTechniqueSet *techSet = material->techniqueSet;
    if (!techSet)
        return 3;
    const MaterialTechnique *prepassTech = techSet->techniques[0];
    if (prepassTech)
    {
        if ((material->stateFlags & 4) != 0)
            return 3;
        return (prepassTech->flags & 4) == 0;
    }
    if (techSet->techniques[1])
        return 2;
    return 3;
}

void __cdecl Material_SortInternal(Material **sortedMaterials, uint32_t materialCount)
{
    for (uint32_t sortedIndex = 0; sortedIndex < materialCount; ++sortedIndex)
    {
        Material *material = sortedMaterials[sortedIndex];
        material->info.drawSurf.packed = 0;
        material->info.drawSurf.fields.primarySortKey = MakeMaterialPrimarySortKey(material);
        material->info.drawSurf.fields.prepass = MakeMaterialPrepassSortKey(material);
        material->info.drawSurf.fields.customIndex = (material->info.gameFlags & 0x40) != 0;
        material->info.drawSurf.fields.materialSortedIndex = sortedIndex;
    }
}

void __cdecl Material_Sort()
{
    if (IsFastFileLoad())
        rgp.materialCount = DB_GetAllXAssetOfType(ASSET_TYPE_MATERIAL, (XAssetHeader *)&rgp, 2048);
    Material_SortInternal(rgp.sortedMaterials, rgp.materialCount);
}

// Rewritten (not ported verbatim) from raw `_DWORD`-offset field pokes that
// assumed the original 32-bit Material layout: Material's pointer fields
// (MaterialInfo::name, stateBitsTable, textureTable, constantTable) are
// real 8-byte pointers on LP64, so this uses their actual struct field
// names/sizeof(Material) instead of hardcoded byte offsets built for a
// 4-byte-pointer struct.
Material *__cdecl Material_Duplicate(Material *mtlCopy, char *name)
{
    iassert(mtlCopy);
    iassert(name);

    uint16_t hashIndex[3];
    bool exists;
    Material_GetHashIndex(name, hashIndex, &exists);
    if (exists)
    {
        Material *mtlNewa = rg.materialHashTable[hashIndex[0]];
        const char *nameBackup = mtlNewa->info.name;
        memcpy(mtlNewa, mtlCopy, sizeof(Material));
        mtlNewa->info.name = nameBackup;
        rgp.needSortMaterials = 1;
        return mtlNewa;
    }

    const uint32_t nameLen = (uint32_t)strlen(name);
    Material *mtlNew = (Material *)Material_Alloc((uint32_t)sizeof(Material) + nameLen + 1);
    memcpy(mtlNew, mtlCopy, sizeof(Material));
    char *nameStorage = (char *)mtlNew + sizeof(Material);
    memcpy(nameStorage, name, nameLen + 1);
    mtlNew->info.name = nameStorage;

    const uint32_t stateBitsTableSize = sizeof(GfxStateBits) * mtlCopy->stateBitsCount;
    mtlNew->stateBitsTable = (GfxStateBits *)Material_Alloc(stateBitsTableSize);
    memcpy(mtlNew->stateBitsTable, mtlCopy->stateBitsTable, stateBitsTableSize);
    if (mtlCopy->textureTable)
    {
        const uint32_t textureTableSize = sizeof(MaterialTextureDef) * mtlCopy->textureCount;
        mtlNew->textureTable = (MaterialTextureDef *)Material_Alloc(textureTableSize);
        memcpy(mtlNew->textureTable, mtlCopy->textureTable, textureTableSize);
    }
    if (mtlCopy->constantTable)
    {
        const uint32_t constantTableSize = sizeof(MaterialConstantDef) * mtlCopy->constantCount;
        mtlNew->constantTable = (MaterialConstantDef *)Material_Alloc(constantTableSize);
        memcpy(mtlNew->constantTable, mtlCopy->constantTable, constantTableSize);
    }
    Material_Add(mtlNew, hashIndex[0]);
    return mtlNew;
}

// Material_Load reads a loose .mtl text file from disk (Material_LoadFile),
// the same non-fastfile authoring path as the excluded D3DX compiler.  No
// such loose files exist in a Switch retail deployment (only fastfiles/
// IWDs), so "file not found" is the true answer, not a stub standing in for
// missing behavior; Material_Register's caller already falls through to
// Material_MakeDefault when this returns null, exactly as it does on PC
// when a loose file is absent.
Material *__cdecl Material_Load(char *assetName, int imageTrack)
{
    (void)assetName;
    (void)imageTrack;
    return nullptr;
}

// Material_FindTechniqueSet only calls this when IsFastFileLoad() is false
// (see r_material.cpp), which a retail-fastfile-only build never is; the
// symbol must still exist to link Material_FindTechniqueSet's other branch.
MaterialTechniqueSet *__cdecl Material_FindTechniqueSet_LoadObj(
    const char *name,
    MtlTechSetNotFoundBehavior notFoundBehavior)
{
    (void)name;
    (void)notFoundBehavior;
    return nullptr;
}

// Three more real, needed functions from the same excluded file, found only
// once a real Com_Init (switch_sp_main.cpp) actually exercised the BSP-load
// and zone-unload paths that call them.  None depend on D3DX/Windows.

// Verbatim port: converts a BSP's raw on-disk material name to a real,
// registered Material during world/map loading (r_bsp_load_obj.cpp).
Material *__cdecl R_GetBspMaterial(uint32_t materialIndex)
{
    if (materialIndex >= 0x4C8)
        MyAssertHandler(".\\r_bsp_load_obj.cpp", 226, 0,
                        "materialIndex doesn't index MAX_MAP_MATERIALS\n\t%i not in [0, %i)",
                        materialIndex, 1224);
    const dmaterial_t *name = &rgl.load.diskMaterials[materialIndex];
    if (!strcmp(name->material, "noshader"))
        MyAssertHandler(".\\r_bsp_load_obj.cpp", 230, 0, "%s", "strcmp( name, \"noshader\" )");
    if (!strcmp(name->material, "$default"))
        strcpy(const_cast<char *>(&name->material[0]), "$default3d");
    char materialName[260];
    if (name->material[0] == 42)
        Com_sprintf(materialName, 0x100u, "%s%s", "", name->material);
    else
        Com_sprintf(materialName, 0x100u, "%s%s", "wc/", name->material);
    return Material_Register(materialName, 9);
}

// Material_PreLoadAllShaderText's real body is entirely
// `#ifdef KISAK_NO_FASTFILES` (loading shader source text from
// shader_bin/shader_names for the non-fastfile dev path); it is a true
// no-op on this fastfile-only, __SWITCH__ build.
void __cdecl Material_PreLoadAllShaderText()
{}

// Material_FreeAllLiterals/Strings/StateMaps' real bodies only reset
// mtlLoadGlob (the excluded file's own non-fastfile text-compiler state,
// never populated with real data on Switch since nothing else writes to
// it), so they are true no-ops here; Material_FreeAllTechniqueSets does
// real, needed work (releasing every registered TechniqueSet's runtime
// resources, e.g. on zone unload) regardless of fastfile mode, so it is a
// verbatim port, and Material_FreeAll's own `if (!IsFastFileLoad())` tail
// (further mtlLoadGlob resets) is dead code on this always-fastfile build
// and is not ported.
void __cdecl Material_FreeAllTechniqueSets()
{
    DB_EnumXAssets(ASSET_TYPE_TECHNIQUE_SET, Material_ReleaseTechniqueSet, 0, 1);
}

void __cdecl Material_FreeAll()
{
    Material_FreeAllTechniqueSets();
}

#endif
