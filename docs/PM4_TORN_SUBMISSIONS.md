# The "PM4 type 1 packet (0x70d1b005)" GPU assertion

> **CORRECTION — 2026-10-19, and read this before the rest of the page.** An overnight campaign
> re-ran this bug properly and found that the evidence table below does not support what it appears
> to claim. Full detail: `docs/pm4-results/recon-2026-10-19.md`, raw runs in
> `docs/pm4-results/prefix-ab-2026-10-19.jsonl`, campaign record in `docs/PM4_PREREG_2026-10-19.md`
> and `docs/PM4_SYNTHETIC_2026-10-19.md`. In short:
>
> 1. **The "before" leg was never pre-fix code.** Its own logs carry the banner
>    `GPU process: graphics submissions do not wait for bb-gpu's copy (BB_SUBMIT_ASYNC=1)` and
>    "Submit check:" lines — and `BB_SUBMIT_ASYNC` / "Submit check" do not exist at `47fa499`
>    (`9215a10^`); they arrive *with* the fix commit. So the before/after table compares the fixed
>    build to itself with an env switch, which is a valid test of the switch and not of the code.
> 2. **`docs/pm4-repro/before.txt`'s `asserts=1` is a counter artifact.** Its eight runs are the
>    eight clean runs: the retained logs it labels contain zero assertion lines and zero
>    `Bad PM4 packet`, each ends `GPU process exited (0)`, and the `max_draws` values match the
>    retained logs exactly. The file came from an earlier uncommitted loop whose counter is not on
>    disk. The "0 asserts in 8" in the table below and the "asserts=1" in that file are the same
>    eight runs.
> 3. **`changed_submissions` has never measured anything.** Both scripts grep
>    `^Submit check: submission`, a string no build prints (the real lines are
>    `Submit check: N of M …` and `Submit check: submission N's dcb …`). It reads 0 for every run
>    ever recorded, with or without a live hash check.
> 4. **Genuine pre-fix code does not reproduce it.** `47fa499` (`9215a10^`, `SubmitWaitsForCopy`
>    absent) built and run 15 times hidden on attack/combat/interact/street with
>    `BB_COPY_GPU_BUFFERS=1`: 15/15 valid, 15/15 clean. Six more with the renderer's reader thread
>    deliberately stalling 200–500 ms between messages (the suspected widening condition): 6/6
>    clean. The only surviving crash log is `out/warmfill/yharnam.log` (2026-10-16 16:52, build
>    `77e4f46`), and the "4 of 17" rate is not re-derivable — three logs survive in `out/warmfill`
>    and one of them crashed.
>
> What still stands: the **synthetic** protocol result — protocol 9 (the fix) 0 torn in 37,000
> submissions vs protocol 8 83–100% torn, with a concurrent-writer control proving the detector
> fires. The fix's guarantee is real at the protocol level. Its credit for the live crash is
> unproven, and unprovable in this harness until the reproduction is resurrected.
>
> **Config landmine found in the same sweep:** `BB_COPY_GPU_BUFFERS` defaults to **false**
> (`gpu/shim/core/emulator_settings.h:55` and its `gpu/bbnet` twin) and no script under `tools/`
> exports it, despite `remote_front.cpp`'s comment calling it "run.sh's default". Any run that does
> not export it decodes guest memory directly — there is no copy to tear, and `BB_SUBMIT_ASYNC` is
> a no-op there. Check that flag before believing any pm4 result, old or new.

`Bad PM4 packet ... PM4 type 1 packet (0x70d1b005)`, then `Unreachable code!` and
`STOP: GPU library assertion failed (cp_graphics.cpp:203)`. bb-gpu exits and takes the run with it.
The warm-route session saw it in 4 of 17 runs. This page covers what the packet is, what the evidence
says, what changed on `fix/pm4-crash`, and what is still open.

## The header is not a register the port lacks

`0x70d1b005` has type 1 (bits 31:30 = 01). GCN command processors never emit type-1 packets, and the
port's walk (`RunGraphics`) accepts only type 3 and type-2 filler. Upstream shadPS4 `main` (checked
2026-10-10, `liverpool.cpp`) also stops with `UNREACHABLE` on type 0 and type 1. There is no
upstream handling to port, and treating the header as a no-op would only decode garbage after it.

