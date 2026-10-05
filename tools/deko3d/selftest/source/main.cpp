// deko9 self-test: drives the optional deko3d D3D9 backend (src/deko9)
// through the D3D9 API only, with hand-assembled shaders, and checks every
// result by GPU readback. No game code or data. Emits one PASS/FAIL line per
// check and PASS:DEKO9_SELFTEST only when all pass; runs on an emulator and on
// hardware (nxlink).

// libnx first: the vendored windows_base.h defines `interface`.
#include <switch.h>

#include <d3d9.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

#include "../../../../src/deko9/deko9_fsr.h"
#include "../../../../src/deko9/deko9_fsr_reference.h"
#include "../../../../src/deko9/deko9_native.h"
#include "../../../../src/deko9/deko9_particles_reference.h"
#include "../../../../src/deko9/deko9_shader.h"
#include "../../../../src/gfx_d3d/r_dynres_controller.h"

#ifndef KISAK_SWITCH_LOG_HOST
#define KISAK_SWITCH_LOG_HOST ""
#endif

namespace
{

int g_failures;
IDirect3DDevice9 *g_device;
// Appended to check names by the FSR variants (compression / wait-idle).
const char *g_checkSuffix = "";

void Emit(const char *line)
{
    std::printf("%s\n", line);
    std::fflush(stdout);
    svcOutputDebugString(line, std::strlen(line));
}

void Check(bool ok, const char *name, const char *detail = "")
{
    char line[256];
    std::snprintf(line, sizeof(line), "%s:DEKO9_SELFTEST_%s%s %s", ok ? "PASS" : "FAIL", name, g_checkSuffix,
                  detail);
    Emit(line);
    if (!ok)
        ++g_failures;
}

// vs_2_0: dcl_position v0; dcl_texcoord v1; mov oPos, v0; mov oT0, v1
const DWORD kVs[] = {
    0xFFFE0200,
    0x0200001F, 0x80000000, 0x900F0000,
    0x0200001F, 0x80000005, 0x900F0001,
    0x02000001, 0xC00F0000, 0x90E40000,
    0x02000001, 0xE00F0000, 0x90E40001,
    0x0000FFFF,
};
// ps_2_0: mov oC0, c0
const DWORD kPsColor[] = {
    0xFFFF0200,
    0x02000001, 0x800F0800, 0xA0E40000,
    0x0000FFFF,
};
// ps_2_0: add r0, c0, c60; mov oC0, r0 (two sparse constant registers)
const DWORD kPsSparse[] = {
    0xFFFF0200,
    0x03000002, 0x800F0000, 0xA0E40000, 0xA0E4003C,
    0x02000001, 0x800F0800, 0x80E40000,
    0x0000FFFF,
};
// ps_2_0: dcl t0; dcl_2d s0; texld r0, t0, s0; mov oC0, r0
const DWORD kPsTexture[] = {
    0xFFFF0200,
    0x0200001F, 0x80000000, 0xB00F0000,
    0x0200001F, 0x90000000, 0xA00F0800,
    0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800,
    0x02000001, 0x800F0800, 0x80E40000,
    0x0000FFFF,
};

// vs_2_0: dcl_position v0; dcl_texcoord v1; add oPos, v0, c4; mov oT0, c5
// (c4, c5 vary per instance in the instancing checks)
const DWORD kVsInstanced[] = {
    0xFFFE0200,
    0x0200001F, 0x80000000, 0x900F0000,
    0x0200001F, 0x80000005, 0x900F0001,
    0x03000002, 0xC00F0000, 0x90E40000, 0xA0E40004,
    0x02000001, 0xE00F0000, 0xA0E40005,
    0x0000FFFF,
};
// ps_2_0: dcl t0; mov oC0, t0
const DWORD kPsVarying[] = {
    0xFFFF0200,
    0x0200001F, 0x80000000, 0xB00F0000,
    0x02000001, 0x800F0800, 0xB0E40000,
    0x0000FFFF,
};

// ps_2_0: dcl t0; texkill t0; mov oC0, c0 (discards where t0.xyz < 0)
const DWORD kPsKill[] = {
    0xFFFF0200,
    0x0200001F, 0x80000000, 0xB00F0000,
    0x01000041, 0xB00F0000,
    0x02000001, 0x800F0800, 0xA0E40000,
    0x0000FFFF,
};
// ps_2_0: kPsKill plus mov oDepth, c1.x (writes depth: never early-Z)
const DWORD kPsKillDepth[] = {
    0xFFFF0200,
    0x0200001F, 0x80000000, 0xB00F0000,
    0x01000041, 0xB00F0000,
    0x02000001, 0x800F0800, 0xA0E40000,
    0x02000001, 0x900F0800, 0xA0000001,
    0x0000FFFF,
};

struct Vertex
{
    float x, y, z, w;
    float u, v;
};

constexpr UINT kSize = 64;
IDirect3DVertexShader9 *g_vs, *g_vsInstanced;
IDirect3DPixelShader9 *g_psColor, *g_psTexture, *g_psSparse, *g_psVarying;
IDirect3DVertexDeclaration9 *g_decl;
IDirect3DTexture9 *g_target;   // 64x64 A8R8G8B8 render target
IDirect3DSurface9 *g_targetSurface;
IDirect3DSurface9 *g_readback; // SYSTEMMEM copy

void SetColor(float r, float g, float b, float a)
{
    const float c[4] = {r, g, b, a};
    g_device->SetPixelShaderConstantF(0, c, 1);
}

void DrawQuad(float x0, float y0, float x1, float y1, float z = 0.5f)
{
    const Vertex v[6] = {
        {x0, y0, z, 1, 0, 0}, {x1, y0, z, 1, 1, 0}, {x0, y1, z, 1, 0, 1},
        {x1, y0, z, 1, 1, 0}, {x1, y1, z, 1, 1, 1}, {x0, y1, z, 1, 0, 1},
    };
    g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(Vertex));
}

