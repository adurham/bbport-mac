# PERF_LOOP — the autonomous perf loop

Established 2026-10-17 after the encoder/break workstream was measured out. This document is the
loop: stages, gates, commands, artifacts. Any session can resume from here.

## Where we are (measured, do not re-walk)

- 2560x1440: off 23.4 ms; FSR 3.1 preset 3 20.8 ms; preset 4 (854x480 internal) 19.4 ms = 51.6 fps.
  Guest startup-patched to 854x480, no upscaler: **16.5 ms = 60.6 fps**. Gap to 60: ~2.9 ms.
- Frame structure at FSR3x: GPU busy 12.53 ms (fragment 4.60, vertex 3.68, compute 4.30), idle
  6.75 ms; ~340 encoders/frame (one MTL4CB per encoder), ~20 us boundary gaps, ~46 pass breaks
  (81% from `Runtime::CopyBuffer`).
- Closed by measurement: store/load on break pairs (+2.1 ms, worse), the per-frame DownloadImage
  drain (null, present-bound), packaging/commits (micro-benchmark: 300 CBs one commit = 300
  individual commits; one CB with 300 encoders worse), copy-merge + in-pass barriers together
  (neutral: -0.04 ms, -0.85 encoders; marginal in-situ break cost ~0.03-0.04 ms).
- Consequence: the break-elimination line caps at ~1-1.5 ms. The remaining gap is upscaler cost
  (FSR dispatches + the live-mode 1080p post + copy-backs) and possibly CPU-side starvation.
- Fable review (2026-10-17): do NOT build a Metal-native renderer core (#6) — it inherits Metal's
  per-encoder cost and the win lives in bb-gpu's scheduler. Rank: (A) startup-patched render size
  -> (B) encoder census + merge in bb-gpu -> (D) bespoke upscaler -> frame gen -> MetalFX zero-copy.
- Idle split, NAILED in stage B5 (stored trace, 100,915 intervals): **3.68 ms CPU-late + 3.07 ms
  queued**, which supersedes the earlier partial estimate (~2.2-2.6 CPU-late / ~3.4 queued). The
  CPU-late side is the port's own recording thread, not the guest and not the commit path.

## Loop status — 2026-10-17, stages A-D run to completion

Artifacts: `UPSCALER_CONFIG_BENCH.md` (A), `ENCODER_CENSUS.md` (B, incl. B4 verdict),
`STAGE_D_CUSTOM_UPSCALER.md` (D). Worktree: `fix/encoder-merge` @ 97e5104. Docs:
`build/macos-native-0.6`.

| stage | outcome | number |
|---|---|---|
| A1 live vs startup-patched + FSR3.1 p4, 2560 | **gate fails** (needs >= 1 ms) | 18.915 -> 18.675 ms, **-0.24 ms**; no default change |
| A2 preset sweep (2/3/4, 2560 + 3440, live) | **done, monotonic** | 2560: 21.02 / 20.11 / **18.92**; 3440: 22.75 / 21.93 / **20.81**; p4 default confirmed |
| B4 encoder line | **KILLED** (0.6 ms best case vs 2.9 needed) | N_min 307, N_vk 335, gap 28.5 (9.3%) = copy line + compute floatability, not the render-pass model; no backend earned; CB packing already closed by `METAL4_PACKING_PROBE.md` |
| C (implement the merges) | **not implemented**, per B4 | only surviving item: relaxed order-aware hazard rule (~0.3-0.4 ms), needs a rule-A-vs-B validator |
| D bespoke 2-pass upscaler | **built, measured, CLOSED** (needs >= 1 ms) | 18.780 -> 18.165 ms, **-0.615 ms / +1.8 fps**, and the custom path is less stable frame-to-frame than FSR; gate ships default-off |
| E1 recording-thread run-ahead | **measured, CLOSED** (the 3.68 ms CPU-late is not recoverable) | timeline shows the guest is held by the console pacing lock until the GPU is idle (`first_submit` +0.07 ms after `gpu_idle`), but with a full frame of run-ahead (6 pipeline modes) p50 stays 18.74-19.06 and 18.77 at the deepest; the frame is GPU-paced at ~18.7 ms |
| E2 GPU-timeline reconciliation | **done, ANSWERED: the GPU executes 12.5 ms** | two traces (default 12.73 / pipe_max 12.54 ms busy of an 18.4 ms frame, 68% occupancy, 0.1-0.2% channel overlap); the 5.9-6.0 ms idle is ~302 command-buffer handoffs at 19.3-19.5 us each (`same-cmdbuf` gaps are 0.2 us), queued/cpu-late 45/55 — so E1's "GPU owns 18.6 ms" was a present-cadence `retire` stamp misread (`gpu_idle -18.35` vs `retire +18.35` in the same frame) |

**The next lever, in rank order (this is where the next session starts):**

0. **DONE (E2, `docs/GPU_TIMELINE_RECONCILIATION.md`): the GPU executes 12.5 ms and idles 5.9 ms
   at command-buffer handoffs.** 60 fps needs 16.67, so the GPU has 4.2 ms of slack in execution
   and the deficit (1.7-2.1 ms) is in the handoff cadence, not in GPU compute. Do not spend the
   next session cutting GPU work alone: the priced inventory totals ~1.5 ms of pass-transition/CPU
   savings that the trace says do not touch the 19.4 us handoff.
1. **Commit cadence, in bb-gpu's recorder** (5.86 ms/frame of GPU-visible drain over ~302 commits,
   a quarter of them committed 16-17 us before the GPU needs them, while pipeline mode gives the
   recorder 18.3 ms of notice for 6.7 ms of work): measure per-CB commit time vs GPU start
   (`BB_FRAME_TIMELINE` extension, no trace needed), then pre-commit a frame's buffers before the
   GPU drains the previous one. This is the only item large enough to carry the whole deficit.
