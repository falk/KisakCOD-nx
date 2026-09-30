#pragma once

// Portable-SIMD vertex skinning (r_model_skin_simd.cpp): the same
// computation as R_SkinXSurfaceSkinnedSse, for targets without x86 SIMD.

#include <universal/q_shared.h>
#include "r_model_skin.h"

#include <xanim/xanim.h>

void R_SkinXSurfaceSkinnedSimd(const XSurface *xsurf, const DObjSkelMat *boneMatrix,
                               GfxPackedVertexNormal *skinVertNormalIn, GfxPackedVertexNormal *skinVertNormalOut,
                               GfxPackedVertex *skinVerticesOut);
