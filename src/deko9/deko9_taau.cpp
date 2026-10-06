// TAAU resolve pass of the deko3d renderer (see deko9_taau.h).
//
// One native full-screen draw at the output size reads the scene colour and
// depth (render size) and the previous history, and renders the result into
// the target and the next history at once (two attachments). The history is
// RGB10A2 (the scene is 8-bit; the 2-bit alpha keeps the pixel's youth):
// half the bytes of RGBA16F on the read and the write. The program comes in
// the kTaauBilinear* variants (TaauFrame picks one per resolve).
//
// Object motion: TaauMotion clears a scene-size RG16F image to
// kTaauMotionNone and draws the moving surfaces into it with the scene depth
// bound read-only (deko9_taau.h TaauMotionDraw); the next resolve samples it
// and drops it. Frames without moving surfaces skip the pass, and the
// resolve reads no motion.
//
// Reactive weighting: TaauOpaque writes the luma of the scene colour into a
// scene-size (or half-size) part of an R16F image allocated for the scene's
// capacity (a dynamic-resolution scale change keeps it) before the
// transparent passes and subtracts it from the luma after them (blend),
// leaving the change the transparents made; post effects after that (bloom,
// film grading) do not count. The next resolve weighs the current frame up
// where the change is large.
//
// Ownership: the history images are device-owned stores; the hazard tracker
// orders render -> sample between consecutive resolves, and a resize retires
// the old images through DestroyStoreAfter (freed once the open list's fence
// passed), as every other store.

