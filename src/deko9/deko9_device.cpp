// Device lifetime, submission, presentation and D3D9 state setters for the
// optional deko3d renderer. Draw-time state application lives in
// deko9_draw.cpp; resources in deko9_resources.cpp.

#include <switch.h>

#include "deko9_hazard_model.h"
#include "deko9_internal.h"
#include "deko9_native.h"
#include <platform/switch/switch_profile_format.h>
#include <port/switch_perf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace deko9
{
namespace
{

void DebugCallback(void *, const char *context, DkResult result, const char *message)
{
    Fail("DK", "context=%s result=%d %s", context ? context : "", (int)result, message ? message : "");
}

} // namespace

Device::Device(IDirect3D9 *d3d, HWND window, const D3DPRESENT_PARAMETERS &params)
    : Object(this), m_d3d(d3d), m_window(window), m_params(params)
{
    m_d3d->AddRef();
    if (!m_params.BackBufferWidth)
        m_params.BackBufferWidth = 1280;
    if (!m_params.BackBufferHeight)
        m_params.BackBufferHeight = 720;
    if (m_params.BackBufferFormat == D3DFMT_UNKNOWN)
        m_params.BackBufferFormat = D3DFMT_X8R8G8B8;
    m_presentInterval = m_params.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? 0 : 1;
}

bool Device::Init(std::string *error)
{
    DeviceLockGuard lock(m_lock);
    DkDeviceMaker deviceMaker;
    dkDeviceMakerDefaults(&deviceMaker);
    // D3D conventions: [0,1] clip depth, upper-left window origin.
    deviceMaker.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft;
    deviceMaker.cbDebug = DebugCallback;
    m_dk = dkDeviceCreate(&deviceMaker);
    if (!m_dk)
        return *error = "dkDeviceCreate failed", false;

    m_heaps[POOL_BUFFER].reset(new Heap(POOL_BUFFER, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 16u << 20));
    m_heaps[POOL_DYNAMIC].reset(new Heap(POOL_DYNAMIC, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached, 8u << 20));
    m_heaps[POOL_IMAGE].reset(new Heap(POOL_IMAGE, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 64u << 20));
    m_heaps[POOL_CMD].reset(new Heap(POOL_CMD, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 8u << 20));
    m_heaps[POOL_CODE].reset(new Heap(POOL_CODE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                                      4u << 20, DK_SHADER_CODE_UNUSABLE_SIZE));
    for (auto &heap : m_heaps)
        heap->SetEvents(&m_gpuEvents, &m_openSeq);

    // Frame-arena chunks are ordinary POOL_BUFFER memory (2.2); the arena
    // never frees a chunk back to the heap itself (retirement only returns
    // it to the arena's own free list), so Heap::Destroy() at device
    // teardown is what actually releases this memory, like every other
    // POOL_BUFFER allocation that outlives its owner.
    m_frameArena.SetFactory([this](uint32_t size, ArenaChunkMemory *out) {
        // A grow comes from any producer thread, outside the device lock;
        // the heap needs it (FrameArena calls this with its own mutex
        // released, so the lock order stays device lock -> arena mutex).
        DeviceLockGuard lock(m_lock, "FrameArena grow");
        GpuAlloc mem;
        if (!AllocMemory(POOL_BUFFER, size, 256, &mem))
            return false;
        out->cpu = mem.cpu;
        out->gpu = mem.gpu;
        out->size = mem.size;
        return true;
    });

    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, m_dk);
    queueMaker.flags = DkQueueFlags_Graphics | DkQueueFlags_HighPrio;
    queueMaker.commandMemorySize = 1u << 20;
    queueMaker.flushThreshold = 128u << 10;
    m_queue = dkQueueCreate(&queueMaker);
    if (!m_queue)
        return *error = "dkQueueCreate failed", false;
    // Zcull stays on for this queue's lifetime (DkQueueFlags_EnableZcull is 0,
    // the default; deko3d has no per-command-buffer zcull switch).
    // What deko3d sizes the zcull region from:
    // the region covers pixel_squares_by_aliquots x aliquot_total pixels.
    if (const nvioctl_zcull_info *zi = nvGpuGetZcullInfo())
        Log("zcull ctx=%u bytes align=%ux%u px/aliquot=%u aliquots=%u (region up to %u px) byte_mult=%u",
            nvGpuGetZcullCtxSize(), zi->width_align_pixels, zi->height_align_pixels, zi->pixel_squares_by_aliquots,
            zi->aliquot_total, zi->pixel_squares_by_aliquots * zi->aliquot_total, zi->region_byte_multiplier);

    DkCmdBufMaker cmdMaker;
    dkCmdBufMakerDefaults(&cmdMaker, m_dk);
    cmdMaker.userData = this;
    cmdMaker.cbAddMem = AddCmdMemory;
    m_cmd = dkCmdBufCreate(&cmdMaker);
    if (!m_cmd)
        return *error = "dkCmdBufCreate failed", false;
    // Capture-only command buffer for baking units (deko9_baked.h): it never
    // owns memory and never submits; dkCmdBufBeginCaptureCmds clears the
    // buffer it captures into, so capturing on m_cmd would drop the open list.
    DkCmdBufMaker bakeMaker;
    dkCmdBufMakerDefaults(&bakeMaker, m_dk);
    m_bakeCmd = dkCmdBufCreate(&bakeMaker);
    if (!m_bakeCmd)
        return *error = "dkCmdBufCreate (bake) failed", false;

    const uint32_t descriptorBytes = kImageDescriptors * sizeof(DkImageDescriptor) +
                                     kSamplerDescriptors * sizeof(DkSamplerDescriptor);
    if (!AllocMemory(POOL_DYNAMIC, kFenceRing * 32, 256, &m_timestamps) ||
        !AllocMemory(POOL_DYNAMIC, kPassSlots * 16, 256, &m_passStamps) ||
        !AllocMemory(POOL_DYNAMIC, kPassSlots * 16, 256, &m_zcullStamps) ||
        !AllocMemory(POOL_BUFFER, descriptorBytes, 256, &m_descriptorMemory) ||
        !AllocMemory(POOL_BUFFER, DEKO9_VS_CONST_REGS * 16, DK_UNIFORM_BUF_ALIGNMENT, &m_vsUbo) ||
        !AllocMemory(POOL_BUFFER, DEKO9_PS_CONST_REGS * 16, DK_UNIFORM_BUF_ALIGNMENT, &m_psUbo))
        return *error = "descriptor/constant memory allocation failed", false;
    std::memset(m_descriptorMemory.cpu, 0, descriptorBytes);
    std::memset(m_vsUbo.cpu, 0, m_vsUbo.size);
    std::memset(m_psUbo.cpu, 0, m_psUbo.size);
    for (uint32_t i = kImageDescriptors; i-- > 0;)
        m_freeImageDescriptors.push_back(i);

    const uint32_t width = m_params.BackBufferWidth, height = m_params.BackBufferHeight;
    // The swapchain is always display size; a smaller back buffer is the
    // engine's render resolution, upscaled at present (FSR 1).
    if (width > kDisplayWidth || height > kDisplayHeight)
        return *error = "back buffer larger than the display", false;
    DkImageLayoutMaker layoutMaker;
    dkImageLayoutMakerDefaults(&layoutMaker, m_dk);
    layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
    layoutMaker.format = DkImageFormat_RGBA8_Unorm;
    layoutMaker.dimensions[0] = kDisplayWidth;
    layoutMaker.dimensions[1] = kDisplayHeight;
    DkImageLayout swapLayout;
    dkImageLayoutInitialize(&swapLayout, &layoutMaker);
    const DkImage *swapImages[3];
    for (int i = 0; i < 3; ++i)
    {
        if (!AllocMemory(POOL_IMAGE, (uint32_t)dkImageLayoutGetSize(&swapLayout),
                         dkImageLayoutGetAlignment(&swapLayout), &m_swapMemory[i]))
            return *error = "swapchain image allocation failed", false;
        dkImageInitialize(&m_swapImages[i], &swapLayout, m_swapMemory[i].block, m_swapMemory[i].offset);
        swapImages[i] = &m_swapImages[i];
    }
    DkSwapchainMaker swapMaker;
    dkSwapchainMakerDefaults(&swapMaker, m_dk, m_window, swapImages, 3);
    m_swapchain = dkSwapchainCreate(&swapMaker);
    if (!m_swapchain)
        return *error = "dkSwapchainCreate failed", false;
    dkSwapchainSetSwapInterval(m_swapchain, m_presentInterval);

    BeginList();

    // The D3D back buffer is an ordinary render target; Present blits it.
    {
        auto store = std::make_shared<ImageStore>();
        store->format = LookupFormat(m_params.BackBufferFormat);
        if (!store->format || store->format->depth || !store->format->renderable)
            return *error = "unsupported back buffer format", false;
        store->type = D3DRTYPE_SURFACE;
        store->pool = D3DPOOL_DEFAULT;
        store->usage = D3DUSAGE_RENDERTARGET;
        store->width = width;
        store->height = height;
        if (!CreateStore(store.get(), error))
            return false;
        m_backBuffer = new Surface(this, store, 0, 0, nullptr);
    }
    if ((width != kDisplayWidth || height != kDisplayHeight) && !InitUpscaler(error))
        return false;
    // Unbound samplers read (0,0,0,1), one per sampler dimension.
    static const D3DRESOURCETYPE kDummyTypes[4] = {D3DRTYPE_TEXTURE, D3DRTYPE_TEXTURE,
                                                   D3DRTYPE_VOLUMETEXTURE, D3DRTYPE_CUBETEXTURE};
    for (int dim = DEKO9_SAMPLER_2D; dim <= DEKO9_SAMPLER_CUBE; ++dim)
    {
        auto *store = new ImageStore();
        store->format = LookupFormat(D3DFMT_A8R8G8B8);
        store->type = kDummyTypes[dim];
        store->pool = D3DPOOL_DEFAULT;
        store->width = store->height = 1;
        store->faces = dim == DEKO9_SAMPLER_CUBE ? 6 : 1;
        if (!CreateStore(store, error))
            return false;
        GpuAlloc upload;
        if (!AllocUpload(4, 256, &upload))
            return *error = "dummy texture upload failed", false;
        const uint8_t black[4] = {0, 0, 0, 255};
        std::memcpy(upload.cpu, black, 4);
        for (uint32_t face = 0; face < store->faces; ++face)
            CopyBufferToImage(store, face, 0, upload.gpu, DkImageRect{0, 0, 0, 1, 1, 1});
        m_dummy[dim] = store;
    }
    m_swapChainObject = new SwapChain(this);
    ResetState();
    Flush(true);
    FrApplySettings(); // publishes the flight recorder, arms the recording-thread check
    return true;
}

Device::~Device()
{
    FrDetach();
    StopWatcher();
    {
        DeviceLockGuard lock(m_lock);
        if (m_queue && m_cmd && m_descriptorMemory)
            WaitIdle();
        std::memset(m_textures, 0, sizeof(m_textures)); // non-owning
        for (auto &slot : m_renderTargets)
            Rebind(slot, (Surface *)nullptr);
        Rebind(m_depthStencil, (Surface *)nullptr);
        for (auto &stream : m_streams)
            stream.buffer = nullptr; // non-owning
        m_indices = nullptr;
        Rebind(m_decl, (VertexDecl *)nullptr);
        Rebind(m_vs, (VertexShader *)nullptr);
        Rebind(m_ps, (PixelShader *)nullptr);
        ReleaseUpscaler();
        ReleaseTaau();
        if (m_backBuffer)
            m_backBuffer->Release();
        if (m_swapChainObject)
            m_swapChainObject->Release();
        for (ImageStore *dummy : m_dummy)
            delete dummy;
        if (m_swapchain)
            dkSwapchainDestroy(m_swapchain);
        m_rasterUnits.Clear();
        m_programUnits.Clear();
        if (m_bakeCmd)
            dkCmdBufDestroy(m_bakeCmd);
        if (m_cmd)
            dkCmdBufDestroy(m_cmd);
        if (m_queue)
            dkQueueDestroy(m_queue);
        for (auto &heap : m_heaps)
        {
            if (heap)
                heap->Destroy();
        }
        if (m_dk)
            dkDeviceDestroy(m_dk);
    }
    m_d3d->Release();
}

// ---- submission -------------------------------------------------------------

void Device::AddCmdMemory(void *userData, DkCmdBuf cmd, size_t minSize)
{
    Device *device = static_cast<Device *>(userData);
    GpuAlloc chunk;
    const uint32_t size = device->m_cmdChunkBytes;
    const uint32_t want = std::max<uint32_t>(size, AlignUp((uint32_t)minSize, 256));
    auto &free = device->m_freeCmdChunks;
    // Oversize or from an earlier chunk size; its list has completed. Reuse
    // takes the back (most recently freed); with poison on, the oldest one
    // out of quarantine instead, so mismatched sizes go from anywhere.
    for (size_t i = free.size(); i-- > 0;)
    {
        if (free[i].size == size || (!device->m_cmdPoison && i + 1 != free.size()))
            continue;
        device->FrNoteDrop(free[i]);
        GpuAlloc whole = free[i];
        whole.size += kCmdGuardBytes;
        device->FreeMemoryAfter(whole, 0);
        free.erase(free.begin() + (ptrdiff_t)i);
    }
    const int pick = want == size ? device->FrPickFree(size) : -1;
    uint32_t ev = fr::kEvReuse;
    if (pick >= 0)
    {
        chunk = free[(size_t)pick];
        free.erase(free.begin() + pick);
        device->CheckCmdGuard(chunk, "reuse", 0);
    }
    else if (!device->AllocMemory(POOL_CMD, want + kCmdGuardBytes, 256, &chunk))
    {
        Fail("CMD_MEMORY", "cannot grow command buffer by %zu bytes", minSize);
        return;
    }
    else
    {
        // chunk.size is the command area; the guard words follow it.
        chunk.size = want;
        ev = fr::kEvAlloc;
    }
    if (chunk.cpu)
        fr::FillGuard(reinterpret_cast<uint32_t *>(chunk.cpu + chunk.size));
    dkCmdBufAddMemory(cmd, chunk.block, chunk.offset, chunk.size);
    device->m_openCmdChunks.push_back(chunk);
    device->FrNoteOpen(chunk, ev);
}

