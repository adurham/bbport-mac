# The transition that faults, and the rule that keeps HiDPI + forced HDR + 240 (2026-10-19)

Read-only investigation of the three DCPEXT1 `axi_rd_err` panics of 2026-10-18. No display
mode was set and no experiment was run to produce this document: every claim below comes
from WindowServer's own display log, CoreGraphics/SkyLight's mode tables, the monitor's
DDC register, and the three panic reports.

## Bottom line

- **The steady state is not the problem.** HiDPI + forced HDR10 + 240 Hz (DSC) has run for
  hours at a time on this machine. Nothing about the configuration at rest faults.
- **What faults is a display-mode set that changes the AW's *pipe source width*** —
  6880x2880 <-> 3440x1440, i.e. any 2x (HiDPI) <-> 1x change — **while the 240 Hz 10-bit
  HDR10 (DSC) connection mode is up.** All three panics were that set. One of them is
  logged in full and carries the mechanical signature (§4).
- **What does not fault is any set that keeps the source width**: 769 <-> 775
  (both 6880x2880), 150 <-> 151, 152 -> 150. Twenty-two such sets are logged after the link
  went 10-bit, thirteen of them 769 <-> 775, one of them with the desktop left on 769 for
  4 h 29 min. Zero faults.
- **The mode change alone is not enough.** The same 2x -> 1x reconfiguration, with the same
  WindowServer signature (`modeset_required: 1, resource_release_required: 1`, pipe width
  6880 -> 3440), was performed **ten times on 2026-10-16 on the 8-bit 4:2:2 link and never
  faulted** (§5). The difference between those ten and the three panics is the link
  format, not the mode number and not the scale change.

So the answer to "how do we keep HiDPI + forced HDR + 240 Hz" is: **keep it, and never
issue a scale-changing mode set while that link is up.** The configuration survives; the
transition does not. §6 is the rule, §7 the recoveries, §8 the three landmines found in
the existing tooling.

Correction to the earlier write-up (`DISPLAY_MODE_2026-10-18.md`): its third-panic
conclusion — "the 775 -> 1x transition is not survivable by anyone who issues it" — stands,
but its framing ("775 arrived at a boot, every panic since is a transition out of 775")
over-attributes to the mode number. 775 is not a one-way door: it was left for 769 three
times (§3), and the fault follows the *plane change on the HDR link*, not 775 itself.

## 1. The mode table as it is now — ids are not stable

`betterdisplaycli get -name=AW3425DW -displayModeList` (949 entries), the SkyLight mode table
and WindowServer's own resolved list all agree. WindowServer prints it with the pixel size in
parentheses, which is the field that decides the hazard:

```
769 RGhA [3440 x 1440 (6880 x 2880) 2x @ 240.0Hz], dp-x = 2.000 (scaler, safe, fake_vfm)
775 RGhA [3440 x 1440 (6880 x 2880) 2x @  60.0Hz], dp-x = 2.000 (scaler, safe, fake_vfm)
724 RGhA [3440 x 1440 (3440 x 1440) 1x @ 240.0Hz], dp-x = 1.000 (scaler, native, safe, default)
```

CoreGraphics's `CGDisplayCopyAllDisplayModes` does **not** list 769-774 (a known gap — do not
conclude "the mode does not exist" from it).

| id | point size | pixel size | density | nominal Hz | note |
|---|---|---|---|---|---|
| 724 | 3440x1440 | 3440x1440 | 1.00 | 240 | 1x native, what the port used to request |
| 725 | 3440x1440 | 3440x1440 | 1.00 | 240 | 1x, VRR-capable twin |
| 726/727/728 | 3440x1440 | 3440x1440 | 1.00 | 165/120/60 | 1x |
| **769** | 3440x1440 | **6880x2880** | **2.00** | **240** | **HiDPI 240 — the gold-standard desktop mode** |
| 770 | 3440x1440 | 6880x2880 | 2.00 | 240 | HiDPI 240, VRR twin |
| 771/772 | 3440x1440 | 6880x2880 | 2.00 | 165/120 | HiDPI 165 / 120 |
| **775** | 3440x1440 | **6880x2880** | **2.00** | **60** | **HiDPI 60 — the mode that is current now** |
| 776-780 | 3440x1440 | 6880x2880 | 2.00 | 60-24 | HiDPI slow variants |

