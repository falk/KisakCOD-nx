#include <universal/q_shared.h>
#include "r_outdoor.h"
#include <universal/assertive.h>
#include "r_image.h"

OutdoorGlob outdoorGlob;

void __cdecl Outdoor_ApplyBoundingBox(const float *outdoorMin, const float *outdoorMax)
{
    int dimIter; // [esp+4h] [ebp-4h]

    outdoorGlob.bbox[0][0] = *outdoorMin;
    outdoorGlob.bbox[0][1] = outdoorMin[1];
    outdoorGlob.bbox[0][2] = outdoorMin[2];
    outdoorGlob.bbox[1][0] = *outdoorMax;
    outdoorGlob.bbox[1][1] = outdoorMax[1];
    outdoorGlob.bbox[1][2] = outdoorMax[2];
    for (dimIter = 0; dimIter != 3; ++dimIter)
    {
        if (outdoorGlob.bbox[0][dimIter] == 131072.0)
        {
            //iassert( outdoorGlob.bbox[SUPREMUM][dimIter] == MIN_WORLD_COORD );
            outdoorGlob.bbox[0][dimIter] = 0.0;
            outdoorGlob.scale[dimIter - 3] = 0.0;
        }
        if (outdoorGlob.bbox[0][dimIter] > outdoorGlob.scale[dimIter - 3])
            MyAssertHandler(
                ".\\r_outdoor.cpp",
                94,
                1,
                "%s",
                "outdoorGlob.bbox[SUPREMUM][dimIter] >= outdoorGlob.bbox[INFIMUM][dimIter]");
        if (outdoorGlob.scale[dimIter - 3] - outdoorGlob.bbox[0][dimIter] < 1.0)
        {
            outdoorGlob.bbox[0][dimIter] = outdoorGlob.bbox[0][dimIter] - 0.5;
            outdoorGlob.scale[dimIter - 3] = outdoorGlob.scale[dimIter - 3] + 0.5;
        }
        if (outdoorGlob.bbox[0][dimIter] >= outdoorGlob.scale[dimIter - 3])
            MyAssertHandler(
                ".\\r_outdoor.cpp",
                100,
                1,
                "%s",
                "outdoorGlob.bbox[SUPREMUM][dimIter] > outdoorGlob.bbox[INFIMUM][dimIter]");
    }
}

int Outdoor_UpdateTransforms()
{
    int result; // eax
    int dimension; // [esp+4h] [ebp-4h]

    for (dimension = 0; dimension != 3; ++dimension)
    {
        outdoorGlob.scale[dimension] = (outdoorMapSize[dimension] - 1)
            / (outdoorGlob.scale[dimension - 3] - outdoorGlob.bbox[0][dimension]);
        outdoorGlob.invScale[dimension] = 1.0 / outdoorGlob.scale[dimension];
        outdoorGlob.add[dimension] = -outdoorGlob.bbox[0][dimension] * outdoorGlob.scale[dimension];
        result = dimension + 1;
    }
    return result;
}

void __cdecl R_RegisterOutdoorImage(GfxWorld *world, const float *outdoorMin, const float *outdoorMax)
{
    iassert( world );
    Outdoor_ApplyBoundingBox(outdoorMin, outdoorMax);
    Outdoor_UpdateTransforms();
    Outdoor_SetRendererOutdoorLookupMatrix(world);
    world->outdoorImage = Image_Register("$outdoor", TS_FUNCTION, IMAGE_TRACK_MISC);
    iassert( world->outdoorImage );
}

void __cdecl Outdoor_SetRendererOutdoorLookupMatrix(GfxWorld *world)
{
    float outdoorScale[3]; // [esp+0h] [ebp-1Ch]
    float outdoorTranslate[3]; // [esp+Ch] [ebp-10h]
    int dimIter; // [esp+18h] [ebp-4h]

    for (dimIter = 0; dimIter != 3; ++dimIter)
    {
        outdoorScale[dimIter] = 1.0 / (outdoorGlob.scale[dimIter - 3] - outdoorGlob.bbox[0][dimIter]);
        outdoorTranslate[dimIter] = -outdoorGlob.bbox[0][dimIter] * outdoorScale[dimIter];
    }
    MatrixIdentity44(world->outdoorLookupMatrix);
    world->outdoorLookupMatrix[0][0] = outdoorScale[0];
    world->outdoorLookupMatrix[1][1] = outdoorScale[1];
    world->outdoorLookupMatrix[2][2] = outdoorScale[2];
    world->outdoorLookupMatrix[3][0] = outdoorTranslate[0];
    world->outdoorLookupMatrix[3][1] = outdoorTranslate[1];
    world->outdoorLookupMatrix[3][2] = outdoorTranslate[2];
}

void Outdoor_TempHunkFreePic()
{
    iassert( outdoorGlob.pic );
    Hunk_FreeTempMemory((char *)outdoorGlob.pic);
}

