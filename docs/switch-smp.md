# Switch SMP: threading the SP frame on 3 A57 cores + deko3d

Question: can SMP be done in a way that fits the Switch and
deko3d instead of the Xbox 360 / x86 decompiled mess? Answer: yes, in layers,
and the profile says only the first layer pays today.

## Where the main thread's time goes (hardware, 960x540 + sgsr, Cargoship deck)

Example medians per displayed frame after `BRIEFING_EXIT` (`switch_perfTrace 1`,
no other diagnostics): fps 54, frame 18.6 ms,
frame p75/p90 20.1/24.1 ms. Everything runs on the main thread (`render` =
18.6 ms; `sv_smp 0` so the server frame is inline).

| main-thread phase | ms | what it is |
| --- | --- | --- |
| `issue` (R_IssueRenderCommands = the render back end, inline) | 9.2 | `draw3d` 8.1 (RB_StandardDrawCommands: R_DrawSurfs list walking, pretess, state, ~3.5 ms of it deko9 submission at 1.2-1.4 us x ~2,750 draws), endframe 0.4, present 0.4, exec 0.3, beginframe 0.2, fence 0.003 |
| `cgame draw` (R_GenerateSortedDrawSurfs, scene build) | 3.3 | smc 0.57, bsp_cam 0.53, cellstatic 0.32, ent 0.24, dpvs/portalwalk 0.23, filterents 0.10, the rest waits on worker0 (xskin 2.7 ms runs on worker0) |
| `cgame entities` (CG_AddPacketEntities) | 2.2 | link 0.58, predskin 0.49, lerp 0.47, mover 0.38, dobjadd 0.27, predbounds 0.25 |
| `game` (G_RunFrame slices, inline server) | 2.3 | entities 1.29, scriptpre 0.37, animupdate 0.25, client 0.20; 1.8 ms of it runs as SV_FrameRateSmoothing slices inside the GPU wait below |
| `cgame syncgpu` (R_SyncGpu: wait for last frame's GPU fence) | 1.8 | pure wait when the GPU is behind; overlaps the game slices |
| `cgame snapshots` | 1.1 | CG_ProcessSnapshots |
| `cgame draw2d` + rest | 0.5 | |

Stack sampler cross-check (`nxlink-simple-114125.log`, `switch_pcSample 1000`,
windows 5:6 = 10 s in the level, thread 0; that run also had a per-entity sound
logging diagnostic, `Com_PrintMessage` 31% of main, excluded here): of the rest,
R_IssueRenderCommands 38% (R_DrawSurfs 31%, deko9 PrepareDraw 12%,
Deko9_DrawIndexedRanges 8%), G_RunFrame 16%, R_GenerateSortedDrawSurfs 13%,
CG_ProcessSnapshots 9%, entity work ~10%, svcWaitForAddress 4% (waits).
Other threads in the same windows: backend 100% idle (r_smp_backend 0),
worker0 80% idle (skinning 9%, FX 3%, scene-ent surfaces 3%), server 84% idle
(it only helps with worker commands), sndmix 92% idle, sndstream 97% idle.

GPU: 12.5 ms/frame in the same view (the `fence` column is ~0 because
`syncgpu` on the main thread absorbs the wait). So the frame is CPU-bound on one
core with 2 cores mostly idle, and the GPU has ~6 ms of slack per frame.

## Plan, in layers (Switch/deko3d native, engine-wide)

Constraints: 3 usable cores (0-2; 3 is the OS), Horizon does not time-slice
threads of lower priority on a core, deko3d records command lists on any thread
but submits in order on one queue, deko9 is one D3D9-style state machine behind
one recursive `DeviceLock` (held across a whole draw-surface list). Placement
today: main = core 0, worker0 = core 1, back end + server + sndmix/sndstream =
core 2 (`Switch_ServerThreadCpuId`); worker1 is disabled on a 3-core mask.

### (a) Render back end on its own core (`r_smp_backend 1`) -- implement now

Retail PC's design: the front end fills `frontEndDataOut` (double-buffered
`s_backEndData[2]`), `R_IssueRenderCommands` hands it to `RB_RenderThread` via
`Sys_WakeRenderer`, main waits at most one frame in `R_ToggleSmpFrameCmd`
(`Sys_IsRendererReady`) while helping with worker commands. This is exactly a
deko3d record-on-another-thread/submit-in-order pipeline: the back end records
and submits, main only creates/locks resources under the device lock. The
handshake and the libnx event bugs were fixed (retail's five events, manual-reset `LEvent`).

Expected from the data: main 18.6 -> ~9.4 ms (cgame + waits), back end ~9.2 ms
on core 2, GPU 12.5 ms: the pipeline is GPU-bound at ~80 fps and the display is
vsync-bound at 60, so the heavy view goes from 54 fps (p90 24 ms) to the 60 fps
cap with ~7 ms of CPU headroom per frame; lighter views gain nothing (already
60). Cost: nothing new to write; the risks are ownership (below) and an
earlier A/B on a previous renderer where `cgame` grew 12.4 -> 20.8 ms with the back end
threaded (unexplained; the GPU was the ceiling then, so no fps change). The
deko3d path must be measured with the lock-wait counters added for this.

### (b) Job system for the front end on the idle worker -- measured target, not now

Candidates, all on main today: `cgame entities` 2.2 ms (per-entity lerp/link/
dobjadd is order-dependent through the scene arrays, so it needs a real
split), scene build 3.3 ms (the DPVS/static/BSP walks already are worker
commands; what is left is the camera BSP pass 0.5, static-model cache 0.6 and
the cell statics 0.3), snapshots 1.1 ms. The existing typed worker commands
(`WRKCMD_*`, `R_AddWorkerCmd`, retail's design) are the job system; nothing new
is needed for that. After (a) main is ~9.4 ms against a 12.5 ms GPU, so (b) buys
no fps until the GPU work drops below ~9 ms. Do it only with a measured main
> GPU case; each move needs the identical-output proof (`r_deko9Verify`, paused
pixel A/B `r_deko9EmissiveTourShots` + an offline compare script).

### (c) Parallel deko3d command recording per pass -- last

Needs per-thread deko9 recording contexts (a `DkCmdBuf` + state machine per
thread, the hazard tracker and constant files split, the ordered submit of the
per-pass lists on the one queue) and the engine's draw lists split per pass.
Pays only when the back-end thread itself is the bottleneck: it is 9.2 ms
against a 12.5 ms GPU, so not now. Estimated 1-2 weeks; revisit if (a) shows
the back end above the GPU in some view (e.g. Killhouse, where the GPU is
cheap).

### Skip: `sv_smp 1`

Retail PC never ran it; the 360 server thread races engine data (a GPU MMU fault
showed up only with `sv_smp 1`), and its work (2.3 ms) is already sliced into
the GPU wait. Dropped as a goal.

## Ownership rules for (a) on deko9

The back end owns the device while it runs; main gets it back only through
`R_SyncRenderThread` (`Sys_FrontEndSleep`, `r_glob.haveThreadOwnership`). The
retail sites are intact (material sort, distortion toggle, lost device, DB
sync/archive, picmip, static-model cache flush, cubemap shots, shutdown).
Main-thread D3D calls that bypass the back end, all serialized by the deko9
`DeviceLock` (which the back end holds for a whole draw-surface list):

- resource creation at load: textures (`r_image.cpp`), buffers
  (`r_buffers.cpp`), shaders/declarations (`r_material*.cpp`), render targets
  (`r_rendertarget.cpp`, under sync);
- locks/uploads per frame: dynamic meshes and the skinned cache
  (`R_LockVertexBuffer`/`R_LockIndexBuffer`, DISCARD renames while the GPU reads
  the old memory), image uploads (`Image_Upload*` -> `UnlockStore`, a copy
  recorded into the open list between the back end's draws), model lighting
  (`UpdateTexture`), cinematic and outdoor images;
- the GPU fence: a frame id of deko9's native frame ring
  (`dx.gpuSyncFrame`, set by `R_InsertGpuFence` at the end of the back end's
  frame F to F - (N - 1)), polled/waited by main (`R_SyncGpu`,
  `Deko9_WaitFrame`) under `CRITSECT_GPU_FENCE`. The swap wait
  (`RB_BackendTimeout`: frame F - N before presenting F) and the
  end-of-scene fence (`backEndData->endFrame`, `R_EndFencePending`) are
  frame ids too; no D3D9 event query is on the pacing path any more. Event
  queries still follow D3D9 semantics (done when the work recorded before
  `Issue(END)` completes: an in-list fence, or the newest submitted list
  when the open list is empty); `Query::GetData` never flushes from a thread
  that is not the draw thread (`Device::IsRecordingThread`), and
  `Query::Wait`/`Deko9_WaitFrame` only poll while a batch holds the lock.
  Before this, the swap query issued after a Present was bound to the list
  that then held the whole next frame, so each frame was recorded, submitted
  and waited for before it was presented (GPU and CPU in series,
  `maxInFlight=1`);
- native calls off the back end: `Deko9_SetBufferRole`/`SetDebugName` at
  creation, `Deko9_WaitQuery` (sync), `Deko9_WaitForGpuIdle` (shutdown),
  `Deko9_SetRtCompression` (init).

**Single submitter.** The unlocked GPU waits below
(`WaitSeq`, the present's acquire and frames-in-flight wait) are correct only
while no other thread submits a command list. That is not "only the thread
that drew last": main legitimately submits with `r_smp_backend 1` after
`R_SyncRenderThread` has made the back end idle (`CL_ShutdownAll` ->
`R_SwitchWaitForGpuIdle` at every map change and shutdown, inline frames
when the hand-off is refused), and the render thread draws the loading
screens. The rule is the engine's render ownership: `RB_BeginFrame` (the
thread rendering the frame) and `R_SyncRenderThread` (main) call
`Deko9_ClaimSubmitThread`, and `SubmitOpenList` compares the caller's
thread tag with the owner (one compare per list). A list from any other
thread logs `FAIL:DEKO9_SUBMIT_THREAD` once; the `DEKO9 perf lock` line
counts `submitClaims=` (owner changes) and `foreignSubmits=`. Host test:
`switch_deko9_fastpath_test.cpp` `TestSubmitOwner`.

**Observed (emulator, r_smp_backend 1, before the fix):** frame 162 ms, the back end's `rbframe` 140 ms, and main
waited `otherWait` 163-230 ms per frame on the device lock (`DEKO9 perf
lock`): main's per-frame buffer locks, uploads and `R_SyncGpu` fence polls
queued behind the batch lock the back end holds across a whole draw-surface
list, so the two threads ran serialized and the second core bought nothing.
Fix (deko9_lock.h `HandOffIfContended`, called after every recorded draw):
the contended path counts waiters; when one waits, the draw thread releases
the lock between two draws, lets the waiter take it (bounded spin), and takes
it back at the same depth. Between draws the device is consistent, and a
main-thread rename (`BufferRenamed` -> re-stamp), upload (a copy recorded into
the open list) or `SplitLongList` (`BeginList` resets `m_recorded` and every
dirty flag) is exactly what the inline back end already interleaves mid-list.
Without a waiter it is one relaxed load per draw. `handoffs=` on the perf
lock line counts them.

**Second holder (run 6, hand-off in place):** `otherWait` stayed 250-280
ms/frame with ~8 contended main calls and only ~4 hand-offs: the rest waited
behind `Device::Present`, which held the lock through `dkQueueAcquireImage`
(blocks until the display frees an image: a vsync period, or the GPU when it
is behind) and through the two-frames-in-flight fence wait. Both now run
with the lock released (`DeviceUnlockScope`, the pattern `WaitSeqFor`
already used for main's fence waits); since the queue must not be used from
two threads at once, `SplitLongList` submits only from the draw thread (the
rule `Query::GetData` already followed). On the hardware this is the wait that
scales with vsync, so it is the one behind the earlier A/B's
unexplained `cgame` growth (12.4 -> 20.8 ms with the back end threaded): main
was blocked on the device lock inside its own frame.

**Holder-site attribution (run 8, `DEKO9 perf lock ... sites=`):** every
blocked thread records the section the holder was in: `waitseq` 7,684 waits
/ 844 s, `submit` 70 s, `batch` 44 s, `none` 17 s, `present` 3.6 s over 14
min. So the third and largest holder was `Device::WaitSeq` (fence-ring slot
reuse in `SubmitOpenList`, `WaitIdle`): the draw thread blocked on the GPU
with the lock held. Its wait now runs on a copy of the fence with the lock
released. On the hardware the GPU is ahead of the CPU (12.5 vs 18.6 ms) so this
site should be small; the `sites=` line on the A/B run is the check, and
`batch` (hand-off latency) is what remains by design.

**Result (all three fixes, emulator medians in a level):**
main's device-lock wait 235 -> 0.67 ms/frame, `cgame` 98 -> 28 ms, main now
waits in `issue handoff` (128 ms) for the back end, whose `rbframe` (160 ms,
the software GPU) sets the frame. That is the intended pipeline shape: the
two threads are decoupled and the slower one sets the pace; on the hardware the
back end is ~9 ms against a 12.5 ms GPU, so the display cap should decide.

**Not a threading bug (found by the Cargoship stress, fixed):** the first
frame of `cargoship_fade` right after the briefing movie failed
`FAIL:DEKO9_TEXTURE_BIND` samplers 4-6 with and without `r_smp_backend`
(`smp-stress-cargoship-smp0.log`): the ffmpeg player rebuilds its retiring
texture set in place, the back end's sampler cache keys on `&image->texture`
(the same struct) and kept the binding while the device had forgotten the
freed texture. `CinematicReleaseTexture` now calls retail's `R_UnbindImage`
first. Retail's second, main-thread `R_Cinematic_UpdateFrame` call per frame
is also gone on Switch (the single-buffered player must update on the thread
that draws; a call off it while the back end owns the renderer fails loudly:
`FAIL:CINEMATIC_THREAD`).

Deko9-internal hazards this implies: a buffer renamed by main while bound
(`BufferRenamed` -> re-stamp at the next draw, under the lock), a copy recorded
mid-list into an image the batch samples (needs the copy barrier), and
`SplitLongList` from main cutting the back end's list. During fastfile loads
the whole front end (`SCR_UpdateScreen`) runs on the back-end thread
(`remoteScreenUpdateNesting`), so port code that assumes `Sys_IsMainThread()`
for input, UI, sound or the perf profiler is exercised there.

## Verification and measurement

- emulator stress with `+set r_smp_backend 1`: Cargoship and Killhouse, several
  minutes each, `switch_save_autoAfterFrames`/`loadAfterFrames`/`loadRepeat`
  cycles, `map_restart`, menu/briefing transitions; must end without
  `FAIL:`/assert/`Unhandled exception`, with `KISAK_QUICKLOAD_REPEAT_DONE`
  (a local stress script -> `PASS:SMP_STRESS`; the scripted
  walk proof this used to cite, `PASS:KILLHOUSE_PLAYER_MOVE`, is retired).
- Host: `./test host` (`SWITCH_RENDER_HANDSHAKE` proof), deko9 selftest in
  emulator.
- hardware A/B cfgs (`kisak_diag.cfg`, one line each):
  - A: `+set switch_perfTrace 1 +set switch_pcSample 1000 +set snd_enableStream 1 +set r_renderResolution 960x540 +set r_fsrMode sgsr +spmap cargoship`
  - B: same + `+set r_smp_backend 1`
  - Read: `SWITCH_PERF frame fps=/frame=` medians in the heavy view (expect
    54 -> 60 fps, p90 down), `SWITCH_PERF issue handoff=` (main's wait for the
    back end; large = back end slower than main), `SWITCH_PERF backend
    rbframe=` (the back end's own draw time per frame, expected ~9 ms; must
    stay below the GPU's 12.5 ms), `cgame syncgpu=` (GPU
    ceiling), `DEKO9 perf lock` (main-thread waits for the device lock; large =
    the batch lock starves main's uploads), `PCSAMPLE` thread 1 (backend) no
    longer 100% `svcWaitForAddress`; no `2520-0000` erpt.