Two facts that matter:

- **769 and 775 are the same plane.** Same 6880x2880 framebuffer, same pipe width, same
  link. They differ only in the mode's *nominal* refresh (240 vs 60). The panel is actually
  scanning 240.00 Hz right now with 775 selected — read from the monitor's own DDC register
  (VCP 0xAE = 240.00 Hz), not from CoreGraphics.
- **The old ids in the scripts are gone.** 150 is now `1583x663 HiDPI 240` (3166x1326
  backing) and 155 is now `1600x670 1x 240`. The table was rebuilt when the link was
  re-initialised; the numbers were reused for different sizes. See §8.

## 2. Every AW mode set in the log store (2026-10-16 .. 2026-10-19)

WindowServer logs each set as `client api - modeset <id in hex>` followed by
`desired mode change A -> B`, the resources it must request, and whether the DCP needs a
mode set. This is the ledger (AW display only; the built-in panel's sets are omitted):

| when | from -> to | pipe width after | modeset / release required | link when it ran | outcome |
|---|---|---|---|---|---|
| 10-16 12:41:46 | 149 -> 99 | 3440 | 1 / 1 | YCbCr422 8-bit limited | ok |
| 10-16 12:47:49 | 149 -> 99 | 3440 | 1 / 1 | YCbCr422 8-bit limited | ok |
| 10-16 14:15:06 | 149 -> 99 | 3440 | 1 / 1 | YCbCr422 8-bit limited | ok |
| 10-16 14:36:08 | 149 -> 99 | 3440 | 1 / 1 | YCbCr422 8-bit limited | ok |
| 10-16 14:39:13 | 149 -> 99 | 3440 | 1 / 1 | YCbCr422 8-bit limited | ok |
| 10-16 15:12:21 | 149 -> 99 | 3440 | 1 / 1 | YCbCr422 8-bit limited | ok |
| 10-16 15:54:41 | 149 -> 99 | 3440 | 1 / 1 | YCbCr422 8-bit limited | ok |
| 10-16 16:05:48 | 149 -> 99 | 3440 | 1 / 1 | YCbCr422 8-bit limited | ok |
| 10-16 16:28:59 | 149 -> 99 | 3440 | 1 / 1 | YCbCr422 8-bit limited | ok |
| 10-16 18:35:27 | 149 -> 99 | 3440 | 1 / 1 | YCbCr422 8-bit limited | ok |
| 10-16 19:08:29 | 149 -> 152 | 6880 | 0 / 0 | RGB 10-bit full | ok |
| 10-16 19:53:53 | 152 -> 150 | 6880 | 0 / 0 | RGB 10-bit full | ok |
| 10-16 20:04:38 | 150 -> 151 | 6880 | 0 / 0 | RGB 10-bit full | ok |
| 10-16 20:04:48 | 151 -> 150 | 6880 | 0 / 0 | RGB 10-bit full | ok |
| 10-16 20:10:42 | 150 -> 151 | 6880 | 0 / 0 | RGB 10-bit full | ok |
| 10-16 20:10:57 | 151 -> 150 | 6880 | 0 / 0 | RGB 10-bit full | ok |
| 10-16 20:12:15 | 150 -> 151 | 6880 | 0 / 0 | RGB 10-bit full | ok |
| 10-16 20:12:24 | 151 -> 150 | 6880 | 0 / 0 | RGB 10-bit full | ok |
| 10-16 20:20:32 | 150 -> 155 | 6880 | 0 / 0 | RGB 10-bit full | ok |
| 10-17 08:16:00 | 155 -> 150 | 6880 | 0 / 0 | RGB 10-bit full | ok |
| 10-17 08:19:35 | 769 -> 775 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |
| **10-17 15:59:35** | **775 -> 769** | **6880** | **0 / 0** | RGB-PQ 10-bit full | **ok — held 4 h 29 min** |
| 10-17 20:28:12 | 769 -> 775 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |
| 10-18 11:24:50 | 775 -> 769 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |
| 10-18 11:25:20 | 769 -> 775 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |
| 10-18 17:23:49 | 775 -> 769 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |
| 10-18 17:24:12 | 769 -> 775 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |
| 10-18 22:14:58 | 775 -> 769 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |
| 10-18 22:15:56 | 769 -> 775 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |
| **10-18 23:24:55** | **775 -> 724** | **3440** | **1 / 1** | **RGB-PQ 10-bit full, DSC 240** | **panic 108.6 s later** |
| 10-18 23:28:07 | 775 -> 769 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |
| 10-18 23:28:35 | 769 -> 775 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |
| 10-18 23:56:11 | 775 -> 769 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |
| 10-18 23:56:42 | 769 -> 775 | 6880 | 0 / 0 | RGB-PQ 10-bit full | ok |

The other two panics have no entry because `logd` lost the tail: the last AW line before
17:22:28 is 17:21:23 and before 23:54:41 is 23:54:04. Both were 775 -> 1x by construction
(the port's SDL set at startup, and the owner's "HiDPI off" in BetterDisplay).

So: **22 same-plane sets, 0 faults. 1 logged cross-plane set on the HDR10 link, fatal, plus
2 unlogged of the same shape, fatal.**

## 3. 769 works, and it works for hours

The strongest single rebuttal of "775 is a one-way door": on 2026-10-17 at 15:59:35 the
desktop was set to **769** and stayed there until 20:28:12 — 4 h 29 min, including a dock
hotplug at 20:27:44 that the machine survived. The image ran the whole time at HiDPI 240.

The shorter 769 excursions (10-18 11:24, 17:23, 22:14, 23:28, 23:56) are all wake/boot
re-enumerations followed by BetterDisplay restoring the saved 775.

Nothing about 769 is untested. It is the documented gold-standard desktop mode
("HiDPI 240"), and the only reason the machine is on 775 today is that the BetterDisplay
mode menu is filtered to `60Hz_refreshString` + `HiDPI_backingScaleFactor`
(`displayModeMenuActiveFilterTagIds@Display:25`), which selects the 60-nominal variant of
the same HiDPI geometry.

## 4. What a fatal set looks like in the log

2026-10-18 23:24:55.404, the port's `CGDisplaySetDisplayMode` (mode 0x2d4 = 724):

```
client api - modeset 2d4 for display 4
desired mode change 775 -> 724
CA Display width 3440, bandwidth 9510          <-- the pipe's source width halves
Parsing modeset_required and resource_release_required
request_resources: IOMFB needs a modeset on display 0x4 for pipe reconfiguration.
request_resources: Doing a mode set on display 0x4.
For display 0x4, hw_reconfig_required: 0, modeset_required: 1, resource_release_required: 1
  setting unmirror mode 724
  setting mode 775 -> 724
Performing mode refresh.
  current mode: <CADisplayMode 3440 x 1440 fmt:RGB_PQ_10bit range:full>
```

Compare a same-plane set at 22:14:58.518 (775 -> 769):

```
desired mode change 775 -> 769
CA Display width 6880, bandwidth 23777         <-- unchanged
For display 0x4, hw_reconfig_required: 0, modeset_required: 0, resource_release_required: 0
  setting mode 769 -> 775
```

Three fields separate the two classes, and they are all readable after the fact:

1. **`CA Display width`** — 6880 (plane kept) vs 3440 (plane halved).
2. **`request_resources: IOMFB needs a modeset ... for pipe reconfiguration`** — present only
   when the plane changes.
3. **`resource_release_required: 1`** — appears exactly once in the entire log store, on the
   set that preceded the 23:26:44 panic.

The fault is **delayed**: the set completed and the pipe ran 1:1 at 3440x1440/240 for 108.6 s
before `axi_rd_err`. That is consistent with a deferred unmap/stale fetch descriptor rather
than a failed set — the DCP had already been re-parameterised and was scanning happily when
it touched memory the old plane owned. It also explains the first panic's immediate fault
(same operation, different timing).

What is *not* in the log: the faulting register's semantics (0x40381400 / 0x40381408 remain
unresolved), the 65 s before panic 1, and the 37 s before panic 3.

