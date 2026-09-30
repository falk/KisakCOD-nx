// IDirect3D9 for the optional deko3d renderer: one adapter (the Switch GPU),
// one display mode, format support from deko9_state_map's table.

// libnx before the D3D9 headers: the vendored windows_base.h defines `interface`.
#include <switch.h>

#include "deko9_internal.h"

#include <platform/switch/switch_port_log.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

namespace deko9
{
namespace
{
// Port_Log/Port_Fail write exact bytes, no added newline (other callers
// build multi-fragment lines or already carry their own); deko9's lines
// are always one complete line per call, so add it here once.
void EmitLine(const char *line, bool fail)
{
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s\n", line);
    if (fail)
        Port_Fail(buf);
    else
        Port_Log(buf);
}
} // namespace

void Log(const char *fmt, ...)
{
    char buf[1024];
    int len = std::snprintf(buf, sizeof(buf), "DEKO9 ");
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
    va_end(args);
    EmitLine(buf, false);
}

void LogLine(const char *line) { EmitLine(line, false); }

HRESULT Fail(const char *what, const char *fmt, ...)
{
    static std::mutex s_lock;
    static std::set<std::string> s_reported;
    char detail[768];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(s_lock);
    // Once per distinct message: a failing call can repeat every frame.
    if (s_reported.size() < 4096 && s_reported.insert(std::string(what) + detail).second)
    {
        char line[1024];
        std::snprintf(line, sizeof(line), "FAIL:DEKO9_%s %s", what, detail);
        EmitLine(line, true);
    }
    return D3DERR_INVALIDCALL;
}

namespace
{

constexpr UINT kWidth = kDisplayWidth;
constexpr UINT kHeight = kDisplayHeight;

class Direct3D final : public Object<Deko9Default_IDirect3D9>
{
public:
    Direct3D() : Object(nullptr) {}

    STDMETHOD(RegisterSoftwareDevice)(void *) override { return D3DERR_INVALIDCALL; }
    STDMETHOD_(UINT, GetAdapterCount)() override { return 1; }

    STDMETHOD(GetAdapterIdentifier)(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *id) override
    {
        if (adapter || !id)
            return D3DERR_INVALIDCALL;
        std::memset(id, 0, sizeof(*id));
        std::snprintf(id->Driver, sizeof(id->Driver), "deko9");
        std::snprintf(id->Description, sizeof(id->Description), "NVIDIA Tegra X1 (deko3d)");
        std::snprintf(id->DeviceName, sizeof(id->DeviceName), "\\\\.\\DISPLAY1");
        id->VendorId = 0x10de;
        id->DeviceId = 0x1234;
        id->WHQLLevel = 1;
        return D3D_OK;
    }

    STDMETHOD_(UINT, GetAdapterModeCount)(UINT adapter, D3DFORMAT format) override
    {
        return !adapter && format == D3DFMT_X8R8G8B8 ? 1 : 0;
    }

    STDMETHOD(EnumAdapterModes)(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE *out) override
    {
        if (adapter || mode || format != D3DFMT_X8R8G8B8 || !out)
            return D3DERR_INVALIDCALL;
        return GetAdapterDisplayMode(adapter, out);
    }

    STDMETHOD(GetAdapterDisplayMode)(UINT adapter, D3DDISPLAYMODE *mode) override
    {
        if (adapter || !mode)
            return D3DERR_INVALIDCALL;
        *mode = {kWidth, kHeight, 60, D3DFMT_X8R8G8B8};
        return D3D_OK;
    }

    STDMETHOD(CheckDeviceType)(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT backBuffer, BOOL windowed) override
    {
        return !adapter && type == D3DDEVTYPE_HAL && display == D3DFMT_X8R8G8B8 &&
                       (backBuffer == D3DFMT_X8R8G8B8 || backBuffer == D3DFMT_A8R8G8B8)
                   ? D3D_OK
                   : D3DERR_NOTAVAILABLE;
    }