The dword is a **value**, not a header. Dump of the first crash (`yharnam`, submission 35696):

```
c0047600 00000010 43400040 00108070 | c0027600 0000020c [70d1b005] 00000000
c0027600 00000212 000c0042 00000098  c0037600 00000207 00000040 00000001 00000001  c00a1000 ...
```

- `c0047600 00000010 ...`: SET_SH_REG with 4 values into USER_DATA 0x10 (a V#). Only **2** of the
  4 values are present before the next packet.
- `c0027600 0000020c 70d1b005 00000000 / c0027600 00000212 ... / c0037600 00000207 40 1 1 /
  c00a1000`: a complete `sceGnmSetCsShader` block. COMPUTE_PGM_LO = 0x70d1b005 (the shader at guest
  0x70d1b00500), then RSRC1/2, NUM_THREAD 64x1x1, and the driver's 11-dword NOP.
- The walk trusts the first packet's count, skips 6 dwords, and lands on PGM_LO's value.

The other two crash dumps on disk look the same:

- `perf-build/out/run-gprof.log`, header `0x201`: valid packets start 3 dwords inside an 11-dword
  NOP's body.
- `kk-build/out/run-legacy2.log`, header `0`: a zero dword among valid packets.

Each buffer is internally inconsistent: **two versions of the command buffer with different packet
boundaries, mixed together**. The game's libGnm wrote neither version wrong. One was overwritten
partway by the other.

## Where it happens

- **In all three dumps the bad buffer is bb-gpu's own heap**, not guest memory. Its address is
  inside the "its own memory at ..." range the log prints, and it has memory type -1. So it is the
  `BB_COPY_GPU_BUFFERS` copy (run.sh turns that on by default).
- **All three hit at the same moment of the run:** right after the title's Continue (pad 0x4000), as
  the game creates CSClothThread0-5 for the area load. The last frame stats show 22-24 draws/frame,
  which is the title screen. Submission number ~20k-35k. The four-route session's crashes were
  reported against combat routes, but every route crosses this transition first.
- In-process, the same symptom ("PM4 type 0 a few seconds after loading a save", run.sh's comment)
  is the reason `BB_COPY_GPU_BUFFERS` exists: the game reuses a command buffer's memory once it
  believes the GPU has it. In-process, the copy is taken **inside** `sceGnmSubmitCommandBuffers`.
  The native split (`remote_front.cpp`) sent the submission **asynchronously**, as a guest address,
  and bb-gpu's reader thread took the copy whenever it reached the message. During an area load
  that thread is busy with write faults and mappings, so the copy could be taken after the game had
  started rewriting the buffer.

## The change (fix/pm4-crash)

1. **The submission waits for the copy** (`remote_front.cpp` `SubmitWaitsForCopy`, `remote_back.cpp`
   `MsgSubmitGfx` replies). With `BB_COPY_GPU_BUFFERS=1`, `MsgSubmitGfx` becomes a call, and bb-gpu
   answers once `CommandProcessor::SubmitGfx` has made its copy. This is the in-process guarantee:
   the words are copied before the game's submit returns. Protocol version 8 -> 9.
   `BB_SUBMIT_ASYNC=1` restores the old behaviour for A/B runs.
2. **Diagnostics (env-gated, nothing runs by default):**
   - `BB_SUBMIT_CHECK=1`: XXH3 of each DCB/CCB taken in the game process at submit, compared in
     bb-gpu just before the copy. Prints `Submit check: N of M ... changed` every 20000 submissions,
     plus the first 16 and every 100th changed submission with its age in microseconds.
   - `BB_SUBMIT_DELAY_US=n`: bb-gpu takes each submission n us late, which widens the window.
   - The bad-packet report (always on, since the run dies there anyway) now also prints:
     - the last 16 packets the walk decoded in that buffer, flagging one that runs past the bad
       dword;
     - **the game's buffer now, next to the copy**: how many dwords differ, and the 32 dwords
       around the fault side by side.

     A tear shows as copy != game at the fault. Copy == game means the stream was already bad in
     guest memory, or the copy was damaged after it was taken.
3. `tools/macos/pm4_repro.sh`: the repro loop (below).

## Evidence so far, stated plainly

| leg | env | runs | asserts | submissions that changed before the copy |
|---|---|---|---|---|
| before (old async path) | `BB_SUBMIT_ASYNC=1` | 8 (attack, combat, interact, street x2) | 0 | 0 of ~5M |
| before, cold cache (220+ compiles per run) | `BB_SUBMIT_ASYNC=1 PM4_COLD=1` | 4 | 0 | 0 |
| before, widened window | `BB_SUBMIT_ASYNC=1 BB_SUBMIT_DELAY_US=3000` | 2 | 0 | 0 |
| after (this fix) | defaults | 12 (attack, combat, interact, street x3) | 0 | 0 |

**This session never reproduced the assertion.** That includes 14 runs on the old path, which
previously failed at about 4/17. The hash check also never saw a guest buffer change between submit
and copy, even with a 3 ms delay. So:

- The fix closes a real gap. It is the in-process copy guarantee the native split dropped, and it
  matches every byte of the dumps. But it is **not proven** to be the cause. Zero failures in 14 old
  runs has roughly a 2-7% chance at the old rate. The instrumentation itself may also hide the race.
- The other open explanation: the copy was intact when taken and was damaged later in bb-gpu's heap
  (freed or reused, or overrun by another writer), or the walk ran past the copy's end into an
  adjacent copy. The new bad-packet report tells these apart on the next hit: compare the "copy:"
  and "game:" lines, and check the "N of M dwords differ" line.
- Per-run lines are in `docs/pm4-repro/*.txt` (copied from `out/pm4_repro/<label>/summary.txt`).
- The fix costs nothing visible: gameplay holds the same ~41 FPS on both paths in the hidden
  3440x1440 runs. Each submit adds one round trip, about 3 per frame.

## Repro recipe

```
pgrep -x bb-probe            # never while the owner plays; pm4_repro.sh waits by itself
BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 BB_WARM_SOURCE_USER=~/repos/bbport-mac-native/user \
  bash tools/macos/pm4_repro.sh before 5 BB_SUBMIT_ASYNC=1        # the old path
BB_GAME_DIR=... BB_WARM_SOURCE_USER=... bash tools/macos/pm4_repro.sh after 5   # this fix
# PM4_ROUTES=attack,interact narrows; PM4_COLD=1 starts each run with no pipeline cache;
# PM4_SECS=n shortens runs (the crash came right after the title, so 120 s covers it).
```

Each run's log is kept in `out/pm4_repro/<label>/<route>-<n>.log`, with one line per run in
`summary.txt` (`bad_pm4`, `asserts`, `changed_submissions`). On a hit, the useful lines are:
`Bad PM4 packet`, `the N packets before it`, `the game's buffer ... differ`, and `copy:` / `game:`.

**Safest mitigation if it recurs with this fix:** keep the warm tooling's re-run, which is already
in `warmfill.sh`. Do not turn the UNREACHABLE into a skip-the-buffer. A torn buffer means the frame's
state is wrong, and dropping it hides the cause.

## The store wipe seen next to a crash

`PipelineCache::WarmUp` (`pipeline_store.cpp`) calls `BlobStore::Clear()` in three cases:

- `profile.bin` is missing;
- `profile.bin` is incompatible (`CacheVersions` or `Shader::Profile` differ: another build or
  device profile);
- any `.key` entry fails to deserialize ("damaged entries").

Blobs are written to `.tmp` and then renamed, so a dying GPU process does not leave a truncated
`.key` behind.

"7 files left where 1480 had been" therefore means the **next** start found the store incompatible
or damaged, cleared it, and the crashing run rebuilt 7 before it died. It does not mean the crash
deleted files. warm_merge already prints this case ("the sandbox's profile.bin changed during the
run: this build's version constants ... differ"): the sandbox was cloned from a store made by a
different build. The wipe was coincident with the assertion, not caused by it. The re-run's 1483
files, all but 3 byte-identical, fit a rebuild by the same build. I could not confirm which of the
three paths fired in that particular run, because its log was overwritten. A
`Pipeline cache isn't compatible` or `damaged entries` line in the run log tells which.