## 5. The control: the same reconfiguration on a different link, ten times, no fault

On 2026-10-16 the AW's live link format was `YCbCr422_8bit range:limited` — the stock
8-bit 4:2:2 mode (no DSC needed at 3440x1440@240: 19.0 Gbps). Ten times that day the
desktop was switched **2x -> 1x** (149 -> 99), and each one produced the *same* pipe
signature as the fatal set:

```
CA Display width 3440
request_resources: IOMFB needs a modeset on display 0x5 for pipe reconfiguration
hw_reconfig_required: 0, modeset_required: 1, resource_release_required: 1
```

…and the machine did not fault. The link changed to `RGBCalibrated_10bit/full` at 19:50:32
and to `RGB_PQ_10bit/full` (HDR10, PQ) at 19:50:33, and the 240 Hz DSC connection mode was
established at 20:20:32. After that, every cross-plane set has been fatal, and no
cross-plane set has been attempted that was not.

Four variables change together between the safe ten and the fatal three — 8-bit vs 10-bit,
4:2:2 vs RGB, SDR vs HDR10/PQ, and no-DSC vs DSC — so this establishes *that* the link is
the ingredient, not *which* field of it. §9 is how to separate them.

## 6. The rule

> **On the AW3425DW, with the 240 Hz 10-bit HDR10 (DSC) connection mode up, never issue a
> display-mode set that changes the pipe's source width.**
>
> Concretely: never switch the AW between a HiDPI (2x, 6880x2880 backing) desktop mode and
> a 1x (3440x1440) one — in either direction — while that connection mode is active.
> Everything inside the 2x family (769, 770, 771, 772, 775, 776 …) is safe, in any order,
> including refresh-rate changes that renegotiate the link (240 <-> 165 was toggled six
> times on 10-16 without incident).
>
> If a 1x mode is genuinely needed, take the link down first (unplug/replug the display, or
> reboot) so the pipe is rebuilt instead of re-parameterised in place. Do not "just set the
> mode back" from a 1x desktop — that is the same crossing in the other direction, and it is
> untested on this link.

