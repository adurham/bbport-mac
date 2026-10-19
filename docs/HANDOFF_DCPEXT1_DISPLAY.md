# Handoff: DCPEXT1 panics on a HiDPI + forced-HDR + 240 Hz BetterDisplay setup

Paste this as the opening prompt for the session that picks the work up.

---

You are picking up a display-subsystem investigation on the owner's MacBook Pro (M4 Max,
Mac16,6), macOS 27.2 (26B5112a). On 2026-10-18 the machine hard-panicked three times, all
display-coprocessor faults, and all three trace to a display-mode transition inside a
BetterDisplay configuration that combines a 2x HiDPI desktop mode, forced HDR and a 240 Hz
10-bit DSC link on an Alienware AW3425DW.

That configuration is deliberate and it works — the owner keeps the panel on its HiDPI desktop
mode at 240 Hz with HDR on and will not accept "turn HiDPI off", "turn HDR off" or "use native
1x" as the answer. The question is what makes the transition unsafe and how to keep the
configuration anyway.

## Goal

1. Establish exactly which display transitions fault this machine and which do not.
2. Produce either a configuration that survives (keeping HiDPI + forced HDR + 240 Hz) or a
   precise, mechanical rule plus a restore command — not a general warning.
3. Keep the Apple report current so the firmware bug gets fixed upstream.

## The three panics (all DCPEXT1, all `IOMFB int_handler_gated: failure: axi_rd_err`, faulting
task `iomfb_driver(9)`, no stackshot, `logd` loses the last ~40 s)

| # | wall time | report | register | what was happening |
|---|---|---|---|---|
| 1 | 2026-10-18 17:22:28 | `panic-full-2026-10-18-172321` | `0x40381400` | A game launch (bbport) 5.9 s in, which had just issued `CGDisplaySetDisplayMode` 775 → 724 |
| 2 | 2026-10-18 23:26:44 | `panic-full-2026-10-18-232751` | `0x40381408` | Same launch path; the set landed, scanout went 1:1 at 3440x1440/240, ran 120 fps for ~110 s, then faulted |
| 3 | 2026-10-18 23:54:41 | `panic-full-2026-10-18-235555` | `0x40381400` | **No app running.** The owner turned HiDPI off in BetterDisplay by hand |

Panic 3 is the decisive one: it removes every third-party surface from the trigger and leaves the
mode transition itself.

## The current configuration — read it, never trust a screenshot

```sh
BD=/opt/homebrew/bin/betterdisplaycli; N='-name=AW3425DW'
$BD get $N -displayModeNumber   # 775  — the 2x HiDPI desktop mode: "looks like 3440x1440",
                                #        6880x2880 backing, nominal 60 Hz
$BD get $N -connectionMode      # 1945588021712129392 — 3440x1440 239.98Hz Fixed 10bit
                                # HDR10 RGB Full P3 (7680x4608)
$BD get $N -hdr                 # on  (the Forced HDR10 RGB Full Range pathway; UI-armed)
$BD get $N -DSCReport           # DSC active
defaults read pro.betterdisplay.BetterDisplay | grep protect
#   protectHDR@Display:25 = 1
#   protectRefreshRate@Display:25 = 240Hz
#   protectRefreshRateMode@Display:25 = 155
#   protectHDRProfileURL@Display:25 = .../AW3425DW-F74F556F...icc
# combinedBrightness 1.0, software gamma -0.25
```

The shape of the problem: the **desktop mode** (775) and the **connection mode** (240 Hz, 10-bit
HDR10, DSC) are two separate BetterDisplay knobs pulling on one link, and the connection mode's
own label carries a `(7680x4608)` plane — neither the panel's 3440x1440 nor the mode's 6880x2880.
Every transition between those two states re-parameterises the DCP pipe.

