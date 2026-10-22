# The object-motion attachment's per-pass load/store (2026-10-21, branch `bench/motion-attachment`)

## Answer

**NO-GO, and TASK 3 was not built.** Removing both the LOAD and the STORE of the 8th RGBA32F
motion attachment on every G-buffer encoder moves the frame by **-0.06 ms flat p50 / -0.06 ms late
p50** (2+2 alternated). That is inside the same-mode noise: the two controls differ by 0.09 flat /
0.33 late. The projection gate was >= 1.0 ms. The ~1.7 ms that `BB_OBJECT_MOTION=0` exposes is
**not** the attachment's load/store. It sits in the motion permutations' vertex stage of the heavy
G-buffer encoders (-1.46 ms) and their fragment work (-0.46 ms). This corrects the reading in
`docs/MOTION_FAST_2026-10-21.md` ("most of the ~1.7 ms sits in that attachment").

## Split (TASK 2)

### Bandwidth, from the pass trace

- The attachment is 1146x480 RGBA32F, i.e. 8.4 MiB.
- It rides on **16.0 G-buffer encoders/frame**. These are the only `nc=8` encoders: 1146x480,
  RGBA8 + D32S8. Under `BB_OBJECT_MOTION=0` the same encoders have 6-7 attachments.
- The first encoder of the frame clears it; the other 15 load it, and all 16 store it.
- Per frame that moves **134 MiB of store + 126 MiB of load**.
- At the M4 Max's 546 GB/s that would be ~0.25 + ~0.24 ms if it were paid at full bandwidth.

### Measured: `BB_MOTION_ATTACH_PROBE=both` (loadOp/storeOp DONT_CARE on the motion view only)

The probe is timing-only and wrong by design. KosmicKrisp maps the ops 1:1 to MTLLoadAction /
MTLStoreAction DONT_CARE (`kk_cmd_draw.c` `kk_fill_common_attachment_description`,
`kk_cmd_buffer.c` `kk_get_attachment_store_op`). Whole-framebuffer encoders without the
RESUMING/SUSPENDING flags are not forced to LOAD.

The probe engaged: the dumped `objects` image is NaN garbage on every pixel in both probe runs,
against 512k written pixels in the controls.

| run | flat 60-110 s p50 / p99 | late last 75 s p50 / p99 | vertex | fragment | G-buffer enc. fragment |
|---|---|---|---|---|---|
| ctrl-1 | 19.06 / 20.53 | 19.03 / 20.06 | 3.34 | 4.77 | 0.91 |
| both-1 | 18.96 / 20.78 | 19.21 / 20.36 | 3.64 | 5.01 | 0.95 |
| both-2 | 19.13 / 25.35 | 19.07 / 20.23 | 3.41 | 4.77 | 0.86 |
| ctrl-2 | 19.15 / 19.94 | 19.36 / 21.08 | 3.87 | 5.25 | 1.04 |
| **both - ctrl (means)** | **-0.06** | **-0.06** | -0.08 | -0.12 | **-0.07** |
| ref: bench/motion-fast off-1 / off-2 | 19.03 / 18.96 | 19.18 / 19.20 | 3.34 / 3.75 | 4.69 / 5.25 | 0.93 / 1.05 |

So **load + store together ≈ 0.06-0.12 ms** (the frame and fragment deltas). That is at the
noise floor, and 4-8x under even the raw-bandwidth estimate. The likely reason is Apple's lossless
render-target compression of an image that is almost entirely zeros: only 95 pixels are valid on
this route. That is an inference; it was not measured.

The **load-only and store-only arms produced no valid run** (5 attempts). The machine-wide
memory watchdog (free+inactive < 4 GiB from other load on the Mac, with bb-gpu at 12-13 GiB) voided
4 of them, and one died on the known `gcn_to_vk.cpp:422` (data_format 6 / num_format 5). Since
both halves together are ~0, the separate load/store split is below what this harness resolves:
**load ≤ ~0.06, store ≤ ~0.06**.

### Where the 1.7 ms actually is (control vs `BB_OBJECT_MOTION=0`, existing traces trace-c2 / motoff-2)

| encoders (by vertex time) | enc/frame ctrl → motoff | vertex ms ctrl → motoff | fragment ms ctrl → motoff |
|---|---|---|---|
| ≥ 100 us (the G-buffer) | 9.2 → 6.1 | 2.392 → 0.936 (**-1.46**) | 1.104 → 0.640 (**-0.46**) |
| 20-100 us | 8.7 → 9.8 | +0.09 | 0.00 |
| < 20 us | 184 → 177 | -0.04 | -0.15 |

