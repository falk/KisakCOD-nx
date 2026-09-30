#pragma once

// Internal declarations of the Switch renderer backend: a D3D9
// implementation (the subset KisakCOD reaches) on deko3d, the engine's only
// Switch renderer. The engine's D3D9 renderer is unchanged; this is what its
// IDirect3DDevice9 calls land on.
//
// GPU/CPU coherence rules the whole backend relies on:
//   - every CPU-written GPU allocation is 256-byte aligned and is written
//     only while the GPU has not read it within the open command list
//     (fresh ring memory, renamed buffers, or memory whose last reader has
//     completed); each submitted list starts by invalidating the GPU L2,
//     texture, shader and descriptor caches;
//   - D3DUSAGE_DYNAMIC buffers live in GPU-uncached memory, because
//     D3DLOCK_NOOVERWRITE writes next to bytes the open list already read;
//   - shader constants are written through the command stream
//     (dkCmdBufPushConstants), ordered with draws; descriptors are written
//     by the CPU into unreferenced slots, and the next draw invalidates the
//     descriptor and L2 caches;
//   - texture uploads are copy commands recorded in order, so a later draw
//     sees them and an earlier one does not, exactly like D3D9.

#include <d3d9.h>
#include <deko3d.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

extern "C" {
#include <switch/kernel/thread.h>
}
#include <unordered_map>
#include <vector>

#include "deko9_arena.h"
#include "deko9_baked.h"
#include "deko9_callcensus.h"
#include "deko9_com_defaults.h"
#include "deko9_fastpath.h"
#include "deko9_framepace.h"
#include "deko9_fsr.h"
#include "deko9_gpufault.h"
#include "deko9_lock.h"
#include "deko9_shader.h"
#include "deko9_state_map.h"
#include "deko9_native.h"

struct Deko9Counters; // deko9_native.h
struct Deko9IndexRange; // deko9_native.h

namespace deko9
{

// ---- memory ---------------------------------------------------------------

enum Pool : uint8_t
{
    POOL_BUFFER,  // CPU write-combined, GPU cached: static VB/IB, staging, uploads
    POOL_DYNAMIC, // CPU write-combined, GPU uncached: D3DUSAGE_DYNAMIC buffers, readback
    POOL_IMAGE,   // GPU only: textures, render targets
    POOL_CODE,    // shader code
    POOL_CMD,     // command lists only: data-write overruns can never reach them
    POOL_COUNT,
};

struct GpuAlloc
{
    DkMemBlock block = nullptr;
    uint32_t offset = 0;
    uint32_t size = 0;
    uint8_t *cpu = nullptr; // null for POOL_IMAGE
    DkGpuAddr gpu = DK_GPU_ADDR_INVALID;
    Pool pool = POOL_BUFFER;
    uint32_t chunk = 0;
    explicit operator bool() const { return block != nullptr; }
};

class Heap
{
public:
    // tailReserve: bytes at the end of every chunk never handed out (shader
    // code memory must keep DK_SHADER_CODE_UNUSABLE_SIZE free for prefetch).
    Heap(Pool pool, uint32_t flags, uint32_t chunkSize, uint32_t tailReserve = 0)
        : m_pool(pool), m_flags(flags), m_chunkSize(chunkSize), m_tailReserve(tailReserve) {}
    bool Alloc(DkDevice device, uint32_t size, uint32_t align, GpuAlloc *out);
    void Free(const GpuAlloc &alloc);
    // [addr, addr + size) lies inside one of this heap's live memblocks
    // (the pitch mapping; images are addressed through their views).
    bool Contains(DkGpuAddr addr, uint32_t size) const;
    uint64_t BytesInUse() const { return m_inUse; }
    uint64_t BytesReserved() const { return m_reserved; }
    void Destroy();
    // GPU-fault black box: memblock creation lands in the device's event ring.
    void SetEvents(GpuEventRing<512> *events, const uint64_t *openSeq)
    {
        m_events = events;
        m_openSeq = openSeq;
    }

private:
    struct Chunk
    {
        DkMemBlock block;
        uint32_t size;
        std::map<uint32_t, uint32_t> freeSpans; // offset -> size
    };
    Pool m_pool;
    uint32_t m_flags;
    uint32_t m_chunkSize;
    uint32_t m_tailReserve;
    std::vector<Chunk> m_chunks;
    uint64_t m_inUse = 0;
    uint64_t m_reserved = 0;
    GpuEventRing<512> *m_events = nullptr;
    const uint64_t *m_openSeq = nullptr;
};

// ---- display --------------------------------------------------------------

// Horizon's default window, the adapter's only display mode and the
// swapchain size. A smaller D3D back buffer (the engine's render
// resolution) is upscaled to it with FSR 1 at present (deko9_fsr.cpp).
constexpr uint32_t kDisplayWidth = 1280;
constexpr uint32_t kDisplayHeight = 720;

// ---- objects ----------------------------------------------------------------

class Device;

// COM refcount + lifetime for every deko9 object.
template <class Base>
class Object : public Base
{
public:
    explicit Object(Device *device) : m_device(device) {}
    virtual ~Object() = default;
    STDMETHOD(QueryInterface)(REFIID riid, void **object) override;
    STDMETHOD_(ULONG, AddRef)() override { return ++m_refs; }
    STDMETHOD_(ULONG, Release)() override
    {
        const ULONG refs = --m_refs;
        if (!refs)
            Destroy();
        return refs;
    }
    Device *GetDeko9Device() const { return m_device; }

protected:
    // Default: delete now. GPU-backed objects defer their memory instead.
    virtual void Destroy() { delete this; }
    Device *m_device;
    std::atomic<ULONG> m_refs{1};
};

// One GPU image (all mips/faces/slices) behind textures and standalone
// surfaces, or plain CPU memory for SYSTEMMEM/SCRATCH resources.
struct ImageStore
{
    const FormatInfo *format = nullptr;
    D3DRESOURCETYPE type = D3DRTYPE_TEXTURE;
    D3DPOOL pool = D3DPOOL_MANAGED;
    DWORD usage = 0;
    uint32_t width = 0, height = 0, depth = 1, levels = 1, faces = 1;
    // GPU side (pool != SYSTEMMEM/SCRATCH)
    bool gpu = false;
    DkImageLayout layout{};
    DkImage image{};
    GpuAlloc memory;
    uint32_t descriptor = UINT32_MAX; // sampling view, all mips
    bool compressed = false;          // DkImageFlags_HwCompression (render/depth targets)
    // S4a (task/deko9-static-hazards): true for a store ever used as a
    // render, depth, copy-source-of-render or blit target -- set at creation
    // from D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL (CreateStore) and
    // latched on for any store later bound as a render/depth target
    // (SetRenderTarget/SetDepthStencilSurface) or a StretchRect blit
    // destination; never cleared. Everything else ("static": material
    // textures, lightmaps, probes) is exempt from per-draw sample hazard
    // checks: ApplyTextures/PrepareDraw only re-check it at bind time and
    // when `pendingRaw` says a copy wrote it while it stayed bound (see
    // pendingRaw); PrepareDraw also unconditionally re-stamps readEpoch for
    // every store it samples, skipped or not, after every draw (whether or
    // not that draw's own hazard commit records a barrier -- the old
    // always-add tracker stamps every sampled store's epoch every draw
    // regardless of hit), so a later copy-write into a store still bound
    // still sees the read (WAR) the way it would have.
    bool attachment = false;
    // S4a: set by a copy-write (CopyBufferToImage, so also UpdateTexture/
    // UpdateSurface) into this store while it is bound in a sampler-cache
    // slot and it is not `attachment`: the per-draw skip above would
    // otherwise miss the RAW hazard the old per-draw tracker always caught.
    // The next draw that samples it (ApplyTextures cache-hit or PrepareDraw's
    // gated loop) treats it like a fresh bind -- runs the real hazard check
    // and clears the flag.
    bool pendingRaw = false;
    // Deko9_MoveContents gave this store's image away; it must be fully
    // overwritten before any read (CheckUndefinedTargets).
    bool contentsUndefined = false;
    // D3D9-mandated colour target of a depth-only pass (Deko9_SetColorless).
    bool colorless = false;
    // Hazard epochs against Device::m_writeClock (bumped by every barrier):
    // equal to the clock means "since the last barrier".
    uint64_t renderEpoch = 0;    // 3D-engine write (draw, clear)
    uint64_t copyWriteEpoch = 0; // copy-engine write (uploads)
    uint64_t blitEpoch = 0;      // 2D-engine write (StretchRect)
    uint64_t readEpoch = 0;      // sampled, or used as a copy/blit source
    uint64_t copyReadEpoch = 0;  // used as a copy/blit source (copy or 2D engine)
    // Per (face, level): true once any data reached the GPU copy, so a
    // later non-discard lock must read it back first.
    std::vector<bool> uploaded;
    // CPU side: SYSTEMMEM/SCRATCH storage, or DYNAMIC shadow.
    std::vector<uint8_t> cpu;
    std::vector<uint32_t> cpuLevelOffset; // per (face, level)
    // SYSTEMMEM dirty region (destination-level-0 coordinates) for
    // UpdateTexture, and the same tracking reused by Texture2D::AddDirtyRect
    // / VolumeTexture::AddDirtyBox on any source texture: an accumulated set
    // of D3D9 dirty boxes (AddDirtyBox/AddDirtyRect, or a LockRect/LockBox on
    // level 0 without D3DLOCK_NO_DIRTY_UPDATE marks its own box). dirtyAll
    // covers "new texture" (D3D9: fully dirty at creation), any dirty call
    // with a null box/rect (whole texture), a lock on a mip level other than
    // 0 (dirty boxes are always level-0 coordinates in D3D9; a sub-level
    // lock is not translated up), and the >kMaxDirtyBoxes overflow fallback
    // (collapsed to one bounding box, see MarkDirty).
    static constexpr uint32_t kMaxDirtyBoxes = 256;
    bool dirtyAll = false;
    std::vector<D3DBOX> dirtyBoxes;
    bool AnyDirty() const { return dirtyAll || !dirtyBoxes.empty(); }
    void MarkAllDirty()
    {
        dirtyAll = true;
        dirtyBoxes.clear();
    }
    void ClearDirty()
    {
        dirtyAll = false;
        dirtyBoxes.clear();
    }
    // nullptr = whole texture (AddDirtyBox(nullptr)/AddDirtyRect(nullptr)).
    // Coalesces the new box into an existing one when they are exactly
    // axis-aligned and touch/overlap on one axis (the model-lighting
    // per-patch pattern: adjacent 4x4x4 boxes merge into one row), and caps
    // the list at kMaxDirtyBoxes by collapsing everything to one bounding
    // box rather than growing unbounded or falling back to a full copy.
    void MarkDirty(const D3DBOX *box);
    // Engine image name (GfxImage::name, a stable interned string; set by
    // Deko9_SetDebugName from r_image.cpp right after CreateTexture), for
    // the r_deko9Census per-texture upload table. Null if never tagged.
    const char *debugName = nullptr;

