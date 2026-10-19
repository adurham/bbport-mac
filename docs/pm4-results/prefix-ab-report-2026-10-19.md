# Pre-fix A/B, 2026-10-19: does the submit race still reproduce on this machine?

Two questions, one answer each.

1. **Does genuine pre-fix code still crash under the current harness?** Arm A: 15 hidden warm runs on
   `47fa499` (the commit before the submit-wait fix), copies on, hash instrumentation off.
2. **Does the historical record hold up?** Arm C, `recon-2026-10-19.md`: it does, once one file is set
   aside - and the file that had to be set aside was the "old path" leg itself.

## Builds

| arm | worktree | branch | `git rev-parse HEAD` | binary |
|---|---|---|---|---|
| A | `/Users/adam.durham/repos/bbport-prefix-build` | `fix/pm4-prefix-ab` (new, off `47fa499`) | `47fa4997555dda8df66ed4b26a97257e245b8ea7` | `out/gpu-arm64/bb-gpu`, built 02:34 |
| D | same worktree | same + the stall diagnostic (uncommitted) | `47fa4997555dda8df66ed4b26a97257e245b8ea7` | rebuilt 02:34 with `BB_SUBMIT_STALL_MS` |
| B | `/Users/adam.durham/repos/bbport-pm4-build` | `fix/pm4-crash` | `1a773b6b25f6f8e7faca9217a81d2d8bb8256497` | built 01:24, not run (arm A produced no failure) |

`47fa499` is the parent of `9215a10` (the fix): `git rev-parse 9215a10^` = `47fa499...`. `SubmitWaitsForCopy`
does not exist at `47fa499` (`git grep` finds it only from `9215a10` on), and the better bad-packet report
(`gpu/cp/cp_diagnostics.cpp:271`, "the packets before it", the game's buffer beside the copy) does.

The harness is byte-identical to the parent's: `tools/macos/pm4_repro.sh` was added at `de65de0`, *after*
`47fa499`, so it was copied verbatim from `bbport-pm4-build` into the pre-fix worktree (`diff -q` clean).
Only the engine binaries differ.

## Configuration, and why it is not the parent's

The historical crash's bad buffer is `memory type -1` inside bb-gpu's range: a bb-gpu copy, which only
exists with `BB_COPY_GPU_BUFFERS=1`. The flag is env-only and **defaults to false**
(`gpu/shim/core/emulator_settings.h:55`), and no script in `tools/` sets it. In the fix build the same flag
gates the wait itself:

```
inline bool WaitsForCopy() { ... const bool on = copied && !(async && async[0]=='1'); ... }
```

so with the flag unset, `BB_SUBMIT_ASYNC` changes nothing - both arms send asynchronously and bb-gpu decodes
guest memory ("graphics submissions are decoded from guest memory"). The parent's runs did have it on (their
logs print `return before bb-gpu has copied them (BB_SUBMIT_ASYNC=1)`), so this is a note for reproduction,
not a defect in their record. Every run here exports `BB_COPY_GPU_BUFFERS=1`:

```
BB_COPY_GPU_BUFFERS=1 BB_SUBMIT_CHECK=0 PM4_SECS=180   (pre-fix is async by construction)
```

Two runs were taken before that was noticed (copy off): both clean, both valid, recorded in the JSONL with
`"counted_in_15": false` and excluded from the 15.

## Arm A - 15 runs, pre-fix, copies on, no hash check

Routes `attack, combat, interact, street` x3, then `attack, combat, interact` once. Every run is a hidden
warm boot (`warm_route.sh`), sandbox store cloned from `~/repos/bbport-mac-native/user`, 180 s each,
`out/pm4_prefix/series.log` for the timestamps.

**15 of 15 valid, 15 of 15 clean, 0 failures.** No `Bad PM4 packet`, no `Unreachable code!`, no
`GPU library assertion failed`, no `STOP:`. Every log ends `GPU process exited (0)`.

