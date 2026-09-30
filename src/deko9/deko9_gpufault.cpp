// GPU-fault black box and GPU VA map of the deko3d renderer (deko9):
// r_deko9FaultTrace / r_deko9GpuMap. Pure parts in deko9_gpufault.h; the
// fault and the evidence gathered on hardware.

#include <switch.h>

#include "deko9_internal.h"

#include <algorithm>
#include <cinttypes>

namespace deko9
{

namespace
{
std::atomic<Device *> s_gpuMapDevice{nullptr}; // Deko9_SetDebugName has no device

const char *PoolName(uint8_t pool)
{
    static const char *const kNames[POOL_COUNT] = {"buffer", "dynamic", "image", "code", "cmd"};
    return pool < POOL_COUNT ? kNames[pool] : "?";
}

constexpr uint64_t kHeartbeatNs = 250000000ull;
constexpr uint64_t kWatchTickNs = 2000000ull;
} // namespace

uint64_t Device::ImageVa(const ImageStore *store) const
{
    if (!store || !store->gpu)
        return 0;
    if (store->descriptor != UINT32_MAX && store->descriptor < kImageDescriptors)
        return TicAddress(m_descriptorMemory.cpu + store->descriptor * sizeof(DkImageDescriptor));
    // Render-target-only surfaces have no sampling descriptor: build one on
    // the stack (not written to the pool) just to read its address.
    DkImageView view;
    dkImageViewDefaults(&view, &store->image);
    DkImageDescriptor descriptor;
    dkImageDescriptorInitialize(&descriptor, &view, false, false);
    return TicAddress(&descriptor);
}

void Device::GpuMapImage(const ImageStore *store, const char *what)
{
    if (!m_gpuMap && !m_faultTrace)
        return;
    const uint64_t va = ImageVa(store);
    if (what[0] != 'n') // "name" only labels an image already mapped
    {
        const uint8_t kind =
            what[0] == 'r' ? kGpuEventImageResize : what[0] == 'f' ? kGpuEventFreeDeferred : kGpuEventImage;
        NoteGpuEvent(kind, va, store->memory.size, (uint8_t)store->memory.pool, (uint16_t)store->memory.chunk);
    }
    if (!m_gpuMap)
        return;
    Log("gpumap %s va=0x%llx-0x%llx mem=0x%llx pool=%s chunk=%u off=0x%x size=0x%x d3dfmt=%u %ux%ux%u lv=%u "
        "faces=%u usage=0x%x cmp=%d desc=%d seq=%llu name=%s",
        what, (unsigned long long)va, (unsigned long long)(va + store->memory.size),
        (unsigned long long)store->memory.gpu, PoolName((uint8_t)store->memory.pool), store->memory.chunk,
        store->memory.offset, store->memory.size, store->format ? (unsigned)store->format->d3d : 0u, store->width,
        store->height, store->depth, store->levels, store->faces, (unsigned)store->usage, (int)store->compressed,
        store->descriptor == UINT32_MAX ? -1 : (int)store->descriptor, (unsigned long long)m_openSeq,
        store->debugName ? store->debugName : "-");
}

void Device::LogGpuMapStatic()
{
    if (m_gpuMapStaticLogged)
        return;
    m_gpuMapStaticLogged = true;
    const auto line = [](const char *name, const GpuAlloc &a, uint32_t size) {
        if (!a)
            return;
        Log("gpumap static name=%s va=0x%llx-0x%llx pool=%s chunk=%u off=0x%x", name, (unsigned long long)a.gpu,
            (unsigned long long)(a.gpu + size), PoolName((uint8_t)a.pool), a.chunk, a.offset);
    };
    // What the GPC constant cache (GCC) reads: descriptor pools (bound at
    // every list start), the constant buffers c[2] (per-draw constants via
    // dkCmdBufPushConstants), c[3] (FSR/probe), shader code + c[1] (code
    // pool, "DEKO9 shader code" style lines per program are not logged: the
    // code pool's memblock range covers them). deko3d's own queue memory
    // (driver constbuf c[0], work buffer, queue command ring) is not visible
    // here: it is the unlabelled space before the first deko9 memblock.
    line("tic_pool", m_descriptorMemory, kImageDescriptors * sizeof(DkImageDescriptor));
    GpuAlloc tsc = m_descriptorMemory;
    tsc.gpu += kImageDescriptors * sizeof(DkImageDescriptor);
    tsc.offset += kImageDescriptors * sizeof(DkImageDescriptor);
    line("tsc_pool", tsc, kSamplerDescriptors * sizeof(DkSamplerDescriptor));
    line("vs_ubo_c2", m_vsUbo, m_vsUbo.size);
    line("ps_ubo_c2", m_psUbo, m_psUbo.size);
    line("fsr_c3", m_fsr.constants, m_fsr.constants.size);
    line("timestamps", m_timestamps, m_timestamps.size);
    line("pass_stamps", m_passStamps, m_passStamps.size);
    line("zcull_stamps", m_zcullStamps, m_zcullStamps.size);
    line("crumbs", m_crumbs, m_crumbs.size);
    for (int i = 0; i < 3; ++i)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "swap%d", i);
        line(name, m_swapMemory[i], m_swapMemory[i].size);
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (m_dummy[i])
            GpuMapImage(m_dummy[i], "image");
    }
}

