# Feedback to Apple — DCPEXT1 `axi_rd_err` on a display-mode switch (2026-10-18)

Paste-ready addendum for the Feedback filed after the second panic. Facts only, with artifact
paths; unknowns stated as unknowns.

Two sysdiagnoses exist and are the important attachment — each was captured 1.5–2.5 minutes after
the reboot that followed a panic:

- `/var/tmp/sysdiagnose_2026.10.18_17-25-47-0500_macOS_Mac_26B5112a` (after the 17:22 panic)
- `/var/tmp/sysdiagnose_2026.10.18_23-28-56-0500_macOS_Mac_26B5112a` (after the 23:26 panic)

Each contains `ioreg/IOReg.xml`, `com.apple.windowserver.displays.plist`,
`SystemProfiler/SPDisplaysDataType.spx` and `logs/powerlogs`.

---

## Body

**Title:** macOS 27.2 (26B5112a): DCPEXT1 panic (`IOMFB … axi_rd_err`) when an external display's
pipe is re-parameterised (2x HiDPI <-> 1x) while the 240 Hz 10-bit HDR10 DSC link is up — three
times in one evening on M4 Max + AW3425DW, twice with no third-party process involved in the switch
itself

**Hardware:** MacBook Pro M4 Max (Mac16,6); Alienware AW3425DW 3440x1440 240 Hz over USB-C.
Internal display: 3024x1964 Liquid Retina XDR.

**OS:** macOS 27.2, build 26B5112a; Darwin 27.2.0 `xnu-13432.41.4~3 RELEASE_ARM64_T6041`;
DCP firmware `AppleDCP-1317.40.16~861-t604xdcp.RELEASE`, `RTKit_armv8_chinook_Oz_flto_pac-3514.41.1`.

**External display configuration:** the AW3425DW runs a 2x HiDPI mode (mode id 775,
6880x2880 backing, "looks like 3440x1440"). macOS reports it as 60 Hz; the link runs at 240 Hz.
The app is a Vulkan-on-Metal game presenting to a fullscreen `CAMetalLayer`
(drawable 6880x2880 on that mode, opaque, FIFO, direct scanout).

**Panic 1 — 2026-10-18 17:22:28** (`panic-full-2026-10-18-172321`, incident
`E514F0F8-3001-452A-84BC-5955F93CB0FB`):
`DCPEXT1 PANIC - IOMFB int_handler_gated: failure: axi_rd_err [0x40381400]`, faulting task
`iomfb_driver(9)`. The app had been running 5.9 s. `CGDisplaySetDisplayMode` 775 (6880x2880@60) →
724 (3440x1440@240) was issued at startup while the first fullscreen drawable was being promoted.
No stackshot; `logd` lost the tail before the panic.

**Panic 2 — 2026-10-18 ~23:26:44** (`panic-full-2026-10-18-232751`, incident
`61B7F251-5DA2-4D6C-8D62-CBE11097D00C`):
`DCPEXT1 PANIC - IOMFB int_handler_gated: failure: axi_rd_err [0x40381408]`, faulting task
`iomfb_driver(9)`. Same initiator and register; the low byte differs (`…400` vs `…408`). This time
the mode change landed normally: the display went to 3440x1440@240 (mode id 724, native flag set),
scanout went 1:1, and the app ran at 120.0 fps with 8.0 ms median frame time for ~110 s before the
DCP faulted. A streamed WindowServer/CoreDisplay log ends at 23:26:43.95.

**Panic 3 — 2026-10-18 23:54:41** (`panic-full-2026-10-18-235555`, incident
`B738ED63-5F39-497C-945B-01E9BDB9A354`):
`DCPEXT1 PANIC - IOMFB int_handler_gated: failure: axi_rd_err [0x40381400]`, faulting task
`iomfb_driver(9)`. **Nothing of ours was running.** The owner turned HiDPI off in BetterDisplay by
hand — the same 775 → native-1x switch, issued by a third-party display tool, with no game, no
`bb-probe`, no `bb-gpu` and no Metal surface of ours alive. Verified from the report itself (its
process list contains no bbport process; the port's own logs end on the previous evening) and from
the machine's boot: up 27 min, WindowServer's mode log shows the desktop on 775 for the whole boot,
and the switch fell in the last 37 s that `logd` lost before the fault.

**This is the decisive report: it removes every component of ours from the trigger and leaves the
775 → native transition itself.** Two of three panics had the port running; the third had nothing,
and all three share the initiator, the faulting task and the register.

**Correlation:** for roughly two months before 2026-10-17 the desktop happened to be on a mode
identical to the one the app requests, so no mode change was ever issued and launches were stable.
Since the desktop moved to the 2x HiDPI mode (775), every 775 → 724 transition attempted on this
machine has panicked it — twice from the game's startup (once within seconds, once after ~110 s of
apparently normal 1:1 operation) and once from BetterDisplay alone with no app at all. The app's
only role in the first two was to request the same transition the OS cannot survive here.

**Request:**
1. Decode the faulting register `0x40381400` / `0x40381408` against IOMFB symbols and tell us which
   fetch/plane it belongs to — we cannot.
2. Is the 775 → 724 transition taken as a "seamless"/fast re-parameterisation (the link timing does
   not change: 240 Hz throughout, only the mode's pixel size and scaler state do), and is that path
   known to be fragile when the source mode is a user-injected HiDPI mode?
3. A userland `CGDisplaySetDisplayMode` should not be able to panic a display coprocessor. Please
   treat this as a DCP firmware fault, independent of the mode's origin.
4. Tell us what to capture at the moment of the switch (CoreDisplay/IOMFB signposts, specific
   `log stream` predicates, an RTKit trace) so the next reproduction carries a real trace rather
   than the truncated log tail.

**Attachments:** all three `.panic` reports; the sysdiagnoses taken 1.5–2.5 min after the first two
reboots; the app's own log for the second run (mode id 724 applied at 23:24:52, 1:1 scanout, 120 fps
until the fault); the streamed WindowServer/CoreDisplay log covering 23:24–23:26; a BetterDisplay
configuration export for the injected mode.

