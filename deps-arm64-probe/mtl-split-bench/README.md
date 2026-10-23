# mtl-split-bench — does MTL4CommandAllocator capacity scale with command buffers per reset?

Standalone, headless Metal 4 probe (no window, no game, no driver). Models KosmicKrisp's gfx
slot (`kk_cmd_buffer.c`): one `MTL4CommandAllocator`, `reset` once (as at
`vkBeginCommandBuffer`), then N `MTL4CommandBuffer`s created with `newCommandBuffer` and begun
on that allocator sequentially (as `mtl_new_command_buffer` + `mtl_begin_command_buffer`), each
with render encoder(s) carrying D draws, `endEncoding` with the `kk_stop_encoder` producer
barrier, `endCommandBuffer`, commit, wait, then `allocatedSize`.

Per draw it encodes what the driver emits per `vkCmdDraw*`: setRenderPipelineState,
setDepthStencilState, setCullMode, setFrontFacingWinding, setViewport, setScissorRect,
argument-table `setAddress` + `setArgumentTable:atStages:VS|FS`, `drawIndexedPrimitives`.
`--compute` swaps in a compute encoder with one setComputePipelineState+dispatch per "draw"
(pre/post-gfx shape).

Every row: a **fresh** allocator (reset → N CBs) and a **reused** allocator (same object across
the whole sweep, to show high-water behaviour).

## Run

```sh
./run.sh        # builds, writes raw.txt (ROW lines) and results.txt (table + fits); ~5 s
```

Built/run on Apple M4 Max, macOS 27.2, Xcode 27.0.

## Results (3 reps per point, spread = 0 B on every point — allocatedSize is deterministic)

