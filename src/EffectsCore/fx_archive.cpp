#include <universal/q_shared.h>
#include "fx_system.h"

#include <database/database.h>

#include <physics/phys_local.h>

// Rebase the vis-state double buffer pointers of a restored FxSystem from
// the saving process's addresses to the live FxSystemBuffers.  The saved
// pointers must each name visState[0] or visState[1] of the saved base and
// differ; anything else is a corrupt record.
bool __cdecl FX_RebaseVisStateBuffers(FxSystem *system, uintptr_t savedVisState, uintptr_t savedRead,
                                      uintptr_t savedWrite)
{
    const uintptr_t stride = sizeof(FxVisState);
    const uintptr_t readOffset = savedRead - savedVisState;
    const uintptr_t writeOffset = savedWrite - savedVisState;
    if (!savedVisState || !system->visState)
        return false;
    if ((readOffset != 0 && readOffset != stride) || (writeOffset != 0 && writeOffset != stride) ||
        readOffset == writeOffset)
        return false;
    system->visStateBufferRead = system->visState + readOffset / stride;
    system->visStateBufferWrite = system->visState + writeOffset / stride;
    return true;
}

void __cdecl FX_Restore(int32_t clientIndex, MemoryFile *memFile)
{
    int32_t v2; // [esp+0h] [ebp-201Ch] BYREF
    FxEffectDefTable table; // [esp+4h] [ebp-2018h] BYREF
    void *p; // [esp+2014h] [ebp-8h]
    FxSystemBuffers *systemBuffers; // [esp+2018h] [ebp-4h]

    p = FX_GetSystem(clientIndex);
    if (!p)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 220, 0, "%s", "system");
    systemBuffers = FX_GetSystemBuffers(clientIndex);
    if (!systemBuffers)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 223, 0, "%s", "systemBuffers");
    FX_RestoreEffectDefTable(memFile, &table);
    // Native sizes, not the decompiler's ILP32 literals (0xA60 FxSystem,
    // 0x47480 FxSystemBuffers): both are full of pointers on LP64, so the
    // literal forms truncate the restore and leave isArchiving set.
    FxSystem *system = (FxSystem *)p;
    MemFile_ReadData(memFile, sizeof(FxSystem), (uint8_t *)system);
    if (!system->isArchiving || system->iteratorCount)
        Com_Error(ERR_DROP, "Invalid save file");
    // The record holds the saving process's absolute visState pointers.
    // Capture them before FX_LinkSystemBuffers overwrites visState: the
    // double buffers are rebased by index below.
    const uintptr_t savedVisState = (uintptr_t)system->visState;
    const uintptr_t savedVisStateRead = (uintptr_t)system->visStateBufferRead;
    const uintptr_t savedVisStateWrite = (uintptr_t)system->visStateBufferWrite;
    FX_LinkSystemBuffers(system, systemBuffers);
    MemFile_ReadData(memFile, sizeof(FxSystemBuffers), (uint8_t *)systemBuffers);
    FX_FixupEffectDefHandles(system, &table);
    // Legacy 32-bit system-address token (FX_Save writes the low half of
    // the FxSystem pointer). The retail relocation added `system - token`
    // to the saved visState pointers, which on LP64 is off by the whole
    // high half once the image loads above 4 GiB. Keep reading the token
    // (save format), but rebase from the full saved pointers instead.
    MemFile_ReadData(memFile, 4, (uint8_t *)&v2);
    if (!FX_RebaseVisStateBuffers(system, savedVisState, savedVisStateRead, savedVisStateWrite))
        Com_Error(ERR_DROP, "Invalid save file (FX vis state buffers)");
    // Evidence for offline tooling: one line per FX restore.
    Com_Printf(CON_CHANNEL_FX, "FX_RESTORE ok=1 read=%d write=%d effects=%d elems=%ld system=%p\n",
               (int)(system->visStateBufferRead - system->visState),
               (int)(system->visStateBufferWrite - system->visState),
               (int)(system->firstNewEffect - system->firstActiveEffect), (long)system->activeElemCount,
               (void *)system);
    FX_RestorePhysicsData(system, memFile);
    system->isArchiving = 0;
}

