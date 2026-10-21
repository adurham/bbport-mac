# The protect/ipc term at 3440x1440: what it is (2026-10-20, branch bench/protect-ipc)

Base: bench/fsr-curve-3440 @ 0a7fa6b (= build/macos-native-0.6 tip 869b900 + the curve harness).
Probe: `BB_PROTECT_PROBE=1` + `BB_PROTECT_PROBE_LOG=path`, default off, no default changed.
Harness: `tools/macos/protect_probe.sh TAG...` (curve3440's standing route, store and watchdog);
report: `tools/macos/protect_probe_report.py`. Raw numbers: `docs/protect-ipc/report.txt`.

## What the probe splits

Gate inventory (read from code): the frame log's `protect_ms` is `BbStats::t_protect` =
`AddressSpace::Protect` (gpu/shim/bbgpu.cpp:196), whose body in bb-gpu is
`ForwardProtect` -> a synchronous `MsgProtect` call to the game process (remote_back.cpp), so
**protect_ms is ipc_ms** (the same ~116 calls; the 0.04 ms gap is the timer nesting). The
'layer memory ms: traps set/cleared/republish' line exists (frame_stats.cpp:406) but is gated
on BB_FRAME_STATS *and* frames >40 ms *and* the layer counters moving — it never prints here
because the layer memory module (BB_LAYER_MEMORY) is off on this Mac (0 trap calls, all runs).
No build-level split of the round trip existed, hence the probe:

- bb-gpu side (remote_back.cpp, `ProtectProbe`): every forwarded protection by kind
  (write-revoke / restore / trap on / trap off), pages, wall time, calling thread.
- game side (runtime_memory.c `probed_mprotect`, remote_front.cpp ProtectThread): mprotect
  syscall time/calls/pages, runtime-lock wait, protect-thread service time per message.
  So ipc round trip = service (lock + mprotect) + wake/queue (everything else: channel
  handoff, the protect thread getting a core, Rosetta).

## Runs (hidden, one at a time, ~888 draws, all attempt 1 clean, no asserts)

| run | raster | frame p50 | ipc p50 (calls) | mprotect | lock | wake/queue | faults (n, handler) |
|---|---|---|---|---|---|---|---|
| probe-p1 (240 s) | 2294x960 | 23.71 | 1.51 (116) | 0.58 ms / 116 calls / 2105 pages | 0.015 | 0.9 | 3.8 ms (39, 1.2) |
| probe-p4 (240 s) | 1146x480 | 19.46 | 1.32 (117) | 0.51 / 117 / 2085 | 0.013 | 0.8 | 3.1 ms (39, 1.1) |
| spot-p1 (240 s, 10-17 save) | 2294x960 | 24.75 | 4.27 (117) | 0.94 | — | 3.3 | 7.8 ms (39) |
| long-p1 (300 s), t 60-140 s | 2294x960 | 23.6 | 1.38 (120) = 12 us/call | 0.56 | 0.013 | 0.77 | 3.5 ms (40, 1.1) |
| long-p1, t 240-330 s | 2294x960 | 25.2 | 6.67 (117) = 57 us/call | 1.06 | 0.030 | **5.5** | 10.8 ms (40, 3.1) |
| curve-p1 (10-20 19:37), t 60-110 | 2294x960 | 24.66 | 1.29 (118) = 11 us/call | | | | 3.3 ms (40) |
| curve-p1, last 75 s | 2294x960 | 36.68 | 24.02 (113) = 217 us/call | | | | 42 ms (40, 10.3) |

Every run, every preset, every moment: **~116 protect calls/frame, ~2100-2200 pages/frame
(~60 write-revoking ~1000 pages, ~57 restoring ~1150 pages), ~39-40 write faults/frame
(~55 pages per fault), 0 layer traps, 0 redundant trap calls.** None of it changes with the
raster.

## Answers

1. **At preset 1 the 24 ms is not syscalls and not fault handling — it is waiting.** The
   mprotect syscalls cost 0.5-1.1 ms/frame in every run (they double at worst), the runtime lock
   0.01-0.03 ms, bb-gpu's fault handler 1-3 ms. The growth is entirely in the round trip's
   *wake/queue* part: 0.8 ms -> 5.5 ms in long-p1, and in curve-p1 the per-call latency went
   11 us -> 217 us with the same 113-118 calls. There is no redundant re-protection
   (0 trap calls, 0 no-op calls; pages per frame are constant).
2. **It does not scale with raster size.** Same raster, same build, same route: probe-p1 reads
   1.5 ms ipc / 23.7 ms frame, curve-p1 read 24 ms / 36.7 ms. The term is TIME-dependent inside a
   run: flat ~1.3 ms for the first ~120-200 s of gameplay, then it climbs (curve-p1 1.3 -> 24.8,
   long-p1 1.3 -> 8.0, spot-p1 1.2 -> 5.3) in lockstep with the game process's own CPU
   (game_cpu_ms 29 -> 48-51 ms per frame, i.e. 1.0 -> 1.4 cores of guest work: more of the game's
   threads (GXWorker, unnamed x3-4, FMOD, Havok) runnable at once, stealing cores from the
   `bb:remote-protect` and fault paths, which are synchronous for both bb-gpu's recording thread
   and the faulting guest threads). The curve's 300 s window put its last-75 s percentiles after
   the climb for p3/p2/p1/off and before it for p4 (p4's climb starts ~270 s), and the series ran
   back to back with the machine at 6-7 GiB free+inactive; that is what made it look like a
   raster ladder. No allocation grows; pages protected per frame are flat at ~2100.
3. **The single change (hypothesis, not built):** stop making bb-gpu's recording thread wait
   for the game process on every protection. `ForwardProtect` (remote_back.cpp) issues a
   synchronous `MsgProtect` Call for each of the ~116 protections per frame; ~60 of them come from
   the recording thread (`record_ms` = 0.75 -> 3.3 ms) and the rest from other bb-gpu threads.
   Write-*revokes* must be applied before the GPU reads the page, but restores (write access
   given back, ~57/frame) need no reply, and both can be batched per draw-pipe flush into one
   message. Code path: `AddressSpace::Protect` (bbgpu.cpp:196) -> `ForwardProtect`
   (remote_back.cpp) -> `ProtectThread` (remote_front.cpp); make restores a `Send` and coalesce
   revokes into one Call per flush. Expected effect: removes the wait from the GPU-side critical
   path when the game process is CPU-saturated; ~1 ms at the start of a run, up to the late-run
   5-24 ms. Second-order: the ~40 guest write faults/frame (each a synchronous MsgWriteFault)
   carry 3-42 ms of guest-thread stall in the same regime.
4. **The earlier campaign's preset-1 figure reproduces on today's build:** 24.19 ms
   (2026-10-17, 3440_fsr3_1 on build 4fbdafd, a 190 s run) vs **23.71 ms** probe-p1 and 24.75 ms
   on that campaign's own save (spot-p1). The old campaign never left the flat regime (190 s runs;
   its ipc stayed 1.1-2.2 ms). Neither the spot nor the build changed: the 36.7 ms belongs to the
   late regime of a 300 s run.

## Attempts

- probe-p1 attempt 0 (not counted): bb-gpu aborted at the title (signal 6) — a stack-array
  overrun in the probe's own delta table (64 slots for 69 counters), fixed to 96 before the
  real series. Not a known assert (no gcn_to_vk.cpp:422, no cp_graphics.cpp:203 PM4 type 0).
- probe-p1, probe-p4, spot-p1, long-p1: one attempt each, clean, 887-893 max draws, no overlap.
- After the last run: no bb-gpu/bb-probe, free+inactive 19.6 GiB.
