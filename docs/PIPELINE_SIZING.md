# Sizing the precompile problem

What a missing pipeline actually costs, split by layer, and how much of the game the pipeline
store covers. Measured 2026-10-21 on bbport `7bedcb7`, macOS 27.2, M4 Max, KosmicKrisp 3af1126
+ bbport patches (0001 on-disk shader cache … 0007 the stage timers this needed), native
`bb-gpu` (arm64), hidden `BB_WARM_ONLY` runs only.

Three runs, one route, one sandbox, one empty pipeline store, 240–300 s each: the same area entered
from the owner's save with `warm_route.sh`'s walk. Only the *caches* differed between them:

| run | pipeline store | Mesa's shader cache | Metal's own compile cache |
|---|---|---|---|
| `cold_kk` | empty | deleted | never seen these shaders (first run of this driver) |
| `hot_kk` | empty | warm (from `cold_kk`) | warm |
| `cold_mesa2` | empty | deleted again | warm |

The store is empty on purpose: the owner's own store already holds his area's pipelines, so a run
against it compiles almost nothing and measures nothing (10 of 11 warm routes add 0 blobs — see
`PIPELINE_WARMTH.md`). An empty store is the population a **first visit** to an area pays for, which
is the thing being sized.

```
BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 BB_WARM_SOURCE_USER=<user dir with the saves> \
    bash tools/macos/sizing_run.sh cold_kk 300
python3 tools/macos/pipeline_sizing.py --hot out/sizing/cold_kk --store <store> \
    --game $BB_GAME_DIR --json out/sizing/cold.json
```

## 1. Per-stage cost, and which layer is cold

Instrumentation, all behind default-off gates (`BB_PIPE_TIMING=<file>` in `pipeline_cache.cpp`,
`BB_KK_TIMING=<file>` in KosmicKrisp patch 0007, `BB_SHADER_BINDS=<file>` in
`gpu/gcn/bind_log.h`). The port logs one line per pipeline created (its stages, render target, draw
count, the async policy's verdict, the port's translating time and the whole
`vkCreateGraphicsPipelines` call); KosmicKrisp logs one line per graphics pipeline with its own
three timers; both carry the creating thread, so the k-th pipeline of a thread in one log is the
k-th line of the other.

Per graphics pipeline, medians over the 155 graphics pipelines of each run (µs):

| layer | what it is | `cold_kk` | `cold_mesa2` | `hot_kk` |
|---|---|---|---|---|
| port | GCN → SPIR-V (`CompileModule`) | 382 | 347 | 366 |
| Mesa 1 | SPIR-V → NIR → MSL (`kk_compile_shaders`) | 3 372 | 3 484 | 0 |
| Mesa 2 | MSL → MTLLibrary (`mtl_new_library`) | **142 738** | 75 | 124 |
| Metal | render pipeline state (`mtl_new_render_pipeline`) | 381 | 273 | 326 |
| driver call | `vkCreateGraphicsPipelines` (all of Mesa 1+2, Metal, cache I/O) | 134 714 | 3 754 | 1 271 |

Totals over the same 155 pipelines (ms), and the whole run (all 240 graphics + compute):

| layer | `cold_kk` | `cold_mesa2` | `hot_kk` |
|---|---|---|---|
| port translating | 120 | 106 | 106 |
| Mesa 1 (NIR→MSL) | 5 116 | 5 200 | 0 |
| Mesa 2 (MTLLibrary) | 22 307 | 14 | 30 |
| Metal PSO | 751 | 43 | 61 |
| driver call, all pipelines | 33 039 | 5 554 | 280 |

What the table says:

- **The port's own translation is 0.3 % of the bill** (204 ms of 33 s in `cold_kk`): 0.35 ms a
  pipeline. A warmer port-side cache can remove this and nothing else.
- **The layer the port's store and KosmicKrisp's cache cover is Mesa's NIR→MSL**: 5.2 s for the
  area's 155 pipelines, 3.4 ms each, and exactly 0 when the shader cache is warm (`hot_kk`).
  `cold_mesa2` shows it is the *only* thing that cache removes — 5.2 s of 5.55 s driver time with
  the cache cold, 0 of 0.28 s with it warm.
