#pragma once

// CPU reference of the off-screen particle passes (r_halfResParticles,
// GPU programs kHrpDepthGlsl / kHrpCompositeGlsl
// in deko9_fsr_shaders.cpp): the same arithmetic as the fragment programs,
// the oracle of the deko9 selftest's HRP_* checks and of
// switch_halfres_particles_test (host).
//
// Blending model: an UNORM8 render target stores round(v * 255) / 255 after
// every blend (the hardware blends in float, then converts).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace deko9
{
namespace hrpref
{

struct Rgba
{
    float r, g, b, a;
};

inline float Unorm8(float v)
{
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return std::floor(v * 255.0f + 0.5f) / 255.0f;
}
inline Rgba Unorm8(const Rgba &c) { return {Unorm8(c.r), Unorm8(c.g), Unorm8(c.b), Unorm8(c.a)}; }

enum class Blend : uint8_t
{
    Over,     // srcalpha, invsrcalpha
    Add,      // one, one
    AddAlpha, // srcalpha, one
};

// A direct draw into the scene target (colour channels only; the scene
// target's alpha is not observed).
inline Rgba BlendDirect(const Rgba &dst, const Rgba &s, Blend b, bool quantize = true)
{
    Rgba o = dst;
    const float ka = b == Blend::Add ? 1.0f : s.a;
    const float kd = b == Blend::Over ? 1.0f - s.a : 1.0f;
    o.r = s.r * ka + dst.r * kd;
    o.g = s.g * ka + dst.g * kd;
    o.b = s.b * ka + dst.b * kd;
    return quantize ? Unorm8(o) : o;
}

// The same draw redirected into the off-screen target (the remapped blend
// of r_halfres_particles_rules.h: colour as above, alpha ZERO/INVSRCALPHA
// for Over and ZERO/ONE for the additive ones). Start value (0, 0, 0, 1).
inline Rgba BlendOffscreen(const Rgba &acc, const Rgba &s, Blend b, bool quantize = true)
{
    Rgba o = BlendDirect(acc, s, b, false);
    o.a = b == Blend::Over ? acc.a * (1.0f - s.a) : acc.a;
    return quantize ? Unorm8(o) : o;
}

// Composite blend ONE / SRCALPHA of the program's output (C, T): dst*T + C.
inline Rgba Composite(const Rgba &dst, const Rgba &acc, bool quantize = true)
{
    Rgba o = dst;
    o.r = acc.r + dst.r * acc.a;
    o.g = acc.g + dst.g * acc.a;
    o.b = acc.b + dst.b * acc.a;
    return quantize ? Unorm8(o) : o;
}

// kHrpDepthGlsl: the off-screen texel (x, y) covers scene texels
// origin + (x, y) * factor + (0..factor-1, 0..factor-1), clamped to the
// rectangle's last texel; it takes the nearest one (smallest window depth,
// deko9's LESSEQUAL convention: conservative for occlusion, a particle
// behind any covered texel's geometry is rejected) and outputs that texel's
// depth and float-Z. The footprint is visited (0,0), (f-1,0), (0,f-1),
// (f-1,f-1) and a later texel wins only when strictly nearer.
// Returns the chosen scene texel.
inline void DownsamplePick(const float *depth, int pitch, const int rect[4], int factor, int x, int y, int *outX,
                           int *outY)
{
    const int bx = rect[0] + x * factor, by = rect[1] + y * factor;
    const int hx = rect[0] + rect[2] - 1, hy = rect[1] + rect[3] - 1;
    const int f = factor - 1;
    const int offs[4][2] = {{0, 0}, {f, 0}, {0, f}, {f, f}};
    int best[2] = {std::min(bx, hx), std::min(by, hy)};
    float bestD = depth[best[1] * pitch + best[0]];
    for (int i = 1; i < 4; ++i)
    {
        const int px = std::min(bx + offs[i][0], hx), py = std::min(by + offs[i][1], hy);
        const float d = depth[py * pitch + px];
        if (d < bestD)
        {
            bestD = d;
            best[0] = px;
            best[1] = py;
        }
    }
    *outX = best[0];
    *outY = best[1];
}

// kHrpCompositeGlsl constants.
struct CompositeConstants
{
    float map[4];     // h = fragCoord * map.xy + map.zw (off-screen texel space, centres at integers)
    int32_t limit[4]; // x, y: last off-screen texel; z: 0 bilinear, 1 nearest-depth; w: 0
    float tol[4];     // x: relative float-Z tolerance of the nearest-depth test
};

// dstRect = scene pixels composited (x, y, w, h); origin = the scene
// viewport origin the off-screen view maps to; view = its size (w, h);
// offscreen = the off-screen size covering it.
inline void CompositeSetup(CompositeConstants *c, const int origin[2], const int view[2], const int offscreen[2],
                           int mode, float tolerance)
{
    const float sx = (float)offscreen[0] / (float)view[0], sy = (float)offscreen[1] / (float)view[1];
    c->map[0] = sx;
    c->map[1] = sy;
    c->map[2] = -(float)origin[0] * sx - 0.5f;
    c->map[3] = -(float)origin[1] * sy - 0.5f;
    c->limit[0] = offscreen[0] - 1;
    c->limit[1] = offscreen[1] - 1;
    c->limit[2] = mode;
    c->limit[3] = 0;
    c->tol[0] = tolerance;
    c->tol[1] = c->tol[2] = c->tol[3] = 0.0f;
}

// The program's output (C, T) at scene pixel (px, py) (fragCoord = p + 0.5).
// color/halfZ: off-screen images (pitch in texels); fullZ: the scene float-Z
// at that pixel.
inline Rgba CompositeSample(const CompositeConstants &c, const Rgba *color, const float *halfZ, int pitch,
                            float fullZ, int px, int py)
{
    const float hx = ((float)px + 0.5f) * c.map[0] + c.map[2];
    const float hy = ((float)py + 0.5f) * c.map[1] + c.map[3];
    const float bx = std::floor(hx), by = std::floor(hy);
    const float fx = hx - bx, fy = hy - by;
    const int ix = (int)bx, iy = (int)by;
    const int x0 = std::clamp(ix, 0, c.limit[0]), x1 = std::clamp(ix + 1, 0, c.limit[0]);
    const int y0 = std::clamp(iy, 0, c.limit[1]), y1 = std::clamp(iy + 1, 0, c.limit[1]);
    const Rgba t[4] = {color[y0 * pitch + x0], color[y0 * pitch + x1], color[y1 * pitch + x0],
                       color[y1 * pitch + x1]};
    const auto lerp = [](const Rgba &a, const Rgba &b, float f) {
        return Rgba{a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, a.a + (b.a - a.a) * f};
    };
    Rgba out = lerp(lerp(t[0], t[1], fx), lerp(t[2], t[3], fx), fy);
    if (c.limit[2])
    {
        const float z = std::fabs(fullZ);
        const float hz[4] = {std::fabs(halfZ[y0 * pitch + x0]), std::fabs(halfZ[y0 * pitch + x1]),
                             std::fabs(halfZ[y1 * pitch + x0]), std::fabs(halfZ[y1 * pitch + x1])};
        float worst = 0.0f, bestDz = std::fabs(hz[0] - z);
        int best = 0;
        for (int i = 0; i < 4; ++i)
        {
            const float dz = std::fabs(hz[i] - z);
            worst = std::max(worst, dz);
            if (dz < bestDz)
            {
                bestDz = dz;
                best = i;
            }
        }
        if (worst > c.tol[0] * z)
            out = t[best];
    }
    return out;
}

} // namespace hrpref
} // namespace deko9
