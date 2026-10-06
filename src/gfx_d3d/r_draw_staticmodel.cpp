#include <universal/q_shared.h>
#include "r_draw_staticmodel.h"
#include "r_dvars.h"
#include "rb_stats.h"
#include <database/database.h>
#include "r_state.h"
#include "r_model_lighting.h"
#include "r_shade.h"
#include "r_draw_bsp.h"
#include "r_pretess.h"
#include "r_utils.h"
#include "r_staticmodelcache.h"
#include "rb_tess.h"
#include "r_xsurface.h"
#include "r_material.h"
#include <platform/switch/switch_diag_dvars.h>
#include "r_init.h"
#include <deko9/deko9_baked.h>
#include <deko9/deko9_native.h>
#include <port/switch_shader_prebake.h>

// Pre-draw bounds checks that turn an OOB static-model
// draw into a counted skip instead of
// VK_ERROR_DEVICE_LOST. Core validation cannot catch bad index contents;
// only the CPU can, before submission. Prints are capped so a fully-bad
// frame cannot flood the guest log; the skip counter is exact.
namespace
{
uint32_t g_staticGuardSkipped = 0;
uint32_t g_staticGuardPrints = 0;
void StaticGuardNoteSkip(const char *path, const char *reason, const XSurface *xsurf,
                         uint32_t smodelCount)
{
    ++g_staticGuardSkipped;
    if (g_staticGuardPrints < 8u)
    {
        ++g_staticGuardPrints;
        
    }
}
// Cached funnel: indices are u16 into a 64k chunk, so any u16 is GPU-safe;
// the CPU-side OOB that matters is baseIndex into smodelCache.indices.
bool GuardStaticCachedDraw(const XSurface *xsurf, uint32_t smodelCount,
                           const uint16_t *list, const char *path)
{
    if (!xsurf)
    {
        StaticGuardNoteSkip(path, "null_xsurf", xsurf, smodelCount);
        return true;
    }
    if (!smodelCount || smodelCount > 128u)
    {
        StaticGuardNoteSkip(path, "smodel_count", xsurf, smodelCount);
        return true;
    }
    if (!xsurf->triCount || xsurf->triCount > 65535u)
    {
        StaticGuardNoteSkip(path, "tri_count", xsurf, smodelCount);
        return true;
    }
    if (!list)
    {
        StaticGuardNoteSkip(path, "null_list", xsurf, smodelCount);
        return true;
    }
    if (!rgp.world)
    {
        StaticGuardNoteSkip(path, "null_world", xsurf, smodelCount);
        return true;
    }
    const uint32_t surfBase = 3u * (uint32_t)xsurf->baseTriIndex;
    for (uint32_t i = 0; i < smodelCount; ++i)
    {
        const uint32_t cacheIndex = list[i];
        if (!cacheIndex)
        {
            StaticGuardNoteSkip(path, "null_cache_index", xsurf, smodelCount);
            return true;
        }
        const GfxCachedSModelSurf *cached = R_GetCachedSModelSurf(cacheIndex);
        if (cached->smodelIndex >= rgp.world->dpvs.smodelCount)
        {
            StaticGuardNoteSkip(path, "smodel_index", xsurf, smodelCount);
            return true;
        }
        const uint32_t baseIndex = surfBase + 4u * cached->baseVertIndex;
        if (baseIndex >= (uint32_t)SMC_MAX_INDEX_IN_CACHE ||
            baseIndex + 3u * (uint32_t)xsurf->triCount > (uint32_t)SMC_MAX_INDEX_IN_CACHE)
        {
            StaticGuardNoteSkip(path, "cache_base", xsurf, smodelCount);
            return true;
        }
    }
    // Dynamic-IB capacity for the whole batch (R_ReserveIndexData asserts
    // triCount*3 <= total; the assert only logs on Switch).
    if (gfxBuf.dynamicIndexBuffer &&
        (uint64_t)smodelCount * xsurf->triCount * 3u > (uint64_t)gfxBuf.dynamicIndexBuffer->total)
    {
        StaticGuardNoteSkip(path, "dynib_capacity", xsurf, smodelCount);
        return true;
    }
    return false;
}
// Rigid funnel: zone VB/IB path. Checks handles, spans against the zone's
// own block-7/8 sizes, and every index value against vertCount.
bool GuardStaticRigidDraw(const XSurface *xsurf, uint32_t smodelCount, const char *path)
{
    if (!xsurf)
    {
        StaticGuardNoteSkip(path, "null_xsurf", xsurf, smodelCount);
        return true;
    }
    if (!smodelCount || smodelCount > 128u)
    {
        StaticGuardNoteSkip(path, "smodel_count", xsurf, smodelCount);
        return true;
    }
    if (!xsurf->triCount || !xsurf->vertCount)
    {
        StaticGuardNoteSkip(path, "zero_counts", xsurf, smodelCount);
        return true;
    }
    if (!xsurf->triIndices)
    {
        StaticGuardNoteSkip(path, "null_tris", xsurf, smodelCount);
        return true;
    }
    if (!xsurf->verts0)
    {
        StaticGuardNoteSkip(path, "null_verts", xsurf, smodelCount);
        return true;
    }
    {
        const uint32_t idxCount = 3u * (uint32_t)xsurf->triCount;
        for (uint32_t i = 0; i < idxCount; ++i)
        {
            if (xsurf->triIndices[i] >= xsurf->vertCount)
            {
                StaticGuardNoteSkip(path, "index_value", xsurf, smodelCount);
                return true;
            }
        }
    }
    {
        void *ib = nullptr;
        int32_t baseIndex = 0;
        DB_GetIndexBufferAndBase(xsurf->zoneHandle, xsurf->triIndices, &ib, &baseIndex);
        void *vb = nullptr;
        int32_t vertexOffset = 0;
        DB_GetVertexBufferAndOffset(xsurf->zoneHandle, (uint8_t *)xsurf->verts0, &vb, &vertexOffset);
        if (!ib || !vb)
        {
            StaticGuardNoteSkip(path, "null_zone_buffer", xsurf, smodelCount);
            return true;
        }
        if (baseIndex < 0 || vertexOffset < 0)
        {
            StaticGuardNoteSkip(path, "negative_span", xsurf, smodelCount);
            return true;
        }
        uint32_t vertBytes = 0, indexBytes = 0;
        if (DB_GetZoneGeometrySizes(xsurf->zoneHandle, &vertBytes, &indexBytes))
        {
            const uint64_t needVert = (uint64_t)(uint32_t)vertexOffset +
                                      (uint64_t)xsurf->vertCount * 32u;
            const uint64_t needIdx = (uint64_t)(uint32_t)baseIndex * 2u +
                                     (uint64_t)xsurf->triCount * 6u;
            if (needVert > vertBytes || needIdx > indexBytes)
            {
                StaticGuardNoteSkip(path, "zone_span", xsurf, smodelCount);
                return true;
            }
        }
    }
    return false;
}
} // namespace


