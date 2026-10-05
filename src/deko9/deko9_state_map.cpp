#include "deko9_state_map.h"

namespace deko9
{
namespace
{
constexpr DkImageSwizzle R = DkImageSwizzle_Red;
constexpr DkImageSwizzle G = DkImageSwizzle_Green;
constexpr DkImageSwizzle B = DkImageSwizzle_Blue;
constexpr DkImageSwizzle A = DkImageSwizzle_Alpha;
constexpr DkImageSwizzle Z = DkImageSwizzle_Zero;
constexpr DkImageSwizzle O = DkImageSwizzle_One;

// Swizzles give D3D9's sampling results: missing channels read 1 (alpha
// and float formats) or luminance replicates, as the D3D9 docs specify.
const FormatInfo kFormats[] = {
    {D3DFMT_A8R8G8B8, DkImageFormat_BGRA8_Unorm, {R, G, B, A}, 1, 4, false, false, true},
    {D3DFMT_X8R8G8B8, DkImageFormat_BGRX8_Unorm, {R, G, B, O}, 1, 4, false, false, true},
    {D3DFMT_A8B8G8R8, DkImageFormat_RGBA8_Unorm, {R, G, B, A}, 1, 4, false, false, true},
    {D3DFMT_X8B8G8R8, DkImageFormat_RGBX8_Unorm, {R, G, B, O}, 1, 4, false, false, true},
    {D3DFMT_L8, DkImageFormat_R8_Unorm, {R, R, R, O}, 1, 1, false, false, false},
    {D3DFMT_A8L8, DkImageFormat_RG8_Unorm, {R, R, R, G}, 1, 2, false, false, false},
    {D3DFMT_A8, DkImageFormat_R8_Unorm, {Z, Z, Z, R}, 1, 1, false, false, false},
    {D3DFMT_L16, DkImageFormat_R16_Unorm, {R, R, R, O}, 1, 2, false, false, false},
    {D3DFMT_G16R16, DkImageFormat_RG16_Unorm, {R, G, O, O}, 1, 4, false, false, true},
    {D3DFMT_A16B16G16R16, DkImageFormat_RGBA16_Unorm, {R, G, B, A}, 1, 8, false, false, true},
    {D3DFMT_R16F, DkImageFormat_R16_Float, {R, O, O, O}, 1, 2, false, false, true},
    {D3DFMT_G16R16F, DkImageFormat_RG16_Float, {R, G, O, O}, 1, 4, false, false, true},
    {D3DFMT_A16B16G16R16F, DkImageFormat_RGBA16_Float, {R, G, B, A}, 1, 8, false, false, true},
    {D3DFMT_A2B10G10R10, DkImageFormat_RGB10A2_Unorm, {R, G, B, A}, 1, 4, false, false, true},
    {D3DFMT_R32F, DkImageFormat_R32_Float, {R, O, O, O}, 1, 4, false, false, true},
    {D3DFMT_G32R32F, DkImageFormat_RG32_Float, {R, G, O, O}, 1, 8, false, false, true},
    {D3DFMT_A32B32G32R32F, DkImageFormat_RGBA32_Float, {R, G, B, A}, 1, 16, false, false, true},
    {D3DFMT_DXT1, DkImageFormat_RGBA_BC1, {R, G, B, A}, 4, 8, false, false, false},
    {D3DFMT_DXT2, DkImageFormat_RGBA_BC2, {R, G, B, A}, 4, 16, false, false, false},
    {D3DFMT_DXT3, DkImageFormat_RGBA_BC2, {R, G, B, A}, 4, 16, false, false, false},
    {D3DFMT_DXT4, DkImageFormat_RGBA_BC3, {R, G, B, A}, 4, 16, false, false, false},
    {D3DFMT_DXT5, DkImageFormat_RGBA_BC3, {R, G, B, A}, 4, 16, false, false, false},
    // Depth formats sample as depth in red (D3D9 on NVIDIA compares instead;
    // the device builds sampler2DShadow variants for those bindings).
    {D3DFMT_D24S8, DkImageFormat_Z24S8, {R, R, R, O}, 1, 4, true, true, true},
    {D3DFMT_D24X8, DkImageFormat_Z24X8, {R, R, R, O}, 1, 4, true, false, true},
    {D3DFMT_D16, DkImageFormat_Z16, {R, R, R, O}, 1, 2, true, false, true},
    {D3DFMT_D32F_LOCKABLE, DkImageFormat_ZF32, {R, R, R, O}, 1, 4, true, false, true},
};
} // namespace

const FormatInfo *LookupFormat(D3DFORMAT format)
{
    for (const FormatInfo &info : kFormats)
    {
        if (info.d3d == format)
            return &info;
    }
    return nullptr;
}

bool MapCompare(DWORD d3d, DkCompareOp *out)
{
    switch (d3d)
    {
    case D3DCMP_NEVER: *out = DkCompareOp_Never; return true;
    case D3DCMP_LESS: *out = DkCompareOp_Less; return true;
    case D3DCMP_EQUAL: *out = DkCompareOp_Equal; return true;
    case D3DCMP_LESSEQUAL: *out = DkCompareOp_Lequal; return true;
    case D3DCMP_GREATER: *out = DkCompareOp_Greater; return true;
    case D3DCMP_NOTEQUAL: *out = DkCompareOp_NotEqual; return true;
    case D3DCMP_GREATEREQUAL: *out = DkCompareOp_Gequal; return true;
    case D3DCMP_ALWAYS: *out = DkCompareOp_Always; return true;
    default: return false;
    }
}

bool MapStencilOp(DWORD d3d, DkStencilOp *out)
{
    switch (d3d)
    {
    case D3DSTENCILOP_KEEP: *out = DkStencilOp_Keep; return true;
    case D3DSTENCILOP_ZERO: *out = DkStencilOp_Zero; return true;
    case D3DSTENCILOP_REPLACE: *out = DkStencilOp_Replace; return true;
    case D3DSTENCILOP_INCRSAT: *out = DkStencilOp_Incr; return true;
    case D3DSTENCILOP_DECRSAT: *out = DkStencilOp_Decr; return true;
    case D3DSTENCILOP_INVERT: *out = DkStencilOp_Invert; return true;
    case D3DSTENCILOP_INCR: *out = DkStencilOp_IncrWrap; return true;
    case D3DSTENCILOP_DECR: *out = DkStencilOp_DecrWrap; return true;
    default: return false;
    }
}

bool MapBlendFactor(DWORD d3d, DkBlendFactor *out)
{
    switch (d3d)
    {
    case D3DBLEND_ZERO: *out = DkBlendFactor_Zero; return true;
    case D3DBLEND_ONE: *out = DkBlendFactor_One; return true;
    case D3DBLEND_SRCCOLOR: *out = DkBlendFactor_SrcColor; return true;
    case D3DBLEND_INVSRCCOLOR: *out = DkBlendFactor_InvSrcColor; return true;
    case D3DBLEND_SRCALPHA: *out = DkBlendFactor_SrcAlpha; return true;
    case D3DBLEND_INVSRCALPHA: *out = DkBlendFactor_InvSrcAlpha; return true;
    case D3DBLEND_DESTALPHA: *out = DkBlendFactor_DstAlpha; return true;
    case D3DBLEND_INVDESTALPHA: *out = DkBlendFactor_InvDstAlpha; return true;
    case D3DBLEND_DESTCOLOR: *out = DkBlendFactor_DstColor; return true;
    case D3DBLEND_INVDESTCOLOR: *out = DkBlendFactor_InvDstColor; return true;
    case D3DBLEND_SRCALPHASAT: *out = DkBlendFactor_SrcAlphaSaturate; return true;
    case D3DBLEND_BLENDFACTOR: *out = DkBlendFactor_ConstColor; return true;
    case D3DBLEND_INVBLENDFACTOR: *out = DkBlendFactor_InvConstColor; return true;
    // BOTHSRCALPHA / BOTHINVSRCALPHA also override the destination factor;
    // the device resolves them before calling this.
    default: return false;
    }
}

bool MapBlendOp(DWORD d3d, DkBlendOp *out)
{
    switch (d3d)
    {
    case D3DBLENDOP_ADD: *out = DkBlendOp_Add; return true;
    case D3DBLENDOP_SUBTRACT: *out = DkBlendOp_Sub; return true;
    case D3DBLENDOP_REVSUBTRACT: *out = DkBlendOp_RevSub; return true;
    case D3DBLENDOP_MIN: *out = DkBlendOp_Min; return true;
    case D3DBLENDOP_MAX: *out = DkBlendOp_Max; return true;
    default: return false;
    }
}

bool MapAddress(DWORD d3d, DkWrapMode *out)
{
    switch (d3d)
    {
    case D3DTADDRESS_WRAP: *out = DkWrapMode_Repeat; return true;
    case D3DTADDRESS_MIRROR: *out = DkWrapMode_MirroredRepeat; return true;
    case D3DTADDRESS_CLAMP: *out = DkWrapMode_ClampToEdge; return true;
    case D3DTADDRESS_BORDER: *out = DkWrapMode_ClampToBorder; return true;
    case D3DTADDRESS_MIRRORONCE: *out = DkWrapMode_MirrorClampToEdge; return true;
    default: return false;
    }
}

bool MapMinMagFilter(DWORD d3d, DkFilter *out)
{
    switch (d3d)
    {
    case D3DTEXF_POINT: *out = DkFilter_Nearest; return true;
    case D3DTEXF_LINEAR:
    case D3DTEXF_ANISOTROPIC: *out = DkFilter_Linear; return true;
    default: return false;
    }
}

bool MapMipFilter(DWORD d3d, DkMipFilter *out)
{
    switch (d3d)
    {
    case D3DTEXF_NONE: *out = DkMipFilter_None; return true;
    case D3DTEXF_POINT: *out = DkMipFilter_Nearest; return true;
    case D3DTEXF_LINEAR:
    case D3DTEXF_ANISOTROPIC: *out = DkMipFilter_Linear; return true;
    default: return false;
    }
}

bool MapFillMode(DWORD d3d, DkPolygonMode *out)
{
    switch (d3d)
    {
    case D3DFILL_POINT: *out = DkPolygonMode_Point; return true;
    case D3DFILL_WIREFRAME: *out = DkPolygonMode_Line; return true;
    case D3DFILL_SOLID: *out = DkPolygonMode_Fill; return true;
    default: return false;
    }
}

bool MapDeclType(BYTE d3d, VertexFormat *out)
{
    switch (d3d)
    {
    case D3DDECLTYPE_FLOAT1: *out = {DkVtxAttribSize_1x32, DkVtxAttribType_Float, false, 4}; return true;
    case D3DDECLTYPE_FLOAT2: *out = {DkVtxAttribSize_2x32, DkVtxAttribType_Float, false, 8}; return true;
    case D3DDECLTYPE_FLOAT3: *out = {DkVtxAttribSize_3x32, DkVtxAttribType_Float, false, 12}; return true;
    case D3DDECLTYPE_FLOAT4: *out = {DkVtxAttribSize_4x32, DkVtxAttribType_Float, false, 16}; return true;
    // D3DCOLOR is B,G,R,A in memory and reads as (R,G,B,A).
    case D3DDECLTYPE_D3DCOLOR: *out = {DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, true, 4}; return true;
    case D3DDECLTYPE_UBYTE4: *out = {DkVtxAttribSize_4x8, DkVtxAttribType_Uscaled, false, 4}; return true;
    case D3DDECLTYPE_SHORT2: *out = {DkVtxAttribSize_2x16, DkVtxAttribType_Sscaled, false, 4}; return true;
    case D3DDECLTYPE_SHORT4: *out = {DkVtxAttribSize_4x16, DkVtxAttribType_Sscaled, false, 8}; return true;
    case D3DDECLTYPE_UBYTE4N: *out = {DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, false, 4}; return true;
    case D3DDECLTYPE_SHORT2N: *out = {DkVtxAttribSize_2x16, DkVtxAttribType_Snorm, false, 4}; return true;
    case D3DDECLTYPE_SHORT4N: *out = {DkVtxAttribSize_4x16, DkVtxAttribType_Snorm, false, 8}; return true;
    case D3DDECLTYPE_USHORT2N: *out = {DkVtxAttribSize_2x16, DkVtxAttribType_Unorm, false, 4}; return true;
    case D3DDECLTYPE_USHORT4N: *out = {DkVtxAttribSize_4x16, DkVtxAttribType_Unorm, false, 8}; return true;
    case D3DDECLTYPE_FLOAT16_2: *out = {DkVtxAttribSize_2x16, DkVtxAttribType_Float, false, 4}; return true;
    case D3DDECLTYPE_FLOAT16_4: *out = {DkVtxAttribSize_4x16, DkVtxAttribType_Float, false, 8}; return true;
    // UDEC3/DEC3N put the 2-bit field first in D3D and last in deko3d's
    // 10_10_10_2 layout; not mapped.
    default: return false;
    }
}

bool MapPrimitive(D3DPRIMITIVETYPE type, UINT primCount, DkPrimitive *prim, uint32_t *count)
{
    switch (type)
    {
    case D3DPT_POINTLIST: *prim = DkPrimitive_Points; *count = primCount; return true;
    case D3DPT_LINELIST: *prim = DkPrimitive_Lines; *count = primCount * 2; return true;
    case D3DPT_LINESTRIP: *prim = DkPrimitive_LineStrip; *count = primCount + 1; return true;
    case D3DPT_TRIANGLELIST: *prim = DkPrimitive_Triangles; *count = primCount * 3; return true;
    case D3DPT_TRIANGLESTRIP: *prim = DkPrimitive_TriangleStrip; *count = primCount + 2; return true;
    case D3DPT_TRIANGLEFAN: *prim = DkPrimitive_TriangleFan; *count = primCount + 2; return true;
    default: return false;
    }
}

} // namespace deko9
