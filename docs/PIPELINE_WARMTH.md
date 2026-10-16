# Pre-warming pipelines: where the first-visit stutter comes from

Why a warm run (`BB_WARM_ONLY=1`) does not stop the stutter when an area is entered for the first
time, what each cache layer actually covers, and how to fill the pipeline cache from an area
deliberately. Measured on bbport 8a3081d, macOS 27.2, M4 Max, KosmicKrisp 26.2.99.

## How a pipeline comes into being

A graphics pipeline is created at the first **draw** that has a given state key — the shader pair,
the render state, the vertex layout, the render targets — not when an area loads (`PipelineCache::
GetGraphicsPipeline`, `RefreshGraphicsKey`). Two draws of the same area with a different moon phase,
a different weapon, a different insight level or a different combat VFX are different keys and
different pipelines. So no amount of pre-loading can enumerate an area's pipelines: they have to be
drawn once, by something, before they can be cached.

## The pipeline cache on disk, as it stands

`gpu/render/pipeline_store.cpp`, `gpu/render/blob_store.cpp`. One file per blob under
`<user>/cache/<serial>-arm64/`:

| file | contents | key |
|---|---|---|
| `<perm hash>.meta` | the permutation's specialization and `Shader::Info` | the permutation hash |
| `<pgm hash>_<perm index>.spv` | the translated SPIR-V | program hash + permutation index |
| `g_<hash>.key` / `c_<hash>.key` | a graphics/compute pipeline's key and what recreating it needs | the pipeline key's hash |
| `profile.bin` | a tag plus the three version constants plus the device profile | — |

Measured state of the cache in use here (`CUSA03173-arm64`, 1129 files, 12 MB): 326 `.key`, 401 `.meta`,
401 `.spv`, all at the current versions, **no key with a missing metadata entry, no duplicate
metadata, two orphaned metadata entries**. It is consistent, not corrupt.

What the warm-up compiles: **everything in the store, not a subset.** `PipelineCache::WarmUp` walks
every `.key` blob (`ForEachBlob(BlobType::PipelineKey)`) and rebuilds each pipeline it holds. There
is no boot scoping. Measured on the store measured here: `325 of 326 cached pipelines were preloaded`.

### Entries do not accumulate across builds, and nothing is dropped

- A write is keyed by content, so recompiling something already stored rewrites the same file.
- The only deletion is `BlobStore::Clear()` — the **whole directory** — and it runs only when
  `profile.bin` is missing, or its tag/version constants/device profile differ: a shader-affecting
  constant bumped anywhere above means one rebuild, not a stale layer. All files in the owner's cache
  are from three runs of the same day and `profile.bin` is from the first of them, so nothing from
  before the last rebuild survived.
- The "stale pipelines" path **drops nothing.** `WarmUp` counts the entries that fail to load and
  logs a warning; every file stays (`325 of 326 ... 1 stale entries are kept`).
- 60 of the 290 graphics keys share a render-state signature with another key (28 groups), i.e. the
  same render state with different shader permutations. With 327 distinct programs in the store and
  one program holding 9 permutations, most of those are legitimately distinct pipelines, so they are
  left alone rather than guessed at.

The consequence that matters: **a version bump throws the whole gameplay set away**, and the port
bumps those constants often. What is left is what the player has re-earned by playing.

## Why the warm run warms nothing

Measured, with the counters split (`Warm run:` line): a hidden warm-only run on that store prints

```
Warm run: 0 pipelines created this run, 325 preloaded from the cache; compiles stopped - asking the game to quit
```

Zero. The `304 pipelines created` it printed before the split counted the preloaded pipelines —
`CheckPipeline` bumps the same counter for a preload as for a compile — so it read like a warm pass
while compiling nothing.

The reason it stops there: the run quits when *pipeline creation* has been quiet for 30 s, and at the
title screen there is nothing left to create. The port's own frame stats during that window:

```
Frame stats: 120.0 FPS, ... 0 shader/pipeline compiles, 0.0 ms; ... 24 draws/frame
```

24 draws a frame, no compiles — an empty 30-s window, at 45 s in, before the game reaches an area.
A warm run without input cannot get past that point, and the early quit makes sure it does not wait.
`BB_WARM_QUIET=0` turns the early quit off, so a run that *is* driven through the game ends at
`BB_WARM_SECONDS` instead.

## Which layer is cold

Three layers stand between a first draw and a pipeline:

1. the game's code → SPIR-V, in bb-gpu (the port's recompiler, `CompileModule`);
2. SPIR-V → Metal, inside KosmicKrisp;
3. the Metal pipeline state object, in Metal itself.

