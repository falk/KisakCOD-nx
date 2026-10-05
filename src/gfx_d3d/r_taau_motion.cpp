// r_taau per-object motion (see r_taau.h): the front end names where each
// animated surface was last frame, the back end turns the frame's moving
// surfaces into motion draws (deko9_taau.h TaauMotionDraw).
//
// Front end, R_SkinSceneDObjModels: every record of a DObj's surface list
// gets the previous frame's state in GfxModelSkinnedSurface's
// oldSkinnedCachedOffset (unused by the cached SP path):
//   skinned  previous byte offset + 1 in last frame's skin cache pool;
//   rigid    byte offset + 1 in this frame's surfsBuffer of a copy of the
//            previous placement;
//   0 = no previous state, -1 = no usable history (taau::MotionState).
// The skin cache has three pools, so last frame's vertices are intact while
// this frame's are written and the frame before that may still be on the GPU.
//
// Back end: the lit list's DObj surfaces with a previous state become draws
// whose matrices map their current and previous positions to this and the
// previous frame's unjittered clip space. Rigid surfaces that did not move
// are left to the depth reprojection (exact for them), except the viewmodel,
// whose fallback is to hold its screen position.

#include "r_taau.h"
#include "r_taau_motion.h"
#include "r_taau_motion_table.h"

#include <deko9/deko9_native.h>
#include <deko9/deko9_taau.h>
#include <database/database.h>
#include <universal/q_shared.h>

#include "r_buffers.h"
#include "r_dobj_skin.h"
#include "r_init.h"
#include "r_material.h"
#include "r_rendercmds.h"
#include "r_scene.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <vector>

namespace
{
using MotionTable = taau::MotionTable<GfxScaledPlacement>;
MotionTable s_table;
std::atomic_flag s_tableLock = ATOMIC_FLAG_INIT;

struct TableGuard
{
    TableGuard()
    {
        while (s_tableLock.test_and_set(std::memory_order_acquire))
        {
        }
    }
    ~TableGuard() { s_tableLock.clear(std::memory_order_release); }
};

int32_t Encode(taau::MotionState state, uint32_t offset)
{
    return state == taau::MotionState::Reject ? -1 : state == taau::MotionState::Prev ? (int32_t)offset + 1 : 0;
}

// Row-vector world matrix of a placement (R_GetWorldMatrixForModelSurf
// without the eye offset).
deko9::TaauMat PlacementMatrix(const GfxScaledPlacement &p)
{
    const double x = p.base.quat[0], y = p.base.quat[1], z = p.base.quat[2], w = p.base.quat[3], s = p.scale;
    deko9::TaauMat m{};
    m.m[0][0] = (1.0 - 2.0 * (y * y + z * z)) * s;
    m.m[0][1] = 2.0 * (x * y + z * w) * s;
    m.m[0][2] = 2.0 * (x * z - y * w) * s;
    m.m[1][0] = 2.0 * (x * y - z * w) * s;
    m.m[1][1] = (1.0 - 2.0 * (x * x + z * z)) * s;
    m.m[1][2] = 2.0 * (y * z + x * w) * s;
    m.m[2][0] = 2.0 * (x * z + y * w) * s;
    m.m[2][1] = 2.0 * (y * z - x * w) * s;
    m.m[2][2] = (1.0 - 2.0 * (x * x + y * y)) * s;
    m.m[3][0] = p.base.origin[0];
    m.m[3][1] = p.base.origin[1];
    m.m[3][2] = p.base.origin[2];
    m.m[3][3] = 1.0;
    return m;
}

deko9::TaauMat Translation(const float t[3])
{
    deko9::TaauMat m{};
    for (int i = 0; i < 4; ++i)
        m.m[i][i] = 1.0;
    m.m[3][0] = t[0];
    m.m[3][1] = t[1];
    m.m[3][2] = t[2];
    return m;
}

void Store(const deko9::TaauMat &m, float out[4][4])
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i][j] = (float)m.m[i][j];
}

bool SamePlacement(const GfxScaledPlacement &a, const GfxScaledPlacement &b)
{
    return !std::memcmp(&a, &b, sizeof(a));
}
} // namespace