void __cdecl R_SetupStaticModelPrim(XSurface *xsurf, GfxDrawPrimArgs *args, GfxCmdBufPrimState *primState)
{
    IDirect3DIndexBuffer9 *ib; // [esp+10h] [ebp-4h] BYREF

    iassert(xsurf);
    args->vertexCount = xsurf->vertCount;
    args->triCount = xsurf->triCount;
    //if (!IsFastFileLoad())
    //    MyAssertHandler(".\\r_draw_staticmodel.cpp", 266, 0, "%s", "XSurfaceHasOptimizedIndices()");
    DB_GetIndexBufferAndBase(xsurf->zoneHandle, xsurf->triIndices, (void **)&ib, &args->baseIndex);
    iassert(ib);
    // Temporary diagnostic: the rigid (non-smodel-cache) static-model path is the
    // only draw funnel in this project that binds a *zone* index/vertex
    // buffer. Print the first few bindings so a live log shows the real
    // buffer handles and derived spans instead of an inference.
    {
        static uint32_t s_zoneDrawDiag = 0;
        if (s_zoneDrawDiag < 4u)
        {
            ++s_zoneDrawDiag;
            Com_Printf(0, "SMC_ZONEDRAW_DIAG zoneHandle=%u ib=%p baseIndex=%d tri=%u vert=%u\n",
                       (unsigned)xsurf->zoneHandle, (const void *)ib, args->baseIndex,
                       (unsigned)xsurf->triCount, (unsigned)xsurf->vertCount);
        }
    }
    if (primState->indexBuffer != ib)
        R_ChangeIndices(primState, ib);
}

// Instanced rigid static models (deko3d renderer only).
//
// A rigid static-model stream draws one XSurface for a list of placements;
// per model the ordinary loop sets the world matrix (and, lit, the
// reflection probe and model lighting coords), re-derives the pass's
// per-prim arguments and draws. Per-prim arguments are vertex shader code
// constants only (R_SetPassShaderPrimArguments), so between the models of a
// run only those registers differ, plus the reflection-probe sampler of a
// lit pass. The deko9 device captures exactly those registers after the
// same per-model engine calls (Deko9_AddInstance) and draws the run once
// with a vertex shader variant that reads them from an instance stream:
// each model sees the values its own draw would have used. Runs split where
// the reflection probe changes; the draw order is unchanged.
//
// Falls back to one draw per model (counted in the DEKO9 perf line as
// instanceFallbacks) when: r_portDebugChecks is on
// (R_DrawIndexedPrimitive's per-draw debug filters); the pass has no
// per-prim arguments or more than 16 per-prim registers; a run has one
// model.
namespace
{
static_assert(deko9::kMaxInstanceRegs == 16, "R_StaticModelInstanceRegs fills at most 16 registers");

void R_DrawStaticModelInstance(const GfxStaticModelDrawInst *smodelDrawInst, GfxCmdBufContext context, bool lit)
{
    R_DrawStaticModelDrawSurfPlacement(smodelDrawInst, context.source);
    if (lit)
        R_SetModelLightingCoordsForSource(smodelDrawInst->lightingHandle, context.source);
    R_SetupPassPerPrimArgs(context);
}

// Returns false when the caller must draw the stream per model.
bool R_DrawStaticModelsInstanced(const uint16_t *list, uint32_t smodelCount, GfxCmdBufContext context,
                                 const GfxDrawPrimArgs &args, bool lit)
{
    if (!R_StaticModelInstancingEnabled() || smodelCount < 2)
        return false;
    IDirect3DDevice9 *device = context.state->prim.device;
    uint8_t regs[deko9::kMaxInstanceRegs];
    uint32_t regCount = 0;
    if (!R_StaticModelInstanceRegs(context.state->pass, regs, &regCount))
    {
        Deko9_NoteInstanceFallback(device, smodelCount);
        return false;
    }
    const GfxStaticModelDrawInst *smodelDrawInsts = rgp.world->dpvs.smodelDrawInsts;
    deko9::ForEachInstanceRun(
        smodelCount,
        [&](uint32_t index) { return lit ? smodelDrawInsts[list[index]].reflectionProbeIndex : 0u; },
        [&](uint32_t first, uint32_t count) {
            if (lit)
                R_SetReflectionProbe(context, smodelDrawInsts[list[first]].reflectionProbeIndex);
            if (count == 1 || !Deko9_BeginInstances(device, regs, regCount))
            {
                Deko9_NoteInstanceFallback(device, count);
                for (uint32_t index = first; index < first + count; ++index)
                {
                    R_DrawStaticModelInstance(&smodelDrawInsts[list[index]], context, lit);
                    R_DrawIndexedPrimitive(&context.state->prim, &args);
                }
                return;
            }
            for (uint32_t index = first; index < first + count; ++index)
            {
                R_DrawStaticModelInstance(&smodelDrawInsts[list[index]], context, lit);
                Deko9_AddInstance(device);
            }
            const int32_t hr =
                Deko9_DrawIndexedInstances(device, 0, 0, args.vertexCount, args.baseIndex, args.triCount);
            if (hr < 0)
            {
                ++g_disableRendering;
                Com_Error(ERR_FATAL, "Deko9_DrawIndexedInstances( %u instances ) failed: %s\n", count,
                          R_ErrorDescription(hr));
            }
        });
    return true;
}
} // namespace

