#ifndef SWITCH_PERF_H
#define SWITCH_PERF_H

// Lightweight per-frame CPU phase profiler for the Switch SP port.
//
// Why this exists: the existing `PROF_SCOPED` tags compile to no-ops unless
// Tracy is enabled, and the coarse PERF buckets (Sys_Milliseconds, 1 ms) only
// say "render" dominates.  A frame can spend most of its time in the
// CPU frontend `scene` bucket with the GPU/backend idle, and nothing in
// the tree could attribute it.  This module accumulates sub-millisecond phase
// times with the cheap ARM system counter (__rdtsc / armGetSystemTick,
// ~19.2 MHz => ~52 ns) and prints one grouped, per-second, per-frame-averaged
// line per phase group.
//
// Overhead when enabled is two counter reads per scope (~40 scopes/frame =>
// ~4 us/frame, <0.01% of a 50 ms frame); when disabled every scope is a single
// load+branch.  It is off by default (`switch_perfTrace`), so production runs
// are unchanged.
//
// Nesting: counters are flat.  A group total (e.g. FRAME_RENDER) includes the
// child counters that run inside it; the report says so.  Groups are not
// double counted against each other.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// X-macro: id, report name, group.  Keep ids stable; append new ones.
#define SWITCH_PERF_COUNTERS(X) \
    /* group: whole frame */ \
    X(FRAME_TOTAL,            "frame",      G_FRAME)  \
    X(FRAME_SETUP,            "setup",      G_FRAME)  \
    X(FRAME_SERVER,           "server",     G_FRAME)  \
    X(FRAME_CLIENT,           "client",     G_FRAME)  \
    X(FRAME_RENDER,           "render",     G_FRAME)  \
    X(FRAME_TAIL,             "tail",       G_FRAME)  \
    /* group: SCR_UpdateScreen split */ \
    X(RENDER_BEGIN,           "begin",      G_RENDER) \
    X(RENDER_CGAME,           "cgame",      G_RENDER) \
    X(RENDER_SCENE,           "scene",      G_RENDER) \
    X(RENDER_UI,              "ui",         G_RENDER) \
    X(RENDER_END,             "end",        G_RENDER) \
    X(RENDER_ISSUE,           "issue",      G_RENDER) \
    /* group: R_IssueRenderCommands split */ \
    X(ISSUE_HANDOFF,          "handoff",    G_ISSUE)  \
    X(ISSUE_PREP,             "prep",       G_ISSUE)  \
    X(ISSUE_WAITFRONTEND,     "waitfront",  G_ISSUE)  \
    X(ISSUE_BEGINFRAME,       "beginframe", G_ISSUE)  \
    X(ISSUE_DRAW3D,           "draw3d",     G_ISSUE)  \
    X(ISSUE_EXEC,             "exec",       G_ISSUE)  \
    X(ISSUE_ENDFRAME,         "endframe",   G_ISSUE)  \
    X(EXEC_ENDSCENE,          "endscene",   G_ISSUE)  \
    X(EXEC_FENCE,             "fence",      G_ISSUE)  \
    X(EXEC_PRESENT,           "present",    G_ISSUE)  \
    /* group: frontend scene build (R_GenerateSortedDrawSurfs) */ \
    X(SCENE_TOTAL,            "total",      G_SCENE)  \
    X(SCENE_SETUP,            "setup",      G_SCENE)  \
    X(SCENE_FILTERENTS,       "filterents", G_SCENE)  \
    X(SCENE_DPVS,             "dpvs",       G_SCENE)  \
    X(SCENE_PORTALWALK,       "portalwalk", G_SCENE)  \
    X(SCENE_CELLSTATIC,       "cellstatic", G_SCENE)  \
    X(SCENE_SUNSETUP,         "sunsetup",   G_SCENE)  \
    X(SCENE_BSP_CAMERA,       "bsp_cam",    G_SCENE)  \
    X(SCENE_BSP_EMISSIVE,     "bsp_emis",   G_SCENE)  \
    X(SCENE_BSP_SUNSHADOW,    "bsp_sun",    G_SCENE)  \
    X(SCENE_BSP_SPOTSHADOW,   "bsp_spot",   G_SCENE)  \
    X(SCENE_SMODEL_CAMERA,    "smc",        G_SCENE)  \
    X(SCENE_SMODEL_SKIN,      "smc_skin",   G_SCENE)  \
    X(SCENE_SMODEL_SORT,      "smc_sort",   G_SCENE)  \
    X(SCENE_SMODEL_SUNSHADOW, "smr_sun",    G_SCENE)  \
    X(SCENE_SMODEL_SPOTSHADOW, "smr_spot",  G_SCENE)  \
    X(SCENE_SCENEENT,         "ent",        G_SCENE)  \
    X(SCENE_DOBJ_CULL,        "dobjcull",   G_SCENE)  \
    X(SCENE_XMODEL_SKIN,      "xskin",      G_SCENE)  \
    X(SCENE_DYNENT,           "dynent",     G_SCENE)  \
    X(SCENE_FX,               "fx",         G_SCENE)  \
    X(SCENE_SHADOWEMIT,       "shadowemit", G_SCENE)  \
    X(SCENE_MERGE,            "merge",      G_SCENE)  \
    X(SCENE_SORT,             "sort",       G_SCENE)  \
    /* group: cgame (`draw` is CG_DrawActive and contains the scene build, so
       sim = total - draw; the rest splits the pre-draw cgame frame) */ \
    X(CGAME_TOTAL,            "total",      G_CGAME)  \
    X(CGAME_ENTINFO,          "entinfo",    G_CGAME)  \
    X(CGAME_DRAW,             "draw",       G_CGAME)  \
    X(CGAME_SNAPSHOTS,        "snapshots",  G_CGAME)  \
    X(CGAME_FX,               "fx",         G_CGAME)  \
    X(CGAME_ENTITIES,         "entities",   G_CGAME)  \
    X(CGAME_PREDICT,          "predict",    G_CGAME)  \
    X(CGAME_VIEW,             "view",       G_CGAME)  \
    X(CGAME_ENTITYPROC,       "entproc",    G_CGAME)  \
    X(CGAME_AIM,              "aim",        G_CGAME)  \
    X(CGAME_DRAW2D,           "draw2d",     G_CGAME)  \
    /* group: other subsystems */ \
    X(SERVER_TOTAL,           "total",      G_OTHER)  \
    /* group: CG_AddPacketEntities split -- per-entity lerp/link, then
       CG_ProcessEntity by eType (fx = ET_FX + ET_LOOP_FX) */ \
    X(ENTS_LERP,              "lerp",       G_ENTS)   \
    X(ENTS_LINK,              "link",       G_ENTS)   \
    X(ENTS_DOBJ_ADD,          "dobjadd",    G_ENTS)   \
    X(ENTS_PREDICT,           "predict",    G_ENTS)   \
    X(ENTS_PRED_BOUNDS,       "predbounds", G_ENTS)   \
    X(ENTS_PRED_SKIN,         "predskin",   G_ENTS)   \
    X(ENTS_LOOPSOUND,         "loopsnd",    G_ENTS)   \
    X(ENTS_POSEUNION,         "poseunion",  G_ENTS)   \
    X(ENTS_GENERAL,           "general",    G_ENTS)   \
    X(ENTS_ITEM,              "item",       G_ENTS)   \
    X(ENTS_MISSILE,           "missile",    G_ENTS)   \
    X(ENTS_MOVER,             "mover",      G_ENTS)   \
    X(ENTS_FX,                "fx",         G_ENTS)   \
    X(ENTS_LIGHT,             "light",      G_ENTS)   \
    X(ENTS_MG42,              "mg42",       G_ENTS)   \
    X(ENTS_VEHICLE,           "vehicle",    G_ENTS)   \
    X(ENTS_ACTOR,             "actor",      G_ENTS)   \
    X(ENTS_OTHER,             "other",      G_ENTS)   \
    /* group: CG_ProcessSnapshots split.  With sv_smp 0 the inline server's
       post-frame (SV_WaitServer, saves, SV_SendClientMessages' snapshot build,
       CL_CreateNextSnap) runs inside SV_WaitServerSnapshot; cgnext is the
       cgame transition (CG_SetNextSnap + CG_ProcessNextSnap). */ \
    X(SNAP_SVWAIT,            "svwait",     G_SNAP)   \
    X(SNAP_SAVES,             "saves",      G_SNAP)   \
    X(SNAP_SEND,              "send",       G_SNAP)   \
    X(SNAP_CREATE,            "create",     G_SNAP)   \
    X(SNAP_CGNEXT,            "cgnext",     G_SNAP)   \
    X(SNAP_INTERP,            "interp",     G_SNAP)   \
    /* group: inline SP G_RunFrame, a 50 ms server tick.  Values are still
       averaged per displayed frame, as for every other SWITCH_PERF group. */ \
    X(GAME_TOTAL,             "total",      G_GAME)   \
    X(GAME_PLAYER_PRE,        "playerpre",  G_GAME)   \
    X(GAME_ANIM_INIT,         "animinit",   G_GAME)   \
    X(GAME_SCRIPT_PRE,        "scriptpre",  G_GAME)   \
    X(GAME_ANIM_UPDATE,       "animupdate", G_GAME)   \
    X(GAME_SCRIPT_TIME,       "scripttime", G_GAME)   \
    X(GAME_BADPLACES,         "badplaces",  G_GAME)   \
    X(GAME_PLAYER_POST,       "playerpost", G_GAME)   \
    X(GAME_ENTITIES,          "entities",   G_GAME)   \
    X(GAME_CLIENT,            "client",     G_GAME)   \
    X(GAME_CORPSES,           "corpses",    G_GAME)   \
    /* retail PC GPU sync (R_SyncGpu): syncgpu on the cgame line is the
       CG_DrawActiveFrame call (SV_FrameRateSmoothing + R_SyncGpu, server
       slices included); syncgpu on the frame line is Com_Frame's call under
       a fullscreen menu; slice on the game line is the sliced G_RunFrame
       time (SV_FRAME_DO_SMOOTHING), a part of cgame syncgpu. */ \
    X(CGAME_SYNCGPU,          "syncgpu",    G_CGAME)  \
    X(FRAME_SYNCGPU,          "syncgpu",    G_FRAME)  \
    X(GAME_SLICE,             "slice",      G_GAME)   \
    /* group: the threaded render back end (r_smp_backend 1, RB_RenderThread):
       RB_RenderCommandFrame's draw (RB_BeginFrame + RB_Draw3D + execute), its
       wait for the swap fence and RB_EndFrame.  Recorded from the back-end
       thread only (SwitchPerf_MarkBackendThread) and printed on the
       `SWITCH_PERF backend` line with every other counter that thread ran
       (the whole front end during remote screen updates). */ \
    X(BACKEND_FRAME,          "rbframe",    G_BACKEND) \
    X(BACKEND_SWAPWAIT,       "swapwait",   G_BACKEND) \
    X(BACKEND_ENDFRAME,       "endframe",   G_BACKEND)

