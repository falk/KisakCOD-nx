// Draw census for the deko3d renderer (r_deko9DrawCensus): which materials,
// pixel shaders and blend states each pass's draws use, how many samples they
// write, and how long the GPU spends on them. See Deko9_SetDrawCensus in
// deko9_native.h. (The D3D9 entry-point call census, r_deko9Census, is
// deko9_callcensus.cpp.)

#include <switch.h>
#include "deko9_internal.h"
#include "deko9_native.h"
#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace deko9
{
namespace
{
// Blend/depth state of a bracket's last draw: alpha blend on, SRCBLEND,
// DESTBLEND, z write, alpha test.
uint32_t CensusBlendKey(const DWORD *rs)
{
    return (rs[D3DRS_ALPHABLENDENABLE] ? 1u << 16 : 0) | ((rs[D3DRS_SRCBLEND] & 0x1f) << 8) |
           ((rs[D3DRS_DESTBLEND] & 0x1f) << 3) | (rs[D3DRS_ZWRITEENABLE] ? 2u : 0) |
           (rs[D3DRS_ALPHATESTENABLE] ? 1u : 0);
}

const char *BlendName(uint32_t factor)
{
    static const char *const kNames[] = {"?",         "zero",        "one",       "srccolor",    "invsrccolor",
                                         "srcalpha",  "invsrcalpha", "dstalpha",  "invdstalpha", "dstcolor",
                                         "invdstcolor", "srcalphasat", "both", "bothinv", "blendfactor", "invblendfactor"};
    return factor < sizeof(kNames) / sizeof(kNames[0]) ? kNames[factor] : "?";
}

constexpr uint64_t kUnwritten = ~0ull;

uint64_t ReportPayload(const uint8_t *report)
{
    uint64_t v;
    std::memcpy(&v, report, 8);
    return v;
}

uint64_t ReportTimestamp(const uint8_t *report)
{
    uint64_t v;
    std::memcpy(&v, report + 8, 8);
    return v;
}
} // namespace

void Device::SetDrawCensus(uint32_t mode, uint32_t passMask)
{
    if (mode > 2)
        mode = 2;
    if (!mode)
        passMask = 0;
    if (mode && !m_censusReports) // GpuAlloc::gpu defaults to DK_GPU_ADDR_INVALID, not 0
    {
        if (!AllocMemory(POOL_DYNAMIC, kCensusSlots * kCensusSlotBytes, 256, &m_censusReports))
        {
            Fail("CENSUS", "report memory allocation failed");
            return;
        }
        m_censusSlots.resize(kCensusSlots);
    }
    if (mode != m_censusMode || passMask != m_censusPassMask)
    {
        Log("draw census %s (mode %u passes 0x%x)", mode ? "on" : "off", (unsigned)mode, (unsigned)passMask);
        if (m_censusOpen)
            CensusEnd();
        // Turning it on starts a new window.
        if (mode && !m_censusMode)
            CensusResetCounts();
    }
    m_censusMode = mode;
    m_censusPassMask = passMask;
}

void Device::CensusLabel(const char *material, const char *technique, const char *shader, const char *vertexShader,
                         const void *psObject)
{
    // Two slots: the lit pass sets up the depth-prepass material right after
    // the lit one (rb_backend R_RenderDrawSurfListMaterial); the pixel shader
    // object each draw binds picks the matching label.
    for (CensusLabelSlot &l : m_censusLabels)
    {
        if (l.ps == psObject)
        {
            l = {material, technique, shader, vertexShader, psObject};
            return;
        }
    }
    m_censusLabels[m_censusLabelNext] = {material, technique, shader, vertexShader, psObject};
    m_censusLabelNext ^= 1;
}

void Device::CensusLight(uint32_t lightIndex, uint32_t viewLights)
{
    if (!m_censusPassMask)
        return;
    if (viewLights)
    {
        m_censusLightViews += viewLights;
        m_censusLightCounted = 0;
    }
    if (lightIndex)
        ++m_censusLightPartitions;
    m_censusLight = lightIndex;
}

void Device::CensusAutoDraw(UINT primCount)
{
    const uint32_t pass = m_curPass;
    if (pass == Deko9GpuPass_PointLights)
    {
        ++m_censusLightDraws;
        if (!m_censusLight)
            ++m_censusLightNoLightDraws;
        else if (m_censusLight != m_censusLightCounted)
        {
            ++m_censusLightsDrawn;
            m_censusLightCounted = m_censusLight;
        }
    }
    if (!(m_censusPassMask & (1u << pass)))
    {
        CensusBreak();
        return;
    }
    const void *psObject = m_ps ? static_cast<const void *>(static_cast<IDirect3DPixelShader9 *>(m_ps)) : nullptr;
    const CensusLabelSlot *label = nullptr;
    for (const CensusLabelSlot &l : m_censusLabels)
    {
        if (psObject && l.ps == psObject)
            label = &l;
    }
    const char *mat = label ? label->material : nullptr;
    const char *tech = label ? label->technique : nullptr;
    if (m_censusOpen && (pass != m_censusKeyPass || mat != m_censusKeyMat || tech != m_censusKeyTech ||
                         m_ps != m_censusPs || m_vs != m_censusVs || m_psCompareMask != m_censusPsMask || m_psEarlyZ != m_censusPsEarlyZ ||
                         CensusBlendKey(m_rs) != m_censusBlend))
        CensusEnd();
    if (!m_censusOpen)
    {
        // Reports recorded here precede the draw PrepareDraw's caller records.
        CensusBegin(mat ? mat : "-", tech ? tech : "-", label && label->shader ? label->shader : "-",
                    label && label->vertexShader ? label->vertexShader : "-");
        if (!m_censusOpen)
            return; // ring full
        m_censusKeyMat = mat;
        m_censusKeyTech = tech;
    }
    CensusNoteDraw(primCount);
}

void Device::CensusBegin(const char *material, const char *technique, const char *shader, const char *vertexShader)
{
    if (!m_censusMode || m_censusSlots.size() != kCensusSlots)
        return;
    if (m_censusOpen)
        CensusEnd();
    if (m_censusHead - m_censusTail >= kCensusSlots)
    {
        ++m_censusDropped; // ring full: this bracket is not counted
        return;
    }
    const uint32_t index = (uint32_t)(m_censusHead % kCensusSlots);
    // Sentinel: a report is consumed only once the GPU overwrote it. The
    // hardware writes reports before the list's fence signals; some emulators
    // write SamplesPassed results asynchronously when its host query
    // resolves, which can be after the fence (reading then would return
    // this slot's previous lap).
    std::memset(m_censusReports.cpu + index * kCensusSlotBytes, 0xff, kCensusSlotBytes);
    const DkGpuAddr base = m_censusReports.gpu + index * kCensusSlotBytes;
    dkCmdBufReportCounter(m_cmd, DkCounter_SamplesPassed, base);
    if (m_censusMode >= 2)
        dkCmdBufReportCounter(m_cmd, DkCounter_FragmentShaderInvocations, base + 16);
    dkCmdBufReportCounter(m_cmd, DkCounter_Timestamp, base + 32);
    MarkWork();
    m_censusOpen = true;
    m_censusKeyPass = m_curPass;
    m_censusMaterial = material ? material : "?";
    m_censusTechnique = technique ? technique : "?";
    m_censusShader = shader ? shader : "?";
    m_censusVertexShader = vertexShader ? vertexShader : "?";
    m_censusDraws = m_censusPrims = m_censusBlend = m_censusEzDraws = m_censusGpuDraws = 0;
    m_censusPs = nullptr;
    m_censusVs = nullptr;
    m_censusPsMask = 0;
    m_censusPsEarlyZ = false;
}

void Device::CensusNoteDraw(UINT primCount)
{
    ++m_censusDraws;
    ++m_censusGpuDraws;
    m_censusPrims += primCount;
    m_censusPs = m_ps;
    m_censusVs = m_vs;
    m_censusPsMask = m_psCompareMask;
    m_censusPsEarlyZ = m_psEarlyZ;
    m_censusEzDraws += m_psEarlyZ;
    m_censusBlend = CensusBlendKey(m_rs);
}

void Device::CensusEnd()
{
    if (!m_censusOpen)
        return;
    m_censusOpen = false;
    const uint32_t index = (uint32_t)(m_censusHead % kCensusSlots);
    const DkGpuAddr base = m_censusReports.gpu + index * kCensusSlotBytes;
    dkCmdBufReportCounter(m_cmd, DkCounter_SamplesPassed, base + 48);
    if (m_censusMode >= 2)
        dkCmdBufReportCounter(m_cmd, DkCounter_FragmentShaderInvocations, base + 64);
    dkCmdBufReportCounter(m_cmd, DkCounter_Timestamp, base + 80);
    MarkWork();
    uint64_t psHash = 0;
    Deko9DkshStats ps{};
    if (m_censusPs)
    {
        psHash = m_censusPs->shader.Hash();
        if (const ShaderVariant *variant = m_censusPs->shader.Variant(this, m_censusPsMask, {}, m_censusPsEarlyZ))
            ps = variant->stats;
    }
    const uint64_t vsHash = m_censusVs ? m_censusVs->shader.Hash() : 0;
    char key[512];
    // Keyed by pass too (the same material in the lit and the lights pass
    // are separate rows).
    std::snprintf(key, sizeof(key), "%s|%s|%016" PRIx64 "|%016" PRIx64 "|%x|%u", m_censusMaterial, m_censusTechnique,
                  psHash, vsHash, (unsigned)m_censusBlend, (unsigned)m_censusKeyPass);
    auto found = m_censusIndex.find(key);
    uint32_t entry;
    if (found == m_censusIndex.end())
    {
        entry = (uint32_t)m_drawCensusEntries.size();
        DrawCensusEntry e;
        e.material = m_censusMaterial;
        e.technique = m_censusTechnique;
        e.shader = m_censusShader;
        e.psHash = psHash;
        e.vertexShader = m_censusVertexShader;
        e.vsHash = vsHash;
        e.ps = ps;
        e.blend = m_censusBlend;
        e.pass = m_censusKeyPass;
        m_drawCensusEntries.push_back(std::move(e));
        m_censusIndex.emplace(key, entry);
    }
    else
    {
        entry = found->second;
    }
    // The fragment-invocation reports are only valid when both ends were
    // recorded in mode 2; bit 31 of the entry index marks that.
    m_censusSlots[index] = {m_openSeq, entry | (m_censusMode >= 2 ? 0x80000000u : 0u), m_censusDraws,
                            m_censusPrims, m_censusEzDraws, m_censusGpuDraws};
    ++m_censusHead;
}

void Device::RetireCensus(uint64_t seq)
{
    while (m_censusTail != m_censusHead)
    {
        const uint32_t index = (uint32_t)(m_censusTail % kCensusSlots);
        const CensusSlot &slot = m_censusSlots[index];
        if (slot.seq > seq)
            break;
        const uint8_t *r = m_censusReports.cpu + index * kCensusSlotBytes;
        bool landed = ReportPayload(r) != kUnwritten && ReportPayload(r + 48) != kUnwritten &&
                      ReportTimestamp(r + 32) != kUnwritten && ReportTimestamp(r + 80) != kUnwritten;
        if (!landed)
        {
            if (m_openSeq - slot.seq < kCensusLateLists)
                break; // not written yet (asynchronous host query): retry at the next retire
            ++m_censusLate;
            ++m_censusTail;
            continue;
        }
        DrawCensusEntry &e = m_drawCensusEntries[slot.entry & 0x7fffffffu];
        ++e.brackets;
        e.draws += slot.draws;
        e.prims += slot.prims;
        e.ezDraws += slot.ezDraws;
        e.gpuDraws += slot.gpuDraws;
        const uint64_t s0 = ReportPayload(r), s1 = ReportPayload(r + 48);
        if (s1 >= s0)
            e.samples += s1 - s0;
        if (slot.entry & 0x80000000u)
        {
            // Not gating retirement: an emulator may never write this
            // counter (some implement SamplesPassed only).
            const uint64_t f0 = ReportPayload(r + 16), f1 = ReportPayload(r + 64);
            if (f0 != kUnwritten && f1 != kUnwritten && f1 >= f0)
                e.fsInv += f1 - f0;
        }
        const uint64_t t0 = ReportTimestamp(r + 32), t1 = ReportTimestamp(r + 80);
        if (t1 >= t0)
            e.ns += dkTimestampToNs(t1 - t0);
        ++m_censusTail;
    }
}

void Device::CensusReport(const char *label, uint32_t width, uint32_t height)
{
    CompletedSeq(); // retire what the GPU has finished (the rest counts next time)
    RetireCensus(m_completedSeq); // reports that landed after their fence
    if (!label)
    {
        // Reset only: discard what was gathered since the last report.
        CensusResetCounts();
        return;
    }
    // Estimated pixel-shader cost of a row: shaded pixels x the pixel
    // shader's static instruction count. Shaded pixels are the fragment
    // shader invocations where the counter ran (mode 2, hardware), else the
    // samples passed (where only samples passed exists: excludes depth-rejected and discarded
    // fragments, so it is a lower bound).
    const double frames = m_drawCensusFrames ? (double)m_drawCensusFrames : 1.0;
    const double pixels = (width && height) ? (double)width * height : 1.0;
    auto shaded = [](const DrawCensusEntry &e) { return (double)(e.fsInv ? e.fsInv : e.samples); };
    auto cost = [&](const DrawCensusEntry &e) { return shaded(e) * e.ps.instrs; };
    struct PassSum
    {
        uint64_t draws = 0, prims = 0, samples = 0, fsInv = 0, ns = 0, gpuDraws = 0;
        double cost = 0;
        uint32_t entries = 0;
    } passes[Deko9GpuPass_Count];
    std::vector<uint32_t> order;
    PassSum all;
    for (uint32_t i = 0; i < m_drawCensusEntries.size(); ++i)
    {
        const DrawCensusEntry &e = m_drawCensusEntries[i];
        if (!e.brackets || e.pass >= Deko9GpuPass_Count)
            continue;
        order.push_back(i);
        for (PassSum *p : {&passes[e.pass], &all})
        {
            p->draws += e.draws;
            p->gpuDraws += e.gpuDraws;
            p->prims += e.prims;
            p->samples += e.samples;
            p->fsInv += e.fsInv;
            p->ns += e.ns;
            p->cost += cost(e);
            ++p->entries;
        }
    }
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const DrawCensusEntry &x = m_drawCensusEntries[a], &y = m_drawCensusEntries[b];
        const double cx = cost(x), cy = cost(y);
        return cx != cy ? cx > cy : x.ns > y.ns;
    });
    const double passFrames =
        m_passFramesTotal > m_censusPassFrames0 ? (double)(m_passFramesTotal - m_censusPassFrames0) : 0.0;
    Log("dcensus label=%s frames=%llu rt=%ux%u entries=%u draws=%.1f prims=%.0f px=%.0f overdraw=%.3f fsinv=%.0f "
        "gpu_us=%.1f mpxi=%.2f dropped=%llu late=%llu mode=%u passes=0x%x (per frame)",
        label, (unsigned long long)m_drawCensusFrames, (unsigned)width, (unsigned)height, all.entries,
        all.draws / frames, all.prims / frames, all.samples / frames, all.samples / frames / pixels,
        all.fsInv / frames, all.ns / 1e3 / frames, all.cost / frames / 1e6, (unsigned long long)m_censusDropped,
        (unsigned long long)m_censusLate, (unsigned)m_censusMode, (unsigned)m_censusPassMask);
    for (uint32_t p = 0; p < Deko9GpuPass_Count; ++p)
    {
        const PassSum &s = passes[p];
        const double passMs = passFrames ? (m_passNsTotal[p] - m_censusPassNs0[p]) / 1e6 / passFrames : 0.0;
        if (!s.entries && passMs == 0.0)
            continue;
        // gpupass_ms: the pass's r_deko9GpuPasses time over the window (with
        // the census's serializing brackets in it; 0 when that is off).
        // gdraws: deko3d draw packets (draws = API calls; a ranged call is
        // one API draw and one packet per range.
        Log("dcensus label=%s pass=%s draws=%.1f prims=%.0f px=%.0f overdraw=%.3f fsinv=%.0f gpu_us=%.1f mpxi=%.2f "
            "entries=%u gpupass_ms=%.3f gdraws=%.1f",
            label, Deko9_GpuPassName(p), s.draws / frames, s.prims / frames, s.samples / frames,
            s.samples / frames / pixels, s.fsInv / frames, s.ns / 1e3 / frames, s.cost / frames / 1e6, s.entries,
            passMs, s.gpuDraws / frames);
    }
    // Lights pass (R_DrawPointLitSurfs): point lights in view, partitions
    // drawn, partitions with at least one draw, draws, and draws outside a
    // partition. Per frame.
    Log("dcensus label=%s lights inview=%.2f partitions=%.2f drawn=%.2f draws=%.1f unowned_draws=%.1f", label,
        m_censusLightViews / frames, m_censusLightPartitions / frames, m_censusLightsDrawn / frames,
        m_censusLightDraws / frames, m_censusLightNoLightDraws / frames);
    // Rows: the global top 40 by cost, each pass's top 12 by cost and the
    // top 12 by serialized GPU time; rank is the global cost rank.
    std::vector<uint8_t> pick(order.size(), 0);
    uint32_t perPass[Deko9GpuPass_Count]{};
    for (uint32_t r = 0; r < order.size(); ++r)
    {
        const DrawCensusEntry &e = m_drawCensusEntries[order[r]];
        if (r < 40 || perPass[e.pass] < 12)
            pick[r] = 1;
        ++perPass[e.pass];
    }
    std::vector<uint32_t> byTime(order.size());
    for (uint32_t r = 0; r < order.size(); ++r)
        byTime[r] = r;
    std::sort(byTime.begin(), byTime.end(), [&](uint32_t a, uint32_t b) {
        return m_drawCensusEntries[order[a]].ns > m_drawCensusEntries[order[b]].ns;
    });
    for (uint32_t r = 0; r < byTime.size() && r < 12; ++r)
        pick[byTime[r]] = 1;
    for (uint32_t r = 0; r < order.size(); ++r)
    {
        if (!pick[r])
            continue;
        const DrawCensusEntry &e = m_drawCensusEntries[order[r]];
        Log("dcensus label=%s rank=%u pass=%s mat=%s tech=%s psname=%s ps=%016" PRIx64 " gprs=%u instrs=%u "
            "vsname=%s vs=%016" PRIx64 " "
            "blend=%s:%s:%s zw=%u at=%u draws=%.2f prims=%.1f px=%.0f fsinv=%.0f gpu_us=%.1f ez=%.2f mpxi=%.3f",
            label, r + 1, Deko9_GpuPassName(e.pass), e.material.c_str(), e.technique.c_str(), e.shader.c_str(),
            e.psHash, (unsigned)e.ps.gprs, (unsigned)e.ps.instrs, e.vertexShader.c_str(), e.vsHash, (e.blend >> 16) ? "on" : "off",
            BlendName((e.blend >> 8) & 0x1f), BlendName((e.blend >> 3) & 0x1f), (unsigned)((e.blend >> 1) & 1),
            (unsigned)(e.blend & 1), e.draws / frames, e.prims / frames, e.samples / frames, e.fsInv / frames,
            e.ns / 1e3 / frames, e.ezDraws / frames, cost(e) / frames / 1e6);
    }
    CensusResetCounts();
}

