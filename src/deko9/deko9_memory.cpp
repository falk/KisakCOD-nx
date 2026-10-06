#include <switch.h> // armGetSystemTick/armTicksToNs for CensusScope (deko9_internal.h)

#include "deko9_internal.h"

#include <algorithm>

namespace deko9
{

bool Heap::Alloc(DkDevice device, uint32_t size, uint32_t align, GpuAlloc *out)
{
    if (!size)
        return false;
    align = std::max<uint32_t>(align, 256);
    size = AlignUp(size, 256);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        for (uint32_t c = 0; c < m_chunks.size(); ++c)
        {
            Chunk &chunk = m_chunks[c];
            for (auto it = chunk.freeSpans.begin(); it != chunk.freeSpans.end(); ++it)
            {
                const uint32_t spanStart = it->first;
                const uint32_t spanSize = it->second;
                const uint32_t start = AlignUp(spanStart, align);
                if (start < spanStart || start - spanStart + (uint64_t)size > spanSize)
                    continue;
                const uint32_t spanEnd = spanStart + spanSize;
                chunk.freeSpans.erase(it);
                if (start > spanStart)
                    chunk.freeSpans[spanStart] = start - spanStart;
                if (start + size < spanEnd)
                    chunk.freeSpans[start + size] = spanEnd - (start + size);
                out->block = chunk.block;
                out->offset = start;
                out->size = size;
                out->pool = m_pool;
                out->chunk = c;
                out->gpu = dkMemBlockGetGpuAddr(chunk.block) + start;
                void *cpu = (m_flags & DkMemBlockFlags_CpuAccessMask) ? dkMemBlockGetCpuAddr(chunk.block) : nullptr;
                out->cpu = cpu ? static_cast<uint8_t *>(cpu) + start : nullptr;
                m_inUse += size;
                return true;
            }
        }
        if (attempt)
            break;
        // Grow: one chunk of at least the default size (whole allocation
        // for large requests), rounded to the memblock alignment.
        const uint32_t chunkSize =
            AlignUp(std::max(m_chunkSize, size + align + m_tailReserve), DK_MEMBLOCK_ALIGNMENT);
        DkMemBlockMaker maker;
        dkMemBlockMakerDefaults(&maker, device, chunkSize);
        maker.flags = m_flags;
        DkMemBlock block = dkMemBlockCreate(&maker);
        if (!block)
            return false;
        // GPU MMU faults are reported by address only (erpt GpuErrorFaultAddress);
        // this map says which pool/chunk an address belongs to.
        // Image memblocks also get a generic and a compressed alias from the
        // kernel (deko3d MemBlock::initialize), mapped right after the pitch
        // range on hardware; offline tooling infers them, and
        // "DEKO9 gpumap image" lines (r_deko9GpuMap) give the exact VA each
        // image is addressed through.
        Log("memblock pool=%d chunk=%zu flags=0x%x gpu=0x%llx-0x%llx size=0x%x", (int)m_pool, m_chunks.size(), m_flags,
            (unsigned long long)dkMemBlockGetGpuAddr(block),
            (unsigned long long)dkMemBlockGetGpuAddr(block) + chunkSize, chunkSize);
        if (m_events)
            m_events->Push({dkMemBlockGetGpuAddr(block), m_openSeq ? *m_openSeq : 0, chunkSize, kGpuEventBlock,
                            (uint8_t)m_pool, (uint16_t)m_chunks.size()});
        Chunk chunk{block, chunkSize, {}};
        chunk.freeSpans[0] = chunkSize - m_tailReserve;
        m_chunks.push_back(std::move(chunk));
        m_reserved += chunkSize;
        if (m_flags & DkMemBlockFlags_Image)
        {
            DkMemBlockMaker sm;
            dkMemBlockMakerDefaults(&sm, device, fr::kSentinelBytes);
            sm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached;
            if (DkMemBlock sb = dkMemBlockCreate(&sm))
            {
                Sentinel s{sb, static_cast<uint32_t *>(dkMemBlockGetCpuAddr(sb)), dkMemBlockGetGpuAddr(sb),
                           dkMemBlockGetGpuAddr(block), chunkSize, 0};
                fr::FillSentinel(s.cpu, fr::kSentinelBytes / 4);
                m_sentinels.push_back(s);
                Log("image sentinel %zu gpu=0x%llx-0x%llx after image memblock gpu=0x%llx (+aliases to 0x%llx)",
                    m_sentinels.size() - 1, (unsigned long long)s.gpu,
                    (unsigned long long)s.gpu + fr::kSentinelBytes, (unsigned long long)s.imageGpu,
                    (unsigned long long)s.imageGpu + 3ull * chunkSize);
            }
        }
    }
    return false;
}

