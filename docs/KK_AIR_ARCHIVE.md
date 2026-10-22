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
| `BB_KK_ARCHIVES=a:b:...` | Loaded once with `newArchiveWithURL` (base first, then the player's). Every render/compute PSO gets `MTL4CompilerTaskOptions.lookupArchives`, so a miss compiles normally. Archive-only creation is never used. An unloadable archive is skipped and logged. |
| `BB_KK_ARCHIVE_CAPTURE=1\|<path>` | Puts a `CaptureBinaries`-only serializer on the compiler. `CaptureDescriptors` is not used because it hits the 27.2 bug. The archive is written to `<path>`, or `$BB_KK_AIR_DIR/capture.mtl4archive` when the value is `1`, via tmp+rename. This happens on the last compiler release and also 5 s after the last new pipeline: bb-gpu ends with `_Exit`, so the release never runs there. With an AIR dir, every library compiled from source is dumped as `<key>.metal` and gets a line in `manifest.txt`: `<key> -std=metal4.1 -fmetal-math-mode=fast -fmetal-math-fp32-functions=fast`. |

**AIR key:** BLAKE3, the hash Mesa's disk cache uses (`util/mesa-blake3.h`). It covers the tag
`bbport-kk-air-1`, the `{msl_version, math_mode, fp32_functions}` triple and the MSL text, in hex. The
MSL language version that `kk_physical_device.c:1071` picks (4.1 on macOS ≥ 27) is part of the key.
The manifest records the matching `xcrun metal` flags, so an AIR file can only be found for the exact
source and options it was compiled from.

**Off path:** when all three are unset, `bb_air()` returns `enabled == false` and `task_options == nil`.
That is the same `compilerTaskOptions:nil` as upstream: no serializer, no thread, no file access, no
log line. Taking the capture rwlock is gated on the serializer. This has not been proven by a run yet
(see the open bar).

Log lines when enabled: `KK archive: loaded|capturing|captured N pipelines ... (bytes, ms)`, plus
`KK air: H libraries from AIR, M compiled from MSL, B AIR files unusable; P pipelines` (every 2 s
while it changes).

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

## Bar (open)

- `mtl_new_library` median ≤ 1 ms (from 142.7).
- Area driver total ≤ 2 s (from 33 s).
- 0 failed pipelines.
- Captures within the ctrl-0008-vs-ctrl-base floor.
- ctrl-0008 (env unset) no slower than ctrl-base and logging no `KK air`/`KK archive` lines.
