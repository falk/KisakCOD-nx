// GPU-fault black box and GPU VA map of the deko3d renderer (deko9):
// r_deko9FaultTrace / r_deko9GpuMap. Pure parts in deko9_gpufault.h; the
// fault and the evidence gathered on hardware.

#include <switch.h>

#include "deko9_internal.h"

#include <algorithm>
#include <cinttypes>

#include <platform/switch/switch_watchdog.h>

namespace deko9
{

namespace
{
std::atomic<Device *> s_gpuMapDevice{nullptr}; // Deko9_SetDebugName has no device
std::atomic<Device *> s_liveDevice{nullptr};   // Deko9_BlackBoxDump has none either

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
        std::memcpy(m_crumbs.cpu + 16, &idle, sizeof(idle));
        if (!CaptureTopWords())
            Fail("FAULT_TRACE", "top-of-pipe crumb words not recognised; top cell stays off");
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
            {
                m_watchRunning = true;
                s_liveDevice.store(this, std::memory_order_release);
            }
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
    s_liveDevice.store(nullptr, std::memory_order_release);
    m_watchStop.store(true, std::memory_order_relaxed);
    threadWaitForExit(&m_watchThread);
    threadClose(&m_watchThread);
    m_watchRunning = false;
}

bool Device::CaptureTopWords()
{
    // deko3d emits DkCounter_TimestampPipelineTop as a GPFIFO semaphore
    // release (address high, address low, payload 0, operation) followed by
    // an inline TiledCacheFlush; only that exact shape is patched.
    constexpr uint32_t capacity = sizeof(m_bakeStorage) / sizeof(m_bakeStorage[0]);
    const DkGpuAddr cell = m_crumbs.gpu + 16;
    dkCmdBufBeginCaptureCmds(m_bakeCmd, m_bakeStorage, capacity);
    dkCmdBufReportCounter(m_bakeCmd, DkCounter_TimestampPipelineTop, cell);
    const uint32_t count = dkCmdBufEndCaptureCmds(m_bakeCmd);
    m_topWordCount = 0;
    if (count != 6 || m_bakeStorage[1] != (uint32_t)(cell >> 32) || m_bakeStorage[2] != (uint32_t)cell ||
        m_bakeStorage[3] != 0)
        return false;
    std::memcpy(m_topWords, m_bakeStorage, count * sizeof(uint32_t));
    m_topWordCount = count;
    m_topPayload = 3;
    return true;
}

void Device::FaultTraceTop(uint32_t crumb)
{
    if (!m_topWordCount)
        return;
    uint32_t words[8];
    std::memcpy(words, m_topWords, m_topWordCount * sizeof(uint32_t));
    words[m_topPayload] = crumb;
    dkCmdBufReplayCmds(m_cmd, words, m_topWordCount);
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
    // Before this draw's commands: the GPU writes the CROP value once the
    // draws before it reached CROP, the top value once its front end fetched
    // this far.
    if (m_crumbDraws && m_crumbDraws % m_faultTrace == 0)
        dkCmdBufReportValue(m_cmd, CrumbValue(m_openSeq, m_crumbDraws), m_crumbs.gpu);
    if (m_crumbDraws % m_faultTrace == 0)
        FaultTraceTop(CrumbValue(m_openSeq, m_crumbDraws));
    DrawRecord r{};
    r.draw = m_crumbDraws;
    r.pass = m_curPass;
    r.vsHash = m_vs ? (uint32_t)m_vs->shader.Hash() : 0;
    r.psHash = m_ps ? (uint32_t)m_ps->shader.Hash() : 0;
    r.vsCode = m_boundVs ? m_boundVs->code.gpu : 0;
    r.psCode = m_boundPs ? m_boundPs->code.gpu : 0;
    // m_recorded.textures is indexed 0 = pixel, 1 = vertex (ApplyTextures).
    r.texCount = (uint16_t)m_recorded.textureCount[0];
    for (uint32_t i = 0; i < kDrawRecordTextures && i < m_recorded.textureCount[0]; ++i)
        r.tex[i] = m_recorded.textures[0][i];
    if (m_renderTargets[0])
        r.rt0 = m_renderTargets[0]->Store()->memory.gpu;
    if (m_depthStencil)
        r.depth = m_depthStencil->Store()->memory.gpu;
    m_drawRecords->Add(m_openSeq, r);
    ++m_crumbDraws;
}

bool Device::FaultTraceCells(uint32_t *crop, uint32_t *top) const
{
    if (!m_faultTrace || !m_crumbs || !m_topWordCount)
        return false;
    *crop = *reinterpret_cast<volatile const uint32_t *>(m_crumbs.cpu);
    *top = *reinterpret_cast<volatile const uint32_t *>(m_crumbs.cpu + 16);
    return true;
}

bool Device::LastDrawRecord(Deko9DrawRecordInfo *out)
{
    const DrawRecord *r = m_drawRecords ? m_drawRecords->Last(m_openSeq) : nullptr;
    if (!r)
        return false;
    *out = {r->draw, r->texCount, r->count, r->instances, r->gpuDraws, r->indexed, r->native, r->vb0, r->ib};
    return true;
}

void Device::FaultTraceNative(NativeDrawKind kind)
{
    if (!m_faultTrace)
        return;
    FaultTraceDraw();
    DrawRecord *r = m_drawRecords->Last(m_openSeq);
    if (!r)
        return;
    // Its own programs and targets, not the D3D9 state FaultTraceDraw saw.
    const uint32_t draw = r->draw;
    const uint16_t pass = r->pass;
    *r = {};
    r->draw = draw;
    r->pass = pass;
    r->native = kind;
    r->prim = (uint8_t)DkPrimitive_Triangles;
    r->count = 3;
    r->instances = 1;
    r->gpuDraws = 1;
}

void Device::FaultTraceDrawArgs(bool indexed, DkPrimitive prim, uint32_t count, uint32_t instances, uint32_t first,
                                int32_t base, uint64_t ib, uint64_t vb0, uint32_t vb0Size, uint32_t gpuDraws)
{
    if (!m_faultTrace)
        return;
    DrawRecord *r = m_drawRecords->Last(m_openSeq);
    if (!r)
        return;
    r->prim = (uint8_t)prim;
    r->indexed = indexed;
    r->gpuDraws = (uint16_t)std::min<uint32_t>(gpuDraws, 0xffff);
    r->count = count;
    r->instances = instances;
    r->first = first;
    r->base = base;
    r->ib = ib;
    r->vb0 = vb0;
    r->vb0Size = vb0Size;
    FaultTraceCheckVa(*r);
}

bool Device::GpuVaMapped(DkGpuAddr va, uint32_t size, bool imageAliases) const
{
    for (uint32_t p = 0; p < POOL_COUNT; ++p)
    {
        const Heap *heap = m_heaps[p].get();
        if (heap && (imageAliases && p == POOL_IMAGE ? heap->ContainsWithAliases(va) : heap->Contains(va, size)))
            return true;
    }
    return false;
}

void Device::FaultTraceCheckVa(const DrawRecord &r)
{
    // A junk address reaching the GPU faults (GCC: constant / header reads)
    // or hangs it far from the CPU code that made it; name it here instead.
    if (m_badVaReports >= 16)
        return;
    const char *what = nullptr;
    uint64_t va = 0;
    uint32_t slot = 0;
    if (r.vb0 && !GpuVaMapped(r.vb0, 1, false))
        what = "vb0", va = r.vb0;
    else if (r.indexed && r.ib && !GpuVaMapped(r.ib, 1, false))
        what = "ib", va = r.ib;
    else if (r.vsCode && !GpuVaMapped(r.vsCode, 1, false))
        what = "vs_code", va = r.vsCode;
    else if (r.psCode && !GpuVaMapped(r.psCode, 1, false))
        what = "ps_code", va = r.psCode;
    for (uint32_t t = 0; !what && t < kDrawRecordTextures && t < r.texCount; ++t)
    {
        const uint32_t image = HandleImage(r.tex[t]), sampler = HandleSampler(r.tex[t]);
        if (image >= kImageDescriptors)
            what = "image_slot", va = image, slot = t;
        else if (sampler >= kSamplerDescriptors)
            what = "sampler_slot", va = sampler, slot = t;
        else
        {
            va = TicAddress(m_descriptorMemory.cpu + image * sizeof(DkImageDescriptor));
            if (!GpuVaMapped(va, 1, true))
                what = "tic_address", slot = t;
        }
    }
    if (!what)
        return;
    ++m_badVaReports;
    Fail("DRAW_VA",
         "seq=%llu draw=%u %s=0x%llx (texture %u) is outside every deko9 heap: vs=%08x ps=%08x tex=%x,%x,%x,%x "
         "n=%u first=%u base=%d",
         (unsigned long long)m_openSeq, r.draw, what, (unsigned long long)va, slot, r.vsHash, r.psHash, r.tex[0],
         r.tex[1], r.tex[2], r.tex[3], r.count, r.first, r.base);
}

void Device::NoteListSegments(uint64_t seq, DkCmdList list)
{
    if (!m_cmdSegmentRecords)
    {
        m_cmdSegmentRecords.reset(new CmdSegmentRecord[kCmdSegmentRecords]{});
        m_segmentScratch.reset(new SegmentWords[kCmdSegmentRecords]{});
    }
    static CmdSegment segs[kCmdSegmentRecords];
    const int32_t n = list ? ListSegments(reinterpret_cast<const void *>(list), segs, kCmdSegmentRecords) : 0;
    if (n < 0)
    {
        Fail("FAULT_TRACE", "seq=%llu: unknown deko3d control command; command dump off for this list",
             (unsigned long long)seq);
        return;
    }
    uint32_t head = m_cmdSegmentRecordHead.load(std::memory_order_relaxed);
    for (int32_t i = 0; i < n && i < (int32_t)kCmdSegmentRecords; ++i)
    {
        // Every entry points into this list's chunks (a coalesced entry may
        // run on into the chunk right after it in the same heap block).
        const CmdSegment &g = segs[i];
        const uint32_t *cpu = nullptr;
        for (const GpuAlloc &c : m_openCmdChunks)
        {
            if (g.gpu >= c.gpu && g.gpu < c.gpu + c.size)
            {
                cpu = reinterpret_cast<const uint32_t *>(c.cpu + (g.gpu - c.gpu));
                break;
            }
        }
        if (!cpu)
            Fail("FAULT_TRACE", "seq=%llu segment %d at 0x%llx is outside the list's command chunks",
                 (unsigned long long)seq, i, (unsigned long long)g.gpu);
        m_cmdSegmentRecords[head % kCmdSegmentRecords] = {seq, cpu, g.gpu, cpu ? g.words : 0};
        ++head;
    }
    m_cmdSegmentRecordHead.store(head, std::memory_order_release);
}

uint32_t Device::ListSegmentWords(uint64_t seq, SegmentWords *out) const
{
    // The list's segments oldest first; a record being overwritten tears at
    // worst one entry of a list far older than the stuck one.
    if (!m_cmdSegmentRecords)
        return 0;
    const uint32_t head = m_cmdSegmentRecordHead.load(std::memory_order_acquire);
    const uint32_t first = head > kCmdSegmentRecords ? head - kCmdSegmentRecords : 0;
    uint32_t n = 0;
    for (uint32_t i = first; i < head; ++i)
    {
        const CmdSegmentRecord r = m_cmdSegmentRecords[i % kCmdSegmentRecords];
        if (r.seq == seq && r.cpu)
            out[n++] = {r.cpu, r.gpu, r.words};
    }
    return n;
}

bool Device::FindCommandWindow(const SegmentWords *segs, uint32_t n, uint32_t fromCrumb, uint32_t toCrumb,
                               CommandWindow *out) const
{
    // The stuck list's chunks are not recycled before its fence, so its
    // words are still the ones the GPU was given.
    const DkGpuAddr cell = m_crumbs.gpu + 16;
    *out = {};
    out->segments = n;
    SegmentPos at, to;
    if (!FindCrumbAcross(segs, n, cell, fromCrumb, {0, 0}, &at))
        return false;
    out->from = at;
    if (FindCrumbAcross(segs, n, cell, toCrumb, {at.seg, at.word + 1}, &to))
    {
        // Through the closing release's own words.
        out->to = {to.seg, to.word + 5};
        out->closed = true;
    }
    else
    {
        // Bounded run to the list's end.
        uint32_t left = 2048;
        out->to = at;
        for (uint32_t s = at.seg; s < n; ++s)
        {
            const uint32_t avail = segs[s].words - (s == at.seg ? at.word : 0);
            out->to = {s, (s == at.seg ? at.word : 0) + std::min(avail, left)};
            if (avail >= left)
                break;
            left -= avail;
        }
    }
    return true;
}

void Device::DumpCommandWindow(uint64_t seq, uint32_t fromCrumb, uint32_t toCrumb)
{
    SegmentWords *segs = m_segmentScratch.get();
    const uint32_t n = segs ? ListSegmentWords(seq, segs) : 0;
    CommandWindow w;
    if (!n || !FindCommandWindow(segs, n, fromCrumb, toCrumb, &w))
    {
        // The GPU was given these words; not finding them means the list's
        // command memory was rewritten after submission.
        Log("blackbox cmds seq=%llu from=0x%08x not found in the list's %u GPFIFO segment(s)%s",
            (unsigned long long)seq, fromCrumb, n, n ? " (command memory overwritten?)" : "");
        return;
    }
    {
        // Where the crumb sits in the list's GPFIFO entries: an entry switch
        // right after it is a command-memory chunk switch.
        char list[600];
        size_t len = 0;
        for (uint32_t s = 0; s < n && s < 16 && len < sizeof(list); ++s)
            len += (size_t)std::snprintf(list + len, sizeof(list) - len, "%s%u:0x%llx+%u", s ? " " : "", s,
                                         (unsigned long long)segs[s].gpu, segs[s].words);
        Log("blackbox segs seq=%llu count=%u crumb_seg=%u crumb_word=%u seg_words=%u left=%u [%s%s]",
            (unsigned long long)seq, n, w.from.seg, w.from.word, segs[w.from.seg].words,
            segs[w.from.seg].words - w.from.word, list, n > 16 ? " ..." : "");
    }
    Log("blackbox cmds seq=%llu from=0x%08x to=0x%08x at=0x%llx seg=%u%s", (unsigned long long)seq, fromCrumb,
        toCrumb, (unsigned long long)(segs[w.from.seg].gpu + (uint64_t)w.from.word * 4), w.from.seg,
        w.closed ? "" : " (closing crumb not in this list: decoded to the list's end, at most 2048 words)");
    uint32_t lines = 0;
    for (uint32_t s = w.from.seg; s <= w.to.seg && s < n && lines < 32; ++s)
    {
        const uint32_t begin = s == w.from.seg ? w.from.word : 0;
        const uint32_t end = s == w.to.seg ? std::min(w.to.word, segs[s].words) : segs[s].words;
        if (s != w.from.seg)
            Log("blackbox cmds |seg %u at=0x%llx words=%u| GPFIFO entry switch (next command-memory run)", s,
                (unsigned long long)segs[s].gpu, segs[s].words);
        uint32_t pos = begin;
        while (pos < end && lines < 32)
        {
            char text[900];
            bool bad = false;
            const uint32_t used = FormatPushbuffer(segs[s].cpu + pos, end - pos, 12, text, sizeof(text), &bad);
            Log("blackbox cmds +%u %s", pos - begin, text);
            ++lines;
            pos += used;
            if (bad || !used)
                return;
        }
    }
}

bool Device::FaultTraceCommandWindow(uint32_t draw, char *out, size_t cap, bool *bad)
{
    // Submits the open list so its segments are recorded, then decodes the
    // words from the draw's top crumb through the next draw's (or the list end).
    if (!m_faultTrace || !m_topWordCount)
        return false;
    const uint64_t seq = m_openSeq;
    Flush(true);
    SegmentWords *segs = m_segmentScratch.get();
    const uint32_t n = segs ? ListSegmentWords(seq, segs) : 0;
    CommandWindow w;
    const uint32_t from = CrumbValue(seq, draw);
    if (!FindCommandWindow(segs, n, from, CrumbValue(seq, draw + m_faultTrace), &w) || !w.closed)
    {
        if (!FindCommandWindow(segs, n, from, CrumbEnd(seq), &w) || !w.closed)
            return false;
    }
    FormatAcross(segs, n, w.from, w.to, 512, out, cap, bad);
    return true;
}

void Device::DumpDescriptors(const DrawRecord &r)
{
    // The headers the GPU reads for this draw's textures, raw: a junk
    // address, format or size field shows here even when the VA looks sane.
    for (uint32_t t = 0; t < kDrawRecordTextures && t < r.texCount; ++t)
    {
        const uint32_t image = HandleImage(r.tex[t]), sampler = HandleSampler(r.tex[t]);
        uint32_t tic[8]{}, tsc[8]{};
        if (image < kImageDescriptors)
            std::memcpy(tic, m_descriptorMemory.cpu + image * sizeof(DkImageDescriptor), sizeof(tic));
        if (sampler < kSamplerDescriptors)
            std::memcpy(tsc,
                        m_descriptorMemory.cpu + kImageDescriptors * sizeof(DkImageDescriptor) +
                            sampler * sizeof(DkSamplerDescriptor),
                        sizeof(tsc));
        Log("blackbox desc draw=%u t=%u image=%u tic=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x sampler=%u "
            "tsc=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
            r.draw, t, image, tic[0], tic[1], tic[2], tic[3], tic[4], tic[5], tic[6], tic[7], sampler, tsc[0], tsc[1],
            tsc[2], tsc[3], tsc[4], tsc[5], tsc[6], tsc[7]);
    }
}

void Device::FaultTraceDrawArgsStreams(bool indexed, DkPrimitive prim, uint32_t count, uint32_t instances,
                                       uint32_t first, int32_t base, uint32_t gpuDraws)
{
    if (!m_faultTrace)
        return;
    const Stream &s0 = m_streams[0];
    const uint64_t vb0 = s0.buffer ? s0.buffer->buffer.Gpu() + s0.offset : 0;
    const uint32_t vb0Size = s0.buffer ? s0.buffer->buffer.Size() : 0;
    const uint64_t ib = indexed && m_indices ? m_indices->buffer.Gpu() : 0;
    FaultTraceDrawArgs(indexed, prim, count, instances, first, base, ib, vb0, vb0Size, gpuDraws);
}

bool Device::BlackBoxDump(const char *reason, uint64_t stalledNs)
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
            return false;
    }
    s_lastDumpNs.store(now, std::memory_order_relaxed);
    s_dumps.fetch_add(1, std::memory_order_relaxed);
    const uint64_t newest = m_submittedSeq.load(std::memory_order_acquire);
    uint32_t crumb = 0, top = 0;
    if (m_crumbs)
    {
        crumb = *reinterpret_cast<volatile const uint32_t *>(m_crumbs.cpu);
        top = *reinterpret_cast<volatile const uint32_t *>(m_crumbs.cpu + 16);
    }
    const bool haveTop = m_topWordCount != 0;
    const uint64_t crumbSeq = CrumbSeq(crumb, newest);
    const uint32_t crumbDraw = CrumbDraw(crumb);
    Log("blackbox reason=%s stalled_ms=%llu crumb=0x%08x gpu_seq=%llu gpu_draw=%u newest_submitted=%llu "
        "completed=%llu open=%llu frames=%llu",
        reason, (unsigned long long)(stalledNs / 1000000ull), crumb, (unsigned long long)crumbSeq, crumbDraw,
        (unsigned long long)newest, (unsigned long long)m_completedSeq, (unsigned long long)m_openSeq,
        (unsigned long long)m_frames);
    Log("blackbox site=%s top=0x%08x top_seq=%llu top_draw=%u crop_seq=%llu crop_draw=%u", 
        StallVerdict(top, haveTop, crumb, newest), top, (unsigned long long)CrumbSeq(top, newest), CrumbDraw(top),
        (unsigned long long)crumbSeq, crumbDraw);
    {
        // The display's release fence for the newest acquired image: a stall
        // in front of that present list with this unsignalled is the
        // compositor, not the GPU.
        DkFence acquire{};
        uint64_t acquireSeq = 0;
        int acquireSlot = -1;
        {
            std::lock_guard<std::mutex> guard(m_acquireMutex);
            acquire = m_acquireFence;
            acquireSeq = m_acquireSeq;
            acquireSlot = m_acquireSlot;
        }
        if (acquireSeq)
        {
            const DkResult r = dkFenceWait(&acquire, 0);
            Log("blackbox acquire seq=%llu slot=%d signalled=%d", (unsigned long long)acquireSeq, acquireSlot,
                r == DkResult_Success ? 1 : r == DkResult_Timeout ? 0 : -1);
        }
    }
    for (uint64_t seq = newest >= kStatsRing ? newest - kStatsRing + 1 : 1; seq <= newest; ++seq)
    {
        const ListStats &l = m_listStats[seq % kStatsRing];
        if (l.seq != seq || seq + 3 < crumbSeq)
            continue;
        // The list's own fence, polled now (completed= above is the CPU's
        // last observation).
        int fenceDone = -1;
        const uint32_t slot = (uint32_t)(seq % kFenceRing);
        if (m_fenceSeq[slot] == seq)
        {
            DkFence fence = m_fences[slot];
            fenceDone = dkFenceWait(&fence, 0) == DkResult_Success ? 1 : 0;
        }
        Log("blackbox list seq=%llu draws=%u clears=%u uploads=%u blits=%u barriers=%u descriptors=%u acquire=%d "
            "present=%d recorded=%u fence_done=%d",
            (unsigned long long)seq, l.draws, l.clears, l.uploads, l.blits, l.barriers, l.descriptors, l.acquire,
            l.present, m_drawRecords ? m_drawRecords->Count(seq) : 0, fenceDone);
    }
    if (m_drawRecords && crumbDraw != kCrumbListEnd)
    {
        // The crumb says draws [0, crumbDraw) finished; the fault is in the
        // draws after it (a few may be in flight at once).
        const uint32_t first = crumbDraw > 2 ? crumbDraw - 2 : 0;
        uint32_t last = crumbDraw + 2 + 2 * std::max<uint32_t>(m_faultTrace, 4);
        // Through the draw the front end reached, when it is further on in
        // this list (bounded: the engine's method FIFO is shallow).
        if (haveTop && CrumbSeq(top, newest) == crumbSeq && CrumbDraw(top) < kCrumbListBegin)
            last = std::max(last, std::min(CrumbDraw(top) + 3, first + 64));
        for (uint32_t i = first; i < last; ++i)
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
            const bool topHere = haveTop && CrumbSeq(top, newest) == crumbSeq && CrumbDraw(top) == r->draw;
            Log("blackbox draw seq=%llu i=%u%s%s pass=%s native=%s vs=%08x@0x%llx ps=%08x@0x%llx rt0=0x%llx ds=0x%llx "
                "tex=%u[%s] %s prim=%u n=%u inst=%u first=%u base=%d x%u ib=0x%llx vb0=0x%llx+0x%x",
                (unsigned long long)crumbSeq, r->draw, r->draw == crumbDraw ? " <gpu" : "", topHere ? " <top" : "",
                pass ? pass : "?", NativeDrawName(r->native), r->vsHash, (unsigned long long)r->vsCode, r->psHash,
                (unsigned long long)r->psCode, (unsigned long long)r->rt0, (unsigned long long)r->depth,
                r->texCount, tex, r->indexed ? "indexed" : "array", (unsigned)r->prim, r->count, r->instances,
                r->first, r->base, (unsigned)r->gpuDraws, (unsigned long long)r->ib, (unsigned long long)r->vb0,
                r->vb0Size);
        }
    }
    if (m_drawRecords && haveTop)
    {
        // The words between the crumbs around where the front end stopped:
        // the previous draw's top crumb through the next one's.
        const uint64_t topSeq = CrumbSeq(top, newest);
        const uint32_t topDraw = CrumbDraw(top);
        const uint32_t step = std::max<uint32_t>(m_faultTrace, 1);
        if (topDraw < kCrumbListBegin)
        {
            const uint32_t from = topDraw >= step ? CrumbValue(topSeq, topDraw - step) : CrumbBegin(topSeq);
            DumpCommandWindow(topSeq, from, CrumbValue(topSeq, topDraw + step));
            const auto describe = [&](uint32_t d) {
                if (const DrawRecord *r = m_drawRecords->Get(topSeq, d))
                    DumpDescriptors(*r);
            };
            describe(topDraw);
            if (crumbSeq == topSeq && crumbDraw != topDraw && crumbDraw < kCrumbListBegin)
                describe(crumbDraw);
        }
        else if (topDraw == kCrumbListBegin)
        {
            DumpCommandWindow(topSeq, top, CrumbValue(topSeq, 0));
        }
        else
        {
            DumpCommandWindow(topSeq, top, CrumbBegin(topSeq + 1));
            DumpCommandWindow(topSeq + 1, CrumbBegin(topSeq + 1), CrumbValue(topSeq + 1, 0));
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
    return true;
}

void Device::WatchMain(void *arg)
{
    Device *d = static_cast<Device *>(arg);
    StallDetector detector;
    uint64_t lastBeat = 0;
    while (!d->m_watchStop.load(std::memory_order_relaxed))
    {
        svcSleepThread((int64_t)kWatchTickNs);
        Watchdog_Crumb(CRUMB_GPU_HB);
        const uint64_t now = armTicksToNs(armGetSystemTick());
        const uint32_t crumb = *reinterpret_cast<volatile const uint32_t *>(d->m_crumbs.cpu);
        const uint64_t newest = d->m_submittedSeq.load(std::memory_order_acquire);
        if (detector.Sample(crumb, newest, newest != 0, now) &&
            !d->BlackBoxDump("stall", detector.StalledNs(now)))
            detector.RetryReport();
        if (now - lastBeat >= kHeartbeatNs)
        {
            lastBeat = now;
            const uint32_t top = *reinterpret_cast<volatile const uint32_t *>(d->m_crumbs.cpu + 16);
            Log("gpuhb seq=%llu draw=%u newest=%llu still_ms=%llu top_seq=%llu top_draw=%u",
                (unsigned long long)CrumbSeq(crumb, newest), CrumbDraw(crumb), (unsigned long long)newest,
                (unsigned long long)(detector.StalledNs(now) / 1000000ull),
                (unsigned long long)CrumbSeq(top, newest), CrumbDraw(top));
        }
    }
}

} // namespace deko9

void Deko9_BlackBoxDump(const char *reason)
{
    deko9::Device *d = deko9::s_liveDevice.load(std::memory_order_acquire);
    if (d)
        d->BlackBoxDump(reason, 0);
}

void Deko9_SetFaultTrace(IDirect3DDevice9 *device, uint32_t interval)
{
    static_cast<deko9::Device *>(device)->SetFaultTrace(interval);
}

bool Deko9_GetFaultTraceCells(IDirect3DDevice9 *device, uint32_t *crop, uint32_t *top)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    return d->FaultTraceCells(crop, top);
}

bool Deko9_FaultTraceCommandWindow(IDirect3DDevice9 *device, uint32_t draw, char *out, size_t cap, bool *bad)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    return out && bad && d->FaultTraceCommandWindow(draw, out, cap, bad);
}

void Deko9_SetCmdChunkBytes(IDirect3DDevice9 *device, uint32_t bytes)
{
    static_cast<deko9::Device *>(device)->SetCmdChunkBytes(bytes);
}

bool Deko9_GetLastDrawRecord(IDirect3DDevice9 *device, Deko9DrawRecordInfo *out)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    return out && d->LastDrawRecord(out);
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
