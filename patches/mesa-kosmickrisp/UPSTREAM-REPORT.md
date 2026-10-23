# Report for Mesa upstream (KosmicKrisp): Metal command-allocator memory

Ready to paste into a GitLab issue/MR on `gitlab.freedesktop.org/mesa/mesa` (KosmicKrisp lives in
`src/kosmickrisp` since Mesa 26.0). The GitHub mirror `shadexternals/mesa-kosmickrisp` is automated builds
only and has issues and discussions disabled, so it is not a venue.

## Title

kosmickrisp: per-VkCommandBuffer Metal command allocators pin ~130 MB each; capacity is set by the worst
recording's command-buffer split count

## Body

On a real application (a PS4 title translated through a Vulkan→Metal port, macOS 26, M4 Max) KosmicKrisp
holds about 10 GB of Metal command memory in `MTL4CommandAllocator`s: 86 live `VkCommandBuffer`s × ~130 MB.

Each `VkCommandBuffer` gets three allocators (pre-gfx / gfx / post-gfx, `kk_init_encoder_state`), each reset
at `vkBeginCommandBuffer`. Two properties of `MTL4CommandAllocator` make that expensive:

1. `allocatedSize` is a high-water mark for the allocator's life - a reset does not shrink it. So one
   unusually split recording fixes the capacity of every allocator in the pool.
2. Capacity scales with the number of **Metal command buffers encoded between resets**, not with the work:
   a standalone Metal 4 bench (render encoders with 0-200 draws each, 1-256 command buffers per reset,
   3 reps, deterministic) measures ~2.38-2.44 MB per render command buffer, R² 1.000000; draws add ~811 B
   each and extra encoders ~28 KB. Holding 10,200 draws constant, 1 command buffer costs 10.5 MB and 51
   command buffers cost 124.5 MB.

The application's worst recording split into 51 gfx command buffers, which is 92-94% of the measured
130 MB per allocator. The splits come from pre-gfx compute (tessellation emulation, geometry unroll) that
must run before a render pass: it cannot be encoded inside the pass, so the pass is ended and restarted in
another command buffer.

What we did downstream, in case it is useful upstream (deferred command recording, behind env flags in our
fork, default off): a render pass is recorded as an op list and replayed into one render encoder at its end
or at a mid-pass compute side op, and a draw's pre-gfx compute is hoisted into a compute encoder ahead of
the pass's render encoder on the same command buffer (with a per-pass write-set predicate and an oracle
that decodes the real bindings and aborts on any overlap). Measured on the game: worst recording 51 → 3 gfx
command buffers, allocator block 10.2 GB → 0.65 GB, gameplay 34.1 → 39.2 fps, p95 frame 43.6 → 31.8 ms,
zero order violations.

We are not proposing that patch as-is (it is large and shaped by one application's assumptions), but the
measurement above may be worth having in the driver: the cost of a split is ~2.4 MB pinned for the
allocator's lifetime, and the allocator pool size multiplies it.

Patch and write-up: `patches/mesa-kosmickrisp/` in https://github.com/adurham/bbport-mac (branch
`build/macos-native-0.6`).