void Device::ApplyGpuMap()
{
    const uint32_t map = m_gpuMapWanted.load(std::memory_order_relaxed);
    if (map == m_gpuMap)
        return;
    m_gpuMap = map;
    s_gpuMapDevice.store(map ? this : nullptr, std::memory_order_relaxed);
    Log("gpumap level=%u", map);
    if (map)
        LogGpuMapStatic();
}

void Device::ApplyFaultTraceSettings()
{
    ApplyGpuMap();
    const uint32_t trace = m_faultTraceWanted.load(std::memory_order_relaxed);
    if (trace == m_faultTrace)
        return;
    if (trace && !m_crumbs)
    {
        if (!AllocMemory(POOL_DYNAMIC, 256, 256, &m_crumbs))
        {
            Fail("FAULT_TRACE", "no breadcrumb memory; r_deko9FaultTrace stays off");
            m_faultTraceWanted.store(0, std::memory_order_relaxed);
            return;
        }
        // Until the GPU writes its first crumb the cell reads as "between
        // lists, after the newest submitted one" (long threshold), not as
        // draw 0 of list 0 (which the watcher took for a mid-list stall).
        std::memset(m_crumbs.cpu, 0, m_crumbs.size);
        const uint32_t idle = CrumbEnd(m_submittedSeq.load(std::memory_order_relaxed));
        std::memcpy(m_crumbs.cpu, &idle, sizeof(idle));
    }
    if (trace && !m_drawRecords)
        m_drawRecords.reset(new DrawRecordRing<kFenceRing, kDrawRecords>());
    m_faultTrace = trace;
    Log("faulttrace interval=%u (breadcrumb every %u draws, watcher %s)", trace, trace, trace ? "on" : "off");
    if (trace)
    {
        // The open list was begun without a crumb (BeginList ran before this).
        FaultTraceBeginList();
        if (!m_watchRunning)
        {
            m_watchStop.store(false, std::memory_order_relaxed);
            // Above every game thread, any core: a tick is a few loads.
            if (R_SUCCEEDED(threadCreate(&m_watchThread, WatchMain, this, nullptr, 0x4000, 0x1D, -2)) &&
                R_SUCCEEDED(threadStart(&m_watchThread)))
                m_watchRunning = true;
            else
                Fail("FAULT_TRACE", "watcher thread not started");
        }
    }
    else
    {
        StopWatcher();
    }
}

void Device::StopWatcher()
{
    if (!m_watchRunning)
        return;
    m_watchStop.store(true, std::memory_order_relaxed);
    threadWaitForExit(&m_watchThread);
    threadClose(&m_watchThread);
    m_watchRunning = false;
}

void Device::FaultTraceBeginList()
{
    if (!m_faultTrace)
        return;
    m_crumbDraws = 0;
    m_drawRecords->BeginList(m_openSeq);
    dkCmdBufReportValue(m_cmd, CrumbValue(m_openSeq, 0), m_crumbs.gpu);
}

