// Textures, surfaces, buffers, declarations, shaders and queries for the
// optional deko3d renderer.

#include <switch.h> // armGetSystemTick/armTicksToNs for CensusScope (deko9_internal.h)

#include "deko9_internal.h"
#include "deko9_native.h"
#include "deko9_shaderpack.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <sys/stat.h>

namespace deko9
{

// ---- image stores ---------------------------------------------------------------

uint32_t ImageStore::RowPitch(uint32_t level) const
{
    const uint32_t bw = format->blockWidth;
    return (LevelWidth(level) + bw - 1) / bw * format->blockBytes;
}

uint32_t ImageStore::RowCount(uint32_t level) const
{
    const uint32_t bw = format->blockWidth;
    return (LevelHeight(level) + bw - 1) / bw;
}

namespace
{
// True when two half-open 1D ranges overlap or exactly touch (no gap), so
// their union is itself a contiguous range.
bool Touch1D(uint32_t aLo, uint32_t aHi, uint32_t bLo, uint32_t bHi)
{
    return aLo <= bHi && bLo <= aHi;
}

// If `b` is axis-aligned with `a` (matches exactly on two of the three axes)
// and touches or overlaps it on the third, extends `a` to their union and
// returns true. A D3D9 dirty box union is only itself an exact box in this
// case; other overlaps are handled by keeping both boxes in the list (still
// correct -- D3D9 allows marking a superset of what changed, never a
// subset -- just less tightly coalesced).
bool TryMergeBox(D3DBOX &a, const D3DBOX &b)
{
    if (a.Top == b.Top && a.Bottom == b.Bottom && a.Front == b.Front && a.Back == b.Back &&
        Touch1D(a.Left, a.Right, b.Left, b.Right))
    {
        a.Left = std::min(a.Left, b.Left);
        a.Right = std::max(a.Right, b.Right);
        return true;
    }
    if (a.Left == b.Left && a.Right == b.Right && a.Front == b.Front && a.Back == b.Back &&
        Touch1D(a.Top, a.Bottom, b.Top, b.Bottom))
    {
        a.Top = std::min(a.Top, b.Top);
        a.Bottom = std::max(a.Bottom, b.Bottom);
        return true;
    }
    if (a.Left == b.Left && a.Right == b.Right && a.Top == b.Top && a.Bottom == b.Bottom &&
        Touch1D(a.Front, a.Back, b.Front, b.Back))
    {
        a.Front = std::min(a.Front, b.Front);
        a.Back = std::max(a.Back, b.Back);
        return true;
    }
    return false;
}
} // namespace

void ImageStore::MarkDirty(const D3DBOX *box)
{
    if (dirtyAll)
        return;
    if (!box)
    {
        MarkAllDirty();
        return;
    }
    if (box->Left >= box->Right || box->Top >= box->Bottom || box->Front >= box->Back)
        return; // empty box: nothing to mark (matches a degenerate lock box)
    D3DBOX merged = *box;
    // Repeatedly fold any existing box that aligns with the growing region;
    // a merge can bring `merged` into alignment with another entry, so keep
    // scanning from the start until nothing folds in.
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (size_t i = 0; i < dirtyBoxes.size(); ++i)
        {
            if (TryMergeBox(merged, dirtyBoxes[i]))
            {
                dirtyBoxes.erase(dirtyBoxes.begin() + (long)i);
                changed = true;
                break;
            }
        }
    }
    if (dirtyBoxes.size() >= kMaxDirtyBoxes)
    {
        // Cap: collapse the whole list plus the new box into one bounding
        // box instead of growing unbounded or giving up and copying
        // everything.
        D3DBOX bound = merged;
        for (const D3DBOX &e : dirtyBoxes)
        {
            bound.Left = std::min(bound.Left, e.Left);
            bound.Top = std::min(bound.Top, e.Top);
            bound.Front = std::min(bound.Front, e.Front);
            bound.Right = std::max(bound.Right, e.Right);
            bound.Bottom = std::max(bound.Bottom, e.Bottom);
            bound.Back = std::max(bound.Back, e.Back);
        }
        dirtyBoxes.clear();
        dirtyBoxes.push_back(bound);
        return;
    }
    dirtyBoxes.push_back(merged);
}

namespace
{
uint32_t FullChain(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t levels = 1, size = std::max(w, std::max(h, d));
    while (size > 1)
        size >>= 1, ++levels;
    return levels;
}

void AllocCpu(ImageStore *store)
{
    store->cpuLevelOffset.resize(store->faces * store->levels);
    uint32_t offset = 0;
    for (uint32_t face = 0; face < store->faces; ++face)
    {
        for (uint32_t level = 0; level < store->levels; ++level)
        {
            store->cpuLevelOffset[store->SubIndex(face, level)] = offset;
            offset += AlignUp(store->LevelBytes(level), 16);
        }
    }
    store->cpu.assign(offset, 0);
}
// r_deko9RtCompression (Deko9_SetRtCompression): render/depth targets get
// DkImageFlags_HwCompression, i.e. a compressible memory kind (C32_2CRA,
// C64_2CRA, Z24S8_2CZ, ...). Maxwell compression lives in the L2/memory
// controller keyed by the page kind, so every GPU engine (3D draws and
// clears, texture sampling, the 2D engine used by StretchRect and Present,
// the copy engine used by ReadImage) sees plain pixels; only the CPU and the
// display engine would see the compressed bytes. deko9 never maps a render
// target for the CPU (LockStore refuses) and presents by a 2D blit into the
// uncompressed swapchain images, so no explicit decompression is needed.
// Zcull is set up by deko3d itself (dkCmdBufBindRenderTargets, queue created
// with DkQueueFlags_EnableZcull, the default).
std::atomic<bool> g_rtCompression{true};
// Deko9_SetRtCompressionOverride: per-thread choice for the images this
// thread creates next (-1: follow g_rtCompression).
thread_local int t_rtCompressionOverride = -1;
} // namespace

std::shared_ptr<ImageStore> Device::MakeStore()
{
    return std::shared_ptr<ImageStore>(new ImageStore(), [this](ImageStore *store) {
        DestroyStoreAfter(store);
        delete store;
    });
}

