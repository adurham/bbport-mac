# Display mode: the panel is never switched unless asked (2026-10-18)

## What happened

2026-10-18 17:22:28, MacBook Pro M4 Max (Mac16,6), macOS 27.2 (26B5112a): the machine
hard-panicked in the **display coprocessor**, seconds after the port started.

```
panicInitiator: DCPEXT1
DCPEXT1 PANIC - IOMFB int_handler_gated: failure: axi_rd_err [0x40381400]
  - iomfb_driver(9)                       [the DCP's own thread, not an app thread]
RTKit: AppleDCP-1317.40.16~861-t604xdcp.RELEASE
```

`axi_rd_err` is the display pipe's DMA read faulting: the pipe was told to fetch a scanout
surface it could not reach. The app cannot cause that in its own address space — it is a
display-pipe transition the firmware did not survive. Report: `panic-full-2026-10-18-172321`.

## What the port was doing

`bbport.ini` had `window_mode=fullscreen`, `fullscreen_mode=exclusive`, `output_res=3440x1440`.
Exclusive fullscreen calls `SDL_SetWindowFullscreenMode(picked)` — i.e. `CGDisplaySetDisplayMode`
— at startup, while the first fullscreen drawable is being handed to the WindowServer.

## Why it worked before (regression)

WindowServer's mode id for the external display (AW3425DW), from the unified log:

