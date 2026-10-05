// Host proof for the TAAU math and program (src/deko9/deko9_taau.h,
// deko9_taau_shaders.cpp; r_taau):
//   - the resolve GLSL compiles to DKSH with UAM;
//   - the Halton (2,3) jitter sequence: 8 distinct phases in [-0.5, 0.5),
//     centred, periodic;
//   - the projection jitter moves every depth by the same sub-pixel amount
//     and TaauJitterFromClip recovers it;
//   - the reprojection matrix, applied in float as the shader does, maps a
//     world point seen this frame to where the previous camera saw it
//     (large world coordinates, rotation + translation, sky at infinity);
//   - the constant block layout and the camera-cut test;
//   - the resolve's instruction count (it runs per output pixel in place
//     of the SGSR upscale): within budget, printed next to SGSR's;
//   - the motion programs compile, and the motion the fragment program
//     derives from the two matrices (evaluated on the CPU as it does) is a
//     moving point's previous minus current screen position.

#include "src/deko9/deko9_fsr.h"
#include "src/deko9/deko9_shader.h"
#include "src/deko9/deko9_taau.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int g_failures;

void Check(bool ok, const char *what, const char *detail = "")
{
    std::printf("%s:DEKO9_TAAU_%s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok)
        ++g_failures;
}

using deko9::TaauMat;

// The engine's InfinitePerspectiveMatrix (row-vector, clip.w = view z).
TaauMat Projection(float tanX, float tanY, float zNear)
{
    const float k = 0.99951172f;
    TaauMat p{};
    p.m[0][0] = k / tanX;
    p.m[1][1] = k / tanY;
    p.m[2][2] = k;
    p.m[2][3] = 1.0;
    p.m[3][2] = -zNear * k;
    return p;
}

// Rigid view transform: world -> view (x right, y up, z forward), camera at
// `eye` turned by `yaw` about y and `pitch` about x.
TaauMat View(const double eye[3], double yaw, double pitch)
{
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    // Rows of the camera basis in world space.
    const double right[3] = {cy, 0.0, -sy};
    const double up[3] = {sy * sp, cp, cy * sp};
    const double fwd[3] = {sy * cp, -sp, cy * cp};
    TaauMat v{};
    for (int i = 0; i < 3; ++i)
    {
        v.m[i][0] = right[i];
        v.m[i][1] = up[i];
        v.m[i][2] = fwd[i];
    }
    v.m[3][0] = -(eye[0] * right[0] + eye[1] * right[1] + eye[2] * right[2]);
    v.m[3][1] = -(eye[0] * up[0] + eye[1] * up[1] + eye[2] * up[2]);
    v.m[3][2] = -(eye[0] * fwd[0] + eye[1] * fwd[1] + eye[2] * fwd[2]);
    v.m[3][3] = 1.0;
    return v;
}

// Maxwell instructions in a fragment program's DKSH (scheduling words and
// NOPs excluded), -1 when it does not compile.
int Instructions(const std::string &glsl)
{
    std::vector<uint8_t> dksh;
    std::string error;
    if (!Deko9_CompileDksh(DEKO9_STAGE_PIXEL, glsl.c_str(), &dksh, &error) || dksh.size() < 24)
        return -1;
    uint32_t hdr[6];
    std::memcpy(hdr, dksh.data(), sizeof(hdr));
    const size_t start = (size_t)hdr[2] + 0x80, end = (size_t)hdr[2] + hdr[3];
    int count = 0;
    for (size_t at = start, word = 0; at + 8 <= end && at + 8 <= dksh.size(); at += 8, ++word)
    {
        uint64_t w;
        std::memcpy(&w, dksh.data() + at, 8);
        if (word % 4 && w && w != 0x50b0000000070f00ull)
            ++count;
    }
    return count;
}

// The resolve's depth-reprojection validity and history uv
// (kTaauResolveGlsl, no object motion) in float: fminf/fmaxf drop a NaN
// operand as the GPU's min/max do. `prev` is the folded previous clip
// (x, y, w); `gap` = viewmodel split - nearest depth (> 0: viewmodel);
// `bar` is the sign test's threshold (2^-20 in the program, 0 before).
struct Reprojected
{
    bool good;
    float u[2];
};
Reprojected ResolveReproject(const float prev[3], const float u[2], float gap, float resetSign, float bar)
{
    const float rz = 1.0f / prev[2];
    const float reproj[2] = {prev[0] * rz, prev[1] * rz};
    const bool viewmodel = gap > 0.0f;
    Reprojected r;
    r.u[0] = viewmodel ? u[0] : reproj[0];
    r.u[1] = viewmodel ? u[1] : reproj[1];
    const float edge = std::fmax(std::fabs(r.u[0] - 0.5f), std::fabs(r.u[1] - 0.5f));
    const float good = std::fmin(std::fmin(std::fmax(gap, prev[2]), 0.5f - edge), resetSign);
    r.good = good > bar;
    return r;
}

void Project(const TaauMat &vp, const double p[4], double out[4])
{
    for (int c = 0; c < 4; ++c)
        out[c] = p[0] * vp.m[0][c] + p[1] * vp.m[1][c] + p[2] * vp.m[2][c] + p[3] * vp.m[3][c];
}

} // namespace

