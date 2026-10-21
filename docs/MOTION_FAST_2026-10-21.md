# BB_MOTION_FAST: a cheaper object-motion epilogue (2026-10-21, branch `bench/motion-fast`)

## Answer

**NO-GO against the bar** (vertex -0.7 ms AND flat p50 -1.0 ms). Both halves together give about
**-0.35 ms flat p50, -0.37 ms late p50 and about -0.36 ms vertex**. All of that comes from the
still twins. The plain store pays nothing (store-only run: flat +0.1, vertex +0.1, i.e. noise).
The gate stays default off. Do not ship it as a frame-time lever.

## Task 1: ceiling repeat (BB_OBJECT_MOTION=0), reproduced

Same 420 s standing route and settings as `docs/VERTEX_SHADERS_2026-10-21.md` (`vertex_ceiling.sh`,
checkout `bbport-nullrender-build` @ 105faa9). Vertex comes from `mtl_encoders.py` on the 3 s trace
taken 200 s into gameplay.

| | flat 60-110 s p50 / p99 | late last 75 s p50 / p99 | vertex ms/frame | fragment |
|---|---|---|---|---|
| `trace-c2` (control) | 19.18 / 21.12 | 19.16 / 20.87 | 3.77 | 5.33 |
| `motoff-2` | **17.47** / 19.41 | **17.65** / 19.34 | **2.37** | 4.72 |
| delta | **-1.71** | -1.51 | **-1.40** | -0.61 |

The first measurement (motoff-1: -1.6 flat, vertex 3.5 -> 2.42) holds. There were 0 asserts.

## Task 2: what the gate does (`BB_MOTION_FAST=1|store|still`, default off)

- **store** (`emit_spirv_special.cpp` EmitVertexMotion): the 4 device-scope `OpAtomicExchange` are
  replaced by one aligned 16-byte `OpStore`.
  - Contract kept: every invocation of a vertex writes identical bits.
  - Loads read the other ping-pong half only.
  - `ObjectMotion::OnFrameStart`'s VERTEX->VERTEX read|write barrier orders frame N's stores
    before frame N+1's loads.
  - The old atomics had relaxed semantics and were not atomic as a 16-byte group, so they added
    no ordering. No fence and no first-writer rule is needed.
- **still**: each motion pipeline gets a twin (`PipelineCache::LinkStillTwin`):
  - The same key, except for the stage hashes and `motion_vectors=2`.
  - The plain VS permutation, plus an FS variant (`MotionVectors::FragmentStill`) that writes
    what the motion FS writes for motion_param 0, but without varyings 30/31: (±0, ±0, 0,
    FragCoord.z).
  - `Rasterizer::DrawRecord` swaps to the twin only for motion draws whose `motion_param == 0`.
    That happens after rendering and attachments were set up with the motion pipeline, so the
    twin draws into the same pass.
  - Twins are compiled once per motion pipeline, synchronously (26 per session).
  - They are session-only: neither the twins nor fast-store VS permutations are written to the
    pipeline store.
  - The key check refused 2 twins per session (their key differed beyond the stages). Those draws
    keep the motion pipeline.
  - Result: 99.4% of still draws ran on a twin (2,605,620 vs 15,820).
- **Dump fix:** `BB_DUMP_TRIGGER` never fired on the scaled FSR 3 path that this preset uses
  (live resolution). `RunScaled` now dumps `motion` (rg16f), `objects` (rgba32f) and `depth` too.

### Gate-off identity

- Pipeline store after all runs: the same 1700 file names as 105faa9's.
- All **418 programs have the same SPIR-V permutation md5 sets**. That includes all 26 motion VS
  permutations, each still with its 4 atomics; the fast store never leaked into the store.
- The gate-off runs rewrote 24 `.spv` files, and every one is in 105faa9's md5 set:
  - 21 are byte-identical by name.
  - 3deaa573_5/6/7 swap indices; they differ only in binding numbers, which is permutation
    ordering, not new code.
- Per-draw decisions are unchanged. Gate-off runs log 0 `Motion fast` lines, and the
  `Object motion:` counters match the control (~50-51k draws stored and ~82k still per 600
  frames, ~2.34 M vertices/frame).
- `.key`/`.meta` blobs differ at scattered bytes between sessions; they are serialised as raw
  structs. **Not shown here** whether 105faa9 alone does the same. I did not run a 105faa9 vs
  105faa9 store diff.
- A pixel-identical motion image across runs is impossible even off vs off (see below). The bar
  used is the same-mode noise floor.

## Task 3: A/B/B/A (`motion_fast_ab.sh`, checkout `bbport-motionfast-build` @ 43ccb27)