// Reads the render target; returns the A8R8G8B8 pixel (x, y) as 0xAARRGGBB.
bool Read(uint32_t *pixels)
{
    if (FAILED(g_device->GetRenderTargetData(g_targetSurface, g_readback)))
        return false;
    D3DLOCKED_RECT locked;
    if (FAILED(g_readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        return false;
    for (UINT y = 0; y < kSize; ++y)
        std::memcpy(pixels + y * kSize, static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch, kSize * 4);
    g_readback->UnlockRect();
    return true;
}

bool Near(uint32_t a, uint32_t b)
{
    for (int shift = 0; shift < 32; shift += 8)
    {
        const int da = (int)((a >> shift) & 0xff) - (int)((b >> shift) & 0xff);
        if (da < -3 || da > 3)
            return false;
    }
    return true;
}

void ExpectPixels(const char *name, const uint32_t *pixels, std::initializer_list<std::pair<int, uint32_t>> probes)
{
    bool ok = true;
    char detail[192] = "";
    for (const auto &probe : probes)
    {
        const int x = probe.first & 0xff, y = probe.first >> 8;
        const uint32_t got = pixels[y * kSize + x];
        if (!Near(got, probe.second))
        {
            if (ok)
                std::snprintf(detail, sizeof(detail), "(%d,%d)=%08x want %08x", x, y, (unsigned)got,
                              (unsigned)probe.second);
            ok = false;
        }
    }
    Check(ok, name, detail);
}

constexpr int P(int x, int y) { return x | (y << 8); }

void BeginPass()
{
    g_device->SetRenderTarget(0, g_targetSurface);
    g_device->SetDepthStencilSurface(nullptr);
    g_device->SetVertexShader(g_vs);
    g_device->SetVertexDeclaration(g_decl);
    g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
}

void RunTests()
{
    uint32_t px[kSize * kSize];
    const uint32_t red = 0xffff0000, green = 0xff00ff00, blue = 0xff0000ff, white = 0xffffffff,
                   black = 0xff000000;

    // Clear.
    BeginPass();
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, red, 1.0f, 0);
    Check(Read(px), "READBACK");
    ExpectPixels("CLEAR", px, {{P(0, 0), red}, {P(63, 63), red}, {P(31, 17), red}});

    // Clear honours the scissor rect.
    const RECT quarter = {0, 0, 32, 32};
    g_device->SetScissorRect(&quarter);
    g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, green, 1.0f, 0);
    g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    Read(px);
    ExpectPixels("CLEAR_SCISSOR", px, {{P(5, 5), green}, {P(31, 31), green}, {P(32, 5), red}, {P(5, 40), red}});

    // D3D orientation: NDC +Y is the top row; the quad covers the top half.
    BeginPass();
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
    g_device->SetPixelShader(g_psColor);
    SetColor(0, 0, 1, 1);
    DrawQuad(-1, 1, 1, 0);
    Read(px);
    ExpectPixels("ORIENTATION", px, {{P(5, 5), blue}, {P(60, 30), blue}, {P(5, 40), black}, {P(60, 60), black}});

    // D3D pixel-center rule: the NDC y [0, 1] quad covers exactly rows 0..31.
    ExpectPixels("PIXEL_CENTERS", px, {{P(0, 0), blue}, {P(63, 0), blue}, {P(0, 31), blue}, {P(0, 32), black}});

    // Culling: D3DCULL_CCW keeps D3D clockwise (screen space) triangles.
    BeginPass();
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
    g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    g_device->SetPixelShader(g_psColor);
    SetColor(1, 1, 1, 1);
    const Vertex cw[3] = {{-1, 1, 0.5f, 1, 0, 0}, {1, 1, 0.5f, 1, 0, 0}, {-1, -1, 0.5f, 1, 0, 0}};
    const Vertex ccw[3] = {{1, -1, 0.5f, 1, 0, 0}, {1, 1, 0.5f, 1, 0, 0}, {-1, -1, 0.5f, 1, 0, 0}};
    g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, cw, sizeof(Vertex));
    g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, ccw, sizeof(Vertex));
    Read(px);
    ExpectPixels("CULL_CCW", px, {{P(4, 4), white}, {P(60, 60), black}});

    // Texture upload + BGRA channel order + UV orientation (point sampling).
    IDirect3DTexture9 *texture = nullptr;
    g_device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr);
    D3DLOCKED_RECT locked;
    if (texture && SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0)))
    {
        uint32_t *row0 = static_cast<uint32_t *>(locked.pBits);
        uint32_t *row1 = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + locked.Pitch);
        row0[0] = red;
        row0[1] = green;
        row1[0] = blue;
        row1[1] = white;
        texture->UnlockRect(0);
    }
    BeginPass();
    g_device->SetPixelShader(g_psTexture);
    g_device->SetTexture(0, texture);
    g_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    DrawQuad(-1, 1, 1, -1);
    Read(px);
    ExpectPixels("TEXTURE_BGRA", px, {{P(8, 8), red}, {P(56, 8), green}, {P(8, 56), blue}, {P(56, 56), white}});

    // Native fast path (deko9_native.h): the same draw bound through
    // Deko9_SetTexture + Deko9_SetSamplerPacked inside a batch. Packed
    // sampler state as the engine encodes it: min/mag filter in bits 8-15,
    // address modes in 20-25 (1 = WRAP, 3 = CLAMP).
    const uint32_t kPackedPointClamp = 1u << 8 | 1u << 12 | 3u << 20 | 3u << 22 | 3u << 24;
    const uint32_t kPackedLinearClamp = 2u << 8 | 2u << 12 | 3u << 20 | 3u << 22 | 3u << 24;
    BeginPass();
    g_device->SetTexture(0, nullptr);
    g_device->SetPixelShader(g_psTexture);
    {
        Deko9Batch batch(g_device);
        Deko9_SetTexture(g_device, 0, texture);
        uint32_t tracked = Deko9_SetSamplerPacked(g_device, 0, kPackedPointClamp, 0);
        DrawQuad(-1, 1, 1, -1);
        Read(px);
        ExpectPixels("NATIVE_TEXTURE", px,
                     {{P(8, 8), red}, {P(56, 8), green}, {P(8, 56), blue}, {P(56, 56), white}});
        // Linear filtering must reach the sampler descriptor: the quad
        // centre blends all four texels.
        tracked = Deko9_SetSamplerPacked(g_device, 0, kPackedLinearClamp, tracked);
        DrawQuad(-1, 1, 1, -1);
        Read(px);
        {
            // ~50% of each texel: every channel mid-range (point sampling
            // would give one of the pure texel colours).
            const uint32_t c = px[32 * kSize + 32];
            bool mid = true;
            for (int shift = 0; shift < 24; shift += 8)
                mid &= ((c >> shift) & 0xff) >= 96 && ((c >> shift) & 0xff) <= 160;
            char detail[64];
            std::snprintf(detail, sizeof(detail), "(32,32)=%08x", (unsigned)c);
            Check(mid, "NATIVE_SAMPLER_LINEAR", detail);
        }
        // And back to point: the per-slot memo must not keep the linear one.
        tracked = Deko9_SetSamplerPacked(g_device, 0, kPackedPointClamp, tracked);
        DrawQuad(-1, 1, 1, -1);
        Read(px);
        ExpectPixels("NATIVE_SAMPLER_POINT", px, {{P(24, 24), red}, {P(40, 40), white}});
        DWORD minFilter = 0;
        g_device->GetSamplerState(0, D3DSAMP_MINFILTER, &minFilter);
        Check(minFilter == D3DTEXF_POINT && tracked == kPackedPointClamp, "NATIVE_SAMPLER_STATE");
    }
    // A texture destroyed while bound leaves its slot (non-owning bind);
    // rebinding a live one works.
    {
        IDirect3DTexture9 *transient = nullptr;
        g_device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &transient, nullptr);
        Deko9_SetTexture(g_device, 0, transient);
        if (transient)
            transient->Release();
        IDirect3DBaseTexture9 *bound = reinterpret_cast<IDirect3DBaseTexture9 *>(1);
        g_device->GetTexture(0, &bound);
        Check(transient && bound == nullptr, "NATIVE_TEXTURE_FORGET");
        Deko9_SetTexture(g_device, 0, texture);
        DrawQuad(-1, 1, 1, -1);
        Read(px);
        ExpectPixels("NATIVE_TEXTURE_REBIND", px, {{P(8, 8), red}, {P(56, 56), white}});
    }

    // L8 through a SYSTEMMEM texture + UpdateTexture (the cinematic path).
    IDirect3DTexture9 *lumSys = nullptr, *lum = nullptr;
    g_device->CreateTexture(2, 2, 1, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM, &lumSys, nullptr);
    g_device->CreateTexture(2, 2, 1, 0, D3DFMT_L8, D3DPOOL_DEFAULT, &lum, nullptr);
    if (lumSys && SUCCEEDED(lumSys->LockRect(0, &locked, nullptr, 0)))
    {
        uint8_t *bits = static_cast<uint8_t *>(locked.pBits);
        bits[0] = 0x40;
        bits[1] = 0x80;
        bits[locked.Pitch] = 0xc0;
        bits[locked.Pitch + 1] = 0xff;
        lumSys->UnlockRect(0);
    }
    Check(lum && SUCCEEDED(g_device->UpdateTexture(lumSys, lum)), "UPDATE_TEXTURE");
    g_device->SetTexture(0, lum);
    DrawQuad(-1, 1, 1, -1);
    Read(px);
    ExpectPixels("TEXTURE_L8", px, {{P(8, 8), 0xff404040}, {P(56, 8), 0xff808080}, {P(8, 56), 0xffc0c0c0},
                                    {P(56, 56), 0xffffffff}});

    // Render to texture, then sample it (render -> sample barrier).
    IDirect3DTexture9 *rtt = nullptr;
    IDirect3DSurface9 *rttSurface = nullptr;
    g_device->CreateTexture(kSize, kSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtt, nullptr);
    if (rtt)
        rtt->GetSurfaceLevel(0, &rttSurface);
    g_device->SetRenderTarget(0, rttSurface);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, green, 1.0f, 0);
    g_device->SetPixelShader(g_psColor);
    SetColor(0, 0, 1, 1);
    DrawQuad(-1, -0.5f, 1, -1); // bottom quarter blue
    BeginPass();
    g_device->SetPixelShader(g_psTexture);
    g_device->SetTexture(0, rtt);
    DrawQuad(-1, 1, 1, -1);
    Read(px);
    ExpectPixels("RENDER_TO_TEXTURE", px, {{P(10, 10), green}, {P(10, 60), blue}});

    // StretchRect (2D engine) from the render texture.
    g_device->SetTexture(0, nullptr);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
    const RECT src = {0, 48, 64, 64}, dst = {0, 0, 64, 16};
    Check(SUCCEEDED(g_device->StretchRect(rttSurface, &src, g_targetSurface, &dst, D3DTEXF_POINT)), "STRETCH_CALL");
    Read(px);
    ExpectPixels("STRETCH_RECT", px, {{P(10, 5), blue}, {P(10, 30), black}});

    // Depth test: nearer red quad wins over a farther blue one drawn later.
    IDirect3DSurface9 *depth = nullptr;
    g_device->CreateDepthStencilSurface(kSize, kSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr);
    BeginPass();
    g_device->SetDepthStencilSurface(depth);
    g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, black, 1.0f, 0);
    g_device->SetPixelShader(g_psColor);
    SetColor(1, 0, 0, 1);
    DrawQuad(-1, 1, 1, -1, 0.3f);
    SetColor(0, 0, 1, 1);
    DrawQuad(-1, 1, 1, -1, 0.6f);
    Read(px);
    ExpectPixels("DEPTH_TEST", px, {{P(32, 32), red}});

    // Gated draw path (PrepareDraw skips re-deriving an unchanged binding):
    // hazards on bound images must still be seen per draw. Sample rtt, then
    // clear it red through the render path while the texture binding and
    // shaders stay untouched, then sample it again.
    BeginPass();
    g_device->SetPixelShader(g_psTexture);
    g_device->SetTexture(0, rtt);
    g_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    DrawQuad(-1, 1, 1, -1);
    g_device->SetRenderTarget(0, rttSurface);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, red, 1.0f, 0);
    g_device->SetRenderTarget(0, g_targetSurface);
    DrawQuad(-1, 1, 1, -1);
    Read(px);
    ExpectPixels("GATED_RENDER_TO_TEXTURE", px, {{P(10, 10), red}, {P(10, 60), red}});

    // Same for an upload into a bound texture.
    g_device->SetTexture(0, texture);
    DrawQuad(-1, 1, 1, -1);
    if (texture && SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0)))
    {
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch)[x] = blue;
        texture->UnlockRect(0);
    }
    DrawQuad(-1, 1, 1, -1);
    Read(px);
    ExpectPixels("GATED_TEXTURE_UPLOAD", px, {{P(8, 8), blue}, {P(56, 56), blue}});

    // A DISCARD lock renames a bound dynamic vertex buffer; the next draw
    // (no SetStreamSource) must read the new memory. Then two index
    // buffers alternate over one vertex buffer (index bind cache).
    {
        IDirect3DVertexBuffer9 *vb = nullptr;
        IDirect3DIndexBuffer9 *ibLeft = nullptr, *ibRight = nullptr;
        g_device->CreateVertexBuffer(12 * sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
                                     &vb, nullptr);
        g_device->CreateIndexBuffer(6 * sizeof(uint16_t), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ibLeft, nullptr);
        g_device->CreateIndexBuffer(6 * sizeof(uint16_t), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ibRight, nullptr);
        auto quad = [](Vertex *v, float x0, float x1) {
            const Vertex q[6] = {{x0, 1, 0.5f, 1, 0, 0}, {x1, 1, 0.5f, 1, 1, 0}, {x0, -1, 0.5f, 1, 0, 1},
                                 {x1, 1, 0.5f, 1, 1, 0}, {x1, -1, 0.5f, 1, 1, 1}, {x0, -1, 0.5f, 1, 0, 1}};
            std::memcpy(v, q, sizeof(q));
        };
        void *data = nullptr;
        bool ok = vb && ibLeft && ibRight;
        if (ok && SUCCEEDED(vb->Lock(0, 0, &data, D3DLOCK_DISCARD)))
        {
            quad(static_cast<Vertex *>(data), -1, 0);
            vb->Unlock();
        }
        BeginPass();
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
        g_device->SetPixelShader(g_psColor);
        SetColor(1, 1, 1, 1);
        g_device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        g_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
        if (ok && SUCCEEDED(vb->Lock(0, 0, &data, D3DLOCK_DISCARD)))
        {
            quad(static_cast<Vertex *>(data), 0, 1);
            vb->Unlock();
        }
        g_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
        Read(px);
        ExpectPixels("GATED_VB_RENAME", px, {{P(10, 32), white}, {P(54, 32), white}});

        if (ok && SUCCEEDED(vb->Lock(0, 0, &data, D3DLOCK_DISCARD)))
        {
            quad(static_cast<Vertex *>(data), -1, 0);
            quad(static_cast<Vertex *>(data) + 6, 0, 1);
            vb->Unlock();
        }
        const uint16_t left[6] = {0, 1, 2, 3, 4, 5}, right[6] = {6, 7, 8, 9, 10, 11};
        for (auto pair : {std::make_pair(ibLeft, left), std::make_pair(ibRight, right)})
        {
            if (ok && SUCCEEDED(pair.first->Lock(0, 0, &data, 0)))
            {
                std::memcpy(data, pair.second, sizeof(left));
                pair.first->Unlock();
            }
        }
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
        g_device->SetIndices(ibLeft);
        SetColor(1, 0, 0, 1);
        g_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 12, 0, 2);
        g_device->SetIndices(ibRight);
        SetColor(0, 0, 1, 1);
        g_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 12, 0, 2);
        Read(px);
        ExpectPixels("GATED_INDEX_BUFFERS", px, {{P(10, 32), red}, {P(54, 32), blue}});
        g_device->SetStreamSource(0, nullptr, 0, 0);
        g_device->SetIndices(nullptr);
        for (IUnknown *object : {(IUnknown *)vb, (IUnknown *)ibLeft, (IUnknown *)ibRight})
        {
            if (object)
                object->Release();
        }
    }

    // Non-owning vertex/index buffer slots: a buffer released while bound
    // leaves its slot (GetStreamSource/GetIndices report none), and a draw
    // reading the slot fails (expected FAIL:DEKO9_BUFFER_BIND line) instead
    // of fetching freed memory; rebinding a live buffer draws again.
    {
        IDirect3DVertexBuffer9 *transientVb = nullptr, *liveVb = nullptr;
        IDirect3DIndexBuffer9 *transientIb = nullptr;
        g_device->CreateVertexBuffer(6 * sizeof(Vertex), 0, 0, D3DPOOL_MANAGED, &transientVb, nullptr);
        g_device->CreateVertexBuffer(6 * sizeof(Vertex), 0, 0, D3DPOOL_MANAGED, &liveVb, nullptr);
        g_device->CreateIndexBuffer(6 * sizeof(uint16_t), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &transientIb, nullptr);
        void *data = nullptr;
        const Vertex full[6] = {{-1, 1, 0.5f, 1, 0, 0}, {1, 1, 0.5f, 1, 1, 0}, {-1, -1, 0.5f, 1, 0, 1},
                                {1, 1, 0.5f, 1, 1, 0}, {1, -1, 0.5f, 1, 1, 1}, {-1, -1, 0.5f, 1, 0, 1}};
        if (liveVb && SUCCEEDED(liveVb->Lock(0, 0, &data, 0)))
        {
            std::memcpy(data, full, sizeof(full));
            liveVb->Unlock();
        }
        BeginPass();
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
        g_device->SetPixelShader(g_psColor);
        SetColor(0, 1, 0, 1);
        g_device->SetStreamSource(0, transientVb, 0, sizeof(Vertex));
        g_device->SetIndices(transientIb);
        if (transientVb)
            transientVb->Release();
        if (transientIb)
            transientIb->Release();
        IDirect3DVertexBuffer9 *boundVb = reinterpret_cast<IDirect3DVertexBuffer9 *>(1);
        IDirect3DIndexBuffer9 *boundIb = reinterpret_cast<IDirect3DIndexBuffer9 *>(1);
        UINT offset = 0, stride = 0;
        g_device->GetStreamSource(0, &boundVb, &offset, &stride);
        g_device->GetIndices(&boundIb);
        Check(transientVb && transientIb && !boundVb && !boundIb, "BUFFER_FORGET");
        const HRESULT stale = g_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
        const HRESULT staleIndexed = g_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 6, 0, 2);
        Check(FAILED(stale) && FAILED(staleIndexed), "BUFFER_FORGOTTEN_DRAW_FAILS");
        g_device->SetStreamSource(0, liveVb, 0, sizeof(Vertex));
        const HRESULT live = g_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
        Read(px);
        Check(SUCCEEDED(live), "BUFFER_REBIND_DRAW");
        ExpectPixels("BUFFER_REBIND_PIXELS", px, {{P(32, 32), green}});
        g_device->SetStreamSource(0, nullptr, 0, 0);
        if (liveVb)
            liveVb->Release();
    }

    // Constants: per-register dirty tracking pushes c0 and c60 as separate
    // runs; an identical rewrite pushes nothing; changing only c60 must
    // still reach the GPU.
    {
        BeginPass();
        g_device->SetPixelShader(g_psSparse);
        const float c0[4] = {0.5f, 0, 0, 1}, magentaPart[4] = {0.5f, 0, 1, 0}, yellowPart[4] = {0.5f, 1, 0, 0};
        g_device->SetPixelShaderConstantF(0, c0, 1);
        g_device->SetPixelShaderConstantF(60, magentaPart, 1);
        DrawQuad(-1, 1, 0, -1);
        g_device->SetPixelShaderConstantF(0, c0, 1); // identical
        g_device->SetPixelShaderConstantF(60, yellowPart, 1);
        DrawQuad(0, 1, 1, -1);
        Read(px);
        ExpectPixels("CONST_SPARSE_REGISTERS", px, {{P(10, 32), 0xffff00ff}, {P(54, 32), 0xffffff00}});
        float back[4] = {};
        g_device->GetPixelShaderConstantF(60, back, 1);
        Check(!std::memcmp(back, yellowPart, sizeof(back)), "CONST_READBACK");
    }

    // Render-target compression (DkImageFlags_HwCompression): the same
    // content rendered into a compressed and an uncompressed 256x256 target
    // (colour + D24S8 depth) must read back byte-identical through the copy
    // engine, sample correctly and blit correctly. Depth survives a list
    // boundary, a non-0/1 depth clear (Zcull clear path) and later tests.
    // Emulators ignore compressible page kinds, so only hardware proves it.
    {
        constexpr UINT kBig = 256;
        IDirect3DTexture9 *big[2] = {};
        IDirect3DSurface9 *bigSurface[2] = {}, *bigDepth[2] = {}, *bigRead[2] = {};
        bool ok = true;
        for (int i = 0; i < 2; ++i)
        {
            Deko9_SetRtCompression(i == 0);
            ok = ok &&
                 SUCCEEDED(g_device->CreateTexture(kBig, kBig, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                   D3DPOOL_DEFAULT, &big[i], nullptr)) &&
                 SUCCEEDED(big[i]->GetSurfaceLevel(0, &bigSurface[i])) &&
                 SUCCEEDED(g_device->CreateDepthStencilSurface(kBig, kBig, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0,
                                                               FALSE, &bigDepth[i], nullptr)) &&
                 SUCCEEDED(g_device->CreateOffscreenPlainSurface(kBig, kBig, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                                 &bigRead[i], nullptr));
        }
        Deko9_SetRtCompression(true);
        Check(ok && Deko9_IsCompressed(bigSurface[0]) && Deko9_IsCompressed(bigDepth[0]) &&
                  !Deko9_IsCompressed(bigSurface[1]) && !Deko9_IsCompressed(bigDepth[1]),
              "COMPRESSED_CREATE");
        static uint32_t pixels[2][kBig * kBig];
        for (int i = 0; ok && i < 2; ++i)
        {
            BeginPass();
            g_device->SetRenderTarget(0, bigSurface[i]);
            g_device->SetDepthStencilSurface(bigDepth[i]);
            g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
            g_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
            g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff203040, 1.0f,
                            0);
            g_device->SetPixelShader(g_psColor);
            SetColor(1, 0, 0, 1);
            DrawQuad(-1, 1, 0, -1, 0.3f); // left half red, near
            // Force a list boundary: depth written above must persist.
            g_device->GetRenderTargetData(bigSurface[i], bigRead[i]);
            SetColor(0, 0, 1, 1);
            DrawQuad(-1, 1, 1, -1, 0.6f); // blue behind the red half only
            SetColor(0, 1, 0, 1);
            DrawQuad(0.5f, 1, 1, 0.5f, 0.2f); // green top-right corner, nearest
            // Partial-tile blended strip (compression tiles are 8x8+ pixels).
            g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            g_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            g_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
            SetColor(1, 1, 1, 0.5f);
            DrawQuad(-0.37f, 0.11f, 0.29f, -0.07f);
            g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            // Depth cleared to a value other than 0/1: nearer passes, farther fails.
            g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
            const D3DRECT bottom = {0, 224, 256, 256};
            g_device->Clear(1, &bottom, D3DCLEAR_ZBUFFER, 0, 0.5f, 0);
            SetColor(1, 1, 0, 1);
            DrawQuad(-1, -0.75f, 0, -1, 0.4f); // yellow, passes
            SetColor(1, 0, 1, 1);
            DrawQuad(0, -0.75f, 1, -1, 0.7f); // magenta, fails
            D3DLOCKED_RECT locked;
            ok = SUCCEEDED(g_device->GetRenderTargetData(bigSurface[i], bigRead[i])) &&
                 SUCCEEDED(bigRead[i]->LockRect(&locked, nullptr, D3DLOCK_READONLY));
            if (ok)
            {
                for (UINT y = 0; y < kBig; ++y)
                    std::memcpy(pixels[i] + y * kBig, static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch,
                                kBig * 4);
                bigRead[i]->UnlockRect();
            }
        }
        g_device->SetDepthStencilSurface(nullptr);
        g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
        Check(ok, "COMPRESSED_READBACK");
        uint32_t mismatches = 0, first = 0;
        for (UINT i = 0; ok && i < kBig * kBig; ++i)
        {
            if (pixels[0][i] != pixels[1][i] && !mismatches++)
                first = i;
        }
        char detail[160];
        std::snprintf(detail, sizeof(detail), "mismatches=%u first=(%u,%u) %08x vs %08x", (unsigned)mismatches,
                      (unsigned)(first % kBig), (unsigned)(first / kBig), (unsigned)pixels[0][first],
                      (unsigned)pixels[1][first]);
        Check(ok && !mismatches, "COMPRESSED_MATCH", detail);
        auto at = [&](int x, int y) { return pixels[0][y * kBig + x]; };
        bool content = Near(at(10, 10), red) && Near(at(200, 100), blue) && Near(at(250, 5), green) &&
                       Near(at(100, 150), red) && Near(at(120, 125), 0xbfff8080) &&
                       Near(at(10, 250), 0xffffff00) && Near(at(200, 250), blue);
        std::snprintf(detail, sizeof(detail), "red=%08x blue=%08x green=%08x strip=%08x yellow=%08x zfail=%08x",
                      (unsigned)at(10, 10), (unsigned)at(200, 100), (unsigned)at(250, 5), (unsigned)at(120, 125),
                      (unsigned)at(10, 250), (unsigned)at(200, 250));
        Check(ok && content, "COMPRESSED_CONTENT", detail);

        // Sample the compressed target (texture unit) into the 64x64 target.
        BeginPass();
        g_device->SetPixelShader(g_psTexture);
        g_device->SetTexture(0, big[0]);
        g_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        DrawQuad(-1, 1, 1, -1);
        g_device->SetTexture(0, nullptr);
        Read(px);
        ExpectPixels("COMPRESSED_SAMPLED", px, {{P(2, 2), red}, {P(50, 25), blue}, {P(62, 1), green},
                                                {P(2, 62), 0xffffff00}});
        // 2D engine reads the compressed target (StretchRect, like Present).
        const RECT srcBig = {0, 0, 256, 256};
        Check(SUCCEEDED(g_device->StretchRect(bigSurface[0], &srcBig, g_targetSurface, nullptr, D3DTEXF_POINT)),
              "COMPRESSED_STRETCH_CALL");
        Read(px);
        ExpectPixels("COMPRESSED_STRETCH", px, {{P(2, 2), red}, {P(50, 25), blue}, {P(62, 1), green},
                                                {P(2, 62), 0xffffff00}});
        for (int i = 0; i < 2; ++i)
        {
            for (IUnknown *object : {(IUnknown *)bigSurface[i], (IUnknown *)big[i], (IUnknown *)bigDepth[i],
                                     (IUnknown *)bigRead[i]})
            {
                if (object)
                    object->Release();
            }
        }
    }

    // Alpha blending: 50% white over black.
    BeginPass();
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_device->SetPixelShader(g_psColor);
    SetColor(1, 1, 1, 0.5f);
    DrawQuad(-1, 1, 1, -1);
    Read(px);
    ExpectPixels("ALPHA_BLEND", px, {{P(32, 32), 0xbf808080}});

    // ---- baked per-pass state (deko9_baked.h) ----------------------------
    // Everything below runs with r_deko9Verify semantics on: every draw is
    // also derived the slow way and compared word for word with the baked
    // units it replayed.
    Deko9_SetVerify(g_device, true);
    Deko9Counters before{}, after{};
    Deko9_GetCounters(g_device, &before);
    {
        // Alternate two render-state sets; the third and fourth draws reuse
        // the units the first two baked (hits, no new units).
        BeginPass();
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
        g_device->SetPixelShader(g_psColor);
        auto blendOn = [] {
            g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            g_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            g_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        };
        blendOn();
        SetColor(1, 1, 1, 0.5f);
        DrawQuad(-1, 1, -0.5f, -1);
        g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        SetColor(1, 0, 0, 1);
        DrawQuad(-0.5f, 1, 0, -1);
        Deko9Counters mid{};
        Deko9_GetCounters(g_device, &mid);
        blendOn();
        SetColor(1, 1, 1, 0.5f);
        DrawQuad(0, 1, 0.5f, -1);
        g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        SetColor(1, 0, 0, 1);
        DrawQuad(0.5f, 1, 1, -1);
        Deko9_GetCounters(g_device, &after);
        Read(px);
        ExpectPixels("BAKED_STATE_SWITCH", px,
                     {{P(8, 32), 0xbf808080}, {P(24, 32), red}, {P(40, 32), 0xbf808080}, {P(56, 32), red}});
        char detail[96];
        std::snprintf(detail, sizeof(detail), "hits=%llu newUnits=%d",
                      (unsigned long long)(after.bakedHits - mid.bakedHits),
                      (int)(after.rasterUnits - mid.rasterUnits));
        Check(after.bakedHits - mid.bakedHits >= 2 && after.rasterUnits == mid.rasterUnits, "BAKED_STATE_REUSE",
              detail);
    }
    {
        // Stencil reference is keyed: mark the left half with ref 1, then
        // draw everywhere with EQUAL ref 1 (left only) and EQUAL ref 2
        // (nowhere). A stale unit would draw with the old reference.
        IDirect3DSurface9 *stencil = nullptr;
        g_device->CreateDepthStencilSurface(kSize, kSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &stencil,
                                            nullptr);
        BeginPass();
        g_device->SetDepthStencilSurface(stencil);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_STENCIL | D3DCLEAR_ZBUFFER, black, 1.0f, 0);
        g_device->SetPixelShader(g_psColor);
        g_device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        g_device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
        g_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
        g_device->SetRenderState(D3DRS_STENCILREF, 1);
        g_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        DrawQuad(-1, 1, 0, -1);
        g_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
        g_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
        g_device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
        SetColor(0, 1, 0, 1);
        DrawQuad(-1, 1, 1, -1);
        g_device->SetRenderState(D3DRS_STENCILREF, 2);
        SetColor(0, 0, 1, 1);
        DrawQuad(-1, 1, 1, -1);
        Read(px);
        ExpectPixels("BAKED_STENCIL_REF", px, {{P(10, 32), green}, {P(54, 32), black}});
        g_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        g_device->SetDepthStencilSurface(nullptr);
        if (stencil)
            stencil->Release();
    }
    {
        // Invalidation: releasing a pixel shader drops the program units
        // baked for it; a new shader then bakes its own and draws right.
        IDirect3DPixelShader9 *transient = nullptr;
        g_device->CreatePixelShader(kPsColor, &transient);
        BeginPass();
        g_device->SetPixelShader(transient);
        SetColor(0, 0, 1, 1);
        DrawQuad(-1, 1, 1, -1);
        Deko9Counters withShader{};
        Deko9_GetCounters(g_device, &withShader);
        g_device->SetPixelShader(g_psColor);
        if (transient)
            transient->Release();
        Deko9Counters released{};
        Deko9_GetCounters(g_device, &released);
        char detail[96];
        std::snprintf(detail, sizeof(detail), "programUnits %u -> %u", withShader.programUnits, released.programUnits);
        Check(transient && released.programUnits < withShader.programUnits, "BAKED_SHADER_RELEASE", detail);
        // A released declaration's units go too.
        IDirect3DVertexDeclaration9 *decl = nullptr;
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END(),
        };
        g_device->CreateVertexDeclaration(elements, &decl);
        g_device->SetVertexDeclaration(decl);
        SetColor(1, 0, 0, 1);
        DrawQuad(-1, 1, 1, -1);
        Read(px);
        ExpectPixels("BAKED_NEW_DECL_DRAW", px, {{P(32, 32), red}});
        Deko9_GetCounters(g_device, &withShader);
        g_device->SetVertexDeclaration(g_decl);
        if (decl)
            decl->Release();
        Deko9_GetCounters(g_device, &released);
        std::snprintf(detail, sizeof(detail), "programUnits %u -> %u", withShader.programUnits, released.programUnits);
        Check(decl && released.programUnits < withShader.programUnits, "BAKED_DECL_RELEASE", detail);
    }

    // ---- instanced draws (Deko9_*Instances) ------------------------------
    // vs c4 = per-instance position offset, c5 = per-instance color (passed
    // to the pixel shader through t0). Three instances in one draw must give
    // exactly the image of three ordinary draws.
    {
        IDirect3DVertexBuffer9 *vb = nullptr;
        IDirect3DIndexBuffer9 *ib = nullptr;
        g_device->CreateVertexBuffer(4 * sizeof(Vertex), 0, 0, D3DPOOL_MANAGED, &vb, nullptr);
        g_device->CreateIndexBuffer(6 * sizeof(uint16_t), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
        void *data = nullptr;
        if (vb && SUCCEEDED(vb->Lock(0, 0, &data, 0)))
        {
            const Vertex quad[4] = {{-0.95f, 0.25f, 0.5f, 1, 0, 0}, {-0.45f, 0.25f, 0.5f, 1, 1, 0},
                                    {-0.95f, -0.25f, 0.5f, 1, 0, 1}, {-0.45f, -0.25f, 0.5f, 1, 1, 1}};
            std::memcpy(data, quad, sizeof(quad));
            vb->Unlock();
        }
        if (ib && SUCCEEDED(ib->Lock(0, 0, &data, 0)))
        {
            const uint16_t indices[6] = {0, 1, 2, 1, 3, 2};
            std::memcpy(data, indices, sizeof(indices));
            ib->Unlock();
        }
        const float offsets[3][4] = {{0, 0, 0, 0}, {0.7f, 0, 0, 0}, {1.4f, 0, 0, 0}};
        const float colors[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
        auto setup = [&] {
            BeginPass();
            g_device->SetVertexShader(g_vsInstanced);
            g_device->SetPixelShader(g_psVarying);
            g_device->SetStreamSource(0, vb, 0, sizeof(Vertex));
            g_device->SetIndices(ib);
            g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
        };
        uint32_t reference[kSize * kSize];
        setup();
        for (int i = 0; i < 3; ++i)
        {
            g_device->SetVertexShaderConstantF(4, offsets[i], 1);
            g_device->SetVertexShaderConstantF(5, colors[i], 1);
            g_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
        }
        Read(reference);
        ExpectPixels("INSTANCE_REFERENCE", reference,
                     {{P(11, 32), red}, {P(33, 32), green}, {P(56, 32), blue}, {P(32, 5), black}});
        setup();
        Deko9Counters instBefore{}, instAfter{};
        Deko9_GetCounters(g_device, &instBefore);
        bool began = false;
        int32_t hr = -1;
        {
            Deko9Batch batch(g_device);
            const uint8_t regs[2] = {4, 5};
            began = Deko9_BeginInstances(g_device, regs, 2);
            for (int i = 0; began && i < 3; ++i)
            {
                g_device->SetVertexShaderConstantF(4, offsets[i], 1);
                g_device->SetVertexShaderConstantF(5, colors[i], 1);
                Deko9_AddInstance(g_device);
            }
            if (began)
                hr = Deko9_DrawIndexedInstances(g_device, 0, 0, 4, 0, 2);
        }
        Deko9_GetCounters(g_device, &instAfter);
        Read(px);
        char detail[128];
        std::snprintf(detail, sizeof(detail), "began=%d hr=%d draws=%llu instanced=%llu instances=%llu", began,
                      (int)hr, (unsigned long long)(instAfter.draws - instBefore.draws),
                      (unsigned long long)(instAfter.instancedDraws - instBefore.instancedDraws),
                      (unsigned long long)(instAfter.instances - instBefore.instances));
        Check(began && hr >= 0 && instAfter.draws - instBefore.draws == 1 &&
                  instAfter.instancedDraws - instBefore.instancedDraws == 1 &&
                  instAfter.instances - instBefore.instances == 3,
              "INSTANCED_ONE_DRAW", detail);
        uint32_t differing = 0;
        for (UINT i = 0; i < kSize * kSize; ++i)
            differing += px[i] != reference[i];
        std::snprintf(detail, sizeof(detail), "%u of %u pixels differ", differing, kSize * kSize);
        Check(differing == 0, "INSTANCED_PIXEL_IDENTICAL", detail);
        // An ordinary draw right after must use the ordinary program again
        // (constants c4/c5 from the file: the last instance's values).
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
        g_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
        Read(px);
        ExpectPixels("INSTANCED_THEN_ORDINARY", px, {{P(56, 32), blue}, {P(11, 32), black}});
        g_device->SetStreamSource(0, nullptr, 0, 0);
        g_device->SetIndices(nullptr);
        g_device->SetVertexShader(g_vs);
        if (vb)
            vb->Release();
        if (ib)
            ib->Release();
    }

    // ---- frame arena (deko9_arena.h) ---------------------------------------------------
    // Deko9_FrameAlloc + Deko9_BindWindow re-point a dynamic VB/IB's
    // storage at fresh per-frame memory with no Lock/Unlock. Four frames
    // each draw into their own column of the target through the SAME
    // window-bound buffers, writing a distinct vertex colour straight
    // through the span's CPU pointer; a heavy filler draw to a throwaway
    // surface keeps the GPU busy so Present() races ahead of it, and only
    // the last frame is waited for -- with kFramesInFlight == 2, earlier
    // frames are still queued on the GPU when later ones are recorded,
    // exactly the case a still-owned chunk must never be reused for. One
    // readback at the end must show every column with its own frame's
    // colour: a stale/overlapping chunk would show the wrong one.
    {
        IDirect3DVertexBuffer9 *vb = nullptr;
        IDirect3DIndexBuffer9 *ib = nullptr;
        IDirect3DTexture9 *fillerTex = nullptr;
        IDirect3DSurface9 *fillerSurf = nullptr;
        g_device->CreateVertexBuffer(4 * sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
                                     &vb, nullptr);
        g_device->CreateIndexBuffer(6 * sizeof(uint16_t), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                    D3DPOOL_DEFAULT, &ib, nullptr);
        g_device->CreateTexture(512, 512, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &fillerTex,
                                nullptr);
        if (fillerTex)
            fillerTex->GetSurfaceLevel(0, &fillerSurf);

        BeginPass();
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);

        constexpr int kFrames = 4;
        uint64_t frameIds[kFrames] = {};
        uint32_t expectedR[kFrames] = {}, expectedG[kFrames] = {};
        bool allocOk = true;
        int overlapCount = 0;
        for (int i = 0; i < kFrames && vb && ib; ++i)
        {
            // Busy-work on a throwaway target so this frame's Present() can
            // race ahead of its own GPU work (never touches g_targetSurface,
            // so earlier columns survive).
            if (fillerSurf)
            {
                g_device->SetRenderTarget(0, fillerSurf);
                g_device->SetPixelShader(g_psColor);
                for (int f = 0; f < 300; ++f)
                {
                    SetColor((float)(f & 7) / 7.0f, 0.5f, (float)(f % 5) / 4.0f, 1.0f);
                    DrawQuad(-1, 1, 1, -1);
                }
            }

            const float x0 = -1.0f + i * 0.5f, x1 = x0 + 0.5f;
            const float u = (i * 64 + 32) / 255.0f, v = 128.0f / 255.0f;
            const Vertex quad[4] = {{x0, 1, 0.5f, 1, u, v}, {x1, 1, 0.5f, 1, u, v}, {x1, -1, 0.5f, 1, u, v},
                                    {x0, -1, 0.5f, 1, u, v}};
            const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};

            const uint64_t frame = Deko9_FrameRecording(g_device);
            frameIds[i] = frame;
            expectedR[i] = (uint32_t)std::lround(u * 255.0f);
            expectedG[i] = (uint32_t)std::lround(v * 255.0f);

            Deko9Span vbSpan{}, ibSpan{};
            const bool a1 = Deko9_FrameAlloc(g_device, frame, sizeof(quad), 256, &vbSpan);
            const bool a2 = Deko9_FrameAlloc(g_device, frame, sizeof(indices), 256, &ibSpan);
            allocOk &= a1 && a2;
            if (a1 && a2)
            {
                Deko9_BindWindow(vb, vbSpan);
                Deko9_BindWindow(ib, ibSpan);
                std::memcpy(vbSpan.cpu, quad, sizeof(quad));
                std::memcpy(ibSpan.cpu, indices, sizeof(indices));
                g_device->SetRenderTarget(0, g_targetSurface);
                g_device->SetPixelShader(g_psVarying);
                g_device->SetStreamSource(0, vb, 0, sizeof(Vertex));
                g_device->SetIndices(ib);
                g_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
            }
            g_device->Present(nullptr, nullptr, nullptr, nullptr);
            if (i > 0 && !Deko9_FrameDone(g_device, frameIds[i - 1]))
                ++overlapCount;
        }
        bool waited = false;
        for (int i = 0; i < 10000 && !(waited = Deko9_WaitFrame(g_device, frameIds[kFrames - 1], 1000000)); ++i)
        {
        }
        char setupDetail[96];
        std::snprintf(setupDetail, sizeof(setupDetail), "allocOk=%d waited=%d overlaps=%d/%d", allocOk, waited,
                      overlapCount, kFrames - 1);
        Check(vb && ib && allocOk && waited, "FRAME_ARENA_SETUP", setupDetail);

        // Compare only R/G (the vertex's own u,v, i.e. the window's actual
        // per-frame content); B/A come from the declaration's default fill
        // for the unwritten FLOAT2 components and are not what this test is
        // proving.
        Check(Read(px), "FRAME_ARENA_READBACK");
        const int colX[kFrames] = {8, 24, 40, 56};
        bool colorOk = true;
        char colorDetail[192] = "";
        for (int i = 0; i < kFrames; ++i)
        {
            const uint32_t got = px[32 * kSize + colX[i]];
            const int gotR = (int)((got >> 16) & 0xff), gotG = (int)((got >> 8) & 0xff);
            const int dR = gotR - (int)expectedR[i], dG = gotG - (int)expectedG[i];
            const bool near = dR >= -3 && dR <= 3 && dG >= -3 && dG <= 3;
            if (!near && colorOk)
                std::snprintf(colorDetail, sizeof(colorDetail), "col %d (%d,32)=%08x want R=%02x G=%02x", i,
                              colX[i], (unsigned)got, (unsigned)expectedR[i], (unsigned)expectedG[i]);
            colorOk &= near;
        }
        Check(colorOk, "FRAME_ARENA_WINDOW_PER_FRAME_COLOR", colorDetail);

        g_device->SetRenderTarget(0, g_targetSurface);
        g_device->SetStreamSource(0, nullptr, 0, 0);
        g_device->SetIndices(nullptr);
        if (vb)
            vb->Release();
        if (ib)
            ib->Release();
        if (fillerSurf)
            fillerSurf->Release();
        if (fillerTex)
            fillerTex->Release();
    }

    // ---- ranged draws (Deko9_DrawIndexedRanges) ---------------------------
    // Three quads in one vertex/index buffer (red constant color). Ranges
    // {quad 0, quad 2} in one call must give exactly the image of two
    // DrawIndexedPrimitive calls, as one state application; baseVertex
    // applies to every range; a range past the index buffer fails whole.
    {
        IDirect3DVertexBuffer9 *vb = nullptr;
        IDirect3DIndexBuffer9 *ib = nullptr;
        g_device->CreateVertexBuffer(12 * sizeof(Vertex), 0, 0, D3DPOOL_MANAGED, &vb, nullptr);
        g_device->CreateIndexBuffer(18 * sizeof(uint16_t), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
        void *data = nullptr;
        if (vb && SUCCEEDED(vb->Lock(0, 0, &data, 0)))
        {
            Vertex quads[12];
            const float x0[3] = {-0.95f, -0.25f, 0.45f};
            for (int q = 0; q < 3; ++q)
            {
                quads[4 * q + 0] = {x0[q], 0.25f, 0.5f, 1, 0, 0};
                quads[4 * q + 1] = {x0[q] + 0.5f, 0.25f, 0.5f, 1, 1, 0};
                quads[4 * q + 2] = {x0[q], -0.25f, 0.5f, 1, 0, 1};
                quads[4 * q + 3] = {x0[q] + 0.5f, -0.25f, 0.5f, 1, 1, 1};
            }
            std::memcpy(data, quads, sizeof(quads));
            vb->Unlock();
        }
        if (ib && SUCCEEDED(ib->Lock(0, 0, &data, 0)))
        {
            uint16_t indices[18];
            const uint16_t quad[6] = {0, 1, 2, 1, 3, 2};
            for (int q = 0; q < 3; ++q)
                for (int i = 0; i < 6; ++i)
                    indices[6 * q + i] = (uint16_t)(quad[i] + 4 * q);
            std::memcpy(data, indices, sizeof(indices));
            ib->Unlock();
        }
        auto setup = [&] {
            BeginPass();
            g_device->SetStreamSource(0, vb, 0, sizeof(Vertex));
            g_device->SetIndices(ib);
            g_device->SetPixelShader(g_psColor);
            SetColor(1, 0, 0, 1);
            g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
        };
        uint32_t reference[kSize * kSize];
        setup();
        g_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 12, 0, 2);
        g_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 12, 12, 2);
        Read(reference);
        ExpectPixels("RANGES_REFERENCE", reference,
                     {{P(10, 32), red}, {P(32, 32), black}, {P(54, 32), red}, {P(32, 5), black}});
        setup();
        Deko9Counters rBefore{}, rAfter{};
        Deko9_GetCounters(g_device, &rBefore);
        const Deko9IndexRange ranges[2] = {{0, 2, 0}, {12, 2, 0}};
        const int32_t hr = Deko9_DrawIndexedRanges(g_device, 12, ranges, 2);
        Deko9_GetCounters(g_device, &rAfter);
        Read(px);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "hr=%d draws=%llu rangeCalls=%llu rangeDraws=%llu", (int)hr,
                      (unsigned long long)(rAfter.draws - rBefore.draws),
                      (unsigned long long)(rAfter.rangeCalls - rBefore.rangeCalls),
                      (unsigned long long)(rAfter.rangeDraws - rBefore.rangeDraws));
        Check(hr >= 0 && rAfter.draws - rBefore.draws == 1 && rAfter.rangeCalls - rBefore.rangeCalls == 1 &&
                  rAfter.rangeDraws - rBefore.rangeDraws == 2,
              "RANGES_ONE_STATE_TWO_DRAWS", detail);
        uint32_t differing = 0;
        for (UINT i = 0; i < kSize * kSize; ++i)
            differing += px[i] != reference[i];
        std::snprintf(detail, sizeof(detail), "%u of %u pixels differ", differing, kSize * kSize);
        Check(differing == 0, "RANGES_PIXEL_IDENTICAL", detail);
        // Per-range baseVertex 4 and 8 move quad 0's indices onto quads 1
        // and 2 (the static-model case: one surface, one slot per instance).
        setup();
        const Deko9IndexRange moved[2] = {{0, 2, 4}, {0, 2, 8}};
        const int32_t hrBase = Deko9_DrawIndexedRanges(g_device, 4, moved, 2);
        Read(px);
        std::snprintf(detail, sizeof(detail), "hr=%d", (int)hrBase);
        Check(hrBase >= 0, "RANGES_BASE_VERTEX_HR", detail);
        ExpectPixels("RANGES_BASE_VERTEX", px, {{P(10, 32), black}, {P(32, 32), red}, {P(54, 32), red}});
        // A range reaching past the 18 indices rejects the whole call.
        setup();
        Deko9_GetCounters(g_device, &rBefore);
        const Deko9IndexRange bad[2] = {{0, 2, 0}, {16, 2, 0}};
        const int32_t hrBad = Deko9_DrawIndexedRanges(g_device, 12, bad, 2);
        Deko9_GetCounters(g_device, &rAfter);
        Read(px);
        std::snprintf(detail, sizeof(detail), "hr=%d draws=%llu", (int)hrBad,
                      (unsigned long long)(rAfter.draws - rBefore.draws));
        Check(hrBad < 0 && rAfter.draws == rBefore.draws, "RANGES_OUT_OF_BOUNDS_REJECTED", detail);
        ExpectPixels("RANGES_OUT_OF_BOUNDS_NOTHING_DRAWN", px, {{P(10, 32), black}, {P(54, 32), black}});
        g_device->SetStreamSource(0, nullptr, 0, 0);
        g_device->SetIndices(nullptr);
        if (vb)
            vb->Release();
        if (ib)
            ib->Release();
    }
    Deko9_GetCounters(g_device, &after);
    {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "draws=%llu mismatches=%llu",
                      (unsigned long long)(after.verifiedDraws - before.verifiedDraws),
                      (unsigned long long)(after.verifyMismatches - before.verifyMismatches));
        Check(after.verifiedDraws > before.verifiedDraws && after.verifyMismatches == before.verifyMismatches,
              "BAKED_VERIFY", detail);
    }
    Deko9_SetVerify(g_device, false);
    // Light barriers (r_deko9LightBarriers 1/2): render -> sample -> render
    // -> sample within the 3D pipe, in one list, with the lighter barrier.
    for (uint32_t mode = 1; mode <= 2; ++mode)
    {
        Deko9_SetBarrierMode(g_device, mode);
        BeginPass();
        g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_device->SetPixelShader(g_psColor);
        g_device->SetRenderTarget(0, rttSurface);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, green, 1.0f, 0);
        SetColor(0, 0, 1, 1);
        DrawQuad(-1, -0.5f, 1, -1); // bottom quarter blue
        g_device->SetRenderTarget(0, g_targetSurface);
        g_device->SetPixelShader(g_psTexture);
        g_device->SetTexture(0, rtt);
        DrawQuad(-1, 1, 0, -1); // left half: sampled rtt
        g_device->SetPixelShader(g_psColor);
        g_device->SetRenderTarget(0, rttSurface);
        SetColor(1, 0, 0, 1);
        DrawQuad(-1, 1, 1, 0.5f); // top quarter red, after it was sampled
        g_device->SetRenderTarget(0, g_targetSurface);
        g_device->SetPixelShader(g_psTexture);
        DrawQuad(0, 1, 1, -1); // right half: sampled again
        g_device->SetTexture(0, nullptr);
        Read(px);
        ExpectPixels(mode == 1 ? "LIGHT_BARRIER_PRIMITIVES" : "LIGHT_BARRIER_FRAGMENTS", px,
                     {{P(10, 5), green}, {P(10, 30), green}, {P(10, 60), blue}, {P(40, 5), red},
                      {P(40, 30), green}, {P(40, 60), blue}});
    }
    Deko9_SetBarrierMode(g_device, 0);

    // Tiled caching (r_deko9TiledCache 1/2) must render exactly like off: many
    // overlapping translucent layers (blend read-modify-write on every pixel),
    // with a render-to-texture sampled in the middle of the list.
    {
        static uint32_t ref[kSize * kSize], got[kSize * kSize];
        for (uint32_t mode = 0; mode <= 2; ++mode)
        {
            Deko9_SetTiledCache(g_device, mode);
            g_device->Present(nullptr, nullptr, nullptr, nullptr); // the mode applies at the next list
            BeginPass();
            g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            g_device->SetPixelShader(g_psColor);
            g_device->SetRenderTarget(0, rttSurface);
            g_device->Clear(0, nullptr, D3DCLEAR_TARGET, green, 1.0f, 0);
            SetColor(0, 0, 1, 1);
            DrawQuad(-1, -0.5f, 1, -1);
            g_device->SetRenderTarget(0, g_targetSurface);
            g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
            g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            g_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            g_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            for (int layer = 0; layer < 24; ++layer)
            {
                const float t = (float)layer / 24.0f;
                SetColor(t, 1.0f - t, 0.5f, 0.2f + 0.02f * (float)(layer % 7));
                DrawQuad(-1.0f + 0.05f * (float)(layer % 9), 1.0f - 0.04f * (float)(layer % 5),
                         0.2f + 0.04f * (float)(layer % 11), -1.0f + 0.03f * (float)(layer % 13));
                if (layer == 11)
                {
                    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                    g_device->SetPixelShader(g_psTexture);
                    g_device->SetTexture(0, rtt);
                    DrawQuad(0.5f, 1, 1, 0);
                    g_device->SetTexture(0, nullptr);
                    g_device->SetPixelShader(g_psColor);
                    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                }
            }
            g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            Read(mode ? got : ref);
            if (mode)
            {
                char detail[64];
                std::snprintf(detail, sizeof(detail), "mode=%u", mode);
                Check(!std::memcmp(ref, got, sizeof(ref)), "TILED_CACHE_IDENTICAL", detail);
            }
        }
        Deko9_SetTiledCache(g_device, 0);
        g_device->Present(nullptr, nullptr, nullptr, nullptr);
    }

    // Zcull (r_deko9ZcullStats; deko9_zcull.cpp; zcull itself is always on,
    // no toggle). The region model: a full depth clear makes the region
    // valid for the pass's depth-tested draws; binding another depth surface
    // invalidates it until the next clear, and a LESSEQUAL -> GREATER change
    // is a direction flip. Then a depth-tested draw must stay exact
    // (some emulators do not emulate zcull, so there this only checks correctness
    // with the region model live; hardware also checks the hardware path).
    {
        IDirect3DSurface9 *depth2 = nullptr;
        g_device->CreateDepthStencilSurface(kSize, kSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth2,
                                            nullptr);
        Deko9_SetZcullStats(g_device, true);
        g_device->Present(nullptr, nullptr, nullptr, nullptr); // applies stats (a fresh window)
        BeginPass();
        Deko9_GpuMarker(g_device, Deko9GpuPass_Lit);
        g_device->SetDepthStencilSurface(depth);
        g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        g_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, black, 1.0f, 0);
        g_device->SetPixelShader(g_psColor);
        SetColor(1, 0, 0, 1);
        DrawQuad(-1, 1, 1, -1, 0.3f);
        Deko9_GpuMarker(g_device, Deko9GpuPass_Decal);
        g_device->SetDepthStencilSurface(depth2);
        DrawQuad(-1, 1, 1, -1, 0.3f);
        g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER);
        DrawQuad(-1, 1, 1, -1, 0.3f);
        Deko9ZcullPassStats lit{}, decal{};
        const bool gotStats = Deko9_GetZcullPassStats(g_device, Deko9GpuPass_Lit, &lit) &&
                              Deko9_GetZcullPassStats(g_device, Deko9GpuPass_Decal, &decal);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "lit ztest=%llu invalid=%llu clears=%llu decal ztest=%llu invalid=%llu "
                                              "binds=%llu flips=%llu",
                      (unsigned long long)lit.ztest, (unsigned long long)lit.invalid,
                      (unsigned long long)lit.fullClears, (unsigned long long)decal.ztest,
                      (unsigned long long)decal.invalid, (unsigned long long)decal.binds,
                      (unsigned long long)decal.flips);
        Check(gotStats && lit.ztest == 1 && lit.invalid == 0 && lit.fullClears == 1, "ZCULL_MODEL_CLEARED", detail);
        Check(gotStats && decal.binds == 1 && decal.ztest == 2 && decal.invalid == 2 && decal.flips == 1,
              "ZCULL_MODEL_INVALIDATED", detail);
        g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        // Nearer red, then a farther blue full quad and a nearer green
        // square: red stays where green is not.
        BeginPass();
        g_device->SetDepthStencilSurface(depth);
        g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        g_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, black, 1.0f, 0);
        g_device->SetPixelShader(g_psColor);
        SetColor(1, 0, 0, 1);
        DrawQuad(-1, 1, 1, -1, 0.3f);
        SetColor(0, 0, 1, 1);
        DrawQuad(-1, 1, 1, -1, 0.6f);
        SetColor(0, 1, 0, 1);
        DrawQuad(-1, 1, 0, 0, 0.1f); // top-left quarter
        Read(px);
        ExpectPixels("ZCULL_DEPTH_TEST", px,
                     {{P(10, 10), green}, {P(50, 10), red}, {P(10, 50), red}, {P(50, 50), red}});
        Deko9_SetZcullStats(g_device, false);
        g_device->SetDepthStencilSurface(nullptr);
        if (depth2)
            depth2->Release();
    }

    // Present a few frames to the real swapchain.
    IDirect3DSurface9 *back = nullptr;
    g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    bool presented = back != nullptr;
    // Per-pass GPU timing: the device logs `DEKO9 gpupass ... lit=... present=...`
    // after 60 timed frames (turned on at the first Present).
    Deko9_SetGpuPasses(g_device, true);
    // Draw census (r_deko9DrawCensus) over the last 60 frames: run.sh checks
    // the `DEKO9 dcensus label=selftest` lines (the hud2d quad writes
    // 640x360 samples per draw; the lights pass: 2 lights in view, 1
    // partition drawn with one draw).
    // A ranged call (quads 0 and 2 of three, under the hud quad) in the
    // decal pass: one API draw and two deko3d draws per frame (`draws=` vs
    // `gdraws=` of the decal pass line).
    IDirect3DVertexBuffer9 *censusVb = nullptr;
    IDirect3DIndexBuffer9 *censusIb = nullptr;
    g_device->CreateVertexBuffer(12 * sizeof(Vertex), 0, 0, D3DPOOL_MANAGED, &censusVb, nullptr);
    g_device->CreateIndexBuffer(18 * sizeof(uint16_t), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &censusIb, nullptr);
    {
        void *data = nullptr;
        if (censusVb && SUCCEEDED(censusVb->Lock(0, 0, &data, 0)))
        {
            Vertex quads[12];
            for (int q = 0; q < 3; ++q)
            {
                const float x0 = -0.3f + 0.2f * q;
                quads[4 * q + 0] = {x0, 0.2f, 0.5f, 1, 0, 0};
                quads[4 * q + 1] = {x0 + 0.15f, 0.2f, 0.5f, 1, 1, 0};
                quads[4 * q + 2] = {x0, -0.2f, 0.5f, 1, 0, 1};
                quads[4 * q + 3] = {x0 + 0.15f, -0.2f, 0.5f, 1, 1, 1};
            }
            std::memcpy(data, quads, sizeof(quads));
            censusVb->Unlock();
        }
        if (censusIb && SUCCEEDED(censusIb->Lock(0, 0, &data, 0)))
        {
            uint16_t indices[18];
            const uint16_t quad[6] = {0, 1, 2, 1, 3, 2};
            for (int q = 0; q < 3; ++q)
                for (int i = 0; i < 6; ++i)
                    indices[6 * q + i] = (uint16_t)(quad[i] + 4 * q);
            std::memcpy(data, indices, sizeof(indices));
            censusIb->Unlock();
        }
    }
    for (int frame = 0; frame < 120 && presented && appletMainLoop(); ++frame)
    {
        if (frame == 60)
        {
            Deko9_SetDrawCensus(g_device, 1, 0);
            Deko9_CensusReport(g_device, nullptr, 0, 0);
        }
        g_device->SetRenderTarget(0, back);
        Deko9_GpuMarker(g_device, Deko9GpuPass_Lit);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, frame & 16 ? blue : green, 1.0f, 0);
        g_device->SetPixelShader(g_psColor);
        Deko9_GpuMarker(g_device, Deko9GpuPass_PointLights);
        Deko9_CensusLight(g_device, 0, 2);
        Deko9_CensusLight(g_device, 1, 0);
        Deko9_CensusLabel(g_device, "selftest_light", "light_tech", "ps_color", "vs_quad", g_psColor);
        SetColor(1, 0, 0, 1);
        DrawQuad(-0.1f, 0.1f, 0.1f, -0.1f); // covered by the hud quad below
        Deko9_CensusLight(g_device, 0, 0);
        if (censusVb && censusIb)
        {
            Deko9_GpuMarker(g_device, Deko9GpuPass_Decal);
            Deko9_CensusLabel(g_device, "selftest_ranges", "ranges_tech", "ps_color", "vs_quad", g_psColor);
            g_device->SetStreamSource(0, censusVb, 0, sizeof(Vertex));
            g_device->SetIndices(censusIb);
            SetColor(0, 1, 0, 1);
            const Deko9IndexRange ranges[2] = {{0, 2, 0}, {12, 2, 0}};
            Deko9_DrawIndexedRanges(g_device, 12, ranges, 2);
            g_device->SetStreamSource(0, nullptr, 0, 0);
            g_device->SetIndices(nullptr);
        }
        Deko9_GpuMarker(g_device, Deko9GpuPass_Hud2D);
        Deko9_CensusLabel(g_device, "selftest_quad", "hud_tech", "ps_color", "vs_quad", g_psColor);
        SetColor(1, 1, 1, 1);
        DrawQuad(-0.5f, 0.5f, 0.5f, -0.5f);
        presented = SUCCEEDED(g_device->Present(nullptr, nullptr, nullptr, nullptr));
    }
    Deko9_CensusReport(g_device, "selftest", 1280, 720);
    Deko9_SetDrawCensus(g_device, 0, 0);
    if (censusVb)
        censusVb->Release();
    if (censusIb)
        censusIb->Release();
    Check(presented, "PRESENT", "120 frames");

    // Front buffer = the last presented swapchain image (blit path: the
    // back buffer is display size). Frame 119 cleared to blue.
    {
        IDirect3DSurface9 *front = nullptr;
        bool ok = SUCCEEDED(g_device->CreateOffscreenPlainSurface(1280, 720, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                                  &front, nullptr)) &&
                  SUCCEEDED(g_device->GetFrontBufferData(0, front));
        char detail[96] = "";
        D3DLOCKED_RECT locked;
        if (ok && SUCCEEDED(front->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        {
            uint32_t center, corner;
            // Outside the white quad the present loop draws over the centre
            // (x 320..960, y 180..540) for the per-pass GPU timing.
            std::memcpy(&center, static_cast<uint8_t *>(locked.pBits) + 100 * locked.Pitch + 100 * 4, 4);
            std::memcpy(&corner, static_cast<uint8_t *>(locked.pBits) + 719 * locked.Pitch + 1279 * 4, 4);
            ok = Near(center, blue) && Near(corner, blue);
            std::snprintf(detail, sizeof(detail), "center=%08x corner=%08x", (unsigned)center, (unsigned)corner);
            front->UnlockRect();
        }
        else
        {
            ok = false;
        }
        Check(ok, "FRONT_BUFFER_BLIT", detail);
        if (front)
            front->Release();
    }

    for (IUnknown *object : {(IUnknown *)texture, (IUnknown *)lumSys, (IUnknown *)lum, (IUnknown *)rttSurface,
                             (IUnknown *)rtt, (IUnknown *)depth, (IUnknown *)back})
    {
        if (object)
            object->Release();
    }
}

// Hardware record for the upscalers: on real hardware,
// textureGather of components 1/2 of the BGRA8 back buffer returned wrong
// texels while an emulator was exact. A 4x4 texture with a distinct byte per
// texel and channel is gathered at the texel corners into a 3x3 target:
// target pixel (x, y) = gather(corner of texels x..x+1, y..y+1), expected
// .x = (x, y+1), .y = (x+1, y+1), .z = (x+1, y), .w = (x, y). One PASS/FAIL
// per format and component; never skipped.
// Event queries around a long GPU job, the way the engine's GPU sync
// (RB_AdaptiveGpuSyncFinal) uses them. The oracle is the raw fence of the
// command list holding the query's END (Deko9_DebugQueryGpuPassed): GPU
// progress only moves forward, so whenever GetData or Deko9_WaitQuery has
// reported done, the raw fence must already have signalled.
void RunQueryTests()
{
    constexpr UINT kJob = 512;
    IDirect3DTexture9 *jobTex = nullptr;
    IDirect3DSurface9 *jobSurface = nullptr;
    IDirect3DQuery9 *query = nullptr;
    bool ok = SUCCEEDED(g_device->CreateTexture(kJob, kJob, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                D3DPOOL_DEFAULT, &jobTex, nullptr)) &&
              SUCCEEDED(jobTex->GetSurfaceLevel(0, &jobSurface)) &&
              SUCCEEDED(g_device->CreateQuery(D3DQUERYTYPE_EVENT, &query));
    Check(ok, "QUERY_SETUP");
    if (!ok)
        return;
    const auto msSince = [](uint64_t start) { return armTicksToNs(armGetSystemTick() - start) / 1e6; };
    // Drain: nothing earlier stays in flight, so only the job is timed.
    query->Issue(D3DISSUE_END);
    for (int i = 0; i < 100000 && query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE; ++i)
        Deko9_WaitQuery(query, 1000000);
    // A job of `quads` full-target fills (400 quads ~ 105 Mpixel).
    const auto recordJob = [&](int quads) {
        BeginPass();
        g_device->SetRenderTarget(0, jobSurface);
        g_device->SetPixelShader(g_psColor);
        for (int i = 0; i < quads; ++i)
        {
            SetColor((float)(i & 7) / 7.0f, 0.5f, (float)(i % 5) / 4.0f, 1.0f);
            DrawQuad(-1, 1, 1, -1);
        }
        query->Issue(D3DISSUE_END);
    };

    // 1. END still in the open list: a poll without FLUSH does not submit
    //    and reports pending; a wait returns at once instead of blocking on
    //    a list that was never submitted.
    recordJob(400);
    const bool openPending = query->GetData(nullptr, 0, 0) == S_FALSE;
    uint64_t start = armGetSystemTick();
    const bool openWait = Deko9_WaitQuery(query, 1000000000ll);
    const double openWaitMs = msSince(start);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "pending=%d wait=%d waitMs=%.2f", openPending, openWait, openWaitMs);
    Check(openPending && !openWait && openWaitMs < 100.0, "QUERY_OPEN_PENDING", detail);

    // Records and submits a job with one FLUSH poll; a host GPU
    // can finish 400 quads before that poll returns, so the job grows 4x
    // until the poll right after submission is pending. Returns whether it
    // was; `quads` is the size used.
    int quads = 400;
    const auto submitLongJob = [&]() {
        for (quads = 400; quads <= 25600; quads *= 4)
        {
            recordJob(quads);
            if (query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
                return true;
        }
        quads /= 4;
        return false;
    };

    // 2. The engine's spin: FLUSH polls until done; done must agree with
    //    the raw fence.
    bool firstPending = submitLongJob();
    bool rawAtSubmit = Deko9_DebugQueryGpuPassed(query);
    start = armGetSystemTick();
    HRESULT hr;
    int pending = 0;
    while ((hr = query->GetData(nullptr, 0, D3DGETDATA_FLUSH)) == S_FALSE && pending < 50000000)
        ++pending;
    bool raw = Deko9_DebugQueryGpuPassed(query);
    std::snprintf(detail, sizeof(detail), "quads=%d firstPending=%d rawAtSubmit=%d hr=%ld raw=%d pendingPolls=%d ms=%.2f",
                  quads, firstPending, rawAtSubmit, (long)hr, raw, pending, msSince(start));
    Check(firstPending && !rawAtSubmit, "QUERY_LONG_JOB_PENDING", detail);
    Check(hr == S_OK && raw, "QUERY_POLL_NOT_EARLY", detail);

    // 3. The deko9 GPU-sync path: block in Deko9_WaitQuery 1 ms at a time.
    //    Done only with the raw fence signalled; GetData agrees afterwards.
    firstPending = submitLongJob();
    rawAtSubmit = Deko9_DebugQueryGpuPassed(query);
    // A short wait honours its timeout (the R_SyncGpu slice depends on it).
    start = armGetSystemTick();
    const bool shortDone = Deko9_WaitQuery(query, 100000);
    const double shortMs = msSince(start);
    const bool shortRaw = Deko9_DebugQueryGpuPassed(query);
    std::snprintf(detail, sizeof(detail), "quads=%d done=%d raw=%d ms=%.2f", quads, shortDone, shortRaw, shortMs);
    Check(shortMs < 20.0 && (!shortDone || shortRaw), "QUERY_WAIT_TIMEOUT", detail);
    start = armGetSystemTick();
    int waits = 0;
    bool done = false;
    while (!(done = Deko9_WaitQuery(query, 1000000)) && waits < 10000)
        ++waits;
    raw = Deko9_DebugQueryGpuPassed(query);
    hr = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    std::snprintf(detail, sizeof(detail), "quads=%d firstPending=%d rawAtSubmit=%d done=%d raw=%d hr=%ld waits=%d ms=%.2f",
                  quads, firstPending, rawAtSubmit, done, raw, (long)hr, waits, msSince(start));
    Check(firstPending && !rawAtSubmit, "QUERY_WAIT_PENDING_AT_SUBMIT", detail);
    Check(done && raw && hr == S_OK, "QUERY_WAIT_DONE", detail);

    // 4. D3D9 event semantics mid-list: a query issued after a few draws
    //    signals when those draws complete, before the long job recorded
    //    after it in the same list (an in-list fence, not the list's end).
    //    The oracle for "the later work has not completed" is the raw fence
    //    of a second query issued after the job.
    IDirect3DQuery9 *mid = nullptr;
    if (SUCCEEDED(g_device->CreateQuery(D3DQUERYTYPE_EVENT, &mid)))
    {
        bool midEarly = false, laterPending = false, midDone = false, midRaw = false;
        for (quads = 400; quads <= 25600 && !(midEarly && laterPending); quads *= 4)
        {
            BeginPass();
            g_device->SetRenderTarget(0, jobSurface);
            g_device->SetPixelShader(g_psColor);
            SetColor(0.25f, 0.5f, 0.75f, 1.0f);
            DrawQuad(-1, 1, 1, -1);
            mid->Issue(D3DISSUE_END);
            recordJob(quads); // the long job, then `query` END, same list
            const bool midPendingOpen = mid->GetData(nullptr, 0, 0) == S_FALSE; // not submitted yet
            query->GetData(nullptr, 0, D3DGETDATA_FLUSH);                       // submits the list
            done = false;
            for (int i = 0; i < 10000 && !(done = Deko9_WaitQuery(mid, 1000000)); ++i)
            {
            }
            laterPending = !Deko9_DebugQueryGpuPassed(query);
            midDone = done && mid->GetData(nullptr, 0, 0) == S_OK;
            midRaw = Deko9_DebugQueryGpuPassed(mid);
            midEarly = midPendingOpen && midDone && midRaw;
            while (query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
                Deko9_WaitQuery(query, 1000000);
        }
        std::snprintf(detail, sizeof(detail), "quads=%d midDone=%d midRaw=%d laterPending=%d", quads / 4, midDone,
                      midRaw, laterPending);
        Check(midEarly && laterPending, "QUERY_MID_LIST_SIGNALS_BEFORE_LATER_WORK", detail);

        // 5. Issue on an empty open list (the engine's swap fence right after
        //    a present): the point is the newest submitted list, so it
        //    completes with no further submit. The old binding (the open
        //    list) could only complete after the next frame's work.
        recordJob(400);
        query->GetData(nullptr, 0, D3DGETDATA_FLUSH); // submit; the open list is now empty
        mid->Issue(D3DISSUE_END);
        done = false;
        for (int i = 0; i < 10000 && !(done = Deko9_WaitQuery(mid, 1000000)); ++i)
        {
        }
        hr = mid->GetData(nullptr, 0, 0);
        const bool listDone = Deko9_DebugQueryGpuPassed(query);
        std::snprintf(detail, sizeof(detail), "done=%d hr=%ld prevListDone=%d", done, (long)hr, listDone);
        Check(done && hr == S_OK && listDone, "QUERY_EMPTY_LIST_POINT", detail);
        mid->Release();
    }
    else
        Check(false, "QUERY_MID_SETUP");

    // 6. Native frame pacing (Deko9_Frame*): a heavy frame F, then a light
    //    frame F + 1. Presenting F + 1 waits only for F - 1 (two frames in
    //    flight), so right after it returns F may still run on the GPU;
    //    F is done before F + 1, and every frame completes.
    {
        const uint32_t n = Deko9_FramesInFlight();
        bool overlapped = false, ordered = true, completed = true;
        int heavy = 400;
        for (; heavy <= 25600 && !overlapped; heavy *= 4)
        {
            recordJob(heavy);
            const uint64_t frameF = Deko9_FrameRecording(g_device);
            g_device->Present(nullptr, nullptr, nullptr, nullptr);
            BeginPass();
            DrawQuad(-1, 1, 1, -1);
            g_device->Present(nullptr, nullptr, nullptr, nullptr);
            overlapped = !Deko9_FrameDone(g_device, frameF);
            bool doneF = false;
            for (int i = 0; i < 10000 && !(doneF = Deko9_WaitFrame(g_device, frameF, 1000000)); ++i)
            {
            }
            ordered &= Deko9_FrameRecording(g_device) == frameF + 2;
            bool doneNext = false;
            for (int i = 0; i < 10000 && !(doneNext = Deko9_WaitFrame(g_device, frameF + 1, 1000000)); ++i)
            {
            }
            completed &= doneF && doneNext && !Deko9_FrameDone(g_device, frameF + 2); // F + 2 not presented
        }
        std::snprintf(detail, sizeof(detail), "framesInFlight=%u heavyQuads=%d overlapped=%d ordered=%d completed=%d",
                      n, heavy / 4, overlapped, ordered, completed);
        Check(n >= 2 && ordered && completed, "FRAME_RING_COMPLETES", detail);
        Check(overlapped, "FRAME_RING_PRESENT_OVERLAPS_GPU", detail);
    }

    // Unbind the job target; it stays allocated. With a 2048^2 target,
    // an emulated GPU took seconds on the next test's first list (texture
    // cache sync of the 16 MB target: "fence wait stalled"), released or
    // not; 512^2 with a longer job does not.
    g_device->SetRenderTarget(0, g_targetSurface);
    query->Release();
}

void RunGatherProbeTests()
{
    constexpr int kTex = 4, kOut = 3;
    const auto value = [](int x, int y, int c) { return (uint32_t)(c * 64 + y * 16 + x * 4 + 3); };
    struct Format
    {
        const char *name;
        D3DFORMAT format;
        DWORD usage;
    } formats[] = {{"BGRA8", D3DFMT_A8R8G8B8, 0},
                   {"RGBA8", D3DFMT_A8B8G8R8, 0},
                   {"BGRX8_RT", D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET}};
    IDirect3DTexture9 *target = nullptr;
    IDirect3DSurface9 *targetSurface = nullptr, *readback = nullptr;
    const bool setup =
        SUCCEEDED(g_device->CreateTexture(kOut, kOut, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                          &target, nullptr)) &&
        SUCCEEDED(target->GetSurfaceLevel(0, &targetSurface)) &&
        SUCCEEDED(g_device->CreateOffscreenPlainSurface(kOut, kOut, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback,
                                                        nullptr));
    for (const Format &format : formats)
    {
        IDirect3DTexture9 *sys = nullptr, *tex = nullptr;
        bool ok = setup &&
                  SUCCEEDED(g_device->CreateTexture(kTex, kTex, 1, 0, format.format, D3DPOOL_SYSTEMMEM, &sys,
                                                    nullptr)) &&
                  SUCCEEDED(g_device->CreateTexture(kTex, kTex, 1, format.usage, format.format, D3DPOOL_DEFAULT,
                                                    &tex, nullptr));
        D3DLOCKED_RECT locked;
        if (ok && SUCCEEDED(sys->LockRect(0, &locked, nullptr, 0)))
        {
            for (int y = 0; y < kTex; ++y)
                for (int x = 0; x < kTex; ++x)
                {
                    // Channel c = 0..3 is R, G, B, A.
                    const uint32_t r = value(x, y, 0), g = value(x, y, 1), b = value(x, y, 2), a = value(x, y, 3);
                    const uint32_t dword = format.format == D3DFMT_A8B8G8R8 ? a << 24 | b << 16 | g << 8 | r
                                                                            : a << 24 | r << 16 | g << 8 | b;
                    std::memcpy(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch + x * 4, &dword, 4);
                }
            sys->UnlockRect(0);
        }
        else
        {
            ok = false;
        }
        ok = ok && SUCCEEDED(g_device->UpdateTexture(sys, tex));
        for (int c = 0; c < 4; ++c)
        {
            char name[64], detail[160] = "";
            std::snprintf(name, sizeof(name), "GATHER_COMPONENTS_%s_C%d", format.name, c);
            const bool ran = ok && Deko9_GatherProbe(g_device, tex, targetSurface, c) &&
                             SUCCEEDED(g_device->GetRenderTargetData(targetSurface, readback)) &&
                             SUCCEEDED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
            if (!ran)
            {
                Check(false, name, "probe did not run");
                continue;
            }
            int wrong = 0;
            for (int y = 0; y < kOut; ++y)
                for (int x = 0; x < kOut; ++x)
                {
                    const uint8_t *p = static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch + x * 4; // B,G,R,A
                    const uint32_t got[4] = {p[2], p[1], p[0], p[3]};
                    const int texel[4][2] = {{x, y + 1}, {x + 1, y + 1}, {x + 1, y}, {x, y}};
                    for (int i = 0; i < 4; ++i)
                    {
                        // X8: alpha reads as one.
                        const uint32_t want = c == 3 && format.format == D3DFMT_X8R8G8B8
                                                  ? 255u
                                                  : value(texel[i][0], texel[i][1], c);
                        if (got[i] != want && !wrong++)
                            std::snprintf(detail, sizeof(detail), "first: pixel (%d,%d) .%c=%u want %u (texel %d,%d)",
                                          x, y, "xyzw"[i], (unsigned)got[i], (unsigned)want, texel[i][0],
                                          texel[i][1]);
                    }
                }
            readback->UnlockRect();
            if (wrong)
            {
                const size_t len = std::strlen(detail);
                std::snprintf(detail + len, sizeof(detail) - len, " wrong=%d of %d", wrong, kOut * kOut * 4);
            }
            Check(!wrong, name, detail);
        }
        for (IUnknown *object : {(IUnknown *)sys, (IUnknown *)tex})
        {
            if (object)
                object->Release();
        }
    }
    for (IUnknown *object : {(IUnknown *)target, (IUnknown *)targetSurface, (IUnknown *)readback})
    {
        if (object)
            object->Release();
    }
}

// Native float-Z (Deko9_BuildFloatZ, r_deko9NativeFloatZ): a depth-stencil
// surface gets quads at known view depths drawn through the engine's two
// viewport depth ranges (scene 1/64..1, depth-hack viewmodel 0..1/64) with
// the engine's infinite projection terms, and one column stays at the
// depth clear. The rebuilt R32F target must hold the signed view depth
// (+z scene, -z viewmodel, 2e6 clear) to within the D24 quantisation of
// the stored depth, and match FloatZReference (the engine's verify oracle)
// evaluated at the quantised depth. Run on a compressed and a plain depth
// surface: a hardware run is the proof that compressed depth samples as
// plain values.
void RunFloatZTests(bool compressed)
{
    Deko9_SetRtCompression(compressed);
    const char *suffix = compressed ? "" : "_NOCOMP";
    IDirect3DSurface9 *depth = nullptr, *floatSurface = nullptr, *readback = nullptr;
    IDirect3DTexture9 *floatTex = nullptr;
    const bool setup =
        SUCCEEDED(g_device->CreateDepthStencilSurface(kSize, kSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                      &depth, nullptr)) &&
        SUCCEEDED(g_device->CreateTexture(kSize, kSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT,
                                          &floatTex, nullptr)) &&
        SUCCEEDED(floatTex->GetSurfaceLevel(0, &floatSurface)) &&
        SUCCEEDED(g_device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &readback,
                                                        nullptr));
    char name[64];
    std::snprintf(name, sizeof(name), "FLOATZ_SETUP%s", suffix);
    Check(setup, name);
    if (!setup)
        return;
    std::snprintf(name, sizeof(name), "FLOATZ_DEPTH_COMPRESSED%s", suffix);
    Check(Deko9_IsCompressed(depth) == compressed, name);

    // The engine's infinite projection (InfinitePerspectiveMatrix): m22 = k,
    // m32 = -zNear * k; depth hack replaces m32 with -r_znear_depthhack.
    const float k = 0.99951172f, zNear = 4.0f, zNearHack = 0.1f;
    deko9::FloatZRange scene{k, -zNear * k, 0.015625f, 1.0f}, viewmodel{k, -zNearHack, 0.0f, 0.015625f};
    deko9::FloatZConstants constants;
    deko9::FloatZSetup(&constants, scene, viewmodel, 2000000.0f);
    // Column c (8 px wide): view depth, and whether it is depth-hack geometry.
    struct Column
    {
        float z;
        bool hack;
    } columns[7] = {{4.5f, false}, {40.0f, false}, {400.0f, false}, {4000.0f, false},
                    {1.0f, true},  {6.0f, true},   {30.0f, true}};
    g_device->SetRenderTarget(0, g_targetSurface);
    g_device->SetDepthStencilSurface(depth);
    g_device->SetVertexShader(g_vs);
    g_device->SetVertexDeclaration(g_decl);
    g_device->SetPixelShader(g_psColor);
    SetColor(1, 1, 1, 1);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    for (int c = 0; c < 7; ++c)
    {
        const deko9::FloatZRange &range = columns[c].hack ? viewmodel : scene;
        D3DVIEWPORT9 vp{0, 0, kSize, kSize, range.minZ, range.maxZ};
        g_device->SetViewport(&vp);
        const float ndc = range.m22 + range.m32 / columns[c].z;
        DrawQuad(-1.0f + c * 0.25f, -1.0f, -1.0f + (c + 1) * 0.25f, 1.0f, ndc);
    }
    D3DVIEWPORT9 full{0, 0, kSize, kSize, 0.0f, 1.0f};
    g_device->SetViewport(&full);
    g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);

    std::snprintf(name, sizeof(name), "FLOATZ_BUILD%s", suffix);
    const bool built = Deko9_BuildFloatZ(g_device, depth, floatSurface, &constants);
    Check(built, name);
    D3DLOCKED_RECT locked;
    const bool read = built && SUCCEEDED(g_device->GetRenderTargetData(floatSurface, readback)) &&
                      SUCCEEDED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
    std::snprintf(name, sizeof(name), "FLOATZ_READBACK%s", suffix);
    Check(read, name);
    if (read)
    {
        for (int c = 0; c < 8; ++c)
        {
            // Sample the column centre row by row: every pixel of a column
            // holds the same depth.
            float worstRef = 0.0f, worstTruth = 0.0f, got0 = 0.0f;
            bool ok = true;
            double want = 2000000.0, dQ = 1.0;
            if (c < 7)
            {
                const deko9::FloatZRange &range = columns[c].hack ? viewmodel : scene;
                const double ndc = (double)range.m22 + (double)range.m32 / columns[c].z;
                const double d = range.minZ + (range.maxZ - (double)range.minZ) * ndc;
                // D24 unorm storage of d.
                dQ = std::floor(d * 16777215.0 + 0.5) / 16777215.0;
                want = columns[c].hack ? -columns[c].z : columns[c].z;
            }
            const float ref = deko9::FloatZReference(constants, (float)dQ);
            // One D24 step of depth, in view depth: the quantisation bound.
            const float step = std::fabs(deko9::FloatZReference(constants, (float)(dQ + 1.0 / 16777215.0)) - ref);
            for (int y = 0; y < (int)kSize; ++y)
            {
                for (int x = c * 8 + 1; x < c * 8 + 7; ++x)
                {
                    float got;
                    std::memcpy(&got, static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch + x * 4, 4);
                    if (x == c * 8 + 1 && !y)
                        got0 = got;
                    const float errRef = std::fabs(got - ref), errTruth = std::fabs(got - (float)want);
                    worstRef = std::max(worstRef, errRef);
                    worstTruth = std::max(worstTruth, errTruth);
                    // Reference: the same float arithmetic on the quantised
                    // depth, allowing the rasteriser's rounding to the
                    // neighbouring D24 value and a few ulp of GPU division.
                    // Truth: the view depth itself, within two D24 steps.
                    if (errRef > 1.01f * step + 1e-5f * std::fabs(ref) ||
                        errTruth > 2.0f * step + 1e-5f * (float)std::fabs(want))
                        ok = false;
                }
            }
            char detail[192];
            std::snprintf(detail, sizeof(detail), "z=%g got=%.7g ref=%.7g step=%.3g err_ref=%.3g err_truth=%.3g",
                          want, got0, ref, step, worstRef, worstTruth);
            std::snprintf(name, sizeof(name), "FLOATZ_COLUMN%d%s", c, suffix);
            Check(ok, name, detail);
        }
        readback->UnlockRect();
    }
    g_device->SetDepthStencilSurface(nullptr);
    for (IUnknown *object : {(IUnknown *)depth, (IUnknown *)floatSurface, (IUnknown *)floatTex, (IUnknown *)readback})
        object->Release();
    Deko9_SetRtCompression(true);
}

