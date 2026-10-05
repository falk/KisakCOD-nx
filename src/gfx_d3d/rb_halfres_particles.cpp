// Off-screen soft particles (see rb_halfres_particles.h).

#include "rb_halfres_particles.h"


#include <deko9/deko9_native.h>
#include <deko9/deko9_particles_reference.h>
#include <qcommon/qcommon.h>
#include <universal/q_shared.h>

#include "r_dvars.h"
#include "r_halfres_particles_rules.h"
#include "r_image.h"
#include "r_init.h"
#include "r_material.h"
#include "r_rendercmds.h"
#include "r_state.h"
#include "r_utils.h"
#include "rb_gpupass.h"
#include "rb_state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

bool g_hrpRedirecting;

namespace
{

// Off-screen targets at one factor, created on first use at the largest
// scene size / factor and kept until renderer shutdown (views use their
// top-left ow x oh; nothing is ever re-laid out while the GPU may read it).
struct Targets
{
    int width = 0, height = 0;
    IDirect3DTexture9 *colorTex = nullptr; // A8R8G8B8: (C, T)
    IDirect3DSurface9 *color = nullptr;
    IDirect3DSurface9 *depth = nullptr;    // D24S8
    IDirect3DSurface9 *floatZ = nullptr;   // level 0 of floatZImage
    GfxImage floatZImage{};                // R32F, bound as TEXTURE_SRC_CODE_FLOATZ while redirected
    bool compressed = false;               // created with DkImageFlags_HwCompression (rule off)
    bool failed = false;
};
Targets s_targets[3]; // [factor]

// Per view (the back end draws one view's emissive list at a time).
struct ViewState
{
    const GfxViewInfo *view = nullptr;
    Targets *t = nullptr;
    int factor = 0;
    int ow = 0, oh = 0; // covered off-screen size
    bool inOffscreen = false;
    // Off-screen content not composited yet, and the primary sort key of its
    // first run (the smallest: the list is sorted by it).
    bool pending = false;
    uint32_t pendingKey = 0;
    // Saved scene-side source state while redirected.
    int rtWidth = 0, rtHeight = 0;
    GfxViewportBehavior behavior{};
    GfxViewport sceneViewport{};
    const GfxImage *floatZImage = nullptr;
    // Run structure of this view: 'O' / 'F' per run.
    char order[48]{};
    int orderLen = 0;
    int runs = 0, composites = 0, redirectedMaterials = 0;
} s_v;

// Classification per sorted material slot. The key is everything the rule
// reads (material, technique, its name and flags, the pass state bits), so
// a slot reused by another level's material is reclassified even when the
// allocator hands back the same addresses.
struct ClassEntry
{
    const Material *material;
    const MaterialTechnique *technique;
    const char *name;
    uint32_t flags;
    uint32_t bits[2];
    uint8_t cls; // hrp::Class
};
ClassEntry s_class[2048];
uint32_t s_classLogged;

// Blend refusal, once per material.
const Material *s_blendFailed[64];
uint32_t s_blendFailedCount;

// r_halfResParticlesStats accumulation.
struct Stats
{
    uint32_t views = 0, activeViews = 0, runs = 0, composites = 0, maxRuns = 0, redirected = 0;
} s_stats;

hrp::Class ClassifyMaterial(const Material *material, MaterialTechniqueType techType, uint32_t sortedIndex)
{
    ClassEntry &e = s_class[sortedIndex & 2047];
    const MaterialTechnique *technique = Material_GetTechnique(material, techType);
    uint32_t bits[2] = {0, 0};
    const uint32_t entry = material->stateBitsEntry[techType];
    const bool haveBits = technique && entry < material->stateBitsCount;
    if (haveBits)
    {
        bits[0] = material->stateBitsTable[entry].loadBits[0];
        bits[1] = material->stateBitsTable[entry].loadBits[1];
    }
    if (e.material == material && e.technique == technique && technique && e.name == technique->name &&
        e.flags == technique->flags && e.bits[0] == bits[0] && e.bits[1] == bits[1])
        return (hrp::Class)e.cls;
    const hrp::Reason reason = haveBits ? hrp::Classify(technique->name, technique->flags, technique->passCount, bits)
                                        : hrp::Reason::NoTechnique;
    const hrp::Class cls = reason == hrp::Reason::Offscreen ? hrp::Class::Offscreen : hrp::Class::FullRes;
    e.material = material;
    e.technique = technique;
    e.name = technique ? technique->name : nullptr;
    e.flags = technique ? technique->flags : 0;
    e.bits[0] = bits[0];
    e.bits[1] = bits[1];
    e.cls = (uint8_t)cls;
    if (s_classLogged < 256)
    {
        ++s_classLogged;
        Com_Printf(CON_CHANNEL_SYSTEM, "HRP_CLASS material=%s technique=%s class=%s reason=%s bits=0x%08x,0x%08x\n",
                   material->info.name ? material->info.name : "?",
                   technique && technique->name ? technique->name : "-",
                   cls == hrp::Class::Offscreen ? "offscreen" : "fullres", hrp::ReasonName(reason), bits[0], bits[1]);
    }
    return cls;
}

void ReleaseTargets(Targets &t)
{
    if (t.floatZImage.texture.basemap)
    {
        R_UnbindImage(&gfxCmdBufState, &t.floatZImage);
        Image_UnregisterLiveTexture(&t.floatZImage);
        t.floatZImage.texture.basemap->Release();
    }
    for (IDirect3DSurface9 *s : {t.color, t.depth, t.floatZ})
    {
        if (s)
            s->Release();
    }
    if (t.colorTex)
        t.colorTex->Release();
    t = Targets();
}

// Hardware rules: r_halfResParticlesHw
// bits = DEKO9_HRP_RULE_* (default all).
uint32_t HwRules()
{
    return r_halfResParticlesHw ? (uint32_t)r_halfResParticlesHw->current.integer & DEKO9_HRP_RULES_ALL
                                : DEKO9_HRP_RULES_ALL;
}

// r_halfResParticlesDebug bits that apply at any factor.
constexpr int kDebugNoComposite = 8, kDebugFullSync = 16, kDebugNoDepth = 32, kDebugProbe = 64;

int DebugBits()
{
    return r_halfResParticlesDebug ? r_halfResParticlesDebug->current.integer : 0;
}

// kDebugFullSync: the strongest barrier at an off-screen edge, beside the
// hazard tracker's own.
void DebugSync()
{
    if (DebugBits() & kDebugFullSync)
        Deko9_ParticleFullBarrier(dx.device);
}

// Every texel of the three targets gets a defined value once, before any
// pass uses them: (C, T) = (0, 0, 0, 1), float-Z 0, depth 1. The per-view
// clears cover only the view's rectangle; the rest of the image is never
// sampled, but it must not hold whatever the memory held before (the heap
// reuses memory freed by other images: some emulators' texture caches never show
// such bytes, the hardware does).
void ClearWholeTargets(const Targets &t)
{
    IDirect3DDevice9 *device = dx.device;
    DWORD scissor = FALSE;
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    D3DVIEWPORT9 vp{0, 0, (DWORD)t.width, (DWORD)t.height, 0.0f, 1.0f};
    device->SetRenderTarget(0, t.floatZ);
    device->SetDepthStencilSurface(t.depth);
    device->SetViewport(&vp);
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    device->SetRenderTarget(0, t.color);
    device->SetViewport(&vp);
    device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
    // The engine's cached target is stale: the next R_SetRenderTarget rebinds.
    gfxCmdBufState.renderTargetId = R_RENDERTARGET_NONE;
}

bool CreateTargets(Targets &t, int factor)
{
    int w, h;
    hrp::OffscreenSize((int)vidConfig.sceneWidth, (int)vidConfig.sceneHeight, factor, &w, &h);
    // DEKO9_HRP_RULE_UNCOMPRESSED: this thread's next images are created
    // without DkImageFlags_HwCompression (the passes refuse compressed ones).
    const bool uncompressed = (HwRules() & DEKO9_HRP_RULE_UNCOMPRESSED) != 0;
    if (uncompressed)
        Deko9_SetRtCompressionOverride(0);
    IDirect3DTexture9 *floatZTex = nullptr;
    HRESULT hr = dx.device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                          &t.colorTex, nullptr);
    if (SUCCEEDED(hr))
        hr = t.colorTex->GetSurfaceLevel(0, &t.color);
    if (SUCCEEDED(hr))
        hr = dx.device->CreateDepthStencilSurface(w, h, dx.depthStencilFormat, D3DMULTISAMPLE_NONE, 0, 0, &t.depth,
                                                  nullptr);
    if (SUCCEEDED(hr))
        hr = dx.device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &floatZTex,
                                      nullptr);
    if (SUCCEEDED(hr))
        hr = floatZTex->GetSurfaceLevel(0, &t.floatZ);
    Deko9_SetRtCompressionOverride(-1);
    if (FAILED(hr))
    {
        if (floatZTex)
            floatZTex->Release();
        ReleaseTargets(t);
        Com_Printf(CON_CHANNEL_SYSTEM, "FAIL:HRP_TARGETS factor=%d %dx%d: %s\n", factor, w, h,
                   R_ErrorDescription(hr));
        return false;
    }
    GfxImage &image = t.floatZImage;
    image.mapType = MAPTYPE_2D;
    image.texture.map = floatZTex;
    image.noPicmip = true;
    image.semantic = TS_2D;
    image.track = IMAGE_TRACK_MISC;
    image.category = IMG_CATEGORY_RENDERTARGET;
    image.width = (uint16_t)w;
    image.height = (uint16_t)h;
    image.depth = 1;
    image.name = factor == 1 ? "$hrp_floatz_full" : "$hrp_floatz_half";
    Image_RegisterLiveTexture(&image);
    Deko9_SetDebugName(floatZTex, image.name);
    Deko9_SetDebugName(t.colorTex, factor == 1 ? "$hrp_color_full" : "$hrp_color_half");
    t.width = w;
    t.height = h;
    t.compressed = Deko9_IsCompressed(t.colorTex) || Deko9_IsCompressed(t.depth) || Deko9_IsCompressed(floatZTex);
    if (uncompressed && t.compressed)
    {
        ReleaseTargets(t);
        Com_Printf(CON_CHANNEL_SYSTEM, "FAIL:HRP_TARGETS factor=%d created compressed despite the override\n",
                   factor);
        return false;
    }
    ClearWholeTargets(t);
    Com_Printf(CON_CHANNEL_SYSTEM,
               "HRP_TARGETS factor=%d size=%dx%d (scene max %ux%u): A8R8G8B8 colour, D24S8 depth, R32F float-z, "
               "compressed=%d rules=%u\n",
               factor, w, h, vidConfig.sceneWidth, vidConfig.sceneHeight, t.compressed ? 1 : 0, HwRules());
    return true;
}