void Device::FaultTraceDraw()
{
    // Before this draw's commands: the GPU writes the value once the draws
    // before it reached CROP.
    if (m_crumbDraws && m_crumbDraws % m_faultTrace == 0)
        dkCmdBufReportValue(m_cmd, CrumbValue(m_openSeq, m_crumbDraws), m_crumbs.gpu);
    DrawRecord r{};
    r.draw = m_crumbDraws;
    r.pass = m_curPass;
    r.vsHash = m_vs ? (uint32_t)m_vs->shader.Hash() : 0;
    r.psHash = m_ps ? (uint32_t)m_ps->shader.Hash() : 0;
    r.vsCode = m_boundVs ? m_boundVs->code.gpu : 0;
    r.psCode = m_boundPs ? m_boundPs->code.gpu : 0;
    r.texCount = (uint16_t)m_recorded.textureCount[1];
    for (uint32_t i = 0; i < kDrawRecordTextures && i < m_recorded.textureCount[1]; ++i)
        r.tex[i] = m_recorded.textures[1][i];
    if (m_renderTargets[0])
        r.rt0 = m_renderTargets[0]->Store()->memory.gpu;
    m_drawRecords->Add(m_openSeq, r);
    ++m_crumbDraws;
}

void Device::BlackBoxDump(const char *reason, uint64_t stalledNs)
{
    // Runs on the watcher thread without the device lock (or under it from
    // ReportQueueError). Everything read here is either GPU-written memory
    // or state of lists already submitted (their records are complete); a
    // concurrent writer can tear a line, never crash it.
    // A stall can also be legitimate GPU work (a long upload list while a
    // level loads): at most one stall dump per 5 s and 32 in all, so the
    // budget is still there when the fault comes; queue errors always dump.
    static std::atomic<uint32_t> s_dumps{0};
    static std::atomic<uint64_t> s_lastDumpNs{0};
    const uint64_t now = armTicksToNs(armGetSystemTick());
    const bool queueError = reason[0] == 'q';
    if (!queueError)
    {
        const uint64_t last = s_lastDumpNs.load(std::memory_order_relaxed);
        if ((last && now - last < 5000000000ull) || s_dumps.load(std::memory_order_relaxed) >= 32)
            return;
    }
    s_lastDumpNs.store(now, std::memory_order_relaxed);
    s_dumps.fetch_add(1, std::memory_order_relaxed);
    const uint64_t newest = m_submittedSeq.load(std::memory_order_acquire);
    uint32_t crumb = 0;
    if (m_crumbs)
        crumb = *reinterpret_cast<volatile const uint32_t *>(m_crumbs.cpu);
    const uint64_t crumbSeq = CrumbSeq(crumb, newest);
    const uint32_t crumbDraw = CrumbDraw(crumb);
    Log("blackbox reason=%s stalled_ms=%llu crumb=0x%08x gpu_seq=%llu gpu_draw=%u newest_submitted=%llu "
        "completed=%llu open=%llu frames=%llu",
        reason, (unsigned long long)(stalledNs / 1000000ull), crumb, (unsigned long long)crumbSeq, crumbDraw,
        (unsigned long long)newest, (unsigned long long)m_completedSeq, (unsigned long long)m_openSeq,
        (unsigned long long)m_frames);
    for (uint64_t seq = newest >= kStatsRing ? newest - kStatsRing + 1 : 1; seq <= newest; ++seq)
    {
        const ListStats &l = m_listStats[seq % kStatsRing];
        if (l.seq != seq || seq + 3 < crumbSeq)
            continue;
        Log("blackbox list seq=%llu draws=%u clears=%u uploads=%u blits=%u barriers=%u descriptors=%u acquire=%d "
            "present=%d recorded=%u",
            (unsigned long long)seq, l.draws, l.clears, l.uploads, l.blits, l.barriers, l.descriptors, l.acquire,
            l.present, m_drawRecords ? m_drawRecords->Count(seq) : 0);
    }
    if (m_drawRecords && crumbDraw != kCrumbListEnd)
    {
        // The crumb says draws [0, crumbDraw) finished; the fault is in the
        // draws after it (a few may be in flight at once).
        const uint32_t first = crumbDraw > 2 ? crumbDraw - 2 : 0;
        for (uint32_t i = first; i < crumbDraw + 2 + 2 * std::max<uint32_t>(m_faultTrace, 4); ++i)
        {
            const DrawRecord *r = m_drawRecords->Get(crumbSeq, i);
            if (!r)
                break;
            char tex[kDrawRecordTextures * 40] = "";
            size_t len = 0;
            for (uint32_t t = 0; t < kDrawRecordTextures && t < r->texCount; ++t)
            {
                const uint32_t image = HandleImage(r->tex[t]);
                const uint64_t va = image < kImageDescriptors
                                        ? TicAddress(m_descriptorMemory.cpu + image * sizeof(DkImageDescriptor))
                                        : 0;
                len += (size_t)std::snprintf(tex + len, sizeof(tex) - len, "%s%u:%u@0x%llx", t ? "," : "", image,
                                             HandleSampler(r->tex[t]), (unsigned long long)va);
                if (len >= sizeof(tex))
                    break;
            }
            const char *pass = Deko9_GpuPassName(r->pass);
            Log("blackbox draw seq=%llu i=%u%s pass=%s vs=%08x@0x%llx ps=%08x@0x%llx rt0=0x%llx tex=%u[%s]",
                (unsigned long long)crumbSeq, r->draw, r->draw == crumbDraw ? " <gpu" : "", pass ? pass : "?",
                r->vsHash, (unsigned long long)r->vsCode, r->psHash, (unsigned long long)r->psCode,
                (unsigned long long)r->rt0, r->texCount, tex);
        }
    }
    GpuEvent events[48];
    const uint32_t count = m_gpuEvents.Newest(events, 48);
    for (uint32_t i = 0; i < count; ++i)
    {
        const GpuEvent &e = events[i];
        Log("blackbox event %s va=0x%llx-0x%llx size=0x%x seq=%llu pool=%s chunk=%u", GpuEventName(e.kind),
            (unsigned long long)e.gpu, (unsigned long long)(e.gpu + e.size), e.size, (unsigned long long)e.seq,
            PoolName(e.pool), e.chunk);
    }
    Log("blackbox end reason=%s", reason);
}

