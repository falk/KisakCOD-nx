// Draw-time translation of D3D9 state to deko3d commands, plus clears,
// blits, uploads and readback for the optional deko3d renderer.

#include <switch.h>

#include "deko9_internal.h"
#include "deko9_native.h"

#include <algorithm>

namespace deko9
{
namespace
{

// deko9_baked.h spells the render-state indices out (it must not need d3d9.h).
static_assert(kRsZEnable == D3DRS_ZENABLE && kRsFillMode == D3DRS_FILLMODE && kRsZWriteEnable == D3DRS_ZWRITEENABLE &&
                  kRsAlphaTestEnable == D3DRS_ALPHATESTENABLE && kRsSrcBlend == D3DRS_SRCBLEND &&
                  kRsDestBlend == D3DRS_DESTBLEND && kRsCullMode == D3DRS_CULLMODE && kRsZFunc == D3DRS_ZFUNC &&
                  kRsAlphaRef == D3DRS_ALPHAREF && kRsAlphaFunc == D3DRS_ALPHAFUNC &&
                  kRsAlphaBlendEnable == D3DRS_ALPHABLENDENABLE && kRsStencilEnable == D3DRS_STENCILENABLE &&
                  kRsStencilFail == D3DRS_STENCILFAIL && kRsStencilZFail == D3DRS_STENCILZFAIL &&
                  kRsStencilPass == D3DRS_STENCILPASS && kRsStencilFunc == D3DRS_STENCILFUNC &&
                  kRsStencilRef == D3DRS_STENCILREF && kRsStencilMask == D3DRS_STENCILMASK &&
                  kRsStencilWriteMask == D3DRS_STENCILWRITEMASK && kRsColorWriteEnable == D3DRS_COLORWRITEENABLE &&
                  kRsBlendOp == D3DRS_BLENDOP && kRsSlopeScaleDepthBias == D3DRS_SLOPESCALEDEPTHBIAS &&
                  kRsTwoSidedStencilMode == D3DRS_TWOSIDEDSTENCILMODE && kRsCcwStencilFail == D3DRS_CCW_STENCILFAIL &&
                  kRsCcwStencilZFail == D3DRS_CCW_STENCILZFAIL && kRsCcwStencilPass == D3DRS_CCW_STENCILPASS &&
                  kRsCcwStencilFunc == D3DRS_CCW_STENCILFUNC && kRsColorWriteEnable1 == D3DRS_COLORWRITEENABLE1 &&
                  kRsColorWriteEnable2 == D3DRS_COLORWRITEENABLE2 && kRsColorWriteEnable3 == D3DRS_COLORWRITEENABLE3 &&
                  kRsBlendFactor == D3DRS_BLENDFACTOR && kRsDepthBias == D3DRS_DEPTHBIAS &&
                  kRsSeparateAlphaBlendEnable == D3DRS_SEPARATEALPHABLENDENABLE &&
                  kRsSrcBlendAlpha == D3DRS_SRCBLENDALPHA && kRsDestBlendAlpha == D3DRS_DESTBLENDALPHA &&
                  kRsBlendOpAlpha == D3DRS_BLENDOPALPHA,
              "deko9_baked.h render-state indices");
static_assert(kMaxInstanceRegs == DEKO9_MAX_INSTANCE_REGS && kVertexAttribLocations == DEKO9_MAX_VS_INPUTS &&
                  kVertexAttribLocations <= DK_MAX_VERTEX_ATTRIBS,
              "instance attribute locations");

// DXVK's correction for D3D9's integer pixel centers (half-pixel offset),
// slightly under 0.5 so exact texel-center math in games rounds the same.
constexpr float kPixelCenterBias = 0.5f - 1.0f / 128.0f;

Deko9SamplerDim DimOf(const ImageStore &store)
{
    switch (store.type)
    {
    case D3DRTYPE_CUBETEXTURE: return DEKO9_SAMPLER_CUBE;
    case D3DRTYPE_VOLUMETEXTURE: return DEKO9_SAMPLER_3D;
    default: return DEKO9_SAMPLER_2D;
    }
}

float Unpack(DWORD bits)
{
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

struct ScopedNs
{
    uint64_t *sum;
    uint64_t start = armTicksToNs(armGetSystemTick());
    ~ScopedNs() { *sum += armTicksToNs(armGetSystemTick()) - start; }
};

} // namespace

bool Device::BindsColor(const Surface *rt) const
{
    return rt && !rt->Store()->colorless;
}

void Device::ApplyRenderTargets()
{
    DkImageView views[4];
    const DkImageView *colors[4];
    uint32_t count = 0;
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (!m_renderTargets[i])
            break;
        // Depth-only pass: no colour attachment at all.
        if (!BindsColor(m_renderTargets[i]))
            continue;
        m_renderTargets[i]->MakeView(&views[i]);
        colors[count++] = &views[i];
    }
    DkImageView depth;
    if (m_depthStencil)
        m_depthStencil->MakeView(&depth);
    CmdBindTargets(colors, count, m_depthStencil ? &depth : nullptr);
    if (m_zcullStats)
        ZcullNoteTargets();
}

void Device::ApplyViewportScissor()
{
    const ImageStore &target = *m_renderTargets[0]->Store();
    const uint32_t width = target.LevelWidth(m_renderTargets[0]->Level());
    const uint32_t height = target.LevelHeight(m_renderTargets[0]->Level());
    // DkDeviceFlags_OriginUpperLeft already maps NDC +Y to the top row, as
    // D3D does (verified by deko9 selftest ORIENTATION); the bias maps D3D9
    // pixel centers onto deko3d's.
    const DkViewport viewport{(float)m_viewport.X + kPixelCenterBias,
                              (float)m_viewport.Y + kPixelCenterBias,
                              (float)m_viewport.Width,
                              (float)m_viewport.Height,
                              m_viewport.MinZ,
                              m_viewport.MaxZ};
    dkCmdBufSetViewports(m_cmd, 0, &viewport, 1);
    LONG left = 0, top = 0, right = (LONG)width, bottom = (LONG)height;
    if (m_rs[D3DRS_SCISSORTESTENABLE])
    {
        left = std::max<LONG>(left, m_scissor.left);
        top = std::max<LONG>(top, m_scissor.top);
        right = std::min<LONG>(right, m_scissor.right);
        bottom = std::min<LONG>(bottom, m_scissor.bottom);
    }
    const DkScissor scissor{(uint32_t)std::max<LONG>(left, 0), (uint32_t)std::max<LONG>(top, 0),
                            (uint32_t)std::max<LONG>(right - left, 0), (uint32_t)std::max<LONG>(bottom - top, 0)};
    dkCmdBufSetScissors(m_cmd, 0, &scissor, 1);
}

// A moved-from target holds stale pixels, so its first draw or clear must
// overwrite all of them; the draw's geometry (a full-screen quad) is trusted.
bool Device::CheckUndefinedTargets(bool clear, LONG l, LONG t, LONG r, LONG b)
{
    if (!clear)
    {
        for (uint32_t st = 0; st < 2; ++st)
        {
            const Deko9ShaderInfo &info = st ? m_vs->shader.Info() : m_ps->shader.Info();
            const uint32_t base = st ? DEKO9_MAX_SAMPLERS : 0;
            for (uint32_t s = 0; s < (st ? 4u : (uint32_t)DEKO9_MAX_SAMPLERS); ++s)
            {
                const ImageStore *store = (info.samplerMask & (1u << s)) ? StoreOf(m_textures[base + s]) : nullptr;
                if (store && store->contentsUndefined)
                {
                    Fail("MOVE_STALE", "draw samples a moved-from %ux%u target before it was rewritten",
                         store->width, store->height);
                    return false;
                }
            }
        }
    }
    for (Surface *rt : m_renderTargets)
    {
        if (!rt || !rt->Store()->contentsUndefined)
            continue;
        ImageStore &store = *rt->Store();
        const LONG w = (LONG)store.width, h = (LONG)store.height;
        bool whole;
        const char *why = "";
        if (clear)
        {
            whole = l <= 0 && t <= 0 && r >= w && b >= h;
            why = "partial clear";
        }
        else
        {
            const bool alphaChannel = store.format->swizzle[3] != DkImageSwizzle_One;
            const DWORD needMask = alphaChannel ? 0xfu : 0x7u;
            const bool viewport = m_viewport.X == 0 && m_viewport.Y == 0 && (LONG)m_viewport.Width >= w &&
                                  (LONG)m_viewport.Height >= h;
            const bool scissor = !m_rs[D3DRS_SCISSORTESTENABLE] ||
                                 (m_scissor.left <= 0 && m_scissor.top <= 0 && m_scissor.right >= w && m_scissor.bottom >= h);
            const bool depth = !m_depthStencil || m_rs[D3DRS_ZENABLE] == D3DZB_FALSE || m_rs[D3DRS_ZFUNC] == D3DCMP_ALWAYS;
            const bool stencil = !m_depthStencil || !m_rs[D3DRS_STENCILENABLE];
            whole = viewport && scissor && depth && stencil && !m_rs[D3DRS_ALPHABLENDENABLE] &&
                    !m_rs[D3DRS_ALPHATESTENABLE] && (m_rs[D3DRS_COLORWRITEENABLE] & needMask) == needMask &&
                    !(m_ps && m_ps->shader.Info().kills);
            why = !viewport ? "viewport" : !scissor ? "scissor" : !depth ? "depth test" : !stencil ? "stencil test"
                  : m_rs[D3DRS_ALPHABLENDENABLE] ? "blend" : m_rs[D3DRS_ALPHATESTENABLE] ? "alpha test"
                  : (m_rs[D3DRS_COLORWRITEENABLE] & needMask) != needMask ? "colour mask" : "discard";
        }
        if (!whole)
        {
            Fail("MOVE_STALE", "first %s into a moved-from %ux%u target does not overwrite it (%s)",
                 clear ? "clear" : "draw", store.width, store.height, why);
            return false;
        }
        store.contentsUndefined = false;
        --m_undefinedTargets;
    }
    return true;
}

DepthTarget Device::CurrentDepthTarget() const
{
    DepthTarget depth;
    if (m_depthStencil)
    {
        const FormatInfo *format = m_depthStencil->Store()->format;
        depth.present = true;
        depth.stencil = format->stencil;
        depth.d16 = format->d3d == D3DFMT_D16;
    }
    return depth;
}

template <typename Record>
bool Device::Capture(std::vector<uint32_t> *words, Record &&record)
{
    constexpr uint32_t capacity = sizeof(m_bakeStorage) / sizeof(m_bakeStorage[0]);
    dkCmdBufBeginCaptureCmds(m_bakeCmd, m_bakeStorage, capacity);
    const DkCmdBuf live = m_cmd;
    m_cmd = m_bakeCmd;
    record();
    m_cmd = live;
    const uint32_t count = dkCmdBufEndCaptureCmds(m_bakeCmd);
    words->assign(m_bakeStorage, m_bakeStorage + count);
    if (count >= capacity)
    {
        Fail("BAKE", "capture overflow (%u words)", count);
        return false;
    }
    return true;
}

void Device::ReplayWords(const std::vector<uint32_t> &words)
{
    if (!words.empty())
        dkCmdBufReplayCmds(m_cmd, words.data(), (uint32_t)words.size());
    ++m_timing.bakedReplays;
    m_timing.bakedWords += words.size();
}

// Baked raster state: a lookup by the normalized render-state key replaces
// the derivation below; the words replayed are the ones the derivation
// recorded when the unit was baked (RecordRasterState, deko9_baked.h).
void Device::ApplyRasterDepthColor()
{
    const RasterKey key = BuildRasterKey(m_rs, CurrentDepthTarget());
    if (m_recorded.raster && m_recorded.raster->key == key)
        return; // e.g. a render state set and restored between draws
    const RasterUnit *unit = m_rasterUnits.Find(key);
    if (unit)
    {
        ++m_timing.bakedHits;
    }
    else
    {
        ++m_timing.bakedMisses;
        auto baked = std::make_unique<RasterUnit>();
        baked->key = key;
        Capture(&baked->words, [&] { RecordRasterState(); });
        unit = m_rasterUnits.Insert(std::move(baked));
    }
    ReplayWords(unit->words);
    m_recorded.raster = unit;
}

// The full raster/depth/stencil/color/blend derivation from the D3D render
// states: emits every state it owns (a baked unit must be complete).
void Device::RecordRasterState()
{
    const DWORD *rs = m_rs;
    DkRasterizerState raster{};
    dkRasterizerStateDefaults(&raster);
    // D3D9's front face is clockwise in window space.
    raster.frontFace = DkFrontFace_CW;
    switch (rs[D3DRS_CULLMODE])
    {
    case D3DCULL_NONE: raster.cullMode = DkFace_None; break;
    case D3DCULL_CW: raster.cullMode = DkFace_Front; break;
    case D3DCULL_CCW: raster.cullMode = DkFace_Back; break;
    default: Fail("RENDER_STATE", "CULLMODE=%u", (unsigned)rs[D3DRS_CULLMODE]); break;
    }
    DkPolygonMode fill = DkPolygonMode_Fill;
    if (!MapFillMode(rs[D3DRS_FILLMODE], &fill))
        Fail("RENDER_STATE", "FILLMODE=%u", (unsigned)rs[D3DRS_FILLMODE]);
    raster.polygonModeFront = raster.polygonModeBack = fill;
    const float depthBias = Unpack(rs[D3DRS_DEPTHBIAS]);
    const float slopeBias = Unpack(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
    if (depthBias != 0.0f || slopeBias != 0.0f)
    {
        raster.depthBiasEnableMask = DkPolygonFlag_All;
        // D3D's bias is in normalized depth; deko3d's constant factor is in
        // units of the depth format's resolution.
        float units = (float)(1u << 24);
        if (m_depthStencil && m_depthStencil->Store()->format->d3d == D3DFMT_D16)
            units = (float)(1u << 16);
        const float bias[2] = {depthBias * units, slopeBias};
        dkCmdBufSetDepthBias(m_cmd, bias[0], 0.0f, bias[1]);
    }
    dkCmdBufBindRasterizerState(m_cmd, &raster);

    DkDepthStencilState ds{};
    dkDepthStencilStateDefaults(&ds);
    const bool haveDepth = m_depthStencil != nullptr;
    ds.depthTestEnable = haveDepth && rs[D3DRS_ZENABLE] != D3DZB_FALSE;
    ds.depthWriteEnable = ds.depthTestEnable && rs[D3DRS_ZWRITEENABLE];
    DkCompareOp op;
    if (!MapCompare(rs[D3DRS_ZFUNC], &op))
        Fail("RENDER_STATE", "ZFUNC=%u", (unsigned)rs[D3DRS_ZFUNC]), op = DkCompareOp_Lequal;
    ds.depthCompareOp = op;
    ds.stencilTestEnable = haveDepth && m_depthStencil->Store()->format->stencil && rs[D3DRS_STENCILENABLE];
    if (ds.stencilTestEnable)
    {
        DkStencilOp fail, zfail, pass;
        DkCompareOp func;
        if (!MapStencilOp(rs[D3DRS_STENCILFAIL], &fail) || !MapStencilOp(rs[D3DRS_STENCILZFAIL], &zfail) ||
            !MapStencilOp(rs[D3DRS_STENCILPASS], &pass) || !MapCompare(rs[D3DRS_STENCILFUNC], &func))
            Fail("RENDER_STATE", "stencil front ops %u/%u/%u func %u", (unsigned)rs[D3DRS_STENCILFAIL],
                 (unsigned)rs[D3DRS_STENCILZFAIL], (unsigned)rs[D3DRS_STENCILPASS], (unsigned)rs[D3DRS_STENCILFUNC]);
        ds.stencilFrontFailOp = fail;
        ds.stencilFrontDepthFailOp = zfail;
        ds.stencilFrontPassOp = pass;
        ds.stencilFrontCompareOp = func;
        if (rs[D3DRS_TWOSIDEDSTENCILMODE])
        {
            // CCW (back-facing in D3D9) triangles use the CCW_ state.
            if (!MapStencilOp(rs[D3DRS_CCW_STENCILFAIL], &fail) || !MapStencilOp(rs[D3DRS_CCW_STENCILZFAIL], &zfail) ||
                !MapStencilOp(rs[D3DRS_CCW_STENCILPASS], &pass) || !MapCompare(rs[D3DRS_CCW_STENCILFUNC], &func))
                Fail("RENDER_STATE", "stencil back ops %u/%u/%u func %u", (unsigned)rs[D3DRS_CCW_STENCILFAIL],
                     (unsigned)rs[D3DRS_CCW_STENCILZFAIL], (unsigned)rs[D3DRS_CCW_STENCILPASS],
                     (unsigned)rs[D3DRS_CCW_STENCILFUNC]);
        }
        ds.stencilBackFailOp = fail;
        ds.stencilBackDepthFailOp = zfail;
        ds.stencilBackPassOp = pass;
        ds.stencilBackCompareOp = func;
        const uint8_t writeMask = (uint8_t)rs[D3DRS_STENCILWRITEMASK];
        const uint8_t ref = (uint8_t)rs[D3DRS_STENCILREF];
        const uint8_t readMask = (uint8_t)rs[D3DRS_STENCILMASK];
        dkCmdBufSetStencil(m_cmd, DkFace_FrontAndBack, writeMask, ref, readMask);
    }
    dkCmdBufBindDepthStencilState(m_cmd, &ds);

    DkColorState color{};
    dkColorStateDefaults(&color);
    const bool blend = rs[D3DRS_ALPHABLENDENABLE] != 0;
    for (uint32_t i = 0; i < 4; ++i)
        dkColorStateSetBlendEnable(&color, i, blend);
    if (rs[D3DRS_ALPHATESTENABLE])
    {
        DkCompareOp alphaOp;
        if (!MapCompare(rs[D3DRS_ALPHAFUNC], &alphaOp))
            Fail("RENDER_STATE", "ALPHAFUNC=%u", (unsigned)rs[D3DRS_ALPHAFUNC]), alphaOp = DkCompareOp_Always;
        color.alphaCompareOp = alphaOp;
        const float alphaRef = (float)(rs[D3DRS_ALPHAREF] & 0xff) / 255.0f;
        dkCmdBufSetAlphaRef(m_cmd, alphaRef);
    }
    dkCmdBufBindColorState(m_cmd, &color);

    DkColorWriteState writes{};
    dkColorWriteStateDefaults(&writes);
    dkColorWriteStateSetMask(&writes, 0, rs[D3DRS_COLORWRITEENABLE]);
    dkColorWriteStateSetMask(&writes, 1, rs[D3DRS_COLORWRITEENABLE1]);
    dkColorWriteStateSetMask(&writes, 2, rs[D3DRS_COLORWRITEENABLE2]);
    dkColorWriteStateSetMask(&writes, 3, rs[D3DRS_COLORWRITEENABLE3]);
    dkCmdBufBindColorWriteState(m_cmd, &writes);

    if (blend)
    {
        DWORD src = rs[D3DRS_SRCBLEND], dst = rs[D3DRS_DESTBLEND];
        if (src == D3DBLEND_BOTHSRCALPHA)
            src = D3DBLEND_SRCALPHA, dst = D3DBLEND_INVSRCALPHA;
        else if (src == D3DBLEND_BOTHINVSRCALPHA)
            src = D3DBLEND_INVSRCALPHA, dst = D3DBLEND_SRCALPHA;
        DWORD srcA = src, dstA = dst, opA = rs[D3DRS_BLENDOP];
        if (rs[D3DRS_SEPARATEALPHABLENDENABLE])
        {
            srcA = rs[D3DRS_SRCBLENDALPHA];
            dstA = rs[D3DRS_DESTBLENDALPHA];
            opA = rs[D3DRS_BLENDOPALPHA];
            if (srcA == D3DBLEND_BOTHSRCALPHA)
                srcA = D3DBLEND_SRCALPHA, dstA = D3DBLEND_INVSRCALPHA;
            else if (srcA == D3DBLEND_BOTHINVSRCALPHA)
                srcA = D3DBLEND_INVSRCALPHA, dstA = D3DBLEND_SRCALPHA;
        }
        DkBlendState state{};
        dkBlendStateDefaults(&state);
        DkBlendFactor f[4];
        DkBlendOp o[2];
        if (!MapBlendFactor(src, &f[0]) || !MapBlendFactor(dst, &f[1]) || !MapBlendFactor(srcA, &f[2]) ||
            !MapBlendFactor(dstA, &f[3]) || !MapBlendOp(rs[D3DRS_BLENDOP], &o[0]) || !MapBlendOp(opA, &o[1]))
        {
            Fail("RENDER_STATE", "blend %u/%u op %u alpha %u/%u op %u", (unsigned)src, (unsigned)dst,
                 (unsigned)rs[D3DRS_BLENDOP], (unsigned)srcA, (unsigned)dstA, (unsigned)opA);
        }
        else
        {
            dkBlendStateSetFactors(&state, f[0], f[1], f[2], f[3]);
            dkBlendStateSetOps(&state, o[0], o[1]);
        }
        const DkBlendState states[4] = {state, state, state, state};
        dkCmdBufBindBlendStates(m_cmd, 0, states, 4);
        const D3DCOLOR factor = rs[D3DRS_BLENDFACTOR];
        const float blendConst[4] = {((factor >> 16) & 0xff) / 255.0f, ((factor >> 8) & 0xff) / 255.0f,
                                     (factor & 0xff) / 255.0f, ((factor >> 24) & 0xff) / 255.0f};
        dkCmdBufSetBlendConst(m_cmd, blendConst[0], blendConst[1], blendConst[2], blendConst[3]);
    }
}

void Device::ApplyTextures(Deko9Stage stage, const Deko9ShaderInfo &info, uint32_t *shadowMask)
{
    const uint32_t base = stage == DEKO9_STAGE_VERTEX ? DEKO9_MAX_SAMPLERS : 0;
    const uint32_t limit = stage == DEKO9_STAGE_VERTEX ? 4 : DEKO9_MAX_SAMPLERS;
    DkResHandle handles[DEKO9_MAX_SAMPLERS]{};
    uint32_t count = 0;
    *shadowMask = 0;
    const uint32_t st = stage == DEKO9_STAGE_VERTEX ? 1 : 0;
    Sampled &sampled = m_sampled[st];
    sampled.count = 0;
    // DEKO9_PERDRAW_TEXTURES: a slot whose texture and sampler state did not
    // change since its last resolution, sampled at the same dimension,
    // reuses that resolution (store, handle, depth compare).
    const bool incremental = (m_perDraw & DEKO9_PERDRAW_TEXTURES) != 0;
    uint32_t resolved = 0;
    // Visit only the sampled slots, in ascending order like the scan over
    // [0, limit) it replaces. `continue` still advances to the next set bit.
    for (uint32_t pending = info.samplerMask & ((1u << limit) - 1u); pending; pending &= pending - 1u)
    {
        const uint32_t s = (uint32_t)__builtin_ctz(pending);
        TexSlot &cache = m_texSlot[st][s];
        if (incremental && cache.valid && cache.dim == (uint8_t)info.samplerDim[s] &&
            !(m_texSlotDirty[st] & (1u << s)))
        {
            // S4a: same store as the last resolution (a cache hit, not a new
            // bind) -- a static store needs no per-draw check unless a copy
            // wrote it while it stayed bound (pendingRaw); PrepareDraw's
            // post-commit restamp keeps its readEpoch current across any
            // other barrier (see PrepareDraw).
            m_timing.staticSamples += !cache.store->attachment;
            if (NeedsStaticHazardCheck(cache.store))
            {
                HazardAdd(cache.store, Access::Sample);
                cache.store->pendingRaw = false;
                m_timing.staticHazardChecks += !cache.store->attachment;
            }
            sampled.stores[sampled.count++] = cache.store;
            if (cache.compare)
                *shadowMask |= 1u << s;
            handles[s] = cache.handle;
            count = s + 1;
            continue;
        }
        resolved |= 1u << s;
        ImageStore *store = m_texStores[base + s];
        if (!store && m_texForgotten[base + s])
            Fail("TEXTURE_BIND", "sampler %u samples a texture destroyed while bound", s);
        if (store && !store->gpu)
        {
            Fail("TEXTURE_BIND", "sampler %u bound to a SYSTEMMEM texture", s);
            store = nullptr;
        }
        if (store && DimOf(*store) != info.samplerDim[s])
        {
            Fail("TEXTURE_BIND", "sampler %u dimension %d bound to texture type %d", s, (int)info.samplerDim[s],
                 (int)store->type);
            store = nullptr;
        }
        if (!store)
            store = m_dummy[info.samplerDim[s]];
        // A genuine new bind (cache miss): committed with the targets in
        // PrepareDraw. S4a: for a static store this is exactly the bind-time
        // check the brief calls for -- HazardCheck's Sample case reduces to
        // `copyWriteEpoch/blitEpoch == writeClock` (a static store's
        // renderEpoch is never stamped), so this unconditional call already
        // catches a copy-write RAW and (via HazardCommit) stamps readEpoch;
        // no separate logic needed here. Any stale pendingRaw is moot now.
        HazardAdd(store, Access::Sample);
        store->pendingRaw = false;
        m_timing.staticSamples += !store->attachment;
        m_timing.staticHazardChecks += !store->attachment;
        sampled.stores[sampled.count++] = store;
        const bool compare = CompareSampler(store->format->depth, info.samplerDim[s]);
        if (compare)
            *shadowMask |= 1u << s;
        handles[s] = dkMakeTextureHandle(store->descriptor, ResolveSampler(base + s, compare));
        count = s + 1;
        cache = {store, handles[s], (uint8_t)info.samplerDim[s], compare, true};
    }
    // Only the slots resolved here are clean again; a dirty slot the shader
    // does not sample stays dirty until one does.
    m_texSlotDirty[st] &= ~resolved;
    m_timing.texSlotsResolved += (uint32_t)__builtin_popcount(resolved);
    if (count && (count > m_recorded.textureCount[st] ||
                  std::memcmp(handles, m_recorded.textures[st], count * sizeof(DkResHandle))))
    {
        dkCmdBufBindTextures(m_cmd, stage == DEKO9_STAGE_VERTEX ? DkStage_Vertex : DkStage_Fragment, 0, handles,
                             count);
        // Slots past count keep what was bound; only [0, count) is known.
        std::memcpy(m_recorded.textures[st], handles, count * sizeof(DkResHandle));
        m_recorded.textureCount[st] = std::max(m_recorded.textureCount[st], count);
    }
}

bool Device::ApplyVertexInput()
{
    if (!m_decl)
    {
        Fail("DRAW", "no vertex declaration bound");
        return false;
    }
    if (m_dirtyInput)
    {
        if (!ApplyVertexStreams())
            return false;
        m_dirtyInput = false;
        ++m_timing.streamApplies;
    }
    return true;
}

// Attribute layout for the bound declaration and VS inputs; with an
// instance layout, locations InstanceAttribBase(count) + i read float4 i of the
// instance stream (the stream after the declaration's).
void Device::RecordVertexAttribs(const Deko9ShaderInfo &vsInfo, const InstanceLayout &instance)
{
    DkVtxAttribState attribs[DK_MAX_VERTEX_ATTRIBS];
    uint32_t count = 0;
    auto fixed = [] {
        DkVtxAttribState attrib{};
        attrib.isFixed = 1;
        attrib.size = DkVtxAttribSize_4x32;
        attrib.type = DkVtxAttribType_Float;
        return attrib;
    };
    for (uint32_t reg = 0; reg < DEKO9_MAX_VS_INPUTS; ++reg)
    {
        if (!(vsInfo.inputMask >> reg))
            break;
        DkVtxAttribState attrib = fixed();
        if (vsInfo.inputMask & (1u << reg))
        {
            const D3DVERTEXELEMENT9 *match = nullptr;
            for (const D3DVERTEXELEMENT9 &e : m_decl->elements)
            {
                if (e.Usage == vsInfo.inputUsage[reg] && e.UsageIndex == vsInfo.inputUsageIndex[reg])
                {
                    match = &e;
                    break;
                }
            }
            VertexFormat format;
            if (match && MapDeclType(match->Type, &format))
            {
                attrib.isFixed = 0;
                attrib.bufferId = match->Stream;
                attrib.offset = match->Offset;
                attrib.size = format.size;
                attrib.type = format.type;
                attrib.isBgra = format.bgra;
            }
            else if (match)
            {
                Fail("VERTEX_DECL", "element type %u unsupported", (unsigned)match->Type);
            }
            // An input the declaration does not feed reads a constant,
            // as D3D9 leaves it undefined.
        }
        attribs[count++] = attrib;
    }
    if (instance.count)
    {
        while (count < InstanceAttribBase(instance.count))
            attribs[count++] = fixed();
        for (uint32_t i = 0; i < instance.count; ++i)
        {
            DkVtxAttribState attrib = fixed();
            attrib.isFixed = 0;
            attrib.bufferId = m_decl->streamCount;
            attrib.offset = i * 16;
            attribs[count++] = attrib;
        }
    }
    if (count)
        dkCmdBufBindVtxAttribState(m_cmd, attribs, count);
}

bool Device::ApplyVertexStreams()
{
    // Stamps lastUse for the open list; m_dirtyInput is set again by
    // BeginList, so every list that draws from a buffer stamps it.
    uint32_t streams = m_decl->streamCount;
    DkVtxBufferState states[DK_MAX_VERTEX_BUFFERS]{};
    DkBufExtents extents[DK_MAX_VERTEX_BUFFERS]{};
    for (uint32_t s = 0; s < streams; ++s)
    {
        const Stream &stream = m_streams[s];
        states[s] = {stream.stride, 0};
        if (stream.buffer)
        {
            Buffer &buffer = stream.buffer->buffer;
            extents[s] = {buffer.Gpu() + stream.offset,
                          buffer.Size() > stream.offset ? buffer.Size() - stream.offset : 0};
            buffer.StampUse(this, m_openSeq);
        }
        else if (stream.forgotten)
        {
            // Non-owning slot whose buffer was destroyed while bound.
            Fail("BUFFER_BIND", "stream %u reads a vertex buffer destroyed while bound", s);
            return false;
        }
    }
    if (m_instancing.drawing)
    {
        // Instance-rate stream: one record of layout.count float4 per instance.
        states[streams] = {m_instancing.layout.count * 16u, 1};
        extents[streams] = m_instancing.extent;
        ++streams;
    }
    // Inline compares: a libc memcmp call per draw for a few words cost
    // a noticeable share of the stream path.
    const auto sameStates = [&] {
        for (uint32_t s = 0; s < streams; ++s)
        {
            if (states[s].stride != m_recorded.streamStates[s].stride ||
                states[s].divisor != m_recorded.streamStates[s].divisor)
                return false;
        }
        return true;
    };
    const auto sameExtents = [&] {
        for (uint32_t s = 0; s < streams; ++s)
        {
            if (extents[s].addr != m_recorded.streamExtents[s].addr ||
                extents[s].size != m_recorded.streamExtents[s].size)
                return false;
        }
        return true;
    };
    if (streams && (streams != m_recorded.streams || !sameStates()))
    {
        dkCmdBufBindVtxBufferState(m_cmd, states, streams);
        std::memcpy(m_recorded.streamStates, states, streams * sizeof(DkVtxBufferState));
        m_recorded.streams = streams;
        dkCmdBufBindVtxBuffers(m_cmd, 0, extents, streams);
        std::memcpy(m_recorded.streamExtents, extents, streams * sizeof(DkBufExtents));
    }
    else if (streams && !sameExtents())
    {
        dkCmdBufBindVtxBuffers(m_cmd, 0, extents, streams);
        std::memcpy(m_recorded.streamExtents, extents, streams * sizeof(DkBufExtents));
    }
    return true;
}

// Texture binding of both stages; the program (shader pair) is bound by
// ApplyProgram with the depth-compare mask found here.
bool Device::ApplyShaders()
{
    uint32_t vsShadow = 0, psShadow = 0;
    ApplyTextures(DEKO9_STAGE_VERTEX, m_vs->shader.Info(), &vsShadow);
    ApplyTextures(DEKO9_STAGE_PIXEL, m_ps->shader.Info(), &psShadow);
    if (vsShadow)
    {
        Fail("DRAW", "depth texture bound to a vertex sampler");
        return false;
    }
    m_psCompareMask = psShadow;
    return true;
}

ProgramUnit *Device::BakeProgram(const ProgramKey &key, uint32_t psShadow)
{
    // Timing and lock-wait attribution only: a variant missing from the
    // shader pack compiles here, at draw time, with the device lock held.
    const uint64_t bakeStart = LockClockNs();
    const bool locked = m_lock.OwnedByCaller();
    // Other threads that block on the lock meanwhile are attributed to the
    // "shaderbake" site (the site is owner-only, so only when locked).
    struct BakeSite
    {
        DeviceLock *lock;
        const char *prev;
        ~BakeSite()
        {
            if (lock)
                lock->SetSite(prev);
        }
    } site{locked ? &m_lock : nullptr, locked ? m_lock.SetSite("shaderbake") : nullptr};
    const InstanceLayout none{};
    const InstanceLayout &instance = key.instance ? m_instancing.layout : none;
    bool vsBuilt = false, psBuilt = false;
    const ShaderVariant *vs = m_vs->shader.Variant(this, 0, instance, false, &vsBuilt);
    const ShaderVariant *ps = m_ps->shader.Variant(this, psShadow, {}, key.psEarlyZ != 0, &psBuilt);
    // Every variant a loaded material pass can select is built at load time
    // (Deko9_PrebakeVariants); one built here stalled this draw. Reported
    // once per variant (Fail dedups), counted every time, drawn correctly.
    if (vsBuilt)
    {
        m_shaderStats.NoteDrawBuild();
        Fail("SHADER_PREBAKE", "vs %016llx inst=%u built at draw time: no load-time prebake covered it",
             (unsigned long long)m_vs->shader.Hash(), (unsigned)instance.count);
    }
    if (psBuilt)
    {
        m_shaderStats.NoteDrawBuild();
        Fail("SHADER_PREBAKE", "ps %016llx mask=0x%x ez=%u built at draw time: no load-time prebake covered it",
             (unsigned long long)m_ps->shader.Hash(), psShadow, key.psEarlyZ);
    }
    if (!vs || !ps)
        return nullptr;
    auto unit = std::make_unique<ProgramUnit>();
    unit->key = key;
    unit->vs = vs;
    unit->ps = ps;
    const DkShader *shaders[2] = {&vs->shader, &ps->shader};
    if (!Capture(&unit->shaderWords, [&] { dkCmdBufBindShaders(m_cmd, DkStageFlag_GraphicsMask, shaders, 2); }) ||
        !Capture(&unit->attribWords, [&] { RecordVertexAttribs(m_vs->shader.Info(), instance); }))
        return nullptr;
    unit->bakeNs = LockClockNs() - bakeStart;
    m_shaderStats.NoteBake(unit->bakeNs, locked);
    AddFrameExtra(DEKO9_EXTRA_BAKE, unit->bakeNs);
    return m_programUnits.Insert(std::move(unit));
}

// The draw qualifies for the pixel shader's early-Z variant: the program
// can discard (or the alpha test is on) and does not write depth, the draw
// tests depth but writes neither depth nor stencil, and no occlusion query
// is open.
bool Device::EarlyZCandidate() const
{
    if (!EarlyZShaderEligible(m_ps->shader.Info(), m_rs[D3DRS_ALPHATESTENABLE] != 0) || m_occlusionOpen > 0)
        return false;
    return RasterAllowsEarlyZ(m_rs, CurrentDepthTarget());
}

// Baked program: one lookup by (VS, PS, declaration, depth-compare mask,
// instance layout, early-Z) replaces the variant lookups, the attribute-layout
// derivation and its cache; the unit's shader and attribute words are
// replayed only when they differ from what the list last recorded.
bool Device::ApplyProgram(uint32_t psShadow)
{
    if (!m_decl)
    {
        Fail("DRAW", "no vertex declaration bound");
        return false;
    }
    const ProgramKey key{m_vs->shader.id, m_ps->shader.id, m_decl->id, psShadow,
                         m_instancing.drawing ? m_instancing.layoutHash : 0, m_psEarlyZ ? 1u : 0u};
    const ProgramUnit *unit = m_program;
    uint64_t bakedNowNs = 0; // a unit baked by this call: its first bind pays that lock time
    if (!unit || m_programKey != key)
    {
        unit = m_programUnits.Find(key);
        if (unit)
        {
            ++m_timing.bakedHits;
        }
        else
        {
            ++m_timing.bakedMisses;
            unit = BakeProgram(key, psShadow);
            if (!unit)
                return false;
            bakedNowNs = unit->bakeNs;
        }
        m_program = unit;
        m_programKey = key;
    }
    if (unit->vs != m_boundVs || unit->ps != m_boundPs)
    {
        if (!unit->vs->bound || !unit->ps->bound)
        {
            Log("first bind seq=%llu vs=%016llx ps=%016llx mask=0x%x inst=%u ez=%u bakeUs=%llu", (unsigned long long)m_openSeq,
                (unsigned long long)m_vs->shader.Hash(), (unsigned long long)m_ps->shader.Hash(), psShadow,
                (unsigned)unit->vs->instance.count, (unsigned)unit->ps->earlyZ,
                (unsigned long long)(bakedNowNs / 1000));
            m_shaderStats.NoteFirstBind(bakedNowNs);
            unit->vs->bound = unit->ps->bound = true;
        }
        ReplayWords(unit->shaderWords);
        m_boundVs = unit->vs;
        m_boundPs = unit->ps;
    }
    if (!m_recorded.attribs || m_recorded.attribDecl != key.decl || m_recorded.attribVs != key.vs ||
        m_recorded.attribInstance != key.instance)
    {
        ReplayWords(unit->attribWords);
        m_recorded.attribs = true;
        m_recorded.attribDecl = key.decl;
        m_recorded.attribVs = key.vs;
        m_recorded.attribInstance = key.instance;
        ++m_timing.attribApplies;
    }
    return true;
}

void Device::ForgetProgramObject(uint32_t id)
{
    DeviceLockGuard lock(m_lock);
    m_programUnits.RemoveIf([id](const ProgramUnit &unit) { return unit.key.Uses(id); });
    m_program = nullptr;
    m_recorded.attribs = false;
    m_dirtyShaders = m_dirtyAttribs = true;
}

template <uint32_t Regs>
void Device::PushConstants(ConstantFile<Regs> &file, uint32_t stage, const GpuAlloc &ubo)
{
    // DEKO9_PERDRAW_CONSTS: the file's O(1) any-dirty flag instead of
    // scanning the dirty bitmask (r_deko9Verify checks they agree).
    if (!((m_perDraw & DEKO9_PERDRAW_CONSTS) ? file.AnyDirty() : file.Dirty()))
        return;
    PushDirtyConstants(file, stage, ubo);
}

template <uint32_t Regs>
void Device::PushDirtyConstants(ConstantFile<Regs> &file, uint32_t stage, const GpuAlloc &ubo)
{
    // Pipelined inline updates: earlier draws keep the old values. Runs are
    // chunked to 1 KB by ConstantFile (some emulators limit inline updates).
    float(*shadow)[4] = m_verify ? (stage ? m_psShadow : m_vsShadow) : nullptr;
    m_timing.constPushes[stage] += file.Flush([&](uint32_t reg, uint32_t regs) {
        dkCmdBufPushConstants(m_cmd, ubo.gpu, ubo.size, reg * 16, regs * 16, file.regs[reg]);
        m_timing.constBytes[stage] += regs * 16;
        if (shadow)
            std::memcpy(shadow[reg], file.regs[reg], regs * 16);
    });
}

void Device::ApplyConstants(Deko9Stage stage)
{
    if (stage == DEKO9_STAGE_VERTEX)
        PushConstants(m_vsFile, 0, m_vsUbo);
    else
        PushConstants(m_psFile, 1, m_psUbo);
}

HRESULT Device::PrepareDraw(D3DPRIMITIVETYPE type, UINT primCount, DkPrimitive *prim, uint32_t *count)
{
    if (!MapPrimitive(type, primCount, prim, count))
        return Fail("DRAW", "primitive type %d", (int)type);
    if (m_probe.flags)
    {
        // Probe: re-derive state the fast paths would have skipped.
        if (m_probe.flags & DEKO9_PROBE_CONSTS)
            m_vsFile.MarkAllDirty(), m_psFile.MarkAllDirty();
        if (m_probe.flags & DEKO9_PROBE_TEXTURES)
            m_dirtyTextures = true, m_texSlotDirty[0] = m_texSlotDirty[1] = ~0u;
        if (m_probe.flags & DEKO9_PROBE_STREAMS)
            m_dirtyInput = true, m_recorded.indexAddress = 0;
    }
    if (!m_vs || !m_ps)
        return Fail("UNSUPPORTED", "fixed-function draw (vs=%p ps=%p)", (void *)m_vs, (void *)m_ps);
    if (!m_renderTargets[0])
        return Fail("DRAW", "no render target");
    if (m_undefinedTargets && !CheckUndefinedTargets(false, 0, 0, 0, 0))
        return D3DERR_INVALIDCALL;
    // Gating below relies on no list being submitted inside PrepareDraw
    // (BeginList would reset what the skipped steps rely on); checked.
    const uint64_t seq = m_openSeq;
    // Texture bindings first: they may need a barrier after render-target
    // writes, which must precede everything this draw records. With the
    // shaders, textures and sampler states unchanged since the last applied
    // binding, only the per-draw hazard check of each sampled image runs.
    bool gated = true;
    HazardBegin(); // samples (ApplyTextures or the gated list) + targets, one commit below
    const bool bindTextures = m_dirtyShaders || m_dirtyTextures;
    // DEKO9_PERDRAW_HAZARD: same sampled images (no re-binding) and targets
    // as the previous draw, whose evaluation was the last one and left the
    // barrier clock unchanged since: evaluating again would record nothing
    // and stamp the epochs it already holds (HazardRolesDisjoint).
    const bool hazardReuse = (m_perDraw & DEKO9_PERDRAW_HAZARD) && m_drawHazardValid && !bindTextures &&
                             !m_dirtyTargets && m_drawHazardSerial == m_hazardSerial &&
                             m_drawHazardClock == m_writeClock;
    m_drawHazardReused = hazardReuse;
    if (bindTextures)
    {
        if (!ApplyShaders())
            return D3DERR_INVALIDCALL;
        m_dirtyTextures = false;
        gated = false;
        ++m_timing.shaderApplies;
    }
    else if (!hazardReuse)
    {
        // S4a: shaders/textures did not change (ApplyTextures did not run
        // this draw), so this is a re-check of the same resolution as the
        // last draw's. Only attachment stores need it; a static store is
        // added only if a copy wrote it while it stayed bound (pendingRaw
        // -- see ImageStore::pendingRaw), which this draw then commits.
        for (const Sampled &sampled : m_sampled)
        {
            for (uint32_t i = 0; i < sampled.count; ++i)
            {
                ImageStore *store = sampled.stores[i];
                m_timing.staticSamples += !store->attachment;
                if (NeedsStaticHazardCheck(store))
                {
                    HazardAdd(store, Access::Sample);
                    store->pendingRaw = false;
                    m_timing.staticHazardChecks += !store->attachment;
                }
            }
        }
    }
    // Early-Z variant choice: shaders (bindTextures), render states
    // (m_dirtyRaster), depth target (m_dirtyTargets), r_deko9EarlyZ and open
    // occlusion queries (m_dirtyEarlyZ) are its inputs.
    if (bindTextures || m_dirtyRaster || m_dirtyTargets || m_dirtyEarlyZ)
    {
        m_ezCandidate = EarlyZCandidate();
        const bool earlyZ = m_ezCandidate && m_earlyZ;
        if (earlyZ != m_psEarlyZ)
        {
            m_psEarlyZ = earlyZ;
            m_dirtyAttribs = true; // re-resolve the program below
        }
        m_dirtyEarlyZ = false;
    }
    m_timing.earlyZCandidates += m_ezCandidate;
    m_timing.earlyZDraws += m_psEarlyZ;
    // The program depends on the shaders, the declaration, the
    // depth-compare mask (texture formats) and the early-Z choice, so it is
    // re-resolved whenever any of them may have changed.
    if (bindTextures || m_dirtyAttribs)
    {
        if (!ApplyProgram(m_psCompareMask))
            return D3DERR_INVALIDCALL;
        m_dirtyShaders = m_dirtyAttribs = false;
        gated = false;
    }
    if (hazardReuse)
    {
        ++m_timing.hazardSkips;
    }
    else
    {
        for (Surface *rt : m_renderTargets)
        {
            if (BindsColor(rt))
                HazardAdd(rt->Store().get(), Access::Render);
        }
        if (m_depthStencil)
            HazardAdd(m_depthStencil->Store().get(), Access::Render);
        const bool disjoint = HazardRolesDisjoint();
        HazardCommit();
        // S4a, unconditional (not just when this draw's own commit
        // barriers): the old tracker adds every currently sampled store to
        // every draw's batch, and HazardCommit's stamp loop gives every
        // entry readEpoch = the resulting clock regardless of whether that
        // entry individually hit -- so a sampled store's readEpoch tracks
        // "now" as of every draw, hit or not, even a draw whose own
        // evaluation hits nothing but where the clock had already moved
        // since this store's last real stamp (an unrelated barrier
        // elsewhere). A static store S4a's skip above left out of this
        // draw's hazard set needs that same unconditional catch-up, or a
        // later copy-write's WAR check against it sees a stale epoch and
        // misses a barrier the old tracker would have recorded (a host
        // trace mismatch either way this was gated differently: see
        // deko9_hazard_model.h).
        for (const Sampled &sampled : m_sampled)
        {
            for (uint32_t i = 0; i < sampled.count; ++i)
            {
                ImageStore *store = sampled.stores[i];
                if (!store->attachment)
                    store->readEpoch = m_writeClock;
            }
        }
        m_drawHazardValid = disjoint;
        m_drawHazardSerial = m_hazardSerial;
        m_drawHazardClock = m_writeClock;
    }
    if (m_descriptorsDirty)
    {
        // New CPU-written descriptors (AllocImageDescriptor/SamplerDescriptor).
        CmdBarrier(DkBarrier_None, DkInvalidateFlags_Descriptors | DkInvalidateFlags_L2Cache);
        m_descriptorsDirty = false;
    }
    if (m_dirtyTargets)
    {
        ApplyRenderTargets();
        m_dirtyTargets = false;
        m_dirtyViewport = true;
    }
    if (m_dirtyViewport)
    {
        ApplyViewportScissor();
        m_dirtyViewport = false;
    }
    if (m_dirtyRaster)
    {
        ApplyRasterDepthColor();
        m_dirtyRaster = false;
        gated = false;
        ++m_timing.rasterApplies;
    }
    // A buffer renamed by another thread (Buffer::Lock without the lock)
    // re-derives the stream extents here.
    NoteForeignRenames();
    gated &= !m_dirtyInput;
    if (!ApplyVertexInput())
        return D3DERR_INVALIDCALL;
    m_timing.gatedDraws += gated;
    if (m_openSeq != seq)
        return Fail("DRAW", "command list submitted inside PrepareDraw (seq %llu -> %llu)",
                    (unsigned long long)seq, (unsigned long long)m_openSeq);
    ApplyConstants(DEKO9_STAGE_VERTEX);
    ApplyConstants(DEKO9_STAGE_PIXEL);
    if (m_verify)
        VerifyDraw();
    m_listHasWork = true;
    m_drawThread = threadGetSelf();
    m_lock.SetDrawTag(ThreadTag());
    if (m_censusPassMask)
        CensusAutoDraw(primCount);
    if (m_zcullStats)
        ZcullNoteDraw();
    if (m_faultTrace)
        FaultTraceDraw();
    ++Stats().draws;
    ++m_timing.draws;
    return D3D_OK;
}

// With r_deko9Verify, like D3D9's debug runtime, reject a draw whose vertex range leaves a bound
// vertex buffer; the retail runtime and deko3d do not check, and the GPU then
// fetches wherever baseVertex points (an sv_smp 1 GPU MMU fault reads a fixed
// address far outside every deko9 memblock). Reject it loudly, with the
// caller's stack, instead.
bool Device::VertexRangeValid(int64_t first, uint64_t vertices, const char *what)
{
    // A per-draw loop over the declaration; only with r_deko9Verify, the
    // fast path's debug mode.
    if (!m_verify || !m_decl || !vertices)
        return true;
    for (const D3DVERTEXELEMENT9 &e : m_decl->elements)
    {
        const Stream &stream = m_streams[e.Stream];
        if (!stream.buffer || !stream.stride)
            continue;
        const uint64_t size = stream.buffer->buffer.Size();
        const int64_t endByte = (int64_t)stream.offset + (first + (int64_t)vertices) * stream.stride;
        if (first < 0 || endByte > (int64_t)size)
        {
            char stack[200];
            int n = 0;
            const uint64_t *fp = static_cast<const uint64_t *>(__builtin_frame_address(0));
            for (int depth = 0; depth < 8 && fp && !(reinterpret_cast<uintptr_t>(fp) & 15) && n < 180; ++depth)
            {
                n += std::snprintf(stack + n, sizeof(stack) - n, depth ? ",%llx" : "%llx", (unsigned long long)fp[1]);
                const uint64_t *next = reinterpret_cast<const uint64_t *>(fp[0]);
                if (next <= fp)
                    break;
                fp = next;
            }
            stack[n] = 0;
            Fail("VERTEX_RANGE", "%s vertices [%lld, +%llu) stream %u offset %u stride %u buffer %llu bytes base=%p stack=%s",
                 what, (long long)first, (unsigned long long)vertices, (unsigned)e.Stream, stream.offset,
                 stream.stride, (unsigned long long)size, reinterpret_cast<void *>(&Log), stack);
            return false;
        }
    }
    return true;
}

void Device::SetVerify(bool enable)
{
    if (enable && !m_verify)
    {
        // Re-push both files in full at the next draw so the shadows start
        // equal to the UBOs.
        m_vsFile.MarkAllDirty();
        m_psFile.MarkAllDirty();
        m_verifyConstantsSynced = false;
    }
    m_verify = enable;
}

// The slow path's results, recomputed from the D3D-level state without
// side effects on what is recorded, against what the fast path recorded.
void Device::VerifyDraw()
{
    ++m_verifiedDraws;
    auto mismatch = [&](const char *what, uint32_t a, uint32_t b) {
        ++m_verifyMismatches;
        Fail("FASTPATH_MISMATCH", "%s (%u vs %u) seq=%llu", what, a, b, (unsigned long long)m_openSeq);
    };
    // Binding: StoreOf through the virtual call, sampler through the full
    // SamplerKey map, shader variants through ShaderBase::Variant.
    uint32_t psShadow = 0;
    for (uint32_t st = 0; st < 2; ++st)
    {
        const Deko9Stage stage = st ? DEKO9_STAGE_VERTEX : DEKO9_STAGE_PIXEL;
        const Deko9ShaderInfo &info = st ? m_vs->shader.Info() : m_ps->shader.Info();
        const uint32_t base = st ? DEKO9_MAX_SAMPLERS : 0;
        const uint32_t limit = st ? 4 : DEKO9_MAX_SAMPLERS;
        (void)stage;
        uint32_t count = 0;
        for (uint32_t s = 0; s < limit; ++s)
        {
            if (!(info.samplerMask & (1u << s)))
                continue;
            ImageStore *store = StoreOf(m_textures[base + s]);
            if (store && (!store->gpu || DimOf(*store) != info.samplerDim[s]))
                store = nullptr;
            if (!store)
                store = m_dummy[info.samplerDim[s]];
            const bool compare = store->format->depth && info.samplerDim[s] == DEKO9_SAMPLER_2D;
            if (compare && !st)
                psShadow |= 1u << s;
            SamplerKey key{};
            std::memcpy(key.state, m_ss[base + s], sizeof(key.state));
            key.compare = compare;
            const DkResHandle handle = dkMakeTextureHandle(store->descriptor, SamplerDescriptor(key));
            if (s >= m_recorded.textureCount[st] || m_recorded.textures[st][s] != handle)
                mismatch(st ? "vs texture handle" : "ps texture handle", s,
                         s < m_recorded.textureCount[st] ? (uint32_t)m_recorded.textures[st][s] : UINT32_MAX);
            bool listed = false;
            const Sampled &sampled = m_sampled[st];
            for (uint32_t i = 0; i < sampled.count; ++i)
                listed |= sampled.stores[i] == store;
            if (!listed)
                mismatch(st ? "vs sampled store not tracked" : "ps sampled store not tracked", s, sampled.count);
            count = s + 1;
        }
        (void)count;
    }
    const InstanceLayout none{};
    const InstanceLayout &instance = m_instancing.drawing ? m_instancing.layout : none;
    const ShaderVariant *vs = m_vs->shader.Variant(this, 0, instance);
    const bool earlyZ = m_earlyZ && EarlyZCandidate();
    if (earlyZ != m_psEarlyZ)
        mismatch("early-Z choice", earlyZ, m_psEarlyZ);
    const ShaderVariant *ps = m_ps->shader.Variant(this, psShadow, {}, earlyZ);
    if (vs != m_boundVs)
        mismatch("vs variant", instance.count, m_boundVs ? m_boundVs->instance.count : UINT32_MAX);
    if (ps != m_boundPs)
        mismatch("ps variant", psShadow, m_boundPs ? m_boundPs->shadowMask : UINT32_MAX);

    // Baked units against the slow derivation, word for word: the raster
    // unit last replayed must be the one for the current render states and
    // hold what RecordRasterState records now; the program unit must be the
    // one for the bound objects and hold what binding them records now.
    std::vector<uint32_t> words;
    const RasterKey rasterKey = BuildRasterKey(m_rs, CurrentDepthTarget());
    if (!m_recorded.raster || m_recorded.raster->key != rasterKey)
        mismatch("baked raster key", m_recorded.raster ? 1u : 0u, 1u);
    else if (Capture(&words, [&] { RecordRasterState(); }) && words != m_recorded.raster->words)
        mismatch("baked raster words", (uint32_t)words.size(), (uint32_t)m_recorded.raster->words.size());
    const ProgramKey programKey{m_vs->shader.id, m_ps->shader.id, m_decl->id, psShadow,
                                m_instancing.drawing ? m_instancing.layoutHash : 0, earlyZ ? 1u : 0u};
    if (!m_program || m_program->key != programKey)
        mismatch("baked program key", m_program ? m_program->key.decl : 0u, m_decl->id);
    else
    {
        if (vs && ps && (m_program->vs != vs || m_program->ps != ps))
            mismatch("baked program variants", 0, 0);
        if (vs && ps)
        {
            const DkShader *shaders[2] = {&vs->shader, &ps->shader};
            if (Capture(&words, [&] { dkCmdBufBindShaders(m_cmd, DkStageFlag_GraphicsMask, shaders, 2); }) &&
                words != m_program->shaderWords)
                mismatch("baked shader words", (uint32_t)words.size(), (uint32_t)m_program->shaderWords.size());
        }
        if (Capture(&words, [&] { RecordVertexAttribs(m_vs->shader.Info(), instance); }) &&
            words != m_program->attribWords)
            mismatch("baked attrib words", (uint32_t)words.size(), (uint32_t)m_program->attribWords.size());
    }
    if (!m_recorded.attribs || m_recorded.attribDecl != m_decl->id || m_recorded.attribVs != m_vs->shader.id ||
        m_recorded.attribInstance != programKey.instance)
        mismatch("vertex attribs", m_decl->id, m_vs->shader.id);

    // Vertex input.
    uint32_t streams = 0;
    for (const D3DVERTEXELEMENT9 &e : m_decl->elements)
        streams = std::max<uint32_t>(streams, e.Stream + 1u);
    const uint32_t expectStreams = streams + (m_instancing.drawing ? 1u : 0u);
    if (expectStreams && expectStreams != m_recorded.streams)
        mismatch("stream count", expectStreams, m_recorded.streams);
    for (uint32_t s = 0; s < streams && s < m_recorded.streams; ++s)
    {
        const Stream &stream = m_streams[s];
        if (m_recorded.streamStates[s].stride != stream.stride)
            mismatch("stream stride", stream.stride, m_recorded.streamStates[s].stride);
        if (stream.buffer)
        {
            const Buffer &buffer = stream.buffer->buffer;
            const DkGpuAddr addr = buffer.Gpu() + stream.offset;
            if (m_recorded.streamExtents[s].addr != addr)
                mismatch("stream address", s, (uint32_t)stream.offset);
            if (buffer.LastUse() != m_openSeq)
                mismatch("stream lastUse stamp", s, (uint32_t)buffer.LastUse());
        }
    }

    // Hazards: a draw that skipped evaluation (DEKO9_PERDRAW_HAZARD) must be
    // one whose full evaluation would have recorded no barrier and changed
    // no epoch. Built from the slow derivation's inputs, never committed.
    if (m_drawHazardReused)
    {
        ++m_verifiedHazardSkips;
        HazardBegin();
        for (uint32_t st = 0; st < 2; ++st)
        {
            const Deko9ShaderInfo &info = st ? m_vs->shader.Info() : m_ps->shader.Info();
            const uint32_t base = st ? DEKO9_MAX_SAMPLERS : 0;
            const uint32_t limit = st ? 4 : DEKO9_MAX_SAMPLERS;
            for (uint32_t s = 0; s < limit; ++s)
            {
                if (!(info.samplerMask & (1u << s)))
                    continue;
                ImageStore *store = StoreOf(m_textures[base + s]);
                if (store && (!store->gpu || DimOf(*store) != info.samplerDim[s]))
                    store = nullptr;
                HazardAdd(store ? store : m_dummy[info.samplerDim[s]], Access::Sample);
            }
        }
        for (Surface *rt : m_renderTargets)
        {
            if (BindsColor(rt))
                HazardAdd(rt->Store().get(), Access::Render);
        }
        if (m_depthStencil)
            HazardAdd(m_depthStencil->Store().get(), Access::Render);
        if (HazardWouldAct())
            mismatch("skipped hazard evaluation would act", m_hazardCount, (uint32_t)m_writeClock);
        HazardBegin();
    }

    // S4a: a static store's per-draw sample hazard is skipped once bound
    // (ImageStore::attachment / pendingRaw); re-derive it with a real,
    // uncommitted HazardAdd against its actual epochs regardless of whether
    // this draw's fast path skipped it or just committed a pendingRaw
    // catch-up. It must already show no pending barrier and a current
    // readEpoch (the invariant Device::Barrier's re-stamp and pendingRaw are
    // meant to hold); if not, a copy-write into a bound static store went
    // unnoticed.
    for (uint32_t st = 0; st < 2; ++st)
    {
        const Sampled &sampled = m_sampled[st];
        for (uint32_t i = 0; i < sampled.count; ++i)
        {
            ImageStore *store = sampled.stores[i];
            if (store->attachment)
                continue;
            HazardBegin();
            HazardAdd(store, Access::Sample);
            if (HazardWouldAct())
                mismatch(st ? "vs static store hazard missed" : "ps static store hazard missed", i,
                         (uint32_t)store->readEpoch);
            HazardBegin();
        }
    }

    // Constants: the any-dirty flags agree with the bitmasks (both clean
    // after this draw's flush), and the pushes (shadow) equal the files.
    if (m_vsFile.AnyDirty() != m_vsFile.Dirty() || m_psFile.AnyDirty() != m_psFile.Dirty())
        mismatch("constant any-dirty flag", m_vsFile.AnyDirty() ? 1u : 0u, m_psFile.AnyDirty() ? 1u : 0u);
    if (m_perDraw & DEKO9_PERDRAW_TEXTURES)
        ++m_verifiedTexIncremental;
    // Constants: the pushes (shadow) must equal the register files.
    if (m_verifyConstantsSynced)
    {
        for (uint32_t r = 0; r < DEKO9_VS_CONST_REGS; ++r)
            if (std::memcmp(m_vsShadow[r], m_vsFile.regs[r], 16))
            {
                mismatch("vs constant", r, 0);
                break;
            }
        for (uint32_t r = 0; r < DEKO9_PS_CONST_REGS; ++r)
            if (std::memcmp(m_psShadow[r], m_psFile.regs[r], 16))
            {
                mismatch("ps constant", r, 0);
                break;
            }
    }
    m_verifyConstantsSynced = true; // this draw's flush was the full re-push (or later)
}

// One draw, or with the probe split mode the same triangles as several
// consecutive draws (same pixels and order, more GPU draws).
void Device::EmitDraw(bool indexed, DkPrimitive prim, uint32_t count, uint32_t first, int32_t baseVertex)
{
    uint32_t parts = m_probe.split;
    const uint32_t tris = count / 3;
    if (prim != DkPrimitive_Triangles || count % 3 || parts < 2 || tris < parts)
        parts = 1;
    uint32_t done = 0;
    for (uint32_t i = 0; i < parts; ++i)
    {
        const uint32_t n = i + 1 == parts ? tris - done : tris / parts;
        const uint32_t c = parts == 1 ? count : n * 3;
        if (i && (m_probe.flags & DEKO9_PROBE_SUBCONSTS))
        {
            m_vsFile.MarkAllDirty();
            m_psFile.MarkAllDirty();
            ApplyConstants(DEKO9_STAGE_VERTEX);
            ApplyConstants(DEKO9_STAGE_PIXEL);
        }
        const uint32_t at = first + (parts == 1 ? 0 : done * 3);
        if (indexed)
            dkCmdBufDrawIndexed(m_cmd, prim, c, 1, at, baseVertex, 0);
        else
            dkCmdBufDraw(m_cmd, prim, c, 1, at, 0);
        done += n;
    }
    m_timing.probeExtraDraws += parts - 1;
}

HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primCount)
{
    CensusScope census(this, Census_DrawPrimitive);
    ScopedNs timed{&m_timing.drawCpuNs};
    DeviceLockGuard lock(m_lock);
    SplitLongList();
    DkPrimitive prim;
    uint32_t count;
    const HRESULT hr = PrepareDraw(type, primCount, &prim, &count);
    if (FAILED(hr))
        return hr;
    if (!VertexRangeValid(startVertex, count, "DrawPrimitive"))
        return D3DERR_INVALIDCALL;
    EmitDraw(false, prim, count, startVertex, 0);
    FaultTraceDrawArgsStreams(false, prim, count, 1, startVertex, 0);
    m_lock.HandOffIfContended();
    return D3D_OK;
}

HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices,
                                     UINT startIndex, UINT primCount)
{
    CensusScope census(this, Census_DrawIndexedPrimitive);
    ScopedNs timed{&m_timing.drawCpuNs};
    DeviceLockGuard lock(m_lock);
    SplitLongList();
    if (!m_indices)
        return m_indicesForgotten ? Fail("BUFFER_BIND", "indexed draw reads an index buffer destroyed while bound")
                                  : Fail("DRAW", "indexed draw without an index buffer");
    DkPrimitive prim;
    uint32_t count;
    Buffer &ib = m_indices->buffer;
    const uint32_t indexSize = m_indices->Format() == D3DFMT_INDEX32 ? 4 : 2;
    if (MapPrimitive(type, primCount, &prim, &count) && (uint64_t)startIndex + count > ib.Size() / indexSize)
        return Fail("DRAW", "index range %u+%u exceeds index buffer of %u indices (type=%d prims=%u base=%d)",
                    startIndex, count, ib.Size() / indexSize, (int)type, primCount, baseVertex);
    const HRESULT hr = PrepareDraw(type, primCount, &prim, &count);
    if (FAILED(hr))
        return hr;
    if (!VertexRangeValid((int64_t)baseVertex + minIndex, numVertices, "DrawIndexedPrimitive"))
        return D3DERR_INVALIDCALL;
    const DkIdxFormat indexFormat = m_indices->Format() == D3DFMT_INDEX32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16;
    if (m_recorded.indexAddress != ib.Gpu() || m_recorded.indexFormat != indexFormat)
    {
        dkCmdBufBindIdxBuffer(m_cmd, indexFormat, ib.Gpu());
        m_recorded.indexAddress = ib.Gpu();
        m_recorded.indexFormat = indexFormat;
        ++m_timing.indexBinds;
    }
    // After the address read: a lock that sees this stamp is ordered after it.
    ib.StampUse(this, m_openSeq);
    EmitDraw(true, prim, count, startIndex, baseVertex);
    FaultTraceDrawArgsStreams(true, prim, count, 1, startIndex, baseVertex);
    m_lock.HandOffIfContended();
    return D3D_OK;
}

