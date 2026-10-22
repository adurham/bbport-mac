# A Metal pipeline archive for a compile-free first run: feasibility and price

Follows `PIPELINE_SIZING.md`: a cold area costs ~33 s for 240 pipelines, 22.3 s of it in
`mtl_new_library` (MSL → MTLLibrary, 142.7 ms per pipeline when Metal has never seen the MSL).
Measured 2026-10-21, M4 Max, macOS 27.2 (26B5112a), SDK 27.0, Metal toolchain 32023.921.
Spike: `tools/macos/binarchive-spike/` (standalone; nothing of bb-gpu or the installed KosmicKrisp
is touched). Raw CSVs: `tools/macos/binarchive-spike/results/`.

## 1. Which API

KosmicKrisp (3af1126, mesa dc41592) has **no archive code at all**: no `MTLBinaryArchive`,
`binaryArchives`, `MTL4Archive`, `lookupArchives` or `pipelineDataSetSerializer` anywhere under
`src/kosmickrisp`. Neither does the port (`gpu/`; the only MTL4 hits are imgui's Metal 4 backend).

KosmicKrisp compiles through the **Metal 4 compiler** (`src/kosmickrisp/bridge/mtl_compiler.m`):
`MTL4Compiler` (`:17-34`), `MTL4LibraryDescriptor` from MSL source (`:37-65`),
`MTL4LibraryFunctionDescriptor` (`:67-79`), `MTL4ComputePipelineDescriptor` (`:82-104`),
`MTL4RenderPipelineDescriptor` → `[MTL4Compiler newRenderPipelineStateWithDescriptor:compilerTaskOptions:nil]`
(`:207-224`). The legacy `MTLBinaryArchive` (`MTLBinaryArchive.h:61`, attached through
`MTLRenderPipelineDescriptor.binaryArchives`, `MTLRenderPipeline.h:225`) plugs into the *old*
descriptor types KosmicKrisp does not use. The matching mechanism for the MTL4 path is
(SDK `Metal.framework/Headers`):

| piece | where |
|---|---|
| capture: `MTL4CompilerDescriptor.pipelineDataSetSerializer` | `MTL4Compiler.h:51` |
| `-[MTLDevice newPipelineDataSetSerializerWithDescriptor:]` | `MTLDevice.h:1486` |
| `MTL4PipelineDataSetSerializerConfigurationCaptureBinaries` | `MTL4PipelineDataSetSerializer.h:33` |
| write: `serializeAsArchiveAndFlushToURL:error:` | `MTL4PipelineDataSetSerializer.h:76` |
| load: `-[MTLDevice newArchiveWithURL:error:]` → `id<MTL4Archive>` | `MTLDevice.h:1475` |
| use (a): `MTL4CompilerTaskOptions.lookupArchives` (compiler falls back on a miss) | `MTL4Compiler.h:75` |
| use (b): `-[MTL4Archive newRenderPipelineStateWithDescriptor:error:]` (archive only) | `MTL4Archive.h:90` |
| offline: `serializeAsPipelinesScriptWithError:` + `metal-tt` (has `-gpu-family`) | `MTL4PipelineDataSetSerializer.h:83` |

So: **build against `MTL4Archive` + `MTL4PipelineDataSetSerializer`, not `MTLBinaryArchive`.**

## 2. The spike

20 real KosmicKrisp vertex+fragment MSL pairs out of Mesa's disk cache (`extract_msl.py`: VS
40–70 KB, FS 13–125 KB of MSL, 1–7 colour targets), compiled exactly as KosmicKrisp does (MSL 4.1,
fast math, `main_entrypoint`), RGBA16F / BGRA8 / RG11B10F targets. Every row is a fresh process;
"cold" deletes the spike's own Metal cache (its embedded bundle id gives it a private
`$DARWIN_USER_CACHE_DIR/io.github.bbport.binarchive-spike`). Per pipeline, ms:

| run | library (VS+FS) | PSO | Metal total |
|---|---|---|---|
| `plain_cold` — MSL source, nothing cached | **202.1** | **34.3** | **236.3** |
| `plain_warm` — same MSL again, Metal's cache warm | 0.13 | 0.10 | 0.23 |
| `lookup_B` — MSL source, archive in `lookupArchives` | 190.2 | 1.00 | 191.2 |
| `archive_B` — MSL source, PSO from the archive | 190.8 | 0.18 | 191.0 |
| `lookup_C_with_B` — archive built from *different* MSL text (same code) | 190.9 | 0.75 | 191.7 |
| `lib_cold` — offline AIR `.metallib`, no archive | 0.47 | 32.1 | 32.6 |
| `liblookup` — AIR + archive via `lookupArchives` | 0.09 | 0.89 | **0.98** |
| `libarchive` — AIR + PSO from the archive | 0.09 | 0.08 | **0.17** |
| `liblookup_two` — AIR + two half-archives together | 0.09 | 1.01 | 1.10 |
| `liblookup_lo_only` — AIR + archive covering pipelines 0–9 only | 0.13 | 16.5 (= 10 misses × 33) | 16.6 |

