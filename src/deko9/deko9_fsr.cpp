// Present upscaling for the optional deko3d renderer.
//
// With r_renderResolution below the display, the engine's D3D back buffer is
// the render resolution (the whole game renders as if the screen were that
// size) and the swapchain stays at the display size. PresentFrame then runs
// one native deko3d full-screen pass, back buffer -> swapchain image, with
// the program r_fsrMode selects (SGSR v1, bilinear + RCAS, or bilinear; see
// deko9_fsr.h) instead of the 2D-engine blit. The back buffer itself is
// untouched, so GetRenderTargetData on it (the capture ring, the screenshot
// command) still returns the render-size frame; GetFrontBufferData returns
// the upscaled swapchain image.
//
// Shader sources and constant setup: deko9_fsr_shaders.cpp. Programs compile
// with UAM like the translated D3D9 shaders and are cached as DKSH next to
// them.

#include <switch.h>

#include "deko9_fsr.h"
#include "deko9_internal.h"
#include "deko9_native.h"
#include "deko9_particles_reference.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>

namespace deko9
{
namespace
{

const char kFsrCacheDir[] = "sdmc:/switch/kisakcod/deko9-cache";
// Bump with any change to the GLSL's meaning that the text hash would not
// see (the UAM pin).
constexpr int kFsrCacheVersion = 1;
// Program block (at most 64 bytes) plus the gamma ramp (GammaConstants).
constexpr uint32_t kFsrConstantBytes = 64 + sizeof(GammaConstants);
} // namespace

bool LoadDkshCached(Device *device, Deko9Stage stage, const char *glsl, ShaderVariant *variant,
                    std::string *error)
{
    const uint64_t hash = Deko9_HashBytecode(glsl, std::strlen(glsl));
    char path[128];
    std::snprintf(path, sizeof(path), "%s/fsr%d-%016llx.dksh", kFsrCacheDir, kFsrCacheVersion,
                  (unsigned long long)hash);
    std::vector<uint8_t> dksh;
    if (FILE *file = std::fopen(path, "rb"))
    {
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::rewind(file);
        if (size > 0)
        {
            dksh.resize((size_t)size);
            if (std::fread(dksh.data(), 1, dksh.size(), file) != dksh.size())
                dksh.clear();
        }
        std::fclose(file);
    }
    if (dksh.size() < 4 || std::memcmp(dksh.data(), "DKSH", 4))
    {
        dksh.clear();
        if (!Deko9_CompileDksh(stage, glsl, &dksh, error))
            return false;
        mkdir(kFsrCacheDir, 0777);
        if (FILE *file = std::fopen(path, "wb"))
        {
            const bool ok = std::fwrite(dksh.data(), 1, dksh.size(), file) == dksh.size();
            if (std::fclose(file) || !ok)
                std::remove(path);
        }
    }
    variant->shadowMask = 0;
    return device->LoadShaderCode(dksh, variant, error);
}

void FreeVariant(Device *device, ShaderVariant *variant, uint64_t seq)
{
    if (variant->code)
        device->FreeMemoryAfter(variant->code, seq);
    variant->code = {};
}

SamplerKey LinearClampKey()
{
    SamplerKey key;
    std::memset(&key, 0, sizeof(key));
    key.state[D3DSAMP_ADDRESSU] = key.state[D3DSAMP_ADDRESSV] = key.state[D3DSAMP_ADDRESSW] = D3DTADDRESS_CLAMP;
    key.state[D3DSAMP_MAGFILTER] = key.state[D3DSAMP_MINFILTER] = D3DTEXF_LINEAR;
    key.state[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
    key.state[D3DSAMP_MAXANISOTROPY] = 1;
    return key;
}

// Programs, sampler, constant and timestamp memory: the present path loads
// them at device creation, the dynamic-resolution path on its first call.
bool Device::EnsureUpscaler(std::string *error)
{
    if (m_fsr.loaded)
        return true;
    std::string detail;
    if (!LoadDkshCached(this, DEKO9_STAGE_VERTEX, kFsrVertexGlsl, &m_fsr.vs, &detail) ||
        !LoadDkshCached(this, DEKO9_STAGE_PIXEL, kSgsrGlsl, &m_fsr.sgsr, &detail) ||
        !LoadDkshCached(this, DEKO9_STAGE_PIXEL, kBilinearRcasGlsl, &m_fsr.bilinearRcas, &detail) ||
        !LoadDkshCached(this, DEKO9_STAGE_PIXEL, kBilinearGlsl, &m_fsr.bilinear, &detail))
        return *error = "upscaler program: " + detail, false;
    m_fsr.sampler = SamplerDescriptor(LinearClampKey());
    if (!AllocMemory(POOL_BUFFER, kFsrConstantBytes, DK_UNIFORM_BUF_ALIGNMENT, &m_fsr.constants) ||
        !AllocMemory(POOL_DYNAMIC, 3 * 32, 256, &m_fsr.timestamps) ||
        !AllocMemory(POOL_DYNAMIC, Upscaler::kRectSlots * 32, 256, &m_fsr.rectTimestamps))
        return *error = "upscaler constant/timestamp memory allocation failed", false;
    std::memset(m_fsr.timestamps.cpu, 0, m_fsr.timestamps.size);
    std::memset(m_fsr.rectTimestamps.cpu, 0, m_fsr.rectTimestamps.size);
    m_fsr.loaded = true;
    return true;
}

// The program for a mode; the source-rectangle build (DEKO9_SOURCE_RECT)
// compiles on first use.
const ShaderVariant *Device::UpscaleProgram(uint32_t mode, bool sourceRect, std::string *error)
{
    if (!sourceRect)
        return mode == UPSCALE_SGSR            ? &m_fsr.sgsr
               : mode == UPSCALE_BILINEAR_RCAS ? &m_fsr.bilinearRcas
                                               : &m_fsr.bilinear;
    ShaderVariant *variant = mode == UPSCALE_SGSR            ? &m_fsr.sgsrRect
                             : mode == UPSCALE_BILINEAR_RCAS ? &m_fsr.bilinearRcasRect
                                                             : &m_fsr.bilinearRect;
    if (!variant->code)
    {
        const char *glsl = mode == UPSCALE_SGSR            ? kSgsrGlsl
                           : mode == UPSCALE_BILINEAR_RCAS ? kBilinearRcasGlsl
                                                           : kBilinearGlsl;
        std::string detail;
        if (!LoadDkshCached(this, DEKO9_STAGE_PIXEL, SourceRectVariant(glsl).c_str(), variant, &detail))
        {
            *error = "upscaler source-rect program: " + detail;
            return nullptr;
        }
    }
    return variant;
}

bool Device::InitUpscaler(std::string *error)
{
    const uint32_t inWidth = m_params.BackBufferWidth, inHeight = m_params.BackBufferHeight;
    if (!EnsureUpscaler(error))
        return false;

    // The back buffer is a standalone surface (no sampling view of its own).
    const ImageStore &back = *m_backBuffer->Store();
    DkImageView view;
    m_backBuffer->MakeView(&view);
    std::copy(back.format->swizzle, back.format->swizzle + 4, view.swizzle);
    m_fsr.backDescriptor = AllocImageDescriptor(view);
    if (m_fsr.backDescriptor == UINT32_MAX)
        return *error = "upscaler back buffer descriptor allocation failed", false;
    m_fsr.active = true;
    Log("upscaling %ux%u -> %ux%u at present (mode %s)", inWidth, inHeight, kDisplayWidth, kDisplayHeight,
        UpscaleModeName(m_fsr.mode));
    return true;
}

void Device::ReleaseUpscaler()
{
    if (m_fsr.backDescriptor != UINT32_MAX)
        FreeImageDescriptorAfter(m_fsr.backDescriptor, m_openSeq);
    m_fsr.backDescriptor = UINT32_MAX;
    for (ShaderVariant *variant : {&m_fsr.vs, &m_fsr.sgsr, &m_fsr.bilinearRcas, &m_fsr.bilinear,
                                   &m_fsr.sgsrRect, &m_fsr.bilinearRcasRect, &m_fsr.bilinearRect,
                                   &m_fsr.gamma[0], &m_fsr.gamma[1], &m_fsr.gamma[2], &m_fsr.gammaCurve[0],
                                   &m_fsr.gammaCurve[1], &m_fsr.gammaCurve[2], &m_gatherProbe.vs, &m_gatherProbe.ps,
                                   &m_floatZ.vs, &m_floatZ.ps})
        FreeVariant(this, variant, m_openSeq);
    for (GpuAlloc *alloc : {&m_fsr.constants, &m_fsr.timestamps, &m_fsr.rectTimestamps, &m_gatherProbe.constants,
                            &m_floatZ.constants})
    {
        if (*alloc)
            FreeMemoryAfter(*alloc, m_openSeq);
        *alloc = {};
    }
    m_fsr.active = false;
    m_fsr.gammaOnly = false;
    m_fsr.loaded = false;
}

void Device::PushGammaRamp(DkGpuAddr buffer, uint32_t bufferBytes, uint32_t offset)
{
    if (GammaIsCurve())
    {
        dkCmdBufPushConstants(Rec(), buffer, bufferBytes, offset, sizeof(m_gamma.curve), &m_gamma.curve);
        return;
    }
    // The table in kGammaPushBytes pieces: Ryujinx aborts on a larger single push.
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&m_gamma.lut);
    for (uint32_t at = 0; at < (uint32_t)sizeof(GammaConstants); at += kGammaPushBytes)
        dkCmdBufPushConstants(Rec(), buffer, bufferBytes, offset + at,
                              std::min<uint32_t>(kGammaPushBytes, (uint32_t)sizeof(GammaConstants) - at), bytes + at);
}

// Called once per present: makes the present pass exist when a gamma ramp is
// set and the back buffer already has the display size (no upscale pass to
// fold it into), and drops it again when the ramp returns to identity.
// Returns whether the pass applies the ramp this frame.
bool Device::PrepareGammaPass()
{
    const bool upscaling = m_params.BackBufferWidth != kDisplayWidth || m_params.BackBufferHeight != kDisplayHeight;
    if (!m_gamma.on)
    {
        if (m_fsr.gammaOnly)
        {
            FreeImageDescriptorAfter(m_fsr.backDescriptor, m_openSeq);
            m_fsr.backDescriptor = UINT32_MAX;
            m_fsr.active = false;
            m_fsr.gammaOnly = false;
            Log("gamma ramp identity: present blit restored");
        }
        return false;
    }
    std::string error;
    if (!m_fsr.active && !upscaling)
    {
        if (!EnsureUpscaler(&error))
            return Fail("GAMMA_PASS", "%s", error.c_str()), false;
        const ImageStore &back = *m_backBuffer->Store();
        DkImageView view;
        m_backBuffer->MakeView(&view);
        std::copy(back.format->swizzle, back.format->swizzle + 4, view.swizzle);
        m_fsr.backDescriptor = AllocImageDescriptor(view);
        if (m_fsr.backDescriptor == UINT32_MAX)
            return Fail("GAMMA_PASS", "back buffer descriptor allocation failed"), false;
        m_fsr.active = true;
        m_fsr.gammaOnly = true;
        Log("gamma ramp set: present applies it at %ux%u", m_params.BackBufferWidth, m_params.BackBufferHeight);
    }
    if (!m_fsr.active)
        return false;
    const uint32_t mode = m_fsr.gammaOnly ? UPSCALE_BILINEAR : m_fsr.mode;
    ShaderVariant &variant = GammaProgram(mode);
    if (!variant.code)
    {
        const char *glsl = mode == UPSCALE_SGSR            ? kSgsrGlsl
                           : mode == UPSCALE_BILINEAR_RCAS ? kBilinearRcasGlsl
                                                           : kBilinearGlsl;
        const std::string text = GammaVariant(glsl, GammaIsCurve());
        std::string detail;
        if (text.empty() || !LoadDkshCached(this, DEKO9_STAGE_PIXEL, text.c_str(), &variant, &detail))
            return Fail("GAMMA_PROGRAM", "mode %s: %s", UpscaleModeName(mode), detail.c_str()), false;
    }
    return true;
}

void Device::SetUpscaleSharpness(float stops)
{
    m_fsr.sharpness = std::min(std::max(stops, 0.0f), 4.0f);
}

void Device::SetUpscaleMode(uint32_t mode)
{
    if (mode >= UPSCALE_MODE_COUNT)
    {
        Fail("UPSCALE_MODE", "mode=%u", mode);
        return;
    }
    if (mode != m_fsr.mode && m_fsr.active)
        Log("upscale mode %s -> %s", UpscaleModeName(m_fsr.mode), UpscaleModeName(mode));
    m_fsr.mode = mode;
}

// Fixed full-screen state for a native pass into the width x height
// rectangle at (x, y) of the bound target.
void BindFullScreenState(DkCmdBuf cmd, uint32_t width, uint32_t height, uint32_t x, uint32_t y)
{
    const DkViewport viewport{(float)x, (float)y, (float)width, (float)height, 0.0f, 1.0f};
    const DkScissor scissor{x, y, width, height};
    DkRasterizerState raster;
    dkRasterizerStateDefaults(&raster);
    raster.cullMode = DkFace_None;
    DkColorState color;
    dkColorStateDefaults(&color);
    DkColorWriteState colorWrite;
    dkColorWriteStateDefaults(&colorWrite);
    DkDepthStencilState depth;
    dkDepthStencilStateDefaults(&depth);
    depth.depthTestEnable = false;
    depth.depthWriteEnable = false;
    depth.stencilTestEnable = false;
    dkCmdBufSetViewports(cmd, 0, &viewport, 1);
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);
    dkCmdBufBindRasterizerState(cmd, &raster);
    dkCmdBufBindColorState(cmd, &color);
    dkCmdBufBindColorWriteState(cmd, &colorWrite);
    dkCmdBufBindDepthStencilState(cmd, &depth);
    dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
}