// ---- instanced draws (deko9_native.h Deko9_*Instances) ----------------------------

bool Device::BeginInstances(const InstanceLayout &layout)
{
    if (m_instancing.active)
    {
        Fail("INSTANCING", "BeginInstances inside an open batch");
        CancelInstances();
    }
    if (!layout.count || layout.count > kMaxInstanceRegs)
        return false;
    m_instancing.active = true;
    m_instancing.layout = layout;
    m_instancing.layoutHash = layout.Hash();
    m_instancing.count = 0;
    m_instancing.data.clear();
    return true;
}

void Device::AddInstance()
{
    if (!m_instancing.active)
    {
        Fail("INSTANCING", "AddInstance without BeginInstances");
        return;
    }
    // The registers exactly as an ordinary draw here would read them.
    const InstanceLayout &layout = m_instancing.layout;
    const size_t at = m_instancing.data.size();
    m_instancing.data.resize(at + layout.count * 4u);
    float *dst = m_instancing.data.data() + at;
    for (uint32_t i = 0; i < layout.count; ++i)
        std::memcpy(dst + i * 4, m_vsFile.regs[layout.regs[i]], 16);
    ++m_instancing.count;
}

void Device::CancelInstances()
{
    m_instancing.active = m_instancing.drawing = false;
    m_instancing.count = 0;
    m_instancing.data.clear();
}