**Unknowns we are not claiming:** we still have no trace captured at the moment of the switch —
`logd` loses the tail and none of the three reports carries a stackshot — and since every attempt
costs another hard reboot we have not deliberately repeated one to get it.

---

## Addendum, 2026-10-19 — the trigger narrowed by log evidence

This addendum is written from the machine's unified log, the mode tables and the monitor's DDC
register. No new mode switch was performed to produce it.

### 1. WindowServer records every mode set, and the fatal ones have a distinct signature

`log show --predicate 'process == "WindowServer" AND eventMessage CONTAINS "Display:"'` carries the
client API call, the mode transition, and the resources WindowServer asks the DCP for. The set that
preceded panic 2 (2026-10-18 23:24:55.404, mode `0x2d4` = 724) reads:

```
client api - modeset 2d4 for display 4
desired mode change 775 -> 724
CA Display width 3440, bandwidth 9510
request_resources: IOMFB needs a modeset on display 0x4 for pipe reconfiguration.
For display 0x4, hw_reconfig_required: 0, modeset_required: 1, resource_release_required: 1
  setting mode 775 -> 724
Performing mode refresh.
  current mode: <CADisplayMode 3440 x 1440 fmt:RGB_PQ_10bit range:full>
```

Mode 775 is `3440x1440` at 2x density (6880x2880 backing); mode 724 is `3440x1440` at 1x
(3440x1440 backing). The set **changes the pipe's source width** from 6880 to 3440. Three fields
mark that class and are absent from every surviving set:

- `CA Display width` drops 6880 -> 3440,
- `request_resources: IOMFB needs a modeset … for pipe reconfiguration` appears,
- `resource_release_required: 1` appears (this line occurs **once** in the whole 3-day log store —
  on this set).

Sets that keep the source width report `modeset_required: 0, resource_release_required: 0` and leave
`CA Display width` at 6880. There are **22** of those after the link went 10-bit (13 of them the
desktop switching between 769 and 775, the rest 150 <-> 151 and 152 -> 150), all inside the 2x
family, **none** of which faulted. One of them left the desktop on 769 for 4 h 29 min, across a
dock hotplug, without faulting.

### 2. The fault is delayed, which points at a stale descriptor rather than a rejected set

The 23:24:55 set **succeeded**: the pipe came up 1:1 at 3440x1440/240 (fmt `RGB_PQ_10bit`,
10-bit HDR10 retained), the app presented at 120.0 fps for 104 s, and the DCP faulted at 23:26:44 —
**108.6 s after a mode set that reported success**. Panic 1 faulted within seconds of the same
operation. A fault that follows a *completed* reconfiguration, with variable latency, is consistent
with the pipe retaining a fetch descriptor (base/size/stride) for the previous plane and touching it
after it has been unmapped, rather than with the reconfiguration being rejected.

### 3. The control: the same reconfiguration on a different link is harmless

On 2026-10-16 the same display ran the same 2x -> 1x transition **ten times** (modes 149 -> 99,
12:41 through 18:35), each with the identical signature — `CA Display width 3440`,
`IOMFB needs a modeset … for pipe reconfiguration`, `modeset_required: 1,
resource_release_required: 1` — and the machine did not fault once. At that time the link was
`YCbCr422_8bit range:limited` (8-bit 4:2:2, ~19 Gbps at 3440x1440@240, no compression).

The link became `RGBCalibrated_10bit/full` at 19:50:32 and `RGB_PQ_10bit/full` (PQ / HDR10) at
19:50:33; the 240 Hz 10-bit connection mode was established at 20:20:32. Since then, every
cross-plane set attempted on this machine has faulted (3 of 3), and no cross-plane set has been
attempted that did not.

So: **the pipe reconfiguration is the trigger, and the 240 Hz 10-bit HDR10 (DSC) link is what makes
it fatal.** Four link properties change together between the safe runs and the fatal ones (8-bit vs
10-bit, 4:2:2 vs RGB, SDR vs PQ, uncompressed vs DSC ~1.87:1), and we cannot yet say which of them
matters.

### 4. Narrowed questions for Apple

1. On a display-mode set that keeps the link timing but changes the source plane (6880x2880 ->
   3440x1440), does IOMFB re-derive the DSC encoder's input descriptor and the rotator/scaler fetch
   descriptor, or does the "no link change" fast path reuse the previous plane's descriptor?
2. Decode `0x40381400` / `0x40381408` against IOMFB symbols: which fetch unit/plane do they belong
   to, and is the address in `axi_rd_err` the *old* plane's base (a stale descriptor) or the new one?
3. Is there a supported way for a userland client to change the source plane while a DSC-coded link
   is up — or is a link re-initialisation (HPD) required by design? If the latter, a `kCGErrorSuccess`
   from `CGDisplaySetDisplayMode` followed by a panic 109 s later is a firmware defect regardless.
4. Same capture request as before: what should we collect at the moment of the switch so the next
   reproduction carries a trace instead of a lost log tail.

### 5. What we changed on our side

Nothing in the OS; the machine's own display tooling was corrected so that it can no longer issue
the fatal class. The two recovery scripts on this machine used mode ids captured from a table that
has since been rebuilt (150 is now a 1583x663 mode, not 3440x1440), which would have issued a
cross-plane set; they now resolve their target by description and refuse to run from a 1x desktop.

