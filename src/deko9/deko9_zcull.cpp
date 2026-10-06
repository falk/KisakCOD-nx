// Zcull control and measurement for the deko3d renderer (r_deko9ZcullStats).
// Zcull itself is always on -- there is
// no toggle; only the stats model below can be switched.
//
// deko3d owns the zcull setup: the queue binds a zcull context
// (nvGpuChannelZcullBind), dkCmdBufBindRenderTargets programs the region for
// the depth target (ZcullWidth/ImageSizeAliquots, zeta type 2 = depth only)
// and invalidates it when the depth address changes (MME
// ConditionalZcullInvalidate), and dkCmdBufClearDepthStencil sets
// ZcullClearDepth before the clear.
//
// With stats on, the device keeps a CPU model of the region per pass (see
// Device::ZcullPassStats) and, when r_deko9GpuPasses marks passes, records a
// DkCounter_ZcullStats report (four 32-bit hardware counters) at every pass
// mark; the per-pass deltas are logged every 60 frames as `DEKO9 zcull`.
// Some emulators emulate neither zcull nor these counters (ZcullBind is a
// stub, the report is never written), so there the hardware part reads
// `hw=unsupported` and only the CPU model is meaningful.

#include <switch.h>
#include "deko9_internal.h"
#include "deko9_native.h"
#include <cstdio>
#include <cstring>

