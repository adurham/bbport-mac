# The "PM4 type 1 packet (0x70d1b005)" GPU assertion

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