```
allocatedSize right after reset (every run): 25784 B

sweep             N_cb  encs   draws     fresh_MB   spread_B      MB/cb  reused_hwm_MB
A_const_total        1     1   10200       10.528          0     10.528        608.200
A_const_total        2     2   10200       12.838          0      6.419        608.200
A_const_total        4     4   10200       17.524          0      4.381        608.200
A_const_total        8     8   10200       26.764          0      3.346        608.200
A_const_total       16    16   10200       44.852          0      2.803        608.200
A_const_total       32    32   10200       81.028          0      2.532        608.200
A_const_total       51    51   10200      124.528          0      2.442        608.200
A_const_total      102   102   10200      242.345          0      2.376        608.200
A_const_total      256   256   10200      608.200          0      2.376        608.200
B_200_per_cb         1     1     200        2.467          0      2.467        624.977
B_200_per_cb         2     2     400        4.908          0      2.454        624.977
B_200_per_cb         4     4     800        9.791          0      2.448        624.977
B_200_per_cb         8     8    1600       19.556          0      2.444        624.977
B_200_per_cb        16    16    3200       39.085          0      2.443        624.977
B_200_per_cb        32    32    6400       78.145          0      2.442        624.977
B_200_per_cb        51    51   10200      124.528          0      2.442        624.977
B_200_per_cb       102   102   20400      249.030          0      2.441        624.977
B_200_per_cb       256   256   51200      624.977          0      2.441        624.977
B50_50_per_cb        1     1      50        2.401          0      2.401        608.200
B50_50_per_cb        2     2     100        4.777          0      2.389        608.200
B50_50_per_cb        4     4     200        9.529          0      2.382        608.200
B50_50_per_cb        8     8     400       19.031          0      2.379        608.200
B50_50_per_cb       16    16     800       38.037          0      2.377        608.200
B50_50_per_cb       32    32    1600       76.048          0      2.376        608.200
B50_50_per_cb       51    51    2550      121.185          0      2.376        608.200
B50_50_per_cb      102   102    5100      242.345          0      2.376        608.200
B50_50_per_cb      256   256   12800      608.200          0      2.376        608.200
C_compute_const      1     1   10200        3.401          0      3.401        134.244
C_compute_const      2     2   10200        3.860          0      1.930        134.244
C_compute_const      4     4   10200        4.679          0      1.170        134.244
C_compute_const      8     8   10200        6.710          0      0.839        134.244
C_compute_const     16    16   10200        9.987          0      0.624        134.244
C_compute_const     32    32   10200       17.852          0      0.558        134.244
C_compute_const     51    51   10200       28.436          0      0.558        134.244
C_compute_const    102   102   10200       53.503          0      0.525        134.244
C_compute_const    256   256   10200      134.244          0      0.524        134.244
D_1cb_draws          1     1       0        2.401          0      2.401          2.401
D_1cb_draws          1     1     100        2.401          0      2.401          2.401
D_1cb_draws          1     1     200        2.467          0      2.467          2.467
D_1cb_draws          1     1     500        2.664          0      2.664          2.664
D_1cb_draws          1     1    1000        3.106          0      3.106          3.106
D_1cb_draws          1     1    2000        3.892          0      3.892          3.892
D_1cb_draws          1     1    5000        6.334          0      6.334          6.334
D_1cb_draws          1     1   10200       10.528          0     10.528         10.528
D_1cb_draws          1     1   20000       18.507          0     18.507         18.507
D_1cb_draws          1     1   50000       42.837          0     42.837         42.837
E_1cb_encoders       1     1       0        2.401          0      2.401          2.401
E_1cb_encoders       1     2       0        2.401          0      2.401          2.401
E_1cb_encoders       1     8       0        2.549          0      2.549          2.549
E_1cb_encoders       1    51       0        3.778          0      3.778          3.778
E_1cb_encoders       1   102       0        5.269          0      5.269          5.269
E_1cb_encoders       1   256       0        9.627          0      9.627          9.627
F_nocommit           (identical to A_const_total, byte for byte)

FITS (least squares on per-rep values)
  A_const_total    capacity_MB = 7.071 + 2.3410 * N_cb   (R^2=0.999917)
  B_200_per_cb     capacity_MB = 0.026 + 2.4412 * N_cb   (R^2=1.000000)
  B50_50_per_cb    capacity_MB = 0.026 + 2.3757 * N_cb   (R^2=1.000000)
  C_compute_const  capacity_MB = 2.263 + 0.5134 * N_cb   (R^2=0.999761)
  joint render     capacity_MB = 0.359 + 2.3303 * N_cb + 0.000683 * draws   (R^2=0.999870)
  D (1 CB, >=1000 draws) capacity_MB = 2.276 + 0.000811 * draws  (R^2=0.999999) -> 811 B/draw
  E (1 CB, 0 draws)    capacity_MB = 2.346 + 0.028457 * encoders (R^2=0.999946) -> 28457 B/encoder

Block attribution (G rows):
  empty CB (begin/end, 0 encoders):       +32768 B per CB  (58552 = 25784 + 32768)
  1 empty compute encoder per CB:          550072 B  (N=51: 26764472 = 25784 + 51*524288)
  1 render encoder per CB, <=~100 draws:  2401464 B  = 25784 + 2375680 (= 145 * 16 KiB) per CB
```

## Reading

* **The capacity is a function of the number of command buffers** encoded between resets.
  Each render-encoder command buffer takes a fixed ~2.376 MB block (2,375,680 B) the moment it
  has a render encoder, whether it carries 0 or ~100 draws. Draws only grow it past ~100–200
  draws per CB at ~811 B/draw; extra render encoders inside one CB cost ~28 KB each. A compute
  CB costs ~0.52 MB. Nothing is recycled across CBs within one reset window (fresh blocks per
  CB), and capacity is fixed at encode time (no-commit is byte-identical).
* **Floor:** 25,784 B after reset; ~2.40 MB for one render CB. Same 10,200 draws cost 10.5 MB in
  1 CB vs 124.5 MB in 51 CBs (11.8x).
* **High-water:** a reused allocator keeps its max forever (`reset` does not shrink it) — one
  51-CB recording pins ~124 MB on that allocator for its lifetime.
* **Game:** 51 render CBs x 2.376–2.441 MB = 121–125 MB, which accounts for ~92–94% of the
  measured 132.7 MB per gfx allocator (residual ≈ draws beyond ~200/CB or extra encoders).
  At 1 CB per recording the same ~10k-draw worst recording is ~10.5 MB → 86 x 10.5 MB ≈ 0.9 GB
  vs 10.15 GB today; at the 11.7 average split (if the high-water were set by an average
  recording) ~28.6 MB → ~2.5 GB. The deferred-recording / hoisting route has a real ceiling:
  ~10x on the gfx allocators.
