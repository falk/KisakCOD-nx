#include <universal/q_shared.h>
#include "com_bsp.h"
#include <win32/win_local.h>

ComWorld comWorld;

char __cdecl Com_CanPrimaryLightAffectPoint(const ComPrimaryLight *light, const float *point)
{
    float v3; // [esp+Ch] [ebp-24h]
    float deltaToLight[3]; // [esp+18h] [ebp-18h] BYREF
    float cosHalfFov; // [esp+24h] [ebp-Ch]
    float spotDotTimesDist; // [esp+28h] [ebp-8h]
    float distSq; // [esp+2Ch] [ebp-4h]

    iassert( light );
    if (light->type != GFX_LIGHT_TYPE_SPOT && light->type != GFX_LIGHT_TYPE_OMNI)
        MyAssertHandler(
            ".\\qcommon\\com_bsp.cpp",
            25,
            0,
            "%s\n\t(light->type) = %i",
            "(light->type == GFX_LIGHT_TYPE_SPOT || light->type == GFX_LIGHT_TYPE_OMNI)",
            light->type);
    iassert( point );
    Vec3Sub(light->origin, point, deltaToLight);
    distSq = Vec3LengthSq(deltaToLight);
    v3 = light->radius * light->radius;
    if (distSq >= (double)v3)
        return 0;
    if (light->type == GFX_LIGHT_TYPE_OMNI || light->rotationLimit <= -light->cosHalfFovOuter)
        return 1;
    spotDotTimesDist = Vec3Dot(deltaToLight, light->dir);
    if (light->rotationLimit == 1.0)
    {
        cosHalfFov = light->cosHalfFovOuter;
    }
    else
    {
        cosHalfFov = CosOfSumOfArcCos(light->cosHalfFovOuter, light->rotationLimit);
        if (cosHalfFov <= 0.0)
            return spotDotTimesDist <= cosHalfFov * light->radius;
    }
    return spotDotTimesDist > 0.0 && cosHalfFov * cosHalfFov * distSq <= spotDotTimesDist * spotDotTimesDist;
}

double __cdecl CosOfSumOfArcCos(float cos0, float cos1)
{
    float v4; // [esp+4h] [ebp-18h]
    float v5; // [esp+10h] [ebp-Ch]
    float sinSq1; // [esp+14h] [ebp-8h]
    float sinSq0; // [esp+18h] [ebp-4h]

    sinSq0 = 1.0 - cos0 * cos0;
    sinSq1 = 1.0 - cos1 * cos1;
    v5 = sinSq1 * sinSq0;
    v4 = sqrt(v5);
    return (float)(cos0 * cos1 - v4);
}

void __cdecl Com_UnloadWorld()
{
    iassert( IsFastFileLoad() );
    if (comWorld.isInUse)
        Sys_Error("Cannot unload world while it is in use");
}

uint32_t Com_FindClosestPrimaryLight(const float *origin)
{
    uint32_t result; // r3
    double v3; // fp0
    uint32_t v4; // r11
    const float *v21; // r10
    double v22; // fp13
    double v23; // fp11
    double v24; // fp13

    iassert( comWorld.isInUse );
    result = 0;
    v3 = FLT_MAX;
    // LP64: index comWorld.primaryLights.  The decompiled loop (unrolled x4)
    // stepped a float * at origin[2] by 68 / 17 floats (68 bytes = ILP32
    // sizeof(ComPrimaryLight), LP64 72), so past the first light it compared
    // drifting non-origin floats and picked the wrong primary light.
    for (v4 = 2; v4 < comWorld.primaryLightCount; ++v4)
    {
        v21 = &comWorld.primaryLights[v4].origin[2];
        v22 = (float)(*(v21 - 2) - *origin);
        v23 = (float)(*(v21 - 1) - origin[1]);
        v24 = (float)((float)((float)v23 * (float)v23)
            + (float)((float)((float)v22 * (float)v22)
                + (float)((float)(*v21 - origin[2]) * (float)(*v21 - origin[2]))));
        if (v24 < v3)
        {
            v3 = v24;
            result = v4;
        }
    }
    return result;
}