// SPDX-License-Identifier: GPL-2.0-or-later
// Interface between the loader and a game library (BB_GAME_LIB). Ported unchanged from the
// Yharnam-Hunters fork (github.com/Yharnam-Hunters/bbport, commit ce7a6ff, src/bbgame.h), the
// runtime scaffold of Paleblood, so a game library built against their header loads here too.
// The loader maps and relocates the image, applies patches, then loads the library and calls
// its bbgame_init before any game code runs. The library replaces game functions by asking
// the loader to install a jump at their entry. Offsets are guest offsets (ELF p_vaddr).
//
// macOS: the library is loaded by the GAME process (bb-probe, x86-64 under Rosetta), never by
// the native arm64 GPU process (bb-gpu), so it must be an x86_64 Mach-O dylib. See
// docs/GAME_LIBRARY.md.
#pragma once

#include <stdint.h>

#define BBGAME_API_VERSION 1u

typedef struct BbGameHost {
    uint32_t version;          /* BBGAME_API_VERSION */
    unsigned char *image;      /* host address of guest offset 0 */
    uint64_t image_size;
    /* Replace the function at `offset`, `size` bytes long, by a jump to `target`. Returns 0, or
       -1 if the function is shorter than the 14-byte jump or not inside an executable segment. */
    int (*install_hook)(uint64_t offset, uint64_t size, const void *target);
} BbGameHost;

/* Exported by the game library. Returns 0, or nonzero to stop the loader. */
typedef int (*BbGameInit)(const BbGameHost *host);
#define BBGAME_INIT_SYMBOL "bbgame_init"
