# KosmicKrisp patch 0008: shipped AIR + MTL4 pipeline archives (default off)

Follows `BINARCHIVE.md`. That spike proved a compile-free first run needs three artefacts together:
precompiled AIR, an MTL4 pipeline archive, and a shipped Mesa disk cache. This patch is the driver
half. **Status 2026-10-22: the patch, tooling and harness are built and tested off-line. No game run has
measured anything yet.** Two attempts were voided: the machine was taken for another GPU benchmark, and
memory was low (compressor at 11.7 GiB, free+inactive under 4 GiB). The bar below is still open.

## What the patch does

`tools/macos/kosmickrisp-patches/0008-kosmickrisp-air-and-pipeline-archives.patch`. It touches only
`bridge/mtl_compiler.{m,h}`, `bridge/stubs/mtl_compiler.c` and `vulkan/kk_device.c` (two call sites in
`kk_release_compiler`). 0007 (the stage timers) was rebased onto 0004+ without changing what it does:
its three `KK_BBSTAT_SHADERS` hunks were already in 0004.

| env | effect |
|---|---|
| `BB_KK_AIR_DIR=<dir>` | `mtl_new_library` first tries `<dir>/<key>.metallib` (`newLibraryWithURL`). If it is missing or unusable, the MSL is compiled as before. |
| `BB_KK_ARCHIVES=a:b:...` | Loaded once with `newArchiveWithURL` (base first, then the player's). An entry that is a directory loads every `*.mtl4archive` in it, in name order (a capture's segments). Every render/compute PSO gets `MTL4CompilerTaskOptions.lookupArchives`, so a miss compiles normally. Archive-only creation is never used. An unloadable archive is skipped and logged. |
| `BB_KK_ARCHIVE_CAPTURE=1\|<dir>` | Puts a `CaptureBinaries`-only serializer on the compiler. `CaptureDescriptors` is not used because it hits the 27.2 bug. Each write is a new segment `<dir>/seg-NNNN.mtl4archive` (`$BB_KK_AIR_DIR/capture.archive.d` when the value is `1`), via tmp+rename. **`serializeAsArchiveAndFlushToURL` flushes the serializer**: each call writes only the pipelines created since the previous call. The first version rewrote one file, so the shipped archive held only the last 13 of 329 pipelines and the proof PSO p50 stayed at 15 ms. A re-capture into an existing dir appends segments. This happens on the last compiler release and also 5 s after the last new pipeline: bb-gpu ends with `_Exit`, so the release never runs there. With an AIR dir, every library compiled from source is dumped as `<key>.metal` and gets a line in `manifest.txt`: `<key> -std=metal4.1 -fmetal-math-mode=fast -fmetal-math-fp32-functions=fast`. |

**AIR key:** BLAKE3, the hash Mesa's disk cache uses (`util/mesa-blake3.h`). It covers the tag
`bbport-kk-air-1`, the `{msl_version, math_mode, fp32_functions}` triple and the MSL text, in hex. The
MSL language version that `kk_physical_device.c:1071` picks (4.1 on macOS ≥ 27) is part of the key.
The manifest records the matching `xcrun metal` flags, so an AIR file can only be found for the exact
source and options it was compiled from.

**Off path:** when all three are unset, `bb_air()` returns `enabled == false` and `task_options == nil`.
That is the same `compilerTaskOptions:nil` as upstream: no serializer, no thread, no file access, no
log line. Taking the capture rwlock is gated on the serializer. This has not been proven by a run yet
(see the open bar).

Log lines when enabled: `KK archive: loaded|capturing|captured pipelines A..B (N new) into seg (bytes, ms)`,
`KK archive: N archives passed as lookupArchives to every pipeline`, plus `KK air: H libraries from
AIR, M compiled from MSL, B AIR files unusable; P pipelines (L with lookupArchives over N archives, F
created in < 1 ms)` (every 2 s while it changes). F/P is the archive hit rate: a hit takes 0.1-0.4 ms,
a miss 6-34 ms.

The shipped Mesa cache must come from the same driver binary: Mesa's disk cache keys on the
driver's build-id, so a rebuilt driver misses every entry (a proof seeded with the old cache got
0/206 hits and 5.1 s of NIR→MSL). `kkair_proof.sh` therefore ships capture2's Mesa cache.

## Local layout (in this worktree, not committed; the port's installed driver is untouched)

- `kk-src/mesa-kosmickrisp`: a private copy of KK 3af1126 + mesa dc41592 with 0001-0008 applied (as
  commits s0006/s0007 + the 0008 working tree), built in `kk-src/mesa-kosmickrisp/build-arm64`.
- `deps-kkroot/lib/kosmickrisp`: the 0008 driver. `deps-kkroot-base/lib/kosmickrisp`: the 0001-0007
  driver, i.e. today's driver plus the timers (copied from bbport-sizing-build).
