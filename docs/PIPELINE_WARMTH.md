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
cut short at the title. It never touches the player's save: the user dir is copied to
`$BB_WARM_USER` (`out/warm_user`) and `BB_USER_DIR` points the run at the copy — the pipeline cache
comes with it, so the run preloads what earlier runs compiled and adds the area's own pipelines to
it. It sets `BB_WARM_ONLY=1` itself and does not let that be overridden: window.cpp creates a hidden
window for such a run and never switches the display mode for it, so a route run cannot appear on
screen. (Before, it did not set the variable at all, so the run was hidden only if the caller had
set it — and the exit at `BB_WARM_SECONDS` in presenter.cpp only arms for `BB_WARM_ONLY=1` runs.)

```
BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/warm_route.sh 300
```

A recorded route (`BB_PAD_RECORD` / `BB_PAD_REPLAY`, F9) is better than a synthetic walk: the states
that were actually on screen are the ones worth compiling.

That is one area per invocation. `tools/macos/warmfill.sh` is the whole thing in one command, and is
what "prebuild the warmth before the game runs" means in practice.

## The warm-fill pipeline (tools/macos/warmfill.sh)

```
BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/warmfill.sh            # the measured route
BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/warmfill.sh --routes all
bash tools/macos/warmfill.sh --list-routes                                       # no game, no writes
```

Four steps, in this order:

1. **Snapshot.** `cp -cR <real user dir> out/warmfill_user` — an APFS clone, so the saves and the
   cache are copied in about a tenth of a second and cost no space (measured: the 26 MB user dir,
   14 MB of it saves, in 0.13 s). The runs point at the copy (`BB_USER_DIR`), so no route can write
   anything into the real user dir.
2. **Route runs.** One or more `warm_route.sh` runs against that copy, hidden, with the pad driven
   exactly as above. The sandbox's store is what each run preloads from and adds to.
3. **Merge.** `tools/macos/warm_merge.sh` moves what the runs compiled into the store the game
   reads. It is the only step that writes to the real user dir, and only under
   `<real user dir>/cache/<store>/`.
4. **Report.** Per route: wall clock, `new_blobs`, `dropped_blobs`, the run's own `Warm run:` line
   (pipelines compiled this run against those preloaded) and its last `Frame stats` line. Then the
   merge's counts, `out/warmfill-report.json` and the per-route logs under `out/warmfill/`.

It refuses to start when the game looks like it is running (a `bb-probe` or `bb-gpu` process — the
merge would be writing into a store another process owns), when `BB_GAME_DIR` and the usual game
locations hold no game data, when `out/bb-probe` is missing (it needs a build), when the sandbox
would be the real user dir or sit inside it, or on an unknown route name. `--dry-run` reports the
plan and the merge it would do and writes nothing. `--force` skips the running-game guard only.

### The merge is additive, and checked

The store is content-addressed — a blob's name is a hash of what it holds — so merging two stores is
a set union and a name present on both sides holds the same bytes. `warm_merge.sh` therefore:

- writes only new names; a name whose bytes are identical is left alone (not rewritten, not
  touched);
