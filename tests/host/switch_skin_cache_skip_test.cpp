// Skinned vertex cache unavailable for a frame (null pool buffer after a
// device or video restart, or a misaligned lock): R_LockSkinnedCache must not
// lock, must report the skip (error line every 300th frame + skincache_skip
// event), and R_SkinSceneDObjModels must still emit the DObj's surfaces by
// skinning them into the temp skin buffer instead of dropping the entity.
// Negative control: with the buffer present the lock is taken, nothing is
// reported and the surfaces get cache offsets.
#include <universal/q_shared.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_buffers.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_model.h>
#include <gfx_d3d/r_skin_cache_fallback.h>
#include <gfx_d3d/r_dobj_skin.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_workercmds.h>
#include <port/switch_perf.h>
#include <xanim/dobj_utils.h>
#include <xanim/xmodel.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

DxGlobals dx{};
GfxBuffers gfxBuf{};
GfxBackEndData *frontEndDataOut;
GfxScene scene;
const dvar_t *r_xdebug;
int SwitchPerf_g_enabled = 1;

static int s_errors, s_lastErrorSkips, s_skipEvents, s_locks, s_unlocks, s_workerCmds;
alignas(16) static uint8_t s_cacheStorage[0x1000];
static uintptr_t s_lockMisalign;

void Com_PrintError(int, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ++s_errors;
    fputs(line, stdout);
}
void SwitchPerf_AddEvent(int event, uint64_t count)
{
    if (event == SWITCH_PERF_EV_SKIN_CACHE_SKIPPED)
        s_skipEvents += (int)count;
}
void *R_LockVertexBuffer(IDirect3DVertexBuffer9 *, int, int, int)
{
    ++s_locks;
    return s_cacheStorage + s_lockMisalign;
}
void R_UnlockVertexBuffer(IDirect3DVertexBuffer9 *) { ++s_unlocks; }
void R_WarnOncePerFrame(GfxWarningType, ...) {}
void R_AddWorkerCmd(WorkerCmdType, unsigned char *) { ++s_workerCmds; }
void R_TaauNoteDObj(const GfxSceneEntity *, const DObj_s *, void *, uint32_t) {}
void R_AddDebugLine(DebugGlobals *, const float *, const float *, const float *) {}
const dvar_t *useFastFile;
void Z_VirtualCommit(void *, int) {}

// One model, one deformed (weighted) surface of four vertices, no hidden parts.
static XSurface s_surface;
static XModel s_model;
static DObjAnimMat s_basePose;
int DObjGetNumModels(const DObj_s *) { return 1; }
XModel *DObjGetModel(const DObj_s *, int) { return &s_model; }
int XModelNumBones(const XModel *) { return 1; }
const DObjAnimMat *XModelGetBasePose(const XModel *) { return &s_basePose; }
int XModelGetSurfaces(const XModel *, XSurface **surfaces, int)
{
    *surfaces = &s_surface;
    return 1;
}
void DObjGetHidePartBits(const DObj_s *, uint32_t *partBits) { memset(partBits, 0, 4 * sizeof(uint32_t)); }
int DObjSkelAreBonesUpToDate(const DObj_s *, int *) { return 1; }
DObjAnimMat *DObjGetRotTransArray(const DObj_s *) { return &s_basePose; }
void DObjGetBoneInfo(const DObj_s *, XBoneInfo **boneInfo) { *boneInfo = nullptr; }
int DObjNumBones(const DObj_s *) { return 1; }

static int failures;
static void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAIL:SKIN_CACHE_SKIP", what);
    failures += !ok;
}

struct FrameResult
{
    bool locked;
    int surfaces;
    int offset;
    bool tempVert;
};