void Device::RecordUpscale(int slot)
{
    ImageStore *back = m_backBuffer->Store().get();
    // A back buffer at display size only gets a pass for the gamma ramp, and
    // that is a plain 1:1 copy.
    const uint32_t mode = m_fsr.gammaOnly ? (uint32_t)UPSCALE_BILINEAR : m_fsr.mode;
    const bool gamma = m_gamma.on && GammaProgram(mode).code;

    // GPU time of the pass. PresentFrame keeps at most two frames in flight,
    // so the report slot written three frames ago is complete. A slot counts
    // only toward the mode it was recorded for (r_fsrMode can change per frame).
    const uint32_t tsSlot = (uint32_t)(m_fsr.frames % 3), tsOffset = tsSlot * 32;
    if (m_fsr.frames >= 3 && m_fsr.slotMode[tsSlot] == mode)
    {
        uint64_t start, end;
        std::memcpy(&start, m_fsr.timestamps.cpu + tsOffset + 8, 8);
        std::memcpy(&end, m_fsr.timestamps.cpu + tsOffset + 16 + 8, 8);
        if (end > start)
        {
            m_fsr.gpuNs += dkTimestampToNs(end - start);
            ++m_fsr.samples;
        }
    }
    m_fsr.slotMode[tsSlot] = mode;
    ++m_fsr.frames;
    if (!(m_fsr.frames % 60))
    {
        Log("fsr frames=60 gpu=%.3fms (per frame) %ux%u -> %ux%u sharpness=%.2f mode=%s samples=%llu",
            m_fsr.samples ? m_fsr.gpuNs / (m_fsr.samples * 1e6) : 0.0, m_params.BackBufferWidth,
            m_params.BackBufferHeight, kDisplayWidth, kDisplayHeight, m_fsr.sharpness, UpscaleModeName(mode),
            (unsigned long long)m_fsr.samples);
        m_fsr.gpuNs = m_fsr.samples = 0;
    }
    dkCmdBufReportCounter(Rec(), DkCounter_Timestamp, m_fsr.timestamps.gpu + tsOffset);

    // Back buffer -> swapchain image (acquire already ordered it after the
    // compositor's read).
    HazardBegin();
    HazardAdd(back, Access::Sample);
    HazardCommit();
    FlushDescriptors();
    DkImageView swapView;
    dkImageViewDefaults(&swapView, &m_swapImages[slot]);
    const DkImageView *swapTarget = &swapView;
    CmdBindTargets(&swapTarget, 1, nullptr);
    BindFullScreenState(Rec(), kDisplayWidth, kDisplayHeight);
    const ShaderVariant &program = gamma                           ? GammaProgram(mode)
                                   : mode == UPSCALE_SGSR            ? m_fsr.sgsr
                                   : mode == UPSCALE_BILINEAR_RCAS ? m_fsr.bilinearRcas
                                                                   : m_fsr.bilinear;
    const DkShader *shaders[2] = {&m_fsr.vs.shader, &program.shader};
    dkCmdBufBindShaders(Rec(), DkStageFlag_GraphicsMask, shaders, 2);
    const DkResHandle backHandle = dkMakeTextureHandle(m_fsr.backDescriptor, m_fsr.sampler);
    dkCmdBufBindTextures(Rec(), DkStage_Fragment, 0, &backHandle, 1);
    if (mode == UPSCALE_SGSR)
    {
        SgsrConstants sgsr;
        SgsrSetup(&sgsr, (int)m_params.BackBufferWidth, (int)m_params.BackBufferHeight);
        dkCmdBufPushConstants(Rec(), m_fsr.constants.gpu, 256, 0, sizeof(sgsr), &sgsr);
    }
    else if (mode == UPSCALE_BILINEAR_RCAS)
    {
        BilinearRcasConstants rcas;
        BilinearRcasSetup(&rcas, m_fsr.sharpness, (int)kDisplayWidth, (int)kDisplayHeight);
        dkCmdBufPushConstants(Rec(), m_fsr.constants.gpu, 256, 0, sizeof(rcas), &rcas);
    }
    if (gamma)
        PushGammaRamp(m_fsr.constants.gpu, kFsrConstantBytes, GammaLutOffset(mode));
    const DkBufExtents ubo{m_fsr.constants.gpu, gamma ? kFsrConstantBytes : 256};
    dkCmdBufBindUniformBuffers(Rec(), DkStage_Fragment, 1, &ubo, 1);
    FaultTraceNative(kNativeUpscale);
    dkCmdBufDraw(Rec(), DkPrimitive_Triangles, 3, 1, 0, 0);

    dkCmdBufReportCounter(Rec(), DkCounter_Timestamp, m_fsr.timestamps.gpu + tsOffset + 16);
    m_listHasWork = true;
    // Everything the engine's draws record was replaced: re-record it all.
    m_dirtyTargets = m_dirtyViewport = m_dirtyRaster = true;
    m_dirtyInput = m_dirtyShaders = m_dirtyTextures = m_dirtyAttribs = true;
    m_boundVs = m_boundPs = nullptr;
    m_recorded = {};
}

