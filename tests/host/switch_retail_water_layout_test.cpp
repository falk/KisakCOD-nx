#include "src/database/db_retail_decode_water.h"
#include "src/gfx_d3d/r_image.h"
#include <cstdio>
#include <cstring>

void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}

int main()
{
    unsigned char bytes[64]{};
    const complex_s originalH0[4] = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};
    const float originalWTerm[4] = {9, 10, 11, 12};
    std::memcpy(bytes + 4, originalH0, sizeof(originalH0));
    std::memcpy(bytes + 36, originalWTerm, sizeof(originalWTerm));
    XZoneMemory zone{};
    zone.blocks[4] = {bytes, sizeof(bytes)};
    RetailWireBlocks blocks{};
    if (!RetailWireBlocksInit(&blocks, &zone)) return 1;
    FsRetailFastfileWater wire{};
    wire.reference = 0x40000001;
    wire.h0Ref = 0x40000005;
    wire.wTermRef = 0x40000025;
    wire.m = wire.n = 2;
    wire.floatTime = 0.25f;
    for (unsigned i = 0; i < 11; ++i) wire.parameters[i] = static_cast<float>(20 + i);
    complex_s h0[4]{};
    float wTerm[4]{};
    water_t water{};
    water.H0 = h0;
    water.wTerm = wTerm;
    GfxImage image{};
    image.mapType = MAPTYPE_2D;
    MaterialTextureDef texture{};
    texture.semantic = 11;
    texture.u.water = &water;
    if (!RetailWidenWaterFieldsFromWire(wire, blocks, &water, &image) ||
        water.M != 2 || water.N != 2 || water.image != &image ||
        water.writable.floatTime != 0.25f || water.Lx != 20 || water.codeConstant[3] != 30 ||
        std::memcmp(h0, originalH0, sizeof(h0)) ||
        std::memcmp(wTerm, originalWTerm, sizeof(wTerm)) ||
        MaterialTextureImage(texture) != &image)
    {
        std::fprintf(stderr, "FAIL:RETAIL_WATER_LAYOUT stage=roundtrip\n");
        return 1;
    }
    water.writable.floatTime = 0.5f;
    if (image.mapType != MAPTYPE_2D) return 1;
    const water_t before = water;
    for (unsigned bad = 0; bad < 6; ++bad)
    {
        FsRetailFastfileWater invalid = wire;
        if (bad == 0) invalid.m = invalid.n = 0;
        if (bad == 1) invalid.m = invalid.n = 3;
        if (bad == 2) invalid.m = invalid.n = 128;
        if (bad == 3) invalid.n = 4;
        if (bad == 4) invalid.h0Ref = 0x40000040;
        if (bad == 5) invalid.wTermRef = 0x30000001;
        if (RetailWidenWaterFieldsFromWire(invalid, blocks, &water, &image) ||
            std::memcmp(&before, &water, sizeof(water)))
        {
            std::fprintf(stderr, "FAIL:RETAIL_WATER_LAYOUT stage=invalid case=%u\n", bad);
            return 1;
        }
    }
    std::puts("PASS:RETAIL_WATER_LAYOUT native=1 arrays=1 union=1 image_intact=1 invalid_dimensions=1 spans=1 atomic=1");
}
