// Host proof for r_model_skin_simd.cpp: the portable-SIMD skinning the Switch
// build uses (GCC vector extensions -> NEON on AArch64) must produce the same
// vertices as the x86 SSE implementation it replaces. ./test compiles the
// real r_model_skin_sse.cpp as the oracle (only its MSVC spellings mapped to
// GCC, see model_skin_simd_check in ./test) and skins random surfaces with
// both: rigid vertex lists and 0..3 blend weights, with and without the
// fast-skin normal streams. Positions must match bit for bit (x86 has no FMA
// contraction here); packed normal/tangent bytes and the copied color/UV
// bytes must match exactly.

#include <universal/q_shared.h>
#include <gfx_d3d/r_model_skin.h>
#include <gfx_d3d/r_model_skin_simd.h>
#include <xanim/dobj.h>
#include <xanim/xanim.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#ifdef KISAK_SKIN_NEON_TEST
// AArch64 run (./test model_skin_neon_check, under qemu-aarch64): the oracle
// is the portable path of r_model_skin_simd.cpp compiled by the Switch
// toolchain (devkitA64 GCC, same flags as the game) with
// KISAK_SKIN_SIMD_PORTABLE -- i.e. the code the game shipped before the
// hand-written NEON path, including GCC's fmla contractions -- and the
// candidate is the NEON path from the same file. Output must be identical.
void R_SkinXSurfaceSkinnedPortable(const XSurface *xsurf, const DObjSkelMat *boneMatrix,
                                   GfxPackedVertexNormal *skinVertNormalIn, GfxPackedVertexNormal *skinVertNormalOut,
                                   GfxPackedVertex *skinVerticesOut);
void R_SkinSimdDecodeNeon(uint32_t packed, float out[4]);
void R_SkinSimdDecodePortable(uint32_t packed, float out[4]);
#define R_SkinXSurfaceSkinnedSse R_SkinXSurfaceSkinnedPortable
#define KISAK_SKIN_TEST_NAME "MODEL_SKIN_NEON"
#else
void __cdecl R_SkinXSurfaceSkinnedSse(const XSurface *xsurf, const DObjSkelMat *boneMatrix,
                                      GfxPackedVertexNormal *skinVertNormalIn,
                                      GfxPackedVertexNormal *skinVertNormalOut, GfxPackedVertex *skinVerticesOut);
#define KISAK_SKIN_TEST_NAME "MODEL_SKIN_SIMD"
#endif

namespace
{
int g_failures;

void Check(bool ok, const char *what)
{
    if (!ok)
    {
        ++g_failures;
        std::printf("FAIL:" KISAK_SKIN_TEST_NAME " %s\n", what);
    }
}

template <typename T> T *AlignedArray(size_t count)
{
    void *p = std::aligned_alloc(16, ((count * sizeof(T) + 15) / 16) * 16 + 16);
    std::memset(p, 0, ((count * sizeof(T) + 15) / 16) * 16 + 16);
    return static_cast<T *>(p);
}

struct Case
{
    XSurface surf{};
    std::vector<uint16_t> blend;
    std::vector<XRigidVertList> lists;
    GfxPackedVertex *verts = nullptr;
    GfxPackedVertexNormal *normalsIn = nullptr;
    int vertCount = 0;
};

void RandomVertex(std::mt19937 &rng, GfxPackedVertex *v)
{
    std::uniform_real_distribution<float> pos(-64.0f, 64.0f);
    std::uniform_int_distribution<uint32_t> any;
    v->xyz[0] = pos(rng);
    v->xyz[1] = pos(rng);
    v->xyz[2] = pos(rng);
    v->binormalSign = (any(rng) & 1) ? 1.0f : -1.0f;
    const uint32_t color = any(rng), uv = any(rng), normal = any(rng), tangent = any(rng);
    std::memcpy(&v->color, &color, 4);
    std::memcpy(&v->texCoord, &uv, 4);
    std::memcpy(&v->normal, &normal, 4);
    std::memcpy(&v->tangent, &tangent, 4);
}

void RandomBones(std::mt19937 &rng, DObjSkelMat *bones, int count)
{
    std::uniform_real_distribution<float> axis(-1.2f, 1.2f), origin(-200.0f, 200.0f);
    for (int b = 0; b < count; ++b)
    {
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                bones[b].axis[r][c] = axis(rng);
            bones[b].axis[r][3] = 0.0f;
        }
        for (int c = 0; c < 3; ++c)
            bones[b].origin[c] = origin(rng);
        bones[b].origin[3] = 1.0f;
    }
}