Archive load 0.3–1.4 ms per archive. Serialize 17–34 ms. Archive size: 1.25 MB for 20 pipelines
(62 KB each; 218 KB zstd-19), 1.70 MB when the libraries came from source. AIR: 40 metallibs,
450 KB (11 KB per shader; 127 KB zstd-19).

What it says:

- **The archive holds the back end only** (AIR → GPU binary, the PSO step: 34 ms → 0.08–1 ms). It
  does **not** hold the front end (MSL → AIR, ~100 ms per library, ~200 ms per pipeline here):
  with MSL source in, `lookup_B`/`archive_B` still pay 190 ms per pipeline. The descriptor names an
  `MTLLibrary`, and making one from source is the front-end compile.
- **The archive does not depend on the MSL bytes**: built from MSL B, it hits for MSL C that
  differs by a comment (`lookup_C_with_B`, 0.75 ms). It is keyed on the compiled function, not
  the text. It does depend on the AIR the library holds, and on the pipeline descriptor.
- **Front end + back end together, i.e. shipped AIR plus an archive: 236 ms → 0.2–1.0 ms per
  pipeline** (240×–1 400×). Shipped AIR on its own already removes 86 % (236 → 33 ms).
- **Incremental works**: archives are read-only, but `lookupArchives` takes a list, misses fall
  back to compiling per pipeline (`liblookup_lo_only`), and two separately captured archives
  together are as good as one (`liblookup_two`). Base archive + player-side archive works.
- **Serializer bug on 27.2**: with `CaptureDescriptors | CaptureBinaries`,
  `serializeAsArchiveAndFlushToURL` returns `NO` with a nil `NSError` and writes nothing.
  `CaptureBinaries` alone works. The `.script` (pipelines script for `metal-tt`) is written in
  both cases.

One open question: the game's own timer (patch 0007) has PSO at 0.27–0.38 ms and the library at
142.7 ms, while this spike has PSO at 34 ms cold. Either the game's cold run had the back end of
many shaders already in Metal's per-user cache (keyed on AIR, which survives MSL text changes, as
`lookup_C_with_B` shows), or its shaders were smaller. That does not change the conclusion, which
is that the 22.3 s is front end, and the archive alone does not touch it.

## 3. What bb-gpu would need

Two artefacts, not one: **AIR per distinct MSL** (front end) plus **an `MTL4Archive`** (back end).

- **Producing them.** Use a warm pass over the pipeline store (the existing warm-route / store
  replay) with KosmicKrisp in capture mode:
  1. Attach a `CaptureBinaries` serializer to the compiler, and serialize to `pipelines.mtl4archive`
     on device destroy.
  2. Dump each MSL KosmicKrisp compiles to `<blake3(MSL+options)>.metal`.
  3. Compile those offline into `<hash>.metallib` with `xcrun metal` (the Metal toolchain is a
     developer-side dependency, not a player one).

  `metal-tt` plus the serializer's pipelines script can build the archive offline for other GPU
  families (`-gpu-family`); this is untested.