typedef enum SwitchPerfCounter
{
#define SWITCH_PERF_ENUM_ENTRY(id, name, group) SWITCH_PERF_##id,
    SWITCH_PERF_COUNTERS(SWITCH_PERF_ENUM_ENTRY)
#undef SWITCH_PERF_ENUM_ENTRY
    SWITCH_PERF_COUNTER_COUNT
} SwitchPerfCounter;

typedef enum SwitchPerfGroup
{
    G_FRAME = 0,
    G_RENDER,
    G_ISSUE,
    G_SCENE,
    G_CGAME,
    G_OTHER,
    G_ENTS,
    G_SNAP,
    G_GAME,
    G_BACKEND,
    G_COUNT
} SwitchPerfGroup;

// Global enabled flag: every scope checks this one load, so disabled runs pay
// essentially nothing.  Refreshed once per frame from the dvar.
extern int SwitchPerf_g_enabled;

// Flat tick accumulator, indexed by SwitchPerfCounter.  Exposed so the RAII
// scope can add inline.
extern uint64_t SwitchPerf_g_ticks[SWITCH_PERF_COUNTER_COUNT];

// Reporting sink: one full line per call.  Defaults to Com_Printf on the
// Switch build; a host test installs its own sink.
typedef void (*SwitchPerfPrintFn)(const char *line);
void SwitchPerf_SetPrintSink(SwitchPerfPrintFn fn);

