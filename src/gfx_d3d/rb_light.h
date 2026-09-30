#pragma once
#include "r_gfx.h"
#include "r_model_lighting.h"

enum GfxSortedHistoryAdd : __int32
{                                       // ...
    SH_ADD_NEVER = 0x0,
    SH_ADD_IF_NEW = 0x1,
};

#define MODELSURF_MAX_VERTICES 0x8000

void __cdecl R_SetLightGridSampleDeltas(int rowStride, int sliceStride);
void __cdecl R_ShowLightVisCachePoints(const float *viewOrigin, const DpvsPlane *clipPlanes, int clipPlaneCount);
int __cdecl R_SortedHistoryEntry(int x, int y, int z, GfxSortedHistoryAdd addMode);
char __cdecl R_AddSortedHistoryEntry(uint16_t x, uint16_t y, uint16_t z, int pos);
void __cdecl R_SetLightGridColors(
    const GfxLightGridColors *colors,
    uint8_t packedSunWeight,
    uint8_t *pixels);
void __cdecl R_FixedPointBlendLightGridColors(
    const GfxLightGrid *lightGrid,
    const uint16_t *colorsIndex,
    uint16_t *fixedPointWeight,
    uint32_t colorsCount,
    GfxLightGridColors *outPacked);
void __cdecl R_ScaleLightGridColors(
    const GfxLightGridColors *colors,
    uint16_t fixedPointWeight,
    uint16_t *scaled);
void __cdecl R_WeightedAccumulateLightGridColors(
    const GfxLightGridColors *colors,
    uint16_t fixedPointWeight,
    uint16_t *accumulated);
void __cdecl R_PackAccumulatedLightGridColors(const uint16_t *accumulated, GfxLightGridColors *packed);
uint8_t __cdecl R_GetPrimaryLightFromGrid(
    const GfxLightGrid *lightGrid,
    const float *samplePos,
    uint8_t sunPrimaryLightIndex);
uint8_t __cdecl R_LightGridLookup(
    const GfxLightGrid *lightGrid,
    const float *samplePos,
    float *cornerWeight,
    const GfxLightGridEntry **cornerEntry,
    uint32_t *defaultGridEntry);
void __cdecl R_ShowLightGrid(
    const GfxLightGrid *lightGrid,
    const uint32_t *pos,
    const float *samplePos,
    const GfxLightGridEntry **cornerEntry,
    bool *suppressEntry,
    bool honorSuppression);
void __cdecl R_ShowGridOrigin(const float *origin);
void __cdecl R_ShowGridBox(const uint32_t *pos);
void __cdecl R_ShowGridCorner(uint32_t x, uint32_t y, uint32_t z, float halfSize, const float *color);
void __cdecl R_UpdateVisHistory(const GfxLightGrid *lightGrid, const uint32_t *pos);
void __cdecl R_GetLightGridSampleEntryQuad(
    const GfxLightGrid *lightGrid,
    const uint32_t *pos,
    const GfxLightGridEntry **entries,
    uint32_t *defaultGridEntry);
bool __cdecl R_IsValidLightGridSample(
    const GfxLightGrid *lightGrid,
    const GfxLightGridEntry *entry,
    char cornerIndex,
    const uint32_t *pos,
    const float *samplePos);
uint32_t __cdecl R_GetLightingAtPoint(
    const GfxLightGrid *lightGrid,
    const float *samplePos,
    uint32_t nonSunPrimaryLightIndex,
    uint16_t dest,
    GfxModelLightExtrapolation extrapolateBehavior);
void __cdecl R_SetLightGridColorsFromIndex(
    const GfxLightGrid *lightGrid,
    uint32_t colorsIndex,
    float primaryLightWeight,
    uint16_t dest);
void __cdecl R_BlendAndSetLightGridColors(
    const GfxLightGrid *lightGrid,
    uint8_t *colorsIndex,
    const float *colorsWeight,
    uint32_t colorsCount,
    float primaryLightWeight,
    float weightNormalizeScale,
    uint16_t dest);
void __cdecl R_GetLightGridColorsFixedPointBlendWeights(
    const float *colorsWeight,
    uint32_t colorsCount,
    float weightNormalizeScale,
    uint16_t *fixedPointWeight);
