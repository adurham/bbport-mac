# macOS: the title-screen artifact vs Direct scanout

The report: with `layer_opaque=1` (our override of KosmicKrisp's deliberate `opaque=NO`) the Metal
HUD reads **2560x1440 / Direct / Game Mode: On**, and a torn, white, streaked band shows in the
bottom right of the title screen. Direct needs an opaque layer (a non-opaque CAMetalLayer is always
composited), so the choice is: fix the artifact with `opaque=1`, or give Direct up.

This page is the procedure that decides which. `layer_opaque` stays `1` by default until it is run.

## The owner's own description (2026-10-17, asked to route the matrix)

Asked one word (flickering or static), the owner corrected the picture: it is **not** around the
title art; it sits at the **very bottom right of the screen**, it is **static** (the same every
frame), and it looked **"stretched way too much"**. His session: 16:9 2560x1440 exclusive
fullscreen, i.e. condition 1 of the matrix below (opaque=1, Fifo).

What that routes, before any capture: a tear moves or flickers, so the tearing explanations
(rules 2 and 4) are deprioritized as the primary mechanism. The survivors are rule 1 (a band in
the frame the port drew — it would sit identically in the port's own pre-present capture) and
rules 3/5 (the opaque=1 Metal 4 drawable path, or the scaled 2560x1440 scanout mode — "stretched"
points at a scaling seam). The captures still decide:

- First, owner-free: a hidden run stopped at the **title** (no Continue press) with a port capture
  (`BB_BUGREPORT_TRIGGER`) at the title. Static band present in that capture → render-side (rule 1);
  the whole visible-condition matrix is then unnecessary for attribution. Absent → present/scanout
  side, and the visible conditions 1-5 (each needs the owner's go-ahead) are what remain.
  Script: `<metalfx worktree>/out/title_capture.sh` (v3 filmstrip). **Result 2026-10-17:** ran
  hidden; 14 captures from the Japan Studio splash through the title menu; the twelve
  title-screen frames (hidden window's drawable, 1512x982) are CLEAN in the bottom-right —
  identical across ~90 s, no band. The port's drawn frame is therefore ruled out at this
  geometry; the artifact's remaining home is the visible fullscreen presentation (the
  opaque/Metal4 drawable path or the DCP-scaled 2560x1440 mode — the rules 3/5 family), pending
  the visible conditions on the owner's setup. The result section is at the end of this page.
- Successive real screen captures ~100 ms apart should show a static band in the SAME place
  (a tear would not).

## What is already known (from the owner's logs, before any new run)

- **The owner's runs were already vsynced.** The video core asks for Mailbox; KosmicKrisp offers
  only Immediate and Fifo, so every run so far fell back to **Fifo**
  (`Present mode Mailbox is not supported, using Fifo`) and the layer dump says
  `displaySync 1`. The title's ~200-235 FPS at 240 Hz is FIFO pacing at the display rate, not an
  unsynced present. So the leading hypothesis ("Direct exposes an unsynced present; composited
  mode hid the tear") is **already contradicted for the runs that showed the artifact**, unless
  the drawable path itself ignores `displaySyncEnabled` under Direct. The matrix below still runs
  vsync on/off: Immediate must make tearing *visibly worse* if tearing is the mechanism.
- **The 2560x1440 mode is not 1:1** (see "Scanout verdict"): it is a non-native framebuffer
  scaled up to the 3440x1440 panel. So "Direct" at 2560x1440 means the display pipe (DCP) scales
  our drawable on scanout. That makes the mode switch itself a suspect, hence condition 5.
- KosmicKrisp presents through the Metal 4 queue (`MTL4CommandQueue waitForDrawable /
  signalDrawable`, `kk_queue.c`), and sets `opaque=NO` on hardware devices "to work around a Metal
  bug when presenting through Metal4" (`wsi_common_metal.c`). Forcing `opaque=1` re-exposes
  whatever that bug is.
- The port's own capture (Ctrl+Shift+C) reads back the swapchain image **before present**: it
  cannot see a tear or anything the scanout/compositor does. Only a real screen capture can.

## The controls (this branch)

| What | How (one run) | Persistent |
|---|---|---|
| Layer opacity | `BB_LAYER_OPAQUE=1` / `=keep` (keep: KosmicKrisp's own `opaque=NO`) | `layer_opaque=1` / `keep` in bbport.ini |
| Vsync / present mode | `BB_VSYNC=1` (Fifo) / `BB_VSYNC=0` (Immediate), or `BB_PRESENT_MODE=fifo\|immediate` (wins over `BB_VSYNC`) | `present_mode=fifo\|immediate` or `vsync=1\|0` |
| Display mode | the output resolution in the game's System menu (2560x1080 → the 2560x1440 mode; 3440x1440 → native) | `output_res` |

Every run logs the result on one line, e.g.

    Present mode: Immediate (vsync off, may tear; asked Immediate by present_mode/vsync setting; surface offers Immediate, Fifo)

and the layer dump shows `displaySync 0|1` and `opaque 0|1`; `Layer check (+4 s)` confirms the
opacity override held; `Layer dump: scanout:` / `Display mode (...): ... scanout:` give the 1:1
verdict for the mode in use.

## Taking a sample: `tools/macos/capture_condition.sh LABEL`

Start the game for the condition, go to the title screen, wait until the artifact shows, then
from a second terminal (or ssh):

    bash tools/macos/capture_condition.sh opaque1-fifo

It requests the port's capture (through `BB_BUGREPORT_TRIGGER`, exported by `run.sh`; the same
code path as Ctrl+Shift+C) and at the same moment takes 3 real screen captures ~100 ms apart
(`screencapture -x`, the whole display, post-scanout). It writes
`out/attribution/<time>-<label>/` (`screen-1..3.png`, `port-frame.png`, a copy of the log) and
appends a section to `out/attribution/report.md` with the present mode, layer dumps, display mode
and scanout verdict. `out/` is `~/Library/Application Support/bbport/out` for the app,
the checkout's `out/` for a checkout (`BB_DATA_DIR` overrides).

- Note the HUD's reading (Direct / Composited) in the report's verdict line, and whether the
  artifact is in the screen captures and in `port-frame.png`.
- A black screen capture means the terminal lacks the Screen Recording permission
  (System Settings → Privacy & Security → Screen Recording).
- The screen captures are at the desktop's backing size while a HiDPI desktop is up, and at the
  mode's pixels while the game holds the 2560x1440 / 3440x1440 mode; compare regions, not pixels.

## The matrix (title screen, same display mode: output 2560x1080 → the 2560x1440 mode)

| # | Condition | Launch with | Label |
|---|---|---|---|
| 1 | opaque=1, vsync on | `BB_LAYER_OPAQUE=1 BB_VSYNC=1` | `opaque1-fifo` |
| 2 | opaque=1, vsync off | `BB_LAYER_OPAQUE=1 BB_VSYNC=0` | `opaque1-immediate` |
| 3 | opaque=keep, vsync on | `BB_LAYER_OPAQUE=keep BB_VSYNC=1` | `opaquekeep-fifo` |
| 4 | opaque=keep, vsync off | `BB_LAYER_OPAQUE=keep BB_VSYNC=0` | `opaquekeep-immediate` |
| 5 | opaque=1, vsync on, **native mode** (output 3440x1440) | `BB_LAYER_OPAQUE=1 BB_VSYNC=1`, output 3440x1440 in the menu | `opaque1-fifo-native3440` |

Expected HUD: 1, 2, 5 Direct; 3, 4 Composited. If 1/2/5 do not say Direct the run is not
testing what it should: check `Layer check (+4 s): opaque 1 (as overridden)`.

Condition 1 is what the owner has been running (vsync was already on, see above).

### Decision rules

Read the screen captures (S) and the port frame (P) of each condition.

1. **Artifact in P (any condition)** → it is in the frame the port drew: a rendering bug (title
   UI / letterbox / upscaler path), not presentation. Neither opacity nor vsync is the lever; keep
   Direct and fix the render side. (Expected to be ruled out: the artifact is only reported with
   opaque=1.)
2. **1 clean (S), 2 artifacts** → tearing exposed by Direct; FIFO hides it. Ship opaque=1 with
   Fifo (the default already resolves to Fifo; make it explicit) and **keep Direct**.
3. **1 and 2 both artifacts in S, P clean, 3 and 4 clean** → not tearing (vsync makes no
   difference) and not the frame: it is the opaque layer on the Metal 4 drawable path — the
   "Metal bug when presenting through Metal4" KosmicKrisp dodges with opaque=NO. → **Default
   `layer_opaque` back to opt-in (keep)** until understood; lose Direct for now. Then condition 5
   says whether it is general or tied to the scaled mode:
   - **5 clean** → the bug needs opaque=1 *and* a non-native (DCP-scaled) mode. Option: keep
     opaque=1 only when the mode is native 1:1 (`bb_output_is_native_mode`), keep otherwise.
   - **5 artifacts** → opaque=1 is broken on this drawable path in any mode: opt-in only.
4. **2 worse than 1 but 1 still shows some** → both: some tearing (Immediate) plus a residual
   presentation fault under opaque=1; treat as rule 3 for the default, keep Fifo.
5. **3 or 4 show it too** → it is not caused by opaque=1; Direct is innocent. Keep opaque=1 and
   look at the mode switch (condition 5) and the frame (P).

The "same display mode" constraint matters: switching between 2560x1440 and 3440x1440 changes
the scanout path (scaled vs 1:1), so conditions 1-4 must all use the 2560x1440 mode and only
condition 5 changes it.

## Scanout verdict: the "2560x1440 1x" mode is not 1:1

Measured on the owner's display (Dell AW3425DW, 3440x1440 panel, M4 Max, macOS 27.2) with
CoreGraphics (`CGDisplayCopyAllDisplayModes` with duplicate low-resolution modes,
`CGDisplayModeGetPixelWidth/Height`, `CGDisplayModeGetIOFlags`):

| Mode (pixels) | points | ioflags | native flag | verdict |
|---|---|---|---|---|
| 3440x1440 @240 (ids 99/100) | 3440x1440 | 0x02000003 / 0x02000007 | **1** | 1:1: the panel's native pixels |
| 2560x1440 @240 (ids 95/96, the picker's "1x") | 2560x1440 | 0x00000003 | **0** | **scaled up** to the panel |
| 6880x2880 @240 (id 149, the desktop's "looks like 3440x1440") | 3440x1440 | 0x00200003 | 0 | HiDPI backing store, scaled down |

The panel's EDID/DisplayPort timings (IOKit `TimingElements`) do list 2560x1440 timings, so the
link may carry a 2560x1440 signal that the **monitor's** scaler stretches, or macOS may keep the
native timing and scale in the display pipe — CoreGraphics does not say which, and IOKit exposes
no current-timing field for this display that would. Either way the image is resampled from
2560 to 3440 columns (1.34x, non-integer), which is exactly why text is soft there. Only the
native-flagged 3440x1440 mode is 1:1. (2560x1440 on a 3440x1440 ultrawide also does not fill the
panel's 21:9 shape without stretching or pillarboxing.)

The port now prints this verdict at each swapchain (`Layer dump: scanout: ...`) and after each
mode switch and capture (`Display mode (...): ... scanout: ...`).

Recommendation independent of the artifact: on this panel the fullscreen mode for a 21:9 output
should be the native 3440x1440 mode (render lower and let the upscaler/presenter scale if needed);
the 2560x1440 mode costs sharpness and is a second, scaled scanout path.

## The hidden-title capture result (2026-10-17)

`out/title_capture.sh` (metalfx worktree, v3) ran the game hidden, never pressed Continue, and
took a filmstrip of the port's own pre-present captures — 14 frames: two of the Japan Studio
splash, twelve of the title menu, across about 90 seconds. The bottom-right corner of every
title frame is clean and identical frame to frame: **the band is not in the frames the port
draws**. Geometry caveat: the hidden window's drawable reads 1512x982 (the window's default
size), not the fullscreen shape, so this rules out a content bug that is independent of the
presentation geometry — it does not exercise the visible fullscreen path itself.

Interpretation: for the owner's visible fullscreen case the artifact most likely lives on the
presentation side — the opaque=1 Metal 4 drawable path and/or the scaled 2560x1440 scanout (the
rules 3/5 family above), not a rendering-content bug (rule 1). The visible conditions matrix
remains the way to finish the attribution; this result removes the cheapest explanation first.

Frames kept at `out/titlecap-v3-*.png` in the metalfx worktree; the mechanism (retriggered
`BB_BUGREPORT_TRIGGER` in a hidden warm run) is reusable for any future title-screen look.

## Found and fixed (2026-10-23): the UI edge-stretch snapping a text draw

The band is in the port's own frame (hidden run at 3440x1440, the drawable at the output size - the
2026-10-17 capture missed it only because its drawable was 1512x982). It is `EmitUiEdgeStretch`: on an
output wider than 16:9 the vertex shader moves UI vertices that sit on the game's 16:9 screen edge out to
the output's edge, so full-screen fills and backgrounds cover the ultrawide frame. The side-edge case
(the subtitles' backdrop band) fired on the title's legal line: its glyph quad's last vertices land on
x = 0.7442 of the output (the 16:9 edge) in the lower part of the screen, so they were dragged to the
output's right edge and the text smeared into a white streak.

The guard that was meant to keep text and pictures intact only skipped textures >= 2048 ("the title's
picture 2048x1024, the font page 2048x2048"). The legal line's page is **1024x512**, so it passed the
guard. Traced with a temporary `BB_UI_SNAP_TRACE`: `edges 0.7442,1.0000 img 1024x512 indices 30` - armed,
every frame.

Fix (`gpu/render/raster_targets.cpp`): the snap is now skipped for every draw that samples a texture, not
just the >= 2048 ones. Untextured fills (menus' dimming, fades, subtitle bands) keep it, which is what the
mechanism exists for; textured content keeps its proportions, which was always the intent. Verified on a
hidden 3440x1440 title capture: the streak is gone, the text reads clean, and the frame still fills the
panel (no seam at the 16:9 edge; the brightness fall-off to the corners is the title art's vignette).