The rule is enforceable because the trigger is always a `CGDisplaySetDisplayMode` /
`CGSConfigureDisplayMode` from some process. Enumerate the processes that can issue one:

| issuer | status |
|---|---|
| `bbport` (any fullscreen mode) | gated: `fullscreen_mode` is clamped to `desktop` in the parser and again at the set call site; `BB_ALLOW_DISPLAY_MODE_SET=1` is the only override. Leave it. |
| BetterDisplay "HiDPI off" (the panic-3 trigger) | do not use on this display. It is a 2x -> 1x set with a UI affordance that looks harmless. |
| BetterDisplay "protect refresh rate" | currently points at a stale mode id — see §8. |
| Any game/Whisky/D3D exclusive-fullscreen set on the AW | a crash button. Run games windowed/borderless on the HiDPI desktop (`fullscreen_mode=desktop` + `retina=1` gives a Direct-capable fullscreen with no set). |
| `display-gold-240` / `display-gold-165` | rewritten with a plane guard — see §8. |
| `system_profiler`-style readers, screenshot tools, DDC probes | read-only, never set. |

## 7. The recoveries

**Same-plane restore (the normal case: the desktop drifted to another 2x mode):**

```
~/bin/display-gold-240          # resolves "3440x1440 HiDPI 240Hz 10bpc" by name, guards the plane
DRY_RUN=1 ~/bin/display-gold-240   # same, prints what it would do, changes nothing
```

Verified in dry-run against the live table (2026-10-19): current mode 775
(`3440x1440 HiDPI 60Hz 10bpc`, 2x plane) -> target 769, "same plane: both 6880x2880",
`DRY_RUN=1 — nothing changed`. The live set itself has **not** been run yet; it needs the
owner's go-ahead like any mode set (it is the same class as the 13 safe sets in §2, but it
does still move the panel's nominal refresh from 60 to 240).

**From a 1x desktop (the dangerous state):** there is no safe software restore. Rebuild the
pipe instead:

1. unplug the AW3425DW's USB-C (or the dock's uplink) and plug it back in, or
2. reboot.

The display returns on its saved 2x mode (the persisted WindowServer config is
`Hz 60, Scale 2, Wide 3440` — a 2x mode), and the machine has done exactly this at every
boot and hotplug in the log without faulting.

**What not to do:** do not set 724/725/726/727/728 while the HDR link is up; do not use
BetterDisplay's HiDPI toggle to "get out of" a bad state; do not run the old
`display-gold-*` (they target 1583x663 modes now).

## 8. Three landmines found in the existing tooling

1. **`display-gold-240` targeted mode 150.** In the rebuilt table 150 is
   `1583x663 HiDPI 240Hz` — a 3166x1326 plane. Running the documented recovery would have
   both wrecked the desktop and issued a cross-plane set. Same for `display-gold-165`
   (mode 151 = `1583x663 HiDPI 48-240`). **Both rewritten** to resolve their target by name
   and to refuse any non-2x target or 1x starting state; `DRY_RUN=1` supported.
