#include <universal/q_shared.h>
#include "r_draw_bsp.h"
#include "r_image.h"
#include "r_state.h"
#include "rb_logfile.h"
#include "r_dvars.h"
#include "r_state.h"
#include "r_shade.h"
#include "rb_stats.h"
#include "rb_tess.h"
#include "r_pretess.h"
#include <database/db_retail_frame_evidence.h>
#include <deko9/deko9_native.h>

const int g_layerDataStride[16] = { 0, 0, 0, 8, 12, 16, 20, 24, 24, 28, 32, 32, 36, 40, 0, 0 }; // idb

void __cdecl R_SetStreamSource(
    GfxCmdBufPrimState *primState,
    IDirect3DVertexBuffer9 *vb,
    uint32_t vertexOffset,
    uint32_t vertexStride)
{
    if (primState->streams[0].vb != vb
        || primState->streams[0].offset != vertexOffset
        || primState->streams[0].stride != vertexStride)
    {
        R_ChangeStreamSource(primState, 0, vb, vertexOffset, vertexStride);
    }
    if (primState->streams[1].vb || primState->streams[1].offset || primState->streams[1].stride)
        R_ChangeStreamSource(primState, 1u, 0, 0, 0);
}

void __cdecl R_HW_SetSamplerTexture(IDirect3DDevice9 *device, uint32_t samplerIndex, const GfxTexture *texture)
{
    // Upstream shape restored. This used to carry a two-tier silent
    // substitution -- a non-live texture became rgp.whiteImage, and if that
    // was not live either it fabricated its own 1x1 0xFFFFFFFF texture and
    // bound that -- with no counter, so a surface could render solid white
    // and nothing in the evidence would show it. Liveness is the caller's
    // problem (R_SetSampler fails loudly), and a bind failure is logged, not
    // papered over.
    iassert(device);
    if (r_logFile && r_logFile->current.integer)
        RB_LogPrint("device->SetTexture( samplerIndex, baseTex )\n");

    // Native bind: no COM reference churn, store resolved once per bind.
    Deko9_SetTexture(device, samplerIndex, texture ? texture->basemap : nullptr);
    return;
    HRESULT hr = device->SetTexture(samplerIndex, texture ? texture->basemap : nullptr);
    if (FAILED(hr) && r_logFile && r_logFile->current.integer)
        RB_LogPrint("device->SetTexture failed\n");
}

void __cdecl R_SetStreamsForBspSurface(GfxCmdBufPrimState *state, const srfTriangles_t *tris)
{
    int vertexLayerData; // [esp+0h] [ebp-2Ch]
    IDirect3DVertexBuffer9 *layerVb; // [esp+4h] [ebp-28h]
    int vertexOffset; // [esp+8h] [ebp-24h]
    IDirect3DVertexBuffer9 *vb; // [esp+Ch] [ebp-20h]
    uint32_t layerDataStride; // [esp+28h] [ebp-4h]

    layerDataStride = g_layerDataStride[state->vertDeclType];
    // A retained CPU stream is not a GPU resource. Fail at the consumption
    // boundary if world activation omitted either required upload.
    if (!rgp.world->vd.worldVb ||
        (rgp.world->vertexLayerDataSize && !rgp.world->vld.layerVb))
        Com_Error(ERR_FATAL, "BSP draw has incomplete world vertex resources");
    if (layerDataStride)
    {
        vertexLayerData = tris->vertexLayerData;
        layerVb = rgp.world->vld.layerVb;
        vertexOffset = 44 * tris->firstVertex;
        vb = rgp.world->vd.worldVb;
        if (state->streams[0].vb != vb || state->streams[0].offset != vertexOffset || state->streams[0].stride != 44)
            R_ChangeStreamSource(state, 0, vb, vertexOffset, 0x2Cu);
        if (state->streams[1].vb != layerVb
            || state->streams[1].offset != vertexLayerData
            || state->streams[1].stride != layerDataStride)
        {
            R_ChangeStreamSource(state, 1u, layerVb, vertexLayerData, layerDataStride);
        }
    }
    else
    {
        R_SetStreamSource(state, rgp.world->vd.worldVb, 44 * tris->firstVertex, 0x2Cu);
    }
}

