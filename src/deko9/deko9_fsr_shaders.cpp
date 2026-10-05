// Present upscaler programs (SGSR v1, bilinear + RCAS, bilinear) as GLSL
// 4.60 for UAM, plus the CPU constant setup. See deko9_fsr.h.
//
// kSgsrGlsl is ported from Qualcomm's Snapdragon Game Super Resolution v1
// (sgsr/v1/include/glsl/sgsr1_shader_mobile.frag,
// https://github.com/SnapdragonStudios/snapdragon-gsr at d926f074), which
// carries this license:
//
// Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// SPDX-License-Identifier: BSD-3-Clause
//
// kBilinearRcasGlsl's sharpening is RCAS from AMD's FidelityFX-FSR reference
// (ffx_fsr1.h, https://github.com/GPUOpen-Effects/FidelityFX-FSR), which
// carries this license:
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "deko9_fsr.h"

#include <cmath>
#include <cstring>

namespace deko9
{

const char *UpscaleModeName(uint32_t mode)
{
    switch (mode)
    {
    case UPSCALE_SGSR: return "sgsr";
    case UPSCALE_BILINEAR_RCAS: return "bilinear_rcas";
    case UPSCALE_BILINEAR: return "bilinear";
    default: return "invalid";
    }
}

const char kFsrVertexGlsl[] = R"GLSL(#version 460
layout(location = 0) out vec2 vUv;
void main()
{
    // 0:(-1,-1) 1:(3,-1) 2:(-1,3): one triangle covering the viewport.
    vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
    gl_Position = vec4(p, 0.0, 1.0);
    // NDC +Y is the top row (row 0): v = 0 there.
    vUv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
}
)GLSL";

// SGSR v1, OperationMode 1 (RGBA: luma is the green channel), EdgeThreshold
// 8/255, EdgeSharpness 2.0, no UseEdgeDirection. Changes from Qualcomm's
// shader, all value-preserving:
//   - the four green-channel textureGathers are twelve texelFetch loads of
//     the same (clamped) texels: on hardware, textureGather of components
//     1/2 of the BGRA8 back buffer returned wrong texels, and only f and j
//     are needed before the edge test;
//   - texel space is imgCoord = uv * size - 0.5 with 'f' = floor(imgCoord)
//     (Qualcomm's +0.5 in y only positions the gather footprint);
//   - 0.55 * (dx^2 + dy^2) is formed per axis once (dx scaled by sqrt 0.55,
//     dy^2 - 4 folded in) and fastLanczos2 is evaluated as (u + 3) * u^3
//     with u = x - 4, the same polynomial as (x*wA - wA) * wA^2;
//   - FP32 throughout (the reference's mediump is a mobile hint).
//
// Texel names (as in deko9_fsr_reference.h), 'f' at (fx, fy):
//      b c
//    e f g h
//    i j k l
//      n o
const char kSgsrGlsl[] = R"GLSL(#version 460
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uInput;
layout(std140, binding = 1) uniform SgsrBlock
{
    vec4 uViewportInfo; // 1/w, 1/h, w, h of the input texture
    ivec4 uTexelRect;   // source rectangle, inclusive texels: x0, y0, x1, y1
    vec4 uTransform;    // texture UV = xy + destination UV * zw
    vec4 uUvClamp;      // bilinear taps: half a texel inside the rectangle
};

// weightY: u = x - 4, x = 0.55 * distance^2 + saturate(|c| * std);
// fastLanczos2(x) = (x - 1) * (x - 4)^3 = (u + 3) * u^3.
void Tap(inout float aW, inout float aY, float d2m4, float c, float stdv)
{
    float u = d2m4 + clamp(abs(c) * stdv, 0.0, 1.0);
    float w = (u * u * u) * (u + 3.0);
    aW += w;
    aY = fma(w, c, aY);
}