void Heap::Free(const GpuAlloc &alloc)
{
    if (!alloc.block)
        return;
    Chunk &chunk = m_chunks[alloc.chunk];
    uint32_t start = alloc.offset;
    uint32_t size = alloc.size;
    auto next = chunk.freeSpans.lower_bound(start);
    if (next != chunk.freeSpans.end() && start + size == next->first)
    {
        size += next->second;
        next = chunk.freeSpans.erase(next);
    }
    if (next != chunk.freeSpans.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start)
        {
            start = prev->first;
            size += prev->second;
            chunk.freeSpans.erase(prev);
        }
    }
    chunk.freeSpans[start] = size;
    m_inUse -= alloc.size;
}

bool Heap::Contains(DkGpuAddr addr, uint32_t size) const
{
    for (const Chunk &chunk : m_chunks)
    {
        const DkGpuAddr base = dkMemBlockGetGpuAddr(chunk.block);
        if (addr >= base && addr + size <= base + chunk.size)
            return true;
    }
    return false;
}

bool Heap::ContainsWithAliases(DkGpuAddr addr) const
{
    for (const Chunk &chunk : m_chunks)
    {
        const DkGpuAddr base = dkMemBlockGetGpuAddr(chunk.block);
        if (addr >= base && addr < base + 3ull * chunk.size)
            return true;
    }
    return false;
}

void FillCanary(uint8_t *tail)
{
    std::memset(tail, kCanaryByte, kCanaryBytes);
}

bool CheckCanary(const uint8_t *tail)
{
    for (uint32_t i = 0; i < kCanaryBytes; ++i)
    {
        if (tail[i] != kCanaryByte)
            return false;
    }
    return true;
}

bool Heap::CheckSentinel(uint32_t index, char *report, size_t reportSize)
{
    if (m_sentinels.empty())
        return false;
    Sentinel &s = m_sentinels[index % m_sentinels.size()];
    const uint32_t words = fr::kSentinelBytes / 4;
    uint32_t first = words;
    const uint32_t dirty = fr::CheckSentinel(s.cpu, words, &first);
    if (!dirty)
        return false;
    ++s.hits;
    const uint32_t *w = s.cpu + first;
    const uint32_t n = words - first;
    std::snprintf(report, reportSize,
                  "sentinel %u gpu=0x%llx (after image memblock gpu=0x%llx size=0x%x, aliases to 0x%llx) hit %u: "
                  "%u of %u sampled words written, first at +0x%x (gpu 0x%llx): %08x %08x %08x %08x %08x %08x %08x "
                  "%08x; a GPU write ran past the end of an image memblock",
                  (unsigned)(index % m_sentinels.size()), (unsigned long long)s.gpu, (unsigned long long)s.imageGpu,
                  s.imageSize, (unsigned long long)s.imageGpu + 3ull * s.imageSize, s.hits, dirty,
                  words / fr::kSentinelStrideWords, first * 4, (unsigned long long)s.gpu + first * 4ull, w[0],
                  n > 1 ? w[1] : 0, n > 2 ? w[2] : 0, n > 3 ? w[3] : 0, n > 4 ? w[4] : 0, n > 5 ? w[5] : 0,
                  n > 6 ? w[6] : 0, n > 7 ? w[7] : 0);
    fr::FillSentinel(s.cpu, words);
    return true;
}

void Heap::Destroy()
{
    for (Sentinel &s : m_sentinels)
        dkMemBlockDestroy(s.block);
    m_sentinels.clear();
    for (Chunk &chunk : m_chunks)
        dkMemBlockDestroy(chunk.block);
    m_chunks.clear();
    m_inUse = m_reserved = 0;
}