    uint32_t LevelWidth(uint32_t level) const { return width >> level ? width >> level : 1; }
    uint32_t LevelHeight(uint32_t level) const { return height >> level ? height >> level : 1; }
    uint32_t LevelDepth(uint32_t level) const { return depth >> level ? depth >> level : 1; }
    uint32_t RowPitch(uint32_t level) const;
    uint32_t RowCount(uint32_t level) const;
    uint32_t SlicePitch(uint32_t level) const { return RowPitch(level) * RowCount(level); }
    uint32_t LevelBytes(uint32_t level) const { return SlicePitch(level) * LevelDepth(level); }
    uint32_t SubIndex(uint32_t face, uint32_t level) const { return face * levels + level; }
};

// Lock state shared by surfaces and volume levels.
struct LockState
{
    bool locked = false;
    DWORD flags = 0;
    D3DBOX box{};
    GpuAlloc staging; // GPU-backed, non-dynamic lock: CPU writes land here
};

class Surface;

class Texture2D final : public Object<Deko9Default_IDirect3DTexture9>
{
public:
    Texture2D(Device *device, std::shared_ptr<ImageStore> store);
    ~Texture2D() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **device) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_TEXTURE; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return m_store->levels; }
    STDMETHOD_(DWORD, SetLOD)(DWORD lod) override { return 0; }
    STDMETHOD_(DWORD, GetLOD)() override { return 0; }
    STDMETHOD(GetLevelDesc)(UINT level, D3DSURFACE_DESC *desc) override;
    STDMETHOD(GetSurfaceLevel)(UINT level, IDirect3DSurface9 **surface) override;
    STDMETHOD(LockRect)(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
    STDMETHOD(UnlockRect)(UINT level) override;
    STDMETHOD(AddDirtyRect)(const RECT *rect) override;
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(DWORD, SetPriority)(DWORD) override { return 0; }
    STDMETHOD_(DWORD, GetPriority)() override { return 0; }
    STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE) override { return D3D_OK; }
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override { return D3DTEXF_LINEAR; }
    STDMETHOD_(void, GenerateMipSubLevels)() override;
    const std::shared_ptr<ImageStore> &Store() const { return m_store; }

private:
    std::shared_ptr<ImageStore> m_store;
    std::vector<Surface *> m_levels;
};

class CubeTexture final : public Object<Deko9Default_IDirect3DCubeTexture9>
{
public:
    CubeTexture(Device *device, std::shared_ptr<ImageStore> store);
    ~CubeTexture() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **device) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_CUBETEXTURE; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return m_store->levels; }
    STDMETHOD_(DWORD, SetLOD)(DWORD) override { return 0; }
    STDMETHOD_(DWORD, GetLOD)() override { return 0; }
    STDMETHOD(GetLevelDesc)(UINT level, D3DSURFACE_DESC *desc) override;
    STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surface) override;
    STDMETHOD(LockRect)(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
    STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES face, UINT level) override;
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(DWORD, SetPriority)(DWORD) override { return 0; }
    STDMETHOD_(DWORD, GetPriority)() override { return 0; }
    const std::shared_ptr<ImageStore> &Store() const { return m_store; }

private:
    std::shared_ptr<ImageStore> m_store;
    std::vector<Surface *> m_faces;
};

class VolumeTexture final : public Object<Deko9Default_IDirect3DVolumeTexture9>
{
public:
    VolumeTexture(Device *device, std::shared_ptr<ImageStore> store);
    ~VolumeTexture() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **device) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_VOLUMETEXTURE; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return m_store->levels; }
    STDMETHOD_(DWORD, SetLOD)(DWORD) override { return 0; }
    STDMETHOD_(DWORD, GetLOD)() override { return 0; }
    STDMETHOD(GetLevelDesc)(UINT level, D3DVOLUME_DESC *desc) override;
    STDMETHOD(LockBox)(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags) override;
    STDMETHOD(UnlockBox)(UINT level) override;
    STDMETHOD(AddDirtyBox)(const D3DBOX *box) override;
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(DWORD, SetPriority)(DWORD) override { return 0; }
    STDMETHOD_(DWORD, GetPriority)() override { return 0; }
    const std::shared_ptr<ImageStore> &Store() const { return m_store; }

private:
    std::shared_ptr<ImageStore> m_store;
    std::vector<LockState> m_locks; // per level
};

// A 2D subresource: a texture level/face, or a standalone render target,
// depth-stencil or offscreen-plain surface (which owns its store).
class Surface final : public Object<Deko9Default_IDirect3DSurface9>
{
public:
    Surface(Device *device, std::shared_ptr<ImageStore> store, uint32_t face, uint32_t level,
            IUnknown *container);
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **device) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_SURFACE; }
    STDMETHOD(GetContainer)(REFIID riid, void **container) override;
    STDMETHOD(GetDesc)(D3DSURFACE_DESC *desc) override;
    STDMETHOD(LockRect)(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
    STDMETHOD(UnlockRect)() override;
    STDMETHOD_(void, PreLoad)() override {}

    const std::shared_ptr<ImageStore> &Store() const { return m_store; }
    uint32_t Face() const { return m_face; }
    uint32_t Level() const { return m_level; }
    // View usable as a render target / blit / copy endpoint.
    void MakeView(DkImageView *view) const;

private:
    std::shared_ptr<ImageStore> m_store;
    uint32_t m_face, m_level;
    IUnknown *m_container; // owning texture, or null for a standalone surface
    LockState m_lock;
};

class Buffer
{
public:
    bool Init(Device *device, uint32_t size, DWORD usage, D3DPOOL pool);
    void ReleaseMemory(Device *device);
    // renamedBytes (out, optional): bytes deko9 itself memcpy'd to preserve
    // contents across a rename (busy buffer, no DISCARD) -- 0 otherwise --
    // for the r_deko9Census dynamic-buffer table.
    HRESULT Lock(Device *device, UINT offset, UINT size, void **data, DWORD flags, uint64_t *renamedBytes = nullptr);
    HRESULT Unlock(Device *device);
    DkGpuAddr Gpu() const { return m_memory.gpu; }
    uint32_t Size() const { return m_size; }
    DWORD Usage() const { return m_usage; }
    D3DPOOL PoolKind() const { return m_pool; }
    // Last open-list sequence that read this buffer.
    uint64_t lastUse = 0;
    // Stable role label (e.g. "dynamicVB", "preTessIB"; Deko9_SetBufferRole,
    // set once at creation from r_buffers.cpp/r_staticmodelcache.cpp) for the
    // r_deko9Census dynamic-buffer table. Null: bucketed by size/usage/pool.
    const char *role = nullptr;

    // A window buffer's storage is a frame-arena span (deko9_arena.h), not
    // memory this Buffer owns: Lock/Unlock (the DISCARD/NOOVERWRITE rename
    // protocol) must not be mixed with it, and ReleaseMemory must not free
    // it (the arena retires it by frame id instead).
    bool IsWindow() const { return m_window; }
    // Re-points this buffer's storage at `span` (gpu base + cpu pointer +
    // size: a Deko9Span) and marks the
    // input dirty so ApplyVertexStreams (or the index bind, which always
    // re-reads Gpu()) rebinds at the new address. The first call frees this
    // buffer's original owned memory (deferred past the open list); later
    // calls just re-point (the arena, not this Buffer, owns that memory).
    void BindWindow(Device *device, uint64_t gpu, void *cpu, uint32_t size);

private:
    bool CanaryIntact() const;
    GpuAlloc m_memory; // m_size bytes + kCanaryBytes guard tail; for a
                       // window, gpu/cpu only -- block stays null so
                       // ReleaseMemory/FreeMemoryAfter never touch it
    uint32_t m_size = 0;
    DWORD m_usage = 0;
    D3DPOOL m_pool = D3DPOOL_DEFAULT;
    bool m_locked = false;
    bool m_window = false;
};

class VertexBuffer final : public Object<Deko9Default_IDirect3DVertexBuffer9>
{
public:
    VertexBuffer(Device *device, DWORD fvf) : Object(device), m_fvf(fvf) {}
    ~VertexBuffer() override;
    Buffer buffer;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **device) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_VERTEXBUFFER; }
    STDMETHOD(Lock)(UINT offset, UINT size, void **data, DWORD flags) override;
    STDMETHOD(Unlock)() override;
    STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC *desc) override;
    STDMETHOD_(void, PreLoad)() override {}

private:
    DWORD m_fvf;
};

class IndexBuffer final : public Object<Deko9Default_IDirect3DIndexBuffer9>
{
public:
    IndexBuffer(Device *device, D3DFORMAT format) : Object(device), m_format(format) {}
    ~IndexBuffer() override;
    Buffer buffer;
    D3DFORMAT Format() const { return m_format; }
    STDMETHOD(GetDevice)(IDirect3DDevice9 **device) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_INDEXBUFFER; }
    STDMETHOD(Lock)(UINT offset, UINT size, void **data, DWORD flags) override;
    STDMETHOD(Unlock)() override;
    STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC *desc) override;
    STDMETHOD_(void, PreLoad)() override {}

private:
    D3DFORMAT m_format;
};

class VertexDecl final : public Object<Deko9Default_IDirect3DVertexDeclaration9>
{
public:
    VertexDecl(Device *device, const D3DVERTEXELEMENT9 *elements, uint32_t count);
    ~VertexDecl() override; // purges baked program units using it
    STDMETHOD(GetDevice)(IDirect3DDevice9 **device) override;
    STDMETHOD(GetDeclaration)(D3DVERTEXELEMENT9 *elements, UINT *count) override;
    std::vector<D3DVERTEXELEMENT9> elements; // without the D3DDECL_END terminator
    uint32_t id;                             // unique, for the attribute-state cache
    uint32_t streamCount = 0;                // highest element stream + 1, resolved once
};

struct ShaderVariant
{
    uint32_t shadowMask;
    // Vertex shaders: the registers read from instance attributes instead
    // of the constant buffer (instanced static-model draws); empty for the
    // ordinary variant.
    InstanceLayout instance;
    // Pixel shaders: the early-Z variant (layout(early_fragment_tests)),
    // chosen only for draws that write neither depth nor stencil
    // (Device::WantEarlyZ).
    bool earlyZ = false;
    // Pixel shaders with a depth-compare sampler: the r_shadowFilter
    // translation (Deko9_TranslateShader shadowFilter); 0 = retail.
    uint32_t shadowFilter = 0;
    mutable bool bound = false; // logged on first bind
    DkShader shader;
    GpuAlloc code;
    // From the DKSH instruction scan (Deko9_ScanDksh): the constant-buffer
    // slots the program reads.
    uint32_t slotMask = 0;
    Deko9DkshStats stats{}; // static cost (r_deko9DrawCensus)
};

class ShaderBase
{
public:
    bool Init(Device *device, const DWORD *function, Deko9Stage stage, std::string *error);
    // Returns the compiled variant for the given depth-compare sampler mask
    // (and, for vertex shaders, instance register layout; for pixel shaders,
    // early fragment tests), translating and compiling (or loading from the
    // SD cache) on first use.
    const ShaderVariant *Variant(Device *device, uint32_t shadowMask, const InstanceLayout &instance = {},
                                 bool earlyZ = false);
    void ReleaseMemory(Device *device);
    const Deko9ShaderInfo &Info() const { return m_info; }
    const std::vector<uint8_t> &Bytecode() const { return m_bytecode; }
    uint64_t Hash() const { return m_hash; }
    uint32_t id;

private:
    std::vector<uint8_t> m_bytecode;
    uint64_t m_hash = 0;
    Deko9Stage m_stage = DEKO9_STAGE_VERTEX;
    Deko9ShaderInfo m_info{};
    std::vector<std::unique_ptr<ShaderVariant>> m_variants;
};

class VertexShader final : public Object<Deko9Default_IDirect3DVertexShader9>
{
public:
    explicit VertexShader(Device *device) : Object(device) {}
    ~VertexShader() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **device) override;
    STDMETHOD(GetFunction)(void *data, UINT *size) override;
    ShaderBase shader;
};

class PixelShader final : public Object<Deko9Default_IDirect3DPixelShader9>
{
public:
    explicit PixelShader(Device *device) : Object(device) {}
    ~PixelShader() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **device) override;
    STDMETHOD(GetFunction)(void *data, UINT *size) override;
    ShaderBase shader;
};

struct EventMarker
{
    DkFence fence{};
    uint64_t seq = 0; // list the fence was recorded into
};