bool Device::CreateStore(ImageStore *store, std::string *error)
{
    if (!store->levels)
        store->levels = FullChain(store->width, store->height, store->depth);
    store->faces = store->type == D3DRTYPE_CUBETEXTURE ? 6 : 1;
    // S4a: a render/depth target at creation is never "static"; see the
    // ImageStore::attachment comment. Latched here, and again if a plain
    // store is later bound as a target (SetRenderTarget/SetDepthStencilSurface)
    // or a StretchRect blit destination.
    if (store->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
        store->attachment = true;
    if (store->pool == D3DPOOL_SYSTEMMEM || store->pool == D3DPOOL_SCRATCH)
    {
        store->gpu = false;
        AllocCpu(store);
        store->MarkAllDirty(); // D3D9: a new SYSTEMMEM texture is fully dirty
        return true;
    }
    const FormatInfo &format = *store->format;
    if ((store->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && !format.renderable)
        return *error = "format is not renderable", false;
    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, m_dk);
    // 2D-engine blits (StretchRect, present) need Usage2DEngine; deko3d
    // rejects it for 3D images, which only take copy-engine uploads.
    maker.flags = store->type == D3DRTYPE_VOLUMETEXTURE ? 0 : DkImageFlags_Usage2DEngine;
    if (store->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
    {
        maker.flags |= DkImageFlags_UsageRender;
        const bool compress = t_rtCompressionOverride >= 0 ? t_rtCompressionOverride != 0
                                                           : g_rtCompression.load(std::memory_order_relaxed);
        if (compress)
        {
            maker.flags |= DkImageFlags_HwCompression;
            store->compressed = true;
            static bool s_logged;
            if (!s_logged)
            {
                s_logged = true;
                Log("rt compression on (first target %ux%u d3dfmt=%u)", store->width, store->height,
                    (unsigned)format.d3d);
            }
        }
    }
    maker.format = format.dk;
    maker.dimensions[0] = store->width;
    maker.dimensions[1] = store->height;
    maker.mipLevels = store->levels;
    switch (store->type)
    {
    case D3DRTYPE_CUBETEXTURE:
        maker.type = DkImageType_Cubemap;
        maker.dimensions[2] = 6;
        break;
    case D3DRTYPE_VOLUMETEXTURE:
        maker.type = DkImageType_3D;
        maker.dimensions[2] = store->depth;
        break;
    default:
        maker.type = DkImageType_2D;
        break;
    }
    dkImageLayoutInitialize(&store->layout, &maker);
    const uint64_t size = dkImageLayoutGetSize(&store->layout);
    if (size > 0xffffffffull || !AllocMemory(POOL_IMAGE, (uint32_t)size, dkImageLayoutGetAlignment(&store->layout),
                                            &store->memory))
        return *error = "image memory allocation failed", false;
    dkImageInitialize(&store->image, &store->layout, store->memory.block, store->memory.offset);
    store->gpu = true;
    store->uploaded.assign(store->faces * store->levels, false);
    // Standalone RT/DS surfaces are never sampled in D3D9; textures are.
    if (store->type != D3DRTYPE_SURFACE)
    {
        DkImageView view;
        dkImageViewDefaults(&view, &store->image);
        std::copy(format.swizzle, format.swizzle + 4, view.swizzle);
        if (format.depth)
            view.dsSource = DkDsSource_Depth;
        store->descriptor = AllocImageDescriptor(view);
        if (store->descriptor == UINT32_MAX)
            return *error = "image descriptor allocation failed", false;
    }
    if (store->usage & D3DUSAGE_DYNAMIC)
        AllocCpu(store); // persistent shadow: D3D9 locks preserve contents
    GpuMapImage(store, "image");
    return true;
}

// Dynamic resolution (r_dynres): a render target allocated for the largest
// size is re-laid out for a smaller (or larger, up to that size) one. deko3d
// derives the block-linear layout (GOB rows, block height, compression kind)
// from the dimensions, so the image is re-initialised at the new size: every
// render, blit, clear and sampled view then addresses exactly width x height
// texels, UVs 0..1 span the new size and clamp-to-edge stops at its
// right/bottom borders. The old contents are undefined afterwards; the engine
// resizes only between frames, before the targets are rewritten.
//
// Explicit lifetime: the new layout gets FRESH memory of
// the same capacity and the old memory and sampling descriptor are released
// only after the open list completes. Re-initialising in place was a mapping
// change under frames in flight: dkImageInitialize of a compressed/depth
// kind calls nvAddressSpaceModify on the memblock's compressed alias (deko3d
// MemBlock::getGpuAddrForImage), rewriting the page kind of memory that
// earlier submitted lists still render into and sample. Recorded commands
// keep their own copies of the old view (bind/blit parameters are in the
// command list), so frames in flight keep addressing the old memory.
bool Device::ResizeStore(ImageStore *store, uint32_t width, uint32_t height, std::string *error)
{
    if (!store || !store->gpu || !(store->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) ||
        store->levels != 1 || store->faces != 1 || store->depth != 1 ||
        (store->type != D3DRTYPE_TEXTURE && store->type != D3DRTYPE_SURFACE))
        return *error = "resize: needs a single-level 2D render target or depth-stencil on the GPU", false;
    if (!width || !height)
        return *error = "resize: zero size", false;
    if (width == store->width && height == store->height)
        return true;
    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, m_dk);
    maker.flags = DkImageFlags_Usage2DEngine | DkImageFlags_UsageRender;
    if (store->compressed)
        maker.flags |= DkImageFlags_HwCompression;
    maker.format = store->format->dk;
    maker.dimensions[0] = width;
    maker.dimensions[1] = height;
    maker.mipLevels = 1;
    maker.type = DkImageType_2D;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &maker);
    const uint64_t size = dkImageLayoutGetSize(&layout);
    const uint32_t align = dkImageLayoutGetAlignment(&layout);
    if (size > store->memory.size)
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "resize: %ux%u needs %llu bytes (align %u), allocation holds %u at %u",
                      width, height, (unsigned long long)size, align, store->memory.size, store->memory.offset);
        return *error = detail, false;
    }
    // Same capacity as before (the largest size stays reachable), aligned
    // for any layout of it: the alignment of the capacity's own layout at
    // creation is at most a big page.
    GpuAlloc fresh;
    if (!AllocMemory(POOL_IMAGE, store->memory.size, std::max<uint32_t>(align, 0x10000u), &fresh))
        return *error = "resize: image memory allocation failed", false;
    FreeMemoryAfter(store->memory, m_openSeq);
    store->memory = fresh;
    store->layout = layout;
    dkImageInitialize(&store->image, &store->layout, store->memory.block, store->memory.offset);
    store->width = width;
    store->height = height;
    store->uploaded.assign(1, false);
    if (store->descriptor != UINT32_MAX)
    {
        FreeImageDescriptorAfter(store->descriptor, m_openSeq);
        DkImageView view;
        dkImageViewDefaults(&view, &store->image);
        std::copy(store->format->swizzle, store->format->swizzle + 4, view.swizzle);
        if (store->format->depth)
            view.dsSource = DkDsSource_Depth;
        store->descriptor = AllocImageDescriptor(view);
        if (store->descriptor == UINT32_MAX)
            return *error = "resize: image descriptor allocation failed", false;
    }
    GpuMapImage(store, "resize");
    // Re-derive everything recorded from this image: target binds (size,
    // zcull region), viewport/scissor clamps, texture handles.
    m_dirtyTargets = m_dirtyViewport = m_dirtyTextures = true;
    m_texSlotDirty[0] = m_texSlotDirty[1] = ~0u;
    m_drawHazardValid = false;
    ++m_resizes;
    return true;
}

// Swaps images, not memory kinds: both sides keep a valid dkImageInitialize
// (same format and compression), recorded lists keep the views they bound,
// and hazard epochs travel with the memory they describe. Sampled stores get
// fresh descriptors because a slot a list may still reference is never
// rewritten.
bool Device::MoveStoreContents(ImageStore *src, ImageStore *dst, std::string *error)
{
    const auto ok = [](const ImageStore *s) {
        return s && s->gpu && (s->usage & D3DUSAGE_RENDERTARGET) && !s->format->depth && s->levels == 1 &&
               s->faces == 1 && s->depth == 1 && (s->type == D3DRTYPE_TEXTURE || s->type == D3DRTYPE_SURFACE);
    };
    if (!ok(src) || !ok(dst) || src == dst)
        return *error = "move: needs two distinct single-level 2D colour render targets on the GPU", false;
    if (src->format != dst->format || src->width != dst->width || src->height != dst->height ||
        src->compressed != dst->compressed)
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "move: %ux%u fmt=%u cmp=%d -> %ux%u fmt=%u cmp=%d differ", src->width,
                      src->height, (unsigned)src->format->d3d, (int)src->compressed, dst->width, dst->height,
                      (unsigned)dst->format->d3d, (int)dst->compressed);
        return *error = detail, false;
    }
    if (src->contentsUndefined)
        return *error = "move: source contents are undefined (moved away and not rewritten)", false;
    std::swap(src->layout, dst->layout);
    std::swap(src->image, dst->image);
    std::swap(src->memory, dst->memory);
    std::swap(src->renderEpoch, dst->renderEpoch);
    std::swap(src->copyWriteEpoch, dst->copyWriteEpoch);
    std::swap(src->blitEpoch, dst->blitEpoch);
    std::swap(src->readEpoch, dst->readEpoch);
    std::swap(src->copyReadEpoch, dst->copyReadEpoch);
    std::swap(src->pendingRaw, dst->pendingRaw);
    std::swap(src->uploaded, dst->uploaded);
    for (ImageStore *s : {src, dst})
    {
        if (s->descriptor == UINT32_MAX)
            continue;
        FreeImageDescriptorAfter(s->descriptor, m_openSeq);
        DkImageView view;
        dkImageViewDefaults(&view, &s->image);
        std::copy(s->format->swizzle, s->format->swizzle + 4, view.swizzle);
        s->descriptor = AllocImageDescriptor(view);
        if (s->descriptor == UINT32_MAX)
            return *error = "move: image descriptor allocation failed", false;
    }
    // The present upscaler samples the back buffer through its own view.
    const ImageStore *back = m_backBuffer ? m_backBuffer->Store().get() : nullptr;
    if (m_fsr.active && (src == back || dst == back))
    {
        FreeImageDescriptorAfter(m_fsr.backDescriptor, m_openSeq);
        DkImageView view;
        m_backBuffer->MakeView(&view);
        std::copy(back->format->swizzle, back->format->swizzle + 4, view.swizzle);
        m_fsr.backDescriptor = AllocImageDescriptor(view);
        if (m_fsr.backDescriptor == UINT32_MAX)
            return *error = "move: back buffer descriptor allocation failed", false;
    }
    if (dst->contentsUndefined)
    {
        dst->contentsUndefined = false;
        --m_undefinedTargets;
    }
    src->contentsUndefined = true;
    ++m_undefinedTargets;
    // The same two allocations trade owners every frame; mapping the first
    // moves is enough.
    if (m_moves < 16)
    {
        GpuMapImage(src, "move");
        GpuMapImage(dst, "move");
    }
    // Re-derive everything recorded from these images, as ResizeStore does.
    m_dirtyTargets = m_dirtyViewport = m_dirtyTextures = true;
    m_texSlotDirty[0] = m_texSlotDirty[1] = ~0u;
    m_drawHazardValid = false;
    ++m_moves;
    return true;
}

void Device::DestroyStoreAfter(ImageStore *store)
{
    DeviceLockGuard lock(m_lock);
    if (!store->gpu)
        return;
    GpuMapImage(store, "free");
    if (store->contentsUndefined)
    {
        store->contentsUndefined = false;
        --m_undefinedTargets;
    }
    FreeImageDescriptorAfter(store->descriptor, m_openSeq);
    FreeMemoryAfter(store->memory, m_openSeq);
    store->gpu = false;
}

// ---- locking ----------------------------------------------------------------------

// r_deko9Census: LockRect/UnlockRect grouped per D3D9 texture type (a
// standalone Surface -- render target, offscreen plain surface -- reports
// separately from the three texture types).
CensusId CensusIdForLock(D3DRESOURCETYPE type)
{
    switch (type)
    {
    case D3DRTYPE_TEXTURE: return Census_LockTexture2D;
    case D3DRTYPE_CUBETEXTURE: return Census_LockCubeTexture;
    case D3DRTYPE_VOLUMETEXTURE: return Census_LockVolumeTexture;
    default: return Census_LockSurface;
    }
}