uint8_t __cdecl R_ExtrapolateLightingAtPoint(
    const GfxLightGrid *lightGrid,
    uint16_t dest,
    GfxModelLightExtrapolation extrapolateBehavior,
    uint32_t defaultGridEntry);
uint32_t __cdecl R_AddLightGridSample(
    uint16_t *sampleColors,
    float *sampleWeight,
    uint32_t sampleCount,
    uint16_t sampleColorsAdd,
    float sampleWeightAdd);
char __cdecl R_CanLightInfluenceLightGridCorner(
    const GfxLightGrid *lightGrid,
    const ComPrimaryLight *light,
    const float *samplePos,
    char cornerIndex);
void __cdecl R_GetAverageLightingAtPoint(const float *samplePos, uint8_t *outColor);
void __cdecl R_BlendAndAverageLightGridColors(
    const GfxLightGrid *lightGrid,
    const uint16_t *colorsIndex,
    const float *colorsWeight,
    uint32_t colorsCount,
    float primaryLightWeight,
    float weightNormalizeScale,
    uint8_t *outAverage);
void __cdecl R_AverageLightGridColors(const GfxLightGridColors *colors, float sunWeight, uint8_t *outColor);
void __cdecl R_InitLightVisHistory(char *bspName);
void __cdecl R_LightVisHistoryFilename(char *bspName, char *filename);
void __cdecl R_SaveLightVisHistory();
uint8_t __cdecl R_GetPrimaryLightForModel(
    const XModel *model,
    const float *origin,
    const float (*axis)[3],
    float scale,
    const float *mins,
    const float *maxs,
    const GfxLightRegion *lightRegions);

GfxModelLightingPatch *__cdecl R_BackEndDataAllocAndClearModelLightingPatch(GfxBackEndData *frontEndDataOut);

// apply probe: bounded, always-on measurement of what the real
// R_LightGridLookup + R_GetLightingAtPoint actually resolved for one named
// static model's lighting origin, so the production log can show the
// world-position -> cell step and the primary/sun term instead of only the
// resulting model-lighting texel.  R_SetStaticModelLighting brackets its own
// R_CalcModelLighting call with Begin/End; the record is read back there and
// emitted as KILLHOUSE_LIGHTGRID_APPLY, bounded to the named-slot limit.
struct R_LightGridApplyProbe
{
    uint32_t active;
    uint32_t recorded;
    uint32_t entryIndex; // model-lighting destination entry being watched
    uint32_t rawCaptured;      // R_LightGridLookup's pre-suppression quad
    uint32_t rawDefaultGridEntry;
    uint32_t rawPos[3];
    uint32_t rawEntryIndex[8]; // entry - lightGrid->entries, or ~0u
    uint16_t rawColors[8];
    uint8_t rawPrimary[8];
    uint8_t rawNeedsTrace[8];
    uint32_t defaultGridEntry;
    uint32_t chosenPrimary;  // raw R_LightGridLookup return
    uint32_t mappedPrimary;  // after the 255 -> sun and region remap
    uint32_t visibleMilli;   // primaryVisibleWeight * 1000
    uint32_t occludedMilli;  // primaryOccludedWeight * 1000
    uint32_t sampleCount;
    uint32_t hasLightRegions;
    uint32_t sunPrimaryLightIndex;      // lightGrid->sunPrimaryLightIndex
    uint32_t worldSunPrimaryLightIndex; // rgp.world->sunPrimaryLightIndex
    uint32_t primaryLightCount;         // rgp.world->primaryLightCount
    uint32_t comPrimaryLightCount;      // Com_GetPrimaryLightCount()
    uint32_t cornerEntryIndex[8];       // entry - lightGrid->entries, or ~0u
    uint16_t cornerColors[8];
    uint8_t cornerPrimary[8];
    uint8_t cornerNeedsTrace[8];
    uint32_t cornerWeightMilli[8];
};
void __cdecl R_LightGridApplyProbeBegin(uint32_t entryIndex);
void __cdecl R_LightGridApplyProbeEnd(void);
const R_LightGridApplyProbe *__cdecl R_LightGridApplyProbeRead(void);