uint8_t *Outdoor_ComputeTexels()
{
    uint8_t *result; // eax
    int zTexture; // [esp+24h] [ebp-1Ch]
    float zWorld; // [esp+28h] [ebp-18h]
    float yWorld; // [esp+2Ch] [ebp-14h]
    uint8_t *outByte; // [esp+30h] [ebp-10h]
    int x; // [esp+34h] [ebp-Ch]
    int y; // [esp+38h] [ebp-8h]
    float xWorld; // [esp+3Ch] [ebp-4h]

    iassert( outdoorGlob.pic );
    result = outdoorGlob.pic;
    outByte = outdoorGlob.pic;
    for (y = 0; y != outdoorMapSize[1]; ++y)
    {
        yWorld = outdoorGlob.invScale[1] * ((double)y + 0.5 - outdoorGlob.add[1]);
        for (x = 0; ; ++x)
        {
            result = (uint8_t *)x;
            if (x == outdoorMapSize[0])
                break;
            xWorld = outdoorGlob.invScale[0] * ((double)x + 0.5 - outdoorGlob.add[0]);
            zWorld = Outdoor_TraceHeightInWorld(xWorld, yWorld);
            zTexture = Outdoor_TransformToTextureClamped(2, zWorld);
            *outByte = zTexture;
            iassert( zTexture == *outByte ); // ?? seems useless
            ++outByte;
        }
    }
    return result;
}

double __cdecl Outdoor_TraceHeightInWorld(float worldX, float worldY)
{
    float traceEndHeight; // [esp+4h] [ebp-4Ch]
    float traceStartHeight; // [esp+8h] [ebp-48h]
    trace_t results; // [esp+Ch] [ebp-44h] BYREF
    float traceStart[3]; // [esp+38h] [ebp-18h] BYREF
    float traceEnd[3]; // [esp+44h] [ebp-Ch] BYREF

    traceStartHeight = outdoorGlob.bbox[1][2] + 1.0;
    traceEndHeight = outdoorGlob.bbox[0][2] - 1.0;
    traceStart[0] = worldX;
    traceStart[1] = worldY;
    traceStart[2] = traceStartHeight;
    traceEnd[0] = worldX;
    traceEnd[1] = worldY;
    traceEnd[2] = traceEndHeight;
    memset((uint8_t *)&results, 0, sizeof(results));
    results.fraction = 1.0;
    CM_BoxTrace(&results, traceStart, traceEnd, vec3_origin, vec3_origin, 0, 8193);
    return (float)((traceEndHeight - traceStartHeight) * results.fraction + traceStartHeight);
}

int __cdecl Outdoor_TransformToTextureClamped(int dimension, float inWorld)
{
    int max; // [esp+4h] [ebp-18h]
    int unclamped; // [esp+14h] [ebp-8h]
    float transformed; // [esp+18h] [ebp-4h]

    transformed = inWorld * outdoorGlob.scale[dimension] + outdoorGlob.add[dimension];
    unclamped = (int)(transformed - 0.4999999990686774);
    max = outdoorMapSize[dimension] - 1;
    iassert(max > 0); // lwss: changed assert
    //iassert( min < max );
    if (unclamped < 0)
        return 0;
    if (unclamped <= max)
        return (int)(transformed - 0.4999999990686774);
    return max;
}

void __cdecl R_GenerateOutdoorImage(GfxImage *outdoorImage)
{
    outdoorGlob.pic = (uint8_t *)Hunk_AllocateTempMemory(
        outdoorMapSize[1] * outdoorMapSize[0],
        "Outdoor_TempHunkAllocatePic");
    Outdoor_ComputeTexels();
    Image_Generate2D(outdoorImage, outdoorGlob.pic, outdoorMapSize[0], outdoorMapSize[1], D3DFMT_L8);
    Outdoor_TempHunkFreePic();
}
// ---------------------------------------------------------------------------
// r_outdoorDebug (port diagnostic, off by default).  Reads the live `$outdoor`
// texture back once per world (the texels the shaders sample), reports its
// histogram, and evaluates the precipitation shaders' outdoor test at the
// camera once a second:
//   lookup = (viewOrg + worldOffset) * outdoorLookupMatrix,
//   visible = saturate((lookup.z - texel) * r_outdoorFeather) > 0.
// `PASS:OUTDOOR_MAP_BAKED` needs a non-flat 8-bit map; a flat map (every texel
// equal -- what the loadobj-only generator produced for fastfile levels)
// fails loudly because it disables indoor culling everywhere.
#include "r_dvars.h"
#include "r_init.h"
#include "rb_backend.h"
#include "r_bsp.h"
#include <qcommon/qcommon.h>
#include <universal/com_memory.h>
#include <cstring>