void Device::CheckCmdGuard(const GpuAlloc &chunk, const char *where, uint64_t seq)
{
    if (!chunk.cpu)
        return;
    const uint32_t *words = reinterpret_cast<const uint32_t *>(chunk.cpu);
    const uint32_t cmdWords = chunk.size / 4;
    uint32_t first = 0;
    const uint32_t dirty = fr::CheckGuard(words + cmdWords, &first);
    if (!dirty)
        return;
    const uint32_t hits = m_cmdGuardHits.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t *g = words + cmdWords + first;
    const uint32_t *tail = words + cmdWords - 4;
    const uint32_t n = fr::kGuardWords - first;
    char detail[384];
    std::snprintf(detail, sizeof(detail),
                  "%s seq=%llu open=%llu chunk %u gpu=0x%llx: %u of %u guard words after the %u-word command area "
                  "were written, first at +%u: %08x %08x %08x %08x; last command words: %08x %08x %08x %08x; "
                  "a command writer ran past the end of its chunk",
                  where, (unsigned long long)seq, (unsigned long long)m_openSeq, FrChunkId(chunk),
                  (unsigned long long)chunk.gpu, dirty, fr::kGuardWords, cmdWords, first, g[0], n > 1 ? g[1] : 0,
                  n > 2 ? g[2] : 0, n > 3 ? g[3] : 0, tail[0], tail[1], tail[2], tail[3]);
    if (hits == 1)
    {
        Fail("CMD_GUARD", "%s", detail);
        FrDump("cmd_guard", false);
    }
    else if (hits <= 8)
    {
        Log("cmd guard hit %u: %s", hits, detail);
    }
    fr::FillGuard(const_cast<uint32_t *>(words + cmdWords));
}

void Device::SetCmdChunkBytes(uint32_t bytes)
{
    bytes = bytes ? std::min<uint32_t>(std::max<uint32_t>(AlignUp(bytes, 256), 1u << 10), 4u << 20) : kCmdChunk;
    m_cmdChunkWanted.store(bytes, std::memory_order_relaxed);
}

void Device::RecordTimestamp(bool end)
{
    if (m_timestamps.gpu)
        dkCmdBufReportCounter(Rec(), DkCounter_Timestamp,
                              m_timestamps.gpu + (m_openSeq % kFenceRing) * 32 + (end ? 16 : 0));
}

void Device::CmdBarrier(DkBarrier mode, uint32_t invalidate)
{
    ++m_cc.barrier[mode];
    m_cc.inval[0] += (invalidate & DkInvalidateFlags_L2Cache) != 0;
    m_cc.inval[1] += (invalidate & DkInvalidateFlags_Image) != 0;
    m_cc.inval[2] += (invalidate & DkInvalidateFlags_Shader) != 0;
    m_cc.inval[3] += (invalidate & DkInvalidateFlags_Descriptors) != 0;
    m_cc.inval[4] += (invalidate & DkInvalidateFlags_Zcull) != 0;
    dkCmdBufBarrier(Rec(), mode, invalidate);
}

void Device::CmdBindTargets(const DkImageView *const colors[], uint32_t count, const DkImageView *depth)
{
    ++m_cc.targetBinds;
    dkCmdBufBindRenderTargets(Rec(), colors, count, depth);
}

void Device::BeginList()
{
    if (m_faultTrace)
        FaultTraceTop(CrumbBegin(m_openSeq));
    RecordTimestamp(false);
    // Every list starts from coherent caches: CPU writes made before this
    // point (uploads, renamed buffers, recycled memory) become visible.
    CmdBarrier(DkBarrier_None,
                    DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors |
                        DkInvalidateFlags_L2Cache);
    // Per-pass timing: the pass that was running when the last list closed
    // continues here (the GPU idle time in between is not counted).
    if (GpuPassesOn())
        RecordPassMark(m_curPass);
    dkCmdBufBindImageDescriptorSet(Rec(), m_descriptorMemory.gpu, kImageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(Rec(), m_descriptorMemory.gpu + kImageDescriptors * sizeof(DkImageDescriptor),
                                     kSamplerDescriptors);
    const DkBufExtents vs{m_vsUbo.gpu, m_vsUbo.size};
    const DkBufExtents ps{m_psUbo.gpu, m_psUbo.size};
    dkCmdBufBindUniformBuffers(Rec(), DkStage_Vertex, 0, &vs, 1);
    dkCmdBufBindUniformBuffers(Rec(), DkStage_Fragment, 0, &ps, 1);
    // No hazard-clock bump here: deko3d inserts no wait between submitted
    // lists, so epochs carry across list boundaries and hazards spanning
    // lists still record real barriers.
    Stats() = {};
    Stats().seq = m_openSeq;
    Stats().acquire = Stats().present = -1;
    m_descriptorsDirty = false;
    m_dirtyTargets = m_dirtyViewport = m_dirtyRaster = true;
    m_dirtyInput = m_dirtyShaders = m_dirtyTextures = m_dirtyAttribs = true;
    m_boundVs = m_boundPs = nullptr;
    m_recorded = {};
    if (m_faultTrace)
        FaultTraceBeginList();
}

void Device::FlushProfileFrames()
{
    if (!m_profileFrameCount)
        return;
    kisakperf::Json record;
    kisakperf::Samples(record, "gpu.busy", "busy", ++m_profileBatchSeq,
                      "\"frame\",\"time\",\"value\",\"width\",\"height\",\"list_seq\"");
    for (unsigned i = 0; i < m_profileFrameCount; ++i)
    {
        const ProfileFrame &f = m_profileFrames[i];
        record.Raw("%s[%llu,%llu,%llu,%u,%u,%llu]", i ? "," : "",
                   (unsigned long long)f.frame, (unsigned long long)f.time,
                   (unsigned long long)f.busy, f.width, f.height, (unsigned long long)f.list);
    }
    // Observation time is the CPU clock at retirement, not a GPU start time.
    record.Raw("],\"tags\":{\"timing\":\"sum_of_list_busy\",\"alignment\":\"retirement_observation\",\"frame_domain\":\"gpu\"}}");
    LogLine(record.Line());
    m_profileFrameCount = 0;
}

void Device::RetireSeq(uint64_t seq)
{
    m_completedSeq = seq;
    m_completedSeqPub.store(seq, std::memory_order_release);
    // Query markers of retired lists: their queries answer from the list's
    // fence from now on (EventDone asks the list first), so the fence
    // struct may be recorded into a later list.
    while (!m_busyMarkers.empty() && m_busyMarkers.front()->seq <= seq)
    {
        m_freeMarkers.push_back(m_busyMarkers.front());
        m_busyMarkers.pop_front();
    }
    RetirePassMarks(seq);
    if (m_censusTail != m_censusHead)
        RetireCensus(seq);
    if (!m_timestamps.cpu)
        return;
    // Counter report: {u64 payload, u64 timestamp}.
    uint64_t start, end;
    const uint8_t *slot = m_timestamps.cpu + (seq % kFenceRing) * 32;
    std::memcpy(&start, slot + 8, 8);
    std::memcpy(&end, slot + 24, 8);
    if (end > start)
    {
        const uint64_t ns = dkTimestampToNs(end - start);
        m_timing.gpuNs += ns;
        m_frameAccumNs += ns;
        AddFrameExtra(DEKO9_EXTRA_GPU, ns);
    }
    ++m_timing.lists;
    // Dynamic resolution: a presented frame's GPU busy time is the sum of
    // its lists' times; publish it when its last list has retired.
    while (!m_frameEnds.empty() && m_frameEnds.front().seq <= seq)
    {
        const FrameEnd end = m_frameEnds.front();
        m_frameEnds.pop_front();
        const uint64_t count = ++m_gpuFramesPublished;
        const uint64_t us = std::min<uint64_t>(m_frameAccumNs / 1000, 0xffffffffull);
        m_frameAccumNs = 0;
        if (GpuPassesOn() && !m_profileFrameIncomplete)
        {
            m_profileFrames[m_profileFrameCount++] = {count, armTicksToNs(armGetSystemTick()) / 1000,
                                                     us, end.seq, end.width, end.height};
            if (m_profileFrameCount == 8)
                FlushProfileFrames();
        }
        m_profileFrameIncomplete = false;
        m_gpuFrameA.store(count << 32 | us, std::memory_order_relaxed);
        m_gpuFrameB.store(count << 32 | (uint64_t)(end.width & 0xffff) << 16 | (end.height & 0xffff),
                          std::memory_order_release);
    }
}

void Device::GetGpuFrame(float *gpuMs, uint32_t *width, uint32_t *height, uint32_t *count) const
{
    for (;;)
    {
        const uint64_t b = m_gpuFrameB.load(std::memory_order_acquire);
        const uint64_t a = m_gpuFrameA.load(std::memory_order_relaxed);
        const uint64_t b2 = m_gpuFrameB.load(std::memory_order_acquire);
        if (b != b2 || (a >> 32) != (b >> 32))
            continue; // a publish landed between the loads
        *gpuMs = (float)(a & 0xffffffffull) / 1000.0f;
        *width = (uint32_t)(b >> 16) & 0xffff;
        *height = (uint32_t)b & 0xffff;
        *count = (uint32_t)(b >> 32);
        return;
    }
}

// ---- per-pass GPU timing ------------------------------------------------------

void Device::RecordPassMark(uint16_t pass)
{
    if (!m_passStamps.gpu)
        return;
    if (m_passHead - m_passTail >= kPassSlots)
    {
        ++m_passDropped; // ring full: that interval goes to the previous pass
        return;
    }
    const uint32_t slot = (uint32_t)(m_passHead % kPassSlots);
    dkCmdBufReportCounter(Rec(), DkCounter_Timestamp, m_passStamps.gpu + slot * 16);
    m_passMarks[slot] = {m_openSeq, pass, m_zcullStats};
    if (m_zcullStats)
        ZcullRecordMark(slot);
    ++m_passHead;
}

void Device::GpuMarker(uint32_t pass)
{
    if (pass >= Deko9GpuPass_Count)
    {
        Fail("GPU_MARKER", "pass id %u out of range", (unsigned)pass);
        return;
    }
    if (pass == m_curPass)
        return;
    CensusBreak();
    m_curPass = (uint16_t)pass;
    // The draw census also tracks the pass while the timing is off.
    if (GpuPassesOn())
        RecordPassMark((uint16_t)pass);
}

void Device::RetirePassMarks(uint64_t seq)
{
    while (m_passTail != m_passHead)
    {
        const uint32_t slot = (uint32_t)(m_passTail % kPassSlots);
        const PassMark &mark = m_passMarks[slot];
        if (mark.seq > seq)
            break;
        // Counter report: {u64 payload, u64 timestamp}.
        uint64_t ts;
        std::memcpy(&ts, m_passStamps.cpu + slot * 16 + 8, 8);
        if (m_passPrevValid && m_passPrev != kPassClose && ts >= m_passPrevTs)
        {
            const uint64_t ns = dkTimestampToNs(ts - m_passPrevTs);
            m_passNs[m_passPrev] += ns;
            m_passNsTotal[m_passPrev] += ns;
            AddGpuPassFrameExtra(m_passPrev, ns);
        }
        if (mark.zcull)
            ZcullRetireMark(slot, mark);
        else
            m_zcullPrevValid = false;
        m_passPrevValid = true;
        m_passPrev = mark.pass;
        m_passPrevTs = ts;
        ++m_passTail;
    }
}

namespace
{
const char *const kGpuPassNames[Deko9GpuPass_Count] = {
    "other", "shadow", "floatz", "clear", "prepass", "lit", "decal", "sun",
    "lights", "resolve", "emissive", "postfx", "sunpost", "view2d", "hud2d", "present", "upscale", "hrp",
    "taau_resolve", "taau_motion", "taau_reactive",
};
// Slow-frame extra names for the GPU passes (kGpuPassNames with a gp. prefix).
const char *const kGpuPassExtraNames[Deko9GpuPass_Count] = {
    "gp.other", "gp.shadow", "gp.floatz", "gp.clear", "gp.prepass", "gp.lit", "gp.decal", "gp.sun",
    "gp.lights", "gp.resolve", "gp.emissive", "gp.postfx", "gp.sunpost", "gp.view2d", "gp.hud2d", "gp.present",
    "gp.upscale", "gp.hrp", "gp.taau_resolve", "gp.taau_motion", "gp.taau_reactive",
};
} // namespace

void AddFrameExtra(Deko9FrameExtra which, uint64_t ns)
{
    static const int slots[DEKO9_EXTRA_COUNT] = {
        SwitchPerf_RegisterFrameExtra("gpu"),
        SwitchPerf_RegisterFrameExtra("lockwait"),
        SwitchPerf_RegisterFrameExtra("compile"),
        SwitchPerf_RegisterFrameExtra("bake"),
    };
    if (SwitchPerf_g_enabled && which < DEKO9_EXTRA_COUNT)
        SwitchPerf_AddFrameExtra(slots[which], ns);
}

void AddGpuPassFrameExtra(uint32_t pass, uint64_t ns)
{
    if (!SwitchPerf_g_enabled || pass >= Deko9GpuPass_Count)
        return;
    static int slots[Deko9GpuPass_Count];
    static const bool registered = [] {
        for (uint32_t i = 0; i < Deko9GpuPass_Count; ++i)
            slots[i] = SwitchPerf_RegisterFrameExtra(kGpuPassExtraNames[i]);
        return true;
    }();
    (void)registered;
    SwitchPerf_AddFrameExtra(slots[pass], ns);
}

void Device::ReportGpuPasses()
{
    const char *const *kNames = kGpuPassNames;
    char line[768];
    int len = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < Deko9GpuPass_Count; ++i)
        total += m_passNs[i];
    const double frames = m_passFrames ? (double)m_passFrames : 1.0;
    len += std::snprintf(line + len, sizeof(line) - len, "gpupass frames=%llu total=%.2fms",
                         (unsigned long long)m_passFrames, total / 1e6 / frames);
    for (uint32_t i = 0; i < Deko9GpuPass_Count && len < (int)sizeof(line); ++i)
    {
        if (m_passNs[i])
            len += std::snprintf(line + len, sizeof(line) - len, " %s=%.2f", kNames[i], m_passNs[i] / 1e6 / frames);
    }
    if (len < (int)sizeof(line))
        std::snprintf(line + len, sizeof(line) - len, " dropped=%llu (GPU ms per frame)",
                      (unsigned long long)m_passDropped);
    Log("%s", line);
    std::memset(m_passNs, 0, sizeof(m_passNs));
    m_passDropped = 0;
    m_passFrames = 0;
}