| run | flat p50 / p99 | late p50 / p99 | vertex | fragment | render enc/frame | trace frames |
|---|---|---|---|---|---|---|
| off-1 | 19.03 / 20.00 | 19.18 / 26.39 | 3.34 | 4.69 | 184 | 32 |
| on-1 | 18.62 / 21.95 | 18.88 / 20.56 | 3.12 | 4.89 | 189 | 38 |
| on-2 | 18.69 / 20.67 | 18.77 / 20.98 | 3.24 | 5.17 | 199 | 35 |
| off-2 | 18.96 / 20.97 | 19.20 / 21.01 | 3.75 | 5.25 | 206 | 49 |
| **on - off (means)** | **-0.34** | **-0.37** | **-0.36** | +0.06 | | |
| store-1 (store only) | 19.10 / 20.73 | 19.48 / 20.72 | 3.68 | 5.16 | 197 | 39 |
| still-1 (still only) | 18.58 / 20.35 | 18.72 / 20.49 | 3.37 | 5.32 | 200 | 37 |

- Draws are 885-895 in every run, and every run is clean: 0 `gcn_to_vk.cpp:422`, 0
  `cp_graphics.cpp:203`, 0 overlap, 0 watchdog.
- A 3 s trace has about ±0.2 ms of vertex noise, so the per-run vertex numbers can only show a
  direction.
- Still-only alone gives -0.42 flat. Store-only gives about 0.
- Why still pays less than the ceiling: the ceiling removes vertex work AND the 8th RGBA32F
  attachment plus its per-pass load/store. The twin keeps the attachment, because it must keep
  writing depth and the "no motion" marker into it. Most of the ~1.7 ms sits in that
  attachment, not in the vertex epilogue.

### Motion image A/B (`motion_dump_compare.py`, frames 0-1, dumped 120 s into gameplay)

Two notes before the numbers:

- **Camera pose groups.** The game parks the camera at one of two bit-patterns per session:
  off-1, off-2 and still-1 share one; on-1, on-2 and store-1 share the other.
- **No object motion in this standing pose in either mode.** Gate off has 0 pixels with
  valid > 0.99, and x/y in the object attachment are exactly 0 everywhere. The resolved image
  FSR reads is pure camera fallback in both modes.

Resolved `motion` (rg16f), |dxy| in pixels:

| comparison | mean | p99 | max |
|---|---|---|---|
| same mode, same pose (off-1 vs off-2; on-1 vs on-2) | 1.9-2.0e-5 | 7-8e-5 | 1.6e-4 |
| **gate on vs off, same pose** (still-1 vs off-1/off-2; store-1 vs on-1/on-2) | 1.9-2.0e-5 | 7-8e-5 | 1.4-1.6e-4 |
| any pair across pose groups (incl. off vs off-pose-B, store-1 vs still-1) | 1.05e-4 | 3.2e-4 | 5e-4 |

So the vectors FSR consumes are **unchanged within the same-mode noise floor** when the pose
matches. The cross-group difference follows the camera pose, not the gate.

The raw `objects` attachment differs by mode. Gate off writes w != 0 on ~512k of 550k pixels;
gate on writes ~212k. Its z/w values match neither expected layout in either mode: w is never
equal to the depth buffer, and z holds small nonzero values. I could not explain that dump in
the time available. The compose accepts 0 of its pixels either way.

**Limitation:** this route has no moving objects, so it cannot show that the vectors of animated
draws are unchanged. The twin only ever runs for draws whose `motion_param` is 0. By
construction that leaves the motion-pipeline path for moving draws untouched under `still`, but
it is not a measurement.

## BB_OBJECT_MOTION=0 crash (title screen, SIGSEGV at 0x0)

- **Not reproduced.** 3 title->gameplay runs (`out/vertex/crash/`) plus motoff-2 ran clean.
- Symbolised with atos against the same library: `GetProgram+552` = `pipeline_cache.cpp:1367`,
  the `FetchCodeDecodes(fetch)` call. `fetch` comes from `GetFetchShaderCode()` (a pointer
  read out of the draw's user data). The `fetch &&` test guards only `program_write_sync`, not
  the decode.
- So a null fetch-shader pointer in the title's user data crashes on any path that selects
  that program. It is not specific to motion-off; that run only happened to hit it.
- The gate cannot add exposure: `LinkStillTwin` re-selects the same registers right after the
  regular selection succeeded on them.
- Fix (not applied, outside this change): `if (!fetch || !FetchCodeDecodes(fetch))` -> skip
  the draw.

## Artifacts

- `out/motionfast/{series.log,attempts.jsonl,run-*,foot-*}`
- `out/frames-*.csv`, `out/mtl-*-gpu.xml`, `out/dump-*/`
- Ceiling repeat: `bbport-nullrender-build/out/vertex/` (`trace-c2`, `motoff-2`, `crash/`)
- Tools: `tools/macos/motion_fast_ab.sh`, `tools/macos/motion_dump_compare.py`