class Query final : public Object<Deko9Default_IDirect3DQuery9>
{
public:
    Query(Device *device, D3DQUERYTYPE type);
    ~Query() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **device) override;
    STDMETHOD_(D3DQUERYTYPE, GetType)() override { return m_type; }
    STDMETHOD_(DWORD, GetDataSize)() override;
    STDMETHOD(Issue)(DWORD flags) override;
    STDMETHOD(GetData)(void *data, DWORD size, DWORD flags) override;
    // Deko9_WaitQuery: kernel wait on the query's list fence.
    bool Wait(int64_t timeoutNs);
    // Deko9_DebugQueryGpuPassed: the self-test's raw-fence oracle.
    bool RawGpuPassed();

private:
    D3DQUERYTYPE m_type;
    // Where the last D3DISSUE_END completes (deko9_framepace.h EventPoint):
    // the in-list marker (m_marker, device-owned) when the open list had
    // work, else the end of the newest submitted list.
    bool Done();
    EventPoint m_point;
    EventMarker *m_marker = nullptr;
    bool m_issued = false;
    bool m_begun = false;
    GpuAlloc m_report;        // two 16-byte counter reports (begin, end)
};

class SwapChain final : public Object<Deko9Default_IDirect3DSwapChain9>
{
public:
    explicit SwapChain(Device *device) : Object(device) {}
    STDMETHOD(GetDevice)(IDirect3DDevice9 **device) override;
    STDMETHOD(Present)(const RECT *src, const RECT *dst, HWND window, const RGNDATA *dirty, DWORD flags) override;
    STDMETHOD(GetBackBuffer)(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface) override;
    STDMETHOD(GetPresentParameters)(D3DPRESENT_PARAMETERS *params) override;
    STDMETHOD(GetDisplayMode)(D3DDISPLAYMODE *mode) override;
    STDMETHOD(GetFrontBufferData)(IDirect3DSurface9 *dest) override;
};

// ---- device -----------------------------------------------------------------

// Baked units (deko9_baked.h). Words are deko3d command words captured from
// the ordinary derivation; replaying them records exactly what the
// derivation would.
struct RasterUnit
{
    RasterKey key;
    std::vector<uint32_t> words;
};
struct ProgramUnit
{
    ProgramKey key;
    const ShaderVariant *vs, *ps;
    std::vector<uint32_t> shaderWords; // dkCmdBufBindShaders
    std::vector<uint32_t> attribWords; // dkCmdBufBindVtxAttribState
};

struct SamplerKey
{
    DWORD state[14]; // D3DSAMP_* 1..13 by index
    bool compare;
    bool operator==(const SamplerKey &o) const { return !std::memcmp(this, &o, sizeof(*this)); }
};

struct SamplerKeyHash
{
    size_t operator()(const SamplerKey &k) const;
};

class Device final : public Object<Deko9Default_IDirect3DDevice9>
{
public:
    Device(IDirect3D9 *d3d, HWND window, const D3DPRESENT_PARAMETERS &params);
    ~Device() override;
    bool Init(std::string *error);

