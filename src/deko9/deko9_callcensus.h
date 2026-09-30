#pragma once

// Per-D3D9-entry-point call census (engine dvar r_deko9Census; native API
// Deko9_SetCensus, deko9_native.h). MEASUREMENT ONLY: nothing here changes
// what any call does. Off (default): each gated call pays one atomic load
// and a predictable branch, nothing recorded.
//
// On: every gated D3D9 method and native fast-path call records one entry
// (deko9_internal.h CensusScope) into Device::m_census[id] -- calls, CPU ns
// spent in the call (armGetSystemTick deltas taken directly, not sampled:
// the existing per-draw ScopedNs timing in deko9_draw.cpp already pays two
// tick reads per draw unconditionally, so the same cost per gated call here
// is negligible), and bytes deko9 itself copied (upload memcpys, lock
// renames, MANAGED readback-then-copy -- never the app's own write into a
// mapped pointer, which deko9 cannot see). Device::ReportCensus
// (deko9_callcensus.cpp) logs a "DEKO9 calls" block every 60 presented frames,
// sorted by ns descending, plus the top 10 uploaded textures
// (Device::CensusTexture) and top dynamic VB/IB buffers by role
// (Device::CensusBufferRole).

#include <cstdint>

namespace deko9
{

enum CensusId : uint32_t
{
    Census_DrawIndexedPrimitive,
    Census_DrawPrimitive,
    Census_DrawPrimitiveUP,
    Census_DrawIndexedPrimitiveUP,
    Census_SetTexture,
    Census_SetSamplerState,
    Census_SetRenderState,
    Census_SetVertexShader,
    Census_SetPixelShader,
    Census_SetVertexShaderConstantF,
    Census_SetPixelShaderConstantF,
    Census_SetStreamSource,
    Census_SetIndices,
    Census_SetVertexDeclaration,
    Census_SetRenderTarget,
    Census_SetDepthStencilSurface,
    Census_Clear,
    Census_StretchRect,
    Census_UpdateTexture,
    Census_UpdateSurface,
    Census_GetRenderTargetData,
    Census_LockTexture2D,   // Texture2D::LockRect/UnlockRect
    Census_LockCubeTexture, // CubeTexture::LockRect/UnlockRect
    Census_LockVolumeTexture, // VolumeTexture::LockBox/UnlockBox
    Census_LockSurface,     // standalone Surface::LockRect/UnlockRect
    Census_LockVbDiscard,
    Census_LockVbNoOverwrite,
    Census_LockVbNone,
    Census_LockVbReadOnly,
    Census_LockIbDiscard,
    Census_LockIbNoOverwrite,
    Census_LockIbNone,
    Census_LockIbReadOnly,
    Census_QueryIssue,
    Census_QueryGetData,
    Census_BeginScene,
    Census_EndScene,
    Census_Present,
    Census_NativeSetTexture,        // Deko9_SetTexture (fast path)
    Census_NativeSetSamplerPacked,  // Deko9_SetSamplerPacked (fast path)
    Census_NativeAddInstance,       // Deko9_AddInstance (fast path)
    Census_NativeDrawIndexedInstances, // Deko9_DrawIndexedInstances (fast path)
    Census_NativeDrawIndexedRanges,    // Deko9_DrawIndexedRanges (static index ranges)
    Census_Count
};

// Names in enum order, defined in deko9_device.cpp.
extern const char *const kCensusNames[Census_Count];

struct CensusEntry
{
    uint64_t calls = 0;
    uint64_t bytes = 0; // deko9's own CPU-side copies, see file comment
    uint64_t ns = 0;    // wall time inside the call (armGetSystemTick delta)
};

} // namespace deko9