- on a name whose bytes *differ* keeps the real store's copy and counts it a conflict
  (`--prefer-sandbox` takes the sandbox's instead) — a hash collision, or two cache generations in
  one store, is not a thing to resolve silently;
- never deletes and never renames a blob; a copy goes in as `<name>.warmfill-tmp` and is renamed,
  the way the game's own writer does it (`BlobStore::WriteFile`), so nothing ever reads a short
  file;
- refuses blobs the loader would reject — a `.spv` without the SPIR-V magic or not a whole number of
  words, a `.key` or `.meta` too short to hold its header — because one damaged entry makes
  `PipelineCache::WarmUp` clear the whole store (`num_damaged` → rebuild). That is the one way this
  step could take warmth away rather than add it, and it is the check that stops it;
- treats the saves, and everything else outside `cache/`, as off limits: the merge resolves both
  sides to a user dir, refuses to run when they are the same directory, and only ever creates or
  writes `<real user>/cache/<store>/<blob>`.

Measured against the real store here (1129 files: 326 `.key`, 401 `.meta`, 401 `.spv`,
`profile.bin`), with the keys removed from one clone so the merge had the worst case to add back:
*326 blobs added, 802 identical, 0 conflicts, 0 invalid, 8.4 s*; every one of the real store's 401
`.spv` files passed the header check, so the check does not cost real blobs. The usual case — a
route's few hundred blobs into an unchanged store — is a few seconds.

### The profile gate, and a fill after a version bump

`profile.bin` is not a blob: it holds the cache format's version constants and the device profile,
and it decides whether the rest of the store is used at all (`PipelineCache::WarmUp` clears the
whole store when it does not match the running build). So:

| sandbox `profile.bin` vs the real store's | what the merge does |
|---|---|
| identical | nothing (the normal case: a fill that adds warmth, not a rebuild) |
| the real store has none | installs it — the store would otherwise be cleared at the next start, and the merged blobs with it |
| differs | reports it and leaves the real store's alone; `--adopt-profile` installs the sandbox's |

That last row is the case this is for. After a version bump the real store is from the old build:
the sandbox run (this build) sees the mismatch, clears *its* copy and rebuilds it, so the blobs it
compiled are this build's. Merging them without touching `profile.bin` changes nothing — the game
clears the store at its next start and the merge goes with it. With `--adopt-profile` the real
store's header is this build's too, so the next start preloads the merged blobs; the old entries
stay on disk as stale ones the loader skips and counts (`N of M cached pipelines were preloaded; N
stale entries are kept`). warmfill says which of the two happened, at the end of the log and in its
report: `profile.bin identical` / `installed` / `adopted` / `differs-kept`.

### Routes, and how to add one

The built-in table in `warmfill.sh`, printed by `--list-routes`:

| route | run | what it drives |
|---|---|---|
| `yharnam` | 300 s | the measured walk of an area: forward, a look right, forward, a look left, back |
| `attack` | 300 s | light attacks, a roll, a walk, a heavy, the transform: combat states a plain walk never reaches |
| `spin` | 240 s | turn in place, then look down and up: headings and the sky are their own draws |

A route is one line: `<name>|<secs>|<title_wait>|<load_wait>|<pad>|<description>`. Add one without
touching the script by putting the same shape of line in an untracked `out/warmfill_routes.conf`, or
in the file `WARMFILL_ROUTES_FILE` names; a name that appears there replaces the built-in route of
the same name. `--routes a,b`, or `--routes all`; `--secs N` overrides the length of each.

- `pad` is warm_route.sh's `<pad file contents>:<seconds>` list: button names (`cross`, `r1`, `l1`,
  `circle` ...) and `lx= ly= rx= ry=` stick values in 0..255 with 128 at rest (`ly=0` forward,
  `ly=255` back). The comma stands in for the space the pad file wants between tokens.
- `secs` is the whole run: the process quits at that mark (`BB_WARM_SECONDS`); nothing is driven
  before `title_wait + load_wait`, and warm_route.sh stops driving 15 s before the end. So a route
  needs `secs > title_wait + load_wait + 15` to walk at all; warmfill raises a shorter one (with a
  line in the log) to `title_wait + load_wait + 30` rather than run a route that sits at the title
  screen the whole time.
- `title_wait` (45 s) is the time from launch to the title's Continue; `load_wait` (75 s) is the
  load after it. Both are the slow parts of a boot; a machine where the title takes longer needs
  them raised, and if the load is not finished the pad presses into nothing.

A route for a specific place is best made by recording one instead of guessing a walk. Play as
usual with `BB_PAD_RECORD=out/route-fishing-district.pad`, press F9 where the route should begin,
walk (and fight) where the warmth is wanted, press F9 again; then add a route whose pad field is the
single entry `replay:240` — `replay` is the token `src/runtime_pad.c` arms the recording with, and
the entry's seconds are the time the fill spends there — and pass the file:
`BB_PAD_REPLAY=out/route-fishing-district.pad bash tools/macos/warmfill.sh --routes <name>`. The
recorded states are the ones that were on screen, which are the ones worth compiling.

### Durations

| step | measured / expected |
|---|---|
| snapshot the user dir (26 MB) | 0.13 s (cloned, measured) |
| a merge of ~326 blobs into a 1129-blob store | 8.4 s (measured, worst case) |
| one route run | `secs` + ~20 s of process start and shutdown: 300 s → about 5½ minutes |
| all three built-in routes | about 14 minutes of GPU time |

The runs are the whole cost, and it is the same compile work the first visit to that area would have
done while the owner was playing — moved off the play session and into a hidden one. Do not start a
fill while playing, and do not start one while another build is running: the guard only knows about
the game itself.