// Dynamic resolution: the engine's scene (a rectangle of a render-target
// texture that ResizeStore re-lays out per size) -> the back buffer, before
// the 2D pass draws on top at the output size. Same programs as the present
// path; a source rectangle smaller than its texture uses the
// DEKO9_SOURCE_RECT build (UV transform + clamps, deko9_fsr.h), the whole
// texture the plain one. Equal sizes: a 2D-engine copy.
bool Device::UpscaleRect(ImageStore *src, const int32_t srcRect[4], Surface *dstSurface, const int32_t dstRect[4],
                         std::string *error)
{
    ImageStore *dst = dstSurface ? dstSurface->Store().get() : nullptr;
    if (!src || !dst || !src->gpu || src->type != D3DRTYPE_TEXTURE || src->format->depth ||
        src->descriptor == UINT32_MAX || !dst->gpu || dst->format->depth || !(dst->usage & D3DUSAGE_RENDERTARGET) ||
        dstSurface->Level())
        return *error = "upscale: needs a color texture source and a color render target", false;
    const int32_t texW = (int32_t)src->width, texH = (int32_t)src->height;
    const int32_t dstW = (int32_t)dst->width, dstH = (int32_t)dst->height;
    const int32_t sr[4] = {srcRect ? srcRect[0] : 0, srcRect ? srcRect[1] : 0, srcRect ? srcRect[2] : texW,
                           srcRect ? srcRect[3] : texH};
    const int32_t dr[4] = {dstRect ? dstRect[0] : 0, dstRect ? dstRect[1] : 0, dstRect ? dstRect[2] : dstW,
                           dstRect ? dstRect[3] : dstH};
    if (sr[0] < 0 || sr[1] < 0 || sr[2] <= 0 || sr[3] <= 0 || sr[0] + sr[2] > texW || sr[1] + sr[3] > texH ||
        dr[0] < 0 || dr[1] < 0 || dr[2] <= 0 || dr[3] <= 0 || dr[0] + dr[2] > dstW || dr[1] + dr[3] > dstH)
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "upscale: rect %d,%d %dx%d of %dx%d -> %d,%d %dx%d of %dx%d", sr[0],
                      sr[1], sr[2], sr[3], texW, texH, dr[0], dr[1], dr[2], dr[3], dstW, dstH);
        return *error = detail, false;
    }
    if (!EnsureUpscaler(error))
        return false;
    const uint32_t mode = m_fsr.mode;
    const bool copy = sr[2] == dr[2] && sr[3] == dr[3];
    const bool whole = sr[0] == 0 && sr[1] == 0 && sr[2] == texW && sr[3] == texH;
    const ShaderVariant *program = copy ? nullptr : UpscaleProgram(mode, !whole, error);
    if (!copy && !program)
        return false;

    // GPU time: read this ring slot's previous pair once its list is done.
    const uint32_t slot = (uint32_t)(m_fsr.rectCalls % Upscaler::kRectSlots), tsOffset = slot * 32;
    if (m_fsr.rectSlotSeq[slot] && m_fsr.rectSlotSeq[slot] <= CompletedSeq())
    {
        uint64_t start, end;
        std::memcpy(&start, m_fsr.rectTimestamps.cpu + tsOffset + 8, 8);
        std::memcpy(&end, m_fsr.rectTimestamps.cpu + tsOffset + 16 + 8, 8);
        if (end > start && !m_fsr.rectSlotCopy[slot])
        {
            m_fsr.rectGpuNs += dkTimestampToNs(end - start);
            ++m_fsr.rectSamples;
        }
    }
    m_fsr.rectSlotSeq[slot] = m_openSeq;
    m_fsr.rectSlotMode[slot] = mode;
    m_fsr.rectSlotCopy[slot] = copy;
    ++m_fsr.rectCalls;
    m_fsr.rectCopies += copy;
    m_fsr.rectInW = (uint32_t)sr[2];
    m_fsr.rectInH = (uint32_t)sr[3];
    m_fsr.rectOutW = (uint32_t)dr[2];
    m_fsr.rectOutH = (uint32_t)dr[3];
    if (!(m_fsr.rectCalls % 60))
    {
        Log("fsr frames=60 gpu=%.3fms (per frame) %ux%u -> %ux%u sharpness=%.2f mode=%s samples=%llu path=scene "
            "copies=%llu",
            m_fsr.rectSamples ? m_fsr.rectGpuNs / (m_fsr.rectSamples * 1e6) : 0.0, m_fsr.rectInW, m_fsr.rectInH,
            m_fsr.rectOutW, m_fsr.rectOutH, m_fsr.sharpness, UpscaleModeName(mode),
            (unsigned long long)m_fsr.rectSamples, (unsigned long long)m_fsr.rectCopies);
        m_fsr.rectGpuNs = m_fsr.rectSamples = m_fsr.rectCopies = 0;
    }
    dkCmdBufReportCounter(Rec(), DkCounter_Timestamp, m_fsr.rectTimestamps.gpu + tsOffset);

    if (copy)
    {
        HazardBegin();
        HazardAdd(src, Access::CopyRead);
        HazardAdd(dst, Access::BlitWrite);
        HazardCommit();
        DkImageView sv, dv;
        dkImageViewDefaults(&sv, &src->image);
        dstSurface->MakeView(&dv);
        const DkImageRect srcBox{(uint32_t)sr[0], (uint32_t)sr[1], 0, (uint32_t)sr[2], (uint32_t)sr[3], 1};
        const DkImageRect dstBox{(uint32_t)dr[0], (uint32_t)dr[1], 0, (uint32_t)dr[2], (uint32_t)dr[3], 1};
        dkCmdBufBlitImage(Rec(), &sv, &srcBox, &dv, &dstBox, DkBlitFlag_FilterNearest, 0);
        ++Stats().blits;
    }
    else
    {
        HazardBegin();
        HazardAdd(src, Access::Sample);
        HazardAdd(dst, Access::Render);
        HazardCommit();
        FlushDescriptors();
        DkImageView targetView;
        dstSurface->MakeView(&targetView);
        const DkImageView *targets = &targetView;
        CmdBindTargets(&targets, 1, nullptr);
        BindFullScreenState(Rec(), (uint32_t)dr[2], (uint32_t)dr[3], (uint32_t)dr[0], (uint32_t)dr[1]);
        const DkShader *shaders[2] = {&m_fsr.vs.shader, &program->shader};
        dkCmdBufBindShaders(Rec(), DkStageFlag_GraphicsMask, shaders, 2);
        const DkResHandle handle = dkMakeTextureHandle(src->descriptor, m_fsr.sampler);
        dkCmdBufBindTextures(Rec(), DkStage_Fragment, 0, &handle, 1);
        UpscaleSource source;
        UpscaleSourceSetup(&source, texW, texH, sr[0], sr[1], sr[2], sr[3]);
        if (mode == UPSCALE_SGSR)
        {
            SgsrConstants c;
            SgsrSetup(&c, texW, texH, source);
            dkCmdBufPushConstants(Rec(), m_fsr.constants.gpu, 256, 0, sizeof(c), &c);
        }
        else if (mode == UPSCALE_BILINEAR_RCAS)
        {
            BilinearRcasConstants c;
            BilinearRcasSetup(&c, m_fsr.sharpness, dr[2], dr[3], source);
            dkCmdBufPushConstants(Rec(), m_fsr.constants.gpu, 256, 0, sizeof(c), &c);
        }
        else
        {
            BilinearConstants c;
            BilinearSetup(&c, source);
            dkCmdBufPushConstants(Rec(), m_fsr.constants.gpu, 256, 0, sizeof(c), &c);
        }
        const DkBufExtents ubo{m_fsr.constants.gpu, 256};
        dkCmdBufBindUniformBuffers(Rec(), DkStage_Fragment, 1, &ubo, 1);
        FaultTraceNative(kNativeUpscaleRect);
        dkCmdBufDraw(Rec(), DkPrimitive_Triangles, 3, 1, 0, 0);
    }
    dkCmdBufReportCounter(Rec(), DkCounter_Timestamp, m_fsr.rectTimestamps.gpu + tsOffset + 16);
    m_listHasWork = true;
    m_dirtyTargets = m_dirtyViewport = m_dirtyRaster = true;
    m_dirtyInput = m_dirtyShaders = m_dirtyTextures = m_dirtyAttribs = true;
    m_boundVs = m_boundPs = nullptr;
    m_recorded = {};
    return true;
}