#include "deko9_internal.h"
#include "deko9_native.h"
#include "deko9_taau.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace deko9
{

void Device::ReleaseTaau()
{
    for (ShaderVariant &variant : m_taau.ps)
        FreeVariant(this, &variant, m_openSeq);
    for (ShaderVariant *variant : {&m_taau.motionVs, &m_taau.motionPs, &m_taau.opaquePs[0], &m_taau.opaquePs[1]})
        FreeVariant(this, variant, m_openSeq);
    for (GpuAlloc *alloc : {&m_taau.constants, &m_taau.timestamps, &m_taau.motionConstants})
    {
        if (*alloc)
            FreeMemoryAfter(*alloc, m_openSeq);
        *alloc = {};
    }
    m_taau.history[0].reset();
    m_taau.history[1].reset();
    m_taau.motion.reset();
    m_taau.opaque.reset();
    m_taau.valid = false;
    m_taau.motionReady = m_taau.opaqueReady = m_taau.reactiveReady = false;
}

namespace
{
// A device-owned render-target texture.
std::shared_ptr<ImageStore> MakeTarget(Device *device, D3DFORMAT format, uint32_t width, uint32_t height,
                                       std::string *error)
{
    std::shared_ptr<ImageStore> store = device->MakeStore();
    store->format = LookupFormat(format);
    store->type = D3DRTYPE_TEXTURE;
    store->pool = D3DPOOL_DEFAULT;
    store->usage = D3DUSAGE_RENDERTARGET;
    store->width = width;
    store->height = height;
    store->levels = 1;
    if (!device->CreateStore(store.get(), error))
        store.reset();
    return store;
}
} // namespace

bool Device::TaauOpaque(ImageStore *color, const int32_t srcRect[4], bool after, bool half, std::string *error)
{
    const bool haveOpaque = m_taau.opaqueReady;
    m_taau.opaqueReady = m_taau.reactiveReady = false;
    if (!color || !color->gpu || color->type != D3DRTYPE_TEXTURE || color->format->depth ||
        color->descriptor == UINT32_MAX)
        return *error = "needs a colour texture", false;
    const int32_t *sr = srcRect;
    if (sr[0] < 0 || sr[1] < 0 || sr[2] <= 0 || sr[3] <= 0 || sr[0] + sr[2] > (int32_t)color->width ||
        sr[1] + sr[3] > (int32_t)color->height)
        return *error = "rectangle outside the colour texture", false;
    const TaauExtent used = TaauReactiveExtent(color->width, color->height, half);
    // The after pass subtracts from this frame's snapshot; without one (the
    // view had no before pass) there is nothing to weigh.
    if (after && (!haveOpaque || !m_taau.opaque || m_taau.opaqueUsed != used || m_taau.opaqueHalf != half))
        return true;
    if (!EnsureUpscaler(error))
        return false;
    std::string detail;
    ShaderVariant *program = &m_taau.opaquePs[half];
    if (!program->code &&
        !LoadDkshCached(this, DEKO9_STAGE_PIXEL, TaauVariant(kTaauOpaqueGlsl, half ? kTaauHalfReactive : 0).c_str(),
                        program, &detail))
        return *error = "opaque program: " + detail, false;
    // Sized for the scene's capacity, so a dynamic-resolution scale change
    // keeps the image; only the reactive mode (or a larger scene) replaces it.
    const TaauExtent capacity =
        TaauReactiveExtent(std::max(color->capacityWidth, color->width), std::max(color->capacityHeight, color->height),
                           half);
    if (!m_taau.opaque || m_taau.opaque->width != capacity.width || m_taau.opaque->height != capacity.height)
    {
        m_taau.opaque = MakeTarget(this, D3DFMT_R16F, capacity.width, capacity.height, &detail);
        if (!m_taau.opaque)
            return *error = "opaque image: " + detail, false;
        ++m_taau.opaqueAllocs;
    }
    m_taau.opaqueUsed = used;
    m_taau.opaqueHalf = half;
    ImageStore *opaque = m_taau.opaque.get();
    uint32_t rect[4];
    TaauReactiveRect(sr, half, used, capacity, rect);
    HazardBegin();
    HazardAdd(color, Access::Sample);
    HazardAdd(opaque, Access::Render);
    HazardCommit();
    FlushDescriptors();
    DkImageView view;
    dkImageViewDefaults(&view, &opaque->image);
    const DkImageView *target = &view;
    CmdBindTargets(&target, 1, nullptr);
    BindFullScreenState(Rec(), rect[2], rect[3], rect[0], rect[1]);
    if (after)
    {
        // dst = luma now - luma before: the transparents' change, signed.
        DkColorState blend;
        dkColorStateDefaults(&blend);
        dkColorStateSetBlendEnable(&blend, 0, true);
        dkCmdBufBindColorState(Rec(), &blend);
        DkBlendState state;
        dkBlendStateDefaults(&state);
        dkBlendStateSetFactors(&state, DkBlendFactor_One, DkBlendFactor_One, DkBlendFactor_One, DkBlendFactor_One);
        dkBlendStateSetOps(&state, DkBlendOp_Sub, DkBlendOp_Sub);
        dkCmdBufBindBlendStates(Rec(), 0, &state, 1);
    }
    const DkShader *shaders[2] = {&m_fsr.vs.shader, &program->shader};
    dkCmdBufBindShaders(Rec(), DkStageFlag_GraphicsMask, shaders, 2);
    SamplerKey pointKey = LinearClampKey();
    pointKey.state[D3DSAMP_MAGFILTER] = pointKey.state[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    const DkResHandle handle =
        dkMakeTextureHandle(color->descriptor, half ? m_fsr.sampler : SamplerDescriptor(pointKey));
    dkCmdBufBindTextures(Rec(), DkStage_Fragment, 0, &handle, 1);
    dkCmdBufDraw(Rec(), DkPrimitive_Triangles, 3, 1, 0, 0);
    if (after)
    {
        DkColorState plain;
        dkColorStateDefaults(&plain);
        dkCmdBufBindColorState(Rec(), &plain);
    }
    (after ? m_taau.reactiveReady : m_taau.opaqueReady) = true;
    EndNativePass();
    return true;
}

bool Device::TaauMotion(Surface *depthSurface, const int32_t srcRect[4], const TaauMotionView &view,
                        const TaauMotionDraw *draws, uint32_t count, std::string *error)
{
    m_taau.motionReady = false;
    ImageStore *depth = depthSurface ? depthSurface->Store().get() : nullptr;
    if (!depth || !depth->gpu || !depth->format->depth || depthSurface->Level())
        return *error = "needs the scene depth surface", false;
    const int32_t *sr = srcRect;
    if (sr[0] < 0 || sr[1] < 0 || sr[2] <= 0 || sr[3] <= 0 || sr[0] + sr[2] > (int32_t)depth->width ||
        sr[1] + sr[3] > (int32_t)depth->height)
        return *error = "rectangle outside the depth surface", false;
    if (!count)
        return true;
    std::string detail;
    if ((!m_taau.motionVs.code &&
         !LoadDkshCached(this, DEKO9_STAGE_VERTEX, kTaauMotionVertexGlsl, &m_taau.motionVs, &detail)) ||
        (!m_taau.motionPs.code &&
         !LoadDkshCached(this, DEKO9_STAGE_PIXEL, kTaauMotionFragmentGlsl, &m_taau.motionPs, &detail)))
        return *error = "motion program: " + detail, false;
    if (!m_taau.motionConstants &&
        !AllocMemory(POOL_BUFFER, 256, DK_UNIFORM_BUF_ALIGNMENT, &m_taau.motionConstants))
        return *error = "motion constant memory allocation failed", false;
    if (!m_taau.motion || m_taau.motion->width != depth->width || m_taau.motion->height != depth->height)
    {
        m_taau.motion = MakeTarget(this, D3DFMT_G16R16F, depth->width, depth->height, &detail);
        if (!m_taau.motion)
            return *error = "motion image: " + detail, false;
        ++m_taau.motionAllocs;
    }
    // Every buffer resolves before any command: a malformed draw fails the
    // pass instead of leaving it half recorded. A range outside its buffers
    // (offsets derived from the previous frame) only skips that draw.
    std::vector<uint8_t> &fits = m_taau.motionFits;
    fits.assign(count, 0);
    uint32_t drawn = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const TaauMotionDraw &d = draws[i];
        Buffer *vb = d.vb ? &static_cast<VertexBuffer *>(d.vb)->buffer : nullptr;
        Buffer *prev = d.prevVb ? &static_cast<VertexBuffer *>(d.prevVb)->buffer : vb;
        const uint32_t prevOffset = d.prevVb ? d.prevVbOffset : d.vbOffset;
        IndexBuffer *ib = d.ib ? static_cast<IndexBuffer *>(d.ib) : nullptr;
        if (!vb || !ib || ib->Format() != D3DFMT_INDEX16 || d.stride < 12 || !d.indexCount)
            return *error = "draw needs a vertex buffer, 16-bit indices and a stride of at least 12", false;
        const TaauMotionFit fit = TaauMotionDrawFits(ib->buffer.Size() / 2, d.firstIndex, d.indexCount, vb->Size(),
                                                     d.vbOffset, prev->Size(), prevOffset, d.vertexCount, d.stride);
        if (fit != TaauMotionFit::Ok)
        {
            // One detailed line per report window; the count goes into the
            // periodic "taau" line.
            if (!m_taau.motionSkips++)
                Log("taau motion: skipped draw %u of %u (%s): first=%u count=%u ib=%u indices vb=%u+%ux%u of %u "
                    "prev=%u of %u",
                    i, count, TaauMotionFitName(fit), d.firstIndex, d.indexCount, ib->buffer.Size() / 2, d.vbOffset,
                    d.vertexCount, d.stride, vb->Size(), prevOffset, prev->Size());
            ++m_taau.motionSkipsTotal;
            continue;
        }
        fits[i] = 1;
        ++drawn;
    }
    if (!drawn)
        return true;

    HazardBegin();
    HazardAdd(depth, Access::Render);
    HazardAdd(m_taau.motion.get(), Access::Render);
    HazardCommit();
    DkImageView motionView, depthView;
    dkImageViewDefaults(&motionView, &m_taau.motion->image);
    depthSurface->MakeView(&depthView);
    const DkImageView *targets = &motionView;
    CmdBindTargets(&targets, 1, &depthView);
    const DkScissor whole{0, 0, depth->width, depth->height};
    dkCmdBufSetScissors(Rec(), 0, &whole, 1);
    const float none[4] = {kTaauMotionNone, kTaauMotionNone, 0.0f, 0.0f};
    dkCmdBufClearColor(Rec(), 0, DkColorMask_RGBA, none);
    ++m_cc.clears;
    const DkScissor scissor{(uint32_t)sr[0], (uint32_t)sr[1], (uint32_t)sr[2], (uint32_t)sr[3]};
    dkCmdBufSetScissors(Rec(), 0, &scissor, 1);

    DkRasterizerState raster;
    dkRasterizerStateDefaults(&raster);
    raster.cullMode = DkFace_None;
    raster.depthBiasEnableMask = DkPolygonFlag_All;
    dkCmdBufBindRasterizerState(Rec(), &raster);
    dkCmdBufSetDepthBias(Rec(), kTaauMotionDepthBias, 0.0f, kTaauMotionSlopeBias);
    DkColorState color;
    dkColorStateDefaults(&color);
    dkCmdBufBindColorState(Rec(), &color);
    DkColorWriteState colorWrite;
    dkColorWriteStateDefaults(&colorWrite);
    dkCmdBufBindColorWriteState(Rec(), &colorWrite);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = true;
    ds.depthWriteEnable = false;
    ds.depthCompareOp = DkCompareOp_Lequal;
    ds.stencilTestEnable = false;
    dkCmdBufBindDepthStencilState(Rec(), &ds);
    DkVtxAttribState attribs[2]{};
    for (uint32_t a = 0; a < 2; ++a)
    {
        attribs[a].bufferId = a;
        attribs[a].size = DkVtxAttribSize_3x32;
        attribs[a].type = DkVtxAttribType_Float;
    }
    dkCmdBufBindVtxAttribState(Rec(), attribs, 2);
    const DkShader *shaders[2] = {&m_taau.motionVs.shader, &m_taau.motionPs.shader};
    dkCmdBufBindShaders(Rec(), DkStageFlag_GraphicsMask, shaders, 2);
    const DkBufExtents ubo{m_taau.motionConstants.gpu, 256};
    dkCmdBufBindUniformBuffers(Rec(), DkStage_Vertex, 0, &ubo, 1);

    // D3D9 pixel centres, as the scene's own draws (deko9_draw.cpp).
    constexpr float kPixelCentre = 0.5f - 1.0f / 128.0f;
    int viewport = -1;
    uint32_t stride = 0;
    DkGpuAddr indexAddress = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const TaauMotionDraw &d = draws[i];
        if (!fits[i])
            continue;
        if ((int)d.viewmodel != viewport)
        {
            viewport = d.viewmodel;
            const float *z = d.viewmodel ? view.viewmodelDepth : view.sceneDepth;
            const DkViewport vp{(float)sr[0] + kPixelCentre, (float)sr[1] + kPixelCentre, (float)sr[2], (float)sr[3],
                                z[0], z[1]};
            dkCmdBufSetViewports(Rec(), 0, &vp, 1);
        }
        if (d.stride != stride)
        {
            stride = d.stride;
            const DkVtxBufferState states[2] = {{stride, 0}, {stride, 0}};
            dkCmdBufBindVtxBufferState(Rec(), states, 2);
        }
        // The device lock keeps validated buffer objects alive through recording.
        // Resolve their addresses directly instead of allocating per-pass scratch.
        Buffer *vb = &static_cast<VertexBuffer *>(d.vb)->buffer;
        Buffer *prev = d.prevVb ? &static_cast<VertexBuffer *>(d.prevVb)->buffer : vb;
        Buffer *ib = &static_cast<IndexBuffer *>(d.ib)->buffer;
        const uint32_t prevOffset = d.prevVb ? d.prevVbOffset : d.vbOffset;
        const DkBufExtents vbs[2] = {{vb->Gpu() + d.vbOffset, vb->Size() - d.vbOffset},
                                     {prev->Gpu() + prevOffset, prev->Size() - prevOffset}};
        BindVtxBuffers(Rec(), 0, vbs, 2);
        if (ib->Gpu() != indexAddress)
        {
            indexAddress = ib->Gpu();
            dkCmdBufBindIdxBuffer(Rec(), DkIdxFormat_Uint16, indexAddress);
        }
        vb->StampUse(this, m_openSeq);
        prev->StampUse(this, m_openSeq);
        ib->StampUse(this, m_openSeq);
        TaauMotionConstants constants;
        TaauMotionSetup(&constants, d, view);
        dkCmdBufPushConstants(Rec(), m_taau.motionConstants.gpu, 256, 0, sizeof(constants), &constants);
        dkCmdBufDrawIndexed(Rec(), DkPrimitive_Triangles, d.indexCount, 1, d.firstIndex, 0, 0);
    }
    m_taau.motionDraws += drawn;
    m_taau.motionReady = true;
    // Slot 0 is the engine's vertex constant block, bound once per list.
    const DkBufExtents vsUbo{m_vsUbo.gpu, m_vsUbo.size};
    dkCmdBufBindUniformBuffers(Rec(), DkStage_Vertex, 0, &vsUbo, 1);
    EndNativePass();
    return true;
}

