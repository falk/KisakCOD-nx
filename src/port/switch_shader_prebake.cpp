// Zone-load shader variant prebake (deko3d renderer). After a zone's assets
// are in, every pass of every registered material is described to the
// device, which builds each compiled variant those passes can select (depth
// compare, early-Z, instanced static models) before the level draws: a draw
// then never translates or compiles with the device lock held.
//
// Runs on the database thread at zone-load end. The compile work runs on the
// database thread and on one thread pinned to frame worker 0's core at the
// lowest priority, which only takes that core's idle time; the main thread
// (loading screen) and the audio/back-end core are untouched. The database
// thread returns once every variant is installed, which keeps the level from
// starting before its variants exist.
// r_deko9Prebake 0 skips it: each variant then builds at its first draw.

#include "switch_shader_prebake.h"

#include <database/database.h>
#include <deko9/deko9_native.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_state.h>
#include <platform/switch/switch_thread.h>
#include <qcommon/qcommon.h>

#include <vector>

static_assert(kPrebakeAtestDisable == GFXS0_ATEST_DISABLE, "prebake alpha-test bit must match the state bits");

// r_material_override.cpp: the technique set the current feature dvars remap
// techSet to (itself when none), computed without writing the remap.
MaterialTechniqueSet *__cdecl Material_RuntimeRemapTarget(MaterialTechniqueSet *techSet);

namespace
{
// Horizon's lowest thread priority: below the frame workers (0x3B).
constexpr int kPrebakePriority = 0x3F;

struct PrebakeCollect
{
    std::vector<Deko9PassVariants> passes;
    bool hardwareShadowmap;
    bool instancing;
};

void R_PrebakeCollectPasses(PrebakeCollect *collect, const Material *material, const MaterialTechniqueSet *techSet)
{
    R_ForEachPrebakePass(material, techSet, collect->hardwareShadowmap, collect->instancing,
                         [collect](const PrebakePassInputs &in) {
                             Deko9PassVariants pass{};
                             pass.vs = in.pass->vertexShader ? in.pass->vertexShader->prog.vs : nullptr;
                             pass.ps = in.pass->pixelShader ? in.pass->pixelShader->prog.ps : nullptr;
                             if (!pass.vs && !pass.ps)
                                 return;
                             pass.depthSamplerMask = in.depthSamplerMask;
                             pass.alphaTest = in.alphaTest ? 1 : 0;
                             pass.instanceRegCount = (uint8_t)in.instanceRegCount;
                             for (uint32_t i = 0; i < in.instanceRegCount; ++i)
                                 pass.instanceRegs[i] = in.instanceRegs[i];
                             collect->passes.push_back(pass);
                         });
}

// A material draws its own technique set until the main thread's override
// pass remaps it (a newly loaded set starts unmapped), then the remap target:
// both are described.
void R_PrebakeCollectMaterial(XAssetHeader header, void *data)
{
    PrebakeCollect *collect = static_cast<PrebakeCollect *>(data);
    const Material *material = header.material;
    if (!material || !material->techniqueSet)
        return;
    R_PrebakeCollectPasses(collect, material, material->techniqueSet);
    const MaterialTechniqueSet *remapped = Material_RuntimeRemapTarget(material->techniqueSet);
    if (remapped && remapped != material->techniqueSet)
        R_PrebakeCollectPasses(collect, material, remapped);
}
} // namespace

bool R_StaticModelInstancingEnabled()
{
    return r_portDebugChecks && !r_portDebugChecks->current.enabled;
}

static void R_PrebakeZoneVariants(const char *zoneName)
{
    PrebakeCollect collect;
    collect.hardwareShadowmap = gfxMetrics.hasHardwareShadowmap != 0;
    collect.instancing = R_StaticModelInstancingEnabled();
    DB_EnumXAssets(ASSET_TYPE_MATERIAL, R_PrebakeCollectMaterial, &collect, true);
    Deko9PrebakeResult result{};
    if (!Deko9_PrebakeVariants(dx.device, collect.passes.data(), (uint32_t)collect.passes.size(),
                               Switch_WorkerThreadCpuId(0), kPrebakePriority, &result))
    {
        Com_PrintError(8, "ERROR: shader variant prebake rejected %u passes of zone '%s'\n",
                       (uint32_t)collect.passes.size(), zoneName);
        return;
    }
    Com_Printf(8,
               "KILLHOUSE_LOAD_PREBAKE zone=%s passes=%u planned=%u present=%u built=%u pack_hits=%u failed=%u "
               "threads=%u plan_ms=%llu build_ms=%llu wall_ms=%llu\n",
               zoneName, result.passes, result.planned, result.present, result.built, result.packHits, result.failed,
               result.threads, (unsigned long long)(result.planUs / 1000), (unsigned long long)(result.buildUs / 1000),
               (unsigned long long)(result.wallUs / 1000));
}

void R_PrebakeShaderVariants(const char *zoneName)
{
    if (!dx.device)
        return;
    const bool enabled = r_deko9Prebake ? r_deko9Prebake->current.enabled : DEKO9_DEFAULT_PREBAKE;
    if (!Deko9_PrebakeIfEnabled(enabled, [zoneName] { R_PrebakeZoneVariants(zoneName); }))
        Com_Printf(8, "KILLHOUSE_LOAD_PREBAKE_OFF zone=%s r_deko9Prebake=0: variants build at first draw\n",
                   zoneName);
}