bool Device::GatherProbe(ImageStore *source, ImageStore *target, int component, std::string *error)
{
    if (!source || !target || !source->gpu || source->descriptor == UINT32_MAX || !target->gpu ||
        !(target->usage & D3DUSAGE_RENDERTARGET) || component < 0 || component > 3)
        return *error = "gather probe: needs a sampled GPU texture, a render target and component 0..3", false;
    if (!m_gatherProbe.ps.code)
    {
        std::string detail;
        if (!LoadDkshCached(this, DEKO9_STAGE_VERTEX, kFsrVertexGlsl, &m_gatherProbe.vs, &detail) ||
            !LoadDkshCached(this, DEKO9_STAGE_PIXEL, kGatherProbeGlsl, &m_gatherProbe.ps, &detail))
            return *error = "gather probe program: " + detail, false;
        if (!AllocMemory(POOL_BUFFER, 256, DK_UNIFORM_BUF_ALIGNMENT, &m_gatherProbe.constants))
            return *error = "gather probe constant allocation failed", false;
    }
    HazardBegin();
    HazardAdd(source, Access::Sample);
    HazardAdd(target, Access::Render);
    HazardCommit();
    FlushDescriptors();
    DkImageView targetView;
    dkImageViewDefaults(&targetView, &target->image);
    const DkImageView *targets = &targetView;
    CmdBindTargets(&targets, 1, nullptr);
    BindFullScreenState(Rec(), target->width, target->height);
    const DkShader *shaders[2] = {&m_gatherProbe.vs.shader, &m_gatherProbe.ps.shader};
    dkCmdBufBindShaders(Rec(), DkStageFlag_GraphicsMask, shaders, 2);
    const DkResHandle handle = dkMakeTextureHandle(source->descriptor, SamplerDescriptor(LinearClampKey()));
    dkCmdBufBindTextures(Rec(), DkStage_Fragment, 0, &handle, 1);
    GatherProbeConstants constants{};
    constants.component[0] = component;
    dkCmdBufPushConstants(Rec(), m_gatherProbe.constants.gpu, 256, 0, sizeof(constants), &constants);
    const DkBufExtents ubo{m_gatherProbe.constants.gpu, 256};
    dkCmdBufBindUniformBuffers(Rec(), DkStage_Fragment, 1, &ubo, 1);
    FaultTraceNative(kNativeGather);
    dkCmdBufDraw(Rec(), DkPrimitive_Triangles, 3, 1, 0, 0);
    m_listHasWork = true;
    m_dirtyTargets = m_dirtyViewport = m_dirtyRaster = true;
    m_dirtyInput = m_dirtyShaders = m_dirtyTextures = m_dirtyAttribs = true;
    m_boundVs = m_boundPs = nullptr;
    m_recorded = {};
    return true;
}

