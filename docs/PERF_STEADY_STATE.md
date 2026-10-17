# Steady-state frame cost on the M4 Max (native line, 1440p 16:9)

Branch fix/perf-stage-a, 2026-10-16. All runs were hidden warm-route gameplay runs (tools/macos/warm_route.sh,
same route, 2560x1440, upscaler off, preset 5). Frames counted: >=500 draws, no pipeline compile.
A watcher logged any bb-probe started from another checkout during a run; every run quoted here had none.

## What the frame log's columns mean (read from the code)

- `stage_a_cpu_ms`: the CPU time (user+sys) of the GPU command thread (bb:GpuComm), sampled at each
  graphics submission. This is not the time the thread spent working.
- `kernel_ms`: the **system** CPU time of that same thread. This is not GPU time.
- `gpu_idle_ms`: that thread's wait for the game's next submission. This is not hardware idle time.
- `present_wait_ms` == `tick_wait_ms`: Presenter::PrepareFrame waiting, on the draw recording thread,
  for the GPU to finish the frame BB_FRAMES_AHEAD flips back.

## Where the frame goes (probe columns ga_*/b_*/cat_*, 1440p, ~700-900 draws)

| | ms/frame |
|---|---|
| Frame | 22-25 |
| GPU command thread (stage A) real work: decode plus pipeline lookup (pipeline lookup 4.2-5.3, ~5.6 us/draw) | ~6 |
| Stage A waiting for stage B (draining the pipe at each submission end) | 14-17 |
| Stage B (bb:DrawRec) busy running packets (user CPU 6.7-8.5; the rest is blocked in tasks) | 17-20 |
| Of that, waiting in FlipTask for the GPU (present_wait) | 11-14 |
| Binding: descriptors, buffers, textures (stage B) / guest bytes copied | 1.1-1.4 / 2.7 MB |
| State: render targets, dynamic state | 0.4 |
| Emit: replay into KosmicKrisp/Metal on 3 VkRecorder threads, plus vkQueueSubmit | 8.7-10.6 (spread over 3 threads) |

Per draw, the CPU work on the critical threads is about 7.7 us (stage A) plus 7.9 us (stage B).
The old "20 us/draw" figure was mostly the drain spin's system time (next section).

## Fixed: the drain spin

Stage A waited for stage B with a sched_yield loop: about 74,000 system calls per frame. That was the
whole of `kernel_ms`, and it kept a P-core at 100% doing nothing. It now sleeps on a futex, and stage B
wakes it once it reaches the packet stage A is waiting for. BB_PIPE_DRAIN_YIELD=1 brings the loop back.

A/B on the same build, two runs each, alternated:

| | futex (new default) | yield loop |
|---|---|---|
| GPU command thread CPU (p50) | 6.0 ms | 22.3 ms |
| Frame time (p50) | 24.3 ms | 25.5 ms |
| Frame time at 800 draws (fitted) | 23.94 ms | 24.01 ms |

The frame time does not move: the spin was a symptom, not the wall. The win is a P-core freed for the game.

## The wall: GPU fragment work at 1440p, plus bubbles

- Same route at 1280x720 (4x fewer pixels): frame 14.4 ms vs 22.1 ms at 1440p (600-750 draws).
  CPU per frame is unchanged (stage A 5.0, stage B user 6.6). The ~8 ms that resolution removes is GPU time.
- Metal System Trace, 3.9 s at 1440p (20.5 ms frames):
  - GPU busy 80% of the time.
  - Fragment 11.7 ms/frame. About 170 fragment encoders per frame; 38 of them take 0.1-0.5 ms each and account for 70% of fragment time.
  - Vertex 2.4 ms/frame, compute 2.3 ms/frame.
  - Idle gaps 4.1 ms/frame.
- Only one GPU channel is ever active: "2 channels active" covers 1% of the time. Vertex, fragment and
  compute never overlap. KosmicKrisp ends every encoder with
  `barrierAfterStages(ALL, ALL)` (kk_stop_encoder, marked "TODO_KOSMICKRISP This is probably overkill"),
  which serializes every pass.
- BB_GPU_PROFILE timestamps come out as garbage (about 1e14 ms) on KosmicKrisp; use Metal System Trace instead.

## BB_FRAMES_AHEAD

Frame time, 750-950 draws, clean runs:

| BB_FRAMES_AHEAD | Frame time |
|---|---|
| 1 (default) | 23.7 ms |
| 2 | 24.3 ms |
| 3 | 24.4 ms |

Neutral. The game's own submit lock waits for the previous frame to retire, and that absorbs the
difference (its wait rises from 17.8 to 21.1 ms). An earlier FRAMES_AHEAD=2 run that measured
43 ms/frame overlapped another agent's hidden warm-fill run. It is void.

## BB_SKIP_EMPTY_DRAWS=1 (one run)

| Draws | Off | On |
|---|---|---|
| 600-750 | 21.2 ms | 20.0 ms |
| 750-950 | 23.7 ms | 23.3 ms |

present_wait drops from 12.4 to 9.9 ms. A small win; it needs a second run, and the owner's eyes, before
it becomes the default.

## Next levers, by size

1. The GPU's single active channel. Lighten KosmicKrisp's end-of-encoder ALL->ALL barrier, or
   cut the render pass count (about 170 fragment encoders per frame). This needs a KosmicKrisp patch
   (tools/macos/kosmickrisp-patches) and a visual check, because the barrier guards real hazards.
2. GPU idle gaps (4 ms/frame): submission granularity. There are 3 guest submissions per frame,
   recorded as 4.7 command buffers each.
3. Resolution: 1440p costs about 8 ms of GPU time over 720p. MetalFX or a lower scene scale
   now pays off, since the GPU is the wall (this reverses the earlier ruling).