- `deps-arm64` / `deps-arm64-base`: symlink farms onto `bbport-mac-native/deps-arm64`, differing
  only in `lib/kosmickrisp`. Runs pick a driver with `BB_GPU_DEPS`.
- `out/`: bb-probe / libbbgpu / bb-gpu copied from bbport-sizing-build (fdb2327 code + timers;
  2a8019e only adds docs and the spike), with rpaths rewritten to this worktree.
  `out/gpu-arm64/bb-gpu.app` has the bundle id **io.github.bbport.kkair**, so deleting
  `$(getconf DARWIN_USER_CACHE_DIR)io.github.bbport.kkair` cold-starts Metal for these runs only.

## Tools

- `tools/macos/kkair_run.sh TAG SECS base|0008 [ENV=..]`: one hidden `sizing_run.sh` run. It waits
  for other bb-probe/bb-gpu (10×60 s) and stops if they are still there. It deletes Metal's cache for
  io.github.bbport.kkair (`METAL=keep` skips that), uses a private `MESA_SHADER_CACHE_DIR` (`MESA_SEED`
  copies a shipped one in), starts from an empty store, and takes port frame captures at 200/230/260/290 s
  (`CAPTURES`). Output goes to `out/kkair/TAG`.
- `tools/macos/kkair_build_air.sh AIR_DIR [JOBS]`: compiles each `<key>.metal` to `<key>.metallib` with
  the manifest's own flags. It is incremental, and `PRUNE=1` drops orphans. It was checked on two spike
  MSL files (60 KB → 10 KB and 126 KB → 16 KB metallib).
- `tools/macos/kkair_proof.sh [SECS]`: the whole proof. ctrl-base, then ctrl-0008 with the env unset,
  then capture1 (dumps the MSL and fills the shipped Mesa cache), then the offline AIR build, then
  capture2 (the base archive, captured against the shipped AIR), then proof, then the report. `STEPS=`
  picks a subset.
- `tools/macos/kkair_report.py`: per run it reports pipelines, the driver total, mtllib/PSO medians,
  failed pipelines, `KK air` lines, hitches and the known asserts. It also compares captures pairwise
  (proof vs ctrl-base, and ctrl-0008 vs ctrl-base as the same-mode noise floor).
- `sizing_run.sh`'s watchdog now samples **bb-gpu** (the process holding graphics memory) and parses
  `footprint`'s `NNNN MB` format. Before this it sampled bb-probe and logged an empty footprint.

## Commands

- (a) Capture a new base archive from a store: `STEPS="capture1 offline capture2" bash tools/macos/kkair_proof.sh 300`.
  The archive lands in `out/kkair/artefacts/base.mtl4archive`, with the AIR in `.../air` and the Mesa
  cache in `.../mesa`. Use `STORE=copy` to replay the owner's store instead of an empty one.
- (b) Rebuild AIR after an MSL change: the new MSL hashes to new keys. Re-run `capture1` (it dumps
  only misses), then `PRUNE=1 bash tools/macos/kkair_build_air.sh out/kkair/artefacts/air`, then `capture2`.
- (c) Load at play time: `BB_KK_AIR_DIR=<air> BB_KK_ARCHIVES=<base.mtl4archive>[:<player.mtl4archive>]
  MESA_SHADER_CACHE_DIR=<shipped mesa cache>`, with the driver built with 0008.

## Result 2026-10-22 (proof vs ctrl-0008, 300 s, hidden, Metal cache cold, empty store)

| | ctrl-0008 (env unset) | proof before the segment fix | proof |
|---|---|---|---|
| graphics/compute pipelines | 228/34 | 254/34 | 221/34 |
| PSO p50 | 17.2 ms | 15.06 ms | **0.155 ms** |
| PSO total | 5.66 s | 5.36 s | 1.10 s (180/223 under 1 ms) |
| mtllib p50 / total | 133 ms / 31.6 s | 0.47 ms / 4.16 s | 0.44 ms / 2.34 s |
| NIR→MSL / Mesa hits | 6.1 s / 0 | 1.14 s / 201 | 0.35 s / 166/223 |
| area driver total | 39.5 s | 5.78 s | **2.35 s** |
| failed pipelines | 0 | 0 | 0 |

ctrl-0008 per pipeline: 121 ms (39.5 s / 326), against 123 ms (37.9 s / 308) before. It logs no
`KK` line. What is left of the proof is coverage: 47 libraries and 43 PSOs that the capture runs never
created (an empty-store run does not walk the same pipelines twice). A union capture closes that.

## Bar

- `mtl_new_library` median ≤ 1 ms (from 142.7).
- Area driver total ≤ 2 s (from 33 s).
- 0 failed pipelines.
- Captures within the ctrl-0008-vs-ctrl-base floor.
- ctrl-0008 (env unset) no slower than ctrl-base and logging no `KK air`/`KK archive` lines.

