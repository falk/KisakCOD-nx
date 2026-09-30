#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include <universal/q_shared.h>
#include "g_scr_load_obj.h"
#include "g_scr_main.h"
#include "g_local.h"
#include "g_main.h"
#include <script/scr_main.h>
#include <xanim/xanim.h>
#include <universal/profile.h>
#include <game/g_bsp.h>

int __cdecl GScr_LoadScriptAndLabel(const char *filename, const char *label, ScriptFunctions *functions)
{
    int FunctionHandle; // r3

    if (G_ExitAfterConnectPaths())
        return 1;

    if (functions->count >= functions->maxSize)
    {
        Com_PrintError(CON_CHANNEL_SERVER, "CODE ERROR: GScr_LoadScriptAndLabel: functions->maxSize exceeded\n");
        return 0;
    }

    // TEMP script-execution scoping note: per-file begin/compiled prints
    // lived here while locating a silent guest death (now root-caused to
    // debugger script-list widths); reverted as log spam. Phase markers
    // (entities/begin/end) in the probe stay.
    if (!Scr_LoadScript(filename))
    {
        functions->address[functions->count++] = 0;
        Com_Printf(CON_CHANNEL_SERVER, "Could not find script '%s'\n", filename);
        return 0;
    }

    FunctionHandle = Scr_GetFunctionHandle(filename, label);
    functions->address[functions->count++] = FunctionHandle;

    if (FunctionHandle)
        return 1;

    Com_Printf(CON_CHANNEL_SERVER, "Could not find label '%s' in script '%s'\n", label, filename);
    return 0;
}

void __cdecl GScr_LoadSingleAnimScript(const char *name, ScriptFunctions *functions)
{
    char filename[72]; // [sp+50h] [-60h] BYREF

    iassert(name);

    Com_sprintf(filename, 64, "animscripts/%s", name);
    GScr_LoadScriptAndLabel(filename, "main", functions);
}

void __cdecl GScr_LoadAnimScripts(ScriptFunctions *functions)
{
    GScr_LoadSingleAnimScript("combat", functions);
    GScr_LoadSingleAnimScript("concealment_crouch", functions);
    GScr_LoadSingleAnimScript("concealment_prone", functions);
    GScr_LoadSingleAnimScript("concealment_stand", functions);
    GScr_LoadSingleAnimScript("cover_arrival", functions);
    GScr_LoadSingleAnimScript("cover_crouch", functions);
    GScr_LoadSingleAnimScript("cover_left", functions);
    GScr_LoadSingleAnimScript("cover_prone", functions);
    GScr_LoadSingleAnimScript("cover_right", functions);
    GScr_LoadSingleAnimScript("cover_stand", functions);
    GScr_LoadSingleAnimScript("cover_wide_left", functions);
    GScr_LoadSingleAnimScript("cover_wide_right", functions);
    GScr_LoadSingleAnimScript("death", functions);
    GScr_LoadSingleAnimScript("grenade_return_throw", functions);
    GScr_LoadSingleAnimScript("init", functions);
    GScr_LoadSingleAnimScript("pain", functions);
    GScr_LoadSingleAnimScript("move", functions);
    GScr_LoadSingleAnimScript("scripted", functions);
    GScr_LoadSingleAnimScript("stop", functions);
    GScr_LoadSingleAnimScript("grenade_cower", functions);
    GScr_LoadSingleAnimScript("flashed", functions);
    GScr_LoadScriptAndLabel("animscripts/scripted", "init", functions);
}

void __cdecl GScr_LoadDogAnimScripts(ScriptFunctions *functions)
{
    GScr_LoadSingleAnimScript("dog_combat", functions);
    GScr_LoadSingleAnimScript("dog_death", functions);
    GScr_LoadSingleAnimScript("dog_init", functions);
    GScr_LoadSingleAnimScript("dog_pain", functions);
    GScr_LoadSingleAnimScript("dog_move", functions);
    GScr_LoadSingleAnimScript("dog_scripted", functions);
    GScr_LoadSingleAnimScript("dog_stop", functions);
    GScr_LoadSingleAnimScript("dog_flashed", functions);
}