// Off-screen soft particles:
// Deko9_ParticleDepth and Deko9_ParticleComposite against the CPU
// reference (deko9_particles_reference.h). A 64x64 scene of 3x3 blocks at
// two depths (0.3 near, 0.9 far; float-Z 30 / 900) gets six layers of
// full-screen particles (over, additive, srcalpha-additive; some behind the
// near blocks):
//   HRP_DEPTH_F2/_RECT: the off-screen float-Z equals DownsamplePick's
//     choice (nearest of each 2x2 footprint) for the whole view and for a
//     view rectangle with an origin;
//   HRP_SCALE1_DIRECT: factor 1 (off-screen at full size): layers drawn
//     off-screen with the remapped blend, then composited, match the same
//     layers drawn directly into the scene within UNORM8 rounding (the
//     plumbing proof, independent of the resolution trade-off);
//   HRP_HALF_REF_BILINEAR / _NEAREST: factor 2, both upsample modes, match
//     the CPU reference of depth test + accumulation + composite.
namespace hrpst
{
constexpr UINT kN = kSize;
struct Layer
{
    float r, g, b, a, z;
    deko9::hrpref::Blend blend;
};
const Layer kLayers[] = {
    {0.9f, 0.6f, 0.3f, 0.35f, 0.20f, deko9::hrpref::Blend::Over},
    {0.2f, 0.4f, 0.8f, 0.03f, 0.50f, deko9::hrpref::Blend::Over}, // faint, behind the near blocks
    {0.10f, 0.05f, 0.02f, 0.5f, 0.25f, deko9::hrpref::Blend::Add},
    {0.5f, 0.9f, 0.1f, 0.6f, 0.95f, deko9::hrpref::Blend::Over},  // behind everything
    {0.4f, 0.3f, 0.2f, 0.25f, 0.40f, deko9::hrpref::Blend::AddAlpha},
    {0.7f, 0.7f, 0.7f, 0.02f, 0.10f, deko9::hrpref::Blend::Over},
};
// Exact in UNORM8 (D3DCOLOR_COLORVALUE truncates).
const float kBase[4] = {0.2f, 0.6f, 0.8f, 1.0f};

bool Near(int x, int y) { return ((x / 3) + (y / 3)) % 2 == 0; }

void SetBlend(deko9::hrpref::Blend b, bool offscreen)
{
    const DWORD src = b == deko9::hrpref::Blend::Add ? D3DBLEND_ONE : D3DBLEND_SRCALPHA;
    const DWORD dst = b == deko9::hrpref::Blend::Over ? D3DBLEND_INVSRCALPHA : D3DBLEND_ONE;
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g_device->SetRenderState(D3DRS_SRCBLEND, src);
    g_device->SetRenderState(D3DRS_DESTBLEND, dst);
    // The engine's remap (r_halfres_particles_rules.h RemapStateBits0).
    g_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, offscreen ? TRUE : FALSE);
    g_device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
    g_device->SetRenderState(D3DRS_DESTBLENDALPHA,
                             b == deko9::hrpref::Blend::Over ? D3DBLEND_INVSRCALPHA : D3DBLEND_ONE);
    g_device->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    g_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
}