void main()
{
#ifdef DEKO9_SOURCE_RECT
    vec2 uv = uTransform.xy + vUv * uTransform.zw;
    vec3 color = textureLod(uInput, clamp(uv, uUvClamp.xy, uUvClamp.zw), 0.0).rgb;
#else
    vec2 uv = vUv;
    vec3 color = textureLod(uInput, uv, 0.0).rgb;
#endif
    vec2 imgCoord = uv * uViewportInfo.zw - vec2(0.5);
    vec2 imgCoordPixel = floor(imgCoord);
    vec2 pl = imgCoord - imgCoordPixel;
    ivec2 fp = ivec2(imgCoordPixel);
    int x0 = max(fp.x, uTexelRect.x), x1 = min(fp.x + 1, uTexelRect.z);
    int y0 = max(fp.y, uTexelRect.y), y1 = min(fp.y + 1, uTexelRect.w);
    float fG = texelFetch(uInput, ivec2(x0, y0), 0).g;
    float jG = texelFetch(uInput, ivec2(x0, y1), 0).g;
    float edgeVote = abs(fG - jG) + abs(color.g - jG) + abs(color.g - fG);
    if (edgeVote > 8.0 / 255.0)
    {
        int xm = max(fp.x - 1, uTexelRect.x), x2 = min(fp.x + 2, uTexelRect.z);
        int ym = max(fp.y - 1, uTexelRect.y), y2 = min(fp.y + 2, uTexelRect.w);
        float iG = texelFetch(uInput, ivec2(xm, y1), 0).g;
        float eG = texelFetch(uInput, ivec2(xm, y0), 0).g;
        float kG = texelFetch(uInput, ivec2(x1, y1), 0).g;
        float lG = texelFetch(uInput, ivec2(x2, y1), 0).g;
        float hG = texelFetch(uInput, ivec2(x2, y0), 0).g;
        float gG = texelFetch(uInput, ivec2(x1, y0), 0).g;
        float bG = texelFetch(uInput, ivec2(x0, ym), 0).g;
        float cG = texelFetch(uInput, ivec2(x1, ym), 0).g;
        float oG = texelFetch(uInput, ivec2(x1, y2), 0).g;
        float nG = texelFetch(uInput, ivec2(x0, y2), 0).g;
        float mean = (jG + fG + kG + gG) * 0.25;
        float b = bG - mean, c = cG - mean, e = eG - mean, f = fG - mean, g = gG - mean, h = hG - mean;
        float i = iG - mean, j = jG - mean, k = kG - mean, l = lG - mean, n = nG - mean, o = oG - mean;
        // Qualcomm's left / right / upDown gathers: (i j f e) (k l h g) (b c o n).
        float sum = ((((abs(i) + abs(j)) + abs(f)) + abs(e)) + (((abs(k) + abs(l)) + abs(h)) + abs(g))) +
                    (((abs(b) + abs(c)) + abs(o)) + abs(n));
        float stdv = 2.181818 / sum;
        // sqrt(0.55) * (pl - offset) for offsets -1, 0, 1, 2 from 'f'.
        const float kS = 0.74161984870956629;
        vec4 dx = fma(vec4(pl.x), vec4(kS), vec4(kS, 0.0, -kS, -2.0 * kS));
        vec4 dy = fma(vec4(pl.y), vec4(kS), vec4(kS, 0.0, -kS, -2.0 * kS));
        dy = fma(dy, dy, vec4(-4.0));
        float aW = 0.0, aY = 0.0;
        Tap(aW, aY, fma(dx.y, dx.y, dy.x), b, stdv);
        Tap(aW, aY, fma(dx.z, dx.z, dy.x), c, stdv);
        Tap(aW, aY, fma(dx.z, dx.z, dy.w), o, stdv);
        Tap(aW, aY, fma(dx.y, dx.y, dy.w), n, stdv);
        Tap(aW, aY, fma(dx.x, dx.x, dy.z), i, stdv);
        Tap(aW, aY, fma(dx.y, dx.y, dy.z), j, stdv);
        Tap(aW, aY, fma(dx.y, dx.y, dy.y), f, stdv);
        Tap(aW, aY, fma(dx.x, dx.x, dy.y), e, stdv);
        Tap(aW, aY, fma(dx.z, dx.z, dy.z), k, stdv);
        Tap(aW, aY, fma(dx.w, dx.w, dy.z), l, stdv);
        Tap(aW, aY, fma(dx.w, dx.w, dy.y), h, stdv);
        Tap(aW, aY, fma(dx.z, dx.z, dy.y), g, stdv);
        float finalY = aY / aW;
        float maxY = max(max(j, f), max(k, g));
        float minY = min(min(j, f), min(k, g));
        finalY = clamp(2.0 * finalY, minY, maxY);
        // Smooth high-contrast input.
        float deltaY = clamp(finalY - (color.g - mean), -23.0 / 255.0, 23.0 / 255.0);
        color = clamp(color + vec3(deltaY), 0.0, 1.0);
    }
    outColor = vec4(color, 1.0);
}
)GLSL";