void Device::CensusResetCounts()
{
    for (DrawCensusEntry &e : m_drawCensusEntries)
        e.brackets = e.draws = e.prims = e.samples = e.fsInv = e.ns = e.ezDraws = e.gpuDraws = 0;
    m_drawCensusFrames = 0;
    m_censusDropped = m_censusLate = 0;
    m_censusLightViews = m_censusLightPartitions = m_censusLightsDrawn = 0;
    m_censusLightDraws = m_censusLightNoLightDraws = 0;
    std::memcpy(m_censusPassNs0, m_passNsTotal, sizeof(m_censusPassNs0));
    m_censusPassFrames0 = m_passFramesTotal;
}

} // namespace deko9

void Deko9_SetDrawCensus(IDirect3DDevice9 *device, uint32_t mode, uint32_t passMask)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    // 0 = every pass.
    d->SetDrawCensus(mode, mode ? (passMask ? passMask : (1u << Deko9GpuPass_Count) - 1) : 0u);
}

void Deko9_CensusLabel(IDirect3DDevice9 *device, const char *material, const char *technique, const char *shader,
                       const char *vertexShader, const void *pixelShader)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->CensusLabel(material, technique, shader, vertexShader, pixelShader);
}

void Deko9_CensusLight(IDirect3DDevice9 *device, uint32_t lightIndex, uint32_t viewLights)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->CensusLight(lightIndex, viewLights);
}

void Deko9_CensusReport(IDirect3DDevice9 *device, const char *label, uint32_t width, uint32_t height)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->CensusReport(label, width, height);
}

void Deko9_GetGpuPassTotals(IDirect3DDevice9 *device, uint64_t ns[Deko9GpuPass_Count], uint64_t *frames)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->CompletedSeq();
    for (uint32_t i = 0; i < Deko9GpuPass_Count; ++i)
        ns[i] = d->m_passNsTotal[i];
    *frames = d->m_passFramesTotal;
}