void SetScissor(IDirect3DDevice9 *device, int x, int y, int w, int h)
{
    RECT r;
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    device->SetScissorRect(&r);
}

void ApplyViewport(GfxCmdBufContext context)
{
    R_Set3D(context.source);
    GfxViewport viewport;
    R_GetViewport(context.source, &viewport);
    context.state->viewport.width = 0; // force: the target bind reset the device viewport
    R_SetViewport(context.state, &viewport);
    R_UpdateViewport(context.source, &viewport);
}

void EnterOffscreen(GfxCmdBufContext context, bool clear)
{
    ViewState &v = s_v;
    Targets &t = *v.t;
    IDirect3DDevice9 *device = context.state->prim.device;
    // r_halfResParticlesDebug 1 (factor 1 only, bisection aid): the scene's
    // own depth and float-Z instead of the depth pass's copies.
    const int dbg = v.factor == 1 ? r_halfResParticlesDebug->current.integer : 0;
    const bool sceneDepth = (dbg & 3) != 0, sceneFloatZ = (dbg & 5) != 0;
    device->SetRenderTarget(0, t.color);
    device->SetDepthStencilSurface((DebugBits() & kDebugNoDepth) ? nullptr
                                   : sceneDepth ? gfxRenderTargets[R_RENDERTARGET_SCENE].surface.depthStencil
                                                : t.depth);
    // Engine bookkeeping: the next R_SetRenderTarget rebinds for real.
    context.state->renderTargetId = R_RENDERTARGET_NONE;
    GfxCmdBufSourceState *source = context.source;
    v.rtWidth = source->renderTargetWidth;
    v.rtHeight = source->renderTargetHeight;
    v.behavior = source->viewportBehavior;
    v.sceneViewport = source->sceneViewport;
    v.floatZImage = source->input.codeImages[TEXTURE_SRC_CODE_FLOATZ];
    source->renderTargetWidth = t.width;
    source->renderTargetHeight = t.height;
    source->viewportBehavior = GFX_USE_VIEWPORT_FOR_VIEW;
    source->sceneViewport.x = 0;
    source->sceneViewport.y = 0;
    source->sceneViewport.width = v.ow;
    source->sceneViewport.height = v.oh;
    if (!sceneFloatZ)
        source->input.codeImages[TEXTURE_SRC_CODE_FLOATZ] = &t.floatZImage;
    ApplyViewport(context);
    SetScissor(device, 0, 0, v.ow, v.oh);
    // (C, T) = (0, 0, 0, 1) over the viewport, unless earlier runs of the
    // same sort key are still pending (they keep accumulating).
    if (clear)
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
    DebugSync();
    g_hrpRedirecting = true;
    v.inOffscreen = true;
    ++v.runs;
}