HRESULT Device::DrawInstances(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices, UINT startIndex,
                              UINT primCount)
{
    ScopedNs timed{&m_timing.drawCpuNs};
    if (!m_instancing.active)
        return Fail("INSTANCING", "DrawInstances without BeginInstances");
    const uint32_t instances = m_instancing.count;
    if (!instances)
    {
        CancelInstances();
        return D3D_OK;
    }
    SplitLongList();
    if (!m_indices)
    {
        CancelInstances();
        return m_indicesForgotten ? Fail("BUFFER_BIND", "instanced draw reads an index buffer destroyed while bound")
                                  : Fail("DRAW", "instanced draw without an index buffer");
    }
    if (!m_decl || m_decl->streamCount >= DK_MAX_VERTEX_BUFFERS)
    {
        CancelInstances();
        return Fail("INSTANCING", "declaration leaves no stream for instance data");
    }
    DkPrimitive prim;
    uint32_t count;
    Buffer &ib = m_indices->buffer;
    const uint32_t indexSize = m_indices->Format() == D3DFMT_INDEX32 ? 4 : 2;
    if (MapPrimitive(type, primCount, &prim, &count) && (uint64_t)startIndex + count > ib.Size() / indexSize)
    {
        CancelInstances();
        return Fail("DRAW", "index range %u+%u exceeds index buffer of %u indices (instanced)", startIndex, count,
                    ib.Size() / indexSize);
    }
    if (!m_vs || !InstanceFits(m_vs->shader.Info().inputMask, m_instancing.layout.count))
    {
        // The instance attributes would overlap this shader's inputs (UAM
        // takes locations 0..15 only): draw each instance the ordinary way,
        // restoring its registers first. Exact, just not batched.
        const InstanceLayout layout = m_instancing.layout;
        const std::vector<float> data = std::move(m_instancing.data);
        CancelInstances();
        m_timing.instanceFallbacks += instances;
        HRESULT result = D3D_OK;
        for (uint32_t i = 0; i < instances && SUCCEEDED(result); ++i)
        {
            for (uint32_t r = 0; r < layout.count; ++r)
                m_vsFile.Set(layout.regs[r], &data[(i * layout.count + r) * 4], 1);
            result = DrawIndexedPrimitive(type, baseVertex, minIndex, numVertices, startIndex, primCount);
        }
        return result;
    }
    const uint32_t bytes = (uint32_t)(m_instancing.data.size() * sizeof(float));
    GpuAlloc upload;
    if (!AllocUpload(bytes, 256, &upload))
    {
        CancelInstances();
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    std::memcpy(upload.cpu, m_instancing.data.data(), bytes);
    m_instancing.extent = {upload.gpu, bytes};
    // The instanced program (VS variant + attribute layout) and the stream
    // set with the instance stream apply for this draw only.
    m_instancing.drawing = true;
    m_dirtyAttribs = m_dirtyInput = true;
    const HRESULT hr = PrepareDraw(type, primCount, &prim, &count);
    if (SUCCEEDED(hr) && VertexRangeValid((int64_t)baseVertex + minIndex, numVertices, "DrawInstances"))
    {
        const DkIdxFormat indexFormat = indexSize == 4 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16;
        if (m_recorded.indexAddress != ib.Gpu() || m_recorded.indexFormat != indexFormat)
        {
            dkCmdBufBindIdxBuffer(m_cmd, indexFormat, ib.Gpu());
            m_recorded.indexAddress = ib.Gpu();
            m_recorded.indexFormat = indexFormat;
            ++m_timing.indexBinds;
        }
        // After the address read: a lock that sees this stamp is ordered after it.
        ib.StampUse(this, m_openSeq);
        dkCmdBufDrawIndexed(m_cmd, prim, count, instances, startIndex, baseVertex, 0);
        FaultTraceDrawArgsStreams(true, prim, count, instances, startIndex, baseVertex);
        ++m_timing.instancedDraws;
        m_timing.instances += instances;
    }
    CancelInstances();
    // The next ordinary draw re-resolves the non-instanced program and streams.
    m_dirtyAttribs = m_dirtyInput = true;
    m_lock.HandOffIfContended();
    return FAILED(hr) ? hr : D3D_OK;
}

HRESULT Device::DrawIndexedRanges(UINT numVertices, const ::Deko9IndexRange *ranges, uint32_t count)
{
    ScopedNs timed{&m_timing.drawCpuNs};
    if (!count)
        return D3D_OK;
    if (!ranges)
        return Fail("DRAW", "DrawIndexedRanges without ranges (count %u)", count);
    SplitLongList();
    if (!m_indices)
        return m_indicesForgotten ? Fail("BUFFER_BIND", "ranged draw reads an index buffer destroyed while bound")
                                  : Fail("DRAW", "ranged draw without an index buffer");
    Buffer &ib = m_indices->buffer;
    const uint32_t indexSize = m_indices->Format() == D3DFMT_INDEX32 ? 4 : 2;
    const uint64_t indexLimit = ib.Size() / indexSize;
    uint64_t prims = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        // Every range must be a draw DrawIndexedPrimitive would accept.
        if (!ranges[i].triCount || (uint64_t)ranges[i].firstIndex + 3ull * ranges[i].triCount > indexLimit)
            return Fail("DRAW", "index range %u: %u+%u tris exceeds index buffer of %llu indices (ranged, %u ranges)",
                        i, ranges[i].firstIndex, ranges[i].triCount, (unsigned long long)indexLimit, count);
        prims += ranges[i].triCount;
    }
    DkPrimitive prim;
    uint32_t primIndices;
    const HRESULT hr = PrepareDraw(D3DPT_TRIANGLELIST, (UINT)std::min<uint64_t>(prims, 0xFFFFFFFFu), &prim,
                                   &primIndices);
    if (FAILED(hr))
        return hr;
    // Vertex bounds are checked only under r_deko9Verify (VertexRangeValid).
    for (uint32_t i = 0; m_verify && i < count; ++i)
    {
        if (!VertexRangeValid(ranges[i].baseVertex, numVertices, "DrawIndexedRanges"))
            return D3DERR_INVALIDCALL;
    }
    const DkIdxFormat indexFormat = indexSize == 4 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16;
    if (m_recorded.indexAddress != ib.Gpu() || m_recorded.indexFormat != indexFormat)
    {
        dkCmdBufBindIdxBuffer(m_cmd, indexFormat, ib.Gpu());
        m_recorded.indexAddress = ib.Gpu();
        m_recorded.indexFormat = indexFormat;
        ++m_timing.indexBinds;
    }
    // After the address read: a lock that sees this stamp is ordered after it.
    ib.StampUse(this, m_openSeq);
    for (uint32_t i = 0; i < count; ++i)
        dkCmdBufDrawIndexed(m_cmd, prim, 3 * ranges[i].triCount, 1, ranges[i].firstIndex, ranges[i].baseVertex, 0);
    // The record keeps the first range; the count says how many followed.
    FaultTraceDrawArgsStreams(true, prim, 3 * ranges[0].triCount, 1, ranges[0].firstIndex, ranges[0].baseVertex,
                              count);
    CensusNoteExtraGpuDraws(count - 1);
    ++m_timing.rangeCalls;
    m_timing.rangeDraws += count;
    m_lock.HandOffIfContended();
    return D3D_OK;
}

