#include <universal/q_shared.h>
#include "r_model_lighting.h"
#include "r_image.h"
#include "r_dvars.h"
#include "r_staticmodelcache.h"
#include "r_dpvs.h"
#include "r_primarylights.h"
#include "rb_light.h"
#include "rb_logfile.h"
#include <universal/profile.h>
#include "r_state.h"
#include <platform/switch/switch_diag_dvars.h>
#include <xanim/xmodel.h>
#include <cstring>

struct $D83B18AC5ED51685DB5F92059A920C50 // sizeof=0x4
{                                       // ...
    uint32_t baseIndex;             // ...
};

struct $616C0C4E0125F5DAA7F70C1AB2F0F42D // sizeof=0x6C
{                                       // ...
    float invImageHeight;               // ...
    $D83B18AC5ED51685DB5F92059A920C50 xmodel; // ...
    uint32_t totalEntryLimit;       // ...
    uint32_t entryBitsY;            // ...
    uint32_t imageHeight;           // ...
    const GfxEntity *entities;
    uint32_t modFrameCount;         // ...
    GfxImage *lightImages[2];           // ...
    GfxImage *image;                    // ...
    uint32_t xmodelEntryLimit;      // ...
    GfxLightingInfo *lightingInfo;      // ...
    float (*lightingOrigins)[3];        // ...
    int allocModelFail;                 // ...
    uint32_t *pixelFreeBits[4];     // ...
    uint32_t *prevPrevPixelFreeBits; // ...
    uint32_t *prevPixelFreeBits;    // ...
    uint32_t *currPixelFreeBits;    // ...
    uint32_t pixelFreeBitsSize;     // ...
    uint32_t pixelFreeBitsWordCount; // ...
    uint32_t pixelFreeRover;        // ...
    _D3DLOCKED_BOX lockedBox;           // ...
};

struct GfxSmodelLightGlob_s // sizeof=0x6080
{                                       // ...
    uint16_t smodelIndex[4096]; // ...
    uint32_t usedFrameCount[4096];  // ...
    uint32_t entryLimit;            // ...
    uint32_t assignedCount;         // ...
    uint32_t freeableCount;         // ...
    uint32_t frameCount;            // ...
    int anyNewLighting;                 // ...
    uint32_t pad[27];
};
struct GfxSmodelLightGlob // sizeof=0xA080
{                                       // ...
    uint16_t freeableHandles[4096]; // ...
    uint32_t lightingBits[2048];    // ...
    GfxSmodelLightGlob_s local; // ...
};

struct GfxFindLightForBox // sizeof=0x1C
{                                       // ...
    const GfxViewInfo *viewInfo;        // ...
    float midPoint[3];                  // ...
    float halfSize[3];                  // ...
};

struct GfxFindLightForSphere // sizeof=0x14
{                                       // ...
    const GfxViewInfo *viewInfo;        // ...
    float origin[3];                    // ...
    float radius;                       // ...
};

$616C0C4E0125F5DAA7F70C1AB2F0F42D modelLightGlob;

GfxSmodelLightGlob smodelLightGlob;

int s_modelLightingSampleDelta[64];

void __cdecl R_SetModelLightingCoords(uint16_t handle, float *out)
{
    uint32_t entryIndex; // [esp+10h] [ebp-18h]
    float yCoord; // [esp+14h] [ebp-14h]
    float xCoord; // [esp+24h] [ebp-4h]

    entryIndex = R_ModelLightingIndexFromHandle(handle);
    xCoord = ((double)(4 * (entryIndex & 0x3F)) + 2.0) * 0.00390625;
    yCoord = ((double)((entryIndex >> 4) & 0xFFFFFFFC) + 2.0) * modelLightGlob.invImageHeight;
    out[0] = xCoord;
    out[1] = yCoord;
    out[2] = 0.5;
    out[3] = 1.0;
}

void __cdecl R_GetPackedStaticModelLightingCoords(uint32_t smodelIndex, PackedLightingCoords *packedCoords)
{
    uint32_t v2; // [esp+0h] [ebp-18h]
    uint32_t v3; // [esp+4h] [ebp-14h]
    uint32_t entryIndex; // [esp+8h] [ebp-10h]
    uint32_t xPixel; // [esp+10h] [ebp-8h]

    entryIndex = R_ModelLightingIndexFromHandle(rgp.world->dpvs.smodelDrawInsts[smodelIndex].lightingHandle);
    xPixel = 4 * (entryIndex & 0x3F);
    iassert( IsPowerOf2( modelLightGlob.imageHeight ) );
    if (modelLightGlob.imageHeight > 0x100)
        MyAssertHandler(
            ".\\r_model_lighting.cpp",
            834,
            0,
            "modelLightGlob.imageHeight <= GFX_ML_IMAGE_WIDTH\n\t%i, %i",
            modelLightGlob.imageHeight,
            256);
    v3 = xPixel + 2;
    if (xPixel + 2 != (uint8_t)(xPixel + 2))
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\qcommon\\../universal/assertive.h",
            281,
            0,
            "i == static_cast< Type >( i )\n\t%i, %i",
            v3,
            (uint8_t)v3);
    packedCoords->array[0] = v3;
    v2 = ((entryIndex >> 4) & 0xFFFFFFFC) + 2 * (0x100 / modelLightGlob.imageHeight);
    if (v2 != (uint8_t)v2)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\qcommon\\../universal/assertive.h",
            281,
            0,
            "i == static_cast< Type >( i )\n\t%i, %i",
            v2,
            (uint8_t)v2);
    packedCoords->array[1] = v2;
    packedCoords->array[2] = 0x80;
    packedCoords->array[3] = 0;
}

uint32_t __cdecl R_ModelLightingIndexFromHandle(uint16_t handle)
{
    iassert(handle && handle <= modelLightGlob.totalEntryLimit);
    return handle - 1;
}

char __cdecl R_AllocStaticModelLighting(GfxStaticModelDrawInst *smodelDrawInst, uint32_t smodelIndex)
{
    uint16_t handle; // [esp+0h] [ebp-10h]
    uint32_t smodelIndexPrev; // [esp+4h] [ebp-Ch]
    uint32_t entryIndex; // [esp+8h] [ebp-8h]

    iassert(rgp.world);
    handle = smodelDrawInst->lightingHandle;
    if (handle)
    {
        entryIndex = R_ModelLightingIndexFromHandle(handle);
        if (!r_cacheSModelLighting->current.enabled)
        {
            bcassert(smodelIndex >> 5, ARRAY_COUNT(smodelLightGlob.lightingBits));

            smodelLightGlob.lightingBits[smodelIndex >> 5] |= 0x80000000 >> (smodelIndex & 0x1F);
            smodelLightGlob.local.anyNewLighting = 1;
        }
    }
    else
    {
        if (smodelLightGlob.local.assignedCount >= smodelLightGlob.local.entryLimit)
        {
            do
            {
                if (!smodelLightGlob.local.freeableCount)
                    return 0;
                handle = smodelLightGlob.freeableHandles[--smodelLightGlob.local.freeableCount];
                entryIndex = R_ModelLightingIndexFromHandle(handle);
            } while (smodelLightGlob.local.frameCount == smodelLightGlob.local.usedFrameCount[entryIndex]);

            smodelIndexPrev = smodelLightGlob.local.smodelIndex[entryIndex];
            rgp.world->dpvs.smodelDrawInsts[smodelIndexPrev].lightingHandle = 0;
            R_UncacheStaticModel(smodelIndexPrev);
        }
        else
        {
            entryIndex = smodelLightGlob.local.assignedCount++;
            handle = entryIndex + 1;
        }
        iassert(handle);
        smodelDrawInst->lightingHandle = handle;
        iassert(smodelIndex == static_cast<unsigned short>(smodelIndex));
        smodelLightGlob.local.smodelIndex[entryIndex] = smodelIndex;
        bcassert(smodelIndex >> 5, ARRAY_COUNT(smodelLightGlob.lightingBits));
        smodelLightGlob.lightingBits[smodelIndex >> 5] |= 0x80000000 >> (smodelIndex & 0x1F);
        smodelLightGlob.local.anyNewLighting = 1;
    }
    smodelLightGlob.local.usedFrameCount[entryIndex] = smodelLightGlob.local.frameCount;
    return 1;
}