- **MSL→MTLLibrary is Metal compiling the shader, and it is the largest single cost when Metal has
  never seen the source**: 142.7 ms a pipeline, 22.3 s for 155 pipelines — 97 % of that run's driver
  time. `cold_mesa2` (same MSL, Metal's cache warm from `cold_kk`) pays 75 µs, i.e. Metal keeps its
  own compiled results and a *second* encounter of the same MSL is free. So this cost is paid once
  per distinct MSL per machine, and again whenever the port's translation changes the MSL text.
- **Metal's PSO creation is never the expensive part on this renderer**: 0.27–0.38 ms a pipeline,
  43–751 ms per run. Answering the question directly: **the KosmicKrisp cache-hot run still pays
  PSO creation** (61 ms over 154 pipelines, 0.33 ms each) — but that is 0.4 % of the cold bill, so
  it is not what a precompile campaign is buying.

One thing this does **not** separate: whether Metal's 142.7 ms in `cold_kk` was compiler start-up,
paging (that run ended at 4.4 GiB free+inactive under the machine's other load) or the compile of
each shader. The median is high across the whole run, not only at its start, and the same run's
counterpart with the cache warm is 1 200× cheaper, so it is per-shader compilation that Metal
caches — but a run on an idle machine would be needed to quote an absolute per-shader number.

## 2. The shape of what is left

Frames over 50 ms, from the port's own frame clock (`FRAME` lines, one per frame the drawing thread
saw: wall time, draws, pipelines created in it):

| run | frames > 50 ms | of those, with a compile | ms in them | ms of it compile | pipelines in them | worst frame | most pipelines in one frame |
|---|---|---|---|---|---|---|---|
| `cold_kk` | 31 | 17 | 42 185 | 41 244 | 247 | 10 400 ms | 77 |
| `cold_mesa2` | 22 | 12 | 7 184 | 6 480 | 229 | 4 577 ms | 80 |
| `hot_kk` | 22 | 14 | 2 191 | 1 605 | 241 | 1 040 ms | 85 |

The hitches are **N serial compiles, not a few large ones**: the worst frames hold 77–85 pipelines,
and 229–247 of the ~284 pipeline jobs a run starts (240 created + 44 in the background) land inside a
>50 ms frame. Mean pipelines per hitch frame with a compile: 14.5 (`cold_kk`), 19 (`cold_mesa2`). A cold area is ~240 pipelines and
5.5–33 s of compile wall time spent in twenty-odd frames of up to 10 s.

Not every hitch is a compile, and the warm run prices that: `hot_kk` still has 22 frames over 50 ms
and 2.2 s in them while its compiles total 0.28 s, so **roughly a third of the hitch time in a
warm-cache run is not pipeline creation at all** (asset streaming, texture upload — the second
source `PIPELINE_WARMTH.md` could not name).

Why the background compiler declined the pipelines that stayed on the drawing thread (`async=`
field of every created pipeline; async was on, `BB_ASYNC_STATIC_VI=1`, policy `wide`):

| verdict | `cold_kk` | `cold_mesa2` | `hot_kk` |
|---|---|---|---|
| started in the background | 44 | 51 | 43 |
| declined: full-screen pass (≤12 indices) | 114 | 114 | 113 |
| declined: target not drawn in the policy's frames | 49 | 49 | 49 |
| declined: compute pipeline (never goes async) | 34 | 34 | 34 |
| already finished (wait returned at once) | 43 | 49 | 42 |
| declined: indirect / frame draws / no dynamic VI | 0 | 0 | 0 |

So of the 240 pipelines created, 44 (18 %) were compiled in the background and 163 of the
remaining 196 (83 % of them, 68 % of all) were refused by exactly two gates: **full-screen passes
(114)** and **one-off render targets (49)**. The draw-count and indirect gates never fire in an
area.

## 3. Shader coverage: bound, drawn, on disc

`BB_SHADER_BINDS` hooks the bind path itself — the SH register writes
(`CommandProcessor::ApplyGraphicsRegisterPacket`, `Op::SetShReg`) — not the draws, so a program the
game binds and does not draw with is still counted. The hash is the `OrbShdr` header's own
`shader_hash`, the same number the store names its `.spv` files by, which is what makes the three
sets comparable.

| set | how it is counted | shaders |
|---|---|---|
| bound in one hidden run (`cold_kk`) | distinct `OrbShdr` hashes seen at the SH register writes | **252** |
| in the store (ever drawn) | distinct `<program hash>_<perm>.spv` prefixes, `CUSA03173-arm64`, 1 869 files | **426** |
| on disc | `OrbShdr` headers in the 8 `*.shaderbnd.dcx` bundles (decompressed, 25 711 headers) + `eboot.bin` (5 493) | **9 845** |

Two checks that the three sets are the same space, and that the disc count is not just byte
pattern luck: **all 426 store shaders are on disc** (426/426), and **all 252 bound shaders are on
disc** (252/252). Neither has a single hash the disc scan does not know.

- **bound but not drawn: 4 of 252** (1.6 %). 248 of the shaders this run bound are already in the
  store, i.e. the game binds what it is about to draw, not a screenful ahead. Pre-creating
  pipelines from the bind path alone would add ~1.6 % over what drawing already gives.
- The run bound 252 of the store's 426 shaders: the store holds shaders this area never touches
  (other weapons, other areas), and the run's 240 new pipelines were **new states over shaders the
  store already had** — the store's coverage is of shaders and states separately, and states are
  what a first visit to a new area adds.
- Compute shaders are not in the bind count (their program goes to the compute queue's state, not
  the SH register file); the store holds 38 compute pipelines.

## 4. Pipelines per shader, and what "the whole game" is

Store `CUSA03173-arm64`, the owner's: 634 pipeline keys (596 graphics, 38 compute), 617 translated
permutations over 426 distinct shaders (max 10 permutations for one shader, mean 1.45).

- **pipelines per shader: 1.40 graphics, 1.49 with compute** (596/426, 634/426).
- **ceiling for the whole game: 1.40 × 9 845 = 13 774 graphics pipelines** (15 200 with compute at
  the same ratio).
- **coverage today: 596 / 13 774 = 4.3 %.** The owner has **1/23rd** of the game's pipelines, and
  "the whole game" is a **23×** job, not 2×.

Two reasons that ceiling is an upper bound rather than a promise: the disc count includes a debug
bundle (1 068 of the 9 845 are in `gxflvershader.shaderbnddebug`, and some repeat the release
bundle), and a pipeline needs a state the shader binary does not pin down — so the 1.40 ratio is an
average over the state combinations this player has actually produced, not over the ones the game
can produce. The 23× is the order of magnitude; the number to move is the store's 4.3 %.

## 5. What the data supports doing first

Each lever named with the measurement that decides it, its acceptance bar, and the code it touches.
None of them is implemented in this change.

**a. Parallel synchronous compile of the frame's misses — not first.** The hitches are 14–19
pipelines in one frame, which is the shape this fixes, but the cost per pipeline is 142.7 ms of
Metal's MSL compiler (`cold_kk`) against 3.4 ms of Mesa and 0.35 ms of the port. Metal's compiler
is a separate service and does compile concurrently, but this measurement cannot say how well it
scales: the one run with heavy contention (`cold_kk`, 4 workers + the drawing thread) is also the
run with a cold Metal cache. Acceptance bar: on the same route and store, 4 threads of
`vkCreateGraphicsPipelines` must take the area's 240 pipelines from 33 s to under 12 s and the worst
frame from 10.4 s to under 3 s, with no frame's draws changing. Code: `PipelineCompiler` in
`gpu/render/pipeline_cache.cpp` — a synchronous fan-out of the frame's misses at the end of the
frame that needs them, which means the draw's pipeline must be *reserved* (a pending entry that the
draw thread waits on) rather than compiled inline. The thread-safety notes in `PIPELINE_WARMTH.md`
("the worker never translates") still hold: the fan-out would be of `vkCreateGraphicsPipelines`
only.