HRESULT Device::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primCount, const void *data, UINT stride)
{
    CensusScope census(this, Census_DrawPrimitiveUP);
    ScopedNs timed{&m_timing.drawCpuNs};
    DeviceLockGuard lock(m_lock);
    SplitLongList();
    DkPrimitive prim;
    uint32_t count;
    if (!data || !MapPrimitive(type, primCount, &prim, &count))
        return D3DERR_INVALIDCALL;
    GpuAlloc upload;
    if (!AllocUpload(count * stride, 256, &upload))
        return D3DERR_OUTOFVIDEOMEMORY;
    std::memcpy(upload.cpu, data, count * stride);
    census.AddBytes((uint64_t)count * stride);
    // D3D9: the UP call replaces stream 0 and leaves it unset afterwards.
    m_streams[0].buffer = nullptr;
    m_streams[0].forgotten = false;
    m_streams[0].offset = 0;
    m_streams[0].stride = stride;
    m_dirtyInput = true;
    const HRESULT hr = PrepareDraw(type, primCount, &prim, &count);
    if (FAILED(hr))
        return hr;
    const DkVtxBufferState state{stride, 0};
    const DkBufExtents extent{upload.gpu, upload.size};
    dkCmdBufBindVtxBufferState(m_cmd, &state, 1);
    dkCmdBufBindVtxBuffers(m_cmd, 0, &extent, 1);
    dkCmdBufDraw(m_cmd, prim, count, 1, 0, 0);
    FaultTraceDrawArgs(false, prim, count, 1, 0, 0, 0, upload.gpu, upload.size);
    m_streams[0].stride = 0;
    m_dirtyInput = true;
    m_recorded.streams = 0; // stream 0 was bound directly
    return D3D_OK;
}