void __cdecl FX_RestoreEffectDefTable(MemoryFile *memFile, FxEffectDefTable *table)
{
    uint32_t p; // [esp+0h] [ebp-10h] BYREF
    const FxEffectDef *effectDef; // [esp+4h] [ebp-Ch]
    uint32_t key; // [esp+8h] [ebp-8h]
    const char *effectDefName; // [esp+Ch] [ebp-4h]

    table->count = 0;
    while (1)
    {
        effectDefName = MemFile_ReadCString(memFile);
        if (!*effectDefName)
            break;
        MemFile_ReadData(memFile, 4, (uint8_t *)&p);
        key = p;
        effectDef = FX_Register((char *)effectDefName);
        FX_AddEffectDefTableEntry(table, key, effectDef);
    }
}

void __cdecl FX_AddEffectDefTableEntry(FxEffectDefTable *table, uint32_t key, const FxEffectDef *effectDef)
{
    if (!table)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 47, 0, "%s", "table");
    if (table->count >= 0x400u)
        MyAssertHandler(
            ".\\EffectsCore\\fx_archive.cpp",
            48,
            0,
            "table->count doesn't index ARRAY_COUNT( table->entries )\n\t%i not in [0, %i)",
            table->count,
            1024);
    if (!effectDef)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 49, 0, "%s", "effectDef");
    table->entries[table->count].key = key;
    table->entries[table->count++].effectDef = effectDef;
}

void __cdecl FX_FixupEffectDefHandles(FxSystem *system, FxEffectDefTable *table)
{
    const FxEffectDef *effectDef; // [esp+Ch] [ebp-10h]
    FxEffect *effect; // [esp+10h] [ebp-Ch]
    volatile int32_t activeIndex; // [esp+18h] [ebp-4h]

    if (!system)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 131, 0, "%s", "system");
    if (!system->isArchiving)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 132, 0, "%s", "system->isArchiving");
    for (activeIndex = system->firstActiveEffect; activeIndex != system->firstNewEffect; ++activeIndex)
    {
        effect = FX_EffectFromHandle(system, system->allEffectHandles[activeIndex & 0x3FF]);
        // Save files identify definitions by the original 32-bit pointer
        // token.  Make the narrowing explicit through uintptr_t on LP64;
        // the restored table still resolves the token by name.
        effectDef = FX_FindEffectDefInTable(table, (uint32_t)(uintptr_t)effect->def);
        if (!effectDef)
            MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 139, 0, "%s", "effectDef");
        effect->def = effectDef;
    }
}

FxEffect *__cdecl FX_EffectFromHandle(FxSystem *system, uint16_t handle)
{
    const char *v2; // eax

    if (!system)
        MyAssertHandler("c:\\trees\\cod3\\src\\effectscore\\fx_system.h", 256, 0, "%s", "system");
    if (handle >= FX_EFFECT_LIMIT * sizeof(FxEffect) / FxEffect::HANDLE_SCALE
        || handle % (sizeof(FxEffect) / FxEffect::HANDLE_SCALE))
    {
        v2 = va("%p %i", system->effects, handle);
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\effectscore\\fx_system.h",
            257,
            0,
            "%s\n\t%s",
            "handle < FX_EFFECT_LIMIT * sizeof( FxEffect ) / FxEffect::HANDLE_SCALE && handle % (sizeof( FxEffect ) / FxEffect:"
            ":HANDLE_SCALE) == 0",
            v2);
    }
    return (FxEffect *)((char *)system->effects + FxEffect::HANDLE_SCALE * handle);
}