// Back to the scene target (engine state restored); no composite.
void LeaveOffscreen(GfxCmdBufContext context)
{
    ViewState &v = s_v;
    IDirect3DDevice9 *device = context.state->prim.device;
    g_hrpRedirecting = false;
    v.inOffscreen = false;
    GfxCmdBufSourceState *source = context.source;
    source->renderTargetWidth = v.rtWidth;
    source->renderTargetHeight = v.rtHeight;
    source->viewportBehavior = v.behavior;
    source->sceneViewport = v.sceneViewport;
    source->input.codeImages[TEXTURE_SRC_CODE_FLOATZ] = v.floatZImage;
    R_SetRenderTarget(context, R_RENDERTARGET_SCENE);
    ApplyViewport(context);
    const GfxViewport &sc = v.view->scissorViewport;
    SetScissor(device, sc.x, sc.y, sc.width, sc.height);
}

bool ProbeDue();
bool ReadSurface(IDirect3DSurface9 *surface, std::vector<uint8_t> *out, uint32_t *w, uint32_t *h, uint32_t *bpp);
void Probe(const ViewState &v, const std::vector<uint8_t> &before, uint32_t sw, uint32_t sh,
           const deko9::hrpref::CompositeConstants &c, const int32_t dstRect[4]);

// Composite the pending off-screen content over the scene (scene bound).
void Composite(GfxCmdBufContext context)
{
    ViewState &v = s_v;
    Targets &t = *v.t;
    IDirect3DDevice9 *device = context.state->prim.device;
    v.pending = false;
    // r_halfResParticlesDebug 8 (bisection): drop the off-screen content.
    if (DebugBits() & kDebugNoComposite)
        return;
    // Over the scissor rectangle of the scene viewport.
    const GfxViewport &sc = v.view->scissorViewport;
    const GfxViewport &sv = v.view->sceneViewport;
    const int x0 = sc.x > sv.x ? sc.x : sv.x, y0 = sc.y > sv.y ? sc.y : sv.y;
    const int x1 = sc.x + sc.width < sv.x + sv.width ? sc.x + sc.width : sv.x + sv.width;
    const int y1 = sc.y + sc.height < sv.y + sv.height ? sc.y + sc.height : sv.y + sv.height;
    if (x1 <= x0 || y1 <= y0)
        return;
    const int32_t dstRect[4] = {x0, y0, x1 - x0, y1 - y0};
    const int origin[2] = {sv.x, sv.y}, view[2] = {sv.width, sv.height}, off[2] = {v.ow, v.oh};
    deko9::hrpref::CompositeConstants constants;
    deko9::hrpref::CompositeSetup(&constants, origin, view, off, r_halfResParticlesUpsample->current.integer,
                                  r_halfResParticlesDepthTol->current.value);
    GfxImage *fullZ = gfxRenderTargets[R_RENDERTARGET_FLOAT_Z].image;
    RB_GPU_PASS(Hrp);
    DebugSync();
    // kDebugProbe: the scene before the composite, for the reference.
    std::vector<uint8_t> before;
    uint32_t sw = 0, sh = 0, sbpp = 0;
    const bool probe = ProbeDue() &&
                       ReadSurface(gfxRenderTargets[R_RENDERTARGET_SCENE].surface.color, &before, &sw, &sh, &sbpp);
    const bool sceneFloatZ = v.factor == 1 && (r_halfResParticlesDebug->current.integer & 5) != 0;
    if (!Deko9_ParticleComposite(device, t.colorTex,
                                 sceneFloatZ && fullZ ? fullZ->texture.basemap : t.floatZImage.texture.basemap,
                                 fullZ ? fullZ->texture.basemap : nullptr,
                                 gfxRenderTargets[R_RENDERTARGET_SCENE].surface.color, dstRect, &constants))
        Com_Error(ERR_FATAL, "r_halfResParticles: composite failed (see FAIL:DEKO9_HRP_COMPOSITE)");
    DebugSync();
    if (probe)
        Probe(v, before, sw, sh, constants, dstRect);
    RB_GPU_PASS(Emissive);
    ++v.composites;
}