void SwitchPerf_Init(void);
void SwitchPerf_SetEnabled(int enabled);
int SwitchPerf_Enabled(void);

// Monotonic counter (ticks); exposed for tests and callers that want to time
// something without a scope object.
uint64_t SwitchPerf_NowTicks(void);
// Convert a tick delta to microseconds (integer, rounded down).
uint64_t SwitchPerf_TicksToUs(uint64_t ticks);
// Convert a tick delta to milliseconds as a double (for averages).
double SwitchPerf_TicksToMs(uint64_t ticks);

void SwitchPerf_AddTicks(int counter, uint64_t ticks);
void SwitchPerf_AddThreadTicks(int counter, uint64_t ticks);

// Renderer worker threads.  The flat counters above are main-thread
// only: a worker calls SwitchPerf_MarkWorkerThread once, after which scopes it
// enters record nothing (they would race the plain accumulators and mix worker
// time into main-thread phases).  Its busy time is reported on its own
// `SWITCH_PERF worker` line through the atomic SwitchPerf_AddWorkerTicks.
#define SWITCH_PERF_WORKER_COUNT 2
void SwitchPerf_MarkWorkerThread(void);
void SwitchPerf_AddWorkerTicks(int worker, uint64_t ticks);
// The render back-end thread (r_smp_backend 1) is neither: its scopes are
// real phases (RB_RenderCommandFrame, and the whole front end while it draws
// the loading screen), so they go to a second atomic accumulator printed as
// the `SWITCH_PERF backend` line, never into the main-thread phases.
void SwitchPerf_MarkBackendThread(void);
void SwitchPerf_AddBackendTicks(int counter, uint64_t ticks);
uint64_t SwitchPerf_WindowBackendTicks(int counter);
uint64_t SwitchPerf_WindowWorkerTicks(int worker);
// The SP server thread (sv_smp 1) is the same case: its G_RunFrame and
// post-frame busy time goes to an atomic slot printed as `sv=` (per displayed
// frame, like w0/w1) on the worker line.  `svframe=` is the same time per
// server frame, the number comparable to sv_smp 0's `game total` per tick.
void SwitchPerf_AddServerThreadTicks(uint64_t ticks, int frames);