int main()
{
    const struct
    {
        Deko9Stage stage;
        const char *glsl, *name;
    } programs[] = {{DEKO9_STAGE_PIXEL, deko9::kTaauResolveGlsl, "COMPILE_RESOLVE"},
                    {DEKO9_STAGE_VERTEX, deko9::kTaauMotionVertexGlsl, "COMPILE_MOTION_VS"},
                    {DEKO9_STAGE_PIXEL, deko9::kTaauMotionFragmentGlsl, "COMPILE_MOTION_PS"}};
    for (const auto &program : programs)
    {
        std::vector<uint8_t> dksh;
        std::string error;
        const bool ok = Deko9_CompileDksh(program.stage, program.glsl, &dksh, &error);
        uint32_t hdr[6] = {}, gprs = 0;
        if (dksh.size() >= sizeof(hdr))
            std::memcpy(hdr, dksh.data(), sizeof(hdr));
        if (hdr[4] && hdr[4] + 12 <= dksh.size())
            std::memcpy(&gprs, dksh.data() + hdr[4] + 8, 4);
        char detail[200];
        std::snprintf(detail, sizeof(detail), "bytes=%zu code=%u gprs=%u %s", dksh.size(), hdr[3], gprs,
                      error.c_str());
        Check(ok && dksh.size() > 4 && !std::memcmp(dksh.data(), "DKSH", 4), program.name, detail);
    }

    {
        // Each variant costs less than the one it simplifies.
        const int resolve = Instructions(deko9::kTaauResolveGlsl);
        const int bilinearHistory =
            Instructions(deko9::TaauVariant(deko9::kTaauResolveGlsl, deko9::kTaauBilinearHistory));
        const int bilinearCurrent =
            Instructions(deko9::TaauVariant(deko9::kTaauResolveGlsl, deko9::kTaauBilinearCurrent));
        const int both = Instructions(
            deko9::TaauVariant(deko9::kTaauResolveGlsl, deko9::kTaauBilinearHistory | deko9::kTaauBilinearCurrent));
        const int opaque = Instructions(deko9::kTaauOpaqueGlsl);
        const int opaqueHalf = Instructions(deko9::TaauVariant(deko9::kTaauOpaqueGlsl, deko9::kTaauHalfReactive));
        const int sgsr = Instructions(deko9::kSgsrGlsl);
        char detail[200];
        std::snprintf(detail, sizeof(detail),
                      "resolve=%d bilinear_history=%d bilinear_current=%d both=%d opaque=%d opaque_half=%d sgsr=%d",
                      resolve, bilinearHistory, bilinearCurrent, both, opaque, opaqueHalf, sgsr);
        Check(resolve > 0 && resolve <= 280 && bilinearHistory > 0 && bilinearHistory <= 215 && bilinearCurrent > 0 &&
                  bilinearCurrent < resolve && both > 0 && both < bilinearHistory && opaque > 0 && opaqueHalf > 0 &&
                  sgsr > 0,
              "RESOLVE_INSTRUCTIONS", detail);
    }

    {
        float xs[deko9::kTaauPhases], ys[deko9::kTaauPhases], sx = 0, sy = 0;
        bool range = true, distinct = true, periodic = true;
        for (uint32_t i = 0; i < deko9::kTaauPhases; ++i)
        {
            deko9::TaauHalton(i, &xs[i], &ys[i]);
            float px, py;
            deko9::TaauHalton(i + deko9::kTaauPhases, &px, &py);
            periodic = periodic && px == xs[i] && py == ys[i];
            range = range && xs[i] >= -0.5f && xs[i] < 0.5f && ys[i] >= -0.5f && ys[i] < 0.5f;
            for (uint32_t k = 0; k < i; ++k)
                distinct = distinct && (xs[k] != xs[i] || ys[k] != ys[i]);
            sx += xs[i];
            sy += ys[i];
        }
        char detail[160];
        std::snprintf(detail, sizeof(detail), "first=(%.4f,%.4f) mean=(%.4f,%.4f)", xs[0], ys[0], sx / 8, sy / 8);
        Check(range && distinct && periodic && std::fabs(xs[0]) < 1e-6f && std::fabs(ys[0] + 1.0f / 6.0f) < 1e-6f &&
                  std::fabs(sx / 8) < 0.07f && std::fabs(sy / 8) < 0.07f,
              "HALTON", detail);
    }

    // Jitter: a world point's pixel position moves by exactly (jx, jy) at
    // every depth, and the jitter reads back from the matrix.
    {
        const uint32_t w = 960, h = 540;
        const double eye[3] = {1234.0, 80.0, -2200.0};
        const TaauMat view = View(eye, 0.3, -0.1);
        double worst = 0.0, worstBack = 0.0;
        for (uint32_t phase = 0; phase < deko9::kTaauPhases; ++phase)
        {
            float jx, jy, m20, m21;
            deko9::TaauHalton(phase, &jx, &jy);
            deko9::TaauJitterClip(jx, jy, w, h, &m20, &m21);
            float bx, by;
            deko9::TaauJitterFromClip(m20, m21, w, h, &bx, &by);
            worstBack = std::fmax(worstBack, std::fmax(std::fabs(bx - jx), std::fabs(by - jy)));
            TaauMat proj = Projection(0.75f, 0.421875f, 4.0f), jittered = proj;
            jittered.m[2][0] += m20;
            jittered.m[2][1] += m21;
            const TaauMat a = deko9::TaauMul(view, proj), b = deko9::TaauMul(view, jittered);
            for (double dist : {5.0, 60.0, 900.0, 20000.0})
            {
                const double p[4] = {eye[0] + 0.3 * dist, eye[1] + 0.1 * dist, eye[2] + dist, 1.0};
                double ca[4], cb[4];
                Project(a, p, ca);
                Project(b, p, cb);
                const double ax = (ca[0] / ca[3] + 1.0) * 0.5 * w, ay = (1.0 - ca[1] / ca[3]) * 0.5 * h;
                const double bx2 = (cb[0] / cb[3] + 1.0) * 0.5 * w, by2 = (1.0 - cb[1] / cb[3]) * 0.5 * h;
                worst = std::fmax(worst, std::fmax(std::fabs(bx2 - ax - jx), std::fabs(by2 - ay - jy)));
            }
        }
        char detail[128];
        std::snprintf(detail, sizeof(detail), "worst_px=%.3g readback=%.3g", worst, worstBack);
        Check(worst < 1e-4 && worstBack < 1e-5, "JITTER_SHIFT", detail);
    }

    // Reprojection in float (the shader's path): this frame's NDC + depth ->
    // previous NDC, against projecting the world point with the previous
    // camera directly.
    {
        const TaauMat proj = Projection(0.75f, 0.421875f, 4.0f);
        const double eyeA[3] = {3100.0, 420.0, -1800.0}, eyeB[3] = {3104.5, 421.0, -1797.0};
        const TaauMat cur = deko9::TaauMul(View(eyeB, 0.52, -0.07), proj);
        const TaauMat prev = deko9::TaauMul(View(eyeA, 0.50, -0.06), proj);
        TaauMat m;
        const bool inverted = deko9::TaauReprojection(cur, prev, &m);
        float mf[4][4];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                mf[i][j] = (float)m.m[i][j];
        double worst = 0.0, worstSky = 0.0;
        for (double dist : {6.0, 50.0, 700.0, 9000.0, 1e7})
        {
            for (double side : {-0.4, 0.0, 0.35})
            {
                const double fy = std::sin(0.52), fz = std::cos(0.52);
                const double p[4] = {eyeB[0] + dist * (fy + side * fz), eyeB[1] + dist * 0.05,
                                     eyeB[2] + dist * (fz - side * fy), 1.0};
                double c[4], want[4];
                Project(cur, p, c);
                Project(prev, p, want);
                // The window depth the GPU stores is float: the NDC z of a
                // point at 1e7 equals m22 in float (the sky case).
                const float ndc[4] = {(float)(c[0] / c[3]), (float)(c[1] / c[3]), (float)(c[2] / c[3]), 1.0f};
                float got[4];
                for (int col = 0; col < 4; ++col)
                    got[col] = ndc[0] * mf[0][col] + ndc[1] * mf[1][col] + ndc[2] * mf[2][col] + ndc[3] * mf[3][col];
                // Error in previous-frame pixels at 1280x720.
                const double ex = std::fabs(got[0] / got[3] - want[0] / want[3]) * 640.0;
                const double ey = std::fabs(got[1] / got[3] - want[1] / want[3]) * 360.0;
                (dist > 1e6 ? worstSky : worst) = std::fmax(dist > 1e6 ? worstSky : worst, std::fmax(ex, ey));
                if (!(got[3] > 0.0f))
                    worst = 1e9;
            }
        }
        char detail[128];
        std::snprintf(detail, sizeof(detail), "worst_px=%.3g sky_px=%.3g", worst, worstSky);
        Check(inverted && worst < 0.05 && worstSky < 0.05, "REPROJECT", detail);

        TaauMat same;
        deko9::TaauReprojection(cur, cur, &same);
        double off = 0.0;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                off = std::fmax(off, std::fabs(same.m[i][j] - (i == j ? 1.0 : 0.0)));
        std::snprintf(detail, sizeof(detail), "max_offdiag=%.3g", off);
        Check(off < 1e-9, "REPROJECT_IDENTITY", detail);
    }

    {
        Check(sizeof(deko9::TaauConstants) == 16 * 16, "CONSTANTS_SIZE");
        deko9::TaauFrame f{};
        f.jitter[0] = 0.25f;
        f.jitter[1] = -0.125f;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                f.reproj[i][j] = (float)(i * 4 + j);
        f.viewmodelSplit = 0.015625f;
        f.sceneMinZ = 0.015625f;
        f.sceneMaxZ = 1.0f;
        f.farNdcZ = 0.99951172f;
        f.blend = 0.1f;
        f.flat = 2.0f / 255.0f;
        f.reset = true;
        const int32_t src[4] = {0, 0, 960, 540}, dst[4] = {0, 0, 1280, 720};
        deko9::TaauConstants c;
        deko9::TaauSetup(&c, src, dst, 1024, 576, 1280, 720, f);
        // Row k of the folded reprojection: (u.x, u.y, z, 1) -> (x + w) / 2,
        // (w - y) / 2, w of the previous clip position.
        const float n2[4] = {f.reproj[2][0], f.reproj[2][1], f.reproj[2][2], f.reproj[2][3]};
        const float n3[4] = {f.reproj[3][0] + f.reproj[1][0] - f.reproj[0][0], f.reproj[3][1] + f.reproj[1][1] - f.reproj[0][1],
                             0.0f, f.reproj[3][3] + f.reproj[1][3] - f.reproj[0][3]};
        const float zs = 64.0f / 63.0f;
        const bool ok = std::fabs(c.reproj[2][0] - 0.5f * (n2[0] + n2[3]) * zs) < 1e-5f &&
                        std::fabs(c.reproj[2][1] - 0.5f * (n2[3] - n2[1]) * zs) < 1e-5f &&
                        std::fabs(c.reproj[2][2] - n2[3] * zs) < 1e-5f && c.reproj[0][2] == 2.0f * f.reproj[0][3] &&
                        c.reproj[1][2] == -2.0f * f.reproj[1][3] &&
                        std::fabs(c.reproj[3][0] - (0.5f * (n3[0] + n3[3]) - 0.5f * (n2[0] + n2[3]) * zs / 64.0f)) < 1e-5f &&
                        std::fabs(c.reproj[3][1] - (0.5f * (n3[3] - n3[1]) - 0.5f * (n2[3] - n2[1]) * zs / 64.0f)) < 1e-5f &&
                        c.pos[0] == 0.75f && c.pos[2] == -0.25f &&
                        c.uv[0] == 1.0f / 1280 && c.gather[0] == 1.0f / 1024 && c.gather[3] == 1.0f / 576 &&
                        c.texel[1] == 1.0f / 576 && c.texel[2] == 0.5f / 1024 && c.dst[0] == 1280.0f && c.dst[2] == -0.5f &&
                        c.hist[1] == 1.0f / 720 && c.hist[3] == -1.0f / 720 && c.hist2[0] == 2.0f / 1280 &&
                        std::fabs(c.scale[0] - 4.0f / 3.0f / 1.2f) < 1e-6f && c.scale[2] == f.flat &&
                        c.blend[1] == -1.0f && std::fabs(c.depth[3] - (f.sceneMinZ + f.farNdcZ * 63.0f / 64.0f)) < 1e-6f;
        Check(ok, "SETUP");

        // The folded rows reproduce the reference path (depth to z, ndc,
        // clip, divide, uv) for any matrix.
        uint32_t seed = 12345;
        const auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f * 2.0f - 1.0f; };
        double worst = 0.0;
        for (int trial = 0; trial < 200; ++trial)
        {
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    f.reproj[i][j] = (i == j ? 1.0f : 0.0f) + 0.3f * rnd();
            deko9::TaauSetup(&c, src, dst, 1024, 576, 1280, 720, f);
            const float u[2] = {0.5f + 0.5f * rnd(), 0.5f + 0.5f * rnd()}, depth = 0.5f + 0.5f * rnd();
            const float z = (depth - f.sceneMinZ) / (f.sceneMaxZ - f.sceneMinZ);
            const float p[4] = {2.0f * u[0] - 1.0f, 1.0f - 2.0f * u[1], z, 1.0f};
            float clip[4];
            for (int col = 0; col < 4; ++col)
                clip[col] = p[0] * f.reproj[0][col] + p[1] * f.reproj[1][col] + p[2] * f.reproj[2][col] + p[3] * f.reproj[3][col];
            if (std::fabs(clip[3]) < 0.2f)
                continue;
            const float want[2] = {clip[0] / clip[3] * 0.5f + 0.5f, 0.5f - clip[1] / clip[3] * 0.5f};
            float prev[3];
            for (int k = 0; k < 3; ++k)
                prev[k] = u[0] * c.reproj[0][k] + u[1] * c.reproj[1][k] + depth * c.reproj[2][k] + c.reproj[3][k];
            worst = std::fmax(worst, std::fabs(prev[0] / prev[2] - want[0]));
            worst = std::fmax(worst, std::fabs(prev[1] / prev[2] - want[1]));
            worst = std::fmax(worst, std::fabs(prev[2] - clip[3]));
        }
        char detail[64];
        std::snprintf(detail, sizeof(detail), "worst=%.3g", worst);
        Check(worst < 1e-5, "SETUP_REPROJ_FOLD", detail);
    }

    {
        const float o[3] = {0, 0, 0}, f[3] = {1, 0, 0}, walk[3] = {6, 0, 0}, tele[3] = {300, 0, 0};
        const float turn[3] = {0.8660254f, 0.5f, 0}; // 30 degrees
        const float cos45 = 0.70710678f;
        Check(!deko9::TaauCameraCut(walk, f, o, f, 64.0f, cos45) && deko9::TaauCameraCut(tele, f, o, f, 64.0f, cos45) &&
                  !deko9::TaauCameraCut(o, turn, o, f, 64.0f, cos45) &&
                  deko9::TaauCameraCut(o, f, o, turn, 64.0f, 0.9f),
              "CAMERA_CUT");
    }

    {
        const float a = deko9::TaauLodBias(0.75f, 0.0f), b = deko9::TaauLodBias(1.0f, 0.0f),
                    c = deko9::TaauLodBias(0.5f, -0.5f), d = deko9::TaauLodBias(0.1f, -1.0f),
                    e = deko9::TaauLodBias(1.0f, 0.5f);
        char detail[96];
        std::snprintf(detail, sizeof(detail), "0.75=%.4f 1=%.4f 0.5-0.5=%.4f clamp=%.4f pos=%.4f", a, b, c, d, e);
        Check(a == -0.375f && b == 0.0f && c == -1.5f && d == -15.0f / 8.0f && e == 0.0f, "LOD_BIAS", detail);
    }

    // Zoom: an ADS step (5% per frame) reprojects, a scope snapping in or
    // out (65 -> 20 degrees) resets.
    {
        const float wide = 1.0f / std::tan(0.5f * 65.0f * 3.14159265f / 180.0f);
        const float narrow = 1.0f / std::tan(0.5f * 20.0f * 3.14159265f / 180.0f);
        Check(!deko9::TaauZoomCut(wide * 1.05f, wide, 1.2f) && !deko9::TaauZoomCut(wide, wide * 1.05f, 1.2f) &&
                  deko9::TaauZoomCut(narrow, wide, 1.2f) && deko9::TaauZoomCut(wide, narrow, 1.2f) &&
                  !deko9::TaauZoomCut(narrow, 0.0f, 1.2f),
              "ZOOM_CUT");
    }

    // Motion: a point on a model that moved (and turned) while the camera
    // panned. The setup's columns, applied as the programs do, give the
    // previous minus current UV of the point.
    {
        Check(sizeof(deko9::TaauMotionConstants) == 9 * 16, "MOTION_CONSTANTS_SIZE");
        const TaauMat proj = Projection(0.75f, 0.421875f, 4.0f);
        const double eyeA[3] = {3100.0, 420.0, -1800.0}, eyeB[3] = {3104.5, 421.0, -1797.0};
        const TaauMat cur = deko9::TaauMul(View(eyeB, 0.52, -0.07), proj);
        const TaauMat prev = deko9::TaauMul(View(eyeA, 0.50, -0.06), proj);
        // Object -> world: yaw 0.2 at (3300, 400, -1500) now, 0.17 at
        // (3292, 400, -1503) last frame.
        const auto place = [](double yaw, double x, double y, double z) {
            TaauMat m{};
            m.m[0][0] = std::cos(yaw);
            m.m[0][2] = -std::sin(yaw);
            m.m[1][1] = 1.0;
            m.m[2][0] = std::sin(yaw);
            m.m[2][2] = std::cos(yaw);
            m.m[3][0] = x;
            m.m[3][1] = y;
            m.m[3][2] = z;
            m.m[3][3] = 1.0;
            return m;
        };
        const TaauMat objNow = place(0.2, 3300.0, 400.0, -1500.0), objPrev = place(0.17, 3292.0, 400.0, -1503.0);
        deko9::TaauMotionDraw d{};
        const TaauMat c = deko9::TaauMul(objNow, cur), q = deko9::TaauMul(objPrev, prev);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
            {
                d.cur[i][j] = (float)c.m[i][j];
                d.prev[i][j] = (float)q.m[i][j];
            }
        deko9::TaauMotionView view{};
        view.jitterClip[0] = 0.001f;
        view.jitterClip[1] = -0.002f;
        deko9::TaauMotionConstants k;
        deko9::TaauMotionSetup(&k, d, view);
        double worst = 0.0;
        const double points[3][3] = {{0, 0, 0}, {12, 60, -7}, {-20, 3, 15}};
        for (const double *o : points)
        {
            const float p[4] = {(float)o[0], (float)o[1], (float)o[2], 1.0f};
            float vc[4], vp[4];
            for (int col = 0; col < 4; ++col)
            {
                vc[col] = p[0] * k.curCol[col][0] + p[1] * k.curCol[col][1] + p[2] * k.curCol[col][2] +
                          p[3] * k.curCol[col][3];
                vp[col] = p[0] * k.prevCol[col][0] + p[1] * k.prevCol[col][1] + p[2] * k.prevCol[col][2] +
                          p[3] * k.prevCol[col][3];
            }
            const float mx = (vp[0] / vp[3] - vc[0] / vc[3]) * 0.5f, my = (vc[1] / vc[3] - vp[1] / vp[3]) * 0.5f;
            // Reference: the world point through both cameras, in double.
            double w[4], a[4], b[4];
            const double ph[4] = {o[0], o[1], o[2], 1.0};
            Project(objNow, ph, w);
            Project(cur, w, a);
            Project(objPrev, ph, w);
            Project(prev, w, b);
            const double ux = (b[0] / b[3] - a[0] / a[3]) * 0.5, uy = (a[1] / a[3] - b[1] / b[3]) * 0.5;
            worst = std::fmax(worst, std::fmax(std::fabs(mx - ux) * 1280.0, std::fabs(my - uy) * 720.0));
        }
        char detail[96];
        std::snprintf(detail, sizeof(detail), "worst_px=%.3g", worst);
        Check(worst < 0.02 && k.jitter[0] == 0.001f && k.jitter[1] == -0.002f, "MOTION_SETUP", detail);
        d.reject = true;
        deko9::TaauMotionSetup(&k, d, view);
        bool zero = true;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                zero = zero && k.prevCol[c][r] == 0.0f;
        Check(zero && k.curCol[3][3] == d.cur[3][3] && deko9::kTaauMotionReject > 8.0f &&
                  deko9::kTaauMotionNone < -8.0f,
              "MOTION_REJECT");
    }

    // Previous w at or near zero: the guarded test never hands the history
    // fetch a NaN or infinite uv; decisions and uvs are unchanged for points
    // well in front of the previous camera. Negative control: the old "> 0"
    // test accepts a denormal w with prev.xy = 0 (1/w = inf, 0 * inf = NaN).
    {
        const float bar = 9.5367431640625e-7f, u[2] = {0.25f, 0.75f};
        const bool inProgram = std::strstr(deko9::kTaauResolveGlsl, "if (good > 9.5367431640625e-7)") != nullptr;
        const float ws[] = {0.0f, -0.0f, 1e-39f, 1e-30f, 5e-7f, -2.0f, -1e-39f};
        const float xys[][2] = {{0.0f, 0.0f}, {0.0f, 1e-30f}, {1e-3f, 2e-3f}, {-4.0f, 3.0f}};
        int guardedBad = 0, guardedGood = 0, oldNan = 0, oldNanAccepted = 0;
        for (float w : ws)
            for (const auto &xy : xys)
                for (float gap : {-0.25f, 0.25f})
                {
                    const float prev[3] = {xy[0], xy[1], w};
                    const Reprojected g = ResolveReproject(prev, u, gap, 1.0f, bar);
                    const Reprojected o = ResolveReproject(prev, u, gap, 1.0f, 0.0f);
                    guardedGood += g.good;
                    guardedBad += g.good && !(std::isfinite(g.u[0]) && std::isfinite(g.u[1]));
                    const bool nan = std::isnan(o.u[0]) || std::isnan(o.u[1]);
                    oldNan += nan;
                    oldNanAccepted += nan && o.good;
                }
        // Normal inputs: random points in front of the previous camera, some
        // inside, some outside the history, some viewmodel, some reset.
        uint32_t seed = 7;
        const auto rnd = [&]() {
            seed = seed * 1664525u + 1013904223u;
            return (float)(seed >> 8) / 16777216.0f;
        };
        int normalDiff = 0, normalAccepted = 0;
        for (int i = 0; i < 200000; ++i)
        {
            const float w = 4.0f + rnd() * 20000.0f;
            const float prev[3] = {(rnd() * 1.6f - 0.3f) * w, (rnd() * 1.6f - 0.3f) * w, w};
            const float pu[2] = {rnd(), rnd()};
            const float gap = rnd() < 0.1f ? 0.01f : -0.5f * rnd();
            const float sign = rnd() < 0.05f ? -1.0f : 1.0f;
            const Reprojected g = ResolveReproject(prev, pu, gap, sign, bar);
            const Reprojected o = ResolveReproject(prev, pu, gap, sign, 0.0f);
            normalAccepted += g.good;
            normalDiff += g.good != o.good || std::memcmp(g.u, o.u, sizeof(g.u)) != 0;
        }
        char detail[200];
        std::snprintf(detail, sizeof(detail),
                      "in_program=%d guarded_accepted=%d guarded_nonfinite=%d old_nan_uv=%d old_nan_accepted=%d "
                      "normal_accepted=%d/200000 normal_diff=%d",
                      (int)inProgram, guardedGood, guardedBad, oldNan, oldNanAccepted, normalAccepted, normalDiff);
        Check(inProgram && guardedBad == 0 && guardedGood > 0 && oldNan > 0 && oldNanAccepted > 0 &&
                  normalAccepted > 50000 && normalDiff == 0,
              "REPROJECT_GUARD", detail);
    }

    // Reactive image pooled for the scene capacity: 100 forced dynamic-
    // resolution sizes keep one allocation (an image sized to each scene,
    // as before, reallocates on every change), and the resolve's reactive
    // tap reads the same texels with the same filter weights as from an
    // image of exactly the used size: the texel coordinate matches, and any
    // texel past the used edge is the guard the reactive passes wrote.
    {
        const uint32_t capW = 1280, capH = 720, outW = 1280, outH = 720;
        uint32_t seed = 11;
        const auto next = [&]() {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 8;
        };
        int oldAllocs[2] = {}, newAllocs[2] = {};
        double worstCoord = 0.0;
        long long taps = 0, indexMismatch = 0, ties = 0, unguarded = 0, mappedSizes = 0;
        for (int half = 0; half < 2; ++half)
        {
            deko9::TaauExtent oldImage{0, 0}, newImage{0, 0};
            const deko9::TaauExtent capacity = deko9::TaauReactiveExtent(capW, capH, half);
            uint32_t prevW = 0;
            for (int change = 0; change <= 100; ++change)
            {
                // Scales 0.5..1 in 1/64 steps, each one a change, odd sizes too.
                uint32_t w;
                do
                    w = capW * (32 + next() % 33) / 64 + (next() & 1);
                while (w == prevW || w > capW);
                prevW = w;
                const uint32_t h = std::min(capH, (uint32_t)((uint64_t)w * capH / capW) + (next() & 1));
                const deko9::TaauExtent used = deko9::TaauReactiveExtent(w, h, half);
                // The first frame allocates under either policy.
                if (oldImage != used)
                    oldAllocs[half] += change > 0, oldImage = used;
                if (newImage != capacity)
                    newAllocs[half] += change > 0, newImage = capacity;
                if (change % 10)
                    continue;
                ++mappedSizes;
                const int32_t src[4] = {0, 0, (int32_t)w, (int32_t)h}, dst[4] = {0, 0, (int32_t)outW, (int32_t)outH};
                uint32_t rect[4];
                deko9::TaauReactiveRect(src, half, used, newImage, rect);
                const uint32_t guardX = rect[0] + rect[2], guardY = rect[1] + rect[3];
                for (uint32_t phase = 0; phase < deko9::kTaauPhases; ++phase)
                {
                    deko9::TaauFrame f{};
                    deko9::TaauHalton(phase, &f.jitter[0], &f.jitter[1]);
                    f.sceneMaxZ = 1.0f;
                    deko9::TaauConstants a, b;
                    deko9::TaauSetup(&a, src, dst, w, h, outW, outH, f);
                    deko9::TaauReactiveUv(used, newImage, f.reactiveUv);
                    deko9::TaauSetup(&b, src, dst, w, h, outW, outH, f);
                    for (int axis = 0; axis < 2; ++axis)
                    {
                        const uint32_t usedN = axis ? used.height : used.width;
                        const uint32_t imageN = axis ? newImage.height : newImage.width;
                        const uint32_t guard = axis ? guardY : guardX;
                        for (uint32_t px = 0; px < (axis ? outH : outW); ++px)
                        {
                            // As the program: fr = frag * pos.xy + pos.zw; uv = fr * texel.xy + texel.zw.
                            const float frag = (float)px + 0.5f;
                            const float frA = frag * a.pos[axis] + a.pos[2 + axis];
                            const float frB = frag * b.pos[axis] + b.pos[2 + axis];
                            const double oldCoord = (double)(frA * a.texel[axis] + a.texel[2 + axis]) * usedN;
                            const double newCoord = (double)(frB * b.texel[axis] + b.texel[2 + axis]) * imageN;
                            worstCoord = std::fmax(worstCoord, std::fabs(oldCoord - newCoord));
                            // Texels read: point (floor) or bilinear (the pair around coord - 0.5).
                            const double base[2] = {half ? oldCoord - 0.5 : oldCoord, half ? newCoord - 0.5 : newCoord};
                            for (int t = 0; t < (half ? 2 : 1); ++t, ++taps)
                            {
                                const long long o = std::clamp((long long)std::floor(base[0]) + t, 0ll,
                                                               (long long)usedN - 1);
                                long long n = std::clamp((long long)std::floor(base[1]) + t, 0ll,
                                                         (long long)imageN - 1);
                                if (n >= (long long)usedN)
                                {
                                    // Past the used edge: only the guard (a copy of the last texel) is defined.
                                    unguarded += n != (long long)usedN || guard != usedN + 1;
                                    n = usedN - 1;
                                }
                                // The GPU quantises the coordinate to 1/256 texel, so a pick that
                                // differs only within the float rounding of a texel boundary is a tie.
                                const double frac = base[0] - std::floor(base[0]);
                                if (o != n && std::fmin(frac, 1.0 - frac) < 1e-3)
                                    ++ties;
                                else
                                    indexMismatch += o != n;
                            }
                        }
                    }
                }
            }
        }
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "changes=100x2 old_allocs=%d/%d pooled_allocs=%d/%d sizes=%lld taps=%lld worst_coord=%.2e texels "
                      "boundary_ties=%lld index_mismatch=%lld unguarded=%lld",
                      oldAllocs[0], oldAllocs[1], newAllocs[0], newAllocs[1], mappedSizes, taps, worstCoord, ties,
                      indexMismatch, unguarded);
        Check(oldAllocs[0] == 100 && oldAllocs[1] == 100 && newAllocs[0] == 0 && newAllocs[1] == 0 &&
                  mappedSizes == 22 && worstCoord < 1e-3 && indexMismatch == 0 && unguarded == 0,
              "REACTIVE_POOL", detail);
    }

    {
        // Kernel holes: the resolve's (1 - d^2)^2 weights of the 2x2 around
        // every output pixel, for every render width the dynamic-resolution
        // controller picks (16 px levels at 16:9) and every jitter phase.
        // With a fixed 1.2 output px unit, scales below ~0.59 leave output
        // pixels whose four samples all weigh 0: the kernel-weighted
        // current colour is 0 / epsilon = black (flat areas and reset frames
        // show it), a lattice that moves with the jitter.
        long oldHoles = 0, newHoles = 0, oldHoles050 = 0;
        float newMin = 1.0f;
        for (int rw = 320; rw <= 1280; rw += 16)
        {
            const int rh = std::max(8, (int)std::lround(rw * 720.0 / 1280.0 / 8.0) * 8);
            for (uint32_t phase = 0; phase < deko9::kTaauPhases; ++phase)
            {
                deko9::TaauFrame f{};
                deko9::TaauHalton(phase, &f.jitter[0], &f.jitter[1]);
                f.sceneMaxZ = 1.0f;
                const int32_t src[4] = {0, 0, rw, rh}, dst[4] = {0, 0, 1280, 720};
                deko9::TaauConstants c;
                deko9::TaauSetup(&c, src, dst, (uint32_t)rw, (uint32_t)rh, 1280, 720, f);
                const float oldScale[2] = {1280.0f / rw / 1.2f, 720.0f / rh / 1.2f};
                for (int y = 0; y < 96; ++y)
                    for (int x = 0; x < 160; ++x)
                    {
                        const float fx = (x + 0.5f) * c.pos[0] + c.pos[2], fy = (y + 0.5f) * c.pos[1] + c.pos[3];
                        const float ox = fx - std::floor(fx), oy = fy - std::floor(fy);
                        float wmax[2] = {0.0f, 0.0f};
                        for (int v = 0; v < 2; ++v)
                        {
                            const float kx = v ? c.scale[0] : oldScale[0], ky = v ? c.scale[1] : oldScale[1];
                            for (int t = 0; t < 4; ++t)
                            {
                                const float dx = (ox - (t & 1)) * kx, dy = (oy - (t >> 1)) * ky;
                                float wt = std::fmin(std::fmax(1.0f - (dx * dx + dy * dy), 0.0f), 1.0f);
                                wmax[v] = std::fmax(wmax[v], wt * wt);
                            }
                        }
                        oldHoles += wmax[0] == 0.0f;
                        oldHoles050 += rw == 640 && wmax[0] == 0.0f;
                        newHoles += wmax[1] == 0.0f;
                        newMin = std::fmin(newMin, wmax[1]);
                    }
            }
        }
        // At and above scale 0.625 the kernel is unchanged.
        const bool same = deko9::TaauKernelScale(1280.0f / 960.0f) == 1280.0f / 960.0f / 1.2f &&
                          deko9::TaauKernelScale(1.0f) == 1.0f / 1.2f && deko9::TaauKernelScale(1.6f) == 1.6f / 1.2f;
        char detail[200];
        std::snprintf(detail, sizeof(detail),
                      "widths=61 phases=8 old_holes=%ld old_holes_at_0.5=%ld new_holes=%ld new_min_weight=%.4f "
                      "unchanged_above_0.625=%d",
                      oldHoles, oldHoles050, newHoles, newMin, (int)same);
        Check(oldHoles > 0 && oldHoles050 > 0 && newHoles == 0 && newMin > 0.01f && same, "KERNEL_HOLES", detail);
    }

    {
        // History youth through the 2-bit history alpha: from 1 (no usable
        // history) the stored level falls 1, 2/3, 1/3, 0 and the current
        // frame's weight floor goes 0.6, 0.4, 0.2, 0 -- for any filtered
        // (fractional) value read back too. Negative control: a step below
        // half a level (0.15) rounds back up and never decays.
        const auto store = [](float v) { return std::round(std::fmin(std::fmax(v, 0.0f), 1.0f) * 3.0f) / 3.0f; };
        const auto framesToZero = [&](float start, float step) {
            float y = store(start);
            for (int n = 0; n < 16; ++n)
            {
                if (y == 0.0f)
                    return n;
                y = store(std::fmax(0.0f, y - step));
            }
            return 99;
        };
        float y = 1.0f, floors[4];
        for (float &w : floors)
        {
            w = y * deko9::kTaauYouthWeight;
            y = store(std::fmax(0.0f, y - deko9::kTaauYouthStep));
        }
        int worst = 0;
        for (int i = 0; i <= 256; ++i)
            worst = std::max(worst, framesToZero(i / 256.0f, deko9::kTaauYouthStep));
        const int control = framesToZero(1.0f, 0.15f);
        char detail[200];
        std::snprintf(detail, sizeof(detail), "floors=%.3f,%.3f,%.3f,%.3f worst_frames=%d control_step0.15=%d",
                      floors[0], floors[1], floors[2], floors[3], worst, control);
        Check(std::fabs(floors[0] - 0.6f) < 1e-6f && std::fabs(floors[1] - 0.4f) < 1e-6f &&
                  std::fabs(floors[2] - 0.2f) < 1e-6f && floors[3] == 0.0f && worst == 3 && control == 99,
              "YOUTH_DECAY", detail);

        // The reset frame's share of the blended history after n frames
        // (product of 1 - alpha; luma weighting and clamping left out), per
        // output pixel: alpha = blend * wmax * (1 - antiFlicker) at agreeing
        // luma, floored by the youth. Mean over pixels of the share at frame
        // 8 and of the frames until it is below 5%, at scales 0.5..1.
        char line[400];
        int len = 0;
        bool better = true;
        for (const int rw : {640, 768, 960, 1280})
        {
            const int rh = rw * 9 / 16;
            double share8[2] = {0, 0}, frames95[2] = {0, 0};
            int n = 0;
            deko9::TaauConstants cs[deko9::kTaauPhases];
            for (uint32_t phase = 0; phase < deko9::kTaauPhases; ++phase)
            {
                deko9::TaauFrame f{};
                deko9::TaauHalton(phase, &f.jitter[0], &f.jitter[1]);
                f.sceneMaxZ = 1.0f;
                const int32_t src[4] = {0, 0, rw, rh}, dst[4] = {0, 0, 1280, 720};
                deko9::TaauSetup(&cs[phase], src, dst, (uint32_t)rw, (uint32_t)rh, 1280, 720, f);
            }
            for (int py = 0; py < 48; ++py)
                for (int px = 0; px < 64; ++px, ++n)
                    for (int v = 0; v < 2; ++v)
                    {
                        double share = 1.0;
                        float youth = 1.0f;
                        int reached = 0;
                        for (int k = 1; k <= 400 && !reached; ++k)
                        {
                            const deko9::TaauConstants &c = cs[k % deko9::kTaauPhases];
                            // The old kernel unit (1.2 output px) for the old path.
                            const float kx = v ? c.scale[0] : 1280.0f / rw / 1.2f;
                            const float ky = v ? c.scale[1] : 720.0f / rh / 1.2f;
                            const float fx = (px + 0.5f) * c.pos[0] + c.pos[2], fy = (py + 0.5f) * c.pos[1] + c.pos[3];
                            const float ox = fx - std::floor(fx), oy = fy - std::floor(fy);
                            float wmax = 0.0f;
                            for (int t = 0; t < 4; ++t)
                            {
                                const float dx = (ox - (t & 1)) * kx, dy = (oy - (t >> 1)) * ky;
                                const float wt = std::fmin(std::fmax(1.0f - (dx * dx + dy * dy), 0.0f), 1.0f);
                                wmax = std::fmax(wmax, wt * wt);
                            }
                            float alpha = 0.1f * wmax * (1.0f - 0.5f);
                            if (v)
                            {
                                alpha = std::fmax(alpha, youth * deko9::kTaauYouthWeight);
                                youth = store(std::fmax(0.0f, youth - deko9::kTaauYouthStep));
                            }
                            share *= 1.0 - alpha;
                            if (k == 8)
                                share8[v] += share;
                            if (share < 0.05)
                                reached = k;
                        }
                        frames95[v] += reached ? reached : 400;
                    }
            for (int v = 0; v < 2; ++v)
            {
                share8[v] /= n;
                frames95[v] /= n;
            }
            better = better && share8[1] < 0.5 * share8[0] && frames95[1] < frames95[0];
            len += std::snprintf(line + len, sizeof(line) - len, "%sscale=%.2f share8 old=%.3f new=%.3f frames_to_5%% "
                                 "old=%.0f new=%.0f", len ? " " : "", rw / 1280.0, share8[0], share8[1], frames95[0],
                                 frames95[1]);
        }
        Check(better, "RESET_SHARE", line);
    }

    {
        // RGB10 history quantisation: a blend step below half a 10-bit level
        // rounds back, so the history stalls up to 0.5 / 1023 / t from the
        // mean of the current frames (static: no output flicker). Stochastic
        // rounding (dither) removes the bias but makes converged 8-bit output
        // flip from frame to frame, which is why the history store rounds.
        double bias[2] = {0, 0}, flips[2] = {0, 0};
        const float t = 0.02f;
        for (int dither = 0; dither < 2; ++dither)
        {
            long changes = 0, frames = 0;
            uint32_t s = 12345u;
            for (int px = 0; px < 400; ++px)
            {
                const double m = 0.3 + px * 0.000617, e0 = (px & 1 ? 1.0 : -1.0) * 6.0 / 255.0;
                double h = std::round((m + e0) * 1023.0) / 1023.0, sum = 0.0;
                int prevOut = -1;
                for (int k = 0; k < 1200; ++k)
                {
                    const double c = std::round((m + 0.5 / 255.0 * std::sin(k * 0.785398 + px)) * 255.0) / 255.0;
                    double r = 0.5;
                    if (dither)
                    {
                        s = s * 1664525u + 1013904223u;
                        r = (s >> 8) / 16777216.0;
                    }
                    h = std::floor((h + t * (c - h)) * 1023.0 + r) / 1023.0;
                    if (k < 600)
                        continue;
                    sum += h;
                    const int out = (int)std::lround(h * 255.0);
                    changes += prevOut >= 0 && out != prevOut;
                    prevOut = out;
                    ++frames;
                }
                bias[dither] = std::fmax(bias[dither], std::fabs(sum / 600.0 - m) * 255.0);
            }
            flips[dither] = (double)changes / frames;
        }
        char detail[200];
        std::snprintf(detail, sizeof(detail),
                      "t=%.2f round: worst_bias=%.2f flip_rate=%.4f dither: worst_bias=%.2f flip_rate=%.4f "
                      "(8-bit units)",
                      t, bias[0], flips[0], bias[1], flips[1]);
        Check(bias[0] <= 0.5 / 1023.0 / t * 255.0 + 0.5 && flips[0] == 0.0 && bias[1] < bias[0] && flips[1] > 0.0,
              "HISTORY_QUANTISATION", detail);
    }

    std::printf("%s:DEKO9_TAAU\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