void __cdecl R_DrawStaticModelDrawSurfLightingNonOptimized(
    GfxStaticModelDrawStream *drawStream,
    GfxCmdBufContext context)
{
    const GfxStaticModelDrawInst *smodelDrawInst; // [esp+0h] [ebp-28h]
    GfxStaticModelDrawInst *smodelDrawInsts; // [esp+4h] [ebp-24h]
    const uint16_t *list; // [esp+8h] [ebp-20h]
    uint32_t smodelCount; // [esp+Ch] [ebp-1Ch]
    uint32_t index; // [esp+10h] [ebp-18h]
    XSurface *xsurf; // [esp+14h] [ebp-14h]
    GfxDrawPrimArgs args; // [esp+18h] [ebp-10h] BYREF
    uint16_t lightingHandle; // [esp+24h] [ebp-4h]

    xsurf = drawStream->localSurf;
    R_SetupStaticModelPrim(xsurf, &args, &context.state->prim);
    R_SetStaticModelVertexBuffer(&context.state->prim, xsurf);
    smodelCount = drawStream->smodelCount;
    list = drawStream->smodelList;
    if (r_portDebugChecks->current.enabled && GuardStaticRigidDraw(xsurf, smodelCount, "rigid_lit"))
        return;
    // (No early return: the diagnostic drain below still runs.)
    const bool instanced = R_DrawStaticModelsInstanced(list, smodelCount, context, args, true);
    smodelDrawInsts = rgp.world->dpvs.smodelDrawInsts;
    for (index = 0; !instanced && index < smodelCount; ++index)
    {
        smodelDrawInst = &smodelDrawInsts[list[index]];
        R_SetReflectionProbe(context, smodelDrawInst->reflectionProbeIndex);
        R_DrawStaticModelDrawSurfPlacement(smodelDrawInst, context.source);
        lightingHandle = smodelDrawInst->lightingHandle;
        R_SetModelLightingCoordsForSource(lightingHandle, context.source);
        R_SetupPassPerPrimArgs(context);
        R_DrawIndexedPrimitive(&context.state->prim, &args);
    }
#ifdef __SWITCH__
    // A device loss is reported asynchronously by the deko3d backend, after
    // several static streams may already have been queued.  In the existing
    // diagnostic mode, drain once per stream so the four capped zone binding
    // markers identify the first stream whose work does not complete.  This
    // submits every real draw; it only changes the diagnostic
    // synchronization boundary.
    if (com_diagMarkers && com_diagMarkers->current.enabled)
    {
        extern void R_SwitchWaitForGpuIdle();
        R_SwitchWaitForGpuIdle();
    }
#endif
}

void __cdecl R_DrawStaticModelSurfLit(const uint32_t *primDrawSurfPos, GfxCmdBufContext context)
{
    GfxStaticModelDrawStream drawStream; // [esp+0h] [ebp-20h] BYREF
    XSurface *surf; // [esp+1Ch] [ebp-4h] BYREF

    drawStream.primDrawSurfPos = primDrawSurfPos;
    drawStream.reflectionProbeTexture = context.state->samplerTexture[1];
    drawStream.customSamplerFlags = context.state->pass->customSamplerFlags;
    while (R_GetNextStaticModelSurf(&drawStream, &surf))
        R_DrawStaticModelDrawSurfLightingNonOptimized(&drawStream, context);
}

int __cdecl R_GetNextStaticModelSurf(GfxStaticModelDrawStream *drawStream, XSurface **outSurf)
{
    XSurface *xsurf; // [esp+0h] [ebp-Ch]
    const uint32_t *primDrawSurfPos; // [esp+4h] [ebp-8h]

    drawStream->smodelCount = *drawStream->primDrawSurfPos++;
    if (!drawStream->smodelCount)
        return 0;
    primDrawSurfPos = drawStream->primDrawSurfPos;
    drawStream->primDrawSurfPos += ((drawStream->smodelCount + 1) >> 1) + PRIM_DRAW_SURF_PTR_WORDS;
    xsurf = R_ReadPrimDrawSurfXSurfacePtr(primDrawSurfPos);
    drawStream->smodelList = (const uint16_t *)(primDrawSurfPos + PRIM_DRAW_SURF_PTR_WORDS);
    drawStream->localSurf = xsurf;
    g_frameStatsCur.geoIndexCount += 3 * drawStream->smodelCount * xsurf->triCount;

    iassert(g_primStats);
    g_primStats->dynamicIndexCount += 3 * drawStream->smodelCount * xsurf->triCount;
    g_primStats->dynamicVertexCount += drawStream->smodelCount * xsurf->vertCount;
    *outSurf = xsurf;
    return 1;
}

void __cdecl R_DrawStaticModelSurf(const uint32_t *primDrawSurfPos, GfxCmdBufContext context)
{
    GfxStaticModelDrawStream drawStream; // [esp+0h] [ebp-20h] BYREF
    XSurface *surf; // [esp+1Ch] [ebp-4h] BYREF

    drawStream.primDrawSurfPos = primDrawSurfPos;
    drawStream.reflectionProbeTexture = context.state->samplerTexture[1];
    drawStream.customSamplerFlags = context.state->pass->customSamplerFlags;
    while (R_GetNextStaticModelSurf(&drawStream, &surf))
        R_DrawStaticModelDrawSurfNonOptimized(&drawStream, context);
}

void __cdecl R_DrawStaticModelDrawSurfNonOptimized(GfxStaticModelDrawStream *drawStream, GfxCmdBufContext context)
{
    GfxStaticModelDrawInst *smodelDrawInsts; // [esp+4h] [ebp-20h]
    const uint16_t *list; // [esp+8h] [ebp-1Ch]
    uint32_t smodelCount; // [esp+Ch] [ebp-18h]
    uint32_t index; // [esp+10h] [ebp-14h]
    XSurface *xsurf; // [esp+14h] [ebp-10h]
    GfxDrawPrimArgs args; // [esp+18h] [ebp-Ch] BYREF

    xsurf = drawStream->localSurf;
    R_SetupStaticModelPrim(xsurf, &args, &context.state->prim);
    R_SetStaticModelVertexBuffer(&context.state->prim, xsurf);
    smodelCount = drawStream->smodelCount;
    list = drawStream->smodelList;
    if (r_portDebugChecks->current.enabled && GuardStaticRigidDraw(xsurf, smodelCount, "rigid"))
        return;
    if (R_DrawStaticModelsInstanced(list, smodelCount, context, args, false))
        return;
    smodelDrawInsts = rgp.world->dpvs.smodelDrawInsts;
    for (index = 0; index < smodelCount; ++index)
    {
        R_DrawStaticModelDrawSurfPlacement(&smodelDrawInsts[list[index]], context.source);
        R_SetupPassPerPrimArgs(context);
        R_DrawIndexedPrimitive(&context.state->prim, &args);
    }
}