// kDebugProbe: after a composite, read the off-screen float-Z and colour,
// the scene float-Z and the passes' constant memory back from the GPU and
// log how the off-screen float-Z relates to the scene's (every covered
// texel must equal one texel of its factor x factor footprint), plus raw
// dumps on the SD card. Stalls on the GPU: a hardware diagnostic only.
bool ReadSurface(IDirect3DSurface9 *surface, std::vector<uint8_t> *out, uint32_t *w, uint32_t *h, uint32_t *bpp)
{
    D3DSURFACE_DESC desc;
    if (!surface || FAILED(surface->GetDesc(&desc)))
        return false;
    IDirect3DSurface9 *sys = nullptr;
    if (FAILED(dx.device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys,
                                                      nullptr)))
        return false;
    D3DLOCKED_RECT locked;
    bool ok = SUCCEEDED(dx.device->GetRenderTargetData(surface, sys)) &&
              SUCCEEDED(sys->LockRect(&locked, nullptr, D3DLOCK_READONLY));
    if (ok)
    {
        *w = desc.Width;
        *h = desc.Height;
        *bpp = 4;
        out->resize((size_t)desc.Width * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(out->data() + (size_t)y * desc.Width * 4,
                        static_cast<const uint8_t *>(locked.pBits) + (size_t)y * locked.Pitch, desc.Width * 4);
        sys->UnlockRect();
    }
    sys->Release();
    return ok;
}