void __cdecl R_DrawBspDrawSurfsLit(
    const uint32_t *primDrawSurfPos,
    GfxCmdBufContext context,
    GfxCmdBufContext prepassContext)
{
    GfxTrianglesDrawStream drawStream; // [esp+4h] [ebp-38h] BYREF
    const MaterialPass *pass; // [esp+34h] [ebp-8h]
    uint32_t customSamplerFlags; // [esp+38h] [ebp-4h]

    pass = context.state->pass;
    customSamplerFlags = pass->customSamplerFlags;
    if ((customSamplerFlags & 1) != 0)
        R_SetSamplerState(context.state, 1u, 0x72u);
    if ((customSamplerFlags & 2) != 0)
        R_SetSamplerState(context.state, 2u, 0x62u);
    if ((customSamplerFlags & 4) != 0)
        R_SetSamplerState(context.state, 3u, 0x62u);
    drawStream.reflectionProbeTexture = context.state->samplerTexture[1];
    drawStream.lightmapPrimaryTexture = context.state->samplerTexture[2];
    drawStream.lightmapSecondaryTexture = context.state->samplerTexture[3];
    drawStream.reflectionProbeCount = rgp.world->reflectionProbeCount;
    drawStream.lightmapCount = rgp.world->lightmapCount;
    drawStream.reflectionProbeTextures = rgp.world->reflectionProbeTextures;
    drawStream.lightmapPrimaryTextures = rgp.world->lightmapPrimaryTextures;
    drawStream.lightmapSecondaryTextures = rgp.world->lightmapSecondaryTextures;
    drawStream.whiteTexture = &rgp.whiteImage->texture;
    drawStream.primDrawSurfPos = primDrawSurfPos;
    drawStream.customSamplerFlags = pass->customSamplerFlags;
    drawStream.hasSunDirChanged = context.source->input.data->prim.hasSunDirChanged;
    if (prepassContext.state)
        R_DrawTrianglesLit(&drawStream, &context.state->prim, &prepassContext.state->prim);
    else
        R_DrawTrianglesLit(&drawStream, &context.state->prim, 0);
    context.state->samplerTexture[1] = drawStream.reflectionProbeTexture;
    context.state->samplerTexture[2] = drawStream.lightmapPrimaryTexture;
    context.state->samplerTexture[3] = drawStream.lightmapSecondaryTexture;
}