bool Device::TaauResolve(ImageStore *color, Surface *depthSurface, const int32_t srcRect[4], Surface *dstSurface,
                         const int32_t dstRect[4], const TaauFrame &frame, std::string *error)
{
    ImageStore *depth = depthSurface ? depthSurface->Store().get() : nullptr;
    ImageStore *dst = dstSurface ? dstSurface->Store().get() : nullptr;
    if (!color || !color->gpu || color->type != D3DRTYPE_TEXTURE || color->format->depth ||
        color->descriptor == UINT32_MAX || !depth || !depth->gpu || !depth->format->depth || !dst || !dst->gpu ||
        dst->format->depth || !(dst->usage & D3DUSAGE_RENDERTARGET) || dstSurface->Level())
        return *error = "needs a colour texture, a depth surface and a colour render target", false;
    if (depth->width != color->width || depth->height != color->height)
        return *error = "scene colour and depth differ in size", false;
    const int32_t *sr = srcRect, *dr = dstRect;
    if (sr[0] < 0 || sr[1] < 0 || sr[2] <= 0 || sr[3] <= 0 || sr[0] + sr[2] > (int32_t)color->width ||
        sr[1] + sr[3] > (int32_t)color->height || dr[0] < 0 || dr[1] < 0 || dr[2] <= 0 || dr[3] <= 0 ||
        dr[0] + dr[2] > (int32_t)dst->width || dr[1] + dr[3] > (int32_t)dst->height)
        return *error = "rectangle outside its image", false;
    if (!EnsureUpscaler(error))
        return false;
    std::string detail;
    const uint32_t variant =
        (frame.bilinearHistory ? kTaauBilinearHistory : 0) | (frame.bilinearCurrent ? kTaauBilinearCurrent : 0);
    ShaderVariant *program = &m_taau.ps[variant];
    if (!program->code && !LoadDkshCached(this, DEKO9_STAGE_PIXEL, TaauVariant(kTaauResolveGlsl, variant).c_str(),
                                          program, &detail))
        return *error = "resolve program: " + detail, false;
    if (!m_taau.constants &&
        (!AllocMemory(POOL_BUFFER, 256, DK_UNIFORM_BUF_ALIGNMENT, &m_taau.constants) ||
         !AllocMemory(POOL_DYNAMIC, TaauState::kSlots * 32, 256, &m_taau.timestamps)))
        return *error = "constant/timestamp memory allocation failed", false;
    if (!EnsureDepthDescriptor(depth))
        return *error = "depth descriptor allocation failed", false;
    if (!m_taau.history[0] || m_taau.history[0]->width != dst->width || m_taau.history[0]->height != dst->height)
    {
        for (auto &history : m_taau.history)
        {
            history = MakeTarget(this, D3DFMT_A2B10G10R10, dst->width, dst->height, &detail);
            if (!history)
            {
                m_taau.history[0].reset();
                m_taau.history[1].reset();
                return *error = "history: " + detail, false;
            }
        }
        m_taau.valid = false;
    }
    ImageStore *prev = m_taau.history[m_taau.next ^ 1].get(), *next = m_taau.history[m_taau.next].get();
    TaauFrame f = frame;
    m_taau.resets += f.reset || !m_taau.valid;
    f.reset = f.reset || !m_taau.valid;
    ImageStore *motion = m_taau.motionReady && m_taau.motion && m_taau.motion->width == color->width &&
                                 m_taau.motion->height == color->height
                             ? m_taau.motion.get()
                             : nullptr;
    m_taau.motionReady = false;
    f.motion = motion != nullptr;
    // The reactive image covers the scene's size or half of it (then
    // bilinear) inside an image allocated for the scene's capacity.
    ImageStore *opaque =
        m_taau.reactiveReady && m_taau.opaque &&
                m_taau.opaqueUsed == TaauReactiveExtent(color->width, color->height, m_taau.opaqueHalf)
            ? m_taau.opaque.get()
            : nullptr;
    const bool opaqueHalf = opaque && m_taau.opaqueHalf;
    m_taau.opaqueReady = m_taau.reactiveReady = false;
    f.opaque = opaque != nullptr;
    if (opaque)
        TaauReactiveUv(m_taau.opaqueUsed, {opaque->width, opaque->height}, f.reactiveUv);

    // GPU time: read this ring slot's previous pair once its list is done.
    const uint32_t slot = (uint32_t)(m_taau.calls % TaauState::kSlots), tsOffset = slot * 32;
    if (m_taau.slotSeq[slot] && m_taau.slotSeq[slot] <= CompletedSeq())
    {
        uint64_t start, end;
        std::memcpy(&start, m_taau.timestamps.cpu + tsOffset + 8, 8);
        std::memcpy(&end, m_taau.timestamps.cpu + tsOffset + 16 + 8, 8);
        if (end > start)
        {
            m_taau.gpuNs += dkTimestampToNs(end - start);
            ++m_taau.samples;
        }
    }
    m_taau.slotSeq[slot] = m_openSeq;
    if (!(++m_taau.calls % 60))
    {
        Log("taau frames=60 gpu=%.3fms (per frame) %dx%d -> %dx%d history=%s current=%s reactive=%s resets=%llu "
            "samples=%llu motion_draws=%llu motion_skips=%llu motion_skips_total=%llu motion_allocs=%llu "
            "reactive_allocs=%llu",
            m_taau.samples ? m_taau.gpuNs / (m_taau.samples * 1e6) : 0.0, sr[2], sr[3], dr[2], dr[3],
            f.bilinearHistory ? "bilinear" : "catmull", f.bilinearCurrent ? "bilinear" : "kernel",
            !opaque ? "off" : opaqueHalf ? "half" : "full", (unsigned long long)m_taau.resets,
            (unsigned long long)m_taau.samples, (unsigned long long)m_taau.motionDraws,
            (unsigned long long)m_taau.motionSkips, (unsigned long long)m_taau.motionSkipsTotal,
            (unsigned long long)m_taau.motionAllocs, (unsigned long long)m_taau.opaqueAllocs);
        m_taau.gpuNs = m_taau.samples = m_taau.resets = m_taau.motionDraws = m_taau.motionSkips = 0;
    }
    dkCmdBufReportCounter(Rec(), DkCounter_Timestamp, m_taau.timestamps.gpu + tsOffset);

    HazardBegin();
    HazardAdd(color, Access::Sample);
    HazardAdd(depth, Access::Sample);
    HazardAdd(prev, Access::Sample);
    if (motion)
        HazardAdd(motion, Access::Sample);
    if (opaque)
        HazardAdd(opaque, Access::Sample);
    HazardAdd(next, Access::Render);
    HazardAdd(dst, Access::Render);
    HazardCommit();
    FlushDescriptors();
    DkImageView dstView, histView;
    dstSurface->MakeView(&dstView);
    dkImageViewDefaults(&histView, &next->image);
    const DkImageView *both[2] = {&dstView, &histView};
    CmdBindTargets(both, 2, nullptr);
    BindFullScreenState(Rec(), (uint32_t)dr[2], (uint32_t)dr[3], (uint32_t)dr[0], (uint32_t)dr[1]);
    const DkShader *shaders[2] = {&m_fsr.vs.shader, &program->shader};
    dkCmdBufBindShaders(Rec(), DkStageFlag_GraphicsMask, shaders, 2);
    SamplerKey pointKey = LinearClampKey();
    pointKey.state[D3DSAMP_MAGFILTER] = pointKey.state[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    const uint32_t point = SamplerDescriptor(pointKey);
    // Without motion or the snapshot the program never reads binding 3 or
    // 4; the colour fills them.
    const DkResHandle handles[5] = {dkMakeTextureHandle(color->descriptor, point),
                                    dkMakeTextureHandle(depth->descriptor, point),
                                    dkMakeTextureHandle(prev->descriptor, m_fsr.sampler),
                                    dkMakeTextureHandle((motion ? motion : color)->descriptor, point),
                                    dkMakeTextureHandle((opaque ? opaque : color)->descriptor,
                                                        opaqueHalf ? m_fsr.sampler : point)};
    dkCmdBufBindTextures(Rec(), DkStage_Fragment, 0, handles, 5);
    TaauConstants constants;
    TaauSetup(&constants, sr, dr, color->width, color->height, dst->width, dst->height, f);
    dkCmdBufPushConstants(Rec(), m_taau.constants.gpu, 256, 0, sizeof(constants), &constants);
    const DkBufExtents ubo{m_taau.constants.gpu, 256};
    dkCmdBufBindUniformBuffers(Rec(), DkStage_Fragment, 1, &ubo, 1);
    dkCmdBufDraw(Rec(), DkPrimitive_Triangles, 3, 1, 0, 0);
    dkCmdBufReportCounter(Rec(), DkCounter_Timestamp, m_taau.timestamps.gpu + tsOffset + 16);
    m_taau.next ^= 1;
    m_taau.valid = true;
    EndNativePass();
    return true;
}

} // namespace deko9

