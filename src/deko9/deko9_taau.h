#pragma once

// Temporal anti-aliased upscaling (r_taau) for the deko3d renderer.
//
// The engine offsets the scene projection by a sub-pixel jitter each frame
// (Halton 2,3 over kTaauPhases frames, in render pixels). At the dynamic
// resolution upscale site one full-screen pass at the output size:
//   - reconstructs the current frame at the output pixel from the 2x2
//     render texels around it, each weighted by the distance from its
//     jittered sample position (output pixels, a Gaussian-like polynomial,
//     or bilinear);
//   - takes the motion of the nearest-depth texel of the 2x2 around it: the
//     object motion texture where a moving object wrote it (TaauMotion),
//     otherwise the scene depth and the matrix that maps this frame's
//     unjittered clip space to the previous frame's; depth-hack (viewmodel)
//     pixels without object motion keep their screen position;
//   - fetches the history (Catmull-Rom in 5 bilinear taps, or one bilinear
//     tap), clamps it to the RGB range of the 4 texels, and blends; where the
//     transparent passes changed the pixel (its luma against a snapshot of
//     the opaque scene taken before them: TaauOpaque), the current frame
//     weighs more, so particles and effects do not smear into the history
//     (the change is taken right after them, before post effects);
//     where the clipped history and the current frame agree in luma it
//     weighs less (anti-flicker), and the blend is luma-weighted so a
//     bright sub-pixel sample cannot flash;
//   - writes the result to the output and to the next history image (two
//     output-sized RGB10A2 images, ping-ponged).
//
// This header and deko9_taau_shaders.cpp are host-compilable (no deko3d):
// switch_deko9_taau_test compiles the GLSL with the host UAM and checks the
// jitter sequence and matrix math. The device side is deko9_taau.cpp.

#include <cstdint>
#include <string>

struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;