bool Device::AllocMemory(Pool pool, uint32_t size, uint32_t align, GpuAlloc *out)
{
    if (m_heaps[pool]->Alloc(m_dk, size, align, out))
    {
        NoteGpuEvent(kGpuEventAlloc, out->gpu, out->size, (uint8_t)pool, (uint16_t)out->chunk);
        return true;
    }
    // Retry once after retiring whatever the GPU has finished with.
    CollectCompleted();
    if (m_heaps[pool]->Alloc(m_dk, size, align, out))
    {
        NoteGpuEvent(kGpuEventAlloc, out->gpu, out->size, (uint8_t)pool, (uint16_t)out->chunk);
        return true;
    }
    Fail("OUT_OF_MEMORY", "pool=%u size=%u inUse=%llu reserved=%llu", (unsigned)pool, size,
         (unsigned long long)m_heaps[pool]->BytesInUse(),
         (unsigned long long)m_heaps[pool]->BytesReserved());
    return false;
}

void Device::FreeMemoryAfter(const GpuAlloc &alloc, uint64_t seq)
{
    if (!alloc.block)
        return;
    // Explicit lifetime: memory is returned to its heap only once the list
    // that last referenced it (seq) has completed on the GPU.
    const bool now = seq <= m_completedSeq;
    m_gpuEvents.Push({alloc.gpu, seq, alloc.size, now ? (uint8_t)kGpuEventFreeNow : (uint8_t)kGpuEventFreeDeferred,
                      (uint8_t)alloc.pool, (uint16_t)alloc.chunk});
    if (now)
        m_heaps[alloc.pool]->Free(alloc);
    else
        m_deferredFrees.push_back({seq, alloc});
}

bool Device::FrameAlloc(uint64_t frame, uint32_t bytes, uint32_t align, ArenaSpan *out)
{
    return m_frameArena.Alloc(ThreadTag(), frame, bytes, align, out);
}

bool Device::AllocUpload(uint32_t size, uint32_t align, GpuAlloc *out)
{
    align = std::max<uint32_t>(align, 256);
    if (size > kUploadChunk / 2)
    {
        // Large one-off upload: dedicated memory freed after the open list.
        if (!AllocMemory(POOL_BUFFER, size, align, out))
            return false;
        FreeMemoryAfter(*out, m_openSeq);
        return true;
    }
    if (m_uploadChunks.empty() ||
        AlignUp(m_uploadChunks.back().used, align) + size > m_uploadChunks.back().mem.size)
    {
        UploadChunk chunk{};
        if (!m_freeUploadChunks.empty())
        {
            chunk = m_freeUploadChunks.back();
            m_freeUploadChunks.pop_back();
        }
        else if (!AllocMemory(POOL_BUFFER, kUploadChunk, 256, &chunk.mem))
        {
            return false;
        }
        chunk.used = 0;
        chunk.seq = m_openSeq;
        m_uploadChunks.push_back(chunk);
    }
    UploadChunk &chunk = m_uploadChunks.back();
    chunk.seq = m_openSeq;
    const uint32_t offset = AlignUp(chunk.used, align);
    *out = chunk.mem;
    out->offset += offset;
    out->size = size;
    out->gpu += offset;
    out->cpu += offset;
    chunk.used = offset + AlignUp(size, 256);
    return true;
}

uint32_t Device::AllocImageDescriptor(const DkImageView &view)
{
    if (m_freeImageDescriptors.empty())
        CollectCompleted();
    if (m_freeImageDescriptors.empty())
    {
        Fail("DESCRIPTORS_EXHAUSTED", "image descriptors=%u", kImageDescriptors);
        return UINT32_MAX;
    }
    const uint32_t slot = m_freeImageDescriptors.back();
    m_freeImageDescriptors.pop_back();
    DkImageDescriptor descriptor;
    dkImageDescriptorInitialize(&descriptor, &view, false, false);
    // CPU-written: the slot is unreferenced (frees are deferred past the
    // GPU's last use), and the next draw invalidates the descriptor and L2
    // caches first. Not dkCmdBufPushData: some emulators stop executing the queue
    // when inline-to-memory writes land in a texture pool that is in use.
    std::memcpy(m_descriptorMemory.cpu + slot * sizeof(DkImageDescriptor), &descriptor, sizeof(descriptor));
    NoteGpuEvent(kGpuEventDescAlloc, TicAddress(&descriptor), slot, 0, 0);
    m_descriptorsDirty = true;
    m_listHasWork = true;
    ++Stats().descriptors;
    return slot;
}