// Probe due at this composite (three times, a second apart at 60 Hz).
bool ProbeDue()
{
    static uint32_t s_composites, s_probes;
    if (!(DebugBits() & kDebugProbe) || s_probes >= 3 || (s_composites++ % 120) != 60)
        return false;
    ++s_probes;
    return true;
}

void Probe(const ViewState &v, const std::vector<uint8_t> &before, uint32_t sw, uint32_t sh,
           const deko9::hrpref::CompositeConstants &c, const int32_t dstRect[4])
{
    using deko9::hrpref::Rgba;
    static uint32_t s_index;
    const uint32_t index = s_index++;
    std::vector<uint8_t> half, full, color, after;
    uint32_t hw = 0, hh = 0, fw = 0, fh = 0, cw = 0, ch = 0, aw = 0, ah = 0, bpp = 0;
    const bool read = ReadSurface(v.t->floatZ, &half, &hw, &hh, &bpp) &&
                      ReadSurface(gfxRenderTargets[R_RENDERTARGET_FLOAT_Z].surface.color, &full, &fw, &fh, &bpp) &&
                      ReadSurface(v.t->color, &color, &cw, &ch, &bpp) &&
                      ReadSurface(gfxRenderTargets[R_RENDERTARGET_SCENE].surface.color, &after, &aw, &ah, &bpp);
    uint32_t depthWords[8] = {}, compositeWords[12] = {};
    const bool constants = Deko9_ReadParticleConstants(dx.device, depthWords, compositeWords);
    if (!read || aw != sw || ah != sh || hw != cw || hh != ch)
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "FAIL:HRP_PROBE readback failed\n");
        return;
    }
    const float *hz = reinterpret_cast<const float *>(half.data());
    const float *fz = reinterpret_cast<const float *>(full.data());
    const GfxViewport &sv = v.view->sceneViewport;
    const int f = v.factor;
    auto fullAt = [&](int x, int y) {
        x = x < 0 ? 0 : x >= (int)fw ? (int)fw - 1 : x;
        y = y < 0 ? 0 : y >= (int)fh ? (int)fh - 1 : y;
        return fz[(size_t)y * fw + x];
    };
    // Off-screen float-Z against its footprint, transmittance histogram.
    uint32_t footprint = 0, total = 0, tBins[5] = {};
    for (int y = 0; y < v.oh && y < (int)hh; ++y)
        for (int x = 0; x < v.ow && x < (int)hw; ++x)
        {
            const float z = hz[(size_t)y * hw + x];
            const int bx = sv.x + x * f, by = sv.y + y * f, lx = sv.x + sv.width - 1, ly = sv.y + sv.height - 1;
            bool inFootprint = false;
            for (int dy = 0; dy < f; ++dy)
                for (int dx = 0; dx < f; ++dx)
                    inFootprint |= fullAt(bx + dx < lx ? bx + dx : lx, by + dy < ly ? by + dy : ly) == z;
            footprint += inFootprint;
            ++total;
            const uint8_t a = color[((size_t)y * cw + x) * 4 + 3];
            ++tBins[a < 64 ? 0 : a < 128 ? 1 : a < 192 ? 2 : a < 243 ? 3 : 4];
        }
    // The composite's output against the CPU reference of the same inputs.
    std::vector<Rgba> offColor((size_t)cw * ch);
    for (size_t i = 0; i < offColor.size(); ++i)
    {
        const uint8_t *p = color.data() + i * 4; // B, G, R, A
        offColor[i] = {p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f, p[3] / 255.0f};
    }
    uint32_t pixels = 0, bad = 0, maxLsb = 0, picks = 0, logged = 0;
    for (int py = dstRect[1]; py < dstRect[1] + dstRect[3]; ++py)
        for (int px = dstRect[0]; px < dstRect[0] + dstRect[2]; ++px)
        {
            const uint8_t *b = before.data() + ((size_t)py * sw + px) * 4;
            const uint8_t *a = after.data() + ((size_t)py * sw + px) * 4;
            const Rgba dst{b[2] / 255.0f, b[1] / 255.0f, b[0] / 255.0f, 1.0f};
            const Rgba s = deko9::hrpref::CompositeSample(c, offColor.data(), hz, (int)cw, fullAt(px, py), px, py);
            deko9::hrpref::CompositeConstants bilinear = c;
            bilinear.limit[2] = 0;
            const Rgba sb = deko9::hrpref::CompositeSample(bilinear, offColor.data(), hz, (int)cw, fullAt(px, py),
                                                           px, py);
            picks += s.r != sb.r || s.g != sb.g || s.b != sb.b || s.a != sb.a;
            const Rgba want = deko9::hrpref::Composite(dst, s);
            const int wr = (int)(want.r * 255.0f + 0.5f), wg = (int)(want.g * 255.0f + 0.5f),
                      wb = (int)(want.b * 255.0f + 0.5f);
            const uint32_t lsb = (uint32_t)std::max(std::abs(wr - a[2]), std::max(std::abs(wg - a[1]), std::abs(wb - a[0])));
            ++pixels;
            maxLsb = lsb > maxLsb ? lsb : maxLsb;
            if (lsb > 4)
            {
                ++bad;
                if (logged < 8 && (bad % 997) == 1)
                {
                    ++logged;
                    const float hx = ((float)px + 0.5f) * c.map[0] + c.map[2];
                    const float hy = ((float)py + 0.5f) * c.map[1] + c.map[3];
                    const int ix = (int)std::floor(hx), iy = (int)std::floor(hy);
                    const int cx = ix < 0 ? 0 : ix > c.limit[0] ? c.limit[0] : ix;
                    const int cy = iy < 0 ? 0 : iy > c.limit[1] ? c.limit[1] : iy;
                    const Rgba &t0 = offColor[(size_t)cy * cw + cx];
                    Com_Printf(CON_CHANNEL_SYSTEM,
                               "HRP_PROBE_PIXEL probe=%u at=%d,%d before=%u,%u,%u after=%u,%u,%u want=%d,%d,%d "
                               "fullz=%g halfz0=%g t0=%.3f,%.3f,%.3f,%.3f c=%.3f,%.3f,%.3f,%.3f\n",
                               index, px, py, b[2], b[1], b[0], a[2], a[1], a[0], wr, wg, wb, fullAt(px, py),
                               hz[(size_t)cy * cw + cx], t0.r, t0.g, t0.b, t0.a, s.r, s.g, s.b, s.a);
                }
            }
        }
    Com_Printf(CON_CHANNEL_SYSTEM,
               "HRP_PROBE probe=%u factor=%d view=%d,%d,%dx%d covered=%dx%d half=%ux%u full=%ux%u scene=%ux%u "
               "texels=%u in_footprint=%u t_bins=%u,%u,%u,%u,%u composite_pixels=%u composite_bad=%u max_lsb=%u "
               "ref_picks=%u constants=%d depth_ubo=%d,%d,%d,%d,%d composite_ubo=%g,%g,%g,%g,%d,%d,%d,%g\n",
               index, f, sv.x, sv.y, sv.width, sv.height, v.ow, v.oh, hw, hh, fw, fh, sw, sh, total, footprint,
               tBins[0], tBins[1], tBins[2], tBins[3], tBins[4], pixels, bad, maxLsb, picks, constants ? 1 : 0,
               (int)depthWords[0], (int)depthWords[1], (int)depthWords[2], (int)depthWords[3], (int)depthWords[4],
               *reinterpret_cast<const float *>(&compositeWords[0]), *reinterpret_cast<const float *>(&compositeWords[1]),
               *reinterpret_cast<const float *>(&compositeWords[2]), *reinterpret_cast<const float *>(&compositeWords[3]),
               (int)compositeWords[4], (int)compositeWords[5], (int)compositeWords[6],
               *reinterpret_cast<const float *>(&compositeWords[8]));
}

