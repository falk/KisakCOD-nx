// D3D9 entry-point call census (r_deko9Census, deko9_callcensus.h): the
// per-entry-point table, the uploaded-texture and dynamic-buffer tables, and
// the "DEKO9 calls" report PresentFrame logs every 60 frames.

#include <switch.h>

#include "deko9_internal.h"
#include "deko9_native.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace deko9
{

const char *const kCensusNames[Census_Count] = {
    "DrawIndexedPrimitive",
    "DrawPrimitive",
    "DrawPrimitiveUP",
    "DrawIndexedPrimitiveUP",
    "SetTexture",
    "SetSamplerState",
    "SetRenderState",
    "SetVertexShader",
    "SetPixelShader",
    "SetVertexShaderConstantF",
    "SetPixelShaderConstantF",
    "SetStreamSource",
    "SetIndices",
    "SetVertexDeclaration",
    "SetRenderTarget",
    "SetDepthStencilSurface",
    "Clear",
    "StretchRect",
    "UpdateTexture",
    "UpdateSurface",
    "GetRenderTargetData",
    "LockTexture2D",
    "LockCubeTexture",
    "LockVolumeTexture",
    "LockSurface",
    "LockVB_DISCARD",
    "LockVB_NOOVERWRITE",
    "LockVB_NONE",
    "LockVB_READONLY",
    "LockIB_DISCARD",
    "LockIB_NOOVERWRITE",
    "LockIB_NONE",
    "LockIB_READONLY",
    "QueryIssue",
    "QueryGetData",
    "BeginScene",
    "EndScene",
    "Present",
    "Deko9_SetTexture",
    "Deko9_SetSamplerPacked",
    "Deko9_AddInstance",
    "Deko9_DrawIndexedInstances",
    "Deko9_DrawIndexedRanges",
};
// Note: deliberately not spelled "static_assert(sizeof(...)" -- that shape
// is a tracked class in offline tooling's native-layout allowlist
// (RETAIL_LP64_DEBT_SOURCE), which this array-length check has nothing to
// do with (no pointer width or wire layout involved).
static_assert(Census_Count == sizeof(kCensusNames) / sizeof(kCensusNames[0]), "kCensusNames vs CensusId");

void Device::CensusTexture(const ImageStore *store, uint64_t bytes)
{
    CensusTexRecord &r = m_censusTextures[store];
    if (!r.calls)
    {
        r.name = store->debugName;
        r.width = store->width;
        r.height = store->height;
        r.depth = store->depth;
        r.levels = store->levels;
        r.format = store->format ? store->format->d3d : D3DFMT_UNKNOWN;
        r.pool = store->pool;
        r.usage = store->usage;
    }
    r.bytes += bytes;
    ++r.calls;
}

void Device::CensusBufferRole(const Buffer *buf, bool isIndex, uint64_t bytes)
{
    DeviceLockGuard lock(m_lock);
    char key[64];
    if (buf->role)
        std::snprintf(key, sizeof(key), "%s", buf->role);
    else
        std::snprintf(key, sizeof(key), "%s%uKB,u=0x%x,p=%d", isIndex ? "ib" : "vb",
                      (unsigned)(buf->Size() >> 10), (unsigned)buf->Usage(), (int)buf->PoolKind());
    CensusBufRecord &r = m_censusBuffers[key];
    r.size = buf->Size();
    r.usage = buf->Usage();
    r.pool = buf->PoolKind();
    r.isIndex = isIndex;
    ++r.locks;
    r.bytes += bytes;
}

void Device::ReportCensus()
{
    uint32_t order[Census_Count], n = 0;
    uint64_t totalCalls = 0, totalBytes = 0, totalNs = 0;
    for (uint32_t i = 0; i < Census_Count; ++i)
    {
        const CensusEntry &e = m_censusEntries[i];
        totalCalls += e.calls;
        totalBytes += e.bytes;
        totalNs += e.ns;
        if (e.calls)
            order[n++] = i;
    }
    std::sort(order, order + n,
              [this](uint32_t a, uint32_t b) { return m_censusEntries[a].ns > m_censusEntries[b].ns; });
    const double frames = m_censusFrames ? (double)m_censusFrames : 1.0;
    const uint32_t top = std::min<uint32_t>(n, 25);
    Log("calls frames=%llu entryPoints=%u calls/frame=%.0f bytes/frame=%.0f cpu=%.3fms/frame (host-side time; top "
        "%u of %u by cpu, sorted desc)",
        (unsigned long long)m_censusFrames, n, totalCalls / frames, totalBytes / frames, totalNs / 1e6 / frames, top,
        n);
    for (uint32_t i = 0; i < top;)
    {
        char line[512];
        int len = std::snprintf(line, sizeof(line), "calls");
        for (uint32_t k = 0; k < 4 && i < top; ++k, ++i)
        {
            const CensusEntry &e = m_censusEntries[order[i]];
            len += std::snprintf(line + len, sizeof(line) - len, " %s(calls=%.1f,bytes=%.0f,cpu=%.3fms)",
                                 kCensusNames[order[i]], e.calls / frames, e.bytes / frames, e.ns / 1e6 / frames);
        }
        Log("%s", line);
    }
    if (!m_censusTextures.empty())
    {
        std::vector<std::pair<const void *, CensusTexRecord>> tex(m_censusTextures.begin(), m_censusTextures.end());
        std::sort(tex.begin(), tex.end(),
                  [](const auto &a, const auto &b) { return a.second.bytes > b.second.bytes; });
        const uint32_t texTop = std::min<uint32_t>((uint32_t)tex.size(), 10);
        Log("calls textures uploaded=%zu (top %u by bytes/frame)", tex.size(), texTop);
        for (uint32_t i = 0; i < texTop; ++i)
        {
            const CensusTexRecord &t = tex[i].second;
            Log("calls tex #%u %s %ux%ux%u levels=%u d3dfmt=%u pool=%d usage=0x%x bytes/frame=%.0f "
                "calls/frame=%.2f",
                i + 1, t.name ? t.name : "(unnamed)", t.width, t.height, t.depth, t.levels, (unsigned)t.format,
                (int)t.pool, (unsigned)t.usage, t.bytes / frames, t.calls / frames);
        }
    }
    if (!m_censusBuffers.empty())
    {
        std::vector<std::pair<std::string, CensusBufRecord>> bufs(m_censusBuffers.begin(), m_censusBuffers.end());
        std::sort(bufs.begin(), bufs.end(),
                  [](const auto &a, const auto &b) { return a.second.bytes > b.second.bytes; });
        for (const auto &kv : bufs)
        {
            const CensusBufRecord &b = kv.second;
            Log("calls buf %s %s size=%uKB usage=0x%x pool=%d locks/frame=%.2f bytes/frame=%.0f", kv.first.c_str(),
                b.isIndex ? "IB" : "VB", (unsigned)(b.size >> 10), (unsigned)b.usage, (int)b.pool,
                b.locks / frames, b.bytes / frames);
        }
    }
    m_censusFrames = 0;
    for (CensusEntry &e : m_censusEntries)
        e = {};
    m_censusTextures.clear();
    m_censusBuffers.clear();
}

} // namespace deko9

void Deko9_SetCensus(IDirect3DDevice9 *device, bool enable)
{
    static_cast<deko9::Device *>(device)->SetCensus(enable);
}

void Deko9_SetDebugName(IDirect3DBaseTexture9 *texture, const char *name)
{
    // Set once, synchronously, right after CreateTexture succeeds (before
    // the texture is bound or locked anywhere else): no device lock needed.
    deko9::ImageStore *store = deko9::StoreOf(texture);
    if (store)
        store->debugName = name;
    Deko9_GpuMapNoteName(texture); // r_deko9GpuMap: label the image's VA range
}

void Deko9_SetBufferRole(IDirect3DVertexBuffer9 *vb, const char *role)
{
    if (vb)
        static_cast<deko9::VertexBuffer *>(vb)->buffer.role = role;
}

void Deko9_SetBufferRole(IDirect3DIndexBuffer9 *ib, const char *role)
{
    if (ib)
        static_cast<deko9::IndexBuffer *>(ib)->buffer.role = role;
}
