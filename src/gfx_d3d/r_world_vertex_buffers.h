#pragma once

#include "r_bsp.h"
#include <cstdint>

// Retail wire walkers retain vertices rather than constructing renderer
// resources on the DB thread. Complete that handoff when a fastfile world is
// published to the renderer. Native DB-loaded buffers remain untouched.
inline bool R_MaterializeWorldVertexBuffers(
    GfxWorld *world,
    void (*create)(IDirect3DVertexBuffer9 **, int *, uint32_t))
{
    if (!world || !create || world->vertexCount > UINT32_MAX / sizeof(GfxWorldVertex) ||
        (world->vertexCount && !world->vd.vertices) ||
        (world->vertexLayerDataSize && !world->vld.data))
        return false;

    if (world->vertexCount && !world->vd.worldVb)
        create(&world->vd.worldVb, reinterpret_cast<int *>(world->vd.vertices),
               world->vertexCount * sizeof(GfxWorldVertex));
    if (world->vertexLayerDataSize && !world->vld.layerVb)
        create(&world->vld.layerVb, reinterpret_cast<int *>(world->vld.data),
               world->vertexLayerDataSize);

    return (!world->vertexCount || world->vd.worldVb) &&
           (!world->vertexLayerDataSize || world->vld.layerVb);
}