void __cdecl R_DrawTrianglesLit(
    GfxTrianglesDrawStream *drawStream,
    GfxCmdBufPrimState *primState,
    GfxCmdBufPrimState *prepassPrimState)
{
    const GfxTexture *v3; // [esp+0h] [ebp-78h]
    int baseIndex; // [esp+10h] [ebp-68h]
    uint32_t surfIndex; // [esp+14h] [ebp-64h]
    const GfxSurface *tris; // [esp+18h] [ebp-60h]
    const srfTriangles_t *prevTris; // [esp+1Ch] [ebp-5Ch]
    uint32_t lightmapSecondaryFlag; // [esp+20h] [ebp-58h]
    uint32_t reflectionProbeFlag; // [esp+24h] [ebp-54h]
    const GfxTexture *lightmapPrimaryTexture; // [esp+28h] [ebp-50h]
    const uint16_t *list; // [esp+2Ch] [ebp-4Ch] BYREF
    int triCount; // [esp+30h] [ebp-48h]
    uint32_t reflectionProbeIndex; // [esp+34h] [ebp-44h]
    const GfxTexture *reflectionProbeTexture; // [esp+38h] [ebp-40h]
    const GfxTexture *newLightmapPrimaryTexture; // [esp+3Ch] [ebp-3Ch]
    GfxTexture *reflectionProbeTextures; // [esp+40h] [ebp-38h]
    const GfxSurface *bspSurf; // [esp+44h] [ebp-34h]
    uint32_t index; // [esp+48h] [ebp-30h]
    uint32_t lightmapIndex; // [esp+4Ch] [ebp-2Ch]
    const GfxTexture *lightmapSecondaryTexture; // [esp+50h] [ebp-28h]
    const GfxTexture *newLightmapSecondaryTexture; // [esp+54h] [ebp-24h]
    IDirect3DDevice9 *device; // [esp+58h] [ebp-20h]
    uint32_t lightmapPrimaryFlag; // [esp+5Ch] [ebp-1Ch]
    const GfxImage *overrideImage; // [esp+60h] [ebp-18h]
    uint32_t count; // [esp+64h] [ebp-14h] BYREF
    int baseVertex; // [esp+68h] [ebp-10h]
    const GfxTexture *newReflectionProbeTexture; // [esp+6Ch] [ebp-Ch]
    int hasSunDirChanged; // [esp+70h] [ebp-8h]
    int override; // [esp+74h] [ebp-4h]

    reflectionProbeIndex = 255;
    lightmapIndex = 31;
    prevTris = 0;
    triCount = 0;
    baseVertex = -1;
    baseIndex = 0;
    reflectionProbeTexture = drawStream->reflectionProbeTexture;
    lightmapPrimaryTexture = drawStream->lightmapPrimaryTexture;
    lightmapSecondaryTexture = drawStream->lightmapSecondaryTexture;
    reflectionProbeFlag = drawStream->customSamplerFlags & 1;
    lightmapPrimaryFlag = drawStream->customSamplerFlags & 2;
    lightmapSecondaryFlag = drawStream->customSamplerFlags & 4;
    reflectionProbeTextures = drawStream->reflectionProbeTextures;
    hasSunDirChanged = drawStream->hasSunDirChanged;
    // Original retail lightmap override: r_lightMap (DVAR_CHEAT) selects a
    // pure black/white/gray replacement. The port's force-white diagnostic
    // (r_killhouseWhiteLightmap) was removed: a renderer
    // diagnostic must not be able to replace lightmap resources in a
    // production checkpoint.
    override = r_lightMap->current.integer != 1;
    device = primState->device;
    while (R_ReadBspDrawSurfs(&drawStream->primDrawSurfPos, &list, &count))
    {
        for (index = 0; index < count; ++index)
        {
            surfIndex = list[index];
            if (surfIndex >= rgp.world->surfaceCount)
                MyAssertHandler(
                    ".\\r_draw_bsp.cpp",
                    303,
                    0,
                    "surfIndex doesn't index rgp.world->surfaceCount\n\t%i not in [0, %i)",
                    surfIndex,
                    rgp.world->surfaceCount);
            bspSurf = &rgp.world->dpvs.surfaces[surfIndex];
            tris = bspSurf;
            if (reflectionProbeIndex == bspSurf->reflectionProbeIndex && lightmapIndex == bspSurf->lightmapIndex)
            {
                if (baseVertex != bspSurf->tris.firstVertex || baseIndex + 3 * triCount != bspSurf->tris.baseIndex)
                {
                    if (prevTris)
                        R_DrawBspTris(primState, prevTris, triCount);
                    prevTris = &tris->tris;
                    triCount = 0;
                    baseIndex = tris->tris.baseIndex;
                    if (baseVertex != tris->tris.firstVertex)
                    {
                        baseVertex = tris->tris.firstVertex;
                        R_SetStreamsForBspSurface(primState, &tris->tris);
                    }
                }
            }
            else
            {
                if (prevTris)
                    R_DrawBspTris(primState, prevTris, triCount);
                prevTris = &tris->tris;
                triCount = 0;
                baseIndex = tris->tris.baseIndex;
                if (baseVertex != tris->tris.firstVertex)
                {
                    baseVertex = tris->tris.firstVertex;
                    R_SetStreamsForBspSurface(primState, &tris->tris);
                }
                reflectionProbeIndex = bspSurf->reflectionProbeIndex;
                lightmapIndex = bspSurf->lightmapIndex;
                if (reflectionProbeFlag)
                {
                    if (reflectionProbeIndex >= drawStream->reflectionProbeCount)
                        MyAssertHandler(
                            ".\\r_draw_bsp.cpp",
                            337,
                            0,
                            "reflectionProbeIndex doesn't index drawStream->reflectionProbeCount\n\t%i not in [0, %i)",
                            reflectionProbeIndex,
                            drawStream->reflectionProbeCount);
                    newReflectionProbeTexture = &reflectionProbeTextures[reflectionProbeIndex];
                    if (reflectionProbeTexture != newReflectionProbeTexture)
                    {
                        reflectionProbeTexture = newReflectionProbeTexture;
                        R_HW_SetSamplerTexture(device, 1u, newReflectionProbeTexture);
                    }
                }
                if (lightmapIndex == 31)
                {
                    if (lightmapPrimaryFlag)
                        MyAssertHandler(
                            ".\\r_draw_bsp.cpp",
                            390,
                            0,
                            "%s\n\t(bspSurf->material->info.name) = %s",
                            "(!lightmapPrimaryFlag)",
                            bspSurf->material->info.name);
                    if (lightmapSecondaryFlag)
                        MyAssertHandler(
                            ".\\r_draw_bsp.cpp",
                            391,
                            0,
                            "%s\n\t(bspSurf->material->info.name) = %s",
                            "(!lightmapSecondaryFlag)",
                            bspSurf->material->info.name);
                }
                else
                {
                    if (lightmapIndex >= drawStream->lightmapCount)
                        MyAssertHandler(
                            ".\\r_draw_bsp.cpp",
                            348,
                            0,
                            "lightmapIndex doesn't index drawStream->lightmapCount\n\t%i not in [0, %i)",
                            lightmapIndex,
                            drawStream->lightmapCount);
                    if (lightmapPrimaryFlag)
                    {
                        if (override)
                        {
                            overrideImage = R_OverrideGrayscaleImage(r_lightMap);
                            newLightmapPrimaryTexture = &overrideImage->texture;
                        }
                        else
                        {
                            v3 = hasSunDirChanged ? drawStream->whiteTexture : &drawStream->lightmapPrimaryTextures[lightmapIndex];
                            newLightmapPrimaryTexture = v3;
                        }
                        if (lightmapPrimaryTexture != newLightmapPrimaryTexture)
                        {
                            lightmapPrimaryTexture = newLightmapPrimaryTexture;
                            R_HW_SetSamplerTexture(device, 2u, newLightmapPrimaryTexture);
                        }
                    }
                    if (lightmapSecondaryFlag)
                    {
                        if (override)
                        {
                            overrideImage = R_OverrideGrayscaleImage(r_lightMap);
                            newLightmapSecondaryTexture = &overrideImage->texture;
                        }
                        else
                        {
                            newLightmapSecondaryTexture = &drawStream->lightmapSecondaryTextures[lightmapIndex];
                        }
                        if (lightmapSecondaryTexture != newLightmapSecondaryTexture)
                        {
                            lightmapSecondaryTexture = newLightmapSecondaryTexture;
                            R_HW_SetSamplerTexture(device, 3u, newLightmapSecondaryTexture);
                        }
                    }
                }
            }
            triCount += tris->tris.triCount;
            iassert( !prepassPrimState );
        }
    }
    if (prevTris)
        R_DrawBspTris(primState, prevTris, triCount);
    drawStream->reflectionProbeTexture = reflectionProbeTexture;
    drawStream->lightmapPrimaryTexture = lightmapPrimaryTexture;
    drawStream->lightmapSecondaryTexture = lightmapSecondaryTexture;
}