void __cdecl R_SetStaticModelVertexBuffer(GfxCmdBufPrimState *primState, XSurface *xsurf)
{
    IDirect3DVertexBuffer9 *vb; // [esp+18h] [ebp-8h] BYREF
    int vertexOffset; // [esp+1Ch] [ebp-4h] BYREF

    iassert(xsurf);
    //if (xsurf->deformed || !IsFastFileLoad())
    //    MyAssertHandler(".\\r_draw_staticmodel.cpp", 246, 0, "%s", "XSurfaceHasOptimizedVertices( xsurf )");
    DB_GetVertexBufferAndOffset(xsurf->zoneHandle, (uint8*)xsurf->verts0, (void **)&vb, &vertexOffset);
    iassert(vb);
    {
        static uint32_t s_zoneVbDiag = 0;
        if (s_zoneVbDiag < 4u)
        {
            ++s_zoneVbDiag;
            Com_Printf(0, "SMC_ZONEVB_DIAG zoneHandle=%u vb=%p vertexOffset=%d vert=%u\n",
                       (unsigned)xsurf->zoneHandle, (const void *)vb, vertexOffset,
                       (unsigned)xsurf->vertCount);
        }
    }
    R_SetStreamSource(primState, vb, vertexOffset, 0x20u);
}

void __cdecl R_DrawStaticModelDrawSurfPlacement(
    const GfxStaticModelDrawInst *smodelDrawInst,
    GfxCmdBufSourceState *source)
{
    GfxCmdBufSourceState *matrix; // [esp+5Ch] [ebp-38h]
    float origin[3]; // [esp+60h] [ebp-34h] BYREF
    float scale; // [esp+6Ch] [ebp-28h]
    mat3x3 axis; // [esp+70h] [ebp-24h] BYREF

    axis[0][0] = smodelDrawInst->placement.axis[0][0]; // LWSS: is it really necessary to copy this? prob not. remove later when working
    axis[0][1] = smodelDrawInst->placement.axis[0][1];
    axis[0][2] = smodelDrawInst->placement.axis[0][2];
    axis[1][0] = smodelDrawInst->placement.axis[1][0];
    axis[1][1] = smodelDrawInst->placement.axis[1][1];
    axis[1][2] = smodelDrawInst->placement.axis[1][2];
    axis[2][0] = smodelDrawInst->placement.axis[2][0];
    axis[2][1] = smodelDrawInst->placement.axis[2][1];
    axis[2][2] = smodelDrawInst->placement.axis[2][2];

    scale = smodelDrawInst->placement.scale;
    matrix = R_GetActiveWorldMatrix(source);
    Vec3Sub(smodelDrawInst->placement.origin, source->eyeOffset, origin);
    MatrixSet44(*(mat4x4*)matrix, origin, axis, scale);
}

void __cdecl R_SetupCachedStaticModelLighting(GfxCmdBufSourceState *source)
{
    source->input.consts[CONST_SRC_CODE_BASE_LIGHTING_COORDS][0] = 0.0f;
    source->input.consts[CONST_SRC_CODE_BASE_LIGHTING_COORDS][1] = 0.0f;
    source->input.consts[CONST_SRC_CODE_BASE_LIGHTING_COORDS][2] = 0.5f;
    source->input.consts[CONST_SRC_CODE_BASE_LIGHTING_COORDS][3] = 1.0f;
    R_DirtyCodeConstant(source, CONST_SRC_CODE_BASE_LIGHTING_COORDS);
}

int __cdecl R_GetNextStaticModelCachedSurf(GfxStaticModelDrawStream *drawStream)
{
    const GfxStaticModelDrawInst *smodelDrawInst; // [esp+0h] [ebp-Ch]
    XSurface *xsurf; // [esp+8h] [ebp-4h]

    drawStream->smodelCount = *drawStream->primDrawSurfPos++;
    if (!drawStream->smodelCount)
        return 0;
    xsurf = R_ReadPrimDrawSurfXSurfacePtr(drawStream->primDrawSurfPos);
    drawStream->primDrawSurfPos += PRIM_DRAW_SURF_PTR_WORDS;
    drawStream->smodelList = (const unsigned short*)drawStream->primDrawSurfPos;
    drawStream->primDrawSurfPos += (drawStream->smodelCount + 1) >> 1;
    smodelDrawInst = &rgp.world->dpvs.smodelDrawInsts[R_GetCachedSModelSurf(*drawStream->smodelList)->smodelIndex];
    drawStream->localSurf = xsurf;
    drawStream->reflectionProbeIndex = smodelDrawInst->reflectionProbeIndex;
    g_frameStatsCur.geoIndexCount += 3 * drawStream->smodelCount * xsurf->triCount;

    iassert(g_primStats);
    g_primStats->dynamicIndexCount += 3 * drawStream->smodelCount * xsurf->triCount;
    g_primStats->dynamicVertexCount += drawStream->smodelCount * xsurf->vertCount;
    return 1;
}

XSurface *__cdecl R_GetCurrentStaticModelCachedSurf(
    GfxStaticModelDrawStream *drawStream,
    uint32_t *reflectionProbeIndex)
{
    if (reflectionProbeIndex)
        *reflectionProbeIndex = drawStream->reflectionProbeIndex;
    return drawStream->localSurf;
}

void __cdecl R_SetStaticModelCachedPrimArgs(const XSurface *xsurf, GfxDrawPrimArgs *args)
{
    iassert(xsurf);
    args->vertexCount = 0x10000;
    args->triCount = xsurf->triCount;
}