void Device::FreeImageDescriptorAfter(uint32_t slot, uint64_t seq)
{
    if (slot == UINT32_MAX)
        return;
    m_gpuEvents.Push({TicAddress(m_descriptorMemory.cpu + slot * sizeof(DkImageDescriptor)), seq, slot,
                      kGpuEventDescFree, 0, 0});
    if (seq <= m_completedSeq)
        m_freeImageDescriptors.push_back(slot);
    else
        m_deferredDescriptorFrees.push_back({seq, slot});
}

size_t SamplerKeyHash::operator()(const SamplerKey &k) const
{
    // Word-wise (was byte-wise FNV over the 60-byte key).
    uint64_t h = k.compare ? 0x9E3779B97F4A7C15ull : 0;
    for (DWORD word : k.state)
        h = (h ^ word) * 0xff51afd7ed558ccdull;
    return (size_t)(h ^ (h >> 32));
}

uint32_t Device::ResolveSampler(uint32_t slot, bool compare)
{
    uint32_t &memo = m_ssDescriptor[slot][compare];
    if (memo != UINT32_MAX)
        return memo;
    uint32_t compact;
    const bool haveCompact = CompactSamplerKey(m_ss[slot], compare, &compact);
    if (haveCompact)
    {
        memo = m_samplerIds.Find(compact);
        if (memo != UINT32_MAX)
        {
            ++m_timing.samplerCompact;
            return memo;
        }
    }
    SamplerKey key{};
    std::memcpy(key.state, m_ss[slot], sizeof(key.state));
    key.compare = compare;
    memo = SamplerDescriptor(key);
    ++m_timing.samplerFull;
    if (haveCompact)
        m_samplerIds.Insert(compact, memo);
    return memo;
}