    // ---- IDirect3DDevice9 ----
    STDMETHOD(TestCooperativeLevel)() override { return D3D_OK; }
    STDMETHOD_(UINT, GetAvailableTextureMem)() override;
    STDMETHOD(EvictManagedResources)() override { return D3D_OK; }
    STDMETHOD(GetDirect3D)(IDirect3D9 **d3d) override;
    STDMETHOD(GetDeviceCaps)(D3DCAPS9 *caps) override;
    STDMETHOD(GetDisplayMode)(UINT swapChain, D3DDISPLAYMODE *mode) override;
    STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS *params) override;
    STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS *params, IDirect3DSwapChain9 **swapChain) override;
    STDMETHOD(GetSwapChain)(UINT index, IDirect3DSwapChain9 **swapChain) override;
    STDMETHOD_(UINT, GetNumberOfSwapChains)() override { return 1; }
    STDMETHOD(Reset)(D3DPRESENT_PARAMETERS *params) override;
    STDMETHOD(Present)(const RECT *src, const RECT *dst, HWND window, const RGNDATA *dirty) override;
    STDMETHOD(GetBackBuffer)(UINT swapChain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface) override;
    STDMETHOD(GetRasterStatus)(UINT swapChain, D3DRASTER_STATUS *status) override;
    STDMETHOD_(void, SetGammaRamp)(UINT swapChain, DWORD flags, const D3DGAMMARAMP *ramp) override;
    STDMETHOD_(void, GetGammaRamp)(UINT swapChain, D3DGAMMARAMP *ramp) override;
    STDMETHOD(CreateTexture)(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **texture, HANDLE *shared) override;
    STDMETHOD(CreateVolumeTexture)(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9 **texture, HANDLE *shared) override;
    STDMETHOD(CreateCubeTexture)(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9 **texture, HANDLE *shared) override;
    STDMETHOD(CreateVertexBuffer)(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **buffer, HANDLE *shared) override;
    STDMETHOD(CreateIndexBuffer)(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9 **buffer, HANDLE *shared) override;
    STDMETHOD(CreateRenderTarget)(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL lockable, IDirect3DSurface9 **surface, HANDLE *shared) override;
    STDMETHOD(CreateDepthStencilSurface)(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL discard, IDirect3DSurface9 **surface, HANDLE *shared) override;
    STDMETHOD(UpdateSurface)(IDirect3DSurface9 *src, const RECT *srcRect, IDirect3DSurface9 *dst, const POINT *dstPoint) override;
    STDMETHOD(UpdateTexture)(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) override;
    STDMETHOD(GetRenderTargetData)(IDirect3DSurface9 *rt, IDirect3DSurface9 *dst) override;
    STDMETHOD(GetFrontBufferData)(UINT swapChain, IDirect3DSurface9 *dst) override;
    STDMETHOD(StretchRect)(IDirect3DSurface9 *src, const RECT *srcRect, IDirect3DSurface9 *dst, const RECT *dstRect, D3DTEXTUREFILTERTYPE filter) override;
    STDMETHOD(ColorFill)(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR color) override;
    STDMETHOD(CreateOffscreenPlainSurface)(UINT w, UINT h, D3DFORMAT format, D3DPOOL pool, IDirect3DSurface9 **surface, HANDLE *shared) override;
    STDMETHOD(SetRenderTarget)(DWORD index, IDirect3DSurface9 *surface) override;
    STDMETHOD(GetRenderTarget)(DWORD index, IDirect3DSurface9 **surface) override;
    STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9 *surface) override;
    STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9 **surface) override;
    STDMETHOD(BeginScene)() override;
    STDMETHOD(EndScene)() override;
    STDMETHOD(Clear)(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;
    STDMETHOD(SetViewport)(const D3DVIEWPORT9 *viewport) override;
    STDMETHOD(GetViewport)(D3DVIEWPORT9 *viewport) override;
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE state, DWORD value) override;
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE state, DWORD *value) override;
    STDMETHOD(GetTexture)(DWORD stage, IDirect3DBaseTexture9 **texture) override;
    STDMETHOD(SetTexture)(DWORD stage, IDirect3DBaseTexture9 *texture) override;
    STDMETHOD(GetSamplerState)(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value) override;
    STDMETHOD(SetSamplerState)(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override;
    STDMETHOD(SetScissorRect)(const RECT *rect) override;
    STDMETHOD(GetScissorRect)(RECT *rect) override;
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE type, UINT startVertex, UINT primCount) override;
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount) override;
    STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE type, UINT primCount, const void *data, UINT stride) override;
    STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT primCount, const void *indices, D3DFORMAT indexFormat, const void *data, UINT stride) override;
    STDMETHOD(CreateVertexDeclaration)(const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **decl) override;
    STDMETHOD(SetVertexDeclaration)(IDirect3DVertexDeclaration9 *decl) override;
    STDMETHOD(GetVertexDeclaration)(IDirect3DVertexDeclaration9 **decl) override;
    STDMETHOD(SetFVF)(DWORD fvf) override;
    STDMETHOD(GetFVF)(DWORD *fvf) override;
    STDMETHOD(CreateVertexShader)(const DWORD *function, IDirect3DVertexShader9 **shader) override;
    STDMETHOD(SetVertexShader)(IDirect3DVertexShader9 *shader) override;
    STDMETHOD(GetVertexShader)(IDirect3DVertexShader9 **shader) override;
    STDMETHOD(SetVertexShaderConstantF)(UINT start, const float *data, UINT count) override;
    STDMETHOD(GetVertexShaderConstantF)(UINT start, float *data, UINT count) override;
    STDMETHOD(SetStreamSource)(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride) override;
    STDMETHOD(GetStreamSource)(UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset, UINT *stride) override;
    STDMETHOD(SetStreamSourceFreq)(UINT stream, UINT divider) override;
    STDMETHOD(GetStreamSourceFreq)(UINT stream, UINT *divider) override;
    STDMETHOD(SetIndices)(IDirect3DIndexBuffer9 *indices) override;
    STDMETHOD(GetIndices)(IDirect3DIndexBuffer9 **indices) override;
    STDMETHOD(CreatePixelShader)(const DWORD *function, IDirect3DPixelShader9 **shader) override;
    STDMETHOD(SetPixelShader)(IDirect3DPixelShader9 *shader) override;
    STDMETHOD(GetPixelShader)(IDirect3DPixelShader9 **shader) override;
    STDMETHOD(SetPixelShaderConstantF)(UINT start, const float *data, UINT count) override;
    STDMETHOD(GetPixelShaderConstantF)(UINT start, float *data, UINT count) override;
    STDMETHOD(CreateQuery)(D3DQUERYTYPE type, IDirect3DQuery9 **query) override;

    // ---- native fast path (deko9_native.h) ----
    // Each takes m_lock (inline re-entry when the caller already holds it,
    // as the engine does across a draw-surface list).
    //
    // Texture slots are non-owning: binding takes no COM reference, and the
    // bound ImageStore is resolved once per bind, not per draw. A texture
    // destroyed while bound is cleared from its slots (ForgetTexture); a
    // later draw that samples such a slot fails loudly instead of sampling
    // freed memory. The D3D9 SetTexture entry uses the same slots.
    void BindTexture(uint32_t slot, IDirect3DBaseTexture9 *texture);
    // Applies the engine's packed sampler state (see ApplyEngineSamplerState)
    // and returns the engine's resulting tracked state.
    uint32_t SetSamplerPacked(uint32_t slot, uint32_t packed, uint32_t oldPacked);
    void ForgetTexture(IDirect3DBaseTexture9 *texture);
    // Vertex/index buffer slots are non-owning too (no COM AddRef/Release
    // per SetStreamSource/SetIndices): a buffer destroyed while bound is
    // cleared from its slots, and a later draw reading such a slot fails
    // loudly (FAIL:DEKO9_BUFFER_BIND) instead of fetching freed memory.
    void ForgetBuffer(VertexBuffer *buffer);
    void ForgetBuffer(IndexBuffer *buffer);
    // A shader or vertex declaration with this id is being released: its
    // baked program units are dropped.
    void ForgetProgramObject(uint32_t id);
    // Instanced draws (deko9_native.h Deko9_*Instances): registers in
    // `layout` are captured from the vertex constant file per AddInstance
    // and read by the instanced vertex shader variant from an instance-rate
    // vertex stream; DrawInstances issues one draw for all of them.
    bool BeginInstances(const InstanceLayout &layout);
    void AddInstance();
    HRESULT DrawInstances(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices, UINT startIndex,
                          UINT primCount);
    void CancelInstances();
    // Deko9_DrawIndexedRanges: one PrepareDraw, then one deko3d indexed draw
    // per range -- exactly `count` DrawIndexedPrimitive calls with unchanged
    // state (the engine's static world index buffer draws, r_pretess.cpp).
    HRESULT DrawIndexedRanges(UINT numVertices, const ::Deko9IndexRange *ranges, uint32_t count);
    void GetCounters(::Deko9Counters *out);
    // Fast-path verification (engine dvar r_deko9Verify): every draw also
    // re-derives the binding, vertex input and constants the slow way and
    // compares them with what the fast path recorded.
    void SetVerify(bool enable);
    // Present upscaler (deko9_fsr.h), used only when the back buffer is
    // smaller than the display: RCAS sharpness in stops (engine dvar
    // r_fsrSharpness) and the mode (r_fsrMode), both per frame.
    void SetUpscaleSharpness(float stops);
    void SetUpscaleMode(uint32_t mode);
    // Dynamic resolution (deko9_native.h Deko9_UpscaleSurface): the r_fsrMode
    // pass from the rectangle `srcRect` of the 2D render-target texture
    // `src` into `dstRect` of the color render target `dst` (the engine's
    // scene -> back buffer step before the 2D pass); a plain 2D-engine copy
    // when both rectangles have the same size.
    // Rectangles are {x, y, width, height}.
    bool UpscaleRect(ImageStore *src, const int32_t srcRect[4], Surface *dst, const int32_t dstRect[4],
                     std::string *error);
    // Deko9_ResizeRenderTarget: re-lays out a single-level 2D render target
    // or depth-stencil image at width x height inside the memory it was
    // created with (no allocation; contents undefined afterwards).
    bool ResizeStore(ImageStore *store, uint32_t width, uint32_t height, std::string *error);
    // Deko9_MoveContents: swaps the images of two compatible colour targets
    // and marks `src` contentsUndefined.
    bool MoveStoreContents(ImageStore *src, ImageStore *dst, std::string *error);
    // `clear`: a colour Clear covering [l,t,r,b); otherwise the current draw.
    bool CheckUndefinedTargets(bool clear, LONG l, LONG t, LONG r, LONG b);
    uint32_t m_undefinedTargets = 0; // stores with contentsUndefined set
    // Colorless stores bind no colour attachment.
    bool BindsColor(const Surface *rt) const;
    uint64_t m_moves = 0;            // Deko9_MoveContents calls (DEKO9 perf moves=)
    // Deko9_SetFrameTag: the engine's render size for the frame being
    // recorded; published with that frame's GPU time (Deko9_GetGpuFrame) and
    // printed as render= on the `DEKO9 perf` line.
    void SetFrameTag(uint32_t width, uint32_t height)
    {
        m_frameTagWidth = width;
        m_frameTagHeight = height;
    }
    // Latest completed frame: GPU busy ms, its tag, and a counter that
    // advances with each published frame (lock-free, any thread).
    void GetGpuFrame(float *gpuMs, uint32_t *width, uint32_t *height, uint32_t *count) const;
    // Selftest: textureGather `component` of `source` into `target`
    // (kGatherProbeGlsl).
    bool GatherProbe(ImageStore *source, ImageStore *target, int component, std::string *error);
    // Native float-Z (deko9_native.h Deko9_BuildFloatZ, kFloatZGlsl):
    // `target` (a float render target) = the view depth reconstructed from
    // `depth` (the scene depth-stencil surface, sampled as depth).
    bool BuildFloatZ(Surface *depth, Surface *target, const FloatZConstants &constants, std::string *error);
    // Off-screen particles (deko9_native.h Deko9_ParticleDepth /
    // Deko9_ParticleComposite, kHrpDepthGlsl / kHrpCompositeGlsl).
    bool ParticleDepth(Surface *depth, ImageStore *floatZ, const int32_t rect[4], uint32_t factor, Surface *dstDepth,
                       Surface *dstFloatZ, std::string *error);
    bool ParticleComposite(ImageStore *color, ImageStore *halfZ, ImageStore *fullZ, Surface *dst,
                           const int32_t dstRect[4], const hrpref::CompositeConstants &constants, std::string *error);
    void GetHrpCounts(uint64_t *depthPasses, uint64_t *composites) const
    {
        *depthPasses = m_hrp.depthPasses;
        *composites = m_hrp.composites;
    }
    void SetHrpRules(uint32_t rules) { m_hrp.rules = rules; }
    uint64_t HrpZcullInvalidates() const { return m_hrp.zcullInvalidates; }
    bool LoadHrpPrograms(std::string *error);
    // Sampling view of a depth-stencil store (dsSource depth), created once.
    bool EnsureDepthDescriptor(ImageStore *depth);
    // After a native full-screen pass: the next D3D draw re-applies targets,
    // viewport, pipeline, textures and vertex input.
    void EndNativePass();
    // r_deko9EarlyZ (deko9_native.h Deko9_SetEarlyZ): pixel shaders that can
    // discard (or draw with the alpha test on) use their early-Z variant for
    // draws that test but write neither depth nor stencil
    // (RasterAllowsEarlyZ), outside occlusion queries.
    void SetPerDraw(uint32_t flags)
    {
        if (flags == m_perDraw)
            return;
        m_perDraw = flags;
        m_drawHazardValid = false;
        m_vsFile.MarkAllDirty(); // re-sync the any-dirty flags with the bitmasks
        m_psFile.MarkAllDirty();
        m_dirtyTextures = true;
        m_texSlotDirty[0] = m_texSlotDirty[1] = ~0u;
    }
    // r_shadowFilter (deko9_native.h Deko9_SetShadowFilter): the translation
    // every depth-compare pixel-shader variant is looked up / compiled with.
    // A change drops the baked program units (they point at variants).
    void SetShadowFilter(uint32_t mode)
    {
        if (mode >= DEKO9_SHADOW_FILTER_MODES)
            mode = 0;
        if (mode == m_shadowFilter)
            return;
        m_shadowFilter = mode;
        m_programUnits.Clear();
        m_program = nullptr;
        m_recorded.attribs = false;
        m_dirtyAttribs = true;
        ForgetBoundShaders();
    }
    uint32_t ShadowFilter() const { return m_shadowFilter; }
    void SetEarlyZ(bool enable)
    {
        if (enable != m_earlyZ)
            m_earlyZ = enable, m_dirtyEarlyZ = true;
    }
    // Occlusion query BEGIN (+1) / END or release (-1): the early-Z variant
    // would count discarded fragments as passed samples, so it is off while
    // one is open.
    void NoteOcclusionQuery(int delta)
    {
        m_occlusionOpen += delta;
        m_dirtyEarlyZ = true;
    }
    // Per-pass GPU timing (deko9_native.h Deko9_GpuMarker).
    void SetGpuPasses(bool enable) { m_gpuPassesWanted.store(enable, std::memory_order_relaxed); }
    bool GpuPassesOn() const { return m_gpuPasses.load(std::memory_order_relaxed); }
    void GpuMarker(uint32_t pass);
    // r_deko9LightBarriers: 0 = DkBarrier_Full + L2 flush for every hazard;
    // 1 = 3D-only hazards use DkBarrier_Primitives + texture-cache invalidate;
    // 2 = 3D-only hazards use DkBarrier_Fragments + texture-cache invalidate.
    void SetBarrierMode(uint32_t mode) { m_barrierMode = mode <= 2 ? mode : 0; }
    // ---- zcull (deko9_zcull.cpp; r_deko9ZcullStats) -------------------------
    // Zcull itself is always on (the queue's default); only the stats model
    // is a toggle, applied at the next Present.
    void SetZcullStats(bool enable) { m_zcullStatsWanted.store(enable, std::memory_order_relaxed); }
    // ---- GPU-fault black box -------
    // r_deko9FaultTrace N: GPU breadcrumb every N draws + per-draw records +
    // a watcher thread that dumps them when the GPU stops mid-list (and a
    // heartbeat line every 250 ms), applied at the next Present;
    // r_deko9GpuMap 1: one "DEKO9 gpumap" line per static pool, image (its
    // TIC VA) and image free/resize, applied at once (Deko9_SetGpuMap).
    void SetFaultTrace(uint32_t interval) { m_faultTraceWanted.store(interval, std::memory_order_relaxed); }
    void SetGpuMap(uint32_t level) { m_gpuMapWanted.store(level, std::memory_order_relaxed); }
    void ApplyGpuMap(); // under the device lock
    void BlackBoxDump(const char *reason, uint64_t stalledNs = 0);
    // Image VA as the GPU sees it (the TIC address: pitch, generic or
    // compressed alias of the memblock).
    uint64_t ImageVa(const ImageStore *store) const;
    void GpuMapImage(const ImageStore *store, const char *what);
    void NoteGpuEvent(uint8_t kind, uint64_t gpu, uint32_t size, uint8_t pool, uint16_t chunk)
    {
        m_gpuEvents.Push({gpu, m_openSeq, size, kind, pool, chunk});
    }
    bool ZcullStatsOn() const { return m_zcullStats; }
    struct ZcullPassStats
    {
        uint64_t ztest = 0;         // depth-tested draws
        uint64_t invalid = 0;       // ... while the region was invalidated and not cleared since
        uint64_t binds = 0;         // depth-target binds that moved the region (invalidate)
        uint64_t fullClears = 0;    // depth clears covering the whole depth surface
        uint64_t partialClears = 0; // scissored/viewport depth clears
        uint64_t flips = 0;         // depth-compare direction changes since the last clear
        uint64_t alwaysWrites = 0;  // draws writing depth with ZFUNC ALWAYS
        uint64_t hw[4] = {};        // DkCounter_ZcullStats word deltas (hardware)
        uint64_t hwIntervals = 0;   // pass intervals the hardware counters covered
    };
    const ZcullPassStats *ZcullPass(uint32_t pass) const { return pass < 32 ? &m_zcullPass[pass] : nullptr; }

    // ---- D3D9 entry-point call census (r_deko9Census, deko9_callcensus.h) ----
    // Applied at the next Present, like GpuPasses. CensusOn() is read by the
    // gated call sites via deko9_internal.h's CensusScope; off costs one
    // atomic load.
    void SetCensus(bool enable) { m_censusWanted.store(enable, std::memory_order_relaxed); }
    bool CensusOn() const { return m_census.load(std::memory_order_relaxed); }
    // Records one call (CensusScope's destructor; direct, not sampled).
    // CensusScope outlives the entry point's own DeviceLockGuard, so the
    // table is written under the lock here: with r_smp_backend 1 the main
    // thread's locks/uploads and the back end's draws record concurrently,
    // and PresentFrame's ReportCensus clears the tables.
    void CensusRecord(CensusId id, uint64_t ns, uint64_t bytes)
    {
        DeviceLockGuard lock(m_lock);
        CensusEntry &e = m_censusEntries[id];
        ++e.calls;
        e.ns += ns;
        e.bytes += bytes;
    }
    // Per-texture upload bytes (Device::CopyBufferToImage), keyed by the
    // ImageStore so repeated uploads of the same texture accumulate.
    void CensusTexture(const ImageStore *store, uint64_t bytes);
    // Per-role dynamic VB/IB lock (VertexBuffer::Lock/IndexBuffer::Lock,
    // called after Buffer::Lock released the device lock: takes it itself);
    // role comes from Buffer::role if tagged, else a size/usage/pool bucket.
    void CensusBufferRole(const Buffer *buf, bool isIndex, uint64_t bytes);
    void ReportCensus(); // "DEKO9 calls" block, called every 60 frames from PresentFrame

    // ---- backend services ----
    DeviceLock &Lock() { return m_lock; }
    bool VertexRangeValid(int64_t first, uint64_t vertices, const char *what);
    // The thread that last recorded a draw (or none yet): only it flushes.
    bool IsRecordingThread() const { return !m_drawThread || m_drawThread == threadGetSelf(); }
    // Single-submitter rule (deko9_lock.h SubmitOwner): the engine's render
    // owner claims the submitter role (Deko9_ClaimSubmitThread); a list
    // submitted by another thread is reported once (FAIL:DEKO9_SUBMIT_THREAD).
    void ClaimSubmitThread() { m_submitOwner.Claim(ThreadTag()); }
    void ReportForeignSubmit();
    DkDevice Dk() const { return m_dk; }
    DkCmdBuf Cmd() { return m_cmd; }
    // Sequence number of the command list being recorded. A resource used
    // by it stays alive until CompletedSeq() >= this value.
    uint64_t OpenSeq() const { return m_openSeq; }
    // Polls the fences of submitted lists in order and retires the ones
    // done; returns the last completed sequence.
    uint64_t CompletedSeq();
    // True once the GPU has passed list `seq`. The cached completed sequence
    // answers without touching a fence (it only ever lags the GPU, so a
    // cached "done" is never early); otherwise one CompletedSeq() poll pass.
    uint64_t CachedCompletedSeq() const { return m_completedSeq; }
    bool SeqDone(uint64_t seq)
    {
        if (seq <= m_completedSeq)
        {
            ++m_timing.seqCacheHits;
            return true;
        }
        return CompletedSeq() >= seq;
    }
    // Sleeps (device lock released, 50 us slices between fence polls) until
    // submitted list `seq` completes or `timeoutNs` passes. Returns
    // SeqDone(seq). Lists not yet submitted are not waited for. Caller must
    // not hold the lock.
    bool WaitSeqFor(uint64_t seq, int64_t timeoutNs);
    // Self-test oracle: whether list `seq`'s own fence has signalled, read
    // straight from the fence (no cache, no retirement).
    bool RawSeqPassed(uint64_t seq);
    // Submits the open list (a no-op when it is empty unless force).
    void Flush(bool force = false);
    bool ListHasWork() const { return m_listHasWork; }
    // Records an in-list fence at the current point of the open list
    // (dkCmdBufSignalFence); it signals once every command recorded before
    // it has completed. Valid until list OpenSeq() retires.
    EventMarker *RecordEventMarker();
    // Native frame pacing (deko9_framepace.h, Deko9_Frame* in deko9_native.h).
    // The frame being recorded (frames presented + 1).
    uint64_t FrameRecording() const { return m_frameRing.Recording(); }
    // Every frame up to this one is known done; read without the lock (the
    // engine's end-fence and GPU-sync polls answer from it first).
    uint64_t FrameDonePublished() const { return m_frameDonePublished.load(std::memory_order_acquire); }
    void NoteFrameDone(uint64_t frame)
    {
        m_frameRing.MarkDone(frame);
        m_frameDonePublished.store(m_frameRing.DoneThrough(), std::memory_order_release);
        // Property (b): every resource of a done frame (or earlier) is
        // free; frame-arena chunks are stamped with a frame id, not a list
        // seq, so they retire here rather than in CollectCompleted().
        m_frameArena.RetireThrough(m_frameRing.DoneThrough());
    }
    // Whether frame `frame`'s fence has passed (polls it; 0 is done; a
    // frame not presented yet is not).
    bool FrameDone(uint64_t frame);
    // Sleeps (lock released, 50 us slices) until FrameDone(frame) or the
    // timeout. A frame not presented yet returns false at once. Caller must
    // not hold the lock (only polls inside a batch).
    bool WaitFrameFor(uint64_t frame, int64_t timeoutNs);
    // Sleep-polls a copy of a fence without the device lock; returns
    // whether it signalled before `timeoutNs`.
    static bool SleepPollFence(DkFence fence, int64_t timeoutNs, uint64_t *waitedNs);
    // Blocks until the given list has finished on the GPU, flushing first
    // if it is still open.
    void WaitSeq(uint64_t seq);
    void WaitIdle();

    bool AllocMemory(Pool pool, uint32_t size, uint32_t align, GpuAlloc *out);
    // Frees memory once every list up to `seq` has completed.
    void FreeMemoryAfter(const GpuAlloc &alloc, uint64_t seq);
    // Frame arena:
    // transient VB/IB memory for `frame` (Deko9_FrameRecording() from the
    // back end), retired -- not by list seq -- where FrameRing::MarkDone
    // advances (NoteFrameDone below).
    bool FrameAlloc(uint64_t frame, uint32_t bytes, uint32_t align, ArenaSpan *out);
    FrameArena &Arena() { return m_frameArena; } // r_deko9Census / selftest counters
    // Upload ring: CPU-writable GPU memory valid for the open list.
    bool AllocUpload(uint32_t size, uint32_t align, GpuAlloc *out);

    uint32_t AllocImageDescriptor(const DkImageView &view);
    void FreeImageDescriptorAfter(uint32_t slot, uint64_t seq);
    uint32_t SamplerDescriptor(const SamplerKey &key);
    // Sampler descriptor for an internal slot's current state: the per-slot
    // memo, else the compact-key cache, else SamplerDescriptor.
    uint32_t ResolveSampler(uint32_t slot, bool compare);

    bool CreateStore(ImageStore *store, std::string *error);
    // Frees a store's GPU memory and descriptor once the open list is done.
    void DestroyStoreAfter(ImageStore *store);
    // Wraps a created store so its GPU memory is released with the last owner.
    std::shared_ptr<ImageStore> MakeStore();
    void MarkWork() { m_listHasWork = true; }
    void CopyBufferToImage(ImageStore *store, uint32_t face, uint32_t level, DkGpuAddr src,
                           const DkImageRect &rect);
    // Reads a subresource back into CPU memory (full stall).
    bool ReadImage(ImageStore *store, uint32_t face, uint32_t level, void *dst);
    // Hazard tracking (see ImageStore epochs). Each may record a barrier.
    // Hazard tracking. One recorded operation (a draw, clear, blit, copy)
    // collects every image it touches, then HazardCommit records at most one
    // barrier and stamps all of them with the epoch the operation really runs
    // in. Stamping each image as it was visited (the old BeforeX order) left
    // an image read before a later image's barrier stamped with the old epoch,
    // so a following write to it skipped its barrier (write-after-read race:
    // rare flickering rectangles on decals/posters).
    enum class Access : uint8_t { Sample, Render, CopyRead, CopyWrite, BlitWrite };
    void HazardBegin() { m_hazardCount = 0; }
    // True when no store appears both sampled and rendered in the pending
    // set: re-evaluating the same set with no barrier and no other hazard
    // commit in between is then a no-op (DEKO9_PERDRAW_HAZARD).
    bool HazardRolesDisjoint() const;
    // Whether HazardCommit on the pending set would record a barrier or
    // change an epoch (r_deko9Verify re-derivation of a skipped evaluation).
    bool HazardWouldAct() const;
    void HazardAdd(ImageStore *store, Access access);
    void HazardCommit();
    // S4a: true if `store` sits valid in a sampler-cache slot right now
    // (either stage). A copy-write into a bound static store sets
    // pendingRaw so the next draw that samples it runs the real hazard
    // check instead of skipping it (see CopyBufferToImage).
    bool StaticStoreBound(const ImageStore *store) const;
    // S4a: whether a sampled store's per-draw hazard should be added for
    // real this draw -- always for an attachment store or one flagged
    // pendingRaw, or (DEKO9_PERDRAW_STATICTEX off, r_deko9StaticHazard --
    // the pixel A/B proof's knob) always, matching the earlier tracker.
    bool NeedsStaticHazardCheck(const ImageStore *store) const
    {
        return store->attachment || store->pendingRaw || !(m_perDraw & DEKO9_PERDRAW_STATICTEX);
    }
    void BeforeSample(ImageStore *store) { HazardBegin(); HazardAdd(store, Access::Sample); HazardCommit(); }
    void BeforeRender(ImageStore *store) { HazardBegin(); HazardAdd(store, Access::Render); HazardCommit(); }
    void BeforeCopyRead(ImageStore *store) { HazardBegin(); HazardAdd(store, Access::CopyRead); HazardCommit(); }
    void BeforeCopyWrite(ImageStore *store) { HazardBegin(); HazardAdd(store, Access::CopyWrite); HazardCommit(); }
    void BeforeBlitWrite(ImageStore *store) { HazardBegin(); HazardAdd(store, Access::BlitWrite); HazardCommit(); }
    void Barrier(bool copyEngine);
    // Submits the open list early once it holds many barriers. Each full
    // barrier splits the list into another GPFIFO entry and libnx queues at
    // most GPFIFO_QUEUE_SIZE (2048) per submit; past that deko3d aborts in
    // dkQueueSubmitCommands. Call only at the entry of a top-level call,
    // before it allocates upload memory or records anything.
    void SplitLongList();

    bool LoadShaderCode(const std::vector<uint8_t> &dksh, ShaderVariant *variant, std::string *error);
    // A released shader's variants may be freed and their addresses reused.
    void ForgetBoundShaders()
    {
        m_boundVs = m_boundPs = nullptr;
        m_dirtyShaders = true;
    }
    uint32_t NextId() { return ++m_nextId; }
    // Buffer::Lock moved a buffer to new memory: stream extents and lastUse
    // stamps are re-derived at the next draw.
    void BufferRenamed() { m_dirtyInput = true; }

