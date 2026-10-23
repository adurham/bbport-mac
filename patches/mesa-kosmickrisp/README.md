# KosmicKrisp (Mesa-on-Metal) changes behind the native renderer

`bbport-kosmickrisp-dc41592-2026-10-23.diff` is the complete working state of the driver tree the native
renderer runs on. It is a patch against Mesa `dc41592` ("kk: Add sparse buffer binding", 2026-09-20) as
vendored by `shadexternals/mesa-kosmickrisp`.

Apply, from the root of an `externals/mesa` checkout at `dc41592`:

    git apply --binary bbport-kosmickrisp-dc41592-2026-10-23.diff

## What is in it

The diff carries the whole bbport series, not only the newest work: the sparse-buffer and WSI/layer
changes that predate it, the `kk_render_enc.h` abstraction (M0), the deferred command recording
(`kk_render_defer.c`, M1), and the pre-gfx hoist (M2), with their counters.

- **M0** - every Metal render-encoder call goes through `kk_render_enc.h`, direct backend by default.
- **M1** - a render pass is recorded as an op list and replayed at the pass's end or at a mid-pass
  compute side op, so it is encoded into one Metal command buffer: `BB_KK_DEFER_RENDER=1`.
- **M2** - pre-gfx compute is hoisted into a compute encoder ahead of the pass's render encoder, so the
  pass is not split for it: `BB_KK_DEFER_HOIST=1` (needs `BB_KK_DEFER_RENDER=1` and
  `BB_KK_ONE_CMDBUF=1`). The geometry-heap reset travels with the hoisted draw.

Full write-ups, measurements and the offline proof harnesses live in the port repo:
`docs/CMDBUF_ALLOCATOR_MEMORY_2026-10-22.md`, `docs/KK_GPU_WALL.md`, and
`deps-arm64-probe/defer-m1-test/` + `defer-m2-test/` there.

## Why it exists

KosmicKrisp gives every live `VkCommandBuffer` three `MTL4CommandAllocator`s and each allocator holds its
high-water mark for life. On Bloodborne's shape that came to 86 allocators x ~132 MB = ~10 GB of reserved
Metal command memory - the machine's jetsam risk. A standalone Metal 4 bench measured the capacity as
~2.4 MB per Metal command buffer encoded between resets, independent of draw count, which is why the
split count is the lever.

## Measured on the game (M4 Max, owner's settings)

| | before | after |
|---|---|---|
| gfx allocator block | 10 151 MB, median 132.7 MB | 649 MB, median 9.6 MB |
| whole allocator block | ~12.2 GB | ~750 MB |
| max gfx Metal command buffers per recording | 56 | 3 |
| gameplay fps (self-driven, fixed spot) | 34.1 | 39.2 |
| p95 frame | 43.6 ms | 31.8 ms |

Safety: `BB_KK_HOIST_ORACLE=abort` decodes the real bindings of every hoisted op and aborts if it overlaps
the pass's write set. Clean over 1,294,830 hoisted ops in a gameplay run; `BB_KK_STATS=1` prints the
counters (hoists, refusals by reason, carried encoders, order violations - all zero in the runs above).

## Building it

The port repo builds this tree with `deps-arm64/src/mesa-kosmickrisp/build-arm64` (meson + ninja,
`-O3 -DNDEBUG`); `ninja` there produces
`outputs/libvulkan_kosmickrisp.dylib` (UUID `A286EEAB-0D50-36BC-8D05-8A8950CEF2A4` for this patch).

## Known gap

A draw refused *inside* a pass still makes the M1 fallback create the pass's render encoder, so the rest
of that pass cannot hoist (reported as ENC). Removing that needs the fallback to stop creating the encoder
- a further design step, not a bug in this patch.