| run | route | s | valid | outcome | max draws/frame | load |
|---|---|---|---|---|---|---|
| attack-1 | attack | 180 | yes | clean | 897 | 7.10 |
| combat-1 | combat | 180 | yes | clean | 892 | 5.22 |
| interact-1 | interact | 180 | yes | clean | 887 | 6.38 |
| street-1 | street | 180 | yes | clean | 918 | 4.51 |
| attack-2 | attack | 180 | yes | clean | 904 | 4.31 |
| combat-2 | combat | 180 | yes | clean | 886 | 5.49 |
| interact-2 | interact | 180 | yes | clean | 886 | 3.52 |
| street-2 | street | 180 | yes | clean | 906 | 6.35 |
| attack-3 | attack | 180 | yes | clean | 904 | 5.60 |
| combat-3 | combat | 180 | yes | clean | 886 | 4.97 |
| interact-3 | interact | 180 | yes | clean | 894 | 6.00 |
| street-3 | street | 180 | yes | clean | 926 | 5.17 |
| attack-1 (ab2) | attack | 180 | yes | clean | 899 | 3.49 |
| combat-1 (ab2) | combat | 180 | yes | clean | 886 | 5.88 |
| interact-1 (ab2) | interact | 180 | yes | clean | 886 | 8.00 |

Load average at each run's end: 3.5-8.0. Per-run records with the exact summary line, start/finish times and
the build sha: `docs/pm4-results/prefix-ab-2026-10-19.jsonl`. Logs: `out/pm4_prefix/prefix-ab-*.log`,
`prefix-ab2-*.log`.

This is a **zero, not a fix**: pre-fix code cannot be fixed. At the historical rate (4 of 17 = 23.5%) 15
clean runs have a 0.765^15 = 1.8% chance, so the honest reading is that the harness on this machine, with a
warm 493-pipeline cache, no longer reproduces the assertion - the same conclusion the parent reached from
the fix build, now with the code variable removed.

## Arm B - not run

Arm A produced no failure, so the only in-game A/B that would have meant anything could not be formed.
Running the fix build against a zero proves nothing about the fix.

## Arm D - the stall diagnostic

`BB_SUBMIT_STALL_MS=n` (default off, this worktree only): the reader thread sleeps n ms at most once every
500 ms, at the top of its loop between messages (`remote_back.cpp`, `StallBetweenMessages`), so submits
queue behind the thread while the guest keeps producing. It prints `GPU process: reader stall 200 ms, N so
far` every 20 stalls, so a run proves the stall fired.

**6 runs, `BB_SUBMIT_STALL_MS=200` (40% of the reader's time), 6 of 6 valid, 6 of 6 clean, 0 failures.**

| run | route | valid | outcome | stalls in the log (~) | FPS |
|---|---|---|---|---|---|
| attack-1 | attack | yes | clean | 340 | 25.6 |
| combat-1 | combat | yes | clean | 340 | 25.9 |
| interact-1 | interact | yes | clean | 340 | 25.5 |
| attack-2 | attack | yes | clean | 360 | 25.6 |
| combat-2 | combat | yes | clean | 340 | 25.5 |
| interact-2 | interact | yes | clean | 340 | 25.4 |

The stall does what it was meant to do to the reader (17-18 `reader stall` lines per 180 s run = one stall
every ~500 ms for the whole route, 200 ms each) and it does **not** freeze the run, but it also does not
leave the guest untouched: FPS falls from the 41-60 of the clean runs to a flat ~25.5. So the guest is
waiting on the renderer for part of every 500 ms window - the stall throttles the producer too, just less
brutally than the parent's uniform 50 ms per submission (2.5 fps). Under that condition, 21 pre-fix runs in
total (15 without the stall, 6 with) produced no tear, no hash instrumentation involved.

## What this does and does not say

- **Pre-fix code, run correctly, is 15 of 15 clean** - and 6 of 6 with the reader deliberately going away.
  Genuine pre-fix binaries, copies on, the same harness, the same routes, 180 s each, 21 runs total.
- At the historical 4-of-17 rate, 15 clean runs are a 1.8% event and 21 clean runs a 0.4% event, so the
  reproduction rate on this machine today is very unlikely to still be 23%. The gap is environmental
  (warm 493-pipeline store, a differently loaded machine, a macOS/game build that is 3 days newer), not a
  code fix - there is no fix in the 47fa499 binary.
- Therefore **no live A/B can support or refute the submit-wait fix**. The fix's case rests where it already
  rested: the mechanism (asynchronous copy of a buffer the guest may rewrite) is real, the fix removes it by
  construction, and the synthetic test shows the protocol change does what it claims. This session adds a
  negative: the in-game harness no longer produces the failure, on either build, so it cannot be used as
  evidence of repair.
- The one number that would change this: a run where `Bad PM4 packet` appears with `memory type -1` **and**
  the `copy:`/`game:` dwords differ. Nineteen of the 21 runs here also had no hash check (by instruction);
  the hash check on the fix build (parent's 2.46M submissions) saw no guest rewrite either.