2. **`protectRefreshRateMode@Display:25 = 155`.** In the rebuilt table 155 is
   `1600x670 1x 240Hz`. If BetterDisplay's refresh-rate protection ever applied that id by
   number it would issue a 2x -> 1x set — the fatal operation. It does not appear to be
   applying it today (nothing in the log shrinks to 1600x670), but the value is a stored
   index into a table that has already been rebuilt once. It should be re-pointed at 769
   (or the protection disabled) before anything else touches the mode table.
3. **The BetterDisplay mode menu filter is pinned to `60Hz` + `HiDPI`**
   (`displayModeMenuActiveFilterTagIds@Display:25`). That is why the desktop sits on 775
   (HiDPI **60**) rather than 769 (HiDPI **240**), which is what makes the "nominal 60 Hz"
   reporting look alarming. The filter is a UI-only setting; nothing depends on it, but it
   will keep re-selecting the 60-nominal variant every time someone picks from the menu.

## 9. The experiment matrix, rewritten against the current ids

The matrix in the handoff was written against the old numbering (775 -> 152, 724 -> 775,
etc.); those ids mean different modes now. What is still worth testing, in risk order.
Each one can hard-reboot the machine, so each needs its own explicit go-ahead, with the
machine idle, the owner present, and the panic reports + a sysdiagnose banked first.

- **X1 — the restore (lowest risk).** `~/bin/display-gold-240` for real: 775 -> 769, same
  plane, the class that is 13/13 safe. Proves the tested restore command and lands the
  documented gold standard (HiDPI 240 + HDR + DSC). Prediction: no fault; panel still
  240.00 Hz by DDC; `DSCReport` still Yes.
- **X2 — does the plane change fault at 165 Hz without HDR?** `~/bin/display-gold-165`
  first (775 -> 771, same plane, safe), then one cross-plane set (771 -> 727). If that
  survives, the HDR10/DSC link is the ingredient and the 1x escape hatch exists. If it
  panics, the 240 Hz link rate is the ingredient instead. This is the experiment that
  decides the Apple report's framing.
- **X3 — which field?** With the link at 165, drop to 8-bit SDR and repeat the cross-plane
  set; then 10-bit SDR; then 10-bit HDR (PQ) without DSC (120 Hz 10-bit fits without
  compression). Four arms, four reboots at worst. This is the only way to name the field.
- **X4 — 1x -> 2x (the reverse).** 724 -> 769. Untested on this link and the one that
  matters for recovery-from-1x. Highest risk of the set; do it last, or never.
- **X5 — the dock.** Repeat X2 with a direct cable if the owner will re-cable. Only worth
  doing if X2 survives and X3 points at DSC.

Nothing in this list is needed to *state* the rule; §2's ledger already separates the
classes. X1 is needed only to convert "same plane is safe, 13/13" into "this command is
tested", and X2/X3 are for Apple's benefit (which field of the link, so the report can name
the bug precisely).

## Evidence

- WindowServer display log, 2026-10-16 .. 2026-10-19: `log show --start "2026-10-16 00:00:00"
  --predicate 'process == "WindowServer" AND eventMessage CONTAINS "Display:"'` (52 905 lines;
  the `Display:Mode` / `Display:Config` lines quoted above are verbatim).
- Mode sightings: `SLSGetDisplaysWithOpenGLDisplayMask` bucketed by (display, mode) over the
  same window — 477 487 lines.
- Mode tables: `betterdisplaycli get -name=AW3425DW -displayModeList`,
  `CGSGetDisplayModeDescriptionOfLength` (SkyLight) and `CGDisplayCopyAllDisplayModes`.
- Panel rate: DDC/CI VCP 0xAE (the monitor's own vertical-frequency register) = 240.00 Hz.
- Panics: `/Library/Logs/DiagnosticReports/Retired/panic-full-2026-10-18-{172321,232751,235555}.0002.panic`.
- Persisted display config: `/Library/Preferences/com.apple.windowserver.displays.plist`
  (AW UUID `F74F556F-59FB-41B1-B5AC-E1BF4AFA76F3`, `CurrentInfo: Hz 60, Scale 2, Wide 3440`).