namespace
{
const GfxWorld *s_outdoorDebugWorld;
uint8_t *s_outdoorTexels;
int s_outdoorW;
int s_outdoorH;
int s_outdoorLastMs;

bool R_OutdoorDebugReadback(const GfxWorld *world)
{
    const GfxImage *image = world->outdoorImage;
    s_outdoorW = s_outdoorH = 0;
    if (!image || image->mapType != MAPTYPE_2D || !image->texture.map)
    {
        Com_Printf(8, "FAIL:OUTDOOR_MAP_BAKED no live 2D $outdoor texture\n");
        return false;
    }
    D3DSURFACE_DESC desc{};
    if (image->texture.map->GetLevelDesc(0, &desc) < 0 || desc.Format != D3DFMT_L8)
    {
        Com_Printf(8, "FAIL:OUTDOOR_MAP_BAKED $outdoor level 0 is not L8 (format %d)\n", (int)desc.Format);
        return false;
    }
    D3DLOCKED_RECT locked{};
    if (image->texture.map->LockRect(0, &locked, nullptr, D3DLOCK_READONLY) < 0 || !locked.pBits)
    {
        Com_Printf(8, "FAIL:OUTDOOR_MAP_BAKED LockRect($outdoor) failed\n");
        return false;
    }
    const int w = (int)desc.Width;
    const int h = (int)desc.Height;
    if (s_outdoorTexels)
        Z_Free(s_outdoorTexels, 0);
    s_outdoorTexels = (uint8_t *)Z_Malloc(w * h, "r_outdoorDebug", 0);
    uint32_t histogram[256] = {};
    uint32_t fnv = 2166136261u;
    for (int y = 0; y < h; ++y)
    {
        const uint8_t *row = (const uint8_t *)locked.pBits + (size_t)y * locked.Pitch;
        memcpy(s_outdoorTexels + (size_t)y * w, row, w);
        for (int x = 0; x < w; ++x)
        {
            ++histogram[row[x]];
            fnv = (fnv ^ row[x]) * 16777619u;
        }
    }
    image->texture.map->UnlockRect(0);
    s_outdoorW = w;
    s_outdoorH = h;

    int distinct = 0, lo = 256, hi = -1;
    for (int v = 0; v < 256; ++v)
    {
        if (!histogram[v])
            continue;
        ++distinct;
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    Com_Printf(8, "OUTDOOR_MAP w=%d h=%d distinct=%d min=%d max=%d fnv=0x%08x zero=%u\n",
               w, h, distinct, lo, hi, fnv, histogram[0]);
    if (distinct > 1)
        Com_Printf(8, "PASS:OUTDOOR_MAP_BAKED distinct=%d fnv=0x%08x\n", distinct, fnv);
    else
        Com_Printf(8, "FAIL:OUTDOOR_MAP_BAKED flat map (every texel %d): precipitation is not culled indoors\n", lo);
    return true;
}
} // namespace

void R_OutdoorDebugFrame(const float *viewOrg, const float *viewForward)
{
    if (!r_outdoorDebug || !r_outdoorDebug->current.enabled || !rgp.world)
        return;
    const GfxWorld *world = rgp.world;
    if (world != s_outdoorDebugWorld)
    {
        s_outdoorDebugWorld = world;
        s_outdoorLastMs = 0;
        R_OutdoorDebugReadback(world);
    }
    const int now = Sys_Milliseconds();
    if (!s_outdoorW || now - s_outdoorLastMs < 1000)
        return;
    s_outdoorLastMs = now;

    // Same offset R_GenerateWorldOutdoorLookupMatrix adds: -awayBias along the
    // view direction, then downBias on world z.
    const float away = r_outdoorAwayBias->current.value;
    float p[3] = { viewOrg[0] - away * viewForward[0], viewOrg[1] - away * viewForward[1],
                   viewOrg[2] - away * viewForward[2] + r_outdoorDownBias->current.value };
    const float(*m)[4] = world->outdoorLookupMatrix;
    float l[3];
    for (int c = 0; c < 3; ++c)
        l[c] = p[0] * m[0][c] + p[1] * m[1][c] + p[2] * m[2][c] + m[3][c];
    int tx = (int)(l[0] * s_outdoorW);
    int ty = (int)(l[1] * s_outdoorH);
    tx = tx < 0 ? 0 : (tx >= s_outdoorW ? s_outdoorW - 1 : tx);
    ty = ty < 0 ? 0 : (ty >= s_outdoorH ? s_outdoorH - 1 : ty);
    const float texel = s_outdoorTexels[ty * s_outdoorW + tx] / 255.0f;
    float alpha = (l[2] - texel) * r_outdoorFeather->current.value;
    alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    const float worldTexelZ = m[2][2] != 0.0f ? (texel - m[3][2]) / m[2][2] : 0.0f;
    Com_Printf(8, "OUTDOOR_PROBE org=%.0f,%.0f,%.0f uvz=%.4f,%.4f,%.4f texel=%d(%d,%d) roof_z=%.0f precip_alpha=%.2f %s\n",
               viewOrg[0], viewOrg[1], viewOrg[2], l[0], l[1], l[2],
               s_outdoorTexels[ty * s_outdoorW + tx], tx, ty, worldTexelZ, alpha,
               alpha > 0.0f ? "outdoor" : "indoor");
}