HRESULT LockStore(Device *device, ImageStore *store, uint32_t face, uint32_t level, const D3DBOX *box,
                  DWORD flags, LockState *state, void **bits, INT *rowPitch, INT *slicePitch)
{
    CensusScope census(device, CensusIdForLock(store->type));
    DeviceLockGuard lock(device->Lock());
    if (state->locked || level >= store->levels || face >= store->faces || !bits)
        return D3DERR_INVALIDCALL;
    const uint32_t w = store->LevelWidth(level), h = store->LevelHeight(level), d = store->LevelDepth(level);
    D3DBOX b = box ? *box : D3DBOX{0, 0, w, h, 0, d};
    const uint32_t bw = store->format->blockWidth;
    if (b.Left >= b.Right || b.Top >= b.Bottom || b.Front >= b.Back || b.Right > w || b.Bottom > h || b.Back > d ||
        b.Left % bw || b.Top % bw)
        return D3DERR_INVALIDCALL;
    const uint32_t sub = store->SubIndex(face, level);
    if (store->gpu && (store->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
        return Fail("LOCK", "render target / depth-stencil lock unsupported");

    // D3D9 MANAGED resources keep a system-memory copy, so a relock never
    // reads the GPU per lock. Build that copy on the first lock that must
    // preserve already-uploaded contents (one readback); later locks use the
    // dynamic-shadow path. A first lock of a never-uploaded subresource (every
    // texture load locks each mip once, flags 0) takes the staging path
    // instead: giving every loaded texture a permanent full-chain CPU copy
    // doubled texture RAM and added a memcpy per mip.
    if (store->gpu && store->cpu.empty() && store->pool == D3DPOOL_MANAGED && !(flags & D3DLOCK_DISCARD) &&
        store->uploaded[sub])
    {
        AllocCpu(store);
        for (uint32_t f = 0; f < store->faces; ++f)
        {
            for (uint32_t l = 0; l < store->levels; ++l)
            {
                const uint32_t s = store->SubIndex(f, l);
                if (!store->uploaded[s])
                    continue;
                if (!device->ReadImage(store, f, l, store->cpu.data() + store->cpuLevelOffset[s]))
                {
                    store->cpu.clear();
                    return D3DERR_DRIVERINTERNALERROR;
                }
                census.AddBytes(store->LevelBytes(l)); // MANAGED shadow-copy readback
            }
        }
    }

    uint8_t *base;
    uint32_t pitch, slice;
    if (!store->cpu.empty())
    {
        // SYSTEMMEM/SCRATCH storage, or the DYNAMIC shadow copy.
        pitch = store->RowPitch(level);
        slice = store->SlicePitch(level);
        base = store->cpu.data() + store->cpuLevelOffset[sub] + b.Front * slice + (b.Top / bw) * pitch +
               (b.Left / bw) * store->format->blockBytes;
        if (!store->gpu && !(flags & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE)))
        {
            // AddDirtyBox/AddDirtyRect boxes are always level-0 coordinates
            // in D3D9; a lock's implicit dirty region is too, so only a
            // level-0 lock can be tracked precisely -- a lock of a lower mip
            // conservatively dirties the whole texture (still correct, just
            // not narrowed, and no worse than the previous always-whole-
            // texture behavior).
            if (level == 0)
                store->MarkDirty(&b);
            else
                store->MarkAllDirty();
        }
    }
    else
    {
        // Managed/default GPU texture: lock through a staging copy of the
        // box, uploaded at unlock. A re-lock without DISCARD must see the
        // current contents, so read the level back first (rare; stalls).
        pitch = ((b.Right - b.Left + bw - 1) / bw) * store->format->blockBytes;
        const uint32_t rows = (b.Bottom - b.Top + bw - 1) / bw;
        slice = pitch * rows;
        if (!device->AllocMemory(POOL_BUFFER, slice * (b.Back - b.Front) + kCanaryBytes, 256, &state->staging))
            return D3DERR_OUTOFVIDEOMEMORY;
        base = state->staging.cpu;
        FillCanary(base + slice * (b.Back - b.Front));
        if (!(flags & D3DLOCK_DISCARD) && store->uploaded[sub])
        {
            std::vector<uint8_t> level_(store->LevelBytes(level));
            if (!device->ReadImage(store, face, level, level_.data()))
            {
                device->FreeMemoryAfter(state->staging, device->OpenSeq());
                state->staging = {};
                return D3DERR_DRIVERINTERNALERROR;
            }
            const uint32_t fullPitch = store->RowPitch(level), fullSlice = store->SlicePitch(level);
            for (uint32_t z = 0; z < b.Back - b.Front; ++z)
            {
                for (uint32_t row = 0; row < rows; ++row)
                {
                    std::memcpy(base + z * slice + row * pitch,
                                level_.data() + (b.Front + z) * fullSlice + (b.Top / bw + row) * fullPitch +
                                    (b.Left / bw) * store->format->blockBytes,
                                pitch);
                }
            }
            census.AddBytes((uint64_t)pitch * rows * (b.Back - b.Front)); // staging re-lock copy
        }
    }
    state->locked = true;
    state->flags = flags;
    state->box = b;
    *bits = base;
    if (rowPitch)
        *rowPitch = (INT)pitch;
    if (slicePitch)
        *slicePitch = (INT)slice;
    return D3D_OK;
}

HRESULT UnlockStore(Device *device, ImageStore *store, uint32_t face, uint32_t level, LockState *state)
{
    CensusScope census(device, CensusIdForLock(store->type));
    DeviceLockGuard lock(device->Lock());
    DeviceLockSite site(device->Lock(), "upload");
    device->SplitLongList();
    if (!state->locked)
        return D3DERR_INVALIDCALL;
    state->locked = false;
    if (!store->gpu)
        return D3D_OK;
    const D3DBOX &b = state->box;
    const DkImageRect rect{b.Left, b.Top, b.Front, b.Right - b.Left, b.Bottom - b.Top, b.Back - b.Front};
    if (state->staging)
    {
        const uint32_t bwc = store->format->blockWidth;
        const uint32_t stagingBytes = ((b.Right - b.Left + bwc - 1) / bwc) * store->format->blockBytes *
                                      ((b.Bottom - b.Top + bwc - 1) / bwc) * (b.Back - b.Front);
#ifdef DEKO9_CANARY_CHECKS
        if (!CheckCanary(state->staging.cpu + stagingBytes))
            Fail("OVERRUN", "write past a %ux%u fmt=%u level %u lock (%u bytes)", b.Right - b.Left,
                 b.Bottom - b.Top, (unsigned)store->format->d3d, level, stagingBytes);
#else
        (void)stagingBytes;
#endif
        if (!(state->flags & D3DLOCK_READONLY))
        {
            device->CopyBufferToImage(store, face, level, state->staging.gpu, rect);
            census.AddBytes(stagingBytes);
        }
        device->FreeMemoryAfter(state->staging, device->OpenSeq());
        state->staging = {};
        return D3D_OK;
    }
    if (state->flags & D3DLOCK_READONLY)
        return D3D_OK;
    // Dynamic shadow: upload the locked box from it.
    const uint32_t bw = store->format->blockWidth;
    const uint32_t pitch = ((b.Right - b.Left + bw - 1) / bw) * store->format->blockBytes;
    const uint32_t rows = (b.Bottom - b.Top + bw - 1) / bw;
    const uint32_t fullPitch = store->RowPitch(level), fullSlice = store->SlicePitch(level);
    GpuAlloc upload;
    if (!device->AllocUpload(pitch * rows * (b.Back - b.Front), 256, &upload))
        return D3DERR_OUTOFVIDEOMEMORY;
    census.AddBytes((uint64_t)pitch * rows * (b.Back - b.Front));
    const uint8_t *src = store->cpu.data() + store->cpuLevelOffset[store->SubIndex(face, level)];
    for (uint32_t z = 0; z < b.Back - b.Front; ++z)
    {
        for (uint32_t row = 0; row < rows; ++row)
        {
            std::memcpy(upload.cpu + (z * rows + row) * pitch,
                        src + (b.Front + z) * fullSlice + (b.Top / bw + row) * fullPitch +
                            (b.Left / bw) * store->format->blockBytes,
                        pitch);
        }
    }
    device->CopyBufferToImage(store, face, level, upload.gpu, rect);
    return D3D_OK;
}

// ---- surfaces ---------------------------------------------------------------------

Surface::Surface(Device *device, std::shared_ptr<ImageStore> store, uint32_t face, uint32_t level,
                 IUnknown *container)
    : Object(device), m_store(std::move(store)), m_face(face), m_level(level), m_container(container)
{
}

ULONG Surface::AddRef()
{
    // A texture's surfaces share the texture's reference count.
    return m_container ? m_container->AddRef() : Object::AddRef();
}

ULONG Surface::Release()
{
    return m_container ? m_container->Release() : Object::Release();
}

HRESULT Surface::GetDevice(IDirect3DDevice9 **device)
{
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
}

HRESULT Surface::GetContainer(REFIID riid, void **container)
{
    if (!container)
        return D3DERR_INVALIDCALL;
    if (m_container)
        return m_container->QueryInterface(riid, container);
    return GetDevice(reinterpret_cast<IDirect3DDevice9 **>(container));
}

HRESULT Surface::GetDesc(D3DSURFACE_DESC *desc)
{
    if (!desc)
        return D3DERR_INVALIDCALL;
    desc->Format = m_store->format->d3d;
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = m_store->usage;
    desc->Pool = m_store->pool;
    desc->MultiSampleType = D3DMULTISAMPLE_NONE;
    desc->MultiSampleQuality = 0;
    desc->Width = m_store->LevelWidth(m_level);
    desc->Height = m_store->LevelHeight(m_level);
    return D3D_OK;
}

HRESULT Surface::LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
{
    if (!locked)
        return D3DERR_INVALIDCALL;
    D3DBOX box, *pbox = nullptr;
    if (rect)
    {
        box = {(UINT)rect->left, (UINT)rect->top, (UINT)rect->right, (UINT)rect->bottom, 0, 1};
        pbox = &box;
    }
    return LockStore(m_device, m_store.get(), m_face, m_level, pbox, flags, &m_lock, &locked->pBits, &locked->Pitch,
                     nullptr);
}

HRESULT Surface::UnlockRect()
{
    return UnlockStore(m_device, m_store.get(), m_face, m_level, &m_lock);
}

void Surface::MakeView(DkImageView *view) const
{
    dkImageViewDefaults(view, &m_store->image);
    view->mipLevelOffset = (uint8_t)m_level;
    view->mipLevelCount = 1;
    if (m_store->faces > 1)
    {
        view->type = DkImageType_2D;
        view->layerOffset = (uint16_t)m_face;
        view->layerCount = 1;
    }
}

// ---- textures ---------------------------------------------------------------------

Texture2D::Texture2D(Device *device, std::shared_ptr<ImageStore> store) : Object(device), m_store(std::move(store))
{
    for (uint32_t level = 0; level < m_store->levels; ++level)
        m_levels.push_back(new Surface(device, m_store, 0, level, this));
}

Texture2D::~Texture2D()
{
    m_device->ForgetTexture(this);
    for (Surface *surface : m_levels)
        delete surface;
}

HRESULT Texture2D::GetDevice(IDirect3DDevice9 **device)
{
    return m_levels[0]->GetDevice(device);
}

HRESULT Texture2D::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc)
{
    return level < m_levels.size() ? m_levels[level]->GetDesc(desc) : D3DERR_INVALIDCALL;
}

