#pragma once

#include "../universal/com_files.h"
#include "db_retail_zone.h"

// Widens one real GfxImage body read live from `reader`, allocating through
// the zone's real native arena and registering through the zone's real
// asset table.  Pixel upload stays null (the renderer resource seam, G3);
// this only widens the scalar header + optional GfxImageLoadDef shape.
//
// Ported from switch_sp_bootstrap.cpp's hardware-proven WidenImage
// (PASS:RETAIL_MENU_GFX_PROOF against the real retail ui.ff, images=4),
// adapted to allocate through RetailZoneLoadSessionAlloc and register
// through RetailZoneLoadSessionRegister instead of Hunk_Alloc/
// DB_RegisterImage directly, for the same zone-lifetime reason as
// db_retail_decode_techniqueset.cpp.  Unlike the bootstrap's WidenImage,
// this does not yet track the Android per-zone pooled-image identity
// (DB_AddXAsset pseudo-block 15) needed to resolve a later texture-def
// alias back to an inline image widened earlier in the same zone -- that
// bookkeeping belongs with Material's texture-table walk, the next piece,
// not with a standalone top-level Image asset.
bool RetailWalkLiveLoadImage(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                              uint32_t headerRef, XAssetHeader *header,
                              bool strict = false);

// The widening-only half of RetailWalkLiveLoadImage, split out so
// db_retail_decode_material.cpp can widen a texture def's already-decoded
// FsRetailFastfileTextureDef::inlineImage (FS_ReadRetailFastfileMaterial
// reads that wire body itself, so calling FS_ReadRetailFastfileImage again
// here would desynchronize the FS stream) without duplicating the
// scalar-field/loadDef mapping.
//
// `fallbackName` recovers the image name for the one case our reader cannot
// resolve on its own: a serialized name slot that is not the inline token but
// a block-4 back/forward reference into the linker's own block-4 image.  Our
// reader only mirrors the block-4 bytes it actually walks, so its block-4
// offsets are not byte-identical to the linker's and such a reference lands on
// unwritten memory (an empty or garbage name).  For a single-texture material
// the caller passes the material name, which CoD4's UI assets share with their
// colorMap image; it is accepted only if images/<name>.iwi exists on disk and
// its header dimensions match the wire loadDef's, so a wrong guess is rejected
// rather than silently bound.
//
// `strict` is the strict acceptance mode: exact typed aliases/inserts or loud
// failure (no synthesis, no hardcoded identities, no white/$default
// substitution, no IWI-filename guessing). Cube/volume forms widen
// exactly like 2D (registry data, never downgraded); only texture UPLOAD
// stays gated at the device seam, and only the meaningless NONE/INVALID
// forms fail here. The tolerant default preserves the menu boot path.
bool RetailWidenImageFromWire(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                              const FsRetailFastfileImage &wire, XAssetHeader *header,
                              const char *fallbackName = nullptr, bool strict = false);
