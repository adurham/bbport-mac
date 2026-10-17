# Game library plug-in point (`BB_GAME_LIB`)

A game library is a shared library that replaces some of the game's own functions with new code
— for [Paleblood](https://github.com/Yharnam-Hunters/Paleblood), the decompiled functions
rebuilt from source (`libbbgame`). The mechanism comes from the Yharnam-Hunters fork of bbport
(`github.com/Yharnam-Hunters/bbport`, commit `ce7a6ff` "loader: game library plug-in point and
function hooks (BB_GAME_LIB)", the fork's `master` at the time of the port: `8a2ed0e`). This fork
keeps the same environment variable, header and ABI, so a library built for their loader is a
library for this one. It is off unless `BB_GAME_LIB` is set; unset or empty, the loader does
exactly what it did before.

## What happens

`src/probe.c`, in the game process:

1. The loader maps the game image, applies its relocations, the patches (`--patches`), the macOS
   TLS fixups and the port's own guest hooks (`bbgpu_patch_image`: frame rate, game menu, GNM).
2. With `BB_GAME_LIB=/path/libbbgame.dylib`: `dlopen(path, RTLD_NOW | RTLD_LOCAL)`, then
   `dlsym("bbgame_init")`, then `bbgame_init(&host)` once.
3. The library calls `host->install_hook(offset, size, target)` for each function it replaces.
   The loader writes a 14-byte absolute jump at the function's entry —
   `ff 25 00 00 00 00` (`jmp [rip+0]`) followed by the 8-byte target address. The original
   function body is never run again; the replacement is called with the original's arguments and
   returns to the original's caller.
4. The loader prints `Game library <path>: N hooks installed, init returned R`. A nonzero `R`, a
   library that does not load, or one without `bbgame_init` stops the loader with an `ERROR:`
   before any game code runs.
5. The data moves into the memory shared with the GPU process, the image gets its final page
   protections and the game starts.

## The ABI (`src/bbgame.h`)

```c
#define BBGAME_API_VERSION 1u
typedef struct BbGameHost {
    uint32_t version;          /* BBGAME_API_VERSION */
    unsigned char *image;      /* host address of guest offset 0 */
    uint64_t image_size;
    int (*install_hook)(uint64_t offset, uint64_t size, const void *target);
} BbGameHost;
int bbgame_init(const BbGameHost *host);   /* exported; 0, or nonzero to stop the loader */
```

- **Offsets are guest offsets** (ELF `p_vaddr` of the loaded image, the address minus the
  executable's base). Paleblood's `runtime/plugin.c` passes `address - RT_EBOOT_BASE`
  (`0x400000`) from its generated hook table.
- `install_hook` returns -1 (and writes nothing) unless the whole `size` bytes lie in one
  executable segment, `size` is at least 14 and `target` is non-null. `size` is the replaced
  function's length: the jump must fit inside it.
- Replacements are called from game code, so they use the game's calling convention: System V
  x86-64. Write them `__attribute__((sysv_abi))` where the compiler's default could differ
  (it does not on macOS or Linux x86-64).
- The library sees the image as mapped (`host->image`, read-write) and may keep the pointer.
- **Only for the supported executable.** Hook addresses are only valid for the eboot that
  `scripts/game_check.py` pins (CUSA03173 1.09), so `BB_SKIP_GAME_CHECK=1` refuses the library.

## macOS: which process, which architecture

On the native line the game runs as two processes: `bb-probe`, the loader and the game's own
x86-64 code under Rosetta 2, and `bb-gpu`, the native arm64 renderer. The game's functions live
in `bb-probe`, so **the library is loaded there, and only there**, exactly as the Linux loader
does. It must therefore be an **x86_64** Mach-O dylib (an arm64 or universal-without-x86_64
library fails `dlopen` with an architecture error, which the loader prints). `bb-gpu` never
sees `BB_GAME_LIB`; the in-process mode (`BB_NATIVE_GPU` unset) loads it at the same point.

Not on Windows (the loader stops with "BB_GAME_LIB is not supported on Windows"), as upstream.

## Interaction with the port's own hooks

The port detours a few places inside the game's code before the library is loaded
(`gpu/shim/bbport_frame_rate.cpp`, `bbport_game_menu.cpp`, `bbport_gnm_hooks.cpp`). A library
replacement is installed after them, so it wins: where a replaced function contains one of the
port's detours, that detour is no longer reached. Today that matters for Paleblood's two frame
timing functions — `frame_timing_frame_step` (0x2018d20 in the image) and
`frame_timing_pace_frame` (0x2034770) — which contain the frame-rate hooks (0x2018e36, 0x203493d,
0x2034d96). By construction, **with Paleblood's library loaded the frame-rate menu's
60/90/unlocked modes have nothing to act on** and the game paces itself as the replacement does
(the original's 30 FPS) — reasoned from the addresses, not measured. To keep the port's frame
rate, skip those two with Paleblood's own switch:
`BB_HOOK_SKIP=bb_frame_timing_frame_step,bb_frame_timing_pace_frame` (names as in its generated
hook table, or the addresses `0x02418d20,0x02434770`).

## The example library and the smoke test

`examples/game_lib/noop.c` is the smallest library: it checks the host interface, says so and
replaces nothing (so the game runs unchanged). `BBGAME_NOOP_SELFTEST=1` also asks for two hooks
the loader must refuse (a 13-byte function; an offset past the image) and prints both answers.

```sh
bash examples/game_lib/build.sh          # out/game_lib/libbbgame-noop.dylib (x86_64)
BB_GAME_DIR=~/Games/Bloodborne/CUSA03173 bash tools/macos/game_lib_smoke.sh
```

The smoke test runs one hidden warm pass (`BB_NATIVE_GPU=1 BB_WARM_ONLY=1`: no window, no sound,
quits when its compiles stop) and requires in `out/game_lib_smoke.log`:

```
bbgame-noop: host interface v1, image 0x800000000, 93538364 bytes; no hooks requested
bbgame-noop: selftest: short function -> -1, outside the image -> -1 (both must be -1)
Game library .../out/game_lib/libbbgame-noop.dylib: 0 hooks installed, init returned 0
Entering original x86-64 code at guest offset 0xa0
Warm run: ...
```

It skips itself while the game is running. `BB_GAME_LIB=<lib> bash tools/macos/game_lib_smoke.sh`
runs the same check with any library.

`tests/test_game_lib.py` (`python3 -m unittest discover -s tests -p test_game_lib.py`) proves the
dispatch on a synthetic image without the game: a library hooks the entry function and the run
reaches the replacement (exit 42) instead of the original; the refusals, an init failure, a
missing symbol, a missing file and `BB_SKIP_GAME_CHECK=1` all stop the loader before game code;
unset/empty `BB_GAME_LIB` runs the original.

## Building Paleblood's `libbbgame` for macOS

Paleblood builds `libbbgame` with CMake (`game/CMakeLists.txt`: target `bbgame`, every source
under `game/<system>/`, the hook table generated from `game/hooks.csv`, linked with the runtime
`bbrt`). Its `bbgame_init` (`runtime/plugin.c`) installs every hook in the table through
`host->install_hook`. On macOS, x86_64:

```sh
cmake -S ~/repos/paleblood -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=x86_64
ninja -C build-mac bbgame                # build-mac/game/libbbgame.dylib
BB_GAME_LIB=$PWD/build-mac/game/libbbgame.dylib BB_GAME_DIR=... bash tools/macos/run.sh
```

As of Paleblood `af97b90` that build needs two Linux-isms fixed **in Paleblood** (not here):

- `runtime/probe.c` uses `MAP_FIXED_NOREPLACE` (Linux-only) to place its probe stubs within 2 GB
  of the image. `-DMAP_FIXED_NOREPLACE=0` makes it compile (a plain address hint), but then
  `mmap` may return memory elsewhere; a portable fix checks that the result equals the hint (and
  unmaps and tries the next candidate if not). It only matters with `BB_PROBES` set.
- `game/CMakeLists.txt` links with `-Wl,--no-undefined` (GNU ld); Apple's ld wants
  `-Wl,-undefined,error` (its default for dylibs anyway).

With those two worked around in a scratch build, Paleblood's library (31 hooks) loaded in a hidden
warm pass here: `bbgame: 31 hook(s) installed` / `Game library .../libbbgame.dylib: 31 hooks
installed, init returned 0`, then the game entered and the warm pass finished normally.
Paleblood's own runner (`tools/run_game.sh`) is Linux-only (flock, Xvfb, nvidia-smi); on the Mac
use `tools/macos/run.sh` with `BB_GAME_LIB` set as above.