### After a build: the hook

`tools/macos/build.sh` (and so `tools/macos/update.sh`, which the Mac app's Update button runs)
carries the warm-fill as a step that is **off by default**:

```
BB_WARMFILL_AFTER_BUILD=1 bash tools/macos/build.sh          # and BB_WARMFILL_ROUTES to change the set
```

With the variable unset the build is exactly as before and nothing is read or started. Set, the end
of the build says so on a line of its own and starts one warm-fill in the background
(`nohup`, output to `out/warmfill.log`, report `out/warmfill-report.json`), so the build and the
update are not held up by five minutes of route. The fill skips itself, saying why in that log, when
the game is running or the game data is missing. The owner's one-off command is the same thing
without the variable:

```
BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/warmfill.sh --adopt-profile
```

`--adopt-profile` is the flag to use when the fill follows a version bump (see the profile gate
above); on an ordinary fill the header already matches and the flag does nothing.

### What a fill cannot cover

- **Only what the route drew.** A pipeline is made at the first draw with its state key, and a route
  walks one patch of one area. Another area, another weapon, another moon phase, an insight level, a
  cutscene, a boss's VFX: all draws that did not happen and pipelines that were not made. Coverage
  is a set of walks, and it is inherently partial; the honest target is the places the owner keeps
  returning to, walked once each.
- **Layer 3 stays cold.** Neither the store nor KosmicKrisp's own shader cache covers Metal's own
  pipeline state creation, and that is where the tens of milliseconds per pipeline are on the
  x86-64 path.
- **Not every hitch is a compile.** Runs 6 and 7 above walked an area with essentially no compiles
  (2.3–5.0 ms in the whole window) and still had 186 ms and 667 ms frames — asset streaming, texture
  upload. A fill does not touch that.
- **A fill is not an install.** The blobs live in one user dir. Delete the store, change the device
  profile, or bump the version constants and the set is gone again: the fill has to be re-run, which
  is what the hook is for.
- **The sandbox is a second copy of the store**, on the same volume. APFS clones share their blocks,
  so it is cheap, and a route that compiles a lot writes what it compiles twice — into the sandbox
  (by the run) and into the real store (by the merge).

### Tests

```
bash tools/macos/tests/test_warm_merge.sh          # the merge: 48 checks, synthetic pairs
bash tools/macos/tests/test_warm_route_drivers.sh  # warm_route.sh against a stand-in for run.sh
bash tools/macos/tests/test_warmfill.sh            # warmfill.sh end to end, without the game
WARMFILL_TEST_REAL_USER=<a user dir> bash tools/macos/tests/test_warm_merge.sh   # and against a real store
```

None of them launch the game or write to a user dir outside their own scratch tree: the first three
work on synthetic pairs, and the fourth clones whatever user dir it is pointed at, twice, checking
the merge against that store's own blobs (a clone of the owner's store was used for the numbers in
"The merge is additive, and checked" above).

## What is not verified

- **No route has been run by this pipeline yet.** Everything above about the merge, the snapshot, the
  hook, the refusals and the pad timing was measured or checked as stated; what was *not* run is a
  fill against the real game — `warmfill.sh` end to end with a real route compiling real pipelines.
  That needs the machine to itself for about ten minutes of GPU time, game data and a build, and it
  is the one step left to the owner. `bash tools/macos/warmfill.sh --dry-run` is the cheap rehearsal:
  it lists the routes, snapshots nothing and merges nothing.
- Whether a fill actually removes the stutter on the first visit to a warmed area has not been
  measured either: it needs a before-and-after play session on the same area, with the store
  swapped underneath.


- The port-vs-driver split (section "Which layer is cold") needs a run in which compiles happen. A
  hidden warm run at the title compiles nothing (0 compiles in every 5-s window measured), so the
  split has to come from a cold store or from a driven run.
- Whether the store's 60 duplicate-signature graphics keys are redundant pipelines or distinct
  shaders that happen to share a render state was not proven either way.
- Nothing here was measured in a visible run; every number above comes from hidden `BB_WARM_ONLY`
  runs, which do draw and present (24 draws/frame at the title, `Gnm 1.0 sceGnmSubmitDone/frame`).
  The route runs behind runs 4–7 were hidden only if whoever ran them had set `BB_WARM_ONLY`
  themselves: `warm_route.sh` did not set it until the warm-fill work landed it in the script (see
  "Filling the cache from an area on purpose").