const FxEffectDef *__cdecl FX_FindEffectDefInTable(const FxEffectDefTable *table, uint32_t key)
{
    int32_t index; // [esp+0h] [ebp-4h]

    for (index = 0; index < table->count; ++index)
    {
        if (table->entries[index].key == key)
            return table->entries[index].effectDef;
    }
    return 0;
}

void __cdecl FX_RestorePhysicsData(FxSystem *system, MemoryFile *memFile)
{
    const XModel *visuals; // [esp+18h] [ebp-20h]
    uint16_t elemHandle; // [esp+1Ch] [ebp-1Ch]
    const FxElemDef *elemDef; // [esp+20h] [ebp-18h]
    const FxEffect *effect; // [esp+24h] [ebp-14h]
    uint16_t elemHandleNext; // [esp+2Ch] [ebp-Ch]
    FxPool<FxElem> *elem; // [esp+30h] [ebp-8h]
    volatile int32_t activeIndex; // [esp+34h] [ebp-4h]

    if (!system)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 185, 0, "%s", "system");
    if (!system->isArchiving)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 186, 0, "%s", "system->isArchiving");
    for (activeIndex = system->firstActiveEffect; activeIndex != system->firstNewEffect; ++activeIndex)
    {
        effect = FX_EffectFromHandle(system, system->allEffectHandles[activeIndex & 0x3FF]);
        for (elemHandle = effect->firstElemHandle[1]; elemHandle != 0xFFFF; elemHandle = elemHandleNext)
        {
            if (!system)
                MyAssertHandler("c:\\trees\\cod3\\src\\effectscore\\fx_system.h", 334, 0, "%s", "system");
            elem = FX_PoolFromHandle_Generic<FxElem, 2048>(system->elems, elemHandle);
            elemDef = &effect->def->elemDefs[elem->item.defIndex];
            elemHandleNext = elem->item.nextElemHandleInEffect;
            if (elemDef->elemType == 5 && (elemDef->flags & 0x8000000) != 0)
            {
                elem->item.physObjId = (uintptr_t)Phys_ObjLoad(PHYS_WORLD_FX, memFile);
                visuals = FX_GetElemVisuals(
                    elemDef,
                    (296 * elem->item.sequence + elem->item.msecBegin + (uint32_t)effect->randomSeed) % 0x1DF).model;
                Phys_ObjSetCollisionFromXModel(visuals, PHYS_WORLD_FX, (dxBody *)elem->item.physObjId);
            }
        }
    }
}

FxElemVisuals __cdecl FX_GetElemVisuals(const FxElemDef *elemDef, int32_t randomSeed)
{
    if (!elemDef->visualCount)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\effectscore\\fx_draw.h",
            79,
            0,
            "%s\n\t(elemDef->visualCount) = %i",
            "(elemDef->visualCount > 0)",
            elemDef->visualCount);
    if (elemDef->visualCount == 1)
        return elemDef->visuals.instance;
    else
        return (FxElemVisuals)elemDef->visuals.markArray->materials[(elemDef->visualCount
            * LOWORD(fx_randomTable[randomSeed + 21])) >> 16];
}

void __cdecl FX_Save(int32_t clientIndex, MemoryFile *memFile)
{
    uint32_t UsedSize; // eax
    uint32_t v3; // eax
    FxSystem *p; // [esp+0h] [ebp-Ch] BYREF
    FxSystem *system; // [esp+4h] [ebp-8h]
    FxSystemBuffers *systemBuffers; // [esp+8h] [ebp-4h]

    system = FX_GetSystem(clientIndex);
    if (!system)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 265, 0, "%s", "system");
    systemBuffers = FX_GetSystemBuffers(clientIndex);
    if (!systemBuffers)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 267, 0, "%s", "systemBuffers");
    if (system->isArchiving)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 270, 0, "%s", "!system->isArchiving");
    system->isArchiving = 1;
    FX_SaveEffectDefTable(system, memFile);
    // sizeof, matching FX_Restore: the ILP32 literals (2656 / 291968) are the
    // 32-bit struct sizes and truncate both records on LP64.
    MemFile_WriteData(memFile, sizeof(FxSystem), system);
    UsedSize = MemFile_GetUsedSize(memFile);
    // ProfMem_Begin("systemBuffers", UsedSize);
    MemFile_WriteData(memFile, sizeof(FxSystemBuffers), systemBuffers);
    v3 = MemFile_GetUsedSize(memFile);
    // ProfMem_End(v3);
    p = system;
    MemFile_WriteData(memFile, 4, &p);
    FX_SavePhysicsData(system, memFile);
    system->isArchiving = 0;
}