The attachment's load/store is at most ~0.1 ms of that 0.46 ms fragment delta (measured above).
The rest of the fragment delta is the motion FS writing a 16-byte RGBA32F output per fragment
on top of 7 guest targets. The vertex delta is the motion permutations themselves: the history
store/load, 2 extra varyings, and the param loads. The still twins only take ~0.36 of it away,
because the **85 moving draws/frame carry 2.34 M vertices**, while the 137 still draws are the
light ones.

### Which passes need it (pass trace with new per-encoder `mv=` / `ms=` counts, ctrl-1 / ctrl-2 / both-1)

- **G-buffer encoders/frame carrying the attachment: 16.0-16.3.** They hold every motion
  draw: 85 moving + 137 still.
- **Encoders with no moving draw: 3.0-3.5/frame.** They hold only **6-7 of the 137 still draws**
  (~1% of the G-buffer's 9.0 M indices/frame).
- **Encoders with no motion draw at all: 0.**
- So ~130 of the 137 still draws share an encoder with moving draws: they cannot drop the
  attachment without a pass split, and a split is a measured loss (`docs/COPY_MERGE_RESULT.md`).

### Projection

- **(a) Attachment-free / DONT_CARE for the 3.2 no-mover encoders:** 3.2/16 of a ≤ 0.1 ms
  load/store ≈ **0.01-0.02 ms**.
- **Upper bound (drop load+store everywhere, measured):** **-0.06 ms**.
- **(b) Pack the vector into a spare channel of an existing target:** this saves the same
  load/store (~0.06) plus part of the fragment output bandwidth. Bounded by the whole ≥100 us
  fragment delta of motion-off, **≤ 0.46 ms**.

All are **below the 1.0 ms gate**, so TASK 3 was not implemented, and no tuning was done.

## Motion image

The resolved `motion` (rg16f) is unchanged under the probe, within the same-mode floor:

- ctrl-1 vs ctrl-2: mean 2e-5, p99 8e-5, max 1.5-1.6e-4 px.
- ctrl-1 vs both-1/2: mean 2e-5, p99 8e-5, max 1.6-1.7e-4 px.

As in motion-fast, this standing route has no valid object vectors (95 pixels with valid > 0.99),
so the compose falls back to camera motion either way. **This route cannot show a moving-object
regression**, and the probe is a timing instrument, not a candidate.

## Runs / incidents

- Settings: hidden standing route 420 s, 3440x1440, fsr3 preset 4, `frame_generation=0`, sandbox
  user dir `out/curve3440/user`, no `BB_VRAM_LIMIT_MB`, `BB_FRAME_LOG`. Each run had a 3 s Metal
  trace 200 s into gameplay with `BB_PASS_TRACE` on the same frames, plus a `BB_DUMP_TRIGGER` dump
  at 120 s (scaled path, `RunScaled`).
- Counted runs: ctrl-1, both-1, both-2, ctrl-2. All had 885-895 draws, 0 overlap, 0 asserts,
  0 watchdog and 0 `Null fetch shader:` lines.
- Voided attempts (not presented as numbers):
  - ctrl-1 #1: `Bad PM4` / `cp_graphics.cpp:203` (PM4 type 0) at the title.
  - load-1 #2: `gcn_to_vk.cpp:422`.
  - 8 attempts: watchdog on machine-wide free+inactive < 4 GiB. The bb-gpu footprint never went
    above 13.3 GiB.
- Gates: `BB_MOTION_ATTACH_PROBE=load|store|both` (default off: one cached false check).
  `BB_PASS_TRACE` lines gain ` mv=N ms=N` (motion draws with / without history).
- Tools:
  - `tools/macos/motion_attach.sh`: harness (ctrl/load/store/both/motoff/gate tags).
  - `motion_attach_report.py`: windows + trace split.
  - `motion_attach_price.py`: per-bucket ctrl vs motoff split, bandwidth.
  - `motion_pass_census.py`: which encoders carry moving draws.
- Artifacts (ignored `out/`): `out/mattach/{series.log,attempts.jsonl,report.json,run-*,foot-*}`,
  `out/frames-*.csv`, `out/mtl-*-gpu.xml`, `out/pass-*.txt`, `out/dump-*/`.

## Next

The remaining motion cost is the moving draws' vertex epilogue and the fragment output, not the
attachment's tile traffic. The one structural lever left is (b), packing: it removes the 8th
output, it is bounded at ≤ 0.46 ms, and it also needs spare channels in the guest's 7 targets,
which nobody has checked. It cannot reach a 1.0 ms bar on its own.