// Bilinear upscale fused with FSR 1 RCAS (FSR_RCAS_DENOISE and
// FSR_RCAS_PASSTHROUGH_ALPHA off, as AMD suggests). The RCAS cross is five
// hardware-bilinear samples of the input at this output pixel and at its
// four neighbours, the neighbour positions clamped to the edge pixels as
// RCAS's clamped loads were: the same values a bilinear pass into an
// intermediate image followed by RCAS would read, minus that image's 8-bit
// rounding and a full-screen write + read. The limiters' reciprocals are
// AMD's except that 4*max and 4*min-4 are kept away from 0: a black (or
// white) neighbourhood made them 0 * inf = NaN, which Maxwell's min/max
// happen to drop but GLSL does not promise.
const char kBilinearRcasGlsl[] = R"GLSL(#version 460
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uInput;
layout(std140, binding = 1) uniform BilinearRcasBlock
{
    vec4 uStep;      // 1/outW, 1/outH, sharpness (linear), 0 (destination UV)
    vec4 uUvClamp;   // 0.5/outW, 0.5/outH, 1 - 0.5/outW, 1 - 0.5/outH (destination UV)
    vec4 uTransform; // texture UV = xy + destination UV * zw
    vec4 uSrcClamp;  // half a texel inside the source rectangle (texture UV)
};

vec3 Tap(vec2 uv)
{
#ifdef DEKO9_SOURCE_RECT
    uv = clamp(uTransform.xy + uv * uTransform.zw, uSrcClamp.xy, uSrcClamp.zw);
#endif
    return textureLod(uInput, uv, 0.0).rgb;
}

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

float APrxMedRcpF1(float a)
{
    float b = uintBitsToFloat(0x7ef19fffu - floatBitsToUint(a));
    return b * (-b * a + 2.0);
}

void main()
{
    //    b
    //  d e f
    //    h
    vec3 b = Tap(vec2(vUv.x, max(vUv.y - uStep.y, uUvClamp.y)));
    vec3 d = Tap(vec2(max(vUv.x - uStep.x, uUvClamp.x), vUv.y));
    vec3 e = Tap(vUv);
    vec3 f = Tap(vec2(min(vUv.x + uStep.x, uUvClamp.z), vUv.y));
    vec3 h = Tap(vec2(vUv.x, min(vUv.y + uStep.y, uUvClamp.w)));
    // Min and max of ring.
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    // Limiters, these need to be high precision RCPs.
    const float kEps = 1.0 / 65536.0;
    vec3 hitMin = min(mn4, e) * (1.0 / max(4.0 * mx4, vec3(kEps)));
    vec3 hitMax = (1.0 - max(mx4, e)) * (1.0 / min(4.0 * mn4 - 4.0, vec3(-kEps)));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-FSR_RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * uStep.z;
    // Resolve, which needs the medium precision rcp approximation to avoid visible tonality changes.
    float rcpL = APrxMedRcpF1(4.0 * lobe + 1.0);
    outColor = vec4((lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL, 1.0);
}
)GLSL";

const char kBilinearGlsl[] = R"GLSL(#version 460
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uInput;
layout(std140, binding = 1) uniform BilinearBlock
{
    vec4 uTransform; // texture UV = xy + destination UV * zw
    vec4 uSrcClamp;  // half a texel inside the source rectangle (texture UV)
};
void main()
{
#ifdef DEKO9_SOURCE_RECT
    vec2 uv = clamp(uTransform.xy + vUv * uTransform.zw, uSrcClamp.xy, uSrcClamp.zw);
#else
    vec2 uv = vUv;
#endif
    outColor = vec4(textureLod(uInput, uv, 0.0).rgb, 1.0);
}
)GLSL";

// GLSL requires a constant gather component: one gather per case.
const char kGatherProbeGlsl[] = R"GLSL(#version 460
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uInput;
layout(std140, binding = 1) uniform GatherProbeBlock
{
    ivec4 uComponent;
};
void main()
{
    vec2 uv = (floor(gl_FragCoord.xy) + 1.0) / vec2(textureSize(uInput, 0));
    vec4 g;
    switch (uComponent.x)
    {
    case 0: g = textureGather(uInput, uv, 0); break;
    case 1: g = textureGather(uInput, uv, 1); break;
    case 2: g = textureGather(uInput, uv, 2); break;
    default: g = textureGather(uInput, uv, 3); break;
    }
    outColor = g;
}
)GLSL";

const char kFloatZGlsl[] = R"GLSL(#version 460
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uDepth;
layout(std140, binding = 1) uniform FloatZBlock
{
    vec4 uViewmodel; // A, B, split
    vec4 uScene;     // A, B, clear
};
void main()
{
    float d = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).x;
    vec2 ab = d < uViewmodel.z ? uViewmodel.xy : uScene.xy;
    float den = ab.y - d;
    float z = ab.x / den;
    outColor = vec4(den > 0.0 && abs(z) < uScene.z ? z : uScene.z, 0.0, 0.0, 1.0);
}
)GLSL";