private:
    friend class SwapChain;
    void ResetState();
    void BeginList();
    void SubmitOpenList();
    void RetireCmdMemory(uint64_t seq);
    void CollectCompleted();
    static void AddCmdMemory(void *userData, DkCmdBuf cmd, size_t minSize);

    HRESULT PrepareDraw(D3DPRIMITIVETYPE type, UINT primCount, DkPrimitive *prim, uint32_t *count);
    void ApplyRenderTargets();
    void ApplyViewportScissor();
    // Baked raster state (deko9_baked.h): key the render states, replay the
    // unit's words; RecordRasterState is the full derivation behind a miss
    // (and behind r_deko9Verify's comparison).
    void ApplyRasterDepthColor();
    void RecordRasterState();
    DepthTarget CurrentDepthTarget() const;
    void ApplyTextures(Deko9Stage stage, const Deko9ShaderInfo &info, uint32_t *shadowMask);
    bool ApplyVertexInput();
    void RecordVertexAttribs(const Deko9ShaderInfo &vsInfo, const InstanceLayout &instance);
    bool ApplyVertexStreams();
    bool ApplyShaders();
    // Baked program (shader pair + vertex attribute layout) for the bound
    // VS/PS/declaration, this depth-compare mask and the instance layout.
    bool ApplyProgram(uint32_t psShadow);
    ProgramUnit *BakeProgram(const ProgramKey &key, uint32_t psShadow);
    // Runs record() with m_cmd redirected to a capture of command words.
    template <typename Record>
    bool Capture(std::vector<uint32_t> *words, Record &&record);
    void ReplayWords(const std::vector<uint32_t> &words);
    void ApplyConstants(Deko9Stage stage);
    void VerifyDraw();
    template <uint32_t Regs>
    HRESULT SetConstants(ConstantFile<Regs> &file, UINT start, const float *data, UINT count);
    template <uint32_t Regs>
    void PushConstants(ConstantFile<Regs> &file, uint32_t stage, const GpuAlloc &ubo);
    template <uint32_t Regs>
    void PushDirtyConstants(ConstantFile<Regs> &file, uint32_t stage, const GpuAlloc &ubo);
    void PresentFrame();
    // FSR 1 upscaling at present (deko9_fsr.cpp).
    bool InitUpscaler(std::string *error);
    void RecordUpscale(int slot);
    void ReleaseUpscaler();

    // deko3d objects
    DkDevice m_dk = nullptr;
    DkQueue m_queue = nullptr;
    DkCmdBuf m_cmd = nullptr;
    DkSwapchain m_swapchain = nullptr;
    DeviceLock m_lock;
    Thread *m_drawThread = nullptr;
    SubmitOwner m_submitOwner;

    IDirect3D9 *m_d3d;
    HWND m_window;
    D3DPRESENT_PARAMETERS m_params;

    // Submission tracking
    static constexpr uint32_t kFenceRing = 16;
    DkFence m_fences[kFenceRing]{};
    uint64_t m_fenceSeq[kFenceRing]{};
    uint64_t m_openSeq = 1;
    uint64_t m_completedSeq = 0;
    bool m_listHasWork = false;
    struct CmdChunk
    {
        GpuAlloc mem;
        uint64_t seq;
    };
    std::vector<GpuAlloc> m_freeCmdChunks;
    std::vector<CmdChunk> m_busyCmdChunks;
    std::vector<GpuAlloc> m_openCmdChunks;
    struct UploadChunk
    {
        GpuAlloc mem;
        uint32_t used;
        uint64_t seq;
    };
    std::vector<UploadChunk> m_uploadChunks; // back() is current
    std::vector<UploadChunk> m_freeUploadChunks;
    struct DeferredFree
    {
        uint64_t seq;
        GpuAlloc alloc;
    };
    std::deque<DeferredFree> m_deferredFrees;
    std::deque<std::pair<uint64_t, uint32_t>> m_deferredDescriptorFrees;

    std::unique_ptr<Heap> m_heaps[POOL_COUNT];
    // Transient per-frame VB/IB memory (2.2); factory bound to AllocMemory
    // in Init(), chunks retired by frame id in NoteFrameDone.
    FrameArena m_frameArena;

    // Descriptors
    static constexpr uint32_t kImageDescriptors = 16384;
    static constexpr uint32_t kSamplerDescriptors = 2048;
    GpuAlloc m_descriptorMemory; // image set, then sampler set
    std::vector<uint32_t> m_freeImageDescriptors;
    std::unordered_map<SamplerKey, uint32_t, SamplerKeyHash> m_samplers;
    bool m_descriptorsDirty = false;

    // Swapchain images and the D3D back buffer (rendered, then blitted)
    DkImage m_swapImages[3]{};
    GpuAlloc m_swapMemory[3];
    Surface *m_backBuffer = nullptr;
    SwapChain *m_swapChainObject = nullptr;
    // Back buffer smaller than the display: present upscales it straight
    // into the swapchain image with one full-screen pass (deko9_fsr.h;
    // mode = r_fsrMode) instead of the blit.
    struct Upscaler
    {
        bool active = false;
        float sharpness = 0.2f;
        uint32_t mode = 0; // deko9::UpscaleMode
        ShaderVariant vs, sgsr, bilinearRcas, bilinear;
        uint32_t backDescriptor = UINT32_MAX; // sampling view of the back buffer
        uint32_t sampler = 0;                 // linear, clamp
        GpuAlloc constants;                   // fragment uniform block (push-constant updated)
        GpuAlloc timestamps;                  // 3 frames x {start, end} counter reports
        uint32_t slotMode[3] = {};            // mode each timestamp slot was recorded for
        uint64_t gpuNs = 0, frames = 0, samples = 0;
        // Programs, sampler and constants loaded (present path or first
        // UpscaleRect); the source-rectangle builds load on first use.
        bool loaded = false;
        ShaderVariant sgsrRect, bilinearRcasRect, bilinearRect;
        // UpscaleRect GPU timing: a ring of {start, end} report pairs, each
        // read once its list has completed.
        static constexpr uint32_t kRectSlots = 8;
        GpuAlloc rectTimestamps;
        uint64_t rectSlotSeq[kRectSlots] = {};
        uint32_t rectSlotMode[kRectSlots] = {};
        bool rectSlotCopy[kRectSlots] = {};
        uint64_t rectCalls = 0, rectGpuNs = 0, rectSamples = 0, rectCopies = 0;
        uint32_t rectInW = 0, rectInH = 0, rectOutW = 0, rectOutH = 0;
    } m_fsr;
    bool EnsureUpscaler(std::string *error);
    const ShaderVariant *UpscaleProgram(uint32_t mode, bool sourceRect, std::string *error);
    // Frame GPU time for dynamic resolution (RetireSeq accumulates each
    // list's time; a frame is published when its last list retires).
    struct FrameEnd
    {
        uint64_t seq;
        uint32_t width, height;
    };
    std::deque<FrameEnd> m_frameEnds;
    uint64_t m_frameAccumNs = 0;
    uint32_t m_frameTagWidth = 0, m_frameTagHeight = 0;
    // Published frame, two words that each carry the publish count so a
    // reader on another thread can tell a torn pair (GetGpuFrame retries).
    std::atomic<uint64_t> m_gpuFrameA{0}; // count (32) | GPU us (32)
    std::atomic<uint64_t> m_gpuFrameB{0}; // count (32) | width (16) | height (16)
    uint32_t m_gpuFramesPublished = 0;
    uint64_t m_resizes = 0;
    // Selftest gather probe (Deko9_GatherProbe), loaded on first use.
    struct GatherProbeState
    {
        ShaderVariant vs, ps;
        GpuAlloc constants;
    } m_gatherProbe;
    // Native float-Z pass (BuildFloatZ), loaded on first use.
    struct FloatZState
    {
        ShaderVariant vs, ps;
        GpuAlloc constants;
    } m_floatZ;
    // Off-screen particle passes, loaded on first use.
    struct HrpState
    {
        ShaderVariant vs, depthPs, compositePs;
        GpuAlloc depthConstants, compositeConstants;
        uint64_t depthPasses = 0, composites = 0;
        uint64_t zcullInvalidates = 0;
        uint32_t rules = 3; // DEKO9_HRP_RULES_ALL
    } m_hrp;

    // Constant register files, UBOs (whole register file per stage)
    // CPU mirrors with per-register dirty bits (ConstantFile): a draw pushes
    // only the registers that changed, in coalesced runs.
    ConstantFile<DEKO9_VS_CONST_REGS> m_vsFile;
    ConstantFile<DEKO9_PS_CONST_REGS> m_psFile;
    GpuAlloc m_vsUbo, m_psUbo;

    // D3D state
    DWORD m_rs[256]{};
    DWORD m_ss[DEKO9_MAX_SAMPLERS * 2][14]{}; // [0,16) PS samplers, [16,20) VS D3DVERTEXTEXTURESAMPLER0..3
    // SamplerDescriptor(m_ss[slot], compare) per slot, or UINT32_MAX; reset
    // by SetSamplerState, so a draw skips the key hash while states hold.
    uint32_t m_ssDescriptor[DEKO9_MAX_SAMPLERS * 2][2];
    IDirect3DBaseTexture9 *m_textures[DEKO9_MAX_SAMPLERS * 2]{}; // non-owning, see BindTexture
    ImageStore *m_texStores[DEKO9_MAX_SAMPLERS * 2]{};           // StoreOf(m_textures[i]), resolved at bind
    bool m_texForgotten[DEKO9_MAX_SAMPLERS * 2]{};               // slot's texture was destroyed while bound
    SamplerIdCache m_samplerIds;                                 // compact sampler key -> descriptor id
    Surface *m_renderTargets[4]{};
    Surface *m_depthStencil = nullptr;
    D3DVIEWPORT9 m_viewport{};
    RECT m_scissor{};
    VertexDecl *m_decl = nullptr; // owning (COM reference)
    VertexShader *m_vs = nullptr;
    PixelShader *m_ps = nullptr;
    struct Stream
    {
        VertexBuffer *buffer = nullptr; // non-owning, see ForgetBuffer
        uint32_t offset = 0, stride = 0, freq = 1;
        bool forgotten = false;         // its buffer was destroyed while bound
    } m_streams[DK_MAX_VERTEX_BUFFERS];
    IndexBuffer *m_indices = nullptr; // non-owning, see ForgetBuffer
    bool m_indicesForgotten = false;
    bool m_inScene = false;

    // Dirty tracking for the recorded deko3d state
    bool m_dirtyTargets = true, m_dirtyViewport = true, m_dirtyRaster = true;
    // Per-draw gating (PrepareDraw): the shader/texture binding and the
    // vertex-input state are re-derived only when these say so; a draw with
    // an unchanged pass records only hazards, constants, buffers and the
    // draw. Every setter that changes an input sets its flag; BeginList sets
    // them all (a new list re-records everything).
    //   m_dirtyShaders/m_dirtyTextures: VS/PS, texture slots, sampler states
    //   m_dirtyAttribs: vertex declaration or VS (attribute layout)
    //   m_dirtyInput: stream sources (buffers, offsets, strides)
    bool m_dirtyInput = true, m_dirtyShaders = true, m_dirtyTextures = true, m_dirtyAttribs = true;
    // Images sampled by the last applied binding, per stage (0 PS, 1 VS):
    // a gated draw still runs BeforeSample on each (hazard tracking is per
    // draw, not per binding).
    struct PendingAccess
    {
        ImageStore *store;
        Access access;
    };
    static constexpr uint32_t kMaxHazards = DEKO9_MAX_SAMPLERS * 2 + 8;
    PendingAccess m_hazards[kMaxHazards];
    uint32_t m_hazardCount = 0;
    // DEKO9_PERDRAW_HAZARD: every HazardCommit bumps m_hazardSerial; the
    // last draw's commit records it with m_writeClock, and a later draw with
    // the same sampled set (no ApplyShaders) and targets (no m_dirtyTargets)
    // skips evaluation while both still match.
    uint32_t m_perDraw = DEKO9_PERDRAW_HAZARD | DEKO9_PERDRAW_CONSTS | DEKO9_PERDRAW_TEXTURES;
    uint64_t m_hazardSerial = 0;
    bool m_drawHazardValid = false;
    bool m_drawHazardReused = false; // this draw skipped evaluation
    uint64_t m_drawHazardSerial = 0, m_drawHazardClock = 0;
    // DEKO9_PERDRAW_TEXTURES: sampler slots (per stage, 0 PS, 1 VS) whose
    // texture or sampler state changed since ApplyTextures last resolved them.
    uint32_t m_texSlotDirty[2] = {~0u, ~0u};
    // Last resolution of each sampler slot: the store sampled (dummy when
    // unbound or invalid), its handle, and the shader dimension it was
    // validated against; reused while the slot is clean and the bound
    // shader declares the same dimension.
    struct TexSlot
    {
        ImageStore *store;
        DkResHandle handle;
        uint8_t dim;
        bool compare;
        bool valid;
    } m_texSlot[2][DEKO9_MAX_SAMPLERS]{};
    void MarkTexSlotDirty(uint32_t slot)
    {
        m_texSlotDirty[slot >= DEKO9_MAX_SAMPLERS ? 1 : 0] |= 1u << (slot % DEKO9_MAX_SAMPLERS);
    }
    struct Sampled
    {
        uint32_t count;
        ImageStore *stores[DEKO9_MAX_SAMPLERS];
    } m_sampled[2]{};
    const ShaderVariant *m_boundVs = nullptr;
    const ShaderVariant *m_boundPs = nullptr;
    // State last recorded into the open list; identical re-binds are
    // skipped (per-draw CPU cost). Reset by BeginList, so a new list always
    // re-records everything.
    struct Recorded
    {
        const RasterUnit *raster; // last replayed baked raster unit
        // Attribute layout last replayed: (declaration, VS, instance layout).
        bool attribs;
        uint32_t attribDecl, attribVs;
        uint64_t attribInstance;
        uint32_t textureCount[2];
        DkResHandle textures[2][DEKO9_MAX_SAMPLERS];
        uint32_t streams;
        DkVtxBufferState streamStates[DK_MAX_VERTEX_BUFFERS];
        DkBufExtents streamExtents[DK_MAX_VERTEX_BUFFERS];
        DkGpuAddr indexAddress; // 0: none recorded
        DkIdxFormat indexFormat;
    } m_recorded{};
    uint64_t m_writeClock = 1; // bumped by every barrier
    ImageStore *m_dummy[4]{};  // 1x1 (0,0,0,1) per Deko9SamplerDim for unbound samplers
    BakedCache<RasterKey, RasterUnit> m_rasterUnits;
    BakedCache<ProgramKey, ProgramUnit> m_programUnits;
    ProgramKey m_programKey{};            // key of the last applied program
    const ProgramUnit *m_program = nullptr; // last applied program unit (null: re-resolve)
    uint32_t m_psCompareMask = 0;          // PS depth-compare mask of the last texture binding
    // Early-Z variant selection (SetEarlyZ, WantEarlyZ): re-derived in
    // PrepareDraw when the shaders, raster state, targets or these inputs
    // changed; m_psEarlyZ enters the program key.
    bool EarlyZCandidate() const;
    bool m_earlyZ = true;           // r_deko9EarlyZ
    uint32_t m_shadowFilter = 0;    // r_shadowFilter
    bool m_dirtyEarlyZ = true;
    bool m_ezCandidate = false;     // the draw qualifies (whether or not enabled)
    bool m_psEarlyZ = false;        // the draw uses the early-Z variant
    int m_occlusionOpen = 0;        // occlusion queries between BEGIN and END
    DkCmdBuf m_bakeCmd = nullptr;          // capture-only command buffer (no memory)
    uint32_t m_bakeStorage[1024];
    // Instanced draw being assembled (BeginInstances .. DrawInstances).
    struct Instancing
    {
        bool active = false;   // between BeginInstances and DrawInstances
        bool drawing = false;  // inside DrawInstances: Apply* use the layout
        InstanceLayout layout;
        uint64_t layoutHash = 0;
        uint32_t count = 0;
        std::vector<float> data; // count x layout.count float4
        DkBufExtents extent{};   // uploaded instance stream
    } m_instancing;
    // Native frame pacing (deko9_framepace.h): frame F's fence is
    // m_frameFences[F % kFramesInFlight], signalled on the queue right after
    // F's present list; PresentFrame waits for frame F - kFramesInFlight
    // before signalling it again.
    FrameRing<kFramesInFlight> m_frameRing;
    DkFence m_frameFences[kFramesInFlight]{};
    std::atomic<uint64_t> m_frameDonePublished{0};
    // In-list fences of event/occlusion queries (Query::Issue). deko3d keeps
    // a pointer to the DkFence in the list until it is submitted, so each
    // marker has a stable address (owned by the pool) and is recycled only
    // after its list retired (RetireSeq).
    std::vector<std::unique_ptr<EventMarker>> m_markerPool;
    std::vector<EventMarker *> m_freeMarkers;
    std::deque<EventMarker *> m_busyMarkers; // in list order
    int m_lastPresentSlot = -1;
    uint32_t m_nextId = 0;
    uint64_t m_frames = 0;
    // Per-list command summary ring, dumped when a fence wait stalls.
