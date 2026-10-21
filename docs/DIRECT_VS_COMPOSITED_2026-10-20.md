# Direct vs composited on the AW3425DW (2026-10-20)

## SOLVED — Direct is reachable, and this is the recipe

Two things were wrong, and neither was what earlier attempts blamed.

1. **The mode must be set by CoreGraphics before any window exists, and SDL must be given no
   fullscreen mode.** Passing a mode to `SDL_SetWindowFullscreenMode` selects SDL3's *exclusive* path,
   which explicitly exits any macOS Space and produces a level-25 borderless window; giving SDL no
   mode keeps the window in a Space (`fullscreen 1`, level 0, no shadow) — the Direct-era state.
   Implemented: `bb_display_set_mode_now()` (bbport_layer.mm) runs in the window constructor before
   `SDL_CreateWindowWithProperties`, and `apply_display_mode` then logs and returns without handing
   SDL a mode (`cg_mode_applied`). The mode set happens with no window, no Metal view and no
   swapchain in existence, so the 2026-10-18 panic ordering cannot apply.
2. **The layer must not be tagged SRGB** — a tagged colour space forces a conversion pass. The port
   already has the knob: `layer_colorspace=nil` (or `BB_LAYER_COLORSPACE=nil`).

And one environmental condition: **BetterDisplay must not be running.** Its 1x1, alpha-0.00 stub
window sits at level 1000, above the game's level-0 window, and that alone drops the display back to
Composited — measured: BD quit + colours nil = **Direct** (HUD, green, 3440x1440, Game Mode On);
BD running with its filter neutral + the same colours setting = Composited. BetterDisplay's
colour filter is a second, independent blocker (it composites the whole display by design).

Verified state in the Direct run (`out/direct-ab/run-ab10.out`): window `fullscreen 1`, level 0,
hasShadow 0, `presentationOptions 0xa`, layer opaque 1, `colorspace nil`, drawable 3440x1440 == mode
727 (1x 3440x1440 @120, 1:1 scanout), nothing above the window; HUD `Direct`.

## Earlier conclusion (superseded)

**Superseded by the recipe above.** The port could not reach Direct on the window construction it had
before the CG pre-set; compositing costs less than the instrument can resolve. Four variants were tried with the BetterDisplay colour filter off, the
layer opaque and the drawable matching the display mode's pixels; the Metal HUD read *Composited* in
every one — read from the agent's own screenshots (`/tmp/hud-now.png` and the ab3–ab5 captures), not
from the logs.

The reason is in the window state, not in the renderer:

| | 2026-10-16 (Direct era, `bbport-async-build/out/last-run.log:48`) | every 2026-10-20 run |
|---|---|---|
| layer dump window | `fullscreen 1`, level **0**, hasShadow 0 | `fullscreen 0`, level **25**, hasShadow 0 |
| presentationOptions | **0x40a** — hideMenuBar 1, hideDock 1, **fullScreen 1** | **0x0** (0xa after our explicit set; AppKit resets it) |
| window rect | 3440x1440 at 0,0 | 3440x1440 at 0,0 |
| layer | opaque 1, drawable == mode pixels | opaque 1, drawable == mode pixels |
| HUD | Direct | Composited |

Direct needs the window to be a **macOS fullscreen Space** (the `fullScreen` bit in the presentation
options, window level 0). The port produces a borderless window on the desktop instead — level 25, no
Space, no fullScreen bit. That changed with the 2026-10-18 anti-panic ordering fix: the window is now
created plain and moved to its mode and fullscreen later, which is what keeps a display-mode set from
happening under a live swapchain, and which apparently also costs the Space.

## The A/B attempts, and why they are void

Attempt 1 (two scripted runs, filter on/off) measured a 0.33 ms p50 difference — but the harness drove
`tools/macos/run.sh`, which reads the **checkout's** `bbport.ini`, where `layer_opaque` is absent. Both
arms logged `opaque 0` at the `Layer check` line, so both were composited by configuration. Attempt 2
fixed the opacity (`opaque 1`) and still had both arms composited — run B never reached Direct either,
confirmed on the HUD.

What survives from the attempts: a **matched-pair noise floor of 0.4 ms** (p50/p99, 1,800 frames per
run, 1,483 draws/frame, 37.8 ms frames). Raw data: `out/direct-ab/frames-ab{1,2,1b,2b}.csv` and the
matching `run-*.out`.

## What was tried, and what the HUD said (all Composited)

1. BetterDisplay compositor filter off (its overlay window gone), layer opaque 1, drawable == mode
   pixels, 1:1 scanout.
2. + explicit `NSApp.presentationOptions = HideMenuBar | HideDock` (`took the display:
   presentationOptions 0xa … window level 25`; AppKit reset it to 0x0 on later checks).
3. + `SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=1` (logged; SDL still took the borderless path).
4. + `SDL_RaiseWindow`, so the game is frontmost with Game Mode On (the earlier scripted runs logged
   `frontmost app com.tinyspeck.slackmacgap`, a harness fault worth removing anyway).
5. + **born fullscreen** (`BB_BOOT_FULLSCREEN=1`, the pre-2026-10-18 creation path, safe to try because
   the picker no longer lands a width-changing set on the DSC link). The window starts at level 0,
   but SDL's fullscreen entry puts it back at level 25 and `presentationOptions` returns to 0x0; the
   HUD still reads Composited. The gate was reverted — it buys nothing.

Five variants, five Composited. The macOS Space is not reachable through SDL's Cocoa fullscreen entry
in this window, however it is created.

## What compositing costs — bounded, not measured

- **The matched-pair noise floor is 0.4 ms.** Nothing below that is a result on this instrument.
- **A composite of 3440x1440 BGRA is ~40 MB of read+write traffic per frame**, which on this M4 Max
  (≈500 GB/s) is ~0.1 ms of bandwidth plus one pass launch — under the noise floor and well under the
  1 ms line the owner set.

So: not worth chasing for frame time. The only argument left for Direct is latency (one stage removed
from input-to-photon), which nothing in this tree measures.

## What would actually get Direct back (not done)

Create the window **fullscreen at creation** when the panel is already on the target mode — the old
creation path, which produced the Space, gated so it never issues a display-mode set (the panic was a
mode set with a live swapchain, not fullscreen creation itself). Concretely: keep the plain-window path
when `fullscreen_mode` moves the panel to a *different* mode, and use fullscreen-at-creation when the
mode is already current (an exclusive launch from an already-1x desktop, or `fullscreen_mode=desktop`).
Verify with the HUD from a screenshot, never from the layer dump — its `fullscreen` field disagreed
with SDL's flag in all four runs (SDL flag 1, dump 0).

## Harness notes (learned the hard way today)

- Drive visible runs through the **app's** settings: `tools/macos/run.sh` reads the checkout's
  `bbport.ini`, where `layer_opaque` is absent; without `BB_LAYER_OPAQUE=1` the layer logs `opaque 0`
  and every arm is composited by configuration.
- A scripted run can leave another app frontmost (`frontmost app com.tinyspeck.slackmacgap`), which
  turns Game Mode off; call `SDL_RaiseWindow` after entering fullscreen.
- Read the HUD from a `screencapture -x` crop of the top-right corner. It is the only reliable tell:
  the layer dump and SDL's fullscreen flag both disagreed with it today.