void MakeWeighted(std::mt19937 &rng, Case *c, int boneCount)
{
    std::uniform_int_distribution<int> count(0, 40), bone(0, boneCount - 1);
    std::uniform_int_distribution<uint32_t> weight(0, 65535);
    int total = 0;
    for (int w = 0; w < 4; ++w)
    {
        c->surf.vertInfo.vertCount[w] = (int16_t)count(rng);
        total += c->surf.vertInfo.vertCount[w];
        for (int v = 0; v < c->surf.vertInfo.vertCount[w]; ++v)
        {
            c->blend.push_back((uint16_t)(bone(rng) << 6));
            for (int k = 0; k < w; ++k)
            {
                c->blend.push_back((uint16_t)(bone(rng) << 6));
                c->blend.push_back((uint16_t)weight(rng));
            }
        }
    }
    c->vertCount = total;
    c->surf.deformed = true;
    c->surf.vertCount = (uint16_t)total;
    c->surf.vertInfo.vertsBlend = c->blend.data();
}

void MakeRigid(std::mt19937 &rng, Case *c, int boneCount)
{
    std::uniform_int_distribution<int> lists(1, 5), count(0, 30), bone(0, boneCount - 1);
    const int n = lists(rng);
    int total = 0;
    for (int i = 0; i < n; ++i)
    {
        XRigidVertList l{};
        l.boneOffset = (uint16_t)(bone(rng) << 6);
        l.vertCount = (uint16_t)count(rng);
        total += l.vertCount;
        c->lists.push_back(l);
    }
    c->vertCount = total;
    c->surf.deformed = false;
    c->surf.vertCount = (uint16_t)total;
    c->surf.vertListCount = (uint32_t)c->lists.size();
    c->surf.vertList = c->lists.data();
}

// The NEON decode replaces (b - shift) / scale with q0 = x * (1/scale),
// e = fma(-q0, scale, x), q = fma(e, 1/scale, q0). Prove it is the correctly
// rounded quotient for every input it can see: bytes 0..255 in the xyz
// lanes (shift 127, scale 127) and the w lane (shift -192, scale 255).
void DecodeDivisionIsExact()
{
    const float shifts[2] = {127.0f, -192.0f};
    const float scales[2] = {127.0f, 255.0f};
    int bad = 0;
    for (int k = 0; k < 2; ++k)
    {
        volatile float scale = scales[k];
        const float recip = 1.0f / scale;
        for (int b = 0; b < 256; ++b)
        {
            const float x = (float)b - shifts[k];
            const float q = x / scale;
            const float q0 = x * recip;
            const float r = std::fma(std::fma(-q0, scale, x), recip, q0);
            if (std::memcmp(&q, &r, sizeof(q)))
                ++bad;
        }
    }
    Check(bad == 0, "reciprocal-refined decode division differs from (b - shift) / scale");
}