// Off-screen particles (deko9_particles_reference.h DownsamplePick is the
// same selection): texel (x, y) of the off-screen depth/float-Z takes the
// nearest scene texel of its footprint.
const char kHrpDepthGlsl[] = R"GLSL(#version 460
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uDepth;
layout(binding = 1) uniform sampler2D uFloatZ;
layout(std140, binding = 1) uniform HrpDepthBlock
{
    ivec4 uRect;  // scene rectangle x, y, last x, last y
    ivec4 uParam; // factor - 1
};
void main()
{
    ivec2 base = uRect.xy + ivec2(gl_FragCoord.xy) * (uParam.x + 1);
    ivec2 f = uParam.xx;
    ivec2 best = min(base, uRect.zw);
    float bestD = texelFetch(uDepth, best, 0).x;
    ivec2 p = min(base + ivec2(f.x, 0), uRect.zw);
    float d = texelFetch(uDepth, p, 0).x;
    if (d < bestD) { bestD = d; best = p; }
    p = min(base + ivec2(0, f.y), uRect.zw);
    d = texelFetch(uDepth, p, 0).x;
    if (d < bestD) { bestD = d; best = p; }
    p = min(base + f, uRect.zw);
    d = texelFetch(uDepth, p, 0).x;
    if (d < bestD) { bestD = d; best = p; }
    outColor = vec4(texelFetch(uFloatZ, best, 0).x, 0.0, 0.0, 1.0);
    gl_FragDepth = bestD;
}
)GLSL";

// deko9_particles_reference.h CompositeSample: (C, T) at a scene pixel,
// blended ONE / SRCALPHA by the pass (dst * T + C).
const char kHrpCompositeGlsl[] = R"GLSL(#version 460
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uColor;
layout(binding = 1) uniform sampler2D uHalfZ;
layout(binding = 2) uniform sampler2D uFullZ;
layout(std140, binding = 1) uniform HrpCompositeBlock
{
    vec4 uMap;
    ivec4 uLimit;
    vec4 uTol;
};
void main()
{
    vec2 h = gl_FragCoord.xy * uMap.xy + uMap.zw;
    vec2 b = floor(h);
    vec2 f = h - b;
    ivec2 i = ivec2(b);
    ivec2 lo = clamp(i, ivec2(0), uLimit.xy);
    ivec2 hi = clamp(i + 1, ivec2(0), uLimit.xy);
    vec4 t0 = texelFetch(uColor, lo, 0);
    vec4 t1 = texelFetch(uColor, ivec2(hi.x, lo.y), 0);
    vec4 t2 = texelFetch(uColor, ivec2(lo.x, hi.y), 0);
    vec4 t3 = texelFetch(uColor, hi, 0);
    vec4 outc = mix(mix(t0, t1, f.x), mix(t2, t3, f.x), f.y);
    // No control flow: the nearest-depth pick differs between pixels of a
    // warp, and the branchy form (SSY/SYNC reconvergence) gave wrong,
    // varying output on Maxwell for pixels that took neither branch.
    float z = abs(texelFetch(uFullZ, ivec2(gl_FragCoord.xy), 0).x);
    vec4 dz = abs(vec4(abs(texelFetch(uHalfZ, lo, 0).x), abs(texelFetch(uHalfZ, ivec2(hi.x, lo.y), 0).x),
                       abs(texelFetch(uHalfZ, ivec2(lo.x, hi.y), 0).x), abs(texelFetch(uHalfZ, hi, 0).x)) - z);
    // Selections are mix() with boolean selectors (component selects).
    bool p1 = dz.y < dz.x;
    vec4 pick = mix(t0, t1, bvec4(p1));
    float best = mix(dz.x, dz.y, p1);
    bool p2 = dz.z < best;
    pick = mix(pick, t2, bvec4(p2));
    best = mix(best, dz.z, p2);
    pick = mix(pick, t3, bvec4(dz.w < best));
    // Mode 2 (diagnostic): mark the pixels that took the nearest-depth texel.
    pick = mix(pick, vec4(0.5, 0.0, 0.0, 0.5), bvec4(uLimit.z == 2));
    // all(), not &&: a short-circuit && compiles to a branch.
    bool nearest = all(bvec2(uLimit.z != 0, max(max(dz.x, dz.y), max(dz.z, dz.w)) > uTol.x * z));
    outc = mix(outc, pick, bvec4(nearest));
    outColor = outc;
}
)GLSL";