uint64_t Device::CompletedSeq()
{
    while (m_completedSeq + 1 < m_openSeq)
    {
        const uint64_t next = m_completedSeq + 1;
        DkFence &fence = m_fences[next % kFenceRing];
        if (m_fenceSeq[next % kFenceRing] != next)
            break;
        ++m_timing.fencePolls;
        if (dkFenceWait(&fence, 0) != DkResult_Success)
            break;
        ++m_timing.fencePollsDone;
        RetireSeq(next);
    }
    return m_completedSeq;
}

void Device::SubmitOpenList()
{
    DeviceLockSite site(m_lock, "submit");
    // One compare per list; the report is out of line and one-shot.
    bool firstForeign;
    if (!m_submitOwner.Check(ThreadTag(), &firstForeign) && firstForeign)
        ReportForeignSubmit();
    // The slot's previous list must be finished before its fence is reused.
    // WaitSeq releases the device lock while the GPU runs, and a thread that
    // submits meanwhile advances m_openSeq, so the slot is derived again
    // after each wait; nothing from here to the signal releases the lock.
    const uint32_t slot = ReserveListSlot(m_fenceSeq, m_openSeq, m_completedSeq, [&](uint64_t seq) {
        WaitSeq(seq);
        return m_completedSeq >= seq;
    });
    Stats().thread = (uintptr_t)threadGetSelf();
    CensusBreak(); // a bracket never spans lists (the idle gap between them)
    if (GpuPassesOn())
        RecordPassMark(kPassClose);
    RecordTimestamp(true);
    if (m_faultTrace)
    {
        dkCmdBufReportValue(Rec(), CrumbEnd(m_openSeq), m_crumbs.gpu);
        FaultTraceTop(CrumbEnd(m_openSeq));
    }
    const DkCmdList list = dkCmdBufFinishList(Rec());
    if (m_faultTrace)
        NoteListSegments(m_openSeq, list);
    FrSubmit(list, slot);
    if (dkQueueIsInErrorState(m_queue))
    {
        // A GPU fault (details come from libdeko3dd as FAIL:DEKO9_DK lines).
        // deko3d aborts the process on a submit to a failed queue; report it
        // once and stop submitting instead.
        ReportQueueError();
        RetireCmdMemory(0);
        // The list's timestamp reports were never submitted.
        while (m_passHead != m_passTail && m_passMarks[(m_passHead - 1) % kPassSlots].seq == m_openSeq)
            --m_passHead;
        while (m_censusHead != m_censusTail && m_censusSlots[(m_censusHead - 1) % kCensusSlots].seq == m_openSeq)
            --m_censusHead;
        m_listHasWork = false;
        BeginList();
        return;
    }
    dkQueueSubmitCommands(m_queue, list);
    dkQueueSignalFence(m_queue, &m_fences[slot], true);
    dkQueueFlush(m_queue);
    ++m_cc.submits;
    ++m_cc.flushes;
    m_fenceSeq[slot] = m_openSeq;
    m_submittedSeq.store(m_openSeq, std::memory_order_release);
    {
        const ListStats &l = Stats();
        ++totals.lists;
        totals.draws += l.draws;
        totals.uploads += l.uploads;
        totals.barriers += l.barriers;
        totals.descriptors += l.descriptors;
        totals.readbacks += l.readbacks;
    }
    RetireCmdMemory(m_openSeq);
    ++m_openSeq;
    m_openSeqPub.store(m_openSeq, std::memory_order_release);
    m_listHasWork = false;
    BeginList();
}

void Device::ReportForeignSubmit()
{
    // Not fatal by itself (the unlocked waits re-derive their fence slot),
    // but it cuts the render owner's list at an arbitrary call: report it
    // with where it happened.
    Fail("SUBMIT_THREAD",
         "list seq=%llu submitted by thread 0x%llx while thread 0x%llx owns rendering (last draw thread %s); "
         "only the render owner may submit (unlocked waits in SubmitOpenList/Present); later violations are "
         "counted on the perf lock line (foreignSubmits=)",
         (unsigned long long)m_openSeq, (unsigned long long)ThreadTag(), (unsigned long long)m_submitOwner.Owner(),
         m_drawThread == threadGetSelf() ? "is this thread" : "is another thread");
}

void Device::RetireCmdMemory(uint64_t seq)
{
    // dkCmdBufClear does not detach command memory: it rewinds to the start
    // of the last chunk added, which would overwrite the list just submitted
    // while the GPU still reads it (hardware: "GPU rejected command list").
    // Retire every chunk the list used and hand the command buffer a fresh
    // one before anything else is recorded.
    for (const GpuAlloc &chunk : m_openCmdChunks)
    {
        CheckCmdGuard(chunk, "submit", seq);
        m_busyCmdChunks.push_back({chunk, seq});
        FrNoteBusy(chunk, seq);
    }
    m_openCmdChunks.clear();
    dkCmdBufClear(m_cmd);
    AddCmdMemory(this, m_cmd, 0);
}

void Device::Flush(bool force)
{
    if (m_listHasWork || force)
        SubmitOpenList();
    CollectCompleted();
}

void Device::WaitSeq(uint64_t seq)
{
    DeviceLockSite site(m_lock, "waitseq");
    if (seq >= m_openSeq)
        Flush(true);
    while (CompletedSeq() < seq)
    {
        const uint64_t next = m_completedSeq + 1;
        if (m_fenceSeq[next % kFenceRing] != next)
        {
            Fail("FENCE_WAIT", "seq=%llu was never submitted (open=%llu)", (unsigned long long)next,
                 (unsigned long long)m_openSeq);
            break;
        }
        // A copy of the fence: the slot is reused only by a SubmitOpenList
        // after this list retired, and the semaphore only moves forward
        // (same reasoning as WaitSeqFor).
        DkFence fence = m_fences[next % kFenceRing];
        // Watchdog: a GPU that stops signalling must show up in the log
        // with the wait it blocks, not as a silent hang.
        DkResult result;
        uint32_t waited = 0;
        const uint64_t waitStart = armTicksToNs(armGetSystemTick());
        for (;;)
        {
            {
                // The GPU wait runs without the device lock: with the render
                // back end on its own thread this is the section the main
                // thread contends on most. Another thread's fence poll may
                // retire lists meanwhile (handled below), and a thread that
                // does not own rendering (a readback, WaitIdle) may submit
                // the open list: callers re-derive what they read before the
                // wait (SubmitOpenList re-derives its fence slot).
                DeviceUnlockScope unlocked(m_lock);
                result = dkFenceWait(&fence, 2000000000ll);
            }
            if (result != DkResult_Timeout)
                break;
            waited += 2;
            Log("fence wait stalled %us: waiting seq=%llu for=%llu open=%llu frames=%llu queueError=%d", waited,
                (unsigned long long)next, (unsigned long long)seq, (unsigned long long)m_openSeq,
                (unsigned long long)m_frames, (int)dkQueueIsInErrorState(m_queue));
            if (waited == 2)
                DumpListStats();
        }
        if (result != DkResult_Success)
        {
            Fail("FENCE_WAIT", "seq=%llu result=%d", (unsigned long long)next, (int)result);
            break;
        }
        m_timing.fenceWaitNs += armTicksToNs(armGetSystemTick()) - waitStart;
        // Another thread's poll (CompletedSeq) may have retired it while the
        // lock was released; the loop's CompletedSeq() check covers both.
        if (m_completedSeq + 1 == next)
            RetireSeq(next);
    }
    CollectCompleted();
}

bool Device::SleepPollFence(DkFence fence, int64_t timeoutNs, uint64_t *waitedNs)
{
    // Sleep-poll, not a timed kernel fence wait: libnx/nvservices timed
    // waits on some emulators ignore the timeout once they have timed out a few
    // times ("GPU processing thread is too slow, waiting on CPU") and leave
    // later fence waits stalled for seconds. A poll of the fence is one
    // load of its semaphore; sleeping kSliceNs between
    // polls bounds both the wake latency and the poll rate (<= ~20k/s),
    // and the timeout is exact. The copy is polled without the device lock
    // (dkFenceWait(.., 0) only reads the semaphore; in the debug deko3d
    // build it also sets the global error-report context, same device).
    constexpr int64_t kSliceNs = 50000;
    const uint64_t waitStart = armTicksToNs(armGetSystemTick());
    const uint64_t deadline = waitStart + (uint64_t)std::max<int64_t>(timeoutNs, 0);
    uint64_t now = waitStart;
    bool done;
    while (!(done = dkFenceWait(&fence, 0) == DkResult_Success) && now < deadline)
    {
        svcSleepThread(std::min<int64_t>(kSliceNs, (int64_t)(deadline - now)));
        now = armTicksToNs(armGetSystemTick());
    }
    if (waitedNs)
        *waitedNs = armTicksToNs(armGetSystemTick()) - waitStart;
    return done;
}

bool Device::WaitSeqFor(uint64_t seq, int64_t timeoutNs)
{
    DkFence fence;
    {
        DeviceLockGuard lock(m_lock);
        if (SeqDone(seq))
            return true;
        const uint32_t slot = (uint32_t)(seq % kFenceRing);
        // Not submitted yet (still open), or its slot was already reused:
        // reuse waits for the older list first, so SeqDone answered that.
        if (seq >= m_openSeq || m_fenceSeq[slot] != seq)
            return false;
        // A copy: the slot may be re-signalled for a later list while this
        // thread waits unlocked. The copy keeps this list's semaphore value,
        // and the semaphore only moves forward.
        fence = m_fences[slot];
    }
    uint64_t waited = 0;
    SleepPollFence(fence, timeoutNs, &waited);
    DeviceLockGuard lock(m_lock);
    ++m_timing.queryWaits;
    m_timing.queryWaitNs += waited;
    // Retirement stays in order and on the device's own fences.
    return SeqDone(seq);
}

EventMarker *Device::RecordEventMarker()
{
    EventMarker *marker;
    if (!m_freeMarkers.empty())
    {
        marker = m_freeMarkers.back();
        m_freeMarkers.pop_back();
    }
    else
    {
        m_markerPool.push_back(std::make_unique<EventMarker>());
        marker = m_markerPool.back().get();
    }
    marker->seq = m_openSeq;
    m_busyMarkers.push_back(marker);
    // No cache flush: the queries only report completion (the occlusion
    // counters are semaphore reports that land before this release).
    dkCmdBufSignalFence(Rec(), &marker->fence, false);
    m_listHasWork = true;
    return marker;
}

bool Device::FrameDone(uint64_t frame)
{
    FoldObservedFrames();
    switch (m_frameRing.Query(frame))
    {
    case FrameRing<kFramesInFlight>::State::Done:
        return true;
    case FrameRing<kFramesInFlight>::State::NotPresented:
        return false;
    case FrameRing<kFramesInFlight>::State::Pending:
        break;
    }
    const uint32_t slot = FrameRing<kFramesInFlight>::Slot(frame);
    // The cached list sequence answers without a fence poll.
    if (m_completedSeq >= m_frameRing.SlotEndSeq(slot))
    {
        NoteFrameDone(frame);
        return true;
    }
    DkFence fence = m_frameFences[slot];
    if (dkFenceWait(&fence, 0) != DkResult_Success)
        return false;
    NoteFrameDone(frame);
    // Every list of the frame passed: retire them (resources, timings).
    CompletedSeq();
    return true;
}

