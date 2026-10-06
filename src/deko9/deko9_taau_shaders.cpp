// TAAU resolve program and its CPU-side math (see deko9_taau.h).

#include "deko9_taau.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>

namespace deko9
{

namespace
{
float RadicalInverse(uint32_t i, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    while (i)
    {
        f /= (float)base;
        r += f * (float)(i % base);
        i /= base;
    }
    return r;
}
} // namespace

void TaauHalton(uint32_t index, float *x, float *y)
{
    const uint32_t i = index % kTaauPhases + 1;
    *x = RadicalInverse(i, 2) - 0.5f;
    *y = RadicalInverse(i, 3) - 0.5f;
}

TaauMat TaauMul(const TaauMat &a, const TaauMat &b)
{
    TaauMat r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

bool TaauInverse(const TaauMat &a, TaauMat *out)
{
    // Gauss-Jordan with partial pivoting.
    double w[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            w[i][j] = a.m[i][j];
            w[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    for (int c = 0; c < 4; ++c)
    {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(w[r][c]) > std::fabs(w[p][c]))
                p = r;
        if (std::fabs(w[p][c]) < 1e-300)
            return false;
        if (p != c)
            for (int j = 0; j < 8; ++j)
            {
                const double t = w[c][j];
                w[c][j] = w[p][j];
                w[p][j] = t;
            }
        const double inv = 1.0 / w[c][c];
        for (int j = 0; j < 8; ++j)
            w[c][j] *= inv;
        for (int r = 0; r < 4; ++r)
        {
            if (r == c || w[r][c] == 0.0)
                continue;
            const double f = w[r][c];
            for (int j = 0; j < 8; ++j)
                w[r][j] -= f * w[c][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out->m[i][j] = w[i][j + 4];
    return true;
}

void TaauJitterClip(float jx, float jy, uint32_t w, uint32_t h, float *m20, float *m21)
{
    *m20 = 2.0f * jx / (float)w;
    *m21 = -2.0f * jy / (float)h;
}

void TaauJitterFromClip(float m20, float m21, uint32_t w, uint32_t h, float *jx, float *jy)
{
    *jx = m20 * (float)w * 0.5f;
    *jy = -m21 * (float)h * 0.5f;
}

bool TaauReprojection(const TaauMat &viewProj, const TaauMat &prevViewProj, TaauMat *out)
{
    TaauMat inv;
    if (!TaauInverse(viewProj, &inv))
        return false;
    *out = TaauMul(inv, prevViewProj);
    return true;
}

bool TaauCameraCut(const float origin[3], const float forward[3], const float prevOrigin[3],
                   const float prevForward[3], float maxMove, float maxTurnCos)
{
    const float dx = origin[0] - prevOrigin[0], dy = origin[1] - prevOrigin[1], dz = origin[2] - prevOrigin[2];
    const float dot = forward[0] * prevForward[0] + forward[1] * prevForward[1] + forward[2] * prevForward[2];
    return dx * dx + dy * dy + dz * dz > maxMove * maxMove || dot < maxTurnCos;
}

bool TaauZoomCut(float scaleX, float prevScaleX, float maxRatio)
{
    if (!(prevScaleX > 0.0f) || !(scaleX > 0.0f))
        return false;
    const float r = scaleX / prevScaleX;
    return r > maxRatio || r * maxRatio < 1.0f;
}

float TaauLodBias(float scale, float offset)
{
    if (!(scale > 0.0f))
        return 0.0f;
    const float bias = std::log2(scale) + offset;
    float steps = std::round(-bias * 8.0f);
    steps = steps < 0.0f ? 0.0f : steps > 15.0f ? 15.0f : steps;
    return steps ? -steps / 8.0f : 0.0f;
}

void TaauSetup(TaauConstants *out, const int32_t srcRect[4], const int32_t dstRect[4], uint32_t colorWidth,
               uint32_t colorHeight, uint32_t histWidth, uint32_t histHeight, const TaauFrame &frame)
{
    std::memset(out, 0, sizeof(*out));
    // Rows k of (u.x, u.y, z, 1) -> previous clip, u the output uv: the NDC
    // mapping ndc = (2 u.x - 1, 1 - 2 u.y) in, (x + w) / 2, (w - y) / 2, w
    // out (divided by w: the previous uv); z = (depth - min) / (max - min)
    // folded into the depth and constant rows.
    double r[4][3] = {};
    for (int c = 0; c < 4; ++c)
    {
        const double n[4] = {2.0 * frame.reproj[0][c], -2.0 * frame.reproj[1][c], frame.reproj[2][c],
                             (double)frame.reproj[3][c] + frame.reproj[1][c] - frame.reproj[0][c]};
        for (int k = 0; k < 4; ++k)
        {
            if (c == 0)
                r[k][0] += 0.5 * n[k];
            else if (c == 1)
                r[k][1] -= 0.5 * n[k];
            else if (c == 3)
            {
                r[k][0] += 0.5 * n[k];
                r[k][1] += 0.5 * n[k];
                r[k][2] = n[k];
            }
        }
    }
    const double zScale = 1.0 / ((double)frame.sceneMaxZ - frame.sceneMinZ);
    for (int c = 0; c < 3; ++c)
    {
        r[3][c] -= frame.sceneMinZ * zScale * r[2][c];
        r[2][c] *= zScale;
    }
    // The program feeds the fragment position (u = (frag - rect x, y) /
    // rect w, h) and wants history px - 0.5 (uv * rect w, h + rect x, y - 0.5).
    const double rect[4] = {(double)dstRect[0], (double)dstRect[1], (double)dstRect[2], (double)dstRect[3]};
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 2; ++i)
            r[k][i] = r[k][i] * rect[2 + i] + (rect[i] - 0.5) * r[k][2];
    for (int c = 0; c < 3; ++c)
    {
        r[3][c] -= rect[0] / rect[2] * r[0][c] + rect[1] / rect[3] * r[1][c];
        r[0][c] /= rect[2];
        r[1][c] /= rect[3];
    }
    for (int k = 0; k < 4; ++k)
        for (int c = 0; c < 3; ++c)
            out->reproj[k][c] = (float)r[k][c];
    const float inv[2] = {1.0f / (float)colorWidth, 1.0f / (float)colorHeight};
    const float src[4] = {(float)srcRect[0], (float)srcRect[1], (float)srcRect[2], (float)srcRect[3]};
    const float dst[4] = {(float)dstRect[0], (float)dstRect[1], (float)dstRect[2], (float)dstRect[3]};
    for (int i = 0; i < 2; ++i)
    {
        out->pos[i] = src[2 + i] / dst[2 + i];
        out->pos[2 + i] = frame.jitter[i] - dst[i] * out->pos[i] - 0.5f;
        out->edge[i] = dst[i] - 0.5f + 0.5f * dst[2 + i];
        out->edge[2 + i] = 0.5f * dst[2 + i];
        out->gather[i] = inv[i];
        out->gather[2 + i] = (src[i] + 1.0f) * inv[i];
        out->texel[i] = inv[i] * frame.reactiveUv[i];
        out->texel[2 + i] = (src[i] + 0.5f) * inv[i] * frame.reactiveUv[i];
        out->dst[i] = dst[2 + i];
        const float histInv = 1.0f / (float)(i ? histHeight : histWidth);
        out->hist[i] = histInv;
        out->hist[2 + i] = -histInv;
        out->hist2[i] = 2.0f * histInv;
        out->hist2[2 + i] = 0.5f * histInv;
        out->scale[i] = TaauKernelScale(dst[2 + i] / src[2 + i]);
    }
    out->scale[2] = frame.flat;
    out->scale[3] = frame.bilinearRange;
    out->depth[0] = frame.viewmodelSplit;
    out->depth[1] = kTaauHistoryStillBar;
    out->depth[3] = frame.sceneMinZ + frame.farNdcZ * (frame.sceneMaxZ - frame.sceneMinZ);
    out->blend[0] = frame.blend;
    out->blend[1] = frame.reset ? -1.0f : 1.0f;
    out->blend[3] = frame.motion ? FLT_MAX : kTaauMotionNone;
    out->reactive[1] = kTaauReactiveThreshold;
    out->reactive[2] = frame.opaque && frame.reactive > 0.0f ? kTaauReactiveScale : 0.0f;
    out->reactive[3] = frame.reactive;
    out->output[0] = frame.antiFlicker;
    out->output[1] = kTaauYouthWeight;
    out->output[2] = kTaauYouthStep;
    out->output[3] = 4.0f * kTaauAntiFlickerLumaFloor;
}

// TAAU_HALF: the image is half the scene's size; one bilinear tap at the
// shared corner of the 2x2 scene texels under the pixel is their mean.
// A fragment past the last used texel (the guard column / row) repeats it.
const char kTaauOpaqueGlsl[] = R"GLSL(#version 460
layout(location = 0) out vec4 outLuma;
layout(binding = 0) uniform sampler2D uColor;
void main()
{
#ifdef TAAU_HALF
    vec2 size = vec2(textureSize(uColor, 0));
    vec2 at = min(gl_FragCoord.xy, ceil(size * 0.5) - 0.5);
    vec3 c = textureLod(uColor, at * 2.0 / size, 0.0).rgb;
#else
    vec3 c = texelFetch(uColor, min(ivec2(gl_FragCoord.xy), textureSize(uColor, 0) - 1), 0).rgb;
#endif
    outLuma = vec4(dot(c, vec3(0.25, 0.5, 0.25)), 0.0, 0.0, 1.0);
}
)GLSL";

void TaauMotionSetup(TaauMotionConstants *out, const TaauMotionDraw &draw, const TaauMotionView &view)
{
    std::memset(out, 0, sizeof(*out));
    // A reject draw keeps the zero previous matrix: clip w 0 marks it.
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            out->curCol[c][r] = draw.cur[r][c];
            if (!draw.reject)
                out->prevCol[c][r] = draw.prev[r][c];
        }
    out->jitter[0] = view.jitterClip[0];
    out->jitter[1] = view.jitterClip[1];
}

const char kTaauMotionVertexGlsl[] = R"GLSL(#version 460
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aPrev;
layout(location = 0) out vec4 vCur;
layout(location = 1) out vec4 vPrev;
layout(std140, binding = 0) uniform MotionBlock
{
    vec4 uCur[4];  // columns: object -> unjittered clip, this frame
    vec4 uPrev[4]; // columns: previous object position -> previous clip (zero: reject)
    vec4 uJitter;  // clip jitter per unit w
};
void main()
{
    vec4 p = vec4(aPos, 1.0), q = vec4(aPrev, 1.0);
    vCur = vec4(dot(p, uCur[0]), dot(p, uCur[1]), dot(p, uCur[2]), dot(p, uCur[3]));
    vPrev = vec4(dot(q, uPrev[0]), dot(q, uPrev[1]), dot(q, uPrev[2]), dot(q, uPrev[3]));
    // Rasterise where the scene pass did: with this frame's jitter.
    gl_Position = vec4(vCur.xy + uJitter.xy * vCur.w, vCur.zw);
}
)GLSL";

const char kTaauMotionFragmentGlsl[] = R"GLSL(#version 460
layout(location = 0) in vec4 vCur;
layout(location = 1) in vec4 vPrev;
layout(location = 0) out vec4 outMotion;
void main()
{
    // NDC +Y is the top row: v = 0.5 - y / 2. Rejected (zero previous
    // matrix) or behind the previous camera: no history.
    vec2 c = vCur.xy / vCur.w, p = vPrev.xy / max(vPrev.w, 1e-20);
    vec2 motion = vec2((p.x - c.x) * 0.5, (c.y - p.y) * 0.5);
    outMotion = vec4(vPrev.w > 0.0 ? motion : vec2(16.0), 0.0, 0.0);
}
)GLSL";

// Written for the instruction count (the pass runs per output pixel, like
// the SGSR upscale it replaces): the 2x2 texels around the sample point come
// from three colour gathers and one depth gather at one uv, a polynomial
// kernel weighs them, their RGB range bounds the history, a flat 2x2 skips
// the history (its clamp would give the current colour), the validity tests
// are one sign, and the Catmull-Rom history fetch (TAAU_BILINEAR_HISTORY:
// one bilinear tap, blurrier in motion) uses factored weights and falls
// back to the one tap where the five taps cannot change the result.
// TAAU_BILINEAR_CURRENT weighs the 4 texels bilinearly instead of by the
// polynomial kernel (a wider, cheaper tent).
const char kTaauResolveGlsl[] = R"GLSL(#version 460
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outHistory;
layout(binding = 0) uniform sampler2D uColor;
layout(binding = 1) uniform sampler2D uDepth;
layout(binding = 2) uniform sampler2D uHistory;
layout(binding = 3) uniform sampler2D uMotion;
layout(binding = 4) uniform sampler2D uReactiveDelta;
layout(std140, binding = 1) uniform TaauBlock
{
    vec4 uReproj[4];  // rows: (x w, y w, w) of (frag.x, frag.y, z, 1); x, y in history px - 0.5
    vec4 uPos;        // output px -> jittered render px - 0.5: fr = frag * xy + zw
    vec4 uEdge;       // output rect centre x, y in history px - 0.5; half its width, height
    vec4 uGather;     // 2x2 gather uv = corner * xy + zw
    vec4 uTexel;      // point-sample uv = (render px - 0.5) * xy + zw
    vec4 uDst;        // output rect w, h (motion uv -> px); 0, 0
    vec4 uHist;       // 1/w, 1/h, -1/w, -1/h of the history
    vec4 uHist2;      // 2/w, 2/h, 0.5/w, 0.5/h
    vec4 uScale;      // kernel units per render px x, y; flat threshold; one-tap history range
    vec4 uDepthParm;  // viewmodel split, one-tap history position bar (f (1 - f)), 0, depth at infinity
    vec4 uBlend;      // blend, history valid sign, 0, motion.x cap (FLT_MAX, or -16: no texture)
    vec4 uReactive;   // 0, threshold, scale (0: no mask), weight
    vec4 uOutput;     // anti-flicker, youth weight, youth step, anti-flicker luma floor (x4)
};

// Four times the luma (0.25, 0.5, 0.25): two instructions instead of three;
// the blend below is written for the scaled value.
float Luma4(vec3 c)
{
    return (c.r + c.b) + 2.0 * c.g;
}

// The history at `fr` + 0.5 (history pixels); alpha carries the pixel's
// youth (see main). The sampler clamps at the history's edge. `spread` is
// the 2x2 colour range the result is clamped to.
vec4 History(vec2 fr, float spread)
{
#ifndef TAAU_BILINEAR_HISTORY
    // Catmull-Rom in 5 bilinear taps (the four corner taps dropped). With
    // e = 1 - f and W = 2 + f e (twice the middle pair's weight), each tap
    // weighs relative to the centre tap: the outer taps -f e e / W and
    // -f e f / W per axis, and the five sum to 1 - f e / W (x) - f e / W (y).
    // The middle pair's bilinear offset is f (1 + f + 3 f e) / W.
    vec2 fl = floor(fr);
    vec2 f = fr - fl;
    vec2 fe = f - f * f;
    // One bilinear tap where the five cannot show: at a texel centre (f (1 - f)
    // below the bar: a still camera, the viewmodel; the kernel is then the
    // centre tap alone), or where the clamp range is below uScale.w (both
    // filtered values land in that range, so they differ by less than it).
    // Both cases hold across whole regions, so a warp rarely runs both paths.
    if (max(fe.x, fe.y) < uDepthParm.y || spread < uScale.w)
        return textureLod(uHistory, fr * uHist.xy + uHist2.zw, 0.0);
    vec2 rw = 1.0 / (fe + 2.0);
    vec2 q = fe * rw;
    // The common factor folded into the weights keeps one value, not two,
    // alive across the taps.
    float norm = 1.0 / (1.0 - q.x - q.y);
    q *= norm;
    vec2 n3 = -q * f, n0 = q * f - q;
    vec2 off = (f + f * (f + 3.0 * fe)) * rw;
    vec2 base = fl * uHist.xy + uHist2.zw;
    vec2 t0 = base + uHist.zw, t3 = base + uHist2.xy, t12 = off * uHist.xy + base;
    vec4 centre = textureLod(uHistory, t12, 0.0);
    vec3 r = centre.rgb * norm + textureLod(uHistory, vec2(t12.x, t0.y), 0.0).rgb * n0.y +
             textureLod(uHistory, vec2(t0.x, t12.y), 0.0).rgb * n0.x +
             textureLod(uHistory, vec2(t3.x, t12.y), 0.0).rgb * n3.x +
             textureLod(uHistory, vec2(t12.x, t3.y), 0.0).rgb * n3.y;
    return vec4(r, centre.a);
#else
    return textureLod(uHistory, fr * uHist.xy + uHist2.zw, 0.0);
#endif
}

void main()
{
    // The 2x2 scene texels around the sample point: one gather uv for the
    // colour and the depth. Gather order: x (0,1), y (1,1), z (1,0), w (0,0).
    vec2 fr = gl_FragCoord.xy * uPos.xy + uPos.zw;
    vec2 corner = floor(fr);
    vec2 guv = corner * uGather.xy + uGather.zw;
    vec4 r = textureGather(uColor, guv, 0), g = textureGather(uColor, guv, 1), b = textureGather(uColor, guv, 2);

    // Current frame over the 4 texels, whose RGB range bounds the history.
#ifdef TAAU_BILINEAR_CURRENT
    // Bilinear weights (a tent one render px wide; the weights sum to 1).
    vec2 f = fr - corner, e = 1.0 - f;
    vec4 w = vec4(e.x * f.y, f.x * f.y, f.x * e.y, e.x * e.y);
    float wmax = max(max(w.x, w.y), max(w.z, w.w));
    vec3 current = vec3(dot(r, w), dot(g, w), dot(b, w));
#else
    // A (1 - d^2)^2 kernel in kernel units (TaauKernelScale).
    // 1 - x^2 - y^2 as (1 - x^2) - y^2: the x terms are shared by two texels.
    vec2 f0 = (fr - corner) * uScale.xy, f1 = f0 - uScale.xy;
    vec2 s = vec2(1.0 - f0.x * f0.x, 1.0 - f1.x * f1.x);
    vec4 w = clamp(vec4(s.x - f1.y * f1.y, s.y - f1.y * f1.y, s.y - f0.y * f0.y, s.x - f0.y * f0.y), 0.0, 1.0);
    w *= w;
    float wmax = max(max(w.x, w.y), max(w.z, w.w));
    vec3 current = vec3(dot(r, w), dot(g, w), dot(b, w)) / max(dot(w, vec4(1.0)), 9.5367431640625e-7);
#endif
    vec3 lo = vec3(min(min(r.x, r.y), min(r.z, r.w)), min(min(g.x, g.y), min(g.z, g.w)), min(min(b.x, b.y), min(b.z, b.w)));
    vec3 hi = vec3(max(max(r.x, r.y), max(r.z, r.w)), max(max(g.x, g.y), max(g.z, g.w)), max(max(b.x, b.y), max(b.z, b.w)));

    // Reactive: the transparent passes changed this pixel's luma; the
    // history remembers it for one more frame, so a particle's trail clears
    // as fast as the particle.
    float delta = textureLod(uReactiveDelta, fr * uTexel.xy + uTexel.zw, 0.0).x;
    float reactive = clamp((abs(delta) - uReactive.y) * uReactive.z, 0.0, 1.0);

    // Youth (the history's 2-bit alpha): 1 where the history restarted from
    // the current frame alone, then one level less per blended frame; it
    // floors the current frame's weight at youth * uOutput.y, so a fresh
    // pixel averages its first frames evenly instead of keeping the aliased
    // first one for 1 / blend frames. Reactive pixels count as fresh.
    vec3 result = current;
    float youth = reactive;
    vec3 range = hi - lo;
    float spread = max(max(range.r, range.g), range.b);
    // A flat 2x2 clamps any history onto the current colour: skip it.
    if (spread > uScale.z)
    {
        youth = 1.0;
        // Nearest depth of the 2x2: edges take the motion of the foreground.
        vec4 dg = textureGather(uDepth, guv, 0);
        vec2 col = min(dg.xy, dg.wz);
        float dz = min(col.x, col.y);
        vec2 near = vec2(col.y < col.x, min(dg.x, dg.y) < min(dg.w, dg.z));

        // Per-object motion at the nearest texel, else depth
        // reprojection; the viewmodel (in front of the split) stays put.
        // Depth clear (sky) and anything past it reprojects as infinitely far.
        vec2 motion = textureLod(uMotion, guv + (near - 0.5) * uGather.xy, 0.0).xy;
        // Without a motion texture (another image is bound) the cap is the
        // no-motion marker; motion.y is only read with motion.x past -8.
        motion.x = min(motion.x, uBlend.w);
        float z = min(dz, uDepthParm.w);
        // Positions below are history px - 0.5 (this pixel: frag - 0.5).
        vec3 prev = gl_FragCoord.x * uReproj[0].xyz + gl_FragCoord.y * uReproj[1].xyz + z * uReproj[2].xyz +
                    uReproj[3].xyz;
        vec2 reproj = prev.xy * (1.0 / prev.z);
        vec2 here = gl_FragCoord.xy - 0.5;
        vec2 moved = motion * uDst.xy + here;
        bool useMotion = motion.x > -8.0;
        bool viewmodel = dz < uDepthParm.x;
        vec2 still = viewmodel ? here : reproj;
        // Positive when the history is usable: not a reset frame, no reject
        // motion, a point in front of the previous camera (or the
        // viewmodel), and inside the history. The bar is 2^-20, not 0: a
        // tiny prev.z makes 1 / prev.z infinite (0 * inf = NaN when prev.xy
        // is 0) and min/max drop a NaN edge term, so the reprojected position
        // is used only where prev.z exceeds the bar and its reciprocal is finite.
        vec2 prevX;
        float good;
        if (useMotion)
        {
            prevX = moved;
            good = 8.0 - motion.x;
        }
        else
        {
            prevX = still;
            good = max(uDepthParm.x - dz, prev.z);
        }
        vec2 edge = uEdge.zw - abs(prevX - uEdge.xy);
        good = min(min(good, min(edge.x, edge.y)), uBlend.y);
        if (good > 9.5367431640625e-7)
        {
            vec4 h = History(prevX, spread);
            vec3 hist = clamp(h.rgb, lo, hi);
            float histY = Luma4(hist), currentY = Luma4(current);
            float alpha = clamp(uBlend.x * wmax, 0.0, 1.0);
            // Anti-flicker: agreeing luma (relative difference near 0)
            // trusts the history more; disagreement keeps the full weight.
            // The relative difference is the same for the scaled lumas.
            float agree = 1.0 - clamp(abs(currentY - histY) / max(max(currentY, histY), uOutput.w), 0.0, 1.0);
            alpha -= alpha * (uOutput.x * agree) * agree;
            alpha = max(alpha, max(reactive * uReactive.w, h.a * uOutput.y));
            youth = max(reactive, h.a - uOutput.z);
            // Luma-weighted blend (1 / (1 + luma) per side, both sides
            // scaled by 4 with the lumas) as one lerp.
            float t = alpha * (4.0 + histY);
            t /= t + (1.0 - alpha) * (4.0 + currentY);
            result = mix(hist, current, t);
        }
    }
    outColor = vec4(result, 1.0);
    outHistory = vec4(result, youth);
}
)GLSL";

std::string TaauVariant(const char *glsl, uint32_t flags)
{
    std::string text = glsl;
    const size_t eol = text.find('\n');
    std::string defines;
    if (flags & kTaauBilinearHistory)
        defines += "#define TAAU_BILINEAR_HISTORY 1\n";
    if (flags & kTaauBilinearCurrent)
        defines += "#define TAAU_BILINEAR_CURRENT 1\n";
    if (flags & kTaauHalfReactive)
        defines += "#define TAAU_HALF 1\n";
    text.insert(eol == std::string::npos ? text.size() : eol + 1, defines);
    return text;
}

} // namespace deko9
