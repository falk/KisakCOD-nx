// deko9 self-test, TAAU (Deko9_TaauResolve, r_taau): the resolve runs on
// synthetic frames at the engine's 0.75 dynres size into a 1280x720 back
// buffer. Each frame point-samples an analytic, aliasing-prone pattern
// (rotated 5 px checker, thin lines) at the jittered sample positions a
// rasteriser would use. Checks, against a 4x4-supersampled 1280x720
// reference (mean absolute error, 8-bit units):
//   TAAU_CONVERGES   a static scene over two Halton cycles ends closer to the
//                    reference than its first (reset) frame and than SGSR of
//                    the unjittered frame at the same input size;
//   TAAU_PAN         the camera pans 8x4 output px per frame: with the
//                    matching reprojection the error stays near the static
//                    one, and below the same pan with an identity matrix;
//   TAAU_PAN_SUBPIXEL  the same at 3.37x1.71 px per frame: the Catmull-Rom
//                    history stays below a reset frame's error and below
//                    the bilinear variant's;
//   TAAU_RESET       a reset frame ignores the history: the same input after
//                    two different histories gives bit-identical output;
//   TAAU_VARIANTS    every program variant (Catmull-Rom/bilinear history x
//                    kernel/bilinear current) converges below SGSR and pans
//                    below a reset frame; the detail line carries each one's
//                    converged error, frame-to-frame change and pan error;
//   TAAU_MOTION      a textured square moves over the static pattern (its
//                    depth in front, drawn into the motion texture by
//                    Deko9_TaauMotion): the error around it (its current and
//                    previous footprint) is lower with the motion pass than
//                    with depth reprojection alone;
//   TAAU_MOTION_REJECT  the square drawn as reject resolves to the current
//                    frame alone (bit-identical to a reset frame inside it);
//   TAAU_REACTIVE    a fast spark blended over the static pattern between
//                    the two reactive passes (Deko9_TaauOpaque): the error
//                    around it is lower with reactive weighting than
//                    without, and with nothing blended (but the frame graded
//                    after the transparents, as post effects do) the passes
//                    change no output bit (TAAU_REACTIVE_NEUTRAL); the
//                    half-size mask does the same (TAAU_REACTIVE_HALF);
//   TAAU_STABILITY   converged static frames change less from one frame to
//                    the next with anti-flicker (0.5) than without;
//   TAAU_DYNRES      after converging at 0.75, a frame at a smaller render
//                    size keeps the history: closer to the reference than
//                    a reset frame at that size;
//   TAAU_CONVERGE_CURVE_050/060/075  64 static frames after a reset at scale
//                    0.5/0.6/0.75: error against the converged frame at
//                    frames 1..32 (mostly gone by frame 8) and the
//                    frame-to-frame change of converged frames (below 1.5);
//   TAAU_REVEAL_EDGE a one-frame 32 px pan: the column it brings in (no
//                    history) converges like a reset (frame 4 well below 1);
//   TAAU_REVEAL_OCCLUDER  the square jumps 32 px: the strip it uncovers,
//                    error at frames 1, 4, 16 after the move;
//   TAAU_REJECTS_SIZE_MISMATCH  colour and depth of different sizes fail;
// plus 64 more resolves so the device logs a `DEKO9 taau frames=60` line.

#include <switch.h>

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../../../src/deko9/deko9_fsr.h"
#include "../../../../src/deko9/deko9_native.h"
#include "../../../../src/deko9/deko9_taau.h"
#include "../../../../src/gfx_d3d/r_dynres_controller.h"

namespace
{
constexpr int kOutW = 1280, kOutH = 720;

struct Rgb
{
    float r, g, b;
};

// The pattern at output pixel coordinates (x, y), shifted by (ox, oy).
Rgb Pattern(double x, double y, double ox, double oy)
{
    x -= ox;
    y -= oy;
    const double c = 0.9393727128, s = 0.3428978075; // 0.35 rad
    const double rx = x * c + y * s, ry = -x * s + y * c;
    const bool checker = (((int)std::floor(rx * 0.2) + (int)std::floor(ry * 0.2)) & 1) != 0;
    double fr = rx * (1.0 / 23.0);
    fr -= std::floor(fr);
    const bool line = std::fabs(fr - 0.5) < 0.04;
    return {checker ? 0.85f : 0.15f, line ? 0.95f : (checker ? 0.45f : 0.2f), checker ? 0.7f : 0.3f};
}

// A moving square: diagonal stripes in its own coordinates.
constexpr double kObjSize = 96.0;
bool InObject(double x, double y, double objX, double objY)
{
    return x >= objX && x < objX + kObjSize && y >= objY && y < objY + kObjSize;
}
Rgb ObjectPattern(double x, double y, double objX, double objY)
{
    double fr = ((x - objX) + (y - objY)) * (1.0 / 7.0);
    fr -= std::floor(fr);
    return fr < 0.5 ? Rgb{0.95f, 0.3f, 0.1f} : Rgb{0.1f, 0.2f, 0.9f};
}
Rgb Scene(double x, double y, double objX, double objY)
{
    return InObject(x, y, objX, objY) ? ObjectPattern(x, y, objX, objY) : Pattern(x, y, 0.0, 0.0);
}

uint32_t Pack(const Rgb &c)
{
    const auto q = [](float v) { return (uint32_t)std::lround(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f); };
    return 0xff000000u | q(c.r) << 16 | q(c.g) << 8 | q(c.b);
}

// A spark: a disc blended at 70% over whatever is under it.
constexpr double kSparkRadius = 18.0;
Rgb WithSpark(Rgb under, double x, double y, double sparkX, double sparkY)
{
    const double dx = x - sparkX, dy = y - sparkY;
    if (dx * dx + dy * dy > kSparkRadius * kSparkRadius)
        return under;
    return {0.3f * under.r + 0.7f * 1.0f, 0.3f * under.g + 0.7f * 0.75f, 0.3f * under.b + 0.7f * 0.3f};
}

// 4x4 supersampled reference of the background and the square at (objX,
// objY), 0..255 per channel.
std::vector<float> ReferenceObject(double objX, double objY, double sparkX = -1e9, double sparkY = -1e9)
{
    std::vector<float> ref((size_t)kOutW * kOutH * 3);
    for (int y = 0; y < kOutH; ++y)
        for (int x = 0; x < kOutW; ++x)
        {
            Rgb sum{0, 0, 0};
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                {
                    const double px = x + (sx + 0.5) / 4.0, py = y + (sy + 0.5) / 4.0;
                    const Rgb c = WithSpark(Scene(px, py, objX, objY), px, py, sparkX, sparkY);
                    sum.r += c.r;
                    sum.g += c.g;
                    sum.b += c.b;
                }
            float *o = &ref[((size_t)y * kOutW + x) * 3];
            o[0] = sum.r * (255.0f / 16.0f);
            o[1] = sum.g * (255.0f / 16.0f);
            o[2] = sum.b * (255.0f / 16.0f);
        }
    return ref;
}

// 4x4 supersampled reference, 0..255 per channel (R, G, B).
std::vector<float> Reference(double ox, double oy)
{
    std::vector<float> ref((size_t)kOutW * kOutH * 3);
    for (int y = 0; y < kOutH; ++y)
        for (int x = 0; x < kOutW; ++x)
        {
            Rgb sum{0, 0, 0};
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                {
                    const Rgb c = Pattern(x + (sx + 0.5) / 4.0, y + (sy + 0.5) / 4.0, ox, oy);
                    sum.r += c.r;
                    sum.g += c.g;
                    sum.b += c.b;
                }
            float *o = &ref[((size_t)y * kOutW + x) * 3];
            o[0] = sum.r * (255.0f / 16.0f);
            o[1] = sum.g * (255.0f / 16.0f);
            o[2] = sum.b * (255.0f / 16.0f);
        }
    return ref;
}

} // namespace