void __cdecl R_SetStaticModelCachedBuffer(GfxCmdBufState *state, uint32_t cachedIndex)
{
    R_SetStreamSource(&state->prim, gfxBuf.smodelCacheVb, ((cachedIndex - 1) & 0xFFFFF000) << 9, 32);
}

void __cdecl R_DrawStaticModelsCachedDrawSurfLighting(GfxStaticModelDrawStream *drawStream, GfxCmdBufContext context)
{
    uint32_t copyBaseIndex; // [esp+0h] [ebp-30h]
    uint32_t baseIndex; // [esp+4h] [ebp-2Ch]
    uint32_t surfBaseIndex; // [esp+8h] [ebp-28h]
    uint32_t reflectionProbeIndex; // [esp+10h] [ebp-20h] BYREF
    const uint16_t *list; // [esp+14h] [ebp-1Ch]
    uint32_t smodelCount; // [esp+18h] [ebp-18h]
    uint32_t index; // [esp+1Ch] [ebp-14h]
    const XSurface *xsurf; // [esp+20h] [ebp-10h]
    GfxDrawPrimArgs args; // [esp+24h] [ebp-Ch] BYREF

    xsurf = R_GetCurrentStaticModelCachedSurf(drawStream, &reflectionProbeIndex);
    list = drawStream->smodelList;
    smodelCount = drawStream->smodelCount;
    if (r_portDebugChecks->current.enabled && GuardStaticCachedDraw(xsurf, smodelCount, list, "cached_lit"))
        return;
    R_SetStaticModelCachedPrimArgs(xsurf, &args);
    R_SetStaticModelCachedBuffer(context.state, *list);
    R_SetupPassPerPrimArgs(context);
    R_SetReflectionProbe(context, reflectionProbeIndex);
    surfBaseIndex = 3 * xsurf->baseTriIndex;
    args.triCount = smodelCount * xsurf->triCount;
    args.baseIndex = R_ReserveIndexData(&context.state->prim, args.triCount);
    index = 0;
    do
    {
        baseIndex = surfBaseIndex + 4 * R_GetCachedSModelSurf(list[index])->baseVertIndex;
        iassert(baseIndex < SMC_MAX_INDEX_IN_CACHE);
        iassert(baseIndex + xsurf->triCount * 3 <= SMC_MAX_INDEX_IN_CACHE);
        R_NOTE_SETIDX(SWITCH_PERF_EV_SETIDX_SMODEL);
        copyBaseIndex = R_SetIndexData(&context.state->prim, (unsigned char*)&gfxBuf.smodelCache.indices[baseIndex], xsurf->triCount);
        iassert(copyBaseIndex == args.baseIndex + xsurf->triCount * 3 * index);
        ++index;
    } while (index < smodelCount);
    R_DrawIndexedPrimitive(&context.state->prim, &args);
}

void __cdecl R_DrawStaticModelsCachedDrawSurf(GfxStaticModelDrawStream *drawStream, GfxCmdBufContext context)
{
    uint32_t copyBaseIndex; // [esp+0h] [ebp-2Ch]
    uint32_t baseIndex; // [esp+4h] [ebp-28h]
    uint32_t surfBaseIndex; // [esp+8h] [ebp-24h]
    const uint16_t *list; // [esp+10h] [ebp-1Ch]
    uint32_t smodelCount; // [esp+14h] [ebp-18h]
    uint32_t index; // [esp+18h] [ebp-14h]
    const XSurface *xsurf; // [esp+1Ch] [ebp-10h]
    GfxDrawPrimArgs args; // [esp+20h] [ebp-Ch] BYREF

    xsurf = R_GetCurrentStaticModelCachedSurf(drawStream, 0);
    list = drawStream->smodelList;
    smodelCount = drawStream->smodelCount;
    if (r_portDebugChecks->current.enabled && GuardStaticCachedDraw(xsurf, smodelCount, list, "cached"))
        return;
    R_SetStaticModelCachedPrimArgs(xsurf, &args);
    R_SetStaticModelCachedBuffer(context.state, *list);
    R_SetupPassPerPrimArgs(context);
    surfBaseIndex = 3 * xsurf->baseTriIndex;
    args.triCount = smodelCount * xsurf->triCount;
    args.baseIndex = R_ReserveIndexData(&context.state->prim, args.triCount);
    index = 0;
    do
    {
        baseIndex = surfBaseIndex + 4 * R_GetCachedSModelSurf(list[index])->baseVertIndex;
        iassert(baseIndex < SMC_MAX_INDEX_IN_CACHE);
        iassert(baseIndex + xsurf->triCount * 3 <= SMC_MAX_INDEX_IN_CACHE);
        R_NOTE_SETIDX(SWITCH_PERF_EV_SETIDX_SMODEL);
        copyBaseIndex = R_SetIndexData(&context.state->prim, (unsigned char*)&gfxBuf.smodelCache.indices[baseIndex], xsurf->triCount);
        iassert(copyBaseIndex == args.baseIndex + xsurf->triCount * 3 * index);
        ++index;
    } while (index < smodelCount);
    R_DrawIndexedPrimitive(&context.state->prim, &args);
}

void __cdecl R_DrawStaticModelCachedSurfLit(const uint32_t *primDrawSurfPos, GfxCmdBufContext context)
{
    GfxStaticModelDrawStream drawStream; // [esp+0h] [ebp-1Ch] BYREF

    R_SetCodeImageTexture(context.source, TEXTURE_SRC_CODE_DYNAMIC_SHADOWS, rgp.whiteImage);
    R_SetupCachedStaticModelLighting(context.source);
    R_SetupPassPerObjectArgs(context);
    drawStream.primDrawSurfPos = primDrawSurfPos;
    drawStream.reflectionProbeTexture = context.state->samplerTexture[1];
    drawStream.customSamplerFlags = context.state->pass->customSamplerFlags;
    while (R_GetNextStaticModelCachedSurf(&drawStream))
        R_DrawStaticModelsCachedDrawSurfLighting(&drawStream, context);
    context.state->samplerTexture[1] = drawStream.reflectionProbeTexture;
}