2. The largest single GPU block is one **1.5 ms compute pass/frame** (the upscaler chain; stage D
   reached 0.6 of it) and **fragment 5.4 ms** (2.99 ms in 13.8 medium passes at 2560, no lever
   scoped yet). Vertex 3.64 is inventory, not a plan.
3. The relaxed hazard rule (0.3-0.4 ms) last, only with its validator.

**Closed by measurement in E1, do not re-walk:** the frame-pipeline / run-ahead line
(`docs/FRAME_PIPELINE_TIMELINE.md`): the recorder is serialised behind the GPU by the console pacing
lock, but unpipelining it (6 modes, up to a full frame of run-ahead and no presenter gate) does not
move the frame (18.74 -> 18.77-19.06 ms). The recorder's per-draw cost (8.4-10 us/draw, 6.6-8.7
ms/frame) is not the wall either.

Do not re-walk: encoder count/merging, CB packing, store/load on break pairs, download drain, FSR
configuration, live-vs-patched render size, the bespoke upscaler as-is.



## Worktree / conventions

- Perf worktree: `~/repos/bbport-encmerge-build` (branch `fix/encoder-merge`, warm build; carries
  all default-off gates: BB_COPY_MERGE, BB_INPASS_BARRIERS, BB_BREAK_STORE_PROBE,
  BB_DOWNLOAD_SYNC_PROBE).
- Docs live in `~/repos/bbport-mac-native/docs/` (branch `build/macos-native-0.6`).
- Runs: hidden only; ONE GPU workload at a time; verify `pgrep -fl 'bb-probe|bb-gpu|gpu_ab_run'`
  empty before and after; a foreign bb-probe = STOP and report.
- Run template (standing camera, 190 s):
  `BB_GAME_DIR=$HOME/Games/Bloodborne/CUSA03173 BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 \
   ROUTE='ly=128,rx=128:5' CAPTURES=2 BB_PASS_BREAK_TRACE=1 bash tools/macos/gpu_ab_run.sh TAG 190`
- Validity per run: `overlap-TAG.txt` empty, draws >= 500 in `frames-TAG.csv`, p50 <= 40 ms;
  a dubious tag re-runs once. Parse steady rows (draws>=500 && t_s>=60) for p25/p50/p75,
  present_wait, draws.
- Commit + push every stage's artifacts (docs to the native line; gates to the worktree branch).
- No default changes without the owner. Correctness = port captures + the capture-comparison tooling
  against the same-mode noise floor.

## Stage A — upscaler configuration — DONE, gate fails (`UPSCALER_CONFIG_BENCH.md`)

A1. Determine whether FSR works with the startup-patched path (`live_resolution=0`): read
    `docs/upscaler.md` + the presenter/upscaler code. TAA needs live; the question is FSR.
    If it works: 2+2 alternated A/B at 2560, fsr3 preset 4, live vs startup-patched. Report
    delta + captures. Gate: >= 1 ms and clean captures -> candidate default (owner decides).
A2. Preset sweep (presets 2-4) in the winning mode, 2560 and 3440.

Artifacts: `docs/UPSCALER_CONFIG_BENCH.md` + JSON/CSV in the worktree `out/`.

## Stage B — encoder census — DONE, line KILLED at B4 (`ENCODER_CENSUS.md`)

B1. Classify the ~340 encoders of one captured frame by TYPE and CAUSE: RT change required by the
    guest, compute<->render transition, barrier split, CopyBuffer break, cache-management copy,
    FSR internal, meta/resolve, query. Use the frame analyzer dump (`BB_CAPTURE_TRIGGER` /
    `BB_CAPTURE_DIR`; two dumps already exist in `out/analyzer*/`) + `BB_KK_STATS` deltas +
    break histograms (`BB_PASS_BREAK_TRACE=1`) + `mtl_gaps.py` on a stored trace.
B2. N_min: the minimum encoders ANY backend could emit for that frame given real data
    dependencies (adjacent computes without hazard -> one encoder; copies deferrable to pass
    edges; RT-compatible draws -> one render encoder). Build it from the dump's per-pass resource
    reads/writes.
B3. N_vk: the minimum reachable within bb-gpu + KosmicKrisp with targeted changes (deferred
    copies, dynamic rendering, compute coalescing in the driver).