- **Loading.** KosmicKrisp has to take both, so the port cannot do it alone: the compiler is
  private to KK. A KK patch (0008) does this:
  - Read `BB_KK_AIR_DIR` and `BB_KK_ARCHIVES` (a `:`-separated list: base archive, then the player's).
  - `mtl_new_library` tries `<dir>/<hash>.metallib` (`newLibraryWithURL`) before compiling source.
  - Every PSO call passes `MTL4CompilerTaskOptions.lookupArchives`. Use lookup and not
    `MTL4Archive` creation directly: a miss then compiles instead of failing.
- **Player-side growth.** Run the same serializer during play into a second archive, then append
  it to the list on the next launch. Metal's per-user cache also keeps covering the player's own
  machine as it does today.

KosmicKrisp patch points (mesa dc41592 under `deps-arm64/src/mesa-kosmickrisp/externals/mesa/src/kosmickrisp`):

| file:line | change |
|---|---|
| `bridge/mtl_compiler.m:17-34` `mtl_new_compiler` | attach serializer (capture); load archives (`newArchiveWithURL`) once |
| `bridge/mtl_compiler.m:37-65` `mtl_new_library` | AIR lookup by hash before `newLibraryWithDescriptor` |
| `bridge/mtl_compiler.m:95` (compute), `:215` (render) | `compilerTaskOptions:nil` → options with `lookupArchives` |
| `bridge/mtl_compiler.h:19,23,71`, `bridge/stubs/mtl_compiler.c:13,20,109` | signatures (archive list / AIR dir; serialize call) |
| `vulkan/kk_device.c:125-165` `kk_acquire_compiler`, `:374` | one compiler per MTLDevice: archives and serializer live here |
| `vulkan/kk_device.c:167-175, 434, 476` `kk_release_compiler` | serialize the capture archive on last release |
| `vulkan/kk_shader.c:1186-1212` `kk_compile_compute_pipeline` | callers of `mtl_new_library` (1194) / compute PSO (1202) |
| `vulkan/kk_shader.c:1413-1516` `kk_compile_graphics_pipeline` | `mtl_new_library` 1441/1452, `mtl_new_render_pipeline` 1504 |
| `vulkan/kk_shader.c:1699-1745` `kk_deserialize_shader` | Mesa-cache-hit path: it recompiles the MSL (1735/1739), so this is where AIR pays off with a warm Mesa cache |
| `vulkan/kk_device_lib.c:31-46` | KK's internal meta shaders, same treatment (optional) |
| `vulkan/kk_physical_device.c:1071` | MSL 4.1 on macOS ≥ 27: part of the AIR key |

## 4. Verdict

- **Mechanism.** The Metal 4 archive is the right back-end mechanism, and the only one that fits
  KosmicKrisp's MTL4 compiler. On its own it is not enough. A compile-free first run needs
  **shipped AIR + an `MTL4Archive` + KosmicKrisp's Mesa disk cache** (MSL generation, 3.4 ms a
  pipeline).
- **Projected wall time for the 240-pipeline cold area**:
  - Metal's 22.3 s + 0.75 s PSO → ~0.25 s (≤ 1 ms a pipeline).
  - Mesa's 5.1 s → 0 with a shipped Mesa cache.
  - The port's 0.1 s stays.
  - Total: **~33 s → ~0.5 s**. Without the shipped Mesa cache, ~5.5 s. With the archive alone and
    no AIR, ~no change to the 22.3 s.
- **Cost.** Per pipeline: ~62 KB of archive and ~22 KB of AIR. For the estimated ~13.8 k graphics
  pipelines that is ~0.85 GB archive + ~0.3 GB AIR raw (fewer AIR files if MSL repeats), or
  ~0.15 + 0.08 GB at zstd-19. It is linear in pipelines covered: the 596 the store holds today are
  ~50 MB raw. Build complexity: one KK patch, a capture pass over the store, and an `xcrun metal`
  step.

**The catch.**
- **Device and OS coupling.** The archive holds GPU-architecture binaries (`__GPU_ARCH` sections)
  for the capturing device, and Apple says to use only device-compatible archives
  (`MTL4Compiler.h:74`). Whether a macOS update invalidates it is **untested**.
- **Misses are safe.** A miss falls back to a normal compile (shown above), so a stale archive
  costs speed, not correctness.
- **AIR is the portable half.** It is GPU-independent, but tied to KK's MSL text (hash key) and
  the MSL 4.1 language version, so any KosmicKrisp or port translation change that alters MSL
  invalidates it. The archive itself survived an MSL text change.
- **Serializer bug.** `CaptureDescriptors` breaks serialize on 27.2 (§2).
- **Other GPUs.** Shipping to anything but an M4-class GPU needs `metal-tt -gpu-family` (untested)
  or player-side capture.

**First step and its bar.** KK patch 0008 behind `BB_KK_ARCHIVES` / `BB_KK_AIR_DIR` /
`BB_KK_ARCHIVE_CAPTURE`, all default off:
1. Capture the archive and the MSL dump with a hidden `sizing_run.sh` warm pass, and run
   `xcrun metal` over the dump.
2. Re-run `cold_kk`: empty store, Metal's cache deleted, archive + AIR + Mesa cache present.

Bar:
- `mtl_new_library` median ≤ 1 ms (from 142.7).
- Driver-call total for the area's pipelines ≤ 2 s (from 33 s).
- Zero failed pipelines.
- No visual difference on the route's captures.
- With the archives removed, the same run is no slower than today's `cold_kk`.