// Event counts (not time), atomic so any thread may add: printed once per
// window as `SWITCH_PERF pretess ...`, averaged per displayed frame except the
// *_peak/*_cap entries, which are the window's maximum and the capacity.
// World pretess (r_add_bsp.cpp R_PreTessBspDrawSurfs / R_AddBspDrawSurfs):
//   batches      successful pretess batches (one per material run)
//   surfs        world surfaces batched
//   draws        sub-draws those batches issue (a split per firstVertex,
//                lightmap or reflection-probe change)
//   split_vert   splits caused only by a firstVertex change (same lightmap and
//                probe): the ones index rebasing could merge
//   split_lmap   splits caused by a lightmap or probe change (irreducible)
//   draws_rebased sub-draws if firstVertex-only splits were merged by rebasing
//                indices, keeping each merged vertex range within 16 bits
//   fallback     world surfaces drawn one draw each because pretess was off or
//                the index buffer was full
//   allocfail    R_AllocPreTessIndices refusals (world and static models)
//   area_*       CM_AreaEntities calls, sector nodes walked, entities
//                bounds-tested and returned
//   mover_pushes G_MoverPush calls; mover_still those with zero move and
//                amove; mover_pushed entities they pushed
//   syncgpu_calls R_SyncGpu calls with r_gpuSync on; syncgpu_waits fence
//                polls that found the GPU still busy; sv_slices sliced
//                G_RunFrame calls (SV_FrameRateSmoothing)
//   used_peak / cap  most pretess indices used in one frame / buffer capacity
//   pt_bytes     index bytes the pretess paths copied (world + static models)
//   st_draws     world sub-draws drawn straight from the static world index
//                buffer (r_deko9StaticPretess: a run splits at every index
//                discontinuity as well as firstVertex/lightmap/probe), and
//                st_surfs the world surfaces they cover
//   sm_lists / sm_inst  static-model cached lists pretessed and the model
//                instances in them
//   ib_calls / ib_bytes  R_SetIndexData copies into the dynamic index buffer
//                (one NOOVERWRITE/DISCARD lock each) and their bytes; ib_bsp,
//                ib_smodel, ib_fx (code meshes), ib_marks, ib_xmodel
//                (uncached skinned), ib_bmodel split the calls by caller
#define SWITCH_PERF_EVENTS(X) \
    X(PRETESS_BATCHES,       "batches")       \
    X(PRETESS_SURFS,         "surfs")         \
    X(PRETESS_DRAWS,         "draws")         \
    X(PRETESS_SPLIT_VERTEX,  "split_vert")    \
    X(PRETESS_SPLIT_LMAP,    "split_lmap")    \
    X(PRETESS_DRAWS_REBASED, "draws_rebased") \
    X(PRETESS_FALLBACK,      "fallback")      \
    X(PRETESS_ALLOC_FAIL,    "allocfail")     \
    X(PRETESS_BYTES,         "pt_bytes")      \
    X(PRETESS_STATIC_DRAWS,  "st_draws")      \
    X(PRETESS_STATIC_SURFS,  "st_surfs")      \
    X(PRETESS_SMODEL_LISTS,  "sm_lists")      \
    X(PRETESS_SMODEL_INST,   "sm_inst")       \
    X(PRETESS_SMODEL_STATIC_INST, "sm_st_inst") \
    X(SETIDX_CALLS,          "ib_calls")      \
    X(SETIDX_BYTES,          "ib_bytes")      \
    X(SETIDX_BSP,            "ib_bsp")        \
    X(SETIDX_SMODEL,         "ib_smodel")     \
    X(SETIDX_CODEMESH,       "ib_fx")         \
    X(SETIDX_MARKS,          "ib_marks")      \
    X(SETIDX_XMODEL,         "ib_xmodel")     \
    X(SETIDX_BMODEL,         "ib_bmodel")     \
    X(AREA_CALLS,            "area_calls")    \
    X(AREA_NODES,            "area_nodes")    \
    X(AREA_TESTED,           "area_tested")   \
    X(AREA_HITS,             "area_hits")     \
    X(MOVER_PUSHES,          "mover_pushes")  \
    X(MOVER_STILL,           "mover_still")   \
    X(MOVER_PUSHED,          "mover_pushed")  \
    X(SYNCGPU_CALLS,         "syncgpu_calls") \
    X(SYNCGPU_WAITS,         "syncgpu_waits") \
    X(SV_SLICES,             "sv_slices")