HRESULT Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT primCount,
                                       const void *indices, D3DFORMAT indexFormat, const void *data, UINT stride)
{
    CensusScope census(this, Census_DrawIndexedPrimitiveUP);
    ScopedNs timed{&m_timing.drawCpuNs};
    DeviceLockGuard lock(m_lock);
    SplitLongList();
    DkPrimitive prim;
    uint32_t count;
    if (!data || !indices || !MapPrimitive(type, primCount, &prim, &count))
        return D3DERR_INVALIDCALL;
    const uint32_t indexBytes = count * (indexFormat == D3DFMT_INDEX32 ? 4 : 2);
    GpuAlloc vertexUpload, indexUpload;
    // Indices address vertices [0, minIndex + numVertices).
    const uint32_t vertexBytes = (minIndex + numVertices) * stride;
    if (!AllocUpload(vertexBytes, 256, &vertexUpload) || !AllocUpload(indexBytes, 256, &indexUpload))
        return D3DERR_OUTOFVIDEOMEMORY;
    std::memcpy(vertexUpload.cpu, data, vertexBytes);
    std::memcpy(indexUpload.cpu, indices, indexBytes);
    census.AddBytes((uint64_t)vertexBytes + indexBytes);
    m_streams[0].buffer = nullptr;
    m_streams[0].forgotten = false;
    m_indices = nullptr;
    m_indicesForgotten = false;
    m_streams[0].offset = 0;
    m_streams[0].stride = stride;
    m_dirtyInput = true;
    const HRESULT hr = PrepareDraw(type, primCount, &prim, &count);
    if (FAILED(hr))
        return hr;
    const DkVtxBufferState state{stride, 0};
    const DkBufExtents extent{vertexUpload.gpu, vertexUpload.size};
    dkCmdBufBindVtxBufferState(m_cmd, &state, 1);
    dkCmdBufBindVtxBuffers(m_cmd, 0, &extent, 1);
    dkCmdBufBindIdxBuffer(m_cmd, indexFormat == D3DFMT_INDEX32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16,
                          indexUpload.gpu);
    m_recorded.indexAddress = 0; // bound directly
    dkCmdBufDrawIndexed(m_cmd, prim, count, 1, 0, 0, 0);
    FaultTraceDrawArgs(true, prim, count, 1, 0, 0, indexUpload.gpu, vertexUpload.gpu, vertexUpload.size);
    m_streams[0].stride = 0;
    m_dirtyInput = true;
    m_recorded.streams = 0; // stream 0 was bound directly
    return D3D_OK;
}

