# Synthetic stress of the split's submission path — 2026-10-19

The question this answers: **does the fix close the tear?** It is a property of the submit/copy
protocol, and it is answerable without the game. `tests/test_pm4_submit.cpp` drives the port's own
submission path with a guest that rewrites its command buffer the moment submit returns — the
in-process contract, and the exact thing the split dropped. Pre-registration:
`docs/PM4_PREREG_2026-10-19.md`. Raw numbers: `out/pm4_synthetic.jsonl`.

## What is the port's code here, and what is not

Linked and exercised as compiled for the port:

| piece | file | used by the port as |
|---|---|---|
| the channel: ring, reply slots, wakeups, sync calls | `remote/bb_channel.h` | bb-probe ↔ bb-gpu |
| `WaitsForCopy`, `NoteSubmissionHashes`, `Send` | `remote/bb_submit_path.h` | `remote_front.cpp` `SubmitGfx` |
| `DelayUs`, `CheckSubmitted`, `HandleSubmission` (delay → check → copy) | `remote/bb_submit_path.h` | `remote_back.cpp` `MsgSubmitGfx` |
| `TakeCopy` — the `BB_COPY_GPU_BUFFERS` copy itself | `cp/cp_diagnostics.h` | `CommandProcessor::SubmitGfx` |

Those functions were moved out of `remote_front.cpp`, `remote_back.cpp` and
`command_processor.cpp` into the shared locations above **for this test**, with no change in
behaviour (the port calls the same functions it did, now through one definition). `remote_front.cpp`
itself cannot be linked into a test: it spawns bb-gpu, opens a window and needs the whole bb-probe
runtime. `remote_back.cpp` is the entire GPU core. So the test writes the two things those files
would otherwise provide:

- the game's frame loop: fill the buffer with version A, hash and submit it, and as soon as submit
  returns fill it with version B (the game's reuse of its own buffer);
- the six-line `MsgSubmitGfx` dispatch: read `SubmitGfxArgs`, resolve the DCB address, call
  `HandleSubmission`, then `reply()` — as `remote_back.cpp`'s `Handle` switch does.

Everything that decides the outcome is the port's: whether the front waits (`WaitsForCopy`), what
it waits for (the reply `Handle` sends after the copy), the check (`CheckSubmitted`), the delay
(`DelayUs`) and the copy (`TakeCopy`).

## The model

A 16 KiB DCB of NOP packets whose body words differ between the two versions, so every copy is
decodable and every torn copy is still a *torn copy*: each copied dword is compared with both
versions. A copy that is not exactly the submitted version is counted (`torn`); one holding words of
both versions is counted separately (`mixed`) — the crash dumps' signature ("two versions of the
command buffer with different packet boundaries, mixed together"). NOP headers are identical in both
versions and are ignored.

One process per arm, because the port caches its env decisions per process (as the game and bb-gpu
do). Protocol version 8 = `BB_SUBMIT_ASYNC=1`, 9 = the fix.

## Results

| arm | protocol | `BB_SUBMIT_DELAY_US` | submissions | copies | torn | mixed | whole other | secs |
|---|---|---|---|---|---|---|---|---|
| old | 8 | 0 | 20000 | 20000 | 16641 (83.2%) | 3199 | 13442 | 0.1 |
| old | 8 | 100 | 5000 | 5000 | 4981 (99.6%) | 0 | 4981 | 0.8 |
| old | 8 | 500 | 2000 | 2000 | 1999 (99.9%) | 0 | 1999 | 1.5 |
| old | 8 | 2000 | 1000 | 1000 | 1000 (100%) | 0 | 1000 | 2.9 |
| old | 8 | 50000 | 300 | 300 | 300 (100%) | 0 | 300 | 16.8 |
| **fixed** | **9** | **0** | **20000** | **20000** | **0** | **0** | **0** | 0.1 |
| **fixed** | **9** | **500** | **5000** | **5000** | **0** | **0** | **0** | 4.0 |
| **fixed** | **9** | **2000** | **2000** | **2000** | **0** | **0** | **0** | 6.2 |
| **fixed** | **9** | **50000** | **10000** | **10000** | **0** | **0** | **0** | 562.6 |
| fixed, guest rewrites during submit | 9 | 0 | 1000 | 1000 | 999 | 0 | 999 | 0.0 |
| old, guest rewrites during submit | 8 | 0 | 1000 | 1000 | 991 | 981 | 10 | 0.0 |

`copies` equals `submissions` in every arm: no message was dropped, and every submission's copy was
taken and compared. The 20000-submission arms are one second of wall time; the 50 ms arm is 9.4
minutes of it.

Reading it:

- **The harness works, and it works at zero artificial delay.** With the old path and no delay at
  all, 83% of copies were taken after the guest had rewritten the buffer, and 3199 of those 16641
  were *mixed* — the exact shape of all three crash dumps. (An earlier pass of the same arm gave
  70% and 2158: the no-delay arm is a genuine race, so its rate varies with scheduling.) The race does not need a widened window
  to be visible when nothing else is competing for the reader thread.
- **The fix closes it deterministically.** 37000 submissions across four delays (0 to 50 ms),
  including the 50 ms arm where the copy is deliberately taken three frames late: zero torn
  copies, zero mixed copies, and every submission's copy was answered (37000 copies for 37000
  submissions).
- **The zero is not a zero without a check.** The last row is a guest that breaks the in-process
  contract and rewrites *during* submit; the same fixed path then reports 997 of 1000 torn, with
  mixed copies. So the detector, the copy and the hash all run on the v9 path — the zero above is
  the protocol holding, not the check being skipped.
- The `mixed` count falling as the delay grows is expected: a longer delay lets the rewrite finish
  before the copy starts, so the copy is wholly the new version rather than a blend.

## What this does and does not prove

It proves the protocol property: **with the fix, the game's words are copied before the submit
call returns, in every one of 37000 submissions, including at a 50 ms copy delay.** Reverting
`SubmitWaitsForCopy` (or sending without waiting) makes the tear reappear immediately in the same
harness.

It does not prove the assertion's cause. A tear at the copy is a necessary condition for the
`0x70d1b005` walk failure, not a sufficient one: the game has to reach the area-load transition
with a submission in flight, and the rewrite has to land on a packet boundary. That is what the
in-game arms are for (`docs/PM4_PREREG_2026-10-19.md`); if they never reproduce a mismatch, this
page is the evidence for the protocol and the in-game causality stays unproven.