void NoteRun(char c)
{
    ViewState &v = s_v;
    if (v.orderLen < (int)sizeof(v.order) - 2)
    {
        v.order[v.orderLen++] = c;
        v.order[v.orderLen] = 0;
    }
    else if (v.orderLen == (int)sizeof(v.order) - 2)
    {
        v.order[v.orderLen++] = '+';
        v.order[v.orderLen] = 0;
    }
}

} // namespace

// r_halfResParticles 3 (auto): decided once per frame (RB_HrpFrameUpdate),
// so a frame never mixes both paths.
namespace
{
hrp::AutoGate s_auto;
uint64_t s_autoPrevNs, s_autoPrevFrames;
bool s_autoSeeded;

int EffectiveMode()
{
    const int mode = r_halfResParticles ? r_halfResParticles->current.integer : 0;
    return mode == 3 ? (s_auto.on ? 1 : 0) : mode;
}
} // namespace

bool RB_HrpWantsGpuPasses()
{
    return r_halfResParticles && r_halfResParticles->current.integer == 3;
}

void RB_HrpFrameUpdate()
{
    if (!RB_HrpWantsGpuPasses())
    {
        s_auto.Reset();
        s_autoSeeded = false;
        return;
    }
    uint64_t ns[Deko9GpuPass_Count];
    uint64_t frames;
    Deko9_GetGpuPassTotals(dx.device, ns, &frames);
    const uint64_t particleNs = ns[Deko9GpuPass_Emissive] + ns[Deko9GpuPass_Hrp];
    if (!s_autoSeeded)
    {
        // Targets exist before the first off-screen frame, so the switch
        // itself allocates nothing mid-frame.
        Targets &t = s_targets[2];
        if (!t.color && !t.failed && !CreateTargets(t, 2))
            t.failed = true;
        s_autoPrevNs = particleNs;
        s_autoPrevFrames = frames;
        s_autoSeeded = true;
        return;
    }
    if (frames <= s_autoPrevFrames)
        return;
    const uint64_t n = std::min<uint64_t>(frames - s_autoPrevFrames, 8);
    const uint64_t per = (particleNs - s_autoPrevNs) / (frames - s_autoPrevFrames);
    s_autoPrevNs = particleNs;
    s_autoPrevFrames = frames;
    const bool was = s_auto.on;
    for (uint64_t i = 0; i < n; ++i)
        s_auto.Update(per, r_halfResParticlesAutoOnMs->current.value, r_halfResParticlesAutoOffMs->current.value);
    if (s_auto.on != was)
        Com_Printf(CON_CHANNEL_SYSTEM, "HRP_AUTO %s\n", s_auto.on ? "on" : "off");
}