Layer 1 is cached only by the port's store. Layer 2 has a persisted cache of its own: the port
patches KosmicKrisp to create a Mesa on-disk shader cache
(`tools/macos/kosmickrisp-patches/0001-...`, keyed on the driver's build id), so a rebuilt driver
starts a new one but an unchanged driver reuses it — the one here is at `~/.cache/mesa_shader_cache`,
2252 files, 23 MB. Layer 3 is not covered by that cache.

`BB_FRAME_STATS=1` reports the split inside the port's control: `N shader/pipeline compiles, X ms
(Y translating, Z in the driver)`, where the translating part is `CompileModule` and the driver part
is `vkCreateGraphicsPipelines` (plus waiting for a background compile). Two hidden runs with an
empty port store, one at a time — the game compiles the same 65 things at boot and title in both:

| driver's MSL cache | compiles | total | translating | in the driver | worst frame |
|---|---|---|---|---|---|
| warm (cold port store) | 65 | 18.4 ms | 4.1 ms (22%) | 14.3 ms (78%) | 70.6 ms |
| cold (cold port store, scratch `XDG_CACHE_HOME`) | 65 | 56.2 ms | 3.9 ms (7%) | 52.4 ms (93%) | 79.6 ms |
| warm (port store warm: 325 preloaded) | 64 | 4510 ms | 35.1 ms (1%) | 4476 ms (99%) | 992 ms |

Per pipeline: ~0.28 ms, ~0.86 ms — and 70 ms in the third. What holds across all three:

- **The driver's layer is everything.** The port's own translating is 1–22% of a number that is
  already small, and it is the one part that does not move between runs (0.7–30 ms per window;
  0.06 ms per shader module). Every run's cost is `vkCreateGraphicsPipelines`, inside KosmicKrisp
  and Metal.
- **It is not deterministic, and the spread is the interesting part.** The same boot-and-title set
  cost 18 ms in one hidden run and 4510 ms in another. The worst frame tracked it: 10–20 ms in the
  quiet windows, 239 ms, 439 ms and 992 ms in the windows where the pipelines were made. So a
  first-visit stutter is this layer, and its size is set by the Metal side's state (its own pipeline
  cache, and how contended the machine is — the third run overlapped other work on this Mac and its
  `Frame stats` show 30–44% of the frame lost in host copies and almost no GPU work, which is
  consistent with contention inflating it).
- **A warmer port pipeline cache cannot help with the dominant part.** It removes at most the
  translating share, which is 1–22%. The persisted shader cache the port already patches into
  KosmicKrisp covers SPIR-V → Metal; what no cache covers is Metal's own pipeline state creation
  (layer 3), and that is where the 70 ms per pipeline and the 1 s frames are. A persisted Metal
  binary archive is the lever, not the port's store.
- Separating layer 2 from layer 3 needs a timer inside KosmicKrisp; the port's numbers cannot split
  them, and these runs were not controlled for concurrent load, so the absolute millisecond figures
  are indicative rather than comparable across runs. The ratios are the robust part.

## Async pipeline compilation

It is already here. `BB_ASYNC_PIPELINES` (on by default) compiles a new pipeline on worker threads
(`PipelineCompiler`, `BB_ASYNC_PIPELINE_THREADS`, default a quarter of the cores, 1–4) and lets the
draw go without it for a frame or two (`AsyncSkippable`). It is deliberately narrow: only a pass
drawn every frame may skip a frame — its render target must have been drawn to in each of the last 8
frames, the frame must have had at least 300 draws, and full-screen or indirect draws never skip. So
a loading screen, a cutscene, a first-visit area's one-off passes and anything drawn once compile on
the GPU thread with the game waiting. That is the right trade (a skipped draw is a visible hole), and
it is why the stutter that remains is concentrated in exactly those one-off passes.

Widening the policy — letting a pass that is not drawn every frame skip its first frames too — is
about ten lines (the three gates in `AsyncSkippable`). The thread safety of the existing mechanism
does not depend on any of them, and the reason is worth keeping if it is ever widened:

- **The worker never translates.** Every job's modules are already compiled; the worker only runs
  `vkCreateGraphicsPipelines` (`CompileInBackground`'s lambda). So `Shader::Pools` and the program
  cache, which the translation touches, stay on the drawing thread.
- `pending_graphics` is a plain `robin_map` with no lock, and that is sound only because every
  access (`GetGraphicsPipeline`, `CompileInBackground`, `FinishPipeline`) is on the drawing thread;
  the worker owns its `PendingPipeline` (mutex + `condition_variable`) and the pipeline object.
- The worker's inputs are copies (`key`, `infos`, `runtime_infos`, `modules`, `fetch`), so the
  drawing thread can carry on with its own `sel`.
- The driver's `VkPipelineCache` is synchronized by KosmicKrisp itself (`PipelineCompiler`'s
  comment); `BlobStore::Save` queues under its own mutex; the counters are atomics.
- Extending async to the *translation* is the change that would not be small: `pools`, the program
  cache and the fetch-shader SRT walk are all single-threaded assumptions today, and the walker JITs
  through `runtime_fault_recover` (a thread-local jump target). That is a senior-tier change with a
  memory-safety surface, not a policy tweak.

Anything wider — skipping draws for passes that are not per-frame — would trade the stutter for
missing geometry, and is a product decision, not a small fix.

## Filling the cache from an area on purpose

`tools/macos/warm_route.sh SECS` runs the game hidden, presses the title's Continue, then walks and
turns on a scripted pad (`BB_PAD_FILE`, `src/runtime_pad.c`), with `BB_WARM_QUIET=0` so the run is not
cut short at the title. It never touches the player's save: `<checkout>/user` is copied to
`out/warm_user` and `BB_USER_DIR` points the run at the copy — the pipeline cache comes with it, so
the run preloads what earlier runs compiled and adds the area's own pipelines to it.

```
BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/warm_route.sh 300
```

A recorded route (`BB_PAD_RECORD` / `BB_PAD_REPLAY`, F9) is better than a synthetic walk: the states
that were actually on screen are the ones worth compiling.

## What is not verified

- The port-vs-driver split (section "Which layer is cold") needs a run in which compiles happen. A
  hidden warm run at the title compiles nothing (0 compiles in every 5-s window measured), so the
  split has to come from a cold store or from a driven run.
- Whether the store's 60 duplicate-signature graphics keys are redundant pipelines or distinct
  shaders that happen to share a render state was not proven either way.
- Nothing here was measured in a visible run; every number above comes from hidden `BB_WARM_ONLY`
  runs, which do draw and present (24 draws/frame at the title, `Gnm 1.0 sceGnmSubmitDone/frame`).