HRESULT Texture2D::GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface)
{
    if (level >= m_levels.size() || !surface)
        return D3DERR_INVALIDCALL;
    m_levels[level]->AddRef();
    *surface = m_levels[level];
    return D3D_OK;
}

HRESULT Texture2D::LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
{
    return level < m_levels.size() ? m_levels[level]->LockRect(locked, rect, flags) : D3DERR_INVALIDCALL;
}

HRESULT Texture2D::UnlockRect(UINT level)
{
    return level < m_levels.size() ? m_levels[level]->UnlockRect() : D3DERR_INVALIDCALL;
}

HRESULT Texture2D::AddDirtyRect(const RECT *rect)
{
    DeviceLockGuard lock(m_device->Lock());
    if (!rect)
    {
        m_store->MarkAllDirty();
        return D3D_OK;
    }
    const D3DBOX box{(UINT)rect->left, (UINT)rect->top, (UINT)rect->right, (UINT)rect->bottom, 0, 1};
    m_store->MarkDirty(&box);
    return D3D_OK;
}

void Texture2D::GenerateMipSubLevels()
{
    Fail("UNSUPPORTED", "GenerateMipSubLevels");
}

CubeTexture::CubeTexture(Device *device, std::shared_ptr<ImageStore> store) : Object(device), m_store(std::move(store))
{
    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t level = 0; level < m_store->levels; ++level)
            m_faces.push_back(new Surface(device, m_store, face, level, this));
    }
}

CubeTexture::~CubeTexture()
{
    m_device->ForgetTexture(this);
    for (Surface *surface : m_faces)
        delete surface;
}

HRESULT CubeTexture::GetDevice(IDirect3DDevice9 **device)
{
    return m_faces[0]->GetDevice(device);
}

HRESULT CubeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc)
{
    return level < m_store->levels ? m_faces[level]->GetDesc(desc) : D3DERR_INVALIDCALL;
}

HRESULT CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surface)
{
    if ((unsigned)face >= 6 || level >= m_store->levels || !surface)
        return D3DERR_INVALIDCALL;
    Surface *s = m_faces[m_store->SubIndex(face, level)];
    s->AddRef();
    *surface = s;
    return D3D_OK;
}

HRESULT CubeTexture::LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
{
    if ((unsigned)face >= 6 || level >= m_store->levels)
        return D3DERR_INVALIDCALL;
    return m_faces[m_store->SubIndex(face, level)]->LockRect(locked, rect, flags);
}

HRESULT CubeTexture::UnlockRect(D3DCUBEMAP_FACES face, UINT level)
{
    if ((unsigned)face >= 6 || level >= m_store->levels)
        return D3DERR_INVALIDCALL;
    return m_faces[m_store->SubIndex(face, level)]->UnlockRect();
}

VolumeTexture::VolumeTexture(Device *device, std::shared_ptr<ImageStore> store)
    : Object(device), m_store(std::move(store)), m_locks(m_store->levels)
{
}

VolumeTexture::~VolumeTexture()
{
    m_device->ForgetTexture(this);
}

HRESULT VolumeTexture::GetDevice(IDirect3DDevice9 **device)
{
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
}

HRESULT VolumeTexture::GetLevelDesc(UINT level, D3DVOLUME_DESC *desc)
{
    if (level >= m_store->levels || !desc)
        return D3DERR_INVALIDCALL;
    desc->Format = m_store->format->d3d;
    desc->Type = D3DRTYPE_VOLUME;
    desc->Usage = m_store->usage;
    desc->Pool = m_store->pool;
    desc->Width = m_store->LevelWidth(level);
    desc->Height = m_store->LevelHeight(level);
    desc->Depth = m_store->LevelDepth(level);
    return D3D_OK;
}

HRESULT VolumeTexture::LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags)
{
    if (level >= m_store->levels || !locked)
        return D3DERR_INVALIDCALL;
    return LockStore(m_device, m_store.get(), 0, level, box, flags, &m_locks[level], &locked->pBits,
                     &locked->RowPitch, &locked->SlicePitch);
}

HRESULT VolumeTexture::UnlockBox(UINT level)
{
    if (level >= m_store->levels)
        return D3DERR_INVALIDCALL;
    return UnlockStore(m_device, m_store.get(), 0, level, &m_locks[level]);
}

HRESULT VolumeTexture::AddDirtyBox(const D3DBOX *box)
{
    DeviceLockGuard lock(m_device->Lock());
    m_store->MarkDirty(box);
    return D3D_OK;
}

// ---- device resource creation --------------------------------------------------

namespace
{
HRESULT CheckPool(D3DPOOL pool, DWORD usage)
{
    if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED && pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH)
        return D3DERR_INVALIDCALL;
    if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC)) && pool == D3DPOOL_MANAGED)
        return D3DERR_INVALIDCALL;
    if (usage & D3DUSAGE_AUTOGENMIPMAP)
        return Fail("UNSUPPORTED", "D3DUSAGE_AUTOGENMIPMAP");
    return D3D_OK;
}
} // namespace

HRESULT Device::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                              IDirect3DTexture9 **texture, HANDLE *shared)
{
    DeviceLockGuard lock(m_lock);
    if (!texture || shared || !w || !h || FAILED(CheckPool(pool, usage)))
        return D3DERR_INVALIDCALL;
    *texture = nullptr;
    auto store = MakeStore();
    store->format = LookupFormat(format);
    if (!store->format)
        return Fail("CREATE_TEXTURE", "format %u unsupported", (unsigned)format);
    store->type = D3DRTYPE_TEXTURE;
    store->pool = pool;
    store->usage = usage;
    store->width = w;
    store->height = h;
    store->levels = levels;
    std::string error;
    if (!CreateStore(store.get(), &error))
        return Fail("CREATE_TEXTURE", "%ux%u fmt=%u: %s", w, h, (unsigned)format, error.c_str());
    *texture = new Texture2D(this, store);
    return D3D_OK;
}

HRESULT Device::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                    IDirect3DVolumeTexture9 **texture, HANDLE *shared)
{
    DeviceLockGuard lock(m_lock);
    if (!texture || shared || !w || !h || !d || FAILED(CheckPool(pool, usage)) ||
        (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
        return D3DERR_INVALIDCALL;
    *texture = nullptr;
    auto store = MakeStore();
    store->format = LookupFormat(format);
    if (!store->format || store->format->depth)
        return Fail("CREATE_VOLUME_TEXTURE", "format %u unsupported", (unsigned)format);
    store->type = D3DRTYPE_VOLUMETEXTURE;
    store->pool = pool;
    store->usage = usage;
    store->width = w;
    store->height = h;
    store->depth = d;
    store->levels = levels;
    std::string error;
    if (!CreateStore(store.get(), &error))
        return Fail("CREATE_VOLUME_TEXTURE", "%ux%ux%u fmt=%u: %s", w, h, d, (unsigned)format, error.c_str());
    *texture = new VolumeTexture(this, store);
    return D3D_OK;
}

HRESULT Device::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                  IDirect3DCubeTexture9 **texture, HANDLE *shared)
{
    DeviceLockGuard lock(m_lock);
    if (!texture || shared || !edge || FAILED(CheckPool(pool, usage)))
        return D3DERR_INVALIDCALL;
    *texture = nullptr;
    auto store = MakeStore();
    store->format = LookupFormat(format);
    if (!store->format)
        return Fail("CREATE_CUBE_TEXTURE", "format %u unsupported", (unsigned)format);
    store->type = D3DRTYPE_CUBETEXTURE;
    store->pool = pool;
    store->usage = usage;
    store->width = store->height = edge;
    store->levels = levels;
    std::string error;
    if (!CreateStore(store.get(), &error))
        return Fail("CREATE_CUBE_TEXTURE", "%u fmt=%u: %s", edge, (unsigned)format, error.c_str());
    *texture = new CubeTexture(this, store);
    return D3D_OK;
}

namespace
{
HRESULT CreateStandaloneSurface(Device *device, UINT w, UINT h, D3DFORMAT format, DWORD usage, D3DPOOL pool,
                                IDirect3DSurface9 **surface, const char *what)
{
    if (!surface || !w || !h)
        return D3DERR_INVALIDCALL;
    *surface = nullptr;
    auto store = device->MakeStore();
    store->format = LookupFormat(format);
    if (!store->format)
        return Fail(what, "format %u unsupported", (unsigned)format);
    if ((usage & D3DUSAGE_DEPTHSTENCIL) && !store->format->depth)
        return Fail(what, "format %u is not a depth format", (unsigned)format);
    store->type = D3DRTYPE_SURFACE;
    store->pool = pool;
    store->usage = usage;
    store->width = w;
    store->height = h;
    store->levels = 1;
    std::string error;
    if (!device->CreateStore(store.get(), &error))
        return Fail(what, "%ux%u fmt=%u: %s", w, h, (unsigned)format, error.c_str());
    *surface = new Surface(device, store, 0, 0, nullptr);
    return D3D_OK;
}
} // namespace

HRESULT Device::CreateRenderTarget(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, DWORD, BOOL,
                                   IDirect3DSurface9 **surface, HANDLE *shared)
{
    DeviceLockGuard lock(m_lock);
    if (shared)
        return D3DERR_INVALIDCALL;
    if (ms != D3DMULTISAMPLE_NONE)
        return Fail("CREATE_RENDER_TARGET", "multisample type %d unsupported", (int)ms);
    return CreateStandaloneSurface(this, w, h, format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, surface,
                                   "CREATE_RENDER_TARGET");
}

HRESULT Device::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, DWORD, BOOL,
                                          IDirect3DSurface9 **surface, HANDLE *shared)
{
    DeviceLockGuard lock(m_lock);
    if (shared)
        return D3DERR_INVALIDCALL;
    if (ms != D3DMULTISAMPLE_NONE)
        return Fail("CREATE_DEPTH_STENCIL", "multisample type %d unsupported", (int)ms);
    return CreateStandaloneSurface(this, w, h, format, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, surface,
                                   "CREATE_DEPTH_STENCIL");
}