void R_TaauNoteDObj(const GfxSceneEntity *sceneEnt, const DObj_s *obj, void *surfs, uint32_t surfCount)
{
    // Walk the records as R_SkinSceneDObjModels laid them out.
    const bool active = R_TaauActive();
    uint64_t signature = 0;
    int32_t skinBase = -1;
    GfxScaledPlacement rigid[MotionTable::kMaxRigid];
    uint32_t rigidCount = 0;
    uint8_t *pos = static_cast<uint8_t *>(surfs);
    for (uint32_t i = 0; i < surfCount; ++i)
    {
        GfxModelSkinnedSurface *s = reinterpret_cast<GfxModelSkinnedSurface *>(pos);
        if (s->skinnedCachedOffset == -3)
        {
            signature = MotionTable::Mix(signature, 3);
            pos += 4;
            continue;
        }
        if (s->skinnedCachedOffset == -2)
        {
            GfxModelRigidSurface *r = reinterpret_cast<GfxModelRigidSurface *>(pos);
            if (rigidCount < MotionTable::kMaxRigid)
                rigid[rigidCount++] = r->placement;
            r->surf.oldSkinnedCachedOffset = 0;
            pos += sizeof(GfxModelRigidSurface);
        }
        else
        {
            if (s->skinnedCachedOffset >= 0)
            {
                if (skinBase < 0)
                    skinBase = s->skinnedCachedOffset;
                s->oldSkinnedCachedOffset = 0;
            }
            pos += sizeof(GfxModelSkinnedSurface);
        }
        signature = MotionTable::Mix(signature, std::hash<const void *>{}(s->xsurf));
    }
    if (!active)
        return;
    MotionTable::Prev prev;
    {
        TableGuard guard;
        prev = s_table.Note(obj, (uint32_t)rg.frontEndFrameCount, signature, sceneEnt->placement.base.origin,
                            skinBase, rigid, rigidCount);
    }
    if (prev.state == taau::MotionState::None)
        return;
    // The previous rigid placements, copied next to this frame's surfaces.
    uint32_t placementsAt = 0;
    if (prev.rigidCount)
    {
        const uint32_t bytes = prev.rigidCount * sizeof(GfxScaledPlacement);
        const uint32_t at = (uint32_t)InterlockedExchangeAdd(&frontEndDataOut->surfPos, bytes);
        if (at + bytes > sizeof(frontEndDataOut->surfsBuffer))
            prev.rigidCount = 0;
        else
        {
            std::memcpy(&frontEndDataOut->surfsBuffer[at], rigid, bytes);
            placementsAt = at;
        }
    }
    uint32_t rigidIndex = 0;
    pos = static_cast<uint8_t *>(surfs);
    for (uint32_t i = 0; i < surfCount; ++i)
    {
        GfxModelSkinnedSurface *s = reinterpret_cast<GfxModelSkinnedSurface *>(pos);
        if (s->skinnedCachedOffset == -3)
        {
            pos += 4;
            continue;
        }
        if (s->skinnedCachedOffset == -2)
        {
            GfxModelRigidSurface *r = reinterpret_cast<GfxModelRigidSurface *>(pos);
            if (prev.state == taau::MotionState::Reject)
                r->surf.oldSkinnedCachedOffset = -1;
            else if (rigidIndex < prev.rigidCount)
                r->surf.oldSkinnedCachedOffset =
                    Encode(prev.state, placementsAt + rigidIndex * (uint32_t)sizeof(GfxScaledPlacement));
            ++rigidIndex;
            pos += sizeof(GfxModelRigidSurface);
            continue;
        }
        if (s->skinnedCachedOffset >= 0 && (prev.state == taau::MotionState::Reject || prev.skinBase >= 0))
            s->oldSkinnedCachedOffset =
                Encode(prev.state, (uint32_t)(prev.skinBase + (s->skinnedCachedOffset - skinBase)));
        pos += sizeof(GfxModelSkinnedSurface);
    }
}