public:
    struct ListStats
    {
        uint64_t seq;
        uint32_t draws, clears, uploads, readbacks, blits, barriers, queries, descriptors;
        int acquire, present;
        uintptr_t thread;
    };
    ListStats &Stats() { return m_listStats[m_openSeq % kStatsRing]; }
    // Totals since the last periodic report (PresentFrame).
    struct Totals
    {
        uint64_t lists, draws, uploads, uploadBytes, barriers, descriptors, readbacks;
    } totals{};
    // Per-60-frame timing (DEKO9 perf line): GPU time from timestamps at
    // each list's start and end, CPU time blocked on fences and acquire.
    struct Timing
    {
        uint64_t gpuNs, fenceWaitNs, acquireNs, draws, lists, drawCpuNs, periodNs;
        // CPU run-ahead: max lists submitted but not completed, at present,
        // and max presented frames whose fence had not passed (frame ring).
        uint64_t maxListsInFlight, maxFramesInFlight;
        // Fast-path work counters (per 60 frames; reported per frame).
        uint64_t textureBinds, samplerSets, samplerCompact, samplerFull;
        uint64_t shaderApplies, attribApplies, streamApplies, rasterApplies, indexBinds, gatedDraws;
        // Baked units: lookups that hit / missed (baked a new unit), units
        // replayed (words recorded), words replayed.
        uint64_t bakedHits, bakedMisses, bakedReplays, bakedWords;
        // Instancing: instanced draws, instances they drew, and draws that
        // fell back to one draw per instance (see Deko9_BeginInstances).
        uint64_t instancedDraws, instances, instanceFallbacks;
        // Draws that qualify for the pixel shader's early-Z variant, and
        // draws that used it (r_deko9EarlyZ).
        uint64_t earlyZCandidates, earlyZDraws;
        // Draws that skipped hazard evaluation (DEKO9_PERDRAW_HAZARD); texture
        // slots resolved by ApplyTextures (all used slots, or only the
        // changed ones with DEKO9_PERDRAW_TEXTURES).
        uint64_t hazardSkips, texSlotsResolved;
        // S4a (task/deko9-static-hazards): every sample of a non-attachment
        // ("static") store counts staticSamples (what the tracker before S4a
        // would have hazard-checked every time); staticHazardChecks is how
        // many of those actually ran a real HazardAdd (a new bind, a
        // pendingRaw catch-up, or DEKO9_PERDRAW_STATICTEX off) -- the gap is
        // the per-draw hazard checks S4a's skip removes.
        uint64_t staticSamples, staticHazardChecks;
        // SetStreamSource/SetIndices calls (non-owning: no COM reference each).
        uint64_t bufferBinds;
        // Deko9_DrawIndexedRanges: calls and the deko3d draws (ranges) they
        // recorded (DEKO9 perf ranges line).
        uint64_t rangeCalls, rangeDraws;
        // Constants: bytes pushed per stage (0 VS, 1 PS), push calls,
        // registers written by Set*ShaderConstantF vs actually changed, and
        // the bytes the former [lo, hi) range scheme would have pushed.
        uint64_t constBytes[2], constPushes[2], constRegsSet, constRegsChanged;
        // GPU-progress checks (DEKO9 perf sync line): fence polls
        // (dkFenceWait timeout 0) and how many found the list done, checks
        // the cached completed sequence answered without a poll,
        // CollectCompleted sweeps, Query::GetData calls and how many returned
        // S_FALSE, Buffer::Lock busy checks that polled, and the engine
        // GPU-sync waits blocked in the kernel on a query's fence.
        uint64_t fencePolls, fencePollsDone, seqCacheHits, collects;
        uint64_t queryGetData, queryPending, bufferLockPolls, queryWaits, queryWaitNs;
        // Native frame waits (Deko9_WaitFrame): calls that slept, time slept.
        uint64_t frameWaits, frameWaitNs;
    } m_timing{};
    uint64_t m_lockAcquisitionsReported = 0, m_lockEntriesReported = 0;
    uint64_t m_lockContendedReported[2] = {0, 0}, m_lockWaitNsReported[2] = {0, 0};
    uint64_t m_lockHandoffsReported = 0;
    uint64_t m_drawsTotal = 0, m_drawCpuNsTotal = 0; // m_timing folded in at each 60-frame report
    // Verification state (SetVerify / VerifyDraw).
    bool m_verify = false;
    bool m_verifyConstantsSynced = false; // shadows hold everything pushed since the full re-push
    uint64_t m_verifiedDraws = 0, m_verifyMismatches = 0;
    // Per-draw fast paths re-derived by VerifyDraw (cumulative): hazard
    // skips checked, and texture-binding draws whose incremental slot
    // resolution was checked (DEKO9_PERDRAW_HAZARD / _TEXTURES).
    uint64_t m_verifiedHazardSkips = 0, m_verifiedTexIncremental = 0;
    float m_vsShadow[DEKO9_VS_CONST_REGS][4]{}; // what the pushes wrote to the UBOs
    float m_psShadow[DEKO9_PS_CONST_REGS][4]{};
    uint64_t m_lastPresentNs = 0;
    GpuAlloc m_timestamps; // kFenceRing x {start, end} counter reports
    void RecordTimestamp(bool end);
    void RetireSeq(uint64_t seq);

    // Per-pass GPU timing: a ring of timestamp reports, one per marker plus
    // a reopen at each list start and a close before each list's end, read
    // back in order as lists retire. m_passMarks[i] describes report slot i.
    static constexpr uint32_t kPassSlots = 2048;
    static constexpr uint16_t kPassClose = 0xffff;
    struct PassMark
    {
        uint64_t seq;
        uint16_t pass;
        bool zcull; // a DkCounter_ZcullStats report sits in m_zcullStamps' slot
    };
    void RecordPassMark(uint16_t pass);
    void RetirePassMarks(uint64_t seq);
    void ReportGpuPasses();
    GpuAlloc m_passStamps;
    PassMark m_passMarks[kPassSlots]{};
    uint64_t m_passHead = 0, m_passTail = 0;
    uint16_t m_curPass = 0;
    bool m_passPrevValid = false;
    uint16_t m_passPrev = 0;
    uint64_t m_passPrevTs = 0;
    uint64_t m_passNs[32]{};
    uint64_t m_passDropped = 0;
    uint64_t m_passFrames = 0;
    uint32_t m_barrierMode = 0;
    uint64_t m_lightBarriers = 0; // per 60 frames, reported on the perf line
    // Cumulative: copy/blit writes into an image read (sampled or copied
    // from) since the last barrier, each ordered by a full barrier
    // (write-after-read; Deko9Counters::uploadAfterReadBarriers).
    uint64_t m_writeAfterReadBarriers = 0;
    std::atomic<bool> m_gpuPasses{false};       // read unlocked by Deko9_GpuMarker
    std::atomic<bool> m_gpuPassesWanted{false}; // applied at Present
    // Cumulative per-pass GPU ns and frames since the device started
    // (Deko9_GetGpuPassTotals; never reset by the periodic report).
    uint64_t m_passNsTotal[32]{};
    uint64_t m_passFramesTotal = 0;

    // ---- zcull (deko9_zcull.cpp) --------------------------------------------
    // Always on (the queue's default; deko3d has no per-command-buffer zcull
    // switch). Only the CPU/hardware stats model below is a toggle.
    std::atomic<bool> m_zcullStatsWanted{false};
    bool m_zcullStats = false;
    void ApplyZcullSettings(); // PresentFrame, after the present
    // CPU model of the zcull region, per pass (m_curPass), when stats are on:
    // deko3d invalidates the region whenever a depth target at a different
    // address is bound (ConditionalZcullInvalidate) and re-initializes it
    // with a depth clear; a region that was invalidated and not cleared since
    // cannot cull. Direction flips (LESS-family <-> GREATER-family compare)
    // and ALWAYS depth writes are the other state that degrades it on Maxwell.
    ZcullPassStats m_zcullPass[32];
    DkGpuAddr m_zcullAddr = 0; // depth address the region tracks (deko3d MME scratch mirror)
    bool m_zcullValid = false;
    uint8_t m_zcullDir = 0; // 0 none since the clear, 1 LESS/LEQUAL, 2 GREATER/GEQUAL
    uint64_t m_zcullFrames = 0;
    uint64_t m_zcullHwMissing = 0; // counter reports the GPU never wrote (emulators)
    GpuAlloc m_zcullStamps;        // kPassSlots x {4 x u32 DkCounter_ZcullStats}
    bool m_zcullPrevValid = false;
    uint16_t m_zcullPrevPass = 0;
    uint32_t m_zcullPrev[4] = {};
    void ZcullNoteTargets();
    void ZcullNoteClear(bool full);
    void ZcullNoteDraw();
    void ZcullRecordMark(uint32_t slot);
    void ZcullRetireMark(uint32_t slot, const PassMark &mark);
    void ReportZcull();

    // ---- draw census (r_deko9DrawCensus, deko9_drawcensus.cpp) -------------
    // The device brackets runs of draws itself (CensusAutoDraw): each bracket
    // records counter reports (samples passed, optionally fragment shader
    // invocations, and a timestamp) at both ends into slot i of a report
    // ring; slots retire in order with their list and accumulate into the
    // entry of their (material, technique, pixel shader, blend, gpupass) key.
    // m_censusPassMask is the only per-draw cost when off (PrepareDraw).
    struct DrawCensusEntry
    {
        std::string material, technique, shader;
        uint64_t psHash = 0;
        Deko9DkshStats ps{};
        uint32_t blend = 0; // CensusBlendKey
        uint64_t brackets = 0, draws = 0, prims = 0, samples = 0, fsInv = 0, ns = 0;
        uint64_t ezDraws = 0;  // draws that used the early-Z variant
        uint64_t gpuDraws = 0; // deko3d draw packets (a ranged call records one per range)
        uint32_t pass = 0;    // Deko9GpuPass of the bracket
    };
    struct CensusSlot
    {
        uint64_t seq;
        uint32_t entry, draws, prims, ezDraws, gpuDraws;
    };
    // The census brackets every draw of the frame, so the ring holds several
    // frames of per-key brackets plus an emulator's late report lag.
    static constexpr uint32_t kCensusSlots = 32768;
    static constexpr uint32_t kCensusSlotBytes = 96; // 6 x {u64 payload, u64 timestamp}
    // mode: 0 off, 1 samples + timestamps, 2 + fragment shader invocations;
    // brackets every draw whose gpupass bit is in passMask (nonzero when on).
    void SetDrawCensus(uint32_t mode, uint32_t passMask);
    void CensusLabel(const char *material, const char *technique, const char *shader, const void *psObject);
    void CensusLight(uint32_t lightIndex, uint32_t viewLights);
    void CensusAutoDraw(UINT primCount);
    // Closes an automatic bracket before non-draw GPU work (clears, copies,
    // pass markers, list ends) so it is not billed to the draws around it.
    void CensusBreak()
    {
        if (m_censusOpen)
            CensusEnd();
    }
    void CensusResetCounts();
    void CensusBegin(const char *material, const char *technique, const char *shader);
    void CensusEnd();
    void CensusNoteDraw(UINT primCount);
    // A ranged call (DrawIndexedRanges) records `count` deko3d draws for the
    // one API draw PrepareDraw noted: bill the extra packets to the bracket.
    void CensusNoteExtraGpuDraws(uint32_t extra)
    {
        if (m_censusOpen)
            m_censusGpuDraws += extra;
    }
    void RetireCensus(uint64_t seq);
    void CensusReport(const char *label, uint32_t width, uint32_t height);
    bool m_censusOpen = false;
    uint32_t m_censusMode = 0; // 0 off, 1 samples + timestamps, 2 + fragment shader invocations
    GpuAlloc m_censusReports;
    std::vector<CensusSlot> m_censusSlots;
    std::vector<DrawCensusEntry> m_drawCensusEntries;
    std::unordered_map<std::string, uint32_t> m_censusIndex;
    uint64_t m_censusHead = 0, m_censusTail = 0, m_censusDropped = 0, m_drawCensusFrames = 0;
    // Brackets whose reports never landed within kCensusLateLists lists.
    uint64_t m_censusLate = 0;
    static constexpr uint64_t kCensusLateLists = 256;
    const char *m_censusMaterial = nullptr, *m_censusTechnique = nullptr, *m_censusShader = nullptr;
    uint32_t m_censusDraws = 0, m_censusPrims = 0, m_censusBlend = 0, m_censusEzDraws = 0, m_censusGpuDraws = 0;
    PixelShader *m_censusPs = nullptr;
    uint32_t m_censusPsMask = 0;
    bool m_censusPsEarlyZ = false;
    // The pass filter, the engine's last
    // two material labels (keyed by the D3D9 pixel shader object they bound,
    // so a label never sticks to another material's draws), the open
    // bracket's key and the lights-pass counters.
    uint32_t m_censusPassMask = 0;
    struct CensusLabelSlot
    {
        const char *material = nullptr, *technique = nullptr, *shader = nullptr;
        const void *ps = nullptr;
    } m_censusLabels[2];
    uint32_t m_censusLabelNext = 0;
    const char *m_censusKeyMat = nullptr, *m_censusKeyTech = nullptr;
    uint32_t m_censusKeyPass = 0;
    uint64_t m_censusPassFrames0 = 0;
    uint32_t m_censusLight = 0, m_censusLightCounted = 0;
    uint64_t m_censusLightViews = 0, m_censusLightPartitions = 0, m_censusLightsDrawn = 0;
    uint64_t m_censusLightDraws = 0, m_censusLightNoLightDraws = 0;
    uint64_t m_censusPassNs0[32]{};

    // ---- call census storage (r_deko9Census) ----
    std::atomic<bool> m_census{false};
    std::atomic<bool> m_censusWanted{false};
    CensusEntry m_censusEntries[Census_Count]{};
    uint64_t m_censusFrames = 0;
    struct CensusTexRecord
    {
        const char *name = nullptr;
        uint32_t width = 0, height = 0, depth = 0, levels = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
        D3DPOOL pool = D3DPOOL_DEFAULT;
        DWORD usage = 0;
        uint64_t bytes = 0, calls = 0;
    };
    std::unordered_map<const void *, CensusTexRecord> m_censusTextures;
    struct CensusBufRecord
    {
        uint32_t size = 0;
        DWORD usage = 0;
        D3DPOOL pool = D3DPOOL_DEFAULT;
        bool isIndex = false;
        uint64_t locks = 0, bytes = 0;
    };
    std::unordered_map<std::string, CensusBufRecord> m_censusBuffers;