void RB_HrpBeginView(const GfxViewInfo *viewInfo)
{
    s_v = ViewState();
    const int mode = EffectiveMode();
    ++s_stats.views;
    if (!mode || !viewInfo->needsFloatZ || !R_HaveFloatZ())
        return;
    const int factor = mode == 1 ? 2 : 1;
    Targets &t = s_targets[factor];
    const uint32_t rules = HwRules();
    Deko9_SetParticleRules(dx.device, rules);
    // A changed compression rule re-creates the targets (a diagnostic A/B;
    // the device defers the old images' release past the frames using them).
    if (t.color && t.compressed == ((rules & DEKO9_HRP_RULE_UNCOMPRESSED) != 0))
        ReleaseTargets(t);
    if (t.failed)
        return;
    if (!t.color && !CreateTargets(t, factor))
    {
        t.failed = true;
        return;
    }
    const GfxViewport &sv = viewInfo->sceneViewport;
    int ow, oh;
    hrp::OffscreenSize(sv.width, sv.height, factor, &ow, &oh);
    if (ow > t.width || oh > t.height)
    {
        static bool s_logged;
        if (!s_logged)
            Com_Printf(CON_CHANNEL_SYSTEM, "FAIL:HRP_VIEW_SIZE scene viewport %dx%d needs %dx%d > targets %dx%d\n",
                       sv.width, sv.height, ow, oh, t.width, t.height);
        s_logged = true;
        return;
    }
    // Only views whose emissive list holds a soft particle pay for the depth
    // pass (one classification lookup per material run).
    const GfxDrawSurfListInfo &info = viewInfo->emissiveInfo;
    bool any = false;
    uint32_t lastIndex = UINT32_MAX;
    for (uint32_t i = 0; i < info.drawSurfCount && !any; ++i)
    {
        const uint32_t sortedIndex = info.drawSurfs[i].fields.materialSortedIndex;
        if (sortedIndex == lastIndex)
            continue;
        lastIndex = sortedIndex;
        const Material *material = rgp.sortedMaterials[sortedIndex];
        any = material && ClassifyMaterial(material, info.baseTechType, sortedIndex) == hrp::Class::Offscreen;
    }
    if (!any)
        return;
    s_v.view = viewInfo;
    s_v.t = &t;
    s_v.factor = factor;
    s_v.ow = ow;
    s_v.oh = oh;
    ++s_stats.activeViews;
    // Off-screen depth + float-Z, once per view, before the first emissive
    // draw: the scene depth and float-Z are final here (the float-Z pass
    // just ran), and nothing of the emissive list is bound yet.
    const int dbg = factor == 1 ? r_halfResParticlesDebug->current.integer : 0;
    if ((dbg & 3) && (dbg & 5))
        return;
    RB_GPU_PASS(Hrp);
    // Depth cleared through the device's ordinary Clear (a known far bound
    // for zcull before the pass writes fragment-shader depth), then the pass.
    // The engine's cached target is invalidated: R_DrawEmissiveCallback's
    // R_SetRenderTarget rebinds the scene.
    dx.device->SetRenderTarget(0, t.floatZ);
    dx.device->SetDepthStencilSurface(t.depth);
    {
        D3DVIEWPORT9 vp{0, 0, (DWORD)ow, (DWORD)oh, 0.0f, 1.0f};
        dx.device->SetViewport(&vp);
        // The device's scissor enable is restored: the engine tracks it in
        // its own callbacks and must find the device as it left it.
        DWORD scissor = FALSE;
        dx.device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
        dx.device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dx.device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        dx.device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
    }
    gfxCmdBufState.renderTargetId = R_RENDERTARGET_NONE;
    const int32_t rect[4] = {sv.x, sv.y, sv.width, sv.height};
    GfxImage *fullZ = gfxRenderTargets[R_RENDERTARGET_FLOAT_Z].image;
    if (!Deko9_ParticleDepth(dx.device, gfxRenderTargets[R_RENDERTARGET_SCENE].surface.depthStencil,
                             fullZ ? fullZ->texture.basemap : nullptr, rect, (uint32_t)factor, t.depth, t.floatZ))
        Com_Error(ERR_FATAL, "r_halfResParticles: off-screen depth pass failed (see FAIL:DEKO9_HRP_DEPTH)");
    DebugSync();
    RB_GPU_PASS(Emissive);
}