void DrawLayers(bool offscreen, UINT w, UINT h)
{
    D3DVIEWPORT9 vp{0, 0, w, h, 0.0f, 1.0f};
    g_device->SetViewport(&vp);
    g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    for (const Layer &l : kLayers)
    {
        SetBlend(l.blend, offscreen);
        SetColor(l.r, l.g, l.b, l.a);
        DrawQuad(-1.0f, -1.0f, 1.0f, 1.0f, l.z);
    }
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    D3DVIEWPORT9 full{0, 0, kN, kN, 0.0f, 1.0f};
    g_device->SetViewport(&full);
}

bool ReadColor(IDirect3DSurface9 *surface, IDirect3DSurface9 *sys, std::vector<deko9::hrpref::Rgba> *out)
{
    D3DLOCKED_RECT locked;
    if (FAILED(g_device->GetRenderTargetData(surface, sys)) || FAILED(sys->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        return false;
    out->resize(kN * kN);
    for (UINT y = 0; y < kN; ++y)
        for (UINT x = 0; x < kN; ++x)
        {
            const uint8_t *p = static_cast<const uint8_t *>(locked.pBits) + y * locked.Pitch + x * 4; // BGRA
            (*out)[y * kN + x] = {p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f, p[3] / 255.0f};
        }
    sys->UnlockRect();
    return true;
}

bool ReadFloat(IDirect3DSurface9 *surface, IDirect3DSurface9 *sys, std::vector<float> *out)
{
    D3DLOCKED_RECT locked;
    if (FAILED(g_device->GetRenderTargetData(surface, sys)) || FAILED(sys->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        return false;
    out->resize(kN * kN);
    for (UINT y = 0; y < kN; ++y)
        std::memcpy(out->data() + y * kN, static_cast<const uint8_t *>(locked.pBits) + y * locked.Pitch, kN * 4);
    sys->UnlockRect();
    return true;
}

// The engine's order (rb_halfres_particles.cpp): the off-screen depth is
// cleared through the ordinary Clear before the depth pass writes it.
void ClearHrDepth(IDirect3DSurface9 *color, IDirect3DSurface9 *depthSurface)
{
    g_device->SetRenderTarget(0, color);
    g_device->SetDepthStencilSurface(depthSurface);
    D3DVIEWPORT9 vp{0, 0, kN, kN, 0.0f, 1.0f};
    g_device->SetViewport(&vp);
    g_device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
}

int MaxLsb(const std::vector<deko9::hrpref::Rgba> &a, const std::vector<deko9::hrpref::Rgba> &b, int *where)
{
    int worst = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        for (float d : {a[i].r - b[i].r, a[i].g - b[i].g, a[i].b - b[i].b})
        {
            const int lsb = (int)std::lround(std::fabs(d) * 255.0f);
            if (lsb > worst)
            {
                worst = lsb;
                *where = (int)i;
            }
        }
    }
    return worst;
}
} // namespace hrpst

void RunHrpTests()
{
    using namespace hrpst;
    using deko9::hrpref::Rgba;
    IDirect3DSurface9 *depth = nullptr, *hrDepth = nullptr, *fullZSurf = nullptr, *hrZSurf = nullptr,
                      *hrColorSurf = nullptr, *sceneASurf = nullptr, *sceneBSurf = nullptr, *sysColor = nullptr,
                      *sysFloat = nullptr;
    IDirect3DTexture9 *fullZ = nullptr, *hrZ = nullptr, *hrColor = nullptr, *sceneA = nullptr, *sceneB = nullptr;
    const auto rt = [](D3DFORMAT format, IDirect3DTexture9 **tex, IDirect3DSurface9 **surf) {
        return SUCCEEDED(g_device->CreateTexture(kN, kN, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, tex,
                                                 nullptr)) &&
               SUCCEEDED((*tex)->GetSurfaceLevel(0, surf));
    };
    // The scene side is compressed as in the game (r_deko9RtCompression 1);
    // the off-screen targets are created the way rb_halfres_particles.cpp
    // creates them: uncompressed (DEKO9_HRP_RULE_UNCOMPRESSED).
    const auto offscreen = [&]() {
        Deko9_SetRtCompressionOverride(0);
        const bool ok = SUCCEEDED(g_device->CreateDepthStencilSurface(kN, kN, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0,
                                                                      FALSE, &hrDepth, nullptr)) &&
                        rt(D3DFMT_R32F, &hrZ, &hrZSurf) && rt(D3DFMT_A8R8G8B8, &hrColor, &hrColorSurf);
        Deko9_SetRtCompressionOverride(-1);
        return ok;
    };
    const bool setup =
        SUCCEEDED(g_device->CreateDepthStencilSurface(kN, kN, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth,
                                                      nullptr)) &&
        rt(D3DFMT_R32F, &fullZ, &fullZSurf) && offscreen() && rt(D3DFMT_A8R8G8B8, &sceneA, &sceneASurf) &&
        rt(D3DFMT_A8R8G8B8, &sceneB, &sceneBSurf) &&
        SUCCEEDED(g_device->CreateOffscreenPlainSurface(kN, kN, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysColor,
                                                        nullptr)) &&
        SUCCEEDED(g_device->CreateOffscreenPlainSurface(kN, kN, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &sysFloat, nullptr));
    Check(setup, "HRP_SETUP");
    if (!setup)
        return;
    Deko9_SetParticleRules(g_device, DEKO9_HRP_RULES_ALL);
    // Hardware rule: the off-screen images are uncompressed; the scene's are
    // compressed as in the game.
    Check(!Deko9_IsCompressed(hrDepth) && !Deko9_IsCompressed(hrZ) && !Deko9_IsCompressed(hrColor) &&
              Deko9_IsCompressed(depth) && Deko9_IsCompressed(fullZ),
          "HRP_TARGETS_UNCOMPRESSED");

    // Scene: depth blocks into `depth`, their float-Z into fullZ.
    std::vector<float> cpuDepth(kN * kN), cpuZ(kN * kN);
    g_device->SetVertexShader(g_vs);
    g_device->SetVertexDeclaration(g_decl);
    g_device->SetPixelShader(g_psColor);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    g_device->SetRenderTarget(0, fullZSurf);
    g_device->SetDepthStencilSurface(depth);
    g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    for (int by = 0; by * 3 < (int)kN; ++by)
        for (int bx = 0; bx * 3 < (int)kN; ++bx)
        {
            const bool nearBlock = Near(bx * 3, by * 3);
            const float d = nearBlock ? 0.3f : 0.9f, z = nearBlock ? 30.0f : 900.0f;
            SetColor(z, 0.0f, 0.0f, 1.0f);
            const float x0 = -1.0f + 2.0f * (bx * 3) / kN, x1 = -1.0f + 2.0f * std::min<int>(bx * 3 + 3, kN) / kN;
            const float y0 = 1.0f - 2.0f * (by * 3) / kN, y1 = 1.0f - 2.0f * std::min<int>(by * 3 + 3, kN) / kN;
            DrawQuad(x0, y0, x1, y1, d);
        }
    g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    // The CPU model takes each pixel's block from the float-Z readback (the
    // rasteriser owns the block edges); the depth is the block's.
    bool twoValues = ReadFloat(fullZSurf, sysFloat, &cpuZ);
    int nearCount = 0;
    for (UINT i = 0; twoValues && i < kN * kN; ++i)
    {
        twoValues = cpuZ[i] == 30.0f || cpuZ[i] == 900.0f;
        cpuDepth[i] = cpuZ[i] == 30.0f ? 0.3f : 0.9f;
        nearCount += cpuZ[i] == 30.0f;
    }
    char sceneDetail[64];
    std::snprintf(sceneDetail, sizeof(sceneDetail), "near_pixels=%d", nearCount);
    Check(twoValues && nearCount > 1500 && nearCount < 2600, "HRP_SCENE_FLOATZ", sceneDetail);

    // Off-screen depth at factor 2: whole view and a rectangle with an origin.
    const int32_t rects[2][4] = {{0, 0, (int32_t)kN, (int32_t)kN}, {3, 5, 50, 40}};
    const char *depthNames[2] = {"HRP_DEPTH_F2", "HRP_DEPTH_F2_RECT"};
    for (int r = 0; r < 2; ++r)
    {
        ClearHrDepth(hrZSurf, hrDepth);
        const bool ran = Deko9_ParticleDepth(g_device, depth, fullZ, rects[r], 2, hrDepth, hrZSurf);
        std::vector<float> half;
        bool ok = ran && ReadFloat(hrZSurf, sysFloat, &half);
        char detail[128] = "";
        const int ow = (rects[r][2] + 1) / 2, oh = (rects[r][3] + 1) / 2;
        for (int y = 0; ok && y < oh; ++y)
            for (int x = 0; ok && x < ow; ++x)
            {
                int px, py;
                deko9::hrpref::DownsamplePick(cpuDepth.data(), kN, rects[r], 2, x, y, &px, &py);
                if (half[y * kN + x] != cpuZ[py * kN + px])
                {
                    std::snprintf(detail, sizeof(detail), "(%d,%d)=%g want %g", x, y, half[y * kN + x],
                                  cpuZ[py * kN + px]);
                    ok = false;
                }
            }
        Check(ok, depthNames[r], detail);
    }

    // Direct reference: layers into sceneA over the scene depth.
    g_device->SetRenderTarget(0, sceneASurf);
    g_device->SetDepthStencilSurface(depth);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(kBase[0], kBase[1], kBase[2], kBase[3]), 1.0f, 0);
    DrawLayers(false, kN, kN);
    std::vector<Rgba> direct;
    Check(ReadColor(sceneASurf, sysColor, &direct), "HRP_DIRECT_READ");
    std::vector<Rgba> halfNearest; // factor 2, nearest-depth upsample: HRP_FULL_SYNC's reference

    for (int factor = 1; factor <= 2; ++factor)
    {
        const int32_t rect[4] = {0, 0, (int32_t)kN, (int32_t)kN};
        const int ow = (int)(kN + factor - 1) / factor, oh = ow;
        // Garbage in the whole off-screen colour first: the run's clear must
        // really reach every covered texel (a skipped or misdirected clear
        // fails the reference below).
        g_device->SetRenderTarget(0, hrColorSurf);
        g_device->SetDepthStencilSurface(nullptr);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 200, 30, 90), 1.0f, 0);
        ClearHrDepth(hrZSurf, hrDepth);
        const bool depthOk = Deko9_ParticleDepth(g_device, depth, fullZ, rect, (uint32_t)factor, hrDepth, hrZSurf);
        // No readback between the depth pass, the off-screen draws and the
        // first composite (as in the game): a readback waits for the GPU and
        // records a full barrier, which would hide a missing one on hardware.
        std::vector<float> halfZ;
        bool zRead = false;
        // Off-screen accumulation.
        g_device->SetRenderTarget(0, hrColorSurf);
        g_device->SetDepthStencilSurface(hrDepth);
        D3DVIEWPORT9 vp{0, 0, (DWORD)ow, (DWORD)oh, 0.0f, 1.0f};
        g_device->SetViewport(&vp);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
        DrawLayers(true, (UINT)ow, (UINT)oh);
        for (int mode = factor == 1 ? 1 : 0; mode <= 1; ++mode)
        {
            g_device->SetRenderTarget(0, sceneBSurf);
            g_device->SetDepthStencilSurface(nullptr);
            g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(kBase[0], kBase[1], kBase[2], kBase[3]),
                            1.0f, 0);
            deko9::hrpref::CompositeConstants c;
            const int origin[2] = {0, 0}, view[2] = {(int)kN, (int)kN}, off[2] = {ow, oh};
            deko9::hrpref::CompositeSetup(&c, origin, view, off, mode, 0.1f);
            const int32_t dstRect[4] = {0, 0, (int32_t)kN, (int32_t)kN};
            const bool composed = Deko9_ParticleComposite(g_device, hrColor, hrZ, fullZ, sceneBSurf, dstRect, &c);
            std::vector<Rgba> got;
            const bool gotRead = composed && ReadColor(sceneBSurf, sysColor, &got);
            if (!zRead)
                zRead = depthOk && ReadFloat(hrZSurf, sysFloat, &halfZ);
            const bool read = gotRead && zRead;
            char detail[192] = "";
            if (factor == 1)
            {
                int where = 0;
                const int lsb = read ? MaxLsb(got, direct, &where) : 99;
                std::snprintf(detail, sizeof(detail), "max_lsb=%d at (%d,%d)", lsb, where % (int)kN, where / (int)kN);
                // SCHEME_UNORM8 bound (switch_halfres_particles_test).
                Check(read && lsb <= 4, "HRP_SCALE1_DIRECT", detail);
                continue;
            }
            // CPU reference of the half path: depth test against the
            // off-screen depth (the chosen texel's), accumulate, composite.
            std::vector<Rgba> acc(kN * kN, Rgba{0, 0, 0, 1});
            for (int y = 0; y < oh; ++y)
                for (int x = 0; x < ow; ++x)
                {
                    int px, py;
                    deko9::hrpref::DownsamplePick(cpuDepth.data(), kN, rect, 2, x, y, &px, &py);
                    for (const Layer &l : kLayers)
                    {
                        if (l.z <= cpuDepth[py * kN + px])
                            acc[y * kN + x] = deko9::hrpref::BlendOffscreen(acc[y * kN + x], {l.r, l.g, l.b, l.a},
                                                                           l.blend, true);
                    }
                }
            std::vector<Rgba> want(kN * kN);
            const Rgba base{kBase[0], kBase[1], kBase[2], kBase[3]};
            for (UINT y = 0; y < kN; ++y)
                for (UINT x = 0; x < kN; ++x)
                {
                    const Rgba s = deko9::hrpref::CompositeSample(c, acc.data(), halfZ.data(), kN, cpuZ[y * kN + x],
                                                                  (int)x, (int)y);
                    want[y * kN + x] = deko9::hrpref::Composite(deko9::hrpref::Unorm8(base), s, true);
                }
            int where = 0;
            const int lsb = read ? MaxLsb(got, want, &where) : 99;
            int directWhere = 0;
            const int vsDirect = read ? MaxLsb(got, direct, &directWhere) : 99;
            std::snprintf(detail, sizeof(detail), "max_lsb=%d at (%d,%d) vs_direct_max_lsb=%d", lsb, where % (int)kN,
                          where / (int)kN, vsDirect);
            Check(read && lsb <= 4, mode ? "HRP_HALF_REF_NEAREST" : "HRP_HALF_REF_BILINEAR", detail);
            if (mode == 1 && read)
                halfNearest = got;
        }
    }
    uint64_t depthPasses = 0, composites = 0;
    Deko9_GetParticleCounts(g_device, &depthPasses, &composites);
    char detail[64];
    std::snprintf(detail, sizeof(detail), "depth=%llu composite=%llu", (unsigned long long)depthPasses,
                  (unsigned long long)composites);
    Check(depthPasses == 4 && composites == 3, "HRP_COUNTS", detail);
    // Hardware rule DEKO9_HRP_RULE_ZCULL: every depth pass (gl_FragDepth
    // writes zcull cannot track) invalidated zcull after its draw. Some emulators
    // do not emulate zcull, so this is asserted on the recorded commands.
    const uint64_t zcull = Deko9_GetParticleZcullInvalidates(g_device);
    std::snprintf(detail, sizeof(detail), "zcull_invalidates=%llu depth=%llu", (unsigned long long)zcull,
                  (unsigned long long)depthPasses);
    Check(zcull == depthPasses, "HRP_ZCULL_INVALIDATE", detail);
    // Hardware rule DEKO9_HRP_RULE_UNCOMPRESSED: compressed off-screen images
    // are refused by both passes (expected FAIL:DEKO9_HRP_DEPTH and
    // FAIL:DEKO9_HRP_COMPOSITE lines).
    {
        IDirect3DSurface9 *cDepth = nullptr, *cColorSurf = nullptr;
        IDirect3DTexture9 *cColor = nullptr;
        Deko9_SetRtCompressionOverride(1);
        const bool made = SUCCEEDED(g_device->CreateDepthStencilSurface(kN, kN, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0,
                                                                        FALSE, &cDepth, nullptr)) &&
                          rt(D3DFMT_A8R8G8B8, &cColor, &cColorSurf);
        Deko9_SetRtCompressionOverride(-1);
        const int32_t rect[4] = {0, 0, (int32_t)kN, (int32_t)kN};
        deko9::hrpref::CompositeConstants c;
        const int origin[2] = {0, 0}, view[2] = {(int)kN, (int)kN}, off[2] = {(int)kN / 2, (int)kN / 2};
        deko9::hrpref::CompositeSetup(&c, origin, view, off, 1, 0.1f);
        const bool refusedDepth =
            made && Deko9_IsCompressed(cDepth) && !Deko9_ParticleDepth(g_device, depth, fullZ, rect, 2, cDepth, hrZSurf);
        const bool refusedComposite =
            made && !Deko9_ParticleComposite(g_device, cColor, hrZ, fullZ, sceneBSurf, rect, &c);
        uint64_t d2 = 0, c2 = 0;
        Deko9_GetParticleCounts(g_device, &d2, &c2);
        std::snprintf(detail, sizeof(detail), "depth=%d composite=%d passes_unchanged=%d", refusedDepth ? 1 : 0,
                      refusedComposite ? 1 : 0, d2 == depthPasses && c2 == composites ? 1 : 0);
        Check(refusedDepth && refusedComposite && d2 == depthPasses && c2 == composites, "HRP_REFUSES_COMPRESSED",
              detail);
        for (IUnknown *object : {(IUnknown *)cDepth, (IUnknown *)cColorSurf, (IUnknown *)cColor})
        {
            if (object)
                object->Release();
        }
    }
    // r_halfResParticlesDebug 16: full barriers around a composite record
    // one barrier each and leave its pixels exactly as without them.
    {
        g_device->SetRenderTarget(0, sceneBSurf);
        g_device->SetDepthStencilSurface(nullptr);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(kBase[0], kBase[1], kBase[2], kBase[3]), 1.0f,
                        0);
        deko9::hrpref::CompositeConstants c;
        const int origin[2] = {0, 0}, view[2] = {(int)kN, (int)kN}, off[2] = {(int)kN / 2, (int)kN / 2};
        deko9::hrpref::CompositeSetup(&c, origin, view, off, 1, 0.1f);
        const int32_t dstRect[4] = {0, 0, (int32_t)kN, (int32_t)kN};
        const uint64_t before = Deko9_GetParticleFullBarriers(g_device);
        Deko9_ParticleFullBarrier(g_device);
        const bool composed = Deko9_ParticleComposite(g_device, hrColor, hrZ, fullZ, sceneBSurf, dstRect, &c);
        Deko9_ParticleFullBarrier(g_device);
        const uint64_t recorded = Deko9_GetParticleFullBarriers(g_device) - before;
        std::vector<Rgba> got;
        const bool read = composed && ReadColor(sceneBSurf, sysColor, &got);
        int where = 0;
        const int lsb = read && halfNearest.size() == got.size() ? MaxLsb(got, halfNearest, &where) : 99;
        std::snprintf(detail, sizeof(detail), "barriers=%llu max_lsb=%d", (unsigned long long)recorded, lsb);
        Check(recorded == 2 && lsb == 0, "HRP_FULL_SYNC", detail);
    }
    g_device->SetRenderTarget(0, g_targetSurface);
    g_device->SetDepthStencilSurface(nullptr);
    for (IUnknown *object : {(IUnknown *)depth, (IUnknown *)hrDepth, (IUnknown *)fullZSurf, (IUnknown *)hrZSurf,
                             (IUnknown *)hrColorSurf, (IUnknown *)sceneASurf, (IUnknown *)sceneBSurf,
                             (IUnknown *)sysColor, (IUnknown *)sysFloat, (IUnknown *)fullZ, (IUnknown *)hrZ,
                             (IUnknown *)hrColor, (IUnknown *)sceneA, (IUnknown *)sceneB})
        object->Release();
}