Desktop-mode history (WindowServer's own log, bucketed by mode id):

- 2026-10-16 08:50 → 2026-10-17 08:06: modes 149/150/151/152/155 (the gold-standard setup was
  landed this way, with successful mode sets and a link reinitialisation)
- 2026-10-17 08:21 → 2026-10-18 17:21: **775** (the injected 2x mode)
- Every panic since then has been a transition out of 775.

## Ruled out / already fixed

- **The game (bbport) is exonerated.** Panic 3 had no bbport process, no game, no Metal surface.
  The port now mechanically refuses to move the panel: `fullscreen_mode` is clamped to `desktop`
  in the parser (ini and `BB_FULLSCREEN_MODE`) and refused again at the call site that would issue
  `CGDisplaySetDisplayMode`; `BB_ALLOW_DISPLAY_MODE_SET=1` is the only override. Both inis are on
  `desktop` + `retina=1`. Do not undo this and do not re-enable `fill`/`exclusive` to "test".
- RAM / memory pressure is not involved (`memoryPressure: false`, no stackshot).
- Not a single unexplained event: three panics, each with a mode transition at or near it.

## Leading hypotheses, ranked

- **H1 — plane-geometry mismatch across the transition.** The DCP's fetch descriptor is built for
  the 2x plane (6880x2880, or the 7680x4608 padded/DSC geometry named in the connection mode) and
  a transition off it re-parameterises the pipe while a stale descriptor is still live. Fits the
  variable latency (instant once, ~110 s once — a deferred unmap) and the `axi_rd_err` (a fetch
  past the mapped surface).
- **H2 — a second actor.** BetterDisplay's `protectHDR` / `protectRefreshRate` / `protectRefreshRateMode=155`
  flags re-assert the 240 Hz HDR connection mode during or right after the desktop-mode change,
  i.e. two mode operations in flight. Test by disabling the protections for one run.
- **H3 — the 775 mode definition itself.** All working sets happened in the 149–155 mode family;
  every failing one was out of 775, which arrived at the 2026-10-17 08:21 boot. Something about
  that mode (nominal 60 vs 240, scaler path, injected id) is different from the family that worked.
- **H4 — the dock.** The AW hangs off a Surface TB4 dock, and the fault is on `DCPEXT1` (external
  pipe). DSC renegotiation through the dock may be part of the path even if it is not the trigger.

## Read-only investigation, do this before touching the panel

1. Bucket the mode history properly (the log store keeps several boots):

```sh
log show --start "2026-10-16 00:00:00" --style compact 2>/dev/null \
 | grep -a "SLSGetDisplaysWithOpenGLDisplayMask" \
 | sed -E 's/^([0-9-]+ [0-9:]+).*Display (0x[0-9a-f]+): Mode ([0-9]+).*\[([0-9]+ x [0-9]+)\].*/\1|\2|\3|\4/' \
 | awk -F'|' '{k=$2"|"$3"|"$4; if(!(k in f)) f[k]=$1; l[k]=$1; n[k]++} END{for(k in n) printf "%s n=%d first=%s last=%s\n",k,n[k],f[k],l[k]}' | sort
```

2. Dump the mode table BetterDisplay offers and pull the injected 775 definition out of
   `~/Library/Preferences/pro.betterdisplay.BetterDisplay.plist` (mode id, pixel size, density,
   refresh fields, scaler flags). Compare it against a 150/155-era record if one is still stored.
3. Read the log around the **successful** 2026-10-16 reinitialisation + mode set — proof that mode
   sets can succeed on this machine — and diff that sequence against the pre-crash windows of
   2026-10-18. The 2026-10-16 sequence is the only known-good reference.
4. Search the whole log store for **any** successful transition out of 775 (to 724 or to anything
   else). If there is not a single one, 775 is a one-way door and that is the answer.
5. Decode nothing by guesswork: the register offsets `0x40381400` / `0x40381408` are unresolved and
   are part of the Apple ask.

## Experiment matrix — each one can hard-reboot the machine

Ordered by information per reboot. **One experiment per explicit go-ahead, machine idle, owner
present, nothing launched by you.** Bank evidence (copy the panic reports, take a sysdiagnose)
before each attempt.

- **E1** — leave the desktop mode alone and change only the *connection* mode to the 165 tier
  (`~/bin/display-gold-165`, no forced HDR). Does a connection-only change fault?
- **E2** — 775 → 152 (the HiDPI 120 tier). Does the fault follow "leaves 775" or "leaves HiDPI"?
- **E3** — the reverse direction: 724 (1x) → 775. Is the transition dangerous only one way?
- **E4** — drop the Forced HDR colour row first (pure SDR link), then leave 775. Tests H1/H2: is
  forced HDR / DSC the ingredient?
- **E5** — the failing case with the dock out of the path (direct cable), if the owner will re-cable.

## Hard rules

- Never propose dropping HiDPI, dropping HDR, or native 1x as the fix. The owner keeps this
  configuration deliberately; make it safe, or state precisely what cannot be made safe.
- No display-mode set without an explicit yes for that one run. No GUI automation of any kind
  (no `computer_use`, no AppleScript/System Events).
- Prefer a read-only readout over an eyeball every time; every claim gets a number or a log line.
- Do not disable the bbport gate, and do not "temporarily" re-enable a panel-moving fullscreen
  mode to reproduce anything.

## Artifacts and references

- `~/Library/Logs/DiagnosticReports/Retired/panic-full-2026-10-18-172321.0002.panic` (and
  `-232751`, `-235555`)
- `/var/tmp/sysdiagnose_2026.10.18_17-25-47-0500_macOS_Mac_26B5112a`,
  `/var/tmp/sysdiagnose_2026.10.18_23-28-56-0500_macOS_Mac_26B5112a`
- `~/repos/bbport-mac-native/docs/DISPLAY_MODE_2026-10-18.md` — mode history, the ordering work,
  the gate, the third panic
- `~/repos/bbport-mac-native/docs/APPLE_FEEDBACK_2026-10-18.md` — the report draft; extend it
- Skill `macbook-xdr-brightness-betterdisplay` — the gold-standard config, the `betterdisplaycli`
  quirks (including the "60 Hz nominal label" trap), the restore presets (`~/bin/display-gold-240`,
  `-165`, `display-hdr-desktop`, `display-hdr-movie`)
- Repo commits for the gate: native `9adab4a`, main `fe68875`

## Done means

1. A written statement of which transitions fault and which do not, backed by log evidence or one
   controlled experiment.
2. Either a configuration that keeps HiDPI + forced HDR + 240 Hz without faulting, or a precise
   rule ("never leave mode 775; use X instead") with a tested one-command restore.
3. The Apple report updated with panic 3 and with whatever the experiments showed.