uint32_t Device::SamplerDescriptor(const SamplerKey &key)
{
    auto it = m_samplers.find(key);
    if (it != m_samplers.end())
        return it->second;
    if (m_samplers.size() >= kSamplerDescriptors)
    {
        Fail("SAMPLERS_EXHAUSTED", "sampler descriptors=%u", kSamplerDescriptors);
        return 0;
    }
    DkSampler sampler;
    dkSamplerDefaults(&sampler);
    DkWrapMode wrap[3] = {DkWrapMode_Repeat, DkWrapMode_Repeat, DkWrapMode_Repeat};
    const DWORD *s = key.state;
    if (!MapAddress(s[D3DSAMP_ADDRESSU], &wrap[0]) || !MapAddress(s[D3DSAMP_ADDRESSV], &wrap[1]) ||
        !MapAddress(s[D3DSAMP_ADDRESSW], &wrap[2]))
        Fail("SAMPLER_ADDRESS", "u=%u v=%u w=%u", (unsigned)s[D3DSAMP_ADDRESSU],
             (unsigned)s[D3DSAMP_ADDRESSV], (unsigned)s[D3DSAMP_ADDRESSW]);
    for (int i = 0; i < 3; ++i)
        sampler.wrapMode[i] = wrap[i];
    if (!MapMinMagFilter(s[D3DSAMP_MINFILTER], &sampler.minFilter) ||
        !MapMinMagFilter(s[D3DSAMP_MAGFILTER], &sampler.magFilter) ||
        !MapMipFilter(s[D3DSAMP_MIPFILTER], &sampler.mipFilter))
        Fail("SAMPLER_FILTER", "min=%u mag=%u mip=%u", (unsigned)s[D3DSAMP_MINFILTER],
             (unsigned)s[D3DSAMP_MAGFILTER], (unsigned)s[D3DSAMP_MIPFILTER]);
    if (s[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC || s[D3DSAMP_MAGFILTER] == D3DTEXF_ANISOTROPIC)
        sampler.maxAnisotropy = (float)std::min<DWORD>(std::max<DWORD>(s[D3DSAMP_MAXANISOTROPY], 1), 16);
    float bias;
    std::memcpy(&bias, &s[D3DSAMP_MIPMAPLODBIAS], sizeof(bias));
    sampler.lodBias = bias;
    // D3DSAMP_MAXMIPLEVEL is the most detailed level sampled.
    sampler.lodClampMin = (float)s[D3DSAMP_MAXMIPLEVEL];
    const D3DCOLOR border = s[D3DSAMP_BORDERCOLOR];
    sampler.borderColor[0].value_f = ((border >> 16) & 0xff) / 255.0f;
    sampler.borderColor[1].value_f = ((border >> 8) & 0xff) / 255.0f;
    sampler.borderColor[2].value_f = (border & 0xff) / 255.0f;
    sampler.borderColor[3].value_f = ((border >> 24) & 0xff) / 255.0f;
    if (key.compare)
    {
        // D3D9 hardware shadow maps pass when the reference is <= the depth.
        sampler.compareEnable = true;
        sampler.compareOp = DkCompareOp_Lequal;
    }
    const uint32_t slot = (uint32_t)m_samplers.size();
    DkSamplerDescriptor descriptor;
    dkSamplerDescriptorInitialize(&descriptor, &sampler);
    std::memcpy(m_descriptorMemory.cpu + kImageDescriptors * sizeof(DkImageDescriptor) +
                    slot * sizeof(DkSamplerDescriptor),
                &descriptor, sizeof(descriptor));
    m_descriptorsDirty = true;
    m_listHasWork = true;
    m_samplers.emplace(key, slot);
    return slot;
}

void Device::CollectCompleted()
{
    ++m_timing.collects;
    CompletedSeq();
    const auto retire = [this](const GpuAlloc &alloc, uint64_t seq) {
        m_gpuEvents.Push({alloc.gpu, seq, alloc.size, kGpuEventFreeRetired, (uint8_t)alloc.pool, (uint16_t)alloc.chunk});
        m_heaps[alloc.pool]->Free(alloc);
    };
    while (!m_deferredFrees.empty() && m_deferredFrees.front().seq <= m_completedSeq)
    {
        retire(m_deferredFrees.front().alloc, m_deferredFrees.front().seq);
        m_deferredFrees.pop_front();
    }
    // Deferred frees are pushed in non-decreasing seq order except for
    // FreeMemoryAfter(..., older seq); sweep the rest too.
    for (auto it = m_deferredFrees.begin(); it != m_deferredFrees.end();)
    {
        if (it->seq <= m_completedSeq)
        {
            retire(it->alloc, it->seq);
            it = m_deferredFrees.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = m_deferredDescriptorFrees.begin(); it != m_deferredDescriptorFrees.end();)
    {
        if (it->first <= m_completedSeq)
        {
            m_freeImageDescriptors.push_back(it->second);
            it = m_deferredDescriptorFrees.erase(it);
        }
        else
        {
            ++it;
        }
    }
    FrVerifyRetired();
    for (auto it = m_busyCmdChunks.begin(); it != m_busyCmdChunks.end();)
    {
        if (it->seq <= m_completedSeq)
        {
            CheckCmdGuard(it->mem, "retire", it->seq);
            FrNoteFree(it->mem, it->seq);
            m_freeCmdChunks.push_back(it->mem);
            it = m_busyCmdChunks.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // Upload chunks: all but the current one are recycled once complete.
    for (size_t i = 0; i + 1 < m_uploadChunks.size();)
    {
        if (m_uploadChunks[i].seq <= m_completedSeq)
        {
            m_freeUploadChunks.push_back(m_uploadChunks[i]);
            m_uploadChunks.erase(m_uploadChunks.begin() + (ptrdiff_t)i);
        }
        else
        {
            ++i;
        }
    }
    if (m_uploadChunks.size() == 1 && m_uploadChunks[0].seq <= m_completedSeq &&
        m_uploadChunks[0].seq < m_openSeq)
        m_uploadChunks[0].used = 0;
}

} // namespace deko9

bool Deko9_FrameAlloc(IDirect3DDevice9 *device, uint64_t frame, uint32_t bytes, uint32_t align, Deko9Span *out)
{
    if (!device || !out)
        return false;
    deko9::Device *d = static_cast<deko9::Device *>(device);
    // No device lock: the arena has its own mutex (a grow takes the device
    // lock inside the chunk factory).
    d->m_lockFree.arenaAllocs.fetch_add(1, std::memory_order_relaxed);
    deko9::ArenaSpan span;
    if (!d->FrameAlloc(frame, bytes, align, &span))
        return false;
    out->cpu = span.cpu;
    out->gpu = span.gpu;
    out->size = span.size;
    return true;
}