uint32_t RB_TaauBuildMotion(const GfxViewInfo *viewInfo, const RB_TaauMotionMatrices &m,
                            std::vector<deko9::TaauMotionDraw> *draws)
{
    draws->clear();
    const GfxBackEndData *data = viewInfo->input.data;
    const GfxDrawSurfListInfo &list = viewInfo->litInfo;
    if (!data || !list.drawSurfs || !data->skinnedCacheVb)
        return 0;
    constexpr uint32_t kPools = ARRAY_COUNT(gfxBuf.skinnedCacheVbPool);
    // Main keeps the next frame's entry locked while this frame's draws read
    // the current and the previous one: three entries keep the three apart.
    static_assert(kPools >= 3, "the skinned-cache pool needs an entry beyond the two the back end reads");
    const uint32_t pool = (uint32_t)(data->skinnedCacheVb - gfxBuf.skinnedCacheVbPool);
    if (pool >= kPools)
        return 0;
    IDirect3DVertexBuffer9 *skinVb = data->skinnedCacheVb->buffer;
    IDirect3DVertexBuffer9 *prevSkinVb = gfxBuf.skinnedCacheVbPool[(pool + kPools - 1) % kPools].buffer;
    const deko9::TaauMat skinCur[2] = {deko9::TaauMul(Translation(m.viewOffset), m.viewProj),
                                       deko9::TaauMul(Translation(m.viewOffset), m.viewProjDepthHack)};
    const deko9::TaauMat skinPrev = deko9::TaauMul(Translation(m.prevViewOffset), m.prevViewProj);
    for (uint32_t i = 0; i < list.drawSurfCount; ++i)
    {
        const GfxDrawSurf drawSurf = list.drawSurfs[i];
        const uint32_t type = (uint32_t)drawSurf.fields.surfType;
        if (type != SF_XMODEL_RIGID && type != SF_XMODEL_SKINNED)
            continue;
        const GfxModelSkinnedSurface *surf =
            reinterpret_cast<const GfxModelSkinnedSurface *>((const char *)data + 4 * drawSurf.fields.objectId);
        const int32_t state = surf->oldSkinnedCachedOffset;
        if (!state || !surf->xsurf)
            continue;
        const XSurface *xsurf = surf->xsurf;
        const bool viewmodel =
            surf->info.gfxEntIndex && (data->gfxEnts[surf->info.gfxEntIndex].renderFxFlags & GFX_DEPTH_RANGE_VIEWMODEL);
        deko9::TaauMotionDraw d{};
        d.stride = sizeof(GfxPackedVertex);
        d.viewmodel = viewmodel;
        d.reject = state < 0;
        void *ib = nullptr;
        int32_t baseIndex = 0;
        DB_GetIndexBufferAndBase(xsurf->zoneHandle, xsurf->triIndices, &ib, &baseIndex);
        d.ib = static_cast<IDirect3DIndexBuffer9 *>(ib);
        d.firstIndex = (uint32_t)baseIndex;
        d.indexCount = 3u * xsurf->triCount;
        d.vertexCount = xsurf->vertCount;
        if (type == SF_XMODEL_SKINNED)
        {
            if (surf->skinnedCachedOffset < 0)
                continue;
            d.vb = skinVb;
            d.vbOffset = (uint32_t)surf->skinnedCachedOffset;
            d.prevVb = d.reject ? nullptr : prevSkinVb;
            d.prevVbOffset = d.reject ? 0 : (uint32_t)(state - 1);
            Store(skinCur[viewmodel], d.cur);
            Store(skinPrev, d.prev);
        }
        else
        {
            const GfxModelRigidSurface *rigid = reinterpret_cast<const GfxModelRigidSurface *>(surf);
            const GfxScaledPlacement *prev =
                d.reject ? &rigid->placement
                         : reinterpret_cast<const GfxScaledPlacement *>(&data->surfsBuffer[state - 1]);
            if (!d.reject && !viewmodel && SamePlacement(*prev, rigid->placement))
                continue;
            void *vb = nullptr;
            int32_t vbOffset = 0;
            DB_GetVertexBufferAndOffset(xsurf->zoneHandle, (uint8_t *)xsurf->verts0, &vb, &vbOffset);
            d.vb = static_cast<IDirect3DVertexBuffer9 *>(vb);
            d.vbOffset = (uint32_t)vbOffset;
            Store(deko9::TaauMul(PlacementMatrix(rigid->placement), viewmodel ? m.viewProjDepthHack : m.viewProj),
                  d.cur);
            Store(deko9::TaauMul(PlacementMatrix(*prev), m.prevViewProj), d.prev);
        }
        if (!d.vb || !d.ib)
            continue;
        draws->push_back(d);
    }
    return (uint32_t)draws->size();
}