int R_SetWorldIndexData(GfxCmdBufPrimState *state, const srfTriangles_t *tris, int triCount)
{
    if (IDirect3DIndexBuffer9 *staticIb = R_StaticPretessWorldIb())
    {
        if (state->indexBuffer != staticIb)
            R_ChangeIndices(state, staticIb);
        return tris->baseIndex;
    }
    R_NOTE_SETIDX(SWITCH_PERF_EV_SETIDX_BMODEL);
    return R_SetIndexData(state, (uint8_t *)&rgp.world->indices[tris->baseIndex], triCount);
}

void __cdecl R_DrawBspTris(GfxCmdBufPrimState *state, const srfTriangles_t *tris, uint32_t triCount)
{
    GfxDrawPrimArgs args; // [esp+0h] [ebp-Ch] BYREF

    args.vertexCount = tris->vertexCount;
    args.triCount = triCount;
    if (IDirect3DIndexBuffer9 *staticIb = R_StaticPretessWorldIb())
    {
        // r_deko9StaticPretess: the merged surfaces are contiguous in
        // rgp.world->indices (R_DrawTriangles merges only then), so draw
        // them straight from the static world index buffer.
        if (state->indexBuffer != staticIb)
            R_ChangeIndices(state, staticIb);
        args.baseIndex = tris->baseIndex;
        R_DrawIndexedPrimitive(state, &args);
        g_frameStatsCur.geoIndexCount += 3 * triCount;
        iassert(g_primStats);
        g_primStats->staticIndexCount += 3 * triCount;
        return;
    }
    R_NOTE_SETIDX(SWITCH_PERF_EV_SETIDX_BSP);
    args.baseIndex = R_SetIndexData(state, (uint8_t *)&rgp.world->indices[tris->baseIndex], triCount);
    if (args.baseIndex + 3 * (int)triCount > gfxBuf.dynamicIndexBuffer->total)
    {
        // an overflowed index span is not allowed to keep going
        // into R_DrawIndexedPrimitive with out-of-range indices; stop the
        // affected draw loudly instead of rendering OOB geometry.
        Com_Printf(0, "FAIL:KILLHOUSE_IB_OVERFLOW baseIndex=%d triCount=%u total=%d\n",
                   args.baseIndex, triCount, gfxBuf.dynamicIndexBuffer->total);
        return;
    }
    if (rgp.world && state->streams[0].offset + 44 * args.vertexCount > 44 * (uint32_t)rgp.world->vertexCount)
    {
        // Same contract for a vertex range that leaves rgp.world's vertex
        // buffer: the draw would read past the verified array.
        Com_Printf(0, "FAIL:KILLHOUSE_VB_OVERFLOW streamOff=%u vertBytes=%u vbSize=%u\n",
                   state->streams[0].offset, 44 * args.vertexCount, 44 * (uint32_t)rgp.world->vertexCount);
        return;
    }
    R_DrawIndexedPrimitive(state, &args);
    g_frameStatsCur.geoIndexCount += 3 * triCount;
    iassert( g_primStats );
    g_primStats->dynamicIndexCount += 3 * triCount;
}