private:
    static constexpr uint32_t kStatsRing = 16;
    ListStats m_listStats[kStatsRing]{};
    // ---- GPU-fault black box state (deko9_gpufault.cpp) ----
    static constexpr uint32_t kDrawRecords = 4096;
    GpuEventRing<512> m_gpuEvents;
    std::atomic<uint32_t> m_faultTraceWanted{0};
    std::atomic<uint32_t> m_gpuMapWanted{0};
    uint32_t m_faultTrace = 0; // breadcrumb interval in draws; 0 = off
    uint32_t m_gpuMap = 0;
    bool m_gpuMapStaticLogged = false;
    std::unique_ptr<DrawRecordRing<kFenceRing, kDrawRecords>> m_drawRecords;
    GpuAlloc m_crumbs;             // 16 bytes, GPU-uncached: the GPU's (seq, draw) report
    uint32_t m_crumbDraws = 0;     // draws recorded in the open list
    std::atomic<uint64_t> m_submittedSeq{0}; // newest submitted list (read by the watcher)
    std::atomic<bool> m_watchStop{false};
    Thread m_watchThread{};
    bool m_watchRunning = false;
    void ApplyFaultTraceSettings(); // PresentFrame, with ApplyZcullSettings
    void LogGpuMapStatic();
    void FaultTraceBeginList();
    void FaultTraceDraw();
    void StopWatcher();
    static void WatchMain(void *arg);
    void DumpListStats();
    // Reports a faulted GPU queue once: FAIL:DEKO9_QUEUE_ERROR plus a
    // CRASH:GPU_QUEUE_ERROR line (frame + the last list's stats) for the
    // hardware crash log. Called from SubmitOpenList and, so a stall with no
    // further submit still gets reported, once per PresentFrame.
    void ReportQueueError();
    UINT m_presentInterval = 1;
    // Upload ring overflow uses dedicated allocations freed after the list.
    static constexpr uint32_t kUploadChunk = 8u << 20;
    static constexpr uint32_t kCmdChunk = 256u << 10; // each list starts a fresh chunk
};