| active | mode id | what |
|---|---|---|
| Oct 16 08:50 – Oct 17 08:06 | 149/152/150/151/155 | native modes; 155 is the 240 Hz one (BetterDisplay's `protectRefreshRateMode`) |
| Oct 17 08:21 – Oct 18 17:21 (crash) | **775** | the injected 2x HiDPI mode: 6880x2880 @ 60 Hz |

SDL **skips** `CGDisplaySetDisplayMode` when the picked mode is already the current one. The
picker wants 1x 3440x1440 @ 240 Hz. Every launch up to Oct 17 08:21 therefore matched the mode
the desktop was on and touched nothing. After that boot the desktop came up on the 2x @ 60 Hz
mode, so the launch on Oct 18 was the first one that actually asked CoreGraphics to move the
panel — 6880x2880 @ 60 → 3440x1440 @ 240 — while the drawable was being promoted. That is the
sequence that panicked the DCP.

Caveat on evidence: logd lost everything after 17:21:58 (before the panic), and the panic report
carries no stackshot, so the mode-set line itself is not in any log. The mode history, the
config, the timing and the SDL skip rule are what the conclusion rests on.

## What changed

`fullscreen_mode` has three values now:

- **`desktop` (default)** — the display keeps the mode it is on. The window covers it and the
  output resolution is rendered and scaled into the window. No `CGDisplaySetDisplayMode` is ever
  issued. This is the only value that cannot move the panel.
- `fill` (opt-in) — the panel is put on its own native 1x mode once, and the output is scaled
  into it.
- `exclusive` (opt-in) — the display is put on the output's mode, applied **at startup only**.

`fullscreen_mode` used to default to exclusive in the settings struct while the comments and the
menu claimed fill was the default; both are now consistent, and the default is the one that
touches nothing.

Also: a mode switch is no longer performed under a running swapchain. `follow_display_mode`
used to re-apply the mode when the output resolution changed at runtime (exclusive); that set
the display mode while the drawable was torn down and re-created. It now logs that the change
needs a restart.

## Direct scanout (for whoever works on this next)

The WindowServer only scans a drawable out directly when the drawable's pixel size equals the
current mode's **framebuffer** pixels (`CGDisplayModeGetPixelWidth/Height`), the layer is opaque,
untagged (or tagged with the display's own space), `framebufferOnly`, and nothing is above it.
Consequences here:

- On a 1x desktop mode (3440x1440) a 3440x1440 drawable matches: Direct is possible.
- On the 2x mode (6880x2880 framebuffer) only a 6880x2880 drawable matches. `retina=1` makes the
  drawable exactly that; `retina=0` leaves it at 3440x1440, which the WindowServer upscales to the
  backing store and the pipe then scales back down to the panel (Composited, "never Direct").
- The cost of the matching drawable is the encode pass only: the scene renders at the output
  resolution (the game's display buffer), and `EncodePass::Render` draws that buffer into the
  frame image — one full-screen textured quad at the backing size, plus the swapchain's memory
  (79 MB per 6880x2880 BGRA8 image). It is not a 4x scene render.
- The refresh rate of a scaled (HiDPI) mode is not what macOS reports: `system_profiler` and
  `CGDisplayModeGetRefreshRate` show the AW's HiDPI mode as 60 Hz while the link runs it at
  240 Hz (BetterDisplay's own record: `defaultRefreshRate@Display:25 = 240`,
  `protectRefreshRate@Display:25 = 240Hz`). Do not use the reported rate to reason about this
  display. A scaled mode at 240 Hz also means a 6880x2880 backing store scanned out and
  downscaled to the panel 240 times a second — an extreme load on the same DCP that panicked, and
  one more reason nothing in the port may set a mode underneath it.

The owner runs the AW3425DW on the HiDPI mode deliberately (240 Hz, reported by macOS as 60);
that is the supported configuration: `fullscreen_mode=desktop` (never touches the panel) plus
`retina=1` (drawable = the mode's framebuffer) gives a Direct-capable fullscreen with no mode set.

## Verification

- `ninja -C out/gpu bbgpu` and `ninja -C out/gpu-arm64` build clean.
- `settings-test` passes, including the new assertions for desktop/fill/exclusive and
  `BB_FULLSCREEN_MODE=desktop`.
- Not yet run live: the fullscreen path needs a covering run on the machine (and the external
  display plugged back in). `fullscreen_mode=desktop` issues no mode set, so it is the run that
  cannot repeat the panic. The layer dump prints `drawable matches/differs from the mode's pixels`
  on the first present, so that run answers the Direct question by itself.

## Second panic, 23:26:44 — the same signature, and it survived the set for 110 s

Same machine, same macOS build, six hours later. Run:
`BB_FULLSCREEN_MODE=exclusive BB_SAVE_LOG=1 bash tools/macos/run.sh` (ini: `window_mode=fullscreen`,
`output_res=3440x1440`).

- `logs/20261018_232449.log`: started 23:24:49 on the HiDPI mode (`drawable matches the mode's
  pixels`, mode 6880x2880), SDL set 3440x1440 @ 240 (mode id 724, `native 1`) at 23:24:52, scanout
  went `1:1`, and the port ran **120.0 FPS steady** at 8.0 ms median — the statistics reach 104 s.
- `panic-full-2026-10-18-232751`: `DCPEXT1 PANIC - IOMFB int_handler_gated: failure: axi_rd_err
  [0x40381408]`, faulting task `iomfb_driver(9)` — same initiator, same register as the 17:22 one.
  Wall clock ~23:26:44 (the streamed WindowServer/DCP log ends 23:26:43.95); the report lists a
  single `bb-probe`, `processUptime` 133 s.

What it means:

- The mode set is **not instantly fatal**. This time the pipe ran 1:1 at 3440x1440/240 for ~110 s
  after `CGDisplaySetDisplayMode` before it faulted, so "set during drawable promotion" is not a
  complete account of the fault — it is only the step the two crashed runs share and the surviving
  Oct 16/17 launches do not (those matched the current mode, so SDL set nothing).
- Correction to an earlier claim in the working session: that run was ~2 minutes old, not
  "40+ minutes". The session's own uptime was misread as the game's; the report's `processUptime`
  settles it.
- An un-tested risk remains: `desktop` fullscreen sets no mode, but it still scans a 6880x2880
  backing store into the panel at the link's 240 Hz (see above). If that also takes the DCP down,
  the panel's HiDPI configuration is the common factor, not the mode set.

## The fix: the panel moves before anything of ours can be on screen

Both crashes had the mode set **after** the swapchain existed. That was structural, not bad luck:

1. `WindowSDL` created the window with `SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN = true`, so it was
   already fullscreen on the desktop's mode.
2. The boot window mode was only *queued* (`SetWindowMode` → `pending_window_mode`) because
   `apply_window_mode` did not exist yet in the object's life.
3. The first `PollEvents()` in the window thread applied it — while the main thread had already
   built the presenter/swapchain and the game could be presenting. `SDL_SetWindowFullscreenMode`
   then issued `CGDisplaySetDisplayMode` under a live, scanout-eligible drawable.

The ordering is now enforced in code:

- A fullscreen launch whose mode moves the panel (`fill`, `exclusive` — i.e. not `desktop`) is
  created as a **plain window**, then moved to its mode and to fullscreen **synchronously in the
  constructor** via `apply_window_mode()`, before `SDL_Metal_CreateView` makes a layer the
  WindowServer could scan, and before `g_window_ready` lets the main thread build the presenter.
  The swapchain is then born on the new mode; no surface is ever re-pointed under the DCP.
- `boot_mode_phase` is cleared at the end of the constructor. `apply_display_mode()` checks it and
  **refuses every later set** — the menu's fullscreen row, a confirmed resolution change, a display
  change — logging `… needs a restart (… moves the panel at startup only, before the first
  drawable …)` and moving nothing. This is a mechanical gate: there is no path left that issues
  `CGDisplaySetDisplayMode` from a running window.
- `BB_DISPLAY_DRY=1` forces the dry path (pick logged, nothing set), so the ordering and the picker
  can be exercised in a fullscreen launch without moving the panel — SDL enters fullscreen on the
  desktop's own mode instead.

What this does **not** cover, and is the next thing to look at if a panic ever recurs:

- **Leaving fullscreen from the menu** (`windowed`/`borderless`) still makes SDL restore the
  desktop mode — a set, under our window, with the swapchain alive. It is user-initiated and the
  port never does it on its own; the unplanned variant (focus loss, Cmd-Tab, a notification) is now
  prevented by `SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0`.
- **Quitting** never reaches SDL's restore: the window thread calls `_Exit(0)` after the loop, so
  there is no transition at exit — and the panel is left on the game's mode, i.e. the desktop comes
  back at 1x until the HiDPI mode is selected again. That is a UX bug, not a DCP hazard (no set is
  issued). Fixing it means an ordered shutdown: stop presents → wait for the last frame → destroy
  the swapchain → destroy the window → then restore, from a state with no drawable of ours alive.
  Not written here because it cannot be verified without a covering run.
- The stronger variant (own the mode ourselves before `SDL_Init`, use borderless fullscreen so SDL
  never has a mode to apply or restore, and never restore at all) removes every transition except
  the startup one, but it hands the owner a manual mode restore in BetterDisplay after each exit —
  rejected for that reason, recorded here as the fallback design.

Live verification status: **not run.** What is checked here: `tools/macos/build.sh` builds both
slices clean (x86-64 `libbbgpu.dylib` + arm64 `bb-gpu`), `settings-test` passes on both, and
`tests/test_display_mode_order.py` (5 checks, run by `python3 -m unittest discover -s tests`) fails
on the pre-fix `window.cpp` — it asserts the ordering above, so a revert to the queued boot mode
turns red. The launch that proves it is a covering run and waits for the owner's word.

`bbport.ini` on the owner's machine was on `fullscreen_mode=exclusive` after the 17:22 crash. That is
no longer defensible — see the next section; both inis are now on `fullscreen_mode=desktop`, which
sets no mode at all.

## Third panic, 23:54:41 — no app at all, the switch itself

The owner turned HiDPI off in BetterDisplay by hand, ~27 min into a clean boot with nothing of ours
running (no game, no `bb-probe`, no `bb-gpu`, no Metal surface), and the machine went down with the
same `DCPEXT1 … axi_rd_err [0x40381400]`, same faulting task, 24 s after the last message of the
working session and within the ~37 s of log that `logd` lost.

That settles the question the first two panics could not answer. The 775 → 1x transition is not
survivable on this machine on macOS 27.2 (26B5112a) by *anyone* who issues it: not the port, not
BetterDisplay, no third-party surface involved. Consequences:

- The ordering fix (mode before any drawable) is still correct hygiene, but it cannot make the
  switch safe. It only guarantees the port is never the process holding a surface across it.
- Anything in the port that sets a mode is now a crash button. `fullscreen_mode=desktop` is the
  only usable value until Apple fixes the DCP firmware; `fill` and `exclusive` must stay opt-in and
  are documented as panic-capable on this OS build.
- The 240 Hz panel mode is reachable only by staying on the HiDPI desktop mode: with
  `fullscreen_mode=desktop` + `retina=1` the drawable matches the mode's 6880x2880 framebuffer, so
  the fullscreen is Direct and no set is issued.



