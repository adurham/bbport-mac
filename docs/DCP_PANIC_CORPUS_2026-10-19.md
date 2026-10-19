# The `axi_rd_err` panic corpus, and what we can add to it (2026-10-19)

Written to feed the Apple follow-up. Facts are separated from inference; every public claim is
linked. The point of the document is to show that our machine is not a one-off, and that our
DSC isolation is a mechanism-level discriminator that none of the public reports contains.

## 1. The same panic has been reported since 2021

Every one of these is the same signature — `DCP`/`DCPEXT0`/`DCPEXT1 PANIC - IOMFB
int_handler_gated: failure: axi_rd_err`, faulting task `iomfb_driver(<n>)`, no app stack:

| where | hardware / OS | stated trigger | register |
|---|---|---|---|
| Apple discussions, Nov 2022 | MBP 16" M1 Max, macOS 12.6 | external display + USB dock, unexplained | `0x40310600` |
| Apple discussions, Oct 2024 | MBP 2021 (M1), macOS 15.x | black screen, then reboot; HDR livestream suspected | `0x40300034` |
| Apple discussions, Oct 2024 | Mac Studio Ultra, macOS 12.6.1 | Pro Tools, unexplained | `0x40300634` |
| Apple discussions, 2024 | iMac M1, macOS 14.3.1 | StarCraft 2 | `0x40300634` |
| Dota 2 issue #2825, Oct 2024 | M1 Max, Sequoia 15.0/15.0.1 | launching / playing Dota 2 fullscreen | `0x40300634` / `0x40300634` |
| EVE Online forums, Nov 2024 | MBP M1 + Studio Display | "only with an external display"; game fullscreen | `0x40310634` |
| r/macbookpro, Jan 2024 | M3 Max + Odyssey G9, **240 Hz, no HDR**, USB-C→DP | daily crashes while dragging/using Chrome | `0x40300624` |
| **this machine, Oct 2026** | **M4 Max + AW3425DW, macOS 27.2** | **2x HiDPI ↔ 1x mode change on a DSC link** | **`0x40381400` / `0x40381408`** |

Links:
- https://discussions.apple.com/thread/254239211
- https://discussions.apple.com/thread/255802799
- https://discussions.apple.com/thread/254330576
- https://discussions.apple.com/thread/255519342
- https://github.com/ValveSoftware/Dota-2/issues/2825
- https://forums.eveonline.com/t/frequent-crashes-on-macos-sequoia-with-mbp-m1-and-studio-display/464723
- https://www.reddit.com/r/macbookpro/comments/19c6bjx/

Pattern in the register: the high half moves with the DCP instance/OS build (`0x4030…`, `0x4031…`,
`0x4038…`), the low half distinguishes the failing unit (`…0634`, `…0624`, `…0600`, `…1400`). We
cannot decode them; that is still the question for Apple.

Two observations that matter:

- The public cases are either **high-rate external links** (240 Hz on the G9 ⇒ DSC-required:
  3440x1440 10-bit at 240 is 35.4 Gbps, 8-bit is 28.4, both above HBR3's ~25.9) or **HDR video**
  (which on these panels is typically DSC too). Nobody in the corpus separates that variable.
- Every public case involves an **external** display (`DCPEXT*`). The built-in panel (`DISPINT`)
  does not appear.

## 2. What this machine adds

- **A reproduction with no third-party app at all** (panic 3, 2026-10-18 23:54:41): a display
  utility issued the mode set and the machine died with nothing else running.
- **A mechanism-level discriminator**: every fatal set changed the display pipe's source plane
  (6880x2880 ↔ 3440x1440) *while the link was running DSC*; the identical set on a non-DSC link
  (twelve times, across two different link formats) never faulted. Same hardware, same build,
  same modes, same 10-bit HDR10 RGB, same transfer function — only the DSC stage differs.
- **A minimal reproducer** (`tools/macos/dcp-mode-probe.c`, this repo): finds the AW by EDID,
  locates the cross-scale twin at the same refresh, refuses to run when the link is DSC-coded,
  and otherwise issues exactly one `CGSConfigureDisplayMode`. No window, no Metal surface, no
  swapchain — so a resulting panic cannot be attributed to an app.
- **A traced harness** (`tools/macos/dcp-trace-repro.sh`): live `log stream` of WindowServer's
  `Display:` records to disk across the switch plus a 130 s observation window, because the
  23:26:44 fault fired **108.6 s after a set that returned success**. It reports whether the
  machine rebooted and whether a new `.panic` appeared.

## 3. What is still unknown

1. Which fetch unit / plane `0x40381400` and `0x40381408` belong to (Apple's question).
2. Whether the trigger is *any* change to the DSC encoder's input geometry, or specifically the
   2x ↔ 1x plane change. Next test: a same-scale size change (2x 6880x2880 → 2x 4586x1920) on a
   DSC link. Needs a reboot window.
3. Whether raising the link from 120 → 240 *while already at 1x* is safe (a link change at
   constant plane). That is the last step of a staged "game mode"; unproven.
4. Whether a stock, non-forced 165 Hz timing exists that negotiates without DSC (17.68 Gbps fits
   easily at 3440x1440 10-bit; the mode we have is DSC-coded by definition).

## 4. Why it matters beyond this machine

The workaround that makes the machine usable — 120 Hz, 10-bit HDR10 RGB Full, DSC off — costs
the panel's top two rates. Every owner hitting this on a 240 Hz panel faces the same choice, and
none of the public threads resolved it. A DCP firmware fix would restore 240 Hz *and* make mode
switches safe for everyone in the corpus.