HRESULT Device::Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    CensusScope census(this, Census_Clear);
    DeviceLockGuard lock(m_lock);
    CensusBreak();
    SplitLongList();
    if (count && !rects)
        return D3DERR_INVALIDCALL;
    const bool clearColor = (flags & D3DCLEAR_TARGET) && BindsColor(m_renderTargets[0]);
    const bool clearDepth = (flags & D3DCLEAR_ZBUFFER) && m_depthStencil;
    const bool clearStencil = (flags & D3DCLEAR_STENCIL) && m_depthStencil && m_depthStencil->Store()->format->stencil;
    if (!clearColor && !clearDepth && !clearStencil)
        return D3D_OK;
    HazardBegin();
    for (Surface *rt : m_renderTargets)
    {
        if (rt && clearColor)
            HazardAdd(rt->Store().get(), Access::Render);
    }
    if (m_depthStencil && (clearDepth || clearStencil))
        HazardAdd(m_depthStencil->Store().get(), Access::Render);
    HazardCommit();
    if (m_dirtyTargets)
    {
        ApplyRenderTargets();
        m_dirtyTargets = false;
    }
    // D3D9 clears the viewport, clipped by the scissor when enabled, and by
    // each rect when given.
    const ImageStore &target = *m_renderTargets[0]->Store();
    LONG left = (LONG)m_viewport.X, top = (LONG)m_viewport.Y;
    LONG right = left + (LONG)m_viewport.Width, bottom = top + (LONG)m_viewport.Height;
    right = std::min<LONG>(right, (LONG)target.LevelWidth(m_renderTargets[0]->Level()));
    bottom = std::min<LONG>(bottom, (LONG)target.LevelHeight(m_renderTargets[0]->Level()));
    if (m_rs[D3DRS_SCISSORTESTENABLE])
    {
        left = std::max<LONG>(left, m_scissor.left);
        top = std::max<LONG>(top, m_scissor.top);
        right = std::min<LONG>(right, m_scissor.right);
        bottom = std::min<LONG>(bottom, m_scissor.bottom);
    }
    const float rgba[4] = {((color >> 16) & 0xff) / 255.0f, ((color >> 8) & 0xff) / 255.0f, (color & 0xff) / 255.0f,
                           ((color >> 24) & 0xff) / 255.0f};
    const DWORD passes = count ? count : 1;
    for (DWORD i = 0; i < passes; ++i)
    {
        LONG l = left, t = top, r = right, b = bottom;
        if (count)
        {
            l = std::max<LONG>(l, rects[i].x1);
            t = std::max<LONG>(t, rects[i].y1);
            r = std::min<LONG>(r, rects[i].x2);
            b = std::min<LONG>(b, rects[i].y2);
        }
        if (r <= l || b <= t)
            continue;
        if (clearColor && m_undefinedTargets && !CheckUndefinedTargets(true, l, t, r, b))
        {
            m_dirtyViewport = true;
            return D3DERR_INVALIDCALL;
        }
        const DkScissor scissor{(uint32_t)l, (uint32_t)t, (uint32_t)(r - l), (uint32_t)(b - t)};
        dkCmdBufSetScissors(m_cmd, 0, &scissor, 1);
        if (clearColor)
        {
            for (uint32_t rt = 0; rt < 4 && m_renderTargets[rt]; ++rt)
                dkCmdBufClearColor(m_cmd, rt, DkColorMask_RGBA, rgba), ++m_cc.clears;
        }
        if (clearDepth || clearStencil)
            dkCmdBufClearDepthStencil(m_cmd, clearDepth, z, clearStencil ? 0xff : 0, (uint8_t)stencil), ++m_cc.clears;
        if (clearDepth && m_zcullStats)
        {
            const ImageStore &ds = *m_depthStencil->Store();
            ZcullNoteClear(l == 0 && t == 0 && (uint32_t)r >= ds.LevelWidth(m_depthStencil->Level()) &&
                           (uint32_t)b >= ds.LevelHeight(m_depthStencil->Level()));
        }
    }
    m_dirtyViewport = true;
    m_listHasWork = true;
    ++Stats().clears;
    return D3D_OK;
}