uint32_t __cdecl R_AllocModelLighting_PrimaryLight(
    float *lightingOrigin,
    uint32_t dynEntId,
    uint16_t *cachedLightingHandle,
    GfxLightingInfo *lightingInfoOut)
{
    return R_AllocModelLighting(
        lightingOrigin,
        cachedLightingHandle,
        R_DynEntPrimaryLightCallback,
        &dynEntId,
        lightingInfoOut);
}

uint32_t __cdecl R_AllocModelLighting(
    float *lightingOrigin,
    uint16_t *cachedLightingHandle,
    uint32_t(__cdecl *GetPrimaryLightCallback)(const void *),
    const void *userData,
    GfxLightingInfo *lightingInfoOut)
{
    DWORD v7; // eax
    int v8; // [esp+4h] [ebp-2Ch]
    float *v9; // [esp+8h] [ebp-28h]
    float *v10; // [esp+10h] [ebp-20h]
    uint32_t pixelFreeRover; // [esp+14h] [ebp-1Ch]
    uint32_t entryIndex; // [esp+18h] [ebp-18h]
    uint32_t usedCount; // [esp+1Ch] [ebp-14h]
    uint32_t usedIndex; // [esp+20h] [ebp-10h]
    uint32_t usedIndexa; // [esp+20h] [ebp-10h]
    uint16_t lightingHandle; // [esp+28h] [ebp-8h]
    uint16_t lightingHandlea; // [esp+28h] [ebp-8h]
    uint32_t nonSunPrimaryLightIndex; // [esp+2Ch] [ebp-4h]

    iassert( cachedLightingHandle );
    lightingHandle = *cachedLightingHandle;
    if (*cachedLightingHandle
        && r_cacheModelLighting->current.enabled
        && ((usedIndex = R_ModelLightingIndexFromHandle(lightingHandle) - modelLightGlob.xmodel.baseIndex,
            v10 = modelLightGlob.lightingOrigins[usedIndex],
            *v10 != *lightingOrigin)
            || v10[1] != lightingOrigin[1]
            || v10[2] != lightingOrigin[2]
            ? (v8 = 0)
            : (v8 = 1),
            v8))
    {
        modelLightGlob.currPixelFreeBits[usedIndex >> 5] &= ~(0x80000000 >> (usedIndex & 0x1F));
        *lightingInfoOut = modelLightGlob.lightingInfo[usedIndex];
        return lightingHandle;
    }
    else if (modelLightGlob.allocModelFail)
    {
        return 0;
    }
    else
    {
        pixelFreeRover = modelLightGlob.pixelFreeRover;
        while (1)
        {
            if (!_BitScanReverse(&v7, modelLightGlob.prevPrevPixelFreeBits[pixelFreeRover] & modelLightGlob.prevPixelFreeBits[pixelFreeRover] & modelLightGlob.currPixelFreeBits[pixelFreeRover]))
                v7 = 63;// `CountLeadingZeros'::`2': : notFound;
            usedCount = v7 ^ 0x1F;
            if ((v7 ^ 0x1Fu) < 0x20)
                break;
            pixelFreeRover = (pixelFreeRover + 1) % modelLightGlob.pixelFreeBitsWordCount;
            if (pixelFreeRover == modelLightGlob.pixelFreeRover)
            {
                modelLightGlob.allocModelFail = 1;
                R_WarnOncePerFrame(R_WARN_MODEL_LIGHT_CACHE);
                return 0;
            }
        }
        modelLightGlob.pixelFreeRover = pixelFreeRover;
        modelLightGlob.currPixelFreeBits[pixelFreeRover] &= ~(0x80000000 >> usedCount);
        usedIndexa = usedCount + 32 * pixelFreeRover;
        if (usedIndexa >= modelLightGlob.xmodelEntryLimit)
            MyAssertHandler(
                ".\\r_model_lighting.cpp",
                362,
                0,
                "usedIndex doesn't index modelLightGlob.xmodelEntryLimit\n\t%i not in [0, %i)",
                usedIndexa,
                modelLightGlob.xmodelEntryLimit);
        entryIndex = usedIndexa + modelLightGlob.xmodel.baseIndex;
        lightingHandlea = usedIndexa + LOWORD(modelLightGlob.xmodel.baseIndex) + 1;
        v9 = modelLightGlob.lightingOrigins[usedIndexa];
        *v9 = *lightingOrigin;
        v9[1] = lightingOrigin[1];
        v9[2] = lightingOrigin[2];
        *cachedLightingHandle = lightingHandlea;
        lightingInfoOut->reflectionProbeIndex = R_CalcReflectionProbeIndex(lightingOrigin);
        nonSunPrimaryLightIndex = GetPrimaryLightCallback(userData);
        lightingInfoOut->primaryLightIndex = R_CalcModelLighting(entryIndex, lightingOrigin, nonSunPrimaryLightIndex, GFX_MODELLIGHT_SHOW_MISSING);
        modelLightGlob.lightingInfo[usedIndexa] = *lightingInfoOut;
        return lightingHandlea;
    }
}

uint32_t __cdecl R_DynEntPrimaryLightCallback(const void *userData)
{
    DWORD *data = (DWORD *)userData;
    return rgp.world->nonSunPrimaryLightForModelDynEnt[*data];
}

uint32_t __cdecl R_AllocModelLighting_Box(
    const GfxViewInfo *viewInfo,
    float *lightingOrigin,
    const float *boxMins,
    const float *boxMaxs,
    uint16_t *cachedLightingHandle,
    GfxLightingInfo *lightingInfoOut)
{
    GfxFindLightForBox boxWork; // [esp+0h] [ebp-1Ch] BYREF

    boxWork.viewInfo = viewInfo;
    Vec3Avg(boxMins, boxMaxs, boxWork.midPoint);
    Vec3Sub(boxWork.midPoint, boxMins, boxWork.halfSize);

    return R_AllocModelLighting(
        lightingOrigin,
        cachedLightingHandle,
        R_GetPrimaryLightForBoxCallback,
        &boxWork,
        lightingInfoOut);
}

uint32_t __cdecl R_GetPrimaryLightForBoxCallback(const void *userData)
{
    GfxFindLightForBox *boxWork = (GfxFindLightForBox *)userData;

    return R_GetNonSunPrimaryLightForBox(boxWork->viewInfo, boxWork->midPoint, boxWork->halfSize);
}

uint32_t __cdecl R_AllocModelLighting_Sphere(
    const GfxViewInfo *viewInfo,
    float *lightingOrigin,
    const float *origin,
    float radius,
    uint16_t *cachedLightingHandle,
    GfxLightingInfo *lightingInfoOut)
{
    GfxFindLightForSphere sphereWork; // [esp+0h] [ebp-14h] BYREF

    sphereWork.viewInfo = viewInfo;
    sphereWork.origin[0] = origin[0];
    sphereWork.origin[1] = origin[1];
    sphereWork.origin[2] = origin[2];
    sphereWork.radius = radius;

    return R_AllocModelLighting(
        lightingOrigin,
        cachedLightingHandle,
        R_GetPrimaryLightForSphereCallback,
        &sphereWork,
        lightingInfoOut);
}