    STDMETHOD(CheckDeviceFormat)(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, DWORD usage, D3DRESOURCETYPE rtype, D3DFORMAT format) override
    {
        if (adapter || type != D3DDEVTYPE_HAL)
            return D3DERR_INVALIDCALL;
        const FormatInfo *info = LookupFormat(format);
        if (!info)
            return D3DERR_NOTAVAILABLE;
        if (usage & D3DUSAGE_DEPTHSTENCIL)
            return info->depth ? D3D_OK : D3DERR_NOTAVAILABLE;
        if (usage & D3DUSAGE_RENDERTARGET)
            return info->renderable && !info->depth ? D3D_OK : D3DERR_NOTAVAILABLE;
        if (usage & (D3DUSAGE_AUTOGENMIPMAP | D3DUSAGE_QUERY_VERTEXTEXTURE | D3DUSAGE_QUERY_SRGBREAD |
                     D3DUSAGE_QUERY_SRGBWRITE | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING))
            return D3DERR_NOTAVAILABLE;
        if (info->depth && rtype == D3DRTYPE_VOLUMETEXTURE)
            return D3DERR_NOTAVAILABLE;
        return D3D_OK;
    }

    STDMETHOD(CheckDeviceMultiSampleType)(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed, D3DMULTISAMPLE_TYPE ms, DWORD *quality) override
    {
        if (quality)
            *quality = 1;
        return !adapter && ms == D3DMULTISAMPLE_NONE ? D3D_OK : D3DERR_NOTAVAILABLE;
    }

    STDMETHOD(CheckDepthStencilMatch)(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT rt, D3DFORMAT ds) override
    {
        const FormatInfo *color = LookupFormat(rt);
        const FormatInfo *depth = LookupFormat(ds);
        return !adapter && color && color->renderable && !color->depth && depth && depth->depth
                   ? D3D_OK
                   : D3DERR_NOTAVAILABLE;
    }

    STDMETHOD(CheckDeviceFormatConversion)(UINT adapter, D3DDEVTYPE type, D3DFORMAT src, D3DFORMAT dst) override
    {
        return D3DERR_NOTAVAILABLE;
    }

    STDMETHOD(GetDeviceCaps)(UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps) override
    {
        if (adapter || !caps)
            return D3DERR_INVALIDCALL;
        FillCaps(caps);
        return D3D_OK;
    }

    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT adapter) override
    {
        // Horizon has one display; any non-null handle identifies it.
        return adapter ? nullptr : reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(1));
    }

    STDMETHOD(CreateDevice)(UINT adapter, D3DDEVTYPE type, HWND window, DWORD behavior, D3DPRESENT_PARAMETERS *params, IDirect3DDevice9 **out) override
    {
        if (!out || !params || adapter || type != D3DDEVTYPE_HAL)
            return D3DERR_INVALIDCALL;
        *out = nullptr;
        HWND target = params->hDeviceWindow ? params->hDeviceWindow : window;
        if (!target)
            return Fail("CREATE_DEVICE", "no native window");
        if (params->MultiSampleType != D3DMULTISAMPLE_NONE)
            return Fail("CREATE_DEVICE", "multisample type %d unsupported", (int)params->MultiSampleType);
        Device *device = new Device(this, target, *params);
        std::string error;
        if (!device->Init(&error))
        {
            device->Release();
            return Fail("CREATE_DEVICE", "%s", error.c_str());
        }
        *out = device;
        Log("device created %ux%u backbuffers=%u interval=%u", params->BackBufferWidth,
            params->BackBufferHeight, params->BackBufferCount, params->PresentationInterval);
        return D3D_OK;
    }
};

} // namespace