bool Deko9_TaauMotion(IDirect3DDevice9 *device, IDirect3DSurface9 *depth, const int32_t srcRect[4],
                      const deko9::TaauMotionView *view, const deko9::TaauMotionDraw *draws, uint32_t count)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    std::string error;
    if (!view || !srcRect || (count && !draws) ||
        !d->TaauMotion(static_cast<deko9::Surface *>(depth), srcRect, *view, draws, count, &error))
    {
        deko9::Fail("TAAU_MOTION", "%s", error.empty() ? "no view, rectangle or draws" : error.c_str());
        return false;
    }
    return true;
}

bool Deko9_TaauOpaque(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *color, const int32_t srcRect[4], bool after,
                      bool half)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    deko9::ImageStore *store =
        color && color->GetType() == D3DRTYPE_TEXTURE ? static_cast<deko9::Texture2D *>(color)->Store().get() : nullptr;
    std::string error;
    if (!srcRect || !d->TaauOpaque(store, srcRect, after, half, &error))
    {
        deko9::Fail("TAAU_OPAQUE", "%s", error.empty() ? "no rectangle" : error.c_str());
        return false;
    }
    return true;
}

bool Deko9_TaauResolve(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *color, IDirect3DSurface9 *depth,
                       const int32_t srcRect[4], IDirect3DSurface9 *dst, const int32_t dstRect[4],
                       const deko9::TaauFrame *frame)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    deko9::ImageStore *store =
        color && color->GetType() == D3DRTYPE_TEXTURE ? static_cast<deko9::Texture2D *>(color)->Store().get() : nullptr;
    std::string error;
    if (!frame || !srcRect || !dstRect ||
        !d->TaauResolve(store, static_cast<deko9::Surface *>(depth), srcRect, static_cast<deko9::Surface *>(dst),
                        dstRect, *frame, &error))
    {
        deko9::Fail("TAAU", "%s", error.empty() ? "no frame or rectangles" : error.c_str());
        return false;
    }
    return true;
}