typedef enum SwitchPerfEvent
{
#define SWITCH_PERF_EVENT_ENUM_ENTRY(id, name) SWITCH_PERF_EV_##id,
    SWITCH_PERF_EVENTS(SWITCH_PERF_EVENT_ENUM_ENTRY)
#undef SWITCH_PERF_EVENT_ENUM_ENTRY
    SWITCH_PERF_EVENT_COUNT
} SwitchPerfEvent;

void SwitchPerf_AddEvent(int event, uint64_t count);
// Pretess index high-water mark for the current frame (max over the window).
void SwitchPerf_NotePreTessUsed(uint64_t used, uint64_t capacity);

// Frame boundary.  BeginFrame starts a frame; EndFrame rolls the one-second
// window and prints when it elapses.
void SwitchPerf_BeginFrame(void);
void SwitchPerf_EndFrame(void);

// Force an immediate report and reset the window (debug / tests).
void SwitchPerf_ReportNow(void);
// Number of frames accumulated in the current window (tests).
uint64_t SwitchPerf_WindowFrames(void);
// Accumulated ticks for one counter in the current window (tests).
uint64_t SwitchPerf_WindowTicks(int counter);

#ifdef __cplusplus
}
#endif

// ---- C++ RAII scope -------------------------------------------------------
#ifdef __cplusplus
extern thread_local int SwitchPerf_t_workerThread;