static FrameResult RunFrame(GfxVertexBufferState *cacheVb)
{
    frontEndDataOut->skinnedCacheVb = cacheVb;
    frontEndDataOut->surfPos = 0;
    frontEndDataOut->tempSkinPos = 0;
    if (cacheVb)
        cacheVb->used = 0;
    gfxBuf.skinCache = true;
    gfxBuf.skinnedCacheLockAddr = 0;
    R_LockSkinnedCache();
    FrameResult r{};
    r.locked = gfxBuf.skinnedCacheLockAddr != 0;
    GfxSceneEntity sceneEnt{};
    sceneEnt.cull.lods[0] = 0;
    DObj_s obj{};
    DObjAnimMat bones[1]{};
    r.surfaces = R_SkinSceneDObjModels(&sceneEnt, &obj, bones);
    if (r.surfaces)
    {
        const GfxModelSkinnedSurface *surf = (const GfxModelSkinnedSurface *)sceneEnt.cull.skinnedSurfs.firstSurf;
        r.offset = surf->skinnedCachedOffset;
        r.tempVert = surf->skinnedCachedOffset == -1
            && (const uint8_t *)surf->skinnedVert == frontEndDataOut->tempSkinBuf;
    }
    // Model the frame end so the next frame starts unlocked.
    gfxBuf.skinnedCacheLockAddr = 0;
    return r;
}

int main()
{
    dvar_t off{}, on{};
    on.current.enabled = true;
    r_xdebug = &off;
    useFastFile = &on;
    std::unique_ptr<GfxBackEndData> data(new GfxBackEndData{});
    frontEndDataOut = data.get();
    alignas(16) static uint8_t tempSkin[0x1000];
    frontEndDataOut->tempSkinBuf = tempSkin;
    s_surface.deformed = true;
    s_surface.vertCount = 4;

    GfxVertexBufferState present{};
    present.buffer = reinterpret_cast<IDirect3DVertexBuffer9 *>(0x1000);
    GfxVertexBufferState nullBuffer{};

    // Negative control: a usable cache is locked and used, nothing reported.
    FrameResult good = RunFrame(&present);
    check(good.locked && s_locks == 1, "present buffer: lock taken");
    check(good.surfaces == 1 && good.offset == 0, "present buffer: surface skinned into the cache at offset 0");
    check(s_errors == 0 && s_skipEvents == 0 && R_SkinnedCacheSkipCount() == 0, "present buffer: no skip reported");

    // Null pool buffer: no lock, loud report, DObj still drawn from temp skin.
    FrameResult skipped = RunFrame(&nullBuffer);
    check(!skipped.locked && s_locks == 1, "null buffer: lock skipped");
    check(s_errors == 1 && s_skipEvents == 1 && R_SkinnedCacheSkipCount() == 1, "null buffer: error line + skincache_skip event");
    check(skipped.surfaces == 1, "null buffer: DObj surfaces still emitted (not dropped)");
    check(skipped.tempVert, "null buffer: surface skins uncached into the temp skin buffer");
    check(frontEndDataOut->tempSkinPos == (long)(4 * sizeof(GfxPackedVertex)), "null buffer: temp skin space reserved for 4 verts");

    // A null skinnedCacheVb state behaves the same.
    RunFrame(nullptr);
    check(R_SkinnedCacheSkipCount() == 2 && s_skipEvents == 2, "null cache state: skip counted");

    // Rate limit: the error line repeats every 300th skipped frame only.
    for (int i = 0; i < 300; ++i)
        RunFrame(&nullBuffer);
    check(s_errors == 2 && s_skipEvents == 302, "rate limit: 2 error lines over 302 skipped frames, every frame counted");

    // A misaligned lock is released and treated as a skip.
    s_lockMisalign = 4;
    FrameResult misaligned = RunFrame(&present);
    s_lockMisalign = 0;
    check(!misaligned.locked && s_unlocks == 1 && s_skipEvents == 303, "misaligned lock: released and counted");
    check(misaligned.tempVert, "misaligned lock: surface skins uncached");

    // Device lost: retail behaviour (no lock, no report, nothing queued).
    dx.deviceLost = 1;
    FrameResult lost = RunFrame(&nullBuffer);
    dx.deviceLost = 0;
    check(!lost.locked && s_skipEvents == 303, "device lost: no lock, not reported as a cache fault");
    check(lost.surfaces == 0, "device lost: retail behaviour, skinned DObjs not queued");

    // Recovery: the cache is used again.
    FrameResult back = RunFrame(&present);
    check(back.locked && back.offset == 0, "buffer restored: cache path again");

    printf("skin cache skip: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