HRESULT Device::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT format, D3DPOOL pool,
                                            IDirect3DSurface9 **surface, HANDLE *shared)
{
    DeviceLockGuard lock(m_lock);
    if (shared || pool == D3DPOOL_MANAGED)
        return D3DERR_INVALIDCALL;
    return CreateStandaloneSurface(this, w, h, format, 0, pool, surface, "CREATE_OFFSCREEN_SURFACE");
}

// ---- buffers ----------------------------------------------------------------------

bool Buffer::Init(Device *device, uint32_t size, DWORD usage, D3DPOOL pool)
{
    m_size = size;
    m_usage = usage;
    m_pool = pool;
    // DYNAMIC (and SYSTEMMEM) buffers take NOOVERWRITE writes next to bytes
    // the open list already read: keep them out of the GPU cache.
    const Pool kind = (usage & D3DUSAGE_DYNAMIC) || pool == D3DPOOL_SYSTEMMEM ? POOL_DYNAMIC : POOL_BUFFER;
    if (!device->AllocMemory(kind, size + kCanaryBytes, 256, &m_memory))
        return false;
    std::memset(m_memory.cpu, 0, size);
    FillCanary(m_memory.cpu + size);
    return true;
}

bool Buffer::CanaryIntact() const
{
    return CheckCanary(m_memory.cpu + m_size);
}

void Buffer::ReleaseMemory(Device *device)
{
    DeviceLockGuard lock(device->Lock());
    // A window's memory belongs to the frame arena (deko9_arena.h), which
    // retires it by frame id; this Buffer never owned it (m_memory.block is
    // null for a window, so FreeMemoryAfter would be a no-op anyway -- the
    // explicit check documents why).
    if (!m_window)
        device->FreeMemoryAfter(m_memory, device->OpenSeq());
    m_memory = {};
}

void Buffer::BindWindow(Device *device, uint64_t gpu, void *cpu, uint32_t size)
{
    DeviceLockGuard lock(device->Lock());
    if (!m_window)
        // First conversion: this buffer's original owned memory is never
        // used again once it becomes a window (2.2's converted protocol
        // must not be mixed with the old one) -- free it, deferred past
        // whatever list may still reference it.
        device->FreeMemoryAfter(m_memory, device->OpenSeq());
    m_window = true;
    m_memory = {};
    m_memory.gpu = gpu;
    m_memory.cpu = static_cast<uint8_t *>(cpu);
    m_size = size;
    // A bound stream's address changed and must be re-stamped (2.2: "the
    // window's previous span stays valid for draws already recorded" --
    // ApplyVertexStreams re-reads Gpu()/Size() only once dirtied; the index
    // bind already re-reads Gpu() every draw unconditionally).
    device->BufferRenamed();
}

HRESULT Buffer::Lock(Device *device, UINT offset, UINT size, void **data, DWORD flags, uint64_t *renamedBytes)
{
    if (renamedBytes)
        *renamedBytes = 0;
    DeviceLockGuard lock(device->Lock());
    if (m_window)
        return Fail("WINDOW_LOCK", "Lock on a window buffer (offset=%u size=%u flags=0x%x)", (unsigned)offset,
                    (unsigned)size, (unsigned)flags);
    DeviceLockSite site(device->Lock(), "buflock");
    if (!data || m_locked || offset > m_size || (size && size > m_size - offset))
        return D3DERR_INVALIDCALL;
    // Busy only matters when the lock may overwrite data the GPU reads, and
    // the cached completed sequence answers first: a buffer the GPU is known
    // to be done with (or never used) needs no fence poll.
    bool busy = false;
    if (!(flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY)) && lastUse > device->CachedCompletedSeq())
    {
        ++device->m_timing.bufferLockPolls;
        busy = !device->SeqDone(lastUse);
    }
    const bool discard = (flags & D3DLOCK_DISCARD) && (m_usage & D3DUSAGE_DYNAMIC);
    if (busy)
    {
        // The GPU may still read the old contents: rename. Without DISCARD
        // the new memory starts as a copy, as D3D9 preserves contents.
        GpuAlloc fresh;
        if (!device->AllocMemory(m_memory.pool, m_size + kCanaryBytes, 256, &fresh))
            return D3DERR_OUTOFVIDEOMEMORY;
        if (!discard)
        {
            std::memcpy(fresh.cpu, m_memory.cpu, m_size);
            if (renamedBytes)
                *renamedBytes = m_size;
        }
        FillCanary(fresh.cpu + m_size);
        device->FreeMemoryAfter(m_memory, device->OpenSeq());
        m_memory = fresh;
        lastUse = 0;
        // A bound stream's address changed and it must be re-stamped.
        device->BufferRenamed();
    }
    m_locked = true;
    *data = m_memory.cpu + offset;
    return D3D_OK;
}

HRESULT Buffer::Unlock(Device *device)
{
    DeviceLockGuard lock(device->Lock());
    if (!m_locked)
        return D3DERR_INVALIDCALL;
    m_locked = false;
#ifdef DEKO9_CANARY_CHECKS
    if (!CanaryIntact())
    {
        Fail("OVERRUN", "write past the end of a %u-byte %s buffer (usage=0x%x)", m_size,
             m_memory.pool == POOL_DYNAMIC ? "dynamic" : "static", (unsigned)m_usage);
        FillCanary(m_memory.cpu + m_size);
    }
#endif
    return D3D_OK;
}

VertexBuffer::~VertexBuffer()
{
    m_device->ForgetBuffer(this); // device slots are non-owning
    buffer.ReleaseMemory(m_device);
}

HRESULT VertexBuffer::GetDevice(IDirect3DDevice9 **device)
{
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
}

namespace
{
// r_deko9Census: Lock/Unlock VB/IB grouped per D3DLOCK flag (DISCARD takes
// priority, then NOOVERWRITE, then READONLY; else NONE).
CensusId CensusIdForVbLock(DWORD flags)
{
    if (flags & D3DLOCK_DISCARD) return Census_LockVbDiscard;
    if (flags & D3DLOCK_NOOVERWRITE) return Census_LockVbNoOverwrite;
    if (flags & D3DLOCK_READONLY) return Census_LockVbReadOnly;
    return Census_LockVbNone;
}
CensusId CensusIdForIbLock(DWORD flags)
{
    if (flags & D3DLOCK_DISCARD) return Census_LockIbDiscard;
    if (flags & D3DLOCK_NOOVERWRITE) return Census_LockIbNoOverwrite;
    if (flags & D3DLOCK_READONLY) return Census_LockIbReadOnly;
    return Census_LockIbNone;
}
} // namespace

HRESULT VertexBuffer::Lock(UINT offset, UINT size, void **data, DWORD flags)
{
    CensusScope census(m_device, CensusIdForVbLock(flags));
    uint64_t renamed = 0;
    const HRESULT hr = buffer.Lock(m_device, offset, size, data, flags, &renamed);
    census.AddBytes(renamed);
    if (SUCCEEDED(hr) && m_device->CensusOn())
        m_device->CensusBufferRole(&buffer, false, renamed);
    return hr;
}

HRESULT VertexBuffer::Unlock()
{
    CensusScope census(m_device, CensusIdForVbLock(0)); // NONE bucket: Unlock carries no D3DLOCK flags
    return buffer.Unlock(m_device);
}

HRESULT VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *desc)
{
    if (!desc)
        return D3DERR_INVALIDCALL;
    desc->Format = D3DFMT_VERTEXDATA;
    desc->Type = D3DRTYPE_VERTEXBUFFER;
    desc->Usage = buffer.Usage();
    desc->Pool = buffer.PoolKind();
    desc->Size = buffer.Size();
    desc->FVF = m_fvf;
    return D3D_OK;
}

IndexBuffer::~IndexBuffer()
{
    m_device->ForgetBuffer(this); // device slots are non-owning
    buffer.ReleaseMemory(m_device);
}

HRESULT IndexBuffer::GetDevice(IDirect3DDevice9 **device)
{
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
}

HRESULT IndexBuffer::Lock(UINT offset, UINT size, void **data, DWORD flags)
{
    CensusScope census(m_device, CensusIdForIbLock(flags));
    uint64_t renamed = 0;
    const HRESULT hr = buffer.Lock(m_device, offset, size, data, flags, &renamed);
    census.AddBytes(renamed);
    if (SUCCEEDED(hr) && m_device->CensusOn())
        m_device->CensusBufferRole(&buffer, true, renamed);
    return hr;
}

HRESULT IndexBuffer::Unlock()
{
    CensusScope census(m_device, CensusIdForIbLock(0)); // NONE bucket: Unlock carries no D3DLOCK flags
    return buffer.Unlock(m_device);
}