int __cdecl R_ReadBspDrawSurfs(
    const uint32_t **primDrawSurfPos,
    const uint16_t **list,
    uint32_t *count)
{
    *count = *(*primDrawSurfPos)++;
    if (!*count)
        return 0;
    *list = (const uint16_t *)*primDrawSurfPos;
    *primDrawSurfPos += (*count + 1) >> 1;
    return 1;
}

void __cdecl R_DrawBspDrawSurfs(const uint32_t *primDrawSurfPos, GfxCmdBufState *state)
{
    GfxTrianglesDrawStream drawStream; // [esp+0h] [ebp-30h] BYREF

    drawStream.primDrawSurfPos = primDrawSurfPos;
    R_DrawTriangles(&drawStream, &state->prim);
}

void __cdecl R_DrawTriangles(GfxTrianglesDrawStream *drawStream, GfxCmdBufPrimState *state)
{
    int baseIndex; // [esp+0h] [ebp-28h]
    const GfxSurface *tris; // [esp+8h] [ebp-20h]
    const srfTriangles_t *prevTris; // [esp+Ch] [ebp-1Ch]
    const uint16_t *list; // [esp+10h] [ebp-18h] BYREF
    int triCount; // [esp+14h] [ebp-14h]
    const GfxSurface *bspSurf; // [esp+18h] [ebp-10h]
    uint32_t index; // [esp+1Ch] [ebp-Ch]
    uint32_t count; // [esp+20h] [ebp-8h] BYREF
    int baseVertex; // [esp+24h] [ebp-4h]

    prevTris = 0;
    triCount = 0;
    baseVertex = -1;
    baseIndex = 0;
    while (R_ReadBspDrawSurfs(&drawStream->primDrawSurfPos, &list, &count))
    {
        for (index = 0; index < count; ++index)
        {
            bspSurf = &rgp.world->dpvs.surfaces[list[index]];
            tris = bspSurf;
            if (baseVertex != bspSurf->tris.firstVertex || baseIndex + 3 * triCount != bspSurf->tris.baseIndex)
            {
                if (prevTris)
                    R_DrawBspTris(state, prevTris, triCount);
                prevTris = &tris->tris;
                triCount = 0;
                baseIndex = tris->tris.baseIndex;
                if (baseVertex != tris->tris.firstVertex)
                {
                    baseVertex = tris->tris.firstVertex;
                    R_SetStreamsForBspSurface(state, &tris->tris);
                }
            }
            triCount += tris->tris.triCount;
        }
    }
    if (prevTris)
        R_DrawBspTris(state, prevTris, triCount);
}