void __cdecl GScr_LoadLevelScript(const char *mapname, ScriptFunctions *functions)
{
    char filename[64]; // [sp+50h] [-50h] BYREF

    Com_sprintf(filename, 64, "maps/%s", mapname);
    GScr_LoadScriptAndLabel(filename, "main", functions);
}

struct PathNodeScriptLoadContext
{
    ScriptFunctions *functions;
    uint32_t stringSet;
    uint32_t nodesSeen;
    uint32_t beginNodes;
    uint32_t beginWithAnim;
    uint32_t added;
};

void __cdecl GScr_LoadScriptsForPathNode(pathnode_t *loadNode, void *data)
{
    PathNodeScriptLoadContext *context; // r28
    const char *animscript; // r31
    char filename[112]; // [sp+50h] [-70h] BYREF

    iassert(data);

    context = (PathNodeScriptLoadContext *)data;
    ++context->nodesSeen;

    if (loadNode->constant.type)
    {
        if (loadNode->constant.type == NODE_NEGOTIATION_BEGIN)
        {
            ++context->beginNodes;
            if (loadNode->constant.animscript)
            {
                ++context->beginWithAnim;
                animscript = SL_ConvertToString(loadNode->constant.animscript);

                iassert(animscript);

                if (Scr_AddStringSet(context->stringSet, animscript))
                {
                    ++context->added;
                    Com_sprintf(filename, 64, "animscripts/traverse/%s", animscript);
                    
                    GScr_LoadScriptAndLabel(filename, "main", context->functions);
                }
            }
            else
            {
                Com_PrintError(
                    CON_CHANNEL_ERROR,
                    "ERROR: Pathnode (Begin) at (%g %g %g) has no animscript specified\n",
                    loadNode->constant.vOrigin[0],
                    loadNode->constant.vOrigin[1],
                    loadNode->constant.vOrigin[2]
                );
                loadNode->constant.type = NODE_BADNODE;
            }
        }
        else
        {
            iassert(!loadNode->constant.animscript);
        }
    }
}

void __cdecl GScr_LoadScriptsForPathNodes(ScriptFunctions *functions)
{
    const uint32_t stringSet = Scr_InitStringSet();
    PathNodeScriptLoadContext context{functions, stringSet, 0, 0, 0, 0};
    Path_CallFunctionForNodes(GScr_LoadScriptsForPathNode, &context);
    
    Scr_ShutdownStringSet(stringSet);
}