B4. Decision (consult Fable at this point; record its verdict in the doc):
    - Kill the encoder line if (340 - N_min) x ~0.03-0.035 ms < ~3 ms  -> it cannot buy the frame.
    - If N_vk ~= N_min -> the Vulkan layering is not the blocker; implement in bb-gpu.
    - Only if N_min << N_vk and the gap is structural to the Vulkan render-pass model does a
      backend (hybrid HAL, Vulkan kept as oracle) earn consideration.
B5. Confirm the idle split: inter-encoder gaps vs CPU-late, with the CPU side named (Rosetta guest,
    bb-gpu recording, driver submission).

Artifacts: `docs/ENCODER_CENSUS.md` (+ data files).

## Stage C — implement what B proves — NOT RUN (B4 killed the line; see the status block)

Top-ranked merges from the census, each behind a default-off gate: measure encoder count first,
then 2+2 A/B + captures; commit each; doc the result honestly (a regression ships default-off with
its numbers, as with COPY_MERGE).

## Stage D — bespoke temporal upscaler — DONE and CLOSED at -0.615 ms (`STAGE_D_CUSTOM_UPSCALER.md`)

A 2-3 pass reproject + accumulate upscaler on the port's existing machinery (camera AND object
motion vectors, history/reprojection), instead of FFX's 8 dispatches + barriers. Gate
`BB_UPSCALE_CUSTOM=1`, default off; A/B vs FSR3_4 at matched output + capture comparison; report
quality (text/HUD sharpness, ghosting on particles/fog/transparents) and cost.

## Reporting

Bottom-line numbers only (p50, fps, encoders, deltas, correctness), TLDR-sized. Every stage ends
with a commit + push and a doc update. Never re-walk a closed lever: check this file first.

## Final state — 2026-10-18 (all stages closed; the loop is done)

Stage E (FRAME_PIPELINE_TIMELINE.md): the guest is held by the console pacing lock so recording
never overlaps the GPU — but six unpipelining variants (up to a full frame of run-ahead, presenter
gate removed) all landed at 18.77-19.06 ms. Serialization is real and worthless to remove.

Stage F (GPU_TIMELINE_RECONCILIATION.md): the GPU really executes **12.5-12.7 ms** busy, not 18.6;
**5.86-6.01 ms of idle sits at command-buffer boundaries** (302 CB/frame at ~19.4 us; between
encoders inside a buffer: 0.2 us). Commit cadence is closed as a mechanism (the Metal 4
micro-benchmark's up-front-commit control shows the same per-boundary drain).

Stage G (BUBBLE_STALL_ANALYSIS.md + FRAME_HANDOFF_WAIT.md): the once-per-frame 1.36 ms bubble is
**not a queue wait** — both driver waits are enqueued a full frame early and resolved before the
GPU arrives (0 of 2,803 rows late), and the drawable pool peaks at 1 of 3 (never exhausted). It
decomposes into **two once-per-frame MTL4 submission boundaries (0.70 + 0.18 ms)** plus ~0.5 ms
inside a commit. No fix was shipped; `BB_DRAWABLES` and `BB_COMMIT_TRACE(_FULL)` remain as tooling.

### The arithmetic, final

Frame (2560, FSR3.1 p4, 854x480 internal) = **12.5 ms GPU work + 5.9 ms Metal submission
boundaries = 18.4 ms (54 fps)**. 60 fps needs 16.67 ms: a **1.7-2.1 ms deficit**. Priced levers:
encoder merges 0.6, custom upscaler 0.615, relaxed hazard rule 0.35 — ~1.5 ms combined and partly
overlapping, and none of them touch the 5.9 ms boundary cost. **60 fps at 2560 is not reachable by
the priced micro-levers.** The portable numbers today: 2560 ~53-55 fps, 3440 ~50-52 fps.

### What remains, priced honestly

1. **GPU work (needs ~1.8 ms):** fragment 5.41 ms (2.99 in 13.8 medium full-res passes), vertex
   3.64 (2.39 in 9.6 heavy draws), compute 3.52 (the FSR chain alone is 1.50-1.63) — a cheaper
   upscaler that keeps quality is the most concentrated item.
2. **Frame generation** (FSR 3.1 FG is compiled in, unwired) — perceived 60+, not true 60; latency
   and artifacts are the trade.
3. **Accept the numbers** above; use the presets (p4 is fastest at both resolutions).
4. The deep renderer rework (Fable: not now; the Metal-native core inherits Metal's per-encoder cost
   anyway).

Tooling left behind (all default off): BB_COPY_MERGE, BB_INPASS_BARRIERS, BB_BREAK_STORE_PROBE,
BB_DOWNLOAD_SYNC_PROBE, BB_UPSCALE_CUSTOM, BB_FRAME_TIMELINE, BB_SUBMIT_PIPELINE, BB_COMMIT_TRACE,
BB_DRAWABLES; plus tools/macos/mtl_gaps.py, mtl_bubble.py, commit_trace.py.