namespace deko9
{
namespace
{
constexpr uint32_t kZcullUnwritten = 0xffffffffu; // sentinel the GPU overwrites
} // namespace

void Device::ApplyZcullSettings()
{
    const bool wantStats = m_zcullStatsWanted.load(std::memory_order_relaxed);
    if (wantStats != m_zcullStats)
    {
        m_zcullStats = wantStats;
        for (ZcullPassStats &s : m_zcullPass)
            s = {};
        m_zcullFrames = m_zcullHwMissing = 0;
        m_zcullPrevValid = false;
        m_zcullValid = false; // unknown until the next depth clear
        m_zcullAddr = 0;
        m_zcullDir = 0;
        Log("zcull stats %s", wantStats ? "on" : "off");
    }
    else if (m_zcullStats && ++m_zcullFrames == 60)
    {
        ReportZcull();
    }
}

void Device::ZcullNoteTargets()
{
    if (!m_depthStencil)
        return; // deko3d leaves the region (and its tracked address) alone
    const DkGpuAddr addr = m_depthStencil->Store()->memory.gpu;
    if (addr == m_zcullAddr)
        return;
    m_zcullAddr = addr;
    m_zcullValid = false;
    m_zcullDir = 0;
    ++m_zcullPass[m_curPass].binds;
}

void Device::ZcullNoteClear(bool full)
{
    ZcullPassStats &s = m_zcullPass[m_curPass];
    if (full)
    {
        ++s.fullClears;
        m_zcullValid = true;
        m_zcullDir = 0;
    }
    else
    {
        ++s.partialClears;
    }
}

void Device::ZcullNoteDraw()
{
    if (!m_depthStencil || m_rs[D3DRS_ZENABLE] == D3DZB_FALSE)
        return;
    ZcullPassStats &s = m_zcullPass[m_curPass];
    ++s.ztest;
    if (!m_zcullValid)
        ++s.invalid;
    const DWORD func = m_rs[D3DRS_ZFUNC];
    uint8_t dir = 0;
    if (func == D3DCMP_LESS || func == D3DCMP_LESSEQUAL)
        dir = 1;
    else if (func == D3DCMP_GREATER || func == D3DCMP_GREATEREQUAL)
        dir = 2;
    if (dir)
    {
        if (m_zcullDir && dir != m_zcullDir)
            ++s.flips;
        m_zcullDir = dir;
    }
    if (func == D3DCMP_ALWAYS && m_rs[D3DRS_ZWRITEENABLE])
        ++s.alwaysWrites;
}

void Device::ZcullRecordMark(uint32_t slot)
{
    uint32_t *words = reinterpret_cast<uint32_t *>(m_zcullStamps.cpu + slot * 16);
    for (int i = 0; i < 4; ++i)
        words[i] = kZcullUnwritten;
    dkCmdBufReportCounter(Rec(), DkCounter_ZcullStats, m_zcullStamps.gpu + slot * 16);
}

void Device::ZcullRetireMark(uint32_t slot, const PassMark &mark)
{
    uint32_t words[4];
    std::memcpy(words, m_zcullStamps.cpu + slot * 16, sizeof(words));
    if (words[0] == kZcullUnwritten && words[1] == kZcullUnwritten && words[2] == kZcullUnwritten &&
        words[3] == kZcullUnwritten)
    {
        ++m_zcullHwMissing;
        m_zcullPrevValid = false;
        return;
    }
    // The counters accumulate per channel; attribute the delta since the
    // previous mark to the pass that ran in between (not to the gap after a
    // list's close mark).
    if (m_zcullPrevValid && m_zcullPrevPass != kPassClose && m_zcullPrevPass < Deko9GpuPass_Count)
    {
        ZcullPassStats &s = m_zcullPass[m_zcullPrevPass];
        for (int i = 0; i < 4; ++i)
            s.hw[i] += (uint32_t)(words[i] - m_zcullPrev[i]);
        ++s.hwIntervals;
    }
    std::memcpy(m_zcullPrev, words, sizeof(words));
    m_zcullPrevPass = mark.pass;
    m_zcullPrevValid = true;
}

void Device::ReportZcull()
{
    const double frames = m_zcullFrames ? (double)m_zcullFrames : 1.0;
    uint64_t hwIntervals = 0;
    for (const ZcullPassStats &s : m_zcullPass)
        hwIntervals += s.hwIntervals;
    const char *hw = hwIntervals ? "counters" : (m_zcullHwMissing ? "unsupported" : (GpuPassesOn() ? "none" : "needs-gpupasses"));
    Log("zcull frames=%llu hw=%s unwritten=%llu (per frame; hw z0..z3 = DkCounter_ZcullStats word deltas)",
        (unsigned long long)m_zcullFrames, hw, (unsigned long long)m_zcullHwMissing);
    for (uint32_t p = 0; p < Deko9GpuPass_Count; ++p)
    {
        const ZcullPassStats &s = m_zcullPass[p];
        if (!s.ztest && !s.binds && !s.fullClears && !s.partialClears && !s.hwIntervals)
            continue;
        char line[384];
        int len = std::snprintf(line, sizeof(line),
                                "zcull pass=%s ztest=%.1f invalid=%.1f binds=%.2f clears=%.2f/%.2f flips=%.2f aw=%.1f",
                                Deko9_GpuPassName(p), s.ztest / frames, s.invalid / frames, s.binds / frames,
                                s.fullClears / frames, s.partialClears / frames, s.flips / frames,
                                s.alwaysWrites / frames);
        if (s.hwIntervals && len > 0 && len < (int)sizeof(line))
            std::snprintf(line + len, sizeof(line) - len, " z0=%.0f z1=%.0f z2=%.0f z3=%.0f", s.hw[0] / frames,
                          s.hw[1] / frames, s.hw[2] / frames, s.hw[3] / frames);
        Log("%s", line);
    }
    for (ZcullPassStats &s : m_zcullPass)
        s = {};
    m_zcullFrames = m_zcullHwMissing = 0;
}

} // namespace deko9

void Deko9_SetZcullStats(IDirect3DDevice9 *device, bool enable)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetZcullStats(enable);
}

bool Deko9_GetZcullPassStats(IDirect3DDevice9 *device, uint32_t pass, Deko9ZcullPassStats *out)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    const deko9::Device::ZcullPassStats *s = d->ZcullPass(pass);
    if (!d->ZcullStatsOn() || !s || pass >= Deko9GpuPass_Count || !out)
        return false;
    *out = {s->ztest, s->invalid, s->binds, s->fullClears, s->partialClears, s->flips, s->alwaysWrites};
    return true;
}