HRESULT IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *desc)
{
    if (!desc)
        return D3DERR_INVALIDCALL;
    desc->Format = m_format;
    desc->Type = D3DRTYPE_INDEXBUFFER;
    desc->Usage = buffer.Usage();
    desc->Pool = buffer.PoolKind();
    desc->Size = buffer.Size();
    return D3D_OK;
}

} // namespace deko9

// Frame-arena window binding: a
// native call, not a D3D9 method, so it lives outside namespace deko9 like
// the rest of deko9_native.h's free functions (Deko9_SetBufferRole above).
void Deko9_BindWindow(IDirect3DVertexBuffer9 *vb, const Deko9Span &span)
{
    if (!vb)
        return;
    auto *v = static_cast<deko9::VertexBuffer *>(vb);
    v->buffer.BindWindow(v->GetDeko9Device(), span.gpu, span.cpu, span.size);
}

void Deko9_BindWindow(IDirect3DIndexBuffer9 *ib, const Deko9Span &span)
{
    if (!ib)
        return;
    auto *i = static_cast<deko9::IndexBuffer *>(ib);
    i->buffer.BindWindow(i->GetDeko9Device(), span.gpu, span.cpu, span.size);
}

namespace deko9
{

HRESULT Device::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                   IDirect3DVertexBuffer9 **out, HANDLE *shared)
{
    DeviceLockGuard lock(m_lock);
    if (!out || shared || !length)
        return D3DERR_INVALIDCALL;
    *out = nullptr;
    VertexBuffer *vb = new VertexBuffer(this, fvf);
    if (!vb->buffer.Init(this, length, usage, pool))
    {
        vb->Release();
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = vb;
    return D3D_OK;
}

HRESULT Device::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                  IDirect3DIndexBuffer9 **out, HANDLE *shared)
{
    DeviceLockGuard lock(m_lock);
    if (!out || shared || !length || (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32))
        return D3DERR_INVALIDCALL;
    *out = nullptr;
    IndexBuffer *ib = new IndexBuffer(this, format);
    if (!ib->buffer.Init(this, length, usage, pool))
    {
        ib->Release();
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = ib;
    return D3D_OK;
}

// ---- vertex declarations -------------------------------------------------------

VertexDecl::VertexDecl(Device *device, const D3DVERTEXELEMENT9 *e, uint32_t count)
    : Object(device), elements(e, e + count), id(device->NextId())
{
    for (const D3DVERTEXELEMENT9 &element : elements)
        streamCount = std::max<uint32_t>(streamCount, element.Stream + 1u);
}

VertexDecl::~VertexDecl()
{
    m_device->ForgetProgramObject(id);
}

HRESULT VertexDecl::GetDevice(IDirect3DDevice9 **device)
{
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
}

HRESULT VertexDecl::GetDeclaration(D3DVERTEXELEMENT9 *out, UINT *count)
{
    if (!count)
        return D3DERR_INVALIDCALL;
    const UINT total = (UINT)elements.size() + 1;
    if (out)
    {
        std::copy(elements.begin(), elements.end(), out);
        out[total - 1] = D3DDECL_END();
    }
    *count = total;
    return D3D_OK;
}

HRESULT Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **decl)
{
    DeviceLockGuard lock(m_lock);
    if (!elements || !decl)
        return D3DERR_INVALIDCALL;
    uint32_t count = 0;
    while (elements[count].Stream != 0xff && count < 64)
    {
        VertexFormat format;
        if (!MapDeclType(elements[count].Type, &format))
            return Fail("VERTEX_DECL", "element %u type %u unsupported", count, (unsigned)elements[count].Type);
        if (elements[count].Stream >= DK_MAX_VERTEX_BUFFERS || elements[count].Method != D3DDECLMETHOD_DEFAULT)
            return Fail("VERTEX_DECL", "element %u stream %u method %u unsupported", count,
                        (unsigned)elements[count].Stream, (unsigned)elements[count].Method);
        ++count;
    }
    *decl = new VertexDecl(this, elements, count);
    return D3D_OK;
}

// ---- shaders --------------------------------------------------------------------

namespace
{
// Byte length of a DXSO token stream, END token included.
size_t BytecodeLength(const DWORD *tokens)
{
    const DWORD version = tokens[0];
    if ((version & 0xfffe0000) != 0xfffe0000)
        return 0;
    const uint32_t major = (version >> 8) & 0xff;
    size_t i = 1;
    for (;;)
    {
        const DWORD token = tokens[i];
        if (token == 0x0000ffff)
            return (i + 1) * 4;
        if ((token & 0xffff) == 0xfffe)
        {
            i += 1 + ((token >> 16) & 0x7fff); // comment block
        }
        else if (major >= 2)
        {
            i += 1 + ((token >> 24) & 0xf); // sm2+: instruction length field
        }
        else
        {
            ++i; // sm1: walk tokens; parameters have bit 31 set, END does not
        }
        if (i > (1u << 20))
            return 0;
    }
}

const char kCacheDir[] = "sdmc:/switch/kisakcod/deko9-cache";
// Bump whenever the translator output, its preludes or the MojoShader/UAM
// pins change: a pack from another version must never be loaded (kept
// in lockstep with deko9_shaderpack.h's kShaderPackVersion default).
constexpr uint32_t kCacheVersion = 5;

std::string ShaderPackPath()
{
    char path[160];
    std::snprintf(path, sizeof(path), "%s/pack-v%u.bin", kCacheDir, kCacheVersion);
    return path;
}

// Process-wide: DKSH bytes do not depend on which Device compiled them, so
// every Device shares one pack. Guarded by its own mutex, independent of
// any Device's lock, so ShaderBase::Init/Variant (a hit is a pure in-memory
// lookup) and Deko9_FlushShaderPack (called from the DB thread at zone-load
// end, outside any draw) never race each other.
std::mutex g_shaderPackLock;
ShaderPack g_shaderPack(kCacheVersion);
bool g_shaderPackLoaded = false;

// Reads the whole pack file (if any) once, outside any lock the caller may
// be about to take -- this is the one place a warm boot does SD I/O for
// shaders; every ShaderBase::Init/Variant after it is memory-only on a hit.
// A version/pin mismatch or any corruption resets to an empty pack (every
// shader retranslates this boot, same as a cold cache) and logs once: a
// stale or damaged pack is never partially trusted.
void EnsureShaderPackLoaded()
{
    std::lock_guard<std::mutex> lock(g_shaderPackLock);
    if (g_shaderPackLoaded)
        return;
    g_shaderPackLoaded = true;
    const std::string path = ShaderPackPath();
    std::vector<uint8_t> bytes;
    if (FILE *file = std::fopen(path.c_str(), "rb"))
    {
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::rewind(file);
        if (size > 0)
        {
            bytes.resize((size_t)size);
            if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
                bytes.clear();
        }
        std::fclose(file);
    }
    if (bytes.empty())
        return;
    std::string error;
    if (!g_shaderPack.Load(bytes, &error))
    {
        Log("shaderpack discard %s: %s (rebuilding this boot)", path.c_str(), error.c_str());
        g_shaderPack = ShaderPack(kCacheVersion);
        return;
    }
    Log("shaderpack loaded %s records=%zu bytes=%zu", path.c_str(), g_shaderPack.RecordCount(), bytes.size());
}
} // namespace

bool ShaderBase::Init(Device *device, const DWORD *function, Deko9Stage stage, std::string *error)
{
    const size_t bytes = BytecodeLength(function);
    if (!bytes)
        return *error = "unparseable DXSO token stream", false;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(function);
    m_bytecode.assign(p, p + bytes);
    m_hash = Deko9_HashBytecode(p, bytes);
    m_stage = stage;
    id = device->NextId();
    // The base variant's key (no shadow mask, no instancing, no early-Z): a
    // hit supplies m_info too, so a fully cached shader needs no MojoShader
    // translation at all, not even to learn its declared inputs/samplers.
    const ShaderVariantKey baseKey{m_hash, 0, 0, 0, 0, 0};
    bool cacheHit = false;
    {
        std::lock_guard<std::mutex> lock(g_shaderPackLock);
        if (const ShaderPackRecord *hit = g_shaderPack.Find(baseKey))
        {
            m_info = hit->info;
            cacheHit = true;
        }
    }
    if (!cacheHit)
    {
        std::string glsl;
        if (!Deko9_TranslateShader(p, bytes, 0, &glsl, &m_info, error))
            return false;
    }
    if (m_info.stage != stage)
        return *error = "shader stage does not match the create call", false;
    // Compile the base variant now, during loading, not at first draw.
    return Variant(device, 0) != nullptr;
}