bool Device::FrameDoneAnyThread(uint64_t frame)
{
    // No device lock: the engine's end-fence and GPU-sync polls run on the
    // main thread while the render back end holds the lock for whole draw
    // lists. The fence is a published copy (the slot is re-signalled only
    // after this frame passed); polling it only reads its semaphore.
    m_lockFree.framePolls.fetch_add(1, std::memory_order_relaxed);
    return PublishedFrameDone(m_framePub, PublishedCompletedSeq(), frame,
                              [](DkFence fence) { return dkFenceWait(&fence, 0) == DkResult_Success; });
}

bool Device::WaitFrameFor(uint64_t frame, int64_t timeoutNs)
{
    m_lockFree.framePolls.fetch_add(1, std::memory_order_relaxed);
    return PublishedFrameWait(
        m_framePub, PublishedCompletedSeq(), frame, m_lock.OwnedByCaller(),
        [](DkFence fence) { return dkFenceWait(&fence, 0) == DkResult_Success; },
        [&](DkFence fence) {
            uint64_t waited = 0;
            const bool done = SleepPollFence(fence, timeoutNs, &waited);
            m_lockFree.frameWaits.fetch_add(1, std::memory_order_relaxed);
            m_lockFree.frameWaitNs.fetch_add(waited, std::memory_order_relaxed);
            return done;
        });
}

bool Device::RawSeqPassed(uint64_t seq)
{
    if (!seq)
        return true;
    if (seq >= m_openSeq)
        return false; // not submitted
    const uint32_t slot = (uint32_t)(seq % kFenceRing);
    // A reused slot means a later list was submitted after this one was
    // waited for (SubmitOpenList): it passed.
    if (m_fenceSeq[slot] != seq)
        return m_fenceSeq[slot] > seq;
    DkFence fence = m_fences[slot];
    return dkFenceWait(&fence, 0) == DkResult_Success;
}

void Device::WaitIdle()
{
    Flush(true);
    WaitSeq(m_openSeq - 1);
    dkQueueWaitIdle(m_queue);
}

// ---- hazards ----------------------------------------------------------------

void Device::DumpListStats()
{
    for (uint32_t i = 0; i < kStatsRing; ++i)
    {
        const ListStats &l = m_listStats[(m_openSeq + 1 + i) % kStatsRing];
        if (!l.seq)
            continue;
        Log("list seq=%llu thread=%llx draws=%u clears=%u uploads=%u readbacks=%u blits=%u barriers=%u queries=%u "
            "descriptors=%u acquire=%d present=%d",
            (unsigned long long)l.seq, (unsigned long long)l.thread, l.draws, l.clears, l.uploads, l.readbacks, l.blits, l.barriers,
            l.queries, l.descriptors, l.acquire, l.present);
    }
}

void Device::ReportQueueError()
{
    static bool s_reported;
    if (s_reported)
        return;
    s_reported = true;
    Fail("QUEUE_ERROR", "GPU channel faulted; completed=%llu open=%llu frames=%llu",
         (unsigned long long)m_completedSeq, (unsigned long long)m_openSeq, (unsigned long long)m_frames);
    const ListStats &l = Stats();
    char line[256];
    std::snprintf(line, sizeof(line),
                  "CRASH:GPU_QUEUE_ERROR frame=%llu seq=%llu draws=%u clears=%u uploads=%u readbacks=%u blits=%u "
                  "barriers=%u queries=%u descriptors=%u acquire=%d present=%d",
                  (unsigned long long)m_frames, (unsigned long long)l.seq, l.draws, l.clears, l.uploads,
                  l.readbacks, l.blits, l.barriers, l.queries, l.descriptors, l.acquire, l.present);
    LogLine(line);
    DumpListStats();
    BlackBoxDump("queue_error");
}

void Device::Barrier()
{
    ++Stats().barriers;
    // Every hazard takes the full barrier plus texture/L2 invalidate. Lighter
    // 3D-only barriers (Primitives/Fragments) were measured to be of no
    // value on hardware and once stalled the queue on some emulators.
    CmdBarrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    ++m_writeClock;
    // The static-store readEpoch re-stamp (a bound store OLD's tracker
    // would have re-added, and so re-stamped, every draw) lives in
    // PrepareDraw right after its HazardCommit, not here: Barrier() also
    // fires from copy/blit-only commits (BeforeCopyWrite, StretchRect) and
    // bare calls (ReadImage) that never touched these stores in the old
    // per-draw tracker either, so restamping them here would record a
    // barrier the old tracker would not have (proven by the host hazard-model trace:
    // a barrier with nothing sampled around it must not touch readEpoch).
    m_listHasWork = true;
}

bool Device::StaticStoreBound(const ImageStore *store) const
{
    for (uint32_t st = 0; st < 2; ++st)
        for (uint32_t s = 0; s < DEKO9_MAX_SAMPLERS; ++s)
            if (m_texSlot[st][s].valid && m_texSlot[st][s].store == store)
                return true;
    return false;
}

void Device::SplitLongList()
{
    constexpr uint32_t kMaxBarriersPerList = 256;
    // Only the draw thread submits (Query::GetData has the same rule): the
    // present's swapchain acquire runs without the device lock, and the
    // queue must not be used from two threads at once. A main-thread upload
    // that crosses the bound leaves the split to the draw thread's next
    // draw or present.
    if (Stats().barriers >= kMaxBarriersPerList && IsRecordingThread())
        SubmitOpenList();
}

void Device::HazardAdd(ImageStore *store, Access access)
{
    if (!store)
        return;
    if (m_hazardCount == kMaxHazards)
    {
        // Never expected (samplers + targets + depth fit); stay correct.
        HazardCommit();
        m_hazardCount = 0;
    }
    m_hazards[m_hazardCount++] = {store, access};
}

// The access check (the host hazard model runs the same function).
using deko9_hazard_model::HazardCheck;

bool Device::HazardRolesDisjoint() const
{
    for (uint32_t i = 0; i < m_hazardCount; ++i)
    {
        if (m_hazards[i].access != Access::Render)
            continue;
        for (uint32_t j = 0; j < m_hazardCount; ++j)
        {
            if (j != i && m_hazards[j].store == m_hazards[i].store && m_hazards[j].access != Access::Render)
                return false;
        }
    }
    return true;
}

bool Device::HazardWouldAct() const
{
    const uint64_t c = m_writeClock;
    for (uint32_t i = 0; i < m_hazardCount; ++i)
    {
        const ImageStore &s = *m_hazards[i].store;
        bool hit, engine;
        HazardCheck(s, m_hazards[i].access, c, &hit, &engine);
        if (hit)
            return true;
        switch (m_hazards[i].access)
        {
        case Access::Sample:
            if (s.readEpoch != c)
                return true;
            break;
        case Access::Render:
            if (s.renderEpoch != c)
                return true;
            break;
        default:
            return true; // draws only sample and render
        }
    }
    return false;
}

void Device::HazardCommit()
{
    ++m_hazardSerial; // any evaluation invalidates the previous draw's (DEKO9_PERDRAW_HAZARD)
    const uint64_t c = m_writeClock;
    // Every access is checked (not just up to the first conflict): each
    // write-after-read is counted.
    bool conflict = false;
    for (uint32_t i = 0; i < m_hazardCount; ++i)
    {
        bool hit, engine;
        HazardCheck(*m_hazards[i].store, m_hazards[i].access, c, &hit, &engine);
        if (hit)
        {
            conflict = true;
            const Access a = m_hazards[i].access;
            if ((a == Access::CopyWrite || a == Access::BlitWrite) && m_hazards[i].store->readEpoch == c)
                ++m_writeAfterReadBarriers;
        }
    }
    if (conflict)
        Barrier();
    // Every access of this operation runs after that barrier: stamp all with
    // the final epoch.
    const uint64_t now = m_writeClock;
    for (uint32_t i = 0; i < m_hazardCount; ++i)
    {
        ImageStore &s = *m_hazards[i].store;
        switch (m_hazards[i].access)
        {
        case Access::Sample: s.readEpoch = now; break;
        case Access::CopyRead: s.readEpoch = s.copyReadEpoch = now; break;
        case Access::Render: s.renderEpoch = now; break;
        case Access::CopyWrite: s.copyWriteEpoch = now; break;
        case Access::BlitWrite: s.blitEpoch = now; break;
        }
    }
    m_hazardCount = 0;
}

// ---- presentation -----------------------------------------------------------

// DEKO9 perf waiters: device-lock acquisitions by threads other than the
// draw thread, per call site, over the last 60 frames (per frame: acquired,
// of those blocked, time blocked), and the lock-free paths that replaced
// the frame-loop ones. Steady state should show otherAcquires=0.00.
void Device::ReportLockWaiters()
{
    char callers[640];
    int off = 0;
    const DeviceLock::CallerStats *cs = m_lock.Callers();
    for (uint32_t i = 0; i < DeviceLock::kCallerSlots && cs[i].site; ++i)
    {
        DeviceLock::CallerStats &prev = m_callersReported[i];
        const uint64_t acq = cs[i].acquisitions - prev.acquisitions;
        const uint64_t blocked = cs[i].contended - prev.contended;
        const uint64_t ns = cs[i].ns - prev.ns;
        prev = cs[i];
        if (!acq || off >= (int)sizeof(callers) - 1)
            continue;
        off += std::snprintf(callers + off, sizeof(callers) - (size_t)off, " %s=%.2f/%.2f/%.3fms", cs[i].site,
                             acq / 60.0, blocked / 60.0, ns / 60e6);
    }
    const uint64_t other = m_lock.OtherAcquisitions();
    const uint64_t now[8] = {
        m_lockFree.framePolls.load(std::memory_order_relaxed), m_lockFree.bufLocks.load(std::memory_order_relaxed),
        m_lockFree.bufRenames.load(std::memory_order_relaxed), m_lockFree.bufGrows.load(std::memory_order_relaxed),
        m_lockFree.bufEvicts.load(std::memory_order_relaxed), m_lockFree.arenaAllocs.load(std::memory_order_relaxed),
        m_lockFree.lockedDraws.load(std::memory_order_relaxed), 0};
    double d[8];
    for (int i = 0; i < 8; ++i)
    {
        d[i] = (now[i] - m_lockFreeReported[i]) / 60.0;
        m_lockFreeReported[i] = now[i];
    }
    Log("perf waiters otherAcquires=%.2f lfFramePolls=%.1f lfBufLocks=%.1f bufRenames=%.2f bufGrows=%.2f "
        "bufEvicts=%.2f arenaAllocs=%.1f lockedDraws=%.2f (per frame) callers=%s",
        (other - m_otherAcquisitionsReported) / 60.0, d[0], d[1], d[2], d[3], d[4], d[5], d[6], off ? callers : " none");
    m_otherAcquisitionsReported = other;
}