// Native float-Z: one full-screen pass depth -> signed view depth into the
// engine's float-Z target (deko9_fsr.h kFloatZGlsl), replacing the engine's
// second geometry pass over the opaque scene.
//
// Hazards: the depth image was last written by the scene's draws (depth
// test/write, Access::Render), so sampling it records the full barrier plus
// texture-cache invalidate like any render -> sample transition; the pass
// binds only the float target (no depth attachment), so the depth image is
// never attached and sampled at once. The next engine draw rebinds the
// scene target and depth (m_dirtyTargets) and, depth being a Render access
// after this Sample, gets the sample -> render barrier.
//
// Compression (r_deko9RtCompression): the depth image is a compressible
// kind (Z24S8_2CZ); Maxwell's compression is resolved by the memory
// controller for every client, which is how the sun/spot shadow maps
// (compressed depth textures) are sampled already. The deko3d selftest's
// FLOATZ_* checks run on a compressed depth surface.
bool Device::BuildFloatZ(Surface *depthSurface, Surface *targetSurface, const FloatZConstants &constants,
                         std::string *error)
{
    ImageStore *depth = depthSurface ? depthSurface->Store().get() : nullptr;
    ImageStore *target = targetSurface ? targetSurface->Store().get() : nullptr;
    if (!depth || !target || !depth->gpu || !depth->format->depth || !target->gpu ||
        !(target->usage & D3DUSAGE_RENDERTARGET) || target->format->depth)
        return *error = "float-z: needs a GPU depth-stencil surface and a color render target", false;
    const uint32_t width = target->LevelWidth(targetSurface->Level()),
                   height = target->LevelHeight(targetSurface->Level());
    if (depthSurface->Level() || width > depth->width || height > depth->height)
        return *error = "float-z: target larger than the depth surface", false;
    if (!m_floatZ.ps.code)
    {
        std::string detail;
        if (!LoadDkshCached(this, DEKO9_STAGE_VERTEX, kFsrVertexGlsl, &m_floatZ.vs, &detail) ||
            !LoadDkshCached(this, DEKO9_STAGE_PIXEL, kFloatZGlsl, &m_floatZ.ps, &detail))
            return *error = "float-z program: " + detail, false;
        if (!AllocMemory(POOL_BUFFER, 256, DK_UNIFORM_BUF_ALIGNMENT, &m_floatZ.constants))
            return *error = "float-z constant allocation failed", false;
    }
    if (!EnsureDepthDescriptor(depth))
        return *error = "float-z depth descriptor allocation failed", false;
    HazardBegin();
    HazardAdd(depth, Access::Sample);
    HazardAdd(target, Access::Render);
    HazardCommit();
    FlushDescriptors();
    DkImageView targetView;
    targetSurface->MakeView(&targetView);
    const DkImageView *targets = &targetView;
    CmdBindTargets(&targets, 1, nullptr);
    BindFullScreenState(Rec(), width, height);
    const DkShader *shaders[2] = {&m_floatZ.vs.shader, &m_floatZ.ps.shader};
    dkCmdBufBindShaders(Rec(), DkStageFlag_GraphicsMask, shaders, 2);
    SamplerKey key = LinearClampKey();
    key.state[D3DSAMP_MAGFILTER] = key.state[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    const DkResHandle handle = dkMakeTextureHandle(depth->descriptor, SamplerDescriptor(key));
    dkCmdBufBindTextures(Rec(), DkStage_Fragment, 0, &handle, 1);
    dkCmdBufPushConstants(Rec(), m_floatZ.constants.gpu, 256, 0, sizeof(constants), &constants);
    const DkBufExtents ubo{m_floatZ.constants.gpu, 256};
    dkCmdBufBindUniformBuffers(Rec(), DkStage_Fragment, 1, &ubo, 1);
    FaultTraceNative(kNativeFloatZ);
    dkCmdBufDraw(Rec(), DkPrimitive_Triangles, 3, 1, 0, 0);
    m_listHasWork = true;
    m_dirtyTargets = m_dirtyViewport = m_dirtyRaster = true;
    m_dirtyInput = m_dirtyShaders = m_dirtyTextures = m_dirtyAttribs = true;
    m_boundVs = m_boundPs = nullptr;
    m_recorded = {};
    return true;
}

// ---- off-screen particles (r_halfResParticles) ----------------------------
//
// Both passes are single full-screen triangles recorded like BuildFloatZ:
// hazards through the tracker (scene depth / float-Z sampled after being
// rendered, the off-screen images rendered after being sampled by the last
// composite), constants pushed through the command buffer into a UBO that
// lives as long as the device (dkCmdBufPushConstants is ordered with the
// draws: no CPU write to memory the GPU may still read), and every piece of
// D3D-derived state marked dirty afterwards so the next engine draw
// re-applies its own targets, viewport and pipeline.

bool Device::LoadHrpPrograms(std::string *error)
{
    if (m_hrp.compositePs.code)
        return true;
    std::string detail;
    if (!LoadDkshCached(this, DEKO9_STAGE_VERTEX, kFsrVertexGlsl, &m_hrp.vs, &detail) ||
        !LoadDkshCached(this, DEKO9_STAGE_PIXEL, kHrpDepthGlsl, &m_hrp.depthPs, &detail) ||
        !LoadDkshCached(this, DEKO9_STAGE_PIXEL, kHrpCompositeGlsl, &m_hrp.compositePs, &detail))
        return *error = "particle programs: " + detail, false;
    if (!AllocMemory(POOL_BUFFER, 256, DK_UNIFORM_BUF_ALIGNMENT, &m_hrp.depthConstants) ||
        !AllocMemory(POOL_BUFFER, 256, DK_UNIFORM_BUF_ALIGNMENT, &m_hrp.compositeConstants))
        return *error = "particle constant allocation failed", false;
    return true;
}

bool Device::EnsureDepthDescriptor(ImageStore *depth)
{
    if (depth->descriptor != UINT32_MAX)
        return true;
    // A standalone depth-stencil surface has no sampling view (D3D9 never
    // samples one); this one lives with the store (DestroyStoreAfter).
    DkImageView view;
    dkImageViewDefaults(&view, &depth->image);
    std::copy(depth->format->swizzle, depth->format->swizzle + 4, view.swizzle);
    view.dsSource = DkDsSource_Depth;
    depth->descriptor = AllocImageDescriptor(view);
    return depth->descriptor != UINT32_MAX;
}

void Device::EndNativePass()
{
    m_listHasWork = true;
    m_dirtyTargets = m_dirtyViewport = m_dirtyRaster = true;
    m_dirtyInput = m_dirtyShaders = m_dirtyTextures = m_dirtyAttribs = true;
    m_boundVs = m_boundPs = nullptr;
    m_recorded = {};
}

bool Device::ParticleDepth(Surface *depthSurface, ImageStore *floatZ, const int32_t rect[4], uint32_t factor,
                           Surface *dstDepthSurface, Surface *dstFloatZSurface, std::string *error)
{
    ImageStore *depth = depthSurface ? depthSurface->Store().get() : nullptr;
    ImageStore *dstDepth = dstDepthSurface ? dstDepthSurface->Store().get() : nullptr;
    ImageStore *dstZ = dstFloatZSurface ? dstFloatZSurface->Store().get() : nullptr;
    if (!depth || !depth->gpu || !depth->format->depth || !floatZ || !floatZ->gpu || floatZ->descriptor == UINT32_MAX ||
        !dstDepth || !dstDepth->gpu || !dstDepth->format->depth || !dstZ || !dstZ->gpu ||
        !(dstZ->usage & D3DUSAGE_RENDERTARGET) || dstZ->format->depth)
        return *error = "particle depth: needs scene depth + float-Z textures, an off-screen depth and float target",
               false;
    if (factor < 1 || factor > 2 || rect[0] < 0 || rect[1] < 0 || rect[2] < 1 || rect[3] < 1 ||
        (uint32_t)(rect[0] + rect[2]) > std::min(depth->width, floatZ->width) ||
        (uint32_t)(rect[1] + rect[3]) > std::min(depth->height, floatZ->height))
        return *error = "particle depth: rectangle outside the scene depth / float-Z or factor not 1..2", false;
    const uint32_t w = ((uint32_t)rect[2] + factor - 1) / factor, h = ((uint32_t)rect[3] + factor - 1) / factor;
    if (w > dstDepth->width || h > dstDepth->height || w > dstZ->width || h > dstZ->height)
        return *error = "particle depth: off-screen targets smaller than the rectangle / factor", false;
    if ((m_hrp.rules & DEKO9_HRP_RULE_UNCOMPRESSED) && (dstDepth->compressed || dstZ->compressed))
        return *error = "particle depth: off-screen depth / float-Z are compressed images (hardware rule "
                        "DEKO9_HRP_RULE_UNCOMPRESSED)",
               false;
    if (!LoadHrpPrograms(error))
        return false;
    if (!EnsureDepthDescriptor(depth))
        return *error = "particle depth: depth descriptor allocation failed", false;
    HazardBegin();
    HazardAdd(depth, Access::Sample);
    HazardAdd(floatZ, Access::Sample);
    HazardAdd(dstDepth, Access::Render);
    HazardAdd(dstZ, Access::Render);
    HazardCommit();
    FlushDescriptors();
    DkImageView colorView, depthView;
    dstFloatZSurface->MakeView(&colorView);
    dstDepthSurface->MakeView(&depthView);
    const DkImageView *targets = &colorView;
    CmdBindTargets(&targets, 1, &depthView);
    // No clear here: the caller clears the off-screen depth through the
    // device's ordinary Clear first: a clear
    // recorded inside this pass made later zfeather draws of the frame lose
    // float-Z intermittently on some emulators). Every covered texel is written.
    const DkScissor scissor{0, 0, w, h};
    dkCmdBufSetScissors(Rec(), 0, &scissor, 1);
    const DkViewport viewport{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    dkCmdBufSetViewports(Rec(), 0, &viewport, 1);
    DkRasterizerState raster;
    dkRasterizerStateDefaults(&raster);
    raster.cullMode = DkFace_None;
    DkColorState color;
    dkColorStateDefaults(&color);
    DkColorWriteState colorWrite;
    dkColorWriteStateDefaults(&colorWrite);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = true;
    ds.depthWriteEnable = true;
    ds.depthCompareOp = DkCompareOp_Always;
    ds.stencilTestEnable = false;
    dkCmdBufBindRasterizerState(Rec(), &raster);
    dkCmdBufBindColorState(Rec(), &color);
    dkCmdBufBindColorWriteState(Rec(), &colorWrite);
    dkCmdBufBindDepthStencilState(Rec(), &ds);
    dkCmdBufBindVtxAttribState(Rec(), nullptr, 0);
    dkCmdBufBindVtxBufferState(Rec(), nullptr, 0);
    const DkShader *shaders[2] = {&m_hrp.vs.shader, &m_hrp.depthPs.shader};
    dkCmdBufBindShaders(Rec(), DkStageFlag_GraphicsMask, shaders, 2);
    SamplerKey key = LinearClampKey();
    key.state[D3DSAMP_MAGFILTER] = key.state[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    const uint32_t sampler = SamplerDescriptor(key);
    const DkResHandle handles[2] = {dkMakeTextureHandle(depth->descriptor, sampler),
                                    dkMakeTextureHandle(floatZ->descriptor, sampler)};
    dkCmdBufBindTextures(Rec(), DkStage_Fragment, 0, handles, 2);
    const int32_t constants[8] = {rect[0], rect[1], rect[0] + rect[2] - 1, rect[1] + rect[3] - 1, (int32_t)factor - 1,
                                  0, 0, 0};
    dkCmdBufPushConstants(Rec(), m_hrp.depthConstants.gpu, 256, 0, sizeof(constants), constants);
    const DkBufExtents ubo{m_hrp.depthConstants.gpu, 256};
    dkCmdBufBindUniformBuffers(Rec(), DkStage_Fragment, 1, &ubo, 1);
    FaultTraceNative(kNativeHrpDepth);
    dkCmdBufDraw(Rec(), DkPrimitive_Triangles, 3, 1, 0, 0);
    if (m_hrp.rules & DEKO9_HRP_RULE_ZCULL)
    {
        // Zcull cannot follow depth a fragment shader writes: its region for
        // this target still holds the bounds of the clear (or whatever the
        // hardware derived from the rasterised z of the full-screen
        // triangle). Invalidate it, so the off-screen particles' depth tests
        // against this target never cull on stale bounds (some emulators do not
        // emulate zcull: only hardware can show this).
        CmdBarrier(DkBarrier_None, DkInvalidateFlags_Zcull);
        ++m_hrp.zcullInvalidates;
    }
    ++m_hrp.depthPasses;
    EndNativePass();
    return true;
}

bool Device::ParticleComposite(ImageStore *color, ImageStore *halfZ, ImageStore *fullZ, Surface *dstSurface,
                               const int32_t dstRect[4], const hrpref::CompositeConstants &constants,
                               std::string *error)
{
    ImageStore *dst = dstSurface ? dstSurface->Store().get() : nullptr;
    const auto sampled = [](const ImageStore *s) { return s && s->gpu && s->descriptor != UINT32_MAX; };
    if (!sampled(color) || !sampled(halfZ) || !sampled(fullZ) || !dst || !dst->gpu ||
        !(dst->usage & D3DUSAGE_RENDERTARGET) || dst->format->depth)
        return *error = "particle composite: needs off-screen colour/float-Z, scene float-Z and a colour target", false;
    const uint32_t dw = dst->LevelWidth(dstSurface->Level()), dh = dst->LevelHeight(dstSurface->Level());
    if (dstRect[0] < 0 || dstRect[1] < 0 || dstRect[2] < 1 || dstRect[3] < 1 ||
        (uint32_t)(dstRect[0] + dstRect[2]) > std::min(dw, fullZ->width) ||
        (uint32_t)(dstRect[1] + dstRect[3]) > std::min(dh, fullZ->height) || constants.limit[0] < 0 ||
        constants.limit[1] < 0 || (uint32_t)constants.limit[0] >= std::min(color->width, halfZ->width) ||
        (uint32_t)constants.limit[1] >= std::min(color->height, halfZ->height))
        return *error = "particle composite: rectangle outside the target or off-screen images", false;
    if ((m_hrp.rules & DEKO9_HRP_RULE_UNCOMPRESSED) && (color->compressed || halfZ->compressed))
        return *error = "particle composite: off-screen colour / float-Z are compressed images (hardware rule "
                        "DEKO9_HRP_RULE_UNCOMPRESSED)",
               false;
    if (!LoadHrpPrograms(error))
        return false;
    HazardBegin();
    HazardAdd(color, Access::Sample);
    HazardAdd(halfZ, Access::Sample);
    HazardAdd(fullZ, Access::Sample);
    HazardAdd(dst, Access::Render);
    HazardCommit();
    FlushDescriptors();
    DkImageView targetView;
    dstSurface->MakeView(&targetView);
    const DkImageView *targets = &targetView;
    CmdBindTargets(&targets, 1, nullptr);
    BindFullScreenState(Rec(), (uint32_t)dstRect[2], (uint32_t)dstRect[3], (uint32_t)dstRect[0], (uint32_t)dstRect[1]);
    // dst * T + C: colour ONE / SRCALPHA; the scene's alpha is kept.
    DkColorState colorState;
    dkColorStateDefaults(&colorState);
    dkColorStateSetBlendEnable(&colorState, 0, true);
    dkCmdBufBindColorState(Rec(), &colorState);
    DkBlendState blend;
    dkBlendStateDefaults(&blend);
    dkBlendStateSetFactors(&blend, DkBlendFactor_One, DkBlendFactor_SrcAlpha, DkBlendFactor_Zero, DkBlendFactor_One);
    dkBlendStateSetOps(&blend, DkBlendOp_Add, DkBlendOp_Add);
    dkCmdBufBindBlendStates(Rec(), 0, &blend, 1);
    DkColorWriteState colorWrite;
    dkColorWriteStateDefaults(&colorWrite);
    dkColorWriteStateSetMask(&colorWrite, 0, DkColorMask_RGB);
    dkCmdBufBindColorWriteState(Rec(), &colorWrite);
    const DkShader *shaders[2] = {&m_hrp.vs.shader, &m_hrp.compositePs.shader};
    dkCmdBufBindShaders(Rec(), DkStageFlag_GraphicsMask, shaders, 2);
    SamplerKey key = LinearClampKey();
    key.state[D3DSAMP_MAGFILTER] = key.state[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    const uint32_t sampler = SamplerDescriptor(key);
    const DkResHandle handles[3] = {dkMakeTextureHandle(color->descriptor, sampler),
                                    dkMakeTextureHandle(halfZ->descriptor, sampler),
                                    dkMakeTextureHandle(fullZ->descriptor, sampler)};
    dkCmdBufBindTextures(Rec(), DkStage_Fragment, 0, handles, 3);
    dkCmdBufPushConstants(Rec(), m_hrp.compositeConstants.gpu, 256, 0, sizeof(constants), &constants);
    const DkBufExtents ubo{m_hrp.compositeConstants.gpu, 256};
    dkCmdBufBindUniformBuffers(Rec(), DkStage_Fragment, 1, &ubo, 1);
    FaultTraceNative(kNativeHrpComposite);
    dkCmdBufDraw(Rec(), DkPrimitive_Triangles, 3, 1, 0, 0);
    ++m_hrp.composites;
    EndNativePass();
    return true;
}

void Device::HrpFullBarrier()
{
    CmdBarrier(DkBarrier_Full,
                    DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors |
                        DkInvalidateFlags_Zcull | DkInvalidateFlags_L2Cache);
    ++Stats().barriers;
    // A barrier: every earlier access is complete (the tracker's clock).
    ++m_writeClock;
    m_descriptorsDirty = false;
    m_listHasWork = true;
    ++m_hrp.fullBarriers;
}

} // namespace deko9

void Deko9_SetUpscaleSharpness(IDirect3DDevice9 *device, float stops)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetUpscaleSharpness(stops);
}

void Deko9_SetUpscaleMode(IDirect3DDevice9 *device, uint32_t mode)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetUpscaleMode(mode);
}

bool Deko9_UpscaleSurface(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *src, const int32_t *srcRect,
                          IDirect3DSurface9 *dst, const int32_t *dstRect)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    std::string error;
    deko9::ImageStore *store =
        src && src->GetType() == D3DRTYPE_TEXTURE ? static_cast<deko9::Texture2D *>(src)->Store().get() : nullptr;
    if (!d->UpscaleRect(store, srcRect, static_cast<deko9::Surface *>(dst), dstRect, &error))
    {
        deko9::Fail("UPSCALE_RECT", "%s", error.c_str());
        return false;
    }
    return true;
}

