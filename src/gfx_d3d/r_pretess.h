#pragma once
#include "r_rendercmds.h"
#include "r_drawsurf.h"

#define MAX_DRAWSURFS 0x8000

enum DrawSurfType : __int32;

struct GfxBspPreTessDrawSurf // sizeof=0x4
{                                       // ...
    uint16_t baseSurfIndex;     // ...
    uint16_t totalTriCount;     // ...
};

void __cdecl R_InitDrawSurfListInfo(GfxDrawSurfListInfo *info);
void __cdecl R_EmitDrawSurfList(GfxDrawSurf *drawSurfs, uint32_t drawSurfCount);
void __cdecl R_MergeAndEmitDrawSurfLists(DrawSurfType firstStage, int stageCount);
uint32_t __cdecl R_EmitDrawSurfListForKey(
    const GfxDrawSurf *drawSurfs,
    uint32_t drawSurfCount,
    uint32_t primarySortKey);


uint16_t *__cdecl R_AllocPreTessIndices(int count);

void __cdecl R_EndPreTess();
void __cdecl R_BeginPreTess();

int __cdecl R_ReadBspPreTessDrawSurfs(
    struct GfxReadCmdBuf *cmdBuf,
    const struct GfxBspPreTessDrawSurf **list,
    uint32_t *count,
    uint32_t *baseIndex);
// ---- Static world index buffer (r_deko9StaticPretess) ------------------------
// rgp.world->indices never change after load, so the world's pre-tessellated
// batches need not be copied into the per-frame pretess index buffer: with
// the world indices in one static GPU index buffer, a batch is drawn as the
// ranges its visible surfaces already occupy there (consecutive surfaces
// whose indices follow each other merge into one range). A batch recorded
// this way carries R_PRETESS_STATIC_FLAG in place of its pretess firstIndex;
// its GfxBspPreTessDrawSurf entries are then runs (baseSurfIndex = the run's
// first surface, totalTriCount = its triangles), each one contiguous range
// of the static buffer with one firstVertex, lightmap and reflection probe.
// deko3d builds only (the ranges go to Deko9_DrawIndexedRanges); elsewhere
// R_StaticPretessWorldIb is always null and nothing changes.
#define R_PRETESS_STATIC_FLAG 0x80000000u

struct GfxWorld;
struct IDirect3DIndexBuffer9;

// World load/unload (r_bsp.cpp): (re)build or free the static buffer.
void R_StaticPretessSetWorld(const GfxWorld *world);
void R_StaticPretessRelease();
// The static buffer for rgp.world when r_deko9StaticPretess is on, else null.
IDirect3DIndexBuffer9 *R_StaticPretessWorldIb();
// Static buffer for rgp.world regardless of the dvar (a batch recorded
// static before the dvar changed still draws), null when there is none.
IDirect3DIndexBuffer9 *R_StaticPretessWorldIbAny();
// Static-model cached lists (r_deko9StaticPretessModels, independent of the world dvar):
// a list's instances all draw the same XSurface triangles, whose indices
// already sit in the zone's static index buffer; per instance only the base
// vertex (its cache slot) differs. A list recorded this way carries
// R_PRETESS_STATIC_FLAG in place of its pretess firstIndex, followed by the
// list of cached-surface indices ((count + 1) / 2 words), and draws one
// range per instance. True when new lists should be recorded that way.
bool R_StaticPretessModels();