**b. Async widening for loading screens and one-offs — second, and cheap.** 163 of 240 pipelines
(68 %) were declined by exactly two gates: full-screen (`num_indices <= 12`, 114) and one-off render
targets (49). Widening means letting those skip their first frames too, which is a product
decision (a skipped full-screen pass is a grey frame) — but on a *loading screen* nothing is
visible, so the honest version is a time-boxed widening: during frames that draw < 300 times (the
narrow policy's own "loading screen" signal, currently a gate that *prevents* async), allow skips.
Acceptance bar: the frames of a first visit over 50 ms drop from 31 to under 10 and the total hitch
time from 42 s to under 10 s, with the same picture at the end of the load (a captured frame
compared against a warm-store run). Code: the three gates in `AsyncSkippable`
(`gpu/render/pipeline_cache.cpp`) plus whatever the compositor does with a skipped full-screen pass
(`rasterizer.cpp`, `vk_draw_prep.cpp`).

**c. Capture coverage — the only lever that reduces the number of pipelines.** 240 pipelines for one
area, 1/23rd of the game's ceiling, and the bind path offers 1.6 % of foreknowledge: everything else
has to be *drawn* once by something. So the campaign is the lever, and its size is now known: 23×
the current store, i.e. ~13 000 pipelines, at 142.7 ms of Metal and ~5 ms of port+Mesa each when
cold — 31 minutes of compile work for the whole game if it were free of contention, versus the
~45 minutes of hidden GPU time the 8-route pack already costs for one area. Acceptance bar: a store
built by a campaign must cover ≥90 % of the shaders a new area binds in its first minute (measured
with `BB_SHADER_BINDS`, against the 4.3 % the store has today) and the same area's first-visit
hitches must fall to the warm-store baseline (`hot_kk`: 22 frames >50 ms, 2.2 s). Code:
`tools/macos/warmfill.sh` for the campaign, `pipeline_store.cpp`/`profile.bin` for shipping it —
and the MSL must not change between the campaign and the player's build, or Metal's cache misses
again and the 142.7 ms comes back.

The order the numbers argue for: **(c) coverage is the only lever that changes the count, (a) is
worth its risk only if (c) has been tried and the remaining hitches are still batches of compiles,
and (b) is the cheapest thing that touches the 56 % of pipelines the current policy refuses** —
though it trades a visible hole for the stutter, so it is the one that needs the owner's decision,
not a measurement.

## 6. What was not measured, and the caveats that matter

- **The machine was not idle.** Other sessions ran throughout; `free+inactive` fell to 4.4 GiB in
  `cold_kk` and stayed above 12 GiB in `cold_mesa2`. The layer *shares* and the per-pipeline scale
  are the robust part; the absolute 142.7 ms may include paging.
- **The runs did not always drive the walk.** `hot_kk`'s title-press loop gave up ("the Continue did
  NOT take in 90s") while the game had in fact reached the area (the bind and pipeline logs show
  1 500 draws a frame); its pipeline set is 234/241 the same keys as `cold_kk`'s, so the A/B is
  comparable, but that run never walked. The press loop's evidence is the run's own log, which lags
  behind the game; a route that stands still at the save position still draws the area, which is why
  the numbers exist at all.
- **Metal's cache is a fourth layer with no name in the port.** Where it keeps the compiled MSL was
  not found (`~/Library/Caches/com.apple.metal` does not exist on this system); that it does keep it
  is what `cold_mesa2` vs `cold_kk` shows. A `xctrace` Metal System Trace of one cold run would name
  it and would separate its start-up cost from per-shader compilation.
- **The empty store is not the owner's state.** It prices a first visit; it does not say how many
  first visits he has left to make. That number needs the store's shader set compared against a
  route that walks a *new* area, which is what the campaign would have to be measured on.
- **Compute pipelines are in the totals but not in the bind count or the KK split** (the driver
  timer is on the graphics path only): 34–38 of a run's pipelines.
- No assert was hit in any of the three runs (no `gcn_to_vk.cpp:422 data_format`, no
  `cp_graphics.cpp:203` PM4 type 0); every run ended at `BB_WARM_SECONDS` with the GPU process
  exiting 0.