// D3D9 dirty-region tracking (AddDirtyBox/AddDirtyRect, and a Lock without
// D3DLOCK_NO_DIRTY_UPDATE) plus UpdateTexture copying only the accumulated
// dirty region, not the whole subresource -- the model-lighting volume's
// real pattern (a SYSTEMMEM volume patched a few small boxes at a time, then
// UpdateTexture'd to a DEFAULT volume every frame; see r_model_lighting.cpp
// and deko9_draw.cpp Device::UpdateTexture). Read via Deko9_GetCounters'
// updateTextureCalls/updateTextureBytes (r_deko9Census Census_UpdateTexture,
// only accumulated while the census is on) and, for values, a LockBox/
// LockRect readback of the D3DPOOL_DEFAULT destination: it carries no
// DYNAMIC usage, so a lock goes through LockStore's staging+ReadImage path
// and genuinely samples the GPU image UpdateTexture wrote, not a stale CPU
// shadow.
// Early fragment tests (Deko9_SetEarlyZ, r_deko9EarlyZ): a discarding or
// alpha-tested draw that tests depth but writes neither depth nor stencil
// binds the pixel shader's early-Z variant; the pixels must be identical
// with the variant on and off, and draws that write depth or stencil, run
// inside an occlusion query, or whose shader writes depth must never get it
// (checked through the device's early-Z draw counters).
void RunEarlyZTests()
{
    IDirect3DPixelShader9 *psKill = nullptr, *psKillDepth = nullptr;
    IDirect3DSurface9 *depth = nullptr;
    IDirect3DQuery9 *occlusion = nullptr;
    bool ok = SUCCEEDED(g_device->CreatePixelShader(kPsKill, &psKill)) &&
              SUCCEEDED(g_device->CreatePixelShader(kPsKillDepth, &psKillDepth)) &&
              SUCCEEDED(g_device->CreateDepthStencilSurface(kSize, kSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE,
                                                            &depth, nullptr)) &&
              SUCCEEDED(g_device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &occlusion));
    Check(ok, "EARLYZ_SETUP");
    if (!ok)
        return;
    const uint32_t black = 0xff000000, green = 0xff00ff00, blue = 0xff0000ff, red = 0xffff0000;
    // Quad whose texcoord u runs from u0 (left) to u1 (right): texkill
    // discards where u < 0.
    const auto quadU = [](float x0, float y0, float x1, float y1, float z, float u0, float u1) {
        const Vertex v[6] = {
            {x0, y0, z, 1, u0, 0}, {x1, y0, z, 1, u1, 0}, {x0, y1, z, 1, u0, 1},
            {x1, y0, z, 1, u1, 0}, {x1, y1, z, 1, u1, 1}, {x0, y1, z, 1, u0, 1},
        };
        g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(Vertex));
    };
    const auto resetStates = [] {
        g_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        g_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    };
    struct Result
    {
        uint32_t px[kSize * kSize];
        uint64_t ezDraws, ezCandidates;
    };
    // Blended scene: an opaque green quad (depth write on) covers the top
    // half at z 0.3; then, depth write off: (a) a blended texkill quad over
    // the whole target at z 0.6 whose left half discards, (b) an
    // alpha-tested quad (alpha 0.6 > ref 0.5 passes) over the bottom-left
    // quarter at z 0.5, (c) one whose alpha 0.4 fails everywhere at z 0.1.
    // The top half stays green (depth-rejected), the bottom right is half
    // red over black, the bottom left blue.
    const auto blendedScene = [&](bool earlyZ, Result *r) {
        Deko9_SetEarlyZ(g_device, earlyZ);
        BeginPass();
        g_device->SetDepthStencilSurface(depth);
        g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        resetStates();
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, black, 1.0f, 0);
        g_device->SetPixelShader(g_psColor);
        SetColor(0, 1, 0, 1);
        DrawQuad(-1, 1, 1, 0, 0.3f);
        Deko9Counters before{}, after{};
        Deko9_GetCounters(g_device, &before);
        g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_device->SetPixelShader(psKill);
        SetColor(1, 0, 0, 0.5f);
        quadU(-1, 1, 1, -1, 0.6f, -1, 1); // (a)
        g_device->SetPixelShader(g_psColor);
        g_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        g_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        g_device->SetRenderState(D3DRS_ALPHAREF, 128);
        g_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        g_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
        SetColor(0, 0, 1, 0.6f);
        DrawQuad(-1, 0, 0, -1, 0.5f); // (b)
        SetColor(1, 1, 1, 0.4f);
        DrawQuad(-1, 1, 1, -1, 0.1f); // (c)
        Deko9_GetCounters(g_device, &after);
        r->ezDraws = after.earlyZDraws - before.earlyZDraws;
        r->ezCandidates = after.earlyZCandidates - before.earlyZCandidates;
        resetStates();
        Read(r->px);
    };
    static Result late, early;
    blendedScene(false, &late);
    blendedScene(true, &early);
    char detail[160];
    uint32_t diff = 0;
    for (uint32_t i = 0; i < kSize * kSize; ++i)
        diff += late.px[i] != early.px[i];
    std::snprintf(detail, sizeof(detail), "differing=%u of %u", (unsigned)diff, (unsigned)(kSize * kSize));
    Check(diff == 0, "EARLYZ_BLEND_PIXELS_IDENTICAL", detail);
    // Bottom right: red 0.5 over black (alpha 0.5 * 0.5 + 1 * 0.5 = 0.75);
    // bottom left: (b) replaces with blue, alpha 0.6.
    ExpectPixels("EARLYZ_BLEND_SCENE", early.px,
                 {{P(10, 10), green}, {P(50, 10), green}, {P(10, 50), 0x990000ffu}, {P(50, 50), 0xbf800000u},
                  {P(40, 40), 0xbf800000u}});
    std::snprintf(detail, sizeof(detail), "off: ez=%llu cand=%llu on: ez=%llu cand=%llu",
                  (unsigned long long)late.ezDraws, (unsigned long long)late.ezCandidates,
                  (unsigned long long)early.ezDraws, (unsigned long long)early.ezCandidates);
    Check(late.ezDraws == 0 && late.ezCandidates == 3 && early.ezDraws == 3 && early.ezCandidates == 3,
          "EARLYZ_VARIANT_CHOSEN", detail);

    // Draws that must stay late: a texkill quad at z 0.2 with depth write on
    // (left half killed: those fragments must not write depth), an
    // alpha-test quad with depth write on, a depth-writing shader, a stencil
    // REPLACE draw and a draw inside an occlusion query (depth write off).
    // A blue quad at z 0.4 then shows through on the left only.
    const auto writesScene = [&](bool earlyZ, Result *r) {
        Deko9_SetEarlyZ(g_device, earlyZ);
        BeginPass();
        g_device->SetDepthStencilSurface(depth);
        g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        resetStates();
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, black, 1.0f, 0);
        Deko9Counters before{}, after{};
        Deko9_GetCounters(g_device, &before);
        g_device->SetPixelShader(psKill);
        SetColor(1, 0, 0, 1);
        quadU(-1, 1, 1, -1, 0.2f, -1, 1);
        g_device->SetPixelShader(g_psColor);
        g_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        g_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        g_device->SetRenderState(D3DRS_ALPHAREF, 128);
        SetColor(1, 1, 1, 0.4f);
        DrawQuad(-1, 1, 1, -1, 0.1f);
        g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        g_device->SetPixelShader(psKillDepth);
        const float c1[4] = {0.5f, 0, 0, 0};
        g_device->SetPixelShaderConstantF(1, c1, 1);
        SetColor(1, 1, 0, 1);
        quadU(-1, 1, 1, -1, 0.9f, -1, 1);
        g_device->SetPixelShader(psKill);
        g_device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        g_device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
        g_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
        g_device->SetRenderState(D3DRS_STENCILREF, 1);
        quadU(-1, 1, 1, -1, 0.9f, -1, 1);
        g_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        g_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
        occlusion->Issue(D3DISSUE_BEGIN);
        quadU(-1, 1, 1, -1, 0.9f, -1, 1);
        occlusion->Issue(D3DISSUE_END);
        g_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        g_device->SetPixelShader(g_psColor);
        SetColor(0, 0, 1, 1);
        DrawQuad(-1, 1, 1, -1, 0.4f);
        Deko9_GetCounters(g_device, &after);
        r->ezDraws = after.earlyZDraws - before.earlyZDraws;
        r->ezCandidates = after.earlyZCandidates - before.earlyZCandidates;
        resetStates();
        Read(r->px);
    };
    writesScene(false, &late);
    writesScene(true, &early);
    diff = 0;
    for (uint32_t i = 0; i < kSize * kSize; ++i)
        diff += late.px[i] != early.px[i];
    std::snprintf(detail, sizeof(detail), "differing=%u of %u", (unsigned)diff, (unsigned)(kSize * kSize));
    Check(diff == 0, "EARLYZ_WRITES_PIXELS_IDENTICAL", detail);
    ExpectPixels("EARLYZ_WRITES_SCENE", early.px, {{P(10, 32), blue}, {P(50, 32), red}});
    std::snprintf(detail, sizeof(detail), "off: ez=%llu cand=%llu on: ez=%llu cand=%llu",
                  (unsigned long long)late.ezDraws, (unsigned long long)late.ezCandidates,
                  (unsigned long long)early.ezDraws, (unsigned long long)early.ezCandidates);
    Check(late.ezDraws == 0 && early.ezDraws == 0 && late.ezCandidates == 0 && early.ezCandidates == 0,
          "EARLYZ_NOT_CHOSEN_FOR_WRITES", detail);
    DWORD samples = 0;
    for (int i = 0; i < 100000 && occlusion->GetData(&samples, sizeof(samples), D3DGETDATA_FLUSH) == S_FALSE; ++i)
        Deko9_WaitQuery(occlusion, 1000000);
    Deko9_SetEarlyZ(g_device, true);
    BeginPass();
}

// Per-draw fast paths (Deko9_SetPerDraw: hazard-evaluation skip, constant
// any-dirty flag, incremental texture-slot resolution). One scene, drawn
// with all of them off and all on under Deko9_SetVerify (every draw
// re-derived: 0 mismatches), must give identical pixels and the expected
// colours; with them on, draws with unchanged bindings skip hazard
// evaluation and a texture change re-resolves only its slot. The scene has
// the hazards the skip must not hide: sampling a texture just rendered
// (target change), and a texture re-uploaded between two draws whose
// bindings are otherwise identical (copy-engine write, no rebind).
void RunPerDrawTests()
{
    const uint32_t black = 0xff000000, red = 0xffff0000, green = 0xff00ff00, blue = 0xff0000ff;
    const uint32_t yellow = 0xffffff00, magenta = 0xffff00ff, cyan = 0xff00ffff;
    const uint32_t kAllPerDraw = DEKO9_PERDRAW_HAZARD | DEKO9_PERDRAW_CONSTS | DEKO9_PERDRAW_TEXTURES;
    IDirect3DTexture9 *texA = nullptr, *texB = nullptr, *rt = nullptr;
    IDirect3DSurface9 *rtSurface = nullptr;
    IDirect3DVertexBuffer9 *vb = nullptr;
    bool ok = SUCCEEDED(g_device->CreateVertexBuffer(12 * sizeof(Vertex), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb,
                                                     nullptr)) &&
              SUCCEEDED(g_device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texA, nullptr)) &&
              SUCCEEDED(g_device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texB, nullptr)) &&
              SUCCEEDED(g_device->CreateTexture(kSize, kSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                D3DPOOL_DEFAULT, &rt, nullptr)) &&
              SUCCEEDED(rt->GetSurfaceLevel(0, &rtSurface));
    // Two quads in one vertex buffer: x 0.25..0.5 and 0.5..0.75, y -0.5..-0.75.
    void *vbData = nullptr;
    if (ok && SUCCEEDED(vb->Lock(0, 0, &vbData, 0)))
    {
        Vertex *v = static_cast<Vertex *>(vbData);
        for (int q = 0; q < 2; ++q)
        {
            const float x0 = 0.25f + 0.25f * q, x1 = x0 + 0.25f, y0 = -0.5f, y1 = -0.75f;
            const Vertex quad[6] = {
                {x0, y0, 0.5f, 1, 0, 0}, {x1, y0, 0.5f, 1, 1, 0}, {x0, y1, 0.5f, 1, 0, 1},
                {x1, y0, 0.5f, 1, 1, 0}, {x1, y1, 0.5f, 1, 1, 1}, {x0, y1, 0.5f, 1, 0, 1},
            };
            std::memcpy(v + 6 * q, quad, sizeof(quad));
        }
        vb->Unlock();
    }
    else
        ok = false;
    Check(ok, "PERDRAW_SETUP");
    if (!ok)
        return;
    // DISCARD: no read-back (whose wait for the GPU would hide a missing
    // barrier between the draws before the re-upload and the upload).
    const DWORD fillFlags = D3DLOCK_DISCARD;
    const auto fill = [fillFlags](IDirect3DTexture9 *texture, uint32_t color) {
        D3DLOCKED_RECT locked;
        if (FAILED(texture->LockRect(0, &locked, nullptr, fillFlags)))
            return false;
        for (int y = 0; y < 2; ++y)
        {
            uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch);
            row[0] = row[1] = color;
        }
        return SUCCEEDED(texture->UnlockRect(0));
    };
    struct Result
    {
        uint32_t px[kSize * kSize];
        uint64_t hazardSkips, verified, mismatches;
    };
    uint32_t probeFlags = 0, probeSplit = 1;
    const auto scene = [&](uint32_t flags, Result *r) {
        Deko9_SetPerDraw(g_device, flags);
        Deko9_SetDrawProbe(g_device, probeFlags, probeSplit);
        Deko9_SetVerify(g_device, true);
        fill(texA, red);
        fill(texB, green);
        Deko9Counters before{}, after{};
        Deko9_GetCounters(g_device, &before);
        // 1. Render into rt: left half texA, right half texB (slot change),
        //    then the left quarter again with identical bindings.
        BeginPass();
        g_device->SetRenderTarget(0, rtSurface);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
        g_device->SetPixelShader(g_psTexture);
        g_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        g_device->SetTexture(0, texA);
        DrawQuad(-1, 1, 0, -1);
        g_device->SetTexture(0, texB);
        DrawQuad(0, 1, 1, -1);
        g_device->SetTexture(0, texA);
        DrawQuad(-1, 1, -0.5f, -1);
        // 2. Main target: sample rt (render -> sample across the target
        //    change) in the top band, twice with identical bindings; a
        //    sampler-state change in between re-resolves the slot.
        BeginPass();
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
        g_device->SetPixelShader(g_psTexture);
        g_device->SetTexture(0, rt);
        const Vertex left[6] = {
            {-1, 1, 0.5f, 1, 0, 0}, {0, 1, 0.5f, 1, 0.5f, 0}, {-1, 0.5f, 0.5f, 1, 0, 1},
            {0, 1, 0.5f, 1, 0.5f, 0}, {0, 0.5f, 0.5f, 1, 0.5f, 1}, {-1, 0.5f, 0.5f, 1, 0, 1},
        };
        const Vertex right[6] = {
            {0, 1, 0.5f, 1, 0.5f, 0}, {1, 1, 0.5f, 1, 1, 0}, {0, 0.5f, 0.5f, 1, 0.5f, 1},
            {1, 1, 0.5f, 1, 1, 0}, {1, 0.5f, 0.5f, 1, 1, 1}, {0, 0.5f, 0.5f, 1, 0.5f, 1},
        };
        g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, left, sizeof(Vertex));
        g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, right, sizeof(Vertex));
        g_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        DrawQuad(-1, 0.5f, 1, 0);
        g_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        // 3. texA in the third band (two draws); re-upload it blue
        //    (copy-engine write, no rebind) and draw the bottom band with
        //    identical bindings.
        g_device->SetTexture(0, texA);
        DrawQuad(-1, 0, 0, -0.5f);
        DrawQuad(0, 0, 1, -0.5f);
        fill(texA, blue);
        DrawQuad(-1, -0.5f, 1, -1);
        // 4. Constants between draws with unchanged bindings: small quads.
        g_device->SetPixelShader(g_psColor);
        SetColor(1, 1, 0, 1);
        DrawQuad(-1, -0.75f, -0.75f, -1);
        SetColor(0, 1, 0, 1);
        DrawQuad(0.75f, -0.75f, 1, -1);
        // 5. Vertex-buffer draws (stream path, constants between them).
        g_device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        SetColor(1, 0, 1, 1);
        g_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
        g_device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        SetColor(0, 1, 1, 1);
        g_device->DrawPrimitive(D3DPT_TRIANGLELIST, 6, 2);
        g_device->SetStreamSource(0, nullptr, 0, 0);
        Deko9_GetCounters(g_device, &after);
        r->hazardSkips = after.hazardSkips - before.hazardSkips;
        r->verified = after.verifiedDraws - before.verifiedDraws;
        r->mismatches = after.verifyMismatches - before.verifyMismatches;
        Read(r->px);
        Deko9_SetVerify(g_device, false);
    };
    static Result off, on, single;
    scene(0, &off);
    char detail[192];
    // Each fast path alone, then all together, against all off.
    const uint32_t kFlags[] = {DEKO9_PERDRAW_HAZARD, DEKO9_PERDRAW_CONSTS, DEKO9_PERDRAW_TEXTURES};
    const char *const kNames[] = {"HAZARD", "CONSTS", "TEXTURES"};
    for (uint32_t f = 0; f < 3; ++f)
    {
        scene(kFlags[f], &single);
        uint32_t diff = 0;
        for (uint32_t i = 0; i < kSize * kSize; ++i)
            diff += off.px[i] != single.px[i];
        char name[64];
        std::snprintf(name, sizeof(name), "PERDRAW_PIXELS_IDENTICAL_%s", kNames[f]);
        std::snprintf(detail, sizeof(detail), "differing=%u of %u mismatches=%llu", (unsigned)diff,
                      (unsigned)(kSize * kSize), (unsigned long long)single.mismatches);
        Check(diff == 0 && single.mismatches == 0, name, detail);
    }
    ExpectPixels("PERDRAW_SCENE_OFF", off.px,
                 {{P(8, 4), red}, {P(56, 4), green}, {P(8, 20), red}, {P(56, 20), green}, {P(32, 52), blue},
                  {P(2, 62), yellow}, {P(62, 62), green}, {P(44, 52), magenta}, {P(52, 52), cyan}});
    scene(kAllPerDraw, &on);
    uint32_t diff = 0;
    for (uint32_t i = 0; i < kSize * kSize; ++i)
        diff += off.px[i] != on.px[i];
    std::snprintf(detail, sizeof(detail), "differing=%u of %u", (unsigned)diff, (unsigned)(kSize * kSize));
    Check(diff == 0, "PERDRAW_PIXELS_IDENTICAL", detail);
    // Top band: rt = red left half, green right half; bands 2 same; band 3
    // red (texA before the re-upload), band 4 blue with the corner quads.
    ExpectPixels("PERDRAW_SCENE", on.px,
                 {{P(8, 4), red}, {P(56, 4), green}, {P(8, 20), red}, {P(56, 20), green}, {P(32, 52), blue},
                  {P(2, 62), yellow}, {P(62, 62), green}, {P(44, 52), magenta}, {P(52, 52), cyan}});
    std::snprintf(detail, sizeof(detail), "off: verified=%llu mismatches=%llu on: verified=%llu mismatches=%llu",
                  (unsigned long long)off.verified, (unsigned long long)off.mismatches,
                  (unsigned long long)on.verified, (unsigned long long)on.mismatches);
    Check(off.verified >= 9 && on.verified >= 9 && off.mismatches == 0 && on.mismatches == 0, "PERDRAW_VERIFY",
          detail);
    // With the skip on: the repeated rt-sampling draw, the texA redraw and
    // the constant-only draws skip (at least 3); never with it off.
    std::snprintf(detail, sizeof(detail), "off=%llu on=%llu", (unsigned long long)off.hazardSkips,
                  (unsigned long long)on.hazardSkips);
    Check(off.hazardSkips == 0 && on.hazardSkips >= 3, "PERDRAW_HAZARD_SKIPS", detail);
    // Band 3 samples texA before its re-upload, so D3D9 semantics make it
    // red (the device records a full barrier before that upload); all off
    // and all on. RunReuploadOrderTests covers the upload paths one by one.
    // An emulator without a write-after-read barrier on inline updates shows
    // blue here.
    std::snprintf(detail, sizeof(detail), "band3 off=%08x on=%08x (want %08x)", (unsigned)off.px[36 * kSize + 32],
                  (unsigned)on.px[36 * kSize + 32], (unsigned)red);
    Check(Near(off.px[36 * kSize + 32], red) && Near(on.px[36 * kSize + 32], red), "PERDRAW_REUPLOAD_ORDER", detail);
    // GPU per-draw probe (Deko9_SetDrawProbe): split draws and forced
    // re-binds change the draw count, never the pixels.
    const struct
    {
        uint32_t flags, split;
        const char *name;
    } kProbes[] = {{0, 2, "SPLIT2"}, {0, 3, "SPLIT3"}, {DEKO9_PROBE_SUBCONSTS, 2, "SPLIT2_SUBCONSTS"},
                   {DEKO9_PROBE_CONSTS | DEKO9_PROBE_TEXTURES | DEKO9_PROBE_STREAMS, 1, "REBIND_ALL"},
                   {15, 2, "ALL"}};
    for (const auto &probe : kProbes)
    {
        probeFlags = probe.flags;
        probeSplit = probe.split;
        scene(kAllPerDraw, &single);
        uint32_t d = 0;
        for (uint32_t i = 0; i < kSize * kSize; ++i)
            d += off.px[i] != single.px[i];
        char name[64];
        std::snprintf(name, sizeof(name), "DRAWPROBE_PIXELS_IDENTICAL_%s", probe.name);
        std::snprintf(detail, sizeof(detail), "differing=%u of %u mismatches=%llu", (unsigned)d,
                      (unsigned)(kSize * kSize), (unsigned long long)single.mismatches);
        Check(d == 0 && single.mismatches == 0, name, detail);
    }
    {
        // The split really issues more draws.
        Deko9Counters c0{}, c1{};
        probeFlags = 0;
        probeSplit = 2;
        Deko9_SetDrawProbe(g_device, 0, 2);
        Deko9_GetCounters(g_device, &c0);
        g_device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        g_device->SetPixelShader(g_psColor);
        g_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
        Deko9_GetCounters(g_device, &c1);
        Deko9_SetDrawProbe(g_device, 0, 1);
        g_device->SetStreamSource(0, nullptr, 0, 0);
        std::snprintf(detail, sizeof(detail), "extra=%llu", (unsigned long long)c1.probeExtraDraws - c0.probeExtraDraws);
        Check(c1.probeExtraDraws - c0.probeExtraDraws == 1, "DRAWPROBE_SPLIT_COUNTS", detail);
    }
    Deko9_SetDrawProbe(g_device, 0, 1);
    Deko9_SetPerDraw(g_device, kAllPerDraw);
    g_device->SetTexture(0, nullptr);
    vb->Release();
    rtSurface->Release();
    rt->Release();
    texA->Release();
    texB->Release();
    BeginPass();
}

