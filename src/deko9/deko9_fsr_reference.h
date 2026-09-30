#pragma once

// Scalar CPU references of deko9's present upscalers (deko9_fsr_shaders.cpp),
// the oracle for switch_deko9_fsr_test (host) and the deko9 selftest's GPU
// readback check. Written separately from the GLSL: SGSR from Qualcomm's
// sgsr1_shader_mobile.frag (BSD-3-Clause), RCAS from AMD's ffx_fsr1.h (MIT);
// both license texts are in deko9_fsr_shaders.cpp. Hardware bilinear is
// modelled with exact float weights (Maxwell filters with 8-bit fractions;
// at the 0.75 ratio of 960x540 -> 1280x720 every fraction is a multiple of
// 1/8, so both agree).
//
// Images are RGB floats in [0, 1], row-major, row 0 at the top.

#include "deko9_fsr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace deko9
{
namespace fsrref
{

struct Rgb
{
    float r, g, b;
};

struct Image
{
    int width = 0, height = 0;
    std::vector<Rgb> px;
    const Rgb &At(int x, int y) const
    {
        x = std::min(std::max(x, 0), width - 1);
        y = std::min(std::max(y, 0), height - 1);
        return px[(size_t)y * width + x];
    }
};

inline float FromBits(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}
inline uint32_t ToBits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
inline float PrxMedRcp(float a)
{
    const float b = FromBits(0x7ef19fffu - ToBits(a));
    return b * (-b * a + 2.0f);
}
inline float Sat(float v) { return std::fmin(1.0f, std::fmax(0.0f, v)); }
// GLSL clamp as Maxwell runs it: min(max(v, lo), hi), a NaN operand dropped.
inline float Clamp(float v, float lo, float hi) { return std::fmin(std::fmax(v, lo), hi); }

// Clamp-to-edge bilinear sample at normalized (u, v), as a linear sampler.
// Texture units filter with fixed-point weights (Maxwell: 8 fraction
// bits). 0 = exact float weights (the default); the dynamic-resolution
// selftest compares against both, since its ratios (e.g. 1120 -> 1280) put
// sample positions between the 1/256 steps.
inline int g_bilinearFractionBits = 0;

inline Rgb Bilinear(const Image &in, float u, float v)
{
    const float sx = u * (float)in.width - 0.5f, sy = v * (float)in.height - 0.5f;
    const float fx = std::floor(sx), fy = std::floor(sy);
    float ax = sx - fx, ay = sy - fy;
    if (g_bilinearFractionBits)
    {
        const float steps = (float)(1 << g_bilinearFractionBits);
        ax = std::round(ax * steps) / steps;
        ay = std::round(ay * steps) / steps;
    }
    const int x = (int)fx, y = (int)fy;
    const Rgb &t00 = in.At(x, y), &t10 = in.At(x + 1, y), &t01 = in.At(x, y + 1), &t11 = in.At(x + 1, y + 1);
    const auto lerp2 = [&](float a, float b, float c, float d) {
        const float top = a + (b - a) * ax, bottom = c + (d - c) * ax;
        return top + (bottom - top) * ay;
    };
    return {lerp2(t00.r, t10.r, t01.r, t11.r), lerp2(t00.g, t10.g, t01.g, t11.g), lerp2(t00.b, t10.b, t01.b, t11.b)};
}

// Pixel-centre UV of output pixel (x, y), as the full-screen triangle
// interpolates it.
inline float CentreUv(int x, int size) { return ((float)x + 0.5f) / (float)size; }

inline Rgb BilinearUpscale(const Image &in, int outWidth, int outHeight, int x, int y)
{
    return Bilinear(in, CentreUv(x, outWidth), CentreUv(y, outHeight));
}

// SGSR v1 (OperationMode 1, EdgeThreshold 8/255, EdgeSharpness 2) output
// pixel (x, y).
// SGSR after the bilinear colour: icX/icY = the output pixel's position in
// input texels minus 0.5; texel loads clamp to [x0, x1] x [y0, y1].
inline Rgb SgsrCore(const Image &in, Rgb color, float icX, float icY, int x0, int y0, int x1, int y1);

inline Rgb Sgsr(const Image &in, int outWidth, int outHeight, int x, int y)
{
    const float u = CentreUv(x, outWidth), v = CentreUv(y, outHeight);
    const Rgb color = Bilinear(in, u, v);
    return SgsrCore(in, color, u * (float)in.width - 0.5f, v * (float)in.height - 0.5f, 0, 0, in.width - 1,
                    in.height - 1);
}

inline Rgb SgsrCore(const Image &in, Rgb color, float icX, float icY, int x0, int y0, int x1, int y1)
{
    const float pX = std::floor(icX), pY = std::floor(icY);
    const float plX = icX - pX, plY = icY - pY;
    const int fx = (int)pX, fy = (int)pY;
    const auto G = [&](int dx, int dy) {
        return in.At(std::min(std::max(fx + dx, x0), x1), std::min(std::max(fy + dy, y0), y1)).g;
    };
    const float fG = G(0, 0), jG = G(0, 1);
    const float edgeVote = std::fabs(fG - jG) + std::fabs(color.g - jG) + std::fabs(color.g - fG);
    if (!(edgeVote > 8.0f / 255.0f))
        return color;
    const float kG = G(1, 1), gG = G(1, 0);
    const float mean = (jG + fG + kG + gG) * 0.25f;
    const float b = G(0, -1) - mean, c = G(1, -1) - mean, e = G(-1, 0) - mean, f = fG - mean, g = gG - mean,
                h = G(2, 0) - mean, i = G(-1, 1) - mean, j = jG - mean, k = kG - mean, l = G(2, 1) - mean,
                n = G(0, 2) - mean, o = G(1, 2) - mean;
    using std::fabs;
    const float sum = ((((fabs(i) + fabs(j)) + fabs(f)) + fabs(e)) + (((fabs(k) + fabs(l)) + fabs(h)) + fabs(g))) +
                      (((fabs(b) + fabs(c)) + fabs(o)) + fabs(n));
    const float stdv = 2.181818f / sum;
    const float kS = 0.74161984870956629f; // sqrt(0.55)
    // Index 0..3 = offset -1, 0, 1, 2 from 'f'.
    float dx[4], dy[4];
    const float off[4] = {kS, 0.0f, -kS, -2.0f * kS};
    for (int a = 0; a < 4; ++a)
    {
        dx[a] = std::fma(plX, kS, off[a]);
        const float t = std::fma(plY, kS, off[a]);
        dy[a] = std::fma(t, t, -4.0f);
    }
    float aW = 0.0f, aY = 0.0f;
    const auto tap = [&](int ox, int oy, float cc) {
        const float d2m4 = std::fma(dx[ox + 1], dx[ox + 1], dy[oy + 1]);
        const float uu = d2m4 + Sat(std::fabs(cc) * stdv);
        const float w = (uu * uu * uu) * (uu + 3.0f);
        aW += w;
        aY = std::fma(w, cc, aY);
    };
    tap(0, -1, b);
    tap(1, -1, c);
    tap(1, 2, o);
    tap(0, 2, n);
    tap(-1, 1, i);
    tap(0, 1, j);
    tap(0, 0, f);
    tap(-1, 0, e);
    tap(1, 1, k);
    tap(2, 1, l);
    tap(2, 0, h);
    tap(1, 0, g);
    const float maxY = std::fmax(std::fmax(j, f), std::fmax(k, g));
    const float minY = std::fmin(std::fmin(j, f), std::fmin(k, g));
    const float finalY = Clamp(2.0f * (aY / aW), minY, maxY);
    const float deltaY = Clamp(finalY - (color.g - mean), -23.0f / 255.0f, 23.0f / 255.0f);
    return {Sat(color.r + deltaY), Sat(color.g + deltaY), Sat(color.b + deltaY)};
}

// FSR 1 RCAS of the cross b (up), d (left), e (centre), f (right), h (down).
inline Rgb RcasCross(const Rgb &b, const Rgb &d, const Rgb &e, const Rgb &f, const Rgb &h, float sharpnessStops)
{
    const float limit = 0.25f - 1.0f / 16.0f;
    const float eps = 1.0f / 65536.0f;
    float lobe = -1e30f;
    const float *ch[5] = {&b.r, &d.r, &e.r, &f.r, &h.r};
    for (int c = 0; c < 3; ++c)
    {
        const float vb = ch[0][c], vd = ch[1][c], ve = ch[2][c], vf = ch[3][c], vh = ch[4][c];
        const float mn4 = std::min(std::min(vb, vd), std::min(vf, vh));
        const float mx4 = std::max(std::max(vb, vd), std::max(vf, vh));
        const float hitMin = std::min(mn4, ve) * (1.0f / std::max(4.0f * mx4, eps));
        const float hitMax = (1.0f - std::max(mx4, ve)) * (1.0f / std::min(4.0f * mn4 - 4.0f, -eps));
        lobe = std::max(lobe, std::max(-hitMin, hitMax));
    }
    lobe = std::max(-limit, std::min(lobe, 0.0f)) * std::exp2(-sharpnessStops);
    const float rcpL = PrxMedRcp(4.0f * lobe + 1.0f);
    return {(lobe * b.r + lobe * d.r + lobe * h.r + lobe * f.r + e.r) * rcpL,
            (lobe * b.g + lobe * d.g + lobe * h.g + lobe * f.g + e.g) * rcpL,
            (lobe * b.b + lobe * d.b + lobe * h.b + lobe * f.b + e.b) * rcpL};
}

// UNORM8 store (round to nearest), as the render targets do.
inline uint8_t ToUnorm8(float v) { return (uint8_t)std::lround(Sat(v) * 255.0f); }
inline float FromUnorm8(uint8_t v) { return (float)v / 255.0f; }

// Bilinear upscale fused with RCAS: the cross is bilinear samples at this
// output pixel and its neighbours, the neighbours clamped to the edge pixels.
// RCAS's per-channel limiters divide channel values, so where a channel is
// near 0 (or 1) sub-LSB differences in the filtered taps move the lobe by
// several LSBs of output. How precisely the sampler filters UNORM8 is up to
// the hardware: unorm8Taps = true models a filter that returns results
// rounded to 8 bits, false an exact one. The selftest accepts a pixel close
// to either, since real hardware output splits between the two models.
inline Rgb BilinearRcas(const Image &in, int outWidth, int outHeight, float sharpnessStops, int x, int y,
                        bool unorm8Taps = false)
{
    const float u = CentreUv(x, outWidth), v = CentreUv(y, outHeight);
    const float stepU = 1.0f / (float)outWidth, stepV = 1.0f / (float)outHeight;
    const float minU = 0.5f / (float)outWidth, minV = 0.5f / (float)outHeight;
    const float maxU = 1.0f - minU, maxV = 1.0f - minV;
    const auto tap = [&](float tu, float tv) {
        const Rgb c = Bilinear(in, tu, tv);
        if (!unorm8Taps)
            return c;
        return Rgb{FromUnorm8(ToUnorm8(c.r)), FromUnorm8(ToUnorm8(c.g)), FromUnorm8(ToUnorm8(c.b))};
    };
    return RcasCross(tap(u, std::max(v - stepV, minV)), tap(std::max(u - stepU, minU), v), tap(u, v),
                     tap(std::min(u + stepU, maxU), v), tap(u, std::min(v + stepV, maxV)), sharpnessStops);
}

// The deko9 present pass for `mode` into an 8-bit swapchain image. Returns
// RGB bytes, row-major. unorm8Taps: see BilinearRcas.
inline std::vector<uint8_t> UpscaleRgb8(const Image &in, int outWidth, int outHeight, uint32_t mode,
                                        float sharpnessStops, bool unorm8Taps = false)
{
    std::vector<uint8_t> out((size_t)outWidth * outHeight * 3);
    for (int y = 0; y < outHeight; ++y)
        for (int x = 0; x < outWidth; ++x)
        {
            Rgb c;
            switch (mode)
            {
            case UPSCALE_SGSR: c = Sgsr(in, outWidth, outHeight, x, y); break;
            case UPSCALE_BILINEAR_RCAS:
                c = BilinearRcas(in, outWidth, outHeight, sharpnessStops, x, y, unorm8Taps);
                break;
            default: c = BilinearUpscale(in, outWidth, outHeight, x, y); break;
            }
            uint8_t *p = &out[((size_t)y * outWidth + x) * 3];
            p[0] = ToUnorm8(c.r);
            p[1] = ToUnorm8(c.g);
            p[2] = ToUnorm8(c.b);
        }
    return out;
}

// The w x h rectangle at (x, y) of `in`.
inline Image Crop(const Image &in, int x, int y, int w, int h)
{
    Image out;
    out.width = w;
    out.height = h;
    out.px.resize((size_t)w * h);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            out.px[(size_t)j * w + i] = in.At(x + i, y + j);
    return out;
}

// The programs' own texture-space arithmetic for a rectangle source
// (UpscaleSource: destination UV -> texture UV, bilinear taps clamped half a
// texel inside the rectangle, SGSR texel loads clamped to it), sampling the
// whole texture `tex`. Must equal UpscaleRgb8 on Crop(tex, rect) whatever
// lies outside the rectangle (switch_deko9_fsr_test).
inline std::vector<uint8_t> UpscaleRgb8Source(const Image &tex, const UpscaleSource &src, int outWidth,
                                              int outHeight, uint32_t mode, float sharpnessStops,
                                              bool unorm8Taps = false)
{
    const auto toTex = [&](float u, float v, float *tu, float *tv) {
        *tu = src.uvTransform[0] + u * src.uvTransform[2];
        *tv = src.uvTransform[1] + v * src.uvTransform[3];
    };
    const auto tapTex = [&](float u, float v) {
        float tu, tv;
        toTex(u, v, &tu, &tv);
        const Rgb c = Bilinear(tex, Clamp(tu, src.uvClamp[0], src.uvClamp[2]), Clamp(tv, src.uvClamp[1], src.uvClamp[3]));
        if (!unorm8Taps)
            return c;
        return Rgb{FromUnorm8(ToUnorm8(c.r)), FromUnorm8(ToUnorm8(c.g)), FromUnorm8(ToUnorm8(c.b))};
    };
    std::vector<uint8_t> out((size_t)outWidth * outHeight * 3);
    for (int y = 0; y < outHeight; ++y)
        for (int x = 0; x < outWidth; ++x)
        {
            const float u = CentreUv(x, outWidth), v = CentreUv(y, outHeight);
            Rgb c;
            switch (mode)
            {
            case UPSCALE_SGSR:
            {
                float tu, tv;
                toTex(u, v, &tu, &tv);
                const Rgb color = tapTex(u, v);
                c = SgsrCore(tex, color, tu * (float)tex.width - 0.5f, tv * (float)tex.height - 0.5f,
                             src.texelRect[0], src.texelRect[1], src.texelRect[2], src.texelRect[3]);
                break;
            }
            case UPSCALE_BILINEAR_RCAS:
            {
                const float stepU = 1.0f / (float)outWidth, stepV = 1.0f / (float)outHeight;
                const float minU = 0.5f / (float)outWidth, minV = 0.5f / (float)outHeight;
                const float maxU = 1.0f - minU, maxV = 1.0f - minV;
                c = RcasCross(tapTex(u, std::max(v - stepV, minV)), tapTex(std::max(u - stepU, minU), v),
                              tapTex(u, v), tapTex(std::min(u + stepU, maxU), v),
                              tapTex(u, std::min(v + stepV, maxV)), sharpnessStops);
                break;
            }
            default: c = tapTex(u, v); break;
            }
            uint8_t *p = &out[((size_t)y * outWidth + x) * 3];
            p[0] = ToUnorm8(c.r);
            p[1] = ToUnorm8(c.g);
            p[2] = ToUnorm8(c.b);
        }
    return out;
}

} // namespace fsrref
} // namespace deko9