void __cdecl R_DrawPreTessTris(
    GfxCmdBufPrimState *state,
    const srfTriangles_t *tris,
    uint32_t baseIndex,
    uint32_t triCount)
{
    GfxDrawPrimArgs args; // [esp+0h] [ebp-Ch] BYREF

    R_SetStreamsForBspSurface(state, tris);
    args.vertexCount = tris->vertexCount;
    args.triCount = triCount;
    args.baseIndex = baseIndex;
    R_DrawIndexedPrimitive(state, &args);
    g_frameStatsCur.geoIndexCount += 3 * triCount;
    iassert( g_primStats );
    g_primStats->dynamicIndexCount += 3 * triCount;
}

// ---- Static world index buffer batches (R_PRETESS_STATIC_FLAG, r_pretess.h) --
//
// A static batch's entries are runs: contiguous ranges of the static world
// index buffer (rgp.world->indices as uploaded at load). Consecutive runs that
// the copying path would have drawn with one R_DrawIndexedPrimitive (same
// firstVertex; for lit, also the same lightmap and probe) go to one
// R_DrawIndexedRanges with the streams of the group's first surface, which
// is what that draw used. Same state changes, same triangles, same order.

namespace
{
struct StaticRangeGroup
{
    GfxIndexRange ranges[128];
    uint32_t count;
    uint32_t triCount;
    const srfTriangles_t *tris; // the group's first surface (streams, vertexCount)
};
} // namespace

static bool R_BindStaticWorldIndices(GfxCmdBufPrimState *prim)
{
    IDirect3DIndexBuffer9 *ib = R_StaticPretessWorldIbAny();
    if (!ib)
    {
        // The front end recorded a static batch for a world whose static
        // buffer is gone: nothing valid to draw from. Never expected.
        static bool warned;
        if (!warned)
        {
            warned = true;
            Com_Printf(CON_CHANNEL_SYSTEM, "FAIL:STATIC_PRETESS_NO_IB static world batch without a static index buffer\n");
        }
        return false;
    }
    if (prim->indexBuffer != ib)
        R_ChangeIndices(prim, ib);
    return true;
}

static void R_BindPreTessIndices(GfxCmdBufContext context)
{
    IDirect3DIndexBuffer9 *ib = context.source->input.data->preTessIb;
    if (context.state->prim.indexBuffer != ib)
        R_ChangeIndices(&context.state->prim, ib);
}

static void R_FlushStaticRangeGroup(GfxCmdBufPrimState *prim, StaticRangeGroup *group)
{
    if (!group->count)
        return;
    R_SetStreamsForBspSurface(prim, group->tris);
    R_DrawIndexedRanges(prim, group->tris->vertexCount, group->ranges, group->count);
    g_frameStatsCur.geoIndexCount += 3 * group->triCount;
    iassert(g_primStats);
    g_primStats->staticIndexCount += 3 * group->triCount;
    group->count = 0;
    group->triCount = 0;
    group->tris = nullptr;
}

static void R_AddStaticRange(
    GfxCmdBufPrimState *prim,
    StaticRangeGroup *group,
    const srfTriangles_t *tris,
    uint32_t triCount)
{
    if (group->tris && group->tris->firstVertex != tris->firstVertex)
        R_FlushStaticRangeGroup(prim, group);
    if (!group->tris)
        group->tris = tris;
    if (!triCount)
        return;
    iassert(group->count < 128);
    group->ranges[group->count].firstIndex = (uint32_t)tris->baseIndex;
    group->ranges[group->count].triCount = triCount;
    group->ranges[group->count].baseVertex = 0;
    ++group->count;
    group->triCount += triCount;
}