void Device::PresentFrame()
{
    DeviceLockSite site(m_lock, "present");
    {
        // Device-lock wait since the last present, for the slow-frame line
        // (owned here, so the wait totals are stable).
        const uint64_t waitNs = m_lock.WaitNs(false) + m_lock.WaitNs(true);
        if (waitNs > m_lockWaitNsExtra)
            AddFrameExtra(DEKO9_EXTRA_LOCKWAIT, waitNs - m_lockWaitNsExtra);
        m_lockWaitNsExtra = waitNs;
    }
    // Cheap once-per-present check: SubmitOpenList only sees a faulted queue
    // on its own next submit, so a fault between submits (nothing more
    // queued) would otherwise go unreported.
    if (dkQueueIsInErrorState(m_queue))
        ReportQueueError();
    // One image sentinel per present (round robin, sampled): a GPU write past
    // an image memblock's end lands there instead of in whatever follows it.
    {
        char report[512];
        if (m_heaps[POOL_IMAGE]->CheckSentinel(m_sentinelCursor++, report, sizeof(report)))
        {
            if (m_imageOverruns.fetch_add(1, std::memory_order_relaxed) == 0)
            {
                Fail("IMAGE_OVERRUN", "frame=%llu %s", (unsigned long long)m_frames, report);
                FrDump("image_overrun", false);
            }
            else
            {
                Log("image overrun frame=%llu %s", (unsigned long long)m_frames, report);
            }
        }
    }
    const uint64_t acquireStart = armTicksToNs(armGetSystemTick());
    if (m_lastPresentNs)
        m_timing.periodNs += acquireStart - m_lastPresentNs;
    m_lastPresentNs = acquireStart;
    int slot = -1;
    DkFence acquire{};
    {
        // The acquire blocks until the display frees an image (vsync, or the
        // GPU when it is behind). With the render back end on its own thread
        // (r_smp_backend 1) every main-thread device call would queue behind
        // it once per frame, so the acquire runs unlocked. The calls that
        // run meanwhile (buffer locks, uploads recorded into the open list,
        // fence polls) are what the inline back end interleaves with a
        // present anyway; a submit by another thread (a readback) lands
        // before the present list on the same queue, and nothing cached
        // above is reused after the wait.
        DeviceUnlockScope unlocked(m_lock);
        dkSwapchainAcquireImage(m_swapchain, &slot, &acquire);
    }
    // dkQueueAcquireImage split in two: the queue waits for the display's
    // release fence before the present list, and the black box keeps the
    // fence to tell a compositor wait from a GPU stall.
    dkQueueWaitFence(m_queue, &acquire);
    {
        std::lock_guard<std::mutex> guard(m_acquireMutex);
        m_acquireFence = acquire;
        m_acquireSeq = m_openSeq;
        m_acquireSlot = slot;
    }
    m_timing.acquireNs += armTicksToNs(armGetSystemTick()) - acquireStart;
    Stats().acquire = slot;
    if (slot < 0 || slot >= 3)
    {
        Fail("ACQUIRE", "slot=%d", slot);
        return;
    }
    if (GpuPassesOn())
        GpuMarker(Deko9GpuPass_Present);
    PrepareGammaPass();
    if (m_fsr.active)
    {
        RecordUpscale(slot);
    }
    else
    {
        ImageStore *back = m_backBuffer->Store().get();
        BeforeCopyRead(back);
        DkImageView src, dst;
        m_backBuffer->MakeView(&src);
        dkImageViewDefaults(&dst, &m_swapImages[slot]);
        const DkImageRect rect{0, 0, 0, m_params.BackBufferWidth, m_params.BackBufferHeight, 1};
        dkCmdBufBlitImage(Rec(), &src, &rect, &dst, &rect, DkBlitFlag_FilterNearest, 0);
        m_listHasWork = true;
        ++Stats().blits;
    }
    Stats().present = slot;
    m_curPass = Deko9GpuPass_Other; // the next frame starts unattributed
    SubmitOpenList();
    dkQueuePresentImage(m_queue, m_swapchain, slot);
    m_lastPresentSlot = slot;
    {
        // No engine tag: the frame rendered at the back buffer's size.
        const uint32_t tagW = m_frameTagWidth ? m_frameTagWidth : m_params.BackBufferWidth;
        const uint32_t tagH = m_frameTagHeight ? m_frameTagHeight : m_params.BackBufferHeight;
        m_frameEnds.push_back({m_openSeq - 1, tagW, tagH});
        while (m_frameEnds.size() > 16)
        {
            m_frameEnds.pop_front();
            m_profileFrameIncomplete = true;
            if (GpuPassesOn())
                LogLine("KPERF {\"v\":1,\"type\":\"event\",\"stream\":\"gpu.busy\",\"clock\":\"source_line\",\"name\":\"frame_boundary_lost\",\"count\":1,\"message\":\"Dropped GPU frame boundary; next combined busy sample omitted\"}");
        }
    }
    ApplyZcullSettings();
    ApplyFaultTraceSettings();
    FrApplySettings();
    {
        // The open list keeps its chunk; the next chunk it or a later list
        // takes has the new size.
        const uint32_t chunkBytes = m_cmdChunkWanted.load(std::memory_order_relaxed);
        if (chunkBytes != m_cmdChunkBytes)
        {
            Log("cmdchunk bytes=%u (was %u)", chunkBytes, m_cmdChunkBytes);
            m_cmdChunkBytes = chunkBytes;
        }
    }
    ++m_frames;
    m_timing.maxListsInFlight = std::max<uint64_t>(m_timing.maxListsInFlight, m_openSeq - 1 - m_completedSeq);
    // Native frame ring (deko9_framepace.h): frame F signals fence slot
    // F % kFramesInFlight after its present list; before that slot is
    // signalled again, frame F - kFramesInFlight must have passed, so at most
    // kFramesInFlight frames are queued and nothing a frame owns is reused
    // before its fence. The wait is on a copy of the fence without the
    // device lock (SleepPollFence), for the same reason as the acquire
    // above; the watchdog matches WaitSeq's.
    {
        FoldObservedFrames();
        const uint64_t frame = m_frameRing.Recording();
        const uint32_t frameSlot = FrameRing<kFramesInFlight>::Slot(frame);
        const uint64_t prev = FrameRing<kFramesInFlight>::ReuseFrame(frame);
        if (prev && !FrameDone(prev))
        {
            const uint64_t waitStart = armTicksToNs(armGetSystemTick());
            uint32_t waited = 0;
            for (;;)
            {
                const DkFence fence = m_frameFences[frameSlot];
                bool done;
                {
                    DeviceUnlockScope unlocked(m_lock);
                    done = SleepPollFence(fence, 2000000000ll, nullptr);
                }
                if (done || FrameDone(prev))
                    break;
                waited += 2;
                Log("present wait stalled %us: waiting frame=%llu open=%llu frames=%llu queueError=%d", waited,
                    (unsigned long long)prev, (unsigned long long)m_openSeq, (unsigned long long)m_frames,
                    (int)dkQueueIsInErrorState(m_queue));
                if (waited == 2)
                    DumpListStats();
                if (waited >= 20)
                {
                    Fail("FENCE_WAIT", "present frame=%llu not signalled after %us", (unsigned long long)prev, waited);
                    break;
                }
            }
            NoteFrameDone(prev);
            m_timing.fenceWaitNs += armTicksToNs(armGetSystemTick()) - waitStart;
        }
        const bool presented = m_frameRing.Present(m_openSeq - 1);
        if (!presented)
            Fail("FRAME_RING", "frame=%llu would reuse slot %u before frame %llu passed", (unsigned long long)frame,
                 frameSlot, (unsigned long long)prev);
        dkQueueSignalFence(m_queue, &m_frameFences[frameSlot], true);
        // Threads without the lock see the frame only from here on, with
        // the fence just signalled into its slot.
        if (presented)
            m_framePub.PublishPresent(frame, m_openSeq - 1, m_frameFences[frameSlot]);
        dkQueueFlush(m_queue);
        ++m_cc.flushes;
        m_timing.maxFramesInFlight = std::max<uint64_t>(m_timing.maxFramesInFlight, m_frameRing.InFlight());
    }
    CollectCompleted();
    if (m_censusMode)
        ++m_drawCensusFrames;
    if (GpuPassesOn())
    {
        ++m_passFramesTotal;
        if (++m_passFrames == 60)
        {
            FlushProfileFrames();
            ReportGpuPasses();
        }
    }
    const bool wantPasses = m_gpuPassesWanted.load(std::memory_order_relaxed);
    if (wantPasses != GpuPassesOn())
    {
        FlushProfileFrames();
        m_gpuPasses.store(wantPasses, std::memory_order_relaxed);
        std::memset(m_passNs, 0, sizeof(m_passNs));
        m_passDropped = m_passFrames = 0;
        m_passPrevValid = false;
        m_curPass = Deko9GpuPass_Other;
        if (wantPasses)
            RecordPassMark(Deko9GpuPass_Other);
        Log("gpupass timing %s", wantPasses ? "on" : "off");
    }
    if (CensusOn())
    {
        if (++m_censusFrames == 60)
            ReportCensus();
    }
    const bool wantCensus = m_censusWanted.load(std::memory_order_relaxed);
    if (wantCensus != CensusOn())
    {
        m_census.store(wantCensus, std::memory_order_relaxed);
        for (CensusEntry &e : m_censusEntries)
            e = {};
        m_censusTextures.clear();
        m_censusBuffers.clear();
        m_censusFrames = 0;
        Log("call census %s", wantCensus ? "on" : "off");
    }
    if (!(m_frames % 60))
    {
        // Native frame waits run without the lock (WaitFrameFor).
        m_timing.frameWaits += m_lockFree.frameWaits.exchange(0, std::memory_order_relaxed);
        m_timing.frameWaitNs += m_lockFree.frameWaitNs.exchange(0, std::memory_order_relaxed);
        // render= is the latest frame's scene size (the engine's dynamic
        // resolution tag, else the back buffer): run digests group by it.
        // Shader build totals for the 60-frame window (not per frame).
        char shaderStats[400];
        ShaderBuildStats::Format(m_shaderStats.Take(), shaderStats, sizeof(shaderStats));
        Log("perf frames=60 period=%.1fms gpu=%.1fms drawCpu=%.1fms drawNs/draw=%.0f fenceWait=%.1fms acquire=%.1fms "
            "draws=%llu lists=%llu (per frame) maxInFlight=%llu maxFramesInFlight=%llu frameWaits=%.1f frameWait=%.2fms "
            "render=%ux%u "
            "resizes=%llu moves=%llu%s",
            m_timing.periodNs / 60e6, m_timing.gpuNs / 60e6, m_timing.drawCpuNs / 60e6,
            m_timing.draws ? (double)m_timing.drawCpuNs / m_timing.draws : 0.0, m_timing.fenceWaitNs / 60e6,
            m_timing.acquireNs / 60e6, (unsigned long long)(m_timing.draws / 60),
            (unsigned long long)(m_timing.lists / 60), (unsigned long long)m_timing.maxListsInFlight,
            (unsigned long long)m_timing.maxFramesInFlight, m_timing.frameWaits / 60.0, m_timing.frameWaitNs / 60e6,
            m_frameTagWidth ? m_frameTagWidth : m_params.BackBufferWidth,
            m_frameTagHeight ? m_frameTagHeight : m_params.BackBufferHeight, (unsigned long long)m_resizes,
            (unsigned long long)m_moves, shaderStats);
        {
            const CmdCensus &c = m_cc;
            Log("perf cmds barriers(none/tiles/frag/prim/full)=%.1f/%.1f/%.1f/%.1f/%.1f "
                "inval(l2/image/shader/desc/zcull)=%.1f/%.1f/%.1f/%.1f/%.1f targetBinds=%.1f clears=%.1f "
                "submits=%.1f flushes=%.1f (per frame)",
                c.barrier[0] / 60.0, c.barrier[1] / 60.0, c.barrier[2] / 60.0,
                c.barrier[3] / 60.0, c.barrier[4] / 60.0, c.inval[0] / 60.0, c.inval[1] / 60.0, c.inval[2] / 60.0,
                c.inval[3] / 60.0, c.inval[4] / 60.0, c.targetBinds / 60.0, c.clears / 60.0,
                c.submits / 60.0, c.flushes / 60.0);
            m_cc = {};
        }
        // Fast-path work per frame: lock entries vs real mutex acquisitions
        // (the rest were inline re-entries), texture binds, sampler state
        // sets and how their descriptors resolved.
        const uint64_t acquisitions = m_lock.Acquisitions(), entries = m_lock.Entries();
        Log("perf work lockEntries=%.0f lockAcquires=%.0f texBinds=%.0f samplerSets=%.0f samplerCompact=%.0f "
            "samplerFull=%.0f shaderApplies=%.0f attribApplies=%.0f streamApplies=%.0f rasterApplies=%.0f "
            "indexBinds=%.0f gatedDraws=%.0f bakedHits=%.0f bakedMisses=%.0f bakedReplays=%.0f bakedWords=%.0f "
            "instancedDraws=%.0f instances=%.0f instanceFallbacks=%.0f bakedUnits=%u/%u bufferBinds=%.0f "
            "earlyZCand=%.0f earlyZ=%.0f hazardSkips=%.0f texSlotsResolved=%.0f staticSamples=%.0f "
            "staticHazardChecks=%.0f perDraw=0x%x (per frame)",
            (entries - m_lockEntriesReported) / 60.0, (acquisitions - m_lockAcquisitionsReported) / 60.0,
            m_timing.textureBinds / 60.0, m_timing.samplerSets / 60.0, m_timing.samplerCompact / 60.0,
            m_timing.samplerFull / 60.0, m_timing.shaderApplies / 60.0, m_timing.attribApplies / 60.0,
            m_timing.streamApplies / 60.0, m_timing.rasterApplies / 60.0, m_timing.indexBinds / 60.0,
            m_timing.gatedDraws / 60.0, m_timing.bakedHits / 60.0, m_timing.bakedMisses / 60.0,
            m_timing.bakedReplays / 60.0, m_timing.bakedWords / 60.0, m_timing.instancedDraws / 60.0,
            m_timing.instances / 60.0, m_timing.instanceFallbacks / 60.0, m_rasterUnits.Size(), m_programUnits.Size(),
            m_timing.bufferBinds / 60.0, m_timing.earlyZCandidates / 60.0, m_timing.earlyZDraws / 60.0,
            m_timing.hazardSkips / 60.0, m_timing.texSlotsResolved / 60.0, m_timing.staticSamples / 60.0,
            m_timing.staticHazardChecks / 60.0, (unsigned)m_perDraw);
        const double draws = m_timing.draws ? (double)m_timing.draws : 1.0;
        Log("perf consts vsBytes/draw=%.0f psBytes/draw=%.0f vsPushes/draw=%.2f psPushes/draw=%.2f "
            "regsSet=%.0f regsChanged=%.0f vsBytesFrame=%.0f psBytesFrame=%.0f "
            "(per frame unless /draw)",
            m_timing.constBytes[0] / draws, m_timing.constBytes[1] / draws, m_timing.constPushes[0] / draws,
            m_timing.constPushes[1] / draws, m_timing.constRegsSet / 60.0, m_timing.constRegsChanged / 60.0,
            m_timing.constBytes[0] / 60.0, m_timing.constBytes[1] / 60.0);
        Log("perf sync fencePolls=%.1f fencePollsDone=%.1f seqCacheHits=%.1f collects=%.1f queryGetData=%.1f "
            "queryPending=%.1f bufferLockPolls=%.1f queryWaits=%.1f queryWait=%.2fms (per frame)",
            m_timing.fencePolls / 60.0, m_timing.fencePollsDone / 60.0, m_timing.seqCacheHits / 60.0,
            m_timing.collects / 60.0, m_timing.queryGetData / 60.0, m_timing.queryPending / 60.0,
            m_timing.bufferLockPolls / 60.0, m_timing.queryWaits / 60.0, m_timing.queryWaitNs / 60e6);
        if (m_timing.rangeCalls)
            Log("perf ranges calls=%.1f draws=%.1f (per frame)", m_timing.rangeCalls / 60.0,
                m_timing.rangeDraws / 60.0);
        if (m_verify || m_verifiedDraws)
            Log("perf verify draws=%llu mismatches=%llu hazardSkips=%llu texIncremental=%llu (cumulative)",
                (unsigned long long)m_verifiedDraws, (unsigned long long)m_verifyMismatches,
                (unsigned long long)m_verifiedHazardSkips, (unsigned long long)m_verifiedTexIncremental);
        m_lockEntriesReported = entries;
        m_lockAcquisitionsReported = acquisitions;
        // Device-lock contention per frame: the draw thread (the render back
        // end with r_smp_backend 1) vs every other thread (main's creates,
        // buffer locks and image uploads). otherWait is time the main thread
        // lost behind the back end's batch lock; drawWait the reverse.
        {
            const uint64_t c[2] = {m_lock.Contended(false), m_lock.Contended(true)};
            const uint64_t w[2] = {m_lock.WaitNs(false), m_lock.WaitNs(true)};
            const uint64_t handoffs = m_lock.HandOffs();
            // sites=: wait time per section the holder was in (cumulative
            // ms since start; the waits above are per frame).
            char sites[256];
            int off = 0;
            const DeviceLock::SiteWait *sw = m_lock.SiteWaits();
            for (uint32_t i = 0; i < DeviceLock::kSiteSlots && sw[i].site && off < (int)sizeof(sites) - 1; ++i)
                off += std::snprintf(sites + off, sizeof(sites) - (size_t)off, " %s=%llu/%.0fms", sw[i].site,
                                     (unsigned long long)sw[i].count, sw[i].ns / 1e6);
            // submitClaims/foreignSubmits: cumulative render-owner changes
            // and lists submitted by a non-owner (SubmitOwner).
            Log("perf lock otherContended=%.2f otherWait=%.3fms drawContended=%.2f drawWait=%.3fms handoffs=%.2f "
                "(per frame) submitClaims=%llu foreignSubmits=%llu sites=%s",
                (c[0] - m_lockContendedReported[0]) / 60.0, (w[0] - m_lockWaitNsReported[0]) / 60e6,
                (c[1] - m_lockContendedReported[1]) / 60.0, (w[1] - m_lockWaitNsReported[1]) / 60e6,
                (handoffs - m_lockHandoffsReported) / 60.0, (unsigned long long)m_submitOwner.Claims(),
                (unsigned long long)m_submitOwner.Violations(), off ? sites : " none");
            m_lockHandoffsReported = handoffs;
            for (int i = 0; i < 2; ++i)
            {
                m_lockContendedReported[i] = c[i];
                m_lockWaitNsReported[i] = w[i];
            }
        }
        ReportLockWaiters();
        m_drawsTotal += m_timing.draws;
        m_drawCpuNsTotal += m_timing.drawCpuNs;
        m_timing = {};
    }
    if (!(m_frames % 600) && m_verifiedDraws)
    {
        // Executable verdict of the fast-path shadow comparison.
        char line[224];
        std::snprintf(line, sizeof(line),
                      "%s:DEKO9_FASTPATH_VERIFY draws=%llu mismatches=%llu hazardSkips=%llu texIncremental=%llu "
                      "perDraw=0x%x",
                      m_verifyMismatches ? "FAIL" : "PASS", (unsigned long long)m_verifiedDraws,
                      (unsigned long long)m_verifyMismatches, (unsigned long long)m_verifiedHazardSkips,
                      (unsigned long long)m_verifiedTexIncremental, (unsigned)m_perDraw);
        Log("%s", line);
    }
    if (!(m_frames % 600))
    {
        Log("totals frames=%llu lists=%llu draws=%llu uploads=%llu uploadKB=%llu barriers=%llu descriptors=%llu "
            "readbacks=%llu completed=%llu open=%llu",
            (unsigned long long)m_frames, (unsigned long long)totals.lists, (unsigned long long)totals.draws,
            (unsigned long long)totals.uploads, (unsigned long long)(totals.uploadBytes >> 10),
            (unsigned long long)totals.barriers, (unsigned long long)totals.descriptors,
            (unsigned long long)totals.readbacks, (unsigned long long)m_completedSeq, (unsigned long long)m_openSeq);
        totals = {};
    }
    if (m_frames == 1 || !(m_frames % 600))
        Log("frame=%llu seq=%llu image=%lluMB buffer=%lluMB dynamic=%lluMB code=%lluKB samplers=%zu",
            (unsigned long long)m_frames, (unsigned long long)m_openSeq,
            (unsigned long long)(m_heaps[POOL_IMAGE]->BytesInUse() >> 20),
            (unsigned long long)(m_heaps[POOL_BUFFER]->BytesInUse() >> 20),
            (unsigned long long)(m_heaps[POOL_DYNAMIC]->BytesInUse() >> 20),
            (unsigned long long)(m_heaps[POOL_CODE]->BytesInUse() >> 10), m_samplers.size());
}