// Write-after-read ordering of texture uploads. D3D9: a draw recorded before
// a LockRect/UnlockRect, UpdateTexture or UpdateSurface of a texture it
// samples must see the old contents, even with the upload still unsubmitted
// in the same command list (the driver renames or orders the copy). deko9
// uploads on the GPU timeline (staging buffer -> dkCmdBufCopyBufferToImage)
// and records a full barrier before any copy into an image read since the
// last barrier (HazardCheck: CopyWrite vs readEpoch). DkBarrier_Full is a
// host wait-for-idle (Gpfifo SetReference) plus a no-prefetch GPFIFO split,
// so the copy cannot overwrite texels an earlier draw still samples.
// Each variant: band 1 (rows 0-15) samples the texture, the texture is
// re-uploaded with a new color, band 2 (rows 16-31) samples it again with no
// rebind. Checked: band 1 old color, band 2 new color, and the device
// recorded the write-after-read barrier for the upload.
void RunReuploadOrderTests()
{
    const uint32_t black = 0xff000000, red = 0xffff0000, blue = 0xff0000ff;
    const uint32_t kAllPerDraw = DEKO9_PERDRAW_HAZARD | DEKO9_PERDRAW_CONSTS | DEKO9_PERDRAW_TEXTURES;
    IDirect3DTexture9 *managed = nullptr, *managed4 = nullptr, *def = nullptr, *sys = nullptr;
    IDirect3DSurface9 *defSurface = nullptr, *sysSurface = nullptr;
    IDirect3DQuery9 *query = nullptr;
    const bool ok =
        SUCCEEDED(g_device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &managed, nullptr)) &&
        SUCCEEDED(g_device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &managed4, nullptr)) &&
        SUCCEEDED(g_device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &def, nullptr)) &&
        SUCCEEDED(g_device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(def->GetSurfaceLevel(0, &defSurface)) && SUCCEEDED(sys->GetSurfaceLevel(0, &sysSurface)) &&
        SUCCEEDED(g_device->CreateQuery(D3DQUERYTYPE_EVENT, &query));
    Check(ok, "REUPLOAD_SETUP");
    if (!ok)
        return;
    // Fills `rect` (whole level 0 when null) with `color`.
    const auto fill = [](IDirect3DTexture9 *texture, uint32_t color, DWORD flags, const RECT *rect) {
        D3DSURFACE_DESC desc;
        texture->GetLevelDesc(0, &desc);
        const UINT w = rect ? (UINT)(rect->right - rect->left) : desc.Width;
        const UINT h = rect ? (UINT)(rect->bottom - rect->top) : desc.Height;
        D3DLOCKED_RECT locked;
        if (FAILED(texture->LockRect(0, &locked, rect, flags)))
            return false;
        for (UINT y = 0; y < h; ++y)
        {
            uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch);
            for (UINT x = 0; x < w; ++x)
                row[x] = color;
        }
        return SUCCEEDED(texture->UnlockRect(0));
    };
    enum Variant
    {
        ManagedDiscard,        // MANAGED full re-upload, D3DLOCK_DISCARD (staging copy)
        ManagedDiscardFastOff, // the same with the per-draw fast paths off
        ManagedSubRect,        // MANAGED sub-rectangle re-lock (shadow copy -> partial upload)
        UpdateTexture,         // SYSTEMMEM -> DEFAULT UpdateTexture
        UpdateSurface,         // SYSTEMMEM -> DEFAULT UpdateSurface
        AcrossSubmit,          // MANAGED DISCARD after the sampling draw's list was submitted (not waited)
        kVariants
    };
    const char *const kNames[kVariants] = {"MANAGED_DISCARD", "MANAGED_DISCARD_FASTOFF", "MANAGED_SUBRECT",
                                           "UPDATETEXTURE",   "UPDATESURFACE",           "ACROSS_SUBMIT"};
    const RECT topRows = {0, 0, 4, 2};
    for (int v = 0; v < kVariants; ++v)
    {
        IDirect3DTexture9 *sampled = v == ManagedSubRect                          ? managed4
                                     : (v == UpdateTexture || v == UpdateSurface) ? def
                                                                                  : managed;
        Deko9_SetPerDraw(g_device, v == ManagedDiscardFastOff ? 0u : kAllPerDraw);
        // Initial contents, settled on the GPU before the scene (the MANAGED
        // re-lock builds its shadow copy here, so the scene itself never
        // reads back or waits).
        bool staged;
        if (v == ManagedSubRect)
            staged = fill(managed4, red, 0, nullptr) && fill(managed4, red, 0, nullptr);
        else if (v == UpdateTexture || v == UpdateSurface)
            staged = fill(sys, red, 0, nullptr) && SUCCEEDED(g_device->UpdateTexture(sys, def));
        else
            staged = fill(managed, red, D3DLOCK_DISCARD, nullptr);
        Deko9_WaitForGpuIdle(g_device);
        Deko9Counters before{}, after{};
        Deko9_GetCounters(g_device, &before);
        BeginPass();
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, black, 1.0f, 0);
        g_device->SetPixelShader(g_psTexture);
        g_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        g_device->SetTexture(0, sampled);
        DrawQuad(-1, 1, 1, 0.5f); // band 1
        if (v == AcrossSubmit)
        {
            query->Issue(D3DISSUE_END);
            query->GetData(nullptr, 0, D3DGETDATA_FLUSH); // submits the list, does not wait
        }
        bool uploaded;
        if (v == ManagedSubRect)
            uploaded = fill(managed4, blue, 0, &topRows);
        else if (v == UpdateTexture)
            uploaded = fill(sys, blue, 0, nullptr) && SUCCEEDED(g_device->UpdateTexture(sys, def));
        else if (v == UpdateSurface)
            uploaded = fill(sys, blue, 0, nullptr) &&
                       SUCCEEDED(g_device->UpdateSurface(sysSurface, nullptr, defSurface, nullptr));
        else
            uploaded = fill(managed, blue, D3DLOCK_DISCARD, nullptr);
        DrawQuad(-1, 0.5f, 1, 0); // band 2, same bindings
        Deko9_GetCounters(g_device, &after);
        static uint32_t px[kSize * kSize];
        const bool read = Read(px);
        // Sub-rect: only the texture's top two rows changed (band 2's top half).
        const uint32_t band1 = px[4 * kSize + 32], band2 = px[18 * kSize + 32], band2Bottom = px[30 * kSize + 32];
        const uint64_t barriers = after.uploadAfterReadBarriers - before.uploadAfterReadBarriers;
        const bool pixels =
            Near(band1, red) && Near(band2, blue) && (v != ManagedSubRect || Near(band2Bottom, red));
        char name[64], detail[192];
        std::snprintf(name, sizeof(name), "REUPLOAD_ORDER_%s", kNames[v]);
        std::snprintf(detail, sizeof(detail),
                      "band1=%08x (want %08x, old) band2=%08x (want %08x) band2bottom=%08x war_barriers=%llu%s%s",
                      (unsigned)band1, (unsigned)red, (unsigned)band2, (unsigned)blue, (unsigned)band2Bottom,
                      (unsigned long long)barriers, staged && uploaded ? "" : " upload_failed",
                      read ? "" : " read_failed");
        Check(staged && uploaded && read && pixels && barriers >= 1, name, detail);
    }
    Deko9_SetPerDraw(g_device, kAllPerDraw);
    g_device->SetTexture(0, nullptr);
    query->Release();
    sysSurface->Release();
    defSurface->Release();
    managed->Release();
    managed4->Release();
    def->Release();
    sys->Release();
    BeginPass();
}

// Deko9_MoveContents: dst reads exactly what src held (a drawn image, not a
// fast clear), src must be fully rewritten before use, and texture <->
// standalone surface moves work (the back-buffer case).
void RunMoveTests()
{
    const uint32_t red = 0xffff0000, green = 0xff00ff00, blue = 0xff0000ff, white = 0xffffffff;
    IDirect3DTexture9 *a = nullptr, *b = nullptr, *small = nullptr;
    IDirect3DSurface9 *aSurface = nullptr, *bSurface = nullptr, *plain = nullptr;
    const bool ok =
        SUCCEEDED(g_device->CreateTexture(kSize, kSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                          &a, nullptr)) &&
        SUCCEEDED(g_device->CreateTexture(kSize, kSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                          &b, nullptr)) &&
        SUCCEEDED(g_device->CreateTexture(kSize / 2, kSize / 2, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                          D3DPOOL_DEFAULT, &small, nullptr)) &&
        SUCCEEDED(a->GetSurfaceLevel(0, &aSurface)) && SUCCEEDED(b->GetSurfaceLevel(0, &bSurface)) &&
        SUCCEEDED(g_device->CreateRenderTarget(kSize, kSize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &plain,
                                               nullptr));
    Check(ok, "MOVE_SETUP");
    if (!ok)
        return;
    const auto quad = [](float x0, float y0, float x1, float y1) {
        const Vertex v[6] = {
            {x0, y0, 0.5f, 1, 0, 0}, {x1, y0, 0.5f, 1, 1, 0}, {x0, y1, 0.5f, 1, 0, 1},
            {x1, y0, 0.5f, 1, 1, 0}, {x1, y1, 0.5f, 1, 1, 1}, {x0, y1, 0.5f, 1, 0, 1},
        };
        return g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(Vertex));
    };
    // Paints `target` red with `left` in its left half.
    const auto paint = [&](IDirect3DSurface9 *target, uint32_t left) {
        BeginPass();
        g_device->SetRenderTarget(0, target);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, red, 1.0f, 0);
        g_device->SetPixelShader(g_psColor);
        SetColor(((left >> 16) & 0xff) / 255.0f, ((left >> 8) & 0xff) / 255.0f, (left & 0xff) / 255.0f, 1);
        return SUCCEEDED(quad(-1, 1, 0, -1));
    };
    // Samples `texture` into the test target and reads back its left and
    // right half (row 32).
    static uint32_t px[kSize * kSize];
    uint32_t left = 0, right = 0;
    const auto show = [&](IDirect3DTexture9 *texture) {
        BeginPass();
        g_device->SetPixelShader(g_psTexture);
        g_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        g_device->SetTexture(0, texture);
        const bool drawn = SUCCEEDED(quad(-1, 1, 1, -1));
        g_device->SetTexture(0, nullptr);
        const bool read = drawn && Read(px);
        left = px[32 * kSize + 8];
        right = px[32 * kSize + 56];
        return read;
    };
    char detail[192];

    // 1. texture -> texture (the postfx resolve): b reads a's image; a is
    // rewritten by an opaque full-screen draw (accepted) and reads that.
    bool step = paint(aSurface, green) && paint(bSurface, blue);
    const bool moved = Deko9_MoveContents(g_device, a, b);
    BeginPass();
    g_device->SetRenderTarget(0, aSurface);
    g_device->SetPixelShader(g_psColor);
    SetColor(1, 1, 1, 1);
    const bool rewrite = SUCCEEDED(quad(-1, 1, 1, -1));
    bool pixels = show(b) && Near(left, green) && Near(right, red);
    const uint32_t bLeft = left, bRight = right;
    const bool aWhite = show(a) && Near(left, white) && Near(right, white);
    std::snprintf(detail, sizeof(detail), "moved=%d rewrite=%d b=%08x,%08x (want %08x,%08x) a_white=%d", moved,
                  rewrite, (unsigned)bLeft, (unsigned)bRight, (unsigned)green, (unsigned)red, aWhite);
    Check(step && moved && rewrite && pixels && aWhite, "MOVE_TEXTURE", detail);

    // 2. The moved-from side refuses a first draw that would keep stale
    // pixels (blending) and a partial clear; a whole-target clear then
    // makes it usable again.
    step = Deko9_MoveContents(g_device, b, a);
    BeginPass();
    g_device->SetRenderTarget(0, bSurface);
    g_device->SetPixelShader(g_psColor);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    const bool blendRefused = FAILED(quad(-1, 1, 1, -1));
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    const D3DRECT half = {0, 0, (LONG)kSize / 2, (LONG)kSize};
    const bool partialRefused = FAILED(g_device->Clear(1, &half, D3DCLEAR_TARGET, blue, 1.0f, 0));
    const bool cleared = SUCCEEDED(g_device->Clear(0, nullptr, D3DCLEAR_TARGET, blue, 1.0f, 0));
    const bool drawOk = SUCCEEDED(quad(-1, 1, 1, -1));
    // a now holds b's old image: green left, red right.
    pixels = show(a) && Near(left, green) && Near(right, red);
    std::snprintf(detail, sizeof(detail), "moved=%d blend_refused=%d partial_clear_refused=%d clear=%d draw=%d a=%d",
                  step, blendRefused, partialRefused, cleared, drawOk, pixels);
    Check(step && blendRefused && partialRefused && cleared && drawOk && pixels, "MOVE_STALE_CHECK", detail);

    // 3. texture -> standalone surface -> texture (the dynres scene -> back
    // buffer case, and back the next frame).
    step = paint(aSurface, blue);
    const bool toSurface = Deko9_MoveContents(g_device, a, plain);
    BeginPass();
    g_device->SetRenderTarget(0, aSurface);
    const bool aCleared = SUCCEEDED(g_device->Clear(0, nullptr, D3DCLEAR_TARGET, white, 1.0f, 0));
    const bool fromSurface = Deko9_MoveContents(g_device, plain, a);
    pixels = show(a) && Near(left, blue) && Near(right, red);
    std::snprintf(detail, sizeof(detail), "to_surface=%d a_cleared=%d from_surface=%d a=%08x,%08x", toSurface,
                  aCleared, fromSurface, (unsigned)left, (unsigned)right);
    Check(step && toSurface && aCleared && fromSurface && pixels, "MOVE_SURFACE", detail);

    // 4. A size mismatch is refused (the caller copies instead).
    Check(!Deko9_MoveContents(g_device, b, small), "MOVE_SIZE_REFUSED");

    // 5. Colourless depth pass (the shadow maps): with
    // a colorless surface as RT0 a draw writes depth only and a colour clear
    // skips it; the depth it wrote then rejects a farther quad.
    IDirect3DSurface9 *depth = nullptr, *sysColor = nullptr;
    const bool dsOk = SUCCEEDED(g_device->CreateDepthStencilSurface(kSize, kSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0,
                                                                    FALSE, &depth, nullptr)) &&
                      SUCCEEDED(g_device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8,
                                                                      D3DPOOL_SYSTEMMEM, &sysColor, nullptr));
    bool colorless = false;
    if (dsOk)
    {
        BeginPass();
        g_device->SetRenderTarget(0, plain);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, blue, 1.0f, 0);
        const bool marked = Deko9_SetColorless(plain);
        g_device->SetDepthStencilSurface(depth);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, red, 1.0f, 0);
        g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        g_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
        g_device->SetPixelShader(g_psColor);
        SetColor(0, 1, 0, 1);
        DrawQuad(-1, 1, 0, -1, 0.25f); // left half, near
        // The colour surface kept its blue (no red clear, no green quad).
        D3DLOCKED_RECT locked;
        uint32_t cLeft = 0, cRight = 0;
        const bool read = SUCCEEDED(g_device->GetRenderTargetData(plain, sysColor)) &&
                          SUCCEEDED(sysColor->LockRect(&locked, nullptr, D3DLOCK_READONLY));
        if (read)
        {
            const uint32_t *row = reinterpret_cast<const uint32_t *>(static_cast<uint8_t *>(locked.pBits) +
                                                                     32 * locked.Pitch);
            cLeft = row[8];
            cRight = row[56];
            sysColor->UnlockRect();
        }
        // The depth it wrote rejects a farther full quad on the left only.
        g_device->SetRenderTarget(0, g_targetSurface);
        g_device->SetDepthStencilSurface(depth);
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
        g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        SetColor(1, 1, 1, 1);
        DrawQuad(-1, 1, 1, -1, 0.5f);
        const bool readTarget = Read(px);
        const uint32_t dLeft = px[32 * kSize + 8], dRight = px[32 * kSize + 56];
        g_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        colorless = marked && read && readTarget && Near(cLeft, blue) && Near(cRight, blue) &&
                    Near(dLeft, 0xff000000) && Near(dRight, white);
        std::snprintf(detail, sizeof(detail), "marked=%d colour=%08x,%08x (want blue) depth_test=%08x,%08x", marked,
                      (unsigned)cLeft, (unsigned)cRight, (unsigned)dLeft, (unsigned)dRight);
    }
    Check(dsOk && colorless, "COLORLESS_DEPTH_PASS", detail);
    if (depth)
        depth->Release();
    if (sysColor)
        sysColor->Release();

    // Leave nothing moved-from behind for later tests.
    BeginPass();
    g_device->SetRenderTarget(0, plain);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, red, 1.0f, 0);
    BeginPass();
    plain->Release();
    aSurface->Release();
    bSurface->Release();
    a->Release();
    b->Release();
    small->Release();
}

// r_deko9FaultTrace: the GPU writes both crumb cells (the top-of-pipe one is
// deko3d's host semaphore release with the crumb patched in as payload), and
// the draw record carries the pixel textures and the draw's arguments.
void RunFaultTraceTests()
{
    Deko9_SetFaultTrace(g_device, 1);
    g_device->Present(nullptr, nullptr, nullptr, nullptr); // the trace applies at Present
    uint32_t crop = 0, top = 0;
    Check(Deko9_GetFaultTraceCells(g_device, &crop, &top), "FAULT_TRACE_ON");
    IDirect3DTexture9 *texture = nullptr;
    D3DLOCKED_RECT locked;
    const bool made = SUCCEEDED(g_device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture,
                                                        nullptr)) &&
                      SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0));
    Check(made, "FAULT_TRACE_SETUP");
    if (!made)
        return;
    for (UINT y = 0; y < 4; ++y)
        std::memset(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch, 0xff, 16);
    texture->UnlockRect(0);
    BeginPass();
    g_device->SetPixelShader(g_psTexture);
    g_device->SetTexture(0, texture);
    DrawQuad(-1, 1, 1, -1);
    Deko9DrawRecordInfo info{};
    const bool recorded = Deko9_GetLastDrawRecord(g_device, &info);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "tex=%u count=%u inst=%u indexed=%u native=%u vb0=%llx", info.texCount,
                  info.count, info.instances, (unsigned)info.indexed, (unsigned)info.native,
                  (unsigned long long)info.vb0);
    Check(recorded && info.texCount == 1 && info.count == 6 && info.instances == 1 && !info.indexed &&
              !info.native && info.vb0,
          "FAULT_TRACE_DRAW_RECORD", detail);
    // The black box's command window on a real deko3d stream: the draw's
    // top crumb is found in the submitted list and every header decodes.
    char window[1024] = "";
    bool bad = true;
    const bool found = Deko9_FaultTraceCommandWindow(g_device, info.draw, window, sizeof(window), &bad);
    Check(found && !bad && window[0] == 's', "FAULT_TRACE_COMMAND_WINDOW", window);
    uint32_t px[kSize * kSize];
    Read(px); // waits for the list, whose end both cells then hold
    ExpectPixels("FAULT_TRACE_DRAW", px, {{P(8, 8), 0xffffffff}, {P(56, 56), 0xffffffff}});
    const bool cells = Deko9_GetFaultTraceCells(g_device, &crop, &top);
    std::snprintf(detail, sizeof(detail), "crop=%08x top=%08x", (unsigned)crop, (unsigned)top);
    Check(cells && crop == top && (crop & 0xffff) == 0xffff, "FAULT_TRACE_CELLS", detail);
    g_device->SetTexture(0, nullptr);
    texture->Release();
}

// Tiny command-memory chunks: every list crosses many GPFIFO entry switches
// (each chunk switch starts a new entry). The GPU must finish every list, the
// image must be the last draw's, and the black box must decode a draw's words
// across a switch from the list's recorded segments.
void RunCmdChunkTests()
{
    Deko9_SetCmdChunkBytes(g_device, 1024);
    g_device->Present(nullptr, nullptr, nullptr, nullptr); // applies at Present
    BeginPass();
    g_device->SetPixelShader(g_psColor);
    constexpr uint32_t kLists = 48;
    uint32_t windows = 0, bad = 0, crossed = 0;
    char window[2048], detail[200] = "";
    for (uint32_t list = 1; list <= kLists; ++list)
    {
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
        for (uint32_t d = 0; d < list; ++d)
        {
            SetColor((d & 1) ? 1.0f : 0.0f, 0, 1, 1);
            DrawQuad(-1, 1, 1, -1);
        }
        Deko9DrawRecordInfo info{};
        bool isBad = true;
        if (!Deko9_GetLastDrawRecord(g_device, &info) ||
            !Deko9_FaultTraceCommandWindow(g_device, info.draw, window, sizeof(window), &isBad))
            continue;
        ++windows;
        bad += isBad ? 1 : 0;
        const char *at = std::strstr(window, "|seg ");
        if (at && !crossed++)
            std::snprintf(detail, sizeof(detail), "%.180s", at - std::min<ptrdiff_t>(at - window, 60));
    }
    char counts[64];
    std::snprintf(counts, sizeof(counts), "windows=%u bad=%u crossed=%u", windows, bad, crossed);
    Check(windows == kLists && !bad, "CMD_CHUNK_WINDOWS", counts);
    Check(crossed > 0, "CMD_CHUNK_SWITCH_DECODED", detail);
    uint32_t px[kSize * kSize];
    Read(px);
    ExpectPixels("CMD_CHUNK_DRAW", px, {{P(8, 8), 0xffff00ff}, {P(56, 56), 0xffff00ff}});
    uint32_t crop = 0, top = 0;
    const bool cells = Deko9_GetFaultTraceCells(g_device, &crop, &top);
    std::snprintf(counts, sizeof(counts), "crop=%08x top=%08x", (unsigned)crop, (unsigned)top);
    Check(cells && crop == top && (crop & 0xffff) == 0xffff, "CMD_CHUNK_CELLS", counts);
    Deko9_SetCmdChunkBytes(g_device, 0);
    g_device->Present(nullptr, nullptr, nullptr, nullptr);
}