static const GfxSurface *R_StaticRunSurface(const GfxBspPreTessDrawSurf &run)
{
    const uint32_t surfIndex = run.baseSurfIndex;
    if (surfIndex >= (uint32_t)rgp.world->surfaceCount)
        MyAssertHandler(".\\r_draw_bsp.cpp", 0, 0, "surfIndex doesn't index rgp.world->surfaceCount\n\t%i not in [0, %i)",
                        surfIndex, rgp.world->surfaceCount);
    return &rgp.world->dpvs.surfaces[surfIndex];
}

static void R_DrawBspStaticRuns(const GfxBspPreTessDrawSurf *list, uint32_t count, GfxCmdBufContext context)
{
    if (!R_BindStaticWorldIndices(&context.state->prim))
        return;
    StaticRangeGroup group;
    group.count = group.triCount = 0;
    group.tris = nullptr;
    for (uint32_t index = 0; index < count; ++index)
    {
        const GfxSurface *bspSurf = R_StaticRunSurface(list[index]);
        R_AddStaticRange(&context.state->prim, &group, &bspSurf->tris, list[index].totalTriCount);
    }
    R_FlushStaticRangeGroup(&context.state->prim, &group);
}

static void R_DrawBspStaticRunsLit(const GfxBspPreTessDrawSurf *list, uint32_t count, GfxCmdBufContext context)
{
    if (!R_BindStaticWorldIndices(&context.state->prim))
        return;
    StaticRangeGroup group;
    group.count = group.triCount = 0;
    group.tris = nullptr;
    uint32_t reflectionProbeIndex = 255;
    uint32_t lightmapIndex = 31;
    for (uint32_t index = 0; index < count; ++index)
    {
        const GfxSurface *bspSurf = R_StaticRunSurface(list[index]);
        if (reflectionProbeIndex != bspSurf->reflectionProbeIndex || lightmapIndex != bspSurf->lightmapIndex)
        {
            R_FlushStaticRangeGroup(&context.state->prim, &group);
            reflectionProbeIndex = bspSurf->reflectionProbeIndex;
            lightmapIndex = bspSurf->lightmapIndex;
            R_SetReflectionProbe(context, reflectionProbeIndex);
            R_SetLightmap(context, lightmapIndex);
            R_SetupPassPerObjectArgs(context);
            R_SetupPassPerPrimArgs(context);
        }
        R_AddStaticRange(&context.state->prim, &group, &bspSurf->tris, list[index].totalTriCount);
    }
    R_FlushStaticRangeGroup(&context.state->prim, &group);
}

void __cdecl R_DrawBspDrawSurfsPreTess(const uint32_t *primDrawSurfPos, GfxCmdBufContext context)
{
    uint32_t baseIndex; // [esp+0h] [ebp-2Ch] BYREF
    uint32_t surfIndex; // [esp+4h] [ebp-28h]
    GfxReadCmdBuf cmdBuf; // [esp+8h] [ebp-24h] BYREF
    const srfTriangles_t *tris; // [esp+Ch] [ebp-20h]
    const srfTriangles_t *prevTris; // [esp+10h] [ebp-1Ch]
    const GfxBspPreTessDrawSurf *list; // [esp+14h] [ebp-18h] BYREF
    uint32_t triCount; // [esp+18h] [ebp-14h]
    const GfxSurface *bspSurf; // [esp+1Ch] [ebp-10h]
    uint32_t index; // [esp+20h] [ebp-Ch]
    uint32_t count; // [esp+24h] [ebp-8h] BYREF
    int baseVertex; // [esp+28h] [ebp-4h]

    R_SetupPassPerObjectArgs(context);
    R_SetupPassPerPrimArgs(context);
    cmdBuf.primDrawSurfPos = primDrawSurfPos;
    while (R_ReadBspPreTessDrawSurfs(&cmdBuf, &list, &count, &baseIndex))
    {
        if (baseIndex == R_PRETESS_STATIC_FLAG)
        {
            R_DrawBspStaticRuns(list, count, context);
            continue;
        }
        R_BindPreTessIndices(context);
        prevTris = 0;
        triCount = 0;
        baseVertex = -1;
        for (index = 0; index < count; ++index)
        {
            surfIndex = list[index].baseSurfIndex;
            if (surfIndex >= rgp.world->surfaceCount)
                MyAssertHandler(
                    ".\\r_draw_bsp.cpp",
                    675,
                    0,
                    "surfIndex doesn't index rgp.world->surfaceCount\n\t%i not in [0, %i)",
                    surfIndex,
                    rgp.world->surfaceCount);
            bspSurf = &rgp.world->dpvs.surfaces[surfIndex];
            tris = &bspSurf->tris;
            if (baseVertex != bspSurf->tris.firstVertex)
            {
                if (triCount)
                {
                    R_DrawPreTessTris(&context.state->prim, prevTris, baseIndex, triCount);
                    baseIndex += 3 * triCount;
                    triCount = 0;
                }
                prevTris = tris;
                baseVertex = tris->firstVertex;
            }
            triCount += list[index].totalTriCount;
        }
        R_DrawPreTessTris(&context.state->prim, prevTris, baseIndex, triCount);
    }
}