void RB_HrpEndView()
{
    ViewState &v = s_v;
    if (v.inOffscreen || v.pending)
        Com_Error(ERR_FATAL, "r_halfResParticles: view ended with an off-screen run not composited");
    s_stats.runs += v.runs;
    s_stats.composites += v.composites;
    s_stats.redirected += v.redirectedMaterials;
    if ((uint32_t)v.runs > s_stats.maxRuns)
        s_stats.maxRuns = v.runs;
    const int every = r_halfResParticlesStats ? r_halfResParticlesStats->current.integer : 0;
    if (every > 0 && s_stats.views >= (uint32_t)every)
    {
        const double n = s_stats.activeViews ? (double)s_stats.activeViews : 1.0;
        Com_Printf(CON_CHANNEL_SYSTEM,
                   "HRP_RUNS views=%u active=%u factor=%d offscreen_runs=%.2f composites=%.2f max_runs=%u "
                   "redirected_materials=%.2f last_order=%s\n",
                   s_stats.views, s_stats.activeViews, v.factor, s_stats.runs / n, s_stats.composites / n,
                   s_stats.maxRuns, s_stats.redirected / n, v.orderLen ? v.order : "-");
        s_stats = Stats();
    }
    s_v = ViewState();
    g_hrpRedirecting = false;
}

bool RB_HrpListActive(const GfxDrawSurfListInfo *info)
{
    return s_v.view && info == &s_v.view->emissiveInfo;
}

void RB_HrpBeforeMaterial(GfxCmdBufContext context, GfxCmdBufContext prepassContext,
                          const GfxDrawSurfListInfo *info, uint32_t drawSurfIndex)
{
    const GfxDrawSurf drawSurf = info->drawSurfs[drawSurfIndex];
    const uint32_t sortedIndex = drawSurf.fields.materialSortedIndex;
    const Material *material = rgp.sortedMaterials[sortedIndex];
    const bool offscreen =
        material && ClassifyMaterial(material, info->baseTechType, sortedIndex) == hrp::Class::Offscreen;
    ViewState &v = s_v;
    if (offscreen)
        ++v.redirectedMaterials;
    const uint32_t key = drawSurf.fields.primarySortKey;
    // r_halfResParticlesOrder 1: a full-res run of the pending content's
    // sort key draws to the scene under it (materials of one sort key have
    // no designed order: the list orders them by sorted material index);
    // one of a later key composites first. 0: every full-res run after
    // off-screen content composites it first (strict list order).
    const bool strict = !r_halfResParticlesOrder || r_halfResParticlesOrder->current.integer == 0;
    if (offscreen == v.inOffscreen && v.orderLen)
    {
        // Same class as the previous run: a full-res run of a later key
        // still ends a deferred run.
        if (!offscreen && v.pending && key != v.pendingKey)
        {
            R_TessEnd(context, prepassContext);
            NoteRun('c');
            Composite(context);
        }
        return;
    }
    NoteRun(offscreen ? 'O' : 'F');
    if (offscreen == v.inOffscreen)
        return;
    R_TessEnd(context, prepassContext);
    if (offscreen)
    {
        const bool clear = !v.pending;
        if (!v.pending)
        {
            v.pending = true;
            v.pendingKey = key;
        }
        EnterOffscreen(context, clear);
        return;
    }
    LeaveOffscreen(context);
    if (strict || key != v.pendingKey)
    {
        NoteRun('c');
        Composite(context);
    }
}

void RB_HrpEndList(GfxCmdBufContext context)
{
    if (s_v.inOffscreen)
        LeaveOffscreen(context);
    if (s_v.pending)
    {
        NoteRun('c');
        Composite(context);
    }
}

uint32_t RB_HrpRemapStateBits0(const Material *material, uint32_t bits0)
{
    uint32_t out;
    if (hrp::RemapStateBits0(bits0, &out))
        return out;
    for (uint32_t i = 0; i < s_blendFailedCount; ++i)
    {
        if (s_blendFailed[i] == material)
            return bits0;
    }
    if (s_blendFailedCount < 64)
        s_blendFailed[s_blendFailedCount++] = material;
    Com_Printf(CON_CHANNEL_SYSTEM, "FAIL:HRP_BLEND material=%s stateBits0=0x%08x (not reproducible off-screen)\n",
               material && material->info.name ? material->info.name : "?", bits0);
    return bits0;
}

void RB_HrpShutdown()
{
    for (Targets &t : s_targets)
        ReleaseTargets(t);
    std::memset(s_class, 0, sizeof(s_class));
    s_v = ViewState();
    g_hrpRedirecting = false;
}