void RunDirtyBoxTests()
{
    Deko9_SetCensus(g_device, true);
    // Applied at the next Present (like Deko9_SetGpuPasses); also resets the
    // census entries this test reads deltas from.
    g_device->Present(nullptr, nullptr, nullptr, nullptr);

    constexpr UINT kVW = 16, kVH = 16, kVD = 4, kBpp = 4;
    const uint64_t fullVolBytes = (uint64_t)kVW * kVH * kVD * kBpp;
    IDirect3DVolumeTexture9 *volSys = nullptr, *volDef = nullptr;
    bool ok = SUCCEEDED(g_device->CreateVolumeTexture(kVW, kVH, kVD, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                      &volSys, nullptr)) &&
             SUCCEEDED(g_device->CreateVolumeTexture(kVW, kVH, kVD, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &volDef,
                                                      nullptr));
    Check(ok, "DIRTYBOX_VOLUME_SETUP");
    if (!ok)
        return;

    const auto fillWhole = [&](uint8_t value) {
        D3DLOCKED_BOX box;
        if (FAILED(volSys->LockBox(0, &box, nullptr, 0)))
            return;
        for (UINT z = 0; z < kVD; ++z)
            for (UINT y = 0; y < kVH; ++y)
                std::memset(static_cast<uint8_t *>(box.pBits) + z * box.SlicePitch + y * box.RowPitch, value,
                            kVW * kBpp);
        volSys->UnlockBox(0);
    };
    const auto readVoxel = [&](UINT x, UINT y, UINT z) -> uint32_t {
        D3DBOX box{x, y, x + 1, y + 1, z, z + 1};
        D3DLOCKED_BOX locked;
        uint32_t value = 0xdeadbeefu;
        if (SUCCEEDED(volDef->LockBox(0, &locked, &box, D3DLOCK_READONLY)))
        {
            std::memcpy(&value, locked.pBits, 4);
            volDef->UnlockBox(0);
        }
        return value;
    };
    char detail[96];

    // 1) A freshly created SYSTEMMEM texture is fully dirty (D3D9): the
    //    first UpdateTexture copies the whole volume even though nothing
    //    called AddDirtyBox yet.
    fillWhole(0x11);
    Deko9Counters before{}, after{};
    Deko9_GetCounters(g_device, &before);
    ok = SUCCEEDED(g_device->UpdateTexture(volSys, volDef));
    Deko9_GetCounters(g_device, &after);
    std::snprintf(detail, sizeof(detail), "bytes=%llu want=%llu",
                 (unsigned long long)(after.updateTextureBytes - before.updateTextureBytes),
                 (unsigned long long)fullVolBytes);
    Check(ok && after.updateTextureCalls == before.updateTextureCalls + 1 &&
              after.updateTextureBytes == before.updateTextureBytes + fullVolBytes,
          "DIRTYBOX_NEW_TEXTURE_FULL", detail);
    Check(readVoxel(0, 0, 0) == 0x11111111u && readVoxel(kVW - 1, kVH - 1, kVD - 1) == 0x11111111u,
          "DIRTYBOX_NEW_TEXTURE_VALUES");

    // 2) Two small AddDirtyBox patches (D3DLOCK_NO_DIRTY_UPDATE lock, exactly
    //    the model-lighting engine pattern) plus a third write that is never
    //    marked dirty at all.
    D3DLOCKED_BOX locked;
    ok = SUCCEEDED(volSys->LockBox(0, &locked, nullptr, D3DLOCK_NO_DIRTY_UPDATE));
    Check(ok, "DIRTYBOX_PATCH_LOCK");
    if (ok)
    {
        const auto poke = [&](UINT x, UINT y, UINT z, uint32_t value) {
            std::memcpy(static_cast<uint8_t *>(locked.pBits) + z * locked.SlicePitch + y * locked.RowPitch + x * kBpp,
                       &value, 4);
        };
        for (UINT dy = 0; dy < 2; ++dy)
            for (UINT dx = 0; dx < 2; ++dx)
                poke(2 + dx, 2 + dy, 0, 0x22222222u); // patch A
        for (UINT dy = 0; dy < 2; ++dy)
            for (UINT dx = 0; dx < 2; ++dx)
                poke(10 + dx, 10 + dy, 2, 0x33333333u); // patch B
        poke(6, 6, 1, 0x44444444u); // region C: written, never marked dirty
        volSys->UnlockBox(0);
        const D3DBOX boxA{2, 2, 4, 4, 0, 1}, boxB{10, 10, 12, 12, 2, 3};
        volSys->AddDirtyBox(&boxA);
        volSys->AddDirtyBox(&boxB);
    }
    const uint64_t patchBytes = (uint64_t)(2 * 2 * 1 + 2 * 2 * 1) * kBpp;
    Deko9_GetCounters(g_device, &before);
    ok = SUCCEEDED(g_device->UpdateTexture(volSys, volDef));
    Deko9_GetCounters(g_device, &after);
    std::snprintf(detail, sizeof(detail), "bytes=%llu want=%llu",
                 (unsigned long long)(after.updateTextureBytes - before.updateTextureBytes),
                 (unsigned long long)patchBytes);
    Check(ok && after.updateTextureCalls == before.updateTextureCalls + 1 &&
              after.updateTextureBytes == before.updateTextureBytes + patchBytes,
          "DIRTYBOX_PARTIAL_BYTES", detail);
    Check(readVoxel(2, 2, 0) == 0x22222222u && readVoxel(3, 3, 0) == 0x22222222u &&
              readVoxel(10, 10, 2) == 0x33333333u && readVoxel(11, 11, 2) == 0x33333333u,
          "DIRTYBOX_PARTIAL_VALUES");
    // Region C changed in the SYSTEMMEM source's CPU bytes but was never
    // marked dirty: real D3D9 UpdateTexture must not have copied it, so the
    // DEFAULT destination still shows pass-1's 0x11, not the new 0x44.
    Check(readVoxel(6, 6, 1) == 0x11111111u, "DIRTYBOX_NOT_MARKED_NOT_COPIED");

    // 3) 16 adjacent 1-voxel-wide AddDirtyBox calls (the engine marks one
    //    4x4x4 box per lighting sample) must coalesce into the exact row,
    //    not more (no inflation) and not less (every box still lands).
    ok = SUCCEEDED(volSys->LockBox(0, &locked, nullptr, D3DLOCK_NO_DIRTY_UPDATE));
    if (ok)
    {
        for (UINT x = 0; x < kVW; ++x)
        {
            const uint32_t value = 0x55000000u | x;
            std::memcpy(static_cast<uint8_t *>(locked.pBits) + 1 * locked.SlicePitch + 5 * locked.RowPitch + x * kBpp,
                       &value, 4);
        }
        volSys->UnlockBox(0);
        for (UINT x = 0; x < kVW; ++x)
        {
            const D3DBOX b{x, 5, x + 1, 6, 1, 2};
            volSys->AddDirtyBox(&b);
        }
    }
    const uint64_t rowBytes = (uint64_t)kVW * kBpp;
    Deko9_GetCounters(g_device, &before);
    ok = SUCCEEDED(g_device->UpdateTexture(volSys, volDef));
    Deko9_GetCounters(g_device, &after);
    std::snprintf(detail, sizeof(detail), "bytes=%llu want=%llu",
                 (unsigned long long)(after.updateTextureBytes - before.updateTextureBytes),
                 (unsigned long long)rowBytes);
    Check(ok && after.updateTextureBytes == before.updateTextureBytes + rowBytes, "DIRTYBOX_MERGE_ROW", detail);
    Check(readVoxel(0, 5, 1) == (0x55000000u | 0) && readVoxel(kVW - 1, 5, 1) == (0x55000000u | (kVW - 1)),
          "DIRTYBOX_MERGE_VALUES");

    // 4) AddDirtyBox(nullptr) = whole texture (D3D9), overriding any prior
    //    partial-dirty state.
    ok = SUCCEEDED(volSys->LockBox(0, &locked, nullptr, D3DLOCK_NO_DIRTY_UPDATE));
    if (ok)
    {
        for (UINT z = 0; z < kVD; ++z)
            for (UINT y = 0; y < kVH; ++y)
                std::memset(static_cast<uint8_t *>(locked.pBits) + z * locked.SlicePitch + y * locked.RowPitch, 0x66,
                            kVW * kBpp);
        volSys->UnlockBox(0);
        volSys->AddDirtyBox(nullptr);
    }
    Deko9_GetCounters(g_device, &before);
    ok = SUCCEEDED(g_device->UpdateTexture(volSys, volDef));
    Deko9_GetCounters(g_device, &after);
    Check(ok && after.updateTextureBytes == before.updateTextureBytes + fullVolBytes, "DIRTYBOX_NULL_BOX_FULL");
    Check(readVoxel(0, 0, 0) == 0x66666666u && readVoxel(kVW - 1, kVH - 1, kVD - 1) == 0x66666666u,
          "DIRTYBOX_NULL_BOX_VALUES");

    volSys->Release();
    volDef->Release();

    // 5) The same accounting for IDirect3DTexture9::AddDirtyRect (2D), which
    //    had the identical always-whole-texture stub.
    constexpr UINT kTW = 16, kTH = 16;
    IDirect3DTexture9 *texSys = nullptr, *texDef = nullptr;
    ok = SUCCEEDED(g_device->CreateTexture(kTW, kTH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &texSys, nullptr)) &&
        SUCCEEDED(g_device->CreateTexture(kTW, kTH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texDef, nullptr));
    Check(ok, "DIRTYRECT_SETUP");
    if (ok)
    {
        D3DLOCKED_RECT rect;
        ok = SUCCEEDED(texSys->LockRect(0, &rect, nullptr, 0));
        if (ok)
        {
            for (UINT y = 0; y < kTH; ++y)
                std::memset(static_cast<uint8_t *>(rect.pBits) + y * rect.Pitch, 0x77, kTW * kBpp);
            texSys->UnlockRect(0);
        }
        Deko9_GetCounters(g_device, &before);
        g_device->UpdateTexture(texSys, texDef);
        Deko9_GetCounters(g_device, &after);
        Check(after.updateTextureBytes == before.updateTextureBytes + (uint64_t)kTW * kTH * kBpp,
              "DIRTYRECT_NEW_TEXTURE_FULL");

        ok = SUCCEEDED(texSys->LockRect(0, &rect, nullptr, D3DLOCK_NO_DIRTY_UPDATE));
        if (ok)
        {
            uint32_t v = 0x88888888u;
            std::memcpy(static_cast<uint8_t *>(rect.pBits) + 4 * rect.Pitch + 4 * kBpp, &v, 4);
            v = 0x99999999u; // never marked dirty below
            std::memcpy(static_cast<uint8_t *>(rect.pBits) + 10 * rect.Pitch + 10 * kBpp, &v, 4);
            texSys->UnlockRect(0);
            const RECT dirty{4, 4, 5, 5};
            texSys->AddDirtyRect(&dirty);
        }
        Deko9_GetCounters(g_device, &before);
        g_device->UpdateTexture(texSys, texDef);
        Deko9_GetCounters(g_device, &after);
        std::snprintf(detail, sizeof(detail), "bytes=%llu want=%llu",
                     (unsigned long long)(after.updateTextureBytes - before.updateTextureBytes),
                     (unsigned long long)kBpp);
        Check(after.updateTextureBytes == before.updateTextureBytes + kBpp, "DIRTYRECT_PARTIAL_BYTES", detail);

        D3DLOCKED_RECT rb;
        uint32_t got4 = 0, got10 = 0;
        if (SUCCEEDED(texDef->LockRect(0, &rb, nullptr, D3DLOCK_READONLY)))
        {
            std::memcpy(&got4, static_cast<uint8_t *>(rb.pBits) + 4 * rb.Pitch + 4 * kBpp, 4);
            std::memcpy(&got10, static_cast<uint8_t *>(rb.pBits) + 10 * rb.Pitch + 10 * kBpp, 4);
            texDef->UnlockRect(0);
        }
        Check(got4 == 0x88888888u, "DIRTYRECT_PARTIAL_VALUE");
        Check(got10 == 0x77777777u, "DIRTYRECT_NOT_MARKED_NOT_COPIED");
    }
    if (texSys)
        texSys->Release();
    if (texDef)
        texDef->Release();

    Deko9_SetCensus(g_device, false);
    g_device->Present(nullptr, nullptr, nullptr, nullptr);
}

// Test pattern for the upscaler checks, as 0xAARRGGBB: colour ramps, a white
// block near the top-left (orientation), 3-px bars (high frequency), a
// diagonal edge, and 1-px lines.
uint32_t FsrPattern(int x, int y)
{
    const auto rgb = [](int r, int g, int b) {
        return 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    };
    if (x >= 40 && x < 200 && y >= 40 && y < 120)
        return rgb(255, 255, 255);
    if (x >= 300 && x < 500 && y >= 200 && y < 300)
        return (x / 3) & 1 ? rgb(250, 240, 20) : rgb(10, 20, 90);
    if (x >= 600 && x < 900 && y >= 100 && y < 400)
        return x - 600 > y - 100 ? rgb(230, 230, 230) : rgb(120, 10, 10);
    if (y == 450 || x == 250)
        return rgb(0, 0, 0);
    return rgb(x * 255 / 959, y * 255 / 539, 64);
}