void Device::WatchMain(void *arg)
{
    Device *d = static_cast<Device *>(arg);
    StallDetector detector;
    uint64_t lastBeat = 0;
    while (!d->m_watchStop.load(std::memory_order_relaxed))
    {
        svcSleepThread((int64_t)kWatchTickNs);
        const uint64_t now = armTicksToNs(armGetSystemTick());
        const uint32_t crumb = *reinterpret_cast<volatile const uint32_t *>(d->m_crumbs.cpu);
        const uint64_t newest = d->m_submittedSeq.load(std::memory_order_acquire);
        if (detector.Sample(crumb, newest, newest != 0, now))
            d->BlackBoxDump("stall", detector.StalledNs(now));
        if (now - lastBeat >= kHeartbeatNs)
        {
            lastBeat = now;
            Log("gpuhb seq=%llu draw=%u newest=%llu still_ms=%llu", (unsigned long long)CrumbSeq(crumb, newest),
                CrumbDraw(crumb), (unsigned long long)newest,
                (unsigned long long)(detector.StalledNs(now) / 1000000ull));
        }
    }
}

} // namespace deko9

void Deko9_SetFaultTrace(IDirect3DDevice9 *device, uint32_t interval)
{
    static_cast<deko9::Device *>(device)->SetFaultTrace(interval);
}

void Deko9_SetGpuMap(IDirect3DDevice9 *device, uint32_t level)
{
    // Immediate (not at the next Present): called right after CreateDevice
    // too, so the render targets created before the first frame are mapped.
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetGpuMap(level);
    d->ApplyGpuMap();
}

void Deko9_GpuMapNoteName(IDirect3DBaseTexture9 *texture)
{
    deko9::Device *d = deko9::s_gpuMapDevice.load(std::memory_order_relaxed);
    deko9::ImageStore *store = deko9::StoreOf(texture);
    if (!d || !store || !store->gpu)
        return;
    deko9::DeviceLockGuard lock(d->Lock());
    d->GpuMapImage(store, "name");
}