HRESULT Device::Present(const RECT *src, const RECT *dst, HWND window, const RGNDATA *dirty)
{
    CensusScope census(this, Census_Present);
    DeviceLockGuard lock(m_lock);
    if (src || dst || (window && window != m_window) || dirty)
        return Fail("PRESENT", "partial or redirected present is unsupported");
    PresentFrame();
    return D3D_OK;
}

// ---- simple device queries ----------------------------------------------------

UINT Device::GetAvailableTextureMem()
{
    return 1024u << 20;
}

HRESULT Device::GetDirect3D(IDirect3D9 **d3d)
{
    if (!d3d)
        return D3DERR_INVALIDCALL;
    m_d3d->AddRef();
    *d3d = m_d3d;
    return D3D_OK;
}

HRESULT Device::GetDeviceCaps(D3DCAPS9 *caps)
{
    if (!caps)
        return D3DERR_INVALIDCALL;
    FillCaps(caps);
    return D3D_OK;
}

HRESULT Device::GetDisplayMode(UINT swapChain, D3DDISPLAYMODE *mode)
{
    if (swapChain || !mode)
        return D3DERR_INVALIDCALL;
    *mode = {kDisplayWidth, kDisplayHeight, 60, D3DFMT_X8R8G8B8};
    return D3D_OK;
}

HRESULT Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *params)
{
    if (!params)
        return D3DERR_INVALIDCALL;
    params->AdapterOrdinal = 0;
    params->DeviceType = D3DDEVTYPE_HAL;
    params->hFocusWindow = m_window;
    params->BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
    return D3D_OK;
}

HRESULT Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **)
{
    return Fail("UNSUPPORTED", "CreateAdditionalSwapChain: Horizon has one display");
}

HRESULT Device::GetSwapChain(UINT index, IDirect3DSwapChain9 **swapChain)
{
    if (index || !swapChain)
        return D3DERR_INVALIDCALL;
    m_swapChainObject->AddRef();
    *swapChain = m_swapChainObject;
    return D3D_OK;
}

HRESULT Device::Reset(D3DPRESENT_PARAMETERS *params)
{
    DeviceLockGuard lock(m_lock);
    if (!params)
        return D3DERR_INVALIDCALL;
    const UINT width = params->BackBufferWidth ? params->BackBufferWidth : m_params.BackBufferWidth;
    const UINT height = params->BackBufferHeight ? params->BackBufferHeight : m_params.BackBufferHeight;
    if (width != m_params.BackBufferWidth || height != m_params.BackBufferHeight ||
        params->MultiSampleType != D3DMULTISAMPLE_NONE)
        return Fail("RESET", "back buffer change %ux%u ms=%d is unsupported", width, height,
                    (int)params->MultiSampleType);
    WaitIdle();
    m_presentInterval = params->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? 0 : 1;
    dkSwapchainSetSwapInterval(m_swapchain, m_presentInterval);
    ResetState();
    return D3D_OK;
}

HRESULT Device::GetBackBuffer(UINT swapChain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface)
{
    if (swapChain || index || !surface)
        return D3DERR_INVALIDCALL;
    m_backBuffer->AddRef();
    *surface = m_backBuffer;
    return D3D_OK;
}

HRESULT Device::GetRasterStatus(UINT, D3DRASTER_STATUS *status)
{
    if (!status)
        return D3DERR_INVALIDCALL;
    status->InVBlank = FALSE;
    status->ScanLine = 0;
    return D3D_OK;
}

void Device::SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *ramp)
{
    // Horizon's compositor has no gamma ramp: the present pass applies it
    // (deko9_fsr.h GammaVariant). An identity ramp turns the pass off.
    if (!ramp)
        return;
    DeviceLockGuard lock(m_lock);
    m_gamma.on = !GammaRampIsIdentity(ramp->red, ramp->green, ramp->blue);
    if (!m_gamma.on)
        return;
    // A power curve (the engine's R_CalcGammaRamp) runs as pow(c, e) in
    // the pass; any other ramp as a table lookup.
    const float exponent = GammaFitExponent(ramp->red, ramp->green, ramp->blue);
    if (exponent != m_gamma.exponent)
    {
        if (exponent != 0.0f)
            Log("gamma ramp: power curve, exponent %.5f (r_gamma %.4f)", exponent, 1.0f / exponent);
        else
            Log("gamma ramp: no single power curve, table lookup");
        m_gamma.exponent = exponent;
    }
    if (exponent != 0.0f)
        GammaCurveSetup(&m_gamma.curve, exponent);
    else
        GammaSetup(&m_gamma.lut, ramp->red, ramp->green, ramp->blue);
}

void Device::GetGammaRamp(UINT, D3DGAMMARAMP *ramp)
{
    if (!ramp)
        return;
    for (int i = 0; i < 256; ++i)
        ramp->red[i] = ramp->green[i] = ramp->blue[i] = (WORD)(i * 257);
}

// ---- state --------------------------------------------------------------------