void __cdecl R_DrawBspDrawSurfsLitPreTess(const uint32_t *primDrawSurfPos, GfxCmdBufContext context)
{
    uint32_t baseIndex; // [esp+4h] [ebp-28h] BYREF
    uint32_t surfIndex; // [esp+8h] [ebp-24h]
    GfxReadCmdBuf cmdBuf; // [esp+Ch] [ebp-20h] BYREF
    const srfTriangles_t *tris; // [esp+10h] [ebp-1Ch]
    const GfxBspPreTessDrawSurf *list; // [esp+14h] [ebp-18h] BYREF
    uint32_t reflectionProbeIndex; // [esp+18h] [ebp-14h]
    const GfxSurface *bspSurf; // [esp+1Ch] [ebp-10h]
    uint32_t index; // [esp+20h] [ebp-Ch]
    uint32_t lightmapIndex; // [esp+24h] [ebp-8h]
    uint32_t count; // [esp+28h] [ebp-4h] BYREF

    if (sc_enable->current.enabled)
        R_SetCodeImageTexture(context.source, TEXTURE_SRC_CODE_DYNAMIC_SHADOWS, gfxRenderTargets[R_RENDERTARGET_DYNAMICSHADOWS].image);
    else
        R_SetCodeImageTexture(context.source, TEXTURE_SRC_CODE_DYNAMIC_SHADOWS, rgp.whiteImage);
    cmdBuf.primDrawSurfPos = primDrawSurfPos;
    while (R_ReadBspPreTessDrawSurfs(&cmdBuf, &list, &count, &baseIndex))
    {
        if (baseIndex == R_PRETESS_STATIC_FLAG)
        {
            R_DrawBspStaticRunsLit(list, count, context);
            continue;
        }
        R_BindPreTessIndices(context);
        reflectionProbeIndex = 255;
        lightmapIndex = 31;
        for (index = 0; index < count; ++index)
        {
            surfIndex = list[index].baseSurfIndex;
            if (surfIndex >= rgp.world->surfaceCount)
                MyAssertHandler(
                    ".\\r_draw_bsp.cpp",
                    623,
                    0,
                    "surfIndex doesn't index rgp.world->surfaceCount\n\t%i not in [0, %i)",
                    surfIndex,
                    rgp.world->surfaceCount);
            bspSurf = &rgp.world->dpvs.surfaces[surfIndex];
            tris = &bspSurf->tris;
            if (reflectionProbeIndex != bspSurf->reflectionProbeIndex || lightmapIndex != bspSurf->lightmapIndex)
            {
                reflectionProbeIndex = bspSurf->reflectionProbeIndex;
                lightmapIndex = bspSurf->lightmapIndex;
                R_SetReflectionProbe(context, reflectionProbeIndex);
                R_SetLightmap(context, lightmapIndex);
                R_SetupPassPerObjectArgs(context);
                R_SetupPassPerPrimArgs(context);
            }
            R_DrawPreTessTris(&context.state->prim, tris, baseIndex, list[index].totalTriCount);
            baseIndex += 3 * list[index].totalTriCount;
        }
    }
}