// ---- helpers ----------------------------------------------------------------

// The ImageStore behind a D3D9 texture (one virtual call; used at bind time).
inline ImageStore *StoreOf(IDirect3DBaseTexture9 *texture)
{
    if (!texture)
        return nullptr;
    switch (texture->GetType())
    {
    case D3DRTYPE_TEXTURE: return static_cast<Texture2D *>(texture)->Store().get();
    case D3DRTYPE_CUBETEXTURE: return static_cast<CubeTexture *>(texture)->Store().get();
    case D3DRTYPE_VOLUMETEXTURE: return static_cast<VolumeTexture *>(texture)->Store().get();
    default: return nullptr;
    }
}

// D3D sampler index -> internal slot: PS 0..15, VS D3DVERTEXTEXTURESAMPLER0..3 -> 16..19; -1 if invalid.
inline int SamplerSlot(DWORD sampler)
{
    if (sampler < DEKO9_MAX_SAMPLERS)
        return (int)sampler;
    if (sampler >= D3DVERTEXTEXTURESAMPLER0 && sampler <= D3DVERTEXTEXTURESAMPLER3)
        return (int)(DEKO9_MAX_SAMPLERS + sampler - D3DVERTEXTEXTURESAMPLER0);
    return -1;
}

void FillCaps(D3DCAPS9 *caps);

// RAII call census (r_deko9Census): records one call to `id` in
// Device::m_censusEntries plus the wall time between construction and
// destruction (armGetSystemTick, taken directly -- see deko9_callcensus.h for
// why this is cheap enough to not sample) when the census is on; off costs
// one atomic load and no timing. AddBytes accumulates bytes deko9 itself
// copied (see deko9_callcensus.h); callers still need a switch.h include (as
// deko9_device.cpp/deko9_draw.cpp/deko9_resources.cpp all take) for
// armGetSystemTick/armTicksToNs.
struct CensusScope
{
    Device *dev;
    CensusId id;
    bool on;
    uint64_t start = 0;
    uint64_t bytes = 0;
    CensusScope(Device *device, CensusId censusId) : dev(device), id(censusId), on(device->CensusOn())
    {
        if (on)
            start = armTicksToNs(armGetSystemTick());
    }
    void AddBytes(uint64_t b) { bytes += b; }
    ~CensusScope()
    {
        if (on)
            dev->CensusRecord(id, armTicksToNs(armGetSystemTick()) - start, bytes);
    }
    CensusScope(const CensusScope &) = delete;
    CensusScope &operator=(const CensusScope &) = delete;
};

// Replaces a COM binding, holding a reference like the D3D9 runtime does.
template <class T>
void Rebind(T *&slot, T *value)
{
    if (slot == value)
        return;
    if (value)
        value->AddRef();
    if (slot)
        slot->Release();
    slot = value;
}


HRESULT LockStore(Device *device, ImageStore *store, uint32_t face, uint32_t level, const D3DBOX *box,
                  DWORD flags, LockState *state, void **bits, INT *rowPitch, INT *slicePitch);
HRESULT UnlockStore(Device *device, ImageStore *store, uint32_t face, uint32_t level, LockState *state);

template <class Base>
HRESULT STDMETHODCALLTYPE Object<Base>::QueryInterface(REFIID riid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown))
    {
        this->AddRef();
        *object = this;
        return S_OK;
    }
    return E_NOINTERFACE;
}

// Reports FAIL:DEKO9_<what> once and returns D3DERR_INVALIDCALL.
HRESULT Fail(const char *what, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Same stdout/debug-string routing as Log/Fail but with no "DEKO9 " prefix,
// for lines whose own prefix (e.g. "CRASH:") a reader matches on directly.
void LogLine(const char *line);

inline uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// Guard tail after every CPU-written buffer/staging allocation; checked at
// unlock so a write past the locked size is reported, not silently absorbed.
constexpr uint32_t kCanaryBytes = 256;
constexpr uint8_t kCanaryByte = 0xd9;
void FillCanary(uint8_t *tail);
bool CheckCanary(const uint8_t *tail);

} // namespace deko9
