#pragma once

// D3D9 enum -> deko3d enum mapping for the optional deko3d renderer.
// No libnx dependency, so the host test checks it directly. Every mapper
// returns false for a value it does not map; callers report that loudly.

#include <d3d9.h>
#include <deko3d.h>

#include <cstdint>

namespace deko9
{

struct FormatInfo
{
    D3DFORMAT d3d;
    DkImageFormat dk;
    DkImageSwizzle swizzle[4];
    uint8_t blockWidth; // 1, or 4 for BCn
    uint8_t blockBytes; // bytes per pixel, or per 4x4 block
    bool depth;
    bool stencil;
    bool renderable; // usable as D3DUSAGE_RENDERTARGET / DEPTHSTENCIL
};

// Null when the format is not supported (CheckDeviceFormat reports that).
const FormatInfo *LookupFormat(D3DFORMAT format);

bool MapCompare(DWORD d3d, DkCompareOp *out);
bool MapStencilOp(DWORD d3d, DkStencilOp *out);
bool MapBlendFactor(DWORD d3d, DkBlendFactor *out);
bool MapBlendOp(DWORD d3d, DkBlendOp *out);
bool MapAddress(DWORD d3d, DkWrapMode *out);
bool MapMinMagFilter(DWORD d3d, DkFilter *out);
bool MapMipFilter(DWORD d3d, DkMipFilter *out);
bool MapFillMode(DWORD d3d, DkPolygonMode *out);

struct VertexFormat
{
    DkVtxAttribSize size;
    DkVtxAttribType type;
    bool bgra;
    uint8_t bytes;
};
bool MapDeclType(BYTE d3d, VertexFormat *out);

// Maps a primitive type and count to the deko3d primitive and the number
// of vertices (or indices) the draw consumes.
bool MapPrimitive(D3DPRIMITIVETYPE type, UINT primCount, DkPrimitive *prim, uint32_t *count);

} // namespace deko9