uint32_t __cdecl R_GetPrimaryLightForSphereCallback(const void *userData)
{
    GfxFindLightForSphere *sphereWork = (GfxFindLightForSphere *)userData;
    
    return R_GetNonSunPrimaryLightForSphere(sphereWork->viewInfo, sphereWork->origin, sphereWork->radius);
}

void __cdecl R_ToggleModelLightingFrame()
{
    uint32_t entryIndex; // [esp+0h] [ebp-4h]

    ++smodelLightGlob.local.frameCount;
    modelLightGlob.modFrameCount = (modelLightGlob.modFrameCount + 1) % 4;
    modelLightGlob.allocModelFail = 0;
    modelLightGlob.prevPrevPixelFreeBits = modelLightGlob.pixelFreeBits[(modelLightGlob.modFrameCount + 2) % 4];
    modelLightGlob.prevPixelFreeBits = modelLightGlob.pixelFreeBits[(modelLightGlob.modFrameCount + 3) % 4];
    modelLightGlob.currPixelFreeBits = modelLightGlob.pixelFreeBits[modelLightGlob.modFrameCount % 4];
    Com_Memset(modelLightGlob.currPixelFreeBits, 255, modelLightGlob.pixelFreeBitsSize);
    smodelLightGlob.local.freeableCount = 0;
    for (entryIndex = 0; entryIndex < smodelLightGlob.local.assignedCount; ++entryIndex)
    {
        if (smodelLightGlob.local.frameCount - smodelLightGlob.local.usedFrameCount[entryIndex] >= 4)
            smodelLightGlob.freeableHandles[smodelLightGlob.local.freeableCount++] = entryIndex + 1;
    }
}

uint32_t __cdecl R_CalcModelLighting(
    uint32_t entryIndex,
    const float *lightingOrigin,
    uint32_t nonSunPrimaryLightIndex,
    GfxModelLightExtrapolation extrapolateBehavior)
{
    KISAK_NULLSUB();
    iassert(entryIndex == (unsigned short)entryIndex);
    bcassert(nonSunPrimaryLightIndex, rgp.world->primaryLightCount);

    return R_GetLightingAtPoint(
        &rgp.world->lightGrid,
        lightingOrigin,
        nonSunPrimaryLightIndex,
        entryIndex,
        extrapolateBehavior);
}

void __cdecl R_BeginAllStaticModelLighting()
{
    uint32_t size; // [esp+0h] [ebp-4h]

    iassert( !smodelLightGlob.local.anyNewLighting );
    size = 4 * ((rgp.world->dpvs.smodelCount + 31) >> 5);
    iassert( size < sizeof( smodelLightGlob.lightingBits ) );
    Com_Memset(smodelLightGlob.lightingBits, 0, size);
}

