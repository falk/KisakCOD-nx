#include <universal/q_shared.h>
#include "src/gfx_d3d/r_world_vertex_buffers.h"
#include <cstdio>

void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}

namespace {
unsigned calls;
int *sources[2];
uint32_t sizes[2];
void Create(IDirect3DVertexBuffer9 **out, int *source, uint32_t size)
{
    if (calls >= 2) return;
    sources[calls] = source;
    sizes[calls] = size;
    *out = reinterpret_cast<IDirect3DVertexBuffer9 *>(uintptr_t(0x1000 + ++calls));
}
void FailCreate(IDirect3DVertexBuffer9 **, int *, uint32_t) {}
}

int main()
{
    GfxWorld world{};
    GfxWorldVertex vertices[4]{};
    unsigned char layers[12]{};
    world.vertexCount = 4;
    world.vd.vertices = vertices;
    world.vertexLayerDataSize = sizeof(layers);
    world.vld.data = layers;
    if (!R_MaterializeWorldVertexBuffers(&world, Create) || calls != 2 ||
        sources[0] != reinterpret_cast<int *>(vertices) || sizes[0] != sizeof(vertices) ||
        sources[1] != reinterpret_cast<int *>(layers) || sizes[1] != sizeof(layers)) return 1;
    const auto resident = world.vd.worldVb;
    if (!R_MaterializeWorldVertexBuffers(&world, Create) || calls != 2 ||
        world.vd.worldVb != resident) return 1;
    // R_ReleaseWorld clears both default-pool handles on device loss.
    // Re-publication must upload the retained streams again, exactly once.
    calls = 0;
    world.vd.worldVb = nullptr;
    world.vld.layerVb = nullptr;
    if (!R_MaterializeWorldVertexBuffers(&world, Create) || calls != 2 ||
        sizes[0] != sizeof(vertices) || sizes[1] != sizeof(layers)) return 1;
    calls = 0;
    world.vld.layerVb = nullptr;
    if (!R_MaterializeWorldVertexBuffers(&world, Create) || calls != 1 ||
        sources[0] != reinterpret_cast<int *>(layers) || world.vd.worldVb != resident) return 1;
    calls = 0;
    world.vd.worldVb = nullptr;
    world.vld.layerVb = nullptr;
    world.vld.data = nullptr;
    if (R_MaterializeWorldVertexBuffers(&world, Create) || calls) return 1;
    world.vld.data = layers;
    world.vd.vertices = nullptr;
    if (R_MaterializeWorldVertexBuffers(&world, Create) || calls) return 1;
    world.vd.vertices = vertices;
    world.vertexCount = UINT32_MAX;
    if (R_MaterializeWorldVertexBuffers(&world, Create) || calls) return 1;
    world.vertexCount = 4;
    if (R_MaterializeWorldVertexBuffers(&world, FailCreate)) return 1;
    world = {};
    if (!R_MaterializeWorldVertexBuffers(&world, Create) || calls ||
        R_MaterializeWorldVertexBuffers(nullptr, Create)) return 1;
    std::puts("PASS:WORLD_VERTEX_BUFFERS retained_data=1 resident_preserved=1 reload=1 partial=1 invalid_atomic=1 upload_failure=1 empty=1");
}