#ifdef KISAK_SKIN_NEON_TEST
// Every (component byte, w byte) pair through both decoders, bit for bit.
void DecodeMatchesPortable()
{
    int bad = 0;
    for (uint32_t w = 0; w < 256; ++w)
    {
        for (uint32_t x = 0; x < 256; ++x)
        {
            const uint32_t packed = x | ((x ^ 0x5Au) << 8) | ((255u - x) << 16) | (w << 24);
            float a[4], b[4];
            R_SkinSimdDecodeNeon(packed, a);
            R_SkinSimdDecodePortable(packed, b);
            if (std::memcmp(a, b, sizeof(a)) && ++bad <= 4)
                std::printf("  decode 0x%08x neon=%.9g,%.9g,%.9g,%.9g portable=%.9g,%.9g,%.9g,%.9g\n", packed, a[0],
                            a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        }
    }
    Check(bad == 0, "NEON unit-vector decode differs from the portable decode");
}
#endif

bool SameVerts(const GfxPackedVertex *a, const GfxPackedVertex *b, int count)
{
    return !std::memcmp(a, b, (size_t)count * sizeof(GfxPackedVertex));
}

void RunCase(std::mt19937 &rng, bool weighted, int mode, const char *name)
{
    constexpr int kBones = 24;
    DObjSkelMat *bones = AlignedArray<DObjSkelMat>(kBones);
    RandomBones(rng, bones, kBones);
    Case c;
    if (weighted)
        MakeWeighted(rng, &c, kBones);
    else
        MakeRigid(rng, &c, kBones);
    c.verts = AlignedArray<GfxPackedVertex>(c.vertCount + 4);
    for (int i = 0; i < c.vertCount + 4; ++i)
        RandomVertex(rng, &c.verts[i]);
    c.surf.verts0 = c.verts;
    GfxPackedVertexNormal *normalsIn = AlignedArray<GfxPackedVertexNormal>(c.vertCount + 1);
    std::uniform_int_distribution<uint32_t> any;
    for (int i = 0; i < c.vertCount; ++i)
    {
        const uint32_t words[2] = {any(rng), any(rng)};
        std::memcpy(&normalsIn[i], words, sizeof(words));
    }

    GfxPackedVertex *sseOut = AlignedArray<GfxPackedVertex>(c.vertCount + 1);
    GfxPackedVertex *simdOut = AlignedArray<GfxPackedVertex>(c.vertCount + 1);
    GfxPackedVertexNormal *sseNormals = AlignedArray<GfxPackedVertexNormal>(c.vertCount + 1);
    GfxPackedVertexNormal *simdNormals = AlignedArray<GfxPackedVertexNormal>(c.vertCount + 1);
    // mode 0: verts only; 1: verts + normal stream out; 2: normals in and out.
    GfxPackedVertexNormal *in = mode == 2 ? normalsIn : nullptr;
    R_SkinXSurfaceSkinnedSse(&c.surf, bones, in, mode ? sseNormals : nullptr, sseOut);
    R_SkinXSurfaceSkinnedSimd(&c.surf, bones, in, mode ? simdNormals : nullptr, simdOut);

    char what[160];
    std::snprintf(what, sizeof(what), "%s mode=%d verts=%d", name, mode, c.vertCount);
    bool ok = SameVerts(sseOut, simdOut, c.vertCount);
    if (!ok)
    {
        for (int i = 0; i < c.vertCount; ++i)
        {
            if (std::memcmp(&sseOut[i], &simdOut[i], sizeof(GfxPackedVertex)))
            {
                const uint8_t *a = (const uint8_t *)&sseOut[i], *b = (const uint8_t *)&simdOut[i];
                std::printf("  vert %d sse  pos=%.9g,%.9g,%.9g,%.9g bytes=", i, sseOut[i].xyz[0], sseOut[i].xyz[1],
                            sseOut[i].xyz[2], sseOut[i].binormalSign);
                for (int k = 16; k < 32; ++k)
                    std::printf("%02x", a[k]);
                std::printf("\n  vert %d simd pos=%.9g,%.9g,%.9g,%.9g bytes=", i, simdOut[i].xyz[0],
                            simdOut[i].xyz[1], simdOut[i].xyz[2], simdOut[i].binormalSign);
                for (int k = 16; k < 32; ++k)
                    std::printf("%02x", b[k]);
                std::printf("\n");
                break;
            }
        }
    }
    Check(ok, what);
    if (mode)
        Check(!std::memcmp(sseNormals, simdNormals, (size_t)c.vertCount * sizeof(GfxPackedVertexNormal)), what);
    std::free(bones);
    std::free(c.verts);
    std::free(normalsIn);
    std::free(sseOut);
    std::free(simdOut);
    std::free(sseNormals);
    std::free(simdNormals);
}
} // namespace

