#pragma once
#include "r_gfx.h"
#include "r_bsp.h"

#define R_MAX_PRETESS_INDICES 0x100000

// Why R_GetStaticModelId routed an instance where it did (cumulative counts).
enum SModelFunnel
{
    SMODEL_FUNNEL_CACHED,
    SMODEL_FUNNEL_RIGID_DISABLED,   // r_smc_enable off
    SMODEL_FUNNEL_RIGID_INELIGIBLE, // LOD has no smcIndexPlusOne in the fastfile
    SMODEL_FUNNEL_RIGID_ALLOC_FAIL, // cache eligible but R_CacheStaticModelSurface failed
    SMODEL_FUNNEL_SKINNED,
    SMODEL_FUNNEL_COUNT
};
extern uint32_t g_smodelFunnelStats[SMODEL_FUNNEL_COUNT];

struct GfxSModelDrawSurfData // sizeof=0x18
{                                       // ...
    GfxDelayedCmdBuf delayedCmdBuf;
    GfxDrawSurfList drawSurfList;       // ...
};

// LP64 fix (PRIM_DRAW_SURF_PTR_WORDS, r_gfx.h): total uint32_t words a
// R_AddDelayedStaticModelDrawSurf(..., count) call consumes -- one count
// word, PRIM_DRAW_SURF_PTR_WORDS for the raw XSurface* (one word on the
// ILP32 reference ABI, two on LP64), then the packed uint8 list. Every
// R_AllocDrawSurf(...) call sizing that reservation must use this instead
// of the old ILP32-only "((count + 1) >> 1) + 2" literal.
static inline uint32_t R_StaticModelDrawSurfWordCount(uint32_t count)
{
    return ((count + 1) >> 1) + 1 + PRIM_DRAW_SURF_PTR_WORDS;
}

void __cdecl R_AddDelayedStaticModelDrawSurf(
    GfxDelayedCmdBuf *delayedCmdBuf,
    struct XSurface *xsurf,
    uint8_t *list,
    uint32_t count);
void __cdecl R_EndDumpStaticModelLodInfo();
void __cdecl R_AddAllStaticModelSurfacesCamera();
void __cdecl R_SkinStaticModelsCameraForLod(
    const XModel *model,
    uint8_t primaryLightIndex,
    uint8_t *list,
    uint32_t count,
    uint32_t surfType,
    uint32_t lod,
    GfxSModelDrawSurfLightingData *surfData);
void __cdecl R_SkinStaticModelsCamera(
    const XModel *model,
    uint8_t primaryLightIndex,
    uint16_t (*staticModelLodList)[4][128],
    uint16_t (*staticModelLodCount)[4],
    GfxSModelDrawSurfLightingData *surfData);
void __cdecl R_SkinStaticModelsCameraForSurface(
    const XModel *model,
    uint8_t primaryLightIndex,
    uint16_t (*staticModelLodList)[128],
    uint16_t *staticModelLodCount,
    uint32_t surfType,
    GfxSModelDrawSurfLightingData *surfData);
void __cdecl R_ShowCountsStaticModel(int smodelIndex, int lod);
void __cdecl R_DumpStaticModelLodInfo(const GfxStaticModelDrawInst *smodelDrawInst, float dist);
void __cdecl R_StaticModelWriteInfo(int fileHandle, const GfxStaticModelDrawInst *smodelDrawInst, const float dist);
void __cdecl R_SortAllStaticModelSurfacesCamera();
void __cdecl R_SortAllStaticModelSurfacesSunShadow();
void __cdecl R_AddAllStaticModelSurfacesSunShadow();
void __cdecl R_AddAllStaticModelSurfacesRangeSunShadow(uint32_t partitionIndex, uint32_t maxDrawSurfCount);
void __cdecl R_SkinStaticModelsShadowForLod(
    const XModel *model,
    uint8_t *list,
    uint32_t count,
    uint32_t surfType,
    uint32_t lod,
    GfxSModelDrawSurfData *surfData);
void __cdecl R_SkinStaticModelsShadow(
    const XModel *model,
    uint16_t (*staticModelLodList)[4][128],
    uint16_t (*staticModelLodCount)[4],
    GfxSModelDrawSurfData *surfData);
void __cdecl R_SkinStaticModelsShadowForSurface(
    const XModel *model,
    uint16_t (*staticModelLodList)[128],
    uint16_t *staticModelLodCount,
    uint32_t surfType,
    GfxSModelDrawSurfData *surfData);
void __cdecl R_AddAllStaticModelSurfacesSpotShadow(uint32_t spotShadowIndex, uint32_t primaryLightIndex);
