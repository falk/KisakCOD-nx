#pragma once

#include "db_retail_wire.h"
#include "../universal/com_files.h"
#include "../gfx_d3d/r_material.h"
#include <cstring>

// Native widening only: the existing Android wire walker supplies the root
// fields and canonical array references. Never interpret its nested image as
// the water_t arm of MaterialTextureDef's union.
inline bool RetailWidenWaterFieldsFromWire(const FsRetailFastfileWater &wire,
                                         const RetailWireBlocks &blocks,
                                         water_t *water, GfxImage *image)
{
    if (!water || !wire.reference || !wire.m || wire.m != wire.n ||
        wire.m > 64 || (wire.m & (wire.m - 1u)) || !water->H0 || !water->wTerm)
        return false;
    const uint32_t cells = wire.m * wire.n;
    RetailWireToken h0{}, wTerm{};
    if (!RetailWireTokenDecode(&blocks, {wire.h0Ref}, cells * sizeof(complex_s),
                               1u << 4, &h0) || h0.kind != RETAIL_WIRE_TOKEN_OFFSET ||
        !RetailWireTokenDecode(&blocks, {wire.wTermRef}, cells * sizeof(float),
                               1u << 4, &wTerm) || wTerm.kind != RETAIL_WIRE_TOKEN_OFFSET)
        return false;
    water_t result{};
    result.writable.floatTime = wire.floatTime;
    result.H0 = water->H0;
    result.wTerm = water->wTerm;
    result.M = static_cast<int>(wire.m);
    result.N = static_cast<int>(wire.n);
    static_assert(offsetof(water_t, codeConstant) + sizeof(result.codeConstant) -
                  offsetof(water_t, Lx) == sizeof(wire.parameters));
    std::memcpy(&result.Lx, wire.parameters, sizeof(wire.parameters));
    result.image = image;
    if (!RetailWireTokenRead(&blocks, {wire.h0Ref}, cells * sizeof(complex_s),
                             1u << 4, result.H0) ||
        !RetailWireTokenRead(&blocks, {wire.wTermRef}, cells * sizeof(float),
                             1u << 4, result.wTerm))
        return false;
    *water = result;
    return true;
}