## Union capture and generalisation (2026-10-22, `tools/macos/kkair_union.sh`)

Held-out route: `roam` (the lawnmower; never captured). Capture routes: yharnam, attack, combat,
spin, sweep, interact, warp. All runs are hidden, with an empty store, a cold Metal cache and the 0008
driver. The footprint watchdog ended every run 1.5-2.5 min after the start, at 15-20 GiB, so "the area"
here means the first area load plus about 1-2 min of walking. That is the same window as the first proof.

**Capture needs two passes.** An MTL4 archive entry keys on the library's function hash. A library
compiled at run time from MSL does not hash like the `xcrun metal` metallib of the same MSL. Tested in
isolation: a runtime-captured entry misses when the pipeline is built from the metallib, and a
metallib-captured entry hits for both. So pass A dumps the missing MSL, the AIR is rebuilt
(383 -> 438 libraries), and pass B captures with every library coming from the AIR. Pass B appends to
`base.archive.d`.

| route | pass A pipes / new MSL | pass B pipes, archive hits (< 1 ms) | new port keys vs earlier routes | new segments |
|---|---|---|---|---|
| yharnam | 298 / +0 | 329, 318 (97%) | 257 (first) | 0003-0009 (7, 11.2 MB) |
| attack | 292 / +9 | 327, 319 | 13 | 0010-0015 (6, 10.9 MB) |
| combat | 281 / +0 (A run reached 269 draws only) | 326, 325 | 1 | 0016-0020 (5, 10.9 MB) |
| spin | 329 / +11 | 317, 315 | 30 | 0021-0023 (3, 10.4 MB) |
| sweep | 342 / +9 | 332, 321 | 3 | 0024-0026 (3, 10.4 MB) |
| interact | 298 / +0 | 319, 315 | 5 | 0027-0030 (4, 10.5 MB) |
| warp | 309 / +26 | 329, 319 | 18 | 0031-0034 (4, 11.0 MB) |

The union is 327 port pipeline keys. Every pass-B run re-serializes the area-load set (58/10/247
pipelines, 10.4 MB), because the serializer captures every pipeline created, archive hits included. The
de-duplicated set (`base.archive.dedup.d`, 14 segments, 12 MB) measured the same as the full one (82 MB).

| held-out roam | ctrl (env unset) x2 | warm store (env unset) | proof (union) x2 | proof (dedup 12 MB) |
|---|---|---|---|---|
| pipelines created | 315 / 311 | 50 (rest preloaded) | 298 / 298 | 282 |
| PSO p50 / total | 17.9 ms / 5.72 s; 17.3 ms / 5.38 s | 0.35 ms / 5.38 s | 0.125 ms / 0.43 s; 0.128 ms / 0.41 s | 0.134 ms / 0.06 s |
| mtllib p50 / total | 135 ms / 32.2 s; 135 ms / 30.9 s | 0.34 ms / 25.2 s | 0.42 ms / 0.10 s; 0.44 ms / 0.11 s | 0.45 ms / 0.10 s |
| libraries from MSL | all | all | 0 / 0 | 0 |
| Mesa hits | 0/225, 0/222 | 176/447 | 205/214, 205/214 | 198/207 |
| area driver total | 40.8 s / 39.1 s | 1.81 s | **0.18 s / 0.24 s** | 0.25 s |
| frames > 50 ms | 70 (51.8 s) / 43 (47.7 s) | 21 (3.4 s) | 24 (1.77 s) / 28 (2.61 s) | 19 (2.21 s) |
| failed pipelines / asserts | 0 / none | 0 / none | 0 / none | 0 / none |

- **Generalisation.** All 248 of the proof run's pipeline keys were in the capture union. In the
  controls, 254/260 and similar were (97.7%): 6 roam keys never appeared in any capture route. The
  driver reports 310/316 and 312/318 pipelines created in < 1 ms (98%), and 0 libraries compiled from MSL.
- **Remaining gap.** 11 of 214 PSOs took ≥ 1 ms, 393 ms in total. Six of them took 26-102 ms. These
  are pipelines whose state only that walk produced (the variants a specific player position/state
  draws), plus contention. At 15-25 ms each over 8 threads, pre-creating them at startup would cost
  about 20-40 ms of wall time.
- **Captures.** Two control runs (H vs H2, ctrl vs ctrl) differ by a mean_abs of 0.035, with 13.4% of
  pixels > 0.1: the runs are not frame-aligned, so the floor is wide. Proof vs ctrl at the same marks:
  0.049 / 18.3% and 0.024 / 6.8%. The first pair is above the floor and the second inside it. The
  first proof capture is the darker frame (luma 0.038 vs 0.044), a different moment of the walk. No
  corruption was seen, but this is not a pixel proof.
