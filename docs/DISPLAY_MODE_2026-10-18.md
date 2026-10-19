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
- On the 2x mode (6880x2880 framebuffer) a 3440x1440 drawable never matches — the frame is
  composited, upscaled to the backing store and downscaled by the pipe. `retina=1` makes the
  drawable 6880x2880 (matching, and 4x the fill cost); `retina=0` does not.
- 240 Hz is unreachable from a 60 Hz desktop mode without moving the panel. The correct way, if
  it is ever wanted as an in-app option, is a timing-only switch (same pixel size and depth,
  different refresh) behind an explicit consent dialog, remembered, with the swapchain torn down
  before the set and the previous mode restored on exit and on crash.

The plain fix for a 240 Hz gaming session is a display setting, not a code path: run the panel at
its native 3440x1440 @ 240 Hz.

## Verification

- `ninja -C out/gpu bbgpu` and `ninja -C out/gpu-arm64` build clean.
- `settings-test` passes, including the new assertions for desktop/fill/exclusive and
  `BB_FULLSCREEN_MODE=desktop`.
- Not yet run live: the fullscreen path needs a covering run on the machine (and the external
  display plugged back in). `fullscreen_mode=desktop` issues no mode set, so it is the run that
  cannot repeat the panic.