void RunTaauTests(IDirect3D9 *d3d, HWND window, void (*check)(bool, const char *, const char *))
{
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
    check(created, "TAAU_DEVICE", "1280x720 back buffer");
    if (!created)
        return;
    dynres::Config cfg;
    const dynres::Size size = dynres::SizeForLevel(cfg, dynres::LevelForScale(cfg, 0.75f));
    const int w = size.width, h = size.height;
    IDirect3DTexture9 *scene = nullptr, *sys = nullptr;
    IDirect3DSurface9 *sceneSurface = nullptr, *depth = nullptr, *smallDepth = nullptr, *back = nullptr,
                      *backCopy = nullptr;
    bool ok = SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
                                              &scene, nullptr)) &&
              SUCCEEDED(scene->GetSurfaceLevel(0, &sceneSurface)) &&
              SUCCEEDED(device->CreateTexture(w, h, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
              SUCCEEDED(device->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth,
                                                          nullptr)) &&
              SUCCEEDED(device->CreateDepthStencilSurface(w / 2, h / 2, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                          &smallDepth, nullptr)) &&
              SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) &&
              SUCCEEDED(device->CreateOffscreenPlainSurface(kOutW, kOutH, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                            &backCopy, nullptr));
    // The scene depth: cleared to the far plane (sky everywhere).
    ok = ok && SUCCEEDED(device->SetRenderTarget(0, sceneSurface)) &&
         SUCCEEDED(device->SetDepthStencilSurface(depth)) &&
         SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0)) &&
         SUCCEEDED(device->SetDepthStencilSurface(nullptr)) && SUCCEEDED(device->SetRenderTarget(0, back));
    char detail[256];
    std::snprintf(detail, sizeof(detail), "scene %dx%d -> %dx%d", w, h, kOutW, kOutH);
    check(ok, "TAAU_SETUP", detail);
    if (!ok)
    {
        device->Release();
        return;
    }

    // Frame `index` of the pattern shifted by (ox, oy) output px, sampled at
    // the jittered positions: texel (i, j) holds the unjittered point
    // (i + 0.5 - jx, j + 0.5 - jy) in render px.
    const double sx = (double)kOutW / w, sy = (double)kOutH / h;
    const auto upload = [&](uint32_t index, double ox, double oy, deko9::TaauFrame *f) {
        deko9::TaauHalton(index, &f->jitter[0], &f->jitter[1]);
        D3DLOCKED_RECT locked;
        if (FAILED(sys->LockRect(0, &locked, nullptr, 0)))
            return false;
        for (int j = 0; j < h; ++j)
        {
            uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + j * locked.Pitch);
            const double py = (j + 0.5 - f->jitter[1]) * sy;
            for (int i = 0; i < w; ++i)
                row[i] = Pack(Pattern((i + 0.5 - f->jitter[0]) * sx, py, ox, oy));
        }
        sys->UnlockRect(0);
        return SUCCEEDED(device->UpdateTexture(sys, scene));
    };
    deko9::TaauFrame base{};
    for (int i = 0; i < 4; ++i)
        base.reproj[i][i] = 1.0f;
    base.viewmodelSplit = 0.015625f;
    base.sceneMinZ = 0.015625f;
    base.sceneMaxZ = 1.0f;
    base.farNdcZ = 0.99951172f;
    base.blend = 0.1f;
    base.antiFlicker = 0.5f;
    base.flat = deko9::kTaauFlatDefault / 255.0f;
    base.bilinearRange = deko9::kTaauBilinearRangeDefault / 255.0f;
    const int32_t src[4] = {0, 0, w, h}, dst[4] = {0, 0, kOutW, kOutH};
    bool ran = true;
    const auto resolve = [&](const deko9::TaauFrame &f) {
        ran = Deko9_TaauResolve(device, scene, depth, src, back, dst, &f) && ran;
    };
    const auto readBack = [&](std::vector<uint8_t> *out) {
        D3DLOCKED_RECT locked;
        if (FAILED(device->GetRenderTargetData(back, backCopy)) ||
            FAILED(backCopy->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            return false;
        out->resize((size_t)kOutW * kOutH * 3);
        for (int y = 0; y < kOutH; ++y)
        {
            const uint8_t *row = static_cast<const uint8_t *>(locked.pBits) + y * locked.Pitch;
            for (int x = 0; x < kOutW; ++x)
                for (int c = 0; c < 3; ++c)
                    (*out)[((size_t)y * kOutW + x) * 3 + c] = row[x * 4 + 2 - c]; // B,G,R,A -> R,G,B
        }
        backCopy->UnlockRect();
        return true;
    };
    const auto mae = [&](const std::vector<float> &ref) {
        std::vector<uint8_t> got;
        if (!readBack(&got))
            return 1e9;
        double sum = 0.0;
        for (size_t i = 0; i < got.size(); ++i)
            sum += std::fabs(got[i] - ref[i]);
        return sum / got.size();
    };

    // Static scene: reset, then two Halton cycles.
    const std::vector<float> ref0 = Reference(0.0, 0.0);
    double first = 0.0, cycle1 = 0.0, cycle2 = 0.0;
    for (uint32_t k = 0; k < 2 * deko9::kTaauPhases; ++k)
    {
        deko9::TaauFrame f = base;
        f.reset = k == 0;
        ran = upload(k, 0.0, 0.0, &f) && ran;
        resolve(f);
        if (k == 0)
            first = mae(ref0);
        if (k == deko9::kTaauPhases - 1)
            cycle1 = mae(ref0);
    }
    cycle2 = mae(ref0);
    // SGSR of the unjittered frame at the same input size.
    {
        // The unjittered grid.
        D3DLOCKED_RECT locked;
        ran = ran && SUCCEEDED(sys->LockRect(0, &locked, nullptr, 0));
        if (ran)
        {
            for (int j = 0; j < h; ++j)
            {
                uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + j * locked.Pitch);
                for (int i = 0; i < w; ++i)
                    row[i] = Pack(Pattern((i + 0.5) * sx, (j + 0.5) * sy, 0.0, 0.0));
            }
            sys->UnlockRect(0);
            ran = SUCCEEDED(device->UpdateTexture(sys, scene));
        }
    }
    Deko9_SetUpscaleMode(device, deko9::UPSCALE_SGSR);
    const bool sgsrRan = Deko9_UpscaleSurface(device, scene, nullptr, back, nullptr);
    const double sgsr = sgsrRan ? mae(ref0) : 1e9;
    Deko9_SetUpscaleMode(device, deko9::UPSCALE_BILINEAR);
    const bool bilinearRan = Deko9_UpscaleSurface(device, scene, nullptr, back, nullptr);
    const double bilinear = bilinearRan ? mae(ref0) : 1e9;
    Deko9_SetUpscaleMode(device, deko9::UPSCALE_SGSR);
    std::snprintf(detail, sizeof(detail), "mae first=%.2f cycle1=%.2f cycle2=%.2f sgsr=%.2f bilinear=%.2f", first,
                  cycle1, cycle2, sgsr, bilinear);
    check(ran && sgsrRan && bilinearRan && cycle2 < first * 0.9 && cycle2 < sgsr && cycle2 <= cycle1 * 1.05,
          "TAAU_CONVERGES", detail);

    // Pan: converge, then move 8x4 output px per frame for 8 frames, with the
    // matching reprojection and with none.
    double panGood = 0.0, panIdentity = 0.0;
    for (const bool matching : {true, false})
    {
        uint32_t k = 0;
        for (; k < 2 * deko9::kTaauPhases; ++k)
        {
            deko9::TaauFrame f = base;
            f.reset = k == 0;
            ran = upload(k, 0.0, 0.0, &f) && ran;
            resolve(f);
        }
        double ox = 0.0, oy = 0.0;
        for (int step = 0; step < 8; ++step, ++k)
        {
            ox += 8.0;
            oy += 4.0;
            deko9::TaauFrame f = base;
            // This frame's pixel p showed at p - (8, 4) in the last one: NDC
            // x - 16/1280, y + 8/720 (NDC y points up).
            if (matching)
            {
                f.reproj[3][0] = -16.0f / kOutW;
                f.reproj[3][1] = 8.0f / kOutH;
            }
            ran = upload(k, ox, oy, &f) && ran;
            resolve(f);
        }
        (matching ? panGood : panIdentity) = mae(Reference(ox, oy));
    }
    std::snprintf(detail, sizeof(detail), "mae matching=%.2f identity=%.2f static=%.2f first=%.2f", panGood,
                  panIdentity, cycle2, first);
    check(ran && panGood < panIdentity && panGood < first, "TAAU_PAN", detail);

    // Sub-pixel pan, bilinear and Catmull-Rom history.
    double subBilinear = 0.0, subCubic = 0.0;
    for (const bool cubic : {false, true})
    {
        uint32_t k = 0;
        for (; k < 2 * deko9::kTaauPhases; ++k)
        {
            deko9::TaauFrame f = base;
            f.bilinearHistory = !cubic;
            f.reset = k == 0;
            ran = upload(k, 0.0, 0.0, &f) && ran;
            resolve(f);
        }
        double ox = 0.0, oy = 0.0;
        for (int step = 0; step < 12; ++step, ++k)
        {
            ox += 3.37;
            oy += 1.71;
            deko9::TaauFrame f = base;
            f.bilinearHistory = !cubic;
            f.reproj[3][0] = (float)(-2.0 * 3.37 / kOutW);
            f.reproj[3][1] = (float)(2.0 * 1.71 / kOutH);
            ran = upload(k, ox, oy, &f) && ran;
            resolve(f);
        }
        (cubic ? subCubic : subBilinear) = mae(Reference(ox, oy));
    }
    std::snprintf(detail, sizeof(detail), "mae bilinear=%.2f cubic=%.2f first=%.2f", subBilinear, subCubic, first);
    check(ran && subCubic < first && subCubic < subBilinear, "TAAU_PAN_SUBPIXEL", detail);

    // Reset: the same input after two different histories.
    std::vector<uint8_t> afterA, afterB;
    {
        deko9::TaauFrame f = base;
        f.reset = true;
        ran = upload(3, 0.0, 0.0, &f) && ran;
        resolve(f); // history: the panned pattern
        const bool a = readBack(&afterA);
        for (uint32_t k = 0; k < deko9::kTaauPhases; ++k)
        {
            deko9::TaauFrame g = base;
            ran = upload(k, 20.0, 10.0, &g) && ran;
            resolve(g);
        }
        ran = upload(3, 0.0, 0.0, &f) && ran;
        resolve(f); // history: another shift
        const bool b = readBack(&afterB);
        size_t differ = 0;
        for (size_t i = 0; a && b && i < afterA.size(); ++i)
            differ += afterA[i] != afterB[i];
        std::snprintf(detail, sizeof(detail), "differing_bytes=%zu", differ);
        check(ran && a && b && differ == 0, "TAAU_RESET", detail);
    }

    // Program variants: converged error after two Halton cycles, the
    // frame-to-frame change over the third, then the sub-pixel pan error.
    // The cheaper variants trade sharpness, never the SGSR bound.
    {
        char line[400];
        int len = 0;
        bool good = true;
        for (uint32_t v = 0; v < deko9::kTaauResolveVariants; ++v)
        {
            const auto variantFrame = [&](uint32_t k) {
                deko9::TaauFrame f = base;
                f.bilinearHistory = (v & deko9::kTaauBilinearHistory) != 0;
                f.bilinearCurrent = (v & deko9::kTaauBilinearCurrent) != 0;
                f.reset = k == 0;
                return f;
            };
            double converged = 0.0, flicker = 0.0;
            std::vector<uint8_t> prevOut, out;
            uint32_t k = 0;
            for (; k < 3 * deko9::kTaauPhases; ++k)
            {
                deko9::TaauFrame f = variantFrame(k);
                ran = upload(k, 0.0, 0.0, &f) && ran;
                resolve(f);
                if (k + 1 == 2 * deko9::kTaauPhases)
                    converged = mae(ref0);
                if (k < 2 * deko9::kTaauPhases - 1)
                    continue;
                ran = readBack(&out) && ran;
                if (!prevOut.empty() && out.size() == prevOut.size())
                {
                    double sum = 0.0;
                    for (size_t i = 0; i < out.size(); ++i)
                        sum += std::abs((int)out[i] - (int)prevOut[i]);
                    flicker += sum / out.size() / deko9::kTaauPhases;
                }
                prevOut.swap(out);
            }
            double ox = 0.0, oy = 0.0;
            for (int step = 0; step < 12; ++step, ++k)
            {
                ox += 3.37;
                oy += 1.71;
                deko9::TaauFrame f = variantFrame(k);
                f.reproj[3][0] = (float)(-2.0 * 3.37 / kOutW);
                f.reproj[3][1] = (float)(2.0 * 1.71 / kOutH);
                ran = upload(k, ox, oy, &f) && ran;
                resolve(f);
            }
            const double pan = mae(Reference(ox, oy));
            len += std::snprintf(line + len, sizeof(line) - len, "%s%s/%s static=%.2f delta=%.3f pan=%.2f",
                                 len ? " " : "", v & deko9::kTaauBilinearHistory ? "bilinear" : "catmull",
                                 v & deko9::kTaauBilinearCurrent ? "bilinear" : "kernel", converged, flicker, pan);
            good = good && converged < sgsr && pan < first;
        }
        check(ran && good, "TAAU_VARIANTS", line);
    }

    // Moving square: 16 static frames, then 8 frames moving 6x2 output px.
    {
        IDirect3DVertexBuffer9 *quadVb = nullptr;
        IDirect3DIndexBuffer9 *quadIb = nullptr;
        bool made = SUCCEEDED(device->CreateVertexBuffer(4 * 12, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &quadVb,
                                                         nullptr)) &&
                    SUCCEEDED(device->CreateIndexBuffer(6 * 2, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &quadIb, nullptr));
        void *bits = nullptr;
        if (made && SUCCEEDED(quadVb->Lock(0, 0, &bits, 0)))
        {
            const float s = (float)kObjSize, corners[12] = {0, 0, 0.5f, s, 0, 0.5f, 0, s, 0.5f, s, s, 0.5f};
            std::memcpy(bits, corners, sizeof(corners));
            quadVb->Unlock();
        }
        else
            made = false;
        if (made && SUCCEEDED(quadIb->Lock(0, 0, &bits, 0)))
        {
            const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
            std::memcpy(bits, idx, sizeof(idx));
            quadIb->Unlock();
        }
        else
            made = false;
        // Object space is output pixels with z the window depth; the device
        // rasterises with D3D9 pixel centres, so the matrices take that
        // half texel back out to cover the texels the CPU frame fills.
        const double centre = 0.5 - 1.0 / 128.0;
        const auto objectToClip = [&](double objX, double objY, float m[4][4]) {
            std::memset(m, 0, 16 * sizeof(float));
            m[0][0] = 2.0f / kOutW;
            m[1][1] = -2.0f / kOutH;
            m[2][2] = 1.0f;
            m[3][0] = (float)(2.0 * objX / kOutW - 1.0 - 2.0 * centre / w);
            m[3][1] = (float)(1.0 - 2.0 * objY / kOutH + 2.0 * centre / h);
            m[3][3] = 1.0f;
        };
        // Frame with the square at (objX, objY): colour as rasterised at the
        // jittered sample positions, depth 0.5 under it, 1 elsewhere.
        const auto uploadObject = [&](uint32_t index, double objX, double objY, deko9::TaauFrame *f) {
            deko9::TaauHalton(index, &f->jitter[0], &f->jitter[1]);
            D3DLOCKED_RECT locked;
            if (FAILED(sys->LockRect(0, &locked, nullptr, 0)))
                return false;
            for (int j = 0; j < h; ++j)
            {
                uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + j * locked.Pitch);
                const double py = (j + 0.5 - f->jitter[1]) * sy;
                for (int i = 0; i < w; ++i)
                    row[i] = Pack(Scene((i + 0.5 - f->jitter[0]) * sx, py, objX, objY));
            }
            sys->UnlockRect(0);
            const D3DRECT rect{(LONG)std::ceil(objX / sx - 0.5 + f->jitter[0]),
                               (LONG)std::ceil(objY / sy - 0.5 + f->jitter[1]),
                               (LONG)std::ceil((objX + kObjSize) / sx - 0.5 + f->jitter[0]),
                               (LONG)std::ceil((objY + kObjSize) / sy - 0.5 + f->jitter[1])};
            return SUCCEEDED(device->UpdateTexture(sys, scene)) &&
                   SUCCEEDED(device->SetRenderTarget(0, sceneSurface)) &&
                   SUCCEEDED(device->SetDepthStencilSurface(depth)) &&
                   SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0)) &&
                   SUCCEEDED(device->Clear(1, &rect, D3DCLEAR_ZBUFFER, 0, 0.5f, 0)) &&
                   SUCCEEDED(device->SetDepthStencilSurface(nullptr)) && SUCCEEDED(device->SetRenderTarget(0, back));
        };
        const auto motion = [&](double objX, double objY, double prevX, double prevY, const deko9::TaauFrame &f,
                                bool reject) {
            deko9::TaauMotionDraw d{};
            d.vb = quadVb;
            d.ib = quadIb;
            d.indexCount = 6;
            d.stride = 12;
            d.vertexCount = 4;
            d.reject = reject;
            objectToClip(objX, objY, d.cur);
            objectToClip(prevX, prevY, d.prev);
            deko9::TaauMotionView view{};
            deko9::TaauJitterClip(f.jitter[0], f.jitter[1], (uint32_t)w, (uint32_t)h, &view.jitterClip[0],
                                  &view.jitterClip[1]);
            view.sceneDepth[1] = view.viewmodelDepth[1] = 1.0f;
            return Deko9_TaauMotion(device, depth, src, &view, &d, 1);
        };
        // Error over the square's current and previous footprints (+8 px).
        const auto regionMae = [&](const std::vector<float> &ref, double x0, double y0, double x1, double y1) {
            std::vector<uint8_t> got;
            if (!readBack(&got))
                return 1e9;
            double sum = 0.0;
            size_t n = 0;
            for (int y = std::max(0, (int)y0); y < std::min(kOutH, (int)y1); ++y)
                for (int x = std::max(0, (int)x0); x < std::min(kOutW, (int)x1); ++x)
                    for (int c = 0; c < 3; ++c, ++n)
                    {
                        const size_t i = ((size_t)y * kOutW + x) * 3 + c;
                        sum += std::fabs(got[i] - ref[i]);
                    }
            return n ? sum / n : 1e9;
        };
        double withMotion = 0.0, depthOnly = 0.0, objX = 0.0, objY = 0.0;
        const double startX = 400.0, startY = 300.0;
        for (const bool useMotion : {true, false})
        {
            objX = startX;
            objY = startY;
            uint32_t k = 0;
            for (; k < 2 * deko9::kTaauPhases; ++k)
            {
                deko9::TaauFrame f = base;
                f.reset = k == 0;
                made = uploadObject(k, objX, objY, &f) && made;
                resolve(f);
            }
            for (int step = 0; step < 8; ++step, ++k)
            {
                const double prevX = objX, prevY = objY;
                objX += 6.0;
                objY += 2.0;
                deko9::TaauFrame f = base;
                made = uploadObject(k, objX, objY, &f) && made;
                if (useMotion)
                    made = motion(objX, objY, prevX, prevY, f, false) && made;
                resolve(f);
            }
            (useMotion ? withMotion : depthOnly) =
                regionMae(ReferenceObject(objX, objY), objX - 6.0 - 8.0, objY - 2.0 - 8.0, objX + kObjSize + 8.0,
                          objY + kObjSize + 8.0);
        }
        std::snprintf(detail, sizeof(detail), "region_mae motion=%.2f depth_only=%.2f", withMotion, depthOnly);
        check(made && ran && withMotion < depthOnly * 0.8, "TAAU_MOTION", detail);

        // Reject: the square's pixels resolve as the current frame alone.
        std::vector<uint8_t> rejected, fresh;
        {
            deko9::TaauFrame f = base;
            made = uploadObject(5, objX + 6.0, objY + 2.0, &f) && made;
            made = motion(objX + 6.0, objY + 2.0, objX, objY, f, true) && made;
            resolve(f);
            const bool a = readBack(&rejected);
            f.reset = true;
            resolve(f);
            const bool b = readBack(&fresh);
            size_t differ = 0, n = 0;
            int worst = 0, wx = 0, wy = 0;
            for (int y = (int)objY + 2 + 4; a && b && y < (int)(objY + 2.0 + kObjSize) - 4; ++y)
                for (int x = (int)objX + 6 + 4; x < (int)(objX + 6.0 + kObjSize) - 4; ++x, ++n)
                    for (int c = 0; c < 3; ++c)
                    {
                        const size_t i = ((size_t)y * kOutW + x) * 3 + c;
                        differ += rejected[i] != fresh[i];
                        const int dd = std::abs((int)rejected[i] - (int)fresh[i]);
                        if (dd > worst)
                            worst = dd, wx = x, wy = y;
                    }
            const std::vector<float> refNow = ReferenceObject(objX + 6.0, objY + 2.0);
            double ea = 0, eb = 0;
            for (int y = (int)objY + 6; a && b && y < (int)(objY + kObjSize); ++y)
                for (int x = (int)objX + 10; x < (int)(objX + kObjSize); ++x)
                    for (int c = 0; c < 3; ++c)
                    {
                        const size_t i = ((size_t)y * kOutW + x) * 3 + c;
                        ea += std::fabs(rejected[i] - refNow[i]);
                        eb += std::fabs(fresh[i] - refNow[i]);
                    }
            std::snprintf(detail, sizeof(detail),
                          "pixels=%zu differing_bytes=%zu max_diff=%d at (%d,%d) err_rejected=%.0f err_fresh=%.0f made=%d",
                          n, differ, worst, wx, wy, ea, eb, (int)made);
            check(made && ran && a && b && n > 0 && differ == 0, "TAAU_MOTION_REJECT", detail);
        }
        // Back to the far plane for the remaining cases.
        made = SUCCEEDED(device->SetRenderTarget(0, sceneSurface)) &&
               SUCCEEDED(device->SetDepthStencilSurface(depth)) &&
               SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0)) &&
               SUCCEEDED(device->SetDepthStencilSurface(nullptr)) && SUCCEEDED(device->SetRenderTarget(0, back));
        check(made, "TAAU_MOTION_CLEANUP", "");

        // Ranges outside the buffers (offsets derived from the previous
        // frame): the pass skips those draws and still succeeds; a valid draw
        // next to them is still recorded.
        {
            deko9::TaauMotionDraw bad[3]{};
            for (deko9::TaauMotionDraw &d : bad)
            {
                d.vb = quadVb;
                d.ib = quadIb;
                d.indexCount = 6;
                d.stride = 12;
                d.vertexCount = 4;
                objectToClip(400.0, 300.0, d.cur);
                objectToClip(400.0, 300.0, d.prev);
            }
            // bad[0]: offset past the buffer; bad[1]: its last vertex past
            // the buffer; bad[2] stays valid.
            bad[0].vbOffset = 4 * 12;
            bad[1].vbOffset = 12;
            deko9::TaauMotionView view{};
            view.sceneDepth[1] = view.viewmodelDepth[1] = 1.0f;
            const bool skipped = Deko9_TaauMotion(device, depth, src, &view, bad, 2);
            const bool mixed = Deko9_TaauMotion(device, depth, src, &view, bad, 3);
            std::snprintf(detail, sizeof(detail), "all_bad_ok=%d mixed_ok=%d", (int)skipped, (int)mixed);
            check(skipped && mixed, "TAAU_MOTION_SKIP", detail);
        }

        // A 2D draw after a resolve that ran the motion pass: its vertex
        // constants (c4 = +0.5 in x) must still be the engine's, and the next
        // resolve must not see it in the history.
        {
            // vs_2_0: dcl_position v0; add oPos, v0, c4
            const DWORD kVsOffset[] = {0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000, 0x03000002,
                                       0xC00F0000, 0x90E40000, 0xA0E40004, 0x0000FFFF};
            // ps_2_0: mov oC0, c0
            const DWORD kPsColor[] = {0xFFFF0200, 0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF};
            const D3DVERTEXELEMENT9 elements[] = {
                {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
            IDirect3DVertexShader9 *vs = nullptr;
            IDirect3DPixelShader9 *ps = nullptr;
            IDirect3DVertexDeclaration9 *decl = nullptr;
            bool drew = SUCCEEDED(device->CreateVertexShader(kVsOffset, &vs)) &&
                        SUCCEEDED(device->CreatePixelShader(kPsColor, &ps)) &&
                        SUCCEEDED(device->CreateVertexDeclaration(elements, &decl));
            deko9::TaauFrame f = base;
            drew = uploadObject(8, 400.0, 300.0, &f) && drew;
            drew = motion(400.0, 300.0, 400.0, 300.0, f, false) && drew;
            resolve(f);
            const float offset[4] = {0.5f, 0, 0, 0}, magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
            const float quad[4][4] = {{-0.25f, 0.25f, 0.5f, 1}, {0.25f, 0.25f, 0.5f, 1},
                                      {-0.25f, -0.25f, 0.5f, 1}, {0.25f, -0.25f, 0.5f, 1}};
            if (drew)
            {
                device->SetRenderState(D3DRS_ZENABLE, FALSE);
                device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                device->SetVertexDeclaration(decl);
                device->SetVertexShader(vs);
                device->SetPixelShader(ps);
                device->SetVertexShaderConstantF(4, offset, 1);
                device->SetPixelShaderConstantF(0, magenta, 1);
                drew = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0])));
            }
            const auto magentaAt = [](const std::vector<uint8_t> &px, int x, int y) {
                const uint8_t *p = &px[((size_t)y * kOutW + x) * 3];
                return p[0] > 240 && p[1] < 16 && p[2] > 240;
            };
            std::vector<uint8_t> after, again;
            const bool a = readBack(&after);
            resolve(f);
            const bool b = readBack(&again);
            const bool survives = a && magentaAt(after, kOutW * 3 / 4, kOutH / 2) && !magentaAt(after, kOutW / 2, kOutH / 2) &&
                                  !magentaAt(after, 20, 20);
            const bool inHistory = b && magentaAt(again, kOutW * 3 / 4, kOutH / 2);
            std::snprintf(detail, sizeof(detail), "drew=%d survives=%d in_history=%d", (int)drew, (int)survives,
                          (int)inHistory);
            check(drew && ran && survives && !inHistory, "TAAU_OVERLAY", detail);
            device->SetVertexShader(nullptr);
            device->SetPixelShader(nullptr);
            for (IUnknown *object : {(IUnknown *)vs, (IUnknown *)ps, (IUnknown *)decl})
                if (object)
                    object->Release();
            made = SUCCEEDED(device->SetRenderTarget(0, sceneSurface)) &&
                   SUCCEEDED(device->SetDepthStencilSurface(depth)) &&
                   SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0)) &&
                   SUCCEEDED(device->SetDepthStencilSurface(nullptr)) && SUCCEEDED(device->SetRenderTarget(0, back));
        }

        // A moving occluder reveals background: the square holds for 16
        // frames, moves 32x0 output px in one frame (drawn into the motion
        // texture), then holds again. The 32 px wide strip it uncovered
        // reprojects by depth onto the square's old colours (clamped to the
        // background's range); its error at frames 1, 4 and 16 after the move.
        {
            const double x0 = 400.0, y0 = 300.0;
            uint32_t k = 0;
            for (; k < 2 * deko9::kTaauPhases; ++k)
            {
                deko9::TaauFrame f = base;
                f.reset = k == 0;
                made = uploadObject(k, x0, y0, &f) && made;
                resolve(f);
            }
            const std::vector<float> refMoved = ReferenceObject(x0 + 32.0, y0);
            double strip[3] = {};
            for (int step = 1; step <= 16; ++step, ++k)
            {
                deko9::TaauFrame f = base;
                made = uploadObject(k, x0 + 32.0, y0, &f) && made;
                if (step == 1)
                    made = motion(x0 + 32.0, y0, x0, y0, f, false) && made;
                resolve(f);
                if (step == 1 || step == 4 || step == 16)
                    strip[step == 1 ? 0 : step == 4 ? 1 : 2] = regionMae(refMoved, x0, y0, x0 + 32.0, y0 + kObjSize);
            }
            std::snprintf(detail, sizeof(detail), "revealed_mae f1=%.2f f4=%.2f f16=%.2f", strip[0], strip[1],
                          strip[2]);
            check(made && ran && strip[2] < strip[0], "TAAU_REVEAL_OCCLUDER", detail);
            made = SUCCEEDED(device->SetRenderTarget(0, sceneSurface)) &&
                   SUCCEEDED(device->SetDepthStencilSurface(depth)) &&
                   SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0)) &&
                   SUCCEEDED(device->SetDepthStencilSurface(nullptr)) && SUCCEEDED(device->SetRenderTarget(0, back));
        }
        if (quadVb)
            quadVb->Release();
        if (quadIb)
            quadIb->Release();
    }

    // Reactive: static frames, then a spark crossing 24 output px per frame.
    {
        const auto uploadSpark = [&](uint32_t index, double sparkX, double sparkY, bool spark,
                                     deko9::TaauFrame *f, float grade = 1.0f) {
            deko9::TaauHalton(index, &f->jitter[0], &f->jitter[1]);
            D3DLOCKED_RECT locked;
            if (FAILED(sys->LockRect(0, &locked, nullptr, 0)))
                return false;
            for (int j = 0; j < h; ++j)
            {
                uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + j * locked.Pitch);
                const double py = (j + 0.5 - f->jitter[1]) * sy;
                for (int i = 0; i < w; ++i)
                {
                    const double px = (i + 0.5 - f->jitter[0]) * sx;
                    const Rgb under = Pattern(px, py, 0.0, 0.0);
                    const Rgb c = spark ? WithSpark(under, px, py, sparkX, sparkY) : under;
                    row[i] = Pack({c.r * grade, c.g * grade, c.b * grade});
                }
            }
            sys->UnlockRect(0);
            return SUCCEEDED(device->UpdateTexture(sys, scene));
        };
        // One frame: the opaque scene, its snapshot, then the spark on top.
        bool snapped = true, reactiveHalf = false;
        // `grade` scales the whole frame after the transparents, as a post
        // effect would: it must not count as reactive.
        const auto frame = [&](uint32_t k, double sparkX, double sparkY, bool spark, float reactive,
                               float grade = 1.0f) {
            deko9::TaauFrame f = base;
            f.reset = k == 0;
            f.reactive = reactive;
            bool ok = true;
            if (reactive > 0.0f)
            {
                ok = uploadSpark(k, sparkX, sparkY, false, &f);
                snapped = Deko9_TaauOpaque(device, scene, src, false, reactiveHalf) && snapped;
            }
            ok = uploadSpark(k, sparkX, sparkY, spark, &f) && ok;
            if (reactive > 0.0f)
                snapped = Deko9_TaauOpaque(device, scene, src, true, reactiveHalf) && snapped;
            if (grade != 1.0f)
                ok = uploadSpark(k, sparkX, sparkY, spark, &f, grade) && ok;
            resolve(f);
            return ok;
        };
        bool made = true;
        std::vector<uint8_t> staticWith, staticWithout, staticHalf;
        double withReactive = 0.0, withHalf = 0.0, without = 0.0, sparkX = 0.0;
        const double sparkY = 380.0;
        // Full-size mask, half-size mask, no mask.
        for (int mode = 0; mode < 3; ++mode)
        {
            const float weight = mode < 2 ? 0.6f : 0.0f;
            reactiveHalf = mode == 1;
            uint32_t k = 0;
            for (; k < 2 * deko9::kTaauPhases; ++k)
                made = frame(k, 0.0, 0.0, false, weight, 0.85f) && made;
            made = readBack(mode == 0 ? &staticWith : mode == 1 ? &staticHalf : &staticWithout) && made;
            sparkX = 300.0;
            for (int step = 0; step < 8; ++step, ++k)
            {
                sparkX += 24.0;
                made = frame(k, sparkX, sparkY, true, weight, 0.85f) && made;
            }
            std::vector<float> ref = ReferenceObject(-1e9, -1e9, sparkX, sparkY);
            for (float &v : ref)
                v *= 0.85f;
            std::vector<uint8_t> got;
            made = readBack(&got) && made;
            double sum = 0.0;
            size_t n = 0;
            for (int y = (int)(sparkY - kSparkRadius) - 8; y < (int)(sparkY + kSparkRadius) + 8; ++y)
                for (int x = (int)(sparkX - 2 * 24.0 - kSparkRadius) - 8; x < (int)(sparkX + kSparkRadius) + 8; ++x)
                    for (int c = 0; c < 3; ++c, ++n)
                    {
                        const size_t i = ((size_t)y * kOutW + x) * 3 + c;
                        sum += std::fabs(got[i] - ref[i]);
                    }
            (mode == 0 ? withReactive : mode == 1 ? withHalf : without) = sum / n;
        }
        size_t differ = 0, differHalf = 0;
        for (size_t i = 0; i < staticWith.size() && i < staticWithout.size(); ++i)
            differ += staticWith[i] != staticWithout[i];
        for (size_t i = 0; i < staticHalf.size() && i < staticWithout.size(); ++i)
            differHalf += staticHalf[i] != staticWithout[i];
        std::snprintf(detail, sizeof(detail), "differing_bytes=%zu half=%zu", differ, differHalf);
        check(made && snapped && ran && staticWith.size() == staticWithout.size() &&
                  staticHalf.size() == staticWithout.size() && differ == 0 && differHalf == 0,
              "TAAU_REACTIVE_NEUTRAL", detail);
        std::snprintf(detail, sizeof(detail), "region_mae reactive=%.2f half=%.2f off=%.2f", withReactive, withHalf,
                      without);
        check(made && snapped && ran && withReactive < without * 0.9, "TAAU_REACTIVE", detail);
        check(made && snapped && ran && withHalf < without * 0.9, "TAAU_REACTIVE_HALF", detail);
    }

    // Stability: frame-to-frame change of converged static frames.
    {
        double flicker[2] = {0.0, 0.0}, error[2] = {0.0, 0.0};
        for (int pass = 0; pass < 2; ++pass)
        {
            std::vector<uint8_t> prevOut, out;
            for (uint32_t k = 0; k < 4 * deko9::kTaauPhases; ++k)
            {
                deko9::TaauFrame f = base;
                f.antiFlicker = pass == 0 ? 0.5f : 0.0f;
                f.reset = k == 0;
                ran = upload(k, 0.0, 0.0, &f) && ran;
                resolve(f);
                if (k < 3 * deko9::kTaauPhases)
                    continue;
                ran = readBack(&out) && ran;
                if (!prevOut.empty() && out.size() == prevOut.size())
                {
                    double sum = 0.0;
                    for (size_t i = 0; i < out.size(); ++i)
                        sum += std::abs((int)out[i] - (int)prevOut[i]);
                    flicker[pass] += sum / out.size() / (deko9::kTaauPhases - 1);
                }
                prevOut.swap(out);
            }
            error[pass] = mae(ref0);
        }
        std::snprintf(detail, sizeof(detail), "frame_delta anti=%.3f off=%.3f mae anti=%.2f off=%.2f sgsr=%.2f",
                      flicker[0], flicker[1], error[0], error[1], sgsr);
        check(ran && flicker[0] < flicker[1] * 0.9 && error[0] < sgsr, "TAAU_STABILITY", detail);
    }

    // Dynamic resolution: converge at 0.75, then one frame at 0.6.
    {
        const int w2 = (int)(kOutW * 0.6) & ~7, h2 = (int)(kOutH * 0.6) & ~7;
        const double sx2 = (double)kOutW / w2, sy2 = (double)kOutH / h2;
        const int32_t src2[4] = {0, 0, w2, h2};
        const auto uploadSmall = [&](uint32_t index, deko9::TaauFrame *f) {
            deko9::TaauHalton(index, &f->jitter[0], &f->jitter[1]);
            D3DLOCKED_RECT locked;
            if (FAILED(sys->LockRect(0, &locked, nullptr, 0)))
                return false;
            for (int j = 0; j < h2; ++j)
            {
                uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + j * locked.Pitch);
                const double py = (j + 0.5 - f->jitter[1]) * sy2;
                for (int i = 0; i < w2; ++i)
                    row[i] = Pack(Pattern((i + 0.5 - f->jitter[0]) * sx2, py, 0.0, 0.0));
            }
            sys->UnlockRect(0);
            return SUCCEEDED(device->UpdateTexture(sys, scene));
        };
        double kept = 0.0, fresh = 0.0;
        for (const bool keep : {true, false})
        {
            uint32_t k = 0;
            for (; k < 2 * deko9::kTaauPhases; ++k)
            {
                deko9::TaauFrame f = base;
                f.reset = k == 0;
                ran = upload(k, 0.0, 0.0, &f) && ran;
                resolve(f);
            }
            deko9::TaauFrame f = base;
            f.reset = !keep;
            ran = uploadSmall(k, &f) && ran;
            ran = Deko9_TaauResolve(device, scene, depth, src2, back, dst, &f) && ran;
            (keep ? kept : fresh) = mae(ref0);
        }
        std::snprintf(detail, sizeof(detail), "render %dx%d mae kept=%.2f reset=%.2f", w2, h2, kept, fresh);
        check(ran && kept < fresh, "TAAU_DYNRES", detail);
    }

    // Convergence after a reset at the adaptive controller's low scales,
    // each with its own whole-texture scene: 64 static frames; the error
    // against the 64th (converged) frame and the reference at frames 1..32,
    // and the frame-to-frame change over frames 17..24 (one Halton cycle).
    // A reset frame's aliasing must be mostly gone by frame 8, and a
    // converged static frame must not flicker by more than 1.5 units.
    for (const float scale : {0.5f, 0.6f, 0.75f})
    {
        const dynres::Size ss = dynres::SizeForLevel(cfg, dynres::LevelForScale(cfg, scale));
        const int cw = ss.width, chh = ss.height;
        const double csx = (double)kOutW / cw, csy = (double)kOutH / chh;
        IDirect3DTexture9 *cScene = nullptr, *cSys = nullptr;
        IDirect3DSurface9 *cSurface = nullptr, *cDepth = nullptr;
        bool made =
            SUCCEEDED(device->CreateTexture(cw, chh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
                                            &cScene, nullptr)) &&
            SUCCEEDED(cScene->GetSurfaceLevel(0, &cSurface)) &&
            SUCCEEDED(device->CreateTexture(cw, chh, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &cSys, nullptr)) &&
            SUCCEEDED(device->CreateDepthStencilSurface(cw, chh, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &cDepth,
                                                        nullptr)) &&
            SUCCEEDED(device->SetRenderTarget(0, cSurface)) && SUCCEEDED(device->SetDepthStencilSurface(cDepth)) &&
            SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0)) &&
            SUCCEEDED(device->SetDepthStencilSurface(nullptr)) && SUCCEEDED(device->SetRenderTarget(0, back));
        const int32_t cSrc[4] = {0, 0, cw, chh};
        constexpr int kFrames = 64;
        const int marks[6] = {1, 2, 4, 8, 16, 32};
        std::vector<std::vector<uint8_t>> at(6);
        std::vector<uint8_t> last, prevOut, out;
        double refAt[3] = {}, flicker = 0.0;
        for (int k = 0; made && k < kFrames; ++k)
        {
            deko9::TaauFrame f = base;
            f.reset = k == 0;
            deko9::TaauHalton((uint32_t)k, &f.jitter[0], &f.jitter[1]);
            D3DLOCKED_RECT locked;
            made = SUCCEEDED(cSys->LockRect(0, &locked, nullptr, 0));
            if (!made)
                break;
            for (int j = 0; j < chh; ++j)
            {
                uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(locked.pBits) + j * locked.Pitch);
                const double py = (j + 0.5 - f.jitter[1]) * csy;
                for (int i = 0; i < cw; ++i)
                    row[i] = Pack(Pattern((i + 0.5 - f.jitter[0]) * csx, py, 0.0, 0.0));
            }
            cSys->UnlockRect(0);
            made = SUCCEEDED(device->UpdateTexture(cSys, cScene)) &&
                   Deko9_TaauResolve(device, cScene, cDepth, cSrc, back, dst, &f) && readBack(&out);
            for (int m = 0; m < 6; ++m)
                if (k + 1 == marks[m])
                    at[m] = out;
            if (k + 1 == 1 || k + 1 == 8 || k + 1 == kFrames)
            {
                double sum = 0.0;
                for (size_t i = 0; i < out.size(); ++i)
                    sum += std::fabs(out[i] - ref0[i]);
                refAt[k + 1 == 1 ? 0 : k + 1 == 8 ? 1 : 2] = sum / out.size();
            }
            if (k + 1 > 16 && k + 1 <= 16 + (int)deko9::kTaauPhases && prevOut.size() == out.size())
            {
                double sum = 0.0;
                for (size_t i = 0; i < out.size(); ++i)
                    sum += std::abs((int)out[i] - (int)prevOut[i]);
                flicker += sum / out.size() / deko9::kTaauPhases;
            }
            prevOut.swap(out);
        }
        last = prevOut;
        double conv[6] = {};
        for (int m = 0; made && m < 6; ++m)
        {
            double sum = 0.0;
            for (size_t i = 0; i < last.size() && i < at[m].size(); ++i)
                sum += std::abs((int)at[m][i] - (int)last[i]);
            conv[m] = last.empty() ? 1e9 : sum / last.size();
        }
        std::snprintf(detail, sizeof(detail),
                      "scale=%.2f render %dx%d conv f1=%.2f f2=%.2f f4=%.2f f8=%.2f f16=%.2f f32=%.2f ref f1=%.2f "
                      "f8=%.2f f64=%.2f flicker=%.3f",
                      scale, cw, chh, conv[0], conv[1], conv[2], conv[3], conv[4], conv[5], refAt[0], refAt[1],
                      refAt[2], flicker);
        check(made && ran && conv[3] < conv[0] * 0.25 && flicker < 1.5,
              scale < 0.55f ? "TAAU_CONVERGE_CURVE_050" : scale < 0.7f ? "TAAU_CONVERGE_CURVE_060"
                                                                      : "TAAU_CONVERGE_CURVE_075",
              detail);
        for (IUnknown *object : {(IUnknown *)cSurface, (IUnknown *)cScene, (IUnknown *)cSys, (IUnknown *)cDepth})
            if (object)
                object->Release();
    }

    // Revealed at the screen edge: converge, pan 32 output px in one frame
    // (matching reprojection), then hold. The 32 px column the pan brings
    // in has no history; its error at frames 1, 4 and 16 after the pan.
    {
        uint32_t k = 0;
        for (; k < 2 * deko9::kTaauPhases; ++k)
        {
            deko9::TaauFrame f = base;
            f.reset = k == 0;
            ran = upload(k, 0.0, 0.0, &f) && ran;
            resolve(f);
        }
        const std::vector<float> refPan = Reference(32.0, 0.0);
        const auto stripMae = [&]() {
            std::vector<uint8_t> got;
            if (!readBack(&got))
                return 1e9;
            double sum = 0.0;
            size_t n = 0;
            for (int y = 0; y < kOutH; ++y)
                for (int x = 0; x < 32; ++x)
                    for (int c = 0; c < 3; ++c, ++n)
                    {
                        const size_t i = ((size_t)y * kOutW + x) * 3 + c;
                        sum += std::fabs(got[i] - refPan[i]);
                    }
            return sum / n;
        };
        double strip[3] = {};
        for (int step = 1; step <= 16; ++step, ++k)
        {
            deko9::TaauFrame f = base;
            if (step == 1)
                f.reproj[3][0] = -2.0f * 32.0f / kOutW;
            ran = upload(k, 32.0, 0.0, &f) && ran;
            resolve(f);
            if (step == 1 || step == 4 || step == 16)
                strip[step == 1 ? 0 : step == 4 ? 1 : 2] = stripMae();
        }
        std::snprintf(detail, sizeof(detail), "strip_mae f1=%.2f f4=%.2f f16=%.2f", strip[0], strip[1], strip[2]);
        check(ran && strip[1] < strip[0] * 0.6 && strip[2] < strip[1], "TAAU_REVEAL_EDGE", detail);
    }

    const int32_t badSrc[4] = {0, 0, w / 2, h / 2};
    check(!Deko9_TaauResolve(device, scene, smallDepth, badSrc, back, dst, &base), "TAAU_REJECTS_SIZE_MISMATCH", "");

    // Timing: enough resolves for a `DEKO9 taau frames=60` line with samples.
    for (uint32_t k = 0; k < 64; ++k)
    {
        deko9::TaauFrame f = base;
        deko9::TaauHalton(k, &f.jitter[0], &f.jitter[1]);
        resolve(f);
        if (!(k % 16))
            device->Present(nullptr, nullptr, nullptr, nullptr);
    }
    device->Present(nullptr, nullptr, nullptr, nullptr);
    check(ran, "TAAU_TIMING_RUNS", "");

    for (IUnknown *object : {(IUnknown *)sceneSurface, (IUnknown *)scene, (IUnknown *)sys, (IUnknown *)depth,
                             (IUnknown *)smallDepth, (IUnknown *)back, (IUnknown *)backCopy})
        object->Release();
    device->Release();
}
