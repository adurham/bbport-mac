# bbport for Mac

*Bloodborne* on Apple Silicon, as a Mac app. A macOS port of
[bbport](https://github.com/deadinside28/bloodborne_pc), deadinside28's native port of the PS4
game (CUSA03173, version 1.09), kept current with each upstream release.

**[Download](https://github.com/bmy/bbport-mac/releases)** ·
**[User guide](docs/USER_GUIDE.md)** ·
**[Report a problem](https://github.com/bmy/bbport-mac/issues)**

> **No game files are included.** You need your own decrypted copy of Bloodborne (CUSA03173)
> with the 1.09 update merged in. Not affiliated with Sony Interactive Entertainment,
> FromSoftware, Apple, shadPS4 or upstream bbport. Report Mac problems **here**, not upstream.

## Status

**Playable, experimental.** Based on bbport 0.6. The whole game boots, plays, saves and loads, with
sound, cutscenes, a DualSense or other controller, or keyboard and mouse.

- **60 FPS in most areas** at 1440p on an M5 Max, with FSR 3.1 or MetalFX; the busiest areas dip
  into the 40s and 50s.
- Output up to 4K in any aspect ratio from 4:3 to 32:9 ultrawide (Auto fills a MacBook's 16:10
  screen), the in-game settings menu, mods and shadPS4-format patches.
- Online play isn't available on the Mac yet.
- Tested on one Mac so far (MacBook Pro, M5 Max, macOS 27). Reports from others are welcome.

## Play

You need an Apple Silicon Mac with **macOS 26 or later**, **Rosetta 2** and the **Command Line
Tools** (for Python 3).

1. Download `bbport-mac-….zip` from [Releases](https://github.com/bmy/bbport-mac/releases) and
   drag **bbport.app** into Applications.
2. Open it. The first time, allow it in **System Settings ▸ Privacy & Security ▸ Open Anyway**
   (the app isn't notarised).
3. Choose your game folder (the one with `eboot.bin`) and press **Play**.

The **[user guide](docs/USER_GUIDE.md)** (also under **Help** in the app) covers every setting,
the controls, where saves live and what to do when something goes wrong. Recommended: 2560×1440,
60 FPS, MetalFX or FSR 3.1, Low Power Mode off.

## Built for the Mac

- **Native where it can be.** Only the game's own x86-64 code runs under Rosetta 2. The renderer
  (bbport's GPU code, the Vulkan driver and Apple's Metal stack) runs as native Apple Silicon code
  in its own process, sharing the game's memory.
- **Metal underneath.** Vulkan runs on KosmicKrisp, Mesa's Vulkan-on-Metal driver and the one
  shadPS4 ships on macOS, so bbport's own renderer runs here with few changes.
- **MetalFX**, Apple's temporal upscaler, alongside FSR 3.1.
- **A real Mac app.** A SwiftUI launcher, a Retina window, Core Audio, native text input, and the
  whole engine, its libraries and its graphics driver inside the app.
- **Reviewed before it ships.** Every upstream release and every dependency change is
  security-reviewed before it is pushed ([docs/security-review.md](docs/security-review.md)).

## Known limitations

- **Geometry shaders are emulated.** Metal has none: the driver runs them as compute passes. If an
  effect looks wrong, `BB_GEOMETRY_SHADERS=0` skips those draws instead.
- **The first launch compiles shaders**: a few minutes of black screen and some stutter, and
  short pauses when a new area loads. Later launches are quick.
- **Occasional pop-in** while an area streams in.

## Building from source

For testing branches and development, see [tools/macos/README-macos.md](tools/macos/README-macos.md).

```
git clone --recursive -b macos-native-0.6 https://github.com/bmy/bbport-mac.git ~/Projects/bbport-mac/src
cd ~/Projects/bbport-mac/src
bash tools/macos/setup_deps.sh
BB_ARCH=arm64 bash tools/macos/setup_deps.sh
bash tools/macos/build.sh
bash tools/macos/build_launcher.sh
open out/bbport.app
```

A source build's app has an **Update** button: it fetches the branch set in bbport ▸ Settings and
rebuilds only what changed.

## Looking ahead

Apple has said macOS 27 is the last release with Rosetta for general use. With the renderer
already native, only the game's own code still needs it; translating that one fixed executable
ahead of time is the long-term plan.

## Credits and licence

Built on **[bbport](https://github.com/deadinside28/bloodborne_pc)** by deadinside28, which builds
on **[shadPS4](https://github.com/shadps4-emu/shadPS4)**'s renderer and shader recompiler. Vulkan
on Metal by **KosmicKrisp** (Mesa), as packaged by
[shadexternals](https://github.com/shadexternals/mesa-kosmickrisp). Components, licences, the
community patches and the Linux build: [upstream README](docs/README-linux.md).

Licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)), like upstream.
