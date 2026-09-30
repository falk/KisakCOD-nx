#pragma once

#include "r_rendercmds.h"
#include "rb_backend.h"

void R_DepthPrepass(
    GfxRenderTargetId renderTargetId,
    const struct GfxViewInfo *viewInfo,
    struct GfxCmdBuf *cmdBuf);
// r_deko9NativeFloatZ 1: float-Z comes from the depth buffer, so the
// prepass callback never renders the build floatz technique.
void R_DepthPrepassFloatZFromDepth(bool fromDepth);