// Captures contain surface inputs from the production decoder, never native
// pointers. Check real character weights and packed directions against the
// same independent implementation used by the randomized proof.
static bool CheckCapturedSurface(const char *path)
{
    FILE *file = std::fopen(path, "rb");
    if (!file)
        return false;
    uint32_t h[10]{};
    auto read = [&](void *p, size_t size) { return std::fread(p, 1, size, file) == size; };
    if (!read(h, sizeof(h)) || h[0] != 0x534b494e || !h[1] || h[1] > 128 ||
        h[2] > 65535 || h[3] > 1 || h[4] > 65535 || h[5] > 7 * 65535u)
    {
        std::fclose(file);
        return false;
    }
    Case c;
    c.vertCount = h[2];
    c.surf.vertCount = h[2];
    c.surf.deformed = h[3];
    c.surf.vertListCount = h[4];
    c.blend.resize(h[5]);
    c.lists.resize(h[4]);
    DObjSkelMat *bones = AlignedArray<DObjSkelMat>(h[1]);
    c.verts = AlignedArray<GfxPackedVertex>(h[2] + 4);
    bool valid = read(bones, h[1] * sizeof(*bones)) && read(c.verts, h[2] * sizeof(*c.verts)) &&
                 read(c.blend.data(), h[5] * sizeof(uint16_t));
    for (auto &list : c.lists)
    {
        uint16_t pair[2]{};
        valid = read(pair, sizeof(pair)) && valid;
        list.boneOffset = pair[0];
        list.vertCount = pair[1];
        valid = valid && pair[0] % 64 == 0 && pair[0] / 64 < h[1];
    }
    uint32_t vertices = 0, entries = 0;
    for (int w = 0; w < 4; ++w)
    {
        valid = valid && h[6 + w] <= 32767;
        c.surf.vertInfo.vertCount[w] = h[6 + w];
        vertices += h[6 + w];
        entries += (2 * w + 1) * h[6 + w];
    }
    if (h[3])
    {
        valid = valid && vertices == h[2] && entries == h[5] && h[4] == 0;
        size_t index = 0;
        for (int w = 0; valid && w < 4; ++w)
            for (uint32_t v = 0; valid && v < h[6 + w]; ++v)
            {
                valid = c.blend[index] % 64 == 0 && c.blend[index] / 64 < h[1];
                for (int k = 1; k <= w; ++k)
                    valid = valid && c.blend[index + 2 * k - 1] % 64 == 0 &&
                            c.blend[index + 2 * k - 1] / 64 < h[1];
                index += 2 * w + 1;
            }
    }
    else
    {
        vertices = 0;
        for (const auto &list : c.lists)
            vertices += list.vertCount;
        valid = valid && vertices == h[2] && h[5] == 0;
    }
    valid = valid && std::fgetc(file) == EOF;
    std::fclose(file);
    c.surf.verts0 = c.verts;
    c.surf.vertInfo.vertsBlend = c.blend.data();
    c.surf.vertList = c.lists.data();
    auto *a = AlignedArray<GfxPackedVertex>(h[2] + 1);
    auto *b = AlignedArray<GfxPackedVertex>(h[2] + 1);
    auto *na = AlignedArray<GfxPackedVertexNormal>(h[2] + 1);
    auto *nb = AlignedArray<GfxPackedVertexNormal>(h[2] + 1);
    auto *input = AlignedArray<GfxPackedVertexNormal>(h[2] + 1);
    for (uint32_t v = 0; v < h[2]; ++v)
    {
        input[v].normal = c.verts[v].normal;
        input[v].tangent = c.verts[v].tangent;
    }
    std::mt19937 rng(0xFACEu);
    for (int pose = 0; valid && pose < 9; ++pose)
    {
        if (pose)
            RandomBones(rng, bones, h[1]);
        for (int mode = 0; mode < 3; ++mode)
        {
            R_SkinXSurfaceSkinnedSse(&c.surf, bones, mode == 2 ? input : nullptr, mode ? na : nullptr, a);
            R_SkinXSurfaceSkinnedSimd(&c.surf, bones, mode == 2 ? input : nullptr, mode ? nb : nullptr, b);
            Check(SameVerts(a, b, h[2]), path);
            if (mode)
                Check(!std::memcmp(na, nb, h[2] * sizeof(*na)), path);
        }
    }
    std::printf("CAPTURE_SKIN valid=%d bones=%u vertices=%u poses=9 modes=3 file=%s\n", valid, h[1], h[2], path);
    std::free(bones);
    std::free(c.verts);
    std::free(a);
    std::free(b);
    std::free(na);
    std::free(nb);
    std::free(input);
    return valid;
}

int main(int argc, char **argv)
{
    DecodeDivisionIsExact();
#ifdef KISAK_SKIN_NEON_TEST
    DecodeMatchesPortable();
#endif
    std::mt19937 rng(0xC0D4u);
    int cases = 0;
    for (int i = 1; i < argc; ++i)
        Check(CheckCapturedSurface(argv[i]), "character surface capture is invalid");
    for (int round = 0; round < 200; ++round)
    {
        for (int mode = 0; mode < 3; ++mode)
        {
            RunCase(rng, true, mode, "weighted");
            RunCase(rng, false, mode, "rigid");
            cases += 2;
        }
    }
    std::printf(KISAK_SKIN_TEST_NAME " cases=%d failures=%d\n", cases, g_failures);
    if (g_failures)
        return 1;
    std::printf("PASS:" KISAK_SKIN_TEST_NAME "\n");
    return 0;
}