void __cdecl R_SetAllStaticModelLighting()
{
    DWORD v1; // eax
    uint32_t wordCount; // [esp+34h] [ebp-18h]
    uint32_t bits; // [esp+38h] [ebp-14h]
    uint32_t indexLow; // [esp+40h] [ebp-Ch]
    uint32_t wordIndex; // [esp+44h] [ebp-8h]

    if (smodelLightGlob.local.anyNewLighting)
    {
        smodelLightGlob.local.anyNewLighting = 0;

        PROF_SCOPED("SModelLighting");

        wordCount = (rgp.world->dpvs.smodelCount + 31) >> 5;
        iassert( wordCount < sizeof( smodelLightGlob.lightingBits ) );
        for (wordIndex = 0; wordIndex < wordCount; ++wordIndex)
        {
            bits = smodelLightGlob.lightingBits[wordIndex];
            if (bits)
            {
                while (1)
                {
                    if (!_BitScanReverse(&v1, bits))
                        v1 = 63;// `CountLeadingZeros'::`2': : notFound;
                    indexLow = v1 ^ 0x1F;
                    if ((v1 ^ 0x1Fu) >= 0x20)
                        break;
                    uint32_t bit = (0x80000000 >> indexLow);
                    iassert( bits & bit );
                    bits &= ~bit;
                    R_SetStaticModelLighting(indexLow + 32 * wordIndex);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Model-lighting values as actually uploaded.
//
// The model-lighting volume texture is not loaded from disk: every value it
// carries is written by RB_PatchModelLighting's LockBox patch upload below.
// The blocker requires the *values* to be checked, not the call counts --
// calls reported success while the props stayed dark.  This block counts the
// bytes in the locked box after each patch write (i.e. exactly what
// UnlockBox/UpdateTexture hand to the device) and correlates one named static
// model from the reported family (shelf/locker/crate) with the light-grid
// entry its lighting handle resolves to.  Always on and bounded, so the
// production walk can see the per-patch state even when the high-volume
// diagnostic markers are off.
namespace
{
constexpr uint32_t kModelLightingReportLineLimit = 512;
constexpr uint32_t kModelLightingNamedSlotCount = 8;
constexpr uint32_t kModelLightingNamedLineLimit = 32;
constexpr uint32_t kModelLightingApplyLineLimit = 16;
constexpr uint32_t kModelLightingSampleCount = 64;

struct ModelLightingNamedSlot
{
    bool inUse;
    bool reported;
    bool matched;
    bool applyReported;
    uint16_t entryIndex;
    uint16_t smodelIndex;
    uint16_t lightingHandle;
    uint8_t groundLightingPacked;
    uint8_t colorsCount;
    uint8_t primaryLightWeight;
    uint16_t colorsIndex0;
    float origin[3];
    char name[48];
};

ModelLightingNamedSlot s_modelLightingNamed[kModelLightingNamedSlotCount];
uint32_t s_modelLightingReportLines = 0;
uint32_t s_modelLightingNamedLines = 0;
uint32_t s_modelLightingApplyLines = 0;
uint32_t s_modelLightingTotalPatches = 0;
uint32_t s_modelLightingTotalBlocks = 0;
uint32_t s_modelLightingTotalZeroBlocks = 0;
uint64_t s_modelLightingTotalNonzeroBytes = 0;
uint64_t s_modelLightingTotalZeroBytes = 0;

bool ModelLightingNameIsReported(const char *name)
{
    return name && (std::strstr(name, "shelf") || std::strstr(name, "locker") ||
                    std::strstr(name, "crate"));
}

uint32_t ModelLightingBlockNonzeroBytes(const uint8_t *pixels)
{
    uint32_t nonzero = 0;
    for (uint32_t sample = 0; sample < kModelLightingSampleCount; ++sample)
    {
        const uint8_t *texel = pixels + s_modelLightingSampleDelta[sample];
        nonzero += (texel[0] != 0) + (texel[1] != 0) + (texel[2] != 0) + (texel[3] != 0);
    }
    return nonzero;
}

// The texel R_SetLightGridColors writes first for this patch, i.e. the value
// the shader samples at the patch's own light-grid entry.  Ground-lit patches
// store the same rgba in every sample; color patches pack rgb[0] with the
// primary-light weight in alpha, exactly like R_SetLightGridColors does.
uint32_t ModelLightingExpectedFirstTexel(const GfxModelLightingPatch *patch)
{
    if (!patch->colorsCount)
    {
        return (uint32_t)patch->groundLighting[0] |
               ((uint32_t)patch->groundLighting[1] << 8) |
               ((uint32_t)patch->groundLighting[2] << 16) |
               ((uint32_t)patch->groundLighting[3] << 24);
    }
    if (!rgp.world || !rgp.world->lightGrid.colors)
        return 0;
    const GfxLightGridColors *colors;
    GfxLightGridColors blended;
    if (patch->colorsCount == 1)
    {
        colors = &rgp.world->lightGrid.colors[patch->colorsIndex[0]];
    }
    else
    {
        R_FixedPointBlendLightGridColors(&rgp.world->lightGrid, patch->colorsIndex,
                                         (unsigned short *)patch->colorsWeight,
                                         patch->colorsCount, &blended);
        colors = &blended;
    }
    const uint8_t *rgb = colors->rgb[0];
    return ((uint32_t)patch->primaryLightWeight << 24) | rgb[2] | ((uint32_t)rgb[1] << 8) |
           ((uint32_t)rgb[0] << 16);
}

// independent resolution of the light-grid entry at a
// world-space lighting origin, from the grid's own mins/maxs/rowAxis/
// colAxis, row headers and RLE bytes.  Deliberately does not call
// R_LightGridLookup: KILLHOUSE_LIGHTGRID_APPLY reports the real lookup's
// returned corners next to this expectation, so byte-perfect grid data
// sampled at the wrong cell fails visibly instead of staying inferred.
struct ModelLightingExpectedCell
{
    uint32_t entryIndex; // 0xFFFFFFFF when no entry resolves
    uint16_t colorsIndex;
    uint8_t primaryLightIndex;
    uint8_t needsTrace;
    uint32_t reason; // 0=entry, 1=outside grid, 2=no row, 3=outside row,
                     // 4=empty RLE block, 5=z outside block, 6=index OOB
    uint32_t pos[3];
    uint32_t rowIndex;
    uint32_t colIndex;
    uint32_t z;
};

void ModelLightingExpectedCellForOrigin(const GfxLightGrid &grid, const float *origin,
                                        ModelLightingExpectedCell &cell)
{
    std::memset(&cell, 0, sizeof(cell));
    cell.entryIndex = 0xFFFFFFFFu;
    if (!grid.entries || !grid.rawRowData || !grid.rowDataStart || grid.rowAxis > 2 ||
        grid.colAxis > 2 || grid.rowAxis == grid.colAxis)
    {
        cell.reason = 1;
        return;
    }
    cell.pos[0] = (uint32_t)(((int)std::floor(origin[0]) + 0x20000) >> 5);
    cell.pos[1] = (uint32_t)(((int)std::floor(origin[1]) + 0x20000) >> 5);
    cell.pos[2] = (uint32_t)(((int)std::floor(origin[2]) + 0x20000) >> 6);
    const uint32_t rowCount = grid.maxs[grid.rowAxis] + 1u - grid.mins[grid.rowAxis];
    if (cell.pos[grid.rowAxis] < grid.mins[grid.rowAxis] ||
        cell.pos[grid.rowAxis] - grid.mins[grid.rowAxis] >= rowCount ||
        cell.pos[grid.colAxis] < grid.mins[grid.colAxis] ||
        cell.pos[grid.colAxis] > grid.maxs[grid.colAxis] ||
        cell.pos[2] < grid.mins[2] || cell.pos[2] > grid.maxs[2])
    {
        cell.reason = 1;
        return;
    }
    cell.rowIndex = cell.pos[grid.rowAxis] - grid.mins[grid.rowAxis];
    const uint16_t rowStart = grid.rowDataStart[cell.rowIndex];
    if (rowStart == 0xFFFFu || 4u * (uint32_t)rowStart + 12u > grid.rawRowDataSize)
    {
        cell.reason = 2;
        return;
    }
    const uint8_t *row = grid.rawRowData + 4u * (uint32_t)rowStart;
    const uint16_t colStart = (uint16_t)(row[0] | (row[1] << 8));
    const uint16_t colCount = (uint16_t)(row[2] | (row[3] << 8));
    const uint16_t zStart = (uint16_t)(row[4] | (row[5] << 8));
    const uint16_t zCount = (uint16_t)(row[6] | (row[7] << 8));
    if (cell.pos[grid.colAxis] < colStart || cell.pos[grid.colAxis] - colStart >= colCount ||
        cell.pos[2] < zStart || cell.pos[2] - zStart >= zCount)
    {
        cell.reason = 3;
        return;
    }
    cell.colIndex = cell.pos[grid.colAxis] - colStart;
    cell.z = cell.pos[2] - zStart;
    const uint32_t firstEntry = (uint32_t)row[8] | ((uint32_t)row[9] << 8) |
                                ((uint32_t)row[10] << 16) | ((uint32_t)row[11] << 24);
    const uint8_t *const rawEnd = grid.rawRowData + grid.rawRowDataSize;
    const uint8_t *rle = row + 12;
    uint32_t col = cell.colIndex;
    uint32_t first = firstEntry;
    const uint32_t zBytes = zCount > 255u ? 4u : 3u;
    for (uint32_t block = 0; block < 4096u; ++block)
    {
        if (rle + 2 > rawEnd)
        {
            cell.reason = 4;
            return;
        }
        const uint32_t run = rle[0];
        const uint32_t height = rle[1];
        if (col < run)
        {
            if (height == 0u || rle + zBytes > rawEnd)
            {
                cell.reason = 4;
                return;
            }
            const uint32_t baseZ = rle[2] | (zBytes == 4u ? ((uint32_t)rle[3] << 8) : 0u);
            if (cell.z < baseZ || cell.z - baseZ >= height)
            {
                cell.reason = 5;
                return;
            }
            const uint64_t index =
                (uint64_t)first + (uint64_t)col * height + (cell.z - baseZ);
            if (index >= grid.entryCount)
            {
                cell.reason = 6;
                return;
            }
            cell.entryIndex = (uint32_t)index;
            cell.colorsIndex = grid.entries[cell.entryIndex].colorsIndex;
            cell.primaryLightIndex = grid.entries[cell.entryIndex].primaryLightIndex;
            cell.needsTrace = grid.entries[cell.entryIndex].needsTrace;
            cell.reason = 0;
            return;
        }
        col -= run;
        first += run * height;
        rle += height != 0u ? zBytes : 2u;
    }
    cell.reason = 4;
}

// Called by R_SetStaticModelLighting right after the patch for a named model
// was allocated in the frame's backend data, so RB_PatchModelLighting can
// report the value actually written into the volume texture for that entry.
void ModelLightingNoteNamedPatch(uint32_t patchCountBefore, uint32_t entryIndex,
                                 uint32_t smodelIndex, uint16_t lightingHandle,
                                 const char *name, uint8_t groundLightingPacked,
                                 const float *lightingOrigin,
                                 uint8_t wirePrimaryLightIndex)
{
    if (!frontEndDataOut || frontEndDataOut->modelLightingPatchCount <= patchCountBefore)
        return;
    const GfxModelLightingPatch *patch =
        &frontEndDataOut->modelLightingPatchList[patchCountBefore];
    if (patch->modelLightingIndex != entryIndex)
        return;
    for (uint32_t slot = 0; slot < kModelLightingNamedSlotCount; ++slot)
    {
        ModelLightingNamedSlot &record = s_modelLightingNamed[slot];
        if (record.inUse && record.entryIndex != entryIndex)
            continue;
        if (!record.inUse)
        {
            record.inUse = true;
            record.entryIndex = (uint16_t)entryIndex;
            record.smodelIndex = (uint16_t)smodelIndex;
            record.lightingHandle = lightingHandle;
            record.name[0] = 0;
            if (name)
            {
                std::strncpy(record.name, name, sizeof(record.name) - 1);
                record.name[sizeof(record.name) - 1] = 0;
            }
        }
        record.groundLightingPacked = groundLightingPacked;
        record.colorsCount = patch->colorsCount;
        record.primaryLightWeight = patch->primaryLightWeight;
        record.colorsIndex0 = patch->colorsCount ? patch->colorsIndex[0] : 0u;
        record.reported = false;
        record.matched = false;
        if (lightingOrigin)
        {
            record.origin[0] = lightingOrigin[0];
            record.origin[1] = lightingOrigin[1];
            record.origin[2] = lightingOrigin[2];
        }
        // one application line per named slot, joining the
        // real lookup record (rb_light.cpp's probe) with the independent
        // cell resolution above and the patch value this entry carries.  The
        // expected entry/colours and the actual quad are directly comparable.
        const R_LightGridApplyProbe *probe = R_LightGridApplyProbeRead();
        if (!record.applyReported && s_modelLightingApplyLines < kModelLightingApplyLineLimit)
        {
            record.applyReported = true;
            ModelLightingExpectedCell expected;
            ModelLightingExpectedCellForOrigin(rgp.world->lightGrid, record.origin, expected);
            char quadText[8 * 48];
            char rawQuadText[8 * 48];
            quadText[0] = 0;
            rawQuadText[0] = 0;
            for (uint32_t corner = 0; corner < 8; ++corner)
            {
                char one[56];
                char rawOne[56];
                if (probe && probe->cornerEntryIndex[corner] != 0xFFFFFFFFu)
                    std::snprintf(one, sizeof(one), "%s%u/%u/%u/%u/%u", corner ? "," : "",
                                  probe->cornerEntryIndex[corner], probe->cornerColors[corner],
                                  probe->cornerPrimary[corner], probe->cornerNeedsTrace[corner],
                                  probe->cornerWeightMilli[corner]);
                else
                    std::snprintf(one, sizeof(one), "%s%u/0/0/0/%u", corner ? "," : "",
                                  0xFFFFFFFFu, probe ? probe->cornerWeightMilli[corner] : 0u);
                if (probe && probe->rawEntryIndex[corner] != 0xFFFFFFFFu)
                    std::snprintf(rawOne, sizeof(rawOne), "%s%u/%u/%u/%u", corner ? "," : "",
                                  probe->rawEntryIndex[corner], probe->rawColors[corner],
                                  probe->rawPrimary[corner], probe->rawNeedsTrace[corner]);
                else
                    std::snprintf(rawOne, sizeof(rawOne), "%s%u/0/0/0", corner ? "," : "",
                                  0xFFFFFFFFu);
                std::strncat(quadText, one, sizeof(quadText) - std::strlen(quadText) - 1);
                std::strncat(rawQuadText, rawOne,
                             sizeof(rawQuadText) - std::strlen(rawQuadText) - 1);
            }
            ++s_modelLightingApplyLines;
            Com_Printf(
                0,
                "KILLHOUSE_LIGHTGRID_APPLY name=%s smodel=%u entry=%u recorded=%u "
                "ground=%u alt=%u "
                "origin=%.1f,%.1f,%.1f pos=%u,%u,%u rawpos=%u,%u,%u "
                "expect=%u/%u/%u/%u/%u quad=%s rawquad=%s rawdefault=%u raw=%u "
                "wire_primary=%u mapped=%u "
                "visible=%u occluded=%u samples=%u default=%u has_regions=%u sun=%u "
                "world_sun=%u prim=%u com=%u colors=%u weight=%u\n",
                record.name, record.smodelIndex, record.entryIndex,
                probe ? probe->recorded : 0u, record.groundLightingPacked,
                Dvar_GetBool("r_altModelLightingUpdate") ? 1u : 0u, record.origin[0],
                record.origin[1], record.origin[2], expected.pos[0], expected.pos[1],
                expected.pos[2], probe ? probe->rawPos[0] : 0u,
                probe ? probe->rawPos[1] : 0u, probe ? probe->rawPos[2] : 0u,
                expected.entryIndex, expected.colorsIndex,
                expected.primaryLightIndex, expected.needsTrace, expected.reason, quadText,
                rawQuadText, probe ? probe->rawDefaultGridEntry : 0u,
                probe ? probe->chosenPrimary : 0u, wirePrimaryLightIndex,
                probe ? probe->mappedPrimary : 0u,
                probe ? probe->visibleMilli : 0u, probe ? probe->occludedMilli : 0u,
                probe ? probe->sampleCount : 0u, probe ? probe->defaultGridEntry : 0u,
                probe ? probe->hasLightRegions : 0u,
                probe ? probe->sunPrimaryLightIndex : 0u,
                probe ? probe->worldSunPrimaryLightIndex : 0u,
                probe ? probe->primaryLightCount : 0u, probe ? probe->comPrimaryLightCount : 0u,
                patch->colorsCount ? patch->colorsIndex[0] : 0u, patch->primaryLightWeight);
        }
        return;
    }
}
} // namespace

void __cdecl R_SetStaticModelLighting(uint32_t smodelIndex)
{
    uint32_t entryIndex; // [esp+0h] [ebp-18h]
    const GfxStaticModelDrawInst *smodelDrawInst; // [esp+4h] [ebp-14h]
    const GfxStaticModelInst *smodelInst; // [esp+8h] [ebp-10h]
    float lightingOrigin[3]; // [esp+Ch] [ebp-Ch] BYREF

    if (smodelIndex >= rgp.world->dpvs.smodelCount)
        MyAssertHandler(
            ".\\r_model_lighting.cpp",
            728,
            0,
            "smodelIndex doesn't index rgp.world->dpvs.smodelCount\n\t%i not in [0, %i)",
            smodelIndex,
            rgp.world->dpvs.smodelCount);
    smodelInst = &rgp.world->dpvs.smodelInsts[smodelIndex];
    Vec3Avg(smodelInst->mins, smodelInst->maxs, lightingOrigin);
    smodelDrawInst = &rgp.world->dpvs.smodelDrawInsts[smodelIndex];
    entryIndex = R_ModelLightingIndexFromHandle(smodelDrawInst->lightingHandle);
    const uint32_t patchCountBefore =
        frontEndDataOut ? (uint32_t)frontEndDataOut->modelLightingPatchCount : 0u;
    const XModel *namedModel = smodelDrawInst->model;
    const char *modelName = namedModel ? XModelGetName(namedModel) : nullptr;
    const bool named = ModelLightingNameIsReported(modelName);
    // watch this model's own R_GetLightingAtPoint call so the
    // application line carries what the real lookup resolved, not a rerun.
    if (named && !smodelInst->groundLighting.packed)
        R_LightGridApplyProbeBegin(entryIndex);
    if (smodelInst->groundLighting.packed)
        R_SetModelGroundLighting(entryIndex, (const uint8_t *)&smodelInst->groundLighting);
    else
        R_CalcModelLighting(entryIndex, lightingOrigin, smodelDrawInst->primaryLightIndex, GFX_MODELLIGHT_EXTRAPOLATE);
    if (named)
        ModelLightingNoteNamedPatch(patchCountBefore, entryIndex, smodelIndex,
                                    smodelDrawInst->lightingHandle, modelName,
                                    smodelInst->groundLighting.packed, lightingOrigin,
                                    smodelDrawInst->primaryLightIndex);
    if (named)
        R_LightGridApplyProbeEnd();
}

void __cdecl R_SetModelGroundLighting(uint32_t entryIndex, const uint8_t *groundLighting)
{
    GfxModelLightingPatch *patch; // [esp+8h] [ebp-4h]

    patch = R_BackEndDataAllocAndClearModelLightingPatch(frontEndDataOut);
    if (entryIndex != (uint16_t)entryIndex)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\qcommon\\../universal/assertive.h",
            281,
            0,
            "i == static_cast< Type >( i )\n\t%i, %i",
            entryIndex,
            (uint16_t)entryIndex);
    patch->modelLightingIndex = entryIndex;
    iassert( patch->colorsCount == 0 );
    *(uint32_t *)patch->groundLighting = *(uint32_t *)groundLighting;
}

void __cdecl R_SetModelLightingCoordsForSource(uint16_t handle, GfxCmdBufSourceState *source)
{
    R_SetModelLightingCoords(handle, source->input.consts[CONST_SRC_CODE_BASE_LIGHTING_COORDS]);
    R_DirtyCodeConstant(source, CONST_SRC_CODE_BASE_LIGHTING_COORDS);
}

void __cdecl R_SetStaticModelLightingCoordsForSource(uint32_t smodelIndex, GfxCmdBufSourceState *source)
{
    R_SetModelLightingCoords(rgp.world->dpvs.smodelDrawInsts[smodelIndex].lightingHandle, source->input.consts[CONST_SRC_CODE_BASE_LIGHTING_COORDS]);
    R_DirtyCodeConstant(source, CONST_SRC_CODE_BASE_LIGHTING_COORDS);
}


uint32_t R_SetModelLightingSampleDeltas()
{
    uint32_t result; // eax
    uint32_t i; // [esp+0h] [ebp-10h]
    uint32_t sampleIndex; // [esp+4h] [ebp-Ch]
    uint32_t dz; // [esp+8h] [ebp-8h]
    uint32_t dy; // [esp+Ch] [ebp-4h]

    sampleIndex = 0;
    for (dz = 0; dz < 4; ++dz)
    {
        for (dy = 0; dy < 4; ++dy)
        {
            for (i = 0; i < 4; ++i)
                s_modelLightingSampleDelta[sampleIndex++] = modelLightGlob.lockedBox.SlicePitch * dz
                + modelLightGlob.lockedBox.RowPitch * dy
                + 4 * i;
        }
        result = dz + 1;
    }
    return result;
}

void __cdecl R_SetModelLightingLookupScale(GfxCmdBufInput *input)
{
    float lookupScale[4]; // [esp+4h] [ebp-10h] BYREF

    lookupScale[0] = 0.005859375;
    lookupScale[1] = modelLightGlob.invImageHeight * 1.5;
    lookupScale[2] = 0.375;
    lookupScale[3] = 0.0;
    R_SetInputCodeConstantFromVec4(input, CONST_SRC_CODE_LIGHTING_LOOKUP_SCALE, lookupScale);
}

void __cdecl R_SetupDynamicModelLighting(GfxCmdBufInput *input)
{
    R_SetInputCodeImageTexture(input, TEXTURE_SRC_CODE_MODEL_LIGHTING, modelLightGlob.image);
    R_SetModelLightingLookupScale(input);
}

void __cdecl R_ShutdownModelLightingGlobals()
{
    uint32_t i; // [esp+0h] [ebp-4h]

    for (i = 0; i < 4; ++i)
        R_FreeGlobalVariable(modelLightGlob.pixelFreeBits[i]);
    R_FreeGlobalVariable(modelLightGlob.lightingOrigins);
    R_FreeGlobalVariable(modelLightGlob.lightingInfo);
}

void __cdecl R_InitModelLightingGlobals()
{
    DWORD v1; // eax
    uint32_t totalBitsNeeded; // [esp+Ch] [ebp-8h]
    uint32_t i; // [esp+10h] [ebp-4h]

    modelLightGlob.xmodelEntryLimit = gfxCfg.maxClientViews << 10;
    if (!_BitScanReverse(&v1, gfxCfg.maxClientViews << 10))
        v1 = 63;// `CountLeadingZeros'::`2': : notFound;
    totalBitsNeeded = 32 - (v1 ^ 31);
    for (smodelLightGlob.local.entryLimit = (1 << totalBitsNeeded) - modelLightGlob.xmodelEntryLimit;
        smodelLightGlob.local.entryLimit < 0x800;
        smodelLightGlob.local.entryLimit += 1 << totalBitsNeeded++)
    {
        ;
    }

    iassert(smodelLightGlob.local.entryLimit <= 4096);

    modelLightGlob.totalEntryLimit = 1 << totalBitsNeeded;
    modelLightGlob.entryBitsY = totalBitsNeeded - 6;
    modelLightGlob.imageHeight = 1 << (totalBitsNeeded - 6 + 2);
    modelLightGlob.invImageHeight = 1.0 / (double)(uint32_t)(1 << (totalBitsNeeded - 6 + 2));
    modelLightGlob.xmodel.baseIndex = smodelLightGlob.local.entryLimit;

    iassert(!(modelLightGlob.xmodelEntryLimit & 31));

    modelLightGlob.pixelFreeBitsSize = modelLightGlob.xmodelEntryLimit >> 3;
    modelLightGlob.pixelFreeBitsWordCount = modelLightGlob.xmodelEntryLimit >> 5;
    modelLightGlob.lightingOrigins = (float (*)[3])R_AllocModelLightingGlobal(12 * modelLightGlob.xmodelEntryLimit);

    for (i = 0; i < 4; ++i)
        modelLightGlob.pixelFreeBits[i] = (uint32_t *)R_AllocModelLightingGlobal(modelLightGlob.pixelFreeBitsSize);

    modelLightGlob.lightingInfo = (GfxLightingInfo *)R_AllocModelLightingGlobal(2 * modelLightGlob.xmodelEntryLimit);
    modelLightGlob.image = modelLightGlob.lightImages[0];
}

char *__cdecl R_AllocModelLightingGlobal(uint32_t bytes)
{
    return Z_VirtualAlloc(bytes, "R_AllocModelLightingGlobal", 18);
}

void __cdecl R_ResetModelLighting()
{
    float *v1; // [esp+0h] [ebp-14h]
    uint32_t entryIndex; // [esp+4h] [ebp-10h]
    uint32_t usedIndex; // [esp+8h] [ebp-Ch]
    uint32_t i; // [esp+Ch] [ebp-8h]
    uint32_t smodelIndex; // [esp+10h] [ebp-4h]

    for (i = 0; i < 4; ++i)
        Com_Memset(modelLightGlob.pixelFreeBits[i], 255, modelLightGlob.pixelFreeBitsSize);

    for (usedIndex = 0; usedIndex < modelLightGlob.xmodelEntryLimit; ++usedIndex)
    {
        v1 = modelLightGlob.lightingOrigins[usedIndex];
        v1[0] = FLT_MAX;
        v1[1] = FLT_MAX;
        v1[2] = FLT_MAX;
    }

    iassert(smodelLightGlob.local.assignedCount == 0 || rgp.world != NULL);

    for (entryIndex = 0; entryIndex < smodelLightGlob.local.assignedCount; ++entryIndex)
    {
        smodelIndex = smodelLightGlob.local.smodelIndex[entryIndex];
        iassert(smodelIndex < rgp.world->dpvs.smodelCount);
        iassert(entryIndex == R_ModelLightingIndexFromHandle(rgp.world->dpvs.smodelDrawInsts[smodelIndex].lightingHandle));
        rgp.world->dpvs.smodelDrawInsts[smodelIndex].lightingHandle = 0;
    }

    smodelLightGlob.local.assignedCount = 0;
}

void __cdecl R_InitModelLightingImage()
{
    bool useAltUpdate; // [esp+1h] [ebp-1h]

    iassert(modelLightGlob.totalEntryLimit);

    useAltUpdate = Dvar_GetBool("r_altModelLightingUpdate");
    modelLightGlob.lightImages[0] = Image_AllocProg(12, IMG_CATEGORY_RAW, TS_FUNCTION);
    if (useAltUpdate)
    {
        Image_SetupAndLoad(modelLightGlob.lightImages[0], 256, modelLightGlob.imageHeight, 4, IMG_FLAG_NOMIPMAPS | IMG_FLAG_VOLMAP | IMG_FLAG_DYNAMIC, D3DFMT_A8R8G8B8);
        modelLightGlob.lightImages[1] = Image_AllocProg(13, IMG_CATEGORY_RAW, TS_FUNCTION);
        Image_SetupAndLoad(modelLightGlob.lightImages[1], 256, modelLightGlob.imageHeight, 4, IMG_FLAG_NOMIPMAPS | IMG_FLAG_VOLMAP | IMG_FLAG_SYSTEMMEM, D3DFMT_A8R8G8B8);
    }
    else
    {
        Image_SetupAndLoad(modelLightGlob.lightImages[0], 256, modelLightGlob.imageHeight, 4, IMG_FLAG_NOMIPMAPS | IMG_FLAG_VOLMAP, D3DFMT_A8R8G8B8);
        modelLightGlob.lightImages[1] = 0;
    }
    modelLightGlob.image = modelLightGlob.lightImages[0];
}

void __cdecl R_ShutdownModelLightingImage()
{
    int imgIndex; // [esp+0h] [ebp-4h]

    iassert(modelLightGlob.totalEntryLimit);

    for (imgIndex = 0; imgIndex < 2; ++imgIndex)
    {
        if (modelLightGlob.lightImages[imgIndex])
        {
            Image_Release(modelLightGlob.lightImages[imgIndex]);
            modelLightGlob.lightImages[imgIndex] = 0;
        }
    }
    modelLightGlob.image = 0;
}

void __cdecl R_InitStaticModelLighting()
{
    smodelLightGlob.local.assignedCount = 0;
    smodelLightGlob.local.freeableCount = 0;
}

void __cdecl R_ApplyLightGridColorsPatch(const GfxModelLightingPatch *patch, uint8_t *pixels)
{
    GfxLightGridColors packed; // [esp+170h] [ebp-A8h] BYREF

    if (patch->colorsCount == 1)
    {
        R_SetLightGridColors(&rgp.world->lightGrid.colors[patch->colorsIndex[0]], patch->primaryLightWeight, pixels);
    }
    else
    {
        R_FixedPointBlendLightGridColors(
            &rgp.world->lightGrid,
            patch->colorsIndex,
            (unsigned short*)patch->colorsWeight,
            patch->colorsCount,
            &packed);
        R_SetLightGridColors(&packed, patch->primaryLightWeight, pixels);
    }
}

void __cdecl RB_PatchModelLighting(const GfxModelLightingPatch *patchList, uint32_t patchCount)
{
    uint32_t modelLightingIndex; // [esp+4h] [ebp-4Ch]
    int v6; // [esp+8h] [ebp-48h]
    int v7; // [esp+Ch] [ebp-44h]
    int hr; // [esp+10h] [ebp-40h]
    uint8_t *pixels; // [esp+18h] [ebp-38h]
    uint32_t sampleIndex; // [esp+1Ch] [ebp-34h]
    uint32_t patchIter; // [esp+20h] [ebp-30h]
    const GfxModelLightingPatch *patch; // [esp+24h] [ebp-2Ch]
    uint32_t y0; // [esp+28h] [ebp-28h]
    GfxImage *lightImage; // [esp+2Ch] [ebp-24h]
    _D3DBOX dirtyBox; // [esp+30h] [ebp-20h] BYREF
    uint32_t lockValue; // [esp+48h] [ebp-8h]
    bool useAltUpdate; // [esp+4Fh] [ebp-1h]
    const bool diagnosticsEnabled = com_diagMarkers && com_diagMarkers->current.enabled;

    if (patchCount)
    {
        iassert(modelLightGlob.lockedBox.pBits == NULL);
        useAltUpdate = Dvar_GetBool("r_altModelLightingUpdate");
        if (useAltUpdate)
            lightImage = modelLightGlob.lightImages[1];
        else
            lightImage = modelLightGlob.lightImages[0];

        iassert(lightImage);

        if (useAltUpdate)
        {
            memset(&dirtyBox, 0, 20);
            dirtyBox.Back = 4;
            lockValue = 0x8000;
        }
        else
        {
            lockValue = 0;
        }
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("lightImage->texture.volmap->LockBox( 0, &modelLightGlob.lockedBox, 0, lockValue )\n");
            hr = lightImage->texture.volmap->LockBox(0, &modelLightGlob.lockedBox, 0, lockValue);
            if (hr < 0)
            if (diagnosticsEnabled)
            {
                do
                {
                    ++g_disableRendering;
                    Com_Error(
                        ERR_FATAL,
                        ".\\r_model_lighting.cpp (%i) lightImage->texture.volmap->LockBox( 0, &modelLightGlob.lockedBox, 0, lockValue ) failed: %s\n",
                        916,
                        R_ErrorDescription(hr));
                } while (alwaysfails);
            }
        } while (alwaysfails);
        R_SetModelLightingSampleDeltas();
        R_SetLightGridSampleDeltas(modelLightGlob.lockedBox.RowPitch, modelLightGlob.lockedBox.SlicePitch);
        uint32_t firstEntry = 0;
        uint32_t firstColorsCount = 0;
        uint32_t firstColorsIndex = 0;
        uint32_t firstWeight = 0;
        uint32_t firstIsGround = 0;
        uint8_t firstSample[4] = {0, 0, 0, 0};
        // per-call values, accumulated across the run.
        uint32_t callNonzeroBytes = 0;
        // read the sentinel back out of the MAPPED box itself. If the four
        // slice groups read back as their own tags, the mapping spans four
        // slices and the loss is in the copy to the image; if they all read the
        // last tag, the mapped pointer aliases and the guest's own writes are
        // overwriting each other before any copy happens.
        unsigned char sentinelReadback[4] = {0, 0, 0, 0};
        uint32_t sentinelReadbackDone = 0;
        uint32_t callZeroBytes = 0;
        uint32_t callZeroBlocks = 0;
        for (patchIter = 0; patchIter < patchCount; ++patchIter)
        {
            patch = &patchList[patchIter];
            modelLightingIndex = patch->modelLightingIndex;
            y0 = (modelLightingIndex >> 4) & 0xFFFFFFFC;
            if (useAltUpdate)
            {
                dirtyBox.Left = 4 * (modelLightingIndex & 0x3F);
                dirtyBox.Right = dirtyBox.Left + 4;
                dirtyBox.Top = (modelLightingIndex >> 4) & 0xFFFFFFFC;
                dirtyBox.Bottom = y0 + 4;
                lightImage->texture.volmap->AddDirtyBox(&dirtyBox);
            }
            pixels = (unsigned char*)modelLightGlob.lockedBox.pBits
                + 16 * (modelLightingIndex & 0x3F)
                + modelLightGlob.lockedBox.RowPitch * y0;
            if (patch->colorsCount)
            {
                R_ApplyLightGridColorsPatch(patch, pixels);
            }
            // SENTINEL (diagnostic, remove after use): stamp each of the 64
            // samples with its own depth-slice index instead of lighting, so the
            // host can find where each slice physically landed. Counting
            // non-zero bytes cannot answer that -- the texture is dense enough
            // that a wrong address formula still hits data almost everywhere,
            // which is exactly how an earlier "the bytes are at gobZ=1" reading
            // fooled me. A per-slice value cannot be faked by density.
            if (Dvar_GetBool("r_killhouseMLSentinel"))
            {
                for (uint32_t s = 0; s < 0x40; ++s)
                {
                    unsigned char *px = (unsigned char *)&pixels[s_modelLightingSampleDelta[s]];
                    const unsigned char tag = (unsigned char)(0xE0u + (s >> 4));
                    px[0] = tag;
                    px[1] = tag;
                    px[2] = tag;
                }

                if (!sentinelReadbackDone)
                {
                    sentinelReadbackDone = 1;
                    for (uint32_t g = 0; g < 4; ++g)
                    {
                        sentinelReadback[g] =
                            ((const unsigned char *)&pixels[s_modelLightingSampleDelta[g * 16]])[0];
                    }
                }
            }
            if (!patch->colorsCount)
            {
                for (sampleIndex = 0; sampleIndex < 0x40; ++sampleIndex)
                {
                    //*(_DWORD *)&pixels[s_modelLightingSampleDelta[sampleIndex]] = *(_DWORD *)patch->groundLighting;
                    unsigned char *pPixels = (unsigned char*)&pixels[s_modelLightingSampleDelta[sampleIndex]];
                    pPixels[0] = patch->groundLighting[0];
                    pPixels[1] = patch->groundLighting[1];
                    pPixels[2] = patch->groundLighting[2];
                    pPixels[3] = patch->groundLighting[3];
                }
            }
            // measure the values that were just written into the
            // locked box (the exact bytes UnlockBox/UpdateTexture hand to the
            // device), and for a named shelf/locker/crate compare the sampled
            // texel against the light-grid entry the patch resolved to.
            if (diagnosticsEnabled)
            {
                const uint32_t blockNonzero = ModelLightingBlockNonzeroBytes(pixels);
                callNonzeroBytes += blockNonzero;
                callZeroBytes += 4u * kModelLightingSampleCount - blockNonzero;
                if (!blockNonzero)
                    ++callZeroBlocks;
                for (uint32_t slot = 0; slot < kModelLightingNamedSlotCount; ++slot)
                {
                    ModelLightingNamedSlot &record = s_modelLightingNamed[slot];
                    if (!record.inUse || record.reported ||
                        record.entryIndex != (uint16_t)modelLightingIndex)
                        continue;
                    const uint32_t applied = *(const uint32_t *)pixels;
                    const uint32_t expected = ModelLightingExpectedFirstTexel(patch);
                    record.reported = true;
                    record.matched = applied == expected;
                    if (s_modelLightingNamedLines < kModelLightingNamedLineLimit)
                    {
                        ++s_modelLightingNamedLines;
                        Com_Printf(
                            0,
                            "KILLHOUSE_MODELLIGHT_NAMED name=%s smodel=%u handle=%u entry=%u "
                            "ground=%u colors=%u weight=%u colors_index=%u expected=%08x "
                            "applied=%08x match=%u block_nonzero=%u frame=%u\n",
                            record.name, record.smodelIndex, record.lightingHandle,
                            record.entryIndex, record.groundLightingPacked, patch->colorsCount,
                            patch->primaryLightWeight, record.colorsIndex0, expected, applied,
                            record.matched ? 1u : 0u, blockNonzero, rg.frontEndFrameCount);
                    }
                }
            }
            // Diagnostic-only capture (feeds the KILLHOUSE_MODELLIGHT print
            // below); skip the work entirely when com_diagMarkers is off.
            if (diagnosticsEnabled && patchIter == 0)
            {
                firstEntry = modelLightingIndex;
                firstColorsCount = patch->colorsCount;
                firstColorsIndex = patch->colorsCount ? patch->colorsIndex[0] : 0u;
                firstWeight = patch->primaryLightWeight;
                firstIsGround = patch->colorsCount ? 0u : 1u;
                firstSample[0] = pixels[0];
                firstSample[1] = pixels[1];
                firstSample[2] = pixels[2];
                firstSample[3] = pixels[3];
            }
        }
        // cumulative run totals, so the verifier can read the final
        // line inside the verifier's walk window without summing lines itself.
        if (diagnosticsEnabled)
        {
            s_modelLightingTotalPatches += patchCount;
            s_modelLightingTotalBlocks += patchCount;
            s_modelLightingTotalNonzeroBytes += callNonzeroBytes;
            s_modelLightingTotalZeroBytes += callZeroBytes;
            s_modelLightingTotalZeroBlocks += callZeroBlocks;
            if (s_modelLightingReportLines < kModelLightingReportLineLimit)
            {
                ++s_modelLightingReportLines;
                Com_Printf(
                    0,
                    "KILLHOUSE_MODELLIGHT frame=%u alt=%u rowpitch=%d slicepitch=%d "
                    "delta0=%d delta16=%d delta32=%d delta48=%d readback=%02x,%02x,%02x,%02x "
                    "patches=%u nonzero_bytes=%u "
                    "zero_bytes=%u allzero_blocks=%u total_patches=%u total_blocks=%u "
                    "total_nonzero_bytes=%llu total_zero_bytes=%llu total_allzero_blocks=%u "
                    "first_entry=%u first_colors_index=%u first_colors_count=%u first_weight=%u "
                    "first_ground=%u first_sample=%02x%02x%02x%02x\n",
                    rg.frontEndFrameCount, useAltUpdate ? 1u : 0u,
                    modelLightGlob.lockedBox.RowPitch, modelLightGlob.lockedBox.SlicePitch,
                    s_modelLightingSampleDelta[0], s_modelLightingSampleDelta[16],
                    s_modelLightingSampleDelta[32], s_modelLightingSampleDelta[48],
                    sentinelReadback[0], sentinelReadback[1], sentinelReadback[2],
                    sentinelReadback[3], patchCount, callNonzeroBytes, callZeroBytes,
                    callZeroBlocks, s_modelLightingTotalPatches, s_modelLightingTotalBlocks,
                    (unsigned long long)s_modelLightingTotalNonzeroBytes,
                    (unsigned long long)s_modelLightingTotalZeroBytes,
                    s_modelLightingTotalZeroBlocks, firstEntry, firstColorsIndex,
                    firstColorsCount, firstWeight, firstIsGround, firstSample[0],
                    firstSample[1], firstSample[2], firstSample[3]);
            }
        }

        iassert(modelLightGlob.lockedBox.pBits);
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("lightImage->texture.volmap->UnlockBox( 0 )\n");
            v7 = lightImage->texture.volmap->UnlockBox(0);
            if (v7 < 0)
            {
                do
                {
                    ++g_disableRendering;
                    Com_Error(
                        ERR_FATAL,
                        ".\\r_model_lighting.cpp (%i) lightImage->texture.volmap->UnlockBox( 0 ) failed: %s\n",
                        949,
                        R_ErrorDescription(v7));
                } while (alwaysfails);
            }
        } while (alwaysfails);

        modelLightGlob.lockedBox.pBits = 0;

        if (useAltUpdate)
        {
            do
            {
                if (r_logFile && r_logFile->current.integer)
                    RB_LogPrint("dx.device->UpdateTexture( lightImage->texture.volmap, modelLightGlob.lightImages[0]->texture.volmap )\n");
                v6 = dx.device->UpdateTexture(lightImage->texture.volmap, modelLightGlob.lightImages[0]->texture.volmap);
                if (v6 < 0)
                {
                    do
                    {
                        ++g_disableRendering;
                        Com_Error(
                            ERR_FATAL,
                            ".\\r_model_lighting.cpp (%i) dx.device->UpdateTexture( lightImage->texture.volmap, modelLightGlob.lightIma"
                            "ges[0]->texture.volmap ) failed: %s\n",
                            953,
                            R_ErrorDescription(v6));
                    } while (alwaysfails);
                }
            } while (alwaysfails);
        }
    }
}