void Device::ResetState()
{
    std::memset(m_rs, 0, sizeof(m_rs));
    m_rs[D3DRS_ZENABLE] = m_params.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    m_rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    m_rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    m_rs[D3DRS_ZWRITEENABLE] = TRUE;
    m_rs[D3DRS_LASTPIXEL] = TRUE;
    m_rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    m_rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    m_rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    m_rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    m_rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_STENCILFAIL] = m_rs[D3DRS_STENCILZFAIL] = m_rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_STENCILMASK] = m_rs[D3DRS_STENCILWRITEMASK] = 0xffffffff;
    m_rs[D3DRS_CCW_STENCILFAIL] = m_rs[D3DRS_CCW_STENCILZFAIL] = m_rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_COLORWRITEENABLE] = m_rs[D3DRS_COLORWRITEENABLE1] = m_rs[D3DRS_COLORWRITEENABLE2] =
        m_rs[D3DRS_COLORWRITEENABLE3] = 0xf;
    m_rs[D3DRS_BLENDOP] = m_rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    m_rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    m_rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    m_rs[D3DRS_BLENDFACTOR] = 0xffffffff;
    m_rs[D3DRS_MULTISAMPLEMASK] = 0xffffffff;
    for (auto &sampler : m_ss)
    {
        std::memset(sampler, 0, sizeof(sampler));
        sampler[D3DSAMP_ADDRESSU] = sampler[D3DSAMP_ADDRESSV] = sampler[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        sampler[D3DSAMP_MAGFILTER] = sampler[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        sampler[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        sampler[D3DSAMP_MAXANISOTROPY] = 1;
    }
    std::memset(m_ssDescriptor, 0xff, sizeof(m_ssDescriptor));
    m_texSlotDirty[0] = m_texSlotDirty[1] = ~0u;
    std::memset(m_textures, 0, sizeof(m_textures));
    std::memset(m_texStores, 0, sizeof(m_texStores));
    std::memset(m_texForgotten, 0, sizeof(m_texForgotten));
    for (uint32_t i = 1; i < 4; ++i)
        Rebind(m_renderTargets[i], (Surface *)nullptr);
    Rebind(m_renderTargets[0], m_backBuffer);
    Rebind(m_depthStencil, (Surface *)nullptr);
    for (auto &stream : m_streams)
    {
        stream.buffer = nullptr;
        stream.offset = stream.stride = 0;
        stream.freq = 1;
        stream.forgotten = false;
    }
    m_indices = nullptr;
    m_indicesForgotten = false;
    Rebind(m_decl, (VertexDecl *)nullptr);
    Rebind(m_vs, (VertexShader *)nullptr);
    Rebind(m_ps, (PixelShader *)nullptr);
    m_viewport = {0, 0, m_params.BackBufferWidth, m_params.BackBufferHeight, 0.0f, 1.0f};
    m_scissor = {0, 0, (LONG)m_params.BackBufferWidth, (LONG)m_params.BackBufferHeight};
    m_vsFile.Reset(); // zeroed, all dirty: the next draw pushes the whole file
    m_psFile.Reset();
    m_inScene = false;
    // Device reset: every baked unit is dropped (explicit invalidation; the
    // units hold nothing reset changes, but a reset is where a stale unit
    // would be hardest to find).
    m_rasterUnits.Clear();
    m_programUnits.Clear();
    m_program = nullptr;
    m_recorded.raster = nullptr;
    m_recorded.attribs = false;
    CancelInstances();
    m_dirtyTargets = m_dirtyViewport = m_dirtyRaster = true;
    m_dirtyInput = m_dirtyShaders = m_dirtyTextures = m_dirtyAttribs = true;
}

HRESULT Device::SetRenderTarget(DWORD index, IDirect3DSurface9 *surface)
{
    CensusScope census(this, Census_SetRenderTarget);
    DeviceLockGuard lock(m_lock);
    if (index >= 4 || (!index && !surface))
        return D3DERR_INVALIDCALL;
    Surface *s = static_cast<Surface *>(surface);
    if (s && (!s->Store()->gpu || s->Store()->format->depth || !(s->Store()->usage & D3DUSAGE_RENDERTARGET)))
        return Fail("SET_RENDER_TARGET", "surface is not a color render target");
    if (s)
        s->Store()->attachment = true; // latch (already true via usage in practice)
    Rebind(m_renderTargets[index], s);
    if (!index)
    {
        // D3D9: setting RT0 resets the viewport and scissor to the target.
        const ImageStore &store = *s->Store();
        m_viewport = {0, 0, store.LevelWidth(s->Level()), store.LevelHeight(s->Level()), 0.0f, 1.0f};
        m_scissor = {0, 0, (LONG)m_viewport.Width, (LONG)m_viewport.Height};
        m_dirtyViewport = true;
    }
    m_dirtyTargets = true;
    return D3D_OK;
}

HRESULT Device::GetRenderTarget(DWORD index, IDirect3DSurface9 **surface)
{
    DeviceLockGuard lock(m_lock);
    if (index >= 4 || !surface)
        return D3DERR_INVALIDCALL;
    *surface = m_renderTargets[index];
    if (!*surface)
        return D3DERR_NOTFOUND;
    (*surface)->AddRef();
    return D3D_OK;
}

HRESULT Device::SetDepthStencilSurface(IDirect3DSurface9 *surface)
{
    CensusScope census(this, Census_SetDepthStencilSurface);
    DeviceLockGuard lock(m_lock);
    Surface *s = static_cast<Surface *>(surface);
    if (s && (!s->Store()->gpu || !s->Store()->format->depth))
        return Fail("SET_DEPTH_STENCIL", "surface is not a depth-stencil target");
    if (s)
        s->Store()->attachment = true; // latch (already true via usage in practice)
    Rebind(m_depthStencil, s);
    m_dirtyTargets = m_dirtyRaster = true;
    return D3D_OK;
}

HRESULT Device::GetDepthStencilSurface(IDirect3DSurface9 **surface)
{
    DeviceLockGuard lock(m_lock);
    if (!surface)
        return D3DERR_INVALIDCALL;
    *surface = m_depthStencil;
    if (!*surface)
        return D3DERR_NOTFOUND;
    (*surface)->AddRef();
    return D3D_OK;
}

HRESULT Device::BeginScene()
{
    CensusScope census(this, Census_BeginScene);
    DeviceLockGuard lock(m_lock);
    if (m_inScene)
        return D3DERR_INVALIDCALL;
    m_inScene = true;
    return D3D_OK;
}

HRESULT Device::EndScene()
{
    CensusScope census(this, Census_EndScene);
    DeviceLockGuard lock(m_lock);
    if (!m_inScene)
        return D3DERR_INVALIDCALL;
    m_inScene = false;
    // D3D9 drivers commonly kick work at EndScene; so do we, so the GPU
    // starts on the frame before Present.
    Flush();
    return D3D_OK;
}

HRESULT Device::SetViewport(const D3DVIEWPORT9 *viewport)
{
    DeviceLockGuard lock(m_lock);
    if (!viewport)
        return D3DERR_INVALIDCALL;
    m_viewport = *viewport;
    m_dirtyViewport = true;
    return D3D_OK;
}

HRESULT Device::GetViewport(D3DVIEWPORT9 *viewport)
{
    DeviceLockGuard lock(m_lock);
    if (!viewport)
        return D3DERR_INVALIDCALL;
    *viewport = m_viewport;
    return D3D_OK;
}

HRESULT Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
    CensusScope census(this, Census_SetRenderState);
    DeviceLockGuard lock(m_lock);
    if ((unsigned)state >= 256)
        return D3DERR_INVALIDCALL;
    if (m_rs[state] == value)
        return D3D_OK;
    m_rs[state] = value;
    switch (state)
    {
    case D3DRS_SCISSORTESTENABLE:
        m_dirtyViewport = true;
        break;
    case D3DRS_ADAPTIVETESS_Y:
        // NVIDIA's alpha-to-coverage switch ('ATOC'); needs MSAA, which this
        // device does not create. Only "off" is exact.
        if (value && value != D3DFMT_UNKNOWN)
            Fail("RENDER_STATE", "ADAPTIVETESS_Y=0x%08x (alpha to coverage) unsupported", (unsigned)value);
        break;
    case D3DRS_SRGBWRITEENABLE:
        if (value)
            Fail("RENDER_STATE", "SRGBWRITEENABLE unsupported");
        break;
    case D3DRS_CLIPPLANEENABLE:
        if (value)
            Fail("RENDER_STATE", "user clip planes unsupported (mask 0x%x)", (unsigned)value);
        break;
    default:
        // Only states that feed the baked raster key (deko9_baked.h) dirty it.
        if (IsRasterRenderState(state))
            m_dirtyRaster = true;
        break;
    }
    return D3D_OK;
}

HRESULT Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD *value)
{
    DeviceLockGuard lock(m_lock);
    if ((unsigned)state >= 256 || !value)
        return D3DERR_INVALIDCALL;
    *value = m_rs[state];
    return D3D_OK;
}

HRESULT Device::GetTexture(DWORD stage, IDirect3DBaseTexture9 **texture)
{
    DeviceLockGuard lock(m_lock);
    const int slot = SamplerSlot(stage);
    if (slot < 0 || !texture)
        return D3DERR_INVALIDCALL;
    *texture = m_textures[slot];
    if (*texture)
        (*texture)->AddRef();
    return D3D_OK;
}

HRESULT Device::SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture)
{
    CensusScope census(this, Census_SetTexture);
    DeviceLockGuard lock(m_lock);
    const int slot = SamplerSlot(stage);
    if (slot < 0)
        return D3DERR_INVALIDCALL;
    BindTexture((uint32_t)slot, texture);
    return D3D_OK;
}

void Device::GetCounters(Deko9Counters *out)
{
    out->draws = m_timing.draws;
    out->bakedHits = m_timing.bakedHits;
    out->bakedMisses = m_timing.bakedMisses;
    out->bakedReplays = m_timing.bakedReplays;
    out->instancedDraws = m_timing.instancedDraws;
    out->instances = m_timing.instances;
    out->instanceFallbacks = m_timing.instanceFallbacks;
    out->rasterUnits = m_rasterUnits.Size();
    out->programUnits = m_programUnits.Size();
    out->verifiedDraws = m_verifiedDraws;
    out->verifyMismatches = m_verifyMismatches;
    out->updateTextureCalls = m_censusEntries[Census_UpdateTexture].calls;
    out->updateTextureBytes = m_censusEntries[Census_UpdateTexture].bytes;
    out->earlyZCandidates = m_timing.earlyZCandidates;
    out->earlyZDraws = m_timing.earlyZDraws;
    out->drawsTotal = m_drawsTotal + m_timing.draws;
    out->drawCpuNsTotal = m_drawCpuNsTotal + m_timing.drawCpuNs;
    out->hazardSkips = m_timing.hazardSkips;
    out->rangeCalls = m_timing.rangeCalls;
    out->rangeDraws = m_timing.rangeDraws;
    out->uploadAfterReadBarriers = m_writeAfterReadBarriers;
}

void Device::BindTexture(uint32_t slot, IDirect3DBaseTexture9 *texture)
{
    m_texForgotten[slot] = false;
    if (m_textures[slot] == texture)
        return;
    m_textures[slot] = texture;
    m_texStores[slot] = StoreOf(texture);
    m_dirtyTextures = true;
    MarkTexSlotDirty(slot);
    ++m_timing.textureBinds;
}

void Device::ForgetTexture(IDirect3DBaseTexture9 *texture)
{
    DeviceLockGuard lock(m_lock);
    for (uint32_t slot = 0; slot < DEKO9_MAX_SAMPLERS * 2; ++slot)
    {
        if (m_textures[slot] == texture)
        {
            m_textures[slot] = nullptr;
            m_texStores[slot] = nullptr;
            m_texForgotten[slot] = true;
            m_dirtyTextures = true;
            MarkTexSlotDirty(slot);
        }
    }
}

void Device::ForgetBuffer(VertexBuffer *buffer)
{
    DeviceLockGuard lock(m_lock);
    for (Stream &stream : m_streams)
    {
        if (stream.buffer == buffer)
        {
            stream.buffer = nullptr;
            stream.forgotten = true;
            m_dirtyInput = true;
        }
    }
}

void Device::ForgetBuffer(IndexBuffer *buffer)
{
    DeviceLockGuard lock(m_lock);
    if (m_indices == buffer)
    {
        m_indices = nullptr;
        m_indicesForgotten = true;
        m_recorded.indexAddress = 0;
    }
}

uint32_t Device::SetSamplerPacked(uint32_t slot, uint32_t packed, uint32_t oldPacked)
{
    bool changed;
    const uint32_t final = ApplyEngineSamplerState(m_ss[slot], packed, oldPacked, &changed);
    if (changed)
    {
        m_ssDescriptor[slot][0] = m_ssDescriptor[slot][1] = UINT32_MAX;
        m_dirtyTextures = true;
        MarkTexSlotDirty(slot);
    }
    ++m_timing.samplerSets;
    return final;
}

void Device::SetEngineLodBias(float bias)
{
    const int steps = std::min(std::max((int)std::lround(-bias * 8.0f), 0), 15);
    const float rounded = -(float)steps / 8.0f;
    DWORD bits = 0;
    if (steps)
        std::memcpy(&bits, &rounded, sizeof(bits));
    for (uint32_t slot = 0; slot < DEKO9_MAX_SAMPLERS * 2; ++slot)
    {
        if (m_ss[slot][D3DSAMP_MIPMAPLODBIAS] == bits)
            continue;
        m_ss[slot][D3DSAMP_MIPMAPLODBIAS] = bits;
        m_ssDescriptor[slot][0] = m_ssDescriptor[slot][1] = UINT32_MAX;
        m_dirtyTextures = true;
        MarkTexSlotDirty(slot);
    }
}

HRESULT Device::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value)
{
    DeviceLockGuard lock(m_lock);
    const int slot = SamplerSlot(sampler);
    if (slot < 0 || type < D3DSAMP_ADDRESSU || type > D3DSAMP_DMAPOFFSET || !value)
        return D3DERR_INVALIDCALL;
    *value = m_ss[slot][type];
    return D3D_OK;
}

HRESULT Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    CensusScope census(this, Census_SetSamplerState);
    DeviceLockGuard lock(m_lock);
    const int slot = SamplerSlot(sampler);
    if (slot < 0 || type < D3DSAMP_ADDRESSU || type > D3DSAMP_DMAPOFFSET)
        return D3DERR_INVALIDCALL;
    if (type == D3DSAMP_SRGBTEXTURE && value)
        return Fail("SAMPLER_STATE", "SRGBTEXTURE unsupported");
    if (m_ss[slot][type] != value)
    {
        m_ss[slot][type] = value;
        m_ssDescriptor[slot][0] = m_ssDescriptor[slot][1] = UINT32_MAX;
        m_dirtyTextures = true;
        MarkTexSlotDirty(slot);
    }
    return D3D_OK;
}

HRESULT Device::SetScissorRect(const RECT *rect)
{
    DeviceLockGuard lock(m_lock);
    if (!rect)
        return D3DERR_INVALIDCALL;
    m_scissor = *rect;
    m_dirtyViewport = true;
    return D3D_OK;
}

HRESULT Device::GetScissorRect(RECT *rect)
{
    DeviceLockGuard lock(m_lock);
    if (!rect)
        return D3DERR_INVALIDCALL;
    *rect = m_scissor;
    return D3D_OK;
}

HRESULT Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *decl)
{
    CensusScope census(this, Census_SetVertexDeclaration);
    DeviceLockGuard lock(m_lock);
    if (m_decl != decl)
    {
        Rebind(m_decl, static_cast<VertexDecl *>(decl));
        m_dirtyAttribs = m_dirtyInput = true;
    }
    return D3D_OK;
}

HRESULT Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **decl)
{
    DeviceLockGuard lock(m_lock);
    if (!decl)
        return D3DERR_INVALIDCALL;
    *decl = m_decl;
    if (m_decl)
        m_decl->AddRef();
    return D3D_OK;
}

HRESULT Device::SetFVF(DWORD fvf)
{
    // Fixed-function vertex formats are not used by the engine's shaders.
    return fvf ? Fail("UNSUPPORTED", "SetFVF(0x%x)", (unsigned)fvf) : D3D_OK;
}

HRESULT Device::GetFVF(DWORD *fvf)
{
    if (!fvf)
        return D3DERR_INVALIDCALL;
    *fvf = 0;
    return D3D_OK;
}