void __cdecl R_DrawStaticModelCachedSurf(const uint32_t *primDrawSurfPos, GfxCmdBufContext context)
{
    GfxStaticModelDrawStream drawStream; // [esp+0h] [ebp-1Ch] BYREF

    R_SetupPassPerObjectArgs(context);
    drawStream.primDrawSurfPos = primDrawSurfPos;
    drawStream.reflectionProbeTexture = context.state->samplerTexture[1];
    drawStream.customSamplerFlags = context.state->pass->customSamplerFlags;
    while (R_GetNextStaticModelCachedSurf(&drawStream))
        R_DrawStaticModelsCachedDrawSurf(&drawStream, context);
    if (context.state->samplerTexture[1] != drawStream.reflectionProbeTexture)
        MyAssertHandler(
            ".\\r_draw_staticmodel.cpp",
            2076,
            0,
            "%s",
            "context.state->samplerTexture[TEXTURE_DEST_CODE_REFLECTION_PROBE] == drawStream.reflectionProbeTexture");
}

const uint32_t *__cdecl R_ReadPrimDrawSurfData(GfxReadCmdBuf *cmdBuf, uint32_t count)
{
    const uint32_t *result; // [esp+0h] [ebp-4h]

    result = cmdBuf->primDrawSurfPos;
    cmdBuf->primDrawSurfPos += count;
    return result;
}

uint32_t __cdecl R_ReadPrimDrawSurfInt(GfxReadCmdBuf *cmdBuf)
{
    return *cmdBuf->primDrawSurfPos++;
}

int __cdecl R_ReadStaticModelPreTessDrawSurf(
    GfxReadCmdBuf *readCmdBuf,
    GfxStaticModelPreTessSurf *pretessSurf,
    uint32_t *firstIndex,
    uint32_t *count)
{
    *count = R_ReadPrimDrawSurfInt(readCmdBuf);
    if (!*count)
        return 0;
    pretessSurf->packed = R_ReadPrimDrawSurfInt(readCmdBuf);
    *firstIndex = R_ReadPrimDrawSurfInt(readCmdBuf);
    if (*firstIndex >= 0x100000 && *firstIndex != R_PRETESS_STATIC_FLAG)
        MyAssertHandler(
            ".\\r_draw_staticmodel.cpp",
            1894,
            0,
            "*firstIndex doesn't index R_MAX_PRETESS_INDICES\n\t%i not in [0, %i)",
            *firstIndex,
            0x100000);
    return 1;
}

const GfxStaticModelDrawInst *__cdecl R_SetupCachedSModelSurface(
    GfxCmdBufState *state,
    uint32_t cachedIndex,
    uint32_t lod,
    uint32_t surfIndex,
    uint32_t count,
    GfxDrawPrimArgs *args,
    uint32_t *baseIndex)
{
    const GfxStaticModelDrawInst *smodelDrawInst; // [esp+14h] [ebp-Ch]
    const XSurface *xsurf; // [esp+1Ch] [ebp-4h]

    iassert( cachedIndex );
    smodelDrawInst = &rgp.world->dpvs.smodelDrawInsts[R_GetCachedSModelSurf(cachedIndex)->smodelIndex];
    xsurf = XModelGetSurface(smodelDrawInst->model, lod, surfIndex);
    if (baseIndex)
        *baseIndex = 3 * xsurf->baseTriIndex;
    args->vertexCount = 0x10000;
    args->triCount = count * xsurf->triCount;
    R_SetStreamSource(&state->prim, gfxBuf.smodelCacheVb, ((cachedIndex - 1) & 0xFFFFF000) << 9, 32);
    g_frameStatsCur.geoIndexCount += 3 * count * xsurf->triCount;
    iassert(g_primStats);
    g_primStats->dynamicIndexCount += 3 * count * xsurf->triCount;
    g_primStats->dynamicVertexCount += count * xsurf->vertCount;
    return smodelDrawInst;
}

bool R_StaticModelSurfHasStaticIndices(const XSurface *xsurf, IDirect3DIndexBuffer9 **ibOut, int32_t *baseIndexOut)
{
    if (!xsurf || !xsurf->triIndices || !xsurf->triCount)
        return false;
    void *ib = nullptr;
    int32_t baseIndex = 0;
    DB_GetIndexBufferAndBase(xsurf->zoneHandle, xsurf->triIndices, &ib, &baseIndex);
    uint32_t vertBytes = 0, indexBytes = 0;
    if (!ib || baseIndex < 0 || !DB_GetZoneGeometrySizes(xsurf->zoneHandle, &vertBytes, &indexBytes)
        || (uint64_t)(uint32_t)baseIndex * 2u + (uint64_t)xsurf->triCount * 6u > indexBytes)
        return false;
    if (ibOut)
        *ibOut = (IDirect3DIndexBuffer9 *)ib;
    if (baseIndexOut)
        *baseIndexOut = baseIndex;
    return true;
}

