# BB_PROTECT_ASYNC: bb-gpu stops waiting for protections (2026-10-21, branch bench/protect-async)

Base: bench/protect-ipc @ 8de78fa (docs/PROTECT_IPC_2026-10-20.md). Gate `BB_PROTECT_ASYNC=1`,
default off; `BB_PROTECT_ASYNC_LOG=path` = per-frame counters. Harness
`tools/macos/protect_async.sh TAG...` (the probe's standing route, curve3440 save, fsr3 preset 1,
3440x1440, 300 s, footprint watchdog); report `tools/macos/protect_async_report.py`; raw numbers
`docs/protect-async/report.txt`, `report.json`, `attempts.jsonl`.

## Result

Hidden runs, alternated, one at a time, all attempt 1, 885-892 draws/frame, no asserts, no overlap.
Flat = gameplay t 60-110 s; late = the last 75 s.

| run | flat p50 | late p50 | late p99 | bb-gpu ipc wait flat -> late | replies awaited/frame | guest fault stall flat -> late |
|---|---|---|---|---|---|---|
| ctrl-1 (gate off) | 23.54 | 24.88 | 39.5 | 1.24 -> 5.78 ms | 118 / 117 | 3.0 -> 9.7 ms |
| async-1 | 23.80 | 24.96 | 40.4 | 0.07 -> 0.19 ms | 3.3 / 3.2 | 3.1 -> 6.2 ms |
| ctrl-2 (gate off) | 24.08 | 26.42 | 43.4 | 1.31 -> 8.71 ms | 119 / 117 | 3.2 -> 14.8 ms |
| async-2 | 23.51 | 25.83 | 43.1 | 0.03 -> 0.23 ms | 3.2 / 3.4 | 1.7 -> 6.8 ms |

- **Late phase: async - control = +0.08 ms (pair 1), -0.59 ms (pair 2); mean -0.25 ms.**
  The 3 ms bar is missed by an order of magnitude. Flat phase: +0.26 / -0.57, mean -0.15 ms:
  no regression beyond the 0.4 ms floor.
- The mechanism worked as built: bb-gpu's waits on the game process fell from ~117 round trips/frame
  (5.8-8.7 ms late) to ~3.2 barrier waits/frame (0.2 ms late); ~57 restores/frame go out with no
  reply; ~60 revokes/frame ride in ~34 revoke batches (1.77 per batch: the draw pipe hands chunks
  over often, so most flushes carry one or two).
- **So the synchronous round trip was not what climbs the frame.** Removing it took ~6-8 ms/frame
  of waiting off bb-gpu's threads and the frame did not move: in the late phase the game process
  runs at ~47 ms CPU/frame and bb-gpu's `present_wait` is ~0-2 ms, i.e. the frame is paced by
  the guest. The wait moved to the game side: the batches' send-to-applied queue time grew
  1.7-3.5 -> 7.0-8.7 ms/frame (the protect thread still cannot get a core when GXWorker/Havok/FMOD
  are all runnable), and guest threads still stall in write faults (6-7 ms/frame late, down
  from 9.7-14.8 with the sync path, but it does not show in the frame).
- The climb's onset follows the game's own CPU step, not the gate: async-1's game CPU jumped
  29 -> 47 ms at t~110 s (the others at ~210 s) and its frame still sat at 23.9-24.3 ms for
  another 100 s. Time-in-run, not protections, sets when it starts.

## Ordering: zero violations

Counters (bb-gpu `ProtectAsync`, game `pa_game`), totals over async-1 + async-2 (+ the 150 s smoke):
- read-site violations (a barrier returning with an overlapping revoke not yet applied): **0**
  over 72,559 barriers (+16,799 in the smoke);
- barriers skipped in lock-held faults (cannot wait): **0**;
- batches applied out of sequence (gap or reorder): **0** of ~1.25M + 1.26M protections;
- submission barriers (every `vkQueueSubmit` waits for all revokes decided before it): 0 violations,
  0.003-0.009 ms/frame waited.
- Same-page re-faults by one guest thread within 200 us (a restore applied late): 14 and 60 per
  300 s run (vs ~1,200 faults/s) — a cost, not a correctness issue: a late restore only keeps a
  page read-only longer than decided.

The async restores are not unsafe in the sense the brief feared: a restore only ever gives write
access back later than the synchronous path did, never earlier, and the protect thread applies
everything in decision order, so no revoke is overtaken.

## What was built (all behind the gate; with it off the old path runs, proved below)

- `gpu/shim/bbport_protect_async.h`: three null hooks (Barrier, BarrierAll, Flush).
- `remote_back.cpp` `ProtectAsync`: one sequenced batch (`MsgProtectBatch`, no reply, up to 168);
  restores/trap-offs send it at once, revokes/trap-ons wait for a flush (Scheduler::HandOver, a
  restore, a full batch, a flip) and stay `pending` until the game publishes `pa_applied`.
  Barrier(range) waits for the newest overlapping pending revoke; BarrierAll for all. The fault
  handler sends its restores at the end and publishes `pa_fault_seq`; lock-held faults keep the
  reply path and the protections are also appended (applied again in order).
- `remote_front.cpp`: `ApplyProtectBatch` on the protect thread (publish + wake); after a fault's
  reply the guest thread waits until `pa_fault_seq` is applied (not when it holds the runtime lock).
- Barrier sites: `MemoryManager::CopySparseMemory` and `ReadBacking` (every CPU copy/readback of
  guest memory), `TextureCache::RefreshImage` (hashes guest memory; UpdateImage's TrackImage
  precedes it), `Scheduler::QueueSubmit` (BarrierAll: GPU-side copies from guest memory). Flush at
  `Scheduler::HandOver`.
- The submit/copy protocol (MsgSubmitGfx waits for bb-gpu's copy) is untouched.
- Frame-log counters: `BB_PROTECT_ASYNC_LOG` CSV per flip (protect calls sent, batches, replies
  awaited, restores without reply, revokes batched and per batch, wait ms, submit waits, barriers,
  violations, game-side queue/service ms, fault waits, re-faults, out-of-order) and one
  `Protect async:` stats line per stats window. The frame log's `ipc_ms/ipc_calls` count the
  barrier waits with the gate on.

**Gate-off proof:** ctrl-1 and ctrl-2 were given `BB_PROTECT_ASYNC_LOG` and wrote no file and zero
`Protect async` lines (attempts.jsonl `pa_csv 0, pa_lines 0`), and kept ~118 synchronous calls/frame.

Known gap (pre-existing, not widened): `TextureCache::ExpandImage` refreshes the new image before
`TrackImage`, the same order as the synchronous build.

## Verdict

Hypothesis refuted as a frame-time lever. Keep the gate default off; do not ship it. The late
climb is the guest's own CPU saturating the cores (the protect thread and the guest's faulting
threads both starve); bb-gpu waiting was a symptom on a thread that was not the critical path. The
next probe is on the game process side: QoS/priority for `bb:remote-protect` and the fault path,
or why the guest's CPU steps from ~29 to ~47 ms/frame partway into a standing run.

After the series: no bb-gpu/bb-probe, free+inactive 19.2 GiB.
