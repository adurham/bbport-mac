# PERF_LOOP — the autonomous perf loop

Established 2026-10-17 after the encoder/break workstream was measured out. This document is the
loop: stages, gates, commands, artifacts. Any session can resume from here.

## Where we are (measured, do not re-walk)

- 2560x1440: off 23.4 ms; FSR 3.1 preset 3 20.8 ms; preset 4 (854x480 internal) 19.4 ms = 51.6 fps.
  Guest startup-patched to 854x480, no upscaler: **16.5 ms = 60.6 fps**. Gap to 60: ~2.9 ms.
- Frame structure at FSR3x: GPU busy 12.53 ms (fragment 4.60, vertex 3.68, compute 4.30), idle
  6.75 ms; ~340 encoders/frame (one MTL4CB per encoder), ~20 us boundary gaps, ~46 pass breaks
  (81% from `Runtime::CopyBuffer`).
- Closed by measurement: store/load on break pairs (+2.1 ms, worse), the per-frame DownloadImage
  drain (null, present-bound), packaging/commits (micro-benchmark: 300 CBs one commit = 300
  individual commits; one CB with 300 encoders worse), copy-merge + in-pass barriers together
  (neutral: -0.04 ms, -0.85 encoders; marginal in-situ break cost ~0.03-0.04 ms).
- Consequence: the break-elimination line caps at ~1-1.5 ms. The remaining gap is upscaler cost
  (FSR dispatches + the live-mode 1080p post + copy-backs) and possibly CPU-side starvation.
- Fable review (2026-10-17): do NOT build a Metal-native renderer core (#6) — it inherits Metal's
  per-encoder cost and the win lives in bb-gpu's scheduler. Rank: (A) startup-patched render size
  -> (B) encoder census + merge in bb-gpu -> (D) bespoke upscaler -> frame gen -> MetalFX zero-copy.
- Open blind spot: is the 6.75 ms idle inter-encoder gaps or CPU submission starvation? Already
  partly measured: ~2.2-2.6 ms CPU-late, ~3.4 ms queued gaps. Stage B must nail the split.

## Worktree / conventions

- Perf worktree: `~/repos/bbport-encmerge-build` (branch `fix/encoder-merge`, warm build; carries
  all default-off gates: BB_COPY_MERGE, BB_INPASS_BARRIERS, BB_BREAK_STORE_PROBE,
  BB_DOWNLOAD_SYNC_PROBE).
- Docs live in `~/repos/bbport-mac-native/docs/` (branch `build/macos-native-0.6`).
- Runs: hidden only; ONE GPU workload at a time; verify `pgrep -fl 'bb-probe|bb-gpu|gpu_ab_run'`
  empty before and after; a foreign bb-probe = STOP and report.
- Run template (standing camera, 190 s):
  `BB_GAME_DIR=$HOME/Games/Bloodborne/CUSA03173 BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=4 \
   ROUTE='ly=128,rx=128:5' CAPTURES=2 BB_PASS_BREAK_TRACE=1 bash tools/macos/gpu_ab_run.sh TAG 190`
- Validity per run: `overlap-TAG.txt` empty, draws >= 500 in `frames-TAG.csv`, p50 <= 40 ms;
  a dubious tag re-runs once. Parse steady rows (draws>=500 && t_s>=60) for p25/p50/p75,
  present_wait, draws.
- Commit + push every stage's artifacts (docs to the native line; gates to the worktree branch).
- No default changes without the owner. Correctness = port captures + the capture-comparison tooling
  against the same-mode noise floor.

## Stage A — upscaler configuration (cheap, hours)

A1. Determine whether FSR works with the startup-patched path (`live_resolution=0`): read
    `docs/upscaler.md` + the presenter/upscaler code. TAA needs live; the question is FSR.
    If it works: 2+2 alternated A/B at 2560, fsr3 preset 4, live vs startup-patched. Report
    delta + captures. Gate: >= 1 ms and clean captures -> candidate default (owner decides).
A2. Preset sweep (presets 2-4) in the winning mode, 2560 and 3440.

Artifacts: `docs/UPSCALER_CONFIG_BENCH.md` + JSON/CSV in the worktree `out/`.

## Stage B — encoder census (analysis; the "valuable kernel of #6, extracted")

B1. Classify the ~340 encoders of one captured frame by TYPE and CAUSE: RT change required by the
    guest, compute<->render transition, barrier split, CopyBuffer break, cache-management copy,
    FSR internal, meta/resolve, query. Use the frame analyzer dump (`BB_CAPTURE_TRIGGER` /
    `BB_CAPTURE_DIR`; two dumps already exist in `out/analyzer*/`) + `BB_KK_STATS` deltas +
    break histograms (`BB_PASS_BREAK_TRACE=1`) + `mtl_gaps.py` on a stored trace.
B2. N_min: the minimum encoders ANY backend could emit for that frame given real data
    dependencies (adjacent computes without hazard -> one encoder; copies deferrable to pass
    edges; RT-compatible draws -> one render encoder). Build it from the dump's per-pass resource
    reads/writes.
B3. N_vk: the minimum reachable within bb-gpu + KosmicKrisp with targeted changes (deferred
    copies, dynamic rendering, compute coalescing in the driver).
B4. Decision (consult Fable at this point; record its verdict in the doc):
    - Kill the encoder line if (340 - N_min) x ~0.03-0.035 ms < ~3 ms  -> it cannot buy the frame.
    - If N_vk ~= N_min -> the Vulkan layering is not the blocker; implement in bb-gpu.
    - Only if N_min << N_vk and the gap is structural to the Vulkan render-pass model does a
      backend (hybrid HAL, Vulkan kept as oracle) earn consideration.
B5. Confirm the idle split: inter-encoder gaps vs CPU-late, with the CPU side named (Rosetta guest,
    bb-gpu recording, driver submission).

Artifacts: `docs/ENCODER_CENSUS.md` (+ data files).

## Stage C — implement what B proves (only if B4 says so)

Top-ranked merges from the census, each behind a default-off gate: measure encoder count first,
then 2+2 A/B + captures; commit each; doc the result honestly (a regression ships default-off with
its numbers, as with COPY_MERGE).

## Stage D — bespoke temporal upscaler (if the encoder line is capped)

A 2-3 pass reproject + accumulate upscaler on the port's existing machinery (camera AND object
motion vectors, history/reprojection), instead of FFX's 8 dispatches + barriers. Gate
`BB_UPSCALE_CUSTOM=1`, default off; A/B vs FSR3_4 at matched output + capture comparison; report
quality (text/HUD sharpness, ghosting on particles/fog/transparents) and cost.

## Reporting

Bottom-line numbers only (p50, fps, encoders, deltas, correctness), TLDR-sized. Every stage ends
with a commit + push and a doc update. Never re-walk a closed lever: check this file first.