bool Deko9_BuildFloatZ(IDirect3DDevice9 *device, IDirect3DSurface9 *depth, IDirect3DSurface9 *target,
                       const deko9::FloatZConstants *constants)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    std::string error;
    if (!constants || !d->BuildFloatZ(static_cast<deko9::Surface *>(depth), static_cast<deko9::Surface *>(target),
                                      *constants, &error))
    {
        deko9::Fail("FLOATZ", "%s", constants ? error.c_str() : "no constants");
        return false;
    }
    return true;
}

bool Deko9_GatherProbe(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *source, IDirect3DSurface9 *target,
                       int component)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    if (!source || !target || source->GetType() != D3DRTYPE_TEXTURE)
        return false;
    deko9::ImageStore *src = static_cast<deko9::Texture2D *>(source)->Store().get();
    deko9::ImageStore *dst = static_cast<deko9::Surface *>(target)->Store().get();
    std::string error;
    if (!d->GatherProbe(src, dst, component, &error))
    {
        deko9::Log("gather probe failed: %s", error.c_str());
        return false;
    }
    return true;
}

namespace
{
deko9::ImageStore *TextureStore(IDirect3DBaseTexture9 *texture)
{
    return texture && texture->GetType() == D3DRTYPE_TEXTURE ? static_cast<deko9::Texture2D *>(texture)->Store().get()
                                                             : nullptr;
}
} // namespace