namespace deko9
{

constexpr uint32_t kTaauPhases = 8;

// Halton (2, 3) point `index` (1-based internally, so index 0 is the first
// point, not the origin), centred to [-0.5, 0.5).
void TaauHalton(uint32_t index, float *x, float *y);

// Row-vector 4x4 matrices (clip = [x y z 1] * M), as the engine's GfxMatrix.
struct TaauMat
{
    double m[4][4];
};
TaauMat TaauMul(const TaauMat &a, const TaauMat &b);
// false when singular.
bool TaauInverse(const TaauMat &a, TaauMat *out);

// The projection's clip-space offset for a jitter of (jx, jy) render pixels
// (positive x right, positive y down the image) on a w x h viewport: added to
// m[2][0] / m[2][1] of a perspective matrix with m[2][3] = 1 (clip.w = view z),
// so NDC moves by the same amount at every depth.
void TaauJitterClip(float jx, float jy, uint32_t w, uint32_t h, float *m20, float *m21);
// The inverse: the render-pixel jitter carried by a projection.
void TaauJitterFromClip(float m20, float m21, uint32_t w, uint32_t h, float *jx, float *jy);

// Unjittered NDC of this frame -> clip of the previous frame:
// inverse(view * proj) * prevViewProj, with both projections unjittered.
// Computed in double: the world translation cancels before it reaches float.
bool TaauReprojection(const TaauMat &viewProj, const TaauMat &prevViewProj, TaauMat *out);
// A camera cut: the view moved further than `maxMove` units or turned by
// more than `maxTurnCos` (cosine of the angle between forward axes).
bool TaauCameraCut(const float origin[3], const float forward[3], const float prevOrigin[3],
                   const float prevForward[3], float maxMove, float maxTurnCos);

// The scene's texture LOD bias at a render/output scale: log2(scale) plus
// `offset`, rounded to the -k/8 steps Deko9_SetEngineLodBias keeps, k in
// 0..15 (never positive: native resolution samples as before).
float TaauLodBias(float scale, float offset);

// A zoom jump: the projection's x scale changed by more than a factor of
// `maxRatio` since the previous frame (0 = no previous frame: no jump).
bool TaauZoomCut(float scaleX, float prevScaleX, float maxRatio);

// What one resolve needs from the engine.
struct TaauFrame
{
    float jitter[2];      // render pixels (TaauJitterFromClip)
    float reproj[4][4];   // TaauReprojection, row-vector
    float viewmodelSplit; // window depth below which a pixel is depth-hack geometry
    float sceneMinZ, sceneMaxZ; // scene viewport depth range
    float farNdcZ;        // NDC z at infinity (the projection's m[2][2]); the depth clear lies beyond it
    float blend;          // history weight of the current frame at full confidence (0..1)
    float reactive;       // current-frame weight where transparents changed the pixel fully (0 = off)
    float antiFlicker;    // 0..1: how much less the current frame weighs where it agrees with the history
    float flat;           // 2x2 colour range (0..1 units) at or below which the history is skipped
    bool reset;           // ignore the history (first frame, cut, map load, resize)
    bool motion;          // the object motion texture is valid (set by the device)
    bool opaque;          // the transparents' luma change is valid (set by the device)
    bool bilinearHistory; // one bilinear history fetch (kTaauBilinearHistory)
    bool bilinearCurrent; // bilinear weights over the 2x2 (kTaauBilinearCurrent)
    // Reactive image used / allocated texels per axis (TaauReactiveUv; set
    // by the device): 1 when the image is exactly the used size.
    float reactiveUv[2] = {1.0f, 1.0f};
};

// Program variants (TaauVariant): the resolve takes the first two, the
// reactive pass the third.
constexpr uint32_t kTaauBilinearHistory = 1; // one bilinear history tap instead of Catmull-Rom
constexpr uint32_t kTaauBilinearCurrent = 2; // bilinear weights instead of the polynomial kernel
constexpr uint32_t kTaauHalfReactive = 4;    // the reactive image is half the scene size
constexpr uint32_t kTaauResolveVariants = 4;
std::string TaauVariant(const char *glsl, uint32_t flags);

// Reactive weight: (|luma - opaque luma| - threshold) * scale, clamped to
// 0..1, times TaauFrame::reactive.
constexpr float kTaauReactiveThreshold = 4.0f / 255.0f;
constexpr float kTaauReactiveScale = 8.0f;
// History youth (the resolve's 2-bit history alpha): a pixel without
// usable history (reset, off screen, reject motion, reactive) restarts at
// youth 1 and loses kTaauYouthStep per blended frame (1, 2/3, 1/3, 0 once
// stored in 2 bits; the step exceeds a level so filtered values still
// decay); the current frame weighs at least youth * kTaauYouthWeight
// (0.6, 0.4, 0.2), close to an even average of the first four frames.
constexpr float kTaauYouthWeight = 0.6f;
constexpr float kTaauYouthStep = 0.4f;

// The resolve kernel's unit in output pixels: 1.2, or 0.75 render pixels
// where that is wider (render scale below 0.625), so the nearest jittered
// sample (at most 0.71 render px from the output pixel in each 2x2) always
// has a positive weight. Returns kernel units per render pixel for a render
// pixel of `outputPerRender` output pixels (TaauConstants::scale).
constexpr float kTaauKernelOutputUnit = 1.2f;
constexpr float kTaauKernelRenderUnit = 0.75f;
inline float TaauKernelScale(float outputPerRender)
{
    const float renderUnit = kTaauKernelRenderUnit * outputPerRender;
    return outputPerRender / (renderUnit > kTaauKernelOutputUnit ? renderUnit : kTaauKernelOutputUnit);
}

// r_taauFlat default (8-bit units): a 2x2 whose colours differ by no more
// than this clamps the history onto the current colour within that range,
// so the resolve skips the history (TaauFrame::flat = this / 255).
constexpr float kTaauFlatDefault = 2.0f;

// std140 layout of the resolve's uniform block.
struct TaauConstants
{
    float reproj[4][4];  // rows k of (u.x, u.y, depth, 1) -> previous clip (x + w) / 2, (w - y) / 2, w: divided, the previous uv
    float pos[4];        // output px -> jittered render px - 0.5: fr = frag * xy + zw
    float uv[4];         // output px -> output uv: u = frag * xy + zw
    float gather[4];     // 2x2 gather uv = corner * xy + zw
    float texel[4];      // point-sample uv of render px p = p * xy + zw
    float dst[4];        // output rect w, h, x - 0.5, y - 0.5
    float hist[4];       // 1 / w, 1 / h, -1 / w, -1 / h of the history
    float hist2[4];      // 2 / w, 2 / h, 0.5 / w, 0.5 / h
    float scale[4];      // kernel units per render px x, y; flat threshold; 0
    float depth[4];      // viewmodel split, 0, 0, depth at infinity (the clear lies beyond it)
    float blend[4];      // blend, history valid (1, reset -1), 0, motion texture (0/1)
    float reactive[4];   // 0, threshold, scale (0: no reactive mask), weight
    float output[4];     // anti-flicker, youth weight, youth step, 0
};
void TaauSetup(TaauConstants *out, const int32_t srcRect[4], const int32_t dstRect[4], uint32_t colorWidth,
               uint32_t colorHeight, uint32_t histWidth, uint32_t histHeight, const TaauFrame &frame);

// Fragment program: sampler 0 = scene colour, 1 = scene depth, 3 = object
// motion (point, clamp), 4 = the transparents' luma change (point, or
// bilinear when it is half size); 2 = previous history (bilinear, clamp);
// uniform binding 1 = TaauConstants. The scene taps clamp at the scene
// texture's edge, so the scene rectangle should be the whole texture
// (r_renderScale lays it out so). Output 0 = the target, output 1 = the next
// history.
extern const char kTaauResolveGlsl[];

// Reactive passes: the luma of sampler 0's texel under the fragment (the
// mean of the 2x2 with kTaauHalfReactive), into an R16F image of the
// scene's size (before the transparents), then subtracted from it by
// blending (after them).
extern const char kTaauOpaqueGlsl[];

// The reactive image is allocated once for the scene colour's capacity (the
// size it was created at; dynamic resolution re-lays the scene out smaller
// inside its memory), so a scale change only changes the part in use: the
// top-left `used` texels plus a guard column and row (when the image is
// larger) holding copies of the last used ones, which is what the sampler's
// edge clamp returned when the image was exactly `used` texels.
struct TaauExtent
{
    uint32_t width, height;
    bool operator==(const TaauExtent &o) const { return width == o.width && height == o.height; }
    bool operator!=(const TaauExtent &o) const { return !(*this == o); }
};
// The texels a scene (or scene capacity) of width x height needs.
inline TaauExtent TaauReactiveExtent(uint32_t width, uint32_t height, bool half)
{
    const uint32_t d = half ? 2 : 1;
    return {(width + d - 1) / d, (height + d - 1) / d};
}
// The rectangle {x, y, w, h} the reactive passes render for scene
// rectangle `sr`: sr in reactive texels, plus the guard column / row where
// it reaches the used edge and the image has room.
inline void TaauReactiveRect(const int32_t sr[4], bool half, TaauExtent used, TaauExtent image, uint32_t out[4])
{
    const uint32_t d = half ? 2 : 1;
    out[0] = (uint32_t)sr[0] / d;
    out[1] = (uint32_t)sr[1] / d;
    out[2] = ((uint32_t)sr[2] + d - 1) / d;
    out[3] = ((uint32_t)sr[3] + d - 1) / d;
    out[2] += out[0] + out[2] == used.width && used.width < image.width;
    out[3] += out[1] + out[3] == used.height && used.height < image.height;
}
// TaauFrame::reactiveUv: scales the reactive tap's uv so the texel
// coordinate in the allocated image is the one an image of `used` texels had.
inline void TaauReactiveUv(TaauExtent used, TaauExtent image, float out[2])
{
    out[0] = used.width == image.width ? 1.0f : (float)used.width / (float)image.width;
    out[1] = used.height == image.height ? 1.0f : (float)used.height / (float)image.height;
}

// Object motion texture (RG16F, scene size): previous minus current
// unjittered UV of the surface seen at a texel, or one of these markers.
// Static geometry is never drawn into it: its texels keep kTaauMotionNone
// and the resolve reprojects them by depth. A real motion stays within
// (-1, 1) (else its history lies off screen); the resolve splits at +-8.
constexpr float kTaauMotionNone = -16.0f;
constexpr float kTaauMotionReject = 16.0f;  // no usable history (spawn, teleport)

// One moving surface for the motion pass: indexed triangles whose float3
// positions sit at offset 0 of `stride`-byte vertices. The previous
// positions come from prevVb (the last frame's skinned vertices) or, with
// prevVb null, from vb itself (a rigid surface: only the transform moved).
struct TaauMotionDraw
{
    IDirect3DVertexBuffer9 *vb;
    uint32_t vbOffset; // bytes
    IDirect3DVertexBuffer9 *prevVb;
    uint32_t prevVbOffset;
    IDirect3DIndexBuffer9 *ib; // 16-bit indices
    uint32_t firstIndex, indexCount, stride;
    uint32_t vertexCount; // the indices address vertices [0, vertexCount) from either offset
    float cur[4][4];  // object -> this frame's unjittered clip (depth-hack projection for the viewmodel)
    float prev[4][4]; // object (previous positions) -> the previous frame's unjittered clip
    bool viewmodel;   // depth-hack depth range
    bool reject;
};

// Per-view state of the motion pass.
struct TaauMotionView
{
    float jitterClip[2]; // the projection's jitter (m[2][0], m[2][1]): clip x/y offset per unit w
    float sceneDepth[2], viewmodelDepth[2]; // viewport depth ranges (min, max)
};

// std140 layout of the motion vertex program's uniform block.
struct TaauMotionConstants
{
    float curCol[4][4];  // columns of TaauMotionDraw::cur
    float prevCol[4][4]; // columns of TaauMotionDraw::prev; zero for a reject draw
    float jitter[4];     // jitter clip x, y, 0, 0
};
void TaauMotionSetup(TaauMotionConstants *out, const TaauMotionDraw &draw, const TaauMotionView &view);

// The motion pass draws with depth test LEQUAL against the scene depth (no
// writes), pulled toward the camera by this constant and slope bias so the
// surface's own depth, computed by different math than the engine's
// program, still passes. Constant in units of the 24-bit depth resolution.
constexpr float kTaauMotionDepthBias = -16.0f;
constexpr float kTaauMotionSlopeBias = -1.0f;

// Range check of one motion draw against its buffers (bytes; the index
// buffer in 16-bit indices). The offsets come from the previous frame's
// tables, so a failing draw is skipped by the pass, never fatal.
enum class TaauMotionFit
{
    Ok,
    IndexRange,      // firstIndex + indexCount past the index buffer
    VertexRange,     // vbOffset + vertexCount * stride past vb (or no vertices)
    PrevVertexRange, // the same for the previous positions
};
inline TaauMotionFit TaauMotionDrawFits(uint64_t ibIndices, uint32_t firstIndex, uint32_t indexCount, uint64_t vbSize,
                                        uint32_t vbOffset, uint64_t prevSize, uint32_t prevOffset,
                                        uint32_t vertexCount, uint32_t stride)
{
    if ((uint64_t)firstIndex + indexCount > ibIndices)
        return TaauMotionFit::IndexRange;
    const uint64_t bytes = (uint64_t)vertexCount * stride;
    if (!vertexCount || vbOffset + bytes > vbSize)
        return TaauMotionFit::VertexRange;
    if (prevOffset + bytes > prevSize)
        return TaauMotionFit::PrevVertexRange;
    return TaauMotionFit::Ok;
}
inline const char *TaauMotionFitName(TaauMotionFit fit)
{
    switch (fit)
    {
    case TaauMotionFit::Ok:
        return "ok";
    case TaauMotionFit::IndexRange:
        return "index range";
    case TaauMotionFit::VertexRange:
        return "vertex range";
    case TaauMotionFit::PrevVertexRange:
        return "previous vertex range";
    }
    return "?";
}

// Vertex program: attribute 0 = current position, 1 = previous position;
// uniform binding 0 = TaauMotionConstants. Fragment program: output 0 =
// the motion (RG16F).
extern const char kTaauMotionVertexGlsl[];
extern const char kTaauMotionFragmentGlsl[];

} // namespace deko9