HRESULT Device::StretchRect(IDirect3DSurface9 *srcSurface, const RECT *srcRect, IDirect3DSurface9 *dstSurface,
                            const RECT *dstRect, D3DTEXTUREFILTERTYPE filter)
{
    CensusScope census(this, Census_StretchRect);
    DeviceLockGuard lock(m_lock);
    CensusBreak();
    SplitLongList();
    Surface *src = static_cast<Surface *>(srcSurface);
    Surface *dst = static_cast<Surface *>(dstSurface);
    if (!src || !dst || !src->Store()->gpu || !dst->Store()->gpu)
        return D3DERR_INVALIDCALL;
    if (src->Store()->format->depth || dst->Store()->format->depth)
        return Fail("STRETCH_RECT", "depth-stencil StretchRect unsupported");
    const ImageStore &s = *src->Store();
    const ImageStore &d = *dst->Store();
    const RECT full = {0, 0, (LONG)s.LevelWidth(src->Level()), (LONG)s.LevelHeight(src->Level())};
    const RECT fullDst = {0, 0, (LONG)d.LevelWidth(dst->Level()), (LONG)d.LevelHeight(dst->Level())};
    const RECT &sr = srcRect ? *srcRect : full;
    const RECT &dr = dstRect ? *dstRect : fullDst;
    // S4a: a blit destination is no longer "static" -- pendingRaw only
    // covers copy-engine writes (CopyBufferToImage), not 2D-engine blits, so
    // a store written this way must go back to full per-draw tracking.
    dst->Store()->attachment = true;
    HazardBegin();
    HazardAdd(src->Store().get(), Access::CopyRead);
    HazardAdd(dst->Store().get(), Access::BlitWrite);
    HazardCommit();
    DkImageView sv, dv;
    src->MakeView(&sv);
    dst->MakeView(&dv);
    const DkImageRect srcBox{(uint32_t)sr.left, (uint32_t)sr.top, 0, (uint32_t)(sr.right - sr.left),
                             (uint32_t)(sr.bottom - sr.top), 1};
    const DkImageRect dstBox{(uint32_t)dr.left, (uint32_t)dr.top, 0, (uint32_t)(dr.right - dr.left),
                             (uint32_t)(dr.bottom - dr.top), 1};
    dkCmdBufBlitImage(m_cmd, &sv, &srcBox, &dv, &dstBox,
                      filter == D3DTEXF_LINEAR ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest, 0);
    m_listHasWork = true;
    ++Stats().blits;
    // 2D-engine blit, not a CPU memcpy; counted as bytes moved (dst rect x
    // dst format) for the census, consistent with CopyBufferToImage below.
    census.AddBytes((uint64_t)(dr.right - dr.left) * (dr.bottom - dr.top) * d.format->blockBytes /
                    (d.format->blockWidth * d.format->blockWidth));
    return D3D_OK;
}