void FloatZSetup(FloatZConstants *out, const FloatZRange &scene, const FloatZRange &viewmodel, float clearValue)
{
    // d = r0 + s * (m22 + m32 / z), s = maxZ - minZ
    //   => z = -m32 * s / (m22 * s + r0 - d) = A / (B - d)
    const auto solve = [](const FloatZRange &r, float sign, float *ab) {
        const double s = (double)r.maxZ - (double)r.minZ;
        ab[0] = (float)(-(double)r.m32 * s * sign);
        ab[1] = (float)((double)r.m22 * s + (double)r.minZ);
    };
    solve(viewmodel, -1.0f, out->viewmodel);
    out->viewmodel[2] = scene.minZ;
    out->viewmodel[3] = 0.0f;
    solve(scene, 1.0f, out->scene);
    out->scene[2] = clearValue;
    out->scene[3] = 0.0f;
}

std::string SourceRectVariant(const char *glsl)
{
    std::string text = glsl;
    const size_t eol = text.find('\n');
    text.insert(eol == std::string::npos ? text.size() : eol + 1, "#define DEKO9_SOURCE_RECT 1\n");
    return text;
}

void UpscaleSourceSetup(UpscaleSource *out, int texWidth, int texHeight, int x, int y, int w, int h)
{
    const float tw = (float)texWidth, th = (float)texHeight;
    out->uvTransform[0] = (float)x / tw;
    out->uvTransform[1] = (float)y / th;
    out->uvTransform[2] = (float)w / tw;
    out->uvTransform[3] = (float)h / th;
    out->uvClamp[0] = ((float)x + 0.5f) / tw;
    out->uvClamp[1] = ((float)y + 0.5f) / th;
    out->uvClamp[2] = ((float)(x + w) - 0.5f) / tw;
    out->uvClamp[3] = ((float)(y + h) - 0.5f) / th;
    out->texelRect[0] = x;
    out->texelRect[1] = y;
    out->texelRect[2] = x + w - 1;
    out->texelRect[3] = y + h - 1;
}

void SgsrSetup(SgsrConstants *out, int texWidth, int texHeight, const UpscaleSource &src)
{
    out->viewportInfo[0] = 1.0f / (float)texWidth;
    out->viewportInfo[1] = 1.0f / (float)texHeight;
    out->viewportInfo[2] = (float)texWidth;
    out->viewportInfo[3] = (float)texHeight;
    std::memcpy(out->texelRect, src.texelRect, sizeof(out->texelRect));
    std::memcpy(out->uvTransform, src.uvTransform, sizeof(out->uvTransform));
    std::memcpy(out->uvClamp, src.uvClamp, sizeof(out->uvClamp));
}

void SgsrSetup(SgsrConstants *out, int inWidth, int inHeight)
{
    UpscaleSource src;
    UpscaleSourceSetup(&src, inWidth, inHeight, 0, 0, inWidth, inHeight);
    SgsrSetup(out, inWidth, inHeight, src);
}

void BilinearSetup(BilinearConstants *out, const UpscaleSource &src)
{
    std::memcpy(out->uvTransform, src.uvTransform, sizeof(out->uvTransform));
    std::memcpy(out->srcClamp, src.uvClamp, sizeof(out->srcClamp));
}

void BilinearRcasSetup(BilinearRcasConstants *out, float sharpnessStops, int outWidth, int outHeight)
{
    // Whole texture: identity transform and a clamp that never binds (the
    // sampler's clamp-to-edge is the border behaviour, as before).
    UpscaleSource src;
    UpscaleSourceSetup(&src, 1, 1, 0, 0, 1, 1);
    src.uvClamp[0] = src.uvClamp[1] = 0.0f;
    src.uvClamp[2] = src.uvClamp[3] = 1.0f;
    BilinearRcasSetup(out, sharpnessStops, outWidth, outHeight, src);
}

void BilinearRcasSetup(BilinearRcasConstants *out, float sharpnessStops, int outWidth, int outHeight,
                       const UpscaleSource &src)
{
    std::memcpy(out->uvTransform, src.uvTransform, sizeof(out->uvTransform));
    std::memcpy(out->srcClamp, src.uvClamp, sizeof(out->srcClamp));
    out->step[0] = 1.0f / (float)outWidth;
    out->step[1] = 1.0f / (float)outHeight;
    // FsrRcasCon: transform from stops to linear value.
    out->step[2] = std::exp2(-sharpnessStops);
    out->step[3] = 0.0f;
    out->uvClamp[0] = 0.5f / (float)outWidth;
    out->uvClamp[1] = 0.5f / (float)outHeight;
    out->uvClamp[2] = 1.0f - 0.5f / (float)outWidth;
    out->uvClamp[3] = 1.0f - 0.5f / (float)outHeight;
}

} // namespace deko9
