# RESUME: Bloodborne on the MacBook (bbport-mac fork) — post-reboot handoff

Date written: 2026-10-17, before the owner's reboot. Everything below is integrated, built, and pushed.

## Read first
- Skill `bbport-mac-bloodborne` (load it — pitfalls live there), then these docs in `~/repos/bbport-mac-native/docs/`:
  `PERF_STEADY_STATE.md` (the wall is the GPU), `KK_GPU_WALL.md`, `BARRIER_MASKS.md`, `PM4_TORN_SUBMISSIONS.md`,
  `PIPELINE_WARMTH.md`, `GAME_LIBRARY.md`, `macos-title-artifact.md`, `macos-native-gpu.md`.

## Repos and what runs
- **Native line** (what the owner runs): `~/repos/bbport-mac-native`, branch `build/macos-native-0.6` (tip ≈ `2b8e0ba`),
  remote `adurham/bbport-mac`. The deployed app `~/Games/Bloodborne/Bloodborne PC Port.app` execs THIS worktree's
  `out/` binaries — rebuild the worktree and the app is current (nothing to redeploy).
- **Main line**: `~/repos/bbport-mac`, `feature/display-options` (≈`153833b`). Sync native→main ONLY via
  `tools/macos/sync_to_main.sh <since-sha> --commit "msg"` (fitted policy: main keeps its own renderer core; it has a
  no-op `gpu/shim/remote/bb_remote.h` stub). Background shells run **zsh** — inline `for f in $VAR` does not
  word-split; the script uses while-read for this reason.
- **Paleblood fork** (the long-term native direction): `~/repos/paleblood` (`adurham/Paleblood`); the full contribution
  environment is on **amd-workstation** (ctest 13/13, dump prepared, verify harness proven; see
  `docs/workstation-setup.md` in that repo).

## Already live in the app (no action needed)
Exclusive fullscreen is the default (the display switches to the game's resolution, the HUD follows; confirm dialog
first; fill mode is the opt-in). Async pipeline compiles actually run (BB_ASYNC_STATIC_VI armed in the bootstrap).
The warming notice is opt-in (`BB_WARM_NOTICE=1`). The submit-call crash fix (torn-submission race) and the futex
drain (74k yields/frame gone) are in. The warmfill route pack (8 verified routes) + merge tooling work.

## Resume these (ONE GPU-bench child at a time — two in parallel void each other's runs)
1. **MetalFX matrix** (the GPU-bound lever toward 60 fps): worktree `~/repos/bbport-metalfx-build`; script
   `tools/macos/metalfx_matrix.sh` is ready. Run the 3440x1440 queue (off/mfx1/mfx4/fsr3_5/fsr3_1), then the corrected
   2560x1440 pair (the first two "2560" runs actually measured 3440 — artifacts renamed). Deliver raw alternated numbers
   + captures; NO defaults changed without the owner.
2. **Driver patch 0006 test**: worktree `~/repos/bbport-transfer-build`. The idea: in KosmicKrisp's vk barrier mode,
   stop counting TRANSFER as vertex-side, then re-measure `BB_KK_BARRIER=vk BB_BARRIER_MASKS=safe` vs legacy. Its
   `out/ab_transfer.sh` exists but the series was voided by contention — rerun clean, alone.
3. After verdicts: whichever configuration nears 60 fps → the owner's visible run (his eyes are the gate) → then any
   default decisions.

## Owner pending (needs a visible session)
- The title-screen artifact: still needs his one-word answer — **flickering or static** (routes the attribution
  matrix; `tools/macos/capture_condition.sh` + `docs/macos-title-artifact.md`; the port was already FIFO, so the
  remaining suspect is the Metal4 drawable path the opaque override walks around).
- Visual checks: the futex drain (no menu/loading hitch), exclusive fullscreen on the next launch, and any
  `BB_KK_BARRIER=vk` default change.
- Perf numbers at his resolutions with the new build (the frame log records itself; skip the first 30 s).
- After any future cache wipe: `tools/macos/warmfill.sh --routes all` (sandboxed; ~20 min hidden).

## Long-term (the fork's native direction — his directive: "more akin to the PT form")
Paleblood runtime track queue: generic input capture, `scePthreadAttrGetaffinity`, module-loading stubs; then the
issues labelled `runtime` (#34–41). Decomp targets per its `STATUS.md`/`NEXT.md`. Upstream PRs ONLY on the owner's
explicit word; fork-first. The macOS bridge is proven: `BB_GAME_LIB` loads Paleblood's real libbbgame (31 hooks) and
the game boots through bbport-mac.

## Hard rules
Never disturb a live game (`pgrep -x bb-probe` first; every bench has a guard — wait, never stomp). Hidden runs only.
One GPU-bench child at a time; after stopping any child, sweep its nohup'd scripts
(`pgrep -fl 'gpu_ab_run|warm_route|warmfill|ab_transfer|metalfx_matrix'`) then verify `bb-probe`/`bb-gpu`/`xctrace`
are gone. Commit and push every turn on both lines.
