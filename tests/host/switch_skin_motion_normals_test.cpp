#include <universal/q_shared.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_buffers.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_dobj_skin.h>
#include <gfx_d3d/r_model_skin_simd.h>
#include <cstdio>
#include <cstring>

DxGlobals dx{};
GfxBuffers gfxBuf{};
const dvar_t *sys_SSE;
const dvar_t *r_sse_skinning;

// The host uses the portable entry for the target's SIMD implementation.
void R_SkinXSurfaceSkinnedSse(const XSurface *s, const DObjSkelMat *m,
                            GfxPackedVertexNormal *in, GfxPackedVertexNormal *out,
                            GfxPackedVertex *vertices)
{
    R_SkinXSurfaceSkinnedSimd(s, m, in, out, vertices);
}

int main()
{
    dvar_t enabled{};
    enabled.current.enabled = true;
    sys_SSE = r_sse_skinning = &enabled;
    gfxBuf.fastSkin = true;
    alignas(16) GfxPackedVertex input{}, output{};
    input.normal.packed = 0x3fff7f7f;
    input.tangent.packed = 0x3f7fff7f;
    input.binormalSign = 1.0f;
    uint16_t blend = 0;
    XSurface surface{};
    surface.deformed = true;
    surface.vertCount = 1;
    surface.verts0 = &input;
    surface.vertInfo.vertCount[0] = 1;
    surface.vertInfo.vertsBlend = &blend;
    DObjAnimMat base{};
    base.quat[3] = 1.0f;
    base.transWeight = 2.0f;
    GfxModelSkinnedSurface record{};
    record.xsurf = &surface;
    record.info.baseMat = &base;
    record.info.boneCount = 1;
    record.skinnedCachedOffset = 0;
    SkinXModelCmd command{};
    command.modelSurfs = &record;
    command.mat = &base;
    command.surfCount = 1;
    command.surfacePartBits[0] = 0x80000000u;
    GfxPackedVertexNormal oldNormals[2]{}, freshNormals[1]{};
    std::memset(oldNormals, 0xa5, sizeof(oldNormals));
    gfxBuf.oldSkinnedCacheNormalsAddr = oldNormals;
    gfxBuf.skinnedCacheNormalsAddr = freshNormals;
    gfxBuf.skinnedCacheLockAddr = reinterpret_cast<uint8_t *>(&output);
    int failures = 0;
    GfxPackedVertex reference{};
    DObjSkelMat identity{};
    for (int i = 0; i < 3; ++i)
        identity.axis[i][i] = 1.0f;
    identity.origin[3] = 1.0f;
    R_SkinXSurfaceSkinnedSimd(&surface, &identity, nullptr, nullptr, &reference);
    // SP motion metadata: first sight, previous byte offset + 1, rejection.
    for (int offset : {0, 33, -1})
    {
        record.oldSkinnedCachedOffset = offset;
        R_SkinXModelCmd(reinterpret_cast<_WORD *>(&command));
        const bool ok = output.normal.packed == reference.normal.packed &&
                        output.tangent.packed == reference.tangent.packed &&
                        freshNormals[0].normal.packed == reference.normal.packed;
        std::printf("%s:SP_SKIN_MOTION_NORMALS metadata=%d\n", ok ? "PASS" : "FAIL", offset);
        failures += !ok;
    }
    return failures ? 1 : 0;
}
