#pragma once
#include "rb_backend.h"
#include "rb_tess.h"
#include <cstring>

// LP64 fix (PRIM_DRAW_SURF_PTR_WORDS, r_gfx.h; writer: R_WritePrimDrawSurfPtr,
// r_add_cmdbuf.cpp): reconstructs the one raw XSurface* the static-model
// prim-draw-surf stream carries from PRIM_DRAW_SURF_PTR_WORDS consecutive
// uint32_t words, byte-for-byte via memcpy. Does not advance `words`; callers
// own their own cursor arithmetic exactly as they did for the old one-word
// read, just walking PRIM_DRAW_SURF_PTR_WORDS words instead of one.
static inline XSurface *R_ReadPrimDrawSurfXSurfacePtr(const uint32_t *words)
{
    XSurface *value = nullptr;
    memcpy(&value, words, sizeof(XSurface *));
    return value;
}

struct GfxStaticModelDrawStream // sizeof=0x1C
{                                       // ...
    const uint32_t *primDrawSurfPos; // ...
    const GfxTexture *reflectionProbeTexture; // ...
    uint32_t customSamplerFlags;    // ...
    XSurface *localSurf;
    uint32_t smodelCount;
    const uint16_t *smodelList;
    uint32_t reflectionProbeIndex;
};

void __cdecl R_DrawStaticModelSurfLit(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
int __cdecl R_GetNextStaticModelSurf(GfxStaticModelDrawStream *drawStream, XSurface **outSurf);
void __cdecl R_DrawStaticModelSurf(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
void __cdecl R_DrawStaticModelDrawSurfNonOptimized(GfxStaticModelDrawStream *drawStream, GfxCmdBufContext context);
void __cdecl R_SetStaticModelVertexBuffer(GfxCmdBufPrimState *primState, XSurface *xsurf);
void __cdecl R_DrawStaticModelDrawSurfPlacement(
    const GfxStaticModelDrawInst *smodelDrawInst,
    GfxCmdBufSourceState *source);
void __cdecl R_DrawStaticModelDrawSurfLightingNonOptimized(
    GfxStaticModelDrawStream *drawStream,
    GfxCmdBufContext context);

void __cdecl R_DrawStaticModelCachedSurfLit(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
void __cdecl R_DrawStaticModelCachedSurf(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
void __cdecl R_SetupCachedStaticModelLighting(GfxCmdBufSourceState *source);
int __cdecl R_ReadStaticModelPreTessDrawSurf(
    GfxReadCmdBuf *readCmdBuf,
    GfxStaticModelPreTessSurf *pretessSurf,
    uint32_t *firstIndex,
    uint32_t *count);
// staticList: the list of a record written with R_PRETESS_STATIC_FLAG
// (r_pretess.h; the caller reads it after R_ReadStaticModelPreTessDrawSurf),
// else null.
void __cdecl R_DrawStaticModelsPreTessDrawSurf(
    GfxStaticModelPreTessSurf pretessSurf,
    uint32_t firstIndex,
    uint32_t count,
    GfxCmdBufContext context,
    const uint16_t *staticList = nullptr);
void __cdecl R_DrawStaticModelsPreTessDrawSurfLighting(
    GfxStaticModelPreTessSurf pretessSurf,
    uint32_t firstIndex,
    uint32_t count,
    GfxCmdBufContext context,
    const uint16_t *staticList = nullptr);
// True when the surface's triangle indices sit in its zone's static index
// buffer (the static pretess path draws it without copying); returns
// the buffer and first index.
bool R_StaticModelSurfHasStaticIndices(
    const XSurface *xsurf,
    IDirect3DIndexBuffer9 **ib = nullptr,
    int32_t *baseIndex = nullptr);

void __cdecl R_DrawStaticModelSkinnedSurf(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
void __cdecl R_DrawStaticModelSkinnedSurfLit(const uint32_t *primDrawSurfPos, GfxCmdBufContext context);
void __cdecl R_DrawStaticModelsSkinnedDrawSurf(GfxStaticModelDrawStream *drawStream, GfxCmdBufContext context);

uint32_t __cdecl R_ReadPrimDrawSurfInt(GfxReadCmdBuf *cmdBuf);
const uint32_t *__cdecl R_ReadPrimDrawSurfData(GfxReadCmdBuf *cmdBuf, uint32_t count);