void FillCaps(D3DCAPS9 *caps)
{
    std::memset(caps, 0, sizeof(*caps));
    caps->DeviceType = D3DDEVTYPE_HAL;
    caps->AdapterOrdinal = 0;
    caps->Caps2 = D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_FULLSCREENGAMMA;
    caps->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD;
    caps->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
    caps->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY |
                    D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY |
                    D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                    D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                    D3DDEVCAPS_TEXTURENONLOCALVIDMEM | D3DDEVCAPS_DRAWPRIMITIVES2 |
                    D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                    D3DDEVCAPS_CANBLTSYSTONONLOCAL | D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_PUREDEVICE;
    caps->PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW |
                              D3DPMISCCAPS_CULLCCW | D3DPMISCCAPS_COLORWRITEENABLE |
                              D3DPMISCCAPS_CLIPPLANESCALEDPOINTS | D3DPMISCCAPS_TSSARGTEMP |
                              D3DPMISCCAPS_BLENDOP | D3DPMISCCAPS_INDEPENDENTWRITEMASKS |
                              D3DPMISCCAPS_SEPARATEALPHABLEND | D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS |
                              D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING;
    caps->RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX |
                       D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_MIPMAPLODBIAS |
                       D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_SCISSORTEST |
                       D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS;
    caps->ZCmpCaps = caps->AlphaCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL |
                                          D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER |
                                          D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL |
                                          D3DPCMPCAPS_ALWAYS;
    caps->SrcBlendCaps = caps->DestBlendCaps =
        D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR |
        D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA |
        D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
        D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA |
        D3DPBLENDCAPS_BLENDFACTOR;
    caps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB |
                      D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;
    caps->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA |
                        D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_CUBEMAP |
                        D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_MIPMAP |
                        D3DPTEXTURECAPS_MIPVOLUMEMAP | D3DPTEXTURECAPS_MIPCUBEMAP;
    const DWORD filters = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                          D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MIPFPOINT |
                          D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT |
                          D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC;
    caps->TextureFilterCaps = caps->CubeTextureFilterCaps = caps->VolumeTextureFilterCaps = filters;
    caps->StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                                  D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    caps->TextureAddressCaps = caps->VolumeTextureAddressCaps =
        D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
        D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
    caps->MaxTextureWidth = caps->MaxTextureHeight = 8192;
    caps->MaxVolumeExtent = 2048;
    caps->MaxTextureRepeat = 8192;
    caps->MaxTextureAspectRatio = 8192;
    caps->MaxAnisotropy = 16;
    caps->MaxVertexW = 1e10f;
    caps->GuardBandLeft = caps->GuardBandTop = -32768.0f;
    caps->GuardBandRight = caps->GuardBandBottom = 32768.0f;
    caps->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE |
                        D3DSTENCILCAPS_INCRSAT | D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT |
                        D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR | D3DSTENCILCAPS_TWOSIDED;
    caps->FVFCaps = 8;
    caps->MaxTextureBlendStages = 8;
    caps->MaxSimultaneousTextures = 8;
    caps->MaxUserClipPlanes = 0;
    caps->MaxVertexBlendMatrices = 0;
    caps->MaxPointSize = 256.0f;
    caps->MaxPrimitiveCount = 0x555555;
    caps->MaxVertexIndex = 0xffffff;
    caps->MaxStreams = 16;
    caps->MaxStreamStride = 508;
    caps->VertexShaderVersion = D3DVS_VERSION(3, 0);
    caps->MaxVertexShaderConst = DEKO9_VS_CONST_REGS;
    caps->PixelShaderVersion = D3DPS_VERSION(3, 0);
    caps->PixelShader1xMaxValue = 65504.0f;
    caps->DevCaps2 = D3DDEVCAPS2_STREAMOFFSET | D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET;
    caps->MasterAdapterOrdinal = 0;
    caps->AdapterOrdinalInGroup = 0;
    caps->NumberOfAdaptersInGroup = 1;
    caps->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N |
                      D3DDTCAPS_USHORT2N | D3DDTCAPS_USHORT4N | D3DDTCAPS_FLOAT16_2 |
                      D3DDTCAPS_FLOAT16_4;
    caps->NumSimultaneousRTs = 4;
    caps->VS20Caps.Caps = D3DVS20CAPS_PREDICATION;
    caps->VS20Caps.DynamicFlowControlDepth = 24;
    caps->VS20Caps.NumTemps = 32;
    caps->VS20Caps.StaticFlowControlDepth = 4;
    caps->PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE | D3DPS20CAPS_GRADIENTINSTRUCTIONS |
                          D3DPS20CAPS_PREDICATION | D3DPS20CAPS_NODEPENDENTREADLIMIT |
                          D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
    caps->PS20Caps.DynamicFlowControlDepth = 24;
    caps->PS20Caps.NumTemps = 32;
    caps->PS20Caps.StaticFlowControlDepth = 4;
    caps->PS20Caps.NumInstructionSlots = 512;
    caps->VertexTextureFilterCaps = filters;
    caps->MaxVShaderInstructionsExecuted = 65535;
    caps->MaxPShaderInstructionsExecuted = 65535;
    caps->MaxVertexShader30InstructionSlots = 32768;
    caps->MaxPixelShader30InstructionSlots = 32768;
}

} // namespace deko9

void Deko9_ReportUnsupported(const char *what)
{
    deko9::Fail("UNSUPPORTED", "%s", what);
}

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT sdkVersion)
{
    deko9::Log("Direct3DCreate9 sdk=0x%x", sdkVersion);
    return new deko9::Direct3D();
}
