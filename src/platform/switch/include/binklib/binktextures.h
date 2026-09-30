// Switch Bink texture-set type shim (see binklib/bink.h shim header).
//
// Mirrors the deps/binklib/binktextures.h DX9 shape with the D3D9 texture
// types supplied by the vendored deps/d3d9-headers.  The texture-set implementation
// functions are not declared: Bink playback is excluded from the Horizon
// closure and must fail loudly if ever reached.
#pragma once

#include <binklib/bink.h>

#ifndef __BINKTEXTURESH__
#define __BINKTEXTURESH__

struct IDirect3DTexture9;
typedef IDirect3DTexture9 *LPDIRECT3DTEXTURE9;

struct BINKFRAMETEXTURES
{
    BINKU32 Ysize;
    BINKU32 cRsize;
    BINKU32 cBsize;
    BINKU32 Asize;
    LPDIRECT3DTEXTURE9 Ytexture;
    LPDIRECT3DTEXTURE9 cRtexture;
    LPDIRECT3DTEXTURE9 cBtexture;
    LPDIRECT3DTEXTURE9 Atexture;
};

struct BINKTEXTURESET
{
    BINKFRAMEBUFFERS bink_buffers;
    BINKFRAMETEXTURES textures[BINKMAXFRAMEBUFFERS];
};

#endif // __BINKTEXTURESH__
