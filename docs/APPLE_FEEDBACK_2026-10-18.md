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

**Title:** macOS 27.2 (26B5112a): DCPEXT1 panic (`IOMFB … axi_rd_err`) when changing an external
display from a 2x HiDPI mode to its native mode — twice in one day on M4 Max + AW3425DW

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

**Correlation:** for roughly two months before 2026-10-17 the desktop happened to be on a mode
identical to the one the app requests, so no mode change was issued and launches were stable. Since
the desktop moved to the 2x HiDPI mode (775), the first two launches that actually performed the
775 → 724 switch both panicked — once within seconds, once after ~110 s of apparently normal
operation at 1:1 scanout.

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

**Attachments:** both `.panic` reports; both sysdiagnoses (taken 1.5–2.5 min after each reboot);
the app's own log for the second run (mode id 724 applied at 23:24:52, 1:1 scanout, 120 fps until
the fault); the streamed WindowServer/CoreDisplay log covering 23:24–23:26; a BetterDisplay
configuration export for the injected mode.

**Unknowns we are not claiming:** we have not tried the same 775 → 724 switch with no app running
and no third-party scanout surface alive; and we have not repeated the failing launch (each attempt
is another hard reboot).