void __cdecl GScr_LoadScriptsForEntities(ScriptFunctions *functions)
{
    int hadDog; // r17
    unsigned int stringset; // r16
    unsigned int animstringset; // r16
    unsigned int weapIdx; // r3
    WeaponDef *WeaponDef; // r3
    const char *szScript; // r3
    const char *classname; // [sp+50h] [-AF0h] BYREF
    const char *weapName; // [sp+54h] [-AECh] BYREF
    char filename[64]; // [sp+60h] [-AE0h] BYREF
    SpawnVar spawnvar; // [sp+A0h] [-AA0h] BYREF

    hadDog = 0;
    if (!G_ParseSpawnVars(&spawnvar))
        Com_Error(ERR_DROP, "GScr_LoadScriptsForEntities: no entities");

    stringset = Scr_InitStringSet();
    animstringset = Scr_InitStringSet();

    while (G_ParseSpawnVars(&spawnvar))
    {
        G_SpawnString(&spawnvar, "classname", "", &classname);

        if (I_strnicmp(classname, "actor_", 6) == 0)
        {
            classname += 6;
            if (Scr_AddStringSet(stringset, classname))
            {
                if (!hadDog && I_stristr(classname, "dog"))
                {
                    hadDog = 1;
                    GScr_LoadDogAnimScripts(functions);
                }
                Com_sprintf(filename, 64, "aitype/%s", classname);
                GScr_LoadScriptAndLabel(filename, "main", functions);
                GScr_LoadScriptAndLabel(filename, "precache", functions);
                GScr_LoadScriptAndLabel(filename, "spawner", functions);
            }
        }
        // LWSS ADD from SP PC bin
        else if (!I_stricmp(classname, "node_negotiation_begin"))
        {
            const char *animscript;
            G_SpawnString(&spawnvar, "animscript", "", &animscript);

            if (animscript[0] && Scr_AddStringSet(animstringset, animscript))
            {
                Com_sprintf(filename, 64, "animscripts/traverse/%s", animscript); // Note the `/traverse/`!
                
                GScr_LoadScriptAndLabel(filename, "main", functions);
            }
        }
        // LWSS END
        else if (!I_stricmp(classname, "misc_mg42") || !I_stricmp(classname, "misc_turret"))
        {
            G_SpawnString(&spawnvar, "weaponinfo", "", &weapName);
            weapIdx = G_GetWeaponIndexForName(weapName);
            if (weapIdx)
            {
                WeaponDef = BG_GetWeaponDef(weapIdx);
                if (WeaponDef->weapClass == WEAPCLASS_TURRET)
                {
                    szScript = WeaponDef->szScript;
                    if (*szScript)
                        GScr_LoadSingleAnimScript(szScript, functions);
                }
                else
                {
                    Com_PrintError(CON_CHANNEL_PARSERSCRIPT, "ERROR: weapClass in weapon info '%s' for %s must be 'turret'\n", weapName, classname);
                }
            }
            else
            {
                Com_PrintError(CON_CHANNEL_PARSERSCRIPT, "ERROR: could not find weapon info '%s' for %s\n", weapName, classname);
            }
        }
    }
    // The compiled path data is the authoritative traverse-animscript set:
    // the entity string can omit node_negotiation_begin spawns that the
    // pathnode array still carries. Load those scripts through the same
    // dedupe set so GScr_SetScripts' pathnode pass (one slot per unique
    // animscript, Hunk-deduped against the entity pass) finds every function
    // address it expects. The load and set passes must stay paired.
    {
        PathNodeScriptLoadContext context{functions, animstringset, 0, 0, 0, 0};
        
        Path_CallFunctionForNodes(GScr_LoadScriptsForPathNode, &context);
        
    }
    Scr_ShutdownStringSet(stringset);
    Scr_ShutdownStringSet(animstringset); // LWSS ADD
    G_ResetEntityParsePoint();
}

void __cdecl GScr_LoadEntities()
{
    const char *v0; // [sp+50h] [-A50h] BYREF
    const char *v1; // [sp+54h] [-A4Ch] BYREF
    SpawnVar v2; // [sp+60h] [-A40h] BYREF

    if (!G_ParseSpawnVars(&v2))
        Com_Error(ERR_DROP, "GScr_LoadEntities: no entities");
    while (G_ParseSpawnVars(&v2))
    {
        G_SpawnString(&v2, "classname", "", &v0);
        if (!I_stricmp(v0, "misc_mg42") || !I_stricmp(v0, "misc_turret"))
        {
            G_SpawnString(&v2, "weaponinfo", "", &v1);
            G_GetWeaponIndexForName(v1);
        }
    }
    G_ResetEntityParsePoint();
}

void __cdecl GScr_LoadScripts(const char *mapname, ScriptFunctions *functions)
{
    char filename[112]; // [sp+60h] [-70h] BYREF

    Scr_BeginLoadScripts();

    {
        PROF_SCOPED("load misc scripts");
        GScr_LoadScriptAndLabel("codescripts/delete", "main", functions);
        GScr_LoadScriptAndLabel("codescripts/struct", "initstructs", functions);
        GScr_LoadScriptAndLabel("codescripts/struct", "createstruct", functions);
    }
    {
        PROF_SCOPED("load anim scripts");
        GScr_LoadAnimScripts(functions);
    }
    {
        PROF_SCOPED("load level script");
        Com_sprintf(filename, 64, "maps/%s", mapname);
        GScr_LoadScriptAndLabel(filename, "main", functions);
    }
    {
        PROF_SCOPED("load entity scripts");
        // Registers node_negotiation_begin entity scripts and the compiled
        // pathnode traverse set together, sharing one dedupe string set.
        GScr_LoadScriptsForEntities(functions);
    }
    Scr_PostCompileScripts();
}