// A static-model list recorded with R_PRETESS_STATIC_FLAG: the surface's
// triangles from its zone index buffer once per instance, the instance's
// cache slot as base vertex -- the same indices the copying path would have
// written (triIndices + slot + xsurf->baseVertIndex), in the same order.
static void R_DrawStaticModelsStaticList(
    GfxStaticModelPreTessSurf pretessSurf,
    uint32_t count,
    const uint16_t *list,
    GfxCmdBufPrimState *prim)
{
    const GfxStaticModelDrawInst *smodelDrawInst =
        &rgp.world->dpvs.smodelDrawInsts[R_GetCachedSModelSurf(pretessSurf.fields.cachedIndex)->smodelIndex];
    const XSurface *xsurf =
        XModelGetSurface(smodelDrawInst->model, pretessSurf.fields.lod, pretessSurf.fields.surfIndex);
    IDirect3DIndexBuffer9 *ib = nullptr;
    int32_t baseIndex = 0;
    if (!R_StaticModelSurfHasStaticIndices(xsurf, &ib, &baseIndex))
    {
        // The front end checked the same surface: never expected.
        static bool warned;
        if (!warned)
        {
            warned = true;
            Com_Printf(CON_CHANNEL_SYSTEM, "FAIL:STATIC_PRETESS_SMODEL_IB static-model list without zone indices\n");
        }
        return;
    }
    if (prim->indexBuffer != ib)
        R_ChangeIndices(prim, ib);
    GfxIndexRange ranges[64];
    uint32_t pending = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        ranges[pending].firstIndex = (uint32_t)baseIndex;
        ranges[pending].triCount = xsurf->triCount;
        // baseVertIndex is the slot's vertex in the whole cache buffer; the
        // stream starts at the slot's 64K-vertex region (R_SetupCachedSModelSurface)
        // and the copying path's 16-bit indices wrap to it the same way.
        ranges[pending].baseVertex =
            (uint16_t)(R_GetCachedSModelSurf(list[i])->baseVertIndex + xsurf->baseVertIndex);
        if (r_deko9Verify->current.enabled)
        {
            // The indices this range draws must be exactly the ones the
            // copying path would copy from the cache's index array.
            const uint32_t cacheBase = 3u * xsurf->baseTriIndex + 4u * R_GetCachedSModelSurf(list[i])->baseVertIndex;
            const uint32_t n = 3u * xsurf->triCount;
            for (uint32_t k = 0; k < n; ++k)
            {
                const uint16_t want = gfxBuf.smodelCache.indices[cacheBase + k];
                const uint16_t got = (uint16_t)(xsurf->triIndices[k] + ranges[pending].baseVertex);
                if (want != got)
                {
                    static uint32_t reported;
                    if (reported++ < 8)
                        Com_Printf(CON_CHANNEL_SYSTEM,
                                   "FAIL:STATIC_PRETESS_SMODEL_MISMATCH model=%s lod=%u surf=%u inst=%u/%u cached=%u "
                                   "k=%u want=%u got=%u baseVertex=%d slotVert=%u surfVert=%u\n",
                                   smodelDrawInst->model->name, (unsigned)pretessSurf.fields.lod,
                                   (unsigned)pretessSurf.fields.surfIndex, i, count, (unsigned)list[i], k, want, got,
                                   ranges[pending].baseVertex, R_GetCachedSModelSurf(list[i])->baseVertIndex,
                                   (unsigned)xsurf->baseVertIndex);
                    break;
                }
            }
        }
        if (++pending == 64)
        {
            R_DrawIndexedRanges(prim, xsurf->vertCount, ranges, pending);
            pending = 0;
        }
    }
    R_DrawIndexedRanges(prim, xsurf->vertCount, ranges, pending);
}

void __cdecl R_DrawStaticModelsPreTessDrawSurf(
    GfxStaticModelPreTessSurf pretessSurf,
    uint32_t firstIndex,
    uint32_t count,
    GfxCmdBufContext context,
    const uint16_t *staticList)
{
    IDirect3DIndexBuffer9 *ib; // [esp+0h] [ebp-2Ch]
    GfxDrawPrimArgs args; // [esp+20h] [ebp-Ch] BYREF

    iassert( count );
    R_SetupCachedSModelSurface(
        context.state,
        pretessSurf.fields.cachedIndex,
        pretessSurf.fields.lod,
        pretessSurf.fields.surfIndex,
        count,
        &args,
        0);
    R_SetupPassPerPrimArgs(context);
    if (staticList)
    {
        R_DrawStaticModelsStaticList(pretessSurf, count, staticList, &context.state->prim);
        return;
    }
    ib = context.source->input.data->preTessIb;
    if (context.state->prim.indexBuffer != ib)
        R_ChangeIndices(&context.state->prim, ib);
    args.baseIndex = firstIndex;
    R_DrawIndexedPrimitive(&context.state->prim, &args);
}

void __cdecl R_DrawStaticModelsPreTessDrawSurfLighting(
    GfxStaticModelPreTessSurf pretessSurf,
    uint32_t firstIndex,
    uint32_t count,
    GfxCmdBufContext context,
    const uint16_t *staticList)
{
    IDirect3DIndexBuffer9 *ib; // [esp+0h] [ebp-30h]
    const GfxStaticModelDrawInst *smodelDrawInst; // [esp+20h] [ebp-10h]
    GfxDrawPrimArgs args; // [esp+24h] [ebp-Ch] BYREF

    iassert( count );
    smodelDrawInst = R_SetupCachedSModelSurface(
        context.state,
        pretessSurf.fields.cachedIndex,
        pretessSurf.fields.lod,
        pretessSurf.fields.surfIndex,
        count,
        &args,
        0);
    R_SetupPassPerPrimArgs(context);
    R_SetReflectionProbe(context, smodelDrawInst->reflectionProbeIndex);
    if (staticList)
    {
        R_DrawStaticModelsStaticList(pretessSurf, count, staticList, &context.state->prim);
        return;
    }
    ib = context.source->input.data->preTessIb;
    if (context.state->prim.indexBuffer != ib)
        R_ChangeIndices(&context.state->prim, ib);
    args.baseIndex = firstIndex;
    R_DrawIndexedPrimitive(&context.state->prim, &args);
}

void __cdecl R_SetStaticModelSkinnedPrimArgs(GfxCmdBufPrimState *state, const XSurface *xsurf, GfxDrawPrimArgs *args)
{
    iassert(xsurf);
    args->triCount = XSurfaceGetNumTris(xsurf);
    args->vertexCount = XSurfaceGetNumVerts(xsurf);
    R_NOTE_SETIDX(SWITCH_PERF_EV_SETIDX_SMODEL);
    args->baseIndex = R_SetIndexData(state, (unsigned char*)xsurf->triIndices, args->triCount);
}

void __cdecl R_DrawStaticModelSkinnedDrawSurfLighting(
    const GfxStaticModelDrawInst *smodelDrawInst,
    uint16_t lightingHandle,
    GfxDrawPrimArgs *args,
    GfxCmdBufContext context)
{
    R_SetReflectionProbe(context, smodelDrawInst->reflectionProbeIndex);
    R_DrawStaticModelDrawSurfPlacement(smodelDrawInst, context.source);
    R_SetModelLightingCoordsForSource(lightingHandle, context.source);
    R_SetupPassPerPrimArgs(context);
    R_DrawIndexedPrimitive(&context.state->prim, args);
}

