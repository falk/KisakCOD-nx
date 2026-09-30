// Generated from d3d9.h by a local script.
// Do not edit by hand.
#pragma once

#include <d3d9.h>

// Reports FAIL:DEKO9_UNSUPPORTED <what> once per name.
void Deko9_ReportUnsupported(const char *what);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

struct Deko9Default_IDirect3D9 : public IDirect3D9
{
    STDMETHOD_(HRESULT, RegisterSoftwareDevice)(void* pInitializeFunction) override
    {
        Deko9_ReportUnsupported("IDirect3D9::RegisterSoftwareDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(UINT, GetAdapterCount)() override
    {
        Deko9_ReportUnsupported("IDirect3D9::GetAdapterCount");
        return (UINT)0;
    }
    STDMETHOD_(HRESULT, GetAdapterIdentifier)(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override
    {
        Deko9_ReportUnsupported("IDirect3D9::GetAdapterIdentifier");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT Adapter, D3DFORMAT Format) override
    {
        Deko9_ReportUnsupported("IDirect3D9::GetAdapterModeCount");
        return (UINT)0;
    }
    STDMETHOD_(HRESULT, EnumAdapterModes)(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override
    {
        Deko9_ReportUnsupported("IDirect3D9::EnumAdapterModes");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetAdapterDisplayMode)(UINT Adapter, D3DDISPLAYMODE* pMode) override
    {
        Deko9_ReportUnsupported("IDirect3D9::GetAdapterDisplayMode");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CheckDeviceType)(UINT iAdapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, WINBOOL bWindowed) override
    {
        Deko9_ReportUnsupported("IDirect3D9::CheckDeviceType");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CheckDeviceFormat)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override
    {
        Deko9_ReportUnsupported("IDirect3D9::CheckDeviceFormat");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CheckDeviceMultiSampleType)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, WINBOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override
    {
        Deko9_ReportUnsupported("IDirect3D9::CheckDeviceMultiSampleType");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CheckDepthStencilMatch)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override
    {
        Deko9_ReportUnsupported("IDirect3D9::CheckDepthStencilMatch");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CheckDeviceFormatConversion)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override
    {
        Deko9_ReportUnsupported("IDirect3D9::CheckDeviceFormatConversion");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDeviceCaps)(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override
    {
        Deko9_ReportUnsupported("IDirect3D9::GetDeviceCaps");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT Adapter) override
    {
        Deko9_ReportUnsupported("IDirect3D9::GetAdapterMonitor");
        return (HMONITOR)0;
    }
    STDMETHOD_(HRESULT, CreateDevice)(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, struct IDirect3DDevice9** ppReturnedDeviceInterface) override
    {
        Deko9_ReportUnsupported("IDirect3D9::CreateDevice");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DDevice9 : public IDirect3DDevice9
{
    STDMETHOD_(HRESULT, TestCooperativeLevel)() override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::TestCooperativeLevel");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(UINT, GetAvailableTextureMem)() override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetAvailableTextureMem");
        return (UINT)0;
    }
    STDMETHOD_(HRESULT, EvictManagedResources)() override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::EvictManagedResources");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDirect3D)(IDirect3D9** ppD3D9) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetDirect3D");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDeviceCaps)(D3DCAPS9* pCaps) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetDeviceCaps");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDisplayMode)(UINT iSwapChain, D3DDISPLAYMODE* pMode) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetDisplayMode");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS *pParameters) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetCreationParameters");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetCursorProperties)(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetCursorProperties");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(void, SetCursorPosition)(int X,int Y, DWORD Flags) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetCursorPosition");
    }
    STDMETHOD_(WINBOOL, ShowCursor)(WINBOOL bShow) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::ShowCursor");
        return (WINBOOL)0;
    }
    STDMETHOD_(HRESULT, CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateAdditionalSwapChain");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetSwapChain)(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetSwapChain");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(UINT, GetNumberOfSwapChains)() override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetNumberOfSwapChains");
        return (UINT)0;
    }
    STDMETHOD_(HRESULT, Reset)(D3DPRESENT_PARAMETERS* pPresentationParameters) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::Reset");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, Present)(const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::Present");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetBackBuffer)(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetBackBuffer");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetRasterStatus)(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetRasterStatus");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetDialogBoxMode)(WINBOOL bEnableDialogs) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetDialogBoxMode");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(void, SetGammaRamp)(UINT swapchain_idx, DWORD flags, const D3DGAMMARAMP *ramp) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetGammaRamp");
    }
    STDMETHOD_(void, GetGammaRamp)(UINT iSwapChain, D3DGAMMARAMP* pRamp) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetGammaRamp");
    }
    STDMETHOD_(HRESULT, CreateTexture)(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateTexture");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreateVolumeTexture)(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateVolumeTexture");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreateCubeTexture)(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateCubeTexture");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreateVertexBuffer)(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateVertexBuffer");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreateIndexBuffer)(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateIndexBuffer");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreateRenderTarget)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, WINBOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateRenderTarget");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreateDepthStencilSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, WINBOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateDepthStencilSurface");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, UpdateSurface)(IDirect3DSurface9 *src_surface, const RECT *src_rect, IDirect3DSurface9 *dst_surface, const POINT *dst_point) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::UpdateSurface");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, UpdateTexture)(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::UpdateTexture");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetRenderTargetData)(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetRenderTargetData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetFrontBufferData)(UINT iSwapChain, IDirect3DSurface9* pDestSurface) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetFrontBufferData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, StretchRect)(IDirect3DSurface9 *src_surface, const RECT *src_rect, IDirect3DSurface9 *dst_surface, const RECT *dst_rect, D3DTEXTUREFILTERTYPE filter) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::StretchRect");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, ColorFill)(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR color) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::ColorFill");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreateOffscreenPlainSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateOffscreenPlainSurface");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetRenderTarget");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetRenderTarget");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetDepthStencilSurface)(IDirect3DSurface9* pNewZStencil) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetDepthStencilSurface");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDepthStencilSurface)(IDirect3DSurface9** ppZStencilSurface) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetDepthStencilSurface");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, BeginScene)() override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::BeginScene");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, EndScene)() override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::EndScene");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, Clear)(DWORD rect_count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::Clear");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetTransform)(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetTransform");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetTransform)(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetTransform");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, MultiplyTransform)(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::MultiplyTransform");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetViewport)(const D3DVIEWPORT9 *viewport) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetViewport");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetViewport)(D3DVIEWPORT9* pViewport) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetViewport");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetMaterial)(const D3DMATERIAL9 *material) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetMaterial");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetMaterial)(D3DMATERIAL9* pMaterial) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetMaterial");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetLight)(DWORD index, const D3DLIGHT9 *light) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetLight");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetLight)(DWORD Index, D3DLIGHT9*) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetLight");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, LightEnable)(DWORD Index, WINBOOL Enable) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::LightEnable");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetLightEnable)(DWORD Index, WINBOOL* pEnable) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetLightEnable");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetClipPlane)(DWORD index, const float *plane) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetClipPlane");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetClipPlane)(DWORD Index, float* pPlane) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetClipPlane");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetRenderState)(D3DRENDERSTATETYPE State, DWORD Value) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetRenderState");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetRenderState)(D3DRENDERSTATETYPE State, DWORD* pValue) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetRenderState");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreateStateBlock)(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateStateBlock");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, BeginStateBlock)() override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::BeginStateBlock");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, EndStateBlock)(IDirect3DStateBlock9** ppSB) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::EndStateBlock");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetClipStatus)(const D3DCLIPSTATUS9 *clip_status) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetClipStatus");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetClipStatus)(D3DCLIPSTATUS9* pClipStatus) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetClipStatus");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetTexture)(DWORD Stage, IDirect3DBaseTexture9** ppTexture) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetTexture");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetTexture)(DWORD Stage, IDirect3DBaseTexture9* pTexture) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetTexture");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetTextureStageState");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetTextureStageState");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetSamplerState");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetSamplerState");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, ValidateDevice)(DWORD* pNumPasses) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::ValidateDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPaletteEntries)(UINT palette_idx, const PALETTEENTRY *entries) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetPaletteEntries");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPaletteEntries)(UINT PaletteNumber,PALETTEENTRY* pEntries) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetPaletteEntries");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetCurrentTexturePalette)(UINT PaletteNumber) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetCurrentTexturePalette");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetCurrentTexturePalette)(UINT *PaletteNumber) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetCurrentTexturePalette");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetScissorRect)(const RECT *rect) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetScissorRect");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetScissorRect)(RECT* pRect) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetScissorRect");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetSoftwareVertexProcessing)(WINBOOL bSoftware) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetSoftwareVertexProcessing");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(WINBOOL, GetSoftwareVertexProcessing)() override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetSoftwareVertexProcessing");
        return (WINBOOL)0;
    }
    STDMETHOD_(HRESULT, SetNPatchMode)(float nSegments) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetNPatchMode");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(float, GetNPatchMode)() override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetNPatchMode");
        return (float)0;
    }
    STDMETHOD_(HRESULT, DrawPrimitive)(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::DrawPrimitive");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, DrawIndexedPrimitive)(D3DPRIMITIVETYPE, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::DrawIndexedPrimitive");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, DrawPrimitiveUP)(D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void *data, UINT stride) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::DrawPrimitiveUP");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE primitive_type, UINT min_vertex_idx, UINT vertex_count, UINT primitive_count, const void *index_data, D3DFORMAT index_format, const void *data, UINT stride) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::DrawIndexedPrimitiveUP");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, ProcessVertices)(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::ProcessVertices");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreateVertexDeclaration)(const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **declaration) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateVertexDeclaration");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetVertexDeclaration)(IDirect3DVertexDeclaration9* pDecl) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetVertexDeclaration");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetVertexDeclaration)(IDirect3DVertexDeclaration9** ppDecl) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetVertexDeclaration");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetFVF)(DWORD FVF) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetFVF");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetFVF)(DWORD* pFVF) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetFVF");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreateVertexShader)(const DWORD *byte_code, IDirect3DVertexShader9 **shader) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateVertexShader");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetVertexShader)(IDirect3DVertexShader9* pShader) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetVertexShader");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetVertexShader)(IDirect3DVertexShader9** ppShader) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetVertexShader");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetVertexShaderConstantF)(UINT reg_idx, const float *data, UINT count) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetVertexShaderConstantF");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetVertexShaderConstantF)(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetVertexShaderConstantF");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetVertexShaderConstantI)(UINT reg_idx, const int *data, UINT count) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetVertexShaderConstantI");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetVertexShaderConstantI)(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetVertexShaderConstantI");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetVertexShaderConstantB)(UINT reg_idx, const WINBOOL *data, UINT count) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetVertexShaderConstantB");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetVertexShaderConstantB)(UINT StartRegister, WINBOOL* pConstantData, UINT BoolCount) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetVertexShaderConstantB");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetStreamSource");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* OffsetInBytes, UINT* pStride) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetStreamSource");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetStreamSourceFreq)(UINT StreamNumber, UINT Divider) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetStreamSourceFreq");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetStreamSourceFreq)(UINT StreamNumber, UINT* Divider) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetStreamSourceFreq");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetIndices)(IDirect3DIndexBuffer9* pIndexData) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetIndices");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetIndices)(IDirect3DIndexBuffer9** ppIndexData) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetIndices");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreatePixelShader)(const DWORD *byte_code, IDirect3DPixelShader9 **shader) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreatePixelShader");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPixelShader)(IDirect3DPixelShader9* pShader) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetPixelShader");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPixelShader)(IDirect3DPixelShader9** ppShader) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetPixelShader");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPixelShaderConstantF)(UINT reg_idx, const float *data, UINT count) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetPixelShaderConstantF");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPixelShaderConstantF)(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetPixelShaderConstantF");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPixelShaderConstantI)(UINT reg_idx, const int *data, UINT count) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetPixelShaderConstantI");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPixelShaderConstantI)(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetPixelShaderConstantI");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPixelShaderConstantB)(UINT reg_idx, const WINBOOL *data, UINT count) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::SetPixelShaderConstantB");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPixelShaderConstantB)(UINT StartRegister, WINBOOL* pConstantData, UINT BoolCount) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::GetPixelShaderConstantB");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, DrawRectPatch)(UINT handle, const float *segment_count, const D3DRECTPATCH_INFO *patch_info) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::DrawRectPatch");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, DrawTriPatch)(UINT handle, const float *segment_count, const D3DTRIPATCH_INFO *patch_info) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::DrawTriPatch");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, DeletePatch)(UINT Handle) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::DeletePatch");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, CreateQuery)(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override
    {
        Deko9_ReportUnsupported("IDirect3DDevice9::CreateQuery");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DSwapChain9 : public IDirect3DSwapChain9
{
    STDMETHOD_(HRESULT, Present)(const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DSwapChain9::Present");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetFrontBufferData)(struct IDirect3DSurface9 *pDestSurface) override
    {
        Deko9_ReportUnsupported("IDirect3DSwapChain9::GetFrontBufferData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetBackBuffer)(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, struct IDirect3DSurface9 **ppBackBuffer) override
    {
        Deko9_ReportUnsupported("IDirect3DSwapChain9::GetBackBuffer");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetRasterStatus)(D3DRASTER_STATUS *pRasterStatus) override
    {
        Deko9_ReportUnsupported("IDirect3DSwapChain9::GetRasterStatus");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDisplayMode)(D3DDISPLAYMODE *pMode) override
    {
        Deko9_ReportUnsupported("IDirect3DSwapChain9::GetDisplayMode");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9 **ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DSwapChain9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPresentParameters)(D3DPRESENT_PARAMETERS *pPresentationParameters) override
    {
        Deko9_ReportUnsupported("IDirect3DSwapChain9::GetPresentParameters");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DSurface9 : public IDirect3DSurface9
{
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9** ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPrivateData)(REFGUID guid, const void *data, DWORD data_size, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::SetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::GetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, FreePrivateData)(REFGUID refguid) override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::FreePrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::SetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetPriority)() override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::GetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(void, PreLoad)() override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::PreLoad");
    }
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::GetType");
        return (D3DRESOURCETYPE)0;
    }
    STDMETHOD_(HRESULT, GetContainer)(REFIID riid, void** ppContainer) override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::GetContainer");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDesc)(D3DSURFACE_DESC* pDesc) override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::GetDesc");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, LockRect)(D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::LockRect");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, UnlockRect)() override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::UnlockRect");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDC)(HDC* phdc) override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::GetDC");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, ReleaseDC)(HDC hdc) override
    {
        Deko9_ReportUnsupported("IDirect3DSurface9::ReleaseDC");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DVolume9 : public IDirect3DVolume9
{
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9** ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DVolume9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPrivateData)(REFGUID guid, const void *data, DWORD data_size, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DVolume9::SetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override
    {
        Deko9_ReportUnsupported("IDirect3DVolume9::GetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, FreePrivateData)(REFGUID refguid) override
    {
        Deko9_ReportUnsupported("IDirect3DVolume9::FreePrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetContainer)(REFIID riid, void** ppContainer) override
    {
        Deko9_ReportUnsupported("IDirect3DVolume9::GetContainer");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDesc)(D3DVOLUME_DESC* pDesc) override
    {
        Deko9_ReportUnsupported("IDirect3DVolume9::GetDesc");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, LockBox)(D3DLOCKED_BOX *locked_box, const D3DBOX *box, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DVolume9::LockBox");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, UnlockBox)() override
    {
        Deko9_ReportUnsupported("IDirect3DVolume9::UnlockBox");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DTexture9 : public IDirect3DTexture9
{
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9** ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPrivateData)(REFGUID guid, const void *data, DWORD data_size, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::SetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::GetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, FreePrivateData)(REFGUID refguid) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::FreePrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::SetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetPriority)() override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::GetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(void, PreLoad)() override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::PreLoad");
    }
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::GetType");
        return (D3DRESOURCETYPE)0;
    }
    STDMETHOD_(DWORD, SetLOD)(DWORD LODNew) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::SetLOD");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetLOD)() override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::GetLOD");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetLevelCount)() override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::GetLevelCount");
        return (DWORD)0;
    }
    STDMETHOD_(HRESULT, SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE FilterType) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::SetAutoGenFilterType");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::GetAutoGenFilterType");
        return (D3DTEXTUREFILTERTYPE)0;
    }
    STDMETHOD_(void, GenerateMipSubLevels)() override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::GenerateMipSubLevels");
    }
    STDMETHOD_(HRESULT, GetLevelDesc)(UINT Level, D3DSURFACE_DESC* pDesc) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::GetLevelDesc");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetSurfaceLevel)(UINT Level, IDirect3DSurface9** ppSurfaceLevel) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::GetSurfaceLevel");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, LockRect)(UINT level, D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::LockRect");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, UnlockRect)(UINT Level) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::UnlockRect");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, AddDirtyRect)(const RECT *dirty_rect) override
    {
        Deko9_ReportUnsupported("IDirect3DTexture9::AddDirtyRect");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DCubeTexture9 : public IDirect3DCubeTexture9
{
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9** ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPrivateData)(REFGUID guid, const void *data, DWORD data_size, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::SetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::GetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, FreePrivateData)(REFGUID refguid) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::FreePrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::SetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetPriority)() override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::GetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(void, PreLoad)() override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::PreLoad");
    }
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::GetType");
        return (D3DRESOURCETYPE)0;
    }
    STDMETHOD_(DWORD, SetLOD)(DWORD LODNew) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::SetLOD");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetLOD)() override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::GetLOD");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetLevelCount)() override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::GetLevelCount");
        return (DWORD)0;
    }
    STDMETHOD_(HRESULT, SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE FilterType) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::SetAutoGenFilterType");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::GetAutoGenFilterType");
        return (D3DTEXTUREFILTERTYPE)0;
    }
    STDMETHOD_(void, GenerateMipSubLevels)() override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::GenerateMipSubLevels");
    }
    STDMETHOD_(HRESULT, GetLevelDesc)(UINT Level,D3DSURFACE_DESC* pDesc) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::GetLevelDesc");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetCubeMapSurface)(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9** ppCubeMapSurface) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::GetCubeMapSurface");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, LockRect)(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::LockRect");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, UnlockRect)(D3DCUBEMAP_FACES FaceType, UINT Level) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::UnlockRect");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, AddDirtyRect)(D3DCUBEMAP_FACES face, const RECT *dirty_rect) override
    {
        Deko9_ReportUnsupported("IDirect3DCubeTexture9::AddDirtyRect");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DVolumeTexture9 : public IDirect3DVolumeTexture9
{
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9** ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPrivateData)(REFGUID guid, const void *data, DWORD data_size, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::SetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::GetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, FreePrivateData)(REFGUID refguid) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::FreePrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::SetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetPriority)() override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::GetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(void, PreLoad)() override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::PreLoad");
    }
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::GetType");
        return (D3DRESOURCETYPE)0;
    }
    STDMETHOD_(DWORD, SetLOD)(DWORD LODNew) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::SetLOD");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetLOD)() override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::GetLOD");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetLevelCount)() override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::GetLevelCount");
        return (DWORD)0;
    }
    STDMETHOD_(HRESULT, SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE FilterType) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::SetAutoGenFilterType");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::GetAutoGenFilterType");
        return (D3DTEXTUREFILTERTYPE)0;
    }
    STDMETHOD_(void, GenerateMipSubLevels)() override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::GenerateMipSubLevels");
    }
    STDMETHOD_(HRESULT, GetLevelDesc)(UINT Level, D3DVOLUME_DESC *pDesc) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::GetLevelDesc");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetVolumeLevel)(UINT Level, IDirect3DVolume9** ppVolumeLevel) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::GetVolumeLevel");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, LockBox)(UINT level, D3DLOCKED_BOX *locked_box, const D3DBOX *box, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::LockBox");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, UnlockBox)(UINT Level) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::UnlockBox");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, AddDirtyBox)(const D3DBOX *dirty_box) override
    {
        Deko9_ReportUnsupported("IDirect3DVolumeTexture9::AddDirtyBox");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DVertexBuffer9 : public IDirect3DVertexBuffer9
{
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9** ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DVertexBuffer9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPrivateData)(REFGUID guid, const void *data, DWORD data_size, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DVertexBuffer9::SetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override
    {
        Deko9_ReportUnsupported("IDirect3DVertexBuffer9::GetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, FreePrivateData)(REFGUID refguid) override
    {
        Deko9_ReportUnsupported("IDirect3DVertexBuffer9::FreePrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override
    {
        Deko9_ReportUnsupported("IDirect3DVertexBuffer9::SetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetPriority)() override
    {
        Deko9_ReportUnsupported("IDirect3DVertexBuffer9::GetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(void, PreLoad)() override
    {
        Deko9_ReportUnsupported("IDirect3DVertexBuffer9::PreLoad");
    }
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override
    {
        Deko9_ReportUnsupported("IDirect3DVertexBuffer9::GetType");
        return (D3DRESOURCETYPE)0;
    }
    STDMETHOD_(HRESULT, Lock)(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override
    {
        Deko9_ReportUnsupported("IDirect3DVertexBuffer9::Lock");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, Unlock)() override
    {
        Deko9_ReportUnsupported("IDirect3DVertexBuffer9::Unlock");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDesc)(D3DVERTEXBUFFER_DESC* pDesc) override
    {
        Deko9_ReportUnsupported("IDirect3DVertexBuffer9::GetDesc");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DIndexBuffer9 : public IDirect3DIndexBuffer9
{
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9** ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DIndexBuffer9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, SetPrivateData)(REFGUID guid, const void *data, DWORD data_size, DWORD flags) override
    {
        Deko9_ReportUnsupported("IDirect3DIndexBuffer9::SetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override
    {
        Deko9_ReportUnsupported("IDirect3DIndexBuffer9::GetPrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, FreePrivateData)(REFGUID refguid) override
    {
        Deko9_ReportUnsupported("IDirect3DIndexBuffer9::FreePrivateData");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override
    {
        Deko9_ReportUnsupported("IDirect3DIndexBuffer9::SetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(DWORD, GetPriority)() override
    {
        Deko9_ReportUnsupported("IDirect3DIndexBuffer9::GetPriority");
        return (DWORD)0;
    }
    STDMETHOD_(void, PreLoad)() override
    {
        Deko9_ReportUnsupported("IDirect3DIndexBuffer9::PreLoad");
    }
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override
    {
        Deko9_ReportUnsupported("IDirect3DIndexBuffer9::GetType");
        return (D3DRESOURCETYPE)0;
    }
    STDMETHOD_(HRESULT, Lock)(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override
    {
        Deko9_ReportUnsupported("IDirect3DIndexBuffer9::Lock");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, Unlock)() override
    {
        Deko9_ReportUnsupported("IDirect3DIndexBuffer9::Unlock");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDesc)(D3DINDEXBUFFER_DESC* pDesc) override
    {
        Deko9_ReportUnsupported("IDirect3DIndexBuffer9::GetDesc");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DVertexDeclaration9 : public IDirect3DVertexDeclaration9
{
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9** ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DVertexDeclaration9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetDeclaration)(D3DVERTEXELEMENT9*, UINT* pNumElements) override
    {
        Deko9_ReportUnsupported("IDirect3DVertexDeclaration9::GetDeclaration");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DVertexShader9 : public IDirect3DVertexShader9
{
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9** ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DVertexShader9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetFunction)(void*, UINT* pSizeOfData) override
    {
        Deko9_ReportUnsupported("IDirect3DVertexShader9::GetFunction");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DPixelShader9 : public IDirect3DPixelShader9
{
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9** ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DPixelShader9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetFunction)(void*, UINT* pSizeOfData) override
    {
        Deko9_ReportUnsupported("IDirect3DPixelShader9::GetFunction");
        return D3DERR_INVALIDCALL;
    }
};

struct Deko9Default_IDirect3DQuery9 : public IDirect3DQuery9
{
    STDMETHOD_(HRESULT, GetDevice)(struct IDirect3DDevice9** ppDevice) override
    {
        Deko9_ReportUnsupported("IDirect3DQuery9::GetDevice");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(D3DQUERYTYPE, GetType)() override
    {
        Deko9_ReportUnsupported("IDirect3DQuery9::GetType");
        return (D3DQUERYTYPE)0;
    }
    STDMETHOD_(DWORD, GetDataSize)() override
    {
        Deko9_ReportUnsupported("IDirect3DQuery9::GetDataSize");
        return (DWORD)0;
    }
    STDMETHOD_(HRESULT, Issue)(DWORD dwIssueFlags) override
    {
        Deko9_ReportUnsupported("IDirect3DQuery9::Issue");
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD_(HRESULT, GetData)(void* pData, DWORD dwSize, DWORD dwGetDataFlags) override
    {
        Deko9_ReportUnsupported("IDirect3DQuery9::GetData");
        return D3DERR_INVALIDCALL;
    }
};

#pragma GCC diagnostic pop
