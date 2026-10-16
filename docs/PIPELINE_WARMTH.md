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
is `vkCreateGraphicsPipelines` (or waiting for a background compile). A hidden warm run that stays at
the title compiles 31 pipelines (that is all boot and title are), and one driven into an area with
`tools/macos/warm_route.sh` compiles the area's own:

| run | path | where | compiles | total | translating | in the driver | worst frame |
|---|---|---|---|---|---|---|---|
| 1 | native (0 preloaded) | boot, title | 65 | 18.4 ms | 4.1 ms (22%) | 14.3 ms (78%) | 70.6 ms |
| 2 | native, driver cache cold | boot, title | 65 | 56.2 ms | 3.9 ms (7%) | 52.4 ms (93%) | 79.6 ms |
| 3 | **x86-64, in-process** | boot, title | 37 | 2246 ms | 30.3 ms (1%) | 2215.8 ms (99%) | 239 ms |
| 4 | **x86-64, in-process** | an area, walking | 6 | 806.5 ms | 5.3 ms | 801.3 ms | 364 ms |
| 5 | **x86-64, in-process** | an area, walking | 22 | 1682.9 ms | 19.1 ms (1%) | 1663.8 ms (99%) | 684.6 ms |
| 6 | native, store warm | an area, walking | 1 | 5.0 ms | 5.0 ms | 0.0 ms | 186 ms |
| 7 | native, store warm | an area, walking | 1 | 2.3 ms | 2.3 ms | 0.0 ms | 667 ms |

Runs 3–5 were meant to be native and were not — `warm_route.sh` did not set `BB_NATIVE_GPU` (fixed).
The two paths are told apart in the file system by the store's directory: `cache/CUSA03173` (x86-64,
the in-process driver) against `cache/CUSA03173-arm64` (native). What holds across all of them:

- **The driver's layer is everything.** The port's own translating is 1–22% of the total and it is
  the part that does not move (0.7–30 ms per window; ~0.06 ms per shader module) whether the driver's
  cache is warm or cold or the GPU process is native. Every expensive window is
  `vkCreateGraphicsPipelines`.
- **Per pipeline, the x86-64 in-process driver costs tens of milliseconds and the native one a few.**
  x86-64: 806.5/6, 1682.9/22 and 2215.8/37 ms — 37 to 134 ms each — and frames of 0.24 s, 0.36 s and
  0.68 s while they are made. Native: 0.3 ms each at boot with the driver's cache warm, 0.9 ms with it
  cold, and 2.3–5.0 ms each in the area itself (runs 6 and 7; one compile each in the windows that
  had any). The area walked draws 664–1151 draws a frame at 36–42 FPS.
- **The comparison is not clean, and the honest version of it matters.** Runs 4–5 wrote their store
  to the *other* directory (they were x86-64), so they started from an empty one and compiled the
  whole area (~540 compiles in that run); runs 6–7 preloaded the 325 in the native store and had
  almost nothing left to compile. So the per-pipeline figures are the comparable part, not the
  window totals: native-vs-x86-64 with the *same* store still needs one run (native, empty store,
  same route).
- **A warmer port pipeline cache cannot help with the dominant part.** It removes at most the
  translating share. The persisted shader cache the port patches into KosmicKrisp covers
  SPIR-V → Metal; what no cache covers is Metal's own pipeline state creation (layer 3), and that is
  where the tens of milliseconds per pipeline are on the x86-64 path, where every Metal call is
  translated.
- **Not every hitch is a compile.** Runs 6–7 walked the same area with essentially no compiles
  (2.3–5.0 ms in the whole window) and still had 186 ms and 667 ms frames. So there is a second
  source of first-visit stutter this measurement does not name — asset streaming, texture upload,
  the port's own caches — and it is worth measuring on its own.
- The runs were not controlled for other load on the machine, so absolute figures are indicative
  rather than comparable across runs; the per-pipeline scale is the robust part. Separating layer 2
  from layer 3 needs a timer inside KosmicKrisp; the port's numbers cannot split them.

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