class SwitchPerfScope
{
public:
    explicit SwitchPerfScope(int counter, bool active = true)
    {
        // 0 = main thread (plain accumulator), 2 = render back end (own
        // atomic accumulator), 1 = renderer worker (records nothing here).
        if (active && SwitchPerf_g_enabled && SwitchPerf_t_workerThread != 1)
        {
            m_counter = counter;
            m_start = SwitchPerf_NowTicks();
        }
        else
        {
            m_counter = -1;
            m_start = 0;
        }
    }

    ~SwitchPerfScope()
    {
        if (m_counter < 0)
            return;
        if (SwitchPerf_t_workerThread == 2)
            SwitchPerf_AddBackendTicks(m_counter, SwitchPerf_NowTicks() - m_start);
        else
            SwitchPerf_g_ticks[m_counter] += SwitchPerf_NowTicks() - m_start;
    }

private:
    int m_counter;
    uint64_t m_start;
};

#define SWITCH_PERF_CAT2(a, b) a##b
#define SWITCH_PERF_CAT(a, b) SWITCH_PERF_CAT2(a, b)
// Unique variable per line; use one scope per source line.
#define SWITCH_PERF_SCOPE(counter) \
    SwitchPerfScope SWITCH_PERF_CAT(switchPerfScope_, __LINE__)((counter))
#define SWITCH_PERF_SCOPE_IF(counter, condition) \
    SwitchPerfScope SWITCH_PERF_CAT(switchPerfScope_, __LINE__)((counter), (condition))

// Selected worker-command scopes use a separate atomic counter so their
// service time can be attributed without racing the main-thread accumulator.
class SwitchPerfThreadScope
{
public:
    explicit SwitchPerfThreadScope(int counter, bool active = true)
        : m_counter(active && SwitchPerf_g_enabled ? counter : -1),
          m_start(m_counter >= 0 ? SwitchPerf_NowTicks() : 0) {}
    ~SwitchPerfThreadScope()
    {
        if (m_counter >= 0)
            SwitchPerf_AddThreadTicks(m_counter, SwitchPerf_NowTicks() - m_start);
    }
private:
    int m_counter;
    uint64_t m_start;
};

#define SWITCH_PERF_THREAD_SCOPE(counter) \
    SwitchPerfThreadScope SWITCH_PERF_CAT(switchPerfThreadScope_, __LINE__)((counter))

// Sequential-section helper for code where a RAII block would be awkward
// (locals declared between the boundaries).  Construct once at the start of a
// section, then mark() at each boundary; each mark attributes the ticks since
// the previous mark to the given counter.
class SwitchPerfMarks
{
public:
    SwitchPerfMarks() { m_last = SwitchPerf_g_enabled ? SwitchPerf_NowTicks() : 0; }

    void mark(int counter)
    {
        if (!SwitchPerf_g_enabled)
            return;
        const uint64_t now = SwitchPerf_NowTicks();
        SwitchPerf_g_ticks[counter] += now - m_last;
        m_last = now;
    }

private:
    uint64_t m_last;
};
#endif // __cplusplus

#endif // SWITCH_PERF_H