const ShaderVariant *ShaderBase::Variant(Device *device, uint32_t shadowMask, const InstanceLayout &instance,
                                         bool earlyZ)
{
    // r_shadowFilter only changes pixel shaders with a depth-compare sampler
    // (the rewrite is inert without one), so only those get another variant.
    const uint32_t shadowFilter = shadowMask && m_stage == DEKO9_STAGE_PIXEL ? device->ShadowFilter() : 0;
    for (const auto &variant : m_variants)
    {
        if (variant->shadowMask == shadowMask && variant->instance == instance && variant->earlyZ == earlyZ &&
            variant->shadowFilter == shadowFilter)
            return variant.get();
    }
    if (instance.count && m_stage != DEKO9_STAGE_VERTEX)
    {
        Fail("SHADER", "%016llx: instance layout on a pixel shader", (unsigned long long)m_hash);
        return nullptr;
    }
    if (earlyZ && (m_stage != DEKO9_STAGE_PIXEL || m_info.writesDepth))
    {
        Fail("SHADER", "%016llx: early-Z variant of a %s", (unsigned long long)m_hash,
             m_stage != DEKO9_STAGE_PIXEL ? "vertex shader" : "depth-writing pixel shader");
        return nullptr;
    }
    // DEKO9_SHADOW_FILTER_VERSION rides in the key (not a separate
    // file-name suffix any more): a changed shadowFilter rewrite still
    // never hits a pack record an older one built.
    const ShaderVariantKey key{m_hash,
                               shadowMask,
                               instance.Hash(),
                               shadowFilter,
                               shadowFilter ? (uint32_t)DEKO9_SHADOW_FILTER_VERSION : 0u,
                               (uint8_t)(earlyZ ? 1 : 0)};
    std::vector<uint8_t> dksh;
    {
        std::lock_guard<std::mutex> lock(g_shaderPackLock);
        if (const ShaderPackRecord *hit = g_shaderPack.Find(key))
            dksh = hit->dksh; // copy out: cheap, and keeps ShaderVariant self-contained
    }
    std::string error;
    if (dksh.empty())
    {
        std::string glsl;
        Deko9ShaderInfo info{};
        if (!Deko9_TranslateShader(m_bytecode.data(), m_bytecode.size(), shadowMask, &glsl, &info, &error,
                                   instance.regs, instance.count, earlyZ, shadowFilter) ||
            !Deko9_CompileDksh(m_stage, glsl, &dksh, &error))
        {
            Fail("SHADER", "%016llx mask=0x%x inst=%u ez=%u sf=%u: %s", (unsigned long long)m_hash, shadowMask,
                 (unsigned)instance.count, (unsigned)earlyZ, shadowFilter, error.c_str());
            return nullptr;
        }
        // Grown only in memory here; Deko9_FlushShaderPack() (called once
        // per zone load, off this path) writes the delta back.
        std::lock_guard<std::mutex> lock(g_shaderPackLock);
        g_shaderPack.Append(key, info, dksh);
    }
    auto variant = std::make_unique<ShaderVariant>();
    variant->shadowMask = shadowMask;
    variant->instance = instance;
    variant->earlyZ = earlyZ;
    variant->shadowFilter = shadowFilter;
    if (!device->LoadShaderCode(dksh, variant.get(), &error))
    {
        Fail("SHADER", "%016llx mask=0x%x: %s", (unsigned long long)m_hash, shadowMask, error.c_str());
        return nullptr;
    }
    m_variants.push_back(std::move(variant));
    return m_variants.back().get();
}

void ShaderBase::ReleaseMemory(Device *device)
{
    DeviceLockGuard lock(device->Lock());
    for (const auto &variant : m_variants)
        device->FreeMemoryAfter(variant->code, device->OpenSeq());
    m_variants.clear();
    device->ForgetBoundShaders();
    device->ForgetProgramObject(id); // baked units point at the freed variants
}

bool Device::LoadShaderCode(const std::vector<uint8_t> &dksh, ShaderVariant *variant, std::string *error)
{
    struct DkshHeader
    {
        uint32_t magic, headerSize, controlSize, codeSize, programsOffset, programCount;
    } header;
    if (dksh.size() < sizeof(header))
        return *error = "DKSH too small", false;
    std::memcpy(&header, dksh.data(), sizeof(header));
    if (header.magic != 0x48534b44 || header.programCount != 1 || header.controlSize < sizeof(header) ||
        (uint64_t)header.controlSize + header.codeSize > dksh.size())
        return *error = "malformed DKSH header", false;
    // Constant-buffer slots the program reads, statically: any slot deko9
    // does not bind for the stage would be read through whatever address
    // the channel holds for it, which can fault the GPU MMU. Refused here,
    // before the program can ever be bound.
    Deko9DkshInfo scan{};
    if (!Deko9_ScanDksh(dksh.data(), dksh.size(), &scan, error))
        return false;
    if (!scan.instructions)
        return *error = "DKSH program decodes to no instructions", false;
    // The early-Z variant must carry the forced early tests (and only it):
    // a stale or foreign cache file would silently test late (or early).
    if (scan.programType == 1 && scan.earlyFragmentTests != variant->earlyZ)
        return *error = variant->earlyZ ? "early-Z variant lacks early_fragment_tests"
                                        : "program forces early fragment tests",
               false;
    if (const uint32_t unbound = scan.slotMask & ~Deko9_BoundCbufSlots(scan))
    {
        char detail[128];
        std::snprintf(detail, sizeof(detail), "program reads unbound constant-buffer slots 0x%x (reads 0x%x, bound 0x%x)",
                      unbound, scan.slotMask, Deko9_BoundCbufSlots(scan));
        return *error = detail, false;
    }
    variant->slotMask = scan.slotMask;
    if (!AllocMemory(POOL_CODE, header.codeSize, DK_SHADER_CODE_ALIGNMENT, &variant->code))
        return *error = "shader code memory exhausted", false;
    std::memcpy(variant->code.cpu, dksh.data() + header.controlSize, header.codeSize);
    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, variant->code.block, variant->code.offset);
    maker.control = dksh.data();
    dkShaderInitialize(&variant->shader, &maker);
    if (!dkShaderIsValid(&variant->shader))
        return *error = "dkShaderInitialize rejected the DKSH", false;
    Deko9_DkshStats(dksh.data(), dksh.size(), &variant->stats);
    return true;
}

VertexShader::~VertexShader()
{
    shader.ReleaseMemory(m_device);
}

HRESULT VertexShader::GetDevice(IDirect3DDevice9 **device)
{
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
}

HRESULT VertexShader::GetFunction(void *data, UINT *size)
{
    if (!size)
        return D3DERR_INVALIDCALL;
    if (data)
        std::memcpy(data, shader.Bytecode().data(), std::min<size_t>(*size, shader.Bytecode().size()));
    *size = (UINT)shader.Bytecode().size();
    return D3D_OK;
}

PixelShader::~PixelShader()
{
    shader.ReleaseMemory(m_device);
}

HRESULT PixelShader::GetDevice(IDirect3DDevice9 **device)
{
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
}

HRESULT PixelShader::GetFunction(void *data, UINT *size)
{
    if (!size)
        return D3DERR_INVALIDCALL;
    if (data)
        std::memcpy(data, shader.Bytecode().data(), std::min<size_t>(*size, shader.Bytecode().size()));
    *size = (UINT)shader.Bytecode().size();
    return D3D_OK;
}

HRESULT Device::CreateVertexShader(const DWORD *function, IDirect3DVertexShader9 **out)
{
    // Outside the device lock: on a warm boot this is the one SD read for
    // every shader this process creates. A no-op after the first call.
    EnsureShaderPackLoaded();
    DeviceLockGuard lock(m_lock);
    if (!function || !out)
        return D3DERR_INVALIDCALL;
    *out = nullptr;
    VertexShader *vs = new VertexShader(this);
    std::string error;
    if (!vs->shader.Init(this, function, DEKO9_STAGE_VERTEX, &error))
    {
        vs->Release();
        return Fail("SHADER", "vertex shader: %s", error.c_str());
    }
    *out = vs;
    return D3D_OK;
}

HRESULT Device::CreatePixelShader(const DWORD *function, IDirect3DPixelShader9 **out)
{
    // Outside the device lock: see Device::CreateVertexShader.
    EnsureShaderPackLoaded();
    DeviceLockGuard lock(m_lock);
    if (!function || !out)
        return D3DERR_INVALIDCALL;
    *out = nullptr;
    PixelShader *ps = new PixelShader(this);
    std::string error;
    if (!ps->shader.Init(this, function, DEKO9_STAGE_PIXEL, &error))
    {
        ps->Release();
        return Fail("SHADER", "pixel shader: %s", error.c_str());
    }
    *out = ps;
    return D3D_OK;
}

// ---- queries --------------------------------------------------------------------

Query::Query(Device *device, D3DQUERYTYPE type) : Object(device), m_type(type)
{
    if (type == D3DQUERYTYPE_OCCLUSION)
        device->AllocMemory(POOL_DYNAMIC, 32, 256, &m_report);
}

Query::~Query()
{
    DeviceLockGuard lock(m_device->Lock());
    if (m_begun)
        m_device->NoteOcclusionQuery(-1);
    m_device->FreeMemoryAfter(m_report, m_device->OpenSeq());
}

HRESULT Query::GetDevice(IDirect3DDevice9 **device)
{
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
}

DWORD Query::GetDataSize()
{
    return m_type == D3DQUERYTYPE_OCCLUSION ? sizeof(DWORD) : sizeof(BOOL);
}

HRESULT Query::Issue(DWORD flags)
{
    CensusScope census(m_device, Census_QueryIssue);
    DeviceLockGuard lock(m_device->Lock());
    if (m_type == D3DQUERYTYPE_OCCLUSION)
    {
        if (!m_report)
            return D3DERR_OUTOFVIDEOMEMORY;
        if (flags & D3DISSUE_BEGIN)
        {
            dkCmdBufReportCounter(m_device->Cmd(), DkCounter_SamplesPassed, m_report.gpu);
            if (!m_begun)
                m_device->NoteOcclusionQuery(+1);
            m_begun = true;
        }
        if (flags & D3DISSUE_END)
        {
            if (!m_begun)
                dkCmdBufReportCounter(m_device->Cmd(), DkCounter_SamplesPassed, m_report.gpu);
            else
                m_device->NoteOcclusionQuery(-1);
            dkCmdBufReportCounter(m_device->Cmd(), DkCounter_SamplesPassed, m_report.gpu + 16);
            m_begun = false;
            m_issued = true;
            // The reports are recorded work: the point is a marker after them.
            m_device->MarkWork();
            m_point = IssueEventPoint(m_device->OpenSeq(), true);
            m_marker = m_device->RecordEventMarker();
        }
        m_device->MarkWork();
        ++m_device->Stats().queries;
        return D3D_OK;
    }
    if (flags & D3DISSUE_END)
    {
        // D3D9 event semantics: done once every command recorded before this
        // Issue has completed. With recorded work in the open list, an
        // in-list fence marks this point (not the end of the list, which
        // may hold a whole later frame); with an empty list, the newest
        // submitted list already holds everything before it.
        m_issued = true;
        m_point = IssueEventPoint(m_device->OpenSeq(), m_device->ListHasWork());
        m_marker = m_point.marker ? m_device->RecordEventMarker() : nullptr;
        ++m_device->Stats().queries;
    }
    return D3D_OK;
}