bool Deko9_ParticleDepth(IDirect3DDevice9 *device, IDirect3DSurface9 *depth, IDirect3DBaseTexture9 *floatZ,
                         const int32_t rect[4], uint32_t factor, IDirect3DSurface9 *dstDepth,
                         IDirect3DSurface9 *dstFloatZ)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    std::string error;
    if (!rect || !d->ParticleDepth(static_cast<deko9::Surface *>(depth), TextureStore(floatZ), rect, factor,
                                   static_cast<deko9::Surface *>(dstDepth),
                                   static_cast<deko9::Surface *>(dstFloatZ), &error))
    {
        deko9::Fail("HRP_DEPTH", "%s", rect ? error.c_str() : "no rectangle");
        return false;
    }
    return true;
}

bool Deko9_ParticleComposite(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *color, IDirect3DBaseTexture9 *halfZ,
                             IDirect3DBaseTexture9 *fullZ, IDirect3DSurface9 *dst, const int32_t dstRect[4],
                             const deko9::hrpref::CompositeConstants *constants)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    std::string error;
    if (!dstRect || !constants ||
        !d->ParticleComposite(TextureStore(color), TextureStore(halfZ), TextureStore(fullZ),
                              static_cast<deko9::Surface *>(dst), dstRect, *constants, &error))
    {
        deko9::Fail("HRP_COMPOSITE", "%s", dstRect && constants ? error.c_str() : "no rectangle/constants");
        return false;
    }
    return true;
}

void Deko9_GetParticleCounts(IDirect3DDevice9 *device, uint64_t *depthPasses, uint64_t *composites)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->GetHrpCounts(depthPasses, composites);
}

void Deko9_SetParticleRules(IDirect3DDevice9 *device, uint32_t rules)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->SetHrpRules(rules & DEKO9_HRP_RULES_ALL);
}

uint64_t Deko9_GetParticleZcullInvalidates(IDirect3DDevice9 *device)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    return d->HrpZcullInvalidates();
}

void Deko9_ParticleFullBarrier(IDirect3DDevice9 *device)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    d->HrpFullBarrier();
}

uint64_t Deko9_GetParticleFullBarriers(IDirect3DDevice9 *device)
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    return d->HrpFullBarriers();
}

bool Deko9_ReadParticleConstants(IDirect3DDevice9 *device, uint32_t depth[8], uint32_t composite[12])
{
    deko9::Device *d = static_cast<deko9::Device *>(device);
    deko9::DeviceLockGuard lock(d->Lock());
    return d->ReadHrpConstants(depth, composite);
}
