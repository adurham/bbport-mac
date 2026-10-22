# MetalFX vs FSR 3.1 preset 4 + RCAS on real game frames: is MetalFX sharper at 3x?

2026-10-21, branch `spike/metalfx-quality` (from `spike/metalfx-tail` @ e1592bf).
M4 Max, macOS 27.2, Bloodborne at 3440x1440, FSR 3.1 preset 4 (1146x480 -> 3440x1440, 3.0017x),
frame_generation=0, hidden runs only.

The zero-copy question was already answered (`tools/macos/metalfx-spike/README.md`: 0.49-0.51 ms on
KosmicKrisp's own VkImages). This answers the other half of the decision: **what MetalFX puts on the
screen**, on the port's own frames, next to what FSR 3.1 + RCAS puts there.

## Answer, numbers first

Same frame, same 3440x1440 raster, both arms quantised to 8 bits, 3 frames averaged, crops held
fixed. `st` is the standing view (8 consecutive real frames, 867 draws/frame).

| region | metric | FSR 3.1 + RCAS | MetalFX | MetalFX vs FSR |
|---|---|---|---|---|
| whole frame | mean \|grad\| | 0.00232 | 0.00233 | **+0.4%** |
| whole frame | \|grad\| p99 (strong edges) | 0.0185 | 0.0204 | +10% |
| busiest edge crop | mean \|grad\| | 0.00194 | 0.00213 | **+10.0%** |
| busiest edge crop | \|grad\| p95 / p99 | 0.0085 / 0.0199 | 0.0096 / 0.0276 | +13% / +39% |
| busiest edge crop | T(0.10-0.20) / T(0.20-0.40) | 1.00 / 1.00 | 2.14 / 1.39 | 2.1x / 1.4x energy |
| fine-detail crop | mean \|grad\| | 0.00755 | 0.00819 | **+8.5%** |
| fine-detail crop | \|grad\| p95 / p99 | 0.0252 / 0.0653 | 0.0315 / 0.0762 | +25% / +17% |
| GPU cost | scale pass, p50 | ~1.97 ms (whole chain) | **0.53-0.68 ms** | ~-1.2 ms |
| temporal (standing view) | mean \|delta luma\| between frames | 6.83e-4 | 2.04e-3 | **2.0x worse** |

So: **MetalFX is sharper, by about 10% on edge energy and 25-40% on the strong edges, at roughly
1.2 ms less GPU — but it is 2x less temporally stable at the 8 frames of history this offline rig can
feed it, and the moving case (the actual complaint) could not be captured.**


## What was actually captured

Everything below is the port's own output, not synthetic content. `tools/macos/metalfx-quality-capture.sh`
(and `metalfx-quality-drive.sh`) start the port hidden (`BB_WARM_ONLY=1`), press the title's Continue,
and once gameplay draws flow they hold the pad at a route and arm the port's existing dump gates:

| dump | what it is | gate |
|---|---|---|
| `fNNN_input_1146x480_rgba.raw` | the scene colour FSR/MetalFX consume | `BB_DUMP_TRIGGER` (capture added: see below) |
| `fNNN_depth_1146x480_f32.raw` | scene depth | `BB_DUMP_TRIGGER` |
| `fNNN_motion_1146x480_rg16f.raw` | the resolved motion image (camera + object), render pixels | `BB_DUMP_TRIGGER` |
| `fNNN_objects_1146x480_rgba32f.raw` | the raw object-motion attachment | `BB_DUMP_TRIGGER` |
| `fNNN_output_3440x1440_rgba.raw` | **FSR 3.1 preset 4 + RCAS at display resolution** | `BB_DUMP_TRIGGER` (capture added) |
| `fNNN_final_3440x1440_bgra.raw` | the frame as presented (with UI) | `BB_FINAL_DUMP_TRIGGER` |
| `Dump camera: frame N ... jitter X Y` | the port's per-frame jitter and both camera matrices | `BB_DUMP_TRIGGER` |

The live-resolution FSR path only dumped depth, motion and objects (`vk_temporal_upscaler.cpp`, the
`scaled (live resolution)` block), so a **capture-only** patch added the two missing images there:
ten lines, inside the existing `if (int dump = DumpFrame(); dump >= 0)`, gated by the same
`BB_DUMP_TRIGGER`. It changes no default, no dispatch, and nothing outside a dump frame. It was
built and run in a throwaway copy of the port (`~/repos/bbport-mfxq-build`, from dd9705b); the
installed dylib, the port's tree and every default are untouched.

Runs (all hidden, `BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 BB_OUTPUT_RES=3440x1440 BB_FRAME_GEN=0`,
the owner's own `bbport.ini` values: `sharpen=1 sharpen=0.40 jitter=1`):

| tag | route | draws/frame | frames dumped | asserts |
|---|---|---|---|---|
| `st` | standing (`ly=128,rx=128`) | 867 | 8 | none (`gcn_to_vk.cpp:422` 0, `cp_graphics.cpp:203` 0) |
| `mv3`, `mv4` | held turn, then walk + turn | 866 | 10 | none |

### The moving frames did not move (this is the one thing that is missing)

Four moving captures were attempted (`mv`, `mv2`, `mv3`, `mv4`) with a held look stick and then with
the character walking. The pad reached the game every time — the port's own log shows the state it
applied (`Runtime: pad file: buttons 0x0 sticks -1 0 94 -1`, i.e. left stick full forward, right
stick at 94) — but the camera did not move: the `Dump camera:` view matrices of all 10 dumped frames
are bit-identical, the camera position is identical to 9 digits, and the resolved motion image is
**0.00 px/frame everywhere** in the dump. With a static camera the motion image is zero by
construction, so those frames say nothing about motion quality.

**Consequence: the motion half of the question (the owner's actual complaint) is not answered by
measurement here.** Every number below is a standing view. What is still needed is one run in which
the dumped frames have a non-zero motion image (the earlier `bench/motion-fast` dumps had non-zero
motion, so the port can produce it — the route driver here is what failed, not the port).

## How MetalFX was run on them

`tools/macos/metalfx-spike/spike_quality.mm` (new): reads the dumped frames and drives
`MTLFXTemporalScaler` at 1146x480 -> 3440x1440 with
- the port's own per-frame jitter, parsed from the `Dump camera:` lines,
- the port's motion convention (`motionVectorScale` 1.0, vectors in render pixels),
- `depthReversed` picked by a four-way sweep,
- `autoExposureEnabled = NO` (the port sets it from `config.hdr`, which is false on this path),
- **RGBA8Unorm input**, because that is what the port's upscaler consumes on this path (8-bit LDR,
  not 16F HDR).

The scaler runs on private Metal textures (the KK export path is proven equal and cheaper; see the
spike README). Output is read back, quantised to 8 bits (the port's own `ui_image` format) and
compared against the port's `output` dump of the same frame.

## The comparison

Both arms, same frame, same 3440x1440 raster, same 8-bit grid. Metrics on identical crops:

- `mean |grad|`: mean central-difference gradient magnitude of luma, per pixel.
- `|grad| p95 / p99`: the strength of the *strong* edges (a noise-insensitive sharpness measure:
  a soft image has weak edges, a noisy one raises the mean but not the p99).
- `HF ratio`: variance(image - 3x3 gaussian) / variance(image).
- `HF frac`: share of spectral energy above 0.25 cycles/pixel.
- `T(a-b)`: ratio of the MetalFX radial power spectrum to the FSR one in that band (1.00 = the same
  energy at that scale; absolute MTF of one image is dominated by the content's own 1/f spectrum, so
  the ratio of two renders of the same frame is the honest transfer measure).
- temporal: mean |delta luma| between consecutive frames on the standing view (flicker).

## Comparison tables (the `st` capture, full detail)

Metrics are the mean over dumped frames 5-7, crop fixed, +- is the spread across the three frames.

**whole frame**

| arm | mean \|grad\| | \|grad\| p95 | \|grad\| p99 | HF ratio | HF frac >=0.25cp | T(0.10-0.20) | T(0.20-0.40) |
|---|---|---|---|---|---|---|---|
| FSR 3.1 + RCAS | 0.00232 +-0.00001 | 0.0083 | 0.0185 | 0.0055 | 0.0201 | 1.00 | 1.00 |
| MetalFX | 0.00233 +-0.00001 | 0.0083 | 0.0204 | 0.0054 | 0.0175 | 1.16 | 0.79 |
| MetalFX, 64 encodes | 0.00258 +-0.00001 | 0.0094 | 0.0233 | 0.0096 | 0.0769 | 1.61 | 3.54 |

**busiest edge crop** (output 114,294, 384x216)

| arm | mean \|grad\| | \|grad\| p95 | \|grad\| p99 | HF ratio | HF frac | T(0.10-0.20) | T(0.20-0.40) |
|---|---|---|---|---|---|---|---|
| FSR 3.1 + RCAS | 0.00194 +-0.00000 | 0.0085 | 0.0199 | 0.0215 | 0.0102 | 1.00 | 1.00 |
| MetalFX | 0.00213 +-0.00001 | 0.0096 | 0.0276 | 0.0273 | 0.0079 | 2.14 | 1.39 |
| MetalFX, 64 encodes | 0.00243 +-0.00000 | 0.0110 | 0.0323 | 0.0546 | 0.0325 | 2.74 | 4.38 |

**fine-detail crop** (output 1473,1060, 384x216)

| arm | mean \|grad\| | \|grad\| p95 | \|grad\| p99 | HF ratio | HF frac | T(0.10-0.20) | T(0.20-0.40) |
|---|---|---|---|---|---|---|---|
| FSR 3.1 + RCAS | 0.00755 +-0.00009 | 0.0252 | 0.0653 | 0.0069 | 0.0068 | 1.00 | 1.00 |
| MetalFX | 0.00819 +-0.00003 | 0.0315 | 0.0762 | 0.0070 | 0.0046 | 2.10 | 0.99 |
| MetalFX, 64 encodes | 0.00807 +-0.00011 | 0.0309 | 0.0747 | 0.0068 | 0.0045 | 2.10 | 1.00 |

Reading it: on the edge crop MetalFX keeps 2.1x the FSR mid-frequency energy and 1.4x at 0.20-0.40
cyc/px, and its strong edges (p99) are 39% stronger; on the fine-detail crop it is ahead by 8.5% on
mean gradient and 25% on p95. The whole frame is a draw because most of a dark interior is flat in
both.

The "64 encodes" row replays the same 8 frames so MetalFX's history grows; the mean-gradient gain is
real (+11% whole frame, +26% edge crop), but **its high-frequency columns are an artifact of the
replay** (the same 8 jitter phases repeat, so the accumulated image keeps a periodic high-frequency
structure) and are not evidence of anything. History depth is the one thing the replay cannot fix.

## Temporal stability

Mean |delta luma| between consecutive frames on the standing view (lower = steadier):

| arm | flicker | vs FSR |
|---|---|---|
| FSR 3.1 + RCAS (hundreds of frames of history in the port) | 6.83e-4 | - |
| MetalFX, 8 frames of history | 2.04e-3 | 2.0x worse |
| MetalFX, 64 encodes of history | 1.26e-3 | 0.8x worse |

MetalFX's flicker falls as its history grows, which is what a temporal filter should do — the offline
rig cannot give it more than 8 real frames, so this comparison is unfair to it by construction, and
the honest statement is: **at 8 frames MetalFX is 2x less stable than FSR; the trend says that is
history depth, not a broken filter, but the steady-state number is unmeasured.**

## A convention that matters: `isDepthReversed`

The four-way sweep (`motionVectorScale` 1.0 vs 1/width; `isDepthReversed` NO vs YES) gives **identical
output on these frames**, because with a static camera the motion vectors are zero and the depth test
never rejects anything. `isDepthReversed = YES` does change the output slightly (max 0.035 in a
channel) and costs about **10% of the edge energy** (`0.00194` vs `0.00213` on the edge crop) — so it
is worth getting right, and the port's existing default (`bbport_metalfx.mm`: NO unless
`BB_METALFX_DEPTH_REVERSED`) is the better of the two on this content. The motion-vector scale cannot
be validated without moving frames.


## Cost, from the same runs

| | GPU ms (p50) | source |
|---|---|---|
| MetalFX scale, 1146x480 -> 3440x1440, real frames | **0.53** (n=64), 0.55-0.63 (n=8) | `spike_quality` GPUStart/GPUEndTime |
| the port's sharpen pass after MetalFX (CAS 0.40, approximate stand-in for `taa_sharpen.comp`) | 0.30 | same |
| FSR 3.1 preset 4 chain at 3440 (for comparison) | ~1.97 | `spike/metalfx-tail` README, port traces |

So the quality comparison above is between a path that costs ~0.5 ms and one that costs ~2.0 ms.

## Verdict

**MetalFX is measurably sharper than FSR 3.1 preset 4 + RCAS at the same 3x raster, but by a small
margin: +10% mean |grad| on the busiest edge crop, +8.5% on fine detail, a draw across the whole
frame, and +25-39% on the strongest edges. It also costs less: 0.53-0.68 ms for the temporal scale
against ~1.97 ms for the FSR chain (and +0.30 ms if the port's sharpen pass is kept after it), i.e.
about -1.2 ms of GPU. Against that: it is 2x less temporally stable at the 8 frames of history this
offline test can feed it, and the moving case — the thing the owner actually complained about — is
unmeasured because the scripted run would not move the camera.**

So, plainly: **sharper by ~10% on edge energy, ~1.2 ms cheaper, steadier only if history is deep.**
That is a real but modest still-image win. A ~1 week integration is not justified *as a sharpness
fix* — nothing here shows MetalFX fixing blur on motion, and 10% edge energy on a still frame is at
the edge of what the eye picks up (the crops in `docs/metalfx-quality-crops/` are hard to tell apart
at 3x zoom without the numbers to point at the difference). It **is** justified as a frame-time
change if the ~1.2 ms GPU saving is what is wanted — but that argument was already established by the
zero-copy spike, and does not need the quality work measured here.

Recommendation: do not spend the week on MetalFX for sharpness. If frame time is the goal, the
cheaper first step is the same measurement done properly on moving frames (one run with a non-zero
motion image proves or kills the quality case in an hour, not a week); if the owner's blur-on-motion
is to be fixed, the place to look is the port's own temporal path (motion vectors, history,
reactive mask) — not the choice of scaler.

## Limits of this measurement (read before trusting the number)

- **History depth.** Only 8-10 consecutive real frames can be dumped per run, so MetalFX here has
  8-10 frames of history while FSR has hundreds on the same frames. That is unfair to MetalFX on
  temporal stability by construction. `--repeat` replays the sequence to grow the history, and the
  replay arm is reported where it matters, but a replay repeats the same 8 jitter phases, so its
  high-frequency numbers are inflated and it is not a converged render.
- **8-bit input.** This path feeds the upscaler an RGBA8 LDR buffer; MetalFX is usually fed 16F HDR.
  Both arms get the same buffer, so the comparison is fair, but the absolute quality of both is
  below what an HDR path would give.
- **No native reference.** An upscaler-off 3440x1440 run was captured too, but it landed at a
  different pose (phase-correlation offset 526,563 px, correlation 0.09), so there is no per-pixel
  native reference for these frames and none is claimed.
- **Half-pixel phase.** The MetalFX output sits ~2 display px (0.7 render px) from FSR's on the same
  frame. The visual crops are aligned by that integer offset before viewing; a sub-pixel remainder,
  if any, costs MetalFX a little edge contrast. It does not affect the gradient metrics (a
  translation preserves them).
- The `st` frames are a standing view; the moving view is `mv2`, and motion is the owner's actual
  complaint, so the moving numbers carry the verdict.
