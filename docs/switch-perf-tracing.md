# Switch per-frame CPU phase tracing (`SWITCH_PERF`)

`src/port/switch_perf.{h,cpp}` is a lightweight per-frame CPU phase
profiler for the Switch SP build. It exists because the coarse `PERF` buckets
(`Sys_Milliseconds`, 1 ms) and the `PROF_SCOPED` tags (no-ops without Tracy)
could not attribute the frame, and a hardware run showed the frame is
CPU-frontend-bound rather than GPU-bound.

## Turning it on

Off by default. Opt in per run:

```
+set switch_perfTrace 1
```

The dvar is `DVAR_NOFLAG`, so it is never archived; pass it on the command line
or through `sdmc:/switch/kisakcod/kisak_diag.cfg`. It can be enabled together
with `performance 1` (the shipping config), because it is a separate switch and
its overhead is tiny (~40 scopes/frame, two ARM counter reads each,
`armGetSystemTick` at ~19.2 MHz).

## What it prints

One grouped line per second per group. Values are microseconds per frame,
averaged over the window; `frames=`/`fps=`/`elapsed=` are on the `frame` line.
Group totals include the child counters that run inside them; groups do not
overlap.

```
SWITCH_PERF frame fps=.. frames=.. elapsed=..ms frame=.. setup=.. server=.. client=.. render=.. tail=..
SWITCH_PERF render begin=.. cgame=.. scene=.. ui=.. end=.. issue=..
SWITCH_PERF issue handoff=.. prep=.. waitfront=.. beginframe=.. draw3d=.. exec=.. endframe=.. endscene=.. fence=.. present=..
SWITCH_PERF scene total=.. setup=.. filterents=.. dpvs=.. portalwalk=.. cellstatic=.. sunsetup=.. \
    bsp_cam=.. bsp_emis=.. bsp_sun=.. bsp_spot=.. smc=.. smc_skin=.. smc_sort=.. smr_sun=.. \
    smr_spot=.. ent=.. dynent=.. fx=.. shadowemit=.. merge=.. sort=..
SWITCH_PERF cgame total=.. draw=..          (sim = total - draw)
SWITCH_PERF ents lerp=.. link=.. dobjadd=.. predict=.. predbounds=.. predskin=.. loopsnd=.. ...
SWITCH_PERF other total=..
SWITCH_PERF game total=.. playerpre=.. animinit=.. scriptpre=.. animupdate=.. scripttime=.. badplaces=.. playerpost=.. entities=.. client=.. corpses=..
SWITCH_PERF backend rbframe=.. swapwait=.. endframe=.. [group.name=..]
```

Reading it:

- `render scene` is `SCR_DrawScreenField` (the frontend scene build);
  `render cgame` is `CL_CGameRendering` (which contains the scene build via
  `CG_DrawActive`). If `scene` dominates, the frame is CPU-frontend-bound.
- `issue` is `R_IssueRenderCommands`; `exec` is `RB_CallExecuteRenderCommands`
  (the deko3d submission), and `present`/`fence` split it further. If
  `issue` is small while `scene` is large, the GPU is not the bottleneck.
- `issue prep` includes `waitfront`: the main thread's wait for queued renderer
  work before `RB_BeginFrame`. These counters are nested, so do not add them.
- `backend` is the render back-end thread's own accumulator (`r_smp_backend
  1`): `rbframe` is RB_RenderCommandFrame's draw (RB_BeginFrame + RB_Draw3D +
  execute), `swapwait` its wait for the swap fence, `endframe` RB_EndFrame;
  any other counter that thread ran (the whole front end while it draws the
  loading screen during fastfile loads) is appended as `group.name=`. With the
  back end inline (`r_smp_backend 0`) the line is all zeros and the work shows
  under `issue` instead. The main-thread `issue handoff=` is then main's wait
  for the back end to take the frame.
- `game total` times `G_RunFrame` and splits its 50 ms server tick into player,
  animation, script, entity, and client phases. With `sv_smp 0` it runs on the
  main thread under `snap svwait`; with `sv_smp 1` the game scopes intentionally
  record zero to avoid writing the main-thread profiler from the server thread.
  Every reported number is averaged per *displayed frame*, not per server tick.
- `scene total` is `R_GenerateSortedDrawSurfs`; the rest of the `scene` group
  attributes it across DPVS culling (`dpvs`/`portalwalk`/`cellstatic`), the BSP
  passes, static-model add + skin + sort, entities, dynents, FX, shadow-map
  emission and the merge/sort tail.
- The entity detail counters separate scene submission (`dobjadd`), cgame's
  prediction decision/queue cost (`predict`), delayed bounds work (`predbounds`),
  and delayed skinning (`predskin`). In `scene`, `dobjcull` measures entity
  frustum processing and `xskin` measures XModel skinning. These counters
  complement the existing `pretess` batch/surface/draw/split counters.
- `dobjadd` and `predict` are caller-thread elapsed time. `predbounds`,
  `predskin`, `dobjcull`, and `xskin` include CPU service time from whichever
  thread executes the worker command, including the inline fallback. Their
  values can add up to more than frame wall time when commands run in parallel;
  counters within each group also overlap or nest, so do not sum them as an
  exclusive breakdown.

## Where the scopes live

| Group | Files |
| --- | --- |
| frame | `src/qcommon/common.cpp` (`Com_Frame_Try_Block_Function`) |
| render | `src/client/cl_scrn.cpp` (`SCR_UpdateFrame`) |
| issue | `src/gfx_d3d/r_rendercmds.cpp`, `src/gfx_d3d/rb_backend.cpp` |
| scene | `src/gfx_d3d/r_scene.cpp`, `r_workercmds.cpp`, `r_dpvs.cpp`, `r_dpvs_static.cpp`, `r_add_staticmodel.cpp`, `r_add_bsp.cpp` |
| cgame | `src/cgame/cg_view.cpp`, `cg_ents.cpp` |
| other | `src/server/sv_main.cpp` (`SV_Frame`) |
| game | `src/game/g_main.cpp` (`G_RunFrame`) |

## Verification

- Host proof: `switch_perf_test.cpp` -> `PASS:SWITCH_PERF ...` (ASan/UBSan),
  wired into `./test host`.
- Source gate: a local source check (not published)
  -> `PASS:SWITCH_PERF_INSTRUMENTATION ...`. It fails if a counter is declared
  but never instrumented, if a `SWITCH_PERF_*` reference names an undeclared
  counter, if an expected call site lost its scope, if the module left the SP
  build, or if the dvar stopped defaulting off.

## Adding a counter

Append an `X(ID, "name", G_GROUP)` line to `SWITCH_PERF_COUNTERS` in
`switch_perf.h`, add the scope at the call site, and add the expected site to
`SITES` in the gate. The report line and name table are generated from the
X-macro, so nothing else needs updating.