void __cdecl R_DrawStaticModelsSkinnedDrawSurfLighting(GfxStaticModelDrawStream *drawStream, GfxCmdBufContext context)
{
    const GfxStaticModelDrawInst *smodelDrawInst; // [esp+10h] [ebp-30h]
    IDirect3DVertexBuffer9 *vb; // [esp+14h] [ebp-2Ch]
    uint32_t vertexOffset; // [esp+18h] [ebp-28h]
    GfxStaticModelDrawInst *smodelDrawInsts; // [esp+1Ch] [ebp-24h]
    const uint16_t *list; // [esp+20h] [ebp-20h]
    uint32_t smodelCount; // [esp+24h] [ebp-1Ch]
    uint32_t index; // [esp+28h] [ebp-18h]
    XSurface *xsurf; // [esp+2Ch] [ebp-14h]
    GfxDrawPrimArgs args; // [esp+30h] [ebp-10h] BYREF
    uint16_t lightingHandle; // [esp+3Ch] [ebp-4h]

    xsurf = drawStream->localSurf;
    R_SetStaticModelSkinnedPrimArgs(&context.state->prim, xsurf, &args);
    R_CheckVertexDataOverflow(32 * args.vertexCount);
    vertexOffset = R_SetVertexData(context.state, xsurf->verts0, args.vertexCount, 32);
    vb = gfxBuf.dynamicVertexBuffer->buffer;
    iassert( vb );
    R_SetStreamSource(&context.state->prim, vb, vertexOffset, 32);
    smodelCount = drawStream->smodelCount;
    smodelDrawInsts = rgp.world->dpvs.smodelDrawInsts;
    list = drawStream->smodelList;
    for (index = 0; index < smodelCount; ++index)
    {
        smodelDrawInst = &smodelDrawInsts[list[index]];
        lightingHandle = smodelDrawInst->lightingHandle;
        R_DrawStaticModelSkinnedDrawSurfLighting(smodelDrawInst, lightingHandle, &args, context);
    }
}

void __cdecl R_DrawStaticModelSkinnedSurfLit(const uint32_t *primDrawSurfPos, GfxCmdBufContext context)
{
    GfxStaticModelDrawStream drawStream; // [esp+0h] [ebp-20h] BYREF
    XSurface *surf; // [esp+1Ch] [ebp-4h] BYREF

    R_SetCodeImageTexture(context.source, TEXTURE_SRC_CODE_DYNAMIC_SHADOWS, rgp.whiteImage);
    R_SetupPassPerObjectArgs(context);
    drawStream.primDrawSurfPos = primDrawSurfPos;
    drawStream.reflectionProbeTexture = context.state->samplerTexture[1];
    drawStream.customSamplerFlags = context.state->pass->customSamplerFlags;
    while (R_GetNextStaticModelSurf(&drawStream, &surf))
        R_DrawStaticModelsSkinnedDrawSurfLighting(&drawStream, context);
    context.state->samplerTexture[1] = drawStream.reflectionProbeTexture;
}

void __cdecl R_DrawStaticModelSkinnedDrawSurf(
    const GfxStaticModelDrawInst *smodelDrawInst,
    GfxDrawPrimArgs *args,
    GfxCmdBufContext context)
{
    R_DrawStaticModelDrawSurfPlacement(smodelDrawInst, context.source);
    R_SetupPassPerPrimArgs(context);
    R_DrawIndexedPrimitive(&context.state->prim, args);
}

void __cdecl R_DrawStaticModelsSkinnedDrawSurf(GfxStaticModelDrawStream *drawStream, GfxCmdBufContext context)
{
    IDirect3DVertexBuffer9 *vb; // [esp+14h] [ebp-28h]
    uint32_t vertexOffset; // [esp+18h] [ebp-24h]
    GfxStaticModelDrawInst *smodelDrawInsts; // [esp+1Ch] [ebp-20h]
    const uint16_t *list; // [esp+20h] [ebp-1Ch]
    uint32_t smodelCount; // [esp+24h] [ebp-18h]
    uint32_t index; // [esp+28h] [ebp-14h]
    XSurface *xsurf; // [esp+2Ch] [ebp-10h]
    GfxDrawPrimArgs args; // [esp+30h] [ebp-Ch] BYREF

    xsurf = drawStream->localSurf;
    R_SetStaticModelSkinnedPrimArgs(&context.state->prim, xsurf, &args);
    R_CheckVertexDataOverflow(32 * args.vertexCount);
    vertexOffset = R_SetVertexData(context.state, xsurf->verts0, args.vertexCount, 32);
    vb = gfxBuf.dynamicVertexBuffer->buffer;
    iassert( vb );
    R_SetStreamSource(&context.state->prim, vb, vertexOffset, 0x20u);
    smodelCount = drawStream->smodelCount;
    smodelDrawInsts = rgp.world->dpvs.smodelDrawInsts;
    list = drawStream->smodelList;
    for (index = 0; index < smodelCount; ++index)
        R_DrawStaticModelSkinnedDrawSurf(&smodelDrawInsts[list[index]], &args, context);
}

void __cdecl R_DrawStaticModelSkinnedSurf(const uint32_t *primDrawSurfPos, GfxCmdBufContext context)
{
    GfxStaticModelDrawStream drawStream; // [esp+0h] [ebp-20h] BYREF
    XSurface *surf; // [esp+1Ch] [ebp-4h] BYREF

    R_SetupPassPerObjectArgs(context);
    drawStream.primDrawSurfPos = primDrawSurfPos;
    drawStream.reflectionProbeTexture = context.state->samplerTexture[1];
    drawStream.customSamplerFlags = context.state->pass->customSamplerFlags;
    while (R_GetNextStaticModelSurf(&drawStream, &surf))
        R_DrawStaticModelsSkinnedDrawSurf(&drawStream, context);
    if (context.state->samplerTexture[1] != drawStream.reflectionProbeTexture)
        MyAssertHandler(
            ".\\r_draw_staticmodel.cpp",
            1799,
            0,
            "%s",
            "context.state->samplerTexture[TEXTURE_DEST_CODE_REFLECTION_PROBE] == drawStream.reflectionProbeTexture");
}