void __cdecl FX_SaveEffectDefTable(FxSystem *system, MemoryFile *memFile)
{
    if (IsFastFileLoad())
        FX_SaveEffectDefTable_FastFile(memFile);
    else
        FX_SaveEffectDefTable_LoadObj(memFile);
    MemFile_WriteCString(memFile, "");
}

void __cdecl FX_SaveEffectDefTableEntry_FileLoadObj(const FxEffectDef* effectDef, MemoryFile* data)
{
    uint32_t key; // serialized 32-bit definition token

    MemFile_WriteCString(data, (char*)effectDef->name);
    key = (uint32_t)(uintptr_t)effectDef;
    MemFile_WriteData(data, sizeof(key), &key);
}

void __cdecl FX_SaveEffectDefTable_LoadObj(MemoryFile* memFile)
{
    FX_ForEachEffectDef((void(__cdecl*)(const FxEffectDef*, void*))FX_SaveEffectDefTableEntry_FileLoadObj, memFile);
}

void __cdecl FX_SaveEffectDefTable_FastFile(MemoryFile *memFile)
{
    DB_EnumXAssets(
        ASSET_TYPE_FX,
        (void(__cdecl *)(XAssetHeader, void *))FX_SaveEffectDefTableEntry_FileLoadObj,
        memFile,
        0);
}

void __cdecl FX_SavePhysicsData(FxSystem *system, MemoryFile *memFile)
{
    uint16_t elemHandle; // [esp+Ch] [ebp-18h]
    const FxElemDef *elemDef; // [esp+10h] [ebp-14h]
    const FxEffect *effect; // [esp+14h] [ebp-10h]
    uint16_t elemHandleNext; // [esp+18h] [ebp-Ch]
    FxPool<FxElem> *elem; // [esp+1Ch] [ebp-8h]
    volatile int32_t activeIndex; // [esp+20h] [ebp-4h]

    if (!system)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 155, 0, "%s", "system");
    if (!system->isArchiving)
        MyAssertHandler(".\\EffectsCore\\fx_archive.cpp", 156, 0, "%s", "system->isArchiving");
    for (activeIndex = system->firstActiveEffect; activeIndex != system->firstNewEffect; ++activeIndex)
    {
        effect = FX_EffectFromHandle(system, system->allEffectHandles[activeIndex & 0x3FF]);
        for (elemHandle = effect->firstElemHandle[1]; elemHandle != 0xFFFF; elemHandle = elemHandleNext)
        {
            if (!system)
                MyAssertHandler("c:\\trees\\cod3\\src\\effectscore\\fx_system.h", 334, 0, "%s", "system");
            elem = FX_PoolFromHandle_Generic<FxElem, 2048>(system->elems, elemHandle);
            elemDef = &effect->def->elemDefs[elem->item.defIndex];
            elemHandleNext = elem->item.nextElemHandleInEffect;
            if (elemDef->elemType == 5 && (elemDef->flags & 0x8000000) != 0)
                Phys_ObjSave((dxBody *)elem->item.physObjId, memFile);
        }
    }
}

void __cdecl FX_Archive(int32_t clientIndex, MemoryFile *memFile)
{
    if (MemFile_IsWriting(memFile))
        FX_Save(clientIndex, memFile);
    else
        FX_Restore(clientIndex, memFile);
}