HRESULT Device::SetVertexShader(IDirect3DVertexShader9 *shader)
{
    CensusScope census(this, Census_SetVertexShader);
    DeviceLockGuard lock(m_lock);
    if (m_vs != shader)
    {
        Rebind(m_vs, static_cast<VertexShader *>(shader));
        m_dirtyShaders = m_dirtyAttribs = true;
    }
    return D3D_OK;
}

HRESULT Device::GetVertexShader(IDirect3DVertexShader9 **shader)
{
    DeviceLockGuard lock(m_lock);
    if (!shader)
        return D3DERR_INVALIDCALL;
    *shader = m_vs;
    if (m_vs)
        m_vs->AddRef();
    return D3D_OK;
}

HRESULT Device::SetPixelShader(IDirect3DPixelShader9 *shader)
{
    CensusScope census(this, Census_SetPixelShader);
    DeviceLockGuard lock(m_lock);
    if (m_ps != shader)
    {
        Rebind(m_ps, static_cast<PixelShader *>(shader));
        m_dirtyShaders = m_dirtyTextures = true;
    }
    return D3D_OK;
}

HRESULT Device::GetPixelShader(IDirect3DPixelShader9 **shader)
{
    DeviceLockGuard lock(m_lock);
    if (!shader)
        return D3DERR_INVALIDCALL;
    *shader = m_ps;
    if (m_ps)
        m_ps->AddRef();
    return D3D_OK;
}

template <uint32_t Regs>
HRESULT Device::SetConstants(ConstantFile<Regs> &file, UINT start, const float *data, UINT count)
{
    if (!data || start > Regs || count > Regs - start)
        return D3DERR_INVALIDCALL;
    if (!count)
        return D3D_OK;
    m_timing.constRegsSet += count;
    m_timing.constRegsChanged += file.Set(start, data, count);
    return D3D_OK;
}

HRESULT Device::SetVertexShaderConstantF(UINT start, const float *data, UINT count)
{
    CensusScope census(this, Census_SetVertexShaderConstantF);
    census.AddBytes((uint64_t)count * 16);
    DeviceLockGuard lock(m_lock);
    return SetConstants(m_vsFile, start, data, count);
}

HRESULT Device::GetVertexShaderConstantF(UINT start, float *data, UINT count)
{
    DeviceLockGuard lock(m_lock);
    if (!data || start > DEKO9_VS_CONST_REGS || count > DEKO9_VS_CONST_REGS - start)
        return D3DERR_INVALIDCALL;
    std::memcpy(data, m_vsFile.regs[start], count * 16);
    return D3D_OK;
}

HRESULT Device::SetPixelShaderConstantF(UINT start, const float *data, UINT count)
{
    CensusScope census(this, Census_SetPixelShaderConstantF);
    census.AddBytes((uint64_t)count * 16);
    DeviceLockGuard lock(m_lock);
    return SetConstants(m_psFile, start, data, count);
}

HRESULT Device::GetPixelShaderConstantF(UINT start, float *data, UINT count)
{
    DeviceLockGuard lock(m_lock);
    if (!data || start > DEKO9_PS_CONST_REGS || count > DEKO9_PS_CONST_REGS - start)
        return D3DERR_INVALIDCALL;
    std::memcpy(data, m_psFile.regs[start], count * 16);
    return D3D_OK;
}

HRESULT Device::SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride)
{
    CensusScope census(this, Census_SetStreamSource);
    DeviceLockGuard lock(m_lock);
    if (stream >= DK_MAX_VERTEX_BUFFERS)
        return D3DERR_INVALIDCALL;
    // Non-owning (ForgetBuffer): no COM reference per bind. Re-setting the
    // current binding is not special-cased: the engine never does it.
    m_streams[stream].buffer = static_cast<VertexBuffer *>(buffer);
    m_streams[stream].offset = offset;
    m_streams[stream].stride = stride;
    m_streams[stream].forgotten = false;
    m_dirtyInput = true;
    ++m_timing.bufferBinds;
    return D3D_OK;
}

HRESULT Device::GetStreamSource(UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset, UINT *stride)
{
    DeviceLockGuard lock(m_lock);
    if (stream >= DK_MAX_VERTEX_BUFFERS || !buffer || !offset || !stride)
        return D3DERR_INVALIDCALL;
    *buffer = m_streams[stream].buffer;
    if (*buffer)
        (*buffer)->AddRef();
    *offset = m_streams[stream].offset;
    *stride = m_streams[stream].stride;
    return D3D_OK;
}

HRESULT Device::SetStreamSourceFreq(UINT stream, UINT divider)
{
    DeviceLockGuard lock(m_lock);
    if (stream >= DK_MAX_VERTEX_BUFFERS)
        return D3DERR_INVALIDCALL;
    if (divider != 1)
        return Fail("UNSUPPORTED", "instancing: SetStreamSourceFreq(%u, 0x%x)", stream, divider);
    m_streams[stream].freq = divider;
    return D3D_OK;
}

HRESULT Device::GetStreamSourceFreq(UINT stream, UINT *divider)
{
    DeviceLockGuard lock(m_lock);
    if (stream >= DK_MAX_VERTEX_BUFFERS || !divider)
        return D3DERR_INVALIDCALL;
    *divider = m_streams[stream].freq;
    return D3D_OK;
}

HRESULT Device::SetIndices(IDirect3DIndexBuffer9 *indices)
{
    CensusScope census(this, Census_SetIndices);
    DeviceLockGuard lock(m_lock);
    // Non-owning (ForgetBuffer): no COM reference per bind.
    m_indices = static_cast<IndexBuffer *>(indices);
    m_indicesForgotten = false;
    ++m_timing.bufferBinds;
    return D3D_OK;
}

HRESULT Device::GetIndices(IDirect3DIndexBuffer9 **indices)
{
    DeviceLockGuard lock(m_lock);
    if (!indices)
        return D3DERR_INVALIDCALL;
    *indices = m_indices;
    if (m_indices)
        m_indices->AddRef();
    return D3D_OK;
}

// ---- swap chain object ------------------------------------------------------

HRESULT SwapChain::GetDevice(IDirect3DDevice9 **device)
{
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *device = m_device;
    return D3D_OK;
}

HRESULT SwapChain::Present(const RECT *src, const RECT *dst, HWND window, const RGNDATA *dirty, DWORD)
{
    return m_device->Present(src, dst, window, dirty);
}

HRESULT SwapChain::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface)
{
    return m_device->GetBackBuffer(0, index, type, surface);
}

HRESULT SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS *params)
{
    if (!params)
        return D3DERR_INVALIDCALL;
    *params = m_device->m_params;
    return D3D_OK;
}

HRESULT SwapChain::GetDisplayMode(D3DDISPLAYMODE *mode)
{
    return m_device->GetDisplayMode(0, mode);
}

HRESULT SwapChain::GetFrontBufferData(IDirect3DSurface9 *dest)
{
    return m_device->GetFrontBufferData(0, dest);
}

} // namespace deko9

// Called by the engine's R_SwitchWaitForGpuIdle when this backend is linked.
void Deko9_WaitForGpuIdle(IDirect3DDevice9 *device)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->WaitIdle();
}

// ---- native fast path (deko9_native.h) -------------------------------------------

void Deko9_BeginBatch(IDirect3DDevice9 *device)
{
    deko9::DeviceLock &lock = static_cast<deko9::Device *>(device)->Lock();
    lock.lock();
    lock.SetSite("batch");
}

void Deko9_EndBatch(IDirect3DDevice9 *device)
{
    deko9::DeviceLock &lock = static_cast<deko9::Device *>(device)->Lock();
    lock.SetSite(nullptr);
    lock.unlock();
}

void Deko9_SetTexture(IDirect3DDevice9 *device, uint32_t sampler, IDirect3DBaseTexture9 *texture)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::CensusScope census(d, deko9::Census_NativeSetTexture);
    deko9::DeviceLockGuard lock(d->Lock());
    const int slot = deko9::SamplerSlot(sampler);
    if (slot < 0)
    {
        deko9::Fail("NATIVE", "Deko9_SetTexture sampler %u", (unsigned)sampler);
        return;
    }
    d->BindTexture((uint32_t)slot, texture);
}

void Deko9_SetEngineLodBias(IDirect3DDevice9 *device, float bias)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetEngineLodBias(bias);
}

uint32_t Deko9_SetSamplerPacked(IDirect3DDevice9 *device, uint32_t sampler, uint32_t packed, uint32_t oldPacked)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::CensusScope census(d, deko9::Census_NativeSetSamplerPacked);
    deko9::DeviceLockGuard lock(d->Lock());
    const int slot = deko9::SamplerSlot(sampler);
    if (slot < 0)
    {
        deko9::Fail("NATIVE", "Deko9_SetSamplerPacked sampler %u", (unsigned)sampler);
        return oldPacked;
    }
    return d->SetSamplerPacked((uint32_t)slot, packed, oldPacked);
}

void Deko9_SetVerify(IDirect3DDevice9 *device, bool enable)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetVerify(enable);
}

void Deko9_SetEarlyZ(IDirect3DDevice9 *device, bool enable)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetEarlyZ(enable);
}

void Deko9_ClaimSubmitThread(IDirect3DDevice9 *device)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->ClaimSubmitThread();
}

void Deko9_SetPerDraw(IDirect3DDevice9 *device, uint32_t flags)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetPerDraw(flags);
}

bool Deko9_BeginInstances(IDirect3DDevice9 *device, const uint8_t *regs, uint32_t regCount)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    if (!regs || !regCount || regCount > deko9::kMaxInstanceRegs)
        return false;
    deko9::InstanceLayout layout;
    if (!deko9::LayoutFromRegs(regs, regCount, &layout))
        return false;
    return d->BeginInstances(layout);
}

void Deko9_AddInstance(IDirect3DDevice9 *device)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::CensusScope census(d, deko9::Census_NativeAddInstance);
    deko9::DeviceLockGuard lock(d->Lock());
    d->AddInstance();
}

int32_t Deko9_DrawIndexedInstances(IDirect3DDevice9 *device, int32_t baseVertex, uint32_t minIndex,
                                   uint32_t numVertices, uint32_t startIndex, uint32_t primCount)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::CensusScope census(d, deko9::Census_NativeDrawIndexedInstances);
    deko9::DeviceLockGuard lock(d->Lock());
    return d->DrawInstances(D3DPT_TRIANGLELIST, baseVertex, minIndex, numVertices, startIndex, primCount);
}

int32_t Deko9_DrawIndexedRanges(IDirect3DDevice9 *device, uint32_t numVertices, const Deko9IndexRange *ranges,
                                uint32_t count)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::CensusScope census(d, deko9::Census_NativeDrawIndexedRanges);
    deko9::DeviceLockGuard lock(d->Lock());
    return d->DrawIndexedRanges(numVertices, ranges, count);
}

void Deko9_NoteInstanceFallback(IDirect3DDevice9 *device, uint32_t draws)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->m_timing.instanceFallbacks += draws;
}

void Deko9_GetCounters(IDirect3DDevice9 *device, Deko9Counters *out)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->GetCounters(out);
}

void Deko9_SetGpuPasses(IDirect3DDevice9 *device, bool enable)
{
    static_cast<deko9::Device *>(device)->SetGpuPasses(enable);
}

void Deko9_GpuMarker(IDirect3DDevice9 *device, uint32_t pass)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    // m_censusPassMask is written by the thread that issues the markers
    // (the render backend, Deko9_SetDrawCensus); so are the zcull stats
    // (applied at Present), whose region model is kept per pass too.
    if (!d->GpuPassesOn() && !d->m_censusPassMask && !d->ZcullStatsOn())
        return;
    deko9::DeviceLockGuard lock(d->Lock());
    if (d->GpuPassesOn() || d->m_censusPassMask || d->ZcullStatsOn())
        d->GpuMarker(pass);
}

const char *Deko9_GpuPassName(uint32_t pass)
{
    return pass < Deko9GpuPass_Count ? deko9::kGpuPassNames[pass] : nullptr;
}

uint32_t Deko9_FramesInFlight()
{
    return deko9::kFramesInFlight;
}

uint64_t Deko9_FrameRecording(IDirect3DDevice9 *device)
{
    // Published by the recording thread at each Present; no device lock.
    return static_cast<deko9::Device *>(device)->FrameRecording();
}

bool Deko9_FrameDone(IDirect3DDevice9 *device, uint64_t frame)
{
    // No device lock from any thread (published frame state + one fence poll).
    return static_cast<deko9::Device *>(device)->FrameDoneAnyThread(frame);
}

bool Deko9_WaitFrame(IDirect3DDevice9 *device, uint64_t frame, int64_t timeoutNs)
{
    return device && static_cast<deko9::Device *>(device)->WaitFrameFor(frame, timeoutNs);
}

bool Deko9_WaitQuery(IDirect3DQuery9 *query, int64_t timeoutNs)
{
    return query && static_cast<deko9::Query *>(query)->Wait(timeoutNs);
}

bool Deko9_DebugQueryGpuPassed(IDirect3DQuery9 *query)
{
    return query && static_cast<deko9::Query *>(query)->RawGpuPassed();
}

void Deko9_SetShadowFilter(IDirect3DDevice9 *device, uint32_t mode)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetShadowFilter(mode);
}

void Deko9_SetShaderOpt(IDirect3DDevice9 *device, uint32_t mask)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetShaderOpt(mask);
}