bool Query::Done()
{
    return EventDone(
        m_point, m_device->OpenSeq(), [this](uint64_t seq) { return m_device->SeqDone(seq); },
        [this]() {
            // The list is submitted and not retired, so the marker still
            // belongs to this point (recycled only after RetireSeq).
            DkFence fence = m_marker->fence;
            return dkFenceWait(&fence, 0) == DkResult_Success;
        });
}

HRESULT Query::GetData(void *data, DWORD size, DWORD flags)
{
    CensusScope census(m_device, Census_QueryGetData);
    DeviceLockGuard lock(m_device->Lock());
    ++m_device->m_timing.queryGetData;
    if (!m_issued)
        return m_type == D3DQUERYTYPE_EVENT ? S_OK : D3DERR_INVALIDCALL;
    // Only the thread recording draws may submit the open list. The SP
    // server thread polls the frame's end fence with FLUSH while it waits
    // (R_EndFenceBusy via R_ProcessWorkerCmdsWithTimeout, sv_smp 1); a flush
    // from there cut the render thread's list at an arbitrary call. D3D9 lets
    // a FLUSH poll report "not done" without flushing.
    // A list is only flushed while the query's point is still in it: once
    // submitted, repeated FLUSH polls (a GPU-sync spin) submit nothing.
    if (EventNeedsSubmit(m_point, m_device->OpenSeq()) && (flags & D3DGETDATA_FLUSH) &&
        m_device->IsRecordingThread())
        m_device->Flush(true);
    if (!Done())
    {
        ++m_device->m_timing.queryPending;
        return S_FALSE;
    }
    if (!data || !size)
        return S_OK;
    if (m_type == D3DQUERYTYPE_OCCLUSION)
    {
        uint64_t begin, end;
        std::memcpy(&begin, m_report.cpu, 8);
        std::memcpy(&end, m_report.cpu + 16, 8);
        const DWORD samples = (DWORD)(end - begin);
        std::memcpy(data, &samples, std::min<DWORD>(size, sizeof(samples)));
    }
    else
    {
        const BOOL done = TRUE;
        std::memcpy(data, &done, std::min<DWORD>(size, sizeof(done)));
    }
    return S_OK;
}

bool Query::Wait(int64_t timeoutNs)
{
    DkFence fence;
    uint64_t listSeq = 0;
    {
        DeviceLockGuard lock(m_device->Lock());
        if (!m_issued || Done())
            return true;
        // Blocking with the device lock held (a batch) would stall every
        // other thread's device call: only poll then. A point still in the
        // open list is never waited for (it would not signal).
        if (m_device->Lock().Depth() > 1 || EventNeedsSubmit(m_point, m_device->OpenSeq()))
            return false;
        if (m_point.marker)
            fence = m_marker->fence;
        else
            listSeq = m_point.seq;
    }
    if (listSeq)
        return m_device->WaitSeqFor(listSeq, timeoutNs);
    uint64_t waited = 0;
    Device::SleepPollFence(fence, timeoutNs, &waited);
    DeviceLockGuard lock(m_device->Lock());
    ++m_device->m_timing.queryWaits;
    m_device->m_timing.queryWaitNs += waited;
    return Done();
}

bool Query::RawGpuPassed()
{
    DeviceLockGuard lock(m_device->Lock());
    if (!m_issued || !m_point.seq)
        return true;
    if (EventNeedsSubmit(m_point, m_device->OpenSeq()))
        return false;
    if (m_point.marker && m_marker->seq == m_point.seq)
    {
        DkFence fence = m_marker->fence;
        if (dkFenceWait(&fence, 0) == DkResult_Success)
            return true;
    }
    return m_device->RawSeqPassed(m_point.seq);
}

HRESULT Device::CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **query)
{
    DeviceLockGuard lock(m_lock);
    if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION)
        return D3DERR_NOTAVAILABLE;
    if (!query)
        return D3D_OK; // support probe
    *query = new Query(this, type);
    return D3D_OK;
}

} // namespace deko9

void Deko9_SetRtCompression(bool enable)
{
    deko9::g_rtCompression.store(enable, std::memory_order_relaxed);
}

void Deko9_SetRtCompressionOverride(int compress)
{
    deko9::t_rtCompressionOverride = compress < 0 ? -1 : compress != 0;
}

bool Deko9_IsCompressed(IDirect3DResource9 *resource)
{
    if (!resource)
        return false;
    switch (resource->GetType())
    {
    case D3DRTYPE_SURFACE: return static_cast<deko9::Surface *>(resource)->Store()->compressed;
    case D3DRTYPE_TEXTURE: return static_cast<deko9::Texture2D *>(resource)->Store()->compressed;
    case D3DRTYPE_CUBETEXTURE: return static_cast<deko9::CubeTexture *>(resource)->Store()->compressed;
    default: return false;
    }
}

namespace
{
deko9::ImageStore *StoreOfResource(IDirect3DResource9 *resource)
{
    if (!resource)
        return nullptr;
    switch (resource->GetType())
    {
    case D3DRTYPE_SURFACE: return static_cast<deko9::Surface *>(resource)->Store().get();
    case D3DRTYPE_TEXTURE: return static_cast<deko9::Texture2D *>(resource)->Store().get();
    default: return nullptr;
    }
}
} // namespace

bool Deko9_ResizeRenderTarget(IDirect3DDevice9 *device, IDirect3DResource9 *resource, uint32_t width,
                              uint32_t height)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    std::string error;
    if (!d->ResizeStore(StoreOfResource(resource), width, height, &error))
    {
        deko9::Fail("RESIZE_RENDER_TARGET", "%s", error.c_str());
        return false;
    }
    return true;
}

bool Deko9_MoveContents(IDirect3DDevice9 *device, IDirect3DResource9 *src, IDirect3DResource9 *dst)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    std::string error;
    if (!d->MoveStoreContents(StoreOfResource(src), StoreOfResource(dst), &error))
    {
        deko9::Fail("MOVE_CONTENTS", "%s", error.c_str());
        return false;
    }
    return true;
}

bool Deko9_SetColorless(IDirect3DSurface9 *surface)
{
    if (!surface || surface->GetType() != D3DRTYPE_SURFACE)
        return false;
    deko9::Surface *s = static_cast<deko9::Surface *>(surface);
    deko9::ImageStore *store = s->Store().get();
    if (!store || store->format->depth || !(store->usage & D3DUSAGE_RENDERTARGET))
        return false;
    store->colorless = true;
    deko9::Log("colorless target %ux%u d3dfmt=%u (bound as no colour attachment)", store->width,
               store->height, (unsigned)store->format->d3d);
    return true;
}

void Deko9_SetFrameTag(IDirect3DDevice9 *device, uint32_t width, uint32_t height)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetFrameTag(width, height);
}

bool Deko9_GetGpuFrame(IDirect3DDevice9 *device, float *gpuMs, uint32_t *width, uint32_t *height, uint32_t *count)
{
    // Lock-free: the render back end may hold the device lock for a whole
    // draw list; the controller on the main thread must not wait for it.
    const deko9::Device *d = static_cast<const deko9::Device *>(device);
    if (!d || !gpuMs || !width || !height || !count)
        return false;
    d->GetGpuFrame(gpuMs, width, height, count);
    return *count != 0;
}

// Writes any DKSH compiled since the shader pack was loaded (or last
// flush) to sdmc:/switch/kisakcod/deko9-cache/pack-v<kCacheVersion>.bin
// (temp file + rename). Called once per successful zone load
// (db_registry.cpp, next to KILLHOUSE_LOAD_SHADERS), never per shader and
// never under any Device's lock -- g_shaderPackLock (deko9_resources.cpp)
// is the only synchronization this needs. A no-op when nothing new
// compiled, or when no pack was ever loaded (no shader created yet).
void Deko9_FlushShaderPack()
{
    std::vector<uint8_t> bytes;
    size_t records = 0;
    {
        std::lock_guard<std::mutex> lock(deko9::g_shaderPackLock);
        if (!deko9::g_shaderPackLoaded || !deko9::g_shaderPack.Dirty())
            return;
        bytes = deko9::g_shaderPack.Serialize();
        records = deko9::g_shaderPack.RecordCount();
    }
    const std::string path = deko9::ShaderPackPath();
    const std::string tmp = path + ".tmp";
    mkdir(deko9::kCacheDir, 0777);
    bool ok = false;
    if (FILE *file = std::fopen(tmp.c_str(), "wb"))
    {
        ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
        ok = std::fclose(file) == 0 && ok;
    }
    if (ok)
    {
        // sdmc: rename() can fail when the destination already exists (the
        // same quirk FS_Rename works around in com_files.cpp): remove the
        // old pack first and retry once before giving up.
        ok = std::rename(tmp.c_str(), path.c_str()) == 0;
        if (!ok)
        {
            std::remove(path.c_str());
            ok = std::rename(tmp.c_str(), path.c_str()) == 0;
        }
    }
    if (!ok)
        std::remove(tmp.c_str());
    deko9::Log("shaderpack flush %s %s records=%zu bytes=%zu", ok ? "ok" : "FAILED", path.c_str(), records,
              bytes.size());
}