// r_renderResolution path: a 960x540 back buffer is upscaled to the
// 1280x720 swapchain at present by each r_fsrMode pass (sgsr,
// bilinear_rcas, bilinear). The presented image is read back
// (GetFrontBufferData) and compared per pixel with the mode's CPU reference
// (deko9_fsr_reference.h, itself checked on the host by
// switch_deko9_fsr_test). With `timing`, each mode then presents 62 more
// frames so the device logs one `DEKO9 fsr frames=60 gpu=... mode=<mode>`
// line per mode (run.sh checks them).
void RunFsrTests(IDirect3D9 *d3d, HWND window, bool timing)
{
    constexpr int kInW = 960, kInH = 540, kOutW = 1280, kOutH = 720;
    constexpr float kSharpness = 0.2f;
    D3DPRESENT_PARAMETERS params{};
    params.BackBufferWidth = kInW;
    params.BackBufferHeight = kInH;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 3;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    params.hDeviceWindow = window;
    IDirect3DDevice9 *device = nullptr;
    const bool created = SUCCEEDED(
        d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device));
    Check(created, "FSR_DEVICE", "960x540 back buffer, 1280x720 swapchain");
    if (!created)
        return;
    D3DDISPLAYMODE mode{};
    Check(SUCCEEDED(device->GetDisplayMode(0, &mode)) && mode.Width == kOutW && mode.Height == kOutH,
          "FSR_DISPLAY_MODE");

    std::vector<uint32_t> pattern((size_t)kInW * kInH);
    for (int y = 0; y < kInH; ++y)
        for (int x = 0; x < kInW; ++x)
            pattern[(size_t)y * kInW + x] = FsrPattern(x, y);
    deko9::fsrref::Image in;
    in.width = kInW;
    in.height = kInH;
    in.px.resize(pattern.size());
    for (size_t i = 0; i < pattern.size(); ++i)
        in.px[i] = {((pattern[i] >> 16) & 0xff) / 255.0f, ((pattern[i] >> 8) & 0xff) / 255.0f,
                    (pattern[i] & 0xff) / 255.0f};

    IDirect3DTexture9 *sys = nullptr, *rt = nullptr;
    IDirect3DSurface9 *rtSurface = nullptr, *back = nullptr, *backCopy = nullptr, *front = nullptr;
    bool ok = SUCCEEDED(device->CreateTexture(kInW, kInH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
              SUCCEEDED(device->CreateTexture(kInW, kInH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                              D3DPOOL_DEFAULT, &rt, nullptr)) &&
              SUCCEEDED(rt->GetSurfaceLevel(0, &rtSurface)) &&
              SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) &&
              SUCCEEDED(device->CreateOffscreenPlainSurface(kInW, kInH, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                            &backCopy, nullptr)) &&
              SUCCEEDED(device->CreateOffscreenPlainSurface(kOutW, kOutH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                            &front, nullptr));
    D3DLOCKED_RECT locked;
    if (ok && SUCCEEDED(sys->LockRect(0, &locked, nullptr, 0)))
    {
        for (int y = 0; y < kInH; ++y)
            std::memcpy(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch, &pattern[(size_t)y * kInW],
                        kInW * 4);
        sys->UnlockRect(0);
    }
    else
    {
        ok = false;
    }
    ok = ok && SUCCEEDED(device->UpdateTexture(sys, rt));
    Deko9_SetUpscaleSharpness(device, kSharpness);

    for (uint32_t upscale = 0; upscale < deko9::UPSCALE_MODE_COUNT; ++upscale)
    {
        const char *modeName = deko9::UpscaleModeName(upscale);
        char name[96];
        const auto check = [&](bool pass, const char *what, const char *detail) {
            std::snprintf(name, sizeof(name), "FSR_%s_%s", what, modeName);
            Check(pass, name, detail);
        };
        Deko9_SetUpscaleMode(device, upscale);
        const bool presented = ok &&
                               SUCCEEDED(device->StretchRect(rtSurface, nullptr, back, nullptr, D3DTEXF_POINT)) &&
                               SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr));
        check(presented, "PRESENT", "");

        // The back buffer keeps the render-size frame (capture ring path).
        if (presented)
        {
            bool same = SUCCEEDED(device->GetRenderTargetData(back, backCopy)) &&
                        SUCCEEDED(backCopy->LockRect(&locked, nullptr, D3DLOCK_READONLY));
            char detail[96] = "";
            if (same)
            {
                for (int y = 0; y < kInH && same; ++y)
                    for (int x = 0; x < kInW && same; ++x)
                    {
                        uint32_t v;
                        std::memcpy(&v, static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch + x * 4, 4);
                        same = (v & 0xffffff) == (pattern[(size_t)y * kInW + x] & 0xffffff);
                        if (!same)
                            std::snprintf(detail, sizeof(detail), "(%d,%d)=%08x want %08x", x, y, (unsigned)v,
                                          (unsigned)pattern[(size_t)y * kInW + x]);
                    }
                backCopy->UnlockRect();
            }
            check(same, "BACKBUFFER_RENDER_SIZE", detail);
        }

        // The presented frame against the mode's CPU reference.
        if (!presented || FAILED(device->GetFrontBufferData(0, front)) ||
            FAILED(front->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        {
            check(false, "FRONT_BUFFER", "");
            continue;
        }
        // bilinear_rcas is also compared with a reference whose filtered taps
        // are rounded to 8 bits (deko9_fsr_reference.h BilinearRcas): a pixel
        // matches when it is close to either filter precision, and the
        // counts of pixels only one model explains record which precision
        // this GPU's sampler has.
        const bool twoModels = upscale == deko9::UPSCALE_BILINEAR_RCAS;
        const std::vector<uint8_t> want = deko9::fsrref::UpscaleRgb8(in, kOutW, kOutH, upscale, kSharpness);
        const std::vector<uint8_t> want8 =
            twoModels ? deko9::fsrref::UpscaleRgb8(in, kOutW, kOutH, upscale, kSharpness, true) : want;
        int maxDiff = 0, worstX = 0, worstY = 0;
        uint32_t over1 = 0, floatOnly = 0, taps8Only = 0;
        for (int y = 0; y < kOutH; ++y)
            for (int x = 0; x < kOutW; ++x)
            {
                const uint8_t *got = static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch + x * 4; // B,G,R,A
                const size_t at = ((size_t)y * kOutW + x) * 3;
                int diffFloat = 0, diff8 = 0;
                for (int c = 0; c < 3; ++c)
                {
                    const int d = (int)got[2 - c] - (int)want[at + c], d8 = (int)got[2 - c] - (int)want8[at + c];
                    diffFloat = std::max(diffFloat, d < 0 ? -d : d);
                    diff8 = std::max(diff8, d8 < 0 ? -d8 : d8);
                }
                floatOnly += diffFloat <= 1 && diff8 > 1;
                taps8Only += diff8 <= 1 && diffFloat > 1;
                const int diff = std::min(diffFloat, diff8);
                if (diff > 1)
                    ++over1;
                if (diff > maxDiff)
                {
                    maxDiff = diff;
                    worstX = x;
                    worstY = y;
                }
            }
        // GPU vs CPU: FMA contraction, MUFU reciprocals and the filter's
        // fixed-point weights can move a value by an LSB, which RCAS / the
        // SGSR luma correction can amplify at hard edges. Plain bilinear has
        // nothing to amplify it.
        const int maxAllowed = upscale == deko9::UPSCALE_BILINEAR ? 2 : 8;
        char detail[200];
        std::snprintf(detail, sizeof(detail), "max_diff=%d at (%d,%d) pixels_over_1=%u of %d", maxDiff, worstX,
                      worstY, (unsigned)over1, kOutW * kOutH);
        if (twoModels)
        {
            const size_t len = std::strlen(detail);
            std::snprintf(detail + len, sizeof(detail) - len, " float_taps_only=%u unorm8_taps_only=%u",
                          (unsigned)floatOnly, (unsigned)taps8Only);
        }
        check(maxDiff <= maxAllowed && over1 <= (uint32_t)(kOutW * kOutH / 1000), "MATCHES_REFERENCE", detail);

        // Direct probes, independent of the reference.
        const auto px = [&](int x, int y) {
            const uint8_t *p = static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch + x * 4;
            return (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
        };
        const auto luma = [&](int x, int y) {
            const uint32_t v = px(x, y);
            return (int)((v >> 16) & 0xff) + (int)((v >> 8) & 0xff) + (int)(v & 0xff);
        };
        // White block: input [40,200)x[40,120) -> output ~[53,267)x[53,160);
        // it stays top-left (no vertical or horizontal flip).
        const bool block = luma(160, 106) >= 3 * 250 && luma(160, 700) < 3 * 200 && luma(1200, 106) < 3 * 250;
        std::snprintf(detail, sizeof(detail), "block=%06x flippedY=%06x flippedX=%06x", (unsigned)px(160, 106),
                      (unsigned)px(160, 700), (unsigned)px(1200, 106));
        check(block, "ORIENTATION", detail);
        // 3-px bars (period 6 px) -> period 8 px: at least 80% of the
        // input contrast (luma 120 vs 510) survives.
        int lo = 999, hi = 0;
        for (int x = 500; x < 516; ++x)
        {
            lo = std::min(lo, luma(x, 330));
            hi = std::max(hi, luma(x, 330));
        }
        std::snprintf(detail, sizeof(detail), "luma lo=%d hi=%d", lo, hi);
        check(hi - lo >= (510 - 120) * 8 / 10, "HIGH_FREQUENCY", detail);
        // Diagonal: input x-600 = y-100 -> output row 333 crosses it near
        // x = 1000; 6 px either side is fully on its side.
        const bool diag = luma(1006, 333) >= 3 * 220 && luma(994, 333) <= 120 + 10 + 10 + 30;
        std::snprintf(detail, sizeof(detail), "light=%06x dark=%06x", (unsigned)px(1006, 333),
                      (unsigned)px(994, 333));
        check(diag, "EDGE_SHARP", detail);
        front->UnlockRect();

        // GPU timing line for this mode (a timestamp slot counts only for the
        // mode it was recorded for).
        for (int frame = 0; timing && frame < 62; ++frame)
            device->Present(nullptr, nullptr, nullptr, nullptr);
    }
    for (IUnknown *object : {(IUnknown *)sys, (IUnknown *)rtSurface, (IUnknown *)rt, (IUnknown *)back,
                             (IUnknown *)backCopy, (IUnknown *)front})
    {
        if (object)
            object->Release();
    }
    device->Release();
}

// Pattern for the dynamic-resolution checks at any size, as 0xAARRGGBB:
// ramps, a white block (orientation), 3-px bars, a diagonal edge, 1-px
// lines. No pixel is magenta-like (r > 200, g < 60, b > 200), the colour the
// stale full-size contents are filled with.
uint32_t DynPattern(int x, int y, int w, int h)
{
    const auto rgb = [](int r, int g, int b) {
        return 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    };
    if (x >= w / 24 && x < w / 5 && y >= h / 13 && y < h / 4)
        return rgb(255, 255, 255);
    if (x >= w * 5 / 16 && x < w / 2 && y >= h * 3 / 8 && y < h * 5 / 9)
        return (x / 3) & 1 ? rgb(250, 240, 20) : rgb(10, 20, 90);
    if (x >= w * 5 / 8 && x < w * 15 / 16 && y >= h / 5 && y < h * 3 / 4)
        return (x - w * 5 / 8) > (y - h / 5) ? rgb(230, 230, 230) : rgb(120, 10, 10);
    // The last row/column get a distinct colour: a border that bled or
    // went missing shows at once.
    if (x == w - 1 || y == h - 1)
        return rgb(40, 200, 60);
    if (y == h * 5 / 6 || x == w * 26 / 100)
        return rgb(0, 0, 0);
    return rgb(x * 255 / (w - 1), y * 255 / (h - 1), 64);
}

bool MagentaLike(const uint8_t *bgra) { return bgra[2] > 200 && bgra[1] < 60 && bgra[0] > 200; }

// r_dynres (src/gfx_d3d/r_dynres.h): a render-target texture and depth
// surface created at 1280x720 are re-laid out in place at the controller's
// quantised sizes (Deko9_ResizeRenderTarget); each size is filled with a
// pattern over stale magenta full-size contents and upscaled into the
// 1280x720 back buffer by every r_fsrMode pass (Deko9_UpscaleSurface), read
// back and compared with the CPU reference of the same upscale from the
// w x h image (float or 8-bit-fraction bilinear, the closer per pixel). No
// magenta may appear (no stale texel bleeds in at the right/bottom
// borders), the last row/column's colour must reach the output's. Plus: the
// resized target renders (clear + scissored clear read back at w x h),
// a source rectangle inside a larger texture (letterboxed view), and the
// equal-size copy path. With `timing`, 62 more upscales per mode log a
// `DEKO9 fsr ... path=scene` line.
void RunDynResTests(IDirect3D9 *d3d, HWND window, bool timing)
{
    constexpr int kOutW = 1280, kOutH = 720;
    constexpr float kSharpness = 0.2f;
    D3DPRESENT_PARAMETERS params{};
    params.BackBufferWidth = kOutW;
    params.BackBufferHeight = kOutH;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 3;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    params.hDeviceWindow = window;
    IDirect3DDevice9 *device = nullptr;
    const bool created = SUCCEEDED(
        d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device));
    Check(created, "DYNRES_DEVICE", "1280x720 back buffer (no present upscale)");
    if (!created)
        return;
    IDirect3DTexture9 *scene = nullptr;
    IDirect3DSurface9 *sceneSurface = nullptr, *depth = nullptr, *back = nullptr, *backCopy = nullptr;
    bool ok = SUCCEEDED(device->CreateTexture(kOutW, kOutH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                              D3DPOOL_DEFAULT, &scene, nullptr)) &&
              SUCCEEDED(scene->GetSurfaceLevel(0, &sceneSurface)) &&
              SUCCEEDED(device->CreateDepthStencilSurface(kOutW, kOutH, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                          &depth, nullptr)) &&
              SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) &&
              SUCCEEDED(device->CreateOffscreenPlainSurface(kOutW, kOutH, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                            &backCopy, nullptr));
    Check(ok, "DYNRES_SETUP");
    if (!ok)
    {
        device->Release();
        return;
    }
    // Stale contents: the whole 1280x720 layout magenta.
    const auto fillMagentaFull = [&]() {
        return Deko9_ResizeRenderTarget(device, scene, kOutW, kOutH) &&
               SUCCEEDED(device->SetRenderTarget(0, sceneSurface)) &&
               SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 255), 1.0f, 0));
    };
    const auto upload = [&](int w, int h, const std::vector<uint32_t> &px) {
        IDirect3DTexture9 *sys = nullptr;
        D3DLOCKED_RECT locked;
        bool done = SUCCEEDED(device->CreateTexture(w, h, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
                    SUCCEEDED(sys->LockRect(0, &locked, nullptr, 0));
        if (done)
        {
            for (int y = 0; y < h; ++y)
                std::memcpy(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch, &px[(size_t)y * w], w * 4);
            sys->UnlockRect(0);
            done = SUCCEEDED(device->UpdateTexture(sys, scene));
        }
        if (sys)
            sys->Release();
        return done;
    };
    const auto toImage = [](int w, int h, const std::vector<uint32_t> &px) {
        deko9::fsrref::Image in;
        in.width = w;
        in.height = h;
        in.px.resize(px.size());
        for (size_t i = 0; i < px.size(); ++i)
            in.px[i] = {((px[i] >> 16) & 0xff) / 255.0f, ((px[i] >> 8) & 0xff) / 255.0f, (px[i] & 0xff) / 255.0f};
        return in;
    };
    // Reads the back buffer and compares rectangle `dst` with the reference
    // of upscaling `in` to dst's size; outside `dst` every pixel must still
    // be `outside` (when non-zero).
    const auto compare = [&](const char *name, const deko9::fsrref::Image &in, uint32_t mode, const int dst[4],
                             bool exact, uint32_t outside) {
        D3DLOCKED_RECT locked;
        if (FAILED(device->GetRenderTargetData(back, backCopy)) ||
            FAILED(backCopy->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        {
            Check(false, name, "readback failed");
            return;
        }
        deko9::fsrref::g_bilinearFractionBits = 0;
        const std::vector<uint8_t> want = deko9::fsrref::UpscaleRgb8(in, dst[2], dst[3], mode, kSharpness);
        deko9::fsrref::g_bilinearFractionBits = 8;
        const std::vector<uint8_t> want8 = deko9::fsrref::UpscaleRgb8(in, dst[2], dst[3], mode, kSharpness);
        deko9::fsrref::g_bilinearFractionBits = 0;
        int maxDiff = 0, worstX = 0, worstY = 0;
        uint32_t over1 = 0, magenta = 0, outsideBad = 0, borderBad = 0;
        for (int y = 0; y < kOutH; ++y)
            for (int x = 0; x < kOutW; ++x)
            {
                const uint8_t *got = static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch + x * 4; // B,G,R,A
                magenta += MagentaLike(got);
                const int rx = x - dst[0], ry = y - dst[1];
                if (rx < 0 || ry < 0 || rx >= dst[2] || ry >= dst[3])
                {
                    const uint32_t v = (uint32_t)got[2] << 16 | (uint32_t)got[1] << 8 | got[0];
                    outsideBad += outside && v != (outside & 0xffffff);
                    continue;
                }
                const size_t at = ((size_t)ry * dst[2] + rx) * 3;
                int dF = 0, d8 = 0;
                for (int c = 0; c < 3; ++c)
                {
                    const int a = (int)got[2 - c] - (int)want[at + c], b = (int)got[2 - c] - (int)want8[at + c];
                    dF = std::max(dF, a < 0 ? -a : a);
                    d8 = std::max(d8, b < 0 ? -b : b);
                }
                const int diff = std::min(dF, d8);
                over1 += diff > 1;
                if (diff > maxDiff)
                {
                    maxDiff = diff;
                    worstX = x;
                    worstY = y;
                }
                // The last output row/column comes from the source's last
                // row/column (green): no stale or missing border.
                if ((rx == dst[2] - 1 || ry == dst[3] - 1) && !(got[1] > 120 && got[2] < 140))
                    ++borderBad;
            }
        backCopy->UnlockRect();
        // Off the 1/8 grid of the 0.75 ratio (1120/1280, 544/720, the
        // letterbox rectangle) the texture unit's fixed-point weights and
        // the float reference differ by an LSB; SGSR's edge correction and
        // RCAS amplify that at the pattern's 1-px black lines (the observed
        // maximum is 20 at a handful of pixels). A stale texel, wrong UV scale or
        // clamp shows as magenta, a border miss or thousands of pixels off.
        const int maxAllowed = exact ? 0 : mode == deko9::UPSCALE_BILINEAR ? 2 : 24;
        const uint32_t overAllowed = exact ? 0 : (uint32_t)(dst[2] * dst[3] / 1000);
        char detail[200];
        std::snprintf(detail, sizeof(detail),
                      "max_diff=%d at (%d,%d) pixels_over_1=%u magenta=%u border_bad=%u outside_changed=%u", maxDiff,
                      worstX, worstY, (unsigned)over1, (unsigned)magenta, (unsigned)borderBad,
                      (unsigned)outsideBad);
        Check(maxDiff <= maxAllowed && over1 <= overAllowed && !magenta && !borderBad && !outsideBad, name, detail);
    };

    Deko9_SetUpscaleSharpness(device, kSharpness);
    dynres::Config cfg; // 1280x720, the engine's quantisation
    for (const float scale : {1.0f, 0.875f, 0.75f, 0.625f})
    {
        const dynres::Size size = dynres::SizeForLevel(cfg, dynres::LevelForScale(cfg, scale));
        const int w = size.width, h = size.height;
        char name[96], detail[128];
        std::vector<uint32_t> px((size_t)w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                px[(size_t)y * w + x] = DynPattern(x, y, w, h);
        D3DSURFACE_DESC desc{};
        const bool resized = fillMagentaFull() && Deko9_ResizeRenderTarget(device, scene, w, h) &&
                             Deko9_ResizeRenderTarget(device, depth, w, h) &&
                             SUCCEEDED(scene->GetLevelDesc(0, &desc)) && (int)desc.Width == w &&
                             (int)desc.Height == h && SUCCEEDED(depth->GetDesc(&desc)) && (int)desc.Width == w &&
                             (int)desc.Height == h;
        std::snprintf(name, sizeof(name), "DYNRES_RESIZE_%dx%d", w, h);
        std::snprintf(detail, sizeof(detail), "desc=%ux%u", desc.Width, desc.Height);
        Check(resized, name, detail);
        if (!resized)
            continue;

        // The resized target renders at its new size: clear it and its
        // depth, then a scissored clear of the bottom-right 4x4 corner.
        {
            IDirect3DSurface9 *rtCopy = nullptr;
            D3DLOCKED_RECT locked;
            const D3DRECT corner = {w - 4, h - 4, w, h};
            bool rendered =
                SUCCEEDED(device->SetRenderTarget(0, sceneSurface)) && SUCCEEDED(device->SetDepthStencilSurface(depth)) &&
                SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                                        D3DCOLOR_XRGB(10, 20, 30), 1.0f, 0)) &&
                SUCCEEDED(device->Clear(1, &corner, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 250, 0), 1.0f, 0)) &&
                SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &rtCopy,
                                                              nullptr)) &&
                SUCCEEDED(device->GetRenderTargetData(sceneSurface, rtCopy)) &&
                SUCCEEDED(rtCopy->LockRect(&locked, nullptr, D3DLOCK_READONLY));
            uint32_t bad = 0;
            if (rendered)
            {
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        uint32_t v;
                        std::memcpy(&v, static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch + x * 4, 4);
                        const uint32_t want = x >= w - 4 && y >= h - 4 ? 0x00fa00u : 0x0a141eu;
                        bad += (v & 0xffffff) != want;
                    }
                rtCopy->UnlockRect();
            }
            if (rtCopy)
                rtCopy->Release();
            device->SetDepthStencilSurface(nullptr);
            std::snprintf(name, sizeof(name), "DYNRES_RENDER_%dx%d", w, h);
            std::snprintf(detail, sizeof(detail), "wrong_pixels=%u", (unsigned)bad);
            Check(rendered && !bad, name, detail);
        }

        // Pattern over stale magenta, then every mode into the back buffer.
        const bool filled = fillMagentaFull() && Deko9_ResizeRenderTarget(device, scene, w, h) && upload(w, h, px);
        std::snprintf(name, sizeof(name), "DYNRES_UPLOAD_%dx%d", w, h);
        Check(filled, name);
        const deko9::fsrref::Image in = toImage(w, h, px);
        const int full[4] = {0, 0, kOutW, kOutH};
        for (uint32_t mode = 0; mode < deko9::UPSCALE_MODE_COUNT; ++mode)
        {
            Deko9_SetUpscaleMode(device, mode);
            const bool ran = filled && Deko9_UpscaleSurface(device, scene, nullptr, back, nullptr);
            std::snprintf(name, sizeof(name), "DYNRES_UPSCALE_%dx%d_%s", w, h, deko9::UpscaleModeName(mode));
            if (!ran)
            {
                Check(false, name, "Deko9_UpscaleSurface failed");
                continue;
            }
            // Full size: the equal-size copy, bit-exact whatever the mode
            // (the 1:1 bilinear reference is the input itself).
            const bool copy = w == kOutW && h == kOutH;
            compare(name, in, copy ? (uint32_t)deko9::UPSCALE_BILINEAR : mode, full, copy, 0);
            for (int frame = 0; timing && frame < 62; ++frame)
            {
                Deko9_UpscaleSurface(device, scene, nullptr, back, nullptr);
                device->Present(nullptr, nullptr, nullptr, nullptr);
            }
        }
    }

    // Letterboxed view: an 800x450 source rectangle at (64,32) of the
    // 1280x720 texture (magenta around it) into a 960x540 rectangle at
    // (100,60) of the back buffer, which is blue elsewhere.
    {
        const int sx = 64, sy = 32, sw = 800, sh = 450;
        std::vector<uint32_t> px((size_t)kOutW * kOutH, 0xffff00ffu);
        std::vector<uint32_t> crop((size_t)sw * sh);
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x)
                px[(size_t)(sy + y) * kOutW + sx + x] = crop[(size_t)y * sw + x] = DynPattern(x, y, sw, sh);
        const bool filled = Deko9_ResizeRenderTarget(device, scene, kOutW, kOutH) && upload(kOutW, kOutH, px);
        const deko9::fsrref::Image in = toImage(sw, sh, crop);
        const int src[4] = {sx, sy, sw, sh}, dst[4] = {100, 60, 960, 540};
        for (uint32_t mode = 0; mode < deko9::UPSCALE_MODE_COUNT; ++mode)
        {
            Deko9_SetUpscaleMode(device, mode);
            const bool ran = filled && SUCCEEDED(device->SetRenderTarget(0, back)) &&
                             SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0)) &&
                             Deko9_UpscaleSurface(device, scene, src, back, dst);
            char name[96];
            std::snprintf(name, sizeof(name), "DYNRES_SOURCE_RECT_%s", deko9::UpscaleModeName(mode));
            if (!ran)
            {
                Check(false, name, "upscale failed");
                continue;
            }
            compare(name, in, mode, dst, false, 0x0000ffu);
        }
    }
    // An out-of-range rectangle fails loudly instead of sampling garbage.
    {
        const int bad[4] = {0, 0, kOutW + 16, kOutH};
        Check(!Deko9_UpscaleSurface(device, scene, bad, back, nullptr), "DYNRES_REJECTS_BAD_RECT");
    }
    for (IUnknown *object : {(IUnknown *)sceneSurface, (IUnknown *)scene, (IUnknown *)depth, (IUnknown *)back,
                             (IUnknown *)backCopy})
    {
        if (object)
            object->Release();
    }
    device->Release();
}

// Block-compressed upload + sample round trip across size classes. Every 4x4
// block is a solid colour keyed by its block coordinates, so a layout or
// pitch mismatch for one size class shows as misplaced or missing blocks.
void RunBlockTextureTests()
{
    struct Case
    {
        UINT w, h;
        D3DFORMAT format;
        const char *name;
    };
    const Case cases[] = {
        {128, 32, D3DFMT_DXT5, "BC3_128x32"}, {128, 64, D3DFMT_DXT5, "BC3_128x64"},
        {64, 64, D3DFMT_DXT5, "BC3_64x64"},   {256, 64, D3DFMT_DXT5, "BC3_256x64"},
        {128, 16, D3DFMT_DXT5, "BC3_128x16"}, {256, 128, D3DFMT_DXT5, "BC3_256x128"},
        {64, 32, D3DFMT_DXT5, "BC3_64x32"},   {256, 32, D3DFMT_DXT5, "BC3_256x32"},
        {32, 32, D3DFMT_DXT5, "BC3_32x32"},   {128, 24, D3DFMT_DXT5, "BC3_128x24"},
        {128, 48, D3DFMT_DXT5, "BC3_128x48"}, {512, 32, D3DFMT_DXT5, "BC3_512x32"},
        {64, 128, D3DFMT_DXT5, "BC3_64x128"}, {128, 32, D3DFMT_DXT1, "BC1_128x32"}, {128, 32, D3DFMT_DXT3, "BC2_128x32"},
    };
    for (const Case &c : cases)
    {
        const UINT bytes = c.format == D3DFMT_DXT1 ? 8 : 16;
        const UINT bw = c.w / 4, bh = c.h / 4;
        IDirect3DTexture9 *tex = nullptr;
        D3DLOCKED_RECT locked;
        const bool made = SUCCEEDED(g_device->CreateTexture(c.w, c.h, 1, 0, c.format, D3DPOOL_MANAGED, &tex, nullptr)) &&
                          SUCCEEDED(tex->LockRect(0, &locked, nullptr, 0));
        if (!made)
        {
            Check(false, c.name, "create/lock failed");
            continue;
        }
        auto r5 = [](UINT bx) { return bx & 31; };
        auto g6 = [](UINT by) { return (by * 4 + 1) & 63; };
        for (UINT by = 0; by < bh; ++by)
        {
            for (UINT bx = 0; bx < bw; ++bx)
            {
                uint8_t *blk = static_cast<uint8_t *>(locked.pBits) + by * locked.Pitch + bx * bytes;
                std::memset(blk, 0, bytes);
                uint8_t *color = blk + (bytes - 8);
                if (c.format == D3DFMT_DXT5)
                    blk[0] = blk[1] = 255;
                else if (c.format == D3DFMT_DXT3)
                    std::memset(blk, 0xff, 8);
                const uint16_t c565 = (uint16_t)(r5(bx) << 11 | g6(by) << 5 | 0x1f);
                color[0] = color[2] = (uint8_t)(c565 & 0xff);
                color[1] = color[3] = (uint8_t)(c565 >> 8);
            }
        }
        tex->UnlockRect(0);
        bool ok = true;
        char detail[96] = "";
        for (UINT wy = 0; wy * kSize < c.h && ok; ++wy)
        {
            for (UINT wx = 0; wx * kSize < c.w && ok; ++wx)
            {
                const UINT pw = std::min<UINT>(kSize, c.w - wx * kSize), ph = std::min<UINT>(kSize, c.h - wy * kSize);
                const float u0 = (float)(wx * kSize) / c.w, u1 = (float)(wx * kSize + pw) / c.w;
                const float v0 = (float)(wy * kSize) / c.h, v1 = (float)(wy * kSize + ph) / c.h;
                const float x1 = -1.0f + 2.0f * pw / kSize, y1 = 1.0f - 2.0f * ph / kSize;
                const Vertex v[6] = {
                    {-1, 1, 0.5f, 1, u0, v0}, {x1, 1, 0.5f, 1, u1, v0}, {-1, y1, 0.5f, 1, u0, v1},
                    {x1, 1, 0.5f, 1, u1, v0}, {x1, y1, 0.5f, 1, u1, v1}, {-1, y1, 0.5f, 1, u0, v1},
                };
                BeginPass();
                g_device->SetPixelShader(g_psTexture);
                g_device->SetTexture(0, tex);
                g_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                g_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                g_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                g_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                g_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                g_device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
                g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(Vertex));
                uint32_t px[kSize * kSize];
                if (!Read(px))
                {
                    ok = false;
                    std::snprintf(detail, sizeof(detail), "readback failed");
                    break;
                }
                for (UINT y = 2; y < ph && ok; y += 4)
                {
                    for (UINT x = 2; x < pw; x += 4)
                    {
                        const UINT bx = (wx * kSize + x) / 4, by = (wy * kSize + y) / 4;
                        const uint32_t r5v = r5(bx), g6v = g6(by);
                        const uint32_t want = 0xff000000u | ((r5v << 3 | r5v >> 2) << 16) | ((g6v << 2 | g6v >> 4) << 8) |
                                              0xff;
                        if (!Near(px[y * kSize + x], want))
                        {
                            std::snprintf(detail, sizeof(detail), "block(%u,%u)=%08x want %08x", bx, by,
                                          (unsigned)px[y * kSize + x], (unsigned)want);
                            ok = false;
                            break;
                        }
                    }
                }
            }
        }
        Check(ok, c.name, detail);
        tex->Release();
    }
}

} // namespace

// taau_test.cpp
// vs_2_0: dcl_position v0; dcl_texcoord v1; mov oPos, v0; mov oT0.xy, v1;
// mov oT0.zw, c0 (c0.z is the shadow-map compare reference)
const DWORD kVsShadowRef[] = {
    0xFFFE0200,
    0x0200001F, 0x80000000, 0x900F0000,
    0x0200001F, 0x80000005, 0x900F0001,
    0x02000001, 0xC00F0000, 0x90E40000,
    0x02000001, 0xE0030000, 0x90E40001,
    0x02000001, 0xE00C0000, 0xA0E40000,
    0x0000FFFF,
};

// Hardware shadow maps, as the engine uses them (R_InitShadowmap and the
// sun/spot shadow passes): the caps the engine checks must select the
// depth-texture path, a depth-only pass writes a D24S8 texture, and a texld
// of that texture returns the depth comparison against texcoord.z (D3D9 on
// NVIDIA: 1 = lit when ref <= stored depth), not the raw depth.
void RunShadowMapTests(IDirect3D9 *d3d)
{
    // R_InitShadowmap tries D24S8 with these colour formats in order; any
    // match selects the depth path, none falls back to an R32F colour map.
    bool caps = false;
    for (const D3DFORMAT color : {D3DFMT_R5G6B5, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8})
        caps |= SUCCEEDED(d3d->CheckDepthStencilMatch(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, color, D3DFMT_D24S8)) &&
                SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
                                                 D3DRTYPE_TEXTURE, D3DFMT_D24S8));
    Check(caps, "SHADOWMAP_CAPS");

    IDirect3DTexture9 *shadow = nullptr;
    IDirect3DSurface9 *shadowSurface = nullptr;
    IDirect3DVertexShader9 *vsRef = nullptr;
    const bool made = SUCCEEDED(g_device->CreateTexture(kSize, kSize, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8,
                                                        D3DPOOL_DEFAULT, &shadow, nullptr)) &&
                      SUCCEEDED(shadow->GetSurfaceLevel(0, &shadowSurface)) &&
                      SUCCEEDED(g_device->CreateVertexShader(kVsShadowRef, &vsRef));
    Check(made, "SHADOWMAP_CREATE");
    if (!made)
        return;

    // Caster pass: depth 0.25 in the left half, cleared 1.0 elsewhere.
    BeginPass();
    g_device->SetDepthStencilSurface(shadowSurface);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    g_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    g_device->SetPixelShader(g_psColor);
    SetColor(0, 1, 0, 1);
    DrawQuad(-1, 1, 0, -1, 0.25f);
    g_device->SetRenderState(D3DRS_ZENABLE, FALSE);

    // Receiver pass: sample the map with linear filtering (the engine's
    // shadow sampler state) at two reference depths.
    uint32_t px[kSize * kSize];
    // The comparison result fills all four channels, alpha included.
    const uint32_t white = 0xffffffff, black = 0x00000000;
    BeginPass();
    g_device->SetDepthStencilSurface(nullptr);
    g_device->SetVertexShader(vsRef);
    g_device->SetPixelShader(g_psTexture);
    g_device->SetTexture(0, shadow);
    g_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    const float refMid[4] = {0, 0, 0.5f, 1};
    g_device->SetVertexShaderConstantF(0, refMid, 1);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff0000ff, 1.0f, 0);
    DrawQuad(-1, 1, 1, -1);
    Read(px);
    ExpectPixels("SHADOWMAP_COMPARE", px, {{P(8, 8), black}, {P(24, 56), black}, {P(40, 8), white}, {P(56, 56), white}});
    const float refNear[4] = {0, 0, 0.1f, 1};
    g_device->SetVertexShaderConstantF(0, refNear, 1);
    DrawQuad(-1, 1, 1, -1);
    Read(px);
    ExpectPixels("SHADOWMAP_COMPARE_LIT", px, {{P(8, 8), white}, {P(56, 56), white}});

    g_device->SetTexture(0, nullptr);
    g_device->SetVertexShader(g_vs);
    vsRef->Release();
    shadowSurface->Release();
    shadow->Release();
}

void RunTaauTests(IDirect3D9 *d3d, HWND window, void (*check)(bool, const char *, const char *));

int main(int, char **)
{
    bool sockets = false;
    if (KISAK_SWITCH_LOG_HOST[0] && inet_pton(AF_INET, KISAK_SWITCH_LOG_HOST, &__nxlink_host) == 1 &&
        R_SUCCEEDED(socketInitializeDefault()))
    {
        sockets = true;
        nxlinkStdio();
    }
    Emit("DEKO9_SELFTEST_START");
    // Every render/depth target (back buffer included) is compressed unless
    // a test says otherwise (r_deko9RtCompression 1 in the game).
    Deko9_SetRtCompression(true);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS params{};
    params.BackBufferWidth = 1280;
    params.BackBufferHeight = 720;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 3;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    HWND window = reinterpret_cast<HWND>(nwindowGetDefault());
    params.hDeviceWindow = window;
    const bool created = d3d && SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
                                                            D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &g_device));
    Check(created, "DEVICE");
    if (created)
    {
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END(),
        };
        const bool setup =
            SUCCEEDED(g_device->CreateVertexShader(kVs, &g_vs)) &&
            SUCCEEDED(g_device->CreatePixelShader(kPsColor, &g_psColor)) &&
            SUCCEEDED(g_device->CreatePixelShader(kPsTexture, &g_psTexture)) &&
            SUCCEEDED(g_device->CreatePixelShader(kPsSparse, &g_psSparse)) &&
            SUCCEEDED(g_device->CreateVertexShader(kVsInstanced, &g_vsInstanced)) &&
            SUCCEEDED(g_device->CreatePixelShader(kPsVarying, &g_psVarying)) &&
            SUCCEEDED(g_device->CreateVertexDeclaration(elements, &g_decl)) &&
            SUCCEEDED(g_device->CreateTexture(kSize, kSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                              D3DPOOL_DEFAULT, &g_target, nullptr)) &&
            SUCCEEDED(g_target->GetSurfaceLevel(0, &g_targetSurface)) &&
            SUCCEEDED(g_device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                            &g_readback, nullptr));
        Check(setup, "SETUP");
        if (setup)
            Check(Deko9_IsCompressed(g_targetSurface), "COMPRESSED_TARGET");
        if (setup)
            RunTests();
        if (setup)
            RunQueryTests();
        if (setup)
            RunGatherProbeTests();
        if (setup)
            RunDirtyBoxTests();
        if (setup)
            RunEarlyZTests();
        if (setup)
            RunPerDrawTests();
        if (setup)
            RunReuploadOrderTests();
        if (setup)
            RunMoveTests();
        if (setup)
            RunBlockTextureTests();
        if (setup)
        {
            // The native passes run with the black box on: their records
            // and crumbs must not change what they render.
            RunFaultTraceTests();
            RunCmdChunkTests();
            RunFloatZTests(true);
            RunFloatZTests(false);
            RunHrpTests();
            RunShadowMapTests(d3d);
            Deko9_SetFaultTrace(g_device, 0);
            g_device->Present(nullptr, nullptr, nullptr, nullptr);
        }
        // The FSR device replaces this one (one swapchain per window). The
        // test's own objects are left unreleased: they never touch the
        // device again.
        g_device->Release();
        g_device = nullptr;
        // Both back-buffer modes: compressed (r_deko9RtCompression 1) and
        // plain (the default). Hardware's gather fault showed in both;
        // an emulator matched in both.
        for (const bool compression : {true, false})
        {
            Deko9_SetRtCompression(compression);
            g_checkSuffix = compression ? "" : "_NOCOMP";
            RunFsrTests(d3d, window, compression);
            RunDynResTests(d3d, window, compression);
        }
        g_checkSuffix = "";
        Deko9_SetRtCompression(true);
        RunTaauTests(d3d, window, [](bool ok, const char *name, const char *detail) { Check(ok, name, detail); });
    }
    Emit(g_failures || !created ? "FAIL:DEKO9_SELFTEST" : "PASS:DEKO9_SELFTEST");
    Emit("DEKO9_SELFTEST_END");
    if (sockets)
        socketExit();
    return g_failures ? 1 : 0;
}