void Device::CopyBufferToImage(ImageStore *store, uint32_t face, uint32_t level, DkGpuAddr src,
                               const DkImageRect &rect)
{
    BeforeCopyWrite(store);
    // S4a: also feeds UpdateTexture/UpdateSurface. A static store's per-draw
    // sample check is skipped once bound, so if this copy-write lands while
    // it is still sitting in a sampler-cache slot, flag it: the next draw
    // that samples it (ApplyTextures cache-hit or PrepareDraw's gated loop)
    // must run the real check instead of skipping, or it would miss this
    // RAW. An attachment store already gets the real check every draw
    // regardless, so this is a no-op for it; likewise with
    // DEKO9_PERDRAW_STATICTEX off (r_deko9StaticHazard 0).
    if ((m_perDraw & DEKO9_PERDRAW_STATICTEX) && !store->attachment && StaticStoreBound(store))
        store->pendingRaw = true;
    DkImageView view;
    dkImageViewDefaults(&view, &store->image);
    view.mipLevelOffset = (uint8_t)level;
    view.mipLevelCount = 1;
    DkImageRect target = rect;
    if (store->faces > 1)
        target.z = face, target.depth = 1;
    const DkCopyBuf copy{src, 0, 0};
    dkCmdBufCopyBufferToImage(m_cmd, &copy, &view, &target, 0);
    store->uploaded[store->SubIndex(face, level)] = true;
    const uint64_t bytes = (uint64_t)rect.width * rect.height * rect.depth * store->format->blockBytes /
                           (store->format->blockWidth * store->format->blockWidth);
    totals.uploadBytes += bytes;
    if (CensusOn())
        CensusTexture(store, bytes);
    m_listHasWork = true;
    ++Stats().uploads;
}

bool Device::ReadImage(ImageStore *store, uint32_t face, uint32_t level, void *dst)
{
    const uint32_t bytes = store->LevelBytes(level);
    GpuAlloc readback;
    if (!AllocMemory(POOL_DYNAMIC, bytes, 256, &readback))
        return false;
    BeforeCopyRead(store);
    // The copy engine must see every earlier render to this image.
    Barrier(true);
    DkImageView view;
    dkImageViewDefaults(&view, &store->image);
    view.mipLevelOffset = (uint8_t)level;
    view.mipLevelCount = 1;
    const uint32_t w = store->LevelWidth(level), h = store->LevelHeight(level), d = store->LevelDepth(level);
    DkImageRect rect{0, 0, 0, w, h, d};
    if (store->faces > 1)
        rect.z = face, rect.depth = 1;
    const DkCopyBuf copy{readback.gpu, 0, 0};
    dkCmdBufCopyImageToBuffer(m_cmd, &view, &rect, &copy, 0);
    CmdBarrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
    m_listHasWork = true;
    ++Stats().readbacks;
    const uint64_t seq = m_openSeq;
    WaitSeq(seq);
    std::memcpy(dst, readback.cpu, bytes);
    FreeMemoryAfter(readback, seq);
    return true;
}

HRESULT Device::GetRenderTargetData(IDirect3DSurface9 *rt, IDirect3DSurface9 *dstSurface)
{
    CensusScope census(this, Census_GetRenderTargetData);
    DeviceLockGuard lock(m_lock);
    Surface *src = static_cast<Surface *>(rt);
    Surface *dst = static_cast<Surface *>(dstSurface);
    if (!src || !dst || !src->Store()->gpu || dst->Store()->gpu)
        return D3DERR_INVALIDCALL;
    ImageStore &s = *src->Store();
    ImageStore &d = *dst->Store();
    if (s.format != d.format || s.LevelWidth(src->Level()) != d.LevelWidth(dst->Level()) ||
        s.LevelHeight(src->Level()) != d.LevelHeight(dst->Level()))
        return Fail("GET_RENDER_TARGET_DATA", "format or size mismatch");
    uint8_t *out = d.cpu.data() + d.cpuLevelOffset[d.SubIndex(dst->Face(), dst->Level())];
    census.AddBytes(s.LevelBytes(src->Level()));
    return ReadImage(&s, src->Face(), src->Level(), out) ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

HRESULT Device::GetFrontBufferData(UINT swapChain, IDirect3DSurface9 *dstSurface)
{
    DeviceLockGuard lock(m_lock);
    Surface *dst = static_cast<Surface *>(dstSurface);
    if (swapChain || !dst || dst->Store()->gpu || dst->Store()->format->d3d != D3DFMT_A8R8G8B8)
        return D3DERR_INVALIDCALL;
    if (m_lastPresentSlot < 0)
        return Fail("FRONT_BUFFER", "nothing presented yet");
    // Front buffer = last presented swapchain image (RGBA8, display size:
    // the FSR-upscaled frame when the back buffer is smaller), returned as
    // D3DFMT_A8R8G8B8 (B,G,R,A bytes).
    ImageStore front;
    front.format = LookupFormat(D3DFMT_A8B8G8R8);
    front.width = kDisplayWidth;
    front.height = kDisplayHeight;
    front.gpu = true;
    front.image = m_swapImages[m_lastPresentSlot];
    front.uploaded.assign(1, true);
    ImageStore &d = *dst->Store();
    if (d.width != front.width || d.height != front.height)
        return D3DERR_INVALIDCALL;
    uint8_t *out = d.cpu.data();
    if (!ReadImage(&front, 0, 0, out))
        return D3DERR_DRIVERINTERNALERROR;
    for (uint32_t i = 0; i < front.width * front.height; ++i)
        std::swap(out[i * 4 + 0], out[i * 4 + 2]);
    return D3D_OK;
}

namespace
{
// Scales a level-0 dirty box down to `level` per D3D9 mip rules (halve each
// axis per level, rounding the far edge up so a sub-texel-sized region at a
// deep mip still covers at least one texel), clamped to that level's extent.
D3DBOX ScaleBoxToLevel(const D3DBOX &box, uint32_t level, uint32_t levelW, uint32_t levelH, uint32_t levelD)
{
    const uint32_t round = (1u << level) - 1;
    D3DBOX b;
    b.Left = std::min(box.Left >> level, levelW);
    b.Top = std::min(box.Top >> level, levelH);
    b.Front = std::min(box.Front >> level, levelD);
    b.Right = std::min(std::max((box.Right + round) >> level, b.Left + 1), levelW);
    b.Bottom = std::min(std::max((box.Bottom + round) >> level, b.Top + 1), levelH);
    b.Back = std::min(std::max((box.Back + round) >> level, b.Front + 1), levelD);
    return b;
}
} // namespace

HRESULT Device::UpdateTexture(IDirect3DBaseTexture9 *srcTexture, IDirect3DBaseTexture9 *dstTexture)
{
    CensusScope census(this, Census_UpdateTexture);
    DeviceLockGuard lock(m_lock);
    SplitLongList();
    ImageStore *src = StoreOf(srcTexture);
    ImageStore *dst = StoreOf(dstTexture);
    if (!src || !dst || src->gpu || !dst->gpu || src->type != dst->type || src->format != dst->format)
        return D3DERR_INVALIDCALL;
    if (!src->AnyDirty())
        return D3D_OK;
    // Source levels matching the destination's top level.
    uint32_t skip = 0;
    while (skip < src->levels && src->LevelWidth(skip) != dst->width)
        ++skip;
    if (skip == src->levels || src->levels - skip < dst->levels || src->LevelHeight(skip) != dst->height ||
        src->LevelDepth(skip) != dst->depth)
        return Fail("UPDATE_TEXTURE", "incompatible level chains");
    // Real D3D9 UpdateTexture copies only the source's accumulated dirty
    // region, not the whole subresource: the whole texture (new resource, a
    // null AddDirtyBox/AddDirtyRect, or the dirty-list overflow fallback
    // already collapsed to one box) or the coalesced per-lock/AddDirtyBox*
    // boxes, in destination-level-0 coordinates.
    const D3DBOX whole{0, 0, dst->width, dst->height, 0, dst->depth};
    const D3DBOX *boxes = src->dirtyAll ? &whole : src->dirtyBoxes.data();
    const size_t boxCount = src->dirtyAll ? 1 : src->dirtyBoxes.size();
    const uint32_t bw = dst->format->blockWidth, bb = dst->format->blockBytes;
    for (uint32_t face = 0; face < dst->faces; ++face)
    {
        for (uint32_t level = 0; level < dst->levels; ++level)
        {
            const uint32_t lw = dst->LevelWidth(level), lh = dst->LevelHeight(level), ld = dst->LevelDepth(level);
            const uint32_t srcPitch = src->RowPitch(level + skip), srcSlice = src->SlicePitch(level + skip);
            const uint8_t *srcBase = src->cpu.data() + src->cpuLevelOffset[src->SubIndex(face, level + skip)];
            for (size_t i = 0; i < boxCount; ++i)
            {
                const D3DBOX box = ScaleBoxToLevel(boxes[i], level, lw, lh, ld);
                if (box.Left >= box.Right || box.Top >= box.Bottom || box.Front >= box.Back)
                    continue;
                const uint32_t pitch = ((box.Right - box.Left + bw - 1) / bw) * bb;
                const uint32_t rows = (box.Bottom - box.Top + bw - 1) / bw;
                const uint32_t depth = box.Back - box.Front;
                const uint32_t bytes = pitch * rows * depth;
                GpuAlloc upload;
                if (!AllocUpload(bytes, 256, &upload))
                    return D3DERR_OUTOFVIDEOMEMORY;
                for (uint32_t z = 0; z < depth; ++z)
                {
                    for (uint32_t row = 0; row < rows; ++row)
                    {
                        std::memcpy(upload.cpu + (z * rows + row) * pitch,
                                    srcBase + (box.Front + z) * srcSlice + (box.Top / bw + row) * srcPitch +
                                        (box.Left / bw) * bb,
                                    pitch);
                    }
                }
                census.AddBytes(bytes);
                CopyBufferToImage(dst, face, level,
                                  upload.gpu, DkImageRect{box.Left, box.Top, box.Front, box.Right - box.Left,
                                                          box.Bottom - box.Top, box.Back - box.Front});
            }
        }
    }
    src->ClearDirty();
    return D3D_OK;
}

HRESULT Device::UpdateSurface(IDirect3DSurface9 *srcSurface, const RECT *srcRect, IDirect3DSurface9 *dstSurface,
                              const POINT *dstPoint)
{
    CensusScope census(this, Census_UpdateSurface);
    DeviceLockGuard lock(m_lock);
    SplitLongList();
    Surface *src = static_cast<Surface *>(srcSurface);
    Surface *dst = static_cast<Surface *>(dstSurface);
    if (!src || !dst || src->Store()->gpu || !dst->Store()->gpu || src->Store()->format != dst->Store()->format)
        return D3DERR_INVALIDCALL;
    if (srcRect || dstPoint)
        return Fail("UPDATE_SURFACE", "sub-rectangle UpdateSurface unsupported");
    ImageStore &s = *src->Store();
    ImageStore &d = *dst->Store();
    if (s.LevelWidth(src->Level()) != d.LevelWidth(dst->Level()) ||
        s.LevelHeight(src->Level()) != d.LevelHeight(dst->Level()))
        return D3DERR_INVALIDCALL;
    const uint32_t bytes = s.SlicePitch(src->Level());
    GpuAlloc upload;
    if (!AllocUpload(bytes, 256, &upload))
        return D3DERR_OUTOFVIDEOMEMORY;
    std::memcpy(upload.cpu, s.cpu.data() + s.cpuLevelOffset[s.SubIndex(src->Face(), src->Level())], bytes);
    census.AddBytes(bytes);
    CopyBufferToImage(&d, dst->Face(), dst->Level(), upload.gpu,
                      DkImageRect{0, 0, 0, d.LevelWidth(dst->Level()), d.LevelHeight(dst->Level()), 1});
    return D3D_OK;
}

HRESULT Device::ColorFill(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR color)
{
    return Fail("UNSUPPORTED", "ColorFill");
}

} // namespace deko